#include "VRInput.h"
#include "MenuChecker.h"
#include "OpenVRHook.h"

// Include OpenVR from F4VRCommonFramework's vendored copy
// The path is set up by the CMake build
#include <openvr.h>

namespace heisenberg
{
    // Helper to read left-handed mode from game INI setting
    static bool ReadLeftHandedModeFromGame()
    {
        // Offset for bLeftHandedMode:VR setting in Fallout4VR.ini
        static auto iniLeftHandedMode = reinterpret_cast<bool*>(REL::Offset(0x37d5e48).address());
        return *iniLeftHandedMode;
    }

    bool VRInput::Initialize()
    {
        // Just mark as ready to initialize
        // Actual VR system initialization happens lazily in Update()
        // because OpenVR may not be ready at plugin load time
        _initialized = false;
        
        // Don't read left-handed mode here - game settings aren't loaded yet!
        // It will be read on first Update() after VR system is acquired
        _isLeftHandedMode = false;
        _leftHandedModeChecked = false;
        
        spdlog::info("VRInput: Ready for lazy initialization");
        return true;
    }

    void VRInput::Update(bool isLeftHandedMode)
    {
        // Lazy initialization of VR system
        if (!_vrSystem) {
            // Try to get the REAL VR system (bypassing our hook) if available
            auto& openvrHook = OpenVRHook::GetSingleton();
            if (openvrHook.IsHooked()) {
                _vrSystem = openvrHook.GetRealVRSystem();
                if (_vrSystem) {
                    spdlog::info("VRInput: Using REAL IVRSystem (bypassing hook) for input");
                    _initialized = true;
                    _useRawInput = true;
                }
            }
            
            // Fallback to standard OpenVR if hook not available
            if (!_vrSystem) {
                _vrSystem = vr::VRSystem();
                if (_vrSystem) {
                    spdlog::info("VRInput: OpenVR system acquired (standard)");
                    _initialized = true;
                    _useRawInput = false;
                } else {
                    // Only log once per second to avoid spam
                    static int skipFrames = 0;
                    if (++skipFrames >= 90) {
                        skipFrames = 0;
                        spdlog::warn("VRInput: Waiting for OpenVR system...");
                    }
                    return;
                }
            }
        }

        // Check left-handed mode once at startup, then only when PauseMenu closes
        // (the setting can only be changed in the pause menu options)
        // Use cached menu state from MenuChecker for thread safety
        bool pauseMenuOpen = MenuChecker::GetSingleton().IsPaused();
        
        if (!_leftHandedModeChecked) {
            // First check - read initial value
            _isLeftHandedMode = ReadLeftHandedModeFromGame();
            _leftHandedModeChecked = true;
            _pauseMenuWasOpen = pauseMenuOpen;
            spdlog::info("VRInput: Left-handed mode: {}", _isLeftHandedMode ? "ON" : "OFF");
        }
        else if (_pauseMenuWasOpen && !pauseMenuOpen) {
            // PauseMenu just closed - re-check setting in case user changed it
            bool newValue = ReadLeftHandedModeFromGame();
            if (newValue != _isLeftHandedMode) {
                _isLeftHandedMode = newValue;
                spdlog::info("VRInput: Left-handed mode changed to: {}", _isLeftHandedMode ? "ON" : "OFF");
            }
        }
        _pauseMenuWasOpen = pauseMenuOpen;

        auto* vrSystem = static_cast<vr::IVRSystem*>(_vrSystem);

        // Save previous state
        _leftController.previous = _leftController.current;
        _rightController.previous = _rightController.current;
        _leftController.previousTriggerValue = _leftController.triggerValue;
        _rightController.previousTriggerValue = _rightController.triggerValue;

        // Get tracked device indices for controllers
        auto leftIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(
            vr::TrackedControllerRole_LeftHand);
        auto rightIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(
            vr::TrackedControllerRole_RightHand);
        
        _leftController.deviceIndex = leftIndex;
        _rightController.deviceIndex = rightIndex;
        
        // Get OpenVRHook for unfiltered access
        auto& openvrHook = OpenVRHook::GetSingleton();

        // Get poses for angular velocity tracking
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vrSystem->GetDeviceToAbsoluteTrackingPose(
            vr::TrackingUniverseStanding, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);

        // Read controller state UNFILTERED — bypassing the vtable hook entirely.
        // The Heisenberg callback strips A/Grip from ulButtonPressed when grabbing
        // (to hide those buttons from the game). If we read through the hooked path,
        // we'd see our own masking and think the player released the button = jitter.
        // GetControllerStateUnfiltered() calls the original vtable function pointer
        // directly, giving us the true hardware state.
        // Fallback: if unfiltered isn't available (vtable not hooked), use vrSystem
        // directly — in that case no vtable hook exists so it's already unfiltered.

        _leftController.valid = false;
        _leftController.poseValid = false;
        if (leftIndex != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            bool gotState = openvrHook.GetControllerStateUnfiltered(leftIndex, &state, sizeof(state));
            if (!gotState) {
                // Fallback: no vtable hook active, call directly (already unfiltered)
                gotState = vrSystem->GetControllerState(leftIndex, &state, sizeof(state));
            }
            if (gotState) {
                _leftController.current = state.ulButtonPressed;
                _leftController.triggerValue = state.rAxis[1].x;
                _leftController.gripValue = state.rAxis[2].x;
                _leftController.thumbstickX = state.rAxis[0].x;
                _leftController.thumbstickY = state.rAxis[0].y;
                _leftController.valid = true;
                if (state.rAxis[2].x > 0.1f) _leftController.hasAnalogGrip = true;
            }
            _leftController.poseValid = poses[leftIndex].bPoseIsValid && poses[leftIndex].bDeviceIsConnected;
            if (_leftController.poseValid) {
                auto& av = poses[leftIndex].vAngularVelocity;
                _leftController.angularVelMag = std::sqrt(av.v[0]*av.v[0] + av.v[1]*av.v[1] + av.v[2]*av.v[2]);
            }
        }

        // Get right controller state
        _rightController.valid = false;
        _rightController.poseValid = false;
        bool rightGotState = false;
        vr::VRControllerState_t rightRawState{};
        if (rightIndex != vr::k_unTrackedDeviceIndexInvalid) {
            rightGotState = openvrHook.GetControllerStateUnfiltered(rightIndex, &rightRawState, sizeof(rightRawState));
            if (!rightGotState) {
                rightGotState = vrSystem->GetControllerState(rightIndex, &rightRawState, sizeof(rightRawState));
            }
            if (rightGotState) {
                _rightController.current = rightRawState.ulButtonPressed;
                _rightController.triggerValue = rightRawState.rAxis[1].x;
                _rightController.gripValue = rightRawState.rAxis[2].x;
                _rightController.thumbstickX = rightRawState.rAxis[0].x;
                _rightController.thumbstickY = rightRawState.rAxis[0].y;
                _rightController.valid = true;
                if (rightRawState.rAxis[2].x > 0.1f) _rightController.hasAnalogGrip = true;
            }
            _rightController.poseValid = poses[rightIndex].bPoseIsValid && poses[rightIndex].bDeviceIsConnected;
            if (_rightController.poseValid) {
                auto& av = poses[rightIndex].vAngularVelocity;
                _rightController.angularVelMag = std::sqrt(av.v[0]*av.v[0] + av.v[1]*av.v[1] + av.v[2]*av.v[2]);
            }
        }

    }

