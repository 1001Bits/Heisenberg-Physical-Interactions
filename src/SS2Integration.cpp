#include "SS2Integration.h"
#include "NpcInjectionPolicy.h"

#include <RE/Bethesda/BSScriptUtil.h>
#include <RE/Bethesda/GameScript.h>
#include <RE/Bethesda/FunctionArgs.h>

namespace heisenberg::ss2
{
    namespace
    {
        constexpr std::string_view kPluginName = "SS2.esm";
        constexpr std::uint32_t kManagerQuestLocalFormID = 0x0000EB6Eu;
        constexpr std::string_view kManagerQuestEditorID =
            "SS2_NPC_RPGManager";
        constexpr const char* kManagerScriptName =
            "SimSettlementsV2:Quests:NPC_RPGManager";
        constexpr const char* kCureFunctionName =
            "TryToUseCureItemOnActor";
        constexpr std::size_t kDispatchMethodCallVtableIndex = 46u;

        RE::TESQuest* g_cachedManagerQuest = nullptr;
        RE::AlchemyItem* g_cachedDiseaseCure = nullptr;

        enum class ManagerContextFailure
        {
            None,
            QuestUnavailable,
            VmUnavailable,
            QuestHandleUnavailable,
            ScriptObjectUnavailable,
        };

        const char* ManagerContextFailureName(
            ManagerContextFailure failure)
        {
            switch (failure) {
            case ManagerContextFailure::None:
                return "none";
            case ManagerContextFailure::QuestUnavailable:
                return "SS2_NPC_RPGManager quest unresolved (SS2.esm/record unavailable)";
            case ManagerContextFailure::VmUnavailable:
                return "Fallout 4 VR Papyrus VM unavailable";
            case ManagerContextFailure::QuestHandleUnavailable:
                return "manager quest has no live Papyrus handle";
            case ManagerContextFailure::ScriptObjectUnavailable:
                return "SimSettlementsV2:Quests:NPC_RPGManager is not bound";
            }
            return "unknown";
        }

        bool HasSourceFile(
            const RE::TESForm* form,
            std::string_view pluginName)
        {
            if (!form || pluginName.empty() || !form->sourceFiles.array) {
                return false;
            }

            const auto* files = form->sourceFiles.array;
            for (std::uint32_t i = 0; i < files->size(); ++i) {
                const RE::TESFile* file = (*files)[i];
                if (!file) {
                    continue;
                }
                const std::string_view filename = file->GetFilename();
                if (!filename.empty() &&
                    _stricmp(
                        std::string(filename).c_str(),
                        pluginName.data()) == 0) {
                    return true;
                }
            }
            return false;
        }

