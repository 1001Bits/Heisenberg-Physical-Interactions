#include "Utils.h"
#include "F4VROffsets.h"
#include "f4vr/F4VRUtils.h"
#include <spdlog/spdlog.h>

namespace heisenberg::Utils
{
    static LARGE_INTEGER g_frequency;
    static LARGE_INTEGER g_startTime;
    static bool g_timerInitialized = false;

    double GetTime()
    {
        if (!g_timerInitialized) {
            QueryPerformanceFrequency(&g_frequency);
            QueryPerformanceCounter(&g_startTime);
            g_timerInitialized = true;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        return static_cast<double>(now.QuadPart - g_startTime.QuadPart) / 
               static_cast<double>(g_frequency.QuadPart);
    }

    // Check if player is in Power Armor
    // Delegates to F4VRCommonFramework's isInPowerArmor()
    bool IsPlayerInPowerArmor()
    {
        return f4cf::f4vr::isInPowerArmor();
    }
    
    // Track when player entered Power Armor for grace period
    static double g_powerArmorEntryTime = 0.0;
    static bool g_wasInPowerArmor = false;
    
    // Returns seconds since player entered Power Armor, or -1 if not in PA
    double GetSecondsSincePowerArmorEntry()
    {
        bool inPA = IsPlayerInPowerArmor();
        double now = GetTime();
        
        if (inPA && !g_wasInPowerArmor) {
            // Just entered Power Armor
            g_powerArmorEntryTime = now;
            g_wasInPowerArmor = true;
            spdlog::info("[PA] Player entered Power Armor");

            // Diagnostic: dump audio/menu state at PA entry to debug missing sound
            auto* ui = RE::UI::GetSingleton();
            uint32_t menuMode = ui ? ui->menuMode : 0xDEAD;
            static auto sfxAddr  = REL::Offset(0x5acd700).address();
            static auto auxAddr  = REL::Offset(0x5acd704).address();
            static auto gateAddr = REL::Offset(0x5acd780).address();
            uint32_t sfx  = *reinterpret_cast<uint32_t*>(sfxAddr);
            uint32_t aux  = *reinterpret_cast<uint32_t*>(auxAddr);
            uint32_t gate = *reinterpret_cast<uint32_t*>(gateAddr);
            spdlog::info("[PA-DIAG] menuMode={} sfxCounter={} auxCounter={} masterGate={}", menuMode, sfx, aux, gate);
        } else if (!inPA && g_wasInPowerArmor) {
            // Just exited Power Armor
            g_wasInPowerArmor = false;
            spdlog::info("[PA] Player exited Power Armor");
        }
        
        if (!inPA) {
            return -1.0;  // Not in Power Armor
        }
        
        return now - g_powerArmorEntryTime;
    }
    
    // Power Armor keyword form IDs (from Fallout4.esm)
    constexpr RE::TESFormID kKeyword_PowerArmor = 0x4D8A1;        // ArmorTypePower
    constexpr RE::TESFormID kKeyword_PowerArmorFrame = 0x15503F;  // PowerArmorFrame
    
    // Check if a form is a Power Armor piece (has PowerArmor or PowerArmorFrame keyword)
    bool IsPowerArmorPiece(RE::TESForm* form)
    {
        if (!form) return false;
        
        // Must be armor type
        if (form->GetFormType() != RE::ENUM_FORM_ID::kARMO) return false;
        
        // TESObjectARMO has BGSKeywordForm at offset 0x208
        // We need to access it manually since CommonLibF4 structure may vary
        constexpr size_t kKeywordFormOffset = 0x208;
        
        uint8_t* formPtr = reinterpret_cast<uint8_t*>(form);
        RE::BGSKeywordForm* keywordForm = reinterpret_cast<RE::BGSKeywordForm*>(formPtr + kKeywordFormOffset);
        
        // Check if keywordForm pointer looks valid (sanity check)
        if (!keywordForm) return false;
        
        // Iterate through keywords
        if (keywordForm->keywords && keywordForm->numKeywords > 0) {
            for (uint32_t i = 0; i < keywordForm->numKeywords; ++i) {
                RE::BGSKeyword* keyword = keywordForm->keywords[i];
                if (keyword) {
                    RE::TESFormID keywordId = keyword->GetFormID();
                    if (keywordId == kKeyword_PowerArmor || keywordId == kKeyword_PowerArmorFrame) {
                        return true;
                    }
                }
            }
        }
        
        return false;
    }

    // Check if a form is a Power Armor frame/chassis (kMISC with PowerArmorFrame keyword)
    bool IsPowerArmorFrame(RE::TESForm* form)
    {
        if (!form) return false;
        if (form->GetFormType() != RE::ENUM_FORM_ID::kMISC) return false;

        // TESObjectMISC has BGSKeywordForm at offset 0x128
        constexpr size_t kKeywordFormOffset = 0x128;

        uint8_t* formPtr = reinterpret_cast<uint8_t*>(form);
        RE::BGSKeywordForm* keywordForm = reinterpret_cast<RE::BGSKeywordForm*>(formPtr + kKeywordFormOffset);

        if (!keywordForm) return false;

        if (keywordForm->keywords && keywordForm->numKeywords > 0) {
            for (uint32_t i = 0; i < keywordForm->numKeywords; ++i) {
                RE::BGSKeyword* keyword = keywordForm->keywords[i];
                if (keyword && keyword->GetFormID() == kKeyword_PowerArmorFrame) {
                    return true;
                }
            }
        }
        return false;
    }

    RE::NiPoint3 GetForwardVector(const RE::NiMatrix3& rotation)
    {
        // Forward is the Y axis in Skyrim/Fallout coordinate system
        return RE::NiPoint3(rotation.entry[0][1], rotation.entry[1][1], rotation.entry[2][1]);
    }

    RE::NiPoint3 GetRightVector(const RE::NiMatrix3& rotation)
    {
        // Right is the X axis
        return RE::NiPoint3(rotation.entry[0][0], rotation.entry[1][0], rotation.entry[2][0]);
    }

    RE::NiPoint3 GetUpVector(const RE::NiMatrix3& rotation)
    {
        // Up is the Z axis
        return RE::NiPoint3(rotation.entry[0][2], rotation.entry[1][2], rotation.entry[2][2]);
    }

    float VectorLength(const RE::NiPoint3& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    RE::NiPoint3 VectorNormalize(const RE::NiPoint3& v)
    {
        float len = VectorLength(v);
        if (len > 0.0001f) {
            return RE::NiPoint3(v.x / len, v.y / len, v.z / len);
        }
        return RE::NiPoint3();
    }

    float VectorDot(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    RE::NiPoint3 VectorCross(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return RE::NiPoint3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        );
    }

    RE::NiPoint3 SteamVRToSkyrim(const RE::NiPoint3& steamvrPos)
    {
        // SteamVR: +y up, +x right, -z forward
        // Fallout4: +z up, +x right, +y forward
        // x <- x, y <- -z, z <- y
        return RE::NiPoint3(steamvrPos.x, -steamvrPos.z, steamvrPos.y);
    }

    RE::NiPoint3 SkyrimToSteamVR(const RE::NiPoint3& skyrimPos)
    {
        // Reverse of above
        // x <- x, y <- z, z <- -y
        return RE::NiPoint3(skyrimPos.x, skyrimPos.z, -skyrimPos.y);
    }

    RE::NiAVObject* FindNode(RE::NiAVObject* root, const char* name, int maxDepth)
    {
        if (!root || maxDepth <= 0) {
            return nullptr;
        }

        if (root->name.c_str() && strcmp(root->name.c_str(), name) == 0) {
            return root;
        }

        // Use IsNode() to get NiNode* if this is a node
        if (auto node = root->IsNode()) {
            for (auto& child : node->GetRuntimeData().children) {
                if (child) {
                    auto found = FindNode(child.get(), name, maxDepth - 1);
                    if (found) {
                        return found;
                    }
                }
            }
        }

        return nullptr;
    }

    RE::NiNode* GetPlayerRootNode()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }

        auto obj3d = player->Get3D();
        if (!obj3d) {
            return nullptr;
        }

        return obj3d->IsNode();
    }
    
