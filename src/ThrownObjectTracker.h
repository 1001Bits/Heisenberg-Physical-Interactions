#pragma once

/**
 * ThrownObjectTracker - Port of powerof3/GrabAndThrow effects to F4VR/Heisenberg.
 *
 * When an object is thrown via Heisenberg's grab system, this tracker:
 *   1. Tags it as a telekinesis object (engine attributes hits to the player)
 *   2. Subscribes to per-body CONTACT_STARTED via hknpWorld_getEventSignal_Body
 *      (same pattern as ContactImpulseListener / bhkTelekinesisListener)
 *   3. On the first contact, dispatches:
 *        - Capped mass/speed impact damage plus an explicit knock reaction (actors)
 *        - Destructible damage via TaskQueueInterface::QueueUpdateDestructibleObject
 *        - AI detection event via AIProcess::SetActorsDetectionEvent
 *      Native ProcessHurtfulBody is intentionally not called after manual
 *      damage because it applies a second, uncapped physics-damage pass.
 *   4. Auto-expires entries after a few seconds so we don't leak signals.
 */

#include <RE/Fallout.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heisenberg
{
    class ThrownObjectTracker
    {
    public:
        static ThrownObjectTracker& GetSingleton()
        {
            static ThrownObjectTracker instance;
            return instance;
        }

        // Stash the bhkWorld so OnThrown can subscribe to its hknpWorld signals.
        // Safe to call repeatedly; later calls overwrite earlier ones.
        void SetWorld(void* bhkWorld);

        // Called from GrabManager::EndGrab when the player throws an object.
        // velocity is in game units/sec (same convention as GrabState.throwVelocity).
        void OnThrown(RE::TESObjectREFR* refr, std::uint32_t bodyId,
                      const RE::NiPoint3& throwVelocity);

        // Drop any tracker entries whose 5s window has elapsed. Safe to call
        // every frame; cheap when the map is empty.
        void Tick();

        // Drain impacts queued by the Havok-worker contact callback and run their
        // damage/knockback/aggro on the GAME THREAD. MUST be called every frame from a
        // game-thread hook (HookPostPhysics). The contact callback only enqueues raw
        // data — all engine actor mutation happens here. Cheap when the queue is empty.
        void DrainPendingImpacts();

        // Drop everything (cell change, save load, shutdown).
        void Reset();

    private:
        ThrownObjectTracker() = default;
        ~ThrownObjectTracker() = default;
        ThrownObjectTracker(const ThrownObjectTracker&) = delete;
        ThrownObjectTracker& operator=(const ThrownObjectTracker&) = delete;

        struct Entry
        {
            RE::ObjectRefHandle refrHandle;
            void* hknpWorld = nullptr;       // exact body-ID namespace for this throw
            std::uint64_t generation = 0;   // rejects delayed contacts after a same-body rethrow
            float throwSpeed = 0.0f;        // game units / sec at release
            float thrownMass = 0.0f;        // form weight of thrown object (HIGGS-style proxy for havok mass)
            std::uint64_t expiryNs = 0;     // steady_clock ns; 0 = no expiry
            bool dispatched = false;        // first contact already handled
        };

        static void OnContactStartedCallback(void* worldPtr, void* eventPtr);
        // targetFormId is resolved at contact time (in the callback, where the contacted
        // body id is fresh) and passed through, rather than re-resolved a frame later in
        // the drain (a recycled/stale body index there resolved to nothing → "world").
        void HandleContact(
            void* hknpWorld,
            std::uint32_t thrownBodyId,
            std::uint64_t throwGeneration,
            std::uint32_t targetFormId);
        void DispatchImpact(RE::TESObjectREFR* thrownRefr,
                            RE::TESObjectREFR* targetRefr, float impactSpeed,
                            float thrownMass, std::uint32_t thrownBodyId);

        void* _bhkWorld = nullptr;
        void* _hknpWorld = nullptr;

        mutable std::mutex _mutex;
        std::unordered_map<std::uint32_t, Entry> _byBodyId;
        // Bodies natively subscribed to CONTACT_STARTED this world session. OnThrown has
        // no unsubscribe path (the native hkSignal subscriber list has no removal API used
        // here), so re-throwing the SAME object (pick up, throw, repeat) without this would
        // add another identical subscription every time, fanning out the callback N-fold per
        // contact. Cleared on SetWorld/Reset (a fresh world means fresh native signals).
        std::unordered_set<std::uint32_t> _subscribedBodyIds;

        // Impacts captured on the Havok worker thread, awaiting game-thread dispatch.
        // Only raw IDs + the world pointer are stored — refr resolution + all engine
        // calls happen in DrainPendingImpacts() on the game thread.
        struct PendingImpact
        {
            void* hknpWorld = nullptr;
            std::uint32_t thrownBodyId = 0;
            std::uint64_t throwGeneration = 0;
            std::uint32_t targetFormId = 0;  // resolved at contact time; 0 = world / unresolved
        };
        std::vector<PendingImpact> _pendingImpacts;
        std::uint64_t _nextThrowGeneration = 1;

        // Per-actor cooldown so a single thrown object that bounces off an NPC
        // doesn't tag them twice in quick succession (mirrors PLANCK's contact
        // cooldown pattern). Keyed on FormID (uint32) to avoid stale Actor ptrs.
        std::unordered_map<std::uint32_t, std::uint64_t> _actorHitCooldown;
    };
}
