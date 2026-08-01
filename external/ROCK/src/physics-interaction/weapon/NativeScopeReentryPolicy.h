#pragma once

namespace rock::native_scope_reentry_policy
{
    /*
     * Beginning a support grip while already scoped is an in-scope pose/
     * authority transition, not a request to leave the optic. Only the
     * deliberate falling edge closes scope presentation. Keeping this edge
     * rule pure makes the host bridge unable to accidentally restore the old
     * "grip-on closes ScopeMenu" regression.
     */
    [[nodiscard]] inline constexpr bool shouldExitForSupportGripTransition(
        const bool wasSupportGripped,
        const bool isSupportGripped) noexcept
    {
        return wasSupportGripped && !isSupportGripped;
    }

    struct Decision
    {
        bool nativeScopeRequested{ false };
        /*
         * FO4VR uses the geometry decision a second time immediately after
         * changing scope state. `true` bypasses its near-eye approach-blackout
         * calculation. This normally equals nativeScopeRequested, except while
         * an armed re-entry latch consumes an inside-cone request: the scope
         * must stay closed, but the normal world must not remain black merely
         * because the optic is still beside the eye.
         */
        bool bypassNativeApproachFade{ false };
        bool blockRemainsArmed{ false };
        bool rearmedThisFrame{ false };
    };

    /*
     * Closing a scope while its ocular is still inside Bethesda's activation
     * cone otherwise makes the next native update open it again immediately.
     * While armed, consume every inside-cone decision.  The first outside-cone
     * decision clears the block but is still kept false; only a later
     * outside->inside transition may activate the scope again.
     */
    [[nodiscard]] inline constexpr Decision filter(
        const bool blockArmed,
        const bool nativeScopeRequested) noexcept
    {
        if (!blockArmed) {
            return Decision{
                .nativeScopeRequested = nativeScopeRequested,
                .bypassNativeApproachFade = nativeScopeRequested,
            };
        }

        if (nativeScopeRequested) {
            return Decision{
                .nativeScopeRequested = false,
                .bypassNativeApproachFade = true,
                .blockRemainsArmed = true,
            };
        }

        return Decision{
            .nativeScopeRequested = false,
            .blockRemainsArmed = false,
            .rearmedThisFrame = true,
        };
    }
}