    RE::NiNode* GetChildNode(const char* nodeName, RE::NiNode* parent)
    {
        if (!parent || !nodeName) {
            return nullptr;
        }
        
        // Search direct children only (not recursive)
        for (auto& child : parent->children) {
            if (child && child->name.c_str()) {
                if (strcmp(child->name.c_str(), nodeName) == 0) {
                    return child->IsNode();
                }
            }
        }
        
        return nullptr;
    }
    
    // =========================================================================
    // TRANSFORM UTILITIES (ported from Skyrim HIGGS)
    // =========================================================================
    
    RE::NiTransform InverseTransform(const RE::NiTransform& t)
    {
        RE::NiTransform inverse;
        
        // Inverse of rotation is transpose (for orthonormal matrices)
        inverse.rotate = t.rotate.Transpose();
        
        // Inverse scale
        float invScale = (t.scale != 0.0f) ? (1.0f / t.scale) : 1.0f;
        inverse.scale = invScale;
        
        // Inverse translation: -R^T * t / scale
        RE::NiPoint3 negTranslate = RE::NiPoint3(-t.translate.x, -t.translate.y, -t.translate.z);
        inverse.translate = inverse.rotate * negTranslate * invScale;
        
        return inverse;
    }
    
    RE::NiTransform GetLocalTransformForWorldTransform(RE::NiAVObject* node, const RE::NiTransform& worldTransform)
    {
        // Given desired world transform, calculate what local transform is needed
        // Based on FRIK's updateTransforms formula (inverted):
        //   world.translate = parent.translate + parent.rotate.T * (local.translate * parent.scale)
        //   world.rotate = local.rotate * parent.rotate
        //   world.scale = parent.scale * local.scale
        //
        // Inverting:
        //   local.translate = parent.rotate * (world.translate - parent.translate) / parent.scale
        //   local.rotate = world.rotate * parent.rotate.T
        //   local.scale = world.scale / parent.scale
        
        if (node->parent) {
            const auto& parentWorld = node->parent->world;
            
            RE::NiTransform local;
            
            // Local rotation = world.rotate * inverse(parent.rotate)
            // Since parent.rotate is orthonormal, inverse = transpose
            local.rotate = worldTransform.rotate * parentWorld.rotate.Transpose();
            
            // Local scale
            float invParentScale = (parentWorld.scale != 0.0f) ? (1.0f / parentWorld.scale) : 1.0f;
            local.scale = worldTransform.scale * invParentScale;
            
            // Local translation:
            // world.translate = parent.translate + parent.rotate.T * (local.translate * parent.scale)
            // Rearranging: local.translate = parent.rotate * (world.translate - parent.translate) / parent.scale
            RE::NiPoint3 worldOffset = worldTransform.translate - parentWorld.translate;
            local.translate = (parentWorld.rotate * worldOffset) * invParentScale;
            
            return local;
        }
        return worldTransform;
    }
    
