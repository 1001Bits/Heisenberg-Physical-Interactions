#pragma once

#include "physics-interaction/TransformMath.h"

#include <algorithm>
#include <cmath>

namespace rock::two_handed_weapon_policy
{
    // A support grip may lengthen only the rendered support-arm chain, and only
    // modestly. The weapon and firing-hand anchor never move to satisfy reach.
    inline constexpr float kSupportArmLengthScale = 1.08f;

    [[nodiscard]] constexpr float armLengthScale(const bool supportGrip)
    {
        return supportGrip ? kSupportArmLengthScale : 1.0f;
    }

    /*
     * Re-anchoring is the final invariant of every full two-hand solve. Rotation
     * cleanup and support-hand corrections may change the weapon basis, but the
     * captured firing grip must remain exactly at the firing controller target.
     */
    template <class Transform, class Vector>
    [[nodiscard]] inline Transform reanchorAtPrimaryGrip(
        Transform weaponWorld,
        const Vector& primaryGripLocal,
        const Vector& primaryTargetWorld)
    {
        const Vector currentPrimary = transform_math::localPointToWorld(weaponWorld, primaryGripLocal);
        weaponWorld.translate.x += primaryTargetWorld.x - currentPrimary.x;
        weaponWorld.translate.y += primaryTargetWorld.y - currentPrimary.y;
        weaponWorld.translate.z += primaryTargetWorld.z - currentPrimary.z;
        return weaponWorld;
    }

    /*
     * Scalar finger values use 1=open and 0=closed. A valid front-surface curve
     * already terminates at the first mesh intersection. Unknown/back-surface
     * results must not fall back to a speculative closed curl, and a pad found
     * inside the surface must be opened completely.
     */
    [[nodiscard]] inline float meshBorderSafeFingerOpenValue(
        const float solvedOpenValue,
        const bool hasFrontSurfaceHit,
        const bool padMayBeInsideSurface)
    {
        const float finiteOpenValue = std::isfinite(solvedOpenValue) ?
            (std::max)(0.0f, solvedOpenValue) : 1.0f;
        if (!hasFrontSurfaceHit || padMayBeInsideSurface) {
            return (std::max)(finiteOpenValue, 1.0f);
        }
        return finiteOpenValue;
    }
}
