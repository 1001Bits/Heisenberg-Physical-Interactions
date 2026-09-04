#include "physics-interaction/grab/PullMotionPolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    struct TestVector
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(float left, float right, float tolerance = 0.0001f)
    {
        return std::fabs(left - right) <= tolerance;
    }
}

int main()
{
    namespace policy = rock::pull_motion_policy;

    constexpr float gravity = 9.81f;
    constexpr float step = 0.01f;
    constexpr float remaining = 0.5f;
    const TestVector object{};
    const TestVector target{ 10.0f, 5.0f, 3.0f };
    const auto velocity = policy::computeDirectMotorVelocity(
        target,
        object,
        remaining,
        step,
        gravity,
        100.0f);

    // Integrating one step, including gravity, must advance on the exact
    // object-to-target line. There is no lateral/orbital steering component.
    const TestVector integratedStep{
        velocity.x * step,
        velocity.y * step,
        velocity.z * step - 0.5f * gravity * step * step,
    };
    Require(Near(integratedStep.x, target.x * step / remaining) &&
            Near(integratedStep.y, target.y * step / remaining) &&
            Near(integratedStep.z, target.z * step / remaining),
        "direct pull must integrate along the straight target vector");

    const auto horizontalVelocity = policy::computeDirectMotorVelocity(
        TestVector{ 4.0f, 0.0f, 0.0f },
        object,
        0.6f,
        1.0f / 90.0f,
        gravity,
        100.0f);
    Require(horizontalVelocity.z > 0.0f && horizontalVelocity.z < 0.1f,
        "gravity compensation must cover one physics step, not launch a full-flight ballistic arc");
    Require(Near(horizontalVelocity.z, 0.5f * gravity / 90.0f),
        "horizontal direct pull must use half-step gravity compensation");

    Require(policy::shouldApplyDirectMotor(0.25f, 0.6f, 0.2f),
        "the direct motor must remain active after the legacy impulse window until planned arrival");
    Require(!policy::shouldApplyDirectMotor(0.6f, 0.6f, 0.2f) &&
            !policy::shouldApplyDirectMotor(0.8f, 0.6f, 0.2f),
        "the direct motor must stop at planned arrival and never hover during owner grace");
    Require(!policy::shouldApplyDirectMotor(0.1f, 0.6f, 0.0f),
        "a disabled velocity window must still disable pull drive");

    const auto cappedVelocity = policy::computeDirectMotorVelocity(
        TestVector{ 100.0f, 100.0f, 100.0f },
        object,
        0.1f,
        step,
        gravity,
        10.0f);
    Require(Near(policy::vectorLength(cappedVelocity), 10.0f, 0.001f),
        "direct pull must retain the configured velocity cap");

    Require(policy::shouldRefreshTarget(false, 10.0f, 0.1f, 0.6f) &&
            policy::shouldRefreshTarget(true, 0.05f, 0.1f, 0.6f) &&
            policy::shouldRefreshTarget(true, 0.59f, 0.1f, 0.6f) &&
            !policy::shouldRefreshTarget(true, 0.61f, 0.1f, 0.6f),
        "direct pull must track the live hand for the complete motor flight and freeze only during owner grace");

    const TestVector palmDestination{ 0.0f, 0.0f, 0.0f };
    const TestVector exactProfileDestination{ 0.30f, -0.20f, 0.10f };
    const auto selectedExactDestination = policy::selectDirectDestination(
        palmDestination,
        exactProfileDestination,
        true);
    Require(Near(selectedExactDestination.x, exactProfileDestination.x) &&
            Near(selectedExactDestination.y, exactProfileDestination.y) &&
            Near(selectedExactDestination.z, exactProfileDestination.z),
        "an exact profile must make its final translated seat point the flight destination");
    const auto exactArrivalVelocity = policy::computeDirectMotorVelocity(
        selectedExactDestination,
        object,
        0.5f,
        0.01f,
        0.0f,
        100.0f);
    const TestVector exactArrival{
        exactArrivalVelocity.x * 0.5f,
        exactArrivalVelocity.y * 0.5f,
        exactArrivalVelocity.z * 0.5f,
    };
    Require(Near(exactArrival.x, exactProfileDestination.x) &&
            Near(exactArrival.y, exactProfileDestination.y) &&
            Near(exactArrival.z, exactProfileDestination.z),
        "exact-profile pull translation must leave no lateral catch correction");
    const auto invalidExactFallback = policy::selectDirectDestination(
        palmDestination,
        TestVector{ NAN, 1.0f, 2.0f },
        true);
    Require(Near(invalidExactFallback.x, palmDestination.x) &&
            Near(invalidExactFallback.y, palmDestination.y) &&
            Near(invalidExactFallback.z, palmDestination.z),
        "a non-finite exact-profile destination must fail closed to the palm");
    Require(!policy::ownerExpired(1.59f, 0.6f, 1.0f) &&
            policy::ownerExpired(1.61f, 0.6f, 1.0f),
        "direct pull must preserve the existing post-flight owner grace window");

    std::cout << "Pull motion policy tests passed\n";
    return 0;
}
