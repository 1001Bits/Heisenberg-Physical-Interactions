#pragma once

/**
 * DropToHand - Intercepts items dropped from inventory and places them in hand
 * 
 * When the player drops an item from their inventory, instead of falling to the
 * ground, the item is automatically grabbed by the configured hand. If the hand
 * is already holding an item, that item is dropped normally first.
 */

#include "RE/Fallout.h"

#include <mutex>
#include <set>
#include <vector>

namespace heisenberg
{
    class DropToHand : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static DropToHand& GetSingleton()
        {
            static DropToHand instance;
            return instance;
        }
        
        /**
         * Clear all pending state on save/load.
         * The form IDs become invalid on load anyway.
         */
        void ClearState()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _pendingDrops.clear();
            _pendingLoots.clear();
            _grabsInProgress.clear();
            _recentlyStored.clear();
            _recentlyLootedFrom.clear();
            spdlog::debug("[DropToHand] Cleared state (save/load cleanup)");
        }
        
        // Initialize and register for events
        void Initialize();
        
        // Called each frame to process queued drops
        void OnFrameUpdate(float deltaTime);
        
        /**
         * Store a world object to inventory, then immediately drop it to hand.
         * This ensures the grabbed weapon is backed by an inventory item, preventing
         * duplication when the quick menu shows the same weapon.
         * 
         * @param refr The world object to store then grab
         * @param isLeft Which hand to grab with
         * @return true if successful
         */
        bool QueueStoreAndGrab(RE::TESObjectREFR* refr, bool isLeft);

        /**
         * Queue an item (already in inventory) to be dropped to hand.
         * Used by CookingHandler and SmartGrabHandler to place items in hand.
         *
         * @param baseFormID Base form ID of the item in inventory
         * @param isLeft Which hand to drop to
         * @param itemCount Number of items to drop (default 1, higher for ammo)
         * @param stickyGrab If true, item stays grabbed without holding grip (default true)
         * @param markAsSmartGrab If true, set isFromSmartGrab on grab state (prevents auto-storage)
         */
        void QueueDropToHand(RE::TESFormID baseFormID, bool isLeft, int itemCount = 1, bool stickyGrab = true, bool markAsSmartGrab = false);

        // BSTEventSink override
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent& a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>* a_eventSource) override;

        // Pipboy drop ref capture - hook sets flag, ProcessEvent captures ref ID
        void SetPipboyDropCapture(bool active) { _pipboyDropCapture = active; _pipboyDropRefID = 0; }
        uint32_t GetPipboyDropRefID() const { return _pipboyDropRefID; }

        // Cancel a pending drop by reference form ID (used when rerouting through loot path)
        void CancelPendingDropByRefID(std::uint32_t refID);

        // Determine which hand to use based on config and current grab state
        bool GetTargetHand(bool& outIsLeft);

        // Track items recently stored to inventory to prevent harvest-to-hand feedback loops
        void MarkAsRecentlyStored(std::uint32_t baseFormID);
        bool WasRecentlyStored(std::uint32_t baseFormID);

        // Track containers recently looted from — prevents immediate store-back to same NPC
        void MarkAsRecentlyLootedFrom(std::uint32_t containerFormID);
        bool WasRecentlyLootedFrom(std::uint32_t containerFormID);

    private:
        DropToHand() = default;
        ~DropToHand() = default;
        DropToHand(const DropToHand&) = delete;
        DropToHand& operator=(const DropToHand&) = delete;
        
        // Queue of dropped item form IDs waiting to be grabbed
        struct PendingDrop
        {
            std::uint32_t referenceFormID = 0;    // FormID of the dropped TESObjectREFR
            std::uint32_t expectedBaseFormID = 0;  // BaseFormID at queue time — reject if ref gets recycled
            float timeQueued = 0.0f;              // Time when queued (for timeout)
            bool skipWeaponFilter = false;        // If true, skip weapon offset check (for QueueStoreAndGrab items)
            bool stickyGrab = true;               // If false, item releases when grip is released (world weapon pickup)
            bool forceHand = false;               // If true, use forcedIsLeft instead of GetTargetHand()
            bool forcedIsLeft = false;            // Which hand to force (if forceHand is true)
            bool markAsSmartGrab = false;         // If true, set isFromSmartGrab on grab state (prevents auto-storage)
        };

        std::vector<PendingDrop> _pendingDrops;
        std::set<std::uint32_t> _grabsInProgress;  // RefIDs currently being grabbed (before StartGrab completes)
        std::mutex _mutex;
        bool _initialized = false;
        
        // Anti-loop tracking for harvest-to-hand
        struct RecentlyStored {
            std::uint32_t baseFormID = 0;
            float timeStored = 0.0f;
        };
        std::vector<RecentlyStored> _recentlyStored;
        static constexpr float RECENTLY_STORED_TIMEOUT = 1.0f;

        // Containers recently looted from (actor formID → time)
        struct RecentlyLootedFrom {
            std::uint32_t containerFormID = 0;
            float timeLooted = 0.0f;
        };
        std::vector<RecentlyLootedFrom> _recentlyLootedFrom;
        static constexpr float RECENTLY_LOOTED_FROM_TIMEOUT = 3.0f;  // 3 seconds cooldown

        // Pending loot items - base form IDs of items looted from containers that need to be dropped
        struct PendingLoot
        {
            std::uint32_t baseFormID = 0;     // Base form ID of the looted item
            std::int32_t itemCount = 1;       // Number of items looted (preserve stack count)
            float timeQueued = 0.0f;          // Time when queued (for timeout)
            bool forceHand = false;           // If true, use forcedIsLeft instead of config
            bool forcedIsLeft = false;        // Which hand to force (if forceHand is true)
            bool stickyGrab = true;           // If false, item releases when grip is released (world weapon pickup)
            bool markAsSmartGrab = false;     // If true, set isFromSmartGrab on grab state (prevents auto-storage)
        };
        std::vector<PendingLoot> _pendingLoots;


        // Pipboy drop ref capture state
        bool _pipboyDropCapture = false;
        uint32_t _pipboyDropRefID = 0;

        // Try to grab a pending drop
        bool TryGrabPendingDrop(const PendingDrop& drop);

        // Try to drop a pending loot item from inventory
        bool TryDropPendingLoot(PendingLoot& loot);
    };
}
