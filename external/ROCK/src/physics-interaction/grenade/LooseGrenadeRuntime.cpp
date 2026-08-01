#include "physics-interaction/grenade/LooseGrenadeRuntime.h"

#include "physics-interaction/PhysicsLog.h"

#include "RockConfig.h"

#include "RE/Bethesda/Actor.h"
#include "RE/Bethesda/BGSMod.h"
#include "RE/Bethesda/BGSInventoryItem.h"
#include "RE/Bethesda/BSExtraData.h"
#include "RE/Bethesda/BSLock.h"
#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"

#include <REL/Relocation.h>
#include <windows.h>

// AFTER <windows.h> and after every RE/CommonLibF4 header on purpose: this header pulls in
// <Windows.h> itself, and putting it at the top of the block would let the Win32 macros
// (GetObject, near/far, min/max) reach the CommonLibF4 headers below it.
#include "physics-interaction/native/HookAddressDiagnostics.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>

namespace rock::loose_grenade_runtime
{
    namespace
    {
        constexpr std::uintptr_t kFuncActorEquipManagerEquipObject = 0x0E6FEA0;
        constexpr std::uint32_t kInvalidStackId = 0xFFFF'FFFFu;
        /*
         * The PRISTINE entry prologue of ActorEquipManager::EquipObject. ROCK no
         * longer patches this address (Heisenberg's detour owns it), so this array
         * is now purely a reporting baseline: installEquipHook() prints found-vs-
         * expected so the log says what is actually sitting on the entry instead of
         * guessing who put it there.
         */
        constexpr std::array<std::uint8_t, 17> kActorEquipManagerEquipObjectExpectedPrefix{
            0x4C, 0x8B, 0xDC,
            0x49, 0x89, 0x53, 0x10,
            0x55,
            0x56,
            0x41, 0x54,
            0x41, 0x57,
            0x49, 0x8D, 0x6B, 0xD9,
        };

        struct InventoryStackMatch
        {
            bool found{ false };
            bool exactRequestedStack{ false };
            bool exactInstanceData{ false };
            std::uint32_t stackId{ kInvalidStackId };
            std::uint32_t count{ 0 };
        };

        std::atomic<bool> s_equipHookInstalled{ false };
        /*
         * FAIL-SAFE LATCH. A live EquipObject detour IS the implementation of
         * loose-grenade equip interception: without one nothing can ever hand a
         * grenade to ROCK, so every consumer of the pending-equip queue must be
         * switched off rather than left waiting for a request that can never
         * arrive. Set once, at install time, and read by the public queue
         * accessors below.
         */
        std::atomic<bool> s_equipInterceptionDisabled{ false };
        /*
         * Host-detour arbitration state. Heisenberg patches 0x0E6FEA0 itself, in
         * Hooks::Install(), which runs BEFORE rock::HostLoad() -> installEquipHook().
         * The host therefore reports its outcome through
         * setHostEquipObjectDetourInstalled() and installEquipHook() reads it back.
         * "Reported" is tracked separately from "installed" so the log can tell a
         * host that said "no detour" apart from a host that never said anything.
         * Constant-initialised: safe to write before any ROCK init has run.
         */
        std::atomic<bool> s_hostDetourReported{ false };
        std::atomic<bool> s_hostDetourInstalled{ false };
        std::atomic<const char*> s_hostDetourAbsentReason{ nullptr };
        std::mutex s_pendingEquipMutex;
        PendingEquipRequest s_pendingEquipRequest{};
        std::uint64_t s_nextRequestId{ 1 };
        /*
         * Non-zero while THIS thread is inside the host detour's pass-through call
         * to the native EquipObject. An equip the game issues from inside that call
         * is a consequence of the outer one and must not be intercepted a second
         * time. Counted rather than boolean so a nested pass-through (the game
         * re-entering EquipObject, which then passes through again) cannot clear
         * the guard early. Driven by beginHostEquipPassThrough/endHostEquipPassThrough.
         */
        thread_local int t_equipPassThroughDepth = 0;

