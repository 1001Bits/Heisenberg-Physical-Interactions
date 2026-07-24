#pragma once

// Plugin-side re-implementation of FRIK v5's applyExternalHandWorldTransform / clear / get.
// FRIK v5's function is just "place the rendered first-person hand node at an arbitrary world
// transform, arbitrated by (tag, priority), after FRIK's arm-IK pass" — which we already do for the
// weapon node (TwoHandedGrip::applyWeaponVisualAuthority) at the same post-FRIK hook. This module lets
// two-handing (off-hand on the weapon) + hand/weapon wall-stop work with an OLDER FRIK that lacks the
// v5 API. A per-hand (tag, priority) registry collects writers each frame; ApplyWinners() (called at
// the tail of HookPostPhysics, AFTER the weapon-node write) picks the highest-priority active writer
// per hand and writes the skinned hand node + a 2-bone arm IK so the elbow/forearm follow.
//
// Thread note: Apply/Clear/Get only touch the registry array (under a mutex) — safe to call from the
// embed's ROCK update thread. The ONLY scene-graph mutation, ApplyWinners(), runs on the game thread.

#include <RE/Fallout.h>
#include <cstdint>

namespace heisenberg
{
    // POD callback table handed to the embedded ROCK engine (rock::HostSetHandAuthority) so its
    // frik_visual_authority bridge routes into this registry when native FRIK lacks v5. hand: 0=Right,
    // 1=Left. worldXform / outWorldXform are RE::NiTransform*.
    struct HandAuthorityHostTable
    {
        bool (*apply)(const char* tag, int hand, const void* worldXform, int priority);
        bool (*clear)(const char* tag, int hand);
        bool (*get)(int hand, void* outWorldXform);
    };

    class HandAuthority
    {
    public:
        // Registered + the COM skeleton is resolvable this frame.
        static bool Available();

        // Writer-facing API (mirrors FRIK apply/clear/get: same tag + priority semantics, level-
        // triggered — a writer must re-Apply every frame it wants control).
        static bool Apply(const char* tag, bool isLeft, const RE::NiTransform& world, int priority);
        static bool Clear(const char* tag, bool isLeft);
        // Rendered-hand transform: last-applied while overriding (stable base for delta writers so they
        // don't feed back on themselves), else the live node's world.
        static bool GetRenderedHand(bool isLeft, RE::NiTransform& out);

        // Arbiter + scene-graph write. Call once per frame at HookPostPhysics tail, AFTER the weapon
        // write (the off-hand two-handing target is derived from the final weapon world).
        static void ApplyWinners();

        // FRIK-GOAL seam (FrikArmGoalHook): read the current per-hand winning target
        // latched by ApplyWinners. Level-triggered; returns false when no fresh winner.
        static bool TryConsumeLatched(bool isLeft, RE::NiTransform& out,
            bool* rigidWeaponTarget = nullptr);

        // Project a free visual target onto the current anatomical wrist-reach sphere.
        // Full-solver rigid weapon targets are handled by the complete support-arm solve;
        // TryConsumeLatched reports that capability so callers do not separate a rigid
        // palm from its weapon with an earlier hand-only projection.
        static bool ConstrainTargetToArmReach(bool isLeft, RE::NiTransform& target,
            float* requestedDistance = nullptr, float* maxReach = nullptr);

        // Copy the current shoulder center and natural maximum wrist reach. FrikArmGoalHook
        // samples this beside its clean pass-1 controller truth and publishes the value to
        // ROCK for the same frame; no scene-graph pointers cross the host seam.
        static bool GetArmReachSphere(bool isLeft, RE::NiPoint3& shoulderWorld,
            float& maxReach);

        // FRIK-GOAL seam: record that the goal swap applied this target (bookkeeping for
        // GetRenderedHand/overriding so delta-writers cannot self-feed).
        static void NoteApplied(bool isLeft, const RE::NiTransform& world);

        // FRIK-GOAL seam: current skinned-hand world rotation (basis-capture for the
        // fp-node goal conversion). False if the arm chain cannot be resolved.
        static bool GetSkinnedHandRotation(bool isLeft, RE::NiMatrix3& out);

        // Frame-order seam: current skinned-hand full world transform (post-FRIK snapshot
        // for the embed's controller-truth input).
        static bool GetSkinnedHandWorld(bool isLeft, RE::NiTransform& out);

        // Cell change / save-load / shutdown — drop all writers + release cached raw nodes.
        static void Reset();

        // Callback table for the embed host seam (rock::HostSetHandAuthority).
        static const HandAuthorityHostTable& HostTable();

    private:
        HandAuthority() = default;
    };
}