    void UpdateNodeTransformLocal(RE::NiAVObject* node, const RE::NiTransform& worldTransform)
    {
        // Given world transform, set the necessary local transform
        node->local = GetLocalTransformForWorldTransform(node, worldTransform);
    }
    
    void UpdateKeyframedNode(RE::NiAVObject* node, const RE::NiTransform& transform)
    {
        if (!node) return;
        
        // =========================================================================
        // SAFE VISUAL UPDATE - Uses engine function for proper synchronization
        // =========================================================================
        // We set the LOCAL transform and let the engine compute WORLD from it.
        // This is safer than direct world writes because:
        // 1. Engine's NiAVObject_Update has internal synchronization with render
        // 2. Properly propagates to children
        // 3. No race condition with render thread reading world transform
        //
        // We skip collision-related updates that HIGGS Skyrim does because:
        // - Our objects are in KEYFRAMED mode with collision disabled
        // - Physics sync is handled separately via SetTransformLocked/ApplyHardKeyframeLocked
        // =========================================================================
        
        // Calculate local transform from desired world transform
        // SAFETY: Validate parent pointer is reasonable (not 0, not 1, etc.)
        // This prevents crashes from corrupted parent pointers during cell transitions
        RE::NiNode* parent = node->parent;
        uintptr_t parentAddr = reinterpret_cast<uintptr_t>(parent);
        bool parentValid = parent && (parentAddr > 0x10000);  // Must be > 64KB (kernel space)
        
        if (parentValid) {
            const auto& parentWorld = parent->world;
            float invParentScale = (parentWorld.scale != 0.0f) ? (1.0f / parentWorld.scale) : 1.0f;
            
            // local.rotate = world.rotate * inverse(parent.rotate)
            node->local.rotate = transform.rotate * parentWorld.rotate.Transpose();
            
            // local.scale = world.scale / parent.scale
            node->local.scale = transform.scale * invParentScale;
            
            // local.translate = inverse(parent.rotate) * (world.translate - parent.translate) / parent.scale
            RE::NiPoint3 worldOffset = transform.translate - parentWorld.translate;
            node->local.translate = (parentWorld.rotate * worldOffset) * invParentScale;
        } else if (!parent) {
            // No parent - local = world
            node->local = transform;
        } else {
            // Parent pointer is invalid (corrupted, sentinel value, etc.) - skip update
            // This prevents crashes during cell transitions or when objects are being deleted
            spdlog::warn("[UpdateKeyframedNode] Skipping update - invalid parent pointer {:X}",
                         reinterpret_cast<uintptr_t>(parent));
            return;
        }
        
        // Let engine compute world transforms and propagate to children
        // Using flags=0 for basic update (no velocity-based collision movement)
        // This is different from HIGGS's 0x2000 flag because:
        // - We handle physics separately via locked physics functions
        // - We just need visual propagation, not collision updates
        //
        // SAFETY: Wrap in SEH to catch any access violations from corrupted nodes
        __try {
            RE::NiUpdateData updateData;
            updateData.flags = 0;
            updateData.time = 0.0f;
            NiAVObject_Update(node, &updateData);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            spdlog::error("[UpdateKeyframedNode] Exception caught in NiAVObject_Update - node may be corrupted");
        }
        
        // NOTE: We skip these HIGGS Skyrim features because they don't apply to us:
        // - bhkBlendCollisionObject handling (we disable collision during grab)
        // - bhkRigidBody_setActivated (we handle physics separately)
        // - bhkRigidBodyT offset handling (handled by our SetTransformLocked)
        // - UpdateBoneMatrices (crashed, most clutter isn't skinned)
        // - ShadowSceneNode_UpdateNodeList (held objects don't need shadow updates)
    }

