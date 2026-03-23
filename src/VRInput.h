#pragma once

/**
 * VRInput - VR Controller input handling for Heisenberg
 * 
 * Reads controller button states directly from OpenVR.
 * This provides grip/trigger detection for grabbing objects.
 */

#include <cstdint>

namespace heisenberg
{
    // VR Controller button IDs (matching OpenVR EVRButtonId values)
    enum class VRButton : std::uint32_t
    {
        System = 0,
        ApplicationMenu = 1,  // Y button on Oculus/Index
        Grip = 2,
        DPad_Left = 3,
        DPad_Up = 4,
        DPad_Right = 5,
        DPad_Down = 6,
        A = 7,
        B = 1,                // B button (same as ApplicationMenu)
        Y = 1,                // Y button (same as ApplicationMenu) - left hand
        X = 7,                // X button (same as A) - left hand
        Touchpad = 32,        // k_EButton_Axis0 - also used for thumbstick press
        ThumbstickPress = 32, // Same as Touchpad - thumbstick click
        Trigger = 33,
    };

    /**
     * VRInput - Manages VR controller input state
     * 
     * Call Update() each frame before checking button states.
     */
    class VRInput
    {
    public:
        static VRInput& GetSingleton()
        {
            static VRInput instance;
            return instance;
        }

        /**
         * Initialize the VR input system
         * @return true if OpenVR is available
         */
        bool Initialize();

        /**
         * Update controller states - call once per frame
         * @param isLeftHandedMode If true, swap left/right for primary/offhand
         */
        void Update(bool isLeftHandedMode = false);

        /**
         * Check if a button is currently pressed
         * @param isLeftHand Which controller
         * @param button Which button
         * @return true if pressed
         */
        bool IsPressed(bool isLeftHand, VRButton button) const;

        /**
         * Get raw button bitmask for debugging
         * @param isLeftHand Which controller
         * @return Raw button bitmask (ulButtonPressed)
         */
        uint64_t GetRawButtonMask(bool isLeftHand) const;

        /**
         * Check if a button was just pressed this frame
         * @param isLeftHand Which controller
         * @param button Which button
         * @return true if just pressed (wasn't pressed last frame)
         */
        bool JustPressed(bool isLeftHand, VRButton button) const;

        /**
         * Check if a button was just released this frame
         * @param isLeftHand Which controller
         * @param button Which button  
         * @return true if just released (was pressed last frame)
         */
        bool JustReleased(bool isLeftHand, VRButton button) const;

        /**
         * Get the trigger axis value (0.0 - 1.0)
         * @param isLeftHand Which controller
         * @return Trigger pull amount
         */
        float GetTriggerValue(bool isLeftHand) const;

        /**
         * Get the grip axis value (0.0 - 1.0) for Index controllers
         * @param isLeftHand Which controller
         * @return Grip squeeze amount
         */
        float GetGripValue(bool isLeftHand) const;

        /**
         * Get the thumbstick X axis value (-1.0 to 1.0)
         * @param isLeftHand Which controller
         * @return Thumbstick X position (left = -1, right = +1)
         */
        float GetThumbstickX(bool isLeftHand) const;

        /**
         * Get the thumbstick Y axis value (-1.0 to 1.0)
         * @param isLeftHand Which controller
         * @return Thumbstick Y position (down = -1, up = +1)
         */
        float GetThumbstickY(bool isLeftHand) const;

        /**
         * Get the angular velocity magnitude of a controller (rad/s)
         * @param isLeftHand Which controller
         * @return Angular velocity magnitude in radians/second
         */
        float GetAngularVelocityMagnitude(bool isLeftHand) const;

        /**
         * Trigger haptic feedback
         * @param isLeftHand Which controller
         * @param durationMicroseconds Duration of vibration
         */
        void TriggerHaptic(bool isLeftHand, unsigned short durationMicroseconds = 1000);

        /**
         * Check if VR system is available
         */
        bool IsAvailable() const { return _initialized; }

        /**
         * Check if left-handed mode is active
         * @return true if the game is in left-handed VR mode
         */
        bool IsLeftHandedMode() const { return _isLeftHandedMode; }

    private:
        VRInput() = default;
        
        // Internal state - opaque to avoid OpenVR header dependency
        struct ControllerStateInternal;
        
        bool _initialized = false;
        bool _useRawInput = false;  // True if using raw IVRSystem (bypassing hook)
        bool _isLeftHandedMode = false;  // True if game is in left-handed VR mode
        bool _leftHandedModeChecked = false;  // True after we've read left-handed mode from game
        bool _pauseMenuWasOpen = false;  // Track pause menu state to detect when it closes
        void* _vrSystem = nullptr;  // vr::IVRSystem*
        
        // Button state packed into uint64
        struct ButtonState
        {
            uint64_t current = 0;
            uint64_t previous = 0;
            float triggerValue = 0.0f;
            float gripValue = 0.0f;
            float thumbstickX = 0.0f;
            float thumbstickY = 0.0f;
            float angularVelMag = 0.0f;  // Angular velocity magnitude (rad/s)
            uint32_t deviceIndex = 0xFFFFFFFF;
            bool valid = false;
            bool hasAnalogGrip = false;  // True once we've seen gripValue > threshold
        };
        
        ButtonState _leftController;
        ButtonState _rightController;
        
        // Helper to get button mask
        static uint64_t ButtonMask(VRButton button) 
        { 
            return 1ull << static_cast<uint32_t>(button); 
        }
        
        // Returns PHYSICAL controller state. isLeftHand = physical left controller.
        // No swap for left-handed mode: Hand objects track physical controllers
        // (via wand nodes), so they must read input from the same physical controller.
        // Left-handed mode logic lives in the game logic layer (IsPrimaryHand() etc).
        const ButtonState& GetControllerState(bool isLeftHand) const
        {
            return isLeftHand ? _leftController : _rightController;
        }
    };

    // Convenient global access
    inline VRInput& g_vrInput = VRInput::GetSingleton();
}