        [[nodiscard]] RE::TESObjectWEAP::InstanceData* weaponInstanceData(
            RE::TESObjectWEAP* weapon,
            RE::TBO_InstanceData* instanceData) noexcept
        {
            if (instanceData) {
                return static_cast<RE::TESObjectWEAP::InstanceData*>(instanceData);
            }
            return weapon ? &weapon->weaponData : nullptr;
        }

        [[nodiscard]] RE::BGSProjectile* resolveProjectile(
            RE::TESObjectWEAP* weapon,
            RE::TBO_InstanceData* instanceData) noexcept
        {
            auto* data = weaponInstanceData(weapon, instanceData);
            if (!weapon || !data) {
                return nullptr;
            }

            if (data->rangedData && data->rangedData->overrideProjectile) {
                return data->rangedData->overrideProjectile;
            }
            if (weapon->weaponData.rangedData && weapon->weaponData.rangedData->overrideProjectile) {
                return weapon->weaponData.rangedData->overrideProjectile;
            }
            if (data->ammo && data->ammo->data.projectile) {
                return data->ammo->data.projectile;
            }
            if (weapon->weaponData.ammo && weapon->weaponData.ammo->data.projectile) {
                return weapon->weaponData.ammo->data.projectile;
            }
            return nullptr;
        }

        [[nodiscard]] bool playObjectPickupSoundAtReference(RE::TESObjectREFR* ref)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* object = ref ? ref->GetObjectReference() : nullptr;
            if (!player || !object) {
                return false;
            }

            player->PlayPickUpSound(object, false, true);
            return true;
        }

        [[nodiscard]] RE::BSTSmartPointer<RE::TBO_InstanceData> resolveReferenceInstanceData(RE::TESObjectREFR* ref) noexcept
        {
            if (!ref || !ref->extraList) {
                return {};
            }

            const auto* instanceExtra = ref->extraList->GetByType<RE::ExtraInstanceData>();
            return instanceExtra ? instanceExtra->data : RE::BSTSmartPointer<RE::TBO_InstanceData>{};
        }

        [[nodiscard]] const RE::BGSObjectInstanceExtra* resolveReferenceObjectInstanceExtra(RE::TESObjectREFR* ref) noexcept
        {
            if (!ref || !ref->extraList) {
                return nullptr;
            }

            return ref->extraList->GetByType<RE::BGSObjectInstanceExtra>();
        }