    // ========================================================================
    // PHYSICS BODY TRANSFORM OFFSET DETECTION
    // ========================================================================
    // In Skyrim, bhkRigidBodyT stores a local transform offset between the
    // scene node and the physics body. In F4VR with hknp, this offset may be
    // stored differently. This detects if there's a significant offset between
    // where the node is and where the physics body is.
    // ========================================================================
    
    bool HasPhysicsBodyOffset(RE::bhkNPCollisionObject* collisionObject, RE::NiAVObject* sceneNode)
    {
        if (!collisionObject || !sceneNode) return false;
        
        // Get the scene object from the collision object
        RE::NiAVObject* physicsSceneObj = collisionObject->sceneObject;
        
        // If the collision object's scene object differs from our scene node,
        // there may be a transform offset in the hierarchy
        if (physicsSceneObj && physicsSceneObj != sceneNode) {
            spdlog::debug("[OFFSET] Physics sceneObject ({}) != sceneNode ({})",
                         physicsSceneObj->name.c_str(), sceneNode->name.c_str());
            return true;
        }
        
        // Additionally, check if the physics body's position differs significantly
        // from the node's world position. This would indicate an internal offset.
        // TODO: Add actual hknpBody position check when we have working AccessBody
        
        return false;
    }
    
    RE::NiTransform GetPhysicsBodyOffset(RE::bhkNPCollisionObject* collisionObject, RE::NiAVObject* sceneNode)
    {
        RE::NiTransform offset;
        offset.translate = RE::NiPoint3(0, 0, 0);
        offset.rotate = RE::NiMatrix3();  // Identity
        offset.scale = 1.0f;
        
        if (!collisionObject || !sceneNode) return offset;
        
        // Get the scene object from the collision object
        RE::NiAVObject* physicsSceneObj = collisionObject->sceneObject;
        
        if (physicsSceneObj && physicsSceneObj != sceneNode) {
            // Calculate the relative transform between sceneNode and physicsSceneObj
            // offset = physicsSceneObj.world * inverse(sceneNode.world)
            RE::NiTransform inverseScene = InverseTransform(sceneNode->world);
            
            // Combine: offset = physicsSceneObj.world * inverseScene
            // This gives the offset that physics body has relative to scene node
            offset.rotate = physicsSceneObj->world.rotate * inverseScene.rotate;
            offset.translate = physicsSceneObj->world.translate - sceneNode->world.translate;
            // For translation, also account for rotation
            offset.translate = inverseScene.rotate * offset.translate;
            offset.scale = physicsSceneObj->world.scale * inverseScene.scale;
            
            spdlog::debug("[OFFSET] Detected offset: pos=({:.2f},{:.2f},{:.2f})",
                         offset.translate.x, offset.translate.y, offset.translate.z);
        }
        
        return offset;
    }
    
