#pragma once

#include <algorithm>
#include <cmath>

namespace rock::pull_motion_policy
{
    inline constexpr float kDefaultPhysicsStepSeconds = 1.0f / 90.0f;
    inline constexpr float kMaximumGravityCompensationStepSeconds = 1.0f / 30.0f;

    [[nodiscard]] inline bool shouldRefreshTarget(
        bool hasPreviousTarget,
        float elapsedSeconds,
        float trackHandSeconds,
        float directMotorDurationSeconds) noexcept
    {
        const float elapsed = std::isfinite(elapsedSeconds) ? (std::max)(0.0f, elapsedSeconds) : 0.0f;
        const float track = std::isfinite(trackHandSeconds) ? (std::max)(0.0f, trackHandSeconds) : 0.0f;
        const float motorDuration = std::isfinite(directMotorDurationSeconds) ?
            (std::max)(0.0f, directMotorDurationSeconds) : 0.0f;

        // A direct motor must steer toward the live hand for every frame that
        // it drives. Freezing at the legacy short tracking window sends the
        // object to an old palm point and forces the catch solve to hook back.
        return !hasPreviousTarget || elapsed <= (std::max)(track, motorDuration);
    }

    [[nodiscard]] inline bool ownerExpired(
        float elapsedSeconds,
        float durationSeconds,
        float ownerGraceSeconds) noexcept
    {
        const float elapsed = std::isfinite(elapsedSeconds) ? (std::max)(0.0f, elapsedSeconds) : 0.0f;
        const float duration = std::isfinite(durationSeconds) ? (std::max)(0.0f, durationSeconds) : 0.0f;
        const float grace = std::isfinite(ownerGraceSeconds) ? (std::max)(0.0f, ownerGraceSeconds) : 0.0f;
        return elapsed > duration + grace;
    }

    template <class Vec3>
    [[nodiscard]] float vectorLength(const Vec3& value) noexcept
    {
        return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    }

    template <class Vec3>
    [[nodiscard]] bool isFiniteVector(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    template <class Vec3>
    [[nodiscard]] Vec3 selectDirectDestination(
        const Vec3& palmDestination,
        const Vec3& exactProfileDestination,
        bool exactProfileDestinationValid) noexcept
    {
        return exactProfileDestinationValid && isFiniteVector(exactProfileDestination) ?
            exactProfileDestination : palmDestination;
    }

    template <class Vec3>
    [[nodiscard]] Vec3 clampLength(Vec3 value, float maxLength) noexcept
    {
        if (!std::isfinite(maxLength) || maxLength <= 0.0f) {
            return value;
        }

        const float length = vectorLength(value);
        if (!std::isfinite(length) || length <= maxLength || length <= 0.0001f) {
            return value;
        }

        const float scale = maxLength / length;
        value.x *= scale;
        value.y *= scale;
        value.z *= scale;
        return value;
    }

    [[nodiscard]] inline bool shouldApplyDirectMotor(
        float elapsedSeconds,
        float durationSeconds,
        float configuredVelocityWindowSeconds) noexcept
    {
        const float elapsed = std::isfinite(elapsedSeconds) ? (std::max)(0.0f, elapsedSeconds) : 0.0f;
        const float duration = std::isfinite(durationSeconds) ? (std::max)(0.0f, durationSeconds) : 0.0f;
        const float configuredWindow = std::isfinite(configuredVelocityWindowSeconds) ?
            (std::max)(0.0f, configuredVelocityWindowSeconds) : 0.0f;

        // A positive legacy velocity window enables the motor. Straight flight
        // requires steering for the complete planned journey; ownership grace
        // remains coast/drop time and never drives the body.
        return configuredWindow > 0.0f && elapsed <= duration && (duration - elapsed) > 0.001f;
    }

    template <class Vec3>
    [[nodiscard]] Vec3 computeDirectMotorVelocity(
        const Vec3& target,
        const Vec3& objectPoint,
        float durationRemainingSeconds,
        float deltaTimeSeconds,
        float gravityMagnitude,
        float maxVelocity) noexcept
    {
        const float remaining = std::isfinite(durationRemainingSeconds) ?
            (std::max)(0.001f, durationRemainingSeconds) : 0.001f;
        const float rawStep = std::isfinite(deltaTimeSeconds) && deltaTimeSeconds > 0.0f ?
            deltaTimeSeconds : kDefaultPhysicsStepSeconds;
        const float step = (std::min)(rawStep, kMaximumGravityCompensationStepSeconds);
        const float gravity = std::isfinite(gravityMagnitude) ? (std::max)(0.0f, gravityMagnitude) : 0.0f;

        Vec3 velocity{
            (target.x - objectPoint.x) / remaining,
            (target.y - objectPoint.y) / remaining,
            (target.z - objectPoint.z) / remaining,
        };

        /*
         * Compensate only the next integration step. Under constant downward
         * gravity, +0.5*g*dt makes that step's displacement follow the direct
         * target vector. The former +0.5*g*remaining term deliberately launched
         * a full ballistic arc and was the visible circular/swooping path.
         */
        velocity.z += 0.5f * gravity * step;
        return clampLength(velocity, maxVelocity);
    }
}