        [[nodiscard]] char toLowerAscii(char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        [[nodiscard]] bool containsMolotovToken(const char* text) noexcept
        {
            constexpr char kMolotovToken[] = "molotov";
            if (!text || text[0] == '\0') {
                return false;
            }

            for (const char* cursor = text; *cursor != '\0'; ++cursor) {
                const char* haystack = cursor;
                const char* needle = kMolotovToken;
                while (*needle != '\0' && *haystack != '\0' && toLowerAscii(*haystack) == *needle) {
                    ++haystack;
                    ++needle;
                }
                if (*needle == '\0') {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool keywordFormHasMolotovToken(const RE::BGSKeywordForm* keywordForm) noexcept
        {
            if (!keywordForm || !keywordForm->keywords) {
                return false;
            }

            for (std::uint32_t index = 0; index < keywordForm->numKeywords; ++index) {
                const auto* keyword = keywordForm->keywords[index];
                if (keyword && containsMolotovToken(keyword->formEditorID.c_str())) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool objectInstanceExtraHasMolotovOmod(const RE::BGSObjectInstanceExtra* objectInstanceExtra) noexcept
        {
            if (!objectInstanceExtra || !objectInstanceExtra->values) {
                return false;
            }

            for (const auto& modIndex : objectInstanceExtra->GetIndexData()) {
                if (modIndex.disabled) {
                    continue;
                }

                const auto* omod = RE::TESForm::GetFormByID<RE::BGSMod::Attachment::Mod>(modIndex.objectID);
                if (!omod) {
                    continue;
                }
                if (containsMolotovToken(omod->fullName.c_str()) || containsMolotovToken(omod->model.c_str())) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool isMolotovGrenade(
            RE::TESObjectWEAP* weapon,
            RE::TBO_InstanceData* instanceData,
            RE::BGSProjectile* projectile,
            const RE::BGSObjectInstanceExtra* objectInstanceExtra) noexcept
        {
            if (!weapon || weapon->weaponData.type != RE::WEAPON_TYPE::kGrenade) {
                return false;
            }

            /*
             * WEAPON_TYPE only says "grenade". Molotov identity is authored on
             * the weapon/instance records and, for modded variants, the active
             * OMOD list. Keep the check local and token-based so unknown
             * records fail closed to normal timed-fuse behavior.
             */
            if (containsMolotovToken(weapon->fullName.c_str()) || keywordFormHasMolotovToken(weapon)) {
                return true;
            }
            if (instanceData && keywordFormHasMolotovToken(instanceData->GetKeywordData())) {
                return true;
            }
            if (projectile && (containsMolotovToken(projectile->fullName.c_str()) || containsMolotovToken(projectile->model.c_str()))) {
                return true;
            }
            return objectInstanceExtraHasMolotovOmod(objectInstanceExtra);
        }

        [[nodiscard]] GrenadeKind classifyGrenadeSources(
            RE::TESObjectWEAP* weapon,
            RE::TBO_InstanceData* instanceData,
            RE::BGSProjectile* projectile,
            const RE::BGSObjectInstanceExtra* objectInstanceExtra) noexcept
        {
            if (!weapon || weapon->weaponData.type != RE::WEAPON_TYPE::kGrenade) {
                return GrenadeKind::NotGrenade;
            }
            return isMolotovGrenade(weapon, instanceData, projectile, objectInstanceExtra) ?
                       GrenadeKind::Molotov :
                       GrenadeKind::Generic;
        }

        [[nodiscard]] bool resolveGrenadeRuntimeDataForSources(
            RE::TESObjectWEAP* weapon,
            RE::TBO_InstanceData* instanceData,
            const RE::BGSObjectInstanceExtra* objectInstanceExtra,
            GrenadeRuntimeData& outRuntime) noexcept
        {
            outRuntime = {};
            if (!weapon || weapon->weaponData.type != RE::WEAPON_TYPE::kGrenade) {
                return false;
            }

            auto* projectile = resolveProjectile(weapon, instanceData);
            if (!projectile || !projectile->data.explosionType) {
                return false;
            }

            const GrenadeKind kind = classifyGrenadeSources(weapon, instanceData, projectile, objectInstanceExtra);
            const GrenadeDetonationMode mode =
                kind == GrenadeKind::Molotov ?
                    GrenadeDetonationMode::Impact :
                    GrenadeDetonationMode::TimedFuse;
            const float configuredFuseSeconds = g_rockConfig.rockRealisticGrenadeFuseSeconds;
            const float fuseSeconds = std::isfinite(configuredFuseSeconds) && configuredFuseSeconds > 0.0f ?
                configuredFuseSeconds :
                projectile->data.explosionTimer;
            if (mode == GrenadeDetonationMode::TimedFuse && (!std::isfinite(fuseSeconds) || fuseSeconds <= 0.0f)) {
                return false;
            }

            outRuntime = GrenadeRuntimeData{
                .projectile = projectile,
                .explosion = projectile->data.explosionType,
                .fuseSeconds = fuseSeconds,
                .detonationMode = mode,
            };
            return true;
        }

        [[nodiscard]] InventoryStackMatch findInventoryStack(
            RE::PlayerCharacter* player,
            RE::TESObjectWEAP* weapon,
            const RE::BSTSmartPointer<RE::TBO_InstanceData>& instanceData,
            std::uint32_t requestedStackId) noexcept
        {
            InventoryStackMatch fallback{};
            InventoryStackMatch firstCandidate{};
            std::uint32_t candidateCount = 0;
            if (!player || !weapon || !player->inventoryList) {
                return {};
            }

            const RE::BSAutoReadLock inventoryLock{ player->inventoryList->rwLock };
            for (auto& inventoryItem : player->inventoryList->data) {
                if (inventoryItem.object != weapon) {
                    continue;
                }

                std::uint32_t stackId = 0;
                for (auto* stack = inventoryItem.stackData.get(); stack; stack = stack->nextStack.get(), ++stackId) {
                    const auto count = stack->GetCount();
                    if (count == 0) {
                        continue;
                    }

                    RE::BSTSmartPointer<RE::TBO_InstanceData> stackInstanceData{};
                    if (stack->extra) {
                        if (const auto* instanceExtra = stack->extra->GetByType<RE::ExtraInstanceData>()) {
                            stackInstanceData = instanceExtra->data;
                        }
                    }

                    InventoryStackMatch candidate{
                        .found = true,
                        .exactRequestedStack = requestedStackId != kInvalidStackId && stackId == requestedStackId,
                        .exactInstanceData = instanceData && stackInstanceData.get() == instanceData.get(),
                        .stackId = stackId,
                        .count = count,
                    };

                    if (candidate.exactRequestedStack) {
                        return candidate;
                    }
                    if (candidate.exactInstanceData) {
                        fallback = candidate;
                    }
                    if (!firstCandidate.found) {
                        firstCandidate = candidate;
                    }
                    ++candidateCount;
                }
            }

            if (fallback.found) {
                return fallback;
            }
            if (!instanceData && candidateCount == 1) {
                return firstCandidate;
            }
            return {};
        }

        [[nodiscard]] bool enqueuePendingEquipRequest(
            RE::TESObjectWEAP* weapon,
            const RE::BSTSmartPointer<RE::TBO_InstanceData>& instanceData,
            std::uint32_t stackId,
            const GrenadeRuntimeData& runtime)
        {
            std::scoped_lock lock(s_pendingEquipMutex);
            if (s_pendingEquipRequest.active) {
                return false;
            }

            s_pendingEquipRequest = PendingEquipRequest{
                .active = true,
                .requestId = s_nextRequestId++,
                .weapon = weapon,
                .instanceData = instanceData,
                .stackId = stackId,
                .runtime = runtime,
            };
            return true;
        }

        [[nodiscard]] bool shouldInterceptEquip(
            RE::Actor* actor,
            const RE::BGSObjectInstance& object,
            std::uint32_t number,
            RE::TESObjectWEAP*& outWeapon,
            GrenadeRuntimeData& outRuntime) noexcept
        {
            outWeapon = nullptr;
            outRuntime = {};

            // HARD OFF (Jul 31, project owner). Loose-grenade equip interception
            // replaces the ENGINE'S OWN throwable behaviour: it swallows the
            // equip, spawns a physical grenade in the hand and detonates it from
            // ROCK's own model — resolveGrenadeRuntimeData assigns Molotovs
            // GrenadeDetonationMode::Impact instead of a timed fuse, so any
            // contact detonates them. Throwables must behave exactly as vanilla,
            // so this never runs regardless of configuration.
            //
            // It was already inert in shipped configs (the serviceability gate
            // below needs rockGrabEnabled, i.e. iGrabMode=9) and a tester's
            // 2026-07-30 session confirms it never fired: zero interception log
            // lines and 6 NATIVE "Equipped Throwable Weapon changed" events in
            // the FRIK log. Flipping this to true is the ONLY way back.
            constexpr bool kLooseGrenadeEquipInterceptionEnabled = false;

            if (!kLooseGrenadeEquipInterceptionEnabled ||
                t_equipPassThroughDepth != 0 || !g_rockConfig.rockEnabled || number == 0) {
                return false;
            }

            /*
             * SERVICEABILITY GATE (2026-07-28). Consuming an equip parks it in
             * s_pendingEquip, and the ONLY consumer is
             * PhysicsInteraction::servicePendingLooseGrenadeEquip, reached exclusively from
             * updateGrabInput, which PhysicsInteraction::update gates on
             * g_rockConfig.rockGrabEnabled. When the host keeps the grab pipeline
             * (iGrabMode != 9 -> HostSetGrabOwnership(false) -> rockGrabEnabled = false)
             * nothing ever drains the queue: the first player grenade equip would be
             * swallowed and every later one would hit the "duplicate while a transaction is
             * active" branch, so grenade equipping would be dead for the entire session -
             * from the Pip-Boy/favourites menu and from every host-internal equip path
             * (weapon-equip zone, holster draw, storage re-equip), all of which re-enter
             * this detour because they call the same patched address.
             *
             * Refusing to intercept here leaves the native equip to run exactly as it did
             * before ROCK was hosted, which is the correct and safe fallback.
             */
            if (!g_rockConfig.rockHostGrabOwnershipConfigured ||
                !g_rockConfig.rockGrabEnabled) {
                return false;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || actor != player || !object.object) {
                return false;
            }

            auto* weapon = object.object->As<RE::TESObjectWEAP>();
            if (!isGrenadeWeapon(weapon)) {
                return false;
            }

            outWeapon = weapon;
            static_cast<void>(resolveGrenadeRuntimeData(weapon, object.instanceData.get(), outRuntime));
            return true;
        }

    }

    EquipInterceptionResult tryInterceptEquipObject(
        RE::Actor* actor,
        const RE::BGSObjectInstance* object,
        std::uint32_t stackId,
        std::uint32_t number)
    {
        if (!object || s_equipInterceptionDisabled.load(std::memory_order_acquire)) {
            return EquipInterceptionResult::NotIntercepted;
        }

        RE::TESObjectWEAP* weapon = nullptr;
        GrenadeRuntimeData runtime{};
        if (!shouldInterceptEquip(actor, *object, number, weapon, runtime)) {
            return EquipInterceptionResult::NotIntercepted;
        }

        if (!runtime.projectile || !runtime.explosion ||
            (runtime.detonationMode == GrenadeDetonationMode::TimedFuse &&
                (!std::isfinite(runtime.fuseSeconds) || runtime.fuseSeconds <= 0.0f))) {
            ROCK_LOG_WARN(Hand,
                "Blocked grenade equip because ROCK could not resolve projectile/explosion/fuse data: weapon={:08X} stack={}",
                weapon ? weapon->GetFormID() : 0,
                stackId);
            return EquipInterceptionResult::ConsumedBlocked;
        }

        if (enqueuePendingEquipRequest(weapon, object->instanceData, stackId, runtime)) {
            ROCK_LOG_INFO(Hand,
                "Queued loose grenade equip interception: weapon={:08X} projectile={:08X} explosion={:08X} stack={} mode={} fuse={:.3f}s",
                weapon ? weapon->GetFormID() : 0,
                runtime.projectile ? runtime.projectile->GetFormID() : 0,
                runtime.explosion ? runtime.explosion->GetFormID() : 0,
                stackId,
                detonationModeName(runtime.detonationMode),
                runtime.fuseSeconds);
            return EquipInterceptionResult::ConsumedEquipped;
        }

        /*
         * The first request remains authoritative through its attach
         * terminal state. Report duplicate menu presses as handled so
         * native equip cannot run, but never preserve them for replay.
         */
        ROCK_LOG_INFO(Hand,
            "Ignored duplicate loose grenade equip while one transaction is active: weapon={:08X} stack={}",
            weapon ? weapon->GetFormID() : 0,
            stackId);
        return EquipInterceptionResult::ConsumedEquipped;
    }

    void beginHostEquipPassThrough() noexcept
    {
        ++t_equipPassThroughDepth;
    }

    void endHostEquipPassThrough() noexcept
    {
        if (t_equipPassThroughDepth > 0) {
            --t_equipPassThroughDepth;
        }
    }

    void setHostEquipObjectDetourInstalled(bool installed, const char* reasonWhenAbsent)
    {
        s_hostDetourInstalled.store(installed, std::memory_order_release);
        s_hostDetourAbsentReason.store(installed ? nullptr : reasonWhenAbsent, std::memory_order_release);
        s_hostDetourReported.store(true, std::memory_order_release);
    }

    bool isHostEquipObjectDetourInstalled()
    {
        return s_hostDetourInstalled.load(std::memory_order_acquire);
    }

    bool installEquipHook()
    {
        if (s_equipHookInstalled.load(std::memory_order_acquire)) {
            return true;
        }

        /*
         * ARBITRATION, NOT PATCHING (2026-07-28).
         *
         * Address/prefix provenance (Ghidra, Fallout4VR.exe 1.2.72, image base
         * 0x140000000): 0x140E6FEA0 is the exact entry of
         * ActorEquipManager::EquipObject (679 bytes, 11 parameters; corroborated
         * by address-library ID 988029). The 17-byte prefix above is a whole-
         * instruction boundary --
         *   mov r11,rsp / mov [r11+10h],rdx / push rbp / push rsi / push r12 /
         *   push r15 / lea rbp,[r11-27h]  -- next instruction is sub rsp,0A8h.
         *
         * ROCK used to install its own raw entry detour here. In this build that
         * could never work: Heisenberg's HookEquipObject detour is installed first
         * (Hooks::Install runs before rock::HostLoad), an entry can hold exactly
         * ONE raw detour, and ROCK's installer then memcmp'd the prologue the host
         * had already replaced and failed. Loose-grenade interception consequently
         * never executed once. Swapping the install order does not help either --
         * the host's own installer only recognises pristine prologue patterns and
         * would refuse to install, breaking holotape/consumable-to-hand instead.
         *
         * So there is no second detour. The host's detour calls
         * tryInterceptEquipObject() directly. All this function does is verify a
         * host detour exists and report what is actually on the entry.
         */
        const bool hostDetour = s_hostDetourInstalled.load(std::memory_order_acquire);
        const bool hostReported = s_hostDetourReported.load(std::memory_order_acquire);
        const auto entryAddress = REL::Relocation<std::uintptr_t>{ REL::Offset(kFuncActorEquipManagerEquipObject) }.address();

        // FINDINGS ONLY: address, what is there now, what a pristine entry looks
        // like, and where the branch that replaced it goes. No theory about who.
        const std::string entryReport = hook_diagnostics::describePrefixMismatch(
            entryAddress,
            kActorEquipManagerEquipObjectExpectedPrefix.data(),
            kActorEquipManagerEquipObjectExpectedPrefix.size());

        s_equipHookInstalled.store(hostDetour, std::memory_order_release);
        s_equipInterceptionDisabled.store(!hostDetour, std::memory_order_release);

        if (hostDetour) {
            /*
             * HostLoad calls this before Heisenberg applies iGrabMode through
             * HostSetGrabOwnership. Do not call the feature ARMED while the
             * serviceability decision is still pending: in the normal
             * keyframed-host configuration the host immediately sets
             * rockGrabEnabled=false, and every grenade equip correctly passes
             * through to native handling.
             */
            if (!g_rockConfig.rockHostGrabOwnershipConfigured) {
                ROCK_LOG_INFO(Init,
                    "Loose-grenade equip seam CONNECTED to the host's ActorEquipManager::EquipObject detour, but activation "
                    "is PENDING the host's iGrabMode ownership decision (ROCK installs no hook of its own here). No grenade "
                    "interception is claimed yet. Entry state: {}.",
                    entryReport);
            } else if (g_rockConfig.rockEnabled && g_rockConfig.rockGrabEnabled) {
                ROCK_LOG_INFO(Init,
                    "Loose-grenade equip interception ARMED as a callee of the host's ActorEquipManager::EquipObject detour "
                    "(rockEnabled=true, rockGrabEnabled=true). Menu/favourites grenade equips can become physical loose "
                    "grenades in the hand. Entry state: {}.",
                    entryReport);
            } else {
                ROCK_LOG_INFO(Init,
                    "Loose-grenade equip seam CONNECTED but interception is INACTIVE by configuration "
                    "(rockEnabled={}, rockGrabEnabled={}); grenade equips pass through to native handling. Entry state: {}.",
                    g_rockConfig.rockEnabled,
                    g_rockConfig.rockGrabEnabled,
                    entryReport);
            }
            return true;
        }

        /*
         * FAIL-SAFE: no detour means nothing can ever feed the pending-equip
         * queue, so latch the feature off rather than leave consumers waiting on a
         * request that cannot arrive.
         */
        clearPendingEquipRequest();
        const char* reason = s_hostDetourAbsentReason.load(std::memory_order_acquire);
        ROCK_LOG_ERROR(Init,
            "FEATURE DISABLED: loose-grenade equip interception is OFF for this session -- {}. Entry state: {}. "
            "Consequence: equipping a grenade from the Pip-Boy or favourites runs the NATIVE equip, so it becomes a held "
            "throwable instead of a physical grenade ROCK can place in the hand.",
            hostReported ? (reason ? reason : "the host reported that its EquipObject detour is not installed")
                         : "the host never reported an EquipObject detour, so ROCK has no entry point to be called from",
            entryReport);
        return false;
    }

    bool isEquipInterceptionActive()
    {
        return s_equipHookInstalled.load(std::memory_order_acquire) &&
               !s_equipInterceptionDisabled.load(std::memory_order_acquire) &&
               s_hostDetourInstalled.load(std::memory_order_acquire) &&
               g_rockConfig.rockEnabled &&
               g_rockConfig.rockHostGrabOwnershipConfigured &&
               g_rockConfig.rockGrabEnabled;
    }

    bool isGrenadeWeapon(const RE::TESObjectWEAP* weapon) noexcept
    {
        // WEAPON_TYPE is a single-valued enum stored in EnumSet; bitmask any() makes guns/mines alias grenades.
        return weapon && weapon->weaponData.type == RE::WEAPON_TYPE::kGrenade;
    }

    bool isGrenadeRef(RE::TESObjectREFR* ref) noexcept
    {
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        auto* weapon = base ? base->As<RE::TESObjectWEAP>() : nullptr;
        return isGrenadeWeapon(weapon);
    }

    GrenadeKind classifyGrenadeRef(RE::TESObjectREFR* ref) noexcept
    {
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        auto* weapon = base ? base->As<RE::TESObjectWEAP>() : nullptr;
        if (!isGrenadeWeapon(weapon)) {
            return GrenadeKind::NotGrenade;
        }

        const auto instanceData = resolveReferenceInstanceData(ref);
        auto* projectile = resolveProjectile(weapon, instanceData.get());
        const auto* objectInstanceExtra = resolveReferenceObjectInstanceExtra(ref);
        return classifyGrenadeSources(weapon, instanceData.get(), projectile, objectInstanceExtra);
    }

    bool resolveGrenadeRuntimeData(RE::TESObjectWEAP* weapon, RE::TBO_InstanceData* instanceData, GrenadeRuntimeData& outRuntime) noexcept
    {
        return resolveGrenadeRuntimeDataForSources(weapon, instanceData, nullptr, outRuntime);
    }

    bool resolveGrenadeRuntimeDataForReference(RE::TESObjectREFR* ref, GrenadeRuntimeData& outRuntime) noexcept
    {
        outRuntime = {};
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        auto* weapon = base ? base->As<RE::TESObjectWEAP>() : nullptr;
        const auto instanceData = resolveReferenceInstanceData(ref);
        const auto* objectInstanceExtra = resolveReferenceObjectInstanceExtra(ref);
        return resolveGrenadeRuntimeDataForSources(weapon, instanceData.get(), objectInstanceExtra, outRuntime);
    }

    bool copyPendingEquipRequest(PendingEquipRequest& outRequest)
    {
        // FAIL-SAFE: with the equip hook uninstalled the queue can never be fed,
        // so report it permanently empty rather than letting callers idle on a
        // feature that has no implementation behind it.
        if (s_equipInterceptionDisabled.load(std::memory_order_acquire)) {
            outRequest = {};
            return false;
        }

        std::scoped_lock lock(s_pendingEquipMutex);
        if (!s_pendingEquipRequest.active) {
            outRequest = {};
            return false;
        }

        outRequest = s_pendingEquipRequest;
        return true;
    }

    bool hasPendingEquipRequest()
    {
        if (s_equipInterceptionDisabled.load(std::memory_order_acquire)) {
            return false;  // FAIL-SAFE, see copyPendingEquipRequest
        }

        std::scoped_lock lock(s_pendingEquipMutex);
        return s_pendingEquipRequest.active;
    }

    void discardPendingEquipRequest(std::uint64_t requestId)
    {
        std::scoped_lock lock(s_pendingEquipMutex);
        if (s_pendingEquipRequest.active && s_pendingEquipRequest.requestId == requestId) {
            s_pendingEquipRequest = {};
        }
    }

    void clearPendingEquipRequest()
    {
        std::scoped_lock lock(s_pendingEquipMutex);
        s_pendingEquipRequest = {};
    }

    DropResult dropPendingEquipRequestToWorld(
        const PendingEquipRequest& request,
        const RE::NiPoint3& dropLocation)
    {
        DropResult result{};
        result.stackId = request.stackId;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            result.reason = "missing-player";
            return result;
        }
        if (!request.active || !request.weapon) {
            result.reason = "missing-request";
            return result;
        }
        if (!player->inventoryList) {
            result.reason = "missing-inventory-list";
            return result;
        }

        const auto stack = findInventoryStack(player, request.weapon, request.instanceData, request.stackId);
        if (!stack.found || stack.count == 0 || stack.stackId == kInvalidStackId) {
            result.reason = "inventory-stack-not-found";
            return result;
        }

        RE::TESObjectREFR::RemoveItemData removeData(request.weapon, 1);
        removeData.reason = RE::ITEM_REMOVE_REASON::KDropping;
        removeData.dropLoc = &dropLocation;
        removeData.stackData.push_back(stack.stackId);

        result.stackId = stack.stackId;
        result.handle = player->RemoveItem(removeData);
        if (!result.handle) {
            result.reason = "remove-item-failed";
            return result;
        }

        const auto droppedRef = result.handle.get();
        result.droppedRef = droppedRef.get();
        if (!result.droppedRef) {
            /*
             * RemoveItem already committed the inventory-to-world transfer.
             * The reference/3D can resolve asynchronously, so retain the
             * handle and let the force-grab transaction wait for it.
             */
            result.success = true;
            result.reason = "dropped-reference-pending";
            return result;
        }

        result.success = true;
        result.reason = "dropped";
        return result;
    }

    bool createExplosionAtReference(RE::TESObjectREFR* ref, RE::BGSExplosion* explosion)
    {
        if (!ref || !explosion) {
            return false;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        auto* cell = ref->GetParentCell();
        if (!dataHandler || !cell) {
            return false;
        }

        RE::NEW_REFR_DATA data{};
        data.location = ref->Get3D() ? ref->Get3D()->world.translate : ref->data.location;
        data.direction = ref->data.angle;
        data.object = explosion;
        data.interior = cell->IsInterior() ? cell : nullptr;
        data.world = cell->IsExterior() ? cell->worldSpace : nullptr;
        data.clearStillLoadingFlag = true;
        data.initializeScripts = true;

        const auto handle = dataHandler->CreateReferenceAtLocation(data);
        return handle.get() != nullptr;
    }

    const char* detonationModeName(GrenadeDetonationMode mode) noexcept
    {
        switch (mode) {
        case GrenadeDetonationMode::TimedFuse:
            return "timed-fuse";
        case GrenadeDetonationMode::Impact:
            return "impact";
        default:
            return "unknown";
        }
    }

    bool playPinPulledFeedbackAtReference(RE::TESObjectREFR* ref)
    {
        return playObjectPickupSoundAtReference(ref);
    }

    bool returnDroppedReferenceToInventory(RE::TESObjectREFR* ref)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !ref || ref->IsDeleted() || ref->IsDisabled()) {
            return false;
        }

        /*
         * Native activation is the same locally established loose-weapon
         * pickup transfer used by ROCK's equip path. It moves this exact
         * reference (including its instance data) back into player inventory.
         */
        return ref->ActivateRef(player, nullptr, 1, false, false, false);
    }

    void disableAndDeleteReference(RE::TESObjectREFR* ref)
    {
        if (!ref || ref->IsDeleted()) {
            return;
        }
        if (!ref->IsDisabled()) {
            ref->Disable();
        }
        ref->SetWantsDelete(true);
    }
}
