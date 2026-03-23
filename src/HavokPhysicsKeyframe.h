#pragma once

/**
 * HavokPhysicsKeyframe.h - Direct hknp body manipulation for keyframed grabbing
 * 
 * Based on Rolling Rock's working F4VR code that directly manipulates hknpBody 
 * and hknpMotion to make objects KEYFRAMED without going through the crashing
 * bhkNPCollisionObject::SetMotionType() function.
 * 
 * The key insight is that in Havok New Physics (hknp*), we can:
 * 1. Access the raw hknpBody via world->accessBody()
 * 2. Set IS_KEYFRAMED flag on the body
 * 3. Set motion to infinite mass via setInfiniteInertiaAndMass()
 * 4. Set motion properties ID to KEYFRAMED
 * 5. Signal the world about changes
 * 
 * This bypasses the wrapper functions that crash.
 */

#include "RE/Fallout.h"

namespace heisenberg
{
    // =========================================================================
    // RAW HAVOK STRUCTURES (minimal definitions for direct access)
    // These are used with reinterpret_cast to access raw memory layouts
    // =========================================================================

    // hknpBodyId - just a uint32 wrapper
    struct hknpBodyId {
        std::uint32_t value;
        
        hknpBodyId() : value(0xFFFFFFFF) {}
        explicit hknpBodyId(std::uint32_t v) : value(v) {}
        bool isValid() const { return value != 0xFFFFFFFF; }
    };

    // hknpMotionId - just a uint32 wrapper
    struct hknpMotionId {
        std::uint32_t value;
        
        hknpMotionId() : value(0xFFFFFFFF) {}
        explicit hknpMotionId(std::uint32_t v) : value(v) {}
        bool isValid() const { return value != 0xFFFFFFFF; }
    };

    // hknpMotionPropertiesId - identifies motion behavior preset
    struct hknpMotionPropertiesIdRaw {
        std::uint16_t value;
        
        static constexpr std::uint16_t STATIC_ID = 0;
        static constexpr std::uint16_t KEYFRAMED_ID = 1;  // From hknpMotionPropertiesId::KEYFRAMED
        static constexpr std::uint16_t DYNAMIC_ID = 2;
    };

    // hknpIslandId - for deactivation manager
    struct hknpIslandId {
        std::uint16_t value;
        explicit hknpIslandId(std::uint16_t v) : value(v) {}
    };

    // hkFlags wrapper
    template<typename Enum, typename Storage>
    struct hkFlags {
        Storage storage;
        
        void orWith(Storage bits) { storage |= bits; }
        void andWith(Storage bits) { storage &= bits; }
        void clear(Storage bits) { storage &= ~bits; }
        bool get(Storage bit) const { return (storage & bit) != 0; }
    };

    // hknpBody flags (from Havok headers)
    namespace hknpBodyFlags {
        constexpr std::uint16_t TEMP_REBUILD_COLLISION_CACHES = 0x0002;  // From Rolling Rock working code
        constexpr std::uint16_t IS_ACTIVE = 0x0004;
        constexpr std::uint16_t IS_KEYFRAMED = 0x0010;  // From Rolling Rock working code
        constexpr std::uint16_t IS_DYNAMIC = 0x0000;    // Default for movable objects
    }

    // Minimal hknpBody structure (enough to manipulate flags and access motion)
    // This is a PARTIAL definition - only the fields we need
    // Real structure is much larger, we access by offset
    struct hknpBodyRaw {
        // We access fields by offset, not by structure layout
        // Key offsets from Rolling Rock:
        //   m_flags: accessible via accessBody()
        //   m_motionId: accessible via accessBody()
        //   m_collisionFilterInfo: accessible via accessBody()
        //   m_indexIntoActiveListOrDeactivatedIslandId: accessible via accessBody()
    };

