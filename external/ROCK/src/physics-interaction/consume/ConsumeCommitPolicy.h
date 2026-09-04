#pragma once

namespace rock::consume_commit_policy
{
    enum class Trigger
    {
        None = 0,
        GripRelease,
        AutoWhileHeld,
    };

    struct Input
    {
        bool eligible = false;
        bool spatiallyInsideCoreZone = false;
        bool detectorConfirmed = false;
        bool gripReleased = false;
        bool autoConsumeWhileHeld = false;
    };

    struct Decision
    {
        Trigger trigger = Trigger::None;

        [[nodiscard]] constexpr bool shouldCommit() const noexcept
        {
            return trigger != Trigger::None;
        }
    };

    // A grip release is already an intentional action, so current geometry is
    // its only detector gate.  Auto mode remains conservative and requires the
    // existing dwell/speed-confirmed detector state.  Release wins if both
    // happen on the same frame, which keeps event/log semantics deterministic.
    [[nodiscard]] inline constexpr Decision decide(const Input& input) noexcept
    {
        if (!input.eligible || !input.spatiallyInsideCoreZone) {
            return {};
        }
        if (input.gripReleased) {
            return { Trigger::GripRelease };
        }
        if (input.autoConsumeWhileHeld && input.detectorConfirmed) {
            return { Trigger::AutoWhileHeld };
        }
        return {};
    }
}
