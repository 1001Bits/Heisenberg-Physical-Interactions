#pragma once

#include <string>
#include <chrono>

#include "RE/Bethesda/TESObjectREFRs.h"

namespace heisenberg
{
    /**
     * NodeCaptureMode - Mode for capturing activator target node positions.
     * 
     * Workflow:
     * 1. Hold LEFT thumbstick to enter capture mode
     * 2. Right hand shows pointing pose (index extended)
     * 3. Position finger at the activation point on the object
     * 4. Hold LEFT thumbstick again to capture position
     * 5. JSON saved with relative position and rotation
     * 6. Mode exits, hands return to normal
     * 
     * The captured data includes:
     * - Finger tip position relative to the activator (in activator's local space)
     * - Activator's world rotation at capture time
     * - This allows recreating the activation point in any activator orientation
     */
    class NodeCaptureMode
    {
    public:
        struct CaptureResult
        {
            bool success = false;
            std::string activatorName;
            std::string nodeName;
            std::string jsonPath;
            std::uint32_t baseFormID = 0;
        };

        static NodeCaptureMode& GetSingleton()
        {
            static NodeCaptureMode instance;
            return instance;
        }

        // Check if capture mode is currently active
        bool IsModeActive() const { return _modeActive; }

        // Called every frame from Heisenberg main update
        // Returns true if mode consumed input this frame
        void Update();

        // Force exit mode (e.g., on save/load)
        void ExitMode();

        // Clear all state
        void ClearState();

        // Get the last capture result (valid until next capture attempt)
        const CaptureResult& GetLastResult() const { return _lastResult; }

    private:
        NodeCaptureMode() = default;
        ~NodeCaptureMode() = default;
        NodeCaptureMode(const NodeCaptureMode&) = delete;
        NodeCaptureMode& operator=(const NodeCaptureMode&) = delete;

        // Enter capture mode
        void EnterMode();

        // Perform the capture using current finger position
        void PerformCapture();

        // Save capture data to JSON
        bool SaveCaptureToJSON(const std::string& activatorName, 
                               std::uint32_t baseFormID,
                               const std::string& closestNodeName,
                               const RE::NiPoint3& fingerWorldPos,
                               const RE::NiPoint3& activatorWorldPos,
                               const RE::NiMatrix3& activatorWorldRotation,
                               const RE::NiPoint3& relativePos);

        // State
        bool _modeActive = false;
        bool _thumbstickWasPressed = false;
        std::chrono::steady_clock::time_point _thumbstickHoldStartTime;
        bool _isHoldingThumbstick = false;
        
        // Capture result
        CaptureResult _lastResult;

        // When the mode became active, for the inactivity auto-exit below.
        std::chrono::steady_clock::time_point _modeEnteredTime;

        // Constants
        static constexpr float THUMBSTICK_HOLD_TIME_MS = 500.0f;  // Half second hold to enter/capture
        // While active this mode FORCES a right-hand pointing pose every frame
        // (thumb/middle/ring/pinky fully curled). Its only exits were a
        // completed capture or a game load, so a player who armed it by
        // accident — a 500 ms right-thumbstick hold, which overlaps ordinary
        // snap-turn input — was left with a curled thumb for the rest of the
        // session with no indication why. Live evidence: a tester's log shows
        // "[NodeCaptureMode] Mode entered" with no matching exit for the
        // remaining ~4 minutes, and reported a constantly bent thumb.
        // Auto-exit restores the hand if no capture happens.
        static constexpr float MODE_AUTO_EXIT_MS = 30000.0f;  // 30s without a capture
    };

} // namespace heisenberg
