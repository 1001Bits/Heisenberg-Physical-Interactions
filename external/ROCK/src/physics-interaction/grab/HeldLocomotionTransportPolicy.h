#pragma once

#include <cmath>
#include <array>
#include <cstdint>
#include <limits>

namespace rock::held_locomotion_transport_policy
{
    inline constexpr float kMinimumRoomSpeedGameUnitsPerSecond = 1.0f;
    inline constexpr float kMaximumRoomSpeedGameUnitsPerSecond = 2000.0f;
    inline constexpr float kVelocityDeltaEpsilonSquared = 0.000001f;
    inline constexpr std::uint32_t kReadFailureTripCount = 30;

    enum class DecisionReason : std::uint8_t
    {
        AlreadyProcessed,
        DisabledIdle,
        DisabledRemoveContribution,
        UnreadableIdle,
        UnreadableGrace,
        UnreadableRemoveContribution,
        StandingIdle,
        VelocityUnchanged,
        VelocityChanged,
    };

    constexpr const char* decisionReasonName(DecisionReason reason) noexcept
    {
        switch (reason) {
        case DecisionReason::AlreadyProcessed:
            return "already-processed";
        case DecisionReason::DisabledIdle:
            return "disabled-idle";
        case DecisionReason::DisabledRemoveContribution:
            return "disabled-remove-contribution";
        case DecisionReason::UnreadableIdle:
            return "unreadable-idle";
        case DecisionReason::UnreadableGrace:
            return "unreadable-grace";
        case DecisionReason::UnreadableRemoveContribution:
            return "unreadable-remove-contribution";
        case DecisionReason::StandingIdle:
            return "standing-idle";
        case DecisionReason::VelocityUnchanged:
            return "velocity-unchanged";
        case DecisionReason::VelocityChanged:
            return "velocity-changed";
        }
        return "unknown";
    }

    template <class Vector3>
    struct RuntimeState
    {
        Vector3 appliedRoomVelocityGame{};
        std::uint64_t lastProcessedGameFrameIndex = 0;
        std::uint32_t readFailures = 0;
        bool active = false;
        bool hasProcessedGameFrame = false;
    };

    /*
     * A mechanically connected held object may contain several independent
     * Havok motions. A single object-wide state cannot describe a partial
     * deferred-write failure: committing it after motion A succeeds prevents
     * motion B from ever receiving the contribution, while retrying it adds A
     * twice. Keep the exact same policy state per motion instead.
     */
    template <class Vector3>
    struct MotionRuntimeSlot
    {
        std::uint32_t motionIndex = 0;
        RuntimeState<Vector3> state{};
        bool occupied = false;
    };

    template <class Vector3, std::size_t Capacity>
    [[nodiscard]] inline MotionRuntimeSlot<Vector3>* findOrCreateMotionSlot(
        std::array<MotionRuntimeSlot<Vector3>, Capacity>& slots,
        const std::uint32_t motionIndex) noexcept
    {
        if (motionIndex == 0) {
            return nullptr;
        }

        MotionRuntimeSlot<Vector3>* freeSlot = nullptr;
        for (auto& slot : slots) {
            if (slot.occupied && slot.motionIndex == motionIndex) {
                return &slot;
            }
            if (!slot.occupied && !freeSlot) {
                freeSlot = &slot;
            }
        }
        if (!freeSlot) {
            return nullptr;
        }

        *freeSlot = MotionRuntimeSlot<Vector3>{
            .motionIndex = motionIndex,
            .state = {},
            .occupied = true,
        };
        return freeSlot;
    }

    template <class Vector3>
    struct FrameInput
    {
        std::uint64_t gameFrameIndex = 0;
        Vector3 roomVelocityGame{};
        bool enabled = false;
        bool roomVelocityUsable = false;
    };

    template <class Vector3>
    struct Decision
    {
        RuntimeState<Vector3> nextState{};
        Vector3 velocityDeltaGame{};
        DecisionReason reason = DecisionReason::AlreadyProcessed;
        bool applyVelocityDelta = false;
    };

