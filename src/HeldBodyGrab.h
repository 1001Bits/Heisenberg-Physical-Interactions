#pragma once

/**
 * HeldBodyGrab - HIGGS Skyrim-style constraint-based grabbing for Fallout 4 VR
 * 
 * This implements the "HeldBody" grab system from HIGGS Skyrim:
 * - Creates KEYFRAMED physics hand bodies that follow VR controllers
 * - Grabbed objects stay DYNAMIC with motor constraints pulling them toward hand
 * - Hand visual follows the object's actual physics position (not controller directly)
 * - Provides physics-based feel with collision response
 * 
 * Key differences from current KEYFRAMED system:
 * - Current: Object set to KEYFRAMED, position updated directly via transform
 * - HeldBody: Object stays DYNAMIC, constraint motors pull it toward hand body
 * 
 * Based on HIGGS Skyrim hand.cpp State::HeldBody implementation.
 */

#include <RE/Fallout.h>
#include "GrabConstraint.h"

namespace heisenberg
{
    enum class HeldBodyDriveMode
    {
        None = 0,
        Spring = 1,
        Custom6DOF = 2,
        NativeConstraint = 3
    };

    // Forward declarations
    struct GrabState;
    
    /**
     * HeldBodyHandPhysics - Manages a physics body for one VR hand
     * 
     * Creates a KEYFRAMED box body that follows the controller position.
     * Used as the anchor point for grab constraints.
     */
    struct HeldBodyHandPhysics
    {
        std::uint32_t bodyId = 0x7FFFFFFF;  // hknpBodyId
        void* shape = nullptr;               // hknpConvexShape*
        void* hknpWorld = nullptr;           // Current hknpWorld*
        void* bhkWorld = nullptr;            // Current bhkWorld*
        void* physicsSystem = nullptr;       // bhkPhysicsSystem* (Bethesda wrapper)
        void* hknpPhysicsSystem = nullptr;   // Native Havok system
        void* collisionObject = nullptr;     // bhkNPCollisionObject* (CRITICAL for proper physics!)
        
        // Aligned memory (Havok requires 16-byte alignment)
        void* alignedSystemDataMem = nullptr;
        void* alignedPhysicsSystemMem = nullptr;
        void* alignedHknpSystemMem = nullptr;
        void* alignedBodyCinfoMem = nullptr;
        void* alignedMaterialMem = nullptr;
        void* alignedCollisionObjMem = nullptr;  // bhkNPCollisionObject allocation
        
        // State
        bool valid = false;
        double createdTime = 0.0;
        RE::NiPoint3 prevPosition{};
        RE::NiMatrix3 prevRotation{};
        
        bool IsValid() const { return valid && bodyId != 0x7FFFFFFF && hknpWorld; }
        void Invalidate();
    };

    /**
     * HeldBodyGrabConstraint - Tracks the active HeldBody backend between hand and object.
     * This can be a true constraint or a velocity-driven dynamic spring.
     */
    struct HeldBodyGrabConstraint
    {
        hknpConstraintId constraintId;        // Constraint handle
        std::uint32_t handBodyId = 0x7FFFFFFF;
        std::uint32_t objectBodyId = 0x7FFFFFFF;
        void* hknpWorld = nullptr;
        HeldBodyDriveMode driveMode = HeldBodyDriveMode::None;
        bool active = false;
        
        // Motor pointers (for runtime parameter adjustment)
        hkpPositionConstraintMotor* angularMotor = nullptr;
        hkpPositionConstraintMotor* linearMotor = nullptr;
        
        // Constraint data (for transform updates)
        void* constraintData = nullptr;  // GrabConstraintData*

        // Pointer to the grabbed object's bhkNPCollisionObject (kept so we can
        // verify / restore motion state at EndGrab without re-resolving it).
        void* objectCollisionObject = nullptr;
        // Did we force a DYNAMIC motion-type override at StartGrab? Used to
        // decide whether to log a restore on release.
        bool forcedObjectDynamic = false;

        // Real object mass (kg) sampled at StartGrab, used for the motor force cap
        // (ROCK parity — replaces the old hardcoded 5.0 estimate). 0 = unknown.
        float cachedObjectMass = 0.0f;
        // Saved packed inverse-inertia for restore at EndGrab (ROCK inertia normalize).
        std::int16_t savedInertia[3] = { 0, 0, 0 };
        bool inertiaModified = false;

        // Object's collision filter info captured at StartGrab, restored at EndGrab so a
        // dropped object can't be left non-collidable (= falls through floors/tables).
        std::uint32_t objectOriginalFilter = 0;
        bool objectFilterSaved = false;

        // Previous frame's resolved hand position (game units) for hand-velocity-referenced
        // residual velocity damping (kills orbital "swirl" without damping hand-tracking).
        RE::NiPoint3 lastHandPos{ 0.0f, 0.0f, 0.0f };
        bool hasLastHandPos = false;

