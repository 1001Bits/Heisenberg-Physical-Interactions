#pragma once

namespace heisenberg
{
    /**
     * Physics utilities for Heisenberg.
     * Wraps Fallout 4's Havok 2014+ (hknp*) physics system.
     *
     * Thread safety:
     *   - All physics queries (CastRay, CastSphere, GetObjectsInRadius) require
     *     WorldReadLock or must be called from main thread with implicit lock.
     *   - WorldReadLock/WorldWriteLock: RAII wrappers, use _locked bool to avoid TOCTOU.
     *   - g_playerCollisionModified, g_savedPlayerCollisionFilterInfo, g_playerBodyId:
     *     std::atomic (module-level statics in Physics.cpp).
     *   - g_sphereShape: module-level static, main-thread only (lazy-init on first query).
     */
    namespace Physics
    {
        // Get the physics world from the player's cell
        RE::bhkWorld* GetWorld();

        // Get the physics world from a specific cell
        RE::bhkWorld* GetWorldFromCell(RE::TESObjectCELL* cell);

        // Raycast from a point in a direction
        struct RaycastResult
        {
            bool hit = false;
            RE::NiPoint3 hitPoint;
            RE::NiPoint3 hitNormal;
            float hitFraction = 1.0f;
            RE::TESObjectREFR* hitRefr = nullptr;
            RE::NiAVObject* hitNode = nullptr;
            RE::hknpBodyId bodyId;
        };

        RaycastResult CastRay(
            const RE::NiPoint3& origin,
            const RE::NiPoint3& direction,
            float maxDistance,
            RE::TESObjectREFR* ignoreRefr = nullptr
        );

        // Sphere cast (linear cast with radius)
        RaycastResult CastSphere(
            const RE::NiPoint3& origin,
            const RE::NiPoint3& direction,
            float radius,
            float maxDistance,
            RE::TESObjectREFR* ignoreRefr = nullptr
        );

        // Get all objects within radius of a point
        std::vector<RE::TESObjectREFR*> GetObjectsInRadius(
            const RE::NiPoint3& center,
            float radius,
            RE::TESObjectREFR* ignoreRefr = nullptr
        );

        // Check if an object has physics (rigid body)
        bool HasPhysics(RE::TESObjectREFR* refr);

        // Resolve a Havok body ID to a reference for immediate use.
        RE::TESObjectREFR* GetRefrFromBodyId(void* bhkWorld, std::uint32_t bodyId);

        // Check if an object can be grabbed
        bool IsGrabbable(RE::TESObjectREFR* refr);
        
        // Clear the blacklist cache (call on cell change)
        void ClearBlacklistCache();

        // Cleanup cached physics shapes (call on shutdown)
        void CleanupCachedShapes();

        // Apply impulse to an object
        void ApplyImpulse(RE::TESObjectREFR* refr, const RE::NiPoint3& impulse, const RE::NiPoint3& point);

        // Set object velocity
        void SetVelocity(RE::TESObjectREFR* refr, const RE::NiPoint3& linear, const RE::NiPoint3& angular);

        // ==================================================================================
        // WORLD LOCKING HELPERS
        // ==================================================================================
        // Havok physics requires proper locking for thread safety.
        // From the Havok tutorials (mmmovania):
        //   - Always call lock() before modifying physics state
        //   - Always call unlock() after modifications are complete
        //   - For read-only operations, use read locks
        //
        // The pattern is:
        //   g_pWorld->lock();
        //   gSelectedActor->setMotionType(hkpMotion::MOTION_FIXED);  // On pick
        //   g_pWorld->unlock();
        //
        //   g_pWorld->lock();  
        //   gSelectedActor->setTransform(trans);  // During drag
        //   g_pWorld->unlock();
        //
        //   g_pWorld->lock();
        //   gSelectedActor->setMotionType(hkpMotion::MOTION_DYNAMIC);  // On release
        //   g_pWorld->unlock();
        // ==================================================================================

        /**
         * Get the hknpBSWorld pointer from a bhkWorld
         * @param bhkWorld The Bethesda physics world wrapper
         * @return Raw pointer to hknpBSWorld (internal Havok world)
         */
        void* GetHknpWorldFromBhk(RE::bhkWorld* bhkWorld);

        /**
         * RAII-style world lock for read access
         * Use this for physics queries (raycasts, body inspection)
         */
        class WorldReadLock
        {
        public:
            WorldReadLock(RE::bhkWorld* bhkWorld);
            ~WorldReadLock();

            WorldReadLock(const WorldReadLock&) = delete;
            WorldReadLock& operator=(const WorldReadLock&) = delete;

            bool IsLocked() const { return _locked; }

        private:
            RE::BSReadWriteLock* _lock = nullptr;
            bool _locked = false;  // Track if we actually acquired the lock (TOCTOU fix)
        };

        /**
         * RAII-style world lock for write access
         * Use this for physics modifications (body creation, constraint management, motion type changes)
         * 
         * Havok tutorial pattern (from mmmovania):
         *   {
         *       WorldWriteLock lock(bhkWorld);  // Automatically locks
         *       CreateBodyInternal(...);
         *       // ... modifications ...
         *   }  // Automatically unlocks when goes out of scope
         */
        class WorldWriteLock
        {
        public:
            WorldWriteLock(RE::bhkWorld* bhkWorld);
            ~WorldWriteLock();

            WorldWriteLock(const WorldWriteLock&) = delete;
            WorldWriteLock& operator=(const WorldWriteLock&) = delete;