    // Minimal hknpMotion structure
    // Key fields we need:
    //   m_motionPropertiesId: offset for setting KEYFRAMED
    //   setInfiniteInertiaAndMass(): function to call
    struct hknpMotionRaw {
        // Accessed by offset
    };

    // =========================================================================
    // FUNCTION SIGNATURES AND RELOCATIONS
    // =========================================================================

    // bhkNPCollisionObject::AccessWorld() - returns writable hknpWorld*
    // VR offset: 0x1e07fa0 - Uses collision object directly!
    using AccessWorld_t = void*(*)(RE::bhkNPCollisionObject* collisionObj);
    inline REL::Relocation<AccessWorld_t> bhkNPCollisionObject_AccessWorld{ REL::Offset(0x1e07fa0) };

    // bhkNPCollisionObject::AccessBody() - returns writable hknpBody&
    // VR offset: 0x1e07e30 - Uses collision object directly!
    using AccessBody_t = void*(*)(RE::bhkNPCollisionObject* collisionObj);
    inline REL::Relocation<AccessBody_t> bhkNPCollisionObject_AccessBody{ REL::Offset(0x1e07e30) };

    // hknpBSWorld::accessMotion(hknpMotionId) - returns hknpMotion&
    // VR offset: 0x1df5ba0 - Status 4 (Verified)
    using AccessMotion_t = void*(*)(void* hknpWorld, std::uint32_t motionId);
    inline REL::Relocation<AccessMotion_t> hknpBSWorld_accessMotion{ REL::Offset(0x1df5ba0) };

    // hknpBSWorld::applyHardKeyFrame(hknpBodyId, hkVector4f&, hkQuaternionf&, float)
    // VR offset: 0x1df5930 - Status 4 (Verified)
    // This moves a keyframed body by setting velocity to reach target
    using ApplyHardKeyFrame_t = void(*)(void* hknpWorld, std::uint32_t bodyId, 
                                         RE::NiPoint4& position, RE::NiPoint4& rotation, float deltaTime);
    inline REL::Relocation<ApplyHardKeyFrame_t> hknpBSWorld_applyHardKeyFrame{ REL::Offset(0x1df5930) };

    // hkSignal2<hknpWorld*, hknpBodyId>::fire - signal body changed
    // VR offset: 0x153ee00 - Status 2
    using SignalBodyChanged_t = void(*)(void* signal, void* world, std::uint32_t bodyId);
    inline REL::Relocation<SignalBodyChanged_t> hkSignal_BodyChanged_fire{ REL::Offset(0x153ee00) };

    // hknpWorld::setBodyMotionProperties(hknpBodyId, hknpMotionPropertiesId)
    // VR offset: 0x153b2f0 - Status 4 (Verified)
    using SetBodyMotionProperties_t = void(*)(void* hknpWorld, std::uint32_t bodyId, std::uint16_t motionPropertiesId);
    inline REL::Relocation<SetBodyMotionProperties_t> hknpWorld_setBodyMotionProperties{ REL::Offset(0x153b2f0) };

    // hknpDeactivationManager::resetDeactivationFrameCounter(hknpMotionId)
    // Need to find this offset
    using ResetDeactivationCounter_t = void(*)(void* deactivationMgr, std::uint32_t motionId);
    inline REL::Relocation<ResetDeactivationCounter_t> hknpDeactivationManager_resetDeactivationFrameCounter{ REL::Offset(0x17d8850) };  // Estimated

    // hknpDeactivationManager::markIslandForActivation(hknpIslandId)
    using MarkIslandForActivation_t = void(*)(void* deactivationMgr, std::uint16_t islandId);
    inline REL::Relocation<MarkIslandForActivation_t> hknpDeactivationManager_markIslandForActivation{ REL::Offset(0x17d8620) };  // Estimated

