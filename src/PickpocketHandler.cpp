#include "PickpocketHandler.h"

#include <random>

#include "Config.h"
#include "DropToHand.h"
#include "F4VROffsets.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "Hooks.h"
#include "VRInput.h"
#include "WandNodeHelper.h"

#include <RE/Bethesda/Settings.h>
#include <RE/Bethesda/ActorValueInfo.h>
#include <RE/Bethesda/FormComponents.h>
#include <RE/Bethesda/BGSInventoryItem.h>
#include <RE/Bethesda/BGSEntryPoint.h>
#include <RE/Bethesda/BGSStoryEventManager.h>

namespace heisenberg
{
    // =========================================================================
    // HELPERS
    // =========================================================================

    static std::mt19937& GetRNG()
    {
        static std::mt19937 rng(std::random_device{}());
        return rng;
    }

    static const char* GetItemName(RE::TESBoundObject* obj)
    {
        if (!obj) return "???";
        auto* fullName = obj->As<RE::TESFullName>();
        if (fullName && fullName->GetFullName() && fullName->GetFullName()[0]) {
            return fullName->GetFullName();
        }
        return "???";
    }

    static bool IsQuestItem(RE::TESBoundObject* obj)
    {
        if (!obj) return false;
        return (obj->GetFormFlags() & 0x400) != 0;
    }

    // =========================================================================
    // CLEAR STATE
    // =========================================================================

    void PickpocketHandler::ClearState()
    {
        if (_state == State::Browsing) {
            // Don't try to return items during save/load - handles are invalid
            _previewRefHandle = {};
            _targetNPCHandle = {};
        }
        _state = State::Idle;
        _candidates.clear();
        _currentIndex = 0;
        _cooldownTimer = 0.0f;
    }

    // =========================================================================
    // DETECTION CHECK
    // =========================================================================

    bool PickpocketHandler::HasNPCDetectedPlayer(RE::Actor* npc) const
    {
        if (!npc) return true;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return true;

        // RequestDetectionLevel: negative = hidden, 0+ = detected
        int detectionLevel = npc->RequestDetectionLevel(static_cast<RE::Actor*>(player));
        return detectionLevel >= DETECTION_THRESHOLD;
    }

    // =========================================================================
    // UPDATE — Main frame tick
    // =========================================================================

