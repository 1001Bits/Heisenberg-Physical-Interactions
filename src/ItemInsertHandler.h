#pragma once

#include <vector>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace heisenberg
{
    /**
     * ItemInsertHandler - Manages proximity-based item insertion into activators
     * 
     * This handles "insert item to activate" mechanics like:
     * - Holding a bottle cap near a Nuka Fridge coin slot
     * - Putting a fusion core in a power generator
     * - Inserting a key card into a door terminal
     * 
     * When a held item is near an "insert zone" and matches the required item type,
     * the activator is triggered and the item is consumed (or remains in hand).
     */
    class ItemInsertHandler
    {
    public:
        /**
         * Definition of an insert zone on an activator
         */
        struct InsertZone
        {
            std::uint32_t activatorBaseFormID = 0;  // Base form ID of the activator (e.g., Nuka Fridge)
            std::string targetNodeName;              // Node to use for position (e.g., "CoinSlot")
            float activationRadius = 15.0f;          // Distance for activation (game units)
            RE::NiPoint3 positionOffset;             // Offset from node position
            
            // Cylindrical zone mode - for objects that can be at any angle around the activator
            // Uses Z offset for height, and checks horizontal distance falls within ring
            bool useCylindricalZone = false;         // If true, use cylindrical check instead of sphere
            float cylindricalMinRadius = 0.0f;       // Inner radius of ring (0 = solid cylinder)
            float cylindricalMaxRadius = 50.0f;      // Outer radius of ring (replaces activationRadius)
            float cylindricalZOffset = 0.0f;         // Center Z height above root
            float cylindricalZTolerance = 10.0f;     // Z tolerance (+/-)
            
            // Required items - any of these will work (e.g., bottle caps)
            std::unordered_set<std::uint32_t> acceptedItemBaseFormIDs;
            
            // Optional: match by keyword instead of specific form IDs
            std::string acceptedKeyword;             // e.g., "ObjectTypeBottleCap"
            
            // Optional: match activators by editor ID pattern (for wildcard zones)
            std::string editorIDPattern;             // e.g., "NukaCola" matches any with that in name
            
            // Optional: match activators by 3D node name pattern
            std::string nodeNamePattern;             // e.g., "NukaCola" matches any with that in 3D name
            
            // Behavior
            bool consumeItem = true;                 // Remove item from player inventory after use
            bool playAnimation = true;               // Trigger the activator's animation
            std::string description;                 // Human-readable name for logging
        };

        /**
         * Runtime instance of an insert zone in the current cell
         */
        struct TrackedInsertZone
        {
            RE::ObjectRefHandle activatorRefHandle;    // Handle for safe lookup (prevents dangling pointers)
            std::uint32_t activatorFormID = 0;
            const InsertZone* zoneConfig = nullptr;

            // Cached node pointer with reference counting (prevents dangling pointers)
            RE::NiPointer<RE::NiAVObject> cachedTargetNode;

            // Runtime state
            std::chrono::steady_clock::time_point lastActivationTime;

            // Safe accessor for reference
            RE::TESObjectREFR* GetRefr() const
            {
                if (!activatorRefHandle) return nullptr;
                RE::NiPointer<RE::TESObjectREFR> refPtr = activatorRefHandle.get();
                return refPtr.get();
            }
        };

        struct ProximityResult
        {
            bool inRange = false;
            TrackedInsertZone* zone = nullptr;
            float distance = 0.0f;
        };

        static ItemInsertHandler& GetSingleton()
        {
            static ItemInsertHandler instance;
            return instance;
        }

        // Initialize the handler and load settings from INI
        void Initialize();
        
        // Load insert zone definitions from INI file
        void LoadZonesFromINI();
        
        // Called each frame to check for cell changes and rescan if needed
        void Update();
        
        // Check if a held item is near an insert zone
        // Returns info about the closest matching zone
        ProximityResult CheckHeldItemProximity(
            const RE::NiPoint3& heldItemPos,
            std::uint32_t heldItemBaseFormID,
            RE::TESObjectREFR* heldItemRef);
        
        // Try to activate an insert zone with the held item
        bool TryActivateInsertZone(
            const RE::NiPoint3& heldItemPos,
            std::uint32_t heldItemBaseFormID,
            RE::TESObjectREFR* heldItemRef,
            bool isLeftHand);
        
        // Register an insert zone programmatically
        void RegisterInsertZone(const InsertZone& zone);
        
        // Get a snapshot of tracked zones (thread-safe copy for debugging)
        std::vector<TrackedInsertZone> GetTrackedZones() const {
            std::scoped_lock lock(_trackedZonesMutex);
            return _trackedZones;
        }
        
        // Discovery mode: logs zone info when items are near activators
        bool IsDiscoveryModeEnabled() const { return _discoveryMode; }
        void SetDiscoveryMode(bool enabled) { _discoveryMode = enabled; }
        
        // Log all child nodes of an activator (for discovery)
        void LogActivatorNodes(RE::TESObjectREFR* ref);

    private:
        ItemInsertHandler() = default;
        ~ItemInsertHandler() = default;
        ItemInsertHandler(const ItemInsertHandler&) = delete;
        ItemInsertHandler& operator=(const ItemInsertHandler&) = delete;

        // Scan current cell for activators with insert zones
        void ScanCellForInsertZones();
        
        // Register default insert zones (Nuka fridge, etc.)
        void RegisterDefaultZones();
        
        // Find a node recursively by name
        RE::NiAVObject* FindNodeRecursive(RE::NiAVObject* root, const std::string& nodeName) const;
        
        // Collect all node names for logging
        void CollectNodeNames(RE::NiAVObject* node, std::vector<std::string>& outNames, int depth) const;
        
        // Get distance from item to insert zone
        float GetDistanceToZone(const RE::NiPoint3& itemPos, const TrackedInsertZone& zone) const;
        
        // Check if an item matches the zone's requirements
        bool ItemMatchesZone(std::uint32_t itemBaseFormID, RE::TESObjectREFR* itemRef, const InsertZone& zone) const;
        
        // Check if we can activate (cooldown, etc.)
        bool CanActivate(const TrackedInsertZone& zone) const;
        
        // Log nearby activators when holding an item (discovery mode)
        void LogNearbyActivators(const RE::NiPoint3& heldItemPos, std::uint32_t heldItemBaseFormID);
        
        // Perform the activation
        void ActivateZone(TrackedInsertZone& zone, RE::TESObjectREFR* itemRef, bool isLeftHand);

        // Registered insert zones (from INI or code)
        std::unordered_map<std::uint32_t, InsertZone> _insertZones;  // keyed by activator base form ID
        
        // Active zones in current cell
        std::vector<TrackedInsertZone> _trackedZones;
        mutable std::mutex _trackedZonesMutex;  // Thread safety: protects _trackedZones
        
        // Current cell tracking
        RE::TESObjectCELL* _currentCell = nullptr;
        
        // Settings
        bool _initialized = false;
        bool _discoveryMode = false;
        float _activationCooldownMs = 1000.0f;
    };

} // namespace heisenberg
