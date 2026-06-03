#pragma once

// Heisenberg port of ROCK's BodyBoneColliderSet (May 2026).
// Source: ROCK-main/src/physics-interaction/body/BodyBoneColliderSet.cpp (~900 LOC).
//
// Builds 23 keyframed "capsule" bodies (rounded-box hull) for the player avatar:
// 5 torso segments (pelvis → spine1 → spine2 → chest → neck → head),
// 5 per arm × 2 sides (collarbone → upper arm → forearm1/2/3 → hand),
// 4 per leg × 2 sides (hip → thigh → calf → foot → toe).
//
// Each body spans the line between two named bones; position = midpoint,
// orientation aligns X-axis to (endBone - startBone), length = inter-bone distance.
// Filter info 0x000B002B (ROCK match) so it doesn't push the player's own proxy.
//
// Gated by Config::rockBodyBoneColliderSet.

namespace heisenberg::rock_body_collider
{
    void Init();
    void Shutdown();
    void Update();
    bool IsActive();
}
