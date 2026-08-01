#pragma once

namespace heisenberg::activator_hitbox_shrink_policy
{
    struct PointingState
    {
        bool left{ false };
        bool right{ false };
    };

    [[nodiscard]] constexpr PointingState clearHand(
        PointingState state,
        const bool isLeft)
    {
        if (isLeft) {
            state.left = false;
        } else {
            state.right = false;
        }
        return state;
    }

    [[nodiscard]] constexpr bool shouldShrink(const PointingState state)
    {
        return state.left || state.right;
    }

    static_assert(!shouldShrink(clearHand(PointingState{ true, false }, true)));
    static_assert(shouldShrink(clearHand(PointingState{ true, true }, true)));
    static_assert(shouldShrink(clearHand(PointingState{ true, true }, false)));
    static_assert(!shouldShrink(clearHand(clearHand(PointingState{ true, true }, true), false)));
}
