#pragma once

#include <algorithm>
#include <cmath>

namespace heisenberg::pipboy_mesh_contact
{
    struct LatchUpdate
    {
        bool latched = false;
        bool triggered = false;
    };

    struct GripReleaseGateUpdate
    {
        bool requiresRelease = false;
        bool blocksAction = false;
    };

    // An action started by releasing a held object must not consume the same
    // still-held Grip as a new action. Keep blocking for as long as Grip stays
    // down; the first released frame re-arms the next deliberate press.
    inline GripReleaseGateUpdate UpdateGripReleaseGate(
        bool requiredRelease,
        bool gripPressed) noexcept
    {
        if (!requiredRelease) {
            return { false, false };
        }
        if (gripPressed) {
            return { true, true };
        }
        return { false, false };
    }

    // Contact entry uses the whole swept fingertip path, while release uses only the
    // current fingertip. This catches a fast pass through a thin mesh without making
    // the old point from the previous frame keep the contact latched after the hand left.
    inline LatchUpdate UpdateLatch(
        bool wasLatched,
        float currentDistance,
        float sweptDistance,
        float enterDistance,
        float exitDistance,
        bool allowTrigger) noexcept
    {
        const float enter = (std::max)(0.0f, enterDistance);
        const float exit = (std::max)(enter, exitDistance);

        const bool outsideRelease = !std::isfinite(currentDistance) || currentDistance > exit;
        if (wasLatched && outsideRelease) {
            // The sweep still contains last frame's touching endpoint. Treat this frame only
            // as release/re-arm; otherwise a long-held button can fire again while leaving.
            return { false, false };
        }

        bool latched = wasLatched;

        const bool entered = std::isfinite(sweptDistance) && sweptDistance <= enter;
        const bool triggered = !latched && entered && allowTrigger;
        if (triggered) {
            // A cooldown must not consume a physical press.  If the fingertip remains
            // depressed when the cooldown expires, the next update is allowed to trigger;
            // once triggered, normal release hysteresis prevents repeated activation.
            latched = true;
        }

        return { latched, triggered };
    }

    // Samples are no farther apart than half a fingertip radius, so a swept contact
    // cannot tunnel through a surface-sized gap. Cap pathological tracking jumps.
    inline int SweepSegmentCount(float travelDistance, float contactRadius) noexcept
    {
        if (!std::isfinite(travelDistance) || travelDistance <= 0.0f) {
            return 1;
        }
        const float spacing = (std::max)(0.5f, contactRadius * 0.5f);
        return (std::clamp)(static_cast<int>(std::ceil(travelDistance / spacing)), 1, 32);
    }

    // A world bound is a sphere containing the rendered mesh. Expanding it by
    // the mesh-to-pivot offset also contains the node-point compatibility
    // fallback. If the complete fingertip path is farther away than that
    // combined sphere plus every behavior-sensitive interaction distance, an
    // exact triangle query cannot affect contact, latch, or release state.
    //
    // Invalid bounds fail open to the exact query. The extra guard covers the
    // button's small local animation and floating-point/bound update slack.
    inline bool CanRejectEjectMeshQuery(
        float pathDistanceToBoundCenter,
        float worldBoundRadius,
        float meshToPivotDistance,
        float interactionDistance,
        float safetyGuard = 1.0f) noexcept
    {
        if (!std::isfinite(pathDistanceToBoundCenter) ||
            pathDistanceToBoundCenter < 0.0f ||
            !std::isfinite(worldBoundRadius) ||
            worldBoundRadius < 0.1f ||
            worldBoundRadius > 10000.0f ||
            !std::isfinite(meshToPivotDistance) ||
            meshToPivotDistance < 0.0f ||
            !std::isfinite(interactionDistance) ||
            interactionDistance < 0.0f ||
            !std::isfinite(safetyGuard) ||
            safetyGuard < 0.0f) {
            return false;
        }

        const float rejectionDistance =
            worldBoundRadius +
            meshToPivotDistance +
            interactionDistance +
            safetyGuard;
        return std::isfinite(rejectionDistance) &&
               pathDistanceToBoundCenter > rejectionDistance;
    }

    // Host collision samples are sparse seed/boundary points from a continuous
    // convex hull. A small skin reconstructs the space between those points
    // without restoring the old oversized proximity bubble.
    inline float GeneratedDeckContactRadius(
        float sampleConvexRadius,
        float contactSkin = 0.5f) noexcept
    {
        const float radius =
            std::isfinite(sampleConvexRadius)
                ? (std::max)(0.0f, sampleConvexRadius)
                : 0.0f;
        const float skin =
            std::isfinite(contactSkin)
                ? (std::max)(0.0f, contactSkin)
                : 0.0f;
        return radius + skin;
    }

    // Entry requires overlap (or a validated continuous crossing). Once the
    // mechanism is already being pushed, a narrow release band holds its
    // current progress while the rotating surface clears the collider. It does
    // not advance the mechanism through empty space.
    inline bool TapeDeckContactKeepsPushing(
        float penetration,
        bool swept,
        bool alreadyPushing,
        float releaseHysteresis = 0.3f) noexcept
    {
        if (swept) {
            return true;
        }
        if (!std::isfinite(penetration)) {
            return false;
        }
        if (penetration > 0.0f) {
            return true;
        }
        const float hysteresis =
            std::isfinite(releaseHysteresis)
                ? (std::max)(0.0f, releaseHysteresis)
                : 0.0f;
        return alreadyPushing &&
            penetration >= -hysteresis;
    }

