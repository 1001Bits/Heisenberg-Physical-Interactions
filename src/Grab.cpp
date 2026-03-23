#include "Grab.h"
#include "Config.h"
#include "DropToHand.h"
#include "F4VROffsets.h"
#include "FRIKInterface.h"
#include "GrabConstraint.h"
#include "Hand.h"
#include "Heisenberg.h"
#include "Hooks.h"
#include "ItemOffsets.h"
#include "ItemPositionConfigMode.h"
#include "MenuChecker.h"
#include "Physics.h"
#include "PipboyInteraction.h"
#include "Utils.h"
#include "VirtualHolstersAPI.h"
#include "VRInput.h"
#include "SharedUtils.h"
#include "WandNodeHelper.h"
#include "f4vr/PlayerNodes.h"
#include "f4vr/F4VRUtils.h"
#include "RE/Bethesda/UI.h"
#include <cmath>
#include <cstring>
#include <format>
#include <sstream>

// =====================================================================
// bhkNPCollisionProxyObject - Proxy collision object structure
// Inherits from bhkNPCollisionObjectBase (NOT bhkNPCollisionObject!)
// The proxy stores a pointer to the actual bhkNPCollisionObject it wraps.
// =====================================================================
// Layout based on CommonLibF4:
//   NiObject:                0x00-0x0F (vtable, refcount, etc.)
//   NiCollisionObject:       0x10 = sceneObject*
//   bhkNPCollisionObjectBase: 0x18 = flags (uint16), padded to 0x20
//   bhkNPCollisionProxyObject: 0x20+ = our members
// 
// Constructor: bhkNPCollisionProxyObject(bhkNPCollisionObject& target, NiTransform& transform)
// This suggests the proxy stores a POINTER to the target at offset 0x20
// =====================================================================

// Helper to get the target collision object from a proxy by reading the pointer at offset 0x20
// Returns null if the pointer at that offset is null
inline RE::bhkNPCollisionObject* GetProxyTarget(RE::NiCollisionObject* proxyObj)
{
    if (!proxyObj) return nullptr;

    uintptr_t proxyAddr = reinterpret_cast<uintptr_t>(proxyObj);
    if (proxyAddr < 0x10000) return nullptr;

    __try {
        // The bhkNPCollisionProxyObject stores a pointer to the target.
        // Try known offsets in order (0x20, 0x28, 0x30).
        constexpr uintptr_t offsets[] = { 0x20, 0x28, 0x30 };
        for (auto off : offsets) {
            auto** targetPtr = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + off);
            auto* target = *targetPtr;
            if (target && reinterpret_cast<uintptr_t>(target) > 0x10000) {
                return target;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("GetProxyTarget: Access violation reading proxy at {:X}", proxyAddr);
        return nullptr;
    }

    return nullptr;
}

// Helper to check if a node has a proxy collision object and return both:
// - The node we should visually grab (the proxy's sceneObject or target's sceneObject)
// - The collision object for physics operations
// Returns true if this is a proxy, false if direct collision
inline bool CheckForProxyCollision(RE::NiAVObject* node, RE::NiAVObject*& outVisualNode, RE::bhkNPCollisionObject*& outCollision)
{
    outVisualNode = node;
    outCollision = nullptr;
    
    if (!node) return false;
    
    auto* collObj = node->collisionObject.get();
    if (!collObj) return false;
    
    auto* rtti = collObj->GetRTTI();
    if (!rtti || !rtti->GetName()) return false;
    
    const char* typeName = rtti->GetName();
    
    if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
        spdlog::debug("[PROXY] Node '{}' has proxy collision", node->name.c_str());
        
        // Get the target collision object
        RE::bhkNPCollisionObject* target = GetProxyTarget(collObj);
        if (!target) {
            spdlog::warn("[PROXY] Proxy has null target!");
            return false;
        }
        
        outCollision = target;
        
        // Check if target's sceneObject is different from the proxy's node
        RE::NiAVObject* targetSceneObj = target->sceneObject;
        if (targetSceneObj && targetSceneObj != node) {
            spdlog::debug("[PROXY] Target sceneObject '{}' differs from proxy node '{}'",
                         targetSceneObj->name.c_str(), node->name.c_str());
            // For proxy objects, we need to grab the target's scene object!
            outVisualNode = targetSceneObj;
        } else {
            spdlog::debug("[PROXY] Target sceneObject is same as proxy node or null");
        }
        
        return true;
    }
    
    // Not a proxy - direct collision
    if (std::strcmp(typeName, "bhkNPCollisionObject") == 0) {
        outCollision = reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
    }
    
    return false;
}