        bool IsSpringMode() const { return driveMode == HeldBodyDriveMode::Spring; }
        bool UsesConstraint() const
        {
            return driveMode == HeldBodyDriveMode::Custom6DOF || driveMode == HeldBodyDriveMode::NativeConstraint;
        }
        bool IsValid() const
        {
            return active && handBodyId != 0x7FFFFFFF && objectBodyId != 0x7FFFFFFF && hknpWorld;
        }
        void Invalidate();
    };

    /**
     * HeldBodyGrabManager - Manages HIGGS-style dynamic HeldBody grabbing.
     */
    class HeldBodyGrabManager
    {
    public:
        static HeldBodyGrabManager& GetSingleton()
        {
            static HeldBodyGrabManager instance;
            return instance;
        }

        /**
         * Initialize the HeldBody grab system
         * Creates physics hand bodies when physics world is ready
         */
        bool Initialize();

        /**
         * Shutdown and cleanup
         */
        void Shutdown();

        /**
         * Start a HeldBody grab on an object
         * 
         * @param state GrabState to populate
         * @param isLeft Which hand
         * @param handPos Hand world position
         * @param handRot Hand world rotation
         * @param refr Object to grab
         * @param node Object's visual node
         * @param collisionObject Object's physics collision object
         * @return true if grab started successfully
         */
        bool StartGrab(GrabState& state, bool isLeft, 
                       const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                       RE::TESObjectREFR* refr, RE::NiAVObject* node,
                       RE::bhkNPCollisionObject* collisionObject);

        /**
         * Update a HeldBody grab each frame
         * 
         * Updates hand body position and constraint motor parameters.
         * The object's visual position is handled by physics simulation.
         * 
         * @param state GrabState
         * @param isLeft Which hand
         * @param handPos Hand world position
         * @param handRot Hand world rotation
         * @param deltaTime Frame delta time
         */
        void UpdateGrab(GrabState& state, bool isLeft,
                        const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                        float deltaTime);

        /**
         * End a HeldBody grab
         * 
         * Destroys the constraint, optionally applies throw velocity.
         * 
         * @param state GrabState
         * @param isLeft Which hand
         * @param throwVelocity Optional throw velocity to apply (nullptr = drop)
         */
        void EndGrab(GrabState& state, bool isLeft, const RE::NiPoint3* throwVelocity);

        /**
         * Check if a HeldBody grab is active for a hand
         */
        bool IsGrabActive(bool isLeft) const;

        /**
         * Get the hand physics body for a hand
         */
        const HeldBodyHandPhysics& GetHandBody(bool isLeft) const;

    private:
        HeldBodyGrabManager() = default;
        ~HeldBodyGrabManager() = default;
        HeldBodyGrabManager(const HeldBodyGrabManager&) = delete;
        HeldBodyGrabManager& operator=(const HeldBodyGrabManager&) = delete;

        // Hand body management - two approaches for testing
        // Full chain: Creates hknpPhysicsSystem manually, then bhkNPCollisionObject
        bool CreateHandBody(HeldBodyHandPhysics& handBody, bool isLeft,
                           const RE::NiPoint3& position, void* hknpWorld, void* bhkWorld);
        
        // Simple approach: Mirrors HIGGS Skyrim more closely
        // Uses bhkPhysicsSystem::CreateInstance to handle hknpPhysicsSystem internally
        bool CreateHandBodySimple(HeldBodyHandPhysics& handBody, bool isLeft,
                                  const RE::NiPoint3& position, void* hknpWorld, void* bhkWorld);
        
        void DestroyHandBody(HeldBodyHandPhysics& handBody);
        void UpdateHandBodyPosition(HeldBodyHandPhysics& handBody, 
                                    const RE::NiPoint3& position, 
                                    const RE::NiMatrix3& rotation,
                                    float deltaTime);

        // Constraint management
        bool CreateGrabConstraint(GrabState& state, bool isLeft,
                      std::uint32_t objectBodyId,
                      const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot);
        void DestroyGrabConstraint(GrabState& state);
        void UpdateConstraintMotors(GrabState& state, float deltaTime);

        // World access
        void* GetCurrentHknpWorld();
        void* GetCurrentBhkWorld();

        // Hand physics bodies (persist across grabs)
        HeldBodyHandPhysics _leftHandBody;
        HeldBodyHandPhysics _rightHandBody;

        // Active grab constraints
        HeldBodyGrabConstraint _leftConstraint;
        HeldBodyGrabConstraint _rightConstraint;

        bool _initialized = false;

        // Player-space tracking (for locomotion compensation)
        RE::NiPoint3 _prevPlayerPos{};
        bool _playerPosInitialized = false;

        // Auto-drop distance tracking (rolling average)
        static constexpr int MAX_DISTANCE_SAMPLES = 10;
        float _leftDistSamples[MAX_DISTANCE_SAMPLES] = {};
        float _rightDistSamples[MAX_DISTANCE_SAMPLES] = {};
        int _leftDistIdx = 0;
        int _rightDistIdx = 0;
    };

}  // namespace heisenberg
