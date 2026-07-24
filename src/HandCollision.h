#pragma once

/**
 * HandCollision - Body-less proximity collision between VR hands and world objects
 *
 * Two-scenario cleanup (Jul 2026): the physics hand-body path (Havok cup/finger
 * bodies, BethesdaPhysicsBody pipeline) was removed. What remains is the proven
 * body-less proximity push — swept hand point-cloud vs object meshes, pushing via
 * SetLinearVelocity + wake — which coexists with the embedded ROCK engine's real
 * hand colliders (ROCK hand DynamicPushAssist is suppressed while this runs), plus
 * the contact tracking + haptics accessors that feed the HeisenbergInterface001 API.
 */

#include <RE/Fallout.h>

namespace heisenberg
{
    // Havok world scale constant
    constexpr float HAVOK_WORLD_SCALE_COLLISION = 0.0142875f;

    /**
     * HandCollision - body-less proximity hand collision + contact tracking
     *
     * Thread safety:
     *   - Main thread: Update, Initialize, Shutdown
     *   - _leftContact/_rightContact: ObjectRefHandle (safe reference), main-thread only
     *   - GetContactObject/IsInContact: main-thread only (no cross-thread callers)
     */
    class HandCollision
    {
    public:
        static HandCollision& GetSingleton()
        {
            static HandCollision instance;
            return instance;
        }

        /**
         * Initialize the hand collision system
         * Called once at game start
         * @return true if successful
         */
        bool Initialize();

        /**
         * Shutdown and cleanup
         */
        void Shutdown();

        /**
         * Clear contact pointers on save/load to prevent dangling pointers.
         * CRITICAL: Must be called on kPreLoadGame/kPostLoadGame.
         */
        void ClearContacts()
        {
            _leftContact.reset();
            _rightContact.reset();
            _leftContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
            _rightContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
            _pendingLeftContactClear.store(false, std::memory_order_relaxed);
            _pendingRightContactClear.store(false, std::memory_order_relaxed);
            spdlog::debug("[HandCollision] Cleared contact pointers (save/load cleanup)");
        }

        /**
         * Update hand collision for both hands (proximity push)
         * @param leftHandPos Left hand world position
         * @param rightHandPos Right hand world position
         * @param leftHandVel Left hand velocity
         * @param rightHandVel Right hand velocity
         * @param leftHandRot Left hand rotation
         * @param rightHandRot Right hand rotation
         * @param deltaTime Frame delta time
         */
        void Update(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos,
                    const RE::NiPoint3& leftHandVel, const RE::NiPoint3& rightHandVel,
                    const RE::NiMatrix3& leftHandRot, const RE::NiMatrix3& rightHandRot,
                    float deltaTime);

        /**
         * Check if a hand is currently in contact with an object
         * @param isLeft Which hand
         * @return true if hand is touching something
         */
        bool IsInContact(bool isLeft) const;

        /**
         * Get the last object the hand touched
         * @param isLeft Which hand
         * @return The reference, or nullptr if not touching
         */
        RE::TESObjectREFR* GetContactObject(bool isLeft) const;

        /**
         * Update/clear the tracked contact object from the non-physics proximity
         * push (or any external contact source).
         */
        void SetContactObject(bool isLeft, RE::TESObjectREFR* refr);
        void ClearContactObject(bool isLeft);
        void SetContactBodyId(bool isLeft, std::uint32_t bodyId);
        void ClearContactBodyId(bool isLeft);

        /**
         * Trigger haptic feedback for collision
         */
        void TriggerCollisionHaptics(bool isLeft, float intensity, float duration = 0.1f);

        /** Flush queued haptics — call from main thread (e.g. post-physics hook) */
        void FlushPendingHaptics();

        /** Get current bhkWorld (for external WorldWriteLock) */
        void* GetCurrentBhkWorld();

