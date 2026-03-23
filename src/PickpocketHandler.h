#pragma once

/**
 * PickpocketHandler - ViewCaster-based VR pickpocketing from NPCs
 * 
 * When the player is sneaking and a VR wand's ViewCaster targets a living NPC,
 * pressing grip will attempt to steal a random item from the NPC's inventory.
 * The stolen item appears in the player's hand via DropToHand.
 * 
 * Detection uses the game's native VR ViewCaster system — the same system that
 * shows "Pickpocket <NPC>" prompts. No custom proximity/distance checks needed.
 * 
 * Uses the native FO4 pickpocket system identically to the base game:
 *  - AiFormulas::ComputePickpocketSuccess for the chance formula
 *    (all fPickPocket* game settings, perk entry point 0x38, combat mult)
 *  - Actor::PickpocketAlarm for the full crime system on failure
 *    (ForceDetect, bounty, witness detection, crime dialogue)
 *  - Real item value via BGSInventoryItem::GetInventoryValue
 *  - Real item weight via TESWeightForm::GetFormWeight
 *  - ITEM_REMOVE_REASON::kStealing for proper stolen flag on items
 *  - BGSStoryEventManager for quest story events (triggers misc stat counters)
 * 
 * Flow:
 *   1. Player sneaks
 *   2. VR wand points at NPC (ViewCaster detects target)
 *   3. Player presses grip → pickpocket attempt
 *   4. Native ComputePickpocketSuccess determines chance (with perks!)
 *   5. Success: item removed from NPC, spawned in player's hand
 *   6. Failure: Actor::PickpocketAlarm triggers full crime system
 */

#include <RE/Fallout.h>
#include <vector>

namespace heisenberg
{
    class PickpocketHandler
    {
    public:
        static PickpocketHandler& GetSingleton()
        {
            static PickpocketHandler instance;
            return instance;
        }

        /**
         * Update pickpocket state each frame.
         * Checks ViewCaster targets for NPCs while sneaking, handles grip input.
         * Called from Heisenberg::OnInputUpdate().
         * 
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime);

        /**
         * Clear state on save/load to prevent dangling pointers.
         */
        void ClearState()
        {
            _cooldownTimer = 0.0f;
        }

    private:
        PickpocketHandler() = default;
        ~PickpocketHandler() = default;
        PickpocketHandler(const PickpocketHandler&) = delete;
        PickpocketHandler& operator=(const PickpocketHandler&) = delete;

        /**
         * Attempt to pickpocket an item from the target NPC.
         * Uses native AiFormulas::ComputePickpocketSuccess for chance calculation
         * and Actor::PickpocketAlarm for crime system on failure.
         * 
         * @param npc The target NPC actor
         * @param isLeft Which hand is doing the stealing
         * @return true if an item was successfully stolen
         */
        bool TryPickpocket(RE::Actor* npc, bool isLeft);

        /**
         * Select a random stealable item from the NPC's inventory.
         * Skips equipped items and quest items.
         * 
         * @param npc The NPC to check
         * @param outObject Output: the selected item's base object
         * @param outCount Output: count of the item (1 for most, more for ammo/aid)
         * @param outInvItem Output: pointer to the BGSInventoryItem in the NPC's inventory
         * @return true if a stealable item was found
         */
        bool SelectRandomItem(RE::Actor* npc, RE::TESBoundObject*& outObject, 
            std::int32_t& outCount, RE::BGSInventoryItem*& outInvItem);

        // Timers
        float _cooldownTimer = 0.0f;          // Cooldown between steal attempts

        // Constants
        static constexpr float STEAL_COOLDOWN = 1.5f;        // Seconds between attempts
    };
}