    void PickpocketHandler::Update(float deltaTime)
    {
        if (!g_config.enablePickpocket) {
            if (_state == State::Browsing) {
                CancelBrowse();
            }
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Tick cooldown
        if (_cooldownTimer > 0.0f) {
            _cooldownTimer -= deltaTime;
        }

        // =================================================================
        // BROWSING STATE
        // =================================================================
        if (_state == State::Browsing) {
            // Validate target NPC still exists
            auto npcPtr = _targetNPCHandle.get();
            RE::Actor* npc = npcPtr ? npcPtr->As<RE::Actor>() : nullptr;
            if (!npc || npc->IsDead(false)) {
                spdlog::info("[PICKPOCKET] Target NPC lost or dead — cancelling browse");
                CancelBrowse();
                return;
            }

            // Cancel if player stopped sneaking
            if (!player->IsSneaking()) {
                spdlog::info("[PICKPOCKET] Player stopped sneaking — cancelling browse");
                CancelBrowse();
                return;
            }

            // Cancel if NPC detected the player
            if (HasNPCDetectedPlayer(npc)) {
                spdlog::info("[PICKPOCKET] NPC detected player — cancelling browse");
                Hooks::ShowHUDMessageDirect("You've been noticed!");
                g_vrInput.TriggerHaptic(true, 30000);
                g_vrInput.TriggerHaptic(false, 30000);
                CancelBrowse();
                return;
            }

            // Check grip press to cycle to next item
            if (g_vrInput.JustPressed(_browsingIsLeft, VRButton::Grip)) {
                CycleNextItem();
            }

            // Check distance: if hand moved far enough from NPC, commit the steal
            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            if (playerNodes) {
                RE::NiNode* wandNode = GetWandNode(playerNodes, _browsingIsLeft);
                if (wandNode) {
                    RE::NiPoint3 handPos = wandNode->world.translate;
                    float dx = handPos.x - _npcPositionAtStart.x;
                    float dy = handPos.y - _npcPositionAtStart.y;
                    float dz = handPos.z - _npcPositionAtStart.z;
                    float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq > STEAL_DISTANCE * STEAL_DISTANCE) {
                        spdlog::info("[PICKPOCKET] Hand moved {:.1f} units from NPC — committing steal",
                            std::sqrt(distSq));
                        CommitSteal();
                        return;
                    }
                }
            }

            return;
        }

        // =================================================================
        // IDLE STATE — Check for new pickpocket initiation
        // =================================================================

        if (!player->IsSneaking()) {
            return;
        }

        if (_cooldownTimer > 0.0f) {
            return;
        }

        auto& grabMgr = GrabManager::GetSingleton();

        // Ensure ViewCaster singletons are initialized
        FindViewCasterSingleton();

        // Check each hand for ViewCaster target
        for (int hand = 0; hand < 2; ++hand) {
            bool isLeft = (hand == 0);

            // Skip if this hand is already grabbing something
            if (grabMgr.IsGrabbing(isLeft)) {
                continue;
            }

            // Get the ViewCaster target for this hand
            RE::ObjectRefHandle handle = GetVRWandTargetHandle(isLeft);
            if (!handle) {
                continue;
            }

            RE::NiPointer<RE::TESObjectREFR> refrPtr = handle.get();
            if (!refrPtr) {
                continue;
            }

            // Must be a living NPC
            RE::Actor* actor = refrPtr->As<RE::Actor>();
            if (!actor || actor == player || actor->IsDead(false)) {
                continue;
            }

            // Must not have detected the player
            if (HasNPCDetectedPlayer(actor)) {
                continue;
            }

            // Check for grip press to enter browse mode
            if (g_vrInput.JustPressed(isLeft, VRButton::Grip)) {
                spdlog::info("[PICKPOCKET] Grip pressed on {} hand, target NPC {:08X} — entering browse mode",
                    isLeft ? "left" : "right", actor->formID);
                EnterBrowseMode(actor, isLeft);
                return;
            }
        }
    }

    // =========================================================================
    // BUILD CANDIDATE LIST
    // =========================================================================

    bool PickpocketHandler::BuildCandidateList(RE::Actor* npc)
    {
        _candidates.clear();
        _currentIndex = 0;

        if (!npc || !npc->inventoryList) return false;

        for (auto& item : npc->inventoryList->data) {
            if (!item.object) continue;

            auto* boundObj = item.object;

            if (IsQuestItem(boundObj)) continue;

            // Get item count from stack data
            std::int32_t totalCount = 0;
            auto* stack = item.stackData.get();
            while (stack) {
                totalCount += stack->count;
                stack = stack->nextStack.get();
            }
            if (totalCount <= 0) continue;

            // Only allow stealable form types
            auto formType = boundObj->GetFormType();
            switch (formType) {
                case RE::ENUM_FORM_ID::kWEAP:
                case RE::ENUM_FORM_ID::kARMO:
                case RE::ENUM_FORM_ID::kALCH:
                case RE::ENUM_FORM_ID::kAMMO:
                case RE::ENUM_FORM_ID::kBOOK:
                case RE::ENUM_FORM_ID::kMISC:
                case RE::ENUM_FORM_ID::kKEYM:
                case RE::ENUM_FORM_ID::kNOTE:
                    break;
                default:
                    continue;
            }

            // Skip equipped weapons/armor (heuristic: count <= 1 means likely equipped)
            if ((formType == RE::ENUM_FORM_ID::kWEAP || formType == RE::ENUM_FORM_ID::kARMO) && totalCount <= 1) {
                continue;
            }

            // For ammo: steal a portion of the stack
            std::int32_t stealCount = 1;
            if (formType == RE::ENUM_FORM_ID::kAMMO) {
                stealCount = (std::max)(1, totalCount / 4);
            }

            _candidates.push_back({ boundObj, stealCount });
        }

        spdlog::info("[PICKPOCKET] Built candidate list: {} stealable items from NPC {:08X}",
            _candidates.size(), npc->formID);
        return !_candidates.empty();
    }

