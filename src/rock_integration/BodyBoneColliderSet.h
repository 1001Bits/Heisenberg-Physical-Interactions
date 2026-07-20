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

#include "rock_integration/SoftContactMath.h"

#include <array>
#include <cstdint>

namespace heisenberg::rock_body_collider
{
    void Init();
    void Shutdown();
    void Update();
    bool IsActive();

    inline constexpr int kCapsuleCount = 23;

    // Geometry-only build of the 23 body capsules from the live FRIK skeleton (no physics
    // bodies needed), for the mode-3 soft-contact runtime. Capsule id = 300 + descriptor
    // index; arm capsules are ids 305-309 (left arm) and 310-314 (right arm) so a hand can
    // skip its own arm. Returns the count of valid capsules.
    int BuildBodyCapsules(std::array<heisenberg::rock_core::soft_contact_math::Capsule, kCapsuleCount>& out);

    // True if the capsule id is the arm on the given hand's side (skip same-side arm).
    inline bool isSameSideArmCapsuleId(std::uint32_t id, bool isLeft)
    {
        return isLeft ? (id >= 305u && id <= 309u) : (id >= 310u && id <= 314u);
    }
}