        /** Push loose clutter that a moving HELD object is ramming into, using the same proven
         *  proximity mechanism the hand uses (SetLinearVelocity + wake). The keyframed-body physics
         *  push never engaged reliably, so a held object drives this directly each frame.
         *  @param center   world centre of the held object
         *  @param velocity world velocity of the held object (game units/s)
         *  @param radius   query radius around the held object
         *  @param ignore   the held object itself (never push it) */
        void PushObjectsToward(const RE::NiPoint3& center, const RE::NiPoint3& velocity,
                               float radius, RE::TESObjectREFR* ignore);

    private:
        HandCollision() = default;
        ~HandCollision() = default;
        HandCollision(const HandCollision&) = delete;
        HandCollision& operator=(const HandCollision&) = delete;

        // =====================================================================
        // WORLD ACCESS
        // =====================================================================

        /**
         * Get the current physics world from the player's cell
         */
        void* GetCurrentHknpWorld();

        // =====================================================================
        // COLLISION HANDLING
        // =====================================================================

        /**
         * Check for nearby objects using proximity queries and push touched ones
         */
        void CheckProximityCollisions(const RE::NiPoint3& handPos,
                                       const RE::NiPoint3& handVel,
                                       bool isLeft,
                                       float deltaTime);

        /**
         * Apply push force to an object that collided with hand
         */
        void ApplyPushForce(RE::TESObjectREFR* refr, const RE::NiPoint3& handPos,
                            const RE::NiPoint3& handVel, float deltaTime);

        // =====================================================================
        // STATE
        // =====================================================================

        bool _initialized = false;

        // Last known hand positions/velocities
        RE::NiPoint3 _leftHandPos;
        RE::NiPoint3 _rightHandPos;
        RE::NiPoint3 _leftHandVel;
        RE::NiPoint3 _rightHandVel;
        RE::NiMatrix3 _leftHandRot;     // for back-of-hand collision detection
        RE::NiMatrix3 _rightHandRot;

        // Previous positions used by the non-physics swept fallback so fast
        // hands still push objects before the visible mesh can pass through.
        RE::NiPoint3 _prevLeftSweepPos;
        RE::NiPoint3 _prevRightSweepPos;
        bool _hasPrevSweepPos[2] = { false, false };

        // Contact tracking (from proximity push or external sources)
        // Uses ObjectRefHandle for safe reference lookup (prevents dangling pointers)
        RE::ObjectRefHandle _leftContact;
        RE::ObjectRefHandle _rightContact;
        std::atomic<std::uint32_t> _leftContactBodyId{0x7FFFFFFF};
        std::atomic<std::uint32_t> _rightContactBodyId{0x7FFFFFFF};
        std::atomic<bool> _pendingLeftContactClear{false};
        std::atomic<bool> _pendingRightContactClear{false};

        // Pending haptics (set from physics thread, flushed on main thread)
        std::atomic<int> _pendingLeftHaptic{0};
        std::atomic<int> _pendingRightHaptic{0};
    };

    // =========================================================================
    // FUNCTION POINTERS FOR COLLISION
    // =========================================================================

    namespace CollisionFunctions
    {
        // SetLinearVelocity for pushing objects (same as in Grab.cpp)
        using SetLinearVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
        inline REL::Relocation<SetLinearVelocity_t> SetLinearVelocity{ REL::Offset(0x1e08050) };

        // GetLinearVelocity — used by the proximity push to preserve the object's existing
        // gravity-derived Z velocity so a just-released object doesn't get its fall overridden.
        using GetLinearVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
        inline REL::Relocation<GetLinearVelocity_t> GetLinearVelocity{ REL::Offset(0x1e07fc0) };

        // Helper to validate collision object before physics operations
        inline bool IsCollisionObjectValid(RE::bhkNPCollisionObject* obj)
        {
            if (!obj) return false;
            if (!obj->spSystem || reinterpret_cast<uintptr_t>(obj->spSystem.get()) == 0xFFFFFFFFFFFFFFFF) {
                return false;
            }
            return true;
        }
    }
}
