#pragma once

#include "Selection.h"
#include <mutex>

namespace heisenberg
{
    /**
     * Hand state machine - manages a single VR hand's interactions.
     * Based on Skyrim HIGGS, adapted for Fallout 4 VR.
     * 
     * THREAD SAFETY: Uses mutex protection for state modification operations.
     * Selection/deselection operations are protected by _stateMutex.
     */
    class Hand
    {
    public:
        enum class State : uint8_t
        {
            Idle,           // Not pointing at anything meaningful
            SelectedClose,  // Object is within grab range (max 1m)
            Pulling,        // Object is being pulled to hand
            Held,           // Player is holding the object in their hand
        };

        explicit Hand(bool isLeft);
        ~Hand() = default;

        // Main update - called every frame
        void Update();

        // Input methods - called by input system when buttons pressed
        void OnGrabPressed();
        void OnGrabReleased();

        // Accessors
        bool IsLeft() const { return _isLeft; }
        State GetState() const { return _state; }
        const RE::NiPoint3& GetPosition() const { return _position; }
        const RE::NiPoint3& GetVelocity() const { return _velocity; }
        const RE::NiMatrix3& GetRotation() const { return _rotation; }
        const Selection& GetSelection() const { return _selection; }

        // Left-handed mode helpers
        // Primary = weapon hand (right normally, left in left-handed mode)
        bool IsPrimaryHand() const;
        bool IsOffHand() const;

    // Check if hand is holding something
        bool IsHolding() const { return _state == State::Held || _state == State::Pulling; }
        bool IsPulling() const { return _state == State::Pulling; }
        bool IsGrabPressed() const { return _grabPressed; }
        void ConsumeGripPress() { _grabPressed = true; }  // Mark grip as consumed to prevent re-triggering
        bool IsPointingAtActivator() const { return _isPointingAtActivator; }
        
        /**
         * Get the currently selected object (if any).
         * Returns nullptr if not selecting anything.
         */
        RE::TESObjectREFR* GetSelectedObject() const
        {
            return _selection.GetRefr();
        }

        /**
         * Clear all references and reset state.
         * Call this on save/load to prevent dangling pointers.
         * Thread-safe via mutex.
         */
        void ClearState()
        {
            std::scoped_lock lock(_stateMutex);
            _selection.Clear();
            _nearbyActivatorHandle.reset();
            _state = State::Idle;
            _isPointingAtActivator = false;
            _wasPointingAtActivator = false;
            _isInActivatorRange = false;
        }

    private:
        // Update phases
        void UpdateInput();
        void UpdateTracking();
        void UpdateSelection();
        void UpdateState();
        void UpdateHeldObject(float deltaTime);
        void UpdateActivatorProximity();  // Check for nearby activators

        // State transitions
        void TransitionToIdle();
        void TransitionToSelectedClose(const Selection& selection);
        void TransitionToPulling();
        void TransitionToHeld();

        // Grab/release helpers
        bool TryStartGrab();
        void Release(bool throw_object);

        // Selection helpers
        bool FindSelectionViewCaster();  // Native game crosshair selection (highest priority)
        bool FindSelectionRaycast();     // Extended range raycast fallback
        bool FindSelectionProximity();

        // Configuration
        bool _isLeft;
        std::string _handNodeName;

        // Tracking data
        RE::NiPoint3 _position;
        RE::NiPoint3 _prevPosition;
        RE::NiPoint3 _velocity;
        RE::NiMatrix3 _rotation;
        std::deque<RE::NiPoint3> _velocityHistory;

        // State machine
        State _state = State::Idle;
        Selection _selection;
        bool _grabPressed = false;
        uint32_t _lastLoggedFormID = 0;  // Only log when selection changes
        RE::ObjectRefHandle _lastViewCasterHandle;  // Track last ViewCaster target for change detection

        // Activator proximity state
        bool _isPointingAtActivator = false;      // Finger is near an activator (pointing range)
        bool _wasPointingAtActivator = false;     // Was pointing last frame (for pose transition)
        bool _isInActivatorRange = false;         // Finger is touching an activator (activation range)
        RE::ObjectRefHandle _nearbyActivatorHandle;  // Handle to nearby activator (thread-safe)
        float _activatorDistance = 0.0f;          // Distance to nearby activator

        // Thread safety
        mutable std::mutex _stateMutex;           // Protects selection/deselection operations

        // Timing
        double _stateEnterTime = 0.0;
        double _lastSelectionTime = 0.0;
        float _lastDeltaTime = 1.0f / 90.0f;
        
        // Performance optimization: cache position and skip frames
        RE::NiPoint3 _lastSelectionPosition;  // Position at last selection check
        int _selectionFrameCounter = 0;       // Frame counter for skipping
        int _debugFrameCounter = 0;           // Per-hand debug logging counter
        static constexpr float kPositionCacheThreshold = 5.0f;  // Units - recheck if moved more than this
        static constexpr int kSelectionFrameSkip = 9;           // Frames to skip when idle+stationary (~100ms at 90Hz)
        
        // Constants - now use config values where possible
        static constexpr float kSelectionLeewayTime = 0.25f; // Seconds
        static constexpr float kThrowVelocityScale = 1.0f;   // Multiplier for throw velocity (normal)
        static constexpr float kMaxThrowVelocity = 500.0f;   // Max throw velocity per axis (high limit)
    };
}
