#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rock::dynamic_grab_policy
{
    /*
     * The legacy frame-context bridge and the newer physics-clock target
     * feed-forward solve the same one-frame locomotion lead. Applying both
     * advances the held target twice. Body transport is a separate velocity
     * channel and may remain paired with either positional lead mechanism.
     */
    [[nodiscard]] inline constexpr bool
    shouldUseLegacyLocomotionAuthorityBridge(
        const bool bridgeRequested,
        const bool targetFeedForwardEnabled,
        const bool /*bodyVelocityTransportEnabled*/) noexcept
    {
        return bridgeRequested &&
               !targetFeedForwardEnabled;
    }

    inline constexpr std::uint32_t kCollisionSuppressionNoCollideBit =
        1u << 14;

    [[nodiscard]] inline constexpr std::uint32_t heldSweepQueryFilterInfo(
        const std::uint32_t) noexcept
    {
        // Held bodies are deliberately moved onto BIPED_NO_CC and can carry a
        // composed no-collide/system group while constrained.  Reusing that
        // live filter for an ad-hoc cast makes static-world hits disappear on
        // some Fallout filter tables.  Use the same proven independent query
        // filter group as selection, but use ROCK's hand layer (43). Its
        // installed matrix row explicitly includes both world geometry and
        // generated weapon layer 44. Fallout's layer-45 selection query can
        // reject layer 44 before our hit collector gets a chance to admit it.
        return 0x000B002Bu;
    }

    [[nodiscard]] inline constexpr bool isHeldSweepObstacleLayer(
        const std::uint32_t layer,
        const bool worldSurfaceLayer,
        const std::uint32_t generatedWeaponLayer) noexcept
    {
        return worldSurfaceLayer || layer == generatedWeaponLayer;
    }

    [[nodiscard]] inline constexpr bool
    heldObjectMayDriveVisualHand(
        const bool gravityPullingPhase,
        const bool nearConvergingPhase,
        const bool seatedPivotReacquirePhase,
        const bool touchHeldPhase,
        const bool predictiveWorldStopOwnsFrame) noexcept
    {
        // Only the original pull/catch convergence may bring the visual hand
        // onto an object during free motion.  Once a seated object's predictive
        // sweep owns a wall stop, however, the object and rendered hand must
        // retain their frozen relation at the same admitted pose.  This is
        // wall authority, not permission for an ordinary held object to push
        // or displace its tracked hand.
        const bool acquisitionAuthority =
            (gravityPullingPhase || nearConvergingPhase) &&
            !seatedPivotReacquirePhase &&
            !touchHeldPhase;
        const bool retainedWallStopAuthority =
            touchHeldPhase && predictiveWorldStopOwnsFrame;
        return acquisitionAuthority || retainedWallStopAuthority;
    }

    [[nodiscard]] inline constexpr bool
    shouldTreatVisualControllerSeparationAsGripLoss(
        const bool predictiveWorldStopOwnsFrame,
        const bool discontinuityWarpApplied) noexcept
    {
        // Controller pressure through a wall is expected while the admitted
        // wall pose owns both the object and visual hand.  It must not feed the
        // deviation-release watchdog.  Ordinary free-held separation remains
        // a valid broken-grip signal, while a discontinuity warp retains its
        // existing one-frame exemption.
        return !predictiveWorldStopOwnsFrame &&
               !discontinuityWarpApplied;
    }

    [[nodiscard]] inline constexpr bool
    shouldBeginVisualReturnFromHeldWorldStop(
        const bool previousPredictiveWorldStopActive,
        const bool predictiveWorldStopOwnsFrame,
        const bool hasPublishedVisualHandPose,
        const bool genuineOutwardControllerMotion = false) noexcept
    {
        // Only the edge leaving wall authority needs a bounded return.  An
        // ordinary TouchHeld frame must never create ongoing object-driven
        // hand authority, and acquisition promotion keeps its existing path.
        // A deliberate retreat already restored a moving-safe body command;
        // holding a separate return authority after that edge makes the hand
        // appear stuck even though the wall latch has released.
        return previousPredictiveWorldStopActive &&
               !predictiveWorldStopOwnsFrame &&
               hasPublishedVisualHandPose &&
               !genuineOutwardControllerMotion;
    }

    enum class HandProcessingOrder : std::uint8_t
    {
        RightThenLeft,
        LeftThenRight,
    };

    /*
     * A same-frame handoff is an acquire-before-release transaction. When only
     * one hand owns an object, service the open hand first so its grip edge can
     * establish the peer constraint before the source hand consumes a release
     * edge. Fixed right-then-left ordering made right-to-left handoffs drop the
     * object while the mirror direction happened to work.
     */
    [[nodiscard]] inline constexpr HandProcessingOrder chooseHandProcessingOrder(
        const bool rightHolding,
        const bool leftHolding) noexcept
    {
        return rightHolding && !leftHolding ?
                   HandProcessingOrder::LeftThenRight :
                   HandProcessingOrder::RightThenLeft;
    }

    /* A peer join adds another finite constraint to a live moving body. */
    [[nodiscard]] inline constexpr bool shouldResetVelocityAtGrabCommit(
        const bool joiningPeerHeldObject) noexcept
    {
        return !joiningPeerHeldObject;
    }

    /*
     * Continuous transform warps bypass Havok collision. They are mutually
     * exclusive with the physical locomotion-transport path; genuine
     * discontinuity warps (teleport/snap-turn) are decided separately.
     */
    [[nodiscard]] inline constexpr bool allowContinuousPlayerSpaceWarp(
        const bool configured,
        const bool locomotionTransportEnabled) noexcept
    {
        return configured && !locomotionTransportEnabled;
    }

    /* Exactly one subsystem may write the standing player-space velocity. */
    [[nodiscard]] inline constexpr bool centralPlayerSpaceOwnsVelocity(
        const bool playerSpaceCompensationEnabled,
        const bool locomotionTransportEnabled) noexcept
    {
        return playerSpaceCompensationEnabled && !locomotionTransportEnabled;
    }

    struct SweepClampResult
    {
        float allowedFraction = 1.0f;
        float allowedDistanceGameUnits = 0.0f;
        bool clamped = false;
    };

    /*
     * A translating shape cast has no angular component.  Held rigid bodies
     * therefore subdivide their requested rotation and cast the native convex
     * shape (or a conservative convex bounds proxy) at every sampled pose.  The
     * angular and surface-arc limits are complementary: a long rifle needs more
     * samples than a bottle for the same controller rotation.  The hard cap is
     * deliberately part of the policy so malformed bounds can never turn one
     * held object into an unbounded query workload.
     */
    [[nodiscard]] inline int rotationalSweepSubsteps(
        const float rotationDegrees,
        const float bodyRadiusGameUnits,
        const float maxAngularStepDegrees,
        const float maxSurfaceArcStepGameUnits,
        const int hardMaxSubsteps) noexcept
    {
        if (!std::isfinite(rotationDegrees) || rotationDegrees <= 0.0f ||
            hardMaxSubsteps <= 1) {
            return 1;
        }

        const float safeAngularStep =
            std::isfinite(maxAngularStepDegrees) &&
                    maxAngularStepDegrees > 0.0f ?
                maxAngularStepDegrees :
                rotationDegrees;
        int requested = static_cast<int>(
            std::ceil(rotationDegrees / safeAngularStep));

        if (std::isfinite(bodyRadiusGameUnits) &&
            bodyRadiusGameUnits > 0.0f &&
            std::isfinite(maxSurfaceArcStepGameUnits) &&
            maxSurfaceArcStepGameUnits > 0.0f) {
            constexpr float kDegreesToRadians =
                0.017453292519943295769f;
            const float surfaceArc = bodyRadiusGameUnits *
                                     rotationDegrees *
                                     kDegreesToRadians;
            requested = (std::max)(requested,
                static_cast<int>(
                    std::ceil(surfaceArc /
                              maxSurfaceArcStepGameUnits)));
        }
        return std::clamp(requested, 1, hardMaxSubsteps);
    }

    [[nodiscard]] inline constexpr int admittedSweepSubsteps(
        const int requestedSubsteps,
        const int remainingCastBudget) noexcept
    {
        return remainingCastBudget <= 0 ?
                   0 :
                   (std::min)((std::max)(requestedSubsteps, 1),
                       remainingCastBudget);
    }

    [[nodiscard]] inline constexpr int fairSweepCastBudget(
        const int totalCastBudget,
        const int admittedBodyCount,
        const int admittedBodyIndex) noexcept
    {
        if (totalCastBudget <= 0 || admittedBodyCount <= 0 ||
            admittedBodyIndex < 0 ||
            admittedBodyIndex >= admittedBodyCount) {
            return 0;
        }
        const int base = totalCastBudget / admittedBodyCount;
        const int remainder = totalCastBudget % admittedBodyCount;
        return base + (admittedBodyIndex < remainder ? 1 : 0);
    }

    [[nodiscard]] inline constexpr std::size_t nextConnectedSweepCursor(
        const std::size_t current,
        const std::size_t connectedBodyCount) noexcept
    {
        return connectedBodyCount == 0 ?
                   0 :
                   (current + 1) % connectedBodyCount;
    }

    enum class HeldWorldSweepArmAction : std::uint8_t
    {
        Disarm,
        RebaseAtLivePose,
        Sweep,
    };

    /*
     * Predictive wall collision is meaningful only after the object has
     * actually reached its final palm-relative seat.  Sweeping an acquisition
     * target from a still-unseated body can turn a start overlap into a
     * fraction-zero clamp; convergence then mistakes that clamped, wrong-side
     * pose for the requested seat.  The first settled TouchHeld frame merely
     * seeds history from the live body.  Only the following frame may cast.
     * Once armed, an active wall stop remains authoritative even as controller
     * pressure intentionally grows the live-to-requested pivot error.
     */
    [[nodiscard]] inline HeldWorldSweepArmAction
    resolveHeldWorldSweepArmAction(
        const bool touchHeldPhase,
        const bool safeLivePoseAlreadySeeded,
        const bool trackingErrorValid,
        const float pivotTrackingErrorGameUnits,
        const float rotationTrackingErrorDegrees,
        const float maximumSeatPositionErrorGameUnits,
        const float maximumSeatRotationErrorDegrees) noexcept
    {
        if (!touchHeldPhase) {
            return HeldWorldSweepArmAction::Disarm;
        }
        if (safeLivePoseAlreadySeeded) {
            return HeldWorldSweepArmAction::Sweep;
        }
        if (!trackingErrorValid ||
            !std::isfinite(pivotTrackingErrorGameUnits) ||
            !std::isfinite(rotationTrackingErrorDegrees) ||
            !std::isfinite(maximumSeatPositionErrorGameUnits) ||
            !std::isfinite(maximumSeatRotationErrorDegrees)) {
            return HeldWorldSweepArmAction::Disarm;
        }
        const float positionTolerance =
            (std::max)(0.1f, maximumSeatPositionErrorGameUnits);
        const float rotationTolerance =
            (std::max)(0.1f, maximumSeatRotationErrorDegrees);
        return pivotTrackingErrorGameUnits <= positionTolerance &&
                       rotationTrackingErrorDegrees <= rotationTolerance
                   ? HeldWorldSweepArmAction::RebaseAtLivePose
                   : HeldWorldSweepArmAction::Disarm;
    }

    /*
     * A start-point hit from the end-orientation sample is deliberately mapped
     * to the beginning of that angular interval.  This is the conservative
     * part of the approximation: the object stops at the last pose proven clear
     * instead of being allowed to jump to an already-overlapping orientation.
     */
    [[nodiscard]] inline float rigidSweepHitFraction(
        const int segmentIndex,
        const int segmentCount,
        const float castHitFraction,
        const bool startPointHit) noexcept
    {
        if (segmentCount <= 0 || segmentIndex < 0 ||
            segmentIndex >= segmentCount) {
            return 1.0f;
        }
        const float localFraction =
            startPointHit || !std::isfinite(castHitFraction) ?
                0.0f :
                std::clamp(castHitFraction, 0.0f, 1.0f);
        return std::clamp(
            (static_cast<float>(segmentIndex) + localFraction) /
                static_cast<float>(segmentCount),
            0.0f,
            1.0f);
    }

    [[nodiscard]] inline float rigidSweepSkinFraction(
        const float skinGameUnits,
        const float bodyCenterPathGameUnits,
        const float bodySurfaceArcGameUnits) noexcept
    {
        const float safeSkin = std::isfinite(skinGameUnits) ?
                                   (std::max)(0.0f, skinGameUnits) :
                                   0.0f;
        const float centerPath =
            std::isfinite(bodyCenterPathGameUnits) ?
                (std::max)(0.0f, bodyCenterPathGameUnits) :
                0.0f;
        const float surfaceArc =
            std::isfinite(bodySurfaceArcGameUnits) ?
                (std::max)(0.0f, bodySurfaceArcGameUnits) :
                0.0f;
        const float effectivePath = centerPath + surfaceArc;
        return effectivePath > 0.0001f ?
                   std::clamp(safeSkin / effectivePath, 0.0f, 1.0f) :
                   0.0f;
    }

    [[nodiscard]] inline float rotationalEnvelopePadding(
        const float bodyRadiusGameUnits,
        const float segmentRotationDegrees,
        const float centerArcSagittaGameUnits) noexcept
    {
        const float radius = std::isfinite(bodyRadiusGameUnits) ?
                                 (std::max)(0.0f, bodyRadiusGameUnits) :
                                 0.0f;
        const float degrees = std::isfinite(segmentRotationDegrees) ?
                                  std::clamp(segmentRotationDegrees,
                                      0.0f,
                                      180.0f) :
                                  0.0f;
        const float sagitta = std::isfinite(centerArcSagittaGameUnits) ?
                                  (std::max)(0.0f,
                                      centerArcSagittaGameUnits) :
                                  0.0f;
        constexpr float kHalfDegreesToRadians =
            0.008726646259971647884f;
        // Every point of the intermediate rotated body lies within this
        // distance of the end-orientation body.  Adding the center-arc sagitta
        // also encloses connected bodies whose centers orbit the primary rather
        // than following the straight cast chord.
        return 2.0f * radius *
                   std::sin(degrees * kHalfDegreesToRadians) +
               sagitta;
    }

    [[nodiscard]] inline bool shouldRunHeldRigidSweep(
        const bool enabled,
        const bool discontinuityWarpApplied,
        const float translationDistanceGameUnits,
        const float rotationDegrees,
        const float minimumTranslationGameUnits,
        const bool rotationEnabled) noexcept
    {
        if (!enabled || discontinuityWarpApplied ||
            !std::isfinite(translationDistanceGameUnits) ||
            !std::isfinite(rotationDegrees)) {
            return false;
        }
        const float configuredTranslationThreshold =
            std::isfinite(minimumTranslationGameUnits) ?
                (std::max)(0.0f, minimumTranslationGameUnits) :
                0.0f;
        // A 0.5-gu per-frame threshold skips ordinary hand motion at 90 Hz
        // (and lets a held body walk through a wall a fraction at a time).
        // Keep only a tiny noise floor; one held-body cast per moving frame is
        // required for continuous collision.
        constexpr float kMaximumContinuousSweepNoiseFloorGameUnits = 0.01f;
        const float translationThreshold = (std::min)(
            configuredTranslationThreshold,
            kMaximumContinuousSweepNoiseFloorGameUnits);
        constexpr float kMinimumAngularMotionDegrees = 0.05f;
        constexpr float kMinimumLinearMotionGameUnits = 0.0001f;
        return (translationDistanceGameUnits >
                    kMinimumLinearMotionGameUnits &&
                   translationDistanceGameUnits >= translationThreshold) ||
               (rotationEnabled &&
                   rotationDegrees >= kMinimumAngularMotionDegrees);
    }

    inline constexpr float
        kHeldWorldStopOutwardReleaseSpeedGameUnitsPerSecond = 0.5f;

    [[nodiscard]] inline bool
    hasGenuineOutwardHeldWorldStopControllerMotion(
        const bool hasControllerVelocity,
        const bool stopNormalValid,
        const float controllerNormalSpeedGameUnitsPerSecond) noexcept
    {
        return hasControllerVelocity && stopNormalValid &&
               std::isfinite(
                   controllerNormalSpeedGameUnitsPerSecond) &&
               controllerNormalSpeedGameUnitsPerSecond >
                   kHeldWorldStopOutwardReleaseSpeedGameUnitsPerSecond;
    }

    [[nodiscard]] inline bool
    shouldUseRawControllerRetreatDeltaFallback(
        const bool genuineOutwardControllerMotion,
        const bool safeStopPoseValid,
        const float generatedTargetNormalMotionGameUnits) noexcept
    {
        constexpr float kGeneratedRetreatWitnessGameUnits = 0.001f;
        return genuineOutwardControllerMotion &&
               safeStopPoseValid &&
               (!std::isfinite(
                    generatedTargetNormalMotionGameUnits) ||
                   generatedTargetNormalMotionGameUnits <=
                       kGeneratedRetreatWitnessGameUnits);
    }

    [[nodiscard]] inline bool shouldHoldLatchedHeldWorldStop(
        const bool stopActive,
        const bool safeStartValid,
        const bool discontinuityWarpApplied,
        const float requestedDistanceGameUnits,
        const float requestedRotationDegrees,
        const bool stopNormalValid,
        const float requestedNormalMotionGameUnits,
        const bool genuineOutwardControllerMotion = false) noexcept
    {
        if (!stopActive || !safeStartValid ||
            discontinuityWarpApplied ||
            !std::isfinite(requestedDistanceGameUnits) ||
            !std::isfinite(requestedRotationDegrees) ||
            (stopNormalValid &&
                !std::isfinite(requestedNormalMotionGameUnits))) {
            return false;
        }
        // The generated palm target is deliberately frozen with the visual
        // hand while this latch owns the wall pose.  It therefore cannot
        // witness retreat on its own.  A room-motion-compensated raw-controller
        // sample is the authoritative first edge away from the saved plane,
        // even while the absolute generated target still lies inward.
        if (genuineOutwardControllerMotion) {
            return false;
        }
        const bool restingAtStop =
            requestedDistanceGameUnits <= 0.01f &&
            requestedRotationDegrees <= 0.05f;
        constexpr float kInwardMotionToleranceGameUnits = 0.001f;
        const bool stillPressingIntoSavedPlane =
            !restingAtStop &&
            stopNormalValid &&
            requestedNormalMotionGameUnits <
                -kInwardMotionToleranceGameUnits;
        return restingAtStop ||
               stillPressingIntoSavedPlane;
    }

    [[nodiscard]] inline bool shouldRetainHeldWorldStopWithoutHit(
        const bool stopActive,
        const bool safeStartValid,
        const bool discontinuityWarpApplied,
        const std::uint32_t castsRun,
        const float requestedDistanceGameUnits,
        const float requestedRotationDegrees,
        const bool stopNormalValid,
        const float requestedNormalMotionGameUnits,
        const bool genuineOutwardControllerMotion = false) noexcept
    {
        (void)castsRun;
        return shouldHoldLatchedHeldWorldStop(
            stopActive,
            safeStartValid,
            discontinuityWarpApplied,
            requestedDistanceGameUnits,
            requestedRotationDegrees,
            stopNormalValid,
            requestedNormalMotionGameUnits,
            genuineOutwardControllerMotion);
    }

    [[nodiscard]] inline constexpr bool
    shouldPreserveHeldWorldStopThroughRecovery(
        const bool predictiveStopOwnsFrame,
        const bool stopActive) noexcept
    {
        // A large-gap recovery writes the already admitted target. If that
        // target is the skin-safe wall pose, recovery must not silently disarm
        // the latch and make the next frame rediscover the same wall.
        return predictiveStopOwnsFrame && stopActive;
    }

    [[nodiscard]] inline constexpr bool
    resolveHeldMotorContactSoftening(
        const bool predictiveStopOwnsFrame,
        const bool contactRequestsSoftening) noexcept
    {
        // The query stop is the sole motion authority while latched. Cached
        // native contact evidence must not weaken the constraint and create a
        // second, oscillating response at the same surface.
        return !predictiveStopOwnsFrame && contactRequestsSoftening;
    }

    template <class Vec3>
    [[nodiscard]] inline bool startPointMotionBlocks(
        const Vec3& movingPointDelta,
        const Vec3& outwardSurfaceNormal,
        const float approachTolerance = 0.001f) noexcept
    {
        const float tolerance = std::isfinite(approachTolerance) ?
                                    (std::max)(0.0f, approachTolerance) :
                                    0.0f;
        const float approach =
            movingPointDelta.x * outwardSurfaceNormal.x +
            movingPointDelta.y * outwardSurfaceNormal.y +
            movingPointDelta.z * outwardSurfaceNormal.z;
        // Existing resting/sliding contact must not glue the object in place;
        // only motion with a component into the static surface is blocking.
        return std::isfinite(approach) && approach < -tolerance;
    }

    [[nodiscard]] inline SweepClampResult evaluateSweepClamp(
        const bool enabled,
        const bool hasWorldHit,
        const float earliestHitFraction,
        const float requestedDistanceGameUnits,
        const float skinGameUnits) noexcept
    {
        SweepClampResult result{};
        if (std::isfinite(requestedDistanceGameUnits) &&
            requestedDistanceGameUnits > 0.0f) {
            result.allowedDistanceGameUnits = requestedDistanceGameUnits;
        }
        if (!enabled || !hasWorldHit ||
            !std::isfinite(earliestHitFraction) ||
            !std::isfinite(requestedDistanceGameUnits) ||
            requestedDistanceGameUnits <= 0.0f) {
            return result;
        }

        const float hitFraction =
            std::clamp(earliestHitFraction, 0.0f, 1.0f);
        const float hitDistance = hitFraction * requestedDistanceGameUnits;
        const float safeSkin =
            std::isfinite(skinGameUnits) ?
                std::max(0.0f, skinGameUnits) :
                0.0f;
        result.allowedDistanceGameUnits =
            std::max(0.0f, hitDistance - safeSkin);
        result.allowedFraction = std::clamp(
            result.allowedDistanceGameUnits /
                requestedDistanceGameUnits,
            0.0f,
            1.0f);
        result.clamped = result.allowedFraction < 1.0f;
        return result;
    }
}
