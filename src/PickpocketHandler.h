#pragma once

/**
 * PickpocketHandler - Browse-and-steal VR pickpocketing from NPCs
 *
 * When the player is sneaking and a VR wand's ViewCaster targets a living NPC
 * who hasn't detected them, pressing grip enters Browse mode:
 *   - A random item from the NPC's inventory is spawned in the player's hand
 *   - Pressing grip again cycles to the next item
 *   - Moving the hand away from the NPC triggers the actual steal attempt
 *   - Stopping sneaking or NPC detecting the player cancels the browse
 *
 * Uses the native FO4 pickpocket system for detection and crime:
 *  - Actor::RequestDetectionLevel to gate entry (must be undetected)
 *  - AiFormulas::ComputePickpocketSuccess for the chance formula
 *  - Actor::PickpocketAlarm for the full crime system on failure
 *  - Real item value/weight for formula inputs
 *  - ITEM_REMOVE_REASON::kStealing for proper stolen flag on success
 *  - BGSStoryEventManager for quest story events
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
         * Handles browse mode cycling, distance checks, and steal commitment.
         * Called from Heisenberg::OnInputUpdate().
         *
         * @param deltaTime Frame delta time
         */
        void Update(float deltaTime);

        /**
         * Clear state on save/load to prevent dangling pointers.
         */
        void ClearState();

        /**
         * Check if the handler is currently in browse mode.
         * Used by Hand.cpp to suppress normal grab while browsing.
         */
        bool IsBrowsing() const { return _state == State::Browsing; }

        /**
         * Which hand is currently browsing (only valid when IsBrowsing() is true).
         */
        bool GetBrowsingHand() const { return _browsingIsLeft; }

    private:
        PickpocketHandler() = default;
        ~PickpocketHandler() = default;
        PickpocketHandler(const PickpocketHandler&) = delete;
        PickpocketHandler& operator=(const PickpocketHandler&) = delete;

        // =====================================================================
        // STATE MACHINE
        // =====================================================================

        enum class State
        {
            Idle,       // Not in pickpocket mode
            Browsing,   // Cycling through NPC items, preview in hand
        };

        State _state = State::Idle;

        // =====================================================================
        // BROWSE MODE DATA
        // =====================================================================

        // The NPC being pickpocketed (only valid during Browsing)
        RE::ObjectRefHandle _targetNPCHandle;

        // Which hand is doing the pickpocketing
        bool _browsingIsLeft = false;

        // NPC position when browse started (for distance check)
        RE::NiPoint3 _npcPositionAtStart;

        // Candidate items from NPC inventory
        struct StealCandidate {
            RE::TESBoundObject* object = nullptr;
            std::int32_t count = 0;
            // We don't store BGSInventoryItem* since it can become invalid
            // after items are removed/added back
        };
        std::vector<StealCandidate> _candidates;
        int _currentIndex = 0;

        // The spawned preview reference currently in hand
        RE::ObjectRefHandle _previewRefHandle;

        // Cooldown after a steal attempt (success or fail)
        float _cooldownTimer = 0.0f;

        // =====================================================================
        // CONSTANTS
        // =====================================================================

        static constexpr float STEAL_COOLDOWN = 1.5f;
        static constexpr float STEAL_DISTANCE = 80.0f;  // Game units - how far hand must move from NPC to commit steal
        static constexpr int DETECTION_THRESHOLD = 0;    // RequestDetectionLevel returns < 0 for hidden

        // =====================================================================
        // METHODS
        // =====================================================================

        /**
         * Build list of stealable items from NPC's inventory.
         * Filters out quest items, non-stealable types, and equipped items.
         */
        bool BuildCandidateList(RE::Actor* npc);

        /**
         * Enter browse mode: spawn first item preview in hand.
         */
        void EnterBrowseMode(RE::Actor* npc, bool isLeft);

        /**
         * Cycle to the next item in the candidate list.
         * Returns the current preview, spawns the next one.
         */
        void CycleNextItem();

        /**
         * Spawn a preview of the current candidate item at the hand.
         * Removes from NPC inventory, drops at hand position, grabs it.
         */
        void SpawnPreviewItem();

        /**
         * Return the current preview item back to the NPC and clean up.
         */
        void ReturnPreviewToNPC();

        /**
         * Commit the steal: run pickpocket formula on current item.
         * On success: mark as stolen, keep in hand.
         * On fail: return item, trigger alarm.
         */
        void CommitSteal();

        /**
         * Cancel browse mode: return item and exit.
         */
        void CancelBrowse();

        /**
         * Check if the target NPC has detected the player.
         * Uses Actor::RequestDetectionLevel.
         * @return true if NPC has detected the player (should cancel/block)
         */
        bool HasNPCDetectedPlayer(RE::Actor* npc) const;
    };
}