// Helper: Manually update world transform - DO NOT use external functions that might crash
inline void UpdateWorldTransformRecursive(RE::NiAVObject* node, const RE::NiTransform& parentWorld)
{
    if (!node) return;
    
    // Safety check: verify node pointer is in a reasonable range (not corrupted)
    uintptr_t nodeAddr = reinterpret_cast<uintptr_t>(node);
    if (nodeAddr < 0x10000) {  // Kernel space, corrupted sentinel value
        spdlog::error("UpdateWorldTransformRecursive: invalid node pointer {:X}", nodeAddr);
        return;
    }
    
    // Safety check: verify node has a valid vtable before calling virtual functions
    // This prevents crashes when node is corrupted or freed
    __try {
        // Always manually calculate using provided parent world
        // This avoids any issues with the node's parent pointer being NULL or invalid
        const auto& localTransform = node->local;
        const RE::NiPoint3 pos = parentWorld.rotate.Transpose() * (localTransform.translate * parentWorld.scale);
        node->world.translate = parentWorld.translate + pos;
        node->world.rotate = localTransform.rotate * parentWorld.rotate;
        node->world.scale = parentWorld.scale * localTransform.scale;
        
        // Recurse to children if this is a node
        // Use RTTI check instead of virtual call for safety
        auto* asNode = node->IsNode();
        if (asNode) {
            auto& children = asNode->GetRuntimeData().children;
            uint32_t childCount = children.size();
            // Sanity check: don't recurse if child count is unreasonable
            if (childCount > 1000) {
                spdlog::warn("UpdateWorldTransformRecursive: unreasonable child count {}, skipping recursion", childCount);
                return;
            }
            for (uint32_t i = 0; i < childCount; ++i) {
                auto* child = children[i].get();
                if (child) {
                    UpdateWorldTransformRecursive(child, node->world);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("UpdateWorldTransformRecursive: exception caught, node may be invalid");
    }
}

namespace
{
    // Havok world scale - 1 game unit = 0.0142875 Havok units (1/70)
    constexpr float HAVOK_WORLD_SCALE = 0.0142875f;
    
    // =====================================================================
    // HELPER: Get FRIK's Weapon node parent world transform
    // FRIK offsets are local transforms for the "Weapon" node in firstPersonSkeleton.
    // When we grab an object with a FRIK offset, we need to use the Weapon node's
    // parent transform instead of the wand transform.
    // 
    // The "Weapon" node follows the PRIMARY hand:
    // - Right-handed mode (default): Weapon is in RIGHT hand
    // - Left-handed mode: Weapon is in LEFT hand
    // 
    // For OFF-HAND grabs: We mirror the weapon parent position across the player's center.
    // This places the grabbed object in the equivalent off-hand position.
    // =====================================================================
    bool GetFRIKWeaponParentTransform(bool isLeft, RE::NiPoint3& outPos, RE::NiMatrix3& outRot)
    {
        auto* player = f4cf::f4vr::getPlayer();
        if (!player || !player->firstPersonSkeleton) {
            return false;
        }
        
        // Option 3: Use the FRIK skeleton hand node directly for the grabbing hand
        // FRIK offsets are designed for the Weapon node's parent, which is a skeleton hand bone.
        // The Weapon node is always attached to the PRIMARY hand (based on left-handed mode),
        // but for off-hand grabs, we need to use the corresponding skeleton hand node.
        //
        // Skeleton hand nodes: "LArm_Hand" (left) and "RArm_Hand" (right)
        // These are the bones that FRIK uses for hand tracking, so their rotation should
        // match what FRIK offset configurators expect.
        
        const char* handNodeName = isLeft ? "LArm_Hand" : "RArm_Hand";
        RE::NiAVObject* handNodeObj = heisenberg::Utils::FindNode(player->firstPersonSkeleton, handNodeName, 15);
        RE::NiNode* handNode = handNodeObj ? handNodeObj->IsNode() : nullptr;
        
        if (handNode) {
            // Found the appropriate skeleton hand node - use it directly
            outPos = handNode->world.translate;
            outRot = handNode->world.rotate;
            
            static int debugLogCount = 0;
            if (debugLogCount++ < 5) {
                spdlog::debug("[GRAB] Using FRIK skeleton hand node '{}' for {} hand: pos=({:.1f},{:.1f},{:.1f})",
                    handNodeName, isLeft ? "LEFT" : "RIGHT",
                    outPos.x, outPos.y, outPos.z);
            }
            return true;
        }
        
        // Fallback: If skeleton hand node not found, try the Weapon node for primary hand
        spdlog::warn("[GRAB] Skeleton hand node '{}' not found, falling back to Weapon node", handNodeName);
        
        RE::NiAVObject* weaponObj = heisenberg::Utils::FindNode(player->firstPersonSkeleton, "Weapon", 15);
        RE::NiNode* weaponNode = weaponObj ? weaponObj->IsNode() : nullptr;
        if (!weaponNode) {
            return false;
        }
        
        // Get the weapon node's parent
        RE::NiNode* parent = weaponNode->parent;
        if (!parent) {
            return false;
        }
        
        // Check if this is the correct hand
        bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
        bool isPrimaryHand = (isLeftHandedMode ? isLeft : !isLeft);
        
        if (isPrimaryHand) {
            // Primary hand matches the Weapon node's parent
            outPos = parent->world.translate;
            outRot = parent->world.rotate;
            return true;
        }
        
        // Off-hand fallback: return false to signal we couldn't find it
        // (mirroring was unreliable, better to use wand directly)
        spdlog::warn("[GRAB] Cannot get FRIK transform for off-hand - skeleton hand node not found");
        return false;
    }
    
    // =====================================================================
    // HELPER: Get grab mode from config
    // =====================================================================
    int GetEffectiveGrabMode()
    {
        return heisenberg::g_config.grabMode;
    }
    
    // =====================================================================
    // DIRECT HKNP ACCESS (Like Skyrim HIGGS but for Fallout 4's hknp API)
    // =====================================================================
    
    // bhkNPCollisionObject::AccessWorld() - returns writable hknpWorld*
    // VR offset: 0x1e07fa0
    using AccessWorld_t = void*(*)(RE::bhkNPCollisionObject* collisionObj);
    REL::Relocation<AccessWorld_t> AccessWorld{ REL::Offset(0x1e07fa0) };
    
    // bhkNPCollisionObject::AccessBody() - returns writable hknpBody&
    // VR offset: 0x1e07e30
    using AccessBody_t = void*(*)(RE::bhkNPCollisionObject* collisionObj);
    REL::Relocation<AccessBody_t> AccessBody{ REL::Offset(0x1e07e30) };
    
    // hknpBSWorld::applyHardKeyFrame(hknpBodyId, hkVector4f& pos, hkQuaternionf& rot, float invDeltaTime)
    // VR offset: 0x1df5930 - Status 4 (Verified)
    // This is the direct equivalent of hkpKeyFrameUtility_applyHardKeyFrame
    using ApplyHardKeyFrameDirect_t = void(*)(void* hknpWorld, std::uint32_t bodyId, 
                                               RE::NiPoint4& position, RE::NiPoint4& rotation, float invDeltaTime);
    REL::Relocation<ApplyHardKeyFrameDirect_t> ApplyHardKeyFrameDirect{ REL::Offset(0x1df5930) };
    
    // hknpBSWorld::setBodyTransform(hknpBodyId, hkTransformf&)
    // VR offset: 0x1df55f0 - Status 4 (Verified) - Fallback if velocity too high
    using SetBodyTransformDirect_t = void(*)(void* hknpWorld, std::uint32_t bodyId, RE::hkTransformf& transform);
    REL::Relocation<SetBodyTransformDirect_t> SetBodyTransformDirect{ REL::Offset(0x1df55f0) };
    
    // hknpDeactivationManager::markBodyForActivation(hknpBodyId)
    // VR offset: 0x17d8590 - Status 4 (Verified)
    using MarkBodyForActivation_t = void(*)(void* deactivationManager, std::uint32_t bodyId);
    REL::Relocation<MarkBodyForActivation_t> MarkBodyForActivation{ REL::Offset(0x17d8590) };
    
    // hknpDeactivationManager::resetDeactivationFrameCounter(hknpMotionId)
    // VR offset: 0x17d8850 - Keeps body from going to sleep
    using ResetDeactivationCounter_t = void(*)(void* deactivationMgr, std::uint32_t motionId);
    REL::Relocation<ResetDeactivationCounter_t> ResetDeactivationCounter{ REL::Offset(0x17d8850) };
    
    // Offsets for direct hknp structure access
    namespace HknpOffsets {
        constexpr std::ptrdiff_t hknpWorld_deactivationManager = 0x4A0;
        constexpr std::ptrdiff_t hknpBody_flags = 0x00;
        constexpr std::ptrdiff_t hknpBody_motionId = 0x04;
        constexpr std::ptrdiff_t hknpBody_bodyId = 0x02;  // uint16 serialAndIndex
    }
    
    // hknpBody flags
    constexpr std::uint16_t HKNP_BODY_IS_ACTIVE = 0x0004;
    
    // Helper: Convert NiMatrix3 to quaternion (xyzw format)
    void MatrixToQuaternion(const RE::NiMatrix3& m, float& x, float& y, float& z, float& w)
    {
        // Use Shepperd's method for robustness
        float trace = m.entry[0][0] + m.entry[1][1] + m.entry[2][2];
        
        if (trace > 0.0f) {
            float s = 0.5f / sqrtf(trace + 1.0f);
            w = 0.25f / s;
            x = (m.entry[2][1] - m.entry[1][2]) * s;
            y = (m.entry[0][2] - m.entry[2][0]) * s;
            z = (m.entry[1][0] - m.entry[0][1]) * s;
        } else if (m.entry[0][0] > m.entry[1][1] && m.entry[0][0] > m.entry[2][2]) {
            float s = 2.0f * sqrtf(1.0f + m.entry[0][0] - m.entry[1][1] - m.entry[2][2]);
            w = (m.entry[2][1] - m.entry[1][2]) / s;
            x = 0.25f * s;
            y = (m.entry[0][1] + m.entry[1][0]) / s;
            z = (m.entry[0][2] + m.entry[2][0]) / s;
        } else if (m.entry[1][1] > m.entry[2][2]) {
            float s = 2.0f * sqrtf(1.0f + m.entry[1][1] - m.entry[0][0] - m.entry[2][2]);
            w = (m.entry[0][2] - m.entry[2][0]) / s;
            x = (m.entry[0][1] + m.entry[1][0]) / s;
            y = 0.25f * s;
            z = (m.entry[1][2] + m.entry[2][1]) / s;
        } else {
            float s = 2.0f * sqrtf(1.0f + m.entry[2][2] - m.entry[0][0] - m.entry[1][1]);
            w = (m.entry[1][0] - m.entry[0][1]) / s;
            x = (m.entry[0][2] + m.entry[2][0]) / s;
            y = (m.entry[1][2] + m.entry[2][1]) / s;
            z = 0.25f * s;
        }
    }
    
    // =====================================================================
    // WRAPPER FUNCTIONS (for compatibility)
    // =====================================================================
    // bhkNPCollisionObject::ApplyHardKeyframe(hkTransformf&, float invDeltaTime)
    // VR offset: 0x1e086e0 - Status 4 (Verified)
    // This sets velocity on the body to move it toward the target transform
    using ApplyHardKeyframe_t = void(*)(RE::bhkNPCollisionObject*, RE::hkTransformf&, float);
    REL::Relocation<ApplyHardKeyframe_t> ApplyHardKeyframe{ REL::Offset(0x1e086e0) };
    
    // bhkNPCollisionObject::SetLinearVelocity(hkVector4f&)
    // VR offset: 0x1e08050 - Status 2
    using SetLinearVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
    REL::Relocation<SetLinearVelocity_t> SetLinearVelocity{ REL::Offset(0x1e08050) };
    
    // bhkNPCollisionObject::GetLinearVelocity(hkVector4f&)
    // VR offset: 0x1e07fc0 - Status 4 (Verified)
    using GetLinearVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
    REL::Relocation<GetLinearVelocity_t> GetLinearVelocity{ REL::Offset(0x1e07fc0) };
    
    // bhkNPCollisionObject::SetAngularVelocity(hkVector4f&)
    // VR offset: 0x1e08170 - Status 2 (from fo4_database.csv)
    using SetAngularVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
    REL::Relocation<SetAngularVelocity_t> SetAngularVelocity{ REL::Offset(0x1e08170) };
    
    // bhkNPCollisionObject::SetTransform(hkTransformf&)
    // VR offset: 0x1e08a70 - Status 4 (Verified)
    // This directly teleports the physics body to the target transform
    using SetTransform_t = bool(*)(RE::bhkNPCollisionObject*, RE::hkTransformf&);
    REL::Relocation<SetTransform_t> SetTransform{ REL::Offset(0x1e08a70) };
    
    // =====================================================================
    // WORLD-LOCKED WRAPPER FUNCTIONS
    // All physics modifications MUST be protected by world locks!
    // These wrappers handle locking automatically.
    // =====================================================================
    
    // Helper to get bhkWorld from a reference
    inline RE::bhkWorld* GetBhkWorldFromRefr(RE::TESObjectREFR* refr)
    {
        if (!refr) return nullptr;
        auto* cell = refr->GetParentCell();
        if (!cell) return nullptr;
        return cell->GetbhkWorld();
    }
    
    // Helper to validate collision object before physics operations
    // Returns true if the collision object appears valid and safe to use
    inline bool IsCollisionObjectValid(RE::bhkNPCollisionObject* obj)
    {
        if (!obj) return false;
        
        // Check if the spSystem pointer is valid (not null and not a sentinel value)
        // 0xFFFFFFFFFFFFFFFF is -1 which indicates an invalid/freed pointer
        if (!obj->spSystem || reinterpret_cast<uintptr_t>(obj->spSystem.get()) == 0xFFFFFFFFFFFFFFFF) {
            return false;
        }
        
        // Check if systemBodyIdx is valid (0x7FFFFFFF is sentinel for "no body")
        if (obj->systemBodyIdx == 0x7FFFFFFF || obj->systemBodyIdx == 0xFFFFFFFF) {
            return false;
        }
        
        return true;
    }
    
    // Locked version of SetLinearVelocity
    void SetLinearVelocityLocked(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        SetLinearVelocity(obj, vel);
    }
    
    // Locked version of GetLinearVelocity
    void GetLinearVelocityLocked(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldReadLock lock(bhkWorld);
        GetLinearVelocity(obj, vel);
    }
    
    // Locked version of SetAngularVelocity
    void SetAngularVelocityLocked(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        SetAngularVelocity(obj, vel);
    }
    
    // Locked version of ApplyHardKeyframe
    void ApplyHardKeyframeLocked(RE::bhkNPCollisionObject* obj, RE::hkTransformf& transform, float invDeltaTime, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        ApplyHardKeyframe(obj, transform, invDeltaTime);
    }
    
    // Locked version of SetTransform
    bool SetTransformLocked(RE::bhkNPCollisionObject* obj, RE::hkTransformf& transform, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return false;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        return SetTransform(obj, transform);
    }
    
    // Locked version of SetMotionType (member function)
    void SetMotionTypeLocked(RE::bhkNPCollisionObject* obj, RE::hknpMotionPropertiesId::Preset motion, RE::bhkWorld* bhkWorld)
    {
        if (!obj || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        obj->SetMotionType(motion);
    }
    
    // Locked version of bhkWorld_SetMotion
    void bhkWorld_SetMotionLocked(RE::NiAVObject* node, RE::hknpMotionPropertiesId::Preset motion, bool a3, bool a4, bool a5, RE::bhkWorld* bhkWorld)
    {
        if (!node || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        heisenberg::bhkWorld_SetMotion(node, motion, a3, a4, a5);
    }
    
    // Locked version of bhkUtilFunctions_SetLayer
    void bhkUtilFunctions_SetLayerLocked(RE::NiAVObject* node, std::uint32_t layer, RE::bhkWorld* bhkWorld)
    {
        if (!node || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        heisenberg::bhkUtilFunctions_SetLayer(node, layer);
    }
    
    // Locked version of bhkNPCollisionObject_AddToWorld
    void bhkNPCollisionObject_AddToWorldLocked(RE::bhkNPCollisionObject* obj, RE::bhkWorld* bhkWorld)
    {
        if (!obj || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        heisenberg::bhkNPCollisionObject_AddToWorld(obj, bhkWorld);
    }
    
    // Locked version of bhkWorld_RemoveObject
    void bhkWorld_RemoveObjectLocked(RE::NiAVObject* node, bool a2, bool a3, RE::bhkWorld* bhkWorld)
    {
        if (!node || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        heisenberg::bhkWorld_RemoveObject(node, a2, a3);
    }

    // =====================================================================
    // HELPER: Apply hard keyframe using wrapper function (safe approach)
    // =====================================================================
    bool ApplyHardKeyframeSafe(RE::bhkNPCollisionObject* collisionObj,
                                const RE::NiPoint3& targetPos,
                                const RE::NiMatrix3& targetRot,
                                float invDeltaTime)
    {
        if (!collisionObj)
            return false;
        
        // Build Havok transform
        RE::hkTransformf targetTransform;
        targetTransform.rotation = targetRot;
        targetTransform.translation = RE::NiPoint4(
            targetPos.x * HAVOK_WORLD_SCALE,
            targetPos.y * HAVOK_WORLD_SCALE,
            targetPos.z * HAVOK_WORLD_SCALE,
            0.0f
        );
        
        // Use the wrapper ApplyHardKeyframe function
        // This is bhkNPCollisionObject::ApplyHardKeyframe which handles internal details
        ApplyHardKeyframe(collisionObj, targetTransform, invDeltaTime);
        
        return true;
    }
    
    // =====================================================================
    // HELPER: Wake up a physics body from deactivated/sleeping state
    // Takes the physics system directly to ensure we use the same world
    // that bhkPhysicsSystem_AddToWorld uses
    // =====================================================================
    // DISABLED: markBodyForActivation is causing crashes - the offset or body ID retrieval
    // is not working correctly. The velocity we set on release should be sufficient to
    // wake the physics body.
    void WakeUpBodyFromPhysicsSystem(void* physicsSystem, RE::bhkNPCollisionObject* collisionObj)
    {
        if (!physicsSystem || !collisionObj)
            return;
        
        // Just log that we would wake up the body - actual wake-up disabled due to crashes
        spdlog::debug("[GRAB] WakeUpBody: Would wake body for physics system {:X} (DISABLED - using velocity instead)",
                     reinterpret_cast<uintptr_t>(physicsSystem));
        
        // The velocity set during EndGrab should be sufficient to wake the physics
        // Setting any non-zero velocity on a body typically wakes it from sleep
    }
    
    // Legacy version for compatibility
    void WakeUpBody(RE::bhkNPCollisionObject* collisionObj)
    {
        if (!collisionObj || !collisionObj->spSystem)
            return;
        WakeUpBodyFromPhysicsSystem(collisionObj->spSystem.get(), collisionObj);
    }
    
    // Fallback: Direct position set if velocity exceeds limits
    void SetPositionDirectFallback(RE::bhkNPCollisionObject* collisionObj,
                                    const RE::NiPoint3& targetPos,
                                    const RE::NiMatrix3& targetRot)
    {
        if (!collisionObj || !collisionObj->spSystem)
            return;
        
        void* world = AccessWorld(collisionObj);
        void* physicsSystem = collisionObj->spSystem.get();
        if (!world || !physicsSystem)
            return;
        
        // Get actual body ID using bhkPhysicsSystem::GetBodyId (output pointer)
        std::uint32_t systemIndex = collisionObj->systemBodyIdx;
        std::uint32_t bodyId = 0x7FFFFFFF;
        heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(physicsSystem, &bodyId, systemIndex);
        
        // Fallback if invalid
        if (bodyId == 0x7FFFFFFF)
            bodyId = systemIndex;
        
        RE::hkTransformf transform;
        transform.rotation = targetRot;
        transform.translation = RE::NiPoint4(
            targetPos.x * HAVOK_WORLD_SCALE,
            targetPos.y * HAVOK_WORLD_SCALE,
            targetPos.z * HAVOK_WORLD_SCALE,
            0.0f
        );
        
        SetBodyTransformDirect(world, bodyId, transform);
    }
    
    // =========================================================================
    // VISIBILITY CLEARING HELPER
    // =========================================================================
    // Culling flags mask - DISABLED since DynamicNode reparenting fixes the issue
    // - bit 24 (0x1000000) - seen in flag byte causing issues
    // - bit 32 (0x100000000) - difference between 210 and 211, related to physics state
    // - bit 47 (0x80000000000) - difference between 2004 and 200C, main visibility cull bit
    // constexpr uint64_t kCullingMask = 0x1000000ULL | 0x100000000ULL | 0x80000000000ULL;
    
    // Recursively clear culling flags on a node and all its children
    void ClearCullingFlagsRecursive(RE::NiAVObject* node)
    {
        if (!node) return;
        
        // Clear culling-related flags - be aggressive and clear ALL culling-related bits
        // - bit 24 (0x1000000) - seen in flag byte causing issues
        // - bit 32 (0x100000000) - related to physics state
        // - bit 35 (0x800000000) - engine keeps setting this
        // - bit 36 (0x1000000000) - set on dynamic/moving objects, causes invisibility on grab
        // - bit 39 (0x8000000000) - seen on child nodes (PlasticPumpkin:0)
        // - bit 40 (0x10000000000) - set after throw/catch
        // - bit 43 (0x80000000000) - difference between 2004 and 200C
        // - bit 47 (0x800000000000) - main visibility cull bit
        constexpr uint64_t kCullingMask = 0x1000000ULL | 0x100000000ULL | 0x800000000ULL | 
                                          0x1000000000ULL | 0x8000000000ULL | 0x10000000000ULL | 
                                          0x80000000000ULL | 0x800000000000ULL;
        uint64_t currentFlags = node->GetFlags();
        uint64_t cleanFlags = currentFlags & ~kCullingMask;
        if (currentFlags != cleanFlags)
        {
            node->flags.flags = cleanFlags;
            spdlog::debug("[GRAB-VIS] Cleared culling flags on '{}': {:X} -> {:X}", 
                         node->name.c_str(), currentFlags, cleanFlags);
        }
        
        // Also use the engine's cull methods
        node->SetAppCulled(false);
        node->CullNode(false);
        
        // If this is a NiNode, recursively process children
        RE::NiNode* asNode = node->IsNode();
        if (asNode && asNode->children.size() > 0)
        {
            for (auto& childPtr : asNode->children)
            {
                if (childPtr.get())
                {
                    ClearCullingFlagsRecursive(childPtr.get());
                }
            }
        }
    }
    
}  // Close anonymous namespace

// =========================================================================
// WEAPON CHECK HELPER - in heisenberg namespace for external access
// =========================================================================
namespace heisenberg
{
    // Allow grabbing with Unarmed equipped, but block grabbing with real weapons.
    // "Unarmed" is a special weapon type that represents bare fists.
    // We let the user grab items even while Unarmed is "drawn" because:
    // 1. It's the natural state when no weapon is equipped
    // 2. We can override the fist pose via FRIK during grabbing
    //
    // APPROACH: Access equipData directly from player memory structure
    // This is safer than calling GetEquippedWeapon function which goes through 
    // AIProcess::GetEquippedItem and can crash when currentProcess is null.
    // We add extensive null checks since middleProcess etc can be null during loading.
    //
    // Returns true if a REAL weapon (gun, melee, etc.) is drawn - block grab
    // Returns false if no weapon or only Unarmed is drawn - allow grab
    bool HasRealWeaponEquipped()
    {
        auto* player = f4vr::getPlayer();
        if (!player) {
            spdlog::debug("[WEAPON CHECK] No player - allow grab");
            return false;  // No player = allow grab (safer default)
        }
        
        bool weaponDrawn = player->actorState.IsWeaponDrawn();
        
        // If no weapon is drawn, allow grab
        if (!weaponDrawn) {
            return false;
        }
        
        // Direct memory access with full null chain checks
        // Path: player->middleProcess->unk08->equipData->item
        // This avoids calling GetEquippedWeapon which goes through AIProcess::GetEquippedItem
        if (!player->middleProcess) {
            spdlog::debug("[WEAPON CHECK] middleProcess null - allow grab");
            return false;
        }
        
        auto* unk08 = player->middleProcess->unk08;
        if (!unk08) {
            spdlog::debug("[WEAPON CHECK] unk08 null - allow grab");
            return false;
        }
        
        auto* equipData = unk08->equipData;
        if (!equipData) {
            spdlog::debug("[WEAPON CHECK] equipData null - Unarmed, allow grab");
            return false;
        }
        
        auto* item = equipData->item;
        if (!item) {
            spdlog::debug("[WEAPON CHECK] item null - Unarmed, allow grab");
            return false;
        }
        
        // Get weapon name
        const char* name = item->GetFullName();
        if (!name || name[0] == '\0') {
            spdlog::debug("[WEAPON CHECK] Weapon name empty - Unarmed, allow grab");
            return false;
        }
        
        // Check if this is a throwable weapon (grenade/mine like Molotov Cocktail)
        // Throwables should NEVER block grabbing - they are not "real" weapons that 
        // occupy the hand in a way that conflicts with object manipulation
        // NOTE: Cannot use EnumSet::any()/IsThrownWeapon() because WEAPON_TYPE is a
        // sequential enum (0-11), not a bitmask. EnumSet::any() uses bitwise AND which
        // gives false positives (e.g. kGun=9 & (kGrenade|kMine)=11 → 9 ≠ 0 → true!)
        // Instead, compare the weapon type directly using equality.
        auto* reForm = reinterpret_cast<RE::TESForm*>(item);
        if (reForm->IsWeapon()) {
            auto* weapon = static_cast<RE::TESObjectWEAP*>(reForm);
            auto weaponType = weapon->weaponData.type.get();
            if (weaponType == RE::WEAPON_TYPE::kGrenade || weaponType == RE::WEAPON_TYPE::kMine) {
                spdlog::debug("[WEAPON CHECK] Throwable weapon '{}' (type={}) equipped - allow grab", 
                    name, static_cast<int>(weaponType));
                return false;
            }
            spdlog::debug("[WEAPON CHECK] Weapon '{}' type={} - checking as real weapon", 
                name, static_cast<int>(weaponType));
        }
        
        // Real weapon is equipped and drawn - BLOCK grabbing
        // Throttle logging - only log once per second
        static std::chrono::steady_clock::time_point lastWeaponLog;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - lastWeaponLog).count() > 1.0f) {
            spdlog::debug("[WEAPON CHECK] Real weapon '{}' equipped and drawn - BLOCKING grab", name);
            lastWeaponLog = now;
        }
        return true;
    }
    
    // Check if an object reference is storable (can be picked up to inventory)
    // Used by selection priority system to prefer storable items
    bool IsStorableItem(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        RE::TESForm* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return false;
        
        RE::ENUM_FORM_ID formType = baseForm->GetFormType();
        
        // Storable form types (items that can go in player inventory)
        switch (formType)
        {
            case RE::ENUM_FORM_ID::kMISC:  // Miscellaneous items (junk, etc.)
            case RE::ENUM_FORM_ID::kWEAP:  // Weapons
            case RE::ENUM_FORM_ID::kARMO:  // Armor/clothing
            case RE::ENUM_FORM_ID::kALCH:  // Consumables (food, chems, etc.)
            case RE::ENUM_FORM_ID::kAMMO:  // Ammunition
            case RE::ENUM_FORM_ID::kBOOK:  // Books/magazines
            case RE::ENUM_FORM_ID::kNOTE:  // Notes/holotapes
            case RE::ENUM_FORM_ID::kKEYM:  // Keys
            case RE::ENUM_FORM_ID::kINGR:  // Ingredients
            case RE::ENUM_FORM_ID::kCMPO:  // Components
                return true;
            default:
                return false;
        }
    }
}  // namespace heisenberg

namespace
{
    // Re-open anonymous namespace for internal helpers
    // Calculate finger curl values based on item dimensions
    // Returns values 0.0 (fully bent/closed) to 1.0 (fully straight/open)
    // Fingers should curl more for smaller items, less for larger items
    // The offset parameter adds a small gap so fingers don't clip through the item
    
    struct FingerCurlValues
    {
        float thumb = 0.7f;
        float index = 0.5f;
        float middle = 0.5f;
        float ring = 0.5f;
        float pinky = 0.5f;
    };
    
    FingerCurlValues CalculateFingerCurl(float itemLength, float itemWidth, float itemHeight, float fingerOffset = 0.1f)
    {
        FingerCurlValues result;
        
        // If dimensions are invalid, use default relaxed grip
        if (itemLength <= 0.0f && itemWidth <= 0.0f && itemHeight <= 0.0f)
        {
            spdlog::debug("[FINGER] Invalid dimensions, using default grip");
            return result;  // Return default values
        }
        
        // Use the smallest positive dimension (typically the item's grip width)
        // This represents how far fingers need to stretch to wrap around the object
        float dims[3] = { itemLength, itemWidth, itemHeight };
        float gripSize = FLT_MAX;
        for (float d : dims)
        {
            if (d > 0.5f && d < gripSize) gripSize = d;
        }
        if (gripSize == FLT_MAX) gripSize = 5.0f;  // Default fallback
        
        // Finger length in game units (calibrated for VR hands)
        // FRIK hand model has fingers approximately 6-8 units long
        constexpr float avgFingerLength = 7.0f;
        
        // Calculate curl ratio: how much fingers need to close to touch the object
        // gripSize / (2 * fingerLength) gives how much of the "diameter" the finger arc covers
        // 
        // gripSize = 0  -> ratio = 0.0 -> fingers fully closed (0.0)
        // gripSize = 7  -> ratio = 0.5 -> fingers half closed (0.5)
        // gripSize = 14 -> ratio = 1.0 -> fingers fully open (1.0)
        //
        // Add fingerOffset to prevent clipping (fingers stop slightly before object surface)
        float rawRatio = (gripSize + fingerOffset * avgFingerLength) / (avgFingerLength * 2.0f);
        
        // Apply a curve for more natural grip - small objects need tighter grip
        // Square root makes small objects curl more aggressively
        float curlRatio = std::sqrt(rawRatio);
        
        // Clamp to valid range: 
        // - Minimum 0.15 (don't fully close fingers, looks unnatural)
        // - Maximum 0.75 (always some curl when grabbing, never fully open)
        curlRatio = std::clamp(curlRatio, 0.15f, 0.75f);
        
        // Thumb opposes and is slightly more open
        result.thumb = std::clamp(curlRatio + 0.2f, 0.35f, 0.85f);
        
        // Index and middle are the primary gripping fingers
        result.index = curlRatio;
        result.middle = curlRatio;
        
        // Ring curls slightly more (natural grip cascade)
        result.ring = std::clamp(curlRatio - 0.05f, 0.1f, 0.7f);
        
        // Pinky curls the most (further from palm)
        result.pinky = std::clamp(curlRatio - 0.1f, 0.05f, 0.65f);
        
        spdlog::debug("[FINGER] gripSize={:.1f}, rawRatio={:.2f}, curlRatio={:.2f}", 
                      gripSize, rawRatio, curlRatio);
        
        return result;
    }
    
}  // Close anonymous namespace for internal helpers

// =========================================================================
// BEHIND-EAR/SHOULDER STORAGE DETECTION
// =========================================================================
// Store items by holding them near configured body zones
// Hand must be held in zone for 2+ seconds to trigger storage

static constexpr float kStorageHoldTime = 2.0f;                     // Seconds to hold before storing

// NOTE: StorageZoneResult is declared in Grab.h within heisenberg namespace

// Create a yaw-only rotation matrix from HMD rotation
// This extracts only the horizontal rotation (around Z axis) to make the zone
// relative to player body facing direction, not head tilt/pitch
static inline RE::NiMatrix3 GetYawOnlyRotation(const RE::NiMatrix3& fullRot)
    {
        // Extract the forward direction (Y axis) and project it onto XY plane
        float forwardX = fullRot.entry[0][1];  // Y column, X row
        float forwardY = fullRot.entry[1][1];  // Y column, Y row
        
        // Normalize the 2D direction
        float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (length < 0.001f) {
            // Fallback to identity if forward is pointing straight up/down
            RE::NiMatrix3 identity;
            identity.entry[0][0] = 1.0f; identity.entry[0][1] = 0.0f; identity.entry[0][2] = 0.0f;
            identity.entry[1][0] = 0.0f; identity.entry[1][1] = 1.0f; identity.entry[1][2] = 0.0f;
            identity.entry[2][0] = 0.0f; identity.entry[2][1] = 0.0f; identity.entry[2][2] = 1.0f;
            return identity;
        }
        
        forwardX /= length;
        forwardY /= length;
        
        // Build rotation matrix: X = right, Y = forward, Z = up
        // Right = perpendicular to forward in XY plane
        float rightX = forwardY;
        float rightY = -forwardX;
        
        RE::NiMatrix3 yawRot;
        // Row 0 (X basis)
        yawRot.entry[0][0] = rightX;
        yawRot.entry[0][1] = forwardX;
        yawRot.entry[0][2] = 0.0f;
        // Row 1 (Y basis)  
        yawRot.entry[1][0] = rightY;
        yawRot.entry[1][1] = forwardY;
        yawRot.entry[1][2] = 0.0f;
        // Row 2 (Z basis - just up)
        yawRot.entry[2][0] = 0.0f;
        yawRot.entry[2][1] = 0.0f;
        yawRot.entry[2][2] = 1.0f;
        
        return yawRot;
    }
    
// Transform a point from local HMD space to world space
// This is the INVERSE of the operation done when setting the zone
// Setting: local = rot^T * world  (uses entry[col][row] indexing)
// Here:    world = rot * local    (uses entry[row][col] indexing) 
// But NiMatrix3 is column-major, so we need to match the transpose pattern
static inline RE::NiPoint3 TransformPoint(const RE::NiMatrix3& rot, const RE::NiPoint3& pos, float scale, float offsetX, float offsetY, float offsetZ)
{
    RE::NiPoint3 point(offsetX, offsetY, offsetZ);
    RE::NiPoint3 scaled = point * scale;
    RE::NiPoint3 rotated;
    // Use column-major multiplication (same as NiMatrix3 standard)
    // To reverse the transpose done in Heisenberg.cpp, we use row-major here
    rotated.x = rot.entry[0][0] * scaled.x + rot.entry[1][0] * scaled.y + rot.entry[2][0] * scaled.z;
    rotated.y = rot.entry[0][1] * scaled.x + rot.entry[1][1] * scaled.y + rot.entry[2][1] * scaled.z;
    rotated.z = rot.entry[0][2] * scaled.x + rot.entry[1][2] * scaled.y + rot.entry[2][2] * scaled.z;
    return rotated + pos;
}

// =========================================================================
// CHECK ITEM STORAGE ZONE (NEW CONFIGURABLE SYSTEM)
// Uses the itemStorageZone config and storage-specific offsets
// Defined in heisenberg namespace for external access
// =========================================================================
namespace heisenberg
{
    StorageZoneResult CheckItemStorageZone(const RE::NiPoint3& handPos)
    {
        StorageZoneResult result;
        
        // Check if item storage zones are disabled
        if (!g_config.enableItemStorageZones)
            return result;
        
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->HmdNode)
            return result;
        
        // Get HMD transform - use yaw-only rotation so the zone stays
        // relative to player body direction, not head tilt/pitch
        const RE::NiPoint3& hmdPos = playerNodes->HmdNode->world.translate;
        const RE::NiMatrix3& hmdRot = playerNodes->HmdNode->world.rotate;
        RE::NiMatrix3 yawRot = GetYawOnlyRotation(hmdRot);
        float hmdScale = playerNodes->HmdNode->world.scale;
        
        float radius = g_config.itemStorageZoneRadius;
        
        // Calculate zone position from single configurable offset using yaw-only rotation
        RE::NiPoint3 zonePos = TransformPoint(yawRot, hmdPos, hmdScale, 
            g_config.storageZoneOffsetX,
            g_config.storageZoneOffsetY,
            g_config.storageZoneOffsetZ);
        
        RE::NiPoint3 toZone = handPos - zonePos;
        float dist = std::sqrt(toZone.x * toZone.x + toZone.y * toZone.y + toZone.z * toZone.z);
        
        if (dist < radius)
        {
            // Check if we require hand to be behind head
            if (g_config.requireHandBehindHead)
            {
                // Transform hand position to HMD local space
                RE::NiPoint3 handLocal = handPos - hmdPos;
                // Use yaw-only inverse rotation to get local coordinates
                RE::NiMatrix3 yawRotInv = yawRot.Transpose();
                RE::NiPoint3 handInHmdSpace;
                handInHmdSpace.x = yawRotInv.entry[0][0] * handLocal.x + yawRotInv.entry[0][1] * handLocal.y + yawRotInv.entry[0][2] * handLocal.z;
                handInHmdSpace.y = yawRotInv.entry[1][0] * handLocal.x + yawRotInv.entry[1][1] * handLocal.y + yawRotInv.entry[1][2] * handLocal.z;
                handInHmdSpace.z = yawRotInv.entry[2][0] * handLocal.x + yawRotInv.entry[2][1] * handLocal.y + yawRotInv.entry[2][2] * handLocal.z;
                
                // Y positive = behind, Y negative = forward (F4VR coordinate system)
                // Only allow storage if hand is behind head (Y > threshold)
                // Negative tolerance means hand must be at least that far behind
                if (handInHmdSpace.y < -g_config.behindHeadTolerance)
                {
                    spdlog::debug("[ITEM-STORAGE] Hand in zone but not behind head (localY={:.1f} < -{:.1f}), rejecting", 
                                 handInHmdSpace.y, g_config.behindHeadTolerance);
                    return result;
                }
            }
            
            result.isInZone = true;
            result.isLeftSide = false;
            spdlog::debug("[ITEM-STORAGE] Hand in storage zone at dist {:.1f} (radius {:.1f})", 
                         dist, radius);
        }
        
        return result;
    }
}  // namespace heisenberg

namespace
{
    // Re-open anonymous namespace for internal helpers

    // =====================================================================
    // DEFERRED DISABLE QUEUE
    // Forward declare — TickDeferredDisables (heisenberg namespace) calls into this
    void TickDeferredDisablesInternal();

    // =====================================================================
    // DEFERRED DISABLE QUEUE
    // Items with behavior graphs (weapons, armor) can crash if disabled
    // immediately — the IO thread may still be cleaning up hkbBehaviorGraph
    // data. Queue them for disable after a few frames.
    // =====================================================================
    static constexpr int DEFERRED_DISABLE_FRAMES = 5;
    static constexpr int MAX_DEFERRED_DISABLES = 16;
    static struct {
        RE::ObjectRefHandle handle;
        int framesLeft = 0;
    } s_deferredDisables[MAX_DEFERRED_DISABLES];

    void QueueDeferredDisable(RE::TESObjectREFR* refr)
    {
        if (!refr) return;
        for (auto& slot : s_deferredDisables) {
            if (slot.framesLeft <= 0) {
                slot.handle = RE::ObjectRefHandle(refr);
                slot.framesLeft = DEFERRED_DISABLE_FRAMES;
                spdlog::debug("[DeferredDisable] Queued {:08X} for disable in {} frames",
                             refr->formID, DEFERRED_DISABLE_FRAMES);
                return;
            }
        }
        // Queue full — fall back to immediate disable
        spdlog::warn("[DeferredDisable] Queue full — immediate disable of {:08X}", refr->formID);
        refr->Disable();
    }

    bool ShouldDeferDisable(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm) return false;
        auto formType = baseForm->GetFormType();
        // Weapons, armor, and holotapes can have behavior graphs (animation .hkx files)
        return formType == RE::ENUM_FORM_ID::kWEAP || formType == RE::ENUM_FORM_ID::kARMO
            || formType == RE::ENUM_FORM_ID::kNOTE;
    }

    // Safe disable: defers for weapons/armor, immediate for everything else
    void SafeDisableRef(RE::TESObjectREFR* refr)
    {
        if (ShouldDeferDisable(refr)) {
            QueueDeferredDisable(refr);
        } else {
            refr->Disable();
        }
    }

    void TickDeferredDisablesInternal()
    {
        for (auto& slot : s_deferredDisables) {
            if (slot.framesLeft > 0) {
                if (--slot.framesLeft == 0) {
                    RE::NiPointer<RE::TESObjectREFR> refPtr = slot.handle.get();
                    if (refPtr) {
                        refPtr->Disable();
                        spdlog::debug("[DeferredDisable] Disabled {:08X}", refPtr->formID);
                    }
                    slot.handle.reset();
                }
            }
        }
    }

    // Add grabbed item to player inventory and delete the world reference
    // Uses ActivateRef like HIGGS Skyrim - this is safer than PickUpObject
    // showHudMessage: If false, don't show "X was stored" message (for quickloot items that already showed a message)
    bool StoreGrabbedItem(RE::TESObjectREFR* refr, bool showHudMessage = true)
    {
        if (!refr)
            return false;
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return false;
        
        // IMPORTANT: Get item name BEFORE ActivateRef - the reference may be deleted/cleaned up after!
        std::string itemName;
        if (showHudMessage) {
            itemName = heisenberg::ItemOffsetManager::GetItemName(refr);
            if (itemName.empty()) itemName = "item";
        }
        
        // Debug: try to get item count from the reference's extra data
        int32_t extraCount = 1;  // Default to 1 if no ExtraCount
        if (refr->extraList) {
            // Try to get ExtraCount data - kCount is in EXTRA_DATA_TYPE enum
            auto* extraData = refr->extraList->GetByType(RE::EXTRA_DATA_TYPE::kCount);
            if (extraData) {
                // ExtraCount layout: BSExtraData base (0x18) then int16_t count at 0x18
                int16_t* countPtr = reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(extraData) + 0x18);
                extraCount = *countPtr;
                spdlog::debug("[STORE] About to store {:08X} '{}', ExtraCount: {}", refr->formID, itemName, extraCount);
            } else {
                spdlog::debug("[STORE] About to store {:08X} '{}', no ExtraCount data (count=1)", refr->formID, itemName);
            }
        } else {
            spdlog::debug("[STORE] About to store {:08X} '{}', no extraList!", refr->formID, itemName);
        }
        
        // Suppress "X was added" message that game shows when picking up items
        // We'll show our own "X was stored" message instead
        // Uses INI setting toggle (bShowHUDMessages:Interface) - much safer than hooking
        heisenberg::Hooks::SetSuppressHUDMessages(true);

        // For holotapes (kNOTE): use AddObjectToContainer instead of ActivateRef.
        // ActivateRef on a holotape triggers native playback, which we don't want
        // when simply storing to inventory. Playback is handled separately by
        // PipboyInteraction when a holotape is inserted into the tape deck.
        // Mark this item as recently stored so loot-to-hand doesn't re-grab it
        // ActivateRef fires TESContainerChangedEvent which loot-to-hand would intercept
        auto* baseForm = refr->GetObjectReference();
        if (baseForm) {
            heisenberg::DropToHand::GetSingleton().MarkAsRecentlyStored(baseForm->formID);
        }

        bool result = false;
        if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
            heisenberg::AddObjectToContainer(player, static_cast<RE::TESBoundObject*>(baseForm),
                                             &nullExtra, extraCount, nullptr, 0);
            // Clean up the world reference since AddObjectToContainer doesn't do this.
            // Do NOT call SetDelete — Inventory3DManager may still hold a handle to this
            // ref for 3D preview, causing crash in FinishItemLoadTask → GetOnLocalMap.
            // SafeDisableRef defers for items with behavior graphs to prevent IO thread crash.
            SafeDisableRef(refr);
            result = true;
            spdlog::info("[STORE] Holotape {:08X} stored via AddObjectToContainer (no playback)", refr->formID);
        } else {
            // Use ActivateRef like HIGGS Skyrim does
            // Parameters: actionRef (player), objectToGet (nullptr), count,
            //             defaultProcessingOnly (false), fromScript (false), looping (false)
            // Use extraCount to pick up the entire stack, not just 1 item
            // Bypass our ActivateRef hook (item is still held/selected at this point)
            heisenberg::Hooks::SetInternalActivation(true);
            result = refr->ActivateRef(player, nullptr, extraCount, false, false, false);
            heisenberg::Hooks::SetInternalActivation(false);
        }

        // Temporarily restore HUD messages so our custom message can display,
        // then re-suppress and schedule deferred unsuppress to catch any native
        // messages that ActivateRef queued for later frames
        heisenberg::Hooks::SetSuppressHUDMessages(false);
        
        if (result) {
            spdlog::info("[STORE] Stored item {:08X} '{}' x{} to inventory via ActivateRef", refr->formID, itemName, extraCount);

            // Show HUD notification with item name (unless suppressed for quickloot items)
            if (showHudMessage && heisenberg::g_config.showStorageMessages) {
                std::string msg;
                if (extraCount > 1) {
                    msg = std::format("{} ({}) stored", itemName, extraCount);
                } else {
                    msg = std::format("{} stored", itemName);
                }

                spdlog::info("[STORE] Calling ShowHUDMessage with: '{}'", msg);
                heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
                spdlog::info("[STORE] ShowHUDMessage returned");
            } else {
                spdlog::info("[STORE] HUD message suppressed (showHudMessage=false)");
            }
        } else {
            spdlog::warn("[STORE] ActivateRef failed for {:08X} '{}'", refr->formID, itemName);
        }

        // Re-suppress and schedule deferred unsuppress to catch any native
        // "X added" messages that ActivateRef queued for later frames
        heisenberg::Hooks::SetSuppressHUDMessages(true);
        heisenberg::Hooks::ScheduleDeferredHUDUnsuppress(5);

        return result;
    }
    
    // =========================================================================
    // MOUTH CONSUME DETECTION (HIGGS Skyrim style - sphere-based)
    // =========================================================================
    // Uses configurable sphere position relative to HMD, like Skyrim HIGGS
    // Hand must be moving slowly AND fingertip must be within the mouth sphere
    // 
    // Like Skyrim HIGGS, we use the FRIK fingertip position (similar to palmPos)
    // to detect when the hand is near the mouth. This feels more natural than
    // using wand position since you're bringing your "hand" to your mouth.
    
    // Helper: Get wand position for mouth detection
    // Uses wand node directly (not fingertip) for more reliable detection
    RE::NiPoint3 GetWandPositionForMouth(bool isLeft)
    {
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (playerNodes)
        {
            RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
            if (wandNode)
            {
                return wandNode->world.translate;
            }
        }

        return RE::NiPoint3(0, 0, 0);
    }
    
    // Helper: Check if wand is in mouth zone (distance only)
    // Used for haptic feedback during testing
    // Uses wand position for consistent detection
    bool IsInMouthZone(bool isLeft)
    {
        if (heisenberg::g_config.consumableActivationZone == 0)
            return false;
        
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->HmdNode)
            return false;
        
        const RE::NiPoint3& hmdPos = playerNodes->HmdNode->world.translate;
        const RE::NiMatrix3& hmdRot = playerNodes->HmdNode->world.rotate;
        
        RE::NiPoint3 mouthLocal(
            heisenberg::g_config.mouthOffsetX,
            heisenberg::g_config.mouthOffsetY,
            heisenberg::g_config.mouthOffsetZ
        );
        RE::NiPoint3 mouthWorld = hmdPos + (hmdRot * mouthLocal);
        
        // Get wand position
        RE::NiPoint3 wandPos = GetWandPositionForMouth(isLeft);
        
        RE::NiPoint3 diff = wandPos - mouthWorld;
        float distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
        float radiusSq = heisenberg::g_config.mouthRadius * heisenberg::g_config.mouthRadius;
        return distSq < radiusSq;
    }
    
    // Check if wand is in mouth zone and update timer in GrabState
    // Returns true if item should be consumed (timer reached 0.5s threshold)
    bool CheckMouthConsume(bool isLeft, heisenberg::GrabState& state)
    {
        // Check if consumable zone is disabled
        if (heisenberg::g_config.consumableActivationZone == 0) {
            state.mouthZoneTimer = 0.0f;
            state.isInMouthZone = false;
            return false;
        }

        // 0.5s cooldown after grabbing — prevents accidental consume when retrieving items
        float elapsed = static_cast<float>(heisenberg::Utils::GetTime()) - state.grabStartTime;
        if (elapsed < 0.5f) {
            state.mouthZoneTimer = 0.0f;
            state.isInMouthZone = false;
            return false;
        }

        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->HmdNode) {
            state.mouthZoneTimer = 0.0f;
            state.isInMouthZone = false;
            return false;
        }
        
        // Get HMD transform to convert local offset to world space
        const RE::NiPoint3& hmdPos = playerNodes->HmdNode->world.translate;
        const RE::NiMatrix3& hmdRot = playerNodes->HmdNode->world.rotate;
        
        // Mouth sphere - transform local offset to world
        RE::NiPoint3 mouthLocal(
            heisenberg::g_config.mouthOffsetX,
            heisenberg::g_config.mouthOffsetY,
            heisenberg::g_config.mouthOffsetZ
        );
        RE::NiPoint3 mouthWorld = hmdPos + (hmdRot * mouthLocal);
        
        // Get wand position
        RE::NiPoint3 wandPos = GetWandPositionForMouth(isLeft);
        
        // Check if in mouth zone (squared distance to avoid sqrt)
        RE::NiPoint3 mouthDiff = wandPos - mouthWorld;
        float mouthDistSq = mouthDiff.x * mouthDiff.x + mouthDiff.y * mouthDiff.y + mouthDiff.z * mouthDiff.z;
        float mouthRadiusSq = heisenberg::g_config.mouthRadius * heisenberg::g_config.mouthRadius;
        bool inZone = mouthDistSq < mouthRadiusSq;
        
        // Velocity threshold - must be moving slower than this to consume (prevents accidental consumes)
        float velocityThreshold = heisenberg::g_config.mouthVelocityThreshold;
        // state.handSpeed is computed in m/s from position delta in CheckHandPositionBasedActions
        bool slowEnough = state.handSpeed < velocityThreshold;
        
        if (inZone) {
            state.isInMouthZone = true;
            
            // Debug logging (every ~90 frames)
            static int logCounter = 0;
            if (++logCounter >= 90) {
                logCounter = 0;
                spdlog::debug("[MOUTH] dist={:.1f} (need<{:.1f}) | speed={:.2f}m/s (need<{:.2f})",
                             std::sqrt(mouthDistSq), heisenberg::g_config.mouthRadius,
                             state.handSpeed, velocityThreshold);
            }
            
            // Check if moving slow enough to consume
            if (slowEnough) {
                spdlog::debug("[MOUTH] CONSUME! In zone and slow enough (speed={:.2f}m/s)", state.handSpeed);
                return true;
            }
        } else {
            state.isInMouthZone = false;
        }
        
        return false;
    }
    
    // =========================================================================
    // HAND INJECTION ZONE DETECTION
    // =========================================================================
    // Sphere on the opposite hand's wand for syringe-style consumption.
    // If holding a consumable in left hand, injection zone is on right wand
    // (and vice versa). Uses same offset convention as Virtual Chems.

    // Helper: Check if holding-hand wand is in the opposite hand's injection zone
    bool IsInHandInjectionZone(bool isLeft)
    {
        if (!heisenberg::g_config.enableHandInjection)
            return false;

        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes)
            return false;

        // Get the wand node of the hand HOLDING the item
        RE::NiNode* holdingWand = heisenberg::GetWandNode(playerNodes, isLeft);
        // Get the wand node of the OPPOSITE hand (injection target)
        RE::NiNode* oppositeWand = heisenberg::GetWandNode(playerNodes, !isLeft);
        if (!holdingWand || !oppositeWand)
            return false;

        // Calculate injection zone center: offset relative to opposite wand
        const RE::NiPoint3& wandPos = oppositeWand->world.translate;
        const RE::NiMatrix3& wandRot = oppositeWand->world.rotate;

        RE::NiPoint3 injectionLocal(
            heisenberg::g_config.handInjectionOffsetX,
            heisenberg::g_config.handInjectionOffsetY,
            heisenberg::g_config.handInjectionOffsetZ
        );
        RE::NiPoint3 injectionWorld = wandPos + (wandRot * injectionLocal);

        // Check distance from holding wand to injection zone (squared to avoid sqrt)
        RE::NiPoint3 holdingPos = holdingWand->world.translate;
        RE::NiPoint3 injDiff = holdingPos - injectionWorld;
        float distSq = injDiff.x * injDiff.x + injDiff.y * injDiff.y + injDiff.z * injDiff.z;
        float radiusSq = heisenberg::g_config.handInjectionRadius * heisenberg::g_config.handInjectionRadius;
        bool inZone = distSq < radiusSq;

        // Periodic logging (every ~90 frames) — warn level so it shows in default log
        static int injLogCounter = 0;
        if (++injLogCounter >= 90) {
            injLogCounter = 0;
            spdlog::debug("[INJECT] dist={:.1f} (need<{:.1f}) speed={:.2f}m/s",
                std::sqrt(distSq), heisenberg::g_config.handInjectionRadius,
                heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft).handSpeed);
        }
        return inZone;
    }

    // Check if wand is in hand injection zone with velocity guard
    // Returns true if item should be consumed
    bool CheckHandInjectionConsume(bool isLeft, heisenberg::GrabState& state)
    {
        if (!heisenberg::g_config.enableHandInjection)
            return false;

        // Same 0.5s post-grab cooldown as mouth zone
        float elapsed = static_cast<float>(heisenberg::Utils::GetTime()) - state.grabStartTime;
        if (elapsed < 0.5f)
            return false;

        if (!IsInHandInjectionZone(isLeft))
        {
            state.isInHandInjectionZone = false;
            return false;
        }

        state.isInHandInjectionZone = true;

        // Same velocity threshold as mouth zone
        float velocityThreshold = heisenberg::g_config.mouthVelocityThreshold;
        if (state.handSpeed < velocityThreshold)
        {
            spdlog::debug("[INJECT] CONSUME! In hand injection zone and slow enough (speed={:.2f}m/s)", state.handSpeed);
            return true;
        }

        return false;
    }

    // Helper: Check if held item is an injectable (syringe-type chem)
    // Whitelist: stimpak, med-x, psycho (and variants), antibiotics, calmex
    bool IsInjectable(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || !baseForm->IsAlchemyItem()) return false;
        auto nameView = RE::TESFullName::GetFullName(*baseForm, false);
        if (nameView.empty()) return false;
        const std::string name(nameView);
        const char* n = name.c_str();
        return heisenberg::ContainsCI(n, "stimpak") ||
               heisenberg::ContainsCI(n, "med-x") ||
               heisenberg::ContainsCI(n, "psycho") ||
               heisenberg::ContainsCI(n, "antibiotic") ||
               heisenberg::ContainsCI(n, "calmex");
    }

    // Armor zone types for zone-specific equipping
    enum class ArmorZoneType
    {
        None,       // Not armor or unrecognized slot
        Head,       // Glasses, hats, helmets
        Chest,      // Shirts, jackets, chest armor
        Legs,       // Pants, leg armor, boots
        Hands,      // Gloves (uses chest zone for now)
        Other       // Rings, amulets, etc.
    };
    
    // Fallout 4 Biped slot masks (from https://falloutck.uesp.net/wiki/Biped_Slots)
    // These are different from Skyrim slots!
    constexpr uint32_t kSlot_HairTop = 1 << 0;      // Slot 30: Hair Top (helmets, hats)
    constexpr uint32_t kSlot_HairLong = 1 << 1;     // Slot 31: Hair Long
    constexpr uint32_t kSlot_Head = 1 << 2;         // Slot 32: Head (face covers)
    constexpr uint32_t kSlot_Body = 1 << 3;         // Slot 33: Body
    constexpr uint32_t kSlot_LHand = 1 << 4;        // Slot 34: Left Hand
    constexpr uint32_t kSlot_RHand = 1 << 5;        // Slot 35: Right Hand
    constexpr uint32_t kSlot_Jacket = 1 << 6;       // Slot 36: Jacket (over body)
    constexpr uint32_t kSlot_Necklace = 1 << 7;     // Slot 37: Necklace
    constexpr uint32_t kSlot_UpperBody = 1 << 8;    // Slot 38: Under Armor Upper Body (chest)
    constexpr uint32_t kSlot_Torso = 1 << 9;        // Slot 39: Torso
    constexpr uint32_t kSlot_LArm = 1 << 10;        // Slot 40: Left Arm
    constexpr uint32_t kSlot_RArm = 1 << 11;        // Slot 41: Right Arm
    constexpr uint32_t kSlot_LLeg = 1 << 12;        // Slot 42: Left Leg
    constexpr uint32_t kSlot_RLeg = 1 << 13;        // Slot 43: Right Leg
    constexpr uint32_t kSlot_LowerBody = 1 << 14;   // Slot 44: Under Armor Lower Body (pants)
    constexpr uint32_t kSlot_LFoot = 1 << 15;       // Slot 45: Left Foot
    constexpr uint32_t kSlot_Headband = 1 << 16;    // Slot 46: Headband
    constexpr uint32_t kSlot_Eyes = 1 << 17;        // Slot 47: Eyes (glasses, goggles!)
    constexpr uint32_t kSlot_Beard = 1 << 18;       // Slot 48: Beard
    constexpr uint32_t kSlot_Mouth = 1 << 19;       // Slot 49: Mouth (masks)
    constexpr uint32_t kSlot_Neck = 1 << 20;        // Slot 50: Neck
    constexpr uint32_t kSlot_Ring = 1 << 21;        // Slot 51: Ring
    constexpr uint32_t kSlot_Scalp = 1 << 22;       // Slot 52: Scalp
    constexpr uint32_t kSlot_RFoot = 1 << 29;       // Slot 59: Right Foot (Pipboy slot usually)
    constexpr uint32_t kSlot_Shield = 1 << 30;      // Slot 60: Shield
    
    // Get the armor zone type from an armor's biped slot mask
    ArmorZoneType GetArmorZoneType(RE::TESObjectARMO* armor)
    {
        if (!armor)
            return ArmorZoneType::None;
        
        // Get biped slot mask - need to cast to f4sevr type to access
        // The slot is stored at offset 0x1E0 + 0x08 (BGSBipedObjectForm.data.parts)
        // For now, use a simplified approach - check the form's model path or keywords
        
        // Use GetSlotMask from the BGSBipedObjectForm component
        // TESObjectARMO inherits from BGSBipedObjectForm at offset 0x1E0
        uint32_t slotMask = 0;
        
        // Access the biped data - the slot mask is at a fixed offset
        // TESObjectARMO layout: ... + BGSBipedObjectForm at 0x1E0
        // BGSBipedObjectForm::Data at +0x08, with 'parts' (uint32) at +0x00
        uint8_t* armorPtr = reinterpret_cast<uint8_t*>(armor);
        uint32_t* slotMaskPtr = reinterpret_cast<uint32_t*>(armorPtr + 0x1E0 + 0x08);
        slotMask = *slotMaskPtr;
        
        spdlog::debug("[ARMOR-ZONE] Armor '{}' has slot mask {:08X}", 
                     armor->GetFullName() ? armor->GetFullName() : "unknown", slotMask);
        
        // Check for head gear (FO4 slots: HairTop, HairLong, Head, Headband, Eyes, Beard, Mouth, Scalp)
        constexpr uint32_t headSlots = kSlot_HairTop | kSlot_HairLong | kSlot_Head | kSlot_Headband | 
                                       kSlot_Eyes | kSlot_Beard | kSlot_Mouth | kSlot_Scalp;
        if (slotMask & headSlots)
            return ArmorZoneType::Head;
        
        // Check for body/chest (Body, Jacket, Necklace, UpperBody, Torso, Arms)
        constexpr uint32_t chestSlots = kSlot_Body | kSlot_Jacket | kSlot_Necklace | kSlot_UpperBody | 
                                        kSlot_Torso | kSlot_LArm | kSlot_RArm | kSlot_Neck;
        if (slotMask & chestSlots)
            return ArmorZoneType::Chest;
        
        // Check for legs/feet (LowerBody, Legs, Feet)
        constexpr uint32_t legSlots = kSlot_LowerBody | kSlot_LLeg | kSlot_RLeg | kSlot_LFoot | kSlot_RFoot;
        if (slotMask & legSlots)
            return ArmorZoneType::Legs;
        
        // Check for hands
        constexpr uint32_t handSlots = kSlot_LHand | kSlot_RHand;
        if (slotMask & handSlots)
            return ArmorZoneType::Hands;
        
        // Other (rings, shields, etc.)
        if (slotMask & (kSlot_Ring | kSlot_Shield))
            return ArmorZoneType::Other;
        
        return ArmorZoneType::None;
    }
    
    // Check if a held weapon is close to the weapon hand's fingertip for equipping
    // When holding a weapon in one hand, bringing it near the opposite (weapon) hand's fingertip
    // will trigger equipping. This allows natural "handing off" the weapon to your weapon hand.
    // Returns true if weapon should be equipped
    bool CheckWeaponEquipByFingertip(const RE::NiPoint3& heldWeaponPos, float handSpeed, bool holdingInLeftHand)
    {
        // Check if weapon equip is disabled
        if (heisenberg::g_config.weaponEquipMode == 0)
            return false;

        // Suppress during menu close cooldown (hands are close together after Pipboy closes)
        if (heisenberg::MenuChecker::GetSingleton().IsInMenuCloseCooldown())
            return false;

        // Determine which hand is the primary (weapon) hand
        bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
        bool primaryHandIsLeft = isLeftHandedMode;

        // CRITICAL: Only allow weapon equip when grabbing with the OFF-HAND.
        // If you're grabbing with your primary (weapon) hand, you can't "hand off"
        // to the weapon hand because it IS the weapon hand.
        // This also prevents the issue where weapons auto-equip and become impossible to drop.
        bool holdingInPrimaryHand = (holdingInLeftHand == primaryHandIsLeft);
        if (holdingInPrimaryHand)
        {
            // Grabbing with weapon hand - block weapon equip
            return false;
        }

        // Velocity check - hand must be moving slowly
        if (handSpeed >= heisenberg::g_config.armorEquipVelocityThreshold)
            return false;

        // The weapon hand is the PRIMARY hand
        bool weaponHandIsLeft = primaryHandIsLeft;
        
        // Try to get the weapon hand's fingertip position from FRIK
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        RE::NiPoint3 weaponHandFingertip;
        
        if (!frik.IsAvailable() || !frik.GetIndexFingerTipPosition(weaponHandIsLeft, weaponHandFingertip))
        {
            // FRIK not available - fall back to wand position
            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            if (!playerNodes)
                return false;
            
            RE::NiAVObject* wandNode = heisenberg::GetWandNode(playerNodes, weaponHandIsLeft);
            if (!wandNode)
                return false;
            
            weaponHandFingertip = wandNode->world.translate;
        }
        
        // Check distance from held weapon to weapon hand fingertip
        constexpr float weaponEquipRadius = 15.0f;  // 15cm radius
        float dist = (heldWeaponPos - weaponHandFingertip).Length();
        
        // Debug logging (every ~90 frames)
        static int logCounter = 0;
        if (++logCounter >= 90) {
            logCounter = 0;
            spdlog::debug("[WEAP-EQUIP] dist to {} fingertip: {:.1f}cm (need<{:.1f}cm) | speed={:.2f}",
                         weaponHandIsLeft ? "LEFT" : "RIGHT", dist, weaponEquipRadius, handSpeed);
        }
        
        if (dist < weaponEquipRadius) {
            spdlog::debug("[WEAP-EQUIP] Weapon within {}cm of {} hand fingertip - equip!",
                        dist, weaponHandIsLeft ? "left" : "right");
            return true;
        }
        
        return false;
    }
    
    // Check if hand is in specific armor equip zone (head, chest, or legs)
    // Returns the zone type if in zone, or None if not in any zone
    ArmorZoneType CheckArmorEquipZone(const RE::NiPoint3& handPos, float handSpeed, ArmorZoneType requiredZone)
    {
        // Check if armor equip zone is disabled
        if (heisenberg::g_config.armorEquipZone == 0)
            return ArmorZoneType::None;
        
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->HmdNode)
            return ArmorZoneType::None;
        
        // Velocity check first - hand must be moving slowly
        if (handSpeed >= heisenberg::g_config.armorEquipVelocityThreshold)
            return ArmorZoneType::None;
        
        // Get HMD transform to convert local offset to world space
        const RE::NiPoint3& hmdPos = playerNodes->HmdNode->world.translate;
        const RE::NiMatrix3& hmdRot = playerNodes->HmdNode->world.rotate;
        
        // Check the required zone based on armor type
        float offsetX, offsetY, offsetZ, radius;
        const char* zoneName;
        
        switch (requiredZone)
        {
        case ArmorZoneType::Head:
            offsetX = heisenberg::g_config.headZoneOffsetX;
            offsetY = heisenberg::g_config.headZoneOffsetY;
            offsetZ = heisenberg::g_config.headZoneOffsetZ;
            radius = heisenberg::g_config.headZoneRadius;
            zoneName = "HEAD";
            break;
            
        case ArmorZoneType::Chest:
        case ArmorZoneType::Hands:  // Hands use chest zone
        case ArmorZoneType::Other:  // Other items use chest zone
            offsetX = heisenberg::g_config.chestZoneOffsetX;
            offsetY = heisenberg::g_config.chestZoneOffsetY;
            offsetZ = heisenberg::g_config.chestZoneOffsetZ;
            radius = heisenberg::g_config.chestZoneRadius;
            zoneName = "CHEST";
            break;
            
        case ArmorZoneType::Legs:
            offsetX = heisenberg::g_config.legZoneOffsetX;
            offsetY = heisenberg::g_config.legZoneOffsetY;
            offsetZ = heisenberg::g_config.legZoneOffsetZ;
            radius = heisenberg::g_config.legZoneRadius;
            zoneName = "LEGS";
            break;
            
        default:
            return ArmorZoneType::None;
        }
        
        // Transform local offset to world space
        RE::NiPoint3 zoneLocal(offsetX, offsetY, offsetZ);
        RE::NiPoint3 zoneWorld = hmdPos + (hmdRot * zoneLocal);
        
        // Check zone sphere
        float dist = (handPos - zoneWorld).Length();
        
        // Debug logging (every ~90 frames)
        static int logCounter = 0;
        if (++logCounter >= 90) {
            logCounter = 0;
            spdlog::debug("[ARMOR-{}] dist={:.1f} (need<{:.1f}) | speed={:.2f}",
                         zoneName, dist, radius, handSpeed);
        }
        
        if (dist < radius) {
            spdlog::debug("[ARMOR-{}] IN ZONE! dist={:.1f} (radius={:.1f})",
                         zoneName, dist, radius);
            return requiredZone;
        }
        
        return ArmorZoneType::None;
    }
    
    // Check if an item is consumable (food, drink, chem, etc.)
    bool IsConsumable(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return false;
        
        // Check if it's an AlchemyItem (food, drink, chems, etc.)
        return baseForm->IsAlchemyItem();
    }
    
    // Check if an item is armor (clothing, armor pieces, glasses, hats, etc.)
    bool IsArmor(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return false;
        
        // Check if it's armor (ARMO form type)
        return baseForm->GetFormType() == RE::ENUM_FORM_ID::kARMO;
    }
    
    // Equip armor by picking it up and then equipping it
    // Returns true if successfully equipped
    bool EquipArmorItem(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || baseForm->GetFormType() != RE::ENUM_FORM_ID::kARMO)
            return false;
        
        RE::TESObjectARMO* armor = static_cast<RE::TESObjectARMO*>(baseForm);
        
        spdlog::debug("[EQUIP-ARMOR] Attempting to equip '{}' ({:08X})",
                     armor->GetFullName() ? armor->GetFullName() : "unknown",
                     baseForm->GetFormID());
        
        // First, activate the item to pick it up into inventory
        heisenberg::Hooks::SetInternalActivation(true);
        bool activated = refr->ActivateRef(player, nullptr, 1, false, false, false);
        heisenberg::Hooks::SetInternalActivation(false);

        if (!activated) {
            spdlog::warn("[EQUIP-ARMOR] ActivateRef failed for {:08X}", refr->formID);
            return false;
        }
        
        spdlog::debug("[EQUIP-ARMOR] Picked up armor, now equipping...");
        
        // Now equip it using ActorEquipManager
        RE::ActorEquipManager** equipMgrPtr = heisenberg::g_ActorEquipManager.get();
        if (!equipMgrPtr || !*equipMgrPtr) {
            spdlog::warn("[EQUIP-ARMOR] ActorEquipManager not available");
            return true;  // Still return true - item was picked up, just not equipped
        }
        
        RE::ActorEquipManager* equipMgr = *equipMgrPtr;
        
        // Create BGSObjectInstance for the armor
        struct LocalObjectInstance {
            RE::TESForm* object{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
        };
        static_assert(sizeof(LocalObjectInstance) == 0x10);
        
        LocalObjectInstance instance;
        instance.object = armor;
        instance.instanceData = nullptr;
        
        // Equip the armor
        bool equipped = heisenberg::ActorEquipManager_EquipObject(
            equipMgr,
            player,
            reinterpret_cast<RE::BGSObjectInstance*>(&instance),
            0,          // stackID
            1,          // number of items
            nullptr,    // equipSlot - let the game figure it out based on armor type
            true,       // queue equip
            false,      // don't force equip
            true,       // play sounds
            false,      // DON'T apply now - defer to safe game phase to avoid deadlock
            false       // not locked
        );
        
        if (equipped) {
            spdlog::info("[EQUIP-ARMOR] Successfully equipped '{}'!", 
                        armor->GetFullName() ? armor->GetFullName() : "armor");
            
            // Show HUD message
            if (heisenberg::g_config.showHolsterMessages) {
                const char* itemName = armor->GetFullName() ? armor->GetFullName() : "Armor";
                std::string msg = std::format("{} equipped", itemName);
                heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
            }
        } else {
            spdlog::warn("[EQUIP-ARMOR] EquipObject returned false");
        }
        
        return true;  // Return true even if equip failed - item was still picked up
    }
    
    // Check if a reference is a weapon
    bool IsWeapon(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return false;
        
        return baseForm->GetFormType() == RE::ENUM_FORM_ID::kWEAP;
    }
    
    // Check if a reference is ammo
    bool IsAmmo(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm)
            return false;
        
        return baseForm->GetFormType() == RE::ENUM_FORM_ID::kAMMO;
    }
    
    /**
     * Try to reload the equipped weapon with dropped ammo.
     * 
     * @param ammoRefr The ammo reference being dropped
     * @param ammoCount The count of ammo in the stack being dropped
     * @return Number of ammo consumed for reloading (0 if no reload happened)
     */
    int TryReloadWeaponWithAmmo(RE::TESObjectREFR* ammoRefr, int ammoCount)
    {
        if (!ammoRefr || ammoCount <= 0)
            return 0;
        
        auto* player = f4vr::getPlayer();
        if (!player)
            return 0;
        
        // Get the dropped ammo's form
        auto* ammoForm = ammoRefr->GetObjectReference();
        if (!ammoForm || ammoForm->GetFormType() != RE::ENUM_FORM_ID::kAMMO)
            return 0;
        
        RE::TESAmmo* droppedAmmo = static_cast<RE::TESAmmo*>(ammoForm);
        spdlog::debug("[RELOAD] Dropped ammo: '{}' (FormID: {:08X}) x{}",
            droppedAmmo->GetFullName() ? droppedAmmo->GetFullName() : "unknown",
            droppedAmmo->GetFormID(), ammoCount);
        
        // Check if player has a weapon drawn
        if (!player->actorState.IsWeaponDrawn()) {
            spdlog::debug("[RELOAD] No weapon drawn - cannot reload");
            return 0;
        }
        
        // Get the equipped weapon - cast to RE::Actor* for function call
        RE::Actor* playerActor = reinterpret_cast<RE::Actor*>(player);
        RE::TESObjectWEAP* equippedWeapon = heisenberg::GetEquippedWeapon.get()(playerActor, 0);
        if (!equippedWeapon) {
            spdlog::debug("[RELOAD] No weapon equipped");
            return 0;
        }
        
        spdlog::debug("[RELOAD] Equipped weapon: '{}' (FormID: {:08X})",
            equippedWeapon->GetFullName() ? equippedWeapon->GetFullName() : "unknown",
            equippedWeapon->GetFormID());
        
        // Get the weapon's ammo type and capacity from weaponData
        // weaponData is of type TESObjectWEAP::Data which extends InstanceData
        RE::TESAmmo* weaponAmmo = equippedWeapon->weaponData.ammo;
        std::uint16_t magCapacity = equippedWeapon->weaponData.ammoCapacity;
        
        if (!weaponAmmo) {
            spdlog::debug("[RELOAD] Weapon has no ammo type defined");
            return 0;
        }
        
        spdlog::debug("[RELOAD] Weapon ammo type: '{}' (FormID: {:08X}), capacity: {}",
            weaponAmmo->GetFullName() ? weaponAmmo->GetFullName() : "unknown",
            weaponAmmo->GetFormID(), magCapacity);
        
        // Check if ammo types match
        if (droppedAmmo->GetFormID() != weaponAmmo->GetFormID()) {
            spdlog::debug("[RELOAD] Ammo type mismatch - dropped {:08X} != weapon {:08X}",
                droppedAmmo->GetFormID(), weaponAmmo->GetFormID());
            return 0;
        }
        
        spdlog::debug("[RELOAD] Ammo type MATCHES!");
        
        // Get current ammo in magazine using the equip index
        RE::BGSEquipIndex equipIndex;
        heisenberg::Actor_GetWeaponEquipIndex.get()(playerActor, &equipIndex, nullptr);

        float currentAmmo = heisenberg::Actor_GetCurrentAmmoCount.get()(playerActor, equipIndex);
        int currentAmmoInt = static_cast<int>(currentAmmo);
        spdlog::debug("[RELOAD] Current magazine: {}/{}", currentAmmoInt, magCapacity);
        
        // Calculate how much ammo we can add
        int spaceInMag = magCapacity - currentAmmoInt;
        if (spaceInMag <= 0) {
            spdlog::debug("[RELOAD] Magazine already full!");
            return 0;
        }
        
        // Calculate how much to reload (min of space available and ammo in the world ref)
        // World refs typically have 1 ammo, but the activation will add the "real" count
        // For now, we'll reload as much as possible from what we have
        int ammoToAdd = (std::min)(spaceInMag, ammoCount);
        int newAmmoCount = currentAmmoInt + ammoToAdd;
        
        spdlog::debug("[RELOAD] Adding {} rounds (space: {}, available: {})",
            ammoToAdd, spaceInMag, ammoCount);
        
        // Set the new ammo count in the magazine
        heisenberg::Actor_SetCurrentAmmoCount.get()(playerActor, equipIndex, newAmmoCount);
        spdlog::info("[RELOAD] ✓ Reloaded! Magazine now: {}/{}", newAmmoCount, magCapacity);
        
        // Show HUD message
        char msg[128];
        snprintf(msg, sizeof(msg), "Reloaded +%d %s (%d/%d)", 
            ammoToAdd, 
            droppedAmmo->GetFullName() ? droppedAmmo->GetFullName() : "rounds",
            newAmmoCount, magCapacity);
        heisenberg::ShowHUDMessage(msg);
        
        return ammoToAdd;
    }
    
    // Equip weapon by picking it up and then equipping it
    // Returns true if successfully equipped
    bool EquipWeaponItem(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || baseForm->GetFormType() != RE::ENUM_FORM_ID::kWEAP)
            return false;
        
        RE::TESObjectWEAP* weapon = static_cast<RE::TESObjectWEAP*>(baseForm);
        
        spdlog::debug("[EQUIP-WEAPON] Attempting to equip '{}' ({:08X})",
                     weapon->GetFullName() ? weapon->GetFullName() : "unknown",
                     baseForm->GetFormID());
        
        // First, activate the item to pick it up into inventory
        heisenberg::Hooks::SetInternalActivation(true);
        bool activated = refr->ActivateRef(player, nullptr, 1, false, false, false);
        heisenberg::Hooks::SetInternalActivation(false);

        if (!activated) {
            spdlog::warn("[EQUIP-WEAPON] ActivateRef failed for {:08X}", refr->formID);
            return false;
        }
        
        spdlog::debug("[EQUIP-WEAPON] Picked up weapon, now equipping...");
        
        // Now equip it using ActorEquipManager
        RE::ActorEquipManager** equipMgrPtr = heisenberg::g_ActorEquipManager.get();
        if (!equipMgrPtr || !*equipMgrPtr) {
            spdlog::warn("[EQUIP-WEAPON] ActorEquipManager not available");
            return true;  // Still return true - item was picked up, just not equipped
        }
        
        RE::ActorEquipManager* equipMgr = *equipMgrPtr;
        
        // Create BGSObjectInstance for the weapon
        struct LocalObjectInstance {
            RE::TESForm* object{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
        };
        static_assert(sizeof(LocalObjectInstance) == 0x10);
        
        LocalObjectInstance instance;
        instance.object = weapon;
        instance.instanceData = nullptr;
        
        // Get the equip slot from the weapon
        RE::BGSEquipSlot* equipSlot = weapon->GetEquipSlot(nullptr);
        
        // Equip the weapon
        bool equipped = heisenberg::ActorEquipManager_EquipObject(
            equipMgr,
            player,
            reinterpret_cast<RE::BGSObjectInstance*>(&instance),
            0,          // stackID
            1,          // number of items
            equipSlot,  // equip slot from weapon
            true,       // queue equip
            false,      // don't force equip
            true,       // play sounds
            false,      // DON'T apply now - defer to safe game phase to avoid deadlock
            false       // not locked
        );
        
        if (equipped) {
            spdlog::info("[EQUIP-WEAPON] Successfully equipped '{}'!", 
                        weapon->GetFullName() ? weapon->GetFullName() : "weapon");
            
            // Show HUD message
            if (heisenberg::g_config.showHolsterMessages) {
                const char* itemName = weapon->GetFullName() ? weapon->GetFullName() : "Weapon";
                std::string msg = std::format("{} equipped", itemName);
                heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
            }
        } else {
            spdlog::warn("[EQUIP-WEAPON] EquipObject returned false");
        }
        
        return true;  // Return true even if equip failed - item was still picked up
    }
    
    // Pick up weapon and store directly to Virtual Holsters via AddHolster API
    // When VH API supports AddHolster: activates ref → adds to nearest free holster slot
    // Fallback (old VH without AddHolster): activates + equips weapon, user presses holster button
    // The zoneName parameter is for HUD display, holsterIndex is the target slot (0 = auto-find)
    // Returns true if successfully handled
    bool PickupWeaponForHolster(RE::TESObjectREFR* refr, const char* zoneName = nullptr,
                                std::uint32_t holsterIndex = 0)
    {
        if (!refr)
            return false;
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || baseForm->GetFormType() != RE::ENUM_FORM_ID::kWEAP)
            return false;
        
        RE::TESObjectWEAP* weapon = static_cast<RE::TESObjectWEAP*>(baseForm);
        const char* weaponName = refr->GetDisplayFullName();
        if (!weaponName || weaponName[0] == '\0')
            weaponName = weapon->GetFullName() ? weapon->GetFullName() : "unknown";
        
        // Determine if melee (types 0-6 are melee/hand-to-hand, 7+ are ranged)
        auto weaponType = weapon->weaponData.type.get();
        bool isMelee = (weaponType <= RE::WEAPON_TYPE::kStaff);
        
        spdlog::debug("[HOLSTER-PICKUP] Weapon '{}' ({:08X}) type={} isMelee={} targetSlot={}",
                     weaponName, baseForm->GetFormID(), static_cast<int>(weaponType),
                     isMelee, holsterIndex);
        spdlog::default_logger()->flush();
        
        // Try the new VH AddHolster API first
        spdlog::debug("[HOLSTER-PICKUP] Step 1: Getting VH API...");
        spdlog::default_logger()->flush();
        auto* vhApi = VirtualHolsters::RequestVirtualHolstersAPI();
        spdlog::debug("[HOLSTER-PICKUP] Step 2: VH API ptr={}", (void*)vhApi);
        spdlog::default_logger()->flush();
        if (vhApi && vhApi->IsInitialized())
        {
            spdlog::debug("[HOLSTER-PICKUP] Step 3: VH initialized, checking if already holstered...");
            spdlog::default_logger()->flush();
            // Check if weapon is already holstered
            if (vhApi->IsWeaponAlreadyHolstered(weaponName))
            {
                spdlog::debug("[HOLSTER-PICKUP] '{}' is already holstered — picking up normally", weaponName);
                // Fall through to normal pickup below (no holster action)
            }
            else
            {
                spdlog::debug("[HOLSTER-PICKUP] Step 4: Not already holstered, finding slot...");
                spdlog::default_logger()->flush();
                // Auto-find a free holster slot if none specified
                if (holsterIndex == 0)
                {
                    // Use GetCurrentHolster if hand is in a zone, otherwise find first free
                    std::uint32_t currentHolster = vhApi->GetCurrentHolster();
                    if (currentHolster >= 1 && currentHolster <= 7 && vhApi->IsHolsterFree(currentHolster))
                    {
                        holsterIndex = currentHolster;
                        spdlog::debug("[HOLSTER-PICKUP] Using current holster zone: {}", holsterIndex);
                    }
                    else
                    {
                        // Scan for first free slot
                        for (std::uint32_t i = 1; i <= 7; ++i)
                        {
                            if (vhApi->IsHolsterFree(i))
                            {
                                holsterIndex = i;
                                spdlog::debug("[HOLSTER-PICKUP] Auto-selected free holster slot: {}", i);
                                break;
                            }
                        }
                    }
                }
                
                if (holsterIndex == 0)
                {
                    spdlog::debug("[HOLSTER-PICKUP] No free holster slots available");
                    if (heisenberg::g_config.showHolsterMessages)
                        heisenberg::Hooks::ShowHUDMessageDirect("No free holster slots");
                    return false;
                }
                
                spdlog::debug("[HOLSTER-PICKUP] Step 5: Checking if slot {} is free...", holsterIndex);
                spdlog::default_logger()->flush();
                // Verify slot is free
                if (!vhApi->IsHolsterFree(holsterIndex))
                {
                    const char* existing = vhApi->GetHolsteredWeaponName(holsterIndex);
                    spdlog::debug("[HOLSTER-PICKUP] Holster {} already has '{}'", holsterIndex,
                                existing ? existing : "?");
                    if (heisenberg::g_config.showHolsterMessages)
                        heisenberg::Hooks::ShowHUDMessageDirect("Holster slot occupied");
                    return false;
                }
                
                // FIX v0.5.165: Defer AddHolster to next frame to avoid deadlock.
                // VH's displayWeapon does heavy NIF cloning (loadNifFromFile, cloneNode,
                // bone->AttachChild) which DEADLOCKS when called during our EndGrab flow.
                // Solution: Queue the request and process it on the next OnGrabUpdate,
                // after EndGrab has fully completed and all locks are released.
                // The weapon stays in the world (DON'T call ActivateRef yet) so VH can
                // safely access its 3D root node for mesh cloning.
                
                spdlog::debug("[HOLSTER-PICKUP] Step 6: Queuing deferred holster request for next frame...");
                spdlog::default_logger()->flush();
                
                // Get a safe handle to the ref so we can look it up next frame
                RE::ObjectRefHandle refrHandle = refr->GetHandle();
                
                auto& grabMgr = heisenberg::GrabManager::GetSingleton();
                grabMgr.QueueHolsterRequest(refrHandle, holsterIndex, weaponName);
                
                spdlog::debug("[HOLSTER-PICKUP] Step 7: Deferred request queued — will process next frame");
                spdlog::default_logger()->flush();
                
                // Show HUD feedback immediately
                static const char* kHolsterNames[] = {
                    "None", "Left Shoulder", "Right Shoulder", "Left Hip",
                    "Right Hip", "Lower Back", "Left Chest", "Right Chest"
                };
                const char* slotName = (holsterIndex <= 7) ? kHolsterNames[holsterIndex] : "Unknown";
                
                std::string msg = std::format("{} has been holstered on {}", weaponName, slotName);
                if (heisenberg::g_config.showHolsterMessages)
                    heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
                spdlog::debug("[HOLSTER-PICKUP] ✓ '{}' queued for holster {} ({})",
                            weaponName, holsterIndex, slotName);
                return true;  // Request queued — weapon stays in world until next frame
            }
        }
        else
        {
            spdlog::debug("[HOLSTER-PICKUP] VH API not available or not initialized — using legacy mode");
        }
        
        // ── Legacy fallback: pick up + equip (draw), user presses holster button ──
        spdlog::debug("[HOLSTER-PICKUP] Legacy mode: picking up '{}' ({:08X}) for manual holstering",
                     weaponName, baseForm->GetFormID());
        
        heisenberg::Hooks::SetInternalActivation(true);
        bool activated = refr->ActivateRef(player, nullptr, 1, false, false, false);
        heisenberg::Hooks::SetInternalActivation(false);
        if (!activated) {
            spdlog::warn("[HOLSTER-PICKUP] ActivateRef failed for {:08X}", refr->formID);
            return false;
        }
        
        // Equip (draw) so VH sees it as "drawn"
        RE::ActorEquipManager** equipMgrPtr = heisenberg::g_ActorEquipManager.get();
        if (equipMgrPtr && *equipMgrPtr)
        {
            RE::ActorEquipManager* equipMgr = *equipMgrPtr;
            
            struct LocalObjectInstance {
                RE::TESForm* object{ nullptr };
                RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
            };
            
            LocalObjectInstance instance;
            instance.object = weapon;
            instance.instanceData = nullptr;
            
            RE::BGSEquipSlot* equipSlot = weapon->GetEquipSlot(nullptr);
            
            heisenberg::ActorEquipManager_EquipObject(
                equipMgr,
                player,
                reinterpret_cast<RE::BGSObjectInstance*>(&instance),
                0, 1, equipSlot, true, false, true, false, false
            );
            
            spdlog::debug("[HOLSTER-PICKUP] Weapon equipped (drawn) — press holster button now!");
        }
        
        if (heisenberg::g_config.showHolsterMessages) {
            std::string msg;
            if (zoneName) {
                msg = std::format("{} ready - press holster button for {}", weaponName, zoneName);
            } else {
                msg = std::format("{} equipped - press holster button", weaponName);
            }
            heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
        }
        
        return true;
    }
    
    // Consume the grabbed item (eat/drink it)
    // For Fallout 4 VR, we need to use Actor::DrinkPotion which properly
    // triggers all consumption effects (health, rads, addiction, etc.)
    bool ConsumeGrabbedItem(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return false;
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return false;
        
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || !baseForm->IsAlchemyItem())
            return false;
        
        // Cast to AlchemyItem for DrinkPotion
        RE::AlchemyItem* alchemyItem = static_cast<RE::AlchemyItem*>(baseForm);
        
        const char* itemName = alchemyItem->GetFullName() ? alchemyItem->GetFullName() : "item";
        spdlog::debug("[CONSUME] Attempting to consume '{}' ({:08X})",
                     itemName, baseForm->GetFormID());
        
        // The held item came from inventory via DropToHand, so:
        // - Inventory has N items (the original stock minus 1 dropped to hand)
        // - World has 1 item (in hand)
        //
        // APPROACH: We need to consume 1 total. Two options:
        //
        // Option 1: ActivateRef (adds world item to inventory) + DrinkPotion (consumes from inventory)
        //   Problem: DrinkPotion might fail or consume the wrong one
        //
        // Option 2: DrinkPotion (consume from inventory, apply effects) + delete world ref
        //   This is cleaner - the world item is just a visual, inventory is the truth
        //
        // Going with Option 2: Consume from inventory, delete world ref
        
        // Check if player has at least 1 of this item in inventory
        bool hasInInventory = false;
        auto* inventory = player->inventoryList;
        if (inventory) {
            for (auto& item : inventory->data) {
                if (item.object && item.object->GetFormID() == baseForm->GetFormID()) {
                    hasInInventory = true;
                    break;
                }
            }
        }
        
        // DrinkPotion applies alchemy effects (health, rads, addiction, etc.)
        // but does NOT remove the item from inventory. We must remove it explicitly.
        if (!hasInInventory) {
            // No backup in inventory - add the world item to inventory first
            spdlog::debug("[CONSUME] No backup in inventory, using ActivateRef+DrinkPotion path");

            // Mark as recently stored so harvest-to-hand doesn't re-grab after ActivateRef
            heisenberg::DropToHand::GetSingleton().MarkAsRecentlyStored(baseForm->GetFormID());

            heisenberg::Hooks::SetSuppressHUDMessages(true);
            heisenberg::Hooks::SetInternalActivation(true);
            bool activateResult = refr->ActivateRef(player, nullptr, 1, false, false, false);
            heisenberg::Hooks::SetInternalActivation(false);
            heisenberg::Hooks::SetSuppressHUDMessages(false);
            spdlog::debug("[CONSUME] ActivateRef returned {} for {:08X}", activateResult, refr->formID);
        } else {
            // Has backup in inventory - disable world ref, consume from inventory
            spdlog::debug("[CONSUME] Has backup in inventory, consuming from inventory");
            refr->Disable();
            spdlog::debug("[CONSUME] Disabled world reference {:08X}", refr->formID);
        }

        // Apply alchemy effects
        bool drinkResult = heisenberg::Actor_DrinkPotion(player, alchemyItem, 1);
        spdlog::debug("[CONSUME] DrinkPotion returned {} for '{}'", drinkResult, itemName);

        // Only remove from inventory in the ActivateRef path (Case 2).
        // In Case 1 (hasInInventory), the world ref was Disabled — that ref IS the
        // consumed item (already removed from inventory by DropToHand). No extra removal needed.
        // DrinkPotion only applies effects, it does NOT remove from inventory.
        if (!hasInInventory) {
            auto* boundObj = baseForm->As<RE::TESBoundObject>();
            if (boundObj) {
                RE::TESObjectREFR::RemoveItemData removeData(boundObj, 1);
                removeData.reason = RE::ITEM_REMOVE_REASON::kNone;
                heisenberg::Hooks::SetSuppressHUDMessages(true);
                player->RemoveItem(removeData);
                heisenberg::Hooks::SetSuppressHUDMessages(false);
                spdlog::debug("[CONSUME] Removed 1x '{}' from inventory (ActivateRef path)", itemName);
            }
        }
        
        // Play the item's specific consumption sound (e.g., Nuka Cola drinking sound)
        RE::BGSSoundDescriptorForm* consumeSound = alchemyItem->data.consumptionSound;
        if (consumeSound) {
            heisenberg::PlaySoundAtActor(consumeSound, player);
            spdlog::debug("[CONSUME] Playing consumption sound {:08X}", consumeSound->GetFormID());
        }
        
        // Show HUD notification with item name
        if (heisenberg::g_config.showConsumeMessages) {
            std::string msg = std::format("You used {}", itemName);
            spdlog::debug("[CONSUME] Calling ShowHUDMessage with: '{}'", msg);
            heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
        }

        spdlog::info("[CONSUME] Successfully consumed '{}'", itemName);
        
        return true;
    }
    
    // =====================================================================
    // MOUSE SPRING FUNCTIONS (Not used in VR, keeping for reference)
    // =====================================================================
    // PlayerCharacter::CreateMouseSpring(TESObjectREFR*, GrabbingType, float, NiAVObject*, NiPoint3&)
    // VR offset: 0x0f1bf70
    using CreateMouseSpring_t = void(*)(RE::PlayerCharacter*, RE::TESObjectREFR*, int, float, RE::NiAVObject*, RE::NiPoint3&);
    REL::Relocation<CreateMouseSpring_t> CreateMouseSpring{ REL::Offset(0x0f1bf70) };
    
    // PlayerCharacter::DestroyMouseSprings(void)
    // VR offset: 0x0f1d4a0
    using DestroyMouseSprings_t = void(*)(RE::PlayerCharacter*);
    REL::Relocation<DestroyMouseSprings_t> DestroyMouseSprings{ REL::Offset(0x0f1d4a0) };
    
    // hknpBSMouseSpringAction::setWorldPosition(hkVector4f&)
    // VR offset: 0x1e4a960
    using SetSpringPosition_t = void(*)(void*, RE::NiPoint4&);
    REL::Relocation<SetSpringPosition_t> SetSpringPosition{ REL::Offset(0x1e4a960) };

    // Helper to update the mouse spring position
    // Uses PlayerCharacter::grabSprings from CommonLibF4
    void SetMouseSpringPosition(RE::PlayerCharacter* player, const RE::NiPoint3& pos)
    {
        // Access grabSprings directly from PlayerCharacter struct
        auto& grabSprings = player->grabSprings;
        
        spdlog::trace("[GRAB] SetMouseSpringPosition: array size={}, pos=({:.2f}, {:.2f}, {:.2f})",
                     grabSprings.size(), pos.x, pos.y, pos.z);
        
        if (grabSprings.empty())
        {
            return;  // No springs, nothing to update
        }
        
        // Update position on first spring
        // hkRefPtr stores raw pointer, access it directly
        void* spring = reinterpret_cast<void*>(&grabSprings[0]);
        // The actual pointer is the first member of hkRefPtr
        void* springPtr = *reinterpret_cast<void**>(spring);
        if (springPtr)
        {
            // Convert to hkVector4 (w=0)
            RE::NiPoint4 hkPos(pos.x * HAVOK_WORLD_SCALE, pos.y * HAVOK_WORLD_SCALE, 
                               pos.z * HAVOK_WORLD_SCALE, 0.0f);
            spdlog::trace("[GRAB] Calling SetSpringPosition on spring {:p}", spring);
            SetSpringPosition(springPtr, hkPos);
        }
    }
    
    // Helper to check spring count
    std::size_t GetSpringCount(RE::PlayerCharacter* player)
    {
        return player->grabSprings.size();
    }

    // Wrapper functions to expose REL::Relocation functions to heisenberg namespace
    // These now include validation to prevent crashes on invalid collision objects
    void DoSetLinearVelocity(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel)
    {
        if (!IsCollisionObjectValid(obj)) return;
        SetLinearVelocity(obj, vel);
    }
    
    void DoSetAngularVelocity(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel)
    {
        if (!IsCollisionObjectValid(obj)) return;
        SetAngularVelocity(obj, vel);
    }
    
    // =========================================================================
    // SIMPLE VIRTUAL SPRING MODE (from Dec 10 backup - no crashes)
    // This is a simpler velocity-based approach that doesn't use parenting.
    // Object stays DYNAMIC, we apply velocity to move it toward the target.
    // Used for interior cells where the complex parenting approach can fail.
    // =========================================================================
    
    void UpdateVirtualSpringSimple(heisenberg::GrabState& state, const RE::NiPoint3& targetPos,
                                   const RE::NiMatrix3& targetRot, float deltaTime)
    {
        if (!state.collisionObject || !state.node)
            return;
        
        // Store target for release
        state.lastTargetPos = targetPos;
        state.lastTargetRot = targetRot;
        
        // =====================================================================
        // SAFE APPROACH: Visual-only updates (like main KEYFRAMED grab)
        // =====================================================================
        // We're in KEYFRAMED mode, so we should NOT call SetLinearVelocity/
        // SetAngularVelocity. Those functions can crash if the internal body
        // reference is invalid. Instead, just update the visual node directly
        // like the main grab update path does.
        // =====================================================================
        
        // Update the visual node directly (no physics velocity calls!)
        if (state.node)
        {
            RE::NiTransform desiredTransform;
            desiredTransform.translate = targetPos;
            desiredTransform.rotate = targetRot;
            desiredTransform.scale = state.node->world.scale;
            heisenberg::Utils::UpdateKeyframedNode(state.node.get(), desiredTransform);
        }
        
        // Keep TESObjectREFR position in sync (prevents ghosting when walking)
        RE::TESObjectREFR* refr = state.GetRefr();
        if (refr)
        {
            refr->data.location.x = targetPos.x;
            refr->data.location.y = targetPos.y;
            refr->data.location.z = targetPos.z;
        }
        
        // Debug log every ~60 frames
        static int simpleLogCount = 0;
        if (++simpleLogCount >= 60) {
            simpleLogCount = 0;
            RE::NiPoint3 currentPos = state.node->world.translate;
            float errorMagnitude = std::sqrt(
                (targetPos.x - currentPos.x) * (targetPos.x - currentPos.x) +
                (targetPos.y - currentPos.y) * (targetPos.y - currentPos.y) +
                (targetPos.z - currentPos.z) * (targetPos.z - currentPos.z));
            spdlog::debug("[GRAB-REPOSITION] target=({:.1f}, {:.1f}, {:.1f}) error={:.1f}",
                         targetPos.x, targetPos.y, targetPos.z, errorMagnitude);
        }
    }
    
    // EndGrab for KEYFRAMED mode (interiors, DynamicNode, proxy objects)
    // Uses KEYFRAMED during hold, restores to DYNAMIC on release
    void EndGrabKeyframed(heisenberg::GrabState& state, const RE::NiPoint3* throwVelocity, bool isLeft)
    {
        RE::TESObjectREFR* refr = state.GetRefr();
        spdlog::debug("[GRAB-KEYFRAMED] EndGrab: Releasing object {:08X}",
                     refr ? refr->formID : 0);
        
        // CRITICAL: Validate reference via handle lookup BEFORE any method calls!
        if (!state.HasValidRefr())
        {
            spdlog::warn("[GRAB-KEYFRAMED] Object was deleted - clearing state only");
            
            // Reset hand pose and release FRIK override
            auto& frik = heisenberg::FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            
            heisenberg::Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            auto& configMode = heisenberg::ItemPositionConfigMode::GetSingleton();
            configMode.OnGrabEnded(isLeft);
            state.Clear();
            return;
        }
        
        // Verify 3D is still valid/unchanged (refr already cached at top)
        RE::NiAVObject* currentNode = refr ? refr->Get3D() : nullptr;
        if (!currentNode || (state.node && currentNode != state.node.get()))
        {
            spdlog::warn("[GRAB-KEYFRAMED] Object 3D changed - clearing state only");
            
            // Reset hand pose and release FRIK override
            auto& frik = heisenberg::FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            
            heisenberg::Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            auto& configMode = heisenberg::ItemPositionConfigMode::GetSingleton();
            configMode.OnGrabEnded(isLeft);
            state.Clear();
            return;
        }
        
        // Restore hand pose via FRIK and release override
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        frik.ClearHandPoseFingerPositions(isLeft);
        heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);

        // Restore holotape scale to 1.0 (was scaled down on grab)
        if (refr && state.node) {
            auto* baseObj = refr->GetObjectReference();
            if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                state.node->local.scale = 1.0f;
                spdlog::debug("[GRAB] Restored holotape {:08X} scale to 1.0", refr->formID);
            }
        }

        // Restore physics state
        if (state.collisionObject && IsCollisionObjectValid(state.collisionObject))
        {
            // Get bhkWorld - prefer saved, but validate current
            RE::bhkWorld* bhkWorld = state.savedState.savedBhkWorld;
            
            // Validate that the saved bhkWorld is still valid by checking the cell
            if (refr) {
                RE::bhkWorld* currentWorld = GetBhkWorldFromRefr(refr);
                if (currentWorld != bhkWorld) {
                    spdlog::warn("[GRAB-KEYFRAMED] bhkWorld changed during grab! Using current world.");
                    bhkWorld = currentWorld;
                }
            }
            
            if (!bhkWorld) {
                spdlog::error("[GRAB-KEYFRAMED] No valid bhkWorld for physics restoration!");
                // Still try to clean up state
                state.usingMouseSpring = false;
                heisenberg::Heisenberg::GetSingleton().OnGrabEnded(isLeft);
                auto& configMode = heisenberg::ItemPositionConfigMode::GetSingleton();
                configMode.OnGrabEnded(isLeft);
                state.Clear();
                return;
            }
            
            // Restore collision object flags (like HIGGS Skyrim's savedRigidBodyFlags)
            // This must be done BEFORE restoring motion type.
            // If saved flags are 0 (e.g. freshly spawned DropToHand object whose physics
            // hadn't initialized), use 0x8C (ACTIVE | MOVED_THIS_FRAME | typical defaults)
            // to ensure the object participates in physics simulation after release.
            auto restoreFlags = state.savedState.collisionObjectFlags;
            if (restoreFlags == 0) {
                restoreFlags = 0x8C;
                spdlog::debug("[GRAB-KEYFRAMED] Collision flags were 0, using default {:X}", restoreFlags);
            }
            state.collisionObject->flags.flags = restoreFlags;
            spdlog::debug("[GRAB-KEYFRAMED] Restored collision object flags: {:X}", restoreFlags);
            
            // CRITICAL: Sync physics body position to current position BEFORE restoring motion type!
            // Without this, the body can snap back to the original grab position when released quickly.
            // For PROXY objects: sync to the physics node's position (child bone), not the visual root
            RE::NiAVObject* syncNode = state.isProxyCollision && state.physicsNode ? state.physicsNode.get() : state.node.get();
            if (syncNode)
            {
                RE::hkTransformf currentTransform;
                // Build transform from current node world position/rotation
                currentTransform.rotation = syncNode->world.rotate;
                currentTransform.translation = RE::NiPoint4(
                    syncNode->world.translate.x * HAVOK_WORLD_SCALE,
                    syncNode->world.translate.y * HAVOK_WORLD_SCALE,
                    syncNode->world.translate.z * HAVOK_WORLD_SCALE,
                    0.0f
                );
                
                SetTransformLocked(state.collisionObject, currentTransform, bhkWorld);
                spdlog::debug("[GRAB-KEYFRAMED] Synced physics body to {} position ({:.1f}, {:.1f}, {:.1f})",
                             state.isProxyCollision ? "physics node" : "visual",
                             syncNode->world.translate.x, syncNode->world.translate.y, syncNode->world.translate.z);
            }
            
            // Re-validate collision object after SetTransform (it can invalidate the body)
            if (!IsCollisionObjectValid(state.collisionObject))
            {
                spdlog::warn("[GRAB-KEYFRAMED] Collision object became invalid after SetTransform - aborting physics restore");
                state.usingMouseSpring = false;
                heisenberg::Heisenberg::GetSingleton().OnGrabEnded(isLeft);
                auto& configMode = heisenberg::ItemPositionConfigMode::GetSingleton();
                configMode.OnGrabEnded(isLeft);
                state.Clear();
                return;
            }
            
            // CRITICAL: Zero out velocities BEFORE changing motion type!
            // ApplyHardKeyframe sets high velocities to move the body during KEYFRAMED grab.
            // If we don't zero them before restoring DYNAMIC, the object will fly away.
            // We do this WHILE STILL KEYFRAMED because:
            // 1. The body is in a stable state while KEYFRAMED
            // 2. After motion type change, the body ID can become temporarily invalid
            RE::NiPoint4 zeroVel(0.0f, 0.0f, 0.0f, 0.0f);
            SetLinearVelocityLocked(state.collisionObject, zeroVel, bhkWorld);
            SetAngularVelocityLocked(state.collisionObject, zeroVel, bhkWorld);
            spdlog::debug("[GRAB-KEYFRAMED] Zeroed velocities before motion type change");
            
            // Restore motion type to DYNAMIC
            SetMotionTypeLocked(state.collisionObject, RE::hknpMotionPropertiesId::Preset::DYNAMIC, bhkWorld);
            spdlog::debug("[GRAB-KEYFRAMED] Restored motion type to DYNAMIC");
            
            // Re-validate after SetMotionType (motion type change can affect body validity)
            if (!IsCollisionObjectValid(state.collisionObject))
            {
                spdlog::warn("[GRAB-KEYFRAMED] Collision object became invalid after SetMotionType - skipping throw velocity");
                state.usingMouseSpring = false;
                heisenberg::Heisenberg::GetSingleton().OnGrabEnded(isLeft);
                auto& configMode = heisenberg::ItemPositionConfigMode::GetSingleton();
                configMode.OnGrabEnded(isLeft);
                state.Clear();
                return;
            }
            
            // Restore collision layer to saved value - was set to kNonCollidable during grab
            bhkUtilFunctions_SetLayerLocked(state.node.get(), state.savedState.collisionLayer, bhkWorld);
            spdlog::debug("[GRAB-KEYFRAMED] Restored collision layer to saved value ({})", state.savedState.collisionLayer);
            
            // Apply throw velocity if throwing, or a small gravity nudge if dropping
            // We zeroed velocities before motion type change, so now we can set fresh velocity
            // CRITICAL: Even for drops, we must apply a small velocity to WAKE the physics body!
            // Without this, the body stays asleep and floats in mid-air.
            if (throwVelocity && 
                (throwVelocity->x != 0 || throwVelocity->y != 0 || throwVelocity->z != 0))
            {
                // Convert to Havok units and apply
                RE::NiPoint4 hkLinearVel(
                    throwVelocity->x * HAVOK_WORLD_SCALE,
                    throwVelocity->y * HAVOK_WORLD_SCALE,
                    throwVelocity->z * HAVOK_WORLD_SCALE,
                    0.0f
                );
                SetLinearVelocityLocked(state.collisionObject, hkLinearVel, bhkWorld);
                spdlog::debug("[GRAB-KEYFRAMED] Applied throw velocity: ({:.1f},{:.1f},{:.1f})",
                            throwVelocity->x, throwVelocity->y, throwVelocity->z);
            }
            else
            {
                // Apply a tiny downward velocity to wake the physics body
                // This ensures gravity takes over immediately instead of the object floating
                // The velocity is imperceptible but enough to wake the body from sleep
                constexpr float GRAVITY_NUDGE = -0.1f;  // Tiny downward nudge in game units/sec
                RE::NiPoint4 gravityNudge(0.0f, 0.0f, GRAVITY_NUDGE * HAVOK_WORLD_SCALE, 0.0f);
                SetLinearVelocityLocked(state.collisionObject, gravityNudge, bhkWorld);
                spdlog::debug("[GRAB-KEYFRAMED] Drop - applied gravity nudge to wake physics body");
            }
        }
        
        state.usingMouseSpring = false;
        
        // NOTE: Cooldown removed - was preventing immediate re-grab after natural grab release
        // Cooldowns are now only applied for storage zone releases (in EndGrabForStorage)
        
        // Notify Heisenberg that grab ended (starts post-grab kFighting suppression)
        // This prevents Unarmed from auto-equipping when grip is released
        heisenberg::Heisenberg::GetSingleton().OnGrabEnded(isLeft);
        
        // Notify item positioning mode
        auto& configMode = heisenberg::ItemPositionConfigMode::GetSingleton();
        configMode.OnGrabEnded(isLeft);
        
        state.Clear();
    }
}

