#pragma once

// Hand wall-pushback (May 2026).
// Pushes the *rendered* hand back out of static geometry so an empty hand can't clip
// through walls — using FRIK v9's applyExternalHandWorldTransform (the hand override hook).
//
// Detection: 6 axis-aligned raycasts (Physics::CastRay) from the raw controller-tracked
// hand position give the deepest static-surface penetration (depth + normal). The raw
// hand position is tracked by integrating the wand-node delta while an override is active,
// so probing never reads the corrected (pushed-back) transform → no feedback oscillation.
//
// Two correction curves, switchable via Config::handWallPushbackMode:
//   0 = off
//   1 = linear depenetration (clamped depth * normal, smoothed)
//   2 = ROCK soft-contact (compliant soft-ramp -> hard stop, ported from ROCK SoftContactMath)
//
// Independent of the physics hand bodies — it only needs FRIK v9 + raycasts. (If physics
// hand bodies are on, see the self-hit note in the .cpp.)

namespace heisenberg::hand_wall_pushback
{
    // Per-frame, game thread (post-FRIK). No-op when handWallPushbackMode == 0.
    void Update();

    // Release any active hand overrides + reset tracking (cell change / save load / shutdown).
    void Reset();
}
