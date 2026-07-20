#pragma once

/**
 * HandCollision - Provides physics-based collision between VR hands and world objects
 * 
 * Following Skyrim HIGGS pattern:
 * - Creates actual physics bodies for each hand
 * - Bodies are KEYFRAMED (position controlled, not simulated)
 * - Collision callbacks trigger haptics and push objects
 * - Bodies persist as long as player is in game
 */

#include <RE/Fallout.h>
#include <array>
#include "GrabConstraint.h"  // For hknpBodyCinfo, ConstraintFunctions

namespace heisenberg
{
    // Havok world scale constant
    constexpr float HAVOK_WORLD_SCALE_COLLISION = 0.0142875f;

    /**
     * PhysicsHandBody - Represents a physics body for a VR hand
     * Similar to Skyrim HIGGS's handBody member
     */
    struct PhysicsHandBody
    {
        std::uint32_t bodyId = 0x7FFFFFFF;      // hknpBodyId - invalid by default
        void* shape = nullptr;                   // hknpConvexShape*
        void* hknpWorld = nullptr;               // The world this body exists in
        void* bhkWorld = nullptr;                // bhkWorld wrapper (for collision callbacks)
        void* physicsSystem = nullptr;           // bhkPhysicsSystem* - Bethesda wrapper for cleanup
        void* collisionObject = nullptr;         // bhkNPCollisionObject* wrapper for AddToWorld / motion type
        void* alignedSystemDataMem = nullptr;    // hknpPhysicsSystemData allocation
        void* alignedBodyCinfoMem = nullptr;     // hknpBodyCinfo allocation
        void* alignedMaterialMem = nullptr;      // hknpMaterialDescriptor allocation
        void* alignedPhysicsSystemMem = nullptr; // bhkPhysicsSystem allocation
        void* alignedCollisionObjMem = nullptr;  // bhkNPCollisionObject allocation
        void* bethesdaBody = nullptr;            // BethesdaPhysicsBody* for the palm (base).
        void* bethesdaBody_wallA = nullptr;      // Thumb-side raised wall (cup rim).
        void* bethesdaBody_wallB = nullptr;      // Pinky-side raised wall (cup rim).
                                                 // All owned by the PhysicsHandBody;
                                                 // Destroy/Invalidate must Destroy()+delete each.
        bool valid = false;
        bool collisionEnabled = false;
        bool proxyRegistered = false;
        bool playerPairFilterApplied = false;
        std::uint32_t collisionFilterInfo = 0;  // Stored for bit 14 toggle
        double createdTime = 0.0;
        RE::NiPoint4 prevHavokPos{0,0,0,0};  // Previous frame position for velocity computation
        
        bool IsValid() const { return valid && bodyId != 0x7FFFFFFF && bodyId != 0; }
        
        void Invalidate() 
        { 
            bodyId = 0x7FFFFFFF; 
            shape = nullptr; 
            hknpWorld = nullptr;
            bhkWorld = nullptr;
            physicsSystem = nullptr;
            collisionObject = nullptr;
            alignedSystemDataMem = nullptr;
            alignedBodyCinfoMem = nullptr;
            alignedMaterialMem = nullptr;
            alignedPhysicsSystemMem = nullptr;
            alignedCollisionObjMem = nullptr;
            bethesdaBody = nullptr;  // caller is responsible for releasing first (HandCollision::ReleaseBody)
            bethesdaBody_wallA = nullptr;
            bethesdaBody_wallB = nullptr;
            valid = false;
            collisionEnabled = false;
            proxyRegistered = false;
            playerPairFilterApplied = false;
            collisionFilterInfo = 0;
            createdTime = 0.0;
            prevHavokPos = {0, 0, 0, 0};  // Forces "first frame" branch in MoveHandBody after re-creation.
        }
    };

    /**
     * HandCollision - Manages physics-based hand collision with world objects
     *
     * This creates actual hknpBody objects in the physics world for each hand,
     * allowing natural collision detection and response through Havok's engine.
     *
     * Thread safety:
     *   - Main thread: Update, Initialize, Shutdown
     *   - _leftHandBody/_rightHandBody: protected by _handBodyMutex
     *   - _leftContact/_rightContact: ObjectRefHandle (safe reference), main-thread only
     *   - GetContactObject/IsInContact: main-thread only (no cross-thread callers)
     */
    /**
     * Physics step listener — implements bhkIWorldStepListener vtable.
     * Registered with bhkWorld::AddPhysicsStepListener to get callbacks
     * at exact physics step timing (same as HIGGS Skyrim's SimulatePlayerSpace).
     *
     * Vtable layout (from Ghidra bhkWorld::Update @ 0x1df7320):
     *   +0x00: destructor
     *   +0x08: (unused)
     *   +0x10: BeforeWholePhysicsUpdate  ← hand body movement here
     *   +0x18: BeforeAnyPhysicsStep (with params)
     *   +0x20: BeforeAnyPhysicsStep (no params)
     *   +0x28: BetweenCollideAndSolve (with params)
     *   +0x30: BetweenCollideAndSolve (no params)
     *   +0x38: AfterAnyPhysicsStep (with params)
     *   +0x40: AfterAnyPhysicsStep (no params)
     *   +0x48: (unused)
     *   +0x50: AfterWholePhysicsUpdate
     */
    class HandCollisionStepListener
    {
    public:
        // C++ vtable layout: first 8 bytes of object = pointer to vtable array.
        // The engine does: rax = *(this); call [rax + offset]
        void** vtablePtr = nullptr;        // First member — the engine reads this as vtable pointer
        void* vtableStorage[11] = {};      // Actual function pointers
        bool registered = false;

