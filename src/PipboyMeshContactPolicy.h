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
