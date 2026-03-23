#include "FingerAnimator.h"
#include "FRIKInterface.h"
#include "Config.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <cstring>
#include <numeric>

namespace heisenberg
{
    void ExpandFingerToJointValues(float thumb, float index, float middle, float ring, float pinky, float out[15])
    {
        auto setFinger = [&out](int finger, float base) {
            out[finger * 3 + 0] = base;                                    // proximal
            out[finger * 3 + 1] = std::clamp(base * 0.95f, 0.0f, 1.0f);   // medial (slightly more bent)
            out[finger * 3 + 2] = std::clamp(base * 0.85f, 0.0f, 1.0f);   // distal (fingertip wraps more)
        };
        setFinger(0, thumb);
        setFinger(1, index);
        setFinger(2, middle);
        setFinger(3, ring);
        setFinger(4, pinky);
    }

    FingerAnimator::FingerAnimator()
    {
        std::fill(std::begin(_current), std::end(_current), 1.0f);
        std::fill(std::begin(_target), std::end(_target), 1.0f);
    }

    void FingerAnimator::SetTargetPose(const float values[NUM_JOINTS], float speed)
    {
        std::memcpy(_target, values, sizeof(_target));
        _speed = speed;
        _state = State::Closing;
        spdlog::debug("[FingerAnim] SetTargetPose: speed={:.1f}, thumb={:.2f}", speed, values[0]);
    }

    void FingerAnimator::RestoreOpen(float speed)
    {
        std::fill(std::begin(_target), std::end(_target), 1.0f);
        _speed = speed;
        _state = State::Opening;
        spdlog::debug("[FingerAnim] RestoreOpen: speed={:.1f}, current avg={:.2f}", speed, GetAverageCurl());
    }

    bool FingerAnimator::Update(bool isLeft, float deltaTime)
    {
        // Periodic state logging (every ~30 frames = ~0.33s at 90fps)
        static int frameCounter[2] = { 0, 0 };
        int handIdx = isLeft ? 0 : 1;
        frameCounter[handIdx]++;
        bool shouldLogPeriodic = (frameCounter[handIdx] % 30 == 0);
        
        if (_state == State::Idle) {
            return false;
        }

        auto& frik = FRIKInterface::GetSingleton();

        // During Holding: re-send current values every frame to prevent FRIK
        // controller tracking from overriding our hand pose during grabs
        if (_state == State::Holding) {
            if (shouldLogPeriodic) {
                spdlog::debug("[FingerAnim] {} hand: HOLDING avg={:.2f}", isLeft ? "Left" : "Right", GetAverageCurl());
            }
            frik.SetHandPoseJointPositions(isLeft, _current);
            return true;
        }

        // Closing or Opening: lerp toward target
        bool allReached = true;
        const float step = _speed * deltaTime;

        for (int i = 0; i < NUM_JOINTS; i++) {
            const float diff = _target[i] - _current[i];
            if (std::abs(diff) > 0.001f) {
                if (diff > 0.0f) {
                    _current[i] = (std::min)(_current[i] + step, _target[i]);
                } else {
                    _current[i] = (std::max)(_current[i] - step, _target[i]);
                }
                allReached = false;
            } else {
                _current[i] = _target[i];
            }
        }

        // Send current values to FRIK
        bool frikSuccess = frik.SetHandPoseJointPositions(isLeft, _current);

        // Log progress during Opening state (every ~10 frames)
        if (_state == State::Opening && (frameCounter[handIdx] % 10 == 0)) {
            spdlog::info("[FingerAnim] {} hand: OPENING progress avg={:.2f} target=1.0 frikOk={}",
                         isLeft ? "Left" : "Right", GetAverageCurl(), frikSuccess);
        }

        if (allReached) {
            if (_state == State::Closing) {
                _state = State::Holding;
                spdlog::info("[FingerAnim] {} hand: Closing -> Holding", isLeft ? "Left" : "Right");
            } else if (_state == State::Opening) {
                // Check config: should we release FRIK control?
                if (g_config.fingerPoseMode == 0) {
                    // FRIK mode: clear override so controller tracking resumes
                    bool cleared = frik.ClearHandPoseFingerPositions(isLeft);
                    spdlog::info("[FingerAnim] {} hand: Opening -> Idle (cleared FRIK override, success={})", isLeft ? "Left" : "Right", cleared);
                } else {
                    spdlog::info("[FingerAnim] {} hand: Opening -> Idle (keeping override)", isLeft ? "Left" : "Right");
                }
                _state = State::Idle;
            }
        }

        return true;
    }

    void FingerAnimator::ForceReset(bool isLeft)
    {
        auto& frik = FRIKInterface::GetSingleton();
        // Set fully open pose FIRST, then clear override
        std::fill(std::begin(_current), std::end(_current), 1.0f);
        std::fill(std::begin(_target), std::end(_target), 1.0f);
        frik.SetHandPoseJointPositions(isLeft, _current);
        frik.ClearHandPoseFingerPositions(isLeft);
        _state = State::Idle;
        spdlog::info("[FingerAnim] {} hand: ForceReset -> Idle (sent open pose + cleared FRIK override)", isLeft ? "Left" : "Right");
    }

    float FingerAnimator::GetAverageCurl() const
    {
        float sum = 0.0f;
        for (int i = 0; i < NUM_JOINTS; i++) {
            sum += _current[i];
        }
        return sum / static_cast<float>(NUM_JOINTS);
    }
}
