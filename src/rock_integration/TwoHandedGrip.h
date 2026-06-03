#pragma once

// Heisenberg port of ROCK's TwoHandedGrip (May 2026 — full impl).
// Source: ROCK-main/src/physics-interaction/weapon/TwoHandedGrip.cpp (~1500 LOC).
//
// Support-hand grip stabilization for two-handed weapons. Polls FRIK for the
// `isOffHandGrippingWeapon` state each frame. When active, computes a stabilization
// factor (0..1) from grip duration + grip stability metrics — this factor is
// queryable by downstream recoil/spread systems via GetStabilizationFactor().
//
// Why no direct firing-math hook: F4VR's recoil application happens deep inside
// the engine, and ROCK's own recoil modification is done via hooks into the spread
// calculation that aren't exposed cleanly. Heisenberg's TwoHandedGrip provides the
// authoritative "is grip active + how stable" state for any subsystem that wants
// to use it (Heisenberg's HeldBodyGrab, future Weapon collision physics, recoil
// hook mods, etc).
//
// Gated by Config::rockTwoHandedGrip + hard dependency on Config::rockWeaponCollision.

namespace heisenberg::rock_two_handed_grip
{
    void Init();
    void Shutdown();
    void Update();
    bool IsActive();

    // True when offhand is currently gripping the dominant-hand weapon (FRIK truth).
    bool IsGripping();

    // 0.0 = no stabilization (grip just engaged or unstable), 1.0 = fully stable
    // (grip held continuously for kFullStabilityTime seconds). Returns 0 when not
    // gripping or when subsystem inactive. Use this as a recoil-reduction factor
    // (e.g. spread *= (1.0 - 0.6*stabilization) — 60% spread cut at full stability).
    float GetStabilizationFactor();
}
