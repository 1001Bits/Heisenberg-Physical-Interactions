#pragma once

#include <cstdint>
#include <algorithm>

namespace heisenberg
{
    // Expand 5 per-finger values to 15 per-joint with natural variation
    // Joint order: [thumb_prox, thumb_med, thumb_dist, index_prox..dist, middle..., ring..., pinky...]
    // Values: 0.0 = bent, 1.0 = straight
    void ExpandFingerToJointValues(float thumb, float index, float middle, float ring, float pinky, float out[15]);

    /**
     * Per-joint finger animation controller.
     * Smoothly interpolates 15 float values (5 fingers x 3 joints) toward target poses.
     * Sends values to FRIK each frame during animation, then releases FRIK control when done.
     */
    class FingerAnimator
    {
    public:
        static constexpr int NUM_JOINTS = 15;

        enum class State : uint8_t
        {
            Idle,       // No override active, FRIK has controller tracking control
            Closing,    // Animating toward target pose (grab start)
            Holding,    // At target pose, FRIK remembers values, no per-frame sends
            Opening,    // Animating back to open hand (grab end)
        };

        FingerAnimator();

        // Set target per-joint values and begin animating toward them
        void SetTargetPose(const float values[NUM_JOINTS], float speed);

        // Begin animating back to fully open hand (all 1.0)
        void RestoreOpen(float speed);

        // Per-frame update. Lerps current values toward target and sends to FRIK.
        // Returns true if pose was sent to FRIK this frame.
        bool Update(bool isLeft, float deltaTime);

        State GetState() const { return _state; }
        bool IsActive() const { return _state != State::Idle; }
        const float* GetCurrentValues() const { return _current; }
        void SetCurrentValues(const float values[NUM_JOINTS]) { std::memcpy(_current, values, sizeof(_current)); }

        // Force-reset to Idle and clear FRIK override. Used as failsafe for stuck hands.
        void ForceReset(bool isLeft);

        // Average of all current joint values (backward compat with single-scalar GetFingerCurlValue)
        float GetAverageCurl() const;

    private:
        float _current[NUM_JOINTS];   // Current interpolated values (1.0 = open)
        float _target[NUM_JOINTS];    // Target values to animate toward
        float _speed = 4.0f;          // Animation speed (values per second)
        State _state = State::Idle;
    };
}
