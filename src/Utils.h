#pragma once

namespace heisenberg
{
    /**
     * Utility functions for Heisenberg.
     */
    namespace Utils
    {
        // Time utilities
        double GetTime();

        // Player state utilities
        bool IsPlayerInPowerArmor();
        double GetSecondsSincePowerArmorEntry();  // Returns seconds since PA entry, or -1 if not in PA
        
        // Power Armor piece detection - checks if an armor has PowerArmor or PowerArmorFrame keywords
        // Used to prevent grabbing equipped PA pieces which causes the armor to fall off
        bool IsPowerArmorPiece(RE::TESForm* form);

        // Power Armor frame detection - checks if a MISC item is a PA chassis (has PowerArmorFrame keyword)
        bool IsPowerArmorFrame(RE::TESForm* form);

        // Math utilities
        RE::NiPoint3 GetForwardVector(const RE::NiMatrix3& rotation);
        RE::NiPoint3 GetRightVector(const RE::NiMatrix3& rotation);
        RE::NiPoint3 GetUpVector(const RE::NiMatrix3& rotation);

        float VectorLength(const RE::NiPoint3& v);
        RE::NiPoint3 VectorNormalize(const RE::NiPoint3& v);
        float VectorDot(const RE::NiPoint3& a, const RE::NiPoint3& b);
        RE::NiPoint3 VectorCross(const RE::NiPoint3& a, const RE::NiPoint3& b);

        // Coordinate system conversion
        // SteamVR: +y up, +x right, -z forward
        // Fallout4: +z up, +x right, +y forward
        RE::NiPoint3 SteamVRToSkyrim(const RE::NiPoint3& steamvrPos);
        RE::NiPoint3 SkyrimToSteamVR(const RE::NiPoint3& skyrimPos);

        // Node utilities
        RE::NiAVObject* FindNode(RE::NiAVObject* root, const char* name, int maxDepth = 100);
        RE::NiNode* GetPlayerRootNode();
        RE::NiNode* GetChildNode(const char* nodeName, RE::NiNode* parent);
        
        // Transform utilities (like Skyrim HIGGS)
        RE::NiTransform InverseTransform(const RE::NiTransform& t);
        RE::NiTransform GetLocalTransformForWorldTransform(RE::NiAVObject* node, const RE::NiTransform& worldTransform);
        void UpdateNodeTransformLocal(RE::NiAVObject* node, const RE::NiTransform& worldTransform);
        void UpdateKeyframedNode(RE::NiAVObject* node, const RE::NiTransform& transform);
        
        // Physics body transform offset detection and handling
        // Some collision bodies have a local offset from their scene node (like bhkRigidBodyT in Skyrim)
        // This detects if there's an offset and returns it
        bool HasPhysicsBodyOffset(RE::bhkNPCollisionObject* collisionObject, RE::NiAVObject* sceneNode);
        RE::NiTransform GetPhysicsBodyOffset(RE::bhkNPCollisionObject* collisionObject, RE::NiAVObject* sceneNode);
        RE::NiTransform ApplyPhysicsBodyOffset(const RE::NiTransform& worldTransform, const RE::NiTransform& bodyOffset);
        
        // Full node update using engine function (like HIGGS's NiAVObject_UpdateNode)
        // This properly propagates transforms to children and syncs physics
        // Set useVelocityFlag=true to use velocity-based physics movement (0x2000 flag equivalent)
        void UpdateNodeFull(RE::NiAVObject* node, bool useVelocityFlag = false);
        
        // =========================================================================
        // SKINNED GEOMETRY UPDATES (Feature #3 from HIGGS comparison)
        // =========================================================================
        // HIGGS calls UpdateBoneMatrices() after UpdateKeyframedNode to ensure
        // skinned geometry (armor, creature bodies) doesn't lag one frame behind.
        // F4VR uses BSFlattenedBoneTree instead of NiSkinInstance for bone transforms.
        // =========================================================================
        
        /**
         * Update bone matrices for skinned geometry on a node and its children.
         * Call after moving a grabbed object to prevent visual lag on skinned meshes.
         * Recursively processes BSGeometry nodes looking for BSSkin::Instance.
         * 
         * @param obj The node tree to update (typically the grabbed object root)
         */
        void UpdateBoneMatrices(RE::NiAVObject* obj);
        
        /**
         * Check if a node has any skinned geometry (BSGeometry with skinInstance).
         * Useful for logging and debug.
         * 
         * @param obj The node to check
         * @return true if the node or its children have skinned geometry
         */
        bool HasSkinnedGeometry(RE::NiAVObject* obj);
    }
}
