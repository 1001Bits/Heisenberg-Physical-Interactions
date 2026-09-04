#include "physics-interaction/weapon/WeaponWallSweepPolicy.h"
#include "physics-interaction/weapon/WeaponWallLocomotionPolicy.h"
#include "physics-interaction/hand/HandVisual.h"

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

    TestTransform identityTransform()
    {
        return rock::transform_math::makeIdentityTransform<TestTransform>();
    }

    void require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace rock::weapon_wall_sweep_policy;

    require(shouldAdmitWeaponSweepHandTarget(
                true, false, true, false) &&
            shouldAdmitWeaponSweepHandTarget(
                true, true, false, true),
        "an ownership-resolved hand target must follow the full free policy for its own side");
    require(!shouldAdmitWeaponSweepHandTarget(
                false, false, true, true) &&
            !shouldAdmitWeaponSweepHandTarget(
                true, false, false, true) &&
            !shouldAdmitWeaponSweepHandTarget(
                true, true, true, false),
        "unresolved, firing, gripped, and otherwise non-free hand targets must fail closed before sweep admission");
    require(!shouldStabilizeVisibleBuild(true, false, 8),
        "an empty initial bank must publish from its first visible witness");
    require(shouldStabilizeVisibleBuild(true, true, 8),
        "a replacement must retain and stabilize its active bank");
    require(shouldScheduleMoreCompleteVisibleReplacement(
                true, 0x10u, 0x20u, 1u, 5u),
        "late ready muzzle/stock geometry must replace the provisional bank without removing it first");
    require(!shouldScheduleMoreCompleteVisibleReplacement(
                true, 0x10u, 0x20u, 5u, 2u) &&
            !shouldScheduleMoreCompleteVisibleReplacement(
                true, 0x10u, 0x20u, 5u, 5u) &&
            !shouldScheduleMoreCompleteVisibleReplacement(
                true, 0x10u, 0x10u, 1u, 5u),
        "reload visibility loss, same-size animation drift, and unchanged composition must not churn a complete bank");
    require(!sourceDistanceFilterRejects(false, 1000.0f, 45.0f),
        "distance culling must be inert unless explicitly enabled");
    require(sourceDistanceFilterRejects(true, 46.0f, 45.0f),
        "diagnostic distance culling must retain its opt-in behavior");
    require(
        weaponDriveSegmentIsContinuous(
            true, true, true, true,
            0.1f,
            kRootDiscontinuityDistanceGameUnits,
            kRootDiscontinuityRotationDegrees),
        "a current finite queued weapon segment must preserve its previous pre-contact root pose");
    require(
        !weaponDriveSegmentIsContinuous(
            false, true, true, true, 1.0f / 90.0f, 1.0f, 1.0f) &&
            !weaponDriveSegmentIsContinuous(
                true, false, true, true, 1.0f / 90.0f, 1.0f, 1.0f) &&
            !weaponDriveSegmentIsContinuous(
                true, true, false, true, 1.0f / 90.0f, 1.0f, 1.0f),
        "first samples and generation/root changes must seed rather than publish rollback history");
    require(
        !weaponDriveSegmentIsContinuous(
            true, true, true, true, 0.0f, 1.0f, 1.0f) &&
            !weaponDriveSegmentIsContinuous(
                true, true, true, true, 0.101f, 1.0f, 1.0f) &&
            !weaponDriveSegmentIsContinuous(
                true,
                true,
                true,
                true,
                std::numeric_limits<float>::quiet_NaN(),
                1.0f,
                1.0f) &&
            !weaponDriveSegmentIsContinuous(
                true,
                true,
                true,
                true,
                1.0f / 90.0f,
                kRootDiscontinuityDistanceGameUnits + 0.01f,
                1.0f) &&
            !weaponDriveSegmentIsContinuous(
                true,
                true,
                true,
                true,
                1.0f / 90.0f,
                1.0f,
                kRootDiscontinuityRotationDegrees + 0.01f),
        "invalid clocks and root discontinuities must never become hand-contact rollback segments");

    int totalBudget = 0;
    for (int body = 0; body < kMaximumBodies; ++body) {
        const int budget = fairCastBudget(kMaximumBodies, body);
        require(budget >= 1,
            "every published hull must receive a cast at maximum occupancy");
        totalBudget += budget;
    }
    require(totalBudget == kMaximumCastsPerFrame,
        "fair hull budgets must equal the fixed global query cap");
    require(!shouldRunStationaryWitness(true, false),
        "an idle current-history weapon must issue no steady-state exact casts");
    require(shouldRunStationaryWitness(true, true),
        "an active stationary corner must reacquire its uncached second exact plane");
    require(shouldRunStationaryWitness(false, false),
        "a fresh generation must run a current-pose overlap witness");

    const auto continuousEntry = classifySweepWitness(
        true, false, false, 1, 4, 0.25f);
    require(continuousEntry.fractionValid &&
            continuousEntry.validatedContinuousEntry &&
            std::abs(continuousEntry.sweepFraction - 0.3125f) <
                0.0001f,
        "a native substep fraction must map to its global continuous TOI");
    require(!classifySweepWitness(
                 true, false, false, 0, 1, 0.0f)
                 .validatedContinuousEntry,
        "a start-overlap witness must not bypass acquisition safety caps");
    require(!classifySweepWitness(
                 true, true, false, 0, 1, 0.5f)
                 .validatedContinuousEntry &&
            !classifySweepWitness(
                 false, false, false, 0, 1, 0.5f)
                 .validatedContinuousEntry,
        "stationary and rebased casts are not continuous entry evidence");
    const auto angularEntry = classifySweepWitness(
        true, false, true, 1, 4, 0.75f);
    require(angularEntry.validatedContinuousEntry &&
            std::abs(angularEntry.sweepFraction - 0.25f) <
                0.0001f,
        "a rotational envelope hit must stop at its segment start instead of using the artificial query-axis fraction");
    require(!classifySweepWitness(
                 true, false, false, 0, 1,
                 std::numeric_limits<float>::quiet_NaN())
                 .fractionValid,
        "non-finite native fractions must be rejected");
    require(std::abs(safeSweepFraction(
                0.5f, 2.0f, 8.0f, 0.0f) -
            0.25f) < 0.0001f,
        "the safe TOI must reserve the configured contact envelope before impact");
    require(std::abs(supportPlaneApproachSpeed(
                12.0f, 9.0f, 0.1f) -
            30.0f) < 0.0001f,
        "support-point motion toward a plane must drive reciprocal weapon contact even when the hull center is stationary");
    require(supportPlaneApproachSpeed(
                9.0f, 12.0f, 0.1f) == 0.0f &&
            supportPlaneApproachSpeed(
                9.0f, 9.0f, 0.1f) == 0.0f &&
            supportPlaneApproachSpeed(
                12.0f, 9.0f, 0.0f) == 0.0f &&
            supportPlaneApproachSpeed(
                std::numeric_limits<float>::quiet_NaN(),
                9.0f,
                0.1f) == 0.0f,
        "retreat, tangent motion, and invalid clocks must not manufacture weapon approach speed");
    require(
        shouldPromoteHandStartOverlapToContinuousEntry(
            true, true, true, 0.0f, 6.0f),
        "an ownership-verified hand start hit on a moving current root must conservatively stop at the previous clear pose");
    require(
        !shouldPromoteHandStartOverlapToContinuousEntry(
            false, true, true, 0.0f, 6.0f) &&
            !shouldPromoteHandStartOverlapToContinuousEntry(
                true, false, true, 0.0f, 6.0f) &&
            !shouldPromoteHandStartOverlapToContinuousEntry(
                true, true, false, 0.0f, 6.0f) &&
            !shouldPromoteHandStartOverlapToContinuousEntry(
                true, true, true, 0.1f, 6.0f) &&
            !shouldPromoteHandStartOverlapToContinuousEntry(
                true, true, true, 0.0f, 0.0f),
        "world hits, stale/stationary roots, interior fractions, and non-approaching overlaps must not use the hand-only rollback fallback");
    require(!shouldReplaceParallelSweepHit(
                true, 0.2f, 5.0f,
                true, 0.8f, 50.0f),
        "a later deep parallel hit must not replace the first blocking TOI");
    require(shouldReplaceParallelSweepHit(
                false, 0.0f, 50.0f,
                true, 0.8f, 5.0f),
        "continuous entry evidence must outrank an unvalidated overlap witness");
    require(
        chooseUnvalidatedWeaponStopPoseSource(
            true, true, false, true,
            true, true, 120.0f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::PreviousSafe,
        "generation-current history must supply the immutable stop even for a deep fast crossing");
    require(
        chooseUnvalidatedWeaponStopPoseSource(
            true, true, false, true,
            false, true, 24.4f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::CurrentStable &&
        chooseUnvalidatedWeaponStopPoseSource(
            true, true, false, true,
            false, true, 2.0f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::CurrentStable,
        "a bounded exact first overlap without history must freeze its current pose instead of depenetrating or oscillating");
    require(
        chooseUnvalidatedWeaponStopPoseSource(
            true, true, false, true,
            false, true, 37.0f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::None &&
        chooseUnvalidatedWeaponStopPoseSource(
            true, true, false, true,
            false, true, 750.0f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::None &&
        chooseUnvalidatedWeaponStopPoseSource(
            true, false, false, true,
            false, true, 2.0f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::None &&
        chooseUnvalidatedWeaponStopPoseSource(
            true, true, true, true,
            false, true, 2.0f, 18.0f) ==
            UnvalidatedWeaponStopPoseSource::None,
        "unbounded glitches, non-weapon evidence, and an existing TOI pose must never synthesize a current-pose stop");
    require(retainStopAcrossContactMiss(
                true, true, false, 0.0f, 1.0f / 90.0f) &&
            !retainStopAcrossContactMiss(
                true, true, true, 0.0f, 1.0f / 90.0f) &&
            !retainStopAcrossContactMiss(
                true, false, false, 0.0f, 1.0f / 90.0f) &&
            !retainStopAcrossContactMiss(
                true, true, false, 0.049f, 1.0f / 90.0f),
        "the stop pose gets only a bounded query-miss grace and never bridges confirmed separation or identity change");
    require(
        chooseImmutableWorldStopAction(
            true, true, false, false, false, false) ==
            ImmutableWorldStopAction::Retain &&
        chooseImmutableWorldStopAction(
            true, false, false, false, false, false) ==
            ImmutableWorldStopAction::Clear &&
        chooseImmutableWorldStopAction(
            false, true, false, false, false, false) ==
            ImmutableWorldStopAction::Clear,
        "a wall stop must survive arbitrary query misses but never an identity or authority change");
    require(!rawProbeRetreatConfirmed(
                false, -4.0f, -3.0f) &&
            !rawProbeRetreatConfirmed(
                true, -4.0f, -4.5f) &&
            !rawProbeRetreatConfirmed(
                true, -4.0f, -3.97f) &&
            rawProbeRetreatConfirmed(
                true, -4.0f, -3.9f),
        "initial capture and inward/noise motion must not be mistaken for a confirmed controller retreat");
    require(rawProbeRetreatConfirmed(
                true, -4.0f, -3.9f) &&
            retainStopForResolvedProbe(
                true, true, true),
        "a motion reversal must retain the stop while the raw probe is still inside the contact plane");
    require(!retainStopForResolvedProbe(
                true, false, true) &&
            !retainStopForResolvedProbe(
                true, true, false) &&
            !retainStopForResolvedProbe(
                false, true, true),
        "only actual plane exit, tangent escape, or identity loss may release a resolved stop");
    require(
        chooseImmutableWorldStopAction(
            true, true, false, true, true, true) ==
            ImmutableWorldStopAction::Retain &&
        chooseImmutableWorldStopAction(
            true, true, false, true, true, false) ==
            ImmutableWorldStopAction::Retain &&
        chooseImmutableWorldStopAction(
            true, true, false, true, false, false) ==
            ImmutableWorldStopAction::Retain &&
        chooseImmutableWorldStopAction(
            true, true, false, true, false, true) ==
            ImmutableWorldStopAction::BeginRelease &&
        chooseImmutableWorldStopAction(
            true, true, true, true, true, false) ==
            ImmutableWorldStopAction::ContinueRelease &&
        chooseImmutableWorldStopAction(
            true, false, true, true, true, false) ==
            ImmutableWorldStopAction::Clear,
        "a wall stop must ignore tangent/deeper motion and begin release only after confirmed outward plane exit");
    require(
        immutableWallStopOwnsFiringHandWriters(
            true, true, false) &&
            !immutableWallStopOwnsFiringHandWriters(
                true, true, true) &&
            !immutableWallStopOwnsFiringHandWriters(
                false, true, false) &&
            !immutableWallStopOwnsFiringHandWriters(
                true, false, false),
        "only a valid static-world full-pose stop may suppress lower firing-hand visual writers");
    const TestVector outwardRetreat{
        0.20f, 0.30f, -0.40f };
    const TestVector wallNormal{
        1.0f, 0.0f, 0.0f };
    const auto firstRetreatStep =
        constrainedPlaneTangentDelta(
            outwardRetreat,
            wallNormal);
    require(std::abs(firstRetreatStep.x) < 0.0001f &&
            std::abs(firstRetreatStep.y) < 0.0001f &&
            std::abs(firstRetreatStep.z) < 0.0001f,
        "a retained wall stop must keep the complete pose immutable despite tangent or retreat noise");
    const auto captureStep =
        constrainedPlaneTangentDelta(
            TestVector{},
            wallNormal);
    require(std::abs(captureStep.x) < 0.0001f &&
            std::abs(captureStep.y) < 0.0001f &&
            std::abs(captureStep.z) < 0.0001f,
        "same-frame stop capture must not move the blocked pose");
    auto blockedReleasePose = identityTransform();
    auto rawReleasePose = identityTransform();
    rawReleasePose.translate.x = 40.0f;
    rawReleasePose.rotate.entry[0][0] = 0.0f;
    rawReleasePose.rotate.entry[0][1] = -1.0f;
    rawReleasePose.rotate.entry[1][0] = 1.0f;
    rawReleasePose.rotate.entry[1][1] = 0.0f;
    const auto boundedRelease =
        rock::hand_visual_lerp_math::advanceTransform(
            blockedReleasePose,
            rawReleasePose,
            kStopReleaseLinearSpeedGameUnitsPerSecond,
            kStopReleaseAngularSpeedDegreesPerSecond,
            1.0f / 90.0f);
    require(
        boundedRelease.transform.translate.x <= 3.001f &&
            rock::hand_visual_lerp_math::rotationDistanceDegrees(
                blockedReleasePose,
                boundedRelease.transform) <= 8.001f &&
            !boundedRelease.reachedTarget,
        "plane exit must release a divergent weapon pose through bounded translation and rotation rather than snapping to raw");
    require(rotationalSweepSubsteps(0.0f, 80.0f) == 1,
        "translation-only motion needs one exact hull cast");
    require(rotationalSweepSubsteps(90.0f, 80.0f) > 1 &&
            rotationalSweepSubsteps(90.0f, 80.0f) <=
                kMaximumRotationSubstepsPerBody,
        "long rotating weapons must subdivide within the hard cap");
    require(rotationalEnvelopePadding(40.0f, 90.0f, 0.0f) > 56.0f,
        "a budget-limited angular segment must conservatively enclose its arc");

    require(!shouldRebaseSweepHistory(
                true, true, false, 40.0f, 70.0f),
        "a fast same-generation controller swing must retain CCD history");
    require(shouldRebaseSweepHistory(
                false, true, false, 1.0f, 1.0f),
        "a generation change must reject stale hull transforms");
    require(shouldRebaseSweepHistory(
                true, false, false, 1.0f, 1.0f),
        "a source-basis change must reject stale hull transforms");
    require(shouldRebaseSweepHistory(
                true, true, true, 1.0f, 1.0f),
        "a player-space teleport or snap turn must rebase");
    require(shouldRebaseSweepHistory(
                true, true, false, 200.0f, 1.0f),
        "an implausible whole-root translation must rebase");

    using rock::weapon_wall_locomotion_policy::Input;
    using rock::weapon_wall_locomotion_policy::RoomScaleHistory;
    using rock::weapon_wall_locomotion_policy::Vec3;
    RoomScaleHistory roomScaleHistory{};
    const auto firstRoomScaleSample =
        rock::weapon_wall_locomotion_policy::
            sampleRoomScaleMotion(
                roomScaleHistory,
                true,
                true,
                Vec3{ 10.0f, 20.0f, 5.0f });
    require(
        roomScaleHistory.valid &&
            !firstRoomScaleSample.deltaValid &&
            !firstRoomScaleSample.discontinuous,
        "the first room-scale HMD sample must establish a baseline without moving the player");
    const auto roomScaleMotion =
        rock::weapon_wall_locomotion_policy::
            sampleRoomScaleMotion(
                roomScaleHistory,
                true,
                true,
                Vec3{ 8.5f, 20.5f, 5.0f });
    require(
        roomScaleMotion.deltaValid &&
            !roomScaleMotion.discontinuous &&
            std::abs(
                roomScaleMotion.deltaPlayerLocalGameUnits.x +
                1.5f) < 0.0001f &&
            std::abs(
                roomScaleMotion.deltaPlayerLocalGameUnits.y -
                0.5f) < 0.0001f,
        "physical HMD travel must be measured inside player space, independently of room-node locomotion");
    const auto roomScaleRecenter =
        rock::weapon_wall_locomotion_policy::
            sampleRoomScaleMotion(
                roomScaleHistory,
                true,
                true,
                Vec3{ 100.0f, 20.5f, 5.0f });
    require(
        !roomScaleRecenter.deltaValid &&
            roomScaleRecenter.discontinuous,
        "a room-scale recenter must rebase instead of becoming a controller impulse");
    const auto afterRoomScaleRecenter =
        rock::weapon_wall_locomotion_policy::
            sampleRoomScaleMotion(
                roomScaleHistory,
                true,
                true,
                Vec3{ 99.5f, 20.5f, 5.0f });
    require(
        afterRoomScaleRecenter.deltaValid &&
            !afterRoomScaleRecenter.discontinuous &&
            std::abs(
                afterRoomScaleRecenter.
                    deltaPlayerLocalGameUnits.x +
                0.5f) < 0.0001f,
        "room-scale sampling must resume from the recentered baseline on the next frame");

    const auto locomotionStop =
        rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ -180.0f, 90.0f, 45.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        });
    require(
        locomotionStop.apply &&
            std::abs(locomotionStop.correctionDisplacementGameUnits.x - 2.0f) < 0.0001f &&
            std::abs(locomotionStop.correctionDisplacementGameUnits.y) < 0.0001f &&
            std::abs(locomotionStop.correctionDisplacementGameUnits.z) < 0.0001f,
        "walking farther into a wall-blocked weapon must cancel only the inward horizontal player delta");
    require(
        !rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .targetIsDynamicHand = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ -180.0f, 0.0f, 0.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        }).apply,
        "weapon/offhand stops must never push the player controller");
    require(
        !rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ 0.0f, 0.0f, -180.0f },
            .wallNormalWorld = Vec3{ 0.0f, 0.0f, 1.0f },
            .deltaSeconds = 1.0f / 90.0f,
        }).apply,
        "floor and ceiling contacts must not alter locomotion or jumping");
    require(
        !rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .playerSpaceDiscontinuous = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ -9000.0f, 0.0f, 0.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        }).apply,
        "teleports and snap-turn rebases must never become controller impulses");
    const auto cappedLocomotionStop =
        rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ -500.0f, 0.0f, 0.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 0.08f,
        });
    require(
        cappedLocomotionStop.apply &&
            std::abs(cappedLocomotionStop.correctionDisplacementGameUnits.x - 8.0f) < 0.0001f &&
            std::abs(cappedLocomotionStop.durationSeconds - 0.05f) < 0.0001f,
        "controller cancellation must stay bounded during an anomalously large but continuous frame");

    const auto combinedLocomotionStop =
        rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .roomScaleDeltaValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ -90.0f, 45.0f, 0.0f },
            .roomScaleDeltaWorldGameUnits =
                Vec3{ -0.5f, 0.25f, 0.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        });
    require(
        combinedLocomotionStop.apply &&
            std::abs(
                combinedLocomotionStop.
                    correctionDisplacementGameUnits.x -
                1.5f) < 0.0001f &&
            std::abs(
                combinedLocomotionStop.
                    correctionDisplacementGameUnits.y) <
                0.0001f,
        "stick locomotion and physical room-scale travel into the retained wall plane must be cancelled together");
    require(
        !rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ 90.0f, 0.0f, 0.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        }).apply &&
            !rock::weapon_wall_locomotion_policy::evaluate(Input{
                .stopActive = true,
                .stopPoseApplied = true,
                .playerSpaceValid = true,
                .locomotionVelocityValid = true,
                .locomotionVelocityGameUnitsPerSecond =
                    Vec3{ 0.0f, 90.0f, 0.0f },
                .wallNormalWorld =
                    Vec3{ 1.0f, 0.0f, 0.0f },
                .deltaSeconds = 1.0f / 90.0f,
            }).apply,
        "retreat and wall-tangent locomotion must remain untouched");
    require(
        !rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = false,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond =
                Vec3{ -90.0f, 0.0f, 0.0f },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        }).apply,
        "locomotion must not move the controller unless the immutable weapon stop pose won and was applied");
    require(
        !rock::weapon_wall_locomotion_policy::evaluate(Input{
            .stopActive = true,
            .stopPoseApplied = true,
            .playerSpaceValid = true,
            .locomotionVelocityValid = true,
            .locomotionVelocityGameUnitsPerSecond = Vec3{
                std::numeric_limits<float>::quiet_NaN(),
                0.0f,
                0.0f,
            },
            .wallNormalWorld = Vec3{ 1.0f, 0.0f, 0.0f },
            .deltaSeconds = 1.0f / 90.0f,
        }).apply,
        "invalid controller motion must fail closed rather than displacing the player or grip pose");

    return 0;
}
