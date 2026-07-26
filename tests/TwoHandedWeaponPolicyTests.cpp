#include "physics-interaction/weapon/TwoHandedWeaponPolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

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
