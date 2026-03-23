#include "HavokPhysicsKeyframe.h"
#include <spdlog/spdlog.h>

namespace heisenberg
{
    bool KeyframedPhysicsHelper::Initialize(RE::bhkNPCollisionObject* collisionObj)
    {
        if (!collisionObj) {
            spdlog::warn("[KeyframedPhysics] Initialize: null collision object");
            return false;
        }
        
        // Store collision object for use with wrapper functions
        _collisionObj = collisionObj;
        
        // Get writable world using bhkNPCollisionObject::AccessWorld wrapper
        _world = bhkNPCollisionObject_AccessWorld(collisionObj);
        
        if (!_world) {
            spdlog::warn("[KeyframedPhysics] Initialize: AccessWorld returned null");
            return false;
        }
        
        spdlog::trace("[KeyframedPhysics] Got world: {:p}", _world);
        
        // Get deactivation manager from world (offset 0x4A0)
        _deactivationMgr = *reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(_world) + Offsets::hknpWorld_deactivationManager);
        
        spdlog::info("[KeyframedPhysics] Initialized successfully: world={:p} deactivationMgr={:p}",
                     _world, _deactivationMgr ? _deactivationMgr : nullptr);
        
        return true;
    }

    void* KeyframedPhysicsHelper::AccessBody()
    {
        if (!_collisionObj)
            return nullptr;
            
        // Use bhkNPCollisionObject::AccessBody wrapper - returns hknpBody&
        return bhkNPCollisionObject_AccessBody(_collisionObj);
    }

    void KeyframedPhysicsHelper::SignalBodyChanged()
    {
        // Skip signaling for now - the wrapper functions should handle this internally
        // If we find we need it, we can add it back with proper body ID retrieval
    }

    bool KeyframedPhysicsHelper::SetupKeyframed()
    {
        if (!IsValid()) {
            spdlog::warn("[KeyframedPhysics] SetupKeyframed: not initialized");
            return false;
        }
        
        // Access the body through the wrapper
        void* body = AccessBody();
        if (!body) {
            spdlog::warn("[KeyframedPhysics] SetupKeyframed: AccessBody returned null");
            return false;
        }
        
        spdlog::trace("[KeyframedPhysics] Accessing body at {:p}", body);
        
        // Now we have direct access to hknpBody
        // Set flags for keyframed mode (from Rolling Rock code):
        //   bodyRW.m_flags.orWith(hknpBody::TEMP_REBUILD_COLLISION_CACHES);  // 0x0002
        //   bodyRW.m_flags.orWith(hknpBody::IS_ACTIVE);                      // 0x0004
        //   bodyRW.m_flags.orWith(hknpBody::IS_KEYFRAMED);                   // 0x0010
        
        // Read current flags
        std::uint16_t* flags = reinterpret_cast<std::uint16_t*>(
            reinterpret_cast<std::uintptr_t>(body) + Offsets::hknpBody_flags);
        
        _savedFlags = *flags;  // Save for restoration
        
        // Set flags for keyframed
        constexpr std::uint16_t TEMP_REBUILD_COLLISION_CACHES = 0x0002;
        constexpr std::uint16_t IS_ACTIVE = 0x0004;
        constexpr std::uint16_t IS_KEYFRAMED = 0x0010;
        
        *flags |= (TEMP_REBUILD_COLLISION_CACHES | IS_ACTIVE | IS_KEYFRAMED);
        
        spdlog::info("[KeyframedPhysics] Set body to KEYFRAMED (flags: {:04X} -> {:04X})", 
                    _savedFlags, *flags);
        
        // Get motion ID from body
        std::uint32_t motionId = *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<std::uintptr_t>(body) + Offsets::hknpBody_motionId);
        
        // Access motion and modify it
        void* motion = hknpBSWorld_accessMotion(_world, motionId);
        if (motion) {
            // Save and set motion properties ID
            std::uint16_t* motionPropsId = reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uintptr_t>(motion) + Offsets::hknpMotion_motionPropertiesId);
            
            _savedMotionPropertiesId = *motionPropsId;
            *motionPropsId = hknpMotionPropertiesIdRaw::KEYFRAMED_ID;
            
            // Set infinite inertia and mass (so the body can't be pushed)
            float* inverseMass = reinterpret_cast<float*>(
                reinterpret_cast<std::uintptr_t>(motion) + Offsets::hknpMotion_inverseMass);
            *inverseMass = 0.0f;
            
            // Set inverse inertia to zero (4 floats)
            float* inverseInertia = reinterpret_cast<float*>(
                reinterpret_cast<std::uintptr_t>(motion) + Offsets::hknpMotion_inverseInertia);
            inverseInertia[0] = inverseInertia[1] = inverseInertia[2] = inverseInertia[3] = 0.0f;
            
            spdlog::info("[KeyframedPhysics] Motion modified: motionId={} propsId: {} -> {}", 
                        motionId, _savedMotionPropertiesId, hknpMotionPropertiesIdRaw::KEYFRAMED_ID);
        }
        
        return true;
    }

    bool KeyframedPhysicsHelper::RestoreDynamic()
    {
        if (!IsValid()) {
            spdlog::warn("[KeyframedPhysics] RestoreDynamic: not initialized");
            return false;
        }
        
        // Access the body through the wrapper
        void* body = AccessBody();
        if (!body) {
            spdlog::warn("[KeyframedPhysics] RestoreDynamic: AccessBody returned null");
            return false;
        }
        
        // Restore flags
        std::uint16_t* flags = reinterpret_cast<std::uint16_t*>(
            reinterpret_cast<std::uintptr_t>(body) + Offsets::hknpBody_flags);
        
        // Clear the keyframed flag
        constexpr std::uint16_t IS_KEYFRAMED = 0x0010;
        *flags &= ~IS_KEYFRAMED;
        
        spdlog::info("[KeyframedPhysics] Restored body flags (removed KEYFRAMED)");
        
        // Get motion ID from body
        std::uint32_t motionId = *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<std::uintptr_t>(body) + Offsets::hknpBody_motionId);
        
        // Access motion and restore it
        void* motion = hknpBSWorld_accessMotion(_world, motionId);
        if (motion && _savedMotionPropertiesId != 0) {
            std::uint16_t* motionPropsId = reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uintptr_t>(motion) + Offsets::hknpMotion_motionPropertiesId);
            
            *motionPropsId = _savedMotionPropertiesId;
            
            spdlog::info("[KeyframedPhysics] Motion restored: motionId={} propsId: {} -> {}", 
                        motionId, hknpMotionPropertiesIdRaw::KEYFRAMED_ID, _savedMotionPropertiesId);
        }
        
        // Note: We're not restoring mass/inertia here
        // The game should handle that when the motion properties are restored
        
        return true;
    }

    bool KeyframedPhysicsHelper::MoveToTransform(const RE::NiPoint3& targetPos, 
                                                   const RE::NiMatrix3& targetRot, 
                                                   float deltaTime)
    {
        if (!IsValid()) {
            return false;
        }
        
        // For now, we're using the virtual spring system which calls ApplyForce
        // This function would be used if we switch to hardKeyFrame approach
        // 
        // TODO: Implement if needed using hknpBSWorld_applyHardKeyFrame
        
        return true;
    }

    void KeyframedPhysicsHelper::KeepActive()
    {
        // The body should stay active while we're manipulating it
        // If deactivation becomes an issue, we can add deactivation manager calls here
    }

}  // namespace heisenberg