    RE::NiTransform ApplyPhysicsBodyOffset(const RE::NiTransform& worldTransform, const RE::NiTransform& bodyOffset)
    {
        // Apply the body offset to get the actual physics body target position
        // physicsTarget = worldTransform * bodyOffset
        RE::NiTransform result;
        result.rotate = worldTransform.rotate * bodyOffset.rotate;
        result.translate = worldTransform.translate + (worldTransform.rotate.Transpose() * bodyOffset.translate) * worldTransform.scale;
        result.scale = worldTransform.scale * bodyOffset.scale;
        return result;
    }
    
    // ========================================================================
    // FULL NODE UPDATE (PREPARED - NOT YET ACTIVE)
    // ========================================================================
    // This uses the engine's NiAVObject::Update function to properly propagate
    // transforms through the scene graph and sync physics. In Skyrim HIGGS,
    // the 0x2000 flag in NiUpdateData.flags enables velocity-based physics sync.
    //
    // F4VR's NiUpdateData has a 'flags' field at offset 0x10 (32-bit).
    // We need to find the equivalent velocity flag for F4VR.
    // ========================================================================
    
    void UpdateNodeFull(RE::NiAVObject* node, bool useVelocityFlag)
    {
        if (!node) return;
        
        // Create update context
        RE::NiUpdateData updateData;
        updateData.time = 0.0f;
        updateData.camera = nullptr;
        updateData.renderObjects = 0;
        updateData.fadeNodeDepth = 0;
        
        // === FLAGS ===
        // Skyrim uses 0x2000 for velocity-based physics movement.
        // F4VR's NiUpdateData.flags is at offset 0x10.
        // Testing needed to find if the same flag works or if it's different.
        //
        // For now, we'll use the same value and see what happens:
        if (useVelocityFlag) {
            updateData.flags = 0x2000;  // Velocity flag (may need adjustment for F4VR)
        } else {
            updateData.flags = 0;
        }
        
        // Call the engine function
        // NOTE: This is currently DISABLED in UpdateKeyframedNode
        // Enable by uncommenting the call in UpdateKeyframedNode
        heisenberg::NiAVObject_Update(node, &updateData);
        
        spdlog::trace("[UPDATE] Called NiAVObject_Update on {} with flags=0x{:X}",
                     node->name.c_str(), updateData.flags);
    }
    