    // Active runtime depenetration: convert the linear overlap at a contact
    // point into angular progress about that surface's own hinge. Ordinary
    // overlap is rate-limited; a validated sweep may clear all of its measured
    // post-crossing depth immediately so the hand cannot emerge behind the
    // mechanism and make it spring open again.
    //
    // A true/estimated contact at the hinge has a near-zero geometric lever
    // radius. Dividing by that radius would snap the mechanism closed, but
    // rejecting it entirely leaves the visible hand passing through a detected
    // contact forever. Use the same deliberate 2.5-unit minimum physical stroke
    // as TapeDeckStrokeDistance instead: near-hinge contact remains gradual and
    // useful without producing an angular singularity.
    inline float TapeDeckProgressAfterPenetration(
        float currentOpenProgress,
        float penetration,
        float contactRadiusFromHinge,
        float fullOpenAngleRadians,
        bool swept,
        float maxSlowProgressStep = 0.2f) noexcept
    {
        const float current =
            std::isfinite(currentOpenProgress)
                ? (std::clamp)(
                      currentOpenProgress,
                      0.0f,
                      1.0f)
                : 1.0f;
        if (!std::isfinite(penetration) ||
            penetration <= 0.0f ||
            !std::isfinite(contactRadiusFromHinge) ||
            contactRadiusFromHinge < 0.0f ||
            !std::isfinite(fullOpenAngleRadians) ||
            fullOpenAngleRadians <= 1.0e-4f) {
            return current;
        }

        constexpr float minimumPhysicalStroke = 2.5f;
        const float geometricStroke =
            contactRadiusFromHinge *
            fullOpenAngleRadians;
        if (!std::isfinite(geometricStroke)) {
            return current;
        }
        const float effectiveStroke =
            (std::max)(
                geometricStroke,
                minimumPhysicalStroke);
        const float rawStep =
            penetration /
            effectiveStroke;
        const float slowCap =
            std::isfinite(maxSlowProgressStep)
                ? (std::clamp)(
                      maxSlowProgressStep,
                      0.0f,
                      1.0f)
                : 0.2f;
        const float step =
            (std::min)(
                rawStep,
                swept ? 1.0f : slowCap);
        return (std::clamp)(
            current - step,
            0.0f,
            1.0f);
    }

    // Estimate the physical hand travel needed to rotate an open tape deck to
    // closed. The contact point follows an arc about the hinge; clamping keeps
    // contacts very near/far from the hinge usable while retaining a deliberate,
    // gradual 2.5+ game-unit push.
    inline float TapeDeckStrokeDistance(
        float contactRadiusFromHinge,
        float startOpenProgress,
        float fullOpenAngleRadians) noexcept
    {
        if (!std::isfinite(contactRadiusFromHinge) ||
            !std::isfinite(startOpenProgress) ||
            !std::isfinite(fullOpenAngleRadians)) {
            return 3.0f;
        }

        const float radius = (std::max)(0.0f, contactRadiusFromHinge);
        const float progress = (std::clamp)(startOpenProgress, 0.0f, 1.0f);
        const float angle = (std::max)(0.0f, fullOpenAngleRadians);
        return (std::clamp)(radius * angle * progress, 2.5f, 6.0f);
    }

    // Map signed pusher travel along the fixed contact normal directly onto
    // deck openness (1=open, 0=closed). No elapsed-time term is present: merely
    // touching/holding still cannot make the deck continue closing.
    inline float TapeDeckProgressFromStroke(
        float startOpenProgress,
        float signedTravel,
        float requiredTravel) noexcept
    {
        const float start = std::isfinite(startOpenProgress)
            ? (std::clamp)(startOpenProgress, 0.0f, 1.0f)
            : 1.0f;
        if (!std::isfinite(signedTravel) ||
            !std::isfinite(requiredTravel) ||
            requiredTravel <= 1.0e-4f) {
            return start;
        }

        const float pressedFraction =
            (std::clamp)(signedTravel / requiredTravel, 0.0f, 1.0f);
        return start * (1.0f - pressedFraction);
    }

    // Collision centroids can move slightly backward as the rotating tray changes
    // which sample is deepest.  Preserve the deepest travel within one uninterrupted
    // physical stroke so that contact jitter cannot undo a deliberate push.
    inline float RetainDeepestTapeDeckTravel(
        float previousDeepestTravel,
        float currentSignedTravel) noexcept
    {
        const float previous =
            std::isfinite(previousDeepestTravel)
                ? (std::max)(0.0f, previousDeepestTravel)
                : 0.0f;
        const float current =
            std::isfinite(currentSignedTravel)
                ? (std::max)(0.0f, currentSignedTravel)
                : 0.0f;
        return (std::max)(previous, current);
    }

    // The real tray has a latch near the end of its travel; requiring an exact
    // floating-point zero made hand/weapon closure unnecessarily fragile.
    inline bool TapeDeckReachedMechanicalLatch(
        float openProgress,
        float latchOpenProgress = 0.15f) noexcept
    {
        if (!std::isfinite(openProgress)) {
            return false;
        }
        const float threshold =
            std::isfinite(latchOpenProgress)
                ? (std::clamp)(latchOpenProgress, 0.0f, 1.0f)
                : 0.15f;
        return openProgress <= threshold;
    }
}
