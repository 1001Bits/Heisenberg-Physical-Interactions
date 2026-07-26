#pragma once

#include "physics-interaction/TransformMath.h"

#include <algorithm>
#include <cmath>

namespace rock::two_handed_weapon_policy
{
    // Maximum visual-only extension of the rendered support-arm chain. The
    // runtime chooses only the scale needed to reach the grip, up to this cap;
    // the weapon and firing-hand anchor never move to satisfy support-arm reach.
    // Live tester data at fVrScale=74 reaches a required scale of 1.344 on
    // ordinary long guns.  A 1.20 cap therefore left the rendered wrist
    // 1-5.5 game units behind an otherwise exact weapon grip.  Keep this a
    // visual-only allowance and retain headroom for different calibrated
    // bodies/VR scales; neither the weapon nor the firing-hand pivot moves.
    inline constexpr float kSupportArmLengthScale = 1.40f;
    inline constexpr float kSupportArmReachSafetyMargin = 0.15f;

    [[nodiscard]] constexpr float armLengthScale(const bool supportGrip)
    {
        return supportGrip ? kSupportArmLengthScale : 1.0f;
    }

    /*
     * Convert the requested shoulder-to-grip distance into a visual arm scale.
     * Both distances are live world/game-unit measurements, so their ratio is
     * independent of fVrScale and of the user's calibrated skeleton size.
     *
     * The safety margin keeps the law-of-cosines solve just inside its numeric
     * domain. Below native reach no stretch is introduced; above it, the arm
     * grows continuously only as much as required, up to the support-only cap.
     */
    [[nodiscard]] inline float adaptiveArmLengthScale(
        const float requestedDistance,
        const float nativeReach,
        const float maximumScale,
        const float safetyMargin = kSupportArmReachSafetyMargin)
    {
        if (!std::isfinite(requestedDistance) || requestedDistance < 0.0f ||
            !std::isfinite(nativeReach) || nativeReach <= 0.0f ||
            !std::isfinite(maximumScale) || maximumScale < 1.0f ||
            !std::isfinite(safetyMargin) || safetyMargin < 0.0f) {
            return 1.0f;
        }

        const float requiredScale = (requestedDistance + safetyMargin) / nativeReach;
        return std::clamp(requiredScale, 1.0f, maximumScale);
    }

    /*
     * Final radial projection after adaptive extension. This is deliberately
     * monotonic: increasing controller distance can move the wrist outward or
     * leave it at the cap, but can never pull it backward. That invariant avoids
     * clamp/unclamp wobble when locomotion crosses the reach boundary.
     */
    [[nodiscard]] inline float projectedArmTargetDistance(
        const float requestedDistance,
        const float nativeReach,
        const float appliedScale,
        const float safetyMargin = kSupportArmReachSafetyMargin)
    {
        if (!std::isfinite(requestedDistance) || requestedDistance < 0.0f ||
            !std::isfinite(nativeReach) || nativeReach <= 0.0f ||
            !std::isfinite(appliedScale) || appliedScale < 1.0f ||
            !std::isfinite(safetyMargin) || safetyMargin < 0.0f) {
            return 0.0f;
        }

        const float reachableDistance =
            (std::max)(nativeReach * appliedScale - safetyMargin, 0.0f);
        return (std::min)(requestedDistance, reachableDistance);
    }

    /*
     * Lever-arm-conditioned support steering authority (Jul 25, pistol grip fix).
     * The full two-hand solver's angular gain is ~1/leverArm: at a pistol cup's
     * ~13gu grip separation the solved weapon orientation swings roughly twice as
     * fast per unit of off-hand motion as at a rifle's 22-38gu — and on FRIK v3
     * the firing hand's orientation delivery cannot follow, so the captured grip
     * visibly diverges (live-confirmed: 10mm cup 13.3gu vs long guns >= 22.4gu).
     * Weight 0 below minLeverArm (demote the cup to visual-only glue), C1
     * smoothstep between, 1 at/above fullLeverArm (rifles bit-exact unchanged).
     * Fail-open to 1.0 on non-finite input or a degenerate window.
     */
    [[nodiscard]] inline float supportSteeringAuthorityWeight(
        const float leverArmGameUnits,
        const float minLeverArm,
        const float fullLeverArm)
    {
        if (!std::isfinite(leverArmGameUnits) || !std::isfinite(minLeverArm) ||
            !std::isfinite(fullLeverArm) || minLeverArm >= fullLeverArm) {
            return 1.0f;
        }
        if (leverArmGameUnits <= minLeverArm) {
            return 0.0f;
        }
        if (leverArmGameUnits >= fullLeverArm) {
            return 1.0f;
        }
        const float t = (leverArmGameUnits - minLeverArm) / (fullLeverArm - minLeverArm);
        return t * t * (3.0f - 2.0f * t);
    }

    /*
     * The ONE steering weight both callers must agree on.
     *
     * Jul 27 bug this exists to prevent: the floor was applied only inside
     * updateFullWeaponAuthorityGrip, while the capture-time demotion in transitionToGripping
     * called supportSteeringAuthorityWeight() RAW. A pistol therefore still measured 0.0 at
     * capture, was demoted to VisualOnlySupport, and never entered the full-solver path at all —
     * so the floor could not take effect and the off-hand stayed dead (confirmed live: exactly one
     * "demoted to visual-only" and zero aimW log lines). Route BOTH sites through this.
     */
    [[nodiscard]] inline float effectiveSupportSteeringWeight(
        const float leverArmGameUnits,
        const float minLeverArm,
        const float fullLeverArm,
        const float minimumAuthority)
    {
        const float gated = supportSteeringAuthorityWeight(leverArmGameUnits, minLeverArm, fullLeverArm);
        if (!std::isfinite(minimumAuthority) || minimumAuthority <= 0.0f) {
            return gated;
        }
        const float floored = (minimumAuthority < 1.0f) ? minimumAuthority : 1.0f;
        return (gated > floored) ? gated : floored;
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
