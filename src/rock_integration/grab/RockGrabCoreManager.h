#pragma once

// Host driver for the ported ROCK grab core (iGrabMode=8, "RockGrabCore"). ROCK's vendored tree
// has no per-frame orchestrator (its engine host lives in ROCKMain), so this reconstructs the
// call order the grab-core contracts imply: StartGrab snapshots object-prep + freezes the grab
// authority frame + normalizes inertia + creates the custom constraint; UpdateGrab classifies
// the acquisition phase and drives the motors; EndGrab tears down crash-safely (ClearPlayerProxy
// support FIRST, then deferred-free retire). P3 wires the seams LOG-ONLY (no physics yet); P4+
// fill in the real constraint/motor work. Mirrors HeldBodyGrabManager's interface so the Grab.cpp
// seams look the same.

#include "rock_integration/grab/RockGrabConstraint.h"  // ActiveConstraint, SavedObjectState, tuning
#include "BethesdaPhysicsBody.h"                        // proxy body A (hidden keyframed anchor)

#include <RE/Fallout.h>
#include <cstdint>

namespace heisenberg
{
    struct GrabState;
}

namespace heisenberg::rock_grab_core
{
    class RockGrabCoreManager
    {
    public:
        static RockGrabCoreManager& GetSingleton();

        void Initialize();
        void Shutdown();

        // Available once initialized (and the engine constraint vtable built). Drives the
        // GetEffectiveGrabMode gate so a misconfigured build silently falls back to keyframed.
        static bool IsAvailable();

        // P4: StartGrab only marks the hand active (the object keeps Heisenberg's keyframed
        // pull-in). The REAL constraint work fires from TryCommitGrab once the object settles.
        bool StartGrab(GrabState& state, bool isLeft, const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot);

        // P4 commit (called from PostPhysicsGrabUpdate's settle gate AFTER the caller flipped
        // the object DYNAMIC): snapshots + normalizes inertia, spawns/keyframes the hidden
        // proxy body A, and creates the motorized constraint proxy(A)<->object(B). Returns
        // false (caller flips the object back keyframed) on any failure, restoring what it
        // changed. handWorld = the live hand/wand world transform.
        bool TryCommitGrab(GrabState& state, bool isLeft, void* bhkWorld, void* hknpWorld,
                           std::uint32_t objectBodyId, const RE::NiTransform& handWorld);

        // P4 per-frame drive for committed hands: hard-keyframe proxy A to the live hand.
        // (Motor-target rewrite + tau ramp = P5.)
        void UpdateGrab(GrabState& state, bool isLeft, const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot, float deltaTime);

        void EndGrab(GrabState& state, bool isLeft, const RE::NiPoint3* throwVelocity);

        // Drain the constraint's deferred-free retire ring (call once per completed physics step).
        void ServicePhysicsStep(std::uint32_t completedSteps);

        bool IsGrabActive(bool isLeft) const { return _active[isLeft ? 0 : 1]; }
        bool IsCommitted(bool isLeft) const { return _committed[isLeft ? 0 : 1]; }

    private:
        RockGrabCoreManager() = default;
        bool _initialized = false;
        bool _active[2] = { false, false };  // [0]=left, [1]=right

        // P4 per-hand grab state ([0]=left, [1]=right).
        rock_core::ActiveConstraint _constraint[2];
        rock_core::SavedObjectState _saved[2];
        bethesda_physics_body::BethesdaPhysicsBody _proxy[2];  // hidden keyframed anchor body A (persistent, parked between grabs)
        void* _proxyWorld[2] = { nullptr, nullptr };           // hknpWorld the proxy was CREATED in (recreate on mismatch — review B2)
        void* _proxyBhkWorld[2] = { nullptr, nullptr };
        void* _cachedWorld[2] = { nullptr, nullptr };          // hknpWorld captured at commit (teardown targets THIS world)
        void* _cachedBhkWorld[2] = { nullptr, nullptr };
        std::uint32_t _objectBodyId[2] = { 0x7FFFFFFF, 0x7FFFFFFF };
        bool _committed[2] = { false, false };
        // [GC-DIAG Jul 18] first-N-post-commit-frames tracer (mode-8 falls-out-of-hand bug)
        int _diagFrames[2] = { 999, 999 };
        RE::NiPoint3 _diagPrevObjPos[2]{};
    };
}