        bool IsCanonicalDiseaseCureForm(const RE::TESForm* form)
        {
            if (!form || !form->IsAlchemyItem()) {
                return false;
            }
            const char* editorID = form->GetFormEditorID();
            return npc_injection_policy::IsCanonicalDiseaseCureIdentity(
                HasSourceFile(form, kPluginName),
                editorID ? std::string_view(editorID) : std::string_view{},
                form->formID & 0x00FF'FFFFu);
        }

        bool IsExpectedManagerQuest(
            RE::TESQuest* quest,
            const RE::TESFile* ss2File)
        {
            if (!quest || !ss2File) {
                return false;
            }
            const char* editorID = quest->GetFormEditorID();
            return editorID &&
                   _stricmp(editorID, kManagerQuestEditorID.data()) == 0 &&
                   static_cast<std::uint8_t>(quest->formID >> 24u) ==
                       ss2File->compileIndex;
        }

        RE::TESQuest* ResolveManagerQuest()
        {
            if (g_cachedManagerQuest) {
                return g_cachedManagerQuest;
            }

            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                return nullptr;
            }

            // TESDataHandler::LookupForm is unreliable in F4VR because its
            // compiled-file collection can be null. Resolve the full-master
            // FormID explicitly, matching the working VR pattern elsewhere in
            // this project.
            auto* file = dataHandler->LookupModByName(kPluginName);
            if (!file || file->compileIndex == 0xFF || file->IsLight()) {
                return nullptr;
            }

            const std::uint32_t fullFormID =
                (static_cast<std::uint32_t>(file->compileIndex) << 24u) |
                (kManagerQuestLocalFormID & 0x00FF'FFFFu);
            auto* form = RE::TESForm::GetFormByID(fullFormID);
            if (form && form->Is(RE::ENUM_FORM_ID::kQUST)) {
                auto* directQuest = form->As<RE::TESQuest>();
                if (IsExpectedManagerQuest(directQuest, file)) {
                    g_cachedManagerQuest = directQuest;
                    return g_cachedManagerQuest;
                }
            }

            // The manager EDID is the stable identity. Keep the known local ID
            // as a fast path, then tolerate an SS2 update that moves the record.
            for (auto* quest : dataHandler->GetFormArray<RE::TESQuest>()) {
                if (IsExpectedManagerQuest(quest, file)) {
                    g_cachedManagerQuest = quest;
                    return g_cachedManagerQuest;
                }
            }
            return nullptr;
        }

        RE::BSTSmartPointer<RE::BSScript::IVirtualMachine> ResolveLiveVM()
        {
            // CommonLib's GameVM::GetSingleton relocation has no dependable
            // F4VR mapping. This singleton offset is verified against
            // Fallout4VR.exe's Main::OnIdle -> GameVM::Update call site.
            static REL::Relocation<RE::GameVM**> gameVMSingleton{
                REL::Offset(0x5935428)
            };
            auto* gameVM = *gameVMSingleton;
            return gameVM ? gameVM->GetVM() : nullptr;
        }

        bool ResolveActorScriptObject(
            RE::BSScript::IVirtualMachine* vm,
            RE::Actor* target,
            RE::BSTSmartPointer<RE::BSScript::Object>& outObject)
        {
            if (!vm || !target) {
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!npc_injection_policy::IsEligibleNpcTarget(
                    reinterpret_cast<std::uintptr_t>(target),
                    reinterpret_cast<std::uintptr_t>(player),
                    target->formID,
                    target->IsDead(true))) {
                return false;
            }

            constexpr std::uint32_t actorType =
                RE::BSScript::GetVMTypeID<RE::Actor>();
            RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
            if (!vm->GetScriptObjectType(actorType, typeInfo) || !typeInfo) {
                return false;
            }

            auto& handles = vm->GetObjectHandlePolicy();
            const std::uint64_t actorHandle =
                handles.GetHandleForObject(actorType, target);
            if (actorHandle == handles.EmptyHandle()) {
                return false;
            }

            // A stale/reused VM handle must not silently turn the requested
            // settler into PlayerRef (or any other actor) between selection
            // and argument packing.
            auto* roundTrippedActor = static_cast<RE::Actor*>(
                handles.GetObjectForHandle(actorType, actorHandle));
            if (roundTrippedActor != target ||
                !npc_injection_policy::IsEligibleNpcTarget(
                    reinterpret_cast<std::uintptr_t>(roundTrippedActor),
                    reinterpret_cast<std::uintptr_t>(player),
                    roundTrippedActor ? roundTrippedActor->formID : 0u,
                    roundTrippedActor
                        ? roundTrippedActor->IsDead(true)
                        : true)) {
                return false;
            }

            if (!vm->FindBoundObject(
                    actorHandle,
                    typeInfo->name.c_str(),
                    false,
                    outObject,
                    false)) {
                if (!vm->CreateObject(typeInfo->name, outObject) ||
                    !outObject) {
                    return false;
                }
                // ID 709728 has a verified F4VR mapping. Binding here mirrors
                // CommonLib's normal object-argument packing without calling
                // its unsafe GameVM::GetSingleton path.
                vm->GetObjectBindPolicy().BindObject(
                    outObject,
                    actorHandle);
            }
            return static_cast<bool>(outObject) &&
                   outObject->handle == actorHandle;
        }

        struct ManagerContext
        {
            RE::BSTSmartPointer<RE::BSScript::IVirtualMachine> vmKeepAlive;
            RE::BSScript::IVirtualMachine* vm = nullptr;
            RE::TESQuest* quest = nullptr;
            std::uint64_t questHandle = 0;
            RE::BSTSmartPointer<RE::BSScript::Object> scriptObject;
        };

        bool ResolveManagerContext(
            ManagerContext& out,
            ManagerContextFailure* outFailure = nullptr)
        {
            const auto fail = [&](ManagerContextFailure failure) {
                if (outFailure) {
                    *outFailure = failure;
                }
                return false;
            };
            if (outFailure) {
                *outFailure = ManagerContextFailure::None;
            }
            out.quest = ResolveManagerQuest();
            if (!out.quest) {
                return fail(ManagerContextFailure::QuestUnavailable);
            }
            out.vmKeepAlive = ResolveLiveVM();
            out.vm = out.vmKeepAlive.get();
            if (!out.vm) {
                return fail(ManagerContextFailure::VmUnavailable);
            }

            auto& handles = out.vm->GetObjectHandlePolicy();
            constexpr std::uint32_t questType =
                RE::BSScript::GetVMTypeID<RE::TESQuest>();
            out.questHandle =
                handles.GetHandleForObject(questType, out.quest);
            if (out.questHandle == handles.EmptyHandle()) {
                return fail(
                    ManagerContextFailure::QuestHandleUnavailable);
            }

            const bool found = out.vm->FindBoundObject(
                       out.questHandle,
                       kManagerScriptName,
                       false,
                       out.scriptObject,
                       false) &&
                   static_cast<bool>(out.scriptObject);
            return found ?
                       true :
                       fail(ManagerContextFailure::ScriptObjectUnavailable);
        }

        RE::AlchemyItem* ResolveDiseaseCureForm(
            const ManagerContext& context)
        {
            if (!context.vm || !context.scriptObject) {
                return nullptr;
            }

            const RE::BSFixedString propertyName("DiseaseCureForm");
            const auto* property =
                context.scriptObject->GetProperty(propertyName);
            if (!property || !property->is<RE::BSScript::Object>()) {
                return nullptr;
            }

            auto propertyObject =
                RE::BSScript::get<RE::BSScript::Object>(*property);
            if (!propertyObject) {
                return nullptr;
            }

            auto& handles = context.vm->GetObjectHandlePolicy();
            const std::uint64_t cureHandle = propertyObject->handle;
            constexpr std::uint32_t cureType =
                RE::BSScript::GetVMTypeID<RE::AlchemyItem>();
            if (cureHandle == handles.EmptyHandle() ||
                !handles.HandleIsType(cureType, cureHandle) ||
                !handles.IsHandleObjectAvailable(cureHandle)) {
                return nullptr;
            }

            return static_cast<RE::AlchemyItem*>(
                handles.GetObjectForHandle(cureType, cureHandle));
        }
    }

