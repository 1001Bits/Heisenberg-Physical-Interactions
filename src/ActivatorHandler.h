#pragma once

#include <vector>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "RE/Bethesda/TESObjectREFRs.h"
#include "RE/NetImmerse/NiSmartPointer.h"

namespace heisenberg
{
    // Forward declarations
    class Hand;

    /**
     * Per-activator settings loaded from INI or JSON captures
     * Can specify custom radii and target node for each base form ID
     */
    struct ActivatorSettings
    {
        std::uint32_t baseFormID = 0;
        float activationRadius = -1.0f;  // -1 means use default
        float pointingRadius = -1.0f;    // -1 means use default
        float zOffset = 0.0f;            // Vertical offset for activation point
        std::string targetNode;          // Optional: specific child node to target for distance
        std::string description;         // Human-readable name for logging
        
        // Captured activation point (from NodeCaptureMode)
        // Position is in activator's LOCAL space - must transform by activator rotation to use
        bool hasCapturedOffset = false;
        float capturedOffsetX = 0.0f;
        float capturedOffsetY = 0.0f;
        float capturedOffsetZ = 0.0f;
    };

    /**
     * ActivatorHandler - Manages proximity-based activation of buttons, switches, doors, etc.
     * 
     * This is ported from InteractiveActivatorsVR and integrated into the Heisenberg hand system.
     * Instead of raycast-based selection (like grabbable objects), activators use proximity:
     * - Finger tip position is checked against cached activator positions
     * - When close enough, the activator is triggered
     * - Finger pose changes to "pointing" when near an activator
     * 
     * Features:
     * - Whitelist mode: Only track specific base form IDs
     * - Target nodes: Use specific child node position instead of root
     * - Discovery mode: Log activator info and child nodes when touched
     * - Per-activator settings: Custom radii and offsets per base form ID
     */
    class ActivatorHandler
    {
    public:
        enum class ActivatorType
        {
            Generic,      // Unknown activator
            TwoState,     // Button/switch (Default2StateActivator)
            Door,         // Door
            Terminal,     // Terminal/computer
            Container     // Container that should be activated not grabbed
        };

        /**
         * TrackedActivator - Represents an activator being tracked for proximity detection.
         * 
         * THREAD SAFETY: Uses RE::ObjectRefHandle for safe reference lookup.
         * LIFETIME SAFETY: Uses RE::NiPointer<> for scene graph nodes.
         */
        struct TrackedActivator
        {
            RE::ObjectRefHandle refrHandle;           // Handle for safe lookup (replaces raw ref pointer)
            std::uint32_t formID = 0;
            std::uint32_t baseFormID = 0;
            ActivatorType type = ActivatorType::Generic;
            
            // Cached settings for performance
            float activationRadius = 8.0f;   // Distance to trigger activation (units)
            float pointingRadius = 25.0f;    // Distance to start pointing pose (units)
            float zOffset = 0.0f;            // Vertical offset for activation point
            std::string targetNodeName;      // Name of specific node to use for distance
            
            // Captured activation point offset (in activator's LOCAL space)
            bool hasCapturedOffset = false;
            float capturedOffsetX = 0.0f;
            float capturedOffsetY = 0.0f;
            float capturedOffsetZ = 0.0f;
            
            // Cached node pointer with reference counting (avoid recursive search every frame)
            RE::NiPointer<RE::NiAVObject> cachedTargetNode;
            
            // Runtime state
            bool isLeftHandInRange = false;
            bool isRightHandInRange = false;
            std::chrono::steady_clock::time_point lastActivationTime;
            
            // Safe accessor for reference
            RE::TESObjectREFR* GetRefr() const
            {
                if (!refrHandle) return nullptr;
                RE::NiPointer<RE::TESObjectREFR> refPtr = refrHandle.get();
                return refPtr.get();
            }
            
            void SetRefr(RE::TESObjectREFR* refr)
            {
                if (refr) {
                    refrHandle = RE::ObjectRefHandle(refr);
                    formID = refr->formID;
                } else {
                    refrHandle.reset();
                    formID = 0;
                }
            }
            
            bool HasValidRefr() const { return GetRefr() != nullptr; }
        };

        struct ProximityResult
        {
            bool inPointingRange = false;    // Should show pointing finger pose
            bool inActivationRange = false;  // Should activate
            TrackedActivator* activator = nullptr;
            float distance = 0.0f;
        };

        static ActivatorHandler& GetSingleton()
        {
            static ActivatorHandler instance;
            return instance;
        }

        /**
         * Clear all state on save/load to prevent dangling pointers.
         * CRITICAL: Must be called on kPreLoadGame/kPostLoadGame.
         */
        void ClearState()
        {
            _trackedActivators.clear();
            _trackedTerminals.clear();
            _currentCell = nullptr;
            spdlog::info("[ActivatorHandler] Cleared state (save/load cleanup)");
        }

        // Initialize the handler and load settings from INI
        void Initialize();
        
        // Load activator settings from INI file
        void LoadSettingsFromINI();
        
        // Save current whitelist/settings to INI (for discovery mode)
        void SaveSettingsToINI();
        
        // Called each frame to check for cell changes and rescan if needed
        void Update();
        
        // Check proximity for a hand's finger tip position
        // Returns info about the closest activator in range
        // handSpeed is in game units per second - used to extend pointing range for fast-moving hands
        ProximityResult CheckProximity(const RE::NiPoint3& fingerTipPos, bool isLeftHand, float handSpeed = 0.0f);
        
        // Activate the closest activator if in range
        bool TryActivate(const RE::NiPoint3& fingerTipPos, bool isLeftHand);
        
