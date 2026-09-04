#pragma once

/*
 * Engine-independent stability policy for the dynamic hand visual stop.
 *
 * Hosted FRIK publishes two hand frames: the flattened/skinned hand (which can
 * already contain the previous frame's external authority translation) and a
 * clean pre-authority hand.  Dynamic collision must drive from the latter or
 * its own correction becomes the next frame's input and contact alternates
 * on/off.
 *
 * Once contact is established, smoothing a correction vector is also the
 * wrong coordinate space: as the controller presses farther through a wall,
 * the required correction grows by the opposite amount.  Filtering that
 * growing vector lets the rendered hand creep into the wall.  The policy
 * therefore stabilizes the blocked WORLD target and derives the correction
 * from the current clean controller frame each frame.
 */

#include <algorithm>
#include <cmath>

namespace rock::dynamic_hand_collision_stability
{
    inline constexpr float kContactMissGraceSeconds = 0.05f;
    inline constexpr float kMinimumCoherentSurfaceNormalDot = 0.25f;
    inline constexpr float kMaximumCoherentWorldTargetJumpGameUnits = 24.0f;
    inline constexpr float kMaximumContactEntryOutwardSpeedGameUnitsPerSecond =
        270.0f;

    [[nodiscard]] inline constexpr bool hostedCleanFrameAccepted(
        const bool hostedCleanFrameRequired,
        const bool cleanSnapshotAvailable,
        const bool cleanTransformFinite,
        const bool cleanTranslationRebaseValid) noexcept
    {
        return !hostedCleanFrameRequired ||
               (cleanSnapshotAvailable && cleanTransformFinite &&
                   cleanTranslationRebaseValid);
    }

    /*
     * Hand/finger roles are descendants of the corrected hand and can be
     * restored by the clean hand delta. The forearm is an IK ancestor: it
     * moves/rotates by a different amount, so applying the full hand delta
     * creates feedback instead of removing it. Admit it in hosted mode only
     * when a separately captured clean forearm frame exists.
     */
    [[nodiscard]] inline constexpr bool roleTargetAccepted(
        const bool hostedCleanFrameRequired,
        const bool roleIsForearm,
        const bool cleanForearmFrameAvailable) noexcept
    {
        return !hostedCleanFrameRequired || !roleIsForearm ||
               cleanForearmFrameAvailable;
    }

    template <class Vector>
    [[nodiscard]] inline bool finitePoint(const Vector& value) noexcept
    {
        return std::isfinite(value.x) &&
               std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    template <class Vector>
    [[nodiscard]] inline Vector add(
        const Vector& lhs,
        const Vector& rhs) noexcept
    {
        return Vector{
            lhs.x + rhs.x,
            lhs.y + rhs.y,
            lhs.z + rhs.z,
        };
    }

    template <class Vector>
    [[nodiscard]] inline Vector subtract(
        const Vector& lhs,
        const Vector& rhs) noexcept
    {
        return Vector{
            lhs.x - rhs.x,
            lhs.y - rhs.y,
            lhs.z - rhs.z,
        };
    }

    template <class Vector>
    [[nodiscard]] inline Vector multiply(
        const Vector& value,
        float scalar) noexcept
    {
        return Vector{
            value.x * scalar,
            value.y * scalar,
            value.z * scalar,
        };
    }

    template <class Vector>
    [[nodiscard]] inline float dot(
        const Vector& lhs,
        const Vector& rhs) noexcept
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    template <class Vector>
    [[nodiscard]] inline float length(const Vector& value) noexcept
    {
        const float lengthSquared = dot(value, value);
        return std::isfinite(lengthSquared) && lengthSquared >= 0.0f ?
                   std::sqrt(lengthSquared) :
                   0.0f;
    }

    template <class Vector>
    [[nodiscard]] inline bool tryNormalize(
        const Vector& value,
        Vector& out) noexcept
    {
        const float valueLength = length(value);
        if (!finitePoint(value) || !std::isfinite(valueLength) ||
            valueLength <= 1.0e-5f) {
            out = {};
            return false;
        }
        out = multiply(value, 1.0f / valueLength);
        return finitePoint(out);
    }

    template <class Vector>
    struct CleanTranslationRebase
    {
        Vector cleanTranslation{};
        Vector translationOffset{};
        bool valid = false;
    };

    template <class Vector>
    [[nodiscard]] inline CleanTranslationRebase<Vector>
        resolveCleanTranslationRebase(
            const Vector& flattenedTranslation,
            const Vector& preAuthorityTranslation) noexcept
    {
        CleanTranslationRebase<Vector> result{};
        result.cleanTranslation = flattenedTranslation;
        if (!finitePoint(flattenedTranslation) ||
            !finitePoint(preAuthorityTranslation)) {
            return result;
        }

        const Vector offset =
            subtract(preAuthorityTranslation, flattenedTranslation);
        result.cleanTranslation = preAuthorityTranslation;
        result.translationOffset = offset;
        result.valid = finitePoint(offset);
        return result;
    }

    template <class Vector>
    [[nodiscard]] inline Vector applyTranslationRebase(
        const Vector& value,
        const CleanTranslationRebase<Vector>& rebase) noexcept
    {
        return rebase.valid ? add(value, rebase.translationOffset) : value;
    }

    template <class Vector>
    [[nodiscard]] inline Vector smoothPoint(
        const Vector& applied,
        const Vector& target,
        float smoothingSpeed,
        float deltaSeconds) noexcept
    {
        if (!finitePoint(applied) || !finitePoint(target)) {
            return finitePoint(target) ? target : Vector{};
        }
        if (!std::isfinite(smoothingSpeed) || smoothingSpeed <= 0.0f) {
            return target;
        }
        const float dt = std::clamp(
            std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f),
            0.0f,
            0.1f);
        const float alpha = std::clamp(
            1.0f - std::exp(-smoothingSpeed * dt),
            0.0f,
            1.0f);
        return Vector{
            applied.x + (target.x - applied.x) * alpha,
            applied.y + (target.y - applied.y) * alpha,
            applied.z + (target.z - applied.z) * alpha,
        };
    }

