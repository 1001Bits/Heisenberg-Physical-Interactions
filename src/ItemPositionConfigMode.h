#pragma once

#include "ItemOffsets.h"

namespace heisenberg
{
    struct GrabState;

    /**
     * Configuration mode for adjusting item grab offset position and rotation.
     * 
     * Button-based workflow (no visual UI):
     * 1. Long-press A button (1 second) to toggle reposition mode ON/OFF
     * 2. When ON, grab an item - it will stay grabbed (sticky grab)
     * 3. Use thumbsticks to adjust position
     * 4. Hold Grip + thumbsticks to adjust rotation
     * 5. Press B to save offset to JSON
     * 6. Press Y to reset to default
     * 7. Long-press A again to exit reposition mode
     */
    class ItemPositionConfigMode
    {
    public:
        static ItemPositionConfigMode& GetSingleton()
        {
            static ItemPositionConfigMode instance;
            return instance;
        }

        // Initialize (call after game load)
        void Initialize();

        // Called each frame to check for toggle and handle updates
        void OnFrameUpdate(float deltaTime);

        // Check if config UI is open (for compatibility - always false in button mode)
        bool IsConfigUIOpen() const { return false; }

        // Check if reposition mode is active (sticky grab enabled)
        bool IsRepositionModeActive() const { return _repositionModeActive; }

        // Toggle the config UI open/closed (just toggles reposition mode)
        void ToggleConfigUI();

        // Toggle reposition mode on/off
        void ToggleRepositionMode();
        
        // Clear all state - call on save load
        void ClearAllState();

        // Called when a grab starts - enables sticky mode if repositioning is active
        void OnGrabStarted(GrabState* grabState, bool isLeft);

        // Called when a grab ends
        void OnGrabEnded(bool isLeft);

        // Get current offset being edited
        ItemOffset& GetCurrentOffset() { return _currentOffset; }

        // Frozen world position for reposition mode
        bool HasFrozenPosition() const { return _hasFrozenPosition; }
        void SetFrozenWorldTransform(const RE::NiPoint3& itemPos, const RE::NiMatrix3& itemRot,
                                     const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot);
        const RE::NiPoint3& GetFrozenWorldPos() const { return _frozenWorldPos; }
        const RE::NiMatrix3& GetFrozenWorldRot() const { return _frozenWorldRot; }
        
        // Update frozen position with thumbstick adjustments
        void AdjustFrozenPosition(const RE::NiPoint3& delta);
        void AdjustFrozenRotation(const RE::NiMatrix3& rot);

    private:
        ItemPositionConfigMode() = default;
        ~ItemPositionConfigMode();

        ItemPositionConfigMode(const ItemPositionConfigMode&) = delete;
        ItemPositionConfigMode& operator=(const ItemPositionConfigMode&) = delete;

        // Create the config UI panel (no-op in button mode)
        void CreateConfigUI();

        // Close the config UI (no-op in button mode)
        void CloseConfigUI();

        // Handle reposition input (thumbsticks)
        void HandleRepositionInput();

        // Save current offset to JSON
        void SaveCurrentOffset();

        // Reset current offset to default
        void ResetCurrentOffset();

        // State
        bool _initialized = false;
        bool _repositionModeActive = false;  // When true, next grab becomes sticky
        bool _offsetSavedThisSession = false;  // True if offset was saved during this reposition session

        // The grab we're currently editing (set when sticky grab activates)
        GrabState* _currentGrabState = nullptr;
        bool _isLeftHand = false;
        std::string _currentItemName;
        ItemOffset _currentOffset;

        // During reposition: item's frozen WORLD position and rotation
        // Thumbsticks adjust this world position directly
        RE::NiPoint3 _frozenWorldPos;
        RE::NiMatrix3 _frozenWorldRot;
        // Store hand transform when frozen position was captured (for relative offset calculation)
        RE::NiPoint3 _frozenHandPos;
        RE::NiMatrix3 _frozenHandRot;
        bool _hasFrozenPosition = false;

        // Input edge detection state (was static locals, now members so ClearAllState resets them)
        float _stickHoldTime = 0.0f;
        bool _stickTriggered = false;
        float _suppressionTimer = 0.0f;
        bool _bWasPressed = false;
        bool _yWasPressed = false;
    };
}
