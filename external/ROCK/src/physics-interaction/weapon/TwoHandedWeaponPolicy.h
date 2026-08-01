#pragma once

#include "physics-interaction/TransformMath.h"

#include <algorithm>
#include <cmath>
#include <string_view>

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

    /*
     * Hand-authority writer identities are part of the embedded-ROCK/legacy-FRIK
     * contract. FRIK 0.77.12's host seam uses the rigid tag to bypass its
     * free-hand escape envelope and reach projection; the final host-side arm
     * solve supplies the bounded visual reach allowance without separating the
     * support palm from the weapon.
     *
     * Keep attach-only, visual-only, and part-carry publications on the ordinary
     * tag. Those paths deliberately do not represent an exact two-hand weld.
     */
    inline constexpr char kSupportGripHandAuthorityTag[] =
        "ROCK_WeaponSupportGrip";
    inline constexpr char kRigidSupportGripHandAuthorityTag[] =
        "ROCK_WeaponSupportGripRigid";
    inline constexpr char kPrimaryGripHandAuthorityTag[] =
        "ROCK_WeaponPrimaryGrip";

    [[nodiscard]] inline constexpr bool isPrimaryGripHandAuthorityTag(
        const std::string_view tag)
    {
        return tag == kPrimaryGripHandAuthorityTag;
    }

    [[nodiscard]] inline constexpr bool usesRigidSupportGripHandAuthority(
        const bool inExactGrippingState,
        const bool supportGripOwnsWeapon,
        const bool attachOnly)
    {
        return inExactGrippingState &&
               supportGripOwnsWeapon &&
               !attachOnly;
    }

    [[nodiscard]] inline constexpr const char* selectSupportGripHandAuthorityTag(
        const bool inExactGrippingState,
        const bool supportGripOwnsWeapon,
        const bool attachOnly)
    {
        return usesRigidSupportGripHandAuthority(
                   inExactGrippingState,
                   supportGripOwnsWeapon,
                   attachOnly) ?
                   kRigidSupportGripHandAuthorityTag :
                   kSupportGripHandAuthorityTag;
    }

    /*
     * A sidearm cup/reseat is only a presentation substitute for a support
     * hand that does not steer the weapon. Applying it during full two-hand
     * authority splits the contract: the solver continues steering from the
     * contacted part while the rendered hand is moved beside the firing hand.
     */
    /*
     * SIDEARM PRESENTATION RESEAT — reachability restored Jul 31.
     *
     * This used to require !supportGripOwnsWeapon, which made it DEAD CODE the
     * moment sidearms started entering the full two-handed solver: no weapon
     * class could satisfy both terms, so the reseat never ran once (confirmed
     * by zero "reseated" lines across a full live session) while the config
     * echo kept advertising it.
     *
     * The reseat is a PRESENTATION concern and is orthogonal to who owns the
     * weapon transform: the solver keeps steering from the real contacted part,
     * while the rendered off-hand is re-seated as a rigid offset off the FIRING
     * hand's own frame. Without it, a pistol's off-hand welds to whatever mesh
     * triangle it first touched — measured 19.1gu out — and the rendered arm
     * had to stretch ~40% past anatomy to follow it (the reported "offhand
     * stretches unnaturally long"). With it, the off-hand sits ~5.7gu from the
     * firing hand and demands only the player's natural reach.
     *
     * Gate on the weapon CLASS and the presentation preconditions only.
     */
    [[nodiscard]] inline constexpr bool shouldApplySidearmPresentationReseat(
        const bool enabled,
        const bool sidearm,
        const bool supportGripOwnsWeapon,
        const bool attachOnly,
        const bool firingHandFrameAvailable)
    {
        (void)supportGripOwnsWeapon;
        return enabled &&
               sidearm &&
               !attachOnly &&
               firingHandFrameAvailable;
    }

    // Reachability guard: the reseat MUST remain reachable for a sidearm whose
    // support grip owns the weapon transform (the full-solver case). This
    // static_assert is the durable defence — the previous predicate silently
    // became unsatisfiable and nothing failed until a user reported the stretch.
    static_assert(
        shouldApplySidearmPresentationReseat(
            /*enabled*/ true,
            /*sidearm*/ true,
            /*supportGripOwnsWeapon*/ true,
            /*attachOnly*/ false,
            /*firingHandFrameAvailable*/ true),
        "the sidearm presentation reseat must stay reachable while the support grip owns the weapon");
    static_assert(
        !shouldApplySidearmPresentationReseat(true, false, true, false, true),
        "the presentation reseat is sidearm-only: long guns keep their contacted support seat");

    /*
     * A hand-only wall correction is safe only when FRIK itself consumes the
     * published visual-authority hand transform and carries its child weapon in
     * the same frame. Legacy FRIK API v3 exposes no such native authority: the
     * Heisenberg host shim can move the skinned arm, but its primary-arm pass
     * deliberately does not consume the latched target because doing so also
     * feeds the weapon child back into the next solve. In that compatibility
     * mode ROCK must translate the weapon explicitly.
     */
    [[nodiscard]] inline constexpr bool canUseHandOnlyWeaponWorldContactTransport(
        const bool ownsWeaponTransform,
        const bool firingHandWorldAvailable,
        const bool nativeFrikVisualAuthorityAvailable)
    {
        return !ownsWeaponTransform &&
               firingHandWorldAvailable &&
               nativeFrikVisualAuthorityAvailable;
    }

    /*
     * Every full two-hand solve publishes the firing hand as well as the
     * support hand. Leaving the stock-FRIK right hand on its controller-driven
     * pose lets the rendered wrist pull out of the captured trigger grip while
     * the support hand steers the weapon.
     *
     * Native FRIK consumes this directly. The legacy host consumes the primary
     * tag in its post-FRIK same-frame pin and ROCK then republishes the solved
     * weapon transform, so the weapon's child relationship cannot feed arm
     * motion back into the solver.
     *
     * Keep the parameters for the call-site contract and explicit policy tests;
     * neither provider generation nor handedness may weaken this invariant.
     */
    [[nodiscard]] inline constexpr bool shouldPublishPrimaryHandAuthority(
        const bool nativeFrikVisualAuthorityAvailable,
        const bool firingHandIsLeft)
    {
        (void)nativeFrikVisualAuthorityAvailable;
        (void)firingHandIsLeft;
        return true;
    }

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
     * FIRING-WRIST FOLLOW BLEND (Jul 27) — the third state the authority enum never had.
     *
     * The firing hand was published as the RIGID weld compose(weaponWorld, primaryHandWeaponLocal),
     * so under the row-vector convention its rotation is L.rotate * W.rotate: every degree the
     * solver rotates the gun was demanded 1:1 of the RENDERED firing wrist, 0% controller. That is
     * correct physics for a real rigid gun, but in VR nothing forces the player's actual wrist to
     * follow, so the rendered wrist rotates away from the real one — the reported "grip of the
     * weapon hand gets disturbed". Previously the ONLY way to reduce it was to lower the steering
     * weight, which also throttled the gun, so aim authority and wrist rigidity were the same
     * scalar and no value satisfied both complaints.
     *
     * This blends the published wrist ROTATION between the weld (followFactor 1 = legacy) and the
     * live controller orientation (0 = wrist stays where the player's hand actually is), leaving
     * TRANSLATION on the weld because reanchorAtPrimaryGrip already pins that to the controller.
     * Aim authority is now free to run at full strength independently.
     */
    template <class Transform>
    [[nodiscard]] inline Transform blendFiringWristTowardController(
        const Transform& weldWorld,
        const Transform& controllerWorld,
        float followFactor)
    {
        if (!std::isfinite(followFactor) || followFactor >= 1.0f) {
            return weldWorld;  // bit-exact legacy path
        }
        const float t = (followFactor > 0.0f) ? followFactor : 0.0f;
        Transform result = weldWorld;  // keep the weld's translation and scale
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) {
                result.rotate.entry[i][k] =
                    controllerWorld.rotate.entry[i][k] +
                    (weldWorld.rotate.entry[i][k] - controllerWorld.rotate.entry[i][k]) * t;
            }
        }
        // Element-wise blending leaves the basis slightly non-orthonormal; callers orthonormalize.
        return result;
    }

    /*
     * FIRING-WRIST FOLLOW FLOOR (Jul 30) — long guns must keep the weld.
     *
     * With followFactor 0 the published firing wrist keeps pure CONTROLLER
     * rotation while the off-hand solver rotates the weapon, so on a rifle the
     * stock/trigger visibly rotate out of the firing palm — reported as "the
     * gun hand loses its grip on the stock and trigger when the gun is pushed
     * by the offhand". The firing hand must keep a constant weapon-relative
     * grip for the whole two-handed hold.
     *
     * Pistol cups are the opposite requirement (Jul 27: "gun hand grip on
     * pistol cannot be disturbed by offhand authority") — their grip
     * separation measures under the minimum steering lever arm, so scope the
     * weld by the RAW lever gate: 0 below minLeverArm (pistols keep the
     * controller wrist), smoothstep between, 1 at/above fullLeverArm (rifles
     * get the rigid weld back). Deliberately NOT effectiveSupportSteeringWeight:
     * its minimumAuthority floor is a steering concept and would leak a
     * partial weld into pistols. The configured factor acts as a global
     * minimum on top of the gate (raw gate fails open to 1.0 = weld on
     * non-finite separation, the safe direction for a long gun).
     */
    [[nodiscard]] inline float firingWristFollowFactorForLeverArm(
        const float configuredFollowFactor,
        const float gripSeparationGameUnits,
        const float minLeverArm,
        const float fullLeverArm)
    {
        const float leverWeld = supportSteeringAuthorityWeight(
            gripSeparationGameUnits, minLeverArm, fullLeverArm);
        if (!std::isfinite(configuredFollowFactor)) {
            return leverWeld;
        }
        return (configuredFollowFactor > leverWeld) ? configuredFollowFactor : leverWeld;
    }

    /*
     * Exact acquisition disables only the hand-authority position lerp.  It
     * must not disable firing-wrist follow: with followFactor=0 the primary
     * hand keeps the controller rotation even on the first full-authority
     * frame, while its translation remains welded to the firing grip.
     */
    [[nodiscard]] constexpr bool shouldBlendFiringWristFromLiveHand(
        const bool liveHandAvailable,
        const bool /*forceExact*/) noexcept
    {
        return liveHandAvailable;
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
