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

namespace heisenberg::rock_weapon_collision
{
    void Init();
    void Shutdown();
    void OnWeaponEquipped();    // hook: weapon equip/unequip events
    void OnWeaponUnequipped();
    void Update();
    bool IsActive();
}