    // =========================================================================
    // ENTER BROWSE MODE
    // =========================================================================

    void PickpocketHandler::EnterBrowseMode(RE::Actor* npc, bool isLeft)
    {
        if (!npc) return;

        if (!BuildCandidateList(npc)) {
            spdlog::info("[PICKPOCKET] NPC {:08X} has no stealable items", npc->formID);
            Hooks::ShowHUDMessageDirect("Nothing to steal");
            g_vrInput.TriggerHaptic(isLeft, 5000);
            _cooldownTimer = STEAL_COOLDOWN;
            return;
        }

        _state = State::Browsing;
        _targetNPCHandle = npc->GetHandle();
        _browsingIsLeft = isLeft;
        _npcPositionAtStart = npc->data.location;
        _currentIndex = 0;

        // Spawn the first item
        SpawnPreviewItem();

        auto& candidate = _candidates[_currentIndex];
        std::string msg = std::string("[Pickpocket] ") + GetItemName(candidate.object);
        if (_candidates.size() > 1) {
            msg += "  [Grip: next item]";
        }
        Hooks::ShowHUDMessageDirect(msg.c_str());
    }

    // =========================================================================
    // CYCLE NEXT ITEM
    // =========================================================================

    void PickpocketHandler::CycleNextItem()
    {
        if (_candidates.empty()) return;

        // Return current preview to NPC
        ReturnPreviewToNPC();

        // Advance to next candidate (wrap around)
        _currentIndex = (_currentIndex + 1) % static_cast<int>(_candidates.size());

        // Spawn new preview
        SpawnPreviewItem();

        auto& candidate = _candidates[_currentIndex];
        std::string msg = std::string("[Pickpocket] ") + GetItemName(candidate.object);
        msg += " (" + std::to_string(_currentIndex + 1) + "/" + std::to_string(_candidates.size()) + ")";
        Hooks::ShowHUDMessageDirect(msg.c_str());

        // Light haptic to confirm cycle
        g_vrInput.TriggerHaptic(_browsingIsLeft, 3000);
    }

    // =========================================================================
    // SPAWN PREVIEW ITEM — Remove from NPC, drop at hand, grab it
    // =========================================================================

    void PickpocketHandler::SpawnPreviewItem()
    {
        if (_currentIndex < 0 || _currentIndex >= static_cast<int>(_candidates.size())) return;

        auto npcPtr = _targetNPCHandle.get();
        if (!npcPtr) return;

        auto& candidate = _candidates[_currentIndex];

        // Get hand position for spawn location
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) return;

        RE::NiNode* wandNode = GetWandNode(playerNodes, _browsingIsLeft);
        if (!wandNode) return;

        RE::NiPoint3 dropPos = wandNode->world.translate;
        RE::NiPoint3 dropRot(0.0f, 0.0f, 0.0f);

        // Remove item from NPC inventory and drop it at the hand position
        RE::TESObjectREFR::RemoveItemData removeData(candidate.object, candidate.count);
        removeData.reason = RE::ITEM_REMOVE_REASON::KDropping;
        removeData.dropLoc = &dropPos;
        removeData.rotate = &dropRot;

        Hooks::SetSuppressHUDMessages(true);
        RE::ObjectRefHandle droppedHandle = npcPtr->RemoveItem(removeData);
        Hooks::SetSuppressHUDMessages(false);

        if (!droppedHandle) {
            spdlog::warn("[PICKPOCKET] Failed to spawn preview item '{}'", GetItemName(candidate.object));
            return;
        }

        _previewRefHandle = droppedHandle;

