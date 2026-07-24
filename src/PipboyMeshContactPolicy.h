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
        if (entered) {
            // Latch even when triggering is temporarily blocked. A hand already resting on
            // the mesh must leave and touch it again after a cooldown/grip guard expires.
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
}