        // =====================================================================
        // NODE CAPTURE MODE
        // Long-press left thumbstick to capture the closest node to right fingertip
        // This sets that node as the target node for the activator
        // =====================================================================
        struct NodeCaptureResult
        {
            bool success = false;
            std::uint32_t baseFormID = 0;
            std::string nodeName;
            std::string activatorName;
            std::string jsonPath;  // Full path to the saved JSON file
            float distance = 0.0f;
        };
        
        // Capture the closest child node of nearby activators to the given position
        // Returns info about the captured node (or failure)
        NodeCaptureResult CaptureTargetNode(const RE::NiPoint3& fingerTipPos);
        
        // Get the list of tracked activators (for debugging)
        const std::vector<TrackedActivator>& GetTrackedActivators() const { return _trackedActivators; }
        
        // Check if a base form ID is in the activator whitelist
        bool IsWhitelisted(std::uint32_t baseFormID) const;
        
        // Add/remove from whitelist at runtime
        void AddToWhitelist(std::uint32_t baseFormID, const std::string& description = "");
        void RemoveFromWhitelist(std::uint32_t baseFormID);
        
        // Register a captured activation point at runtime (immediately effective)
        void RegisterCapturedOffset(std::uint32_t baseFormID, const std::string& description,
                                    float offsetX, float offsetY, float offsetZ);
        
        // Log all child nodes of an activator (for discovery)
        void LogActivatorNodes(RE::TESObjectREFR* ref);
        
        // Find the nearest tracked terminal within maxRange of the given position
        // Returns nullptr if no terminal is in range
        TrackedActivator* GetNearestTerminal(const RE::NiPoint3& pos, float maxRange = 200.0f);

        // Get tracked terminals list (for external queries)
        const std::vector<TrackedActivator>& GetTrackedTerminals() const { return _trackedTerminals; }

        // Check if a hand is currently in pointing range of an activator
        bool IsHandInPointingRange(bool isLeft) const { return isLeft ? _leftHandInPointingRange : _rightHandInPointingRange; }

        // HMD Movement Threshold control - shrinks player hitbox when near activators
        // Call this when hand enters/exits pointing range to let hands reach through hitbox
        void SetHitboxShrinkEnabled(bool enabled);
        bool IsHitboxShrinkEnabled() const { return _hitboxShrinkActive; }
        
        // Update hitbox shrink state based on current hand-in-range tracking
        // Called after CheckProximity to automatically manage hitbox shrink
        void UpdateHitboxShrink();

        // Find a named child node recursively in the scene graph (public for terminal screen discovery)
        RE::NiAVObject* FindNodeRecursive(RE::NiAVObject* root, const std::string& nodeName) const;

    private:
        ActivatorHandler() = default;
        ~ActivatorHandler() = default;
        ActivatorHandler(const ActivatorHandler&) = delete;
        ActivatorHandler& operator=(const ActivatorHandler&) = delete;

        // Scan current cell for activators
        void ScanCellForActivators();
        
        // Check if a reference is a valid activator we should track
        bool IsValidActivator(RE::TESObjectREFR* ref) const;
        
        // Determine the type of activator
        ActivatorType DetermineActivatorType(RE::TESObjectREFR* ref) const;
        
        // Get distance from finger to activator's activation point
        float GetDistanceToActivator(const RE::NiPoint3& fingerPos, const TrackedActivator& activator) const;
        
        // Check if we can activate (cooldown, etc.)
        bool CanActivate(const TrackedActivator& activator) const;
        
        // Perform the activation
        void ActivateObject(RE::TESObjectREFR* ref);
        
        // Collect all child node names for discovery logging
        void CollectNodeNames(RE::NiAVObject* node, std::vector<std::string>& outNames, int depth = 0) const;
        
        // Get activator settings for a base form ID (or nullptr for defaults)
        const ActivatorSettings* GetActivatorSettings(std::uint32_t baseFormID) const;
        
        // Load captured activation points from JSON files
        void LoadCapturedActivatorOffsets();

        // State
        bool _initialized = false;
        RE::TESObjectCELL* _currentCell = nullptr;
        std::vector<TrackedActivator> _trackedActivators;
        std::vector<TrackedActivator> _trackedTerminals;  // Terminals tracked separately for world-screen redirect
        
        // Whitelist mode
        bool _useWhitelist = false;
        
        // Per-activator settings (keyed by base form ID)
        std::unordered_map<std::uint32_t, ActivatorSettings> _activatorSettings;
        
        // Newly discovered activators (for saving to INI)
        std::unordered_map<std::uint32_t, ActivatorSettings> _discoveredActivators;
        
        // Cooldown between activations (prevents rapid re-triggering)
        float _activationCooldownMs = 500.0f;
        
        // HMD movement threshold management (shrinks hitbox when near activators)
        bool _hitboxShrinkActive = false;
        float _originalHMDThreshold = -1.0f;  // Cached original value, -1 = not cached
        bool _leftHandInPointingRange = false;   // Current frame state
        bool _rightHandInPointingRange = false;  // Current frame state

        // Proximity throttle (skip full scan 2 out of 3 frames per hand)
        int  _leftProximityFrame  = 0;
        int  _rightProximityFrame = 0;
        ProximityResult _cachedLeftResult;
        ProximityResult _cachedRightResult;

        // INI file path for activator-specific settings
        static constexpr const char* ACTIVATOR_INI_PATH = "Data\\F4SE\\Plugins\\HeisenbergActivators.ini";
    };

} // namespace heisenberg
