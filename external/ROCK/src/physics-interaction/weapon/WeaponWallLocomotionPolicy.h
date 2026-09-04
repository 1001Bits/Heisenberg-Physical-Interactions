#pragma once

#include <algorithm>
#include <cmath>

namespace rock::weapon_wall_locomotion_policy
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Input
    {
        bool stopActive = false;
        bool targetIsDynamicHand = false;
        bool stopPoseApplied = false;
        bool playerSpaceValid = false;
        bool playerSpaceDiscontinuous = false;
        bool locomotionVelocityValid = false;
        bool roomScaleDeltaValid = false;
        Vec3 locomotionVelocityGameUnitsPerSecond{};
        Vec3 roomScaleDeltaWorldGameUnits{};
        Vec3 wallNormalWorld{};
        float deltaSeconds = 0.0f;
    };

    struct RoomScaleHistory
    {
        bool valid = false;
        Vec3 previousHmdPlayerLocal{};
    };

    struct RoomScaleSample
    {
        bool deltaValid = false;
        bool discontinuous = false;
        Vec3 deltaPlayerLocalGameUnits{};
    };

    struct Decision
    {
        bool apply = false;
        Vec3 correctionDisplacementGameUnits{};
        float durationSeconds = 0.0f;
    };

    inline constexpr float kMinimumHorizontalNormalLength = 0.5f;
    inline constexpr float kMinimumInwardDeltaGameUnits = 0.01f;
    inline constexpr float kMaximumCorrectionGameUnits = 8.0f;
    inline constexpr float kMinimumDurationSeconds = 1.0f / 240.0f;
    inline constexpr float kMaximumDurationSeconds = 0.05f;
    inline constexpr float kMaximumAcceptedFrameSeconds = 0.1f;
    // A headset cannot physically traverse this distance during one accepted
    // frame. Treat it as a recenter/tracking discontinuity, rebase, and never
    // turn it into a character-controller impulse.
    inline constexpr float kMaximumRoomScaleDeltaGameUnits = 12.0f;

    [[nodiscard]] inline bool isFinite(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) &&
               std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    [[nodiscard]] inline RoomScaleSample sampleRoomScaleMotion(
        RoomScaleHistory& history,
        bool playerSpaceValid,
        bool hmdFrameValid,
        const Vec3& hmdPlayerLocalGameUnits) noexcept
    {
        RoomScaleSample sample{};
        if (!playerSpaceValid ||
            !hmdFrameValid ||
            !isFinite(hmdPlayerLocalGameUnits)) {
            history = {};
            return sample;
        }

        if (!history.valid) {
            history.valid = true;
            history.previousHmdPlayerLocal =
                hmdPlayerLocalGameUnits;
            return sample;
        }

        sample.deltaPlayerLocalGameUnits = Vec3{
            .x = hmdPlayerLocalGameUnits.x -
                 history.previousHmdPlayerLocal.x,
            .y = hmdPlayerLocalGameUnits.y -
                 history.previousHmdPlayerLocal.y,
            .z = hmdPlayerLocalGameUnits.z -
                 history.previousHmdPlayerLocal.z,
        };
        history.previousHmdPlayerLocal = hmdPlayerLocalGameUnits;

        const float deltaLengthSquared =
            sample.deltaPlayerLocalGameUnits.x *
                sample.deltaPlayerLocalGameUnits.x +
            sample.deltaPlayerLocalGameUnits.y *
                sample.deltaPlayerLocalGameUnits.y +
            sample.deltaPlayerLocalGameUnits.z *
                sample.deltaPlayerLocalGameUnits.z;
        if (!std::isfinite(deltaLengthSquared) ||
            deltaLengthSquared >
                kMaximumRoomScaleDeltaGameUnits *
                    kMaximumRoomScaleDeltaGameUnits) {
            sample.deltaPlayerLocalGameUnits = {};
            sample.discontinuous = true;
            return sample;
        }

        sample.deltaValid = true;
        return sample;
    }

    [[nodiscard]] inline Decision evaluate(const Input& input) noexcept
    {
        Decision decision{};
        if (!input.stopActive ||
            input.targetIsDynamicHand ||
            !input.stopPoseApplied ||
            !input.playerSpaceValid ||
            input.playerSpaceDiscontinuous ||
            !std::isfinite(input.deltaSeconds) ||
            input.deltaSeconds <= 0.0f ||
            input.deltaSeconds > kMaximumAcceptedFrameSeconds ||
            !std::isfinite(input.wallNormalWorld.x) ||
            !std::isfinite(input.wallNormalWorld.y)) {
            return decision;
        }

        const bool locomotionFinite =
            input.locomotionVelocityValid &&
            isFinite(input.locomotionVelocityGameUnitsPerSecond);
        const bool roomScaleFinite =
            input.roomScaleDeltaValid &&
            isFinite(input.roomScaleDeltaWorldGameUnits);
        if (!locomotionFinite && !roomScaleFinite) {
            return decision;
        }

        const float horizontalNormalLength = std::sqrt(
            input.wallNormalWorld.x * input.wallNormalWorld.x +
            input.wallNormalWorld.y * input.wallNormalWorld.y);
        if (!std::isfinite(horizontalNormalLength) ||
            horizontalNormalLength < kMinimumHorizontalNormalLength) {
            return decision;
        }

        const float nx = input.wallNormalWorld.x / horizontalNormalLength;
        const float ny = input.wallNormalWorld.y / horizontalNormalLength;
        const Vec3 intendedPlayerDelta{
            .x =
                (locomotionFinite ?
                     input.locomotionVelocityGameUnitsPerSecond.x *
                         input.deltaSeconds :
                     0.0f) +
                (roomScaleFinite ?
                     input.roomScaleDeltaWorldGameUnits.x :
                     0.0f),
            .y =
                (locomotionFinite ?
                     input.locomotionVelocityGameUnitsPerSecond.y *
                         input.deltaSeconds :
                     0.0f) +
                (roomScaleFinite ?
                     input.roomScaleDeltaWorldGameUnits.y :
                     0.0f),
            .z = 0.0f,
        };
        float inwardDelta = -(
            intendedPlayerDelta.x * nx +
            intendedPlayerDelta.y * ny);
        if (!std::isfinite(inwardDelta) ||
            inwardDelta <= kMinimumInwardDeltaGameUnits) {
            return decision;
        }

        const float horizontalDeltaLength = std::sqrt(
            intendedPlayerDelta.x * intendedPlayerDelta.x +
            intendedPlayerDelta.y * intendedPlayerDelta.y);
        const float correctionMagnitude = std::min(
            inwardDelta,
            std::min(horizontalDeltaLength, kMaximumCorrectionGameUnits));
        if (!std::isfinite(correctionMagnitude) ||
            correctionMagnitude <= kMinimumInwardDeltaGameUnits) {
            return decision;
        }

        decision.apply = true;
        decision.correctionDisplacementGameUnits = Vec3{
            .x = nx * correctionMagnitude,
            .y = ny * correctionMagnitude,
            .z = 0.0f,
        };
        decision.durationSeconds = std::clamp(
            input.deltaSeconds,
            kMinimumDurationSeconds,
            kMaximumDurationSeconds);
        return decision;
    }
}
