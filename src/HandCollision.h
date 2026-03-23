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
        bool valid = false;
        bool collisionEnabled = false;
        double createdTime = 0.0;
        
        bool IsValid() const { return valid && bodyId != 0x7FFFFFFF && bodyId != 0; }
        
        void Invalidate() 
        { 
            bodyId = 0x7FFFFFFF; 
            shape = nullptr; 
            hknpWorld = nullptr;
            bhkWorld = nullptr;
            physicsSystem = nullptr;
            valid = false;
            collisionEnabled = false;
            createdTime = 0.0;
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
            spdlog::debug("[HandCollision] Cleared contact pointers (save/load cleanup)");
        }

        /**
         * Update hand collision for both hands
         * Creates bodies if needed, updates positions
         * @param leftHandPos Left hand world position
         * @param rightHandPos Right hand world position
         * @param leftHandVel Left hand velocity
         * @param rightHandVel Right hand velocity
         * @param deltaTime Frame delta time
         */
        void Update(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos,
                    const RE::NiPoint3& leftHandVel, const RE::NiPoint3& rightHandVel,
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
                                     const RE::NiPoint3& velocity,
                                     float deltaTime);

        /**
         * Enable/disable collision for a hand body
         */
        void SetHandCollisionEnabled(PhysicsHandBody& handBody, bool enabled);

        // =====================================================================
        // WORLD ACCESS
        // =====================================================================

        /**
         * Get the current physics world from the player's cell
         */
        void* GetCurrentHknpWorld();
        void* GetCurrentBhkWorld();

        // =====================================================================
        // COLLISION HANDLING
        // =====================================================================

        /**
         * Check for nearby objects using physics queries (fallback)
         */
        void CheckProximityCollisions(const RE::NiPoint3& handPos, 
                                       const RE::NiPoint3& handVel,
                                       bool isLeft);

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
        
        // Last known hand positions/velocities
        RE::NiPoint3 _leftHandPos;
        RE::NiPoint3 _rightHandPos;
        RE::NiPoint3 _leftHandVel;
        RE::NiPoint3 _rightHandVel;
        
        // Contact tracking (from collision callbacks or proximity)
        // Uses ObjectRefHandle for safe reference lookup (prevents dangling pointers)
        RE::ObjectRefHandle _leftContact;
        RE::ObjectRefHandle _rightContact;
        
        // Timing
        double _lastUpdateTime = 0.0;

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
