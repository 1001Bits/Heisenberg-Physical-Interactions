#pragma once

#include "RE/Fallout.h"

#include <cmath>

namespace heisenberg
{

    // ────────────────────────────────────────────────────────────────────────
    // Re-orthonormalize a rotation matrix (Gram-Schmidt on rows).
    //
    // This codebase inverts rotations with Transpose() in a dozen places, which is only
    // correct for an ORTHONORMAL matrix. When a matrix has drifted, transpose is no longer
    // the inverse and the forward/inverse pair no longer cancels: the object is multiplied
    // by M^T*M = diag(|r0|^2, |r1|^2, |r2|^2). A short row squashes that axis and a long row
    // stretches it - which renders as a mesh that is flattened or several times too large,
    // NOT as a bad scale value (NiTransform::scale is a single uniform float and physically
    // cannot flatten one axis).
    //
    // Live report that produced this: a held Nuka-Cola bottle flattened on grab, later grew
    // ~3x mid-hold, and returned to normal on release - the release "fix" being Havok
    // re-driving the node from a clean quaternion once the body goes dynamic again.
    //
    // No-op (to float precision) on a matrix that is already clean, so it is safe to apply
    // defensively on the hot path.
    inline void OrthoNormalize(RE::NiMatrix3& m)
    {
        auto row = [&m](int r) {
            return RE::NiPoint3{ m.entry[r][0], m.entry[r][1], m.entry[r][2] };
        };
        auto setRow = [&m](int r, const RE::NiPoint3& v) {
            m.entry[r][0] = v.x; m.entry[r][1] = v.y; m.entry[r][2] = v.z;
        };
        auto dot = [](const RE::NiPoint3& a, const RE::NiPoint3& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        auto cross = [](const RE::NiPoint3& a, const RE::NiPoint3& b) {
            return RE::NiPoint3{ a.y * b.z - a.z * b.y,
                                 a.z * b.x - a.x * b.z,
                                 a.x * b.y - a.y * b.x };
        };

        RE::NiPoint3 r0 = row(0);
        float len0 = std::sqrt(dot(r0, r0));
        if (!(len0 > 1e-6f)) return;            // degenerate: leave it alone rather than invent one
        r0 = { r0.x / len0, r0.y / len0, r0.z / len0 };

        RE::NiPoint3 r1 = row(1);
        const float proj = dot(r1, r0);
        r1 = { r1.x - proj * r0.x, r1.y - proj * r0.y, r1.z - proj * r0.z };
        float len1 = std::sqrt(dot(r1, r1));
        if (!(len1 > 1e-6f)) return;
        r1 = { r1.x / len1, r1.y / len1, r1.z / len1 };

        const RE::NiPoint3 r2 = cross(r0, r1);  // completes a right-handed orthonormal basis

        setRow(0, r0); setRow(1, r1); setRow(2, r2);
    }

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
