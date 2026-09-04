#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rock::pull_catch_retry_policy
{
    /*
     * A failed pull-catch commit can be expensive: the grab path prepares and
     * rescans the complete object body set before late pocket validation can
     * reject it.  Keep transient retries, but never repeat that work every
     * rendered frame while the same grip and target remain unchanged.
     */
    struct PoseSample
    {
        std::array<float, 3> position{};
        std::array<float, 9> rotation{};
        bool valid = false;
    };

    struct Config
    {
        float retryIntervalSeconds = 0.18f;
        float meaningfulPositionDeltaGameUnits = 2.5f;
        float meaningfulRotationDeltaDegrees = 7.5f;
        std::uint32_t maxFailedAttempts = 4;
    };

    struct RuntimeState
    {
        bool hasFailedAttempt = false;
        PoseSample lastFailedPose{};
        float elapsedSeconds = 0.0f;
        float retryElapsedSeconds = 0.0f;
        std::uint32_t failedAttempts = 0;
    };

    enum class Status
    {
        Inactive,
        Waiting,
        Attempt,
        RetryWindowExpired,
        AttemptBudgetExhausted,
    };

    struct Decision
    {
        Status status = Status::Inactive;
        const char* reason = "inactive";

        [[nodiscard]] bool shouldAttempt() const { return status == Status::Attempt; }
        [[nodiscard]] bool expired() const
        {
            return status == Status::RetryWindowExpired || status == Status::AttemptBudgetExhausted;
        }
    };

    [[nodiscard]] inline float finiteNonNegative(float value, float fallback = 0.0f)
    {
        return std::isfinite(value) && value >= 0.0f ? value : fallback;
    }

    struct CurrentMeshSnapDecision
    {
        bool attempt = false;
        float maxDistanceGameUnits = 0.0f;
        const char* reason = "not-verified-pull-catch";
    };

    /*
     * Pull arrival can retain a collision point from before the object moved.
     * If the normal close-grab mesh queries cannot use that stale point, permit
     * one position-only query from the current palm pocket against the freshly
     * extracted, current-world mesh.  This is deliberately gated by the exact
     * pull-catch identity and only extends the normal snap envelope by a bounded
     * amount; ordinary close grabs keep their strict mesh-contact contract.
     */
    [[nodiscard]] inline CurrentMeshSnapDecision evaluateCurrentMeshSnap(
        bool verifiedPullCatch,
        bool meshSurfaceAlreadyFound,
        bool hasSurfaceTriangles,
        bool palmPocketValid,
        float ordinarySnapDistanceGameUnits,
        float configuredPullCatchRadiusGameUnits)
    {
        CurrentMeshSnapDecision decision{};
        if (!verifiedPullCatch) {
            return decision;
        }
        if (meshSurfaceAlreadyFound) {
            decision.reason = "mesh-surface-already-found";
            return decision;
        }
        if (!hasSurfaceTriangles) {
            decision.reason = "no-surface-triangles";
            return decision;
        }
        if (!palmPocketValid) {
            decision.reason = "invalid-palm-pocket";
            return decision;
        }

        const float ordinaryDistance = finiteNonNegative(ordinarySnapDistanceGameUnits);
        constexpr float kMaximumExtendedSnapDistanceGameUnits = 48.0f;
        const float configuredDistance = (std::min)(
            finiteNonNegative(configuredPullCatchRadiusGameUnits),
            kMaximumExtendedSnapDistanceGameUnits);
        decision.maxDistanceGameUnits = (std::max)(ordinaryDistance, configuredDistance);
        if (decision.maxDistanceGameUnits <= ordinaryDistance) {
            decision.reason = "no-bounded-envelope-extension";
            return decision;
        }

        decision.attempt = true;
        decision.reason = "verified-current-mesh-snap";
        return decision;
    }

    [[nodiscard]] inline bool allowsMissingMeshForExactProfilePullCatch(
        bool verifiedPullCatch,
        bool profileEligibleTarget,
        bool exactProfileResolved,
        bool exactAttachTransformValid,
        bool hasMeshSurfaceContact)
    {
        return verifiedPullCatch &&
               profileEligibleTarget &&
               exactProfileResolved &&
               exactAttachTransformValid &&
               !hasMeshSurfaceContact;
    }

    [[nodiscard]] inline bool finitePose(const PoseSample& pose)
    {
        if (!pose.valid) {
            return false;
        }
        for (const float component : pose.position) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        for (const float component : pose.rotation) {
            if (!std::isfinite(component)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline float positionDeltaSquared(const PoseSample& lhs, const PoseSample& rhs)
    {
        float result = 0.0f;
        for (std::size_t index = 0; index < lhs.position.size(); ++index) {
            const float delta = lhs.position[index] - rhs.position[index];
            result += delta * delta;
        }
        return result;
    }

    [[nodiscard]] inline float rotationDeltaDegrees(const PoseSample& lhs, const PoseSample& rhs)
    {
        // trace(R_lhs^T * R_rhs) yields 1 + 2*cos(theta) for rotation matrices.
        float trace = 0.0f;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                trace += lhs.rotation[row * 3 + column] * rhs.rotation[row * 3 + column];
            }
        }
        const float cosine = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
        constexpr float radiansToDegrees = 57.295779513082320876f;
        return std::acos(cosine) * radiansToDegrees;
    }

    inline void reset(RuntimeState& state)
    {
        state = {};
    }

    inline void noteFailedAttempt(RuntimeState& state, const PoseSample& pose)
    {
        state.hasFailedAttempt = true;
        state.lastFailedPose = finitePose(pose) ? pose : PoseSample{};
        state.retryElapsedSeconds = 0.0f;
        ++state.failedAttempts;
    }

    [[nodiscard]] inline Decision advance(
        RuntimeState& state,
        const PoseSample& currentPose,
        float deltaSeconds,
        float maxRetryWindowSeconds,
        const Config& config = {})
    {
        if (!state.hasFailedAttempt) {
            return { Status::Attempt, "first-attempt" };
        }

        const float dt = finiteNonNegative(deltaSeconds);
        state.elapsedSeconds += dt;
        state.retryElapsedSeconds += dt;

        const float retryWindow = finiteNonNegative(maxRetryWindowSeconds);
        if (retryWindow > 0.0f && state.elapsedSeconds > retryWindow) {
            return { Status::RetryWindowExpired, "retry-window-expired" };
        }

        const std::uint32_t maxFailedAttempts = (std::max)(1u, config.maxFailedAttempts);
        if (state.failedAttempts >= maxFailedAttempts) {
            return { Status::AttemptBudgetExhausted, "attempt-budget-exhausted" };
        }

        if (finitePose(currentPose) && finitePose(state.lastFailedPose)) {
            const float positionThreshold = finiteNonNegative(config.meaningfulPositionDeltaGameUnits, 2.5f);
            if (positionDeltaSquared(currentPose, state.lastFailedPose) >= positionThreshold * positionThreshold) {
                return { Status::Attempt, "position-changed" };
            }

            const float rotationThreshold = finiteNonNegative(config.meaningfulRotationDeltaDegrees, 7.5f);
            if (rotationDeltaDegrees(currentPose, state.lastFailedPose) >= rotationThreshold) {
                return { Status::Attempt, "rotation-changed" };
            }
        }

        const float retryInterval = std::clamp(
            finiteNonNegative(config.retryIntervalSeconds, 0.18f),
            0.05f,
            0.5f);
        if (state.retryElapsedSeconds >= retryInterval) {
            return { Status::Attempt, "retry-interval" };
        }

        return { Status::Waiting, "unchanged-pose-cooldown" };
    }
}
