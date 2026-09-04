#include "physics-interaction/grab/DynamicGrabPolicy.h"
#include "physics-interaction/grab/GrabHeldObject.h"
#include "physics-interaction/hand/HandColliderTypes.h"
#include "physics-interaction/hand/HandVisual.h"
#include "physics-interaction/native/PhysicsShapeCastCachePolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
    struct TestVector
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct TestMatrix
    {
        float entry[3][3]{};
    };

    struct TestTransform
    {
        TestMatrix rotate{};
        TestVector translate{};
        float scale = 1.0f;
    };

    TestTransform IdentityTransform()
    {
        return rock::transform_math::makeIdentityTransform<TestTransform>();
    }

    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(const float left, const float right)
    {
        return std::fabs(left - right) < 0.0001f;
    }

    bool NearTransform(
        const TestTransform& left,
        const TestTransform& right)
    {
        if (!Near(left.translate.x, right.translate.x) ||
            !Near(left.translate.y, right.translate.y) ||
            !Near(left.translate.z, right.translate.z) ||
            !Near(left.scale, right.scale)) {
            return false;
        }
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (!Near(
                        left.rotate.entry[row][column],
                        right.rotate.entry[row][column])) {
                    return false;
                }
            }
        }
        return true;
    }
}

int main()
{
    namespace handVisual = rock::hand_visual_lerp_math;
    Require(
        !handVisual::shouldSmoothHeldObjectRelativeHand(
            true, false, false, false, true),
        "an ordinary TouchHeld grab must seat exactly in the palm without an artificial acquisition lag");
    Require(
        handVisual::shouldSmoothHeldObjectRelativeHand(
            true, true, false, false, false),
        "a non-touch pull acquisition may blend from the current clean hand");
    Require(
        handVisual::shouldSmoothHeldObjectRelativeHand(
            true, false, true, true, true),
        "the initial held-hand blend must survive phase promotion until it reaches the object-relative target");
    Require(
        !handVisual::shouldSmoothHeldObjectRelativeHand(
            true, false, true, false, false),
        "a settled held hand may follow its frozen object relation exactly");
    Require(
        handVisual::heldHandTargetDistanceIsSafe(18.0f) &&
            !handVisual::heldHandTargetDistanceIsSafe(80.0f),
        "a malformed pull-catch target must be withheld at acquisition");
    Require(
        handVisual::heldHandTargetContinuityIsSafe(
            true, 40.0f, 0.0f) &&
            !handVisual::heldHandTargetContinuityIsSafe(
                true, 40.0f, 20.0f),
        "persistent controller obstruction must remain valid while a one-frame publication jump is rejected");
    Require(
        handVisual::shouldPreferBodyDerivedHeldPose(true, 12.0f, 0.0f) &&
            handVisual::shouldPreferBodyDerivedHeldPose(true, 0.0f, 90.0f) &&
            !handVisual::shouldPreferBodyDerivedHeldPose(true, 1.0f, 2.0f),
        "a stale held scene node must cede to the authoritative body pose");
    Require(
        handVisual::targetWithinArmReach(42.0f, 40.0f) &&
            !handVisual::targetWithinArmReach(50.0f, 40.0f),
        "grab visual publication must respect the hosted arm's natural reach plus a small solver allowance");

    auto currentHeldHand = IdentityTransform();
    auto invalidHeldTarget = IdentityTransform();
    invalidHeldTarget.rotate.entry[0][0] =
        std::numeric_limits<float>::quiet_NaN();
    const auto rejectedNonfiniteStep = handVisual::advanceTransform(
        currentHeldHand,
        invalidHeldTarget,
        handVisual::kMaximumHeldHandVisualLinearSpeedGameUnitsPerSecond,
        handVisual::kMaximumHeldHandVisualAngularSpeedDegreesPerSecond,
        1.0f / 90.0f);
    Require(
        !handVisual::heldHandTransformIsUsable(invalidHeldTarget) &&
            Near(rejectedNonfiniteStep.transform.translate.x,
                currentHeldHand.translate.x) &&
            !rejectedNonfiniteStep.reachedTarget,
        "a non-finite held-node rotation must retain the last safe pose instead of reaching FRIK");
    auto degenerateHeldTarget = IdentityTransform();
    degenerateHeldTarget.rotate = {};
    auto reflectedHeldTarget = IdentityTransform();
    reflectedHeldTarget.rotate.entry[0][0] = -1.0f;
    Require(
        !handVisual::heldHandTransformIsUsable(degenerateHeldTarget) &&
            !handVisual::heldHandTransformIsUsable(reflectedHeldTarget),
        "a degenerate or reflected held-node basis must be rejected before quaternion conversion");

    using namespace rock::dynamic_grab_policy;

    Require(
        !shouldUseLegacyLocomotionAuthorityBridge(
            true, true, true) &&
            !shouldUseLegacyLocomotionAuthorityBridge(
                true, true, false) &&
            shouldUseLegacyLocomotionAuthorityBridge(
                true, false, true) &&
            shouldUseLegacyLocomotionAuthorityBridge(
                true, false, false) &&
            !shouldUseLegacyLocomotionAuthorityBridge(
                false, false, false),
        "held locomotion must choose one positional lead while allowing the independent body-velocity transport channel");

    const auto overflowKey =
        rock::physics_shape_cast_cache_policy::logarithmicOverflowKey(
            { 8000u, 3u, 1u });
    Require(overflowKey.x == 8192u && overflowKey.y == 4u &&
                overflowKey.z == 1u,
        "overflow shape cache must round axes independently instead of inflating a long thin body into a largest-axis cube");

    Require(chooseHandProcessingOrder(true, false) ==
                HandProcessingOrder::LeftThenRight,
        "right-to-left handoff must service the open left hand before release");
    Require(chooseHandProcessingOrder(false, true) ==
                HandProcessingOrder::RightThenLeft,
        "left-to-right handoff must service the open right hand before release");
    Require(chooseHandProcessingOrder(false, false) ==
                HandProcessingOrder::RightThenLeft &&
                chooseHandProcessingOrder(true, true) ==
                    HandProcessingOrder::RightThenLeft,
        "non-handoff frames must retain deterministic normal ordering");

    Require(shouldResetVelocityAtGrabCommit(false),
        "an initial capture may clear stale free-flight velocity");
    Require(!shouldResetVelocityAtGrabCommit(true),
        "joining a peer-held body must preserve live shared motion");

    Require(!allowContinuousPlayerSpaceWarp(true, true),
        "physical locomotion transport must exclude collision-bypassing continuous warps");
    Require(allowContinuousPlayerSpaceWarp(true, false),
        "legacy continuous warp remains explicitly configurable without transport");
    Require(centralPlayerSpaceOwnsVelocity(true, false) &&
                !centralPlayerSpaceOwnsVelocity(true, true),
        "exactly one standing locomotion velocity writer must own held motions");
    Require(
        rock::held_object_physics_math::shouldQueueGrabAuthorityTargetForDelta(
            0.096f) &&
            rock::held_object_physics_math::shouldQueueGrabAuthorityTargetForDelta(
                0.1f) &&
            !rock::held_object_physics_math::shouldQueueGrabAuthorityTargetForDelta(
                0.101f) &&
            !rock::held_object_physics_math::shouldQueueGrabAuthorityTargetForDelta(
                std::numeric_limits<float>::quiet_NaN()),
        "low-FPS hand samples must keep advancing the held proxy through the source clock's 100 ms interval");
    Require(heldSweepQueryFilterInfo(0x1234C02Du) == 0x000B002Bu,
        "held predictive queries must use the independent ROCK hand-query filter that reaches both world and weapon geometry");
    Require(isHeldSweepObstacleLayer(1u, true, 44u) &&
            isHeldSweepObstacleLayer(44u, false, 44u) &&
            !isHeldSweepObstacleLayer(43u, false, 44u),
        "held sweeps must stop on world or generated weapon geometry without treating hands as rigid obstacles");
    Require(
        heldObjectMayDriveVisualHand(
            true, false, false, false, false) &&
            heldObjectMayDriveVisualHand(
                false, true, false, false, false) &&
            !heldObjectMayDriveVisualHand(
                false, false, true, false, false) &&
            !heldObjectMayDriveVisualHand(
                false, false, false, true, false) &&
            heldObjectMayDriveVisualHand(
                false, false, false, true, true) &&
            !heldObjectMayDriveVisualHand(
                false, false, false, false, true),
        "ordinary TouchHeld motion must stay controller-tracked while a retained predictive wall stop keeps the visual hand rigidly attached to its stopped object");
    Require(
        shouldTreatVisualControllerSeparationAsGripLoss(false, false) &&
            !shouldTreatVisualControllerSeparationAsGripLoss(true, false) &&
            !shouldTreatVisualControllerSeparationAsGripLoss(false, true),
        "controller pressure through a retained wall stop or a discontinuity warp must not become a broken-grip deviation");
    Require(
        shouldBeginVisualReturnFromHeldWorldStop(true, false, true) &&
            !shouldBeginVisualReturnFromHeldWorldStop(false, false, true) &&
            !shouldBeginVisualReturnFromHeldWorldStop(true, true, true) &&
            !shouldBeginVisualReturnFromHeldWorldStop(true, false, false) &&
            !shouldBeginVisualReturnFromHeldWorldStop(
                true, false, true, true),
        "only a non-retreat edge leaving a published retained wall stop may begin the bounded return to controller authority");

    auto trackedAtRequestedBody = IdentityTransform();
    trackedAtRequestedBody.translate = TestVector{ 12.0f, 3.0f, -2.0f };
    auto requestedBody = IdentityTransform();
    requestedBody.translate = TestVector{ 10.0f, 0.0f, 0.0f };
    auto admittedBody = IdentityTransform();
    admittedBody.translate = TestVector{ 4.0f, 0.0f, 0.0f };
    const auto correctedTrackedHand =
        handVisual::buildRigidlyCorrectedTrackedHandWorld(
            requestedBody,
            admittedBody,
            trackedAtRequestedBody);
    const auto clearTrackedHand =
        handVisual::buildRigidlyCorrectedTrackedHandWorld(
            requestedBody,
            requestedBody,
            trackedAtRequestedBody);
    Require(
        Near(correctedTrackedHand.translate.x, 6.0f) &&
            Near(correctedTrackedHand.translate.y, 3.0f) &&
            Near(correctedTrackedHand.translate.z, -2.0f) &&
            Near(clearTrackedHand.translate.x,
                trackedAtRequestedBody.translate.x) &&
            Near(clearTrackedHand.translate.y,
                trackedAtRequestedBody.translate.y) &&
            Near(clearTrackedHand.translate.z,
                trackedAtRequestedBody.translate.z),
        "a wall stop must apply only the requested-to-admitted rigid correction to the live tracked hand, while a clear command returns exact tracking");

    auto capturedProxy = IdentityTransform();
    capturedProxy.rotate.entry[0][0] = 0.0f;
    capturedProxy.rotate.entry[0][1] = -1.0f;
    capturedProxy.rotate.entry[1][0] = 1.0f;
    capturedProxy.rotate.entry[1][1] = 0.0f;
    capturedProxy.translate = TestVector{ 7.0f, -2.0f, 5.0f };
    capturedProxy.scale = 1.25f;
    auto capturedProxyToRaw = IdentityTransform();
    capturedProxyToRaw.rotate.entry[0][0] = 0.0f;
    capturedProxyToRaw.rotate.entry[0][1] = 1.0f;
    capturedProxyToRaw.rotate.entry[1][0] = -1.0f;
    capturedProxyToRaw.rotate.entry[1][1] = 0.0f;
    capturedProxyToRaw.translate = TestVector{ 2.0f, -3.0f, 4.0f };
    capturedProxyToRaw.scale = 0.64f;
    auto capturedProxyRelation = capturedProxy;
    capturedProxyRelation.rotate =
        rock::hand_bone_collider_geometry_math::
            transposeStoredRotation(capturedProxy.rotate);
    const auto cleanRawHand = rock::transform_math::composeTransforms(
        capturedProxyRelation,
        capturedProxyToRaw);
    const auto reconstructedProxy =
        rock::hand_bone_collider_geometry_math::
            generatedColliderFrameFromObjectLocalRelation(
            cleanRawHand,
            capturedProxyToRaw);
    Require(
        NearTransform(reconstructedProxy, capturedProxy),
        "the immutable generated-proxy-to-raw relation must reconstruct the exact clean tracked proxy, including palm offset, rotation convention, and scale");
    Require(
        resolveHeldWorldSweepArmAction(
            true, false, true, 0.5f, 3.0f, 0.75f, 5.0f) ==
            HeldWorldSweepArmAction::RebaseAtLivePose,
        "the first settled TouchHeld frame must seed sweep history without casting");
    Require(
        resolveHeldWorldSweepArmAction(
            true, true, true, 12.0f, 25.0f, 0.75f, 5.0f) ==
            HeldWorldSweepArmAction::Sweep,
        "an armed held-world stop must remain active under intentional controller pressure");
    Require(
        resolveHeldWorldSweepArmAction(
            false, true, true, 0.0f, 0.0f, 0.75f, 5.0f) ==
                HeldWorldSweepArmAction::Disarm &&
            resolveHeldWorldSweepArmAction(
                true, false, true, 0.8f, 0.0f, 0.75f, 5.0f) ==
                HeldWorldSweepArmAction::Disarm &&
            resolveHeldWorldSweepArmAction(
                true,
                false,
                true,
                0.0f,
                std::numeric_limits<float>::quiet_NaN(),
                0.75f,
                5.0f) == HeldWorldSweepArmAction::Disarm,
        "acquisition phases and unsettled or invalid poses must not clamp an object on the wrong side of the hand");

    const auto clamp = evaluateSweepClamp(true, true, 0.5f, 10.0f, 1.0f);
    Require(clamp.clamped && Near(clamp.allowedDistanceGameUnits, 4.0f) &&
                Near(clamp.allowedFraction, 0.4f),
        "held shape sweep must stop one skin width before the earliest world hit");
    const auto nearStartClamp =
        evaluateSweepClamp(true, true, 0.02f, 10.0f, 1.0f);
    Require(nearStartClamp.clamped &&
                Near(nearStartClamp.allowedFraction, 0.0f),
        "a hit inside the configured skin must hold the current body position");
    const auto noHit = evaluateSweepClamp(true, false, 0.0f, 10.0f, 1.0f);
    Require(!noHit.clamped && Near(noHit.allowedFraction, 1.0f) &&
                Near(noHit.allowedDistanceGameUnits, 10.0f),
        "a clear sweep must preserve the requested translation");
    const auto invalidHit = evaluateSweepClamp(
        true,
        true,
        std::numeric_limits<float>::quiet_NaN(),
        10.0f,
        1.0f);
    Require(!invalidHit.clamped && Near(invalidHit.allowedFraction, 1.0f),
        "invalid hit evidence must not invent a clamp");

    Require(rotationalSweepSubsteps(20.0f, 10.0f, 5.0f, 100.0f, 12) == 4,
        "angular subdivision must cover the full requested rotation");
    Require(rotationalSweepSubsteps(20.0f, 100.0f, 90.0f, 5.0f, 12) == 7,
        "long held bodies must also subdivide by swept surface arc");
    Require(rotationalSweepSubsteps(180.0f, 1000.0f, 1.0f, 0.1f, 12) == 12,
        "rotational sweep subdivision must obey its hard cap");
    Require(admittedSweepSubsteps(8, 3) == 3 &&
                admittedSweepSubsteps(8, 0) == 0,
        "the global cast budget must bound every body's admitted substeps");
    Require(fairSweepCastBudget(48, 16, 0) == 3 &&
                fairSweepCastBudget(48, 16, 15) == 3 &&
                fairSweepCastBudget(10, 3, 0) == 4 &&
                fairSweepCastBudget(10, 3, 1) == 3 &&
                fairSweepCastBudget(10, 3, 2) == 3,
        "every same-frame admitted body must receive one fair share before rotational extras");
    Require(nextConnectedSweepCursor(0, 3) == 1 &&
                nextConnectedSweepCursor(2, 3) == 0 &&
                nextConnectedSweepCursor(7, 0) == 0,
        "connected-body admission must make progress even when prior cast strides divide the body count");

    Require(Near(rigidSweepHitFraction(2, 8, 0.5f, false), 0.3125f),
        "a translating sub-cast hit must map back into the whole rigid motion");
    Require(Near(rigidSweepHitFraction(2, 8, 0.8f, true), 0.25f),
        "an end-orientation start hit must stop at the last proven-clear pose");
    Require(Near(rigidSweepSkinFraction(1.0f, 5.0f, 5.0f), 0.1f),
        "rigid sweep skin must account for center and rotating-surface travel");
    Require(Near(rotationalEnvelopePadding(10.0f, 60.0f, 2.0f), 12.0f),
        "the conservative angular envelope must cover rotating support points and connected-body center sagitta");

    Require(shouldRunHeldRigidSweep(true, false, 0.0f, 10.0f, 0.5f, true),
        "pure held-object rotation must run predictive world collision");
    Require(!shouldRunHeldRigidSweep(true, true, 1000.0f, 90.0f, 0.5f, true),
        "a deliberate teleport/snap-turn warp must not become a world-spanning physical sweep");
    Require(shouldRunHeldRigidSweep(true, false, 0.1f, 0.01f, 0.5f, true),
        "ordinary sub-half-unit hand motion must still receive continuous wall collision");
    Require(!shouldRunHeldRigidSweep(true, false, 0.001f, 0.01f, 0.5f, true),
        "true sub-noise motion must not consume shape-cast budget");
    Require(!shouldRunHeldRigidSweep(true, false, 0.0f, 0.0f, 0.0f, true),
        "a zero configured translation threshold must not cast a stationary held object every frame");
    Require(
        hasGenuineOutwardHeldWorldStopControllerMotion(
            true, true, 0.51f) &&
            !hasGenuineOutwardHeldWorldStopControllerMotion(
                true, true, 0.5f) &&
            !hasGenuineOutwardHeldWorldStopControllerMotion(
                true, true, -10.0f) &&
            !hasGenuineOutwardHeldWorldStopControllerMotion(
                false, true, 10.0f) &&
            !hasGenuineOutwardHeldWorldStopControllerMotion(
                true, false, 10.0f),
        "only a valid raw-controller velocity genuinely moving away from the saved wall plane may release the latch");
    Require(
        shouldUseRawControllerRetreatDeltaFallback(
            true, true, 0.0f) &&
            shouldUseRawControllerRetreatDeltaFallback(
                true, true, -4.0f) &&
            shouldUseRawControllerRetreatDeltaFallback(
                true,
                true,
                std::numeric_limits<float>::quiet_NaN()) &&
            !shouldUseRawControllerRetreatDeltaFallback(
                true, true, 0.01f) &&
            !shouldUseRawControllerRetreatDeltaFallback(
                false, true, -4.0f) &&
            !shouldUseRawControllerRetreatDeltaFallback(
                true, false, -4.0f),
        "raw retreat delta must replace only a stale/frozen generated target; a healthy outward target remains exact and cannot be double-advanced");
    Require(
        shouldHoldLatchedHeldWorldStop(
            true, true, false, 5.0f, 10.0f,
            true, -2.0f) &&
            shouldHoldLatchedHeldWorldStop(
                true, true, false, 0.0f, 0.0f,
                true, 0.0f) &&
            !shouldHoldLatchedHeldWorldStop(
                true, true, false, 5.0f, 0.0f,
                true, 2.0f) &&
            !shouldHoldLatchedHeldWorldStop(
                true, true, false, 5.0f, 0.0f,
                true, -4.0f, true) &&
            !shouldHoldLatchedHeldWorldStop(
                true, true, false, 0.0f, 0.0f,
                true, 0.0f, true),
        "an active stop must reuse its exact safe pose while resting or pressing inward, but the first genuine raw-controller retreat overrides both stale inward and frozen generated targets");
    Require(
        shouldRetainHeldWorldStopWithoutHit(
            true, true, false, 0, 0.0f, 0.0f,
            true, 0.0f) &&
            shouldRetainHeldWorldStopWithoutHit(
                true, true, false, 0, 5.0f, 10.0f,
                true, -2.0f) &&
            !shouldRetainHeldWorldStopWithoutHit(
                true, true, false, 0, 5.0f, 10.0f,
                true, -2.0f, true),
        "a resting stop and a no-query frame still pressing into its saved plane retain, but raw-controller retreat cannot be hidden by that frozen target");
    Require(
        shouldRetainHeldWorldStopWithoutHit(
            true, true, false, 1, 5.0f, 0.0f,
            true, -2.0f) &&
            !shouldRetainHeldWorldStopWithoutHit(
                true, true, true, 0, 0.0f, 0.0f,
                true, 0.0f) &&
            !shouldRetainHeldWorldStopWithoutHit(
                true, true, false, 0, 5.0f, 0.0f,
                true, 2.0f) &&
            !shouldRetainHeldWorldStopWithoutHit(
                true, true, false, 0, 5.0f, 0.0f,
                true, 0.0f) &&
            !shouldRetainHeldWorldStopWithoutHit(
                true, true, false, 0, 5.0f, 0.0f,
                false, -2.0f),
        "a one-frame query miss while pressing must retain; warp, retreat, tangential locomotion, or missing stop normal must release/rebase the stop");
    const bool recoveredStopActive =
        shouldPreserveHeldWorldStopThroughRecovery(true, true);
    Require(
        recoveredStopActive &&
            shouldHoldLatchedHeldWorldStop(
                recoveredStopActive,
                true,
                false,
                8.0f,
                0.0f,
                true,
                -4.0f) &&
            !shouldPreserveHeldWorldStopThroughRecovery(false, true) &&
            !shouldPreserveHeldWorldStopThroughRecovery(true, false),
        "large-gap recovery must preserve a stop-owned latch so the following inward-pressure frame cannot churn or tunnel");
    Require(
        !resolveHeldMotorContactSoftening(true, true) &&
            !resolveHeldMotorContactSoftening(true, false) &&
            resolveHeldMotorContactSoftening(false, true) &&
            !resolveHeldMotorContactSoftening(false, false),
        "cached native contact must never soften the held constraint while the predictive wall stop owns the frame");

    struct Point3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
    Require(startPointMotionBlocks(
                Point3{ 0.0f, 0.0f, -1.0f },
                Point3{ 0.0f, 0.0f, 1.0f }),
        "a rotational pose witness moving into its outward plane must block");
    Require(!startPointMotionBlocks(
                Point3{ 1.0f, 0.0f, 0.0f },
                Point3{ 0.0f, 0.0f, 1.0f }) &&
                !startPointMotionBlocks(
                    Point3{ 0.0f, 0.0f, 1.0f },
                    Point3{ 0.0f, 0.0f, 1.0f }),
        "resting/sliding and separating start contacts must not glue a held object");
    const auto releaseVelocity =
        rock::grab_held_response::composeControllerReleaseVelocity(
            rock::grab_held_response::ReleaseVelocityInput<Point3>{
                .controllerDerivedEnabled = true,
                .hasHandLocalVelocity = true,
                .hasObjectLocalVelocity = true,
                .handLocalVelocityHavok = Point3{ 3.0f, 0.0f, 0.0f },
                .objectLocalVelocityHavok = Point3{ 3.0f, 0.0f, 0.0f },
                .playerVelocityHavok = Point3{ 2.0f, 0.0f, 0.0f },
                .objectVelocityBlend = 0.35f,
                .throwMultiplier = 1.5f,
                .maxVelocityHavok = 20.0f,
            });
    Require(Near(releaseVelocity.x, 6.5f),
        "matching hand/object relative velocity must be blended once and room velocity added outside the throw multiplier");
    const auto stoppedReleaseVelocity =
        rock::grab_held_response::composeControllerReleaseVelocity(
            rock::grab_held_response::ReleaseVelocityInput<Point3>{
                .controllerDerivedEnabled = false,
                .hasHandLocalVelocity = true,
                .hasObjectLocalVelocity = false,
                .handLocalVelocityHavok =
                    Point3{ 8.0f, 0.0f, 0.0f },
                .playerVelocityHavok = {},
                .throwMultiplier = 1.5f,
                .maxVelocityHavok = 20.0f,
            });
    Require(
        Near(stoppedReleaseVelocity.x, 0.0f),
        "release from a predictive wall stop must not reuse controller-through-wall throw velocity");
    const auto objectBlendEndpoint =
        rock::grab_held_response::composeControllerReleaseVelocity(
            rock::grab_held_response::ReleaseVelocityInput<Point3>{
                .controllerDerivedEnabled = true,
                .hasHandLocalVelocity = true,
                .hasObjectLocalVelocity = true,
                .handLocalVelocityHavok = Point3{ 10.0f, 0.0f, 0.0f },
                .objectLocalVelocityHavok = Point3{ 4.0f, 0.0f, 0.0f },
                .objectVelocityBlend = 1.0f,
                .throwMultiplier = 1.0f,
                .maxVelocityHavok = 20.0f,
            });
    Require(Near(objectBlendEndpoint.x, 4.0f),
        "object blend one must select object velocity rather than add it to hand velocity");

    std::cout << "Dynamic grab policy tests passed\n";
    return 0;
}