    template <class Vector3>
    inline bool isFiniteVector(const Vector3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    template <class Vector3>
    inline float squaredMagnitude(const Vector3& value) noexcept
    {
        return value.x * value.x + value.y * value.y + value.z * value.z;
    }

    template <class Vector3>
    inline Vector3 negate(const Vector3& value) noexcept
    {
        return Vector3{ -value.x, -value.y, -value.z };
    }

    template <class Vector3>
    inline Vector3 removeAppliedRoomVelocity(
        const Vector3& worldVelocity,
        const Vector3& appliedRoomVelocity) noexcept
    {
        return Vector3{
            worldVelocity.x - appliedRoomVelocity.x,
            worldVelocity.y - appliedRoomVelocity.y,
            worldVelocity.z - appliedRoomVelocity.z,
        };
    }

    inline constexpr bool shouldRecordHeldReleaseMotionSample(
        const bool playerSpaceWarpApplied,
        const bool warpSampleSuppressionPending) noexcept
    {
        return !playerSpaceWarpApplied &&
               !warpSampleSuppressionPending;
    }

    /*
     * A snap turn rotates the body's world velocity with its transform. Keep
     * the bookkeeping in that same post-warp world basis and reopen this game
     * frame so the transport writer can reconcile against the controller's
     * newly oriented room velocity instead of applying the old direction a
     * second time.
     */
    template <class Vector3>
    inline void rebaseForPlayerSpaceWarp(
        RuntimeState<Vector3>& state,
        const Vector3& reorientedAppliedRoomVelocity) noexcept
    {
        state.appliedRoomVelocityGame = state.active ? reorientedAppliedRoomVelocity : Vector3{};
        state.active = state.active &&
                       isFiniteVector(state.appliedRoomVelocityGame) &&
                       squaredMagnitude(state.appliedRoomVelocityGame) > kVelocityDeltaEpsilonSquared;
        if (!state.active) {
            state.appliedRoomVelocityGame = Vector3{};
        }
        state.readFailures = 0;
        state.lastProcessedGameFrameIndex = 0;
        state.hasProcessedGameFrame = false;
    }

    template <class Vector3>
    inline Decision<Vector3> evaluate(
        const RuntimeState<Vector3>& state,
        const FrameInput<Vector3>& input) noexcept
    {
        Decision<Vector3> decision{};
        decision.nextState = state;

        if (state.hasProcessedGameFrame && state.lastProcessedGameFrameIndex == input.gameFrameIndex) {
            decision.reason = DecisionReason::AlreadyProcessed;
            return decision;
        }

        auto& next = decision.nextState;
        next.hasProcessedGameFrame = true;
        next.lastProcessedGameFrameIndex = input.gameFrameIndex;

        const auto clearContribution = [&]() {
            next.appliedRoomVelocityGame = Vector3{};
            next.readFailures = 0;
            next.active = false;
        };

        if (!input.enabled) {
            if (state.active) {
                decision.velocityDeltaGame = negate(state.appliedRoomVelocityGame);
                decision.applyVelocityDelta = squaredMagnitude(decision.velocityDeltaGame) > kVelocityDeltaEpsilonSquared;
                decision.reason = DecisionReason::DisabledRemoveContribution;
            } else {
                decision.reason = DecisionReason::DisabledIdle;
            }
            clearContribution();
            return decision;
        }

        const float speedSquared = input.roomVelocityUsable ? squaredMagnitude(input.roomVelocityGame) : 0.0f;
        const bool readUsable = input.roomVelocityUsable &&
                                isFiniteVector(input.roomVelocityGame) &&
                                std::isfinite(speedSquared) &&
                                speedSquared <= kMaximumRoomSpeedGameUnitsPerSecond * kMaximumRoomSpeedGameUnitsPerSecond;
        if (!readUsable) {
            if (!state.active) {
                decision.reason = DecisionReason::UnreadableIdle;
                return decision;
            }

            next.readFailures = state.readFailures < (std::numeric_limits<std::uint32_t>::max)() ?
                                    state.readFailures + 1 :
                                    state.readFailures;
            if (next.readFailures < kReadFailureTripCount) {
                decision.reason = DecisionReason::UnreadableGrace;
                return decision;
            }

            decision.velocityDeltaGame = negate(state.appliedRoomVelocityGame);
            decision.applyVelocityDelta = squaredMagnitude(decision.velocityDeltaGame) > kVelocityDeltaEpsilonSquared;
            decision.reason = DecisionReason::UnreadableRemoveContribution;
            clearContribution();
            return decision;
        }

        next.readFailures = 0;
        const float minimumSpeedSquared =
            kMinimumRoomSpeedGameUnitsPerSecond * kMinimumRoomSpeedGameUnitsPerSecond;
        if (!state.active && speedSquared < minimumSpeedSquared) {
            decision.reason = DecisionReason::StandingIdle;
            return decision;
        }

        const Vector3 targetVelocity = speedSquared < minimumSpeedSquared ? Vector3{} : input.roomVelocityGame;
        decision.velocityDeltaGame = Vector3{
            targetVelocity.x - state.appliedRoomVelocityGame.x,
            targetVelocity.y - state.appliedRoomVelocityGame.y,
            targetVelocity.z - state.appliedRoomVelocityGame.z,
        };
        next.appliedRoomVelocityGame = targetVelocity;
        next.active = squaredMagnitude(targetVelocity) > 0.0f;
        decision.applyVelocityDelta = squaredMagnitude(decision.velocityDeltaGame) > kVelocityDeltaEpsilonSquared;
        decision.reason = decision.applyVelocityDelta ?
                              DecisionReason::VelocityChanged :
                              DecisionReason::VelocityUnchanged;
        return decision;
    }
}