    bool VRInput::IsPressed(bool isLeftHand, VRButton button) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return false;
        
        // Grip: prefer analog grip axis (rAxis[2]) — immune to SteamVR binding remaps.
        // The digital ulButtonPressed bit IS remapped by SteamVR, so with e.g. Grip<>A swap
        // pressing physical A would set the digital grip bit and falsely trigger our grab.
        // Fall back to digital only when analog data is absent this frame.
        if (button == VRButton::Grip) {
            if (state.gripValue > 0.5f) return true;    // Analog: pressed
            if (state.gripValue > 0.01f) return false;  // Analog present but below threshold
            // Analog reads ~0: either grip not pressed (normal), or analog data missing
            // (e.g. FO4VRTools/hook issue). Fall through to digital as safety net.
            // Digital may be affected by SteamVR remaps, but no detection is worse.
            return (state.current & ButtonMask(VRButton::Grip)) != 0;
        }

        // Special handling for trigger - use analog value
        if (button == VRButton::Trigger) {
            return state.triggerValue > 0.5f;
        }

        // A/X buttons: SteamVR may swap Grip <-> A in legacy bindings.
        // With such a swap, physical A sets bit 2 (Grip digital) instead of bit 7,
        // and physical Grip sets bit 7 (A digital) instead of bit 2.
        // Use the analog grip axis to disambiguate — physical A/X doesn't move the
        // analog grip, but physical Grip does:
        //   bit set + low analog grip  = physical A/X press  → detect
        //   bit set + high analog grip = physical Grip press → ignore
        if (button == VRButton::A /* == VRButton::X, both bit 7 */) {
            constexpr uint64_t aBit = 1ULL << 7;   // Native A/X bit
            constexpr uint64_t gripBit = 1ULL << 2; // Native Grip bit (target of A→Grip remap)
            bool bit7 = (state.current & aBit) != 0;
            bool bit2 = (state.current & gripBit) != 0;

            if (state.hasAnalogGrip) {
                // Accept either bit, as long as the physical grip isn't engaged.
                // This catches both normal (A→bit7) and remapped (A→bit2) bindings.
                return (bit7 || bit2) && state.gripValue < 0.2f;
            }
            // No analog grip data: can only check native bit 7
            return bit7;
        }

