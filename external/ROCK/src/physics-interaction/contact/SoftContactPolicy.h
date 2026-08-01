#pragma once

#include <cmath>

namespace rock::soft_contact_policy
{
    struct WorldContactChannels
    {
        bool handPushback = false;
        bool weaponWallCollision = false;

        [[nodiscard]] constexpr bool anyEnabled() const noexcept
        {
            return handPushback || weaponWallCollision;
        }
    };

    // Free-hand presentation and equipped-weapon collision share this runtime
    // but are separate authority channels. Turning off hand pushback must not
    // make the gun clip through the world again.
    [[nodiscard]] inline constexpr WorldContactChannels
    resolveWorldContactChannels(
        bool softContactWorldEnabled,
        bool handWorldPushbackEnabled,
        bool weaponCollisionEnabled,
        bool weaponStaticWorldEnabled,
        bool worldReady,
        bool menuBlocked) noexcept
    {
        if (!worldReady || menuBlocked) {
            return {};
        }

        return {
            .handPushback =
                softContactWorldEnabled &&
                handWorldPushbackEnabled,
            .weaponWallCollision =
                weaponCollisionEnabled &&
                weaponStaticWorldEnabled,
        };
    }

    /*
     * fWorldMaxCorrection is an acquisition/discontinuity guard. It is large
     * enough to reject bogus first witnesses, but it cannot also be the travel
     * limit of an already validated hand stop plane: locomotion would carry
     * the controller beyond that fixed distance and the hand would then pass
     * through the wall.
     *
     * Only a cached HAND plane may retain the full required normal response.
     * Fresh evidence and the independent weapon channel remain capped. The
     * caller subsequently projects the rendered hand target to the same-frame
     * anatomical reach sphere, so this does not grant unbounded arm authority.
     */
    [[nodiscard]] inline float worldContactCorrectionLimit(
        bool cachedPlane,
        bool sourceIsWeapon,
        float requiredCorrection,
        float acquisitionLimit)
    {
        const float safeAcquisitionLimit =
            std::isfinite(acquisitionLimit) && acquisitionLimit > 0.0f ?
                acquisitionLimit :
                0.0f;
        if (!cachedPlane ||
            sourceIsWeapon ||
            !std::isfinite(requiredCorrection) ||
            requiredCorrection <= safeAcquisitionLimit) {
            return safeAcquisitionLimit;
        }

        return requiredCorrection;
    }
}
