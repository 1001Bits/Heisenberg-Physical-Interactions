#pragma once

// Heisenberg port of ROCK's WeaponCollision (May 2026 — IN PROGRESS).
// Source: ROCK-main/src/physics-interaction/weapon/WeaponCollision.cpp (~1500 LOC subset
// — leaving SeeThroughScopesCompatibility/WeaponVisualRemapRuntime out).
//
// Generates collision hulls for currently-equipped weapons so they physically interact
// with the world: melee swings push clutter, scoped rifles bump off walls, etc. Operates
// on whatever weapon is in either hand.
//
// Gated by Config::rockWeaponCollision.
//
// Status: HEADER + STUB ONLY.

#include "rock_integration/SoftContactMath.h"

namespace heisenberg::rock_weapon_collision
{
    void Init();
    void Shutdown();
    void OnWeaponEquipped();    // hook: weapon equip/unequip events
    void OnWeaponUnequipped();
    void Update();
    bool IsActive();

    // Geometry-only: build a single capsule approximating the equipped weapon (AABB profiled
    // from the weapon NIF, capsule along its longest local axis), in WORLD space — for the
    // mode-3 soft-contact runtime (hand-vs-weapon). Independent of the physics hull. Returns
    // true if a weapon with geometry is equipped on that hand. Capsule id 500=right, 501=left.
    bool BuildWeaponCapsule(bool isLeft, heisenberg::rock_core::soft_contact_math::Capsule& out);
}
