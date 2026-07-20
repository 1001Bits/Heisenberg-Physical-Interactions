#pragma once

// Heisenberg port of ROCK's HandBoneColliderSet (May 2026).
// Source: ROCK-main/src/physics-interaction/hand/HandBoneColliderSet.{h,cpp} (~1300 LOC).
//
// Builds 16 keyframed bodies per hand (palm + 5 fingers × 3 segments) driven by
// FRIK skeleton bone positions each frame. Replaces Heisenberg's 3-body cup
// approximation when bRockHandBoneColliderSet is enabled.
//
// Uses Heisenberg-side facilities:
//   - BethesdaPhysicsBody (already ported)
//   - Utils::FindNode for bone access by name
//   - f4cf::f4vr::getPlayerNodes for skeleton root
//   - hand layer 43 + filter 0x000B002B (already proven)
//
// Body count: 16 per hand × 2 hands = 32 keyframed bodies.
//
// Gated by Config::rockHandBoneColliderSet AND requires:
//   bUsePhysicsHandBodies=true + bUseBethesdaPhysicsBody=true + bEnableHandCollision=true.
// When IsActive(), HandCollision should yield its 3-body cup path.

#include "rock_integration/SoftContactMath.h"

#include <array>
#include <cstdint>

namespace heisenberg::rock_hand_collider
{
    // Module lifecycle
    void Init();
    void Shutdown();

    // Per-frame entry — call from HandCollision::Update when the toggle is on.
    // First call builds bodies, subsequent calls drive them to bone positions.
    void Update();

    // True if this subsystem owns the hand-collision pipeline this frame.
    // HandCollision's existing cup path should check this and yield.
    bool IsActive();

    // 16 capsule segments per hand (palm + 5 fingers × 3).
    inline constexpr int kCapsulesPerHand = 16;

    // GEOMETRY-ONLY: build the 16 finger-segment capsules for a hand directly from the
    // FRIK skeleton (getCommonNode), independent of whether the physics bodies exist.
    // Reuses the same bone table + radii as the physics colliders. Used by the mode-3
    // soft-contact runtime (hand-vs-hand/body/weapon). Returns the count of valid capsules.
    int BuildHandCapsules(bool isLeft, std::array<heisenberg::rock_core::soft_contact_math::Capsule, kCapsulesPerHand>& out);
}
