#pragma once

#include <cstddef>
#include <cstdint>

namespace rock::held_player_space_warp_policy
{
    enum class Action : std::uint8_t
    {
        Commit,
        Retry,
        FailClosed,
    };

    struct Input
    {
        std::size_t requiredTransformWrites = 0;
        std::size_t queuedTransformWrites = 0;
        std::size_t requiredVelocityWrites = 0;
        std::size_t queuedVelocityWrites = 0;
        std::size_t rollbackTransformWritesQueued = 0;
        std::size_t rollbackVelocityWritesQueued = 0;
        std::uint32_t consecutiveRetryCount = 0;
        std::uint32_t maxSafeRetries = 0;
        bool preflightComplete = false;
    };

    struct Decision
    {
        Action action = Action::Retry;
        const char* reason = "preflight-incomplete";
        bool rebaseHands = false;
        bool advancePlayerSpaceBaseline = false;
        bool freezeHeldUpdate = true;
        bool invalidateGrab = false;
    };

    [[nodiscard]] inline Decision evaluate(const Input& input) noexcept
    {
        const bool countsPlausible =
            input.queuedTransformWrites <= input.requiredTransformWrites &&
            input.queuedVelocityWrites <= input.requiredVelocityWrites &&
            input.rollbackTransformWritesQueued <=
                input.queuedTransformWrites &&
            input.rollbackVelocityWritesQueued <=
                input.queuedVelocityWrites;
        if (!countsPlausible) {
            return Decision{
                .action = Action::FailClosed,
                .reason = "invalid-transaction-counts",
                .freezeHeldUpdate = true,
                .invalidateGrab = true,
            };
        }

        const bool hasRequiredWrites =
            input.requiredTransformWrites > 0 &&
            input.requiredVelocityWrites > 0;
        const bool complete =
            input.preflightComplete &&
            hasRequiredWrites &&
            input.queuedTransformWrites == input.requiredTransformWrites &&
            input.queuedVelocityWrites == input.requiredVelocityWrites;
        if (complete) {
            return Decision{
                .action = Action::Commit,
                .reason = "complete",
                .rebaseHands = true,
                .advancePlayerSpaceBaseline = true,
                .freezeHeldUpdate = false,
                .invalidateGrab = false,
            };
        }

        const bool allQueuedWritesRolledBack =
            input.rollbackTransformWritesQueued ==
                input.queuedTransformWrites &&
            input.rollbackVelocityWritesQueued ==
                input.queuedVelocityWrites;
        if (allQueuedWritesRolledBack) {
            if (input.maxSafeRetries > 0 &&
                input.consecutiveRetryCount >= input.maxSafeRetries) {
                return Decision{
                    .action = Action::FailClosed,
                    .reason = "retry-exhausted",
                    .rebaseHands = false,
                    .advancePlayerSpaceBaseline = true,
                    .freezeHeldUpdate = true,
                    .invalidateGrab = true,
                };
            }
            return Decision{
                .action = Action::Retry,
                .reason = input.preflightComplete ?
                              "partial-queue-rolled-back" :
                              "preflight-incomplete",
                .rebaseHands = false,
                .advancePlayerSpaceBaseline = false,
                .freezeHeldUpdate = true,
                .invalidateGrab = false,
            };
        }

        return Decision{
            .action = Action::FailClosed,
            .reason = "rollback-incomplete",
            .rebaseHands = false,
            .advancePlayerSpaceBaseline = true,
            .freezeHeldUpdate = true,
            .invalidateGrab = true,
        };
    }
}