    // =========================================================================
    // STRUCTURE OFFSETS (from Rolling Rock and CommonLibF4)
    // =========================================================================
    namespace Offsets {
        // hknpWorld offsets (from CommonLibF4 hknpWorld.h)
        constexpr std::ptrdiff_t hknpWorld_bodyManager = 0x010;       // hknpBodyManager
        constexpr std::ptrdiff_t hknpWorld_deactivationManager = 0x4A0;  // hknpDeactivationManager*
        constexpr std::ptrdiff_t hknpWorld_bodyChangedSignal = 0x538; // hkSignal - from Rolling Rock!
        
        // hknpBody offsets (estimated from Havok 2014 headers)
        constexpr std::ptrdiff_t hknpBody_flags = 0x00;              // hkFlags<Flags, uint16>
        constexpr std::ptrdiff_t hknpBody_motionId = 0x04;           // hknpMotionId
        constexpr std::ptrdiff_t hknpBody_collisionFilterInfo = 0x08; // uint32
        constexpr std::ptrdiff_t hknpBody_indexOrIslandId = 0x0E;    // uint16
        
        // hknpMotion offsets
        constexpr std::ptrdiff_t hknpMotion_motionPropertiesId = 0x02; // hknpMotionPropertiesId (uint16)
        // setInfiniteInertiaAndMass() sets values at:
        constexpr std::ptrdiff_t hknpMotion_inverseMass = 0x30;       // float = 0
        constexpr std::ptrdiff_t hknpMotion_inverseInertia = 0x40;    // hkVector4f = {0,0,0,0}
    }

    // =========================================================================
    // HELPER CLASSES FOR KEYFRAMED PHYSICS
    // =========================================================================

    /**
     * KeyframedPhysicsHelper - Helper class for making objects keyframed
     * 
     * Usage:
     *   KeyframedPhysicsHelper helper;
     *   if (helper.Initialize(collisionObject)) {
     *       helper.SetupKeyframed();  // On grab start
     *       // During grab:
     *       helper.MoveToTransform(targetTransform, deltaTime);
     *       // On grab end:
     *       helper.RestoreDynamic();
     *   }
     */
    class KeyframedPhysicsHelper
    {
    public:
        KeyframedPhysicsHelper() = default;
        
        /**
         * Initialize from a bhkNPCollisionObject
         * Gets the hknp structures needed for direct manipulation
         */
        bool Initialize(RE::bhkNPCollisionObject* collisionObj);
        
        /**
         * Set up the body for keyframed mode
         * This makes the object follow the controller exactly without physics fighting
         */
        bool SetupKeyframed();
        
        /**
         * Restore the body to dynamic mode
         * Called when releasing the grab
         */
        bool RestoreDynamic();
        
        /**
         * Move the keyframed body to target transform
         * Uses applyHardKeyFrame which sets velocity to reach target
         */
        bool MoveToTransform(const RE::NiPoint3& targetPos, const RE::NiMatrix3& targetRot, float deltaTime);
        
        /**
         * Keep the body from being deactivated by the physics system
         */
        void KeepActive();
        
        /**
         * Check if helper was successfully initialized
         */
        bool IsValid() const { return _world != nullptr && _bodyId.isValid(); }
        
        // Getters for debugging
        void* GetWorld() const { return _world; }
        std::uint32_t GetBodyId() const { return _bodyId.value; }
        
    private:
        // Cached pointers/values - use collision object for wrapper access
        RE::bhkNPCollisionObject* _collisionObj = nullptr;  // Store for wrapper functions
        void* _world = nullptr;               // hknpWorld*
        void* _deactivationMgr = nullptr;     // hknpDeactivationManager*
        hknpBodyId _bodyId;                   // Body ID in the physics system (for reference - may not be set)
        
        // Saved state for restoration
        std::uint16_t _savedFlags = 0;
        std::uint16_t _savedMotionPropertiesId = 0;
        
        // Signal body changed to world
        void SignalBodyChanged();
        
        // Access body via wrapper function
        void* AccessBody();
    };

}  // namespace heisenberg
