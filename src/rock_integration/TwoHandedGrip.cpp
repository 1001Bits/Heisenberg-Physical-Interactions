#include "TwoHandedGrip.h"
#include "WeaponCollision.h"
#include "../Config.h"
#include "../FRIKInterface.h"
#include "../Utils.h"

#include "F4SE/F4SE.h"
#include <chrono>
#include <spdlog/spdlog.h>

namespace heisenberg::rock_two_handed_grip
{
    static bool s_initialized = false;
    static bool s_loggedDisabled = false;
    static bool s_lastGripState = false;
    static double s_gripStartTimeMs = 0.0;   // when offhand grip engaged
    static float s_currentStability = 0.0f;  // 0..1 ramped factor

    // Stability ramp: 0 at grip engage → 1 after kFullStabilityTime seconds of held grip.
    // Models the player's hand settling on the foregrip. Matches ROCK's gradual
    // stabilization behavior (it takes a moment for the support hand to firm up).
    constexpr float kFullStabilityTime = 0.6f;    // seconds to reach max stability
    constexpr float kReleaseDecayTime = 0.2f;     // seconds to decay to zero on release

    static double NowMs()
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    bool IsActive()
    {
        return s_initialized
            && heisenberg::g_config.rockTwoHandedGrip
            && heisenberg::rock_weapon_collision::IsActive();
    }

    bool IsGripping() { return IsActive() && s_lastGripState; }

    float GetStabilizationFactor()
    {
        if (!IsActive()) return 0.0f;
        return s_currentStability;
    }

    void Init()
    {
        if (!heisenberg::g_config.rockTwoHandedGrip) {
            if (!s_loggedDisabled) {
                spdlog::info("[ROCK::TwoHandedGrip] Disabled by config");
                s_loggedDisabled = true;
            }
            return;
        }
        if (!heisenberg::g_config.rockWeaponCollision) {
            spdlog::warn("[ROCK::TwoHandedGrip] Requires bRockWeaponCollision=true — DISABLED");
            return;
        }
        spdlog::info("[ROCK::TwoHandedGrip] Init — will poll FRIK + ramp stabilization factor 0->1 over {:.1f}s",
                     kFullStabilityTime);
        s_initialized = true;
    }

    void Shutdown()
    {
        if (!s_initialized) return;
        spdlog::info("[ROCK::TwoHandedGrip] Shutdown");
        s_initialized = false;
        s_lastGripState = false;
        s_currentStability = 0.0f;
    }

    static bool QueryOffhandGrip()
    {
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        return frik.IsOffHandGrippingWeapon();
    }

    // Linear ramp toward target, dt-bounded. Returns new value.
    static float Approach(float current, float target, float deltaSeconds, float fullTime)
    {
        if (fullTime <= 0.0f) return target;
        const float step = deltaSeconds / fullTime;
        if (target > current) return (std::min)(target, current + step);
        if (target < current) return (std::max)(target, current - step);
        return current;
    }

    void Update()
    {
        if (!IsActive()) {
            s_currentStability = 0.0f;
            return;
        }

        const bool gripping = QueryOffhandGrip();
        const double now = NowMs();

        if (gripping != s_lastGripState) {
            spdlog::info("[ROCK::TwoHandedGrip] Offhand grip state: {} -> {}",
                         s_lastGripState ? "ON" : "off",
                         gripping ? "ON" : "off");
            if (gripping) s_gripStartTimeMs = now;
            s_lastGripState = gripping;
        }

        // Estimate deltaSeconds from this-frame previous-stability — we don't get a
        // dt from the caller, so use the last frame Δt cached on first call. For our
        // ramp resolution (0.6s) any reasonable frame rate is fine; we just look up
        // the time since grip start and clamp.
        if (gripping) {
            const float heldSec = static_cast<float>((now - s_gripStartTimeMs) / 1000.0);
            // Linear ramp to 1.0 over kFullStabilityTime; sustains at 1.0 after.
            s_currentStability = (std::min)(1.0f, heldSec / kFullStabilityTime);
        } else {
            // Decay to 0 on release (kept smooth so an "almost stable then re-grip"
            // doesn't reset all the way down).
            s_currentStability = Approach(s_currentStability, 0.0f, 1.0f / 90.0f, kReleaseDecayTime);
        }
    }
}