    template <class Vector>
    struct ContactHoldState
    {
        Vector blockedWorldTarget{};
        Vector outwardNormal{};
        Vector lastCleanHandTranslation{};
        float missedContactSeconds = 0.0f;
        bool active = false;
        bool outwardNormalValid = false;
        bool lastCleanHandTranslationValid = false;
    };

    template <class Vector>
    struct ContactHoldStep
    {
        ContactHoldState<Vector> state{};
        Vector correction{};
        bool holdsWorldTarget = false;
        bool confirmedContact = false;
        bool surfaceReset = false;
        bool releasedByRetreat = false;
    };

    /*
     * A first solver witness may trail the controller by one substep. While
     * the controller is moving into the contact normal, never allow that old
     * witness to move the rendered hand outward past its previous free pose.
     * A surface moving into a stationary hand is different: admit a bounded
     * outward step so the obstacle pushes the hand instead of overtaking it.
     */
    template <class Vector>
    [[nodiscard]] inline Vector continuousEntryWorldTarget(
        const Vector& previousHandTranslation,
        const Vector& currentHandTranslation,
        const Vector& candidateWorldTarget,
        const Vector& outwardNormal,
        float deltaSeconds) noexcept
    {
        if (!finitePoint(previousHandTranslation) ||
            !finitePoint(currentHandTranslation) ||
            !finitePoint(candidateWorldTarget) ||
            !finitePoint(outwardNormal)) {
            return candidateWorldTarget;
        }
        const float controllerOutwardMotion = dot(
            subtract(currentHandTranslation, previousHandTranslation),
            outwardNormal);
        const float candidateOutwardMotion = dot(
            subtract(candidateWorldTarget, previousHandTranslation),
            outwardNormal);
        if (!std::isfinite(controllerOutwardMotion) ||
            !std::isfinite(candidateOutwardMotion) ||
            candidateOutwardMotion <= 0.0f) {
            return candidateWorldTarget;
        }

        const float dt = std::clamp(
            std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f),
            0.0f,
            0.1f);
        const float maximumOutwardMotion =
            controllerOutwardMotion < -1.0e-4f ?
                0.0f :
                kMaximumContactEntryOutwardSpeedGameUnitsPerSecond * dt;
        if (candidateOutwardMotion <= maximumOutwardMotion) {
            return candidateWorldTarget;
        }
        return subtract(
            candidateWorldTarget,
            multiply(
                outwardNormal,
                candidateOutwardMotion - maximumOutwardMotion));
    }

    /*
     * Convert per-frame contact deviation into a stable world-space hand stop.
     * The one-sided projection forbids an established target from creeping
     * farther into the contacted plane when a physics sample is one frame
     * behind the controller. Outward solver corrections and new corner
     * constraints remain allowed; only their surface-normal component is
     * smoothed, while tangential sliding remains exact.
     */
    template <class Vector>
    [[nodiscard]] inline ContactHoldStep<Vector> advanceContactHold(
        ContactHoldState<Vector> state,
        const Vector& cleanHandTranslation,
        const Vector& contactDeviation,
        bool contactActive,
        float deltaSeconds,
        float smoothingSpeed,
        float minimumCorrectionGameUnits,
        float missGraceSeconds = kContactMissGraceSeconds) noexcept
    {
        ContactHoldStep<Vector> result{};
        const float dt = std::clamp(
            std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f),
            0.0f,
            0.1f);
        const float minCorrection =
            std::isfinite(minimumCorrectionGameUnits) ?
                std::max(0.0f, minimumCorrectionGameUnits) :
                0.0f;
        const float grace = std::isfinite(missGraceSeconds) ?
            std::max(0.0f, missGraceSeconds) :
            kContactMissGraceSeconds;
        const float deviationLength = length(contactDeviation);
        const bool contactWitnessValid =
            contactActive && finitePoint(cleanHandTranslation) &&
            finitePoint(contactDeviation) &&
            deviationLength > minCorrection;
        const Vector candidateWorldTarget = contactWitnessValid ?
            add(cleanHandTranslation, contactDeviation) :
            Vector{};
        Vector currentNormal{};
        const bool currentNormalValid =
            contactWitnessValid &&
            tryNormalize(contactDeviation, currentNormal);
        bool resetForSurfaceChange = false;
        bool targetJumpImplausible = false;
        if (contactWitnessValid && state.active &&
            finitePoint(state.blockedWorldTarget)) {
            const float targetJump = length(subtract(
                candidateWorldTarget,
                state.blockedWorldTarget));
            targetJumpImplausible =
                std::isfinite(targetJump) &&
                targetJump >
                    kMaximumCoherentWorldTargetJumpGameUnits;
            const float normalDot =
                currentNormalValid && state.outwardNormalValid ?
                    dot(currentNormal, state.outwardNormal) :
                    1.0f;
            const bool surfaceNormalChanged =
                currentNormalValid && state.outwardNormalValid &&
                (!std::isfinite(normalDot) ||
                    normalDot < kMinimumCoherentSurfaceNormalDot);
            // A contact episode owns one immutable last-safe world pose. A
            // changing residual/normal is common at weapon edges and wall
            // triangles and must not move or replace that pose.
            resetForSurfaceChange = !state.active && surfaceNormalChanged;
        }

        if (targetJumpImplausible) {
            /*
             * A body rebuild, stale role sample, or teleport can manufacture
             * an arm-scale candidate. It is not safe to replace the current
             * stop with that witness (the old behavior hard-snapped to it),
             * nor to drag the hand back to a now-distant old plane. Relinquish
             * visual authority and let a coherent subsequent sample acquire
             * through the bounded entry path.
             */
            result.state = state;
            result.correction = subtract(
                state.blockedWorldTarget,
                cleanHandTranslation);
            result.holdsWorldTarget = finitePoint(result.correction);
            result.confirmedContact = true;
            return result;
        }

        if (state.active && state.outwardNormalValid &&
            finitePoint(cleanHandTranslation) &&
            finitePoint(state.blockedWorldTarget) &&
            finitePoint(state.outwardNormal)) {
            const float cleanDistanceOutsideOldPlane = dot(
                subtract(
                    cleanHandTranslation,
                    state.blockedWorldTarget),
                state.outwardNormal);
            if (!resetForSurfaceChange &&
                std::isfinite(cleanDistanceOutsideOldPlane) &&
                cleanDistanceOutsideOldPlane >
                    std::max(minCorrection, 1.0e-4f)) {
                /*
                 * Physics telemetry is one substep behind the clean controller
                 * frame. Once the controller has crossed outward through the
                 * established stop plane, even a still-active old contact must
                 * not pull the rendered hand back behind the controller.
                 */
                result.state = {};
                result.releasedByRetreat = true;
                return result;
            }
        }

        if (contactWitnessValid) {
            const Vector previousBlockedWorldTarget =
                state.blockedWorldTarget;
            const Vector previousOutwardNormal = state.outwardNormal;
            const bool previousSurfaceValid =
                state.active && state.outwardNormalValid &&
                state.lastCleanHandTranslationValid &&
                finitePoint(previousBlockedWorldTarget) &&
                finitePoint(previousOutwardNormal) &&
                finitePoint(state.lastCleanHandTranslation);
            const float cleanRetreatDelta = previousSurfaceValid ?
                dot(
                    subtract(
                        cleanHandTranslation,
                        state.lastCleanHandTranslation),
                    previousOutwardNormal) :
                0.0f;
            const bool cleanControllerRetreating =
                previousSurfaceValid &&
                std::isfinite(cleanRetreatDelta) &&
                cleanRetreatDelta > 1.0e-4f;
            if (!state.active || !finitePoint(state.blockedWorldTarget) ||
                resetForSurfaceChange) {
                /*
                 * The solver-supported point may lie between the previous
                 * clear controller pose and this frame's requested pose. Use
                 * that exact boundary when coherent, so contact does not jump
                 * the hand all the way back to the previous frame. The helper
                 * clamps a stale/ejection witness to the last-clear pose and
                 * bounds a moving obstacle's outward push.
                 */
                state.blockedWorldTarget =
                    state.lastCleanHandTranslationValid &&
                            finitePoint(state.lastCleanHandTranslation) &&
                            currentNormalValid ?
                        continuousEntryWorldTarget(
                            state.lastCleanHandTranslation,
                            cleanHandTranslation,
                            candidateWorldTarget,
                            currentNormal,
                            dt) :
                        cleanHandTranslation;
            } else {
                // Keep the complete latched pose fixed until the clean hand
                // retreats out of the contact plane. This prevents solver
                // noise and moving-weapon residuals from producing jitter.
            }

            if (cleanControllerRetreating && !resetForSurfaceChange &&
                finitePoint(state.blockedWorldTarget)) {
                const float laggedBackwardAnchorMotion = dot(
                    subtract(
                        state.blockedWorldTarget,
                        previousBlockedWorldTarget),
                    previousOutwardNormal);
                if (std::isfinite(laggedBackwardAnchorMotion) &&
                    laggedBackwardAnchorMotion > 0.0f) {
                    /*
                     * A previous-substep deviation can otherwise be added to
                     * the newer retreating controller frame twice, moving the
                     * stop target away from the wall and behind the hand. Keep
                     * the old plane fixed until the clean hand crosses it;
                     * tangential motion and genuine surface resets remain free.
                     */
                    state.blockedWorldTarget = subtract(
                        state.blockedWorldTarget,
                        multiply(
                            previousOutwardNormal,
                            laggedBackwardAnchorMotion));
                }
            }

            if (currentNormalValid) {
                state.outwardNormal = currentNormal;
                state.outwardNormalValid = true;
            }
            state.missedContactSeconds = 0.0f;
            state.active = true;
            state.lastCleanHandTranslation = cleanHandTranslation;
            state.lastCleanHandTranslationValid = true;

            result.state = state;
            result.correction = subtract(
                state.blockedWorldTarget,
                cleanHandTranslation);
            result.holdsWorldTarget = finitePoint(result.correction);
            result.confirmedContact = true;
            result.surfaceReset = resetForSurfaceChange;
            return result;
        }

        if (state.active && finitePoint(cleanHandTranslation) &&
            finitePoint(state.blockedWorldTarget)) {
            state.missedContactSeconds += dt;
            const Vector correction = subtract(
                state.blockedWorldTarget,
                cleanHandTranslation);
            const float requiredOutwardCorrection =
                state.outwardNormalValid ?
                    dot(correction, state.outwardNormal) :
                    length(correction);
            if (state.missedContactSeconds <= grace &&
                std::isfinite(requiredOutwardCorrection) &&
                requiredOutwardCorrection > minCorrection) {
                // Bridge short after-solve/contact-threshold gaps while the
                // controller still requests a pose through the same plane.
                result.state = state;
                result.correction = correction;
                result.holdsWorldTarget = true;
                return result;
            }
        }

        result.state = {};
        return result;
    }
}