        void Init();
        void Register(void* bhkWorld);
        void Unregister();

        // Called from vtable dispatch at BeforeWholePhysicsUpdate timing
        static void OnBeforeWholePhysicsUpdate(HandCollisionStepListener* self);
        static void OnAfterWholePhysicsUpdate(HandCollisionStepListener* self);
        static void StubNoOp(void*) {}
        static void StubNoOp2(void*, void*) {}
    };

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
         * Create hand bodies if they don't exist yet.
         * MUST be called from HookEndUpdate (no physics locks held).
         * Body creation requires world write access which deadlocks from post-physics.
         */
        void CreateBodiesIfNeeded(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos);

        /**
         * Apply player pair filter to hand bodies.
         * MUST be called from EndUpdate (no physics locks held).
         * Disables collision between hand bodies and player capsule.
         */
        void ApplyPlayerPairFilterIfNeeded();

        /**
         * Update hand collision for both hands
         * Only updates positions of existing bodies (safe from post-physics hook).
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
         * Update/clear the tracked contact object from the active physics contact
         * listener or from the non-physics proximity fallback.
         */
        void SetContactObject(bool isLeft, RE::TESObjectREFR* refr);
        void ClearContactObject(bool isLeft);
        void SetContactBodyId(bool isLeft, std::uint32_t bodyId);
        void ClearContactBodyId(bool isLeft);

        /**
         * Get the physics body for a hand
         * @param isLeft Which hand
         * @return The hand body struct
         */
        const PhysicsHandBody& GetHandBody(bool isLeft) const;

        /**
         * Check if hand bodies have been created
         */
        bool HasHandBodies() const { return _leftHandBody.IsValid() || _rightHandBody.IsValid(); }

        /**
         * Trigger haptic feedback for collision
         */
        void TriggerCollisionHaptics(bool isLeft, float intensity, float duration = 0.1f);

        /** Flush queued haptics — call from main thread (e.g. post-physics hook) */
        void FlushPendingHaptics();

        /** Get current bhkWorld (for external WorldWriteLock) */
        void* GetCurrentBhkWorld();

        /** Register/unregister the physics step listener with the world */
        void RegisterStepListener();
        void UnregisterStepListener();

        /** Called from step listener — runs inside bhkWorld::Update, before physics step */
        void OnPrePhysicsStep();

        /** Push loose clutter that a moving HELD object is ramming into, using the same proven
         *  proximity mechanism the hand uses (SetLinearVelocity + wake). The keyframed-body physics
         *  push never engaged reliably, so a held object drives this directly each frame.
         *  @param center   world centre of the held object
         *  @param velocity world velocity of the held object (game units/s)
         *  @param radius   query radius around the held object
         *  @param ignore   the held object itself (never push it) */
        void PushObjectsToward(const RE::NiPoint3& center, const RE::NiPoint3& velocity,
                               float radius, RE::TESObjectREFR* ignore);

        HandCollisionStepListener _stepListener;

    private:
        HandCollision() = default;
        ~HandCollision() = default;
        HandCollision(const HandCollision&) = delete;
        HandCollision& operator=(const HandCollision&) = delete;

        // =====================================================================
        // HAND BODY MANAGEMENT
        // =====================================================================

        /**
         * Create a physics body for a hand in the physics world
         * @param handBody Output structure for the created body
         * @param hknpWorld The hknp physics world
         * @param bhkWorld The bhkWorld wrapper
         * @param position Initial position
         * @param isLeft Which hand
         * @return true if successful
         */
        bool CreatePhysicsHandBody(PhysicsHandBody& handBody, void* hknpWorld, void* bhkWorld,
                                    const RE::NiPoint3& position, bool isLeft);

        /**
         * Destroy a hand physics body
         */
        void DestroyPhysicsHandBody(PhysicsHandBody& handBody);