    // =========================================================================
    // SKINNED GEOMETRY / BONE MATRIX UPDATES (Feature #3 from HIGGS)
    // =========================================================================
    // HIGGS Skyrim calls UpdateBoneMatrices() after moving grabbed objects to
    // ensure skinned geometry (armor, bodies) doesn't lag one frame behind.
    //
    // F4VR uses BSFlattenedBoneTree instead of NiSkinInstance for bone transforms.
    // We iterate over BSGeometry nodes and check for skinInstance (BSSkin::Instance).
    //
    // From HIGGS Skyrim (utils.cpp:261-278):
    //   void UpdateBoneMatrices(NiAVObject *obj) {
    //       if (BSGeometry *geom = obj->GetAsBSGeometry()) {
    //           NiSkinInstance *skinInstance = geom->m_spSkinInstance;
    //           if (skinInstance) {
    //               skinInstance->unk38 = -1; // Reset frameID so it updates
    //               NiSkinInstance_UpdateBoneMatrices(skinInstance, obj->m_worldTransform);
    //           }
    //       }
    //       // Recurse to children...
    //   }
    // =========================================================================
    
    bool HasSkinnedGeometry(RE::NiAVObject* obj)
    {
        if (!obj) return false;
        
        // Check if this is a BSGeometry with skin
        if (RE::BSGeometry* geom = obj->IsGeometry()) {
            if (geom->IsTriShape()) {
                auto* triShape = static_cast<RE::BSTriShape*>(geom);
                if (triShape->skinInstance && triShape->skinInstance.get()) {
                    return true;
                }
            }
        }
        
        // Recurse to children
        RE::NiNode* node = obj->IsNode();
        if (node) {
            auto& children = node->GetRuntimeData().children;
            for (uint32_t i = 0; i < children.size() && i < 100; ++i) {
                if (RE::NiAVObject* child = children[i].get()) {
                    if (HasSkinnedGeometry(child)) {
                        return true;
                    }
                }
            }
        }
        
        return false;
    }
    
    void UpdateBoneMatrices(RE::NiAVObject* obj)
    {
        if (!obj) return;
        
        // Check if this is a BSGeometry with skin instance
        if (RE::BSGeometry* geom = obj->IsGeometry()) {
            if (geom->IsTriShape()) {
                auto* triShape = static_cast<RE::BSTriShape*>(geom);
                if (triShape->skinInstance && triShape->skinInstance.get()) {
                    // F4VR uses BSSkin::Instance which has a pointer to BSFlattenedBoneTree.
                    // The skinInstance handles bone-to-world transforms for skinned meshes.
                    //
                    // In Skyrim HIGGS, they set skinInstance->unk38 = -1 to force update.
                    // F4VR's BSSkin::Instance structure is different, but we can try
                    // calling UpdateModelBound to refresh the skinned geometry.
                    //
                    // For now, log when we encounter skinned geometry.
                    // Full implementation would need to:
                    // 1. Access the BSSkin::Instance
                    // 2. Find the BSFlattenedBoneTree
                    // 3. Call BSFlattenedBoneTree::UpdateBoneArray
                    
                    // NOTE: Most grabbed clutter objects are NOT skinned.
                    // This mainly affects grabbed actors (ragdolls) and some special objects.
                    // For basic clutter grabbing, this can be a no-op.
                    
                    static int skinLogCounter = 0;
                    if (++skinLogCounter % 60 == 0) {  // Log once per second at 60fps
                        spdlog::debug("[SKIN] Found skinned geometry on '{}' - bone matrices may need update",
                                     obj->name.c_str());
                    }
                    
                    // Try to update the model bound at minimum
                    // This helps with culling/visibility after moving skinned objects
                    try {
                        void* skinPtr = triShape->skinInstance.get();
                        if (skinPtr) {
                            // BSSkinInstance_UpdateModelBound expects (skinInstance, &bound)
                            // But we don't have direct access to the bound here.
                            // The BSGeometry has modelBound at offset 0x120.
                            heisenberg::BSSkinInstance_UpdateModelBound(skinPtr, &geom->modelBound);
                        }
                    } catch (...) {
                        // Silently ignore errors - this is optional functionality
                    }
                }
            }
        }
        
        // Recurse to children
        RE::NiNode* node = obj->IsNode();
        if (node) {
            auto& children = node->GetRuntimeData().children;
            for (uint32_t i = 0; i < children.size() && i < 100; ++i) {
                if (RE::NiAVObject* child = children[i].get()) {
                    UpdateBoneMatrices(child);
                }
            }
        }
    }
}