namespace heisenberg
{
    void TickDeferredDisables()
    {
        TickDeferredDisablesInternal();
    }

    void SafeDisable(RE::TESObjectREFR* refr)
    {
        SafeDisableRef(refr);
    }

    // Cooldown helper methods
    bool GrabManager::IsOnCooldown(std::uint32_t formID) const
    {
        std::scoped_lock lock(_cooldownMutex);
        auto it = _releaseCooldowns.find(formID);
        if (it == _releaseCooldowns.end()) return false;

        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - it->second).count();
        return elapsed < kReleaseCooldown;
    }

    void GrabManager::AddCooldown(std::uint32_t formID)
    {
        std::scoped_lock lock(_cooldownMutex);
        _releaseCooldowns[formID] = std::chrono::steady_clock::now();

        // Lazy cleanup: only when map grows large, avoid per-frame iteration
        if (_releaseCooldowns.size() > 50) {
            auto now = std::chrono::steady_clock::now();
            for (auto it = _releaseCooldowns.begin(); it != _releaseCooldowns.end(); ) {
                float elapsed = std::chrono::duration<float>(now - it->second).count();
                if (elapsed > kReleaseCooldown * 2.0f)
                    it = _releaseCooldowns.erase(it);
                else
                    ++it;
            }
        }
    }

    void GrabManager::CleanupCooldowns()
    {
        std::scoped_lock lock(_cooldownMutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = _releaseCooldowns.begin(); it != _releaseCooldowns.end(); )
        {
            float elapsed = std::chrono::duration<float>(now - it->second).count();
            if (elapsed > kReleaseCooldown * 2.0f)  // Clean up after 2x cooldown time
                it = _releaseCooldowns.erase(it);
            else
                ++it;
        }
    }

    bool GrabManager::StartGrab(const Selection& selection, const RE::NiPoint3& handPos,
                                const RE::NiMatrix3& handRot, bool isLeft, bool skipWeaponEquip)
    {
        // Master toggle check
        if (!g_config.enableGrabbing) {
            return false;
        }
        
        // CRITICAL: Check if this hand already has an active grab
        // This prevents double-grab when DropToHand grabs and Hand proximity also tries to grab
        const auto& currentGrab = isLeft ? _leftGrab : _rightGrab;
        if (currentGrab.active) {
            RE::TESObjectREFR* currentRefr = currentGrab.GetRefr();
            spdlog::debug("[GRAB] {} hand already has active grab on {:08X} - ignoring new grab attempt",
                         isLeft ? "Left" : "Right", currentRefr ? currentRefr->formID : 0);
            return false;
        }
        
        // Check if we're in reposition mode - don't allow ANY new grabs from either hand
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        if (configMode.IsRepositionModeActive()) {
            // Check if EITHER hand has a sticky grab - if so, block all new grabs
            if ((_leftGrab.active && _leftGrab.stickyGrab) || 
                (_rightGrab.active && _rightGrab.stickyGrab)) {
                spdlog::debug("[GRAB] Ignoring {} hand grab attempt - reposition mode active with sticky grab",
                             isLeft ? "left" : "right");
                return false;
            }
        }
        
        // WEAPON CHECK: Block grab on PRIMARY (weapon) hand if a real weapon is equipped
        // Primary = right hand normally, left hand in left-handed mode
        // Storage zone unequip is handled earlier in Hand::OnGrabPressed()
        {
            bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
            bool isPrimaryHand = (isLeftHandedMode ? isLeft : !isLeft);
            if (isPrimaryHand && HasRealWeaponEquipped()) {
                spdlog::debug("[GRAB] Real weapon equipped - blocking grab on primary hand (holster weapon first)");
                return false;
            }
            // If unarmed fists are active on the primary hand, deactivate them so the
            // hand fully opens before wrapping around the grabbed item.
            if (isPrimaryHand) {
                heisenberg::Heisenberg::GetSingleton().DeactivateUnarmedForGrab();
            }
        }
        
        // FAVORITES MENU CHECK: Block grab when FavoritesMenu is open
        // Holstering is done via FavoritesMenu (thumbstick click → grip), so if the
        // FavoritesMenu is open and player presses grip, they're holstering, not grabbing.
        auto& menuChecker = MenuChecker::GetSingleton();
        if (menuChecker.IsFavoritesOpen()) {
            spdlog::debug("[GRAB] FavoritesMenu open - blocking grab (player is holstering)");
            return false;
        }

        // STEALING CHECK: Block grab if picking up this item would be stealing (unless config allows it)
        RE::TESObjectREFR* selRefr = selection.GetRefr();
        if (!g_config.allowGrabbingOwnedItems && selRefr && TESObjectREFR_IsCrimeToActivate(selRefr)) {
            spdlog::debug("[GRAB] Item {:08X} is owned - cannot grab (would be stealing)", selRefr->formID);
            g_vrInput.TriggerHaptic(isLeft, 500);  // Feedback that grab was blocked
            return false;
        }
        
        // POWER ARMOR WEAPON CHECK: Skip grabbing weapons while in Power Armor
        // PA has its own weapon mount system (minigun, gatling laser, etc.) and our grab
        // mechanics interfere with native game behavior, causing infinite loading screens
        if (selRefr && Utils::IsPlayerInPowerArmor()) {
            auto* baseObj = selRefr->data.objectReference;
            if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
                spdlog::debug("[GRAB] Skipping weapon in Power Armor - using native game behavior");
                return false;  // Let native game handle PA weapon pickup
            }
        }
        
        // WEAPON FROM FLOOR: Previously used store-then-grab pattern to prevent quick menu
        // duplication. DISABLED — weapons now follow the same grab rules as all other items.
        // No special teleport-to-hand behavior for weapons.
        // if (!skipWeaponEquip && selRefr) { ... }
        
        // ACTOR CHECK: Block grabbing actors unless hidden easter egg INI setting is enabled
        if (selRefr && selRefr->GetFormType() == RE::ENUM_FORM_ID::kACHR && !g_config.enableGrabActors) {
            return false;
        }

        // FLORA HARVEST TO HAND: If grabbing a flora object, activate it (harvest) instead
        // The harvested item will be routed to hand via DropToHand's harvest event detection
        if (g_config.enableHarvestToHand && selRefr) {
            auto* baseObj = selRefr->data.objectReference;
            if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kFLOR) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    Hooks::SetSuppressHUDMessages(true);
                    Hooks::SetInternalActivation(true);
                    selRefr->ActivateRef(player, nullptr, 1, false, false, false);
                    Hooks::SetInternalActivation(false);
                    Hooks::SetSuppressHUDMessages(false);
                    Hooks::ScheduleDeferredHUDUnsuppress(3);
                    spdlog::debug("[GRAB] Harvested flora {:08X}", selRefr->formID);
                }
                return false;
            }
        }

        // ARMOR/WEAPON HANDLING: Allow all items to be grabbed directly in world
        // No filtering - user can grab any weapon or armor from the ground
        // Offset matching is used for positioning, not for filtering
        
        // Cooldown disabled - allow immediate re-grab
        // if (selection.refr && IsOnCooldown(selection.refr->formID))
        // {
        //     spdlog::info("[GRAB] Item {:08X} is on cooldown, cannot grab yet",
        //                  selection.refr->formID);
        //     return false;
        // }
        // CleanupCooldowns();
        
        // WEAPON HANDLING: Weapons are now held in hand like other items
        // Auto-equip on grab is DISABLED - weapons can be equipped by dropping on body (mode 1) or holster (mode 2)
        // weaponEquipMode: 0=disabled, 1=zone equip, 2=VH zone holster
        
        // Check if we should use constraint-based grabbing (modes 1 or 2) - BROKEN
        int effectiveGrabMode = GetEffectiveGrabMode();
        if (effectiveGrabMode == static_cast<int>(GrabMode::BallSocket) ||
            effectiveGrabMode == static_cast<int>(GrabMode::Motor))
        {
            const char* modeStr = (effectiveGrabMode == 2) ? "MOTOR (6-DOF)" : "BALL-SOCKET";
            spdlog::debug("[GRAB] {} hand: Using {} CONSTRAINT mode", isLeft ? "Left" : "Right", modeStr);
            auto& constraintMgr = ConstraintGrabManager::GetSingleton();
            return constraintMgr.StartConstraintGrab(selection, handPos, handRot, isLeft);
        }

        // Virtual Spring mode (3) or Keyframed mode (0)
        const char* modeStr = (effectiveGrabMode == static_cast<int>(GrabMode::VirtualSpring)) 
                              ? "VIRTUAL-SPRING" : "KEYFRAME";
        spdlog::debug("[GRAB] {} hand: Using {} mode", isLeft ? "Left" : "Right", modeStr);
        
        // Keyframe mode - directly control object position via ApplyHardKeyframe
        GrabState& state = isLeft ? _leftGrab : _rightGrab;
        GrabState& otherState = isLeft ? _rightGrab : _leftGrab;

        // Clear any existing grab on THIS hand
        if (state.active)
        {
            spdlog::debug("[GRAB] {} hand: Clearing existing grab", isLeft ? "Left" : "Right");
            EndGrab(isLeft, nullptr);
        }
        
        // Note: We unequipped weapon earlier if needed (unequippedWeaponForThisGrab),
        // but we don't track it anymore since the player manually re-equips if desired
        
        // Check if the OTHER hand is holding this object - hand-to-hand transfer
        bool isTransfer = false;
        RE::NiPointer<RE::NiNode> transferFromOriginalParent;
        selRefr = selection.GetRefr();
        if (otherState.active && otherState.GetRefr() == selRefr)
        {
            spdlog::info("[GRAB] Hand-to-hand TRANSFER detected! {} -> {} hand",
                         isLeft ? "Right" : "Left", isLeft ? "Left" : "Right");
            isTransfer = true;
            // Save the ORIGINAL world parent before we release from other hand (NiPointer keeps it alive)
            transferFromOriginalParent = otherState.originalParent;
            // Release from other hand (but don't throw)
            RE::NiPoint3 zeroVel(0, 0, 0);
            EndGrab(!isLeft, &zeroVel);  // Release with zero velocity
        }

        if (!selRefr || !selection.node)
        {
            spdlog::warn("[GRAB] {} hand: Invalid selection (refr={}, node={})",
                         isLeft ? "Left" : "Right",
                         selRefr ? "valid" : "null",
                         selection.node ? "valid" : "null");
            return false;
        }

        // Log the BaseFormID for debugging item variations
        auto* baseForm = selRefr->data.objectReference;
        std::uint32_t baseFormID = baseForm ? baseForm->GetFormID() : 0;
        spdlog::debug("[GRAB] {} hand: Grabbing ref {:08X} (BaseFormID={:08X})",
                      isLeft ? "Left" : "Right", selRefr->formID, baseFormID);

        // Store grab info
        state.SetRefr(selRefr);
        state.node = selection.node;  // NiPointer assignment
        state.collisionObject = nullptr;  // Will be set later
        state.initialHandPos = handPos;
        state.initialHandRot = handRot;
        
        // If this was a transfer, preserve the original world parent
        if (isTransfer && transferFromOriginalParent)
        {
            state.originalParent = transferFromOriginalParent;
            spdlog::debug("[GRAB] Transfer: preserving original parent '{}'",
                         transferFromOriginalParent->name.c_str());
        }
        
        // Get collision object first - we need it to detect special cases
        state.collisionObject = GetCollisionObject(selRefr);
        
        // Diagnostic logging for collision object
        if (state.collisionObject) {
            RE::NiAVObject* sceneObj = state.collisionObject->sceneObject;
            spdlog::debug("[GRAB] CollisionObject for {:08X}: sceneObject='{}', node='{}'",
                selRefr->formID,
                sceneObj ? sceneObj->name.c_str() : "NULL",
                state.node ? state.node->name.c_str() : "NULL");
            
            // Check if physics is on a different node than the grabbed root
            // This happens with:
            // 1. Ragdolls (Jangles) - physics on child bone like c_Torso
            // 2. Objects with __DummyRootNode - physics on child mesh like ToyTruck01
            // In BOTH cases: keep state.node as the ROOT, mark as proxy, physics will follow
            // when we move the root because sceneObject is a child of root.
            if (sceneObj && state.node && sceneObj != state.node.get()) {
                spdlog::debug("[GRAB] PROXY collision detected: physics on '{}', visual root '{}'",
                    sceneObj->name.c_str(), state.node->name.c_str());
                state.isProxyCollision = true;  // Mark for special physics handling
                state.physicsNode.reset(sceneObj);   // Store the physics node for later
            } else {
                state.physicsNode = state.node;  // Normal case: physics is on grabbed node (NiPointer copy)
            }
        } else {
            spdlog::warn("[GRAB] No collision object for {:08X}! Node='{}'",
                selRefr->formID, state.node ? state.node->name.c_str() : "NULL");
            state.physicsNode = state.node;  // NiPointer copy
        }
        
        // ═══════════════════════════════════════════════════════════════════════════
        // KEYFRAMED MODE - Always use KEYFRAMED for collision support
        // For proxy objects, we'll handle physics specially in the update loop
        // ═══════════════════════════════════════════════════════════════════════════
        state.usingKeyframedMode = true;
        spdlog::debug("[GRAB-KEYFRAMED] Using KEYFRAMED mode for collision support");
        
        // Reset room tracking state for keyframed mode (prevents jitter from stale data)
        state.lastRoomPos = RE::NiPoint3(0, 0, 0);
        state.smoothedRoomDelta = RE::NiPoint3(0, 0, 0);
        state.roomTrackingInitialized = false;
        
        // For VR grabbing - get object's current world transform
        RE::NiTransform worldTransform = selection.node->world;
        RE::NiPoint3 objectPos = worldTransform.translate;
        state.initialObjectPos = objectPos;  // Save for pull animation
        
        // Get bounding sphere radius for surface distance calculation
        float boundRadius = 10.0f;  // Default
        if (auto* asNode = selection.node->IsNode()) {
            boundRadius = asNode->worldBound.fRadius;
            if (boundRadius < 1.0f) boundRadius = 1.0f;  // Minimum 1cm
        }
        
        // Calculate distance from hand to object CENTER and SURFACE
        // forceOffset (DropToHand) uses a sentinel value (99999.0) to force offset loading
        float distToCenter;
        bool forceUseOffset = (selection.distance > 99998.0f);  // 99999.0 sentinel set by StartGrabOnRef with forceOffset
        if (forceUseOffset) {
            distToCenter = 100.0f;  // Use a reasonable fake distance for offset loading calculations
        } else {
            distToCenter = (objectPos - handPos).Length();  // Distance to center
        }
        
        // Calculate distance to SURFACE (subtract bounding radius)
        // This is what matters for natural grab - are we touching the object?
        float distToSurface = distToCenter - boundRadius;
        if (distToSurface < 0.0f) distToSurface = 0.0f;  // Hand is inside bounding sphere = touching
        
        // Use surface distance for NATURAL GRAB decision (hand touching object)
        // Use center distance for PALM SNAP decision (pointing at object from distance)
        float distToObject = distToSurface;
        
        spdlog::debug("[GRAB] Distance: center={:.1f}cm surface={:.1f}cm boundR={:.1f}cm (natural if <= {:.1f}cm)",
                     distToCenter, distToSurface, boundRadius, heisenberg::g_config.naturalGrabDistance);
        
        // =========================================================================
        // TWO-TIER GRAB DISTANCE SYSTEM:
        // Tier 1: 0 to naturalGrabDistance (0-7cm default) = NATURAL GRAB / TELEKINESIS
        //         Object stays where grabbed, moves with hand from that point
        //         (telekinesis mode if enableNaturalGrab is on)
        // Tier 2: Beyond naturalGrabDistance = PALM SNAP
        //         Object snaps to palm center using saved offsets or calculated position
        //         No maximum distance limit — any selected object can be grabbed.
        // 
        // IMPORTANT: Items without an exact offset match (name or FormID) use the extended
        // natural grab distance (naturalGrabDistanceNoMatch) to avoid applying potentially
        // bad fuzzy/partial matches from dimension-based fallback matching.
        // =========================================================================
        
        // Check if item has an exact offset match (by FormID or name)
        auto& offsetMgr = ItemOffsetManager::GetSingleton();
        bool hasExactOffsetMatch = offsetMgr.HasExactMatch(selRefr);
        
        // Use extended natural grab distance for items without exact offset match
        // This prevents bad fuzzy matches (e.g., "Tire Iron" matching "Tire") from being applied
        // Telekinesis range — set by MCM slider, applies to ALL objects
        float naturalGrabThreshold = heisenberg::g_config.naturalGrabDistance;
        
        // Cache item name once for all logging in this function
        std::string itemName = ItemOffsetManager::GetItemName(selRefr);

        if (!hasExactOffsetMatch) {
            spdlog::debug("[GRAB] No exact offset match for '{}' - using extended natural grab distance ({:.1f}cm)",
                         itemName, naturalGrabThreshold);
        }

        bool isNaturalGrab = (distToObject <= naturalGrabThreshold);
        bool isPalmSnap = !isNaturalGrab;  // Everything beyond natural grab range = palm snap (no max limit)

        // DEBUG: Log distance calculation to diagnose natural grab issues
        spdlog::debug("[GRAB] Distance check for '{}': distToObject={:.1f}cm, naturalThresh={:.1f}cm, hasExactMatch={}, isNatural={}, isPalmSnap={}",
                    itemName, distToObject, naturalGrabThreshold, hasExactOffsetMatch, isNaturalGrab, isPalmSnap);

        // =========================================================================
        // GRAB MODE DECISION:
        //
        // Two modes only:
        // 1. Within naturalGrabDistance: telekinesis (object follows hand)
        // 2. Beyond naturalGrabDistance: rejected (too far)
        // (DropToHand bypass always uses instant snap)
        // =========================================================================
        bool isRemoteSelection = (selection.distance > g_config.snapDistance);
        bool useTelekinesis = false;  // Will this grab use telekinesis (follow-hand) mode?
        bool usePullToHand = false;   // Will this grab pull the object to the hand?
        
        if (forceUseOffset) {
            // DropToHand — instant snap to hand (always works regardless of settings)
            isPalmSnap = true;
            isNaturalGrab = false;
            spdlog::debug("[GRAB] DropToHand '{}' at {:.1f}cm — instant snap",
                        itemName, distToObject);
        } else if (heisenberg::g_config.enableNaturalGrab && isNaturalGrab) {
            // Telekinesis ON + within telekinesis range → object follows hand at distance
            useTelekinesis = true;
            spdlog::debug("[GRAB] TELEKINESIS for '{}' at {:.1f}cm (range={:.1f}cm)",
                        itemName, distToObject, naturalGrabThreshold);
        } else if (heisenberg::g_config.enablePalmSnap) {
            // Pull ON + (telekinesis OFF or beyond telekinesis range) → pull/snap to hand
            if (isRemoteSelection) {
                // Selected from distance — animate pull to hand
                usePullToHand = true;
            }
            // Either way, object ends up at palm snap position
            isPalmSnap = true;
            isNaturalGrab = false;
            spdlog::debug("[GRAB] {} for '{}': surfaceDist={:.1f}cm, selectionDist={:.1f}",
                        usePullToHand ? "PULL TO HAND" : "PALM SNAP",
                        itemName, distToObject, selection.distance);
        } else {
            // Both Pull and Telekinesis off (or telekinesis on but beyond range with pull off)
            spdlog::debug("[GRAB] REJECTED '{}' at {:.1f}cm — grab disabled (pull={}, telekinesis={}, inRange={})",
                        itemName, distToObject,
                        heisenberg::g_config.enablePalmSnap,
                        heisenberg::g_config.enableNaturalGrab,
                        isNaturalGrab);
            return false;
        }
        
        // (DropToHand handled in mode decision above)
        
        // Check if this is a Moveable Static (MSTT) or holotape (NOTE)
        // MSTT items have special distance rules:
        // - 0-10cm: Natural grab (always allowed)
        // - 10-20cm: Palm snap ONLY if has exact offset match
        // - Beyond 20cm: Rejected
        // NOTE items (holotapes) always use palm snap with __NOTE_DEFAULT offset
        bool isMSTT = false;
        bool isHolotape = false;
        if (auto* baseObj = selRefr ? selRefr->GetObjectReference() : nullptr) {
            isMSTT = (baseObj->GetFormType() == RE::ENUM_FORM_ID::kMSTT);
            isHolotape = (baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE);
        }

        // Holotapes always use palm snap with the shared NOTE_DEFAULT offset
        // regardless of pickup distance — they're all the same shape
        if (isHolotape && !forceUseOffset) {
            isNaturalGrab = false;
            isPalmSnap = true;
            useTelekinesis = false;
            spdlog::debug("[GRAB] Holotape '{}' at {:.1f}cm — forcing palm snap for consistent offset",
                        itemName, distToObject);
        }
        if (isMSTT && !isRemoteSelection) {
            constexpr float MSTT_NATURAL_GRAB_MAX = 10.0f;   // 0-10cm = natural grab
            constexpr float MSTT_PALM_SNAP_MAX = 20.0f;      // 10-20cm = palm snap (if exact match)

            if (distToObject <= MSTT_NATURAL_GRAB_MAX) {
                // Within 10cm - natural grab
                isNaturalGrab = true;
                isPalmSnap = false;
            } else if (distToObject <= MSTT_PALM_SNAP_MAX && hasExactOffsetMatch) {
                // 10-20cm with exact offset - allow palm snap
                isNaturalGrab = false;
                isPalmSnap = true;
                spdlog::debug("[GRAB] MSTT '{}' at {:.1f}cm - palm snap allowed (has exact offset)",
                            itemName, distToObject);
            } else {
                // Beyond allowed range or no exact match for palm snap
                spdlog::debug("[GRAB] REJECTED: MSTT '{}' at {:.1f}cm (max={:.1f}cm, hasExactMatch={})",
                            itemName, distToObject,
                            hasExactOffsetMatch ? MSTT_PALM_SNAP_MAX : MSTT_NATURAL_GRAB_MAX,
                            hasExactOffsetMatch);
                return false;
            }
        }
        
        // Load item-specific offset if available (with dimension-based fallback matching)
        // Only load for PALM SNAP mode (Tier 2) - natural grabs ignore offsets
        // Note: offsetMgr already obtained above for HasExactMatch check
        std::optional<ItemOffset> customOffset = std::nullopt;
        
        if (isMSTT && isNaturalGrab) {
            spdlog::debug("[GRAB] NATURAL GRAB for MSTT '{}': dist={:.1f}cm (0-10cm range)",
                        itemName, distToObject);
        } else if (isMSTT && isPalmSnap) {
            // MSTT palm snap - MUST load exact offset (no dimension-based fallback for MSTT)
            customOffset = offsetMgr.GetExactOffset(selRefr, isLeft);
            if (customOffset.has_value()) {
                spdlog::debug("[GRAB] PALM SNAP for MSTT '{}': dist={:.1f}cm, using exact {} hand offset",
                            itemName, distToObject, isLeft ? "LEFT" : "RIGHT");
            } else {
                // No exact offset - this shouldn't happen since hasExactOffsetMatch was true
                // But if it does, reject the grab
                spdlog::warn("[GRAB] REJECTED: MSTT '{}' at {:.1f}cm - no exact offset found (only natural grab allowed)",
                            itemName, distToObject);
                return false;
            }
        } else if (isNaturalGrab) {
            spdlog::debug("[GRAB] NATURAL GRAB for '{}': dist={:.1f}cm <= {:.1f}cm threshold",
                        itemName, distToObject, naturalGrabThreshold);
        } else if (isPalmSnap && (heisenberg::g_config.enablePalmSnap || isHolotape)) {
            customOffset = offsetMgr.GetOffsetWithFallback(selRefr, isLeft);
            if (customOffset.has_value()) {
                spdlog::debug("[GRAB] PALM SNAP for '{}': dist={:.1f}cm, using {} hand offset",
                            itemName, distToObject, isLeft ? "LEFT" : "RIGHT");
            } else {
                spdlog::debug("[GRAB] PALM SNAP for '{}': dist={:.1f}cm, no offset found (will calculate)",
                            itemName, distToObject);
            }
        } else {
            spdlog::debug("[GRAB] Natural grab for '{}': palmSnap disabled, dist={:.1f}cm",
                        itemName, distToObject);
        }
        
        // For the rest of the code, withinSnapDistance means "use natural grab"
        bool withinSnapDistance = isNaturalGrab;
        
        // Track natural grab state for potential offset saving on release
        state.isNaturalGrab = isNaturalGrab;
        
        // Get wand node to calculate offset relative to it
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        RE::NiNode* wandNode = nullptr;
        if (playerNodes)
        {
            wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        }

        if (customOffset.has_value())
        {
            // Use saved profile offset
            state.itemOffset = customOffset.value();
            state.hasItemOffset = true;
            
            // Track if this is a FRIK-style offset (needs Weapon node parent transform)
            state.isFRIKOffset = state.itemOffset.isFRIKOffset;
            
            // Log offset usage at debug level
            spdlog::debug("[GRAB] USING OFFSET for '{}': pos=({:.2f}, {:.2f}, {:.2f}) isRightHandSpace={} isFRIK={}",
                        itemName,
                        state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z,
                        state.itemOffset.isRightHandSpace, state.isFRIKOffset);
            
            // Determine if mirroring is needed based on offset coordinate system vs grab hand
            // - FRIK offsets (isRightHandSpace=true): designed for RIGHT hand
            // - Original offsets (isRightHandSpace=false): designed for LEFT hand
            //
            // CRITICAL: Check if the LOADED offset is already a hand-specific variant!
            // The offset may have come from partial name matching (e.g., "Security Baton" -> "Baton_L")
            // In that case, state.itemOffset.isLeftHanded or matchedName ending with _L indicates
            // we already have the correct hand-specific offset loaded.
            bool needsMirroring = false;
            
            // Check if the loaded offset is already a left-hand specific variant
            bool loadedOffsetIsLeftHanded = state.itemOffset.isLeftHanded || 
                (state.itemOffset.matchedName.length() > 2 && 
                 state.itemOffset.matchedName.substr(state.itemOffset.matchedName.length() - 2) == "_L");
            
            // Check if the loaded offset is already a right-hand specific variant  
            bool loadedOffsetIsRightHanded = 
                (state.itemOffset.matchedName.length() > 2 && 
                 state.itemOffset.matchedName.substr(state.itemOffset.matchedName.length() - 2) == "_R");
            
            spdlog::debug("[GRAB] Mirror check: isLeft={} isRightHandSpace={} loadedIsLeftHanded={} loadedIsRightHanded={} matchedName='{}'",
                         isLeft, state.itemOffset.isRightHandSpace, loadedOffsetIsLeftHanded, loadedOffsetIsRightHanded,
                         state.itemOffset.matchedName);
            
            // Re-enabled mirroring logic
            if (state.itemOffset.isRightHandSpace) {
                // FRIK offset (right hand space) - mirror when grabbing with LEFT hand
                // BUT don't mirror if we already have a left-hand specific offset loaded
                needsMirroring = isLeft && !loadedOffsetIsLeftHanded;
            } else {
                // Original offset (left hand space) - mirror when grabbing with RIGHT hand
                // BUT don't mirror if we already have a right-hand specific offset loaded
                needsMirroring = !isLeft && !loadedOffsetIsRightHanded;
            }
            spdlog::debug("[GRAB] Mirroring decision: needsMirroring={}", needsMirroring);
            
            if (needsMirroring) {
                if (state.itemOffset.isRightHandSpace) {
                    // FRIK offset mirroring: based on FRIK's getMeleeWeaponDefaultAdjustment()
                    // FRIK uses different values for left-handed mode, not simple axis negation:
                    // - Z position is negated (not X!)
                    // - Y rotation is negated
                    // This matches the VR coordinate system where left/right hands are physically mirrored
                    state.itemOffset.position.z = -state.itemOffset.position.z;
                    
                    // Mirror rotation around Y axis (negate yaw)
                    // This is: R_mirrored = diag(1, -1, 1) * R * diag(1, -1, 1)
                    // Which negates: entry[0][1], entry[1][0], entry[1][2], entry[2][1]
                    state.itemOffset.rotation.entry[0][1] = -state.itemOffset.rotation.entry[0][1];
                    state.itemOffset.rotation.entry[1][0] = -state.itemOffset.rotation.entry[1][0];
                    state.itemOffset.rotation.entry[1][2] = -state.itemOffset.rotation.entry[1][2];
                    state.itemOffset.rotation.entry[2][1] = -state.itemOffset.rotation.entry[2][1];
                    
                    spdlog::debug("[GRAB] FRIK-style mirror for LEFT hand: Z negated, Y-rot negated: pos=({:.2f}, {:.2f}, {:.2f})",
                                 state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z);
                } else {
                    // Original offset mirroring: negate X and mirror rotation across YZ plane
                    state.itemOffset.position.x = -state.itemOffset.position.x;
                    
                    // Mirror rotation: reflect across YZ plane
                    // R_mirrored = diag(-1, 1, 1) * R * diag(-1, 1, 1)
                    // Which negates: entry[0][1], entry[0][2], entry[1][0], entry[2][0]
                    state.itemOffset.rotation.entry[0][1] = -state.itemOffset.rotation.entry[0][1];
                    state.itemOffset.rotation.entry[0][2] = -state.itemOffset.rotation.entry[0][2];
                    state.itemOffset.rotation.entry[1][0] = -state.itemOffset.rotation.entry[1][0];
                    state.itemOffset.rotation.entry[2][0] = -state.itemOffset.rotation.entry[2][0];
                    
                    spdlog::debug("[GRAB] Original-style mirror for RIGHT hand: X negated: pos=({:.2f}, {:.2f}, {:.2f})",
                                 state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z);
                }
            }
            
            spdlog::debug("[GRAB] Using custom offset for '{}': pos=({:.2f}, {:.2f}, {:.2f}) rot[0]=({:.2f},{:.2f},{:.2f})",
                         itemName,
                         state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z,
                         state.itemOffset.rotation[0][0], state.itemOffset.rotation[0][1], state.itemOffset.rotation[0][2]);
            // Log object's current world rotation for debugging
            spdlog::debug("[GRAB] Object current world rot[0]=({:.2f},{:.2f},{:.2f})",
                         worldTransform.rotate.entry[0][0], worldTransform.rotate.entry[0][1], worldTransform.rotate.entry[0][2]);
        }
        else if (wandNode)
        {
            // No profile - use palm snap or natural grab based on config and distance
            state.itemOffset = ItemOffset();  // Start fresh
            
            // Get item dimensions first (needed for positioning calculations)
            float itemLength = 0, itemWidth = 0, itemHeight = 0;
            ItemOffsetManager::GetItemDimensions(selRefr, itemLength, itemWidth, itemHeight);
            
            // distToObject and withinSnapDistance already calculated above
            
            if (heisenberg::g_config.enablePalmSnap && !withinSnapDistance)
            {
                // PALM SNAP: Position object as close to palm as possible without touching
                // Only triggers when object is far (>50cm), otherwise use natural grab
                
                // Palm is roughly at wand origin, fingers extend in +Y direction
                // We want object centered in palm with minimal clearance
                constexpr float palmClearance = 1.0f;  // 1cm minimum gap from palm surface
                
                // Position object center just in front of palm
                // Y = forward (toward fingers): object back edge should be palmClearance from palm
                float yOffset = palmClearance + (itemLength * 0.5f);
                
                // Z = up: center object vertically at palm level (slightly above wand)
                float zOffset = 3.0f;  // Palm is roughly 3cm above wand center
                
                // X = left/right: centered
                float xOffset = 0.0f;
                
                state.itemOffset.position = RE::NiPoint3(xOffset, yOffset, zOffset);
                
                spdlog::debug("[GRAB] PALM SNAP for '{}': dist={:.1f}cm, dims=({:.1f}x{:.1f}x{:.1f}), offset=({:.2f}, {:.2f}, {:.2f})",
                             itemName,
                             distToObject, itemLength, itemWidth, itemHeight,
                             state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z);
                
                // Keep object's original rotation relative to world, then convert to wand-local
                state.itemOffset.rotation = worldTransform.rotate * wandNode->world.rotate.Transpose();
                
                state.hasItemOffset = false;
                state.usedSnapMode = true;  // Used snap positioning - will open hand fully
            }
            else
            {
                // NATURAL GRAB: Object is close (<50cm) or snap disabled
                // Calculate offset that PRESERVES object's current world position
                // Formula from FRIK: local = parent.rotate * (world - parent.translate)
                RE::NiPoint3 worldOffset = objectPos - wandNode->world.translate;
                state.itemOffset.position = wandNode->world.rotate * worldOffset;
                
                // Keep object's current rotation
                // Formula: local.rotate = world.rotate * parent.rotate^T
                state.itemOffset.rotation = worldTransform.rotate * wandNode->world.rotate.Transpose();
                
                state.hasItemOffset = false;
                state.usedSnapMode = false;  // Natural grab - will curl fingers around object
                spdlog::debug("[GRAB] NATURAL grab for '{}': dist={:.1f}cm, PRESERVE offset=({:.2f}, {:.2f}, {:.2f})",
                             itemName,
                             distToObject,
                             state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z);
                spdlog::debug("[GRAB] NATURAL grab offset rotation[0]=({:.2f},{:.2f},{:.2f})",
                             state.itemOffset.rotation[0][0], state.itemOffset.rotation[0][1], state.itemOffset.rotation[0][2]);
            }
        }
        else
        {
            // Fallback if wand node not found
            state.itemOffset = offsetMgr.GetDefaultOffset();
            state.hasItemOffset = false;
            state.usedSnapMode = false;
            spdlog::debug("[GRAB] Wand node not found, using default offset");
        }

        // Get item dimensions from bounding box (for smart positioning)
        ItemOffsetManager::GetItemDimensions(selRefr, 
                                              state.itemOffset.length, 
                                              state.itemOffset.width, 
                                              state.itemOffset.height);
        spdlog::debug("[GRAB] Item dimensions: {:.1f} x {:.1f} x {:.1f}",
                      state.itemOffset.length, state.itemOffset.width, state.itemOffset.height);
        
        // Log input values for debugging
        spdlog::debug("[GRAB] StartGrab: objectPos=({:.1f}, {:.1f}, {:.1f}) handPos=({:.1f}, {:.1f}, {:.1f})",
                     objectPos.x, objectPos.y, objectPos.z,
                     handPos.x, handPos.y, handPos.z);
        
        // distToObject already calculated above when determining snap mode

        // Apply grab mode decided above
        if (useTelekinesis)
        {
            // Telekinesis: object follows hand from its current world position
            // No pull, no snap — object maintains its world-space offset from hand
            // Ignore any stored item offsets — always compute fresh from world positions
            state.isPulling = false;
            state.grabOffsetLocal = objectPos - handPos;
            RE::NiMatrix3 handRot = wandNode ? wandNode->world.rotate : RE::NiMatrix3();
            // Compute hand-local offset that preserves the object's current world position
            state.itemOffset = ItemOffset();  // Clear any stored offset completely
            state.itemOffset.position = handRot * state.grabOffsetLocal;
            // Preserve object's current world rotation (convert to hand-local)
            state.itemOffset.rotation = worldTransform.rotate * handRot.Transpose();
            state.hasItemOffset = false;  // Not a stored offset
            state.isFRIKOffset = false;
            state.isNaturalGrab = true;
            state.isTelekinesis = true;
            
            // Force hand fully open during telekinesis grab
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.IsAvailable()) {
                frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                spdlog::debug("[GRAB] Telekinesis - hand forced fully open");
            }
            
            spdlog::debug("[GRAB] StartGrab: TELEKINESIS — object stays at {:.1f}cm distance (stored offsets ignored)", distToObject);
        }
        else if (usePullToHand)
        {
            // Pull-to-hand: animate object from current world position toward palm snap target
            state.isPulling = true;
            state.pullProgress = 0.0f;
            state.grabOffsetLocal = objectPos - handPos;
            float pullDistance = (objectPos - handPos).Length();
            spdlog::debug("[GRAB] StartGrab: PULLING object from {:.1f}cm (center-to-hand) selDist={:.1f}",
                        pullDistance, selection.distance);
        }
        else
        {
            // Close grab or palm snap — use item offset immediately
            state.isPulling = false;
            state.grabOffsetLocal = state.itemOffset.position;
            spdlog::debug("[GRAB] StartGrab: using item offset directly (dist={:.1f}cm)", distToObject);
        }
        
        // Store initial object rotation (object will keep this rotation)
        state.grabRotationLocal = worldTransform.rotate;
        
        spdlog::debug("[GRAB] StartGrab: worldOffset=({:.2f}, {:.2f}, {:.2f})",
                     state.grabOffsetLocal.x, state.grabOffsetLocal.y, state.grabOffsetLocal.z);

        // VR doesn't use mouse springs (they stay at 0 even with native A/X grab)
        // Instead, we'll directly update the reference position each frame
        
        spdlog::debug("[GRAB] {} hand: Grabbed {:08X} (node={}) - will use direct position updates",
                     isLeft ? "Left" : "Right", 
                     selRefr->formID,
                     selection.node ? selection.node->name.c_str() : "null");

        // Scale holotapes for hand size. All holotapes use the same __NOTE_DEFAULT offset.
        // Hologram_* game variants have smaller NIFs than standard Holotape01 — scale up to match.
        if (auto* baseObj = selRefr->GetObjectReference()) {
            if (baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE && state.node) {
                const float pipScale = PipboyInteraction::GetSingleton().GetFrikPipboyScale();
                constexpr float BASE_HAND_SCALE = 0.7f;
                float finalScale = BASE_HAND_SCALE * pipScale;

                std::string_view nodeName(state.node->name.c_str());
                const auto& bd = baseObj->boundData;
                float bx = static_cast<float>(bd.boundMax.x - bd.boundMin.x);
                float by = static_cast<float>(bd.boundMax.y - bd.boundMin.y);
                float bz = static_cast<float>(bd.boundMax.z - bd.boundMin.z);
                float wbr = state.node->worldBound.fRadius;
                spdlog::debug("[GRAB] Holotape {:08X} '{}' node='{}' bound=({:.0f},{:.0f},{:.0f}) wbRadius={:.2f} worldScale={:.3f}",
                            selRefr->formID, itemName, nodeName, bx, by, bz, wbr, state.node->world.scale);

                if (nodeName.find("Hologram") != std::string_view::npos) {
                    constexpr float HOLOGRAM_COMPENSATION = 1.9f;
                    finalScale *= HOLOGRAM_COMPENSATION;
                    spdlog::debug("[GRAB] Hologram variant → scale={:.2f} (compensation={}x)", finalScale, HOLOGRAM_COMPENSATION);
                } else {
                    spdlog::debug("[GRAB] Standard holotape → scale={:.2f}", finalScale);
                }

                state.node->local.scale = finalScale;
            }
        }

        // DISABLED: Automatic finger curl - now controlled by thumbstick click
        // The player can use left thumbstick click (open) and right thumbstick click (close)
        // to manually control finger positions
        /*
        auto& frik = FRIKInterface::GetSingleton();
        if (state.usedSnapMode)
        {
            // SNAP MODE: Open hand fully (1.0 = fully extended fingers)
            // Object is positioned below fingers with clearance
            if (frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f))
            {
                spdlog::debug("[GRAB] Snap mode - hand fully open (1.0)");
            }
        }
        else
        {
            // NATURAL GRAB: Calculate finger curl based on item dimensions
            auto fingerCurl = CalculateFingerCurl(
                state.itemOffset.length, 
                state.itemOffset.width, 
                state.itemOffset.height,
                0.1f  // Finger offset to prevent clipping
            );
            
            if (frik.SetHandPoseFingerPositions(isLeft, fingerCurl.thumb, fingerCurl.index, 
                                                 fingerCurl.middle, fingerCurl.ring, fingerCurl.pinky))
            {
                spdlog::debug("[GRAB] Applied finger curl: thumb={:.2f}, index={:.2f}, middle={:.2f}, ring={:.2f}, pinky={:.2f}",
                             fingerCurl.thumb, fingerCurl.index, fingerCurl.middle, fingerCurl.ring, fingerCurl.pinky);
                spdlog::debug("[GRAB] Item dimensions: {:.1f}x{:.1f}x{:.1f}", 
                             state.itemOffset.length, state.itemOffset.width, state.itemOffset.height);
            }
        }
        */
        
        // Apply saved finger curls if available in the offset
        // (Skip for telekinesis grabs — hand stays fully open)
        spdlog::debug("[GRAB-FINGERS] '{}' {:08X}: isTelekinesis={}, hasItemOffset={}, hasFingerCurls={}, thumbCurl={:.2f}",
                     itemName, selRefr->formID, state.isTelekinesis, state.hasItemOffset,
                     state.itemOffset.hasFingerCurls, state.itemOffset.thumbCurl);
        if (!state.isTelekinesis && state.hasItemOffset && state.itemOffset.hasFingerCurls)
        {
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.IsAvailable())
            {
                if (state.isPulling) {
                    // During pull: hand stays fully open until object arrives
                    frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                    state.pendingFingerCurls = true;  // Apply curls when pull completes
                    spdlog::debug("[GRAB-FINGERS] DEFERRED finger curls (pulling): thumb={:.2f}", state.itemOffset.thumbCurl);
                }
                else if (frik.SetHandPoseFingerPositions(isLeft,
                    state.itemOffset.thumbCurl, state.itemOffset.indexCurl,
                    state.itemOffset.middleCurl, state.itemOffset.ringCurl, state.itemOffset.pinkyCurl))
                {
                    spdlog::debug("[GRAB-FINGERS] APPLIED finger curls: thumb={:.2f}, index={:.2f}, middle={:.2f}, ring={:.2f}, pinky={:.2f}",
                                 state.itemOffset.thumbCurl, state.itemOffset.indexCurl,
                                 state.itemOffset.middleCurl, state.itemOffset.ringCurl, state.itemOffset.pinkyCurl);

                    // Also update Heisenberg's pose value so thumbstick adjustments start from saved value
                    Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, state.itemOffset.thumbCurl);
                }
                else
                {
                    spdlog::warn("[GRAB-FINGERS] SetHandPoseFingerPositions FAILED for '{}'", itemName);
                }
            }
            else
            {
                spdlog::warn("[GRAB-FINGERS] FRIK not available for finger curls on '{}'", itemName);
            }
        }
        else
        {
            spdlog::debug("[GRAB-FINGERS] SKIPPED finger curls for '{}' (check failed)", itemName);
        }
        
        state.usingMouseSpring = false;  // Not using mouse springs
        state.active = true;
        
        spdlog::debug("[GRAB] {} hand: StartGrab SUCCESS for {:08X} (isPulling={}, dist={:.1f})",
                     isLeft ? "Left" : "Right",
                     selRefr->formID,
                     state.isPulling, distToObject);
        
        // Enable sticky grab if configured (press grip once to grab, again to release)
        if (heisenberg::g_config.enableStickyGrab) {
            state.stickyGrab = true;
        }
        
        // Notify ItemPositionConfigMode that a grab started
        // This enables sticky grab if reposition mode is active
        configMode.OnGrabStarted(&state, isLeft);
        
        return true;
    }

    void GrabManager::UpdateGrab(const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                                 bool isLeft, float deltaTime)
    {
        GrabState& state = isLeft ? _leftGrab : _rightGrab;
        
        // Early exit if not active
        if (!state.active) {
            return;
        }
        
        // CRITICAL: Validate reference via handle lookup BEFORE any method calls!
        // This prevents crashes when the game deletes objects we're holding.
        if (!state.HasValidRefr()) {
            spdlog::debug("[GRAB] UpdateGrab: {} hand reference invalid (object deleted?)", isLeft ? "Left" : "Right");
            // Clean up grab state
            state.Clear();
            // Reset fingers and release FRIK override
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            return;
        }
        
        // Check if we should use constraint-based grabbing (modes 1 or 2) - BROKEN
        RE::TESObjectREFR* refr = state.GetRefr();
        int effectiveGrabMode = GetEffectiveGrabMode();
        if (effectiveGrabMode == static_cast<int>(GrabMode::BallSocket) ||
            effectiveGrabMode == static_cast<int>(GrabMode::Motor))
        {
            auto& constraintMgr = ConstraintGrabManager::GetSingleton();
            constraintMgr.UpdateConstraintGrab(handPos, handRot, isLeft, deltaTime);
            return;
        }

        // =====================================================================
        // OBJECT VALIDITY CHECK - Detect when game has deleted our grabbed object
        // =====================================================================
        // This can happen when:
        // 1. A DropToHand weapon is released and the game deletes the reference
        // 2. The object is destroyed/picked up by something else
        // 3. Cell unload/reload deletes the object
        //
        // Check that the refr still has valid 3D and it matches our cached node
        if (!refr) {
            spdlog::debug("[GRAB] Reference became invalid - aborting grab");
            state.Clear();
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            cfgMode.OnGrabEnded(isLeft);
            return;
        }
        RE::NiAVObject* currentNode = refr->Get3D();
        if (!currentNode || (state.node && currentNode != state.node.get()))
        {
            spdlog::debug("[GRAB] Object {:08X} was deleted or recreated - aborting grab (cached node={:X}, current node={:X})",
                        refr->formID,
                        reinterpret_cast<uintptr_t>(state.node.get()),
                        reinterpret_cast<uintptr_t>(currentNode));
            
            // Clear state safely (don't try to restore physics on deleted object)
            state.Clear();
            
            // Reset fingers to extended position and release FRIK override
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            
            // Notify grab ended
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            cfgMode.OnGrabEnded(isLeft);
            return;
        }
        
        // Update our cached node pointer (in case it was recreated at same address)
        if (state.node.get() != currentNode)
        {
            spdlog::debug("[GRAB] Updating cached node pointer from {:X} to {:X}",
                         reinterpret_cast<uintptr_t>(state.node.get()),
                         reinterpret_cast<uintptr_t>(currentNode));
            state.node.reset(static_cast<RE::NiNode*>(currentNode));
        }

        // =====================================================================
        // CALCULATE HAND VELOCITY (for shoulder/mouth detection)
        // =====================================================================
        // Compute hand speed from position delta (in m/s)
        if (deltaTime > 0.0f && state.lastHandPos.x != 0.0f) {
            RE::NiPoint3 posDelta = handPos - state.lastHandPos;
            // Convert game units to meters: ~100 game units = 1 meter
            float distMeters = posDelta.Length() / 100.0f;
            state.handSpeed = (deltaTime > 0.0001f) ? (distMeters / deltaTime) : 0.0f;
        }
        state.lastHandPos = handPos;

        // =====================================================================
        // ITEM STORAGE ZONE CHECK - Automatic storage after X seconds in zone
        // No need to press grip - just hold item in storage zone
        // =====================================================================
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        float storageHoldTime = heisenberg::g_config.storageZoneHoldTime;
        // Clear skipStorageZone after 1 second (allows storage after initial Pipboy proximity)
        if (state.skipStorageZone) {
            float elapsed = static_cast<float>(Utils::GetTime()) - state.grabStartTime;
            if (elapsed >= 1.0f) {
                state.skipStorageZone = false;
                spdlog::debug("[STORAGE] Cleared skipStorageZone after {:.1f}s", elapsed);
            }
        }
        if (heisenberg::g_config.enableAutoStorage && !configMode.IsRepositionModeActive() && !state.isPulling && !state.skipStorageZone)
        {
            StorageZoneResult storageCheck = CheckItemStorageZone(handPos);
            bool wasInZone = state.isInStorageZone;  // Track previous state
            state.isInStorageZone = storageCheck.isInZone;
            
            if (storageCheck.isInZone)
            {
                // Haptic pulse when ENTERING zone (one-shot feedback)
                if (!wasInZone)
                {
                    g_vrInput.TriggerHaptic(isLeft, 40000);  // Strong pulse on entry
                    state.behindEarTimer = 0.0f;  // Reset timer on zone entry
                    spdlog::debug("[STORAGE] Entered storage zone - hold for {:.1f}s to store!", storageHoldTime);
                }
                
                // Accumulate time in zone
                state.behindEarTimer += deltaTime;
                
                // AUTO-STORE after configured time in zone
                if (state.behindEarTimer >= storageHoldTime)
                {
                    spdlog::debug("[STORAGE] {:.1f}s in zone - auto-storing item!", storageHoldTime);
                    
                    bool showMessage = true;
                    bool stored = StoreGrabbedItem(refr, showMessage);
                    
                    if (stored)
                    {
                        spdlog::debug("[GRAB] Auto-storage succeeded after {:.1f}s", storageHoldTime);
                        g_vrInput.TriggerHaptic(isLeft, 50000);  // Strong haptic for success
                        
                        // Start 1 second cooldown to prevent accidental re-grab
                        g_heisenberg.StartStickyGrabCooldown(isLeft);
                        
                        // End the grab - item was consumed
                        EndGrab(isLeft, nullptr, true);  // forStorage=true
                        return;  // Exit early - grab is over
                    }
                    else
                    {
                        spdlog::warn("[GRAB] Auto-storage failed");
                        state.behindEarTimer = 0.0f;  // Reset timer to try again
                    }
                }
            }
            else
            {
                // Left zone - reset timer
                if (wasInZone && state.behindEarTimer > 0.0f)
                {
                    spdlog::debug("[STORAGE] Left storage zone, timer was {:.1f}s", state.behindEarTimer);
                }
                state.behindEarTimer = 0.0f;
            }
        }

        // =====================================================================
        // VH HOLSTER ZONE CHECK — haptic when grabbed weapon enters VH zone
        // =====================================================================
        if (!configMode.IsRepositionModeActive() && !state.isPulling && IsWeapon(refr))
        {
            auto* vhApi = VirtualHolsters::RequestVirtualHolstersAPI();
            if (vhApi && vhApi->IsInitialized() && heisenberg::g_config.enableVHHolstering)
            {
                bool inZone = false;
                uint32_t bestSlot = 0;
                float bestDist = 99999.0f;

                for (uint32_t s = 1; s <= 7; s++) {
                    float hx = 0, hy = 0, hz = 0;
                    if (vhApi->GetHolsterPosition(s, hx, hy, hz)) {
                        float radius = vhApi->GetHolsterRadius(s);
                        if (radius <= 0.0f) radius = 10.0f;
                        float dx = handPos.x - hx, dy = handPos.y - hy, dz = handPos.z - hz;
                        float distSq = dx*dx + dy*dy + dz*dz;
                        if (distSq < radius * radius && distSq < bestDist) {
                            bestDist = distSq; bestSlot = s; inZone = true;
                        }
                    }
                }

                bool wasInVHZone = state.isInVHZone;
                state.isInVHZone = inZone;
                state.vhHolsterSlot = inZone ? bestSlot : 0;

                if (inZone && !wasInVHZone) {
                    g_vrInput.TriggerHaptic(isLeft, 40000);
                    spdlog::debug("[VH-ZONE] Weapon entered holster slot={} dist={:.1f}", bestSlot, std::sqrt(bestDist));
                }
            }
            else
            {
                state.isInVHZone = false;
                state.vhHolsterSlot = 0;
            }
        }

        // =====================================================================
        // CONSUME CHECK — mouth zone OR hand injection zone
        // =====================================================================
        // Mouth zone: bring consumable to face (food, drinks, chems)
        // Hand injection zone: bring consumable to opposite hand (syringes)
        // Both use one-shot consume (only try once per zone visit)
        if (!configMode.IsRepositionModeActive() && !state.isPulling)
        {
            bool inMouthZone = IsInMouthZone(isLeft);
            // Hand injection zone only activates for injectables (chems, stimpaks — not food/drinks)
            bool inHandInjectionZone = IsInjectable(refr) && IsInHandInjectionZone(isLeft);
            bool inAnyConsumeZone = inMouthZone || inHandInjectionZone;

            // Per-hand zone visit tracking
            bool& consumeAttemptedThisVisit = state.consumeAttemptedThisVisit;
            bool& wasInMouthZone = state.wasInMouthZoneLocal;
            bool& wasInHandInjectionZone = state.wasInHandInjectionZoneLocal;

            if (!inMouthZone)
            {
                wasInMouthZone = false;
            }
            else
            {
                if (!wasInMouthZone)
                    g_vrInput.TriggerHaptic(isLeft, 3000);
                wasInMouthZone = true;
            }

            if (!inHandInjectionZone)
            {
                wasInHandInjectionZone = false;
            }
            else
            {
                if (!wasInHandInjectionZone)
                    g_vrInput.TriggerHaptic(isLeft, 3000);
                wasInHandInjectionZone = true;
            }

            // Reset one-shot guard when leaving ALL consume zones
            if (!inAnyConsumeZone)
                consumeAttemptedThisVisit = false;

            // Check consumption: mouth zone OR hand injection zone
            // Blocked in Power Armor — PA helmet prevents eating/drinking/injecting
            bool shouldConsume = false;
            const char* zoneLabel = "";
            if (!consumeAttemptedThisVisit && IsConsumable(refr) && !(g_config.blockConsumptionInPA && Utils::IsPlayerInPowerArmor()))
            {
                if (inMouthZone && CheckMouthConsume(isLeft, state)) {
                    shouldConsume = true;
                    zoneLabel = "mouth";
                } else if (inHandInjectionZone && CheckHandInjectionConsume(isLeft, state)) {
                    shouldConsume = true;
                    zoneLabel = "hand injection";
                }
            }

            if (shouldConsume)
            {
                consumeAttemptedThisVisit = true;
                spdlog::debug("[GRAB] Consumable entered {} zone - attempting consume (one-shot)!", zoneLabel);

                RE::TESObjectREFR* refrToConsume = refr;
                bool consumed = ConsumeGrabbedItem(refrToConsume);

                if (consumed)
                {
                    spdlog::debug("[GRAB] Consume succeeded via {} zone", zoneLabel);
                    g_vrInput.TriggerHaptic(isLeft, 2000);
                    EndGrab(isLeft, nullptr, true);
                }
                else
                {
                    spdlog::warn("[GRAB] Consume FAILED via {} zone - keeping in hand", zoneLabel);
                    g_vrInput.TriggerHaptic(isLeft, 500);
                }

                return;
            }
            
            // =====================================================================
            // ARMOR/WEAPON EQUIP CHECK - Equip via body zones
            // =====================================================================
            // If item is armor or weapon, check if hand is in a body zone to equip
            // SKIP if in Power Armor - equipping on PA skeleton causes issues
            bool isArmorItem = IsArmor(refr);
            bool isWeaponItem = IsWeapon(refr);
            bool inPowerArmor = Utils::IsPlayerInPowerArmor();
            // Weapon zone detection: weaponEquipMode >= 1 for equip, enableVHHolstering for VH zones
            auto* vhApiForCheck = VirtualHolsters::RequestVirtualHolstersAPI();
            bool vhAvailable = (vhApiForCheck && vhApiForCheck->IsInitialized());
            bool vhHolsteringEnabled = (heisenberg::g_config.enableVHHolstering && vhAvailable);
            bool weaponZoneEnabled = (heisenberg::g_config.weaponEquipMode >= 1);

            if ((isArmorItem || (isWeaponItem && (weaponZoneEnabled || vhHolsteringEnabled))) && !inPowerArmor)
            {
                // Get item name - use refr->GetDisplayFullName() which works for all types
                const char* itemName = "unknown";
                if (isArmorItem)
                {
                    auto* armor = static_cast<RE::TESObjectARMO*>(refr->GetObjectReference());
                    if (armor) itemName = armor->GetFullName() ? armor->GetFullName() : "unknown";
                }
                else if (isWeaponItem)
                {
                    auto* weapon = static_cast<RE::TESObjectWEAP*>(refr->GetObjectReference());
                    if (weapon) itemName = weapon->GetFullName() ? weapon->GetFullName() : "unknown";
                }
                const char* itemType = isWeaponItem ? "WEAPON" : "ARMOR";
                
                spdlog::debug("[{}] {} detected", itemType, itemName);
                
                // Check equip zones for armor/weapons
                {
                    ArmorZoneType currentZone = ArmorZoneType::None;
                    const char* zoneName = "";
                    bool weaponNearFingertip = false;
                    
                    if (isWeaponItem)
                    {
                        // WEAPONS: Check if weapon is near the weapon hand's fingertip
                        // This allows "handing off" a weapon to your weapon hand
                        // Only check fingertip equip when weaponEquipMode is enabled
                        if (weaponZoneEnabled) {
                            weaponNearFingertip = CheckWeaponEquipByFingertip(handPos, state.handSpeed, isLeft);
                            if (weaponNearFingertip) {
                                currentZone = ArmorZoneType::Chest;  // Use Chest as placeholder zone type
                                zoneName = "WEAPON_HAND";
                            }
                        }
                        
                        // VH holster zone — use result from dedicated top-level check
                        if (currentZone == ArmorZoneType::None && state.isInVHZone && state.vhHolsterSlot >= 1)
                        {
                            static const char* kSlotNames[] = {
                                "", "LEFT_SHOULDER", "RIGHT_SHOULDER", "LEFT_HIP",
                                "RIGHT_HIP", "LOWER_BACK", "LEFT_CHEST", "RIGHT_CHEST"
                            };
                            currentZone = ArmorZoneType::Chest;  // Placeholder zone type
                            zoneName = kSlotNames[state.vhHolsterSlot];
                        }
                    }
                    else if (isArmorItem)
                    {
                        // ARMOR: Check body zones (head, chest, legs)
                        // Check chest zone first (most common)
                        currentZone = CheckArmorEquipZone(handPos, state.handSpeed, ArmorZoneType::Chest);
                        if (currentZone != ArmorZoneType::None) {
                            zoneName = "CHEST";
                        }
                        
                        // Check head/face zone
                        if (currentZone == ArmorZoneType::None) {
                            currentZone = CheckArmorEquipZone(handPos, state.handSpeed, ArmorZoneType::Head);
                            if (currentZone != ArmorZoneType::None) {
                                zoneName = "HEAD";
                            }
                        }
                        
                        // Check legs zone
                        if (currentZone == ArmorZoneType::None) {
                            currentZone = CheckArmorEquipZone(handPos, state.handSpeed, ArmorZoneType::Legs);
                            if (currentZone != ArmorZoneType::None) {
                                zoneName = "LEGS";
                            }
                        }
                    }
                    
                    // Track zone state for release-based equipping
                    bool wasInZone = state.isInEquipZone;
                    state.isInEquipZone = (currentZone != ArmorZoneType::None);
                    state.currentZoneName = zoneName;  // Store for use in release handler
                    
                    // Log zone entry/exit
                    if (state.isInEquipZone && !wasInZone)
                    {
                        spdlog::debug("[{}] '{}' in {} zone - release grip to equip!",
                                    itemType, itemName, zoneName);
                        g_vrInput.TriggerHaptic(isLeft, 40000);  // Strong pulse on entry (matches storage zone)
                    }
                    else if (!state.isInEquipZone && wasInZone)
                    {
                        spdlog::debug("[{}] Left equip zone", itemType);
                    }
                }
            }
        }
        
        // =====================================================================
        // AMMO RELOAD CHECK - Reload weapon by dropping matching ammo in equip zone
        // =====================================================================
        // DISABLED FOR TROUBLESHOOTING
        if (false && IsAmmo(refr) && !Utils::IsPlayerInPowerArmor())
        {
            auto* playerPCh = f4vr::getPlayer();
            RE::Actor* playerForAmmo = reinterpret_cast<RE::Actor*>(playerPCh);
            
            // Only check if player has a weapon drawn
            if (playerPCh && playerPCh->actorState.IsWeaponDrawn())
            {
                // Check if in any equip zone (chest zone for reloading)
                ArmorZoneType currentZone = CheckArmorEquipZone(handPos, state.handSpeed, ArmorZoneType::Chest);
                
                bool wasInAmmoZone = state.isInEquipZone;  // Reuse the flag
                bool nowInAmmoZone = (currentZone != ArmorZoneType::None);
                
                // Check if ammo matches weapon
                auto* ammoForm = refr->GetObjectReference();
                RE::TESAmmo* droppedAmmo = ammoForm ? static_cast<RE::TESAmmo*>(ammoForm) : nullptr;
                RE::TESObjectWEAP* weapon = heisenberg::GetEquippedWeapon.get()(playerForAmmo, 0);
                RE::TESAmmo* weaponAmmo = weapon ? weapon->weaponData.ammo : nullptr;
                
                bool ammoMatches = (droppedAmmo && weaponAmmo && 
                                    droppedAmmo->GetFormID() == weaponAmmo->GetFormID());
                
                if (nowInAmmoZone && !wasInAmmoZone && ammoMatches)
                {
                    // Just entered zone with matching ammo - haptic feedback!
                    spdlog::debug("[AMMO] '{}' entered reload zone - release to reload!",
                                droppedAmmo->GetFullName() ? droppedAmmo->GetFullName() : "ammo");
                    g_vrInput.TriggerHaptic(isLeft, 40000);  // Strong pulse on entry (matches storage/armor)
                    state.isInEquipZone = true;
                }
                else if (!nowInAmmoZone && wasInAmmoZone)
                {
                    spdlog::debug("[AMMO] Left reload zone");
                    state.isInEquipZone = false;
                }
                else
                {
                    state.isInEquipZone = nowInAmmoZone && ammoMatches;
                }
            }
        }

        // ═══════════════════════════════════════════════════════════════════════════
        // KEYFRAMED MODE: All grabs use KEYFRAMED physics for collision support
        // KEYFRAMED = infinite mass (can't be pushed) but STILL COLLIDES with world!
        // This matches HIGGS Skyrim behavior where held objects don't clip through walls.
        // ═══════════════════════════════════════════════════════════════════════════
        // Setup KEYFRAMED physics (runs once on first update after grab)
        if (!state.keyframedSetupComplete && state.node)
        {
            spdlog::debug("[GRAB-KEYFRAMED] Setting up KEYFRAMED physics mode");
            
            // NOTE: We previously stored state.wandNode here, but storing NiPointer
            // to player skeleton nodes caused crashes during flashlight toggle.
            // The engine's UpdateDownwardPass on PipboyRoot conflicted with our held reference.
            // Now we get fresh wand node via getPlayerNodes() each frame instead.
            
            // Save original parent (we'll stay parented to it, not reparent to wand)
            state.originalParent.reset(state.node->parent);
            
            if (state.originalParent)
            {
                spdlog::debug("[GRAB-KEYFRAMED] Original parent: '{}' at world ({:.1f},{:.1f},{:.1f})",
                    state.originalParent->name.c_str(),
                    state.originalParent->world.translate.x,
                    state.originalParent->world.translate.y,
                    state.originalParent->world.translate.z);
            }
            
            // Set to KEYFRAMED mode (like HIGGS legacy grab)
            // KEYFRAMED = infinite mass, can't be pushed, but still collides with world
            if (state.collisionObject && state.node)
            {
                RE::bhkWorld* bhkWorld = GetBhkWorldFromRefr(refr);
                
                // Save the bhkWorld (critical for interior cells with room bounds)
                state.savedState.savedBhkWorld = bhkWorld;
                state.savedState.motionType = RE::hknpMotionPropertiesId::Preset::DYNAMIC;
                
                // Save collision object flags (like HIGGS Skyrim's savedRigidBodyFlags)
                // These are restored on release to preserve any special behavior flags
                state.savedState.collisionObjectFlags = state.collisionObject->flags.flags;
                spdlog::debug("[GRAB-KEYFRAMED] Saved collision object flags: {:X}", 
                             state.savedState.collisionObjectFlags);
                
                // Set to KEYFRAMED using bhkWorld_SetMotion (same as DYNAMIC on release)
                // This operates on the NiAVObject node and properly updates the physics system
                // The 3 bool params (true,true,true) enable collision filter updates
                bhkWorld_SetMotionLocked(state.node.get(), RE::hknpMotionPropertiesId::Preset::KEYFRAMED, true, true, true, bhkWorld);
                spdlog::debug("[GRAB-KEYFRAMED] Set motion type to KEYFRAMED via bhkWorld_SetMotion (bhkWorld {:X})",
                            reinterpret_cast<uintptr_t>(bhkWorld));
                
                // DISABLE COLLISION during grab to prevent grabbed object from blocking player movement
                // Use layer 15 (kNonCollidable) - no collision at all during grab
                // This allows free movement. Objects won't collide with walls while held.
                // Save original collision layer for restoration on release
                state.savedState.collisionLayer = 4;  // Default kClutter - actual layer saved in SetLayer
                bhkUtilFunctions_SetLayerLocked(state.node.get(), 15, bhkWorld);  // 15 = kNonCollidable
                spdlog::debug("[GRAB-KEYFRAMED] Set collision layer to kNonCollidable (15) - no collision during grab");
                
                // PHYSICS BODY OFFSET DETECTION
                // Check if the collision body has an offset from the scene node
                // (like bhkRigidBodyT in Skyrim). If so, we need to compensate.
                if (Utils::HasPhysicsBodyOffset(state.collisionObject, state.node.get()))
                {
                    RE::NiTransform bodyOffset = Utils::GetPhysicsBodyOffset(state.collisionObject, state.node.get());
                    spdlog::debug("[GRAB-KEYFRAMED] Detected physics body offset: pos=({:.2f},{:.2f},{:.2f})",
                                bodyOffset.translate.x, bodyOffset.translate.y, bodyOffset.translate.z);
                    // TODO: Store this offset in GrabState for use in UpdateGrab
                }
                
                // DON'T remove from world - physics stays active
                state.savedState.wasDeactivated = false;
            }
            
            // Mark setup complete so we don't re-run this block
            state.keyframedSetupComplete = true;
            spdlog::debug("[GRAB-KEYFRAMED] Setup complete: will update transforms each frame");
        }

        // NOTE: Scene graph parenting code has been removed.
        // All grabs now use KEYFRAMED mode for proper collision support.
        // See git history for the old parenting implementation if needed.

        // Handle pull animation - interpolate position toward item offset target
        // Note: We interpolate in WORLD space, then the final target calculation
        // properly applies the hand-local offset. When pull completes, we just use
        // the stored itemOffset directly.
        if (state.isPulling)
        {
            // Advance pull progress based on pull speed
            float distToTarget = (state.initialObjectPos - handPos).Length();
            if (distToTarget > 0.01f) {
                float pullDelta = (heisenberg::g_config.pullSpeed * deltaTime) / distToTarget;
                state.pullProgress = (std::min)(state.pullProgress + pullDelta, 1.0f);
            } else {
                state.pullProgress = 1.0f;
            }
            
            // Check if pull is complete
            if (state.pullProgress >= 1.0f)
            {
                state.isPulling = false;
                spdlog::debug("[GRAB] Pull complete - now using stored item offset");

                // Apply deferred finger curls now that object has arrived in hand
                if (state.pendingFingerCurls && state.hasItemOffset && state.itemOffset.hasFingerCurls) {
                    auto& frik = FRIKInterface::GetSingleton();
                    if (frik.IsAvailable()) {
                        frik.SetHandPoseFingerPositions(isLeft,
                            state.itemOffset.thumbCurl, state.itemOffset.indexCurl,
                            state.itemOffset.middleCurl, state.itemOffset.ringCurl, state.itemOffset.pinkyCurl);
                        Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, state.itemOffset.thumbCurl);
                        spdlog::debug("[GRAB-FINGERS] Pull complete - applied deferred curls: thumb={:.2f}", state.itemOffset.thumbCurl);
                    }
                    state.pendingFingerCurls = false;
                }
            }
        }

        // =====================================================================
        // PER-FRAME FINGER CURL RE-APPLICATION
        // FRIK's controller tracking maps trigger input to finger curl, which
        // overrides our SetHandPoseFingerPositions. Re-apply every frame to
        // keep the correct grab pose regardless of trigger state.
        // =====================================================================
        if (!state.isPulling && !state.isTelekinesis && !state.pendingFingerCurls &&
            state.hasItemOffset && state.itemOffset.hasFingerCurls)
        {
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.IsAvailable()) {
                frik.SetHandPoseFingerPositions(isLeft,
                    state.itemOffset.thumbCurl, state.itemOffset.indexCurl,
                    state.itemOffset.middleCurl, state.itemOffset.ringCurl, state.itemOffset.pinkyCurl);
                static int curlLogCount = 0;
                if (curlLogCount++ < 5) {
                    spdlog::debug("[GRAB] Per-frame curl re-apply: thumb={:.2f} hand={}", state.itemOffset.thumbCurl, isLeft ? "L" : "R");
                }
            }
        }

        // Calculate target transform
        // Object follows controller transform exactly (position + rotation)
        
        RE::NiPoint3 targetPos;
        RE::NiMatrix3 targetRot;
        
        // Check if in reposition mode - if so, use frozen world position
        // configMode already obtained above
        if (configMode.IsRepositionModeActive() && state.stickyGrab) {
            // First time in reposition mode - capture current world position AND hand transform
            if (!configMode.HasFrozenPosition()) {
                // Calculate where item would be with current offset (using FRIK formulas)
                RE::NiPoint3 localOffset = state.itemOffset.position;
                RE::NiPoint3 rotatedOffset = handRot.Transpose() * localOffset;
                RE::NiPoint3 currentWorldPos = handPos + rotatedOffset;
                RE::NiMatrix3 currentWorldRot = state.itemOffset.rotation * handRot;
                // Store both item world position AND hand transform for relative offset calculation
                configMode.SetFrozenWorldTransform(currentWorldPos, currentWorldRot, handPos, handRot);
            }
            
            // Use frozen world position - item stays still while adjustments are made
            targetPos = configMode.GetFrozenWorldPos();
            targetRot = configMode.GetFrozenWorldRot();
        } else {
            // Normal mode - calculate from hand/weapon position + offset
            
            // For FRIK offsets, use the FRIK skeleton hand node (LArm_Hand/RArm_Hand)
            // instead of the wand. This provides correct coordinate system for FRIK offsets.
            RE::NiPoint3 parentPos = handPos;
            RE::NiMatrix3 parentRot = handRot;
            
            // Use FRIK skeleton hand node for FRIK offsets (now fixed for left/right hand)
            if (state.isFRIKOffset) {
                RE::NiPoint3 frikParentPos;
                RE::NiMatrix3 frikParentRot;
                if (GetFRIKWeaponParentTransform(isLeft, frikParentPos, frikParentRot)) {
                    parentPos = frikParentPos;
                    parentRot = frikParentRot;
                    // Debug log first few times
                    static int frikDebugCount = 0;
                    if (frikDebugCount++ < 5) {
                        spdlog::debug("[GRAB] FRIK offset mode: using skeleton hand node pos=({:.1f},{:.1f},{:.1f}) instead of wand=({:.1f},{:.1f},{:.1f})",
                            parentPos.x, parentPos.y, parentPos.z,
                            handPos.x, handPos.y, handPos.z);
                    }
                }
            }

            
            // Use parent rotation combined with item offset rotation
            // FRIK formula: world.rotate = local.rotate * parent.rotate
            RE::NiMatrix3 finalRot = state.itemOffset.rotation * parentRot;
            
            // Offset in parent-local space from item offset
            // FRIK formula: world.translate = parent.translate + parent.rotate.T * (local.translate * parent.scale)
            RE::NiPoint3 localOffset = state.itemOffset.position;

            // Power armor glove compensation — push objects outward so they aren't hidden
            if (Utils::IsPlayerInPowerArmor()) {
                localOffset.x += g_config.paGrabOffsetX;
                localOffset.y += g_config.paGrabOffsetY;
                localOffset.z += g_config.paGrabOffsetZ;
            }

            RE::NiPoint3 rotatedOffset = parentRot.Transpose() * localOffset;  // Use Transpose like FRIK
            RE::NiPoint3 finalPos = parentPos + rotatedOffset;
            
            // Debug logging every 60 frames
            static int debugCounter = 0;
            if (++debugCounter >= 60) {
                debugCounter = 0;
                spdlog::debug("[GRAB] Normal mode: parentPos=({:.1f},{:.1f},{:.1f}) offset=({:.2f},{:.2f},{:.2f}) rotatedOffset=({:.2f},{:.2f},{:.2f}) finalPos=({:.1f},{:.1f},{:.1f}) isFRIK={}",
                    parentPos.x, parentPos.y, parentPos.z,
                    localOffset.x, localOffset.y, localOffset.z,
                    rotatedOffset.x, rotatedOffset.y, rotatedOffset.z,
                    finalPos.x, finalPos.y, finalPos.z,
                    state.isFRIKOffset);
                spdlog::debug("[GRAB] finalRot row0=({:.2f},{:.2f},{:.2f}) offsetRot row0=({:.2f},{:.2f},{:.2f})",
                    finalRot.entry[0][0], finalRot.entry[0][1], finalRot.entry[0][2],
                    state.itemOffset.rotation.entry[0][0], state.itemOffset.rotation.entry[0][1], state.itemOffset.rotation.entry[0][2]);
            }
            
            // If still pulling, interpolate from initial object position to final target
            if (state.isPulling) {
                float t = state.pullProgress;
                targetPos = state.initialObjectPos * (1.0f - t) + finalPos * t;
                // For rotation, just use the target rotation (or could lerp if needed)
                // Using final rotation makes the item orient correctly as it moves
                targetRot = finalRot;
            } else {
                // Pull complete or no pull needed - use stored offset directly
                targetPos = finalPos;
                targetRot = finalRot;
            }
        }
        
        // Check for NaN - skip this frame if we have bad data
        if (std::isnan(targetPos.x) || std::isnan(targetPos.y) || std::isnan(targetPos.z)) {
            spdlog::warn("[GRAB] UpdateGrab: NaN detected in targetPos, skipping frame");
            return;
        }
        
        // =====================================================================
        // POST-PHYSICS GRAB UPDATE
        // Physics updates now happen in PrePhysicsUpdate() BEFORE physics runs.
        // This function (UpdateGrab) runs AFTER physics for:
        // - Updating pull animation progress
        // - Hand speed tracking for zones
        // - Storage zone checks
        // - Consume/equip checks
        // =====================================================================
        
        // Ensure collision object is cached
        if (!state.collisionObject)
        {
            state.collisionObject = GetCollisionObject(refr);
        }
        
        // Cache target for zones/storage checks
        state.lastTargetPos = targetPos;
        state.lastTargetRot = targetRot;
        
        // Hand speed already computed earlier in UpdateGrab (in meters/s)
        
        // Mark that we're using keyframed mode (for EndGrab)
        state.usingMouseSpring = true;
        
        // Check physics body vs target — if too far, game likely reset motion type
        // (happens when Pipboy closes in PA: right arm transition reverts KEYFRAMED to DYNAMIC)
        RE::NiPoint3 objPos = state.node->world.translate;
        float lag = (objPos - targetPos).Length();
        if (lag > 50.0f && state.keyframedSetupComplete) {
            auto* bhkWorld = GetBhkWorldFromRefr(refr);
            if (bhkWorld) {
                bhkWorld_SetMotionLocked(state.node.get(), RE::hknpMotionPropertiesId::Preset::KEYFRAMED, true, true, true, bhkWorld);
                spdlog::warn("[POST-PHYSICS] Re-applied KEYFRAMED (lag={:.0f}cm)", lag);
            }
        }

        static int postPhysDebug = 0;
        if (++postPhysDebug >= 10) {
            postPhysDebug = 0;
            spdlog::debug("[POST-PHYSICS] Object at ({:.1f},{:.1f},{:.1f}) target=({:.1f},{:.1f},{:.1f}) lag={:.2f}cm",
                objPos.x, objPos.y, objPos.z, targetPos.x, targetPos.y, targetPos.z, lag);
        }
    }

    void GrabManager::EndGrab(bool isLeft, const RE::NiPoint3* throwVelocity, bool forStorage)
    {
        // Get state for this hand first
        GrabState& state = isLeft ? _leftGrab : _rightGrab;
        
        if (!state.active)
            return;
        
        // CRITICAL: Validate reference via handle lookup BEFORE any method calls!
        // This prevents crashes when the game deletes objects we're holding.
        bool refrValid = state.HasValidRefr();
        
        // If refr was deleted, just clean up state and exit
        if (!refrValid) {
            spdlog::debug("[GRAB] EndGrab: {} hand reference invalid (object deleted?)", isLeft ? "Left" : "Right");
            state.Clear();
            // Reset fingers to extended position and release FRIK override
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            cfgMode.OnGrabEnded(isLeft);
            return;
        }
        
        // Check if we should use constraint-based grabbing (modes 1 or 2)
        RE::TESObjectREFR* refr = state.GetRefr();
        int effectiveGrabMode = GetEffectiveGrabMode();
        if (effectiveGrabMode == static_cast<int>(GrabMode::BallSocket) ||
            effectiveGrabMode == static_cast<int>(GrabMode::Motor))
        {
            auto& constraintMgr = ConstraintGrabManager::GetSingleton();
            constraintMgr.EndConstraintGrab(isLeft, throwVelocity);
            return;
        }
        
        // =====================================================================
        // OBJECT VALIDITY CHECK - Detect when game has deleted our grabbed object
        // =====================================================================
        // If the object's 3D changed (e.g., respawned), abort gracefully
        RE::NiAVObject* currentNode = refr->Get3D();
        if (!currentNode || (state.node && currentNode != state.node.get()))
        {
            spdlog::warn("[GRAB] EndGrab: Object {:08X} 3D changed - clearing state only",
                        refr->formID);
            
            // Clear state safely (don't try to restore physics on deleted object)
            state.Clear();
            
            // Reset fingers to extended position and release FRIK override
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            frik.ClearHandPoseFingerPositions(isLeft);
            Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            
            // Notify grab ended
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            cfgMode.OnGrabEnded(isLeft);
            return;
        }
        
        // =====================================================================
        // ARMOR/WEAPON EQUIP ZONE CHECK ON RELEASE (CHECKED FIRST - PRIORITY)
        // =====================================================================
        // If releasing while in an equip zone, equip the armor or weapon
        // SKIP if in Power Armor - equipping on PA skeleton causes issues
        // Equip takes PRIORITY over storage when zones overlap
        // weaponEquipMode: 0=disabled, 1=zone equip, 2=VH zone holster
        if (!forStorage && !state.isPulling && state.isInEquipZone && refr && !Utils::IsPlayerInPowerArmor())
        {
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            if (!cfgMode.IsRepositionModeActive())
            {
                RE::TESObjectREFR* refrToEquip = refr;
                bool handled = false;
                
                // Check what type of item we're handling
                if (IsArmor(refr))
                {
                    spdlog::debug("[ARMOR] Release in equip zone - equipping armor!");
                    handled = EquipArmorItem(refrToEquip);
                }
                else if (IsWeapon(refr))
                {
                    spdlog::debug("[WEAPON] Release in {} zone - weaponEquipMode={} vhSlot={}",
                                state.currentZoneName,
                                heisenberg::g_config.weaponEquipMode,
                                state.vhHolsterSlot);

                    if (state.vhHolsterSlot > 0 && heisenberg::g_config.enableVHHolstering)
                    {
                        // VH holster zone — holster regardless of weaponEquipMode
                        spdlog::debug("[WEAPON] Release in VH holster zone slot={} - holstering!", state.vhHolsterSlot);
                        handled = PickupWeaponForHolster(refrToEquip, state.currentZoneName, state.vhHolsterSlot);
                    }
                    else if (heisenberg::g_config.weaponEquipMode >= 1)
                    {
                        // Weapon hand equip (fingertip zone)
                        spdlog::debug("[WEAPON] Release in equip zone - equipping weapon!");
                        handled = EquipWeaponItem(refrToEquip);
                    }
                }
                else if (false && IsAmmo(refr))  // DISABLED FOR TROUBLESHOOTING
                {
                    // AMMO dropped in equip zone - try to reload equipped weapon
                    spdlog::debug("[AMMO] Release in equip zone - trying to reload weapon!");
                    
                    // Get the dropped ammo's form
                    auto* ammoForm = refr->GetObjectReference();
                    RE::TESAmmo* droppedAmmo = ammoForm ? static_cast<RE::TESAmmo*>(ammoForm) : nullptr;
                    
                    if (droppedAmmo)
                    {
                        // Check if player has weapon drawn and get weapon's ammo type
                        auto* playerPCh = f4vr::getPlayer();
                        RE::Actor* playerForReload = reinterpret_cast<RE::Actor*>(playerPCh);
                        RE::TESObjectWEAP* weapon = playerPCh && playerPCh->actorState.IsWeaponDrawn() 
                            ? heisenberg::GetEquippedWeapon.get()(playerForReload, 0) : nullptr;
                        
                        RE::TESAmmo* weaponAmmo = weapon ? weapon->weaponData.ammo : nullptr;
                        std::uint16_t magCapacity = weapon ? weapon->weaponData.ammoCapacity : 0;
                        
                        if (weaponAmmo && droppedAmmo->GetFormID() == weaponAmmo->GetFormID())
                        {
                            spdlog::debug("[AMMO] Ammo type matches weapon ({:08X})!", weaponAmmo->GetFormID());
                            
                            // Get current ammo in magazine
                            RE::BGSEquipIndex equipIndex;
                            heisenberg::Actor_GetWeaponEquipIndex.get()(playerForReload, &equipIndex, nullptr);
                            
                            float currentAmmo = heisenberg::Actor_GetCurrentAmmoCount.get()(playerForReload, equipIndex);
                            int currentAmmoInt = static_cast<int>(currentAmmo);
                            
                            int spaceInMag = magCapacity - currentAmmoInt;
                            
                            if (spaceInMag > 0)
                            {
                                // Pick up the ammo to inventory first
                                RE::TESObjectREFR* playerRefr = reinterpret_cast<RE::TESObjectREFR*>(playerPCh);
                                heisenberg::TESObjectREFR_ActivateRef.get()(refr, playerRefr, nullptr, 1, false, false, false);
                                
                                // For simplicity, reload as much as possible (space in mag)
                                // The game's ammo system will handle the inventory count properly
                                int ammoToAdd = spaceInMag;  // We'll add up to magazine capacity
                                int newAmmoCount = magCapacity;  // Fill the magazine
                                
                                // Set the new ammo count in the magazine
                                heisenberg::Actor_SetCurrentAmmoCount.get()(playerForReload, equipIndex, newAmmoCount);
                                
                                spdlog::debug("[AMMO] ✓ Reloaded! Magazine now: {}/{}", newAmmoCount, magCapacity);
                                
                                // Show HUD message
                                char msg[128];
                                snprintf(msg, sizeof(msg), "Reloaded %s (%d/%d)", 
                                    droppedAmmo->GetFullName() ? droppedAmmo->GetFullName() : "rounds",
                                    newAmmoCount, magCapacity);
                                heisenberg::ShowHUDMessage(msg);
                                
                                handled = true;
                            }
                            else
                            {
                                spdlog::debug("[AMMO] Magazine already full ({}/{})", currentAmmoInt, magCapacity);
                            }
                        }
                        else
                        {
                            spdlog::debug("[AMMO] Ammo doesn't match weapon (dropped {:08X} vs weapon {:08X})",
                                droppedAmmo->GetFormID(), weaponAmmo ? weaponAmmo->GetFormID() : 0);
                        }
                    }
                }
                
                if (handled)
                {
                    spdlog::debug("[EQUIP] Action succeeded on zone release");
                    g_vrInput.TriggerHaptic(isLeft, 50000);  // Strong haptic for success
                    forStorage = true;  // Use forStorage path (item is consumed)
                }
                else if (IsArmor(refr) || IsWeapon(refr))
                {
                    spdlog::warn("[EQUIP] Action FAILED on zone release");
                    g_vrInput.TriggerHaptic(isLeft, 5000);  // Short haptic for failure
                }
            }
        }
        
        // =====================================================================
        // COMPANION DROP CHECK ON RELEASE (AFTER EQUIP, BEFORE STORAGE)
        // =====================================================================
        // If either wand's viewcaster is pointing at a companion, transfer the
        // grabbed item to the companion's inventory instead of dropping it.
        // A "companion" is any Actor with the kIsCommandedActor flag set.
        // Skip if hand is in storage zone — always store to player inventory in that case.
        if (!forStorage && !state.isPulling && !state.isInStorageZone &&
            g_config.enableDropToCompanion && refr
            && refr->GetFormType() != RE::ENUM_FORM_ID::kACHR)  // Don't store actors to companions
        {
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            if (!cfgMode.IsRepositionModeActive())
            {
                spdlog::debug("[COMPANION] Checking viewcaster targets for companion (enableDropToCompanion={})...",
                            g_config.enableDropToCompanion);
                spdlog::debug("[COMPANION] Grabbed item: {:08X} formType={}",
                            refr->formID, static_cast<int>(refr->GetFormType()));
                
                // Check both wands for companion/container target
                RE::Actor* companionActor = nullptr;
                RE::TESObjectREFR* containerRef = nullptr;  // Non-actor container (chest, desk, etc.)
                auto* player = f4vr::getPlayer();
                
                for (bool checkLeft : { true, false })
                {
                    RE::ObjectRefHandle targetHandle = heisenberg::GetVRWandTargetHandle(checkLeft);
                    if (!targetHandle) {
                        spdlog::debug("[COMPANION] {} wand: no target (handle invalid)", checkLeft ? "Left" : "Right");
                        continue;
                    }
                    
                    RE::NiPointer<RE::TESObjectREFR> targetRefr = targetHandle.get();
                    if (!targetRefr) {
                        spdlog::debug("[COMPANION] {} wand: handle valid but get() returned null", checkLeft ? "Left" : "Right");
                        continue;
                    }
                    
                    // Get target name for logging
                    const char* targetName = "?";
                    if (auto* baseForm = targetRefr->GetObjectReference()) {
                        if (auto* fullName = baseForm->As<RE::TESFullName>()) {
                            auto nameView = RE::TESFullName::GetFullName(*baseForm, false);
                            if (!nameView.empty()) targetName = nameView.data();
                        }
                    }
                    const char* displayName = targetRefr->GetDisplayFullName();
                    
                    spdlog::debug("[COMPANION] {} wand target: {:08X} formType={} baseName='{}' displayName='{}'",
                                checkLeft ? "Left" : "Right", targetRefr->formID, 
                                static_cast<int>(targetRefr->GetFormType()),
                                targetName, displayName ? displayName : "null");
                    
                    // Skip the grabbed item itself (viewcaster may hit the held object)
                    if (targetRefr->formID == refr->formID) {
                        spdlog::debug("[COMPANION] {} wand target IS the grabbed item — skipping", checkLeft ? "Left" : "Right");
                        continue;
                    }
                    
                    // Check if target is an Actor (kACHR)
                    if (targetRefr->GetFormType() == RE::ENUM_FORM_ID::kACHR)
                    {
                        auto* actor = static_cast<RE::Actor*>(targetRefr.get());
                        if (!actor || actor == reinterpret_cast<RE::Actor*>(player)) continue;
                        
                        // Check multiple companion detection methods:
                        // 1. kIsCommandedActor flag
                        bool hasCommandedFlag = actor->boolFlags.all(RE::Actor::BOOL_FLAGS::kIsCommandedActor);
                        
                        // 2. commandingActor handle in MiddleHighProcessData
                        bool commandedByPlayer = false;
                        if (actor->currentProcess && actor->currentProcess->middleHigh) {
                            auto& cmdHandle = actor->currentProcess->middleHigh->commandingActor;
                            if (cmdHandle) {
                                auto cmdActorPtr = cmdHandle.get();
                                if (cmdActorPtr && cmdActorPtr.get() == reinterpret_cast<RE::Actor*>(player)) {
                                    commandedByPlayer = true;
                                }
                            }
                        }
                        
                        // 3. Actor is in player's commandedActors array
                        bool inPlayerCommandList = false;
                        auto* playerActor = reinterpret_cast<RE::Actor*>(player);
                        if (playerActor && playerActor->currentProcess && playerActor->currentProcess->middleHigh) {
                            for (std::uint32_t i = 0; i < playerActor->currentProcess->middleHigh->commandedActors.size(); ++i) {
                                auto& cmdData = playerActor->currentProcess->middleHigh->commandedActors[i];
                                auto cmdRef = cmdData.commandedActor.get();
                                if (cmdRef && cmdRef.get() == actor) {
                                    inPlayerCommandList = true;
                                    break;
                                }
                            }
                        }
                        
                        // 4. Actor is in CurrentCompanionFaction (FormID 0x1C21C)
                        //    FO4 companions (Codsworth, etc.) use faction membership, not commanded actor flags
                        bool inCompanionFaction = false;
                        {
                            auto* factionForm = RE::TESForm::GetFormByID(0x0001C21C);
                            if (factionForm) {
                                auto* faction = factionForm->As<RE::TESFaction>();
                                if (faction) {
                                    inCompanionFaction = actor->IsInFaction(faction);
                                }
                                spdlog::debug("[COMPANION] Faction 0x1C21C lookup: form={} type={} asFaction={} inFaction={}",
                                    (void*)factionForm, (int)factionForm->GetFormType(),
                                    (void*)(factionForm->As<RE::TESFaction>()), inCompanionFaction);
                            } else {
                                spdlog::warn("[COMPANION] CurrentCompanionFaction 0x1C21C NOT FOUND in form database");
                            }
                        }
                        
                        // 5. Non-hostile check: if actor is not hostile, accept as valid drop target
                        //    This is the most permissive approach — player is deliberately pointing at an NPC
                        bool isNotHostile = !actor->GetHostileToActor(reinterpret_cast<RE::Actor*>(player));
                        
                        spdlog::debug("[COMPANION] Actor {:08X}: cmdFlag={}, cmdByPlayer={}, inPlayerList={}, inCompFaction={}, notHostile={}",
                                    actor->formID, hasCommandedFlag, commandedByPlayer, inPlayerCommandList, inCompanionFaction, isNotHostile);
                        
                        // Accept if ANY companion indicator matches, OR if not hostile (permissive)
                        if (hasCommandedFlag || commandedByPlayer || inPlayerCommandList || inCompanionFaction || isNotHostile)
                        {
                            companionActor = actor;
                            break;
                        }
                    }
                    // Check if target is a world container (chest, desk, etc.)
                    else if (!containerRef) {
                        auto* baseForm = targetRefr->GetObjectReference();
                        if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kCONT) {
                            containerRef = targetRefr.get();
                            spdlog::debug("[CONTAINER] {} wand target is container {:08X} '{}'",
                                        checkLeft ? "Left" : "Right", targetRefr->formID,
                                        targetRefr->GetDisplayFullName() ? targetRefr->GetDisplayFullName() : "?");
                        }
                    }
                }
                
                if (!companionActor)
                {
                    // Viewcaster didn't find a companion — try ProcessLists proximity search as fallback
                    spdlog::debug("[COMPANION] Viewcaster found no companion — trying proximity search...");
                    auto* procLists = heisenberg::GetProcessListsSingleton();
                    if (procLists) {
                        // Get player position from HMD node
                        RE::NiPoint3 playerPos;
                        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                        if (playerNodes && playerNodes->HmdNode) {
                            playerPos = playerNodes->HmdNode->world.translate;
                        }
                        RE::NiPointer<RE::Actor> nearestActor;
                        float searchRadius = 300.0f;  // ~3 meters
                        bool found = heisenberg::ProcessLists_GetClosestActorWithinRangeOfPoint(
                            procLists, playerPos, searchRadius, nearestActor);
                        if (found && nearestActor) {
                            auto* actor = nearestActor.get();
                            const char* actorName = "?";
                            if (auto* npcBase = actor->GetObjectReference()) {
                                auto nameView = RE::TESFullName::GetFullName(*npcBase, false);
                                if (!nameView.empty()) actorName = nameView.data();
                            }
                            bool hasCommandedFlag = actor->boolFlags.all(RE::Actor::BOOL_FLAGS::kIsCommandedActor);
                            spdlog::debug("[COMPANION] Proximity search found actor {:08X} '{}' at dist={:.0f} commandedFlag={}",
                                        actor->formID, actorName,
                                        (actor->GetPosition() - playerPos).Length(), hasCommandedFlag);
                            // Don't auto-use — just log for diagnostics
                        } else {
                            spdlog::debug("[COMPANION] Proximity search found no actors within {:.0f} units", searchRadius);
                        }
                    }
                }
                
                // Skip if we recently looted from this target (prevents store-back)
                auto& dropToHand = heisenberg::DropToHand::GetSingleton();
                if (companionActor &&
                    dropToHand.WasRecentlyLootedFrom(companionActor->formID))
                {
                    spdlog::debug("[COMPANION] Skipping store to {:08X} — recently looted from (cooldown)",
                                companionActor->formID);
                    companionActor = nullptr;
                }
                if (containerRef &&
                    dropToHand.WasRecentlyLootedFrom(containerRef->formID))
                {
                    spdlog::debug("[CONTAINER] Skipping store to {:08X} — recently looted from (cooldown)",
                                containerRef->formID);
                    containerRef = nullptr;
                }

                if (companionActor)
                {
                    // Get item info before transfer
                    std::string itemName = heisenberg::ItemOffsetManager::GetItemName(refr);
                    if (itemName.empty()) itemName = "item";
                    
                    // Get item count from ExtraCount
                    int32_t extraCount = 1;
                    if (refr->extraList) {
                        auto* extraData = refr->extraList->GetByType(RE::EXTRA_DATA_TYPE::kCount);
                        if (extraData) {
                            int16_t* countPtr = reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(extraData) + 0x18);
                            extraCount = *countPtr;
                        }
                    }
                    
                    // Get companion name from base form (TESNPC inherits TESFullName)
                    std::string companionName = "companion";
                    if (auto* npcBase = companionActor->GetObjectReference()) {
                        auto nameView = RE::TESFullName::GetFullName(*npcBase, false);
                        if (!nameView.empty())
                            companionName = std::string(nameView);
                    }
                    
                    // Transfer item to companion inventory
                    auto* baseForm = refr->GetObjectReference();
                    if (baseForm)
                    {
                        // Check if other hand grip is held (dual-grip = equip on companion)
                        bool otherGripHeld = g_vrInput.IsPressed(!isLeft, VRButton::Grip);

                        // Suppress default HUD messages
                        heisenberg::Hooks::SetSuppressHUDMessages(true);

                        RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                        heisenberg::AddObjectToContainer(
                            companionActor,
                            static_cast<RE::TESBoundObject*>(baseForm),
                            &nullExtra, extraCount, nullptr, 0);

                        // Remove world reference — SafeDisableRef defers for weapons/armor
                        // to prevent hkbBehaviorGraph crash from IO thread cleanup.
                        SafeDisableRef(refr);

                        // Dual-grip: equip weapons/armor on companion
                        bool didEquip = false;
                        if (otherGripHeld) {
                            auto formType = baseForm->GetFormType();
                            if (formType == RE::ENUM_FORM_ID::kWEAP || formType == RE::ENUM_FORM_ID::kARMO) {
                                // ActorEquipManager::GetSingleton VR offset 0x5a38bf8 (ID 1174340)
                                static REL::Relocation<RE::ActorEquipManager**> equipMgrSingleton{ REL::Offset(0x5a38bf8) };
                                auto* equipMgr = *equipMgrSingleton;
                                if (equipMgr) {
                                    // ActorEquipManager::EquipObject VR offset 0xe6fea0 (ID 988029, status 4)
                                    using EquipObjectFn = bool(*)(RE::ActorEquipManager*, RE::Actor*,
                                        const RE::BGSObjectInstance&, uint32_t, uint32_t, const RE::BGSEquipSlot*,
                                        bool, bool, bool, bool, bool);
                                    static REL::Relocation<EquipObjectFn> EquipObjectVR{ REL::Offset(0xe6fea0) };

                                    RE::BGSObjectInstance instance(baseForm, nullptr);
                                    EquipObjectVR(equipMgr, companionActor, instance,
                                        0,       // stackID
                                        1,       // number
                                        nullptr, // slot (auto)
                                        true,    // queueEquip
                                        false,   // forceEquip
                                        true,    // playSounds
                                        true,    // applyNow
                                        false);  // locked
                                    didEquip = true;
                                    spdlog::debug("[COMPANION] Dual-grip equip: {} on {} ({:08X})",
                                                itemName, companionName, companionActor->formID);
                                }
                            }
                        }

                        heisenberg::Hooks::SetSuppressHUDMessages(false);

                        // Show confirmation message
                        char msg[256];
                        if (didEquip) {
                            if (extraCount > 1)
                                snprintf(msg, sizeof(msg), "Equipped %s x%d on %s", itemName.c_str(), extraCount, companionName.c_str());
                            else
                                snprintf(msg, sizeof(msg), "Equipped %s on %s", itemName.c_str(), companionName.c_str());
                        } else {
                            if (extraCount > 1)
                                snprintf(msg, sizeof(msg), "Gave %s x%d to %s", itemName.c_str(), extraCount, companionName.c_str());
                            else
                                snprintf(msg, sizeof(msg), "Gave %s to %s", itemName.c_str(), companionName.c_str());
                        }
                        heisenberg::ShowHUDMessage(msg);

                        spdlog::info("[COMPANION] Transferred '{}' x{} to {} ({:08X}) equipped={}",
                                    itemName, extraCount, companionName, companionActor->formID, didEquip);

                        g_vrInput.TriggerHaptic(isLeft, 50000);  // Strong haptic for success
                        forStorage = true;  // Use forStorage cleanup path
                    }
                }

                // =====================================================================
                // WORLD CONTAINER TRANSFER (chest, desk, etc.)
                // =====================================================================
                if (!forStorage && containerRef)
                {
                    auto* baseForm = refr->GetObjectReference();
                    if (baseForm)
                    {
                        std::string itemName = heisenberg::ItemOffsetManager::GetItemName(refr);
                        if (itemName.empty()) itemName = "item";

                        int32_t extraCount = 1;
                        if (refr->extraList) {
                            auto* extraData = refr->extraList->GetByType(RE::EXTRA_DATA_TYPE::kCount);
                            if (extraData) {
                                int16_t* countPtr = reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(extraData) + 0x18);
                                extraCount = *countPtr;
                            }
                        }

                        std::string containerName = "container";
                        const char* dispName = containerRef->GetDisplayFullName();
                        if (dispName && dispName[0]) containerName = dispName;

                        heisenberg::Hooks::SetSuppressHUDMessages(true);

                        RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                        heisenberg::AddObjectToContainer(
                            containerRef,
                            static_cast<RE::TESBoundObject*>(baseForm),
                            &nullExtra, extraCount, nullptr, 0);

                        SafeDisableRef(refr);

                        heisenberg::Hooks::SetSuppressHUDMessages(false);

                        char msg[256];
                        if (extraCount > 1)
                            snprintf(msg, sizeof(msg), "Stored %s x%d in %s", itemName.c_str(), extraCount, containerName.c_str());
                        else
                            snprintf(msg, sizeof(msg), "Stored %s in %s", itemName.c_str(), containerName.c_str());
                        heisenberg::ShowHUDMessage(msg);

                        spdlog::info("[CONTAINER] Transferred '{}' x{} to {} ({:08X})",
                                    itemName, extraCount, containerName, containerRef->formID);

                        g_vrInput.TriggerHaptic(isLeft, 50000);
                        forStorage = true;
                    }
                }
            }
        }
        
        // =====================================================================
        // STORAGE ZONE CHECK ON RELEASE (FALLBACK - AFTER EQUIP CHECK)
        // =====================================================================
        // Primary storage is auto-triggered after holding in zone for X seconds.
        // This is a FALLBACK for releasing grip while in storage zone.
        // (e.g., if user releases before timer completes)
        // Only triggers if equip zone didn't already handle the item (forStorage check)
        if (!forStorage && !state.isPulling && state.isInStorageZone && !state.skipStorageZone)
        {
            auto& cfgMode = ItemPositionConfigMode::GetSingleton();
            if (!cfgMode.IsRepositionModeActive())
            {
                spdlog::debug("[GRAB] Release in storage zone - storing item!");
                
                RE::TESObjectREFR* refrToStore = refr;
                // Always show HUD message for storage (user wants to know item was stored)
                bool showMessage = true;
                bool stored = StoreGrabbedItem(refrToStore, showMessage);
                
                if (stored)
                {
                    spdlog::debug("[GRAB] Storage succeeded on zone release");
                    g_vrInput.TriggerHaptic(isLeft, 50000);  // Strong haptic for success
                    forStorage = true;  // Continue with forStorage cleanup path
                }
                else
                {
                    spdlog::warn("[GRAB] Storage FAILED on zone release");
                    g_vrInput.TriggerHaptic(isLeft, 5000);  // Short haptic for failure
                }
            }
        }
        
        // =====================================================================
        // NATURAL GRAB OFFSET SAVING
        // =====================================================================
        // If enabled and this was a natural grab, save the current offset for this item
        // This allows users to "teach" the mod how they like to hold items
        // ONLY saves if NO offset exists at all (no generic, no _L, no _R)
        if (g_config.saveNaturalGrabAsOffset && state.isNaturalGrab && !forStorage && refr)
        {
            auto& offsetMgr = ItemOffsetManager::GetSingleton();
            std::string itemName = ItemOffsetManager::GetItemName(refr);
            
            // Check if ANY offset already exists for this item
            // Don't overwrite existing offsets - only create for items with no offset at all
            bool hasGenericOffset = offsetMgr.HasOffset(itemName);
            bool hasLeftOffset = offsetMgr.HasOffset(itemName + "_L");
            bool hasRightOffset = offsetMgr.HasOffset(itemName + "_R");
            
            if (hasGenericOffset || hasLeftOffset || hasRightOffset)
            {
                spdlog::debug("[GRAB] Skipping natural grab offset save for '{}' - offset already exists (generic={}, L={}, R={})",
                             itemName, hasGenericOffset, hasLeftOffset, hasRightOffset);
            }
            else
            {
                // Use the current itemOffset which was calculated during grab
                ItemOffset offsetToSave = state.itemOffset;
                
                // Get item dimensions for the saved offset
                float itemLength = 0, itemWidth = 0, itemHeight = 0;
                ItemOffsetManager::GetItemDimensions(refr, itemLength, itemWidth, itemHeight);
                offsetToSave.length = itemLength;
                offsetToSave.width = itemWidth;
                offsetToSave.height = itemHeight;
                
                // Get form ID for precise matching on reload
                if (auto* baseObj = refr->GetObjectReference()) {
                    std::ostringstream oss;
                    oss << std::hex << std::uppercase << baseObj->formID;
                    offsetToSave.formId = oss.str();
                }
                
                offsetMgr.SaveOffset(itemName, offsetToSave, isLeft);
                spdlog::debug("[GRAB] Saved natural grab offset for '{}' ({} hand): pos=({:.2f}, {:.2f}, {:.2f})",
                            itemName, isLeft ? "LEFT" : "RIGHT", offsetToSave.position.x, offsetToSave.position.y, offsetToSave.position.z);
            }
        }
        
        // If storing to inventory, just cleanup and return
        // The object is about to be deleted anyway, so skip all the physics/worldBound updates
        if (forStorage)
        {
            spdlog::debug("[GRAB] EndGrab for storage - simplified cleanup");
            
            // NOTE: We no longer reparent nodes to the wand (KEYFRAMED mode keeps original parent),
            // so no unparenting is needed. Just clear state.
            
            // Reset fingers to extended position before clearing state
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f))
            {
                spdlog::debug("[GRAB] Reset {} hand fingers to extended after storage", isLeft ? "left" : "right");
            }
            frik.ClearHandPoseFingerPositions(isLeft);
            // Also reset Heisenberg's internal pose value
            Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
            
            // Clear state and return - no physics restoration needed
            state.Clear();
            
            // NOTE: We unequipped weapon on grab start, no need to re-holster
            // The player will manually re-equip if desired
            
            // Notify Heisenberg that grab ended (starts post-grab kFighting suppression)
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            
            auto& configMode = ItemPositionConfigMode::GetSingleton();
            configMode.OnGrabEnded(isLeft);
            return;
        }

        spdlog::debug("[GRAB] EndGrab: Releasing object {:08X}",
                     refr ? refr->formID : 0);

        // Record this ref as recently dropped so ActivateRef hook can block
        // immediate re-activation (Grip>A binding anti-reactivation)
        if (refr) {
            Hooks::RecordDroppedRef(refr->formID);
        }

        // All grabs use KEYFRAMED mode now - use the KEYFRAMED EndGrab path
        // This restores motion type to DYNAMIC and applies throw velocity
        if (state.node)
        {
            EndGrabKeyframed(state, throwVelocity, isLeft);
            return;
        }
        
        // Fallback: If no node, just clear state
        spdlog::warn("[GRAB] EndGrab: No node, clearing state only");
        auto& frik = FRIKInterface::GetSingleton();
        frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        frik.ClearHandPoseFingerPositions(isLeft);
        Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);
        Heisenberg::GetSingleton().OnGrabEnded(isLeft);
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        configMode.OnGrabEnded(isLeft);
        state.Clear();
    }

    bool GrabManager::IsGrabbing(bool isLeft) const
    {
        // Check if we should use constraint-based grabbing (modes 1 or 2)
        int effectiveGrabMode = GetEffectiveGrabMode();
        if (effectiveGrabMode == static_cast<int>(GrabMode::BallSocket) ||
            effectiveGrabMode == static_cast<int>(GrabMode::Motor))
        {
            auto& constraintMgr = ConstraintGrabManager::GetSingleton();
            return constraintMgr.IsGrabbing(isLeft);
        }
        return isLeft ? _leftGrab.active : _rightGrab.active;
    }

    void GrabManager::OverrideNativeGrabPosition(const RE::NiPoint3& handPos)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return;
        
        // Safety check: Don't access player data during startup before game is fully initialized
        // The springs array may be uninitialized/garbage during early startup
        // Check if player has valid 3D and the game is past the loading screen
        if (!player->Get3D())
            return;

        // Check if native system has an active grab (springs array not empty)
        std::size_t springCount = GetSpringCount(player);
        
        // Sanity check - spring count should never be huge (indicates garbage/uninitialized)
        if (springCount > 100) {
            // This is garbage data, skip
            return;
        }
        
        // Log spring count periodically for debugging
        static int logCounter = 0;
        if (++logCounter >= 300) {  // Log every ~5 seconds at 60fps
            spdlog::debug("[GRAB] Periodic check: {} springs active", springCount);
            logCounter = 0;
        }
        
        if (springCount > 0)
        {
            // Log first time we detect native grab
            static bool loggedOnce = false;
            if (!loggedOnce) {
                spdlog::debug("[GRAB] Native grab detected! {} springs active", springCount);
                spdlog::debug("[GRAB] grabDistance: {:.2f}", player->grabDistance);
                loggedOnce = true;
            }
            
            // Override the spring's target position to the hand
            SetMouseSpringPosition(player, handPos);
        }
    }

    const GrabState& GrabManager::GetGrabState(bool isLeft) const
    {
        return isLeft ? _leftGrab : _rightGrab;
    }

    GrabState& GrabManager::GetGrabState(bool isLeft)
    {
        return isLeft ? _leftGrab : _rightGrab;
    }

    bool GrabManager::IsPulling(bool isLeft) const
    {
        const GrabState& state = isLeft ? _leftGrab : _rightGrab;
        return state.active && state.isPulling;
    }
    
    void GrabManager::ClearAllState()
    {
        spdlog::info("[GRAB] ClearAllState - resetting all grab state");
        
        // Clear left hand grab state
        if (_leftGrab.active) {
            spdlog::debug("[GRAB] Clearing active left grab");
            _leftGrab.Clear();
        }
        
        // Clear right hand grab state
        if (_rightGrab.active) {
            spdlog::debug("[GRAB] Clearing active right grab");
            _rightGrab.Clear();
        }
        
        // Clear any pending holster request
        _pendingHolster.pending = false;
    }

    void GrabManager::ForceReleaseAll()
    {
        spdlog::info("[GRAB] ForceReleaseAll - properly releasing all grabbed objects");
        
        // Use EndGrab to properly restore physics (KEYFRAMED → DYNAMIC),
        // sync physics body position, restore collision layers, and apply gravity nudge.
        // This ensures the game saves/loads objects at their current position with correct physics.
        if (_leftGrab.active) {
            spdlog::debug("[GRAB] Force-releasing left hand grab");
            EndGrab(true, nullptr, false);
        }
        
        if (_rightGrab.active) {
            spdlog::debug("[GRAB] Force-releasing right hand grab");
            EndGrab(false, nullptr, false);
        }
    }

    bool GrabManager::StartGrabOnRef(RE::TESObjectREFR* refr, bool isLeft, bool stickyGrab, bool instantGrab, bool skipWeaponEquip, bool forceOffset)
    {
        if (!refr) {
            spdlog::warn("[GRAB] StartGrabOnRef: null refr");
            return false;
        }
        
        auto* node = refr->Get3D();
        if (!node) {
            spdlog::warn("[GRAB] StartGrabOnRef: refr {:08X} has no 3D", refr->formID);
            return false;
        }
        
        // Get hand position and rotation
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) {
            spdlog::warn("[GRAB] StartGrabOnRef: could not get player nodes");
            return false;
        }
        
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!wandNode) {
            spdlog::warn("[GRAB] StartGrabOnRef: could not get wand node");
            return false;
        }
        
        RE::NiPoint3 handPos = wandNode->world.translate;
        RE::NiMatrix3 handRot = wandNode->world.rotate;
        
        // Calculate actual distance from hand to object
        RE::NiPoint3 objPos = node->world.translate;
        RE::NiPoint3 diff = objPos - handPos;
        float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
        
        // If forceOffset is true (LootToHand), use sentinel value to signal DropToHand path
        if (forceOffset) {
            distance = 99999.0f;  // Sentinel value — detected by StartGrab to force offset loading
            spdlog::debug("[GRAB] StartGrabOnRef: forceOffset=true, using sentinel distance for offset loading");
        }
        
        // Create a Selection for this object
        Selection selection;
        selection.SetRefr(refr);
        selection.node.reset(node);  // NiPointer assignment from raw pointer
        selection.hitPoint = objPos;  // Use object center as hit point
        selection.hitNormal = RE::NiPoint3(0, 0, 1);  // Default up normal
        selection.distance = distance;  // Actual distance - allows pull if far
        selection.isClose = (distance < g_config.snapDistance);
        
        spdlog::debug("[GRAB] StartGrabOnRef: starting grab on {:08X} in {} hand (sticky={}, dist={:.1f}, skipEquip={}, forceOffset={})",
                     refr->formID, isLeft ? "left" : "right", stickyGrab, distance, skipWeaponEquip, forceOffset);
        
        bool result = StartGrab(selection, handPos, handRot, isLeft, skipWeaponEquip);
        
        if (!result) {
            spdlog::warn("[GRAB] StartGrabOnRef: StartGrab returned false for {:08X}", refr->formID);
        }
        
        if (result) {
            GrabState& state = isLeft ? _leftGrab : _rightGrab;
            
            // Track grab start time for equip protection
            state.grabStartTime = static_cast<float>(Utils::GetTime());

            // Holotape scaling already handled in StartGrab()

            // Override sticky grab based on caller's explicit request.
            // StartGrab() may have already set stickyGrab=true via g_config.enableStickyGrab,
            // but the caller's explicit parameter should take priority.
            if (stickyGrab) {
                state.stickyGrab = true;
                state.isFromLootDrop = true;  // Mark for 2s equip protection
                spdlog::debug("[GRAB] StartGrabOnRef: Enabled sticky grab for dropped item");
            } else {
                state.stickyGrab = false;  // Caller explicitly wants non-sticky (e.g., holotape removal)
            }
            
            // Instant grab: teleport item IMMEDIATELY to hand position
            // This prevents the "drop from sky" effect where item spawns at skeleton position
            // for one frame before moving to hand
            //
            // SIMPLIFIED APPROACH: Only do visual teleport and flag clearing.
            // Let the normal grab path handle physics setup - this treats dropped items
            // the same as world items, avoiding special-case bugs.
            if (instantGrab) {
                spdlog::debug("[GRAB] ========== INSTANT GRAB (SIMPLIFIED) ==========");
                spdlog::debug("[GRAB] RefID: {:08X}, hasItemOffset: {}, hasFingerCurls: {}",
                            refr->formID, state.hasItemOffset, state.itemOffset.hasFingerCurls);
                
                // Calculate target position at hand with item offset
                RE::NiPoint3 targetPos = handPos + state.itemOffset.position;
                RE::NiMatrix3 targetRot = state.itemOffset.rotation * handRot;
                
                // Disable pull animation - item goes directly to hand
                state.isPulling = false;
                state.pullProgress = 1.0f;
                state.grabOffsetLocal = state.itemOffset.position;
                state.initialObjectPos = targetPos;

                // Apply any deferred finger curls (StartGrab may have deferred them
                // because isPulling was true at that point)
                if (state.pendingFingerCurls && state.hasItemOffset && state.itemOffset.hasFingerCurls) {
                    auto& frik2 = FRIKInterface::GetSingleton();
                    if (frik2.IsAvailable()) {
                        frik2.SetHandPoseFingerPositions(isLeft,
                            state.itemOffset.thumbCurl, state.itemOffset.indexCurl,
                            state.itemOffset.middleCurl, state.itemOffset.ringCurl, state.itemOffset.pinkyCurl);
                        Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, state.itemOffset.thumbCurl);
                    }
                    state.pendingFingerCurls = false;
                }
                
                // Update visual node position immediately
                if (state.node) {
                    spdlog::debug("[GRAB] Teleporting node '{}' to ({:.1f},{:.1f},{:.1f})",
                                state.node->name.c_str(), targetPos.x, targetPos.y, targetPos.z);
                    
                    RE::NiTransform targetTransform;
                    targetTransform.translate = targetPos;
                    targetTransform.rotate = targetRot;
                    targetTransform.scale = state.node->local.scale;
                    Utils::UpdateKeyframedNode(state.node.get(), targetTransform);
                    
                    // Sync refr->data.location to prevent ghosting/culling
                    RE::TESObjectREFR* stateRefr = state.GetRefr();
                    if (stateRefr) {
                        stateRefr->data.location.x = targetPos.x;
                        stateRefr->data.location.y = targetPos.y;
                        stateRefr->data.location.z = targetPos.z;
                    }
                    
                    // NOTE: Removed ClearCullingFlagsRecursive - Skyrim HIGGS doesn't use it.
                    // Syncing refr->data.location is sufficient. The culling flag manipulation
                    // was causing race conditions with the render thread.
                    
                    spdlog::debug("[GRAB] Visual teleport complete - letting normal grab path handle physics");
                }
                
                // DON'T do special physics setup here!
                // Let the normal UpdateGrab -> SetupGrabPhysics path handle it.
                // This treats dropped items the same as world items.
            }
        }
        
        return result;
    }

    bool GrabManager::SetupGrabPhysics(GrabState& state)
    {
        if (!state.collisionObject || !state.node)
            return false;

        // Get bhkWorld for locked physics operations
        RE::TESObjectREFR* stateRefr = state.GetRefr();
        RE::bhkWorld* bhkWorld = GetBhkWorldFromRefr(stateRefr);

        // TODO: Save current motion type
        // The exact way to query current motion type depends on available APIs
        // For now, assume DYNAMIC is the default for movable objects
        state.savedState.motionType = RE::hknpMotionPropertiesId::Preset::DYNAMIC;

        // Set to KEYFRAMED using bhkWorld_SetMotion - this properly updates collision filters
        // The 3 bool params (true,true,true) enable collision filter updates
        bhkWorld_SetMotionLocked(state.node.get(), RE::hknpMotionPropertiesId::Preset::KEYFRAMED, true, true, true, bhkWorld);

        spdlog::debug("SetupGrabPhysics: Set motion type to KEYFRAMED via bhkWorld_SetMotion");
        return true;
    }

    void GrabManager::RestorePhysics(GrabState& state)
    {
        if (!state.collisionObject || !state.node)
            return;

        // Get bhkWorld for locked physics operations
        RE::TESObjectREFR* stateRefr = state.GetRefr();
        RE::bhkWorld* bhkWorld = GetBhkWorldFromRefr(stateRefr);

        // Restore original motion type using bhkWorld_SetMotion for consistency
        bhkWorld_SetMotionLocked(state.node.get(), state.savedState.motionType, true, true, true, bhkWorld);
        
        spdlog::debug("RestorePhysics: Restored motion type to {} via bhkWorld_SetMotion", 
                      static_cast<int>(state.savedState.motionType));
    }

    void GrabManager::ApplyGrabVelocity(GrabState& state, const RE::NiPoint3& targetPos,
                                        const RE::NiMatrix3& targetRot, float deltaTime)
    {
        // This method would be used for velocity-based grabbing instead of KEYFRAMED
        // Keeping as placeholder for potential alternative implementation
        //
        // The velocity approach would:
        // 1. Calculate position error = targetPos - currentPos
        // 2. Calculate desired velocity = positionError * grabStrength
        // 3. Clamp to maxVelocity
        // 4. Apply to the body via SetLinearVelocity or similar
        //
        // This gives more natural feel but requires finding the velocity APIs
    }
    
    void GrabManager::UpdateVirtualSpring(GrabState& state, const RE::NiPoint3& targetPos,
                                          const RE::NiMatrix3& targetRot, float deltaTime)
    {
        // SAFETY CHECK: Verify grabbed object is still valid before accessing
        // The game can delete objects at any time (cell unload, script, etc.)
        // If we access a dangling pointer, we'll crash on virtual function calls
        RE::TESObjectREFR* stateRefr = state.GetRefr();
        if (!stateRefr || stateRefr->IsDeleted()) {
            spdlog::warn("[GRAB] UpdateVirtualSpring: refr is null or deleted, forcing release");
            state.active = false;
            return;
        }
        
        // Verify the 3D object still exists and matches what we grabbed
        auto* current3D = stateRefr->Get3D();
        if (!current3D) {
            spdlog::warn("[GRAB] UpdateVirtualSpring: refr has no 3D, forcing release");
            state.active = false;
            return;
        }
        
        // If state.node doesn't match the current 3D root, it's a dangling pointer
        // (This can happen if the game recreated the 3D for this object)
        if (state.node && state.node.get() != current3D) {
            // Check if state.node is still a valid child of current3D
            bool nodeStillValid = false;
            if (current3D->IsNode()) {
                // Simple check: is our node the root or reachable from root?
                // For now just check if it's the same or if parent chain leads to current3D
                RE::NiNode* parent = state.node->parent;
                while (parent) {
                    if (parent == current3D) {
                        nodeStillValid = true;
                        break;
                    }
                    parent = parent->parent;
                }
            }
            if (!nodeStillValid && state.node.get() != current3D) {
                spdlog::warn("[GRAB] UpdateVirtualSpring: state.node is stale (3D was recreated), forcing release");
                state.active = false;
                return;
            }
        }
        
        // Store target for release
        state.lastTargetPos = targetPos;
        state.lastTargetRot = targetRot;
        
        // Check if in reposition mode - use simpler velocity-based approach from backup
        // This approach works well for repositioning because the player is stationary
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        if (configMode.IsRepositionModeActive() && state.stickyGrab)
        {
            // REPOSITION MODE: Use simple velocity-based approach (like Dec 11 backup)
            // This naturally works with frozen world position
            if (!state.collisionObject || !state.node)
                return;
            
            // Get current object position from node
            RE::NiPoint3 currentPos = state.node->world.translate;
            
            // Calculate position error (how far from target)
            RE::NiPoint3 posError = targetPos - currentPos;
            float errorMagnitude = std::sqrt(posError.x * posError.x + 
                                             posError.y * posError.y + 
                                             posError.z * posError.z);
            
            // Spring parameters
            constexpr float springStrength = 20.0f;   // How aggressively to correct position
            constexpr float maxVelocity = 2000.0f;    // Max velocity in game units/s
            
            // Calculate desired velocity = springStrength * positionError
            RE::NiPoint3 desiredVelocity;
            desiredVelocity.x = posError.x * springStrength;
            desiredVelocity.y = posError.y * springStrength;
            desiredVelocity.z = posError.z * springStrength;
            
            // Clamp velocity magnitude
            float velMagnitude = std::sqrt(desiredVelocity.x * desiredVelocity.x +
                                           desiredVelocity.y * desiredVelocity.y +
                                           desiredVelocity.z * desiredVelocity.z);
            if (velMagnitude > maxVelocity)
            {
                float scale = maxVelocity / velMagnitude;
                desiredVelocity.x *= scale;
                desiredVelocity.y *= scale;
                desiredVelocity.z *= scale;
            }
            
            // Get bhkWorld for locked physics operations
            RE::TESObjectREFR* stateRefrRepos = state.GetRefr();
            RE::bhkWorld* bhkWorldRepos = stateRefrRepos ? GetBhkWorldFromRefr(stateRefrRepos) : nullptr;
            
            // Convert to Havok units and apply
            RE::NiPoint4 hkVelocity(
                desiredVelocity.x * HAVOK_WORLD_SCALE,
                desiredVelocity.y * HAVOK_WORLD_SCALE,
                desiredVelocity.z * HAVOK_WORLD_SCALE,
                0.0f
            );
            
            SetLinearVelocityLocked(state.collisionObject, hkVelocity, bhkWorldRepos);
            
            // Lock rotation - set angular velocity to zero
            RE::NiPoint4 zeroAngVel(0.0f, 0.0f, 0.0f, 0.0f);
            SetAngularVelocityLocked(state.collisionObject, zeroAngVel, bhkWorldRepos);
            
            // Also update the visual node to match physics
            if (state.node)
            {
                RE::NiTransform desiredTransform;
                desiredTransform.translate = targetPos;
                desiredTransform.rotate = targetRot;
                desiredTransform.scale = state.node->world.scale;
                Utils::UpdateKeyframedNode(state.node.get(), desiredTransform);
            }
            
            // Debug log every ~60 frames
            static int reposLogCount = 0;
            if (++reposLogCount >= 60) {
                reposLogCount = 0;
                spdlog::debug("[GRAB-REPOSITION] frozen=({:.1f},{:.1f},{:.1f}) current=({:.1f},{:.1f},{:.1f}) error={:.1f}",
                             targetPos.x, targetPos.y, targetPos.z,
                             currentPos.x, currentPos.y, currentPos.z,
                             errorMagnitude);
            }
            return;
        }
        
        // For PROXY objects: Update local transform relative to original parent to position at target
        // For normal objects: Node is parented to wand, update local/world transforms
        
        // Debug: Check which conditions are failing
        static int vsDebugCounter = 0;
        if (++vsDebugCounter >= 120) {
            vsDebugCounter = 0;
            spdlog::debug("[GRAB-VS-DEBUG] usingKeyframedMode={} node={} originalParent={} setupComplete={}",
                         state.usingKeyframedMode ? "TRUE" : "FALSE",
                         state.node ? "OK" : "NULL",
                         state.originalParent ? "OK" : "NULL",
                         state.keyframedSetupComplete ? "TRUE" : "FALSE");
        }
        
        if (state.usingKeyframedMode && state.node && state.originalParent)
        {
            // KEYFRAMED MODE: Use KEYFRAMED physics + visual updates
            // Update both the visual node AND the physics body
            
            RE::NiTransform& parentWorld = state.originalParent->world;
            float invScale = 1.0f / parentWorld.scale;
            RE::NiMatrix3 parentRotInv = parentWorld.rotate.Transpose();
            
            // Debug: Log parent world transform occasionally
            static int parentDebugCounter = 0;
            if (++parentDebugCounter >= 120) {
                parentDebugCounter = 0;
                spdlog::debug("[GRAB-KEYFRAMED] Parent '{}' world: pos=({:.1f},{:.1f},{:.1f}) rot[0]=({:.2f},{:.2f},{:.2f}) scale={:.2f}",
                             state.originalParent->name.c_str(),
                             parentWorld.translate.x, parentWorld.translate.y, parentWorld.translate.z,
                             parentWorld.rotate.entry[0][0], parentWorld.rotate.entry[0][1], parentWorld.rotate.entry[0][2],
                             parentWorld.scale);
            }
            
            // Compute local position from target world position
            state.node->local.translate = parentRotInv * ((targetPos - parentWorld.translate) * invScale);
            
            // Compute local rotation from target world rotation
            state.node->local.rotate = targetRot * parentRotInv;
            
            // Update world transforms for visual
            UpdateWorldTransformRecursive(state.node.get(), parentWorld);
            
            // Update world bounding volume to prevent culling
            state.node->worldBound.center = targetPos;
            if (state.originalParent)
            {
                // Validate originalParent pointer before use
                uintptr_t parentAddr = reinterpret_cast<uintptr_t>(state.originalParent.get());
                if (parentAddr > 0x10000) {  // Valid pointer range
                    state.originalParent->worldBound.center = targetPos;
                }
            }
            
            // Call the engine's UpdateWorldBound function for proper bounding sphere propagation
            // Use IsNode() to safely verify it's actually a NiNode before calling
            // SAFETY: Wrap in SEH to catch corrupted node access
            RE::NiNode* niNode = state.node ? state.node->IsNode() : nullptr;
            if (niNode)
            {
                __try {
                    (*f4cf::f4vr::NiNode_UpdateWorldBound)(niNode);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    spdlog::error("[GRAB] Exception in NiNode_UpdateWorldBound - node may be corrupted");
                }
            }
            
            // CRITICAL: Also update the physics body via SetTransform + ApplyHardKeyframe
            // This moves the KEYFRAMED physics body to match the visual
            // SetTransform directly sets position (prevents drift)
            // ApplyHardKeyframe sets velocity for collision response
            if (state.collisionObject)
            {
                RE::hkTransformf targetTransform;
                targetTransform.rotation = targetRot;
                targetTransform.translation = RE::NiPoint4(
                    targetPos.x * HAVOK_WORLD_SCALE,
                    targetPos.y * HAVOK_WORLD_SCALE,
                    targetPos.z * HAVOK_WORLD_SCALE,
                    0.0f
                );
                
                // Use both SetTransform (direct positioning) + ApplyHardKeyframe (velocity for collision)
                float clampedDelta = (deltaTime > 0.001f) ? deltaTime : 0.016f;
                float invDeltaTime = 1.0f / clampedDelta;
                RE::TESObjectREFR* stateRefrProxy = state.GetRefr();
                RE::bhkWorld* bhkWorldProxy = stateRefrProxy ? GetBhkWorldFromRefr(stateRefrProxy) : nullptr;
                if (bhkWorldProxy) {
                    SetTransformLocked(state.collisionObject, targetTransform, bhkWorldProxy);
                    ApplyHardKeyframeLocked(state.collisionObject, targetTransform, invDeltaTime, bhkWorldProxy);
                }
            }
            
            // Keep TESObjectREFR position in sync with visual
            // This prevents teleportation to cell origin on save/load or cell transitions
            auto stateRefrSync = state.GetRefr();
            if (stateRefrSync)
            {
                stateRefrSync->data.location.x = targetPos.x;
                stateRefrSync->data.location.y = targetPos.y;
                stateRefrSync->data.location.z = targetPos.z;
            }
            
            // Debug log every ~60 frames
            static int proxyDebugCounter = 0;
            if (++proxyDebugCounter >= 60)
            {
                proxyDebugCounter = 0;
                spdlog::debug("[GRAB-PROXY] node='{}' targetPos=({:.1f},{:.1f},{:.1f}) nodeWorld=({:.1f},{:.1f},{:.1f})",
                             state.node->name.c_str(),
                             targetPos.x, targetPos.y, targetPos.z,
                             state.node->world.translate.x, state.node->world.translate.y, state.node->world.translate.z);
                spdlog::debug("[GRAB-PROXY] targetRot[0]=({:.2f},{:.2f},{:.2f}) nodeRot[0]=({:.2f},{:.2f},{:.2f}) localRot[0]=({:.2f},{:.2f},{:.2f})",
                             targetRot.entry[0][0], targetRot.entry[0][1], targetRot.entry[0][2],
                             state.node->world.rotate.entry[0][0], state.node->world.rotate.entry[0][1], state.node->world.rotate.entry[0][2],
                             state.node->local.rotate.entry[0][0], state.node->local.rotate.entry[0][1], state.node->local.rotate.entry[0][2]);
                // Log physics node if different
                if (state.collisionObject && state.collisionObject->sceneObject != state.node.get()) {
                    spdlog::debug("[GRAB-PROXY] Physics on DIFFERENT node: '{}'", 
                                 state.collisionObject->sceneObject->name.c_str());
                }
            }
        }
        else if (state.usingKeyframedMode && state.node && !state.originalParent)
        {
            // KEYFRAMED MODE FALLBACK: originalParent is NULL (common for dropped items from inventory)
            // We can't calculate local transforms, but we CAN update world transforms and physics directly
            
            // Update world transform directly
            // Preserve local.scale (e.g. holotapes use 0.7) instead of forcing 1.0
            state.node->world.translate = targetPos;
            state.node->world.rotate = targetRot;
            state.node->world.scale = state.node->local.scale > 0.0f ? state.node->local.scale : 1.0f;
            
            // Update world bounding volume to prevent culling
            state.node->worldBound.center = targetPos;
            
            // Update child nodes recursively with an identity parent transform
            RE::NiTransform identityParent;
            identityParent.translate = RE::NiPoint3(0, 0, 0);
            identityParent.rotate = RE::NiMatrix3();  // Identity rotation
            identityParent.rotate.entry[0][0] = 1.0f;
            identityParent.rotate.entry[1][1] = 1.0f;
            identityParent.rotate.entry[2][2] = 1.0f;
            identityParent.scale = 1.0f;
            
            // For nodes without parents, we set local = world
            state.node->local = state.node->world;
            
            // Call engine's UpdateWorldBound - use IsNode() to safely verify type
            // SAFETY: Wrap in SEH to catch corrupted node access
            RE::NiNode* niNode = state.node ? state.node->IsNode() : nullptr;
            if (niNode) {
                __try {
                    (*f4cf::f4vr::NiNode_UpdateWorldBound)(niNode);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    spdlog::error("[GRAB] Exception in NiNode_UpdateWorldBound (no parent) - node may be corrupted");
                }
            }
            
            // Update physics body via SetTransform + ApplyHardKeyframe
            // SetTransform directly sets position (prevents drift)
            // ApplyHardKeyframe sets velocity for collision response
            if (state.collisionObject)
            {
                RE::hkTransformf targetTransform;
                targetTransform.rotation = targetRot;
                targetTransform.translation = RE::NiPoint4(
                    targetPos.x * HAVOK_WORLD_SCALE,
                    targetPos.y * HAVOK_WORLD_SCALE,
                    targetPos.z * HAVOK_WORLD_SCALE,
                    0.0f
                );
                
                float clampedDelta = (deltaTime > 0.001f) ? deltaTime : 0.016f;
                float invDeltaTime = 1.0f / clampedDelta;
                RE::TESObjectREFR* stateRefrFallback = state.GetRefr();
                RE::bhkWorld* bhkWorldProxy = stateRefrFallback ? GetBhkWorldFromRefr(stateRefrFallback) : nullptr;
                if (bhkWorldProxy) {
                    SetTransformLocked(state.collisionObject, targetTransform, bhkWorldProxy);
                    ApplyHardKeyframeLocked(state.collisionObject, targetTransform, invDeltaTime, bhkWorldProxy);
                }
            }
            
            // Keep TESObjectREFR position in sync
            auto stateRefrFallbackSync = state.GetRefr();
            if (stateRefrFallbackSync)
            {
                stateRefrFallbackSync->data.location.x = targetPos.x;
                stateRefrFallbackSync->data.location.y = targetPos.y;
                stateRefrFallbackSync->data.location.z = targetPos.z;
            }
            
            // Debug log occasionally
            static int proxyNullParentCounter = 0;
            if (++proxyNullParentCounter >= 120) {
                proxyNullParentCounter = 0;
                spdlog::debug("[GRAB-PROXY-NULLPARENT] node='{}' pos=({:.1f},{:.1f},{:.1f})",
                             state.node->name.c_str(), targetPos.x, targetPos.y, targetPos.z);
            }
        }
        
        // NOTE: Scene graph parenting code has been removed. All grabs now use KEYFRAMED mode
        // which keeps objects in their original parent and updates position via physics.
        // See git history for the old parenting implementation if needed.
    }

    void GrabManager::PostPhysicsUpdate()
    {
        // POST-PHYSICS: For VirtualSpring mode, no extra work needed
        // The velocity-based approach handles everything in pre-physics
        // (The physics sim will have applied our velocity)
    }
    
    void GrabManager::PrePhysicsUpdate()
    {
        // =========================================================================
        // SIMPLIFIED VISUAL-ONLY UPDATE (Testing minimal approach)
        // =========================================================================
        // This version ONLY updates the visual node to match wand position.
        // No player movement prediction, no physics updates.
        // Goal: Establish baseline to determine if lag is from our code or external.
        // =========================================================================
        
        // Check for blocking menus - but CONTINUE if we have an active grab
        // This is critical for DropToHand: items spawn while PipboyMenu is open,
        // and we need to keep updating their position or they'll appear frozen
        // at the spawn location until the menu closes.
        // Use cached menu state from MenuChecker for thread safety (avoids race conditions)
        auto& menuChecker = MenuChecker::GetSingleton();
        bool menuBlocking = menuChecker.IsPipboyOpen() || menuChecker.IsPaused() || 
                            menuChecker.IsLoading() || menuChecker.IsMainMenu();
        
        // Only skip if menu is blocking AND we have no active grabs
        bool hasActiveGrab = _leftGrab.active || _rightGrab.active;
        if (menuBlocking && !hasActiveGrab)
            return;
        
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes)
            return;
        
        // Check if repositioning mode is active
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        bool repositionActive = configMode.IsRepositionModeActive() && configMode.HasFrozenPosition();

        auto updateGrabVisual = [&](GrabState& state, bool isLeft) {
            if (!state.active || !state.node)
                return;

            // CRITICAL: Validate reference via handle lookup BEFORE any method calls!
            if (!state.HasValidRefr())
                return;

            if (!state.keyframedSetupComplete)
                return;

            // Verify 3D is still valid
            RE::TESObjectREFR* stateRefr = state.GetRefr();
            if (!stateRefr || !stateRefr->Get3D())
                return;

            RE::NiPoint3 targetPos;
            RE::NiMatrix3 targetRot;

            if (repositionActive && state.stickyGrab) {
                // Reposition mode: use frozen world position (adjusted by thumbsticks)
                targetPos = configMode.GetFrozenWorldPos();
                targetRot = configMode.GetFrozenWorldRot();
            } else {
                // Normal mode: calculate from hand/weapon position + offset
                RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
                if (!wandNode)
                    return;

                RE::NiPoint3 parentPos = wandNode->world.translate;
                RE::NiMatrix3 parentRot = wandNode->world.rotate;

                // For FRIK offsets, use the FRIK skeleton hand node (LArm_Hand/RArm_Hand)
                if (state.isFRIKOffset) {
                    RE::NiPoint3 frikParentPos;
                    RE::NiMatrix3 frikParentRot;
                    if (GetFRIKWeaponParentTransform(isLeft, frikParentPos, frikParentRot)) {
                        parentPos = frikParentPos;
                        parentRot = frikParentRot;
                    }
                }

                // Calculate target: parent + item offset
                targetRot = state.itemOffset.rotation * parentRot;
                RE::NiPoint3 localOffset = state.itemOffset.position;

                // Power armor glove compensation (same as UpdateGrab)
                if (Utils::IsPlayerInPowerArmor()) {
                    localOffset.x += g_config.paGrabOffsetX;
                    localOffset.y += g_config.paGrabOffsetY;
                    localOffset.z += g_config.paGrabOffsetZ;
                }

                RE::NiPoint3 rotatedOffset = parentRot.Transpose() * localOffset;
                targetPos = parentPos + rotatedOffset;

                // Handle pull animation
                if (state.isPulling && state.pullProgress < 1.0f)
                {
                    float t = state.pullProgress;
                    RE::NiPoint3 finalPos = parentPos + (parentRot.Transpose() * state.itemOffset.position);
                    targetPos = state.initialObjectPos * (1.0f - t) + finalPos * t;
                }
            }
            
            // =====================================================================
            // VISUAL UPDATE - Update node and propagate to children
            // =====================================================================
            // We MUST propagate to children because the visible mesh is often on a
            // child node, not the root. Without this, the object appears to stay in place.
            //
            // NOTE: The crash we fixed was from iterating PLAYER SKELETON children
            // during weapon checks - NOT from iterating GRABBED OBJECT children.
            // These are completely different node hierarchies.
            // =====================================================================
            RE::NiTransform desiredTransform;
            desiredTransform.translate = targetPos;
            desiredTransform.rotate = targetRot;
            desiredTransform.scale = state.node->local.scale > 0.0f ? state.node->local.scale : 1.0f;
            
            Utils::UpdateKeyframedNode(state.node.get(), desiredTransform);
            
            // Also update refr->data.location to prevent ghosting
            if (stateRefr)
            {
                stateRefr->data.location.x = targetPos.x;
                stateRefr->data.location.y = targetPos.y;
                stateRefr->data.location.z = targetPos.z;
            }
            
            // NOTE: We used to clear culling flags every frame here, but that caused
            // race conditions with the render thread (crash reading node flags).
            // Instead, we only clear flags once when grab starts (in StartGrabOnRef).
            // Syncing refr->data.location above should be sufficient to prevent culling.
            
            // Debug logging every ~30 frames
            static int logCounter = 0;
            if (++logCounter >= 30)
            {
                logCounter = 0;
                RE::NiPoint3 objPos = state.node->world.translate;
                float lag = (objPos - targetPos).Length();
                spdlog::debug("[VISUAL-ONLY] {} hand: target=({:.1f},{:.1f},{:.1f}) obj=({:.1f},{:.1f},{:.1f}) lag={:.2f} repos={}",
                    isLeft ? "Left" : "Right",
                    targetPos.x, targetPos.y, targetPos.z,
                    objPos.x, objPos.y, objPos.z, lag, repositionActive);
            }
        };
        
        updateGrabVisual(_leftGrab, true);
        updateGrabVisual(_rightGrab, false);
    }
    
    // =========================================================================
    // PRE-RENDER UPDATE - Final visual sync just before rendering
    // =========================================================================
    // This runs AFTER player movement is applied but BEFORE rendering.
    // By this point, the wand world position includes all player movement.
    // No prediction needed - we just read the final wand position and sync.
    // =========================================================================
    void GrabManager::PreRenderUpdate()
    {
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes)
            return;
            
        // Check for blocking menus - but CONTINUE if we have an active grab
        // This is critical for DropToHand: items spawn while PipboyMenu is open,
        // and we need to keep updating their position or they'll appear frozen
        // Use cached menu state from MenuChecker for thread safety
        auto& menuChecker = MenuChecker::GetSingleton();
        bool menuBlocking = menuChecker.IsPipboyOpen() || menuChecker.IsPaused() || 
                            menuChecker.IsLoading() || menuChecker.IsMainMenu();
        bool hasActiveGrab = _leftGrab.active || _rightGrab.active;
        if (menuBlocking && !hasActiveGrab)
            return;
        
        // Check if repositioning mode is active
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        bool repositionActive = configMode.IsRepositionModeActive() && configMode.HasFrozenPosition();

        // Update each active grab's visual position
        auto updateVisualPosition = [&](GrabState& state, bool isLeft) {
            if (!state.active || !state.node)
                return;

            // CRITICAL: Validate reference via formID lookup
            if (!state.HasValidRefr())
                return;

            RE::NiPoint3 targetPos;
            RE::NiMatrix3 targetRot;

            if (repositionActive && state.stickyGrab) {
                // Reposition mode: use frozen world position
                targetPos = configMode.GetFrozenWorldPos();
                targetRot = configMode.GetFrozenWorldRot();
            } else {
                // Get FINAL wand position (after all player movement applied)
                RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
                if (!wandNode)
                    return;

                RE::NiPoint3 handPos = wandNode->world.translate;
                RE::NiMatrix3 handRot = wandNode->world.rotate;

                // Calculate target transform (same formula as PrePhysicsUpdate but NO prediction)
                targetRot = state.itemOffset.rotation * handRot;
                RE::NiPoint3 localOffset = state.itemOffset.position;

                // Power armor glove compensation (same as UpdateGrab)
                if (Utils::IsPlayerInPowerArmor()) {
                    localOffset.x += g_config.paGrabOffsetX;
                    localOffset.y += g_config.paGrabOffsetY;
                    localOffset.z += g_config.paGrabOffsetZ;
                }

                RE::NiPoint3 rotatedOffset = handRot.Transpose() * localOffset;
                targetPos = handPos + rotatedOffset;
            }

            // Update visual node to final position
            state.node->world.translate = targetPos;
            state.node->world.rotate = targetRot;
            
            // Also sync refr location to prevent ghosting
            auto stateRefrVisual = state.GetRefr();
            if (stateRefrVisual)
            {
                stateRefrVisual->data.location.x = targetPos.x;
                stateRefrVisual->data.location.y = targetPos.y;
                stateRefrVisual->data.location.z = targetPos.z;
            }
            
            // NOTE: We used to clear culling flags every frame here, but that caused
            // race conditions with the render thread. Only clear once on grab start now.
        };
        
        updateVisualPosition(_leftGrab, true);
        updateVisualPosition(_rightGrab, false);
    }

    RE::bhkNPCollisionObject* GrabManager::GetCollisionObject(RE::TESObjectREFR* refr)
    {
        if (!refr)
            return nullptr;

        // Get the 3D node
        auto* root = refr->Get3D();
        if (!root)
        {
            spdlog::warn("GetCollisionObject: No 3D on ref {:08X}", refr->formID);
            return nullptr;
        }

        spdlog::debug("GetCollisionObject: Got 3D for ref {:08X}, root node name='{}'", 
                      refr->formID, root->name.c_str());

        // Try to initialize havok physics if not already done
        // Some objects don't have physics until touched
        f4cf::f4vr::TESObjectREFR_InitHavokForCollisionObject(refr);

        // Helper lambda to check and cast collision object
        // NOTE: CommonLibF4VR doesn't properly override IsbhkNPCollisionObject() in bhkNPCollisionObject,
        // so we need to check RTTI manually.
        // IMPORTANT: bhkNPCollisionProxyObject stores a pointer to the actual bhkNPCollisionObject at offset 0x20.
        // We follow this pointer to get the real physics object for grabbing.
        auto tryCastCollision = [](RE::NiCollisionObject* collObj) -> RE::bhkNPCollisionObject* {
            if (!collObj) return nullptr;
            
            // Check the RTTI name to verify the actual type
            auto* rtti = collObj->GetRTTI();
            if (!rtti || !rtti->GetName()) {
                spdlog::debug("tryCastCollision: No RTTI on collision object");
                return nullptr;
            }
            
            const char* typeName = rtti->GetName();
            spdlog::debug("tryCastCollision: RTTI type = '{}'", typeName);
            
            // Handle proxy objects - follow the target pointer to get the real collision object
            if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
                spdlog::debug("tryCastCollision: Found ProxyObject - following target pointer");
                
                // Get the target collision object from the proxy (at offset 0x20)
                RE::bhkNPCollisionObject* target = GetProxyTarget(collObj);
                if (!target) {
                    spdlog::warn("tryCastCollision: ProxyObject has null target!");
                    return nullptr;
                }
                
                // Log info about the target
                auto* targetRtti = target->GetRTTI();
                if (targetRtti && targetRtti->GetName()) {
                    spdlog::debug("tryCastCollision: ProxyObject target type = '{}', ptr={:016X}",
                                 targetRtti->GetName(), (uintptr_t)target);
                }
                
                // Log what node the target collision is attached to
                RE::NiAVObject* targetSceneObj = target->sceneObject;
                if (targetSceneObj) {
                    spdlog::debug("tryCastCollision: Target's sceneObject = '{}' at {:016X}",
                                 targetSceneObj->name.c_str(), (uintptr_t)targetSceneObj);
                } else {
                    spdlog::warn("tryCastCollision: Target has NULL sceneObject!");
                }
                
                return target;
            }
            
            // Accept bhkNPCollisionObject (and any derived types that aren't proxy)
            if (std::strcmp(typeName, "bhkNPCollisionObject") == 0) {
                return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
            }
            
            // Check if it's a derived type by walking the inheritance chain
            for (auto iter = rtti; iter; iter = iter->GetBaseRTTI()) {
                if (iter->GetName() && std::strcmp(iter->GetName(), "bhkNPCollisionObject") == 0) {
                    // Accept derived types of bhkNPCollisionObject (but not proxy which we handled above)
                    return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
                }
            }
            
            spdlog::debug("tryCastCollision: Type '{}' is not a bhkNPCollisionObject", typeName);
            return nullptr;
        };

        // Check the root's collision object member directly
        auto* collisionObj = root->collisionObject.get();
        if (collisionObj)
        {
            spdlog::debug("GetCollisionObject: Found collision object on root");
            auto* npCollObj = tryCastCollision(collisionObj);
            if (npCollObj)
            {
                spdlog::debug("GetCollisionObject: Successfully cast to bhkNPCollisionObject");
                return npCollObj;
            }
        }
        else
        {
            spdlog::debug("GetCollisionObject: No collision object on root node");
        }

        // If not on root, try to find it in children (recursive search)
        std::function<RE::bhkNPCollisionObject*(RE::NiAVObject*)> searchNode = 
            [&](RE::NiAVObject* node) -> RE::bhkNPCollisionObject* {
            if (!node) return nullptr;
            
            // Check this node
            auto* coll = node->collisionObject.get();
            if (coll) {
                auto* npColl = tryCastCollision(coll);
                if (npColl) {
                    spdlog::debug("GetCollisionObject: Found on node '{}'", node->name.c_str());
                    return npColl;
                }
            }
            
            // Recurse into children
            if (auto* asNode = node->IsNode()) {
                for (auto& child : asNode->children) {
                    if (child) {
                        auto* found = searchNode(child.get());
                        if (found) return found;
                    }
                }
            }
            return nullptr;
        };

        auto* found = searchNode(root);
        if (found) return found;

        spdlog::warn("GetCollisionObject: No bhkNPCollisionObject found on ref {:08X}", 
                      refr->formID);
        return nullptr;
    }
    
    // =========================================================================
    // DEFERRED HOLSTER REQUEST (v0.5.165)
    // =========================================================================

    void GrabManager::QueueHolsterRequest(RE::ObjectRefHandle refrHandle, std::uint32_t holsterIndex,
                                          const std::string& weaponName)
    {
        _pendingHolster.pending = true;
        _pendingHolster.framesRemaining = 2;  // Skip 2 frames to ensure EndGrab is fully done
        _pendingHolster.refrHandle = refrHandle;
        _pendingHolster.holsterIndex = holsterIndex;
        _pendingHolster.weaponName = weaponName;
        spdlog::debug("[HOLSTER-DEFERRED] Queued: weapon='{}' slot={} handle={:08X}",
                     weaponName, holsterIndex, refrHandle.native_handle());
    }

    void GrabManager::ProcessPendingHolster()
    {
        if (!_pendingHolster.pending)
            return;
        
        // Wait N frames before processing so EndGrab is fully complete
        if (_pendingHolster.framesRemaining > 0) {
            _pendingHolster.framesRemaining--;
            return;
        }
        
        // Clear pending flag immediately to prevent re-processing
        _pendingHolster.pending = false;
        
        spdlog::debug("[HOLSTER-DEFERRED] Processing deferred holster request: weapon='{}' slot={}",
                     _pendingHolster.weaponName, _pendingHolster.holsterIndex);
        spdlog::default_logger()->flush();
        
        // Resolve the handle to a live ref
        RE::TESObjectREFR* refr = nullptr;
        auto refrPtr = _pendingHolster.refrHandle.get();
        if (refrPtr) {
            refr = refrPtr.get();
        }
        
        if (!refr) {
            spdlog::warn("[HOLSTER-DEFERRED] Weapon ref handle resolved to null — weapon may have been consumed");
            // Try to pick up anyway by name? No ref means we can't. Fall through.
            return;
        }
        
        spdlog::debug("[HOLSTER-DEFERRED] Ref {:08X} resolved OK, checking 3D...", refr->formID);
        
        // Verify the ref still has 3D (it should — we deliberately didn't ActivateRef yet)
        auto* rootNode = refr->Get3D();
        if (!rootNode) {
            spdlog::warn("[HOLSTER-DEFERRED] Ref {:08X} has no 3D — falling back to normal pickup", refr->formID);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                heisenberg::Hooks::SetInternalActivation(true);
                refr->ActivateRef(player, nullptr, 1, false, false, false);
                heisenberg::Hooks::SetInternalActivation(false);
            }
            return;
        }

        // Get VH API
        auto* vhApi = VirtualHolsters::RequestVirtualHolstersAPI();
        if (!vhApi || !vhApi->IsInitialized()) {
            spdlog::warn("[HOLSTER-DEFERRED] VH API unavailable — falling back to normal pickup");
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                heisenberg::Hooks::SetInternalActivation(true);
                refr->ActivateRef(player, nullptr, 1, false, false, false);
                heisenberg::Hooks::SetInternalActivation(false);
            }
            return;
        }
        
        // Step 1: Call AddHolster WITH the live refr (has valid 3D for NIF cloning)
        // This is now safe because EndGrab has fully completed on the previous frame
        spdlog::debug("[HOLSTER-DEFERRED] Calling AddHolster slot={} name='{}' refr={:08X}...",
                     _pendingHolster.holsterIndex, _pendingHolster.weaponName,
                     refr->formID);
        spdlog::default_logger()->flush();

        bool added = vhApi->AddHolster(
            _pendingHolster.holsterIndex,
            static_cast<void*>(refr)
        );
        
        spdlog::debug("[HOLSTER-DEFERRED] AddHolster returned {}", added);
        spdlog::default_logger()->flush();
        
        // Step 2: Now pick up the weapon into inventory
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            spdlog::debug("[HOLSTER-DEFERRED] Calling ActivateRef to pick up weapon...");
            spdlog::default_logger()->flush();
            heisenberg::Hooks::SetInternalActivation(true);
            bool activated = refr->ActivateRef(player, nullptr, 1, false, false, false);
            heisenberg::Hooks::SetInternalActivation(false);
            spdlog::debug("[HOLSTER-DEFERRED] ActivateRef returned {}", activated);
            
            if (!activated) {
                spdlog::warn("[HOLSTER-DEFERRED] ActivateRef failed for {:08X}", refr->formID);
            }
        }
        
        if (!added) {
            spdlog::warn("[HOLSTER-DEFERRED] AddHolster failed — weapon picked up without holstering");
            if (g_config.showHolsterMessages)
                heisenberg::Hooks::ShowHUDMessageDirect("Holster failed");
        }
        
        spdlog::debug("[HOLSTER-DEFERRED] ✓ Deferred holster processing complete");
    }

    // =========================================================================
    // PLAYER-SPACE VELOCITY COMPENSATION
    // =========================================================================
}





