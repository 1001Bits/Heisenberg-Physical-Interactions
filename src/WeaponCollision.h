#pragma once

/**
 * WeaponCollision - Physics collision bodies for drawn weapons
 *
 * Based on Skyrim HIGGS weapon collision system (hand.cpp:720-896).
 * When a melee weapon is drawn, creates a KEYFRAMED hknp body shaped
 * like an elongated box that follows the weapon node transform each frame.
 * Allows weapons to physically push world objects on contact.
 *
 * Key differences from Skyrim HIGGS:
 * - Havok 2014 (hknp) instead of Havok 2012 (hkp)
 * - Uses hknpWorld::createBody + ApplyHardKeyFrame instead of bhkRigidBody wrapper
 * - Simplified box shape instead of NiCloningProcess shape extraction
 * - Weapon node found via PlayerNodes struct (not VRMeleeData)
 */

#include <RE/Fallout.h>
#include <mutex>

namespace heisenberg
{
    class WeaponCollision
    {
    public:
        static WeaponCollision& GetSingleton()
        {
            static WeaponCollision instance;
            return instance;
        }

        /**
         * Initialize the weapon collision system.
         * Call once at game start (from OnGameLoad).
         */
        bool Initialize();

        /**
         * Shutdown and cleanup.
         */
        void Shutdown();

        /**
         * Main update - call each frame from UpdateHands.
         * Creates/destroys/moves weapon collision body based on weapon state.
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime);

        /**
         * Clear state on save/load to prevent dangling references.
         */
        void ClearState();

        /**
         * Temporarily disable weapon collision (e.g., during physics grab).
         * Collision body persists but bit 14 is set to disable contacts.
         */
        void SetEnabled(bool enabled) { _externalEnabled = enabled; }
        bool IsEnabled() const { return _externalEnabled; }

        /**
         * Check if a weapon collision body currently exists.
         */
        bool HasWeaponBody() const;

        /**
         * Get the body ID (for collision listener integration).
         */
        std::uint32_t GetBodyId() const;

    private:
        WeaponCollision() = default;
        ~WeaponCollision() = default;
        WeaponCollision(const WeaponCollision&) = delete;
        WeaponCollision& operator=(const WeaponCollision&) = delete;

        // =====================================================================
        // WEAPON BODY STATE
        // =====================================================================

        struct WeaponBody
        {
            std::uint32_t bodyId = 0x7FFFFFFF;
            void* shape = nullptr;
            void* hknpWorld = nullptr;
            bool valid = false;
            bool collisionEnabled = false;
            std::uint32_t collisionFilterInfo = 0;
            double createdTime = 0.0;

            bool IsValid() const { return valid && bodyId != 0x7FFFFFFF && bodyId != 0; }

            void Invalidate()
            {
                bodyId = 0x7FFFFFFF;
                shape = nullptr;
                hknpWorld = nullptr;
                valid = false;
                collisionEnabled = false;
                collisionFilterInfo = 0;
                createdTime = 0.0;
            }
        };

        // =====================================================================
        // CORE OPERATIONS
        // =====================================================================

        bool CreateWeaponBody(void* hknpWorld, const RE::NiPoint3& position,
                              const RE::NiMatrix3& rotation);
        void DestroyWeaponBody();
        void MoveWeaponBody(const RE::NiPoint3& position, const RE::NiMatrix3& rotation,
                            float invDeltaTime);
        void UpdateCollisionState(bool shouldEnable);

        // =====================================================================
        // HELPERS
        // =====================================================================

        void* GetCurrentHknpWorld();
        bool IsWeaponDrawnAndMelee() const;

        /**
         * Get the weapon collision center position.
         * The weapon extends forward from the hand - collision body is centered
         * along the weapon's length axis.
         * @param handPos Wand/hand world position
         * @param handRot Wand/hand world rotation
         * @return Center position for the collision body in world space
         */
        RE::NiPoint3 ComputeWeaponCenter(const RE::NiPoint3& handPos,
                                          const RE::NiMatrix3& handRot) const;

        /**
         * Convert NiMatrix3 rotation to quaternion (x, y, z, w).
         * Uses Shepperd method for numerical stability.
         */
        static RE::NiPoint4 MatrixToQuaternion(const RE::NiMatrix3& m);

        // =====================================================================
        // STATE
        // =====================================================================

        bool _initialized = false;
        bool _externalEnabled = true;
        WeaponBody _weaponBody;
        mutable std::mutex _mutex;

        // Track state changes
        bool _lastWeaponDrawn = false;
        void* _lastWorld = nullptr;

        // Collision enable delay (seconds after creation)
        static constexpr double ENABLE_DELAY = 0.3;

        // Havok scale (game units to Havok units)
        static constexpr float HAVOK_SCALE = 0.0142875f;
        static constexpr std::uint32_t INVALID_BODY_ID = 0x7FFFFFFF;
    };
}