            bool IsLocked() const { return _locked; }

        private:
            RE::BSReadWriteLock* _lock = nullptr;
            bool _locked = false;  // Track if we actually acquired the lock (TOCTOU fix)
        };

        // Legacy manual lock functions (prefer RAII versions above)
        bool LockWorldForRead(RE::bhkWorld* bhkWorld);
        void UnlockWorldRead(RE::bhkWorld* bhkWorld);
        bool LockWorldForWrite(RE::bhkWorld* bhkWorld);
        void UnlockWorldWrite(RE::bhkWorld* bhkWorld);
        
        // ==================================================================================
        // HIGGS-STYLE PAIR COLLISION FILTERING
        // ==================================================================================
        // Disable/enable collision between two specific bodies.
        // Used to prevent player↔grabbed-object collisions without affecting world collisions.
        // This is the HIGGS pattern: grabbed objects still collide with walls, just not player.
        // ==================================================================================
        
        /**
         * Disable collision between two specific bodies.
         * Uses hknpPairCollisionFilter::disableCollisionsBetween internally.
         * @param hknpWorld The Havok physics world (hknpBSWorld pointer)
         * @param bodyIdA First body ID
         * @param bodyIdB Second body ID
         */
        void DisableCollisionsBetween(void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB);
        
        /**
         * Get the player character controller body ID.
         * Used for disabling player↔grabbed object collisions.
         * @return Player body ID, or 0x7FFFFFFF if not found
         */
        std::uint32_t GetPlayerBodyId();

        /**
         * Drop the cached player body id so the next GetPlayerBodyId() re-walks the
         * resolution chain (character-proxy phantom first). Used to recover from an
         * early/transient resolution that latched onto the wrong (e.g. ragdoll) body.
         */
        void InvalidatePlayerBodyId();

        /**
         * Force the player character proxy to drop its cached "support" (ground) body.
         * The proxy refcounts that pointer (bhkCharacterController::SetSupportBody);
         * freeing one of our keyframed collider bodies while the proxy still caches it
         * leaves a dangling pointer -> next-frame char-proxy crash. Call at the START of
         * any ROCK collider-body teardown (before releasing our own refs). SEH-guarded;
         * no-op if no player/charController or the proxy holds nothing.
         */
        void ClearPlayerProxySupportBody();

        /**
         * Held-object mass / inertia (1:1 with ROCK GrabConstraint). Mass comes from the
         * hknpMotion packed inverse-mass (bfloat16). NormalizeHeldObjectInertia clamps the
         * inverse-inertia ratio to maxRatio (ROCK default 10) so elongated objects don't
         * spin wildly under the grab motor, saving the original packed inertia into saved[3]
         * (returns true only if it modified anything). RestoreHeldObjectInertia puts it back
         * on release. Both rebuild the solver's derived mass props.
         */
        float GetHeldObjectMass(RE::bhkNPCollisionObject* obj);
        bool GetHeldObjectVelocity(RE::bhkNPCollisionObject* obj, RE::NiPoint3& outLinearHavok, RE::NiPoint3& outAngularHavok);
        bool NormalizeHeldObjectInertia(RE::bhkNPCollisionObject* obj, float maxRatio, std::int16_t savedOut[3]);
        void RestoreHeldObjectInertia(RE::bhkNPCollisionObject* obj, const std::int16_t saved[3]);

        /**
         * ROCK-style collision suppression for held/grabbed bodies. Reads the body's
         * collisionFilterInfo field and toggles bit 14 (kSuppressionNoCollideBit = 0x4000)
         * — the engine's collision filter rejects pairs involving any body that has this
         * bit, including against the player character proxy. More reliable than per-pair
         * disableCollisionsBetween (no player-body resolution required).
         *
         * @param hknpWorld           World containing the body
         * @param bodyId              Body to modify
         * @param enable              true = OR the bit in (suppress collision), false = clear it
         * @param outOriginalFilter   Optional: pre-modification filter value (for restore)
         * @return true on successful read+write; false on invalid body or AV
         */
        bool SetBodyNoCollideBit(void* hknpWorld, std::uint32_t bodyId, bool enable, std::uint32_t* outOriginalFilter = nullptr);

        /** Direct write of the body's collisionFilterInfo (SEH-wrapped). */
        bool TryWriteBodyFilterInfo(void* hknpWorld, std::uint32_t bodyId, std::uint32_t filterInfo);

        /** Direct read of the body's collisionFilterInfo (SEH-wrapped). */
        bool TryReadBodyFilterInfo(void* hknpWorld, std::uint32_t bodyId, std::uint32_t& outFilterInfo);

        /**
         * Enable or disable player collision with world objects.
         * Used to let hands reach through player hitbox to touch activators.
         * @param enabled True to enable normal collision, false to disable
         * @return True if successful
         */
        bool SetPlayerCollisionEnabled(bool enabled);
        
        /**
         * Check if player collision is currently disabled
         * @return True if player collision has been disabled via SetPlayerCollisionEnabled(false)
         */
        bool IsPlayerCollisionDisabled();

        /**
         * Disable collision between two specific bodies using hknpPairCollisionFilter.
         * Both bodies keep their normal collisions with everything else.
         * Returns false if the underlying engine call threw (caller should treat
         * the pair as still active — e.g. by destroying the body so it doesn't
         * shove the player capsule).
         */
        bool DisableCollisionBetween(void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB);

        /**
         * Re-enable collision between two bodies (removes the pair filter entry).
         */
        void EnableCollisionBetween(void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB);
    }
}