        return (state.current & ButtonMask(button)) != 0;
    }

    uint64_t VRInput::GetRawButtonMask(bool isLeftHand) const
    {
        const auto& state = GetControllerState(isLeftHand);
        return state.current;
    }

    bool VRInput::JustPressed(bool isLeftHand, VRButton button) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return false;
        if (button == VRButton::Trigger) {
            return state.triggerValue > 0.5f && state.previousTriggerValue <= 0.5f;
        }
        uint64_t mask = ButtonMask(button);
        return (state.current & mask) != 0 && (state.previous & mask) == 0;
    }

    bool VRInput::JustReleased(bool isLeftHand, VRButton button) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return false;
        if (button == VRButton::Trigger) {
            return state.triggerValue <= 0.5f && state.previousTriggerValue > 0.5f;
        }
        uint64_t mask = ButtonMask(button);
        return (state.current & mask) == 0 && (state.previous & mask) != 0;
    }

    float VRInput::GetTriggerValue(bool isLeftHand) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return 0.0f;
        return state.triggerValue;
    }

    float VRInput::GetGripValue(bool isLeftHand) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return 0.0f;
        // Prefer analog axis, fall back to digital button (1.0 or 0.0)
        if (state.gripValue > 0.01f) return state.gripValue;
        return (state.current & ButtonMask(VRButton::Grip)) != 0 ? 1.0f : 0.0f;
    }

    float VRInput::GetThumbstickX(bool isLeftHand) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return 0.0f;
        return state.thumbstickX;
    }

    float VRInput::GetThumbstickY(bool isLeftHand) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return 0.0f;
        return state.thumbstickY;
    }

    float VRInput::GetAngularVelocityMagnitude(bool isLeftHand) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return 0.0f;
        return state.angularVelMag;
    }

    void VRInput::TriggerHaptic(bool isLeftHand, unsigned short durationMicroseconds)
    {
        auto* vrSystem = static_cast<vr::IVRSystem*>(_vrSystem);
        if (!vrSystem) return;

        const auto& state = GetControllerState(isLeftHand);
        if (state.deviceIndex == 0xFFFFFFFF) return;

        vrSystem->TriggerHapticPulse(state.deviceIndex, 0, durationMicroseconds);
    }
}
