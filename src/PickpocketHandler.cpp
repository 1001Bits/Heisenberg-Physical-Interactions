#include "PickpocketHandler.h"

#include <random>

#include "Config.h"
#include "DropToHand.h"
#include "F4VROffsets.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "Hooks.h"
#include "VRInput.h"

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

    /// Get a display name for a TESBoundObject (for HUD messages).
    static const char* GetItemName(RE::TESBoundObject* obj)
    {
        if (!obj) return "???";
        auto* fullName = obj->As<RE::TESFullName>();
        if (fullName && fullName->GetFullName() && fullName->GetFullName()[0]) {
            return fullName->GetFullName();
        }
        return "???";
    }

    /// Check if a form is a quest item (shouldn't be stolen).
    /// We check the form flags directly since TESBoundObject doesn't expose IsQuestObject.
    static bool IsQuestItem(RE::TESBoundObject* obj)
    {
        if (!obj) return false;
        // Quest items have form flag bit 0x400 (kQuestObject)
        // We can also check via TESForm::IsQuestObject on the BGSInventoryItem level,
        // but for a simple check on the base form, form flags work.
        return (obj->GetFormFlags() & 0x400) != 0;
    }

    // =========================================================================
    // UPDATE — ViewCaster-based NPC detection
    // =========================================================================

    void PickpocketHandler::Update(float deltaTime)
    {
        if (!g_config.enablePickpocket) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Only allow pickpocketing while sneaking
        if (!player->IsSneaking()) {
            return;
        }

        // Tick down cooldown
        if (_cooldownTimer > 0.0f) {
            _cooldownTimer -= deltaTime;
        }

        auto& grabMgr = GrabManager::GetSingleton();

        // Ensure ViewCaster singletons are initialized
        FindViewCasterSingleton();

        // Process each hand's ViewCaster target
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

            // Check if it's a living Actor (valid pickpocket target)
            RE::Actor* actor = refrPtr->As<RE::Actor>();
            if (!actor || actor == player || actor->IsDead(false)) {
                continue;
            }

            // Check for grip press to attempt pickpocket
            if (g_vrInput.JustPressed(isLeft, VRButton::Grip)) {
                if (_cooldownTimer <= 0.0f) {
                    spdlog::info("[PICKPOCKET] Grip pressed on {} hand, ViewCaster target {:08X}",
                        isLeft ? "left" : "right", actor->formID);

                    bool success = TryPickpocket(actor, isLeft);
                    _cooldownTimer = STEAL_COOLDOWN;

                    if (success) {
                        spdlog::info("[PICKPOCKET] Success!");
                    } else {
                        spdlog::info("[PICKPOCKET] Failed or no items available");
                    }
                } else {
                    spdlog::debug("[PICKPOCKET] Grip pressed but on cooldown ({:.2f}s remaining)", _cooldownTimer);
                }
            }
        }
    }

    // =========================================================================
    // TRY PICKPOCKET — Uses native AiFormulas::ComputePickpocketSuccess
    // and Actor::PickpocketAlarm for identical behavior to the base game.
    // =========================================================================

    bool PickpocketHandler::TryPickpocket(RE::Actor* npc, bool isLeft)
    {
        if (!npc) return false;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        // Select a random item from the NPC's inventory
        RE::TESBoundObject* itemObj = nullptr;
        std::int32_t itemCount = 0;
        RE::BGSInventoryItem* invItem = nullptr;
        if (!SelectRandomItem(npc, itemObj, itemCount, invItem)) {
            spdlog::info("[PICKPOCKET] NPC {:08X} has no stealable items", npc->formID);
            Hooks::ShowHUDMessageDirect("Nothing to steal");
            g_vrInput.TriggerHaptic(isLeft, 5000);
            return false;
        }

        // --- Get real item value (native: BGSInventoryItem::GetInventoryValue) ---
        std::int32_t perUnitValue = 1;
        if (invItem) {
            perUnitValue = invItem->GetInventoryValue(0, true);
        }
        if (perUnitValue <= 0) {
            // Fallback: try TESValueForm::GetFormValue
            perUnitValue = RE::TESValueForm::GetFormValue(itemObj, nullptr);
        }
        std::uint32_t stealValue = static_cast<std::uint32_t>((std::max)(1, perUnitValue * itemCount));

        // --- Get real item weight (native: TESWeightForm::GetFormWeight) ---
        float itemWeight = RE::TESWeightForm::GetFormWeight(itemObj, nullptr);
        float totalWeight = itemWeight * static_cast<float>(itemCount);

        // --- Get actor values (native uses Agility for player, Perception for NPC) ---
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

        // --- Check combat status (native: in-combat applies a multiplier) ---
        // AttemptPickpocket passes !notInCombat to ComputePickpocketSuccess
        bool npcInCombat = npc->IsInCombat();

        // --- Call native ComputePickpocketSuccess ---
        // This handles: all fPickPocket* game settings, perk entry point 0x38
        // (kModPickpocketChance), and combat multiplier. Returns 0-100.
        int chance = AiFormulas_ComputePickpocketSuccess(
            playerAgility, npcPerception, stealValue, totalWeight,
            static_cast<RE::Actor*>(player), npc,
            static_cast<RE::TESForm*>(itemObj), npcInCombat);

        const char* itemName = GetItemName(itemObj);
        spdlog::info("[PICKPOCKET] Item: '{}' x{}, value={}, weight={:.1f}, "
            "Agi={:.0f}, Per={:.0f}, combat={}, chance={}%",
            itemName, itemCount, stealValue, totalWeight,
            playerAgility, npcPerception, npcInCombat, chance);

        // --- Roll the dice (native: BSRandom::Int(0,100) < chance) ---
        std::uniform_int_distribution<int> dist(0, 100);
        int roll = dist(GetRNG());

        if (roll >= chance) {
            // === FAILED — call native Actor::PickpocketAlarm ===
            // This triggers the full crime system: ForceDetect, bounty,
            // witness detection, crime dialogue, faction response.
            spdlog::warn("[PICKPOCKET] FAILED (rolled {} >= {})", roll, chance);

            Actor_PickpocketAlarm(
                static_cast<RE::Actor*>(player), npc, nullptr, 0);

            // Fire story event with success=0 (increments "Pickpocket Attempts" stat)
            BGSPickpocketEvent failEvent(npc, 0);
            if (auto* storyMgr = RE::BGSStoryEventManager::GetSingleton()) {
                storyMgr->AddEvent(failEvent);
                spdlog::debug("[PICKPOCKET] Fired story event (failure)");
            }

            // Show the same message as the native game
            Hooks::ShowHUDMessageDirect("You've been caught pickpocketing!");

            // Strong haptic feedback for being caught
            g_vrInput.TriggerHaptic(true, 50000);
            g_vrInput.TriggerHaptic(false, 50000);

            return false;
        }

        // === SUCCESS — transfer item with stolen flag and spawn in hand ===
        spdlog::info("[PICKPOCKET] SUCCESS: Stealing '{}' x{} from NPC {:08X}",
            itemName, itemCount, npc->formID);

        // Remove from NPC and transfer directly to player with kStealing reason.
        // Using a_otherContainer + kStealing makes the engine handle the transfer
        // AND mark the item as stolen (sets ownership to the NPC's faction/self).
        RE::TESObjectREFR::RemoveItemData removeData(itemObj, itemCount);
        removeData.reason = RE::ITEM_REMOVE_REASON::kStealing;
        removeData.a_otherContainer = static_cast<RE::TESObjectREFR*>(player);

        Hooks::SetSuppressHUDMessages(true);
        npc->RemoveItem(removeData);
        Hooks::SetSuppressHUDMessages(false);

        // Fire story event with success=1
        // The engine's ItemsPickpocketedToMiscStatHandler listens for this and
        // automatically increments both "Successful Pickpockets" and "Pickpocket Attempts".
        BGSPickpocketEvent successEvent(npc, 1);
        if (auto* storyMgr = RE::BGSStoryEventManager::GetSingleton()) {
            storyMgr->AddEvent(successEvent);
            spdlog::debug("[PICKPOCKET] Fired story event (success)");
        }

        // Queue item to appear in hand via DropToHand
        auto& dropToHand = DropToHand::GetSingleton();
        dropToHand.QueueDropToHand(itemObj->formID, isLeft, itemCount, true, false);

        // Show HUD message
        std::string msg = std::string("[Stolen] ") + itemName;
        if (itemCount > 1) {
            msg += " x" + std::to_string(itemCount);
        }
        Hooks::ShowHUDMessageDirect(msg.c_str());

        // Medium haptic feedback for successful steal
        g_vrInput.TriggerHaptic(isLeft, 30000);

        return true;
    }

    // =========================================================================
    // SELECT RANDOM ITEM
    // =========================================================================

    bool PickpocketHandler::SelectRandomItem(RE::Actor* npc, RE::TESBoundObject*& outObject, 
        std::int32_t& outCount, RE::BGSInventoryItem*& outInvItem)
    {
        if (!npc || !npc->inventoryList) return false;

        struct StealCandidate {
            RE::TESBoundObject* object;
            std::int32_t count;
            RE::BGSInventoryItem* invItem;
        };

        std::vector<StealCandidate> candidates;

        for (auto& item : npc->inventoryList->data) {
            if (!item.object) continue;

            auto* boundObj = item.object;

            // Skip quest items
            if (IsQuestItem(boundObj)) continue;

            // Get item count from stack data
            std::int32_t totalCount = 0;
            auto* stack = item.stackData.get();
            while (stack) {
                totalCount += stack->count;
                stack = stack->nextStack.get();
            }
            if (totalCount <= 0) continue;

            // Skip form types that shouldn't be stolen
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
                    // These are valid stealable types
                    break;
                default:
                    continue;  // Skip everything else (perks, spells, factions, etc.)
            }

            // For weapons and armor: only steal if NPC has > 1 (don't steal equipped items)
            // This is a simple heuristic — if they only have 1, it's probably equipped
            if ((formType == RE::ENUM_FORM_ID::kWEAP || formType == RE::ENUM_FORM_ID::kARMO) && totalCount <= 1) {
                continue;
            }

            // For ammo/aid: steal a portion, not the full stack
            std::int32_t stealCount = 1;
            if (formType == RE::ENUM_FORM_ID::kAMMO) {
                // Steal up to 25% of ammo stack, minimum 1
                stealCount = (std::max)(1, totalCount / 4);
            }

            candidates.push_back({ boundObj, stealCount, &item });
        }

        if (candidates.empty()) return false;

        // Random selection
        std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
        auto& selected = candidates[dist(GetRNG())];
        outObject = selected.object;
        outCount = selected.count;
        outInvItem = selected.invItem;
        return true;
    }

}
