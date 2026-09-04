#pragma once

namespace heisenberg::post_consume_activation_policy
{
    [[nodiscard]] constexpr bool shouldKeepSuppressed(
        const bool minimumTailElapsed,
        const bool consumingHandGripHeld) noexcept
    {
        return !minimumTailElapsed || consumingHandGripHeld;
    }

    [[nodiscard]] constexpr bool shouldBlockPlayerActivation(
        const bool suppressionActive,
        const bool internalActivation,
        const bool playerInitiated,
        const bool fromScript) noexcept
    {
        return suppressionActive && !internalActivation &&
               playerInitiated && !fromScript;
    }
}
