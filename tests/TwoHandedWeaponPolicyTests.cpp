#include "physics-interaction/weapon/TwoHandedWeaponPolicy.h"
#include "physics-interaction/weapon/NativeScopeReentryPolicy.h"
#include "physics-interaction/weapon/WeaponInteractionProbeFramePolicy.h"
#include "physics-interaction/contact/SoftContactPolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Matrix3
    {
        float entry[3][3]{};
    };

    struct Transform
    {
        Matrix3 rotate{};
        Vec3 translate{};
        float scale = 1.0f;
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(const float lhs, const float rhs, const float epsilon = 0.001f)
    {
        return std::abs(lhs - rhs) <= epsilon;
    }

    Matrix3 Yaw90()
    {
        Matrix3 result{};
        result.entry[0][1] = 1.0f;
        result.entry[1][0] = -1.0f;
        result.entry[2][2] = 1.0f;
        return result;
    }
}

int main()
{
    using namespace rock::two_handed_weapon_policy;

    {
        using rock::weapon_interaction_probe_frame_policy::Source;
        using rock::weapon_interaction_probe_frame_policy::select;

        Require(
            select(false, false) == Source::RootFlattened,
            "ordinary weapon acquisition must preserve the root/proxy hand "
            "path when no scope-safe frame exists");
        Require(
            select(false, true) == Source::RootFlattened,
            "a cached scope-safe frame must not replace ordinary non-scope "
            "weapon acquisition");
        Require(
            select(true, true) == Source::ScopeSafe,
            "ScopeMenu weapon acquisition must use the prepared "
            "driver-reconstructed hand frame");
        Require(
            select(true, false) == Source::Unavailable,
            "ScopeMenu weapon acquisition must defer instead of probing from "
            "hFRIK's collapsed root");
    }

    {
        using rock::native_scope_reentry_policy::filter;
        using rock::native_scope_reentry_policy::
            shouldExitForSupportGripTransition;

        Require(
            !shouldExitForSupportGripTransition(false, true),
            "acquiring an offhand support grip while scoped must preserve "
            "ScopeMenu");
        Require(
            !shouldExitForSupportGripTransition(true, true),
            "holding an established support grip must not close ScopeMenu");
        Require(
            shouldExitForSupportGripTransition(true, false),
            "releasing an established offhand support grip must close "
            "ScopeMenu");
        Require(
            !shouldExitForSupportGripTransition(false, false),
            "an idle support hand must not issue repeated scope exits");

        const auto unblockedInside = filter(false, true);
        Require(
            unblockedInside.nativeScopeRequested &&
                unblockedInside.bypassNativeApproachFade &&
                !unblockedInside.blockRemainsArmed &&
                !unblockedInside.rearmedThisFrame,
            "normal native scope entry must pass through while unblocked");

        const auto blockedInside = filter(true, true);
        Require(
            !blockedInside.nativeScopeRequested &&
                blockedInside.bypassNativeApproachFade &&
                blockedInside.blockRemainsArmed &&
                !blockedInside.rearmedThisFrame,
            "an armed scope close must consume repeated inside-cone entry "
            "without retaining the native approach blackout");

        const auto firstOutside = filter(true, false);
        Require(
            !firstOutside.nativeScopeRequested &&
                !firstOutside.bypassNativeApproachFade &&
                !firstOutside.blockRemainsArmed &&
                firstOutside.rearmedThisFrame,
            "the first outside-cone sample must rearm without activating");

        const auto laterReentry = filter(
            firstOutside.blockRemainsArmed,
            true);
        Require(
            laterReentry.nativeScopeRequested &&
                laterReentry.bypassNativeApproachFade,
            "scope activation must resume only after leave then re-enter");
    }

    {
        using rock::soft_contact_policy::worldContactCorrectionLimit;
        Require(
            Near(
                worldContactCorrectionLimit(
                    false,
                    false,
                    37.0f,
                    18.0f),
                18.0f),
            "fresh wall evidence must retain the acquisition safety cap");
        Require(
            Near(
                worldContactCorrectionLimit(
                    true,
                    true,
                    37.0f,
                    18.0f),
                18.0f),
            "the independent weapon channel must retain its bounded correction");
        Require(
            Near(
                worldContactCorrectionLimit(
                    true,
                    false,
                    37.0f,
                    18.0f),
                37.0f),
            "a validated hand stop plane must not let locomotion ratchet through at 18 gu");
        Require(
            Near(
                worldContactCorrectionLimit(
                    true,
                    false,
                    9.0f,
                    18.0f),
                18.0f),
            "ordinary cached hand contact must preserve the configured acquisition envelope");
    }

    {
        using rock::soft_contact_policy::
            resolveWorldContactChannels;
        const auto releaseChannels =
            resolveWorldContactChannels(
                true,
                false,
                true,
                true,
                true,
                false);
        Require(
            !releaseChannels.handPushback,
            "the release gate must disable free-hand wall pushback");
        Require(
            releaseChannels.weaponWallCollision,
            "disabling hand pushback must preserve equipped-weapon wall collision");

        const auto weaponDisabled =
            resolveWorldContactChannels(
                true,
                false,
                true,
                false,
                true,
                false);
        Require(
            !weaponDisabled.anyEnabled(),
            "weapon wall collision must honor its static-world setting when hand pushback is off");

        const auto menuBlocked =
            resolveWorldContactChannels(
                true,
                true,
                true,
                true,
                true,
                true);
        Require(
            !menuBlocked.anyEnabled(),
            "a blocking menu must suppress both world-contact channels");
    }

    {
        Require(
            !canUseHandOnlyWeaponWorldContactTransport(
                false,
                true,
                false),
            "FRIK API v3 must translate the weapon explicitly because the host shim only moves the skinned arm");
        Require(
            canUseHandOnlyWeaponWorldContactTransport(
                false,
                true,
                true),
            "native FRIK visual authority may carry an unowned child weapon with the firing hand");
        Require(
            !canUseHandOnlyWeaponWorldContactTransport(
                true,
                true,
                true),
            "a ROCK-owned weapon always requires explicit weapon transport");
        Require(
            !canUseHandOnlyWeaponWorldContactTransport(
                false,
                false,
                true),
            "hand-only transport requires a valid firing-hand world transform");
    }

    {
        Require(
            shouldPublishPrimaryHandAuthority(false, false),
            "legacy FRIK must pin the right firing hand to the solved trigger grip");
        Require(
            shouldPublishPrimaryHandAuthority(false, true),
            "legacy FRIK must preserve left-firing hand authority");
        Require(
            shouldPublishPrimaryHandAuthority(true, false),
            "native FRIK visual authority must preserve right-firing hand publication");
        Require(
            shouldPublishPrimaryHandAuthority(true, true),
            "native FRIK visual authority must preserve left-firing hand publication");
        Require(
            isPrimaryGripHandAuthorityTag(
                kPrimaryGripHandAuthorityTag),
            "the legacy host must recognize the canonical primary-grip writer");
        Require(
            !isPrimaryGripHandAuthorityTag(
                kSupportGripHandAuthorityTag),
            "the primary same-frame pin must not capture support-grip writers");
    }

    {
        Require(
            usesRigidSupportGripHandAuthority(true, true, false),
            "a Gripping support hand whose solver owns the weapon must use rigid authority");
        Require(
            std::string_view(selectSupportGripHandAuthorityTag(
                true,
                true,
                false)) == kRigidSupportGripHandAuthorityTag,
            "the full two-hand solver must publish the legacy-FRIK rigid tag");

        Require(
            !usesRigidSupportGripHandAuthority(true, false, false),
            "visual-only support must retain free-hand safety");
        Require(
            !usesRigidSupportGripHandAuthority(true, true, true),
            "AttachOnly support must retain free-hand safety");
        Require(
            !usesRigidSupportGripHandAuthority(false, true, false),
            "part-carry and non-Gripping states must retain free-hand safety");
        Require(
            std::string_view(selectSupportGripHandAuthorityTag(
                false,
                true,
                false)) == kSupportGripHandAuthorityTag,
            "part-carry must publish the ordinary support tag");

        // REACHABILITY INVARIANT (Jul 31, replaces the inverted assertion this
        // slot used to hold). The old rule — "a full-authority sidearm must
        // retain its contacted support-hand weld" — became unsatisfiable once
        // sidearms started entering the full two-handed solver: no weapon class
        // could pass both terms, so the reseat silently became dead code and a
        // pistol's off-hand stayed welded to whatever mesh triangle it first
        // touched (measured 19.1gu out), forcing the rendered arm ~40% past
        // anatomy — the reported "offhand stretches unnaturally long".
        //
        // The reseat only rewrites the RENDERED hand (handWeaponLocal) and
        // clears the source frames; the solver keeps steering from the captured
        // contacted point (gripLocal, which the reseat never touches — verified
        // via resolvePartGripWorld -> resolveCurrentSupportAttachmentRoot).
        // Presentation and steering are therefore genuinely separable, and the
        // reseat MUST stay reachable in the full-solver case.
        Require(
            shouldApplySidearmPresentationReseat(
                true,
                true,
                true,
                false,
                true),
            "the sidearm presentation reseat must stay reachable while the support grip owns the weapon");
        Require(
            shouldApplySidearmPresentationReseat(
                true,
                true,
                false,
                false,
                true),
            "a visual-only sidearm may use the presentation cup/reseat");
        Require(
            !shouldApplySidearmPresentationReseat(
                true,
                false,
                false,
                false,
                true),
            "a long gun must never use the sidearm presentation reseat");
        Require(
            !shouldApplySidearmPresentationReseat(
                true,
                true,
                false,
                true,
                true),
            "AttachOnly support must retain its provider-authored frame");
    }

    {
        Transform weapon{};
        weapon.rotate = Yaw90();
        weapon.translate = Vec3{ 14000.0f, -87000.0f, 250.0f };
        const Vec3 primaryLocal{ 2.0f, -3.0f, 1.0f };
        const Vec3 primaryTarget{ 14020.0f, -86970.0f, 260.0f };

        // Simulate any post-solve translation (the removed whole-gun reach clamp),
        // then prove the invariant restores the captured primary pivot exactly even
        // at large world coordinates.
        weapon.translate.x += 17.0f;
        weapon.translate.y -= 9.0f;
        const Transform anchored = reanchorAtPrimaryGrip(weapon, primaryLocal, primaryTarget);
        const Vec3 primaryWorld = rock::transform_math::localPointToWorld(anchored, primaryLocal);
        Require(Near(primaryWorld.x, primaryTarget.x), "primary grip x must remain exact");
        Require(Near(primaryWorld.y, primaryTarget.y), "primary grip y must remain exact");
        Require(Near(primaryWorld.z, primaryTarget.z), "primary grip z must remain exact");
    }

    {
        // The rigid firing-hand anchor is the captured hand-frame origin, not
        // a palm point offset from it.  Even after a large offhand-induced
        // weapon rotation, recomposing the captured hand frame must leave the
        // wrist at the controller position exactly.
        Transform weapon{};
        weapon.rotate = Yaw90();
        weapon.translate = Vec3{ -1200.0f, 8400.0f, 95.0f };
        Transform handWeaponLocal{};
        handWeaponLocal.translate = Vec3{ 1.5f, -4.0f, 2.25f };
        const Vec3 liveWristTarget{ -1190.0f, 8412.0f, 101.0f };
        const Transform anchored = reanchorAtPrimaryGrip(
            weapon,
            handWeaponLocal.translate,
            liveWristTarget);
        const Transform recomposedHand =
            rock::transform_math::composeTransforms(
                anchored,
                handWeaponLocal);
        Require(
            Near(recomposedHand.translate.x, liveWristTarget.x) &&
                Near(recomposedHand.translate.y, liveWristTarget.y) &&
                Near(recomposedHand.translate.z, liveWristTarget.z),
            "the firing wrist must not orbit or translate when the offhand "
            "rotates the weapon");
    }

    {
        Require(Near(armLengthScale(false), 1.0f), "ordinary and primary hands must keep native reach");
        Require(Near(armLengthScale(true), 1.40f), "support grip should expose only the bounded maximum reach allowance");
    }

    {
        constexpr float nativeReach = 38.7f;
        const float localScale = adaptiveArmLengthScale(40.5f, nativeReach, armLengthScale(true));
        const float testerScale = adaptiveArmLengthScale(44.8f, nativeReach, armLengthScale(true));
        Require(localScale > 1.0f && localScale < testerScale,
            "larger tracked distances should request proportionally more visual reach");
        Require(testerScale < armLengthScale(true),
            "the tester's 44.8-unit target should remain exactly reachable below the cap");
        Require(Near(
                    projectedArmTargetDistance(44.8f, nativeReach, testerScale),
                    44.8f),
            "adaptive reach should keep the support hand seated at the grip");
    }

    {
        // Model the same physical extension under several common fVrScale values.
        // The policy consumes the resulting world-space ratio, so no hard-coded
        // reference VR scale is required.
        constexpr float nativeReach = 38.7f;
        constexpr float physicalDistanceAtScaleOne = 0.60f;
        for (const float vrScale : { 64.0f, 70.0f, 74.0f }) {
            const float requested = physicalDistanceAtScaleOne * vrScale;
            const float scale = adaptiveArmLengthScale(
                requested, nativeReach, armLengthScale(true));
            Require(Near(
                        projectedArmTargetDistance(requested, nativeReach, scale),
                        requested),
                "representative VR scales must retain an exact support-hand seat");
        }
    }

    {
        // The fVrScale=74 tester log reached 51.8 game units while the live
        // native chain was 38.7.  The old 1.20 cap stopped at 46.4 and left a
        // 5.5-unit visible wrist-to-grip gap.
        constexpr float nativeReach = 38.7f;
        constexpr float observedTesterDistance = 51.8f;
        const float scale = adaptiveArmLengthScale(
            observedTesterDistance,
            nativeReach,
            armLengthScale(true));
        Require(scale > 1.20f && scale < armLengthScale(true),
            "the observed tester reach must use the added scale headroom without saturating");
        Require(Near(
                    projectedArmTargetDistance(
                        observedTesterDistance,
                        nativeReach,
                        scale),
                    observedTesterDistance),
            "the observed fVrScale=74 support hand must stay exactly seated");
    }

    {
        // Beyond the visual stretch cap, projection must saturate monotonically.
        // The old easing curve increased and then decreased in this interval,
        // which made walk sway visibly pull the hand back and forth.
        constexpr float nativeReach = 38.7f;
        const float maxScale = armLengthScale(true);
        float previous = -1.0f;
        for (float requested = 35.0f; requested <= 60.0f; requested += 0.05f) {
            const float scale = adaptiveArmLengthScale(requested, nativeReach, maxScale);
            const float projected =
                projectedArmTargetDistance(requested, nativeReach, scale);
            Require(projected + 0.0001f >= previous,
                "reach projection must never move backward as the controller extends");
            Require(projected <= requested + 0.0001f,
                "reach projection must never move ahead of the requested target");
            previous = projected;
        }
        Require(Near(
                    previous,
                    nativeReach * maxScale - kSupportArmReachSafetyMargin),
            "far targets should settle at the bounded visual reach cap");
    }

    {
        // Lever-arm-conditioned steering authority (pistol grip fix). The live
        // measurements the thresholds came from: 10mm pistol cup 13.3gu, shortest
        // long gun 22.4gu, defaults min=15 full=22.
        Require(Near(supportSteeringAuthorityWeight(13.3f, 15.0f, 22.0f), 0.0f),
            "a pistol-length lever arm must fully demote support steering");
        Require(Near(supportSteeringAuthorityWeight(15.0f, 15.0f, 22.0f), 0.0f),
            "the minimum lever arm itself must still be demoted");
        Require(Near(supportSteeringAuthorityWeight(22.0f, 15.0f, 22.0f), 1.0f),
            "the full lever arm must grant bit-exact legacy authority");
        Require(Near(supportSteeringAuthorityWeight(22.4f, 15.0f, 22.0f), 1.0f),
            "every measured long gun must stay on the legacy path");
        Require(Near(supportSteeringAuthorityWeight(18.5f, 15.0f, 22.0f), 0.5f),
            "the smoothstep must cross one-half exactly mid-band");
        {
            // C1 smoothstep: monotonic, no overshoot, flat-tangent entry/exit.
            float previous = -1.0f;
            for (float lever = 14.0f; lever <= 23.0f; lever += 0.05f) {
                const float weight = supportSteeringAuthorityWeight(lever, 15.0f, 22.0f);
                Require(weight >= previous - 0.0001f,
                    "steering authority must never decrease as the lever arm grows");
                Require(weight >= 0.0f && weight <= 1.0f,
                    "steering authority must stay inside [0, 1]");
                previous = weight;
            }
        }
        // Fail-open contract: bad inputs and degenerate windows keep full authority.
        Require(Near(supportSteeringAuthorityWeight(
                        std::numeric_limits<float>::quiet_NaN(), 15.0f, 22.0f),
                    1.0f),
            "a non-finite lever arm must fail open to full authority");
        Require(Near(supportSteeringAuthorityWeight(18.0f, 22.0f, 15.0f), 1.0f),
            "an inverted window must fail open to full authority");
        Require(Near(supportSteeringAuthorityWeight(18.0f, 18.0f, 18.0f), 1.0f),
            "a zero-width window must fail open to full authority");

        Require(Near(
                    effectiveSupportSteeringWeight(
                        13.3f,
                        15.0f,
                        22.0f,
                        0.35f),
                    0.35f),
            "a pistol-length lever must retain the configured authority floor");
        Require(Near(
                    effectiveSupportSteeringWeight(
                        18.5f,
                        15.0f,
                        22.0f,
                        0.35f),
                    0.5f),
            "the authority floor must not reduce a larger smoothstep weight");
        Require(Near(
                    effectiveSupportSteeringWeight(
                        13.3f,
                        15.0f,
                        22.0f,
                        0.0f),
                    0.0f),
            "an explicit zero floor must preserve the opt-out contract");
        Require(Near(
                    effectiveSupportSteeringWeight(
                        13.3f,
                        15.0f,
                        22.0f,
                        1.0f),
                    1.0f),
            "the MCM maximum must give a short weapon full off-hand steering authority");
    }

    {
        Transform weld{};
        weld.rotate = Yaw90();
        weld.translate = Vec3{ 12.0f, 34.0f, 56.0f };
        Transform controller{};
        controller.rotate.entry[0][0] = 1.0f;
        controller.rotate.entry[1][1] = 1.0f;
        controller.rotate.entry[2][2] = 1.0f;
        controller.translate = Vec3{ -2.0f, -4.0f, -6.0f };

        const Transform controllerWrist =
            blendFiringWristTowardController(weld, controller, 0.0f);
        Require(
            Near(controllerWrist.rotate.entry[0][0], 1.0f) &&
                Near(controllerWrist.rotate.entry[0][1], 0.0f) &&
                Near(controllerWrist.translate.x, weld.translate.x),
            "zero wrist follow must keep controller orientation but preserve weld translation");

        const Transform rigidWrist =
            blendFiringWristTowardController(weld, controller, 1.0f);
        Require(
            Near(rigidWrist.rotate.entry[0][1], weld.rotate.entry[0][1]) &&
                Near(rigidWrist.translate.y, weld.translate.y),
            "full wrist follow must preserve the rigid weld exactly");

        Require(
            shouldBlendFiringWristFromLiveHand(true, true),
            "an exact full-authority acquisition must still honor firing-wrist follow");
        Require(
            !shouldBlendFiringWristFromLiveHand(false, false),
            "firing-wrist follow requires a live primary-hand frame");
    }

    {
        // Jul 30 regression guard: the firing hand must keep the rigid weld on
        // long guns even when the configured follow factor is 0.
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 22.0f, 15.0f, 22.0f), 1.0f),
            "a long-gun grip separation at the full lever arm must restore the rigid wrist weld");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 38.6f, 15.0f, 22.0f), 1.0f),
            "a wide long-gun grip must keep the full wrist weld");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 13.3f, 15.0f, 22.0f), 0.0f),
            "a sidearm cup under the minimum lever arm must keep the controller wrist (Jul 27)");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 15.0f, 15.0f, 22.0f), 0.0f),
            "the minimum lever arm itself must still protect the sidearm wrist");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 18.5f, 15.0f, 22.0f), 0.5f),
            "the transition band must blend the weld smoothly");
        Require(Near(firingWristFollowFactorForLeverArm(0.8f, 13.3f, 15.0f, 22.0f), 0.8f),
            "an explicit configured follow factor must act as a global minimum");
        Require(Near(firingWristFollowFactorForLeverArm(
                    std::numeric_limits<float>::quiet_NaN(), 30.0f, 15.0f, 22.0f),
                    1.0f),
            "a non-finite configured factor must defer to the lever gate");
        Require(Near(firingWristFollowFactorForLeverArm(
                    0.0f, std::numeric_limits<float>::quiet_NaN(), 15.0f, 22.0f),
                    1.0f),
            "a non-finite grip separation must fail open to the weld (safe for long guns)");
    }

    {
        Require(Near(meshBorderSafeFingerOpenValue(0.42f, true, false), 0.42f),
            "a valid first-surface curl should be preserved");
        Require(Near(meshBorderSafeFingerOpenValue(0.30f, false, false), 1.0f),
            "a mesh miss must not use the generic closed fallback");
        Require(Near(meshBorderSafeFingerOpenValue(0.55f, true, true), 1.0f),
            "a pad detected inside the mesh must be opened fully");
        Require(Near(meshBorderSafeFingerOpenValue(1.15f, false, false), 1.15f),
            "mesh safety must never close an already over-open thumb");
    }

    return 0;
}