        auto* droppedRefr = droppedHandle.get().get();
        if (droppedRefr) {
            spdlog::info("[PICKPOCKET] Spawned preview: '{}' x{} (ref {:08X})",
                GetItemName(candidate.object), candidate.count, droppedRefr->formID);

            // Grab the preview item instantly (sticky so it doesn't drop)
            auto& grabMgr = GrabManager::GetSingleton();
            grabMgr.StartGrabOnRef(droppedRefr, _browsingIsLeft,
                /*stickyGrab=*/true, /*instantGrab=*/true,
                /*skipWeaponEquip=*/true, /*forceOffset=*/false);
        }
    }

    // =========================================================================
    // RETURN PREVIEW TO NPC — Give item back and clean up world ref
    // =========================================================================

    void PickpocketHandler::ReturnPreviewToNPC()
    {
        auto npcPtr = _targetNPCHandle.get();
        auto previewPtr = _previewRefHandle.get();

        if (!previewPtr || !npcPtr) {
            _previewRefHandle = {};
            return;
        }

        auto* previewRefr = previewPtr.get();
        if (!previewRefr) {
            _previewRefHandle = {};
            return;
        }

        if (_currentIndex >= 0 && _currentIndex < static_cast<int>(_candidates.size())) {
            auto& candidate = _candidates[_currentIndex];

            spdlog::info("[PICKPOCKET] Returning '{}' x{} to NPC {:08X}",
                GetItemName(candidate.object), candidate.count, npcPtr->formID);

            // End the grab first
            auto& grabMgr = GrabManager::GetSingleton();
            if (grabMgr.IsGrabbing(_browsingIsLeft)) {
                grabMgr.EndGrab(_browsingIsLeft);
            }

            // Add the item back to NPC's inventory
            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
            AddObjectToContainer(
                npcPtr.get(), candidate.object, &nullExtra,
                candidate.count, nullptr, 0);

            // Clean up the world reference
            SafeDisable(previewRefr);
        }

        _previewRefHandle = {};
    }

    // =========================================================================
    // COMMIT STEAL — Run pickpocket formula, apply outcome
    // =========================================================================

    void PickpocketHandler::CommitSteal()
    {
        if (_currentIndex < 0 || _currentIndex >= static_cast<int>(_candidates.size())) {
            CancelBrowse();
            return;
        }

        auto npcPtr = _targetNPCHandle.get();
        if (!npcPtr) {
            CancelBrowse();
            return;
        }
        RE::Actor* npc = npcPtr->As<RE::Actor>();
        if (!npc) {
            CancelBrowse();
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            CancelBrowse();
            return;
        }

        auto& candidate = _candidates[_currentIndex];
        auto* itemObj = candidate.object;
        auto itemCount = candidate.count;

        // --- Get item value ---
        std::int32_t perUnitValue = RE::TESValueForm::GetFormValue(itemObj, nullptr);
        if (perUnitValue <= 0) perUnitValue = 1;
        std::uint32_t stealValue = static_cast<std::uint32_t>((std::max)(1, perUnitValue * itemCount));

        // --- Get item weight ---
        float itemWeight = RE::TESWeightForm::GetFormWeight(itemObj, nullptr);
        float totalWeight = itemWeight * static_cast<float>(itemCount);

        // --- Get actor values ---
        auto* avSingleton = RE::ActorValue::GetSingleton();
        float playerAgility = 5.0f;
        float npcPerception = 5.0f;

        if (avSingleton) {
            if (avSingleton->agility) {
                playerAgility = player->GetActorValue(*avSingleton->agility);
            }
            if (avSingleton->perception) {
                npcPerception = npc->GetActorValue(*avSingleton->perception);
            }
        }

        bool npcInCombat = npc->IsInCombat();

        // --- Call native pickpocket formula ---
        int chance = AiFormulas_ComputePickpocketSuccess(
            playerAgility, npcPerception, stealValue, totalWeight,
            static_cast<RE::Actor*>(player), npc,
            static_cast<RE::TESForm*>(itemObj), npcInCombat);

        const char* itemName = GetItemName(itemObj);
        spdlog::info("[PICKPOCKET] Stealing '{}' x{}, value={}, weight={:.1f}, "
            "Agi={:.0f}, Per={:.0f}, combat={}, chance={}%",
            itemName, itemCount, stealValue, totalWeight,
            playerAgility, npcPerception, npcInCombat, chance);

        // --- Roll ---
        std::uniform_int_distribution<int> dist(0, 100);
        int roll = dist(GetRNG());

        if (roll >= chance) {
            // === FAILED ===
            spdlog::warn("[PICKPOCKET] FAILED (rolled {} >= {})", roll, chance);

            // Return item to NPC
            ReturnPreviewToNPC();

            // Trigger alarm
            Actor_PickpocketAlarm(
                static_cast<RE::Actor*>(player), npc, nullptr, 0);

            // Fire story event (failure)
            BGSPickpocketEvent failEvent(npc, 0);
            if (auto* storyMgr = RE::BGSStoryEventManager::GetSingleton()) {
                storyMgr->AddEvent(failEvent);
            }

            Hooks::ShowHUDMessageDirect("You've been caught pickpocketing!");
            g_vrInput.TriggerHaptic(true, 50000);
            g_vrInput.TriggerHaptic(false, 50000);
        } else {
            // === SUCCESS ===
            spdlog::info("[PICKPOCKET] SUCCESS (rolled {} < {}): Stealing '{}' x{}",
                roll, chance, itemName, itemCount);

            // End the grab — item drops to ground or stays in hand depending on mode
            auto& grabMgr = GrabManager::GetSingleton();
            if (grabMgr.IsGrabbing(_browsingIsLeft)) {
                grabMgr.EndGrab(_browsingIsLeft);
            }

            // The item is already in the world from SpawnPreviewItem (removed from NPC).
            // Now we need to pick it up properly into the player's inventory with stolen flag.
            auto previewPtr = _previewRefHandle.get();
            if (previewPtr) {
                auto* previewRefr = previewPtr.get();
                if (previewRefr) {
                    // Store the world item into player inventory with stolen flag
                    RE::TESObjectREFR::RemoveItemData storeData(itemObj, itemCount);
                    storeData.reason = RE::ITEM_REMOVE_REASON::kStealing;
                    storeData.a_otherContainer = static_cast<RE::TESObjectREFR*>(player);

                    // The world ref is the "container" we remove from — use ActivateRef
                    // or manually add + disable. Safest: add to player directly, disable ref.
                    RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                    AddObjectToContainer(
                        static_cast<RE::TESObjectREFR*>(player),
                        itemObj, &nullExtra, itemCount,
                        npcPtr.get(),  // oldContainer = NPC (marks as stolen)
                        static_cast<std::uint32_t>(RE::ITEM_REMOVE_REASON::kStealing));

                    // Clean up world reference
                    SafeDisable(previewRefr);
                }
            }

            _previewRefHandle = {};

            // Fire story event (success)
            BGSPickpocketEvent successEvent(npc, 1);
            if (auto* storyMgr = RE::BGSStoryEventManager::GetSingleton()) {
                storyMgr->AddEvent(successEvent);
            }

            // Show message
            std::string msg = std::string("[Stolen] ") + itemName;
            if (itemCount > 1) {
                msg += " x" + std::to_string(itemCount);
            }
            Hooks::ShowHUDMessageDirect(msg.c_str());

            // Haptic feedback
            g_vrInput.TriggerHaptic(_browsingIsLeft, 30000);
        }

        // Reset state
        _state = State::Idle;
        _candidates.clear();
        _currentIndex = 0;
        _targetNPCHandle = {};
        _cooldownTimer = STEAL_COOLDOWN;
    }

    // =========================================================================
    // CANCEL BROWSE — Return item and exit cleanly
    // =========================================================================

    void PickpocketHandler::CancelBrowse()
    {
        spdlog::info("[PICKPOCKET] Cancelling browse mode");

        ReturnPreviewToNPC();

        _state = State::Idle;
        _candidates.clear();
        _currentIndex = 0;
        _targetNPCHandle = {};
        _cooldownTimer = 0.5f;  // Short cooldown to prevent accidental re-entry
    }

}