    bool IsDiseaseCureForm(RE::TESForm* form)
    {
        if (!form || !form->IsAlchemyItem()) {
            return false;
        }

        // SS2 3.6.1's generic cure is stable data, but the manager's bound
        // Papyrus object can be temporarily unavailable while a save finishes
        // loading (or in a save with script errors). Reserve the exact form
        // from player consumption even in that state. Requiring both its
        // non-localised EDID and SS2.esm source prevents similarly named aid
        // items and SS2's internal per-disease potions from matching.
        if (IsCanonicalDiseaseCureForm(form)) {
            return true;
        }
        if (g_cachedDiseaseCure) {
            return form == g_cachedDiseaseCure;
        }

        try {
            ManagerContext context;
            if (!ResolveManagerContext(context)) {
                return false;
            }
            g_cachedDiseaseCure = ResolveDiseaseCureForm(context);
            if (g_cachedDiseaseCure) {
                spdlog::info(
                    "[INJECT-NPC] Resolved SS2 DiseaseCureForm to {:08X}",
                    g_cachedDiseaseCure->formID);
            }
            return form == g_cachedDiseaseCure;
        } catch (const std::exception& e) {
            spdlog::warn(
                "[INJECT-NPC] Could not resolve SS2 DiseaseCureForm: {}",
                e.what());
            return false;
        }
    }

