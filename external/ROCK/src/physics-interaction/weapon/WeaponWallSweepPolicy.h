#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rock::weapon_wall_sweep_policy
{
    inline constexpr int kMaximumBodies = 100;
    inline constexpr int kMaximumCastsPerFrame = 512;
    inline constexpr int kMaximumRotationSubstepsPerBody = 32;
    inline constexpr float kMaximumAngularStepDegrees = 6.0f;
    inline constexpr float kMaximumSurfaceArcStepGameUnits = 4.0f;
    inline constexpr float kPoseWitnessDistanceGameUnits = 0.01f;
    inline constexpr float kRootDiscontinuityDistanceGameUnits = 96.0f;
    inline constexpr float kRootDiscontinuityRotationDegrees = 135.0f;
    inline constexpr float kPlayerSpaceDiscontinuityDistanceGameUnits = 24.0f;
    inline constexpr float kPlayerSpaceDiscontinuityRotationDegrees = 20.0f;
    inline constexpr float kSweepStartFractionEpsilon = 0.0001f;
    inline constexpr float kContactMissGraceSeconds = 0.05f;
    inline constexpr float
        kStopReleaseLinearSpeedGameUnitsPerSecond = 270.0f;
    inline constexpr float
        kStopReleaseAngularSpeedDegreesPerSecond = 720.0f;
    inline constexpr float kRetreatDistanceEpsilonGameUnits = 0.05f;
    inline constexpr float
        kStableCurrentPoseAcquisitionLimitMultiplier = 2.0f;

    enum class UnvalidatedWeaponStopPoseSource : std::uint8_t
    {
        None,
        PreviousSafe,
        CurrentStable,
    };

    [[nodiscard]] inline UnvalidatedWeaponStopPoseSource
    chooseUnvalidatedWeaponStopPoseSource(
        const bool candidateValid,
        const bool sourceIsWeapon,
        const bool alreadyHasBlockedPose,
        const bool anchorValid,
        const bool previousSafePoseValid,
        const bool currentPoseValid,
        const float penetrationGameUnits,
        const float acquisitionLimitGameUnits) noexcept
    {
        if (!candidateValid || !sourceIsWeapon ||
            alreadyHasBlockedPose || !anchorValid ||
            !std::isfinite(penetrationGameUnits) ||
            penetrationGameUnits < 0.0f) {
            return UnvalidatedWeaponStopPoseSource::None;
        }
        // Generation-current history is the exact last-safe pose and always
        // outranks an overlap-depth estimate, including a fast deep crossing.
        if (previousSafePoseValid) {
            return UnvalidatedWeaponStopPoseSource::PreviousSafe;
        }

        // A first-frame QueryWorld overlap has no TOI when history was rebased
        // (for example after a dashboard stall). Freezing the finite current
        // pose creates no depenetration snap and is safe only within a bounded
        // acquisition envelope; enormous native/query glitches still fail
        // closed through the existing implausible-witness guard.
        if (!currentPoseValid ||
            !std::isfinite(acquisitionLimitGameUnits) ||
            acquisitionLimitGameUnits <= 0.0f ||
            penetrationGameUnits >
                acquisitionLimitGameUnits *
                    kStableCurrentPoseAcquisitionLimitMultiplier) {
            return UnvalidatedWeaponStopPoseSource::None;
        }
        return UnvalidatedWeaponStopPoseSource::CurrentStable;
    }

    [[nodiscard]] inline constexpr bool
    shouldAdmitWeaponSweepHandTarget(
        const bool ownershipResolved,
        const bool targetIsLeft,
        const bool rightHandFreeForWeaponStop,
        const bool leftHandFreeForWeaponStop) noexcept
    {
        // CastShape sees collision-suppressed production colliders as query
        // candidates too. Admit only a body whose side was resolved through a
        // live ROCK ownership registry and whose complete per-frame hand
        // policy says that side is free for reciprocal weapon stopping.
        return ownershipResolved &&
               (targetIsLeft ? leftHandFreeForWeaponStop :
                               rightHandFreeForWeaponStop);
    }

    [[nodiscard]] inline bool weaponDriveSegmentIsContinuous(
        const bool historyValid,
        const bool sameGeneration,
        const bool sameRoot,
        const bool posesFinite,
        const float sourceDeltaSeconds,
        const float rootTranslationGameUnits,
        const float rootRotationDegrees) noexcept
    {
        return historyValid && sameGeneration && sameRoot && posesFinite &&
               std::isfinite(sourceDeltaSeconds) &&
               sourceDeltaSeconds > 0.0f &&
               sourceDeltaSeconds <= 0.1f &&
               std::isfinite(rootTranslationGameUnits) &&
               std::isfinite(rootRotationDegrees) &&
               rootTranslationGameUnits <=
                   kRootDiscontinuityDistanceGameUnits &&
               rootRotationDegrees <=
                   kRootDiscontinuityRotationDegrees;
    }

    struct SweepWitness
    {
        bool fractionValid = false;
        bool validatedContinuousEntry = false;
        float sweepFraction = 1.0f;
    };

    [[nodiscard]] inline SweepWitness classifySweepWitness(
        const bool historyCurrent,
        const bool stationary,
        const bool poseWitness,
        const int segment,
        const int substeps,
        const float nativeFraction) noexcept
    {
        SweepWitness result{};
        if (substeps <= 0 || segment < 0 || segment >= substeps ||
            !std::isfinite(nativeFraction) || nativeFraction < 0.0f ||
            nativeFraction > 1.0f) {
            return result;
        }

        result.fractionValid = true;
        // A zero-distance cast is an angular-envelope witness: its artificial
        // +X query fraction has no relation to motion time, so conservatively
        // stop at the beginning of that rotational segment. Translation casts
        // retain the native TOI within the segment.
        result.sweepFraction = std::clamp(
            poseWitness ?
                static_cast<float>(segment) /
                    static_cast<float>(substeps) :
                (static_cast<float>(segment) + nativeFraction) /
                    static_cast<float>(substeps),
            0.0f,
            1.0f);
        result.validatedContinuousEntry =
            historyCurrent && !stationary &&
            (poseWitness ||
                result.sweepFraction >
                    kSweepStartFractionEpsilon);
        return result;
    }

    [[nodiscard]] inline float safeSweepFraction(
        const float sweepFraction,
        const float contactEnvelopeGameUnits,
        const float centerPathGameUnits,
        const float surfaceArcGameUnits) noexcept
    {
        if (!std::isfinite(sweepFraction)) {
            return 1.0f;
        }
        const float path =
            (std::isfinite(centerPathGameUnits) ?
                    (std::max)(0.0f, centerPathGameUnits) :
                    0.0f) +
            (std::isfinite(surfaceArcGameUnits) ?
                    (std::max)(0.0f, surfaceArcGameUnits) :
                    0.0f);
        const float envelope =
            std::isfinite(contactEnvelopeGameUnits) ?
                (std::max)(0.0f, contactEnvelopeGameUnits) :
                0.0f;
        const float skinFraction = path > 0.0001f ?
            std::clamp(envelope / path, 0.0f, 1.0f) :
            0.0f;
        return std::clamp(sweepFraction - skinFraction, 0.0f, 1.0f);
    }

    [[nodiscard]] inline float supportPlaneApproachSpeed(
        const float previousMinimumSignedDistanceGameUnits,
        const float currentMinimumSignedDistanceGameUnits,
        const float deltaSeconds) noexcept
    {
        if (!std::isfinite(previousMinimumSignedDistanceGameUnits) ||
            !std::isfinite(currentMinimumSignedDistanceGameUnits) ||
            !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) {
            return 0.0f;
        }
        return (std::max)(
            0.0f,
            (previousMinimumSignedDistanceGameUnits -
                currentMinimumSignedDistanceGameUnits) /
            deltaSeconds);
    }

    [[nodiscard]] inline bool
    shouldPromoteHandStartOverlapToContinuousEntry(
        const bool verifiedHandTarget,
        const bool rootHistoryCurrent,
        const bool rootHasMotion,
        const float sweepFraction,
        const float weaponApproachSpeedGameUnits) noexcept
    {
        /*
         * A moving generated hull can first become visible to Havok as a
         * fraction-zero start hit when the hand proxy broadphase updates one
         * sample later than the weapon root.  For static world that overlap
         * remains untrusted and uses the separate deep-overlap safeguard.  For
         * an ownership-verified hand, however, the current root segment plus a
         * positive support-plane approach supplies a conservative last-clear
         * pose: the previous root. Directional weapon-vs-hand arbitration still
         * has to approve the candidate downstream.
         */
        return verifiedHandTarget && rootHistoryCurrent && rootHasMotion &&
               std::isfinite(sweepFraction) &&
               sweepFraction <= kSweepStartFractionEpsilon &&
               std::isfinite(weaponApproachSpeedGameUnits) &&
               weaponApproachSpeedGameUnits > 0.0f;
    }

    [[nodiscard]] inline bool shouldReplaceParallelSweepHit(
        const bool existingValidated,
        const float existingSafeFraction,
        const float existingPenetration,
        const bool candidateValidated,
        const float candidateSafeFraction,
        const float candidatePenetration) noexcept
    {
        if (candidateValidated != existingValidated) {
            return candidateValidated;
        }
        if (candidateValidated &&
            std::isfinite(existingSafeFraction) &&
            std::isfinite(candidateSafeFraction)) {
            if (candidateSafeFraction + kSweepStartFractionEpsilon <
                existingSafeFraction) {
                return true;
            }
            if (existingSafeFraction + kSweepStartFractionEpsilon <
                candidateSafeFraction) {
                return false;
            }
        }
        return std::isfinite(candidatePenetration) &&
               (!std::isfinite(existingPenetration) ||
                   candidatePenetration > existingPenetration);
    }

    [[nodiscard]] inline bool retainStopAcrossContactMiss(
        const bool stopActive,
        const bool identityCurrent,
        const bool separationConfirmed,
        const float previousMissSeconds,
        const float deltaSeconds,
        float graceSeconds = kContactMissGraceSeconds) noexcept
    {
        if (!stopActive || !identityCurrent || separationConfirmed) {
            return false;
        }
        const float elapsed =
            (std::isfinite(previousMissSeconds) ?
                    (std::max)(0.0f, previousMissSeconds) :
                    graceSeconds) +
            (std::isfinite(deltaSeconds) ?
                    std::clamp(deltaSeconds, 0.0f, 0.1f) :
                    graceSeconds);
        const float safeGrace =
            std::isfinite(graceSeconds) && graceSeconds > 0.0f ?
                graceSeconds :
                kContactMissGraceSeconds;
        return elapsed <= safeGrace;
    }

    [[nodiscard]] inline bool rawProbeRetreatConfirmed(
        const bool previousSampleValid,
        const float previousSignedDistanceGameUnits,
        const float currentSignedDistanceGameUnits,
        float epsilonGameUnits =
            kRetreatDistanceEpsilonGameUnits) noexcept
    {
        if (!previousSampleValid ||
            !std::isfinite(previousSignedDistanceGameUnits) ||
            !std::isfinite(currentSignedDistanceGameUnits)) {
            return false;
        }
        const float epsilon =
            std::isfinite(epsilonGameUnits) &&
                    epsilonGameUnits >= 0.0f ?
                epsilonGameUnits :
                kRetreatDistanceEpsilonGameUnits;
        return currentSignedDistanceGameUnits >
               previousSignedDistanceGameUnits + epsilon;
    }

    [[nodiscard]] inline constexpr bool retainStopForResolvedProbe(
        const bool identityCurrent,
        const bool contactActive,
        const bool tangentCurrent) noexcept
    {
        // A motion reversal is not separation.  Releasing while the raw probe
        // is still behind the contact plane exposes the old deep-overlap
        // depenetration path and recreates the visible backward snap.
        return identityCurrent && contactActive && tangentCurrent;
    }

    enum class ImmutableWorldStopAction : std::uint8_t
    {
        Clear,
        Retain,
        BeginRelease,
        ContinueRelease,
    };

    [[nodiscard]] inline constexpr ImmutableWorldStopAction
    chooseImmutableWorldStopAction(
        const bool stopActive,
        const bool identityCurrent,
        const bool releaseActive,
        const bool probeResolved,
        const bool contactActive,
        const bool outwardRetreatConfirmed) noexcept
    {
        if (!stopActive || !identityCurrent) {
            return ImmutableWorldStopAction::Clear;
        }
        if (releaseActive) {
            // Once outward exit wins, a stationary outside sample must not
            // re-retain the partially released pose.
            return ImmutableWorldStopAction::ContinueRelease;
        }
        if (!probeResolved) {
            // Query/broadphase evidence can miss for several frames while the
            // raw weapon is held against a wall. Absence of evidence is not
            // retreat, regardless of elapsed time.
            return ImmutableWorldStopAction::Retain;
        }
        if (!contactActive && outwardRetreatConfirmed) {
            return ImmutableWorldStopAction::BeginRelease;
        }
        // Tangent drift, deeper pushing, and an unconfirmed outside sample
        // never rewrite or release the complete blocked pose.
        return ImmutableWorldStopAction::Retain;
    }

    [[nodiscard]] inline constexpr bool
    immutableWallStopOwnsFiringHandWriters(
        const bool stopPoseAvailable,
        const bool stopSurfaceAvailable,
        const bool targetIsDynamicHand) noexcept
    {
        // A retained static-world stop is one rigid weapon + firing-hand
        // transaction.  Soft-contact, dynamic-hand, and visual-return writers
        // must yield before that transaction captures/publishes its grip pair.
        // Reciprocal weapon/hand stops deliberately keep their existing
        // directional owner and are not promoted by this wall-only policy.
        return stopPoseAvailable && stopSurfaceAvailable &&
               !targetIsDynamicHand;
    }

    template <class Vector>
    [[nodiscard]] inline Vector constrainedPlaneTangentDelta(
        const Vector&,
        const Vector&) noexcept
    {
        // A retained contact is a complete last-safe pose, not a sliding
        // plane constraint. Raw tangent noise participates only in escape
        // detection and never advances the rendered weapon.
        return Vector{};
    }

    [[nodiscard]] inline constexpr bool shouldStabilizeVisibleBuild(
        const bool generationDrivenRebuild,
        const bool hasActiveBank,
        const int configuredStableFrames) noexcept
    {
        return generationDrivenRebuild && hasActiveBank &&
               configuredStableFrames > 0;
    }

    /*
     * The first visible frame is published immediately so a newly drawn gun
     * never has a collision-free window.  That bank remains provisional with
     * respect to geometry which the engine attaches a few frames later. Only
     * monotonic READY visible-geometry growth schedules a replacement: child
     * visibility decreases during reload animations must not tear down or
     * churn an already-complete collider bank.
     */
    [[nodiscard]] inline constexpr bool
    shouldScheduleMoreCompleteVisibleReplacement(
        const bool hasActiveBank,
        const std::uint64_t publishedVisualKey,
        const std::uint64_t observedVisualKey,
        const std::uint32_t publishedVisibleTriShapeCount,
        const std::uint32_t observedVisibleTriShapeCount) noexcept
    {
        return hasActiveBank &&
               publishedVisualKey != 0 &&
               observedVisualKey != 0 &&
               observedVisualKey != publishedVisualKey &&
               observedVisibleTriShapeCount >
                   publishedVisibleTriShapeCount;
    }

    [[nodiscard]] inline bool sourceDistanceFilterRejects(
        const bool enabled,
        const float centerDistanceGameUnits,
        const float maximumDistanceGameUnits) noexcept
    {
        return enabled && std::isfinite(centerDistanceGameUnits) &&
               std::isfinite(maximumDistanceGameUnits) &&
               maximumDistanceGameUnits > 0.0f &&
               centerDistanceGameUnits > maximumDistanceGameUnits;
    }

    [[nodiscard]] inline int rotationalSweepSubsteps(
        const float rotationDegrees,
        const float bodyRadiusGameUnits) noexcept
    {
        if (!std::isfinite(rotationDegrees) || rotationDegrees <= 0.0f) {
            return 1;
        }
        int requested = static_cast<int>(std::ceil(
            rotationDegrees / kMaximumAngularStepDegrees));
        if (std::isfinite(bodyRadiusGameUnits) &&
            bodyRadiusGameUnits > 0.0f) {
            constexpr float kDegreesToRadians =
                0.017453292519943295769f;
            requested = (std::max)(requested,
                static_cast<int>(std::ceil(
                    bodyRadiusGameUnits * rotationDegrees *
                    kDegreesToRadians /
                    kMaximumSurfaceArcStepGameUnits)));
        }
        return std::clamp(
            requested,
            1,
            kMaximumRotationSubstepsPerBody);
    }

    /*
     * Reserve one query for every published hull before distributing angular
     * extras. With <=100 hulls and a 512-query hard cap, no late attachment can
     * be starved by a long receiver consuming the whole budget.
     */
    [[nodiscard]] inline constexpr int fairCastBudget(
        const int bodyCount,
        const int bodyIndex) noexcept
    {
        if (bodyCount <= 0 || bodyCount > kMaximumBodies ||
            bodyIndex < 0 || bodyIndex >= bodyCount) {
            return 0;
        }
        const int base = kMaximumCastsPerFrame / bodyCount;
        const int remainder = kMaximumCastsPerFrame % bodyCount;
        return base + (bodyIndex < remainder ? 1 : 0);
    }

    [[nodiscard]] inline constexpr bool shouldRunStationaryWitness(
        const bool sweepHistoryCurrent,
        const bool contactEpisodeActive) noexcept
    {
        // Both exact planes have persistent anchor state, but the native
        // overlap is still the authoritative witness that the hull remains in
        // contact. Re-query while an episode is active so a stationary corner
        // cannot disappear merely because motion stopped.
        return !sweepHistoryCurrent || contactEpisodeActive;
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
        return 2.0f * radius *
                   std::sin(degrees * kHalfDegreesToRadians) +
               sagitta;
    }

    [[nodiscard]] inline bool poseIsDiscontinuous(
        const float translationDistanceGameUnits,
        const float rotationDegrees,
        const float maximumTranslationGameUnits,
        const float maximumRotationDegrees) noexcept
    {
        return !std::isfinite(translationDistanceGameUnits) ||
               !std::isfinite(rotationDegrees) ||
               translationDistanceGameUnits > maximumTranslationGameUnits ||
               rotationDegrees > maximumRotationDegrees;
    }

    [[nodiscard]] inline bool shouldRebaseSweepHistory(
        const bool generationMatches,
        const bool sourceBasisMatches,
        const bool playerSpaceDiscontinuity,
        const float rootTranslationDistanceGameUnits,
        const float rootRotationDegrees) noexcept
    {
        return !generationMatches || !sourceBasisMatches ||
               playerSpaceDiscontinuity ||
               poseIsDiscontinuous(
                   rootTranslationDistanceGameUnits,
                   rootRotationDegrees,
                   kRootDiscontinuityDistanceGameUnits,
                   kRootDiscontinuityRotationDegrees);
    }
}
