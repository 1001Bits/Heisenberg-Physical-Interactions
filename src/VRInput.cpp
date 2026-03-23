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

        // Get left controller state
        // Use standard GetControllerState (goes through vtable hook path which has correct
        // SteamVR input binding data). The Heisenberg callback only masks ulButtonPressed/ulButtonTouched,
        // rAxis values are untouched so we get real analog grip/trigger values.
        _leftController.valid = false;
        if (leftIndex != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if (vrSystem->GetControllerState(leftIndex, &state, sizeof(state))) {
                _leftController.current = state.ulButtonPressed;
                _leftController.triggerValue = state.rAxis[1].x;
                _leftController.gripValue = state.rAxis[2].x;
                _leftController.thumbstickX = state.rAxis[0].x;
                _leftController.thumbstickY = state.rAxis[0].y;
                _leftController.valid = true;
                if (state.rAxis[2].x > 0.1f) _leftController.hasAnalogGrip = true;
                // Grip hysteresis for analog controllers (Index): grab at 0.5, release at 0.3
                if (_leftController.hasAnalogGrip) {
                    bool wasGrip = (_leftController.previous & ButtonMask(VRButton::Grip)) != 0;
                    float threshold = wasGrip ? 0.3f : 0.5f;
                    if (state.rAxis[2].x > threshold)
                        _leftController.current |= ButtonMask(VRButton::Grip);
                    else
                        _leftController.current &= ~ButtonMask(VRButton::Grip);
                }
            }
            if (poses[leftIndex].bPoseIsValid) {
                auto& av = poses[leftIndex].vAngularVelocity;
                _leftController.angularVelMag = std::sqrt(av.v[0]*av.v[0] + av.v[1]*av.v[1] + av.v[2]*av.v[2]);
            }
        }

        // Get right controller state
        _rightController.valid = false;
        if (rightIndex != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if (vrSystem->GetControllerState(rightIndex, &state, sizeof(state))) {
                _rightController.current = state.ulButtonPressed;
                _rightController.triggerValue = state.rAxis[1].x;
                _rightController.gripValue = state.rAxis[2].x;
                _rightController.thumbstickX = state.rAxis[0].x;
                _rightController.thumbstickY = state.rAxis[0].y;
                _rightController.valid = true;
                if (state.rAxis[2].x > 0.1f) _rightController.hasAnalogGrip = true;
                // Grip hysteresis for analog controllers (Index): grab at 0.5, release at 0.3
                if (_rightController.hasAnalogGrip) {
                    bool wasGrip = (_rightController.previous & ButtonMask(VRButton::Grip)) != 0;
                    float threshold = wasGrip ? 0.3f : 0.5f;
                    if (state.rAxis[2].x > threshold)
                        _rightController.current |= ButtonMask(VRButton::Grip);
                    else
                        _rightController.current &= ~ButtonMask(VRButton::Grip);
                }
            }
            if (poses[rightIndex].bPoseIsValid) {
                auto& av = poses[rightIndex].vAngularVelocity;
                _rightController.angularVelMag = std::sqrt(av.v[0]*av.v[0] + av.v[1]*av.v[1] + av.v[2]*av.v[2]);
            }
        }
    }

    bool VRInput::IsPressed(bool isLeftHand, VRButton button) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return false;
        
        // Grip: use ONLY the physical analog grip axis (rAxis[2]).
        // This is immune to SteamVR button remapping — always reflects physical grip hardware.
        // The digital ulButtonPressed bit IS remapped by SteamVR, so with e.g. Grip<>A swap
        // pressing physical A would set the digital grip bit and falsely trigger our grab.
        // Only fall back to digital bit for controllers that never report analog grip data
        // (rare — all modern VR controllers have analog grip).
        if (button == VRButton::Grip) {
            if (state.hasAnalogGrip) {
                return state.gripValue > 0.5f;  // Analog only — always physical grip
            }
            // No analog data ever seen — digital fallback (may be affected by remaps)
            return (state.current & ButtonMask(VRButton::Grip)) != 0;
        }

        // Special handling for trigger - use analog value
        if (button == VRButton::Trigger) {
            return state.triggerValue > 0.5f;
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
        uint64_t mask = ButtonMask(button);
        return (state.current & mask) != 0 && (state.previous & mask) == 0;
    }

    bool VRInput::JustReleased(bool isLeftHand, VRButton button) const
    {
        const auto& state = GetControllerState(isLeftHand);
        if (!state.valid) return false;
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