    DiseaseCureDispatchResult DispatchDiseaseCure(
        RE::Actor* target,
        RE::TESForm* heldCure,
        const std::function<bool()>& commitInventory)
    {
        bool inventoryCommitted = false;
        try {
            if (!target || !heldCure || !commitInventory) {
                return DiseaseCureDispatchResult::PreflightFailed;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!npc_injection_policy::IsEligibleNpcTarget(
                    reinterpret_cast<std::uintptr_t>(target),
                    reinterpret_cast<std::uintptr_t>(player),
                    target->formID,
                    target->IsDead(true))) {
                spdlog::error(
                    "[INJECT-NPC] Rejected target {:08X} before SS2 dispatch (playerPointer={} playerForm={} dead={})",
                    target->formID,
                    target == player ? "yes" : "no",
                    target->formID ==
                            npc_injection_policy::kPlayerRefFormID ?
                        "yes" : "no",
                    target->IsDead(true) ? "yes" : "no");
                return DiseaseCureDispatchResult::PreflightFailed;
            }

            ManagerContext context;
            ManagerContextFailure contextFailure =
                ManagerContextFailure::None;
            if (!ResolveManagerContext(
                    context,
                    &contextFailure)) {
                spdlog::warn(
                    "[INJECT-NPC] SS2 manager preflight failed: {}",
                    ManagerContextFailureName(contextFailure));
                return DiseaseCureDispatchResult::PreflightFailed;
            }
            auto* vm = context.vm;

            RE::AlchemyItem* liveDiseaseCure =
                ResolveDiseaseCureForm(context);
            if (!liveDiseaseCure || heldCure != liveDiseaseCure) {
                spdlog::warn(
                    "[INJECT-NPC] Held form {:08X} does not match SS2's live DiseaseCureForm (live={:08X}; property/handle may be unbound if zero)",
                    heldCure->formID,
                    liveDiseaseCure ? liveDiseaseCure->formID : 0u);
                return DiseaseCureDispatchResult::PreflightFailed;
            }
            g_cachedDiseaseCure = liveDiseaseCure;

            RE::BSTSmartPointer<RE::BSScript::Object> actorObject;
            if (!ResolveActorScriptObject(vm, target, actorObject)) {
                spdlog::warn(
                    "[INJECT-NPC] Could not bind actor {:08X} for Papyrus",
                    target->formID);
                return DiseaseCureDispatchResult::PreflightFailed;
            }

            using ArgumentFunction =
                Papyrus::BSTThreadScrapFunctionOG<bool(
                    RE::BSScrapArray<RE::BSScript::Variable>&)>;
            using Callback =
                RE::BSTSmartPointer<
                    RE::BSScript::IStackCallbackFunctor>;
            using DispatchMethodCall = bool(*)(
                RE::BSScript::IVirtualMachine*,
                std::uint64_t,
                const RE::BSFixedString&,
                const RE::BSFixedString&,
                const ArgumentFunction&,
                const Callback&);

            void** vtable = *reinterpret_cast<void***>(vm);
            if (!vtable || !vtable[kDispatchMethodCallVtableIndex]) {
                spdlog::error(
                    "[INJECT-NPC] Papyrus VM dispatch slot {} is unavailable",
                    kDispatchMethodCallVtableIndex);
                return DiseaseCureDispatchResult::PreflightFailed;
            }
            auto dispatch = reinterpret_cast<DispatchMethodCall>(
                vtable[kDispatchMethodCallVtableIndex]);

            // Passing Actor* to FunctionArgs would call CommonLib's broken VR
            // GameVM singleton. Supplying the object already bound through the
            // live VM selects the safe vmobject_ptr packing overload.
            Papyrus::FunctionArgs<
                RE::BSTSmartPointer<RE::BSScript::Object>,
                bool>
                arguments(vm, actorObject, true);
            ArgumentFunction packedArguments = arguments.get();
            const RE::BSFixedString scriptName(kManagerScriptName);
            const RE::BSFixedString functionName(kCureFunctionName);
            const Callback callback;

            // SS2's function removes DiseaseCureForm from PlayerRef when its
            // second argument is true. Commit the held world stack only after
            // every resolver and argument builder above has succeeded.
            if (!commitInventory()) {
                spdlog::warn(
                    "[INJECT-NPC] Held backend refused the inventory-commit handoff; SS2 dispatch aborted");
                return DiseaseCureDispatchResult::PreflightFailed;
            }
            inventoryCommitted = true;

            const bool accepted = dispatch(
                vm,
                context.questHandle,
                scriptName,
                functionName,
                packedArguments,
                callback);
            return accepted
                       ? DiseaseCureDispatchResult::Accepted
                       : DiseaseCureDispatchResult::
                             RejectedAfterInventoryCommit;
        } catch (const std::exception& e) {
            spdlog::error(
                "[INJECT-NPC] SS2 Papyrus dispatch failed: {}",
                e.what());
        }

        return inventoryCommitted
                   ? DiseaseCureDispatchResult::
                         RejectedAfterInventoryCommit
                   : DiseaseCureDispatchResult::PreflightFailed;
    }
}
