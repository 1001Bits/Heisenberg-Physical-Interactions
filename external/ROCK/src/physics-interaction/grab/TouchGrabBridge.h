#pragma once

// Minimal, host-callable bridge to ROCK's mesh-based grab finger-pose solver
// (GrabFinger.h / MeshGrab.h). Deliberately declares ONLY simple types (no FRIK
// API types, no GrabFinger/MeshGrab types) so Heisenberg's Grab.cpp can call this
// without pulling in the transitive FrikVisualAuthorityBridge.h dependency chain
// that isn't resolvable from outside ROCK's own compile context.

#include <array>
#include <cstdint>

#include <RE/Fallout.h>

namespace rock::touch_grab_bridge
{
    // Extracts the object's mesh from its scene-graph root, resolves the live
    // finger skeleton for the given hand, and solves finger curl so each finger
    // stops just short of the mesh surface instead of assuming a fixed pose.
    // outJointCurls receives 15 per-joint values (3 joints x 5 fingers, ROCK's
    // standard expansion) on success. Returns false if the object has no usable
    // mesh, the live finger skeleton can't be resolved, or the solve fails.
    [[nodiscard]] bool SolveTouchGrabFingerPose(
        RE::NiAVObject* objectRoot,
        const RE::NiTransform& handWorld,
        bool isLeft,
        const RE::NiPoint3& grabAnchorWorld,
        const RE::NiPoint3& grabContactWorld,
        std::array<float, 15>& outJointCurls);

    // Solves the same visible mesh into ROCK's complete finger-pose payload,
    // then publishes its per-joint curls, contact-derived splay, and bounded
    // local surface corrections through frik_visual_authority. On stock FRIK
    // 0.77.12 this automatically uses Heisenberg's legacy full-pose host seam.
    //
    // deltaTime drives optional repeated-call smoothing. One-shot touch-grab
    // callers should keep snapJointPoseToTarget=true (the default).
    // Returns true only when the rich pose requested by the current ROCK
    // settings was accepted; callers may then use SolveTouchGrabFingerPose as
    // their curl-only fallback when this returns false.
    [[nodiscard]] bool SolveAndPublishTouchGrabFingerPose(
        RE::NiAVObject* objectRoot,
        const RE::NiTransform& handWorld,
        bool isLeft,
        const RE::NiPoint3& grabAnchorWorld,
        const RE::NiPoint3& grabContactWorld,
        float deltaTime = 0.0f,
        bool snapJointPoseToTarget = true,
        std::array<float, 15>* outJointCurls = nullptr);

    // False when a skeleton or power-armor identity reset invalidates the
    // tagged FRIK/legacy-host winner, allowing the caller to republish.
    [[nodiscard]] bool IsTouchGrabFingerPoseActive(bool isLeft);

    // Clears ROCK_Grab for one hand and drops all bridge smoothing state.
    [[nodiscard]] bool ClearTouchGrabFingerPose(bool isLeft);
}