        /**
         * Update a hand body's position using keyframe
         */
        void UpdateHandBodyPosition(PhysicsHandBody& handBody,
                                     const RE::NiPoint3& position,
                                     const RE::NiMatrix3& rotation,
                                     float deltaTime);

        /**
         * Enable/disable collision for a hand body
         */
        void SetHandCollisionEnabled(PhysicsHandBody& handBody, bool enabled);

        /**
         * Only create physics hand bodies once the world is stable after
         * loading. If requested, this also attempts to resolve the player body
         * for deferred pair filtering, but lack of a player body is no longer a
         * hard blocker for hand-body creation.
         */
        bool CanCreatePhysicsHandBodiesNow(std::uint32_t* playerBodyId = nullptr) const;

        /**
         * Resolve the player proxy + player body pair-filter prerequisites.
         */
        bool ResolveReadyPlayerCollisionState(std::uint32_t& playerBodyId) const;

        /**
         * Disable collision between a physics hand body and the player
         * controller. This must run outside the world write lock to avoid the
         * load/save deadlock path.
         */
        bool TryDisableCollisionWithPlayer(PhysicsHandBody& handBody, std::uint32_t playerBodyId);

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
         * Check for nearby objects using physics queries (fallback)
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

        // Thread safety: protects hand body creation/destruction/update
        mutable std::mutex _handBodyMutex;

        // Physics bodies for each hand
        PhysicsHandBody _leftHandBody;
        PhysicsHandBody _rightHandBody;

        // Task #11: per-hand finger-tip segment bodies (thumb..pinky = 0..4).
        // Gated by g_config.enableFingerSegmentColliders; created lazily on the
        // first frame the skeleton's distal finger bones resolve successfully.
        std::array<PhysicsHandBody, 5> _leftFingerSegments{};
        std::array<PhysicsHandBody, 5> _rightFingerSegments{};
        bool _fingerSegmentsReady[2] = { false, false };  // [0]=left, [1]=right

        // Helpers for finger-segment lifecycle
        void UpdateFingerSegments(bool isLeft, float deltaTime);
        void DestroyFingerSegments(bool isLeft);
        
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
        
        // Contact tracking (from collision callbacks or proximity)
        // Uses ObjectRefHandle for safe reference lookup (prevents dangling pointers)
        RE::ObjectRefHandle _leftContact;
        RE::ObjectRefHandle _rightContact;
        std::atomic<std::uint32_t> _leftContactBodyId{0x7FFFFFFF};
        std::atomic<std::uint32_t> _rightContactBodyId{0x7FFFFFFF};
        std::atomic<bool> _pendingLeftContactClear{false};
        std::atomic<bool> _pendingRightContactClear{false};
        
        // Player space compensation (FIX #3)
        RE::NiPoint3 _prevPlayerPos;
        RE::NiPoint3 _currentPlayerPos;
        bool _playerSpaceInitialized = false;

        // Pending haptics (set from physics thread, flushed on main thread)
        std::atomic<int> _pendingLeftHaptic{0};
        std::atomic<int> _pendingRightHaptic{0};

        // Timing
        double _lastUpdateTime = 0.0;
        double _bodyCreationBlockedUntil = 0.0;
        double _playerBodyRetryBlockedUntil = 0.0;

        // Hand↔player and hand↔hand collision pair-filter disable state. Without this the
        // keyframed (infinite-mass) hand bodies shove the player proxy on contact (can't walk
        // through your own arms) and the two hands fight each other when they meet, and the
        // solver impulses can crash. Re-applied periodically: the player body GetPlayerBodyId
        // returns can be a transient/ragdoll body before the walking character proxy resolves.
        bool _handPlayerFilterApplied[2] = { false, false };  // [0]=left, [1]=right
        bool _handHandFilterApplied = false;
        int  _handFilterFrames = 0;

        // =====================================================================
        // COLLISION LAYER CONSTANTS (following Skyrim HIGGS)
        // =====================================================================
        
        // Custom collision layer for hands - must not collide with player but collide with world
        // In Skyrim: layer 56 with player group
        // In F4VR: We'll use a similar pattern
        static constexpr std::uint32_t HAND_COLLISION_LAYER = 56;
        static constexpr std::uint32_t COLLISION_GROUP_OFFSET = 16;  // Collision group in bits 16-31
    };

    // =========================================================================
    // FUNCTION POINTERS FOR COLLISION
    // =========================================================================
    
    namespace CollisionFunctions
    {
        // hknpBSWorld::setBodyTransform(hknpBodyId bodyId, const hkTransformf& transform)
        // VR offset: 0x1df55f0 - Status 4 (Verified)
        using SetBodyTransform_t = void(__fastcall*)(void* hknpBSWorld, std::uint32_t bodyId, 
                                                      const void* transform);
        inline REL::Relocation<SetBodyTransform_t> SetBodyTransform{ REL::Offset(0x1df55f0) };

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
