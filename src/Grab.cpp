#include "Grab.h"
#include "Config.h"
#include "rock/RockBridge.h"
#include "../external/ROCK/src/ROCKMain.h"  // rock::HostNotifyExternalGrab — embedded-engine grab-lifecycle seam (audit rank 2)
#include "../external/ROCK/src/physics-interaction/grab/TouchGrabBridge.h"
#include "../external/ROCK/src/physics-interaction/native/HavokRuntime.h"
#include "../external/ROCK/src/physics-interaction/TransformMath.h"
#include "DropToHand.h"
#include "DualWieldAPI.h"
#include "F4VROffsets.h"
#include "FingerCurves.h"
#include "FRIKInterface.h"
#include "GrabConstraint.h"
#include "GrabPosePolicy.h"
#include "HandCollision.h"
#include "Hand.h"
#include "Heisenberg.h"
#include "Hooks.h"
#include "ItemOffsets.h"
#include "ItemPositionConfigMode.h"
#include "MenuChecker.h"
#include "Physics.h"
#include "ThrownObjectTracker.h"
#include "PipboyInteraction.h"
#include "Utils.h"
#include "VirtualHolstersAPI.h"
#include "VRInput.h"
#include "SharedUtils.h"
#include "WandNodeHelper.h"
#include "HeisenbergInterface001.h"
#include "HandAuthority.h"
#include "f4vr/PlayerNodes.h"
#include "f4vr/F4VRUtils.h"
#include "RE/Bethesda/UI.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
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

namespace heisenberg
{
    static RE::bhkNPCollisionObject*
    TryResolveNpcCollisionObjectFromRaw(
        RE::NiCollisionObject* rawCollision);
}

namespace
{
    // Havok world scale - 1 game unit = 0.0142875 Havok units (1/70)
    constexpr float HAVOK_WORLD_SCALE = 0.0142875f;

    void StoreCalculatedFingerCurls(heisenberg::GrabState& state,
                                    float thumb,
                                    float index,
                                    float middle,
                                    float ring,
                                    float pinky)
    {
        state.SetRuntimeFingerCurls(
            std::clamp(thumb, 0.0f, 1.0f),
            std::clamp(index, 0.0f, 1.0f),
            std::clamp(middle, 0.0f, 1.0f),
            std::clamp(ring, 0.0f, 1.0f),
            std::clamp(pinky, 0.0f, 1.0f));
    }

    bool ApplyRuntimeFingerCurls(const heisenberg::GrabState& state, bool isLeft)
    {
        if (!state.hasRuntimeFingerCurls) {
            return false;
        }

        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        if (!frik.IsAvailable()) {
            return false;
        }

        // FRIK's controller tracking overrides a one-shot SetHandPoseFingerPositions
        // within a frame of it being applied, so we route grab poses through the
        // FingerAnimator — it lerps to the target in Closing state and then keeps
        // re-sending the pose every frame during Holding to block controller
        // override. (See FingerAnimator.cpp line 62-68.)
        float joints[heisenberg::FingerAnimator::NUM_JOINTS];
        if (state.hasRuntimeJointCurls) {
            std::copy(
                state.runtimeJointCurls.begin(),
                state.runtimeJointCurls.end(),
                joints);
        } else {
            heisenberg::ExpandFingerToJointValues(
                state.runtimeThumbCurl,
                state.runtimeIndexCurl,
                state.runtimeMiddleCurl,
                state.runtimeRingCurl,
                state.runtimePinkyCurl,
                joints);
        }

        auto& animator = heisenberg::Heisenberg::GetSingleton().GetFingerAnimator(isLeft);
        // Prime _current so we don't lerp from the last RestoreOpen's 1.0 — start
        // from the current controller pose the player feels, then close to target.
        animator.SetTargetPose(joints, heisenberg::g_config.fingerAnimCloseSpeed);

        // Also push once synchronously so the very first frame sees the new pose
        // (the animator would only kick in on the next Heisenberg::Update tick).
        if (state.hasRuntimeJointCurls) {
            frik.SetHandPoseJointPositions(
                isLeft,
                state.runtimeJointCurls.data());
        } else {
            frik.SetHandPoseFingerPositions(
                isLeft,
                state.runtimeThumbCurl,
                state.runtimeIndexCurl,
                state.runtimeMiddleCurl,
                state.runtimeRingCurl,
                state.runtimePinkyCurl);
        }
        heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, state.runtimeThumbCurl);
        return true;
    }

    bool HasStoredFingerCurls(const heisenberg::GrabState& state)
    {
        return state.itemOffset.hasJointCurls || state.itemOffset.hasFingerCurls;
    }

    bool ApplyStoredFingerCurls(const heisenberg::GrabState& state, bool isLeft)
    {
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        if (!frik.IsAvailable()) {
            return false;
        }

        auto& animator = heisenberg::Heisenberg::GetSingleton().GetFingerAnimator(isLeft);

        if (state.itemOffset.hasJointCurls) {
            // Route through animator so Holding state keeps re-sending the pose
            // each frame (prevents FRIK controller-tracking override).
            animator.SetTargetPose(state.itemOffset.jointCurls, heisenberg::g_config.fingerAnimCloseSpeed);
            bool applied = frik.SetHandPoseJointPositions(isLeft, state.itemOffset.jointCurls);
            if (applied) {
                heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, state.itemOffset.thumbCurl);
            }
            return applied;
        }

        if (state.itemOffset.hasFingerCurls) {
            float joints[heisenberg::FingerAnimator::NUM_JOINTS];
            heisenberg::ExpandFingerToJointValues(
                state.itemOffset.thumbCurl,
                state.itemOffset.indexCurl,
                state.itemOffset.middleCurl,
                state.itemOffset.ringCurl,
                state.itemOffset.pinkyCurl,
                joints);
            animator.SetTargetPose(joints, heisenberg::g_config.fingerAnimCloseSpeed);

            bool applied = frik.SetHandPoseFingerPositions(
                isLeft,
                state.itemOffset.thumbCurl,
                state.itemOffset.indexCurl,
                state.itemOffset.middleCurl,
                state.itemOffset.ringCurl,
                state.itemOffset.pinkyCurl);
            if (applied) {
                heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, state.itemOffset.thumbCurl);
            }
            return applied;
        }

        return false;
    }

    bool UseAutomaticFingerCurls()
    {
        return heisenberg::g_config.enableAutomaticFingerCurls;
    }

    bool UsesAuthoredGripPose(const heisenberg::GrabState& state)
    {
        return heisenberg::grab_pose_policy::UsesAuthoredPose(
            state.hasItemOffset,
            state.naturalFingerPosing,
            HasStoredFingerCurls(state));
    }

    bool HasConfiguredFingerCurls(const heisenberg::GrabState& state)
    {
        return HasStoredFingerCurls(state) || state.hasRuntimeFingerCurls;
    }

    bool ApplyConfiguredFingerCurls(const heisenberg::GrabState& state, bool isLeft)
    {
        // A saved transform and its curls are one coherent authored pose. The
        // jumpsuit audit demonstrated that replacing only the curls with a live
        // solve produces a different grip on every pull. Natural/touch grabs do
        // not use the saved transform, so they remain geometry-driven.
        if (UsesAuthoredGripPose(state) &&
            ApplyStoredFingerCurls(state, isLeft)) {
            return true;
        }
        return state.hasRuntimeFingerCurls && ApplyRuntimeFingerCurls(state, isLeft);
    }

    // Forward decl — GetSkinnedHandNode is defined further down but needed by
    // the calibrated-palm helper below.
    static RE::NiNode* GetSkinnedHandNode(bool isLeft);

    // Palm in world space, derived from the calibrated finger geometry on the
    // SKINNED hand (COM-tree LArm_Hand / RArm_Hand). This fixes the frame
    // mismatch where placement ran in the wand frame but fingers cast from
    // the bone frame — with FRIK IK offsetting the two, the grabbed object
    // landed at the wand's palm while fingers reached for the bone's palm,
    // leaving them visibly apart.
    //
    // Returns false if calibration hasn't captured an open-hand frame yet or
    // the skinned hand isn't reachable — callers fall back to the wand-based
    // palm in that case (pre-calibration behavior).
    bool GetCalibratedPalmWorld(bool isLeft, RE::NiPoint3& outPos, RE::NiPoint3& outDir)
    {
        RE::NiPoint3 palmLocal, dirLocal;
        if (!heisenberg::GetCalibratedPalmLocal(isLeft, palmLocal, dirLocal)) {
            return false;
        }
        RE::NiNode* skinned = GetSkinnedHandNode(isLeft);
        if (!skinned) {
            return false;
        }

        // Shift palm from knuckle centroid into palm cup. The centroid sits on
        // the DORSAL side (back of hand) — if we place objects there, they
        // hover above the hand instead of landing on the fingers. Palmar
        // direction is derived from finger curl geometry at calibration time.
        const float depth = heisenberg::g_config.palmDepthOffset;
        if (depth != 0.0f) {
            RE::NiPoint3 palmarLocal;
            if (heisenberg::GetCalibratedPalmarLocal(isLeft, palmarLocal)) {
                palmLocal = palmLocal + palmarLocal * depth;
            }
        }

        const auto& xform = skinned->world;
        outPos = xform.rotate * (palmLocal * xform.scale) + xform.translate;
        outDir = heisenberg::VectorNormalized(xform.rotate * dirLocal);
        return true;
    }

    // HIGGS-style palm direction: hand-local palmVector transformed to world space
    // (hand.cpp:2274-2278). The left hand mirrors X to match the skeleton, matching
    // HIGGS's main.cpp:956-958 pattern. The previous implementation returned the raw
    // Y-axis column of the wand's world rotation, which in F4VR does not correspond
    // to "out of palm" — that mismatch caused palm-ray placement to miss every time.
    RE::NiPoint3 GetPalmDirection(RE::NiAVObject* handNode, bool isLeft)
    {
        // HIGGS applies palmVector to the Skyrim hand-bone frame
        // (main.cpp:956-1017 + hand.h:237 — handNodeName="NPC R Hand [RHnd]").
        // Applying the same constants to the F4VR wand frame puts palmDir in
        // the wrong place: the wand's local axes are the controller device
        // frame, not the Bethesda hand rig. Prefer the calibrated skinned-hand
        // palm (FingerCurves derives palmDir from the actual finger geometry),
        // and only fall back to wand-frame config constants if calibration
        // hasn't captured an open-hand frame yet.
        RE::NiPoint3 palmPosW, palmDirW;
        if (GetCalibratedPalmWorld(isLeft, palmPosW, palmDirW)) {
            return palmDirW;
        }

        if (!handNode) {
            return RE::NiPoint3(0.0f, 1.0f, 0.0f);
        }

        RE::NiPoint3 palmVectorLocal(
            heisenberg::g_config.palmVectorX,
            heisenberg::g_config.palmVectorY,
            heisenberg::g_config.palmVectorZ);
        if (isLeft) palmVectorLocal.x *= -1.0f;

        return heisenberg::VectorNormalized(handNode->world.rotate * palmVectorLocal);
    }

    // HIGGS-style palm position. See GetPalmDirection for the frame rationale:
    // prefer the calibrated skinned-hand palm (which lives in the same frame
    // as the finger-curl cast), fall back to wand-frame config constants when
    // calibration isn't available yet.
    RE::NiPoint3 GetPalmPosition(RE::NiAVObject* handNode, bool isLeft)
    {
        RE::NiPoint3 palmPosW, palmDirW;
        if (GetCalibratedPalmWorld(isLeft, palmPosW, palmDirW)) {
            return palmPosW;
        }

        if (!handNode) {
            return RE::NiPoint3(0.0f, 0.0f, 0.0f);
        }

        RE::NiPoint3 palmPosLocal(
            heisenberg::g_config.palmPositionX,
            heisenberg::g_config.palmPositionY,
            heisenberg::g_config.palmPositionZ);
        if (isLeft) palmPosLocal.x *= -1.0f;

        const auto& xform = handNode->world;
        return xform.rotate * (palmPosLocal * xform.scale) + xform.translate;
    }

    RE::NiMatrix3 MakeIdentityMatrix()
    {
        RE::NiMatrix3 rotation;
        rotation.MakeIdentity();
        return rotation;
    }

    RE::NiMatrix3 MakeAxisAngleRotation(const RE::NiPoint3& axis, float angle)
    {
        RE::NiMatrix3 rotation = MakeIdentityMatrix();

        float cosAngle = std::cos(angle);
        float sinAngle = std::sin(angle);
        float oneMinusCos = 1.0f - cosAngle;

        rotation.entry[0][0] = cosAngle + axis.x * axis.x * oneMinusCos;
        rotation.entry[0][1] = axis.x * axis.y * oneMinusCos - axis.z * sinAngle;
        rotation.entry[0][2] = axis.x * axis.z * oneMinusCos + axis.y * sinAngle;

        rotation.entry[1][0] = axis.y * axis.x * oneMinusCos + axis.z * sinAngle;
        rotation.entry[1][1] = cosAngle + axis.y * axis.y * oneMinusCos;
        rotation.entry[1][2] = axis.y * axis.z * oneMinusCos - axis.x * sinAngle;

        rotation.entry[2][0] = axis.z * axis.x * oneMinusCos - axis.y * sinAngle;
        rotation.entry[2][1] = axis.z * axis.y * oneMinusCos + axis.x * sinAngle;
        rotation.entry[2][2] = cosAngle + axis.z * axis.z * oneMinusCos;

        return rotation;
    }

    RE::NiMatrix3 MakeVectorAlignmentRotation(const RE::NiPoint3& fromVector, const RE::NiPoint3& toVector)
    {
        RE::NiPoint3 from = heisenberg::VectorNormalized(fromVector);
        RE::NiPoint3 to = heisenberg::VectorNormalized(toVector);

        float dot = (std::clamp)(heisenberg::DotProduct(from, to), -1.0f, 1.0f);
        if (dot > 0.9999f) {
            return MakeIdentityMatrix();
        }

        if (dot < -0.9999f) {
            constexpr float kPi = 3.14159265358979323846f;
            RE::NiPoint3 arbitraryAxis = std::abs(from.z) < 0.9f ? RE::NiPoint3(0.0f, 0.0f, 1.0f) : RE::NiPoint3(0.0f, 1.0f, 0.0f);
            RE::NiPoint3 axis = heisenberg::VectorNormalized(heisenberg::CrossProduct(from, arbitraryAxis));
            return MakeAxisAngleRotation(axis, kPi);
        }

        RE::NiPoint3 axis = heisenberg::VectorNormalized(heisenberg::CrossProduct(from, to));
        float angle = std::acos(dot);
        return MakeAxisAngleRotation(axis, angle);
    }

    bool TryCalculateRuntimeHandPlacementFromGeometry(heisenberg::GrabState& state, RE::NiAVObject* wandNode, bool isLeft)
    {
        state.ClearRuntimeHandPlacement();

        if (!state.node || !wandNode) {
            spdlog::debug("[GEOM-PLACE] Failed: node={:p} wand={:p}", (void*)state.node.get(), (void*)wandNode);
            return false;
        }

        // HIGGS's handNode is Skyrim's weapon/controller node — a wand-equivalent
        // with a deterministic OpenVR-derived orientation, not a skinned bone.
        // The palmVector and g_finger* constants were calibrated against that
        // frame, so on F4VR we anchor them to wandNode (not FRIK's LArm_Hand,
        // whose rotation is pose-dependent because IK adjusts wrist bend per
        // arm pose — measured corrRot(hand->wand) varied widely grab-to-grab).
        RE::NiNode* handNode = wandNode->IsNode();
        if (!handNode) {
            spdlog::warn("[GEOM-PLACE] wandNode for {} hand is not an NiNode", isLeft ? "left" : "right");
            return false;
        }

        std::vector<heisenberg::TriangleData> triangles;
        triangles.reserve(256);
        heisenberg::GetTriangles(state.node.get(), triangles);
        if (triangles.empty()) {
            spdlog::info("[GEOM-PLACE] FALLBACK: no triangles for '{}' - will use stored item offset", state.node->name.c_str());
            return false;
        }
        spdlog::info("[GEOM-PLACE] Extracted {} triangles for '{}'", triangles.size(), state.node->name.c_str());

        // HIGGS-style palm: hardcoded offset in wand-local frame
        // (Config palmPosition/palmVector, transformed via wandNode->world).
        // This matches HIGGS's `palmPosHandspace = {0, -2.4, 6}` — a known
        // point on the palm skin surface, NOT the knuckle centroid. Using
        // the calibrated knuckle centroid placed objects INSIDE the hand
        // flesh; the wand-frame hardcoded palm is on the skin, so surface-
        // snap lands the closest mesh point on the skin as intended.
        RE::NiPoint3 calibratedPalmPos;
        RE::NiPoint3 calibratedPalmDir;
        const bool usingCalibratedPalm = GetCalibratedPalmWorld(isLeft, calibratedPalmPos, calibratedPalmDir);
        RE::NiPoint3 palmPos = usingCalibratedPalm ? calibratedPalmPos : GetPalmPosition(handNode, isLeft);
        RE::NiPoint3 palmDir = usingCalibratedPalm ? calibratedPalmDir : GetPalmDirection(handNode, isLeft);
        spdlog::info("[GEOM-PLACE] palm source: {}",
                     usingCalibratedPalm ? "SKINNED-HAND (calibrated)" : "WAND (config fallback)");
        // Palm-frame diagnostic. Dumps both anchor transforms (wand + skinned
        // hand bone) and both palm-direction candidates (computed = what we
        // use; wand-config = what the old code would have given). If the two
        // don't align, placement differs visibly from finger-cast geometry.
        {
            const auto& wr = handNode->world.rotate;
            spdlog::warn("[WAND-DIAG] wand name='{}' pos=({:.1f},{:.1f},{:.1f})",
                         handNode->name.c_str(),
                         handNode->world.translate.x, handNode->world.translate.y, handNode->world.translate.z);
            spdlog::warn("[WAND-DIAG] wand rot row0=({:.3f},{:.3f},{:.3f}) row1=({:.3f},{:.3f},{:.3f}) row2=({:.3f},{:.3f},{:.3f})",
                         wr.entry[0][0], wr.entry[0][1], wr.entry[0][2],
                         wr.entry[1][0], wr.entry[1][1], wr.entry[1][2],
                         wr.entry[2][0], wr.entry[2][1], wr.entry[2][2]);
            // wr * (1,0,0) = col0; wr * (0,1,0) = col1; wr * (0,0,1) = col2.
            // NiMatrix3 * NiPoint3 is row-dot (column-vector convention), so
            // `wr * axisLocal` gives world-space direction the local axis points.
            spdlog::warn("[WAND-DIAG] wand axes +X=({:.3f},{:.3f},{:.3f}) +Y=({:.3f},{:.3f},{:.3f}) +Z=({:.3f},{:.3f},{:.3f})",
                         wr.entry[0][0], wr.entry[1][0], wr.entry[2][0],
                         wr.entry[0][1], wr.entry[1][1], wr.entry[2][1],
                         wr.entry[0][2], wr.entry[1][2], wr.entry[2][2]);

            // Skinned hand bone (LArm_Hand / RArm_Hand under COM) — what
            // HIGGS calls "NPC R Hand [RHnd]" in Skyrim. Its axes are what
            // palmPosition / palmVector were originally tuned against.
            if (RE::NiNode* skinned = GetSkinnedHandNode(isLeft)) {
                const auto& sr = skinned->world.rotate;
                spdlog::warn("[WAND-DIAG] skin name='{}' pos=({:.1f},{:.1f},{:.1f})",
                             skinned->name.c_str(),
                             skinned->world.translate.x, skinned->world.translate.y, skinned->world.translate.z);
                spdlog::warn("[WAND-DIAG] skin axes +X=({:.3f},{:.3f},{:.3f}) +Y=({:.3f},{:.3f},{:.3f}) +Z=({:.3f},{:.3f},{:.3f})",
                             sr.entry[0][0], sr.entry[1][0], sr.entry[2][0],
                             sr.entry[0][1], sr.entry[1][1], sr.entry[2][1],
                             sr.entry[0][2], sr.entry[1][2], sr.entry[2][2]);
            } else {
                spdlog::warn("[WAND-DIAG] skin: UNAVAILABLE (GetSkinnedHandNode returned null)");
            }

            // What the wand-config path would produce (bug-(b) witness).
            RE::NiPoint3 palmVecLocal(
                heisenberg::g_config.palmVectorX,
                heisenberg::g_config.palmVectorY,
                heisenberg::g_config.palmVectorZ);
            if (isLeft) palmVecLocal.x *= -1.0f;
            const RE::NiPoint3 wandDir = heisenberg::VectorNormalized(wr * palmVecLocal);

            // Direction from palm anchor to object center — ground truth of
            // where palm *should* be pointed.
            RE::NiPoint3 palmToObj = state.node->world.translate - palmPos;
            float len = std::sqrt(palmToObj.x*palmToObj.x + palmToObj.y*palmToObj.y + palmToObj.z*palmToObj.z);
            if (len > 1e-3f) palmToObj = RE::NiPoint3(palmToObj.x/len, palmToObj.y/len, palmToObj.z/len);
            spdlog::warn("[WAND-DIAG] palmPos=({:.1f},{:.1f},{:.1f}) palmDir_used=({:.3f},{:.3f},{:.3f}) palmDir_wandCfg=({:.3f},{:.3f},{:.3f}) palm->objDir=({:.3f},{:.3f},{:.3f})",
                         palmPos.x, palmPos.y, palmPos.z,
                         palmDir.x, palmDir.y, palmDir.z,
                         wandDir.x, wandDir.y, wandDir.z,
                         palmToObj.x, palmToObj.y, palmToObj.z);
        }

        // HIGGS "shouldMoveHandBack" pre-shift (hand.cpp:1373-1382 +
        // 1407-1413). Shift the object's starting transform by
        // palmDir * pulledGrabHandAdjustDistance and apply the same
        // translation to every triangle BEFORE running closest-point. The
        // surface-snap then operates against the shifted geometry. Skipped
        // when the config value is ~0 so testing can toggle it without
        // recompiling.
        const float preShiftDist = heisenberg::g_config.pulledGrabHandAdjustDistance;
        const RE::NiPoint3 preShiftVec = (std::abs(preShiftDist) > 1e-4f)
            ? palmDir * preShiftDist
            : RE::NiPoint3(0.0f, 0.0f, 0.0f);
        if (std::abs(preShiftDist) > 1e-4f) {
            for (auto& tri : triangles) {
                tri.v0 += preShiftVec;
                tri.v1 += preShiftVec;
                tri.v2 += preShiftVec;
            }
            spdlog::warn("[GEOM-PLACE] PRE-SHIFT '{}': dist={:.2f} vec=({:+.2f},{:+.2f},{:+.2f})",
                         state.node->name.c_str(), preShiftDist,
                         preShiftVec.x, preShiftVec.y, preShiftVec.z);
        }

        // HIGGS closest-point-on-graphics-geometry-to-line (hand.cpp:1432).
        // Single pass: for every triangle, find the closest point on that
        // triangle to the palm RAY (position + direction). Degrades gracefully
        // as aim error grows, and on-axis aim gives the same result as a
        // ray hit. HIGGS's shouldMoveHandBack pre-shift is applied above.
        RE::NiPoint3 closestPoint;
        RE::NiPoint3 closestNormal;
        int closestTriIndex = -1;
        float distToRay = 0.0f;
        bool found = heisenberg::GetClosestPointOnGeometryToLine(
            triangles, palmPos, palmDir,
            closestPoint, closestNormal, closestTriIndex, distToRay);

        if (!found) {
            spdlog::info("[GEOM-PLACE] FALLBACK: no closest-point hit for '{}' - will use stored item offset", state.node->name.c_str());
            return false;
        }
        const float closestDistSq = heisenberg::VectorLengthSquared(closestPoint - palmPos);
        float closestDistance = std::sqrt(closestDistSq);
        spdlog::info("[GEOM-PLACE] Nearest point: ({:.1f},{:.1f},{:.1f}), normal=({:.3f},{:.3f},{:.3f}), dist={:.1f}, tri={}",
                     closestPoint.x, closestPoint.y, closestPoint.z,
                     closestNormal.x, closestNormal.y, closestNormal.z,
                     closestDistance, closestTriIndex);

        // HIGGS surface-snap with pre-shifted transform (hand.cpp:1545-1546):
        //   desiredNodeTransform = adjustedTransform;
        //   desiredNodeTransform.pos += palmPos - ptPos;
        // adjustedTransform.pos = originalTransform.pos + preShiftVec. For
        // same-triangle selection the shift cancels; it only changes the
        // result if the shift moves a different triangle into the closest
        // position, which can happen on elongated objects.
        RE::NiTransform desiredWorldTransform = state.node->world;
        desiredWorldTransform.translate =
            state.node->world.translate + preShiftVec + (palmPos - closestPoint);
        if (heisenberg::g_config.enableAxialPlacement &&
            std::abs(heisenberg::g_config.axialPlacementClearance) > 1.0e-4f) {
            desiredWorldTransform.translate += palmDir * heisenberg::g_config.axialPlacementClearance;
        }
        spdlog::warn("[GEOM-PLACE] SURFACE-SNAP '{}': closestDist={:.2f}",
                     state.node->name.c_str(), std::sqrt(closestDistSq));

        RE::NiNode* storageParent = usingCalibratedPalm ? GetSkinnedHandNode(isLeft) : nullptr;
        bool usingSkinnedStorage = storageParent != nullptr;
        if (!storageParent) {
            storageParent = handNode;  // wand fallback
            usingSkinnedStorage = false;
        }

        RE::NiPoint3 worldOffset = desiredWorldTransform.translate - storageParent->world.translate;
        // F4VR row-vector convention (see FRIK updateTransforms / MEMORY.md project_f4vr_matrix_convention):
        //   world.rotate   = local.rotate * parent.rotate
        //   world.translate= parent.translate + parent.rotate.Transpose() * local.translate
        // Inverse store: local.translate = parent.rotate * (world.translate - parent.translate)
        //                local.rotate    = world.rotate * parent.rotate.Transpose()
        RE::NiPoint3 localOffset = storageParent->world.rotate * worldOffset;
        RE::NiMatrix3 localRotation = desiredWorldTransform.rotate * storageParent->world.rotate.Transpose();

        spdlog::warn("[GEOM-PLACE] RESULT '{}': parent={} worldOffset=({:.2f},{:.2f},{:.2f}) localOffset=({:.2f},{:.2f},{:.2f})",
                    state.node->name.c_str(),
                    usingSkinnedStorage ? "SKINNED" : "WAND",
                    worldOffset.x, worldOffset.y, worldOffset.z,
                    localOffset.x, localOffset.y, localOffset.z);

        // Also log what the integrated/saved item offset would have been for comparison.
        // Lookup is independent of whether we actually apply the offset — geometry-calc
        // testing leaves customOffset=nullopt, but we still want the baseline for diffing.
        {
            std::optional<heisenberg::ItemOffset> baseline;
            if (auto* refr = state.GetRefr()) {
                baseline = heisenberg::ItemOffsetManager::GetSingleton().GetOffsetWithFallback(refr, isLeft);
            }
            if (baseline.has_value()) {
                const auto& b = baseline.value();
                spdlog::warn("[GEOM-PLACE] COMPARE baseline '{}': pos=({:.2f},{:.2f},{:.2f}) curls=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f}) matched='{}'",
                            state.node->name.c_str(),
                            b.position.x, b.position.y, b.position.z,
                            b.thumbCurl, b.indexCurl, b.middleCurl, b.ringCurl, b.pinkyCurl,
                            b.matchedName);
                spdlog::warn("[GEOM-PLACE] DIFF calc-baseline '{}': d=({:+.2f},{:+.2f},{:+.2f})",
                            state.node->name.c_str(),
                            localOffset.x - b.position.x,
                            localOffset.y - b.position.y,
                            localOffset.z - b.position.z);
            } else {
                spdlog::warn("[GEOM-PLACE] No baseline offset for '{}' (nothing to compare to)",
                            state.node->name.c_str());
            }
        }

        state.SetRuntimeHandPlacement(localOffset, localRotation, usingSkinnedStorage);
        return true;
    }

    // Direct-children recursion — Utils::FindNode walks GetRuntimeData().children
    // which on F4VR sometimes returns null for nodes that direct iteration does
    // find. Used only by GetSkinnedHandNode below.
    static RE::NiAVObject* FindNodeDirectChildren(RE::NiAVObject* root, const char* name, int maxDepth)
    {
        if (!root || maxDepth <= 0) return nullptr;
        if (root->name.c_str() && std::strcmp(root->name.c_str(), name) == 0) return root;
        RE::NiNode* asNode = root->IsNode();
        if (!asNode) return nullptr;
        for (std::uint32_t i = 0; i < asNode->children.size(); ++i) {
            auto& ch = asNode->children[i];
            if (!ch) continue;
            if (auto found = FindNodeDirectChildren(ch.get(), name, maxDepth - 1)) return found;
        }
        return nullptr;
    }

    // Multiple LArm_Hand / RArm_Hand nodes exist in the F4VR skeleton — the
    // first-person skeleton's LArm_Hand is a weapon-attach wrapper with only
    // WeaponLeft children; the *real* skinned hand (with LArm_Finger11..53
    // children) lives under the 3rd-person rootNode beneath the COM node.
    // FRIK uses this same split — see Skeleton.cpp:139-141 calling
    // findAVObject(getCommonNode(), "LArm_Hand"). Mirror that here.
    //
    // We probe LArm_Finger11 first and walk up to its parent so we get the
    // hand bone that actually owns the fingers, then fall back to a direct
    // hand lookup if Finger11 isn't present.
    static RE::NiNode* GetSkinnedHandNode(bool isLeft)
    {
        const char* fingerProbe = isLeft ? "LArm_Finger11" : "RArm_Finger11";
        const char* handName    = isLeft ? "LArm_Hand"     : "RArm_Hand";

        // Primary: 3rd-person common node ("COM" under unkF0->rootNode).
        if (RE::NiNode* commonNode = f4cf::f4vr::getCommonNode()) {
            if (RE::NiAVObject* finger = FindNodeDirectChildren(commonNode, fingerProbe, 20)) {
                if (finger->parent) return finger->parent;
            }
            if (RE::NiAVObject* handObj = FindNodeDirectChildren(commonNode, handName, 20)) {
                if (auto* asNode = handObj->IsNode()) return asNode;
            }
        }

        // Fallback: firstPersonSkeleton (in case skeleton layout differs in PA).
        auto* player = f4cf::f4vr::getPlayer();
        if (player && player->firstPerson3D.get()) {
            if (RE::NiAVObject* finger = FindNodeDirectChildren(player->firstPerson3D.get(), fingerProbe, 20)) {
                if (finger->parent) return finger->parent;
            }
            if (RE::NiAVObject* handObj = FindNodeDirectChildren(player->firstPerson3D.get(), handName, 20)) {
                if (auto* asNode = handObj->IsNode()) return asNode;
            }
        }
        return nullptr;
    }

    bool TryCalculateRuntimeFingerCurlsFromGeometry(heisenberg::GrabState& state, RE::NiNode* wandNode, bool isLeft)
    {
        if (!state.node || !wandNode) {
            return false;
        }

        // Use the same live root-flattened FRIK skeleton and generated
        // calibration that ROCK uses for physical hand contact. The old HIGGS
        // curve-table path mixed a fixed 0.85 hand scale with already-scaled
        // live bones and then shifted every finger toward one shared mesh
        // point, which is why results changed across VR scales and thin meshes.
        RE::NiNode* handNode = GetSkinnedHandNode(isLeft);
        if (!handNode) handNode = wandNode;

        RE::NiPoint3 palmPos = GetPalmPosition(handNode, isLeft);
        RE::NiPoint3 calibratedPalm{};
        RE::NiPoint3 calibratedDirection{};
        if (GetCalibratedPalmWorld(isLeft, calibratedPalm, calibratedDirection)) {
            palmPos = calibratedPalm;
        }

        std::array<float, 15> jointCurls{};
        if (!rock::touch_grab_bridge::SolveTouchGrabFingerPose(
                state.node.get(),
                handNode->world,
                isLeft,
                palmPos,
                palmPos,
                jointCurls)) {
            return false;
        }

        state.SetRuntimeJointCurls(jointCurls);
        return true;
    }

    bool ResolvePendingFingerCurls(heisenberg::GrabState& state, RE::NiNode* wandNode, bool isLeft, const char* context)
    {
        if (!state.pendingFingerCurls || state.isPulling) {
            return false;
        }

        // If the saved transform is driving placement, its curls were authored
        // against that exact transform and must arrive as the other half of the
        // same pose. This is the path the final, correct jumpsuit grab used.
        if (UsesAuthoredGripPose(state)) {
            state.ClearRuntimeFingerCurls();
            if (ApplyStoredFingerCurls(state, isLeft)) {
                state.pendingFingerCurls = false;
                spdlog::info(
                    "[GRAB-POSE] {} applied paired authored offset curls",
                    context);
                return true;
            }
        }

        // A touch grab is solved against the object's real, already-final world
        // pose before ownership changes. Reuse that exact 15-joint result; a
        // second solve after the keyframed body handoff only introduces a
        // different pose and visible jitter.
        if (state.hasRuntimeJointCurls &&
            ApplyRuntimeFingerCurls(state, isLeft))
        {
            state.pendingFingerCurls = false;
            spdlog::debug(
                "[GRAB-FINGERS] {} applied stable first-contact joint pose",
                context);
            return true;
        }

        // Natural/touch and generated placements are arbitrary relative to the
        // hand, so solve their curls from the final visible mesh.
        if (TryCalculateRuntimeFingerCurlsFromGeometry(state, wandNode, isLeft) &&
            ApplyRuntimeFingerCurls(state, isLeft))
        {
            state.pendingFingerCurls = false;
            spdlog::debug(
                "[GRAB-FINGERS] {} applied mesh-safe geometry curls: thumb={:.2f} index={:.2f} mid={:.2f} ring={:.2f} pinky={:.2f}",
                context,
                state.runtimeThumbCurl,
                state.runtimeIndexCurl,
                state.runtimeMiddleCurl,
                state.runtimeRingCurl,
                state.runtimePinkyCurl);
            return true;
        }

        // Fail open if geometry is unavailable or has no trustworthy
        // first-surface intersection. Grip input must never make a missing
        // mesh trace turn into a fist through the item.
        StoreCalculatedFingerCurls(state, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        if (frik.IsAvailable()) {
            ApplyRuntimeFingerCurls(state, isLeft);
        }
        state.pendingFingerCurls = false;
        spdlog::debug("[GRAB-FINGERS] {} geometry had no safe stop - fail-open pose applied", context);
        return true;
    }

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
        if (!player || !player->firstPerson3D.get()) {
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
        RE::NiAVObject* handNodeObj = heisenberg::Utils::FindNode(player->firstPerson3D.get(), handNodeName, 15);
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
        
        RE::NiAVObject* weaponObj = heisenberg::Utils::FindNode(player->firstPerson3D.get(), "Weapon", 15);
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

    // Resolve the parent transform a stored localOffset/localRotation should
    // be applied against. Priority:
    //   1. hasRigidRenderedHandPlacement → COM-tree rendered hand. This is
    //      the final, acquisition-time rebase that prevents a raw wand from
    //      leading the visible FRIK hand while the player turns.
    //   2. runtimePlacementSkinnedHand → COM-tree LArm_Hand/RArm_Hand (the
    //      same node the fingers live under, so object and fingers share a
    //      parent and can't drift apart when FRIK IK moves the bone).
    //   3. isFRIKOffset → firstPersonSkeleton hand node (legacy saved
    //      offsets were calibrated against that).
    //   4. default (wand) — passed in as fallback.
    void ResolveGrabParent(const heisenberg::GrabState& state, bool isLeft,
                           const RE::NiPoint3& defaultPos, const RE::NiMatrix3& defaultRot,
                           RE::NiPoint3& outPos, RE::NiMatrix3& outRot)
    {
        outPos = defaultPos;
        outRot = defaultRot;

        if (state.hasRigidRenderedHandPlacement) {
            if (RE::NiNode* skinned = GetSkinnedHandNode(isLeft)) {
                outPos = skinned->world.translate;
                outRot = skinned->world.rotate;
                return;
            }
        }

        if (state.runtimePlacementSkinnedHand) {
            if (RE::NiNode* skinned = GetSkinnedHandNode(isLeft)) {
                outPos = skinned->world.translate;
                outRot = skinned->world.rotate;
                return;
            }
        }

        if (state.isFRIKOffset) {
            RE::NiPoint3 frikPos;
            RE::NiMatrix3 frikRot;
            if (GetFRIKWeaponParentTransform(isLeft, frikPos, frikRot)) {
                outPos = frikPos;
                outRot = frikRot;
            }
        }
    }

    // Convert the final placement selected by the normal offset/touch/geometry
    // rules into the rendered hand's frame without changing its world pose.
    // Thereafter both the item and the visible fingers consume the same FRIK
    // transform each frame, so body rotation cannot make the raw wand lead the
    // hand and create the familiar "item moves first, hand catches up" lag.
    bool RebaseGrabPlacementToRenderedHand(
        heisenberg::GrabState& state,
        bool isLeft,
        const RE::NiPoint3& defaultPos,
        const RE::NiMatrix3& defaultRot)
    {
        RE::NiNode* renderedHand = GetSkinnedHandNode(isLeft);
        if (!renderedHand) {
            return false;
        }

        RE::NiPoint3 sourceParentPos;
        RE::NiMatrix3 sourceParentRot;
        ResolveGrabParent(
            state,
            isLeft,
            defaultPos,
            defaultRot,
            sourceParentPos,
            sourceParentRot);

        RE::NiPoint3 sourceLocalPos;
        RE::NiMatrix3 sourceLocalRot;
        heisenberg::GetEffectiveGrabPlacement(
            state,
            sourceLocalPos,
            sourceLocalRot);

        // PA compensation remains a per-frame tuning offset. Include it while
        // converting the world pose, then remove the same numeric local value
        // from the stored base so the normal update path can add it once.
        RE::NiPoint3 paLocal{};
        if (heisenberg::Utils::IsPlayerInPowerArmor()) {
            paLocal.x = heisenberg::g_config.paGrabOffsetX;
            paLocal.y = heisenberg::g_config.paGrabOffsetY;
            paLocal.z = heisenberg::g_config.paGrabOffsetZ;
        }

        const RE::NiPoint3 desiredWorldPos =
            sourceParentPos +
            sourceParentRot.Transpose() *
                (sourceLocalPos + paLocal);
        const RE::NiMatrix3 desiredWorldRot =
            sourceLocalRot * sourceParentRot;

        RE::NiPoint3 renderedLocalPos =
            renderedHand->world.rotate *
            (desiredWorldPos -
             renderedHand->world.translate);
        renderedLocalPos -= paLocal;
        const RE::NiMatrix3 renderedLocalRot =
            desiredWorldRot *
            renderedHand->world.rotate.Transpose();

        state.SetRigidRenderedHandPlacement(
            renderedLocalPos,
            renderedLocalRot);
        state.grabOffsetLocal = renderedLocalPos;

        spdlog::info(
            "[GRAB-SYNC] {} '{}' placement rebased to rendered hand "
            "without world-pose change local=({:.2f},{:.2f},{:.2f})",
            isLeft ? "L" : "R",
            state.node ? state.node->name.c_str() : "NULL",
            renderedLocalPos.x,
            renderedLocalPos.y,
            renderedLocalPos.z);
        return true;
    }

    // =====================================================================
    // HELPER: Get grab mode from config
    // =====================================================================
    int GetEffectiveGrabMode()
    {
        int configuredGrabMode = heisenberg::g_config.grabMode;

        // Full Dynamic (iGrabMode=9): the embedded ROCK engine owns grab+selection.
        // Only honored while the engine is actually hosted; else degrade to Keyframed so
        // selecting 9 without bUseRockEngineArchitecture=1 never disables grabbing.
        if (configuredGrabMode == static_cast<int>(heisenberg::GrabMode::FullDynamic)) {
            return heisenberg::IsRockEngineHosted()
                       ? configuredGrabMode
                       : static_cast<int>(heisenberg::GrabMode::Keyframed);
        }

        // Two-scenario cleanup: every other historical mode (1-8) was removed.
        // Anything that isn't Full Dynamic degrades to the keyframed backend.
        return static_cast<int>(heisenberg::GrabMode::Keyframed);
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
    
    // SEH-only leaf helpers — engine velocity setters dereference the
    // underlying havok body, which IsCollisionObjectValid (wrapper-only)
    // can't verify. If the body is freed between the wrapper check and
    // the engine call (e.g. cell unload, post-grab teardown race) we
    // catch the AV here instead of crashing the game. Must be leaf
    // functions: __try and C++ object unwinding can't coexist in MSVC.
    bool SetLinearVelocitySeh(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel)
    {
        __try {
            SetLinearVelocity(obj, vel);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SetAngularVelocitySeh(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel)
    {
        __try {
            SetAngularVelocity(obj, vel);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool GetLinearVelocitySeh(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel)
    {
        __try {
            GetLinearVelocity(obj, vel);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Locked version of SetLinearVelocity
    void SetLinearVelocityLocked(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!SetLinearVelocitySeh(obj, vel)) {
            spdlog::warn("[GRAB-PHYSICS] SetLinearVelocity AV (body freed under wrapper) obj={:X}",
                         reinterpret_cast<uintptr_t>(obj));
        }
    }

    // Locked version of GetLinearVelocity
    void GetLinearVelocityLocked(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldReadLock lock(bhkWorld);
        if (!GetLinearVelocitySeh(obj, vel)) {
            spdlog::warn("[GRAB-PHYSICS] GetLinearVelocity AV (body freed under wrapper) obj={:X}",
                         reinterpret_cast<uintptr_t>(obj));
        }
    }

    // Locked version of SetAngularVelocity
    void SetAngularVelocityLocked(RE::bhkNPCollisionObject* obj, RE::NiPoint4& vel, RE::bhkWorld* bhkWorld)
    {
        if (!IsCollisionObjectValid(obj) || !bhkWorld) return;
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!SetAngularVelocitySeh(obj, vel)) {
            spdlog::warn("[GRAB-PHYSICS] SetAngularVelocity AV (body freed under wrapper) obj={:X}",
                         reinterpret_cast<uintptr_t>(obj));
        }
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

    static void RebuildBodyCollisionCachesNative(
        void* hknpWorld,
        std::uint32_t bodyId);

    static bool CapturedHeldBodyStillOwned(
        const heisenberg::GrabState& state,
        RE::hknpWorld* world,
        const heisenberg::GrabState::CapturedHeldBody& captured)
    {
        if (!state.collisionObject ||
            !world ||
            !captured.ownerCollisionObject ||
            !captured.ownerNode) {
            return false;
        }
        auto* body =
            rock::havok_runtime::getBody(
                world,
                RE::hknpBodyId{ captured.bodyId });
        if (!body) {
            return false;
        }
        if (rock::havok_runtime::getCollisionObjectFromBody(body) !=
            captured.ownerCollisionObject) {
            return false;
        }
        for (auto* current = captured.ownerNode.get();
             current;
             current = current->parent) {
            if (current == state.node.get()) {
                return true;
            }
        }
        return false;
    }

    // Build a descendant's transform from the scene graph's authored local
    // links instead of subtracting its current world transform from the root.
    //
    // This distinction matters for multipart clutter. Havok is allowed to
    // write the simulated pose of a child collision owner (for example
    // 10mmAmmo/AmmoMultiple) into ownerNode->world before the parent root is
    // updated. Treating that transient world pose as a permanent root-local
    // offset feeds the separation back into the next grab/release.
    static bool TryBuildDescendantRootLocal(
        const RE::NiAVObject* root,
        const RE::NiAVObject* descendant,
        RE::NiTransform& outRootLocal)
    {
        if (!root || !descendant) {
            return false;
        }
        if (root == descendant) {
            outRootLocal =
                rock::transform_math::makeIdentityTransform<
                    RE::NiTransform>();
            return true;
        }

        constexpr std::size_t kMaxHierarchyDepth = 64;
        std::array<const RE::NiAVObject*, kMaxHierarchyDepth> path{};
        std::size_t depth = 0;
        auto* current = descendant;
        while (current && current != root) {
            if (depth >= path.size()) {
                return false;
            }
            path[depth++] = current;
            current = current->parent;
        }
        if (current != root) {
            return false;
        }

        auto result =
            rock::transform_math::makeIdentityTransform<RE::NiTransform>();
        while (depth > 0) {
            result = rock::transform_math::composeTransforms(
                result,
                path[--depth]->local);
        }
        outRootLocal = result;
        return std::isfinite(outRootLocal.translate.x) &&
               std::isfinite(outRootLocal.translate.y) &&
               std::isfinite(outRootLocal.translate.z) &&
               std::isfinite(outRootLocal.scale);
    }

    static bool UsesSelectedCollisionWrapperMotionScope(
        const heisenberg::GrabState& state)
    {
        const auto* refr = state.GetRefr();
        return refr &&
               refr->GetFormType() != RE::ENUM_FORM_ID::kACHR &&
               state.collisionObject &&
               state.capturedHeldBodyCount == 0 &&
               state.isProxyCollision;
    }

    // Capture every body in the selected wrapper's shared physics system whose
    // owner belongs to this reference subtree. Mutually exclusive mesh variants
    // still share the same reference root; leaving a hidden alternate dynamic
    // lets it overwrite that root after the visible body was keyframed. We keep
    // the selected visible wrapper as placement authority, but drive all shared
    // bodies through authored root->owner frames so they cannot split.
    // Ragdoll/actor proxies remain outside this rigid clutter path.
    static bool CaptureHeldCollisionBodyFrames(heisenberg::GrabState& state)
    {
        state.capturedHeldBodies = {};
        state.capturedHeldBodyCount = 0;
        state.capturedHeldBodiesExcludeAlternateWrappers = false;
        state.capturedHeldBodiesIncludeAlternateWrappers = false;
        if (!state.collisionObject || !state.node) {
            return false;
        }
        const auto* refr = state.GetRefr();
        const bool actorProxy =
            refr &&
            refr->GetFormType() == RE::ENUM_FORM_ID::kACHR;
        if (actorProxy) {
            return false;
        }

        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world) {
            return false;
        }

        struct CaptureContext
        {
            heisenberg::GrabState* state = nullptr;
            RE::hknpWorld* world = nullptr;
            RE::NiTransform rootInverse{};
            std::uint32_t eligibleBodyCount = 0;
            std::uint32_t alternateWrapperBodyCount = 0;
            bool eligibleCaptureFailed = false;
        } context{
            &state,
            world,
            rock::transform_math::invertTransform(state.node->world),
        };

        auto visitor = [](std::uint32_t bodyId, void* rawContext) {
            auto* context = static_cast<CaptureContext*>(rawContext);
            if (!context || !context->state || !context->world) {
                return false;
            }
            auto& state = *context->state;
            auto* body = rock::havok_runtime::getBody(
                context->world,
                RE::hknpBodyId{ bodyId });
            if (!body) {
                context->eligibleCaptureFailed = true;
                return false;
            }

            auto* ownerCollision =
                rock::havok_runtime::getCollisionObjectFromBody(body);
            auto* resolvedOwnerCollision =
                heisenberg::TryResolveNpcCollisionObjectFromRaw(
                    ownerCollision);
            if (resolvedOwnerCollision != state.collisionObject) {
                ++context->alternateWrapperBodyCount;
            }

            ++context->eligibleBodyCount;
            if (state.capturedHeldBodyCount >=
                heisenberg::GrabState::kMaxCapturedHeldBodies) {
                context->eligibleCaptureFailed = true;
                return false;
            }

            RE::NiTransform bodyWorld{};
            if (!rock::havok_runtime::tryGetBodyWorldTransform(
                    context->world,
                    RE::hknpBodyId{ bodyId },
                    bodyWorld)) {
                context->eligibleCaptureFailed = true;
                return false;
            }

            auto* ownerNode =
                rock::havok_runtime::getOwnerNodeFromCollisionObject(
                    ownerCollision);
            bool ownerInsideReference = false;
            for (auto* current = ownerNode;
                 current;
                 current = current->parent) {
                if (current == state.node.get()) {
                    ownerInsideReference = true;
                    break;
                }
            }
            if (!ownerCollision || !ownerNode ||
                !ownerInsideReference) {
                context->eligibleCaptureFailed = true;
                return false;
            }

            std::uint32_t filterInfo = 0;
            if (!heisenberg::Physics::TryReadBodyFilterInfo(
                    context->world,
                    bodyId,
                    filterInfo)) {
                context->eligibleCaptureFailed = true;
                return false;
            }

            auto& captured =
                state.capturedHeldBodies[state.capturedHeldBodyCount++];
            captured.bodyId = bodyId;
            captured.motionId = body->motionIndex;
            captured.originalFilterInfo = filterInfo;
            captured.ownerCollisionObject = ownerCollision;
            captured.ownerNode.reset(ownerNode);
            // PHANTOM-BODY FIX (Jul 25): remember whether this body belongs to the
            // wrapper that won grab acquisition (the VISIBLE mesh variant). Bodies of
            // alternate/hidden wrappers get dragged along invisibly and must not be
            // allowed to knock over world objects — see ApplyCapturedHeldBodyFilters.
            captured.isActiveWrapper = (resolvedOwnerCollision == state.collisionObject);
            const RE::NiTransform runtimeOwnerRootLocal =
                rock::transform_math::composeTransforms(
                    context->rootInverse,
                    ownerNode->world);
            if (!TryBuildDescendantRootLocal(
                    state.node.get(),
                    ownerNode,
                    captured.ownerRootLocal)) {
                // The ancestry was already validated above, so this is only a
                // defensive fallback for an implausibly deep/corrupt tree.
                captured.ownerRootLocal = runtimeOwnerRootLocal;
            } else {
                const float frameMismatch =
                    (runtimeOwnerRootLocal.translate -
                     captured.ownerRootLocal.translate)
                        .Length();
                if (frameMismatch > 0.5f) {
                    spdlog::warn(
                        "[GRAB-MULTIBODY] Detected collision-owner/root split "
                        "for body {} owner='{}': runtimeRootLocal="
                        "({:.2f},{:.2f},{:.2f}) authoredLocalChain="
                        "({:.2f},{:.2f},{:.2f}) mismatch={:.2f}gu",
                        bodyId,
                        ownerNode->name.c_str(),
                        runtimeOwnerRootLocal.translate.x,
                        runtimeOwnerRootLocal.translate.y,
                        runtimeOwnerRootLocal.translate.z,
                        captured.ownerRootLocal.translate.x,
                        captured.ownerRootLocal.translate.y,
                        captured.ownerRootLocal.translate.z,
                        frameMismatch);
                }
            }
            captured.bodyOwnerLocal =
                rock::transform_math::composeTransforms(
                    rock::transform_math::invertTransform(
                        ownerNode->world),
                    bodyWorld);
            captured.valid = true;
            captured.filterChanged = false;
            return true;
        };

        constexpr int kMaxPhysicsSystemBodiesToInspect = 64;
        const auto scan =
            rock::havok_runtime::forEachPhysicsSystemBodyIdDetailed(
                static_cast<RE::NiCollisionObject*>(state.collisionObject),
                world,
                kMaxPhysicsSystemBodiesToInspect,
                visitor,
                &context);
        if (!scan.enumerated() ||
            context.eligibleCaptureFailed ||
            context.eligibleBodyCount >
                heisenberg::GrabState::kMaxCapturedHeldBodies ||
            state.capturedHeldBodyCount !=
                context.eligibleBodyCount ||
            state.capturedHeldBodyCount == 0) {
            spdlog::warn(
                "[GRAB-MULTIBODY] Active-wrapper capture skipped: status={} "
                "systemBodies={} eligible={} captured={} alternates={} cap={}",
                static_cast<std::uint32_t>(scan.status),
                scan.bodyCount,
                context.eligibleBodyCount,
                state.capturedHeldBodyCount,
                context.alternateWrapperBodyCount,
                heisenberg::GrabState::kMaxCapturedHeldBodies);
            state.capturedHeldBodies = {};
            state.capturedHeldBodyCount = 0;
            state.capturedHeldBodiesExcludeAlternateWrappers = false;
            state.capturedHeldBodiesIncludeAlternateWrappers = false;
            return false;
        }

        state.capturedHeldBodiesExcludeAlternateWrappers = false;
        state.capturedHeldBodiesIncludeAlternateWrappers =
            context.alternateWrapperBodyCount > 0;
        if (state.capturedHeldBodyCount > 0) {
            state.heldOriginalFilterInfo =
                state.capturedHeldBodies[0].originalFilterInfo;
            state.heldHadSuppressionBitOriginally =
                (state.heldOriginalFilterInfo & 0x000B0000u) ==
                0x000B0000u;
        }
        spdlog::info(
            "[GRAB-MULTIBODY] Captured {} shared-reference body frame(s) "
            "for {:08X} root='{}' (systemBodies={} alternateWrappers={})",
            state.capturedHeldBodyCount,
            state.GetRefr() ? state.GetRefr()->formID : 0,
            state.node->name.c_str(),
            scan.bodyCount,
            context.alternateWrapperBodyCount);
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& body = state.capturedHeldBodies[i];
            spdlog::info(
                "[GRAB-MULTIBODY] body[{}]={} motion={} filter=0x{:08X} "
                "owner='{}' ownerRootLocal=({:.2f},{:.2f},{:.2f}) "
                "bodyOwnerLocal=({:.2f},{:.2f},{:.2f})",
                i,
                body.bodyId,
                body.motionId,
                body.originalFilterInfo,
                body.ownerNode
                    ? body.ownerNode->name.c_str()
                    : "NULL",
                body.ownerRootLocal.translate.x,
                body.ownerRootLocal.translate.y,
                body.ownerRootLocal.translate.z,
                body.bodyOwnerLocal.translate.x,
                body.bodyOwnerLocal.translate.y,
                body.bodyOwnerLocal.translate.z);
        }
        return state.capturedHeldBodyCount > 0;
    }

    static bool ApplyCapturedHeldBodyFilters(heisenberg::GrabState& state)
    {
        if (!state.collisionObject ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world) {
            return false;
        }

        constexpr std::uint32_t kHandGroupBits = 0x000Bu << 16;
        bool applied = false;
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            auto& captured = state.capturedHeldBodies[i];
            if (!captured.valid ||
                !CapturedHeldBodyStillOwned(
                    state,
                    world,
                    captured)) {
                continue;
            }
            std::uint32_t current = 0;
            if (!heisenberg::Physics::TryReadBodyFilterInfo(
                    world,
                    captured.bodyId,
                    current)) {
                continue;
            }
            // PHANTOM-BODY FIX (Jul 25, user-reported knockover): bodies of alternate/
            // hidden wrappers (the invisible second Havok body of dual-body NIFs like
            // the ammo box) are teleport-dragged along the visible mesh every frame at
            // keyframed authority — on their native clutter layer they batted world
            // objects over invisibly. Park them on the non-collidable layer (15, the
            // same constant the held-object non-collidable path uses) for the capture
            // lifetime; the raw filter write preserves group/system bits, dodging the
            // known SetLayerLocked preset-clobber bug. The VISIBLE wrapper's body keeps
            // the intended bHeldObjectCollidable behavior. Restore is byte-exact from
            // originalFilterInfo on release.
            constexpr std::uint32_t kNonCollidableLayer = 15u;
            const std::uint32_t desired = captured.isActiveWrapper
                ? (current | kHandGroupBits)
                : ((current & ~0x7Fu) | kNonCollidableLayer | kHandGroupBits);
            if (desired != current &&
                !heisenberg::Physics::TryWriteBodyFilterInfo(
                    world,
                    captured.bodyId,
                    desired)) {
                continue;
            }
            // filterChanged must reflect the FULL divergence from the original filter
            // (the old hand-bits-only test under-reported once the layer changes too),
            // so RestoreCapturedHeldBodyFilters knows a byte-exact restore is needed.
            captured.filterChanged = (desired != captured.originalFilterInfo);
            if (desired != current && !captured.isActiveWrapper) {
                // Layer transition on a live body: rebuild its pair caches so existing
                // contact pairs drop immediately (standing rule: every raw Havok filter
                // write needs a pair-cache rebuild).
                RebuildBodyCollisionCachesNative(world, captured.bodyId);
            }
            applied = true;
        }
        return applied;
    }

    static bool RestoreCapturedHeldBodyFilters(heisenberg::GrabState& state)
    {
        if (!state.collisionObject ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world) {
            return false;
        }
        bool restoredAny = false;
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            auto& captured = state.capturedHeldBodies[i];
            if (!captured.valid ||
                !captured.filterChanged ||
                !CapturedHeldBodyStillOwned(
                    state,
                    world,
                    captured)) {
                continue;
            }
            if (heisenberg::Physics::TryWriteBodyFilterInfo(
                    world,
                    captured.bodyId,
                    captured.originalFilterInfo)) {
                restoredAny = true;
                // Phantom bodies spent the hold on the non-collidable layer; without a
                // pair-cache rebuild the stale "no collide" verdicts would outlive the
                // restore and the body would stay intangible after release.
                RebuildBodyCollisionCachesNative(world, captured.bodyId);
                spdlog::info(
                    "[GRAB-MULTIBODY] Restored body {} filter to "
                    "0x{:08X} (layer={})",
                    captured.bodyId,
                    captured.originalFilterInfo,
                    captured.originalFilterInfo & 0x7Fu);
            }
        }
        return restoredAny;
    }

    static bool SyncCapturedHeldBodyFrames(
        heisenberg::GrabState& state,
        const RE::NiTransform& rootWorld,
        RE::bhkWorld* bhkWorld)
    {
        if (!state.collisionObject ||
            !bhkWorld ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world) {
            return false;
        }

        heisenberg::Physics::WorldWriteLock lock(bhkWorld);

        // Multipart clutter is one rigid visual object.  Validate the entire
        // captured set before writing any body so a wrapper disappearing or
        // being rebound mid-grab cannot move only half of the object.
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            if (!captured.valid ||
                !CapturedHeldBodyStillOwned(
                    state,
                    world,
                    captured)) {
                return false;
            }
        }

        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            const RE::NiTransform desiredOwnerWorld =
                rock::transform_math::composeTransforms(
                    rootWorld,
                    captured.ownerRootLocal);
            const RE::NiTransform desiredBodyWorld =
                rock::transform_math::composeTransforms(
                    desiredOwnerWorld,
                    captured.bodyOwnerLocal);
            rock::havok_runtime::setBodyTransformDeferred(
                world,
                captured.bodyId,
                desiredBodyWorld,
                1);
        }
        return true;
    }

    // Alternate collision variants can enter acquisition with the reference
    // root and active visible child in different places. The concrete Fallout
    // case is 10mmAmmo: visible AmmoMultiple can be more than a metre from the
    // stale AmmoSingle/root body. Preserve the active visible child and stage a
    // root correction; moving the child to the stale root is the visible
    // "ammo box teleported away, then got pulled into my hand" bug.
    static bool StageProxyRootRebaseToVisibleAuthority(
        heisenberg::GrabState& state)
    {
        state.pendingVisibleRootRebase = false;
        if (!state.isProxyCollision ||
            !state.node ||
            !state.physicsNode ||
            state.physicsNode.get() == state.node.get()) {
            return false;
        }

        auto* refr = state.GetRefr();
        if (!refr ||
            refr->GetFormType() == RE::ENUM_FORM_ID::kACHR) {
            return false;
        }

        RE::NiTransform authoredOwnerRootLocal{};
        if (!TryBuildDescendantRootLocal(
                state.node.get(),
                state.physicsNode.get(),
                authoredOwnerRootLocal)) {
            return false;
        }

        const RE::NiTransform oldRootWorld = state.node->world;
        const RE::NiTransform visibleAuthorityWorld =
            state.physicsNode->world;
        const RE::NiTransform runtimeOwnerRootLocal =
            rock::transform_math::composeTransforms(
                rock::transform_math::invertTransform(oldRootWorld),
                visibleAuthorityWorld);
        const float splitDistance =
            (runtimeOwnerRootLocal.translate -
             authoredOwnerRootLocal.translate)
                .Length();

        // Small discrepancies are ordinary scene/physics update latency. Only
        // normalize a clearly split multipart object.
        constexpr float kMinimumSplitDistance = 2.0f;
        if (!std::isfinite(splitDistance) ||
            splitDistance <= kMinimumSplitDistance) {
            return false;
        }

        // ownerWorld = rootWorld * authoredOwnerRootLocal
        // therefore rootWorld = ownerWorld * inverse(authoredOwnerRootLocal).
        const RE::NiTransform desiredRootWorld =
            rock::transform_math::composeTransforms(
                visibleAuthorityWorld,
                rock::transform_math::invertTransform(
                    authoredOwnerRootLocal));
        state.pendingVisibleRootWorld = desiredRootWorld;
        state.pendingVisibleRootRebase = true;
        spdlog::info(
            "[GRAB-ROOT-AUTHORITY] Ref {:08X} '{}' staged root rebase from "
            "({:.1f},{:.1f},{:.1f}) onto active visible proxy '{}' "
            "({:.1f},{:.1f},{:.1f}); split={:.2f}gu (applies after selected "
            "body becomes KEYFRAMED)",
            refr->formID,
            state.node->name.c_str(),
            oldRootWorld.translate.x,
            oldRootWorld.translate.y,
            oldRootWorld.translate.z,
            state.physicsNode->name.c_str(),
            visibleAuthorityWorld.translate.x,
            visibleAuthorityWorld.translate.y,
            visibleAuthorityWorld.translate.z,
            splitDistance);
        return true;
    }

    static bool SetCapturedHeldBodyVelocities(
        heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld,
        const RE::hkVector4f& linear,
        const RE::hkVector4f& angular)
    {
        if (!state.collisionObject ||
            !bhkWorld ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world) {
            return false;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);

        // Release velocity must also be all-or-nothing.  Giving only one body
        // in a multipart NIF a velocity immediately splits its collision frame
        // from the rendered root on the next simulation step.
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            if (!captured.valid ||
                !CapturedHeldBodyStillOwned(
                    state,
                    world,
                    captured)) {
                return false;
            }
        }

        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            rock::havok_runtime::setBodyVelocityDeferred(
                world,
                captured.bodyId,
                linear,
                angular);
        }
        return true;
    }

    static void RebuildCapturedHeldBodyCollisionCaches(
        heisenberg::GrabState& state,
        float lookAheadHavok)
    {
        if (!state.collisionObject ||
            state.capturedHeldBodyCount == 0) {
            return;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world) {
            return;
        }
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            if (!captured.valid ||
                !CapturedHeldBodyStillOwned(
                    state,
                    world,
                    captured)) {
                continue;
            }
            RebuildBodyCollisionCachesNative(
                world,
                captured.bodyId);
            heisenberg::Physics::TrySetBodyCollisionLookAhead(
                world,
                captured.bodyId,
                lookAheadHavok);
        }
    }

    // Suppress collision between the held (collidable, keyframed) object and the player by
    // OR'ing ROCK's "hand group" bits (0x000B << 16 = 0x000B0000) into the held object's
    // collisionFilterInfo. The engine's pair filter recognises the 0x000B group as "hand-
    // group body" and rejects pairs against player-attached bodies (proxy, biped, weapons,
    // FRIK avatar etc.) at broadphase. Same mechanism HandCollision uses for hand bodies.
    //
    // Crucially: the object's LAYER (low 7 bits) is preserved — so it still collides with
    // world clutter / walls / tables normally and can still be grabbed by the other hand
    // (selection rays look at the layer, not the group bits).
    //
    // Replaces the previous DisableCollisionBetween approach which was SEH-faulting every
    // single attempt on keyframed bodies (the engine's per-pair filter doesn't accept those
    // body ids cleanly while they're in keyframed state).
    static bool TryDisablePlayerHeldObjectCollision(heisenberg::GrabState& state)
    {
        if (state.capturedHeldBodyCount > 0) {
            return ApplyCapturedHeldBodyFilters(state);
        }
        const bool firstApply = !state.heldPlayerFilterApplied;
        if (!state.collisionObject || !state.collisionObject->spSystem) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: collObj/spSystem null");
            return false;
        }
        void* hknpWorld = AccessWorld(state.collisionObject);
        if (!hknpWorld) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: AccessWorld null");
            return false;
        }
        std::uint32_t objBodyId = 0x7FFFFFFF;
        heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
            state.collisionObject->spSystem.get(), &objBodyId, state.collisionObject->systemBodyIdx);
        if (objBodyId == 0x7FFFFFFF) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: GetBodyId returned invalid id");
            return false;
        }

        // Direct 0x000B group-bit OR (proven to work; see
        // feedback_rock_hand_layer_for_no_player_push.md).
        std::uint32_t cur = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objBodyId, cur)) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: TryReadBodyFilterInfo failed for body 0x{:08X}", objBodyId);
            return false;
        }
        if (firstApply) {
            state.heldOriginalFilterInfo = cur;
            state.heldHadSuppressionBitOriginally = (cur & 0x000B0000u) == 0x000B0000u;
        }
        // ROCK-style "held object that pushes world but not the player" filter:
        //   KEEP the body's NATIVE layer and OR in only the 0x000B group bits — the
        //   engine's pair filter rejects player-attached pairs by GROUP, not layer.
        // FAITHFULNESS FIX (2026-07-05 audit rank 6): this used to also swap the layer to
        // ROCK's hand layer 43, contradicting the documented working approach (see the
        // "REVERTED 2026-05-29" comment at the call site) and diverging from standalone
        // ROCK, which never re-layers held objects. Layer 43's matrix row excludes layer 43
        // itself, PROJECTILE/SPELL and ITEMPICK/LOS — so the free hand's colliders,
        // bullets, and activation picks all passed through anything you held. Native
        // CharController-object contact suppression (already active in the embed) is what
        // actually prevents player-push, making the layer swap redundant AND harmful.
        // Write directly into the hknp body's filter info. We deliberately do NOT call
        // bhkUtilFunctions_SetLayerLocked here — that helper rewrites the filter from
        // layer-presets and clobbers group/system bits, poisoning the saved "original"
        // filter and breaking thrown-object damage on release.
        constexpr std::uint32_t kHandGroupBits = 0x000Bu << 16;
        const std::uint32_t merged = cur | kHandGroupBits;
        if (merged == cur) {
            if (firstApply) spdlog::info("[GRAB-FILTER] body 0x{:08X} already has 0x000B group bits (filter=0x{:08X})", objBodyId, cur);
            return true;
        }
        if (!heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, objBodyId, merged)) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: TryWriteBodyFilterInfo failed for body 0x{:08X}", objBodyId);
            return false;
        }
        if (firstApply) {
            spdlog::info("[GRAB-FILTER] body 0x{:08X} filter 0x{:08X} -> 0x{:08X} (native layer kept + 0x000B group bits)",
                         objBodyId, cur, merged);
        }
        return true;
    }

    // Clear ROCK's hand-group bits from the held object's collisionFilterInfo on release,
    // unless they were already set before we grabbed (in which case leave them — the body
    // is intentionally part of the hand group). Restores the body to its pre-grab collision
    // behavior so the thrown/dropped object hits NPCs + the player normally afterward.
    // Jul 18 v3: engine primitive that destroys+rebuilds a body's collision caches so stale
    // pair verdicts die immediately. hknpWorld::rebuildBodyCollisionCaches(hknpBodyId), PDB
    // VA 0x14153C5A0 (F4VR 1.2.72). SEH rule: Relocation lives outside any __try.
    static void RebuildBodyCollisionCachesNative(void* hknpWorld, std::uint32_t bodyId)
    {
        using Fn = void (*)(void*, std::uint32_t);
        static REL::Relocation<Fn> s_fn{ REL::Offset(0x153C5A0) };
        s_fn(hknpWorld, bodyId);
    }

    // [REL-DIAG v2] tiny deferred filter-restore queue for the pair-cache poke. Entries are
    // written back N post-physics frames after release so the engine sees two distinct filter
    // transitions (bit14 on -> steps -> off) and rebuilds the stale hand<->object pair.
    struct DeferredFilterRestore
    {
        void* world = nullptr;
        std::uint32_t bodyId = 0x7FFFFFFF;
        std::uint32_t filter = 0;
        int framesLeft = 0;
    };
    static DeferredFilterRestore g_deferredFilterRestores[8];

    static void QueueDeferredFilterRestore(void* world, std::uint32_t bodyId, std::uint32_t filter, int frames)
    {
        for (auto& e : g_deferredFilterRestores) {
            if (e.framesLeft <= 0) {
                e = { world, bodyId, filter, frames };
                return;
            }
        }
        // queue full: restore immediately rather than dropping the write
        heisenberg::Physics::TryWriteBodyFilterInfo(world, bodyId, filter);
    }

    void TickDeferredFilterRestores(void* liveHknpWorld)
    {
        for (auto& e : g_deferredFilterRestores) {
            if (e.framesLeft <= 0) continue;
            // REGRESSION FIX (Jul 18): do NOT drop on world-pointer mismatch — EndGrab captures
            // the world via AccessWorld() while this tick reads bhk+0x60; both can name the SAME
            // world through different accessors. Dropping left bit14 set forever -> the released
            // object collided with NOTHING and fell through the floor. Always complete the
            // restore against the world captured at queue time (write is SEH-guarded).
            (void)liveHknpWorld;
            if (--e.framesLeft == 0) {
                heisenberg::Physics::TryWriteBodyFilterInfo(e.world, e.bodyId, e.filter);
                spdlog::debug("[REL-DIAG] deferred filter restore body=0x{:08X} -> 0x{:08X}", e.bodyId, e.filter);
            }
        }
    }

    static void TryRestoreHeldObjectCollision(heisenberg::GrabState& state)
    {
        if (!state.heldPlayerFilterApplied) return;
        if (state.capturedHeldBodyCount > 0) {
            (void)RestoreCapturedHeldBodyFilters(state);
            return;
        }
        if (!state.collisionObject || !state.collisionObject->spSystem) return;
        void* hknpWorld = AccessWorld(state.collisionObject);
        if (!hknpWorld) return;
        std::uint32_t objBodyId = 0x7FFFFFFF;
        heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
            state.collisionObject->spSystem.get(), &objBodyId, state.collisionObject->systemBodyIdx);
        if (objBodyId == 0x7FFFFFFF) return;

        // Clear the 0x000B hand-group bits we OR'd in during disable.
        // Since we no longer call SetLayerLocked on grab, the body's layer/system are
        // untouched and only the group bits diverge. heldOriginalFilterInfo holds the
        // exact pre-grab filter (captured before we OR'd anything), so we can either
        // write it back directly or strip just the 0x000B bits. Writing the original
        // back is the cleanest revert.
        if (state.heldHadSuppressionBitOriginally) return;  // already-suppressed bodies — leave alone
        std::uint32_t cur = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objBodyId, cur)) return;
        const std::uint32_t restored = state.heldOriginalFilterInfo;
        if (restored != cur) {
            heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, objBodyId, restored);
            spdlog::info("[GRAB-KEYFRAMED] Held object 0x{:08X} filter restored 0x{:08X} -> 0x{:08X} (layer={})",
                         objBodyId, cur, restored, restored & 0x7Fu);
        }
    }

    // FAITHFULNESS FIX (2026-07-05 audit rank 7): capture the held object's REAL pre-grab
    // collision layer at the moment the grab is about to change it. savedState.collisionLayer
    // previously stayed at its default (4 = kClutter) — the "save original on grab" was never
    // implemented — so any re-layered object whose native layer wasn't 4 (weapons=5,
    // debris=19/20, clutter-large=29) came back wrongly re-layered on release until cell
    // reload. Standalone ROCK's release restore is always byte-exact. Falls back to the old
    // behavior (restore to 4) if the filter can't be read. Marks collisionLayerChanged so the
    // release path knows a SetLayer restore is actually needed.
    static void CaptureHeldObjectLayerBeforeChange(heisenberg::GrabState& state)
    {
        // Idempotent per grab: the FIRST capture (true pre-grab layer) wins. Later re-layer
        // events in the same grab (deferred HeldBody transitions, fallback-to-keyframed) must
        // not re-capture — the body may already be on the temporary kNonCollidable layer.
        if (state.savedState.collisionLayerChanged) return;
        state.savedState.collisionLayerChanged = true;  // caller is about to re-layer the body
        if (!state.collisionObject || !state.collisionObject->spSystem) return;
        void* hknpWorld = AccessWorld(state.collisionObject);
        if (!hknpWorld) return;
        std::uint32_t objBodyId = 0x7FFFFFFF;
        heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
            state.collisionObject->spSystem.get(), &objBodyId, state.collisionObject->systemBodyIdx);
        if (objBodyId == 0x7FFFFFFF) return;
        std::uint32_t cur = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objBodyId, cur)) return;
        state.savedState.collisionLayer = cur & 0x7Fu;
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
        
        bool weaponDrawn = player->GetWeaponMagicDrawn();
        
        // If no weapon is drawn, allow grab
        if (!weaponDrawn) {
            return false;
        }
        
        // Direct memory access with full null chain checks
        // RE framework path: player->currentProcess->middleHigh->equippedItems.front().item.object
        // (the old F4SEVR middleProcess->unk08->equipData->item chain was renamed in the newer
        // CommonLibF4VR). Avoids GetEquippedWeapon which goes through AIProcess::GetEquippedItem.
        if (!player->currentProcess) {
            spdlog::debug("[WEAPON CHECK] currentProcess null - allow grab");
            return false;
        }

        auto* middleHigh = player->currentProcess->middleHigh;
        if (!middleHigh || middleHigh->equippedItems.empty()) {
            spdlog::debug("[WEAPON CHECK] no equipped items - Unarmed, allow grab");
            return false;
        }

        auto* item = middleHigh->equippedItems.front().item.object;
        if (!item) {
            spdlog::debug("[WEAPON CHECK] item null - Unarmed, allow grab");
            return false;
        }
        
        // Get weapon name. item is a TESBoundObject* (no GetFullName mixin) — use the
        // TESFullName helper (RE framework; TESForm itself doesn't expose GetFullName).
        const auto nameView = RE::TESFullName::GetFullName(*item);
        const char* name = nameView.empty() ? nullptr : nameView.data();
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
                return false;
            }
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

    bool heisenberg::IsAutomaticHandPlacementEnabled()
    {
        return g_config.enableAutomaticHandPlacement;
    }

    void heisenberg::GetEffectiveGrabPlacement(const GrabState& state, RE::NiPoint3& outLocalOffset, RE::NiMatrix3& outLocalRotation)
    {
        if (state.hasRigidRenderedHandPlacement) {
            outLocalOffset =
                state.rigidRenderedHandPlacementPosition;
            outLocalRotation =
                state.rigidRenderedHandPlacementRotation;
            return;
        }

        // Prefer runtime (geometry-calculated) placement whenever it is set,
        // regardless of config — it is populated only when no exact saved
        // offset matched, or when the config flag forces geometry.
        if (!state.isTelekinesis && state.hasRuntimeHandPlacement) {
            outLocalOffset = state.runtimeHandPlacementPosition;
            outLocalRotation = state.runtimeHandPlacementRotation;
            return;
        }

        outLocalOffset = state.itemOffset.position;
        outLocalRotation = state.itemOffset.rotation;
    }

    void heisenberg::TryCalibrateFingerDataIfIdle(bool isLeft)
    {
        auto& grabManager = GrabManager::GetSingleton();
        const GrabState& state = grabManager.GetGrabState(isLeft);
        if (grabManager.IsGrabbing(isLeft) || state.active || state.isPulling) {
            return;
        }

        const float gripValue = g_vrInput.GetGripValue(isLeft);
        const float triggerValue = g_vrInput.GetTriggerValue(isLeft);
        if (gripValue > 0.05f || triggerValue > 0.05f ||
            g_vrInput.IsPressed(isLeft, VRButton::Grip) ||
            g_vrInput.IsPressed(isLeft, VRButton::Trigger)) {
            return;
        }

        if (RE::NiNode* handNode = GetSkinnedHandNode(isLeft)) {
            heisenberg::CalibrateFingerDataFromSkeleton(handNode, isLeft);
        }
    }

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
        // Use base form name (TESFullName) to avoid HUD frameworks appending component/material tags
        // to GetDisplayFullName() (e.g. "Desk Fan [Steel, Screw]" instead of "Desk Fan")
        std::string itemName;
        if (showHudMessage) {
            auto* baseForm = refr->GetObjectReference();
            if (baseForm) {
                auto fullName = RE::TESFullName::GetFullName(*baseForm, false);
                if (!fullName.empty())
                    itemName = std::string(fullName);
            }
            if (itemName.empty())
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

        // For holotapes (kNOTE) AND readable books/magazines (kBOOK): use
        // AddObjectToContainer instead of ActivateRef. ActivateRef on a holotape
        // triggers native playback; on a perk magazine/comic (Grognak etc.) it fires
        // the vanilla "read" — granting the perk + popping the "You've collected an
        // issue" splash — which we do NOT want when simply STORING to inventory.
        // Reading is instead triggered by bringing the item up to your FACE (the
        // face-read block above). Holotape playback is handled separately by
        // PipboyInteraction when inserted into the tape deck.
        // Mark this item as recently stored so loot-to-hand doesn't re-grab it
        // ActivateRef fires TESContainerChangedEvent which loot-to-hand would intercept
        auto* baseForm = refr->GetObjectReference();
        if (baseForm) {
            heisenberg::DropToHand::GetSingleton().MarkAsRecentlyStored(baseForm->formID);
        }

        bool result = false;
        const auto storeFormType = baseForm ? baseForm->GetFormType() : RE::ENUM_FORM_ID::kNONE;
        const bool storeSilently = baseForm &&
            (storeFormType == RE::ENUM_FORM_ID::kNOTE || storeFormType == RE::ENUM_FORM_ID::kBOOK);
        if (storeSilently) {
            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
            heisenberg::AddObjectToContainer(player, static_cast<RE::TESBoundObject*>(baseForm),
                                             &nullExtra, extraCount, nullptr, 0);
            // Clean up the world reference since AddObjectToContainer doesn't do this.
            // Do NOT call SetDelete — Inventory3DManager may still hold a handle to this
            // ref for 3D preview, causing crash in FinishItemLoadTask → GetOnLocalMap.
            // SafeDisableRef defers for items with behavior graphs to prevent IO thread crash.
            SafeDisableRef(refr);
            result = true;
            spdlog::info("[STORE] {} {:08X} stored via AddObjectToContainer (no read/playback)",
                         storeFormType == RE::ENUM_FORM_ID::kBOOK ? "Book/magazine" : "Holotape", refr->formID);
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

        // Keep HUD suppressed — don't unsuppress here. Native messages (including
        // material/component breakdown for junk) would leak through any unsuppress window.
        // Instead, queue our custom message for display after deferred unsuppress.

        if (result) {
            spdlog::info("[STORE] Stored item {:08X} '{}' x{} to inventory via ActivateRef", refr->formID, itemName, extraCount);

            // Build HUD message and queue it for deferred display
            if (showHudMessage && heisenberg::g_config.showStorageMessages) {
                std::string msg;
                if (extraCount > 1) {
                    msg = std::format("{} ({}) stored", itemName, extraCount);
                } else {
                    msg = std::format("{} stored", itemName);
                }
                spdlog::info("[STORE] Queuing deferred HUD message: '{}'", msg);
                heisenberg::Hooks::ScheduleDeferredHUDUnsuppress(15, msg.c_str());
            } else {
                spdlog::info("[STORE] HUD message suppressed (showHudMessage=false)");
                heisenberg::Hooks::ScheduleDeferredHUDUnsuppress(15);
            }
        } else {
            spdlog::warn("[STORE] ActivateRef failed for {:08X} '{}'", refr->formID, itemName);
            heisenberg::Hooks::ScheduleDeferredHUDUnsuppress(15);
        }

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
               heisenberg::ContainsCI(n, "radaway") ||
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
        bool holdingInPrimaryHand = (holdingInLeftHand == primaryHandIsLeft);
        bool weaponHandIsLeft = primaryHandIsLeft;

        // ── Diagnostics: figure out the outcome + the reason WITHOUT early-returning, then
        // log it (info-level, throttled) so we can see exactly which check blocks the
        // "drop on weapon hand" equip. This function only runs while grabbing a weapon, so
        // the log isn't spammy during normal play.
        const float speedMax = heisenberg::g_config.armorEquipVelocityThreshold;
        constexpr float weaponEquipRadius = 15.0f;  // 15cm
        bool fingertipOk = false;
        float dist = -1.0f;
        bool result = false;
        const char* reason = "";

        if (holdingInPrimaryHand) {
            // You must grab the weapon with the OFF-hand and bring it to the WEAPON hand —
            // you can't hand a weapon to the hand already holding it.
            reason = "BLOCKED: weapon grabbed with PRIMARY (weapon) hand — grab with the OFF-hand";
        } else if (handSpeed >= speedMax) {
            reason = "BLOCKED: hand moving too fast";
        } else {
            auto& frik = heisenberg::FRIKInterface::GetSingleton();
            RE::NiPoint3 weaponHandFingertip;
            if (frik.IsAvailable() && frik.GetIndexFingerTipPosition(weaponHandIsLeft, weaponHandFingertip)) {
                fingertipOk = true;
            } else {
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                RE::NiAVObject* wandNode = playerNodes ? heisenberg::GetWandNode(playerNodes, weaponHandIsLeft) : nullptr;
                if (wandNode) { weaponHandFingertip = wandNode->world.translate; fingertipOk = true; }
            }
            if (!fingertipOk) {
                reason = "BLOCKED: no FRIK fingertip and no wand node for weapon hand";
            } else {
                dist = (heldWeaponPos - weaponHandFingertip).Length();
                if (dist < weaponEquipRadius) { result = true; reason = "DETECTED — will equip on release"; }
                else { reason = "too far from weapon-hand fingertip"; }
            }
        }

        static int weqLogCtr = 0;
        if (result || ++weqLogCtr >= 30) {
            weqLogCtr = 0;
            spdlog::info("[WEAP-EQUIP] grabHand={} usingOffHand={} speed={:.2f}(max{:.2f}) fingertipOK={} dist={:.1f}cm(need<{:.0f}) => {}",
                         holdingInLeftHand ? "LEFT" : "RIGHT", !holdingInPrimaryHand, handSpeed, speedMax,
                         fingertipOk, dist, weaponEquipRadius, reason);
        }
        return result;
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

    // True if the weapon is a throwable (grenade/mine). Throwables are NOT
    // holstered via Virtual Holsters — they use Heisenberg's chest-pocket zone.
    bool IsThrowableWeapon(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || baseForm->GetFormType() != RE::ENUM_FORM_ID::kWEAP)
            return false;
        auto wt = static_cast<RE::TESObjectWEAP*>(baseForm)->weaponData.type.get();
        return wt == RE::WEAPON_TYPE::kGrenade || wt == RE::WEAPON_TYPE::kMine;
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
        if (!player->GetWeaponMagicDrawn()) {
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
    // NATIVE MOUSE SPRING OVERRIDE (live: OverrideNativeGrabPosition)
    // =====================================================================
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
    
    
    // EndGrab for KEYFRAMED mode (interiors, DynamicNode, proxy objects)
    // Uses KEYFRAMED during hold, restores to DYNAMIC on release
    void EndGrabKeyframed(heisenberg::GrabState& state, const RE::NiPoint3* throwVelocity, bool isLeft,
                          const RE::NiPoint3* throwAngularVelocity = nullptr)
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
            // Restore the collision filter BEFORE clearing state — otherwise the body keeps
            // the layer-43 / 0x000B grab filter (or CSR suppression) forever and state.Clear()
            // wipes heldOriginalFilterInfo so it can never be undone. The helper is internally
            // guarded (null + SEH) so it's safe even if the body is partway gone.
            TryRestoreHeldObjectCollision(state);
            state.Clear();
            return;
        }
        
        // Restore hand pose via FRIK and release override
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        frik.ClearHandPoseFingerPositions(isLeft);
        heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, 1.0f);

        // Restore holotape scale to 1.0 (was scaled down on grab). Paper notes were never
        // scaled, so leave their scale alone.
        if (refr && state.node) {
            auto* baseObj = refr->GetObjectReference();
            if (heisenberg::IsHolotapeNote(baseObj)) {
                state.node->local.scale = 1.0f;
                spdlog::debug("[GRAB] Restored holotape {:08X} scale to 1.0", refr->formID);
            }
        }

        // Restore ROCK-style collision suppression bit (bit 14) on the held object before
        // we let go. This is independent of motion-type/layer restoration: the bit was
        // ORed in on grab to stop the body shoving the player, and must be cleared so the
        // dropped object collides normally again.
        //
        // WORLD LOCK (Jul 20): this file's own invariant (see the *Locked wrapper comments
        // below) is that all physics modifications must go through a world write lock -
        // this cluster (filter restore + native cache rebuild + CCD look-ahead) was the one
        // release-path mutation that skipped it, racing background cell streaming or other
        // in-flight Havok tasks that hold the SAME lock while mutating the same hknpWorld.
        // Resolve the lock target the same way the physics-restore block below does.
        RE::bhkWorld* poserLockWorld = refr ? GetBhkWorldFromRefr(refr) : state.savedState.savedBhkWorld;
        if (!poserLockWorld) {
            poserLockWorld = state.savedState.savedBhkWorld;
        }
        {
            std::unique_ptr<heisenberg::Physics::WorldWriteLock> poserLock;
            if (poserLockWorld) {
                poserLock = std::make_unique<heisenberg::Physics::WorldWriteLock>(poserLockWorld);
            }

            TryRestoreHeldObjectCollision(state);

            // PAIR-CACHE POKE (Jul 18): even with byte-exact filter restore through the native
            // setter, the hand<->object narrowphase pair CREATED while the hold suppressed it
            // keeps its cached no-collide verdict as long as the two stay overlapping — the
            // "must pull my hand back a few cm before it collides" symptom. Force the pair to be
            // destroyed+rebuilt by cycling the object's filter through the suppression bit and
            // back via the BS setter (each transition refreshes the body's pairing).
            if (state.capturedHeldBodyCount > 0) {
                RebuildCapturedHeldBodyCollisionCaches(state, 0.25f);
                spdlog::debug(
                    "[REL-DIAG] rebuilt collision caches for {} "
                    "captured held bodies",
                    state.capturedHeldBodyCount);
            } else if (state.collisionObject && state.collisionObject->spSystem) {
                if (void* pokeWorld = AccessWorld(state.collisionObject)) {
                    std::uint32_t pokeBodyId = 0x7FFFFFFF;
                    heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
                        state.collisionObject->spSystem.get(), &pokeBodyId, state.collisionObject->systemBodyIdx);
                    std::uint32_t pokeCur = 0;
                    if (pokeBodyId != 0x7FFFFFFF &&
                        heisenberg::Physics::TryReadBodyFilterInfo(pokeWorld, pokeBodyId, pokeCur)) {
                        // v3 (Jul 18): the REAL engine primitive. v2's 3-frame no-collide window let
                        // resting objects sink into surfaces and tunnel through. PDB names the exact
                        // call for this: hknpWorld::rebuildBodyCollisionCaches(hknpBodyId) @0x14153C5A0
                        // — destroys and rebuilds ALL of this body's collision caches (incl. the stale
                        // hand<->object pair created no-collide during the hold) with the CURRENT,
                        // already-restored filter. No collision gap, no deferred state.
                        RebuildBodyCollisionCachesNative(pokeWorld, pokeBodyId);
                        // CATCH FIX (Jul 18): a moving released object tunnels through the thin
                        // hand colliders in one physics step ("collides only after it stops",
                        // drop-to-other-hand passes through the palm). Enable per-body CCD
                        // look-ahead (0.25 Havok m ~= 17.5 gu) while it flies; ThrownObjectTracker
                        // drops it back to ~0 when tracking expires.
                        heisenberg::Physics::TrySetBodyCollisionLookAhead(pokeWorld, pokeBodyId, 0.25f);
                        spdlog::debug("[REL-DIAG] rebuildBodyCollisionCaches body=0x{:08X} filter=0x{:08X}", pokeBodyId, pokeCur);
                    }
                }
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
                const bool syncedMultipart =
                    state.capturedHeldBodyCount > 0 &&
                    SyncCapturedHeldBodyFrames(
                        state,
                        state.node->world,
                        bhkWorld);
                if (!syncedMultipart) {
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
                }
                spdlog::debug(
                    "[GRAB-KEYFRAMED] Synced {} physics body frame(s) "
                    "to {} position ({:.1f}, {:.1f}, {:.1f})",
                    syncedMultipart
                        ? state.capturedHeldBodyCount
                        : 1,
                    state.isProxyCollision ? "physics node" : "visual",
                    syncNode->world.translate.x,
                    syncNode->world.translate.y,
                    syncNode->world.translate.z);
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
            RE::hkVector4f zeroVelocity{};
            const bool zeroedMultipart =
                SetCapturedHeldBodyVelocities(
                    state,
                    bhkWorld,
                    zeroVelocity,
                    zeroVelocity);
            if (!zeroedMultipart) {
                SetLinearVelocityLocked(state.collisionObject, zeroVel, bhkWorld);
                SetAngularVelocityLocked(state.collisionObject, zeroVel, bhkWorld);
            }
            spdlog::debug(
                "[GRAB-KEYFRAMED] Zeroed velocities on {} body frame(s) "
                "before motion type change",
                zeroedMultipart ? state.capturedHeldBodyCount : 1);
            
            // Restore the same motion scope used at setup. Alternate visual
            // wrappers sharing this physics system were never keyframed and
            // must not be activated or released with the selected variant.
            const bool selectedWrapperScope =
                UsesSelectedCollisionWrapperMotionScope(state);
            if (selectedWrapperScope) {
                SetMotionTypeLocked(
                    state.collisionObject,
                    RE::hknpMotionPropertiesId::Preset::DYNAMIC,
                    bhkWorld);
            } else if (state.capturedHeldBodyCount > 0) {
                bhkWorld_SetMotionLocked(
                    state.node.get(),
                    RE::hknpMotionPropertiesId::Preset::DYNAMIC,
                    true,
                    true,
                    true,
                    bhkWorld);
            } else {
                SetMotionTypeLocked(
                    state.collisionObject,
                    RE::hknpMotionPropertiesId::Preset::DYNAMIC,
                    bhkWorld);
            }
            spdlog::debug(
                "[GRAB-KEYFRAMED] Restored motion type to DYNAMIC "
                "(scope={})",
                selectedWrapperScope
                    ? "active collision wrapper"
                    : (state.capturedHeldBodyCount > 0
                           ? "reference subtree"
                           : "primary body"));
            
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
            
            // Restore collision layer ONLY if the grab actually changed it (kNonCollidable=15
            // path). FAITHFULNESS FIX (2026-07-05 audit rank 7): this used to run
            // unconditionally with a never-captured default of 4, clobbering the byte-exact
            // filter restore above (TryRestoreHeldObjectCollision) — SetLayerLocked rewrites
            // the filter from layer presets, wiping group/system bits, and mis-layered any
            // object whose native layer wasn't 4 (weapons=5, debris=19/20, clutter-large=29).
            // In the bHeldObjectCollidable path the layer was never touched, so nothing to do.
            // audit rank 7c: also skip when the 0x000B group-bit filter was applied — the
            // byte-exact TryRestoreHeldObjectCollision above already wrote back the true
            // pre-grab filter (layer included), and this preset-based SetLayer would clobber
            // its group/system bits (both flags can co-set via a deferred-HeldBody fallback).
            if (state.savedState.collisionLayerChanged && !state.heldPlayerFilterApplied) {
                bhkUtilFunctions_SetLayerLocked(state.node.get(), state.savedState.collisionLayer, bhkWorld);
                spdlog::debug("[GRAB-KEYFRAMED] Restored collision layer to captured pre-grab value ({})", state.savedState.collisionLayer);
            }
            
            // Re-validate the collision object: the SetLayerLocked above can
            // trigger an internal engine re-add of the collision object to the
            // world. If the proxy/body was already torn down (e.g. cell unload
            // race, despawn after a previous grab cycle on the same refr), the
            // wrapper still looks valid (spSystem != null) but the underlying
            // havok body is freed and the next velocity call AVs inside
            // bhkNPCollisionObject::SetLinearVelocity. Bail before that.
            if (!IsCollisionObjectValid(state.collisionObject)) {
                spdlog::warn("[GRAB-KEYFRAMED] Collision object invalid before velocity apply — skipping");
            }
            // Apply throw velocity if throwing, or a small gravity nudge if dropping
            // We zeroed velocities before motion type change, so now we can set fresh velocity
            // CRITICAL: Even for drops, we must apply a small velocity to WAKE the physics body!
            // Without this, the body stays asleep and floats in mid-air.
            else if (throwVelocity &&
                (throwVelocity->x != 0 || throwVelocity->y != 0 || throwVelocity->z != 0))
            {
                // Convert to Havok units and apply
                RE::NiPoint4 hkLinearVel(
                    throwVelocity->x * HAVOK_WORLD_SCALE,
                    throwVelocity->y * HAVOK_WORLD_SCALE,
                    throwVelocity->z * HAVOK_WORLD_SCALE,
                    0.0f
                );
                RE::hkVector4f capturedLinearVelocity{};
                capturedLinearVelocity.x = hkLinearVel.x;
                capturedLinearVelocity.y = hkLinearVel.y;
                capturedLinearVelocity.z = hkLinearVel.z;
                RE::hkVector4f capturedAngularVelocity{};
                if (throwAngularVelocity) {
                    capturedAngularVelocity.x =
                        throwAngularVelocity->x;
                    capturedAngularVelocity.y =
                        throwAngularVelocity->y;
                    capturedAngularVelocity.z =
                        throwAngularVelocity->z;
                }
                const bool appliedMultipartVelocity =
                    SetCapturedHeldBodyVelocities(
                        state,
                        bhkWorld,
                        capturedLinearVelocity,
                        capturedAngularVelocity);
                if (!appliedMultipartVelocity) {
                    SetLinearVelocityLocked(
                        state.collisionObject,
                        hkLinearVel,
                        bhkWorld);
                }
                spdlog::debug("[GRAB-KEYFRAMED] Applied throw velocity: ({:.1f},{:.1f},{:.1f})",
                            throwVelocity->x, throwVelocity->y, throwVelocity->z);

                // FAITHFULNESS (audit rank 3): ROCK also imparts the hand's ANGULAR velocity
                // on release (composeControllerReleaseAngularVelocity, capped 18 rad/s) —
                // thrown objects spin with the wrist. Rad/s, world frame, no world scale.
                if (throwAngularVelocity &&
                    (throwAngularVelocity->x != 0 || throwAngularVelocity->y != 0 || throwAngularVelocity->z != 0)) {
                    RE::NiPoint4 hkAngularVel(
                        throwAngularVelocity->x,
                        throwAngularVelocity->y,
                        throwAngularVelocity->z,
                        0.0f);
                    if (!appliedMultipartVelocity) {
                        SetAngularVelocityLocked(
                            state.collisionObject,
                            hkAngularVel,
                            bhkWorld);
                    }
                    spdlog::debug("[GRAB-KEYFRAMED] Applied release angular velocity: ({:.2f},{:.2f},{:.2f}) rad/s",
                                throwAngularVelocity->x, throwAngularVelocity->y, throwAngularVelocity->z);
                }

                // Register the thrown body for impact-effect tracking (damage,
                // destruction, detection, hit events). Resolves the body ID
                // from the collision object's physics system index.
                if (refr && state.collisionObject && state.collisionObject->spSystem) {
                    void* physicsSystem = state.collisionObject->spSystem.get();
                    std::uint32_t systemIndex = state.collisionObject->systemBodyIdx;
                    std::uint32_t bodyId = 0x7FFFFFFF;
                    heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
                        physicsSystem, &bodyId, systemIndex);
                    if (bodyId != 0x7FFFFFFF) {
                        heisenberg::ThrownObjectTracker::GetSingleton().OnThrown(
                            refr, bodyId, *throwVelocity);
                    }
                }
            }
            else
            {
                // Apply a tiny downward velocity to wake the physics body
                // This ensures gravity takes over immediately instead of the object floating
                // The velocity is imperceptible but enough to wake the body from sleep
                constexpr float GRAVITY_NUDGE = -0.1f;  // Tiny downward nudge in game units/sec
                RE::NiPoint4 gravityNudge(0.0f, 0.0f, GRAVITY_NUDGE * HAVOK_WORLD_SCALE, 0.0f);
                RE::hkVector4f capturedGravityNudge{};
                capturedGravityNudge.z =
                    GRAVITY_NUDGE * HAVOK_WORLD_SCALE;
                RE::hkVector4f zeroAngular{};
                if (!SetCapturedHeldBodyVelocities(
                        state,
                        bhkWorld,
                        capturedGravityNudge,
                        zeroAngular)) {
                    SetLinearVelocityLocked(
                        state.collisionObject,
                        gravityNudge,
                        bhkWorld);
                }
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

    RE::bhkWorld* ResolveGrabBhkWorld(heisenberg::GrabState& state)
    {
        RE::bhkWorld* bhkWorld = state.savedState.savedBhkWorld;
        if (RE::TESObjectREFR* refr = state.GetRefr()) {
            if (RE::bhkWorld* currentWorld = GetBhkWorldFromRefr(refr)) {
                bhkWorld = currentWorld;
            }
        }
        return bhkWorld;
    }

    bool ComputeHeldGrabTargetTransform(const heisenberg::GrabState& state, bool isLeft,
                                        const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                                        RE::NiTransform& outTransform)
    {
        if (!state.node) {
            return false;
        }

        RE::NiPoint3 parentPos;
        RE::NiMatrix3 parentRot;
        ResolveGrabParent(state, isLeft, handPos, handRot, parentPos, parentRot);

        RE::NiPoint3 localOffset;
        RE::NiMatrix3 localRotation;
        heisenberg::GetEffectiveGrabPlacement(state, localOffset, localRotation);
        if (heisenberg::Utils::IsPlayerInPowerArmor()) {
            localOffset.x += heisenberg::g_config.paGrabOffsetX;
            localOffset.y += heisenberg::g_config.paGrabOffsetY;
            localOffset.z += heisenberg::g_config.paGrabOffsetZ;
        }

        // HIGGS apply: world = parentWorld * local → world.rotate = parent.rotate * local.rotate;
        // world.translate = parent.translate + parent.rotate * local.translate.
        outTransform.rotate = localRotation * parentRot;
        outTransform.translate = parentPos + (parentRot.Transpose() * localOffset);
        outTransform.scale = state.node->local.scale > 0.0f ? state.node->local.scale : 1.0f;
        return true;
    }

    // (Two-scenario cleanup: the deferred-HeldBody activation helpers that lived here
    // were removed with the HeldBodyGrab system. ComputeHeldGrabTargetTransform above
    // stays — the instant-grab path still uses it.)
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

    // Limb grab: walk an actor's loaded 3D and return the ragdoll-limb bhkNPCollisionObject
    // whose bone node is nearest handPos (so you grab the arm/leg under your hand, not the
    // actor root). Uses the SAME RTTI-validated cast as GetCollisionObject (follows proxy
    // targets, rejects non-bhkNPCollisionObject) so it can't grab the char-proxy by mistake.
    static RE::bhkNPCollisionObject* FindClosestLimbCollision(RE::NiAVObject* root, const RE::NiPoint3& handPos,
                                                              RE::NiAVObject** outNode)
    {
        if (!root) {
            return nullptr;
        }
        auto validate = [](RE::NiCollisionObject* collObj) -> RE::bhkNPCollisionObject* {
            if (!collObj) return nullptr;
            auto* rtti = collObj->GetRTTI();
            if (!rtti || !rtti->GetName()) return nullptr;
            const char* typeName = rtti->GetName();
            if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
                return GetProxyTarget(collObj);
            }
            for (auto iter = rtti; iter; iter = iter->GetBaseRTTI()) {
                if (iter->GetName() && std::strcmp(iter->GetName(), "bhkNPCollisionObject") == 0) {
                    return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
                }
            }
            return nullptr;
        };
        RE::bhkNPCollisionObject* best = nullptr;
        RE::NiAVObject* bestNode = nullptr;
        float bestDistSq = 1.0e30f;
        std::function<void(RE::NiAVObject*)> walk = [&](RE::NiAVObject* node) {
            if (!node) return;
            if (auto* npColl = validate(node->collisionObject.get())) {
                const RE::NiPoint3 d = node->world.translate - handPos;
                const float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    best = npColl;
                    bestNode = node;
                }
            }
            if (auto* asNode = node->IsNode()) {
                for (auto& child : asNode->children) {
                    if (child) walk(child.get());
                }
            }
        };
        walk(root);
        if (best && outNode) *outNode = bestNode;
        return best;
    }

    static bool IsNodeOwnedByReferenceRoot(
        RE::NiAVObject* node,
        RE::NiAVObject* referenceRoot)
    {
        for (auto* current = node; current; current = current->parent) {
            if (current == referenceRoot) {
                return true;
            }
        }
        return false;
    }

    // Keep SEH in a trivial leaf function. MSVC rejects __try in
    // ResolvePhysicalTouchCollisionObject because that caller owns C++ objects
    // which require unwinding (C2712).
    static RE::bhkNPCollisionObject*
    TryResolveNpcCollisionObjectFromRaw(
        RE::NiCollisionObject* rawCollision)
    {
        if (!rawCollision) {
            return nullptr;
        }

        RE::bhkNPCollisionObject* resolved = nullptr;
        __try {
            auto* rtti = rawCollision->GetRTTI();
            if (!rtti || !rtti->GetName()) {
                return nullptr;
            }
            const char* typeName = rtti->GetName();
            if (std::strcmp(
                    typeName,
                    "bhkNPCollisionProxyObject") == 0) {
                resolved = GetProxyTarget(rawCollision);
            } else {
                for (auto* current = rtti;
                     current;
                     current = current->GetBaseRTTI()) {
                    const char* currentName = current->GetName();
                    if (currentName &&
                        std::strcmp(
                            currentName,
                            "bhkNPCollisionObject") == 0) {
                        resolved =
                            reinterpret_cast<
                                RE::bhkNPCollisionObject*>(
                                rawCollision);
                        break;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return resolved;
    }

    // Return true only when this collision owner's own rendered subtree is
    // visible. A nested node with a different collision wrapper belongs to a
    // separate physics/visual variant and is deliberately excluded.
    static bool CollisionOwnerHasVisibleGeometry(
        RE::NiAVObject* node,
        RE::NiAVObject* ownerNode,
        RE::bhkNPCollisionObject* ownerCollision,
        std::uint32_t depth)
    {
        if (!node || !ownerNode || !ownerCollision || depth > 64) {
            return false;
        }
        if ((node->flags.flags & static_cast<std::uint64_t>(0x1)) != 0 ||
            !(node->local.scale > 0.001f)) {
            return false;
        }

        if (node != ownerNode) {
            if (auto* nestedCollision =
                    TryResolveNpcCollisionObjectFromRaw(
                        node->collisionObject.get());
                nestedCollision &&
                nestedCollision != ownerCollision) {
                return false;
            }
        }

        if (node->IsGeometry()) {
            return true;
        }
        if (auto* asNode = node->IsNode()) {
            for (auto& child : asNode->children) {
                if (child &&
                    CollisionOwnerHasVisibleGeometry(
                        child.get(),
                        ownerNode,
                        ownerCollision,
                        depth + 1)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Pick the collision wrapper whose own mesh variant is currently rendered.
    // This is required for Bethesda NIFs such as 10mmAmmo.nif: AmmoSingle and
    // AmmoMultiple are alternate representations sharing one physics system,
    // not two rigid pieces of one object.
    static RE::bhkNPCollisionObject*
    ResolveActiveVisibleCollisionObject(
        RE::NiAVObject* referenceRoot,
        const RE::NiPoint3& handPosition,
        RE::NiAVObject** outOwnerNode,
        std::uint32_t* outWrapperCount = nullptr,
        std::uint32_t* outVisibleWrapperCount = nullptr)
    {
        if (outOwnerNode) {
            *outOwnerNode = nullptr;
        }
        if (outWrapperCount) {
            *outWrapperCount = 0;
        }
        if (outVisibleWrapperCount) {
            *outVisibleWrapperCount = 0;
        }
        if (!referenceRoot) {
            return nullptr;
        }

        RE::bhkNPCollisionObject* bestCollision = nullptr;
        RE::NiAVObject* bestOwner = nullptr;
        float bestDistanceSquared =
            (std::numeric_limits<float>::max)();

        std::function<void(RE::NiAVObject*, std::uint32_t)> visit =
            [&](RE::NiAVObject* node, std::uint32_t depth) {
                if (!node || depth > 64) {
                    return;
                }

                if (auto* collision =
                        TryResolveNpcCollisionObjectFromRaw(
                            node->collisionObject.get())) {
                    if (outWrapperCount) {
                        ++*outWrapperCount;
                    }
                    if (CollisionOwnerHasVisibleGeometry(
                            node,
                            node,
                            collision,
                            0)) {
                        if (outVisibleWrapperCount) {
                            ++*outVisibleWrapperCount;
                        }
                        const RE::NiPoint3 delta =
                            node->world.translate - handPosition;
                        const float distanceSquared =
                            delta.x * delta.x +
                            delta.y * delta.y +
                            delta.z * delta.z;
                        if (distanceSquared < bestDistanceSquared) {
                            bestDistanceSquared = distanceSquared;
                            bestCollision = collision;
                            bestOwner = node;
                        }
                    }
                }

                if (auto* asNode = node->IsNode()) {
                    for (auto& child : asNode->children) {
                        if (child) {
                            visit(child.get(), depth + 1);
                        }
                    }
                }
            };
        visit(referenceRoot, 0);

        if (bestCollision && outOwnerNode) {
            *outOwnerNode = bestOwner;
        }
        return bestCollision;
    }

    // Resolve the collision wrapper that owns ROCK's exact contacted hknp body.
    // GetCollisionObject() returns the first wrapper found in scene traversal,
    // which is ambiguous for multipart clutter. The body's native backpointer is
    // authoritative, but it is accepted only when its owner is still inside the
    // selected reference's live scene tree so a stale body ID cannot grab an
    // unrelated object.
    static RE::bhkNPCollisionObject* ResolvePhysicalTouchCollisionObject(
        RE::TESObjectREFR* refr,
        RE::NiAVObject* referenceRoot,
        std::uint32_t bodyId,
        RE::NiAVObject** outOwnerNode)
    {
        if (outOwnerNode) {
            *outOwnerNode = nullptr;
        }
        if (!refr || !referenceRoot ||
            bodyId == rock::havok_runtime::kInvalidBodyIdValue) {
            return nullptr;
        }

        auto* bhkWorld = GetBhkWorldFromRefr(refr);
        auto* hknpWorld =
            rock::havok_runtime::getHknpWorldFromBhk(bhkWorld);
        if (!hknpWorld) {
            return nullptr;
        }

        auto* rawCollision =
            rock::havok_runtime::getCollisionObjectFromBody(
                hknpWorld,
                RE::hknpBodyId{ bodyId });
        auto* owner =
            rock::havok_runtime::getOwnerNodeFromCollisionObject(
                rawCollision);
        if (!rawCollision || !owner ||
            !IsNodeOwnedByReferenceRoot(owner, referenceRoot)) {
            return nullptr;
        }

        auto* resolved =
            TryResolveNpcCollisionObjectFromRaw(rawCollision);

        if (resolved && outOwnerNode) {
            *outOwnerNode = resolved->sceneObject
                ? resolved->sceneObject
                : owner;
        }
        return resolved;
    }

    bool GrabManager::StartGrab(const Selection& selection, const RE::NiPoint3& handPos,
                                const RE::NiMatrix3& handRot, bool isLeft, bool skipWeaponEquip)
    {
        // Master toggle check
        if (!g_config.enableGrabbing) {
            return false;
        }

        // Check if hand is disabled via API
        if (HeisenbergPluginAPI::IsHandDisabledByAPI(isLeft)) {
            spdlog::debug("[GRAB] {} hand disabled by API - blocking grab", isLeft ? "Left" : "Right");
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
        
        // WEAPON CHECK: block grab on the PRIMARY (weapon) hand if a REAL weapon
        // (melee or ranged) is drawn — the player must holster it first. Bare fists
        // are the exception: sheathe them so the hand opens to grab. Fists are global
        // (both hands), so the unarmed sheathe runs for either hand; the real-weapon
        // block is primary-hand only.
        {
            const auto physicalHand = isLeft ? HeisenbergPluginAPI::PhysicalHand::kLeft :
                                               HeisenbergPluginAPI::PhysicalHand::kRight;
            auto dualState = HeisenbergPluginAPI::MakeHandState(physicalHand);
            if (HeisenbergPluginAPI::QueryDualWieldHandState(physicalHand, dualState) &&
                (dualState.flags & HeisenbergPluginAPI::kHandStateOccupied) != 0) {
                spdlog::debug("[GRAB] Dual-wield provider reports {} hand occupied - blocking grab",
                              isLeft ? "left" : "right");
                return false;
            }

            bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
            bool isPrimaryHand = (isLeftHandedMode ? isLeft : !isLeft);
            if (isPrimaryHand && g_heisenberg.GetCachedHasRealWeapon()) {
                spdlog::debug("[GRAB] Real weapon equipped - blocking grab on primary hand (holster weapon first)");
                return false;
            }
            // Unarmed fists only — sheathe so the hand fully opens (no-op if a real
            // weapon is drawn, or nothing is drawn).
            heisenberg::Heisenberg::GetSingleton().DeactivateUnarmedForGrab();
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
        // Only bypass that guard when the build-3 provider explicitly accepts this
        // candidate form for this physical hand; merely registering is insufficient.
        // mechanics interfere with native game behavior, causing infinite loading screens
        if (selRefr && Utils::IsPlayerInPowerArmor()) {
            auto* baseObj = selRefr->data.objectReference;
            if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
                const auto physicalHand = isLeft ? HeisenbergPluginAPI::PhysicalHand::kLeft :
                                                   HeisenbergPluginAPI::PhysicalHand::kRight;
                auto candidate = HeisenbergPluginAPI::MakeHandState(physicalHand);
                candidate.flags |= HeisenbergPluginAPI::kHandStateCandidateQuery;
                candidate.formID = baseObj->GetFormID();
                const bool accepted = HeisenbergPluginAPI::QueryDualWieldHandState(
                    physicalHand, candidate) &&
                    (candidate.flags & HeisenbergPluginAPI::kHandStateCandidateEligible) != 0;
                if (!accepted) {
                spdlog::debug("[GRAB] Skipping weapon in Power Armor - using native game behavior");
                return false;  // Let native game handle PA weapon pickup
                }
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
        
        spdlog::debug("[GRAB] {} hand: Using KEYFRAME mode", isLeft ? "Left" : "Right");

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
            // Two-handed CO-HOLD: when enabled and the other hand is the PRIMARY holder (not
            // itself a secondary aim hand), this hand joins as the secondary aim hand instead of
            // transferring the object. The primary keeps full physics ownership/drive; this hand
            // is a marker only (active+refr so input/release work, coHeldSecondary so the drive
            // skips it and the primary's drive reads our wand for the aim swing).
            if (g_config.enableTwoHandedGrab && !otherState.coHeldSecondary)
            {
                spdlog::info("[GRAB] Two-handed CO-HOLD: {} hand joins as aim hand on ref {:08X}",
                             isLeft ? "Left" : "Right", selRefr->formID);
                state.SetRefr(selRefr);
                state.node = selection.node;
                state.active = true;
                state.coHeldSecondary = true;
                return true;
            }
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

        // Native contact follows the rendered/skinned FRIK hand, whereas the
        // input state supplied to StartGrab is the controller/wand frame. These
        // normally agree closely, but the ammo repro showed a 330-gu split:
        // ROCK was touching the round at the visible hand while the wand still
        // lived in the other frame. A physical-touch grab must be captured and
        // subsequently driven in the same skinned-hand frame that made contact.
        RE::NiPoint3 grabHandPos = handPos;
        RE::NiMatrix3 grabHandRot = handRot;
        RE::NiNode* physicalTouchHandNode = nullptr;
        if (selection.isPhysicalTouch) {
            physicalTouchHandNode = GetSkinnedHandNode(isLeft);
            if (physicalTouchHandNode) {
                grabHandPos = physicalTouchHandNode->world.translate;
                grabHandRot = physicalTouchHandNode->world.rotate;
            }
            spdlog::info(
                "[GRAB-TOUCH] {} ref={:08X} controller=({:.1f},{:.1f},{:.1f}) "
                "skinned=({:.1f},{:.1f},{:.1f}) delta={:.1f} skinnedResolved={}",
                isLeft ? "L" : "R",
                selRefr->formID,
                handPos.x, handPos.y, handPos.z,
                grabHandPos.x, grabHandPos.y, grabHandPos.z,
                (grabHandPos - handPos).Length(),
                physicalTouchHandNode ? "yes" : "no");
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
        state.physicalTouchGrab = selection.isPhysicalTouch;
        state.physicalTouchBodyId =
            selection.physicalTouchBodyId;
        state.initialHandPos = grabHandPos;
        state.initialHandRot = grabHandRot;
        
        // If this was a transfer, preserve the original world parent
        if (isTransfer && transferFromOriginalParent)
        {
            state.originalParent = transferFromOriginalParent;
            spdlog::debug("[GRAB] Transfer: preserving original parent '{}'",
                         transferFromOriginalParent->name.c_str());
        }
        
        // Get collision object first - we need it to detect special cases
        state.collisionObject = GetCollisionObject(selRefr);
        if (selRefr->GetFormType() != RE::ENUM_FORM_ID::kACHR) {
            RE::NiAVObject* activeOwner = nullptr;
            std::uint32_t wrapperCount = 0;
            std::uint32_t visibleWrapperCount = 0;
            if (auto* activeCollision =
                    ResolveActiveVisibleCollisionObject(
                        state.node.get(),
                        grabHandPos,
                        &activeOwner,
                        &wrapperCount,
                        &visibleWrapperCount);
                activeCollision &&
                activeOwner &&
                wrapperCount > 1) {
                state.collisionObject = activeCollision;
                state.physicsNode.reset(activeOwner);
                state.isProxyCollision =
                    activeOwner != state.node.get();
                spdlog::info(
                    "[GRAB-VARIANT] Selected active visible collision owner "
                    "'{}' for {:08X} (root='{}' wrappers={} visible={} proxy={})",
                    activeOwner->name.c_str(),
                    selRefr->formID,
                    state.node->name.c_str(),
                    wrapperCount,
                    visibleWrapperCount,
                    state.isProxyCollision);
            }
        }
        if (selection.isPhysicalTouch) {
            RE::NiAVObject* touchedOwner = nullptr;
            if (auto* touchedCollision =
                    ResolvePhysicalTouchCollisionObject(
                        selRefr,
                        state.node.get(),
                        selection.physicalTouchBodyId,
                        &touchedOwner)) {
                state.collisionObject = touchedCollision;
                state.physicsNode.reset(
                    touchedOwner ? touchedOwner : state.node.get());
                state.isProxyCollision =
                    touchedOwner &&
                    touchedOwner != state.node.get();
                spdlog::info(
                    "[GRAB-TOUCH] Exact contacted body {} resolved to "
                    "collision owner '{}' (root='{}' proxy={})",
                    selection.physicalTouchBodyId,
                    touchedOwner
                        ? touchedOwner->name.c_str()
                        : "NULL",
                    state.node->name.c_str(),
                    state.isProxyCollision);
            } else {
                spdlog::warn(
                    "[GRAB-TOUCH] Exact contacted body {} could not be "
                    "validated against ref {:08X}; using scene traversal "
                    "collision wrapper",
                    selection.physicalTouchBodyId,
                    selRefr->formID);
            }
        }
        // v2: baseline must use the SAME signal as the steady-state check below (player's
        // parentCell, not the held object's — see the WORLD-CHANGE RESYNC comment).
        if (auto* worldPlayer = RE::PlayerCharacter::GetSingleton()) {
            if (auto* worldPlayerCell = worldPlayer->GetParentCell()) {
                state.lastSyncedBhkWorld = worldPlayerCell->GetbhkWorld();
            }
        }

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
            } else if (!state.physicsNode) {
                state.physicsNode = state.node;  // Normal case: physics is on grabbed node (NiPointer copy)
            }

            // Limb grab: for an actor, retarget physics to the ragdoll limb nearest the hand
            // (the arm/leg under your hand) instead of whatever body GetCollisionObject found
            // first. Keeps state.node as the actor root (visual/handle) and drives the limb via
            // the proxy-collision path. Double opt-in: bEnableLimbGrab AND bEnableGrabActors.
            if (g_config.enableLimbGrab && g_config.enableGrabActors && selRefr &&
                selRefr->GetFormType() == RE::ENUM_FORM_ID::kACHR && selRefr->Get3D()) {
                RE::NiAVObject* limbNode = nullptr;
                if (auto* limbColl = FindClosestLimbCollision(selRefr->Get3D(), grabHandPos, &limbNode);
                    limbColl && limbNode) {
                    state.collisionObject = limbColl;
                    state.physicsNode.reset(limbNode);
                    state.isProxyCollision = true;
                    spdlog::info("[GRAB] Limb-grab: targeting limb '{}' on actor {:08X}",
                                 limbNode->name.c_str(), selRefr->formID);
                }
            }
        } else {
            spdlog::warn("[GRAB] No collision object for {:08X}! Node='{}'",
                selRefr->formID, state.node ? state.node->name.c_str() : "NULL");
            state.physicsNode = state.node;  // NiPointer copy
        }

        // If the selected active proxy is visibly somewhere else than its
        // reference root, stage the reference correction around that visible
        // pose before any distance, offset, or pull calculation reads objectPos.
        StageProxyRootRebaseToVisibleAuthority(state);
        
        // ═══════════════════════════════════════════════════════════════════════════
        // PHYSICS MODE — KEYFRAMED (the only Heisenberg-owned backend after the
        // two-scenario cleanup; iGrabMode=9 hands grab ownership to the embedded engine
        // before this path is ever reached).
        // ═══════════════════════════════════════════════════════════════════════════
        state.usingKeyframedMode = true;
        spdlog::debug("[GRAB-KEYFRAMED] Using KEYFRAMED mode");
        
        // Reset room tracking state for keyframed mode (prevents jitter from stale data)
        state.lastRoomPos = RE::NiPoint3(0, 0, 0);
        state.smoothedRoomDelta = RE::NiPoint3(0, 0, 0);
        state.roomTrackingInitialized = false;
        
        // For VR grabbing - get object's current world transform
        RE::NiTransform worldTransform =
            state.pendingVisibleRootRebase
                ? state.pendingVisibleRootWorld
                : selection.node->world;
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
            distToCenter = (objectPos - grabHandPos).Length();  // Distance to center
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
        // A surface-distance hit is authoritative even when the object's centre is
        // farther from the controller (large/offset meshes such as Radroach Meat).
        // Centre/selection distance may still decide remote pull for non-contact
        // selections, but it must never turn a real touch into a palm snap.
        constexpr float kTouchGrabMaxDistance = 3.0f;
        const bool isActualTouchContact =
            !forceUseOffset &&
            (selection.isPhysicalTouch || distToObject <= kTouchGrabMaxDistance);

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
        bool isRemoteSelection =
            selection.distance > g_config.snapDistance && !isActualTouchContact;
        bool useTelekinesis = false;  // Will this grab use telekinesis (follow-hand) mode?
        bool usePullToHand = false;   // Will this grab pull the object to the hand?
        
        if (forceUseOffset) {
            // DropToHand — instant snap to hand (always works regardless of settings)
            isPalmSnap = true;
            isNaturalGrab = false;
            spdlog::debug("[GRAB] DropToHand '{}' at {:.1f}cm — instant snap",
                        itemName, distToObject);
        } else if (isTransfer) {
            // Hand-to-hand transfer: receiving hand always grabs in place (telekinesis),
            // never snaps to palm — regardless of distance or config.
            useTelekinesis = true;
            isNaturalGrab = true;
            isPalmSnap = false;
            spdlog::debug("[GRAB] TRANSFER (natural grab) for '{}' at {:.1f}cm",
                        itemName, distToObject);
        } else if (heisenberg::g_config.enableNaturalGrab &&
                   isNaturalGrab &&
                   !isActualTouchContact) {
            // Telekinesis ON + within telekinesis range → object follows hand at distance
            // A proven physical touch is handled by the mesh-contact branch below
            // even when it is inside this range. Treating a touch as telekinesis
            // forced the fingers open and bypassed the exact no-snap contact pose.
            useTelekinesis = true;
            spdlog::debug("[GRAB] TELEKINESIS for '{}' at {:.1f}cm (range={:.1f}cm)",
                        itemName, distToObject, naturalGrabThreshold);
        } else {
            // Beyond telekinesis range OR telekinesis off → palm snap.
            // Object Pull (bEnablePalmSnap) only gates remote pulls; close-range
            // palm snap always works so users who only disable Object Pull can
            // still grab nearby items.
            if (isRemoteSelection && !heisenberg::g_config.enablePalmSnap) {
                spdlog::debug("[GRAB] REJECTED '{}' at {:.1f}cm — Object Pull disabled and selection is remote (selDist={:.1f})",
                            itemName, distToObject, selection.distance);
                return false;
            }
            if (isRemoteSelection) {
                // Selected from distance with pull enabled — animate pull to hand
                usePullToHand = true;
            }
            isPalmSnap = true;
            isNaturalGrab = false;
            spdlog::debug("[GRAB] {} for '{}': surfaceDist={:.1f}cm, selectionDist={:.1f}",
                        usePullToHand ? "PULL TO HAND" : "PALM SNAP",
                        itemName, distToObject, selection.distance);
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
            // Only actual holotapes get the forced palm-snap + shared NOTE_DEFAULT offset.
            // Paper notes grab like normal clutter (natural/palm by distance + their own offset).
            isHolotape = heisenberg::IsHolotapeNote(baseObj);
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
            } else if (distToObject <= MSTT_PALM_SNAP_MAX) {
                // 10-20cm - geometry-based palm snap (embedded offsets bypassed for testing)
                isNaturalGrab = false;
                isPalmSnap = true;
                spdlog::debug("[GRAB] MSTT '{}' at {:.1f}cm - palm snap (geometry calc)",
                            itemName, distToObject);
            } else {
                // Beyond allowed range
                spdlog::debug("[GRAB] REJECTED: MSTT '{}' at {:.1f}cm (max={:.1f}cm)",
                            itemName, distToObject, MSTT_PALM_SNAP_MAX);
                return false;
            }
        }
        
        // Prefer the integrated/saved offset for this item when we have an
        // exact match (FormID or name). Geometry-based placement acts only
        // as a fallback when no exact match exists. Telekinesis grabs still
        // ignore saved offsets (world position is preserved below).
        std::optional<ItemOffset> customOffset = std::nullopt;
        if (!useTelekinesis) {
            customOffset = offsetMgr.GetExactOffset(selRefr, isLeft);
            // No FormID/name match? Fall back to a 100% EXACT-dimensions match. Items that reuse
            // another item's model (Addictol↔Jet, Buffout↔Bufftats) share identical bounds, so they
            // inherit that item's hand-tuned offset instead of floating via geometry placement.
            // Strictly exact dims — no fuzzy/similar matching.
            if (!customOffset.has_value()) {
                customOffset = offsetMgr.GetExactDimensionsOffset(selRefr, isLeft);
            }
        } else {
            // Natural / telekinesis grab keeps the object exactly where grabbed (no position
            // snap), but the user still wants the item's SAVED finger-curl pose rather than a
            // worse in-place geometry wrap. Pull in ONLY the offset's finger/joint curls so
            // HasStoredFingerCurls + ApplyStoredFingerCurls use them; leave position/rotation
            // and hasItemOffset untouched so placement below still preserves the natural pose.
            auto teleOffset = offsetMgr.GetExactOffset(selRefr, isLeft);
            if (teleOffset.has_value() && (teleOffset->hasFingerCurls || teleOffset->hasJointCurls)) {
                state.itemOffset.hasFingerCurls = teleOffset->hasFingerCurls;
                state.itemOffset.thumbCurl  = teleOffset->thumbCurl;
                state.itemOffset.indexCurl  = teleOffset->indexCurl;
                state.itemOffset.middleCurl = teleOffset->middleCurl;
                state.itemOffset.ringCurl   = teleOffset->ringCurl;
                state.itemOffset.pinkyCurl  = teleOffset->pinkyCurl;
                state.itemOffset.hasJointCurls = teleOffset->hasJointCurls;
                for (int ji = 0; ji < 15; ++ji) state.itemOffset.jointCurls[ji] = teleOffset->jointCurls[ji];
                spdlog::debug("[GRAB] Natural grab '{}': using SAVED offset finger curls (object kept in place)", itemName);
            }
        }
        spdlog::debug("[GRAB] '{}' at {:.1f}cm: mode={} exactMatch={}",
                    itemName, distToObject,
                    isNaturalGrab ? "natural" : (isPalmSnap ? "palm-snap/geometry" : "other"),
                    customOffset.has_value());
        
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
                
                // F4VR row-vector convention: local.rotate = world.rotate * parent.rotate.Transpose()
                state.itemOffset.rotation = worldTransform.rotate * wandNode->world.rotate.Transpose();

                state.hasItemOffset = false;
                state.usedSnapMode = true;  // Used snap positioning - will open hand fully
            }
            else
            {
                // NATURAL GRAB: Object is close (<50cm) or snap disabled
                // Calculate offset that PRESERVES object's current world position.
                // F4VR row-vector convention:
                //   local.pos    = parent.rotate * (world - parent.translate)
                //   local.rotate = world.rotate * parent.rotate.Transpose()
                RE::NiPoint3 worldOffset = objectPos - wandNode->world.translate;
                state.itemOffset.position = wandNode->world.rotate * worldOffset;
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
                     grabHandPos.x, grabHandPos.y, grabHandPos.z);
        
        // distToObject already calculated above when determining snap mode

        // Apply grab mode decided above. Keep this flag alive through the
        // placement/finger stages below: a successful touch capture owns the
        // exact no-snap hand relation and must not be erased by the generic
        // saved-offset fallback later in this function.
        bool touchGrabApplied = false;
        if (useTelekinesis)
        {
            // Telekinesis: object follows hand from its current world position
            // No pull, no snap — object maintains its world-space offset from hand
            // Ignore any stored item offsets — always compute fresh from world positions
            state.isPulling = false;
            state.grabOffsetLocal = objectPos - grabHandPos;
            RE::NiMatrix3 placementRot = physicalTouchHandNode
                ? physicalTouchHandNode->world.rotate
                : (wandNode ? wandNode->world.rotate : grabHandRot);
            // Preserve the saved per-item finger curls the block above (the `else` of
            // `!useTelekinesis`, just before this) may have just populated - the
            // documented contract ("saved per-item finger curls ALWAYS win", consumed via
            // HasStoredFingerCurls) requires them to survive this reset. A full
            // `state.itemOffset = ItemOffset()` here wiped them every time, silently
            // making that block dead code for every natural/telekinesis grab.
            const bool preservedHasFingerCurls = state.itemOffset.hasFingerCurls;
            const bool preservedHasJointCurls = state.itemOffset.hasJointCurls;
            const ItemOffset preservedCurls = state.itemOffset;
            // Compute hand-local offset that preserves the object's current world position.
            // F4VR row-vector convention (see project_f4vr_matrix_convention).
            state.itemOffset = ItemOffset();  // Clear any stored offset completely
            if (preservedHasFingerCurls) {
                state.itemOffset.hasFingerCurls = true;
                state.itemOffset.thumbCurl = preservedCurls.thumbCurl;
                state.itemOffset.indexCurl = preservedCurls.indexCurl;
                state.itemOffset.middleCurl = preservedCurls.middleCurl;
                state.itemOffset.ringCurl = preservedCurls.ringCurl;
                state.itemOffset.pinkyCurl = preservedCurls.pinkyCurl;
            }
            if (preservedHasJointCurls) {
                state.itemOffset.hasJointCurls = true;
                for (int ji = 0; ji < 15; ++ji) state.itemOffset.jointCurls[ji] = preservedCurls.jointCurls[ji];
            }
            state.itemOffset.position = placementRot * state.grabOffsetLocal;
            state.itemOffset.rotation = worldTransform.rotate * placementRot.Transpose();
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
            // CROSS-HAND GUARD (Jul 19 audit fix, v2): refuse PULLING an object the other
            // hand still holds — two drives on one body wedge it into the player capsule
            // and launch the character (tester "flying weapon"). Guard lives HERE, not
            // before the transfer branch: v1 sat above it and killed hand-to-hand transfer
            // and two-handed co-hold outright (audit finding). Close grabs never reach this
            // point with the other hand still active — the transfer/co-hold branch already
            // resolved them.
            if (otherState.active && selRefr && otherState.GetRefr() == selRefr) {
                spdlog::info("[GRAB] {} hand PULL refused - other hand already holds this object",
                             isLeft ? "Left" : "Right");
                return false;
            }
            // Pull-to-hand: animate object from current world position toward palm snap target
            state.isPulling = true;
            state.pullProgress = 0.0f;
            HeisenbergPluginAPI::InvokePulledCallbacks(isLeft, selRefr);
            state.grabOffsetLocal = objectPos - grabHandPos;
            float pullDistance = (objectPos - grabHandPos).Length();
            spdlog::debug("[GRAB] StartGrab: PULLING object from {:.1f}cm (center-to-hand) selDist={:.1f}",
                        pullDistance, selection.distance);
        }
        else
        {
            // Close grab or palm snap.
            state.isPulling = false;

            // TOUCH GRAB (2026-07-23): when the hand is in actual contact with the object
            // (not just "close" per naturalGrabThreshold), skip the stored/authored item
            // offset entirely. Move the object exactly like telekinesis — preserve its
            // current world-space offset from the hand, no snap — and solve finger curl
            // against the object's real mesh (ROCK's own grab-finger solver) so fingers
            // stop just short of the surface instead of assuming a pre-authored pose.
            // Genuine contact is a direct grab, independent of the optional telekinesis
            // setting. Holotapes remain the one deliberate exception because their shared
            // scaled placement is part of the deck/inventory interaction.
            const bool touchGrabEligible = isActualTouchContact && !isHolotape;
            if (touchGrabEligible && state.node && wandNode)
            {
                std::array<float, 15> jointCurls{};
                // The native contact point is independent evidence that the
                // rendered mesh is actually under the hand. Keep it separate
                // from the finger-skeleton solve: calibration can be
                // temporarily unavailable even though the player is visibly
                // touching the object, and that must not turn a no-snap grab
                // into an authored-offset teleport. A broad collision shape
                // whose visible mesh is elsewhere (the Vault suit case) fails
                // this proximity check and still uses its authored offset.
                RE::NiAVObject* contactGeometryNode =
                    selection.isPhysicalTouch && state.physicsNode
                        ? state.physicsNode.get()
                        : state.node.get();
                bool contactNearVisibleMesh = false;
                float contactMeshDistance = -1.0f;
                if (contactGeometryNode) {
                    std::vector<TriangleData> contactTriangles;
                    contactTriangles.reserve(512);
                    GetTriangles(
                        contactGeometryNode,
                        contactTriangles,
                        4096);
                    RE::NiPoint3 closestVisiblePoint{};
                    if (GetClosestMeshPointToPoint(
                            contactTriangles,
                            selection.hasPhysicalTouchPoint
                                ? selection.hitPoint
                                : grabHandPos,
                            closestVisiblePoint,
                            contactMeshDistance)) {
                        // A native manifold point should land essentially on
                        // the graphics mesh. Older F4VR events sometimes carry
                        // only a body id; in that case allow the distance from
                        // the rendered wrist to the reachable finger envelope.
                        // The latter remains small enough that broad collision
                        // on clothing cannot masquerade as visible contact.
                        const float
                            visibleContactToleranceGameUnits =
                                selection.hasPhysicalTouchPoint
                                    ? 5.0f
                                    : 14.0f;
                        contactNearVisibleMesh =
                            contactMeshDistance <=
                            visibleContactToleranceGameUnits;
                    }
                }
                const bool solvedFingerPose =
                    rock::touch_grab_bridge::SolveTouchGrabFingerPose(
                        contactGeometryNode,
                        physicalTouchHandNode ? physicalTouchHandNode->world : wandNode->world,
                        isLeft,
                        grabHandPos,
                        selection.hasPhysicalTouchPoint
                            ? selection.hitPoint
                            : grabHandPos,
                        jointCurls);
                const bool preservePhysicalTouch =
                    solvedFingerPose || contactNearVisibleMesh;

                // Preserve item dimensions (just populated above via
                // GetItemDimensions) and fingerDistance across the full
                // itemOffset reset below - otherwise the calibration UI
                // (ItemPositionConfigMode) shows 0x0x0 dimensions for
                // touch-grabbed items, and any offset saved from that
                // state gets disqualified by FindSimilarOffset's dimension
                // check. Crucially, preserve the no-snap placement ONLY when
                // at least one finger has a trustworthy visible-mesh stop. If
                // no finger can reach any visible mesh (Vault 111 jumpsuit),
                // the configured authored offset is the correct fallback.
                if (preservePhysicalTouch) {
                    const ItemOffset priorOffset = state.itemOffset;
                    const RE::NiTransform& placementParent =
                        physicalTouchHandNode ? physicalTouchHandNode->world : wandNode->world;
                    const RE::NiPoint3 worldDelta =
                        objectPos - placementParent.translate;
                    const RE::NiPoint3 localOffset =
                        placementParent.rotate * worldDelta;
                    const RE::NiMatrix3 localRotation =
                        worldTransform.rotate * placementParent.rotate.Transpose();

                    state.itemOffset = ItemOffset();
                    state.itemOffset.length = priorOffset.length;
                    state.itemOffset.width = priorOffset.width;
                    state.itemOffset.height = priorOffset.height;
                    state.itemOffset.fingerDistance = priorOffset.fingerDistance;
                    if (solvedFingerPose) {
                        state.SetRuntimeJointCurls(jointCurls);
                    }
                    state.itemOffset.position = localOffset;
                    state.itemOffset.rotation = localRotation;
                    state.grabOffsetLocal = localOffset;
                    state.SetRuntimeHandPlacement(
                        localOffset,
                        localRotation,
                        physicalTouchHandNode != nullptr);
                    state.hasItemOffset = false;
                    state.isFRIKOffset = false;
                    state.isNaturalGrab = true;
                    touchGrabApplied = true;
                    spdlog::debug(
                        "[GRAB] StartGrab: TOUCH GRAB — no snap "
                        "(dist={:.1f}cm frame={} fingerSolved={} "
                        "contactMeshDist={:.2f})",
                        distToObject,
                        physicalTouchHandNode ? "skinned" : "wand",
                        solvedFingerPose,
                        contactMeshDistance);
                } else {
                    state.ClearRuntimeHandPlacement();
                    spdlog::info(
                        "[GRAB] StartGrab: TOUCH mesh solve found no reachable "
                        "visible surface — using authored offset for '{}'",
                        itemName);
                }
            }

            if (!touchGrabApplied)
            {
                // Fallback: stored/authored item offset (existing palm-snap behavior) for
                // grabs that are close but not in genuine mesh contact.
                state.grabOffsetLocal = state.itemOffset.position;
                spdlog::debug("[GRAB] StartGrab: using item offset directly (dist={:.1f}cm)", distToObject);
            }
        }

        // Geometry-based placement runs as a fallback when the item has no
        // saved offset at all. ANY saved-offset match (FormID, name, editor-ID
        // partial, NOTE form-type default, or dims-match — Priority 1-5) wins
        // over geometry, because geometry's surface-snap regularly produces
        // poses that don't match the user's saved hand-pose for the item
        // (e.g. holotape: saved __NOTE_DEFAULT_L vs. geom override several cm
        // off; vase: saved dims-match pose vs. geom snapping to far surface).
        // bEnableAutomaticHandPlacement only opts in geometry for items the
        // user hasn't customised at all.
        // REMOTE-GRAB GATE (restored Jun-4 fix, lost in the branch swap): only CLOSE grabs may
        // extract meshes. For a far grab the palm ray hits the object's surface at arm's reach,
        // and the snap then shifts the object so that distant surface point lands on the palm —
        // a multi-meter teleport (and the success path below cancels the pull animation too).
        // Remote grabs use the stored/dims item offset and pull in normally.
        const bool useGeometryPlacement =
            !state.isTelekinesis &&
            !state.isNaturalGrab &&
            !isRemoteSelection &&
            !state.hasItemOffset &&
            g_config.enableAutomaticHandPlacement;

        if (useGeometryPlacement) {
            state.isFRIKOffset = false;

            if (wandNode && TryCalculateRuntimeHandPlacementFromGeometry(state, wandNode, isLeft)) {
                RE::NiMatrix3 effectiveRotation;
                GetEffectiveGrabPlacement(state, state.grabOffsetLocal, effectiveRotation);
                if (state.usingKeyframedMode && state.isPulling) {
                    state.isPulling = false;
                    state.pullProgress = 1.0f;
                    spdlog::debug(
                        "[GRAB-POS] Geometry fallback resolved '{}' directly into hand - skipping keyframed pull",
                        itemName);
                }
                spdlog::debug(
                    "[GRAB-POS] Geometry placement for '{}': localPos=({:.2f}, {:.2f}, {:.2f})",
                    itemName,
                    state.grabOffsetLocal.x,
                    state.grabOffsetLocal.y,
                    state.grabOffsetLocal.z);
            } else {
                state.ClearRuntimeHandPlacement();
                state.grabOffsetLocal = state.itemOffset.position;
                spdlog::warn(
                    "[GRAB-POS] Geometry placement failed for '{}' - using default item offset ({:.2f}, {:.2f}, {:.2f})",
                    itemName,
                    state.grabOffsetLocal.x,
                    state.grabOffsetLocal.y,
                    state.grabOffsetLocal.z);
            }
        } else if (!touchGrabApplied) {
            state.ClearRuntimeHandPlacement();
            spdlog::debug("[GRAB-POS] Using saved exact offset for '{}' (no geometry fallback)", itemName);
        } else {
            spdlog::debug(
                "[GRAB-POS] Preserving physical-touch placement for '{}' "
                "(body={} age={}f)",
                itemName,
                selection.physicalTouchBodyId,
                selection.physicalTouchAgeFrames);
        }

        // The placement rules above intentionally decide WHERE the item should
        // sit. Geometry/touch poses can be rebased immediately. A complete
        // authored pose must wait until its saved fingers have reached their
        // target, otherwise this captures a transient FRIK wrist/controller
        // relation and gives the same saved offset a different visible seat on
        // each grip.
        const RE::NiPoint3 placementFallbackPos =
            wandNode ? wandNode->world.translate : handPos;
        const RE::NiMatrix3 placementFallbackRot =
            wandNode ? wandNode->world.rotate : handRot;
        if (UsesAuthoredGripPose(state)) {
            spdlog::info(
                "[GRAB-POSE] {} '{}' selected paired authored transform/curls; "
                "rendered-hand rebase deferred until pull and finger pose settle",
                isLeft ? "L" : "R",
                state.node ? state.node->name.c_str() : "NULL");
        } else {
            RebaseGrabPlacementToRenderedHand(
                state,
                isLeft,
                placementFallbackPos,
                placementFallbackRot);
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
            if (heisenberg::IsHolotapeNote(baseObj) && state.node) {
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
        
        // Apply finger curls: saved curls from an exact per-item offset are
        // authoritative. Geometry is only a fallback when none are stored.
        //
        // NATURAL GRAB POSING: natural grabs run in telekinesis mode (object held where
        // grabbed), and this block used to be skipped for telekinesis, leaving the hand
        // wide open. For a CLOSE natural grab the object is right in the hand, so we DO
        // want the fingers to wrap around its mesh. Allow the curl pass for natural grabs
        // (force geometry curls so they wrap the actual shape, not a saved/open pose).
        const bool naturalPosing =
            touchGrabApplied ||
            (state.isTelekinesis && isNaturalGrab);
        state.naturalFingerPosing = naturalPosing;  // persists so per-frame reapply keeps the wrap
        if ((!state.isTelekinesis || naturalPosing) && state.node)
        {
            // Saved per-item finger curls win for SNAP/OFFSET grabs — there the object is
            // seated in exactly the pose the curls were authored against. For a NATURAL
            // (touch) grab the object stays wherever it was touched, at an arbitrary pose
            // relative to the hand, so those same authored curls no longer line up with the
            // mesh — a near-fist saved pose curls the fingers straight through the object
            // (user-confirmed with Radroach Meat). Natural grabs therefore solve curls from
            // the actual geometry first and keep saved curls only as a fallback (see
            // ResolvePendingFingerCurls, which honors naturalFingerPosing the same way).
            const bool hasStoredCurls = HasStoredFingerCurls(state);
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.IsAvailable()) {
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                RE::NiNode* wandNode = playerNodes ? heisenberg::GetWandNode(playerNodes, isLeft) : nullptr;

                if (state.isPulling) {
                    frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                    state.pendingFingerCurls = true;  // resolved to stored/geometry on arrival
                    state.ClearRuntimeFingerCurls();
                    spdlog::debug("[GRAB-FINGERS] Deferred curls (stored first, geometry fallback) until object reaches the hand");
                } else if (!naturalPosing && hasStoredCurls && ApplyStoredFingerCurls(state, isLeft)) {
                    spdlog::debug("[GRAB-FINGERS] Applied saved offset curls for '{}'", itemName);
                } else if (naturalPosing &&
                           state.hasRuntimeJointCurls &&
                           ApplyRuntimeFingerCurls(state, isLeft)) {
                    state.pendingFingerCurls = false;
                    spdlog::debug(
                        "[GRAB-FINGERS] Applied stable first-contact joint pose for '{}'",
                        itemName);
                } else if (wandNode && (!state.isTelekinesis || state.naturalFingerPosing)) {
                    // POST-SNAP SOLVE (Jul 19 finger audit): solving here reads the object's
                    // PRE-snap resting pose — GEOM-PLACE then seats it at a DIFFERENT anchor
                    // (palm-ray + pre-shift + authored rotation) that the translate-only
                    // palmToPoint compensation cannot represent, so curls were solved against
                    // a phantom pose and misses slammed fingers to near-fist through the mesh.
                    // Defer to ResolvePendingFingerCurls at the post-physics sites, which run
                    // AFTER UpdateKeyframedNode has seated the object at its final placement —
                    // the solve then sees the TRUE held pose (the pulled-grab path always
                    // worked this way, and never showed the penetration).
                    frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                    state.pendingFingerCurls = true;
                    // Keep an already-solved touch pose. Only generated/snap
                    // placements need a post-physics solve against their final
                    // transformed mesh.
                    if (!state.hasRuntimeJointCurls) {
                        state.ClearRuntimeFingerCurls();
                    }
                    spdlog::debug("[GRAB-FINGERS] Deferred geometry curls to post-snap solve for '{}'", itemName);
                } else if (wandNode && TryCalculateRuntimeFingerCurlsFromGeometry(state, wandNode, isLeft)) {
                    // telekinesis holds without natural posing: post-physics resolve never
                    // runs for them, and the object is never snapped — solve now as before
                    ApplyRuntimeFingerCurls(state, isLeft);
                    spdlog::debug("[GRAB-FINGERS] Immediate geometry curls (telekinesis) for '{}'", itemName);
                } else {
                    state.ClearRuntimeFingerCurls();
                    frik.ClearHandPoseFingerPositions(isLeft);
                    spdlog::debug("[GRAB-FINGERS] No stored curls and geometry failed for '{}' - hand open", itemName);
                }
            }
        }
        
        state.usingMouseSpring = false;  // Not using mouse springs
        state.active = true;

        // ROCK coexistence: while ROCK owns hand collision/push, claim the object
        // we're holding so ROCK doesn't also grab it, AND disable ROCK's hand
        // collision rig on this hand so its capsules don't shove our dynamic held
        // object (that was the cause of held objects flying around). This also
        // realises "ROCK hand collision only when NOT grabbing". Restored in EndGrab.
        // No-op when ROCK is absent/inactive.
        if (RockBridge::GetSingleton().IsActive()) {
            if (selRefr) {
                RockBridge::GetSingleton().ClaimObject(selRefr);
            }
            // Keyframed holds never suspend ROCK's hand rig: the held object is
            // infinite-mass and unaffected by the hand capsules, and re-enabling
            // ROCK's hand on release would fling the just-released object.
        }

        // EMBEDDED ROCK engine hand-collision suppression is driven LEVEL-triggered from
        // PostPhysicsGrabUpdate (pushes grabState.active every frame), NOT edge-notified here
        // — that way every teardown path (normal release, mid-hold abort, world change)
        // clears it automatically and the engine re-asserts through mid-hold collider
        // rebuilds (audit rank 2). See PostPhysicsGrabUpdate.

        // Fire public API callback
        HeisenbergPluginAPI::InvokeGrabbedCallbacks(isLeft, selRefr);

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

        // Two-handed secondary aim hand is a marker only — no target/storage/consume work.
        // Still validate the ref though: if the co-held object is deleted (cell reset/
        // despawn/script) the PRIMARY hand's own invalid-refr abort below only clears
        // `state` (its own slot) — nothing clears THIS marker, so it stays active=true
        // forever and rock::HostNotifyExternalGrab keeps reporting a grab on this hand,
        // leasing its collider suite (can't push/collide) until the user happens to
        // press-and-release grip or start a new grab on this hand.
        if (state.coHeldSecondary) {
            if (!state.HasValidRefr()) {
                spdlog::debug("[GRAB] UpdateGrab: {} hand co-held secondary marker reference invalid - clearing", isLeft ? "Left" : "Right");
                state.Clear();
                Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            }
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
            // Every other teardown path in this file also notifies ItemPositionConfigMode
            // (it nulls _currentGrabState there) - this one didn't, so reposition mode kept
            // a pointer to the now-Clear()ed GrabState after an invalid-refr abort, and
            // thumbstick adjustment writes kept landing on the dead state under a stale
            // item name instead of resetting.
            heisenberg::ItemPositionConfigMode::GetSingleton().OnGrabEnded(isLeft);
            return;
        }

        RE::TESObjectREFR* refr = state.GetRefr();

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

        // Item-position editing freezes the object's world pose and writes a
        // new legacy itemOffset relative to the controller.  Do not let the
        // acquisition-time rendered-hand placement mask those edits.  Once
        // editing ends, rebase the newly saved offset into the rendered hand
        // without moving the object, restoring exact hand/item synchrony for
        // the remainder of the grab.
        if (configMode.IsRepositionModeActive()) {
            if (configMode.HasFrozenPosition() &&
                state.hasRigidRenderedHandPlacement) {
                state.ClearRigidRenderedHandPlacement();
            }
        } else if (!state.hasRigidRenderedHandPlacement) {
            const bool authoredPose =
                UsesAuthoredGripPose(state);
            const auto& frik =
                FRIKInterface::GetSingleton();
            const bool authoredFingerPoseSettled =
                !state.pendingFingerCurls &&
                (!frik.IsAvailable() ||
                 Heisenberg::GetSingleton()
                         .GetFingerAnimator(isLeft)
                         .GetState() ==
                     FingerAnimator::State::Holding);

            if (grab_pose_policy::ShouldCommitRenderedHandRebase(
                    authoredPose,
                    state.isPulling,
                    authoredFingerPoseSettled) &&
                RebaseGrabPlacementToRenderedHand(
                    state,
                    isLeft,
                    handPos,
                    handRot) &&
                authoredPose) {
                spdlog::info(
                    "[GRAB-POSE] {} '{}' committed settled authored "
                    "transform/curls to rendered-hand frame",
                    isLeft ? "L" : "R",
                    state.node ? state.node->name.c_str() : "NULL");
            }
        }

        float storageHoldTime = heisenberg::g_config.storageZoneHoldTime;
        // Clear skipStorageZone after 1 second (allows storage after initial Pipboy proximity)
        if (state.skipStorageZone) {
            float elapsed = static_cast<float>(Utils::GetTime()) - state.grabStartTime;
            if (elapsed >= 1.0f) {
                state.skipStorageZone = false;
                spdlog::debug("[STORAGE] Cleared skipStorageZone after {:.1f}s", elapsed);
            }
        }
        if (!configMode.IsRepositionModeActive() && !state.isPulling && !state.skipStorageZone)
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
                // Perk magazines / comics (kBOOK) are NOT collected by the behind-head storage
                // zone: ANY inventory-add grants the perk + pops the vanilla "collected an issue"
                // splash, which the user wants ONLY on the deliberate bring-to-face read. So a
                // magazine held in the storage zone is left in hand — read it at your face to
                // collect it. One soft haptic so the no-op isn't silent.
                bool storeBlockedBook = false;
                if (state.behindEarTimer >= storageHoldTime)
                {
                    auto* storeBase = refr ? refr->GetObjectReference() : nullptr;
                    if (storeBase && storeBase->GetFormType() == RE::ENUM_FORM_ID::kBOOK)
                    {
                        storeBlockedBook = true;
                        if (!state.bookStoreBlockedHinted)
                        {
                            state.bookStoreBlockedHinted = true;
                            g_vrInput.TriggerHaptic(isLeft, 15000);  // soft "can't stash — read at face"
                            spdlog::info("[STORAGE] Magazine/book {:08X} not stashed behind head — bring it to your face to read/collect",
                                         refr->formID);
                        }
                        state.behindEarTimer = 0.0f;  // don't keep re-trying every frame
                    }
                }
                if (state.behindEarTimer >= storageHoldTime && !storeBlockedBook)
                {
                    spdlog::debug("[STORAGE] {:.1f}s in zone - auto-storing item!", storageHoldTime);

                    bool showMessage = true;
                    bool stored = StoreGrabbedItem(refr, showMessage);
                    
                    if (stored)
                    {
                        spdlog::debug("[GRAB] Auto-storage succeeded after {:.1f}s", storageHoldTime);
                        RE::TESForm* baseForm = refr ? refr->GetObjectReference() : nullptr;
                        HeisenbergPluginAPI::InvokeStashedCallbacks(isLeft, baseForm);
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

            // ── NOTE FACE-READ ──────────────────────────────────────────────
            // Hold a paper note up to your FACE to read it (opens the note overlay).
            // Storing it behind your head stays silent (auto-store above uses
            // AddObjectToContainer, never ActivateRef). Reading takes the note to
            // inventory, matching the vanilla "read note" behavior. Holotapes are
            // excluded — they use the Pip-Boy tape-deck flow. The 1s skipStorageZone
            // grace after grab also gates this whole block, so a note grabbed at eye
            // level isn't read instantly.
            if (refr)
            {
                if (auto* noteBase = refr->GetObjectReference())
                {
                    auto* note = noteBase->As<RE::BGSNote>();
                    const bool isPaperNote = note &&
                        note->type != RE::BGSNote::NOTE_TYPE::kVoice &&
                        note->type != RE::BGSNote::NOTE_TYPE::kProgram &&
                        note->type != RE::BGSNote::NOTE_TYPE::kTerminal;
                    // Perk magazines / comics (Grognak, Tesla Science, etc.) are kBOOK.
                    // Reading one at the face fires the vanilla "read" — grants the perk
                    // and pops the "You've collected an issue" splash. Storing it behind
                    // the head goes through AddObjectToContainer (silent), so the splash
                    // only ever appears on this deliberate bring-to-face gesture.
                    const bool isReadableBook = noteBase->GetFormType() == RE::ENUM_FORM_ID::kBOOK;
                    const bool isReadable = isPaperNote || isReadableBook;
                    auto* pn = isReadable ? f4cf::f4vr::getPlayerNodes() : nullptr;
                    if (pn && pn->HmdNode)
                    {
                        const RE::NiPoint3& hmdPos = pn->HmdNode->world.translate;
                        const RE::NiMatrix3& hmdRot = pn->HmdNode->world.rotate;
                        const RE::NiPoint3 toNote = handPos - hmdPos;
                        const float dist = toNote.Length();
                        // HMD forward = Y column. Require the note in FRONT of the face so
                        // the behind-head storage zone never counts as "brought to face".
                        const float inFront = toNote.x * hmdRot.entry[0][1] +
                                              toNote.y * hmdRot.entry[1][1] +
                                              toNote.z * hmdRot.entry[2][1];
                        // Paper notes must be brought RIGHT UP to the face (~14cm) — a deliberate
                        // gesture — so a note that just dropped to hand isn't read the instant you
                        // glance at it (the "notes go to the world on pickup" report). Magazines /
                        // comics are read at a comfortable ~28cm: you hold the comic up and look at
                        // it rather than mashing it into your nose. Reading a kBOOK grants its perk
                        // and pops the vanilla "collected an issue" splash — which is exactly the
                        // bring-to-face collect the user wants (and the ONLY place it should happen;
                        // the behind-head storage zone is gated off for books below).
                        const float readFaceDist = isReadableBook ? 20.0f : 10.0f;  // ~28cm / ~14cm
                        if (dist < readFaceDist && inFront > 0.0f)
                        {
                            RE::NiPointer<RE::TESObjectREFR> keep(refr);
                            EndGrab(isLeft, nullptr, true);  // release the hold first
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            if (player && keep)
                            {
                                // Bypass our activate hook — we WANT the native note overlay /
                                // magazine read here (the deliberate face gesture).
                                heisenberg::Hooks::SetInternalActivation(true);
                                keep->ActivateRef(player, nullptr, 1, false, false, false);
                                heisenberg::Hooks::SetInternalActivation(false);
                                // ActivateRef fires the read/collect but does NOT reliably remove
                                // the placed world reference for kNOTE/kBOOK (the exact reason
                                // StoreGrabbedItem uses AddObjectToContainer + SafeDisableRef).
                                // Without this the note/magazine survives as a frozen world ref at
                                // the last hand position → it floats. Disable it like every other
                                // storage path (SafeDisableRef defers for behavior-graph items).
                                SafeDisableRef(keep.get());
                            }
                            spdlog::info("[GRAB] {} read at face (dist={:.1f}cm) - collected/opened + taken",
                                         isReadableBook ? "Magazine" : "Note", dist);
                            return;  // grab is over
                        }
                    }
                }
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
            // Injectables (stimpak, RadAway, med-x, etc.) are wrist-injected, not
            // eaten — when hand injection is enabled they must NOT mouth-consume.
            // (Falls back to mouth if hand injection is disabled, so they're still usable.)
            const bool isInjectableItem = IsInjectable(refr);
            bool inMouthZone = IsInMouthZone(isLeft) &&
                               !(isInjectableItem && heisenberg::g_config.enableHandInjection);
            // Hand injection zone only activates for injectables (chems, stimpaks — not food/drinks)
            bool inHandInjectionZone = isInjectableItem && IsInHandInjectionZone(isLeft);
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
                    RE::TESForm* baseForm = refrToConsume ? refrToConsume->GetObjectReference() : nullptr;
                    HeisenbergPluginAPI::InvokeConsumedCallbacks(isLeft, baseForm);
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

                spdlog::info("[{}] '{}' grabbed — equip-zone detection active (weaponEquipMode={} vhEnabled={})",
                             itemType, itemName, heisenberg::g_config.weaponEquipMode, vhHolsteringEnabled);
                
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
                        // ARMOR: Check body zones (head + chest)
                        // All body/leg items use the chest zone — no separate legs zone.
                        // Check chest zone first (most common — covers body, legs, hands, etc.)
                        currentZone = CheckArmorEquipZone(handPos, state.handSpeed, ArmorZoneType::Chest);
                        if (currentZone != ArmorZoneType::None) {
                            zoneName = "CHEST";
                        }

                        // Check head/face zone (glasses, hats, helmets)
                        if (currentZone == ArmorZoneType::None) {
                            currentZone = CheckArmorEquipZone(handPos, state.handSpeed, ArmorZoneType::Head);
                            if (currentZone != ArmorZoneType::None) {
                                zoneName = "HEAD";
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
            if (playerPCh && playerPCh->GetWeaponMagicDrawn())
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
        // PHYSICS SETUP (runs once on first update after grab) — KEYFRAMED
        // (direct position control, object set to KEYFRAMED)
        // ═══════════════════════════════════════════════════════════════════════════
        if (!state.keyframedSetupComplete && state.node)
        {
            // Save original parent (we'll stay parented to it, not reparent to wand)
            state.originalParent.reset(state.node->parent);

            // ── KEYFRAMED MODE (original path) ──
            // KEYFRAMED = infinite mass, can't be pushed, but still collides with world
            if (!state.keyframedSetupComplete)
            {
                spdlog::debug("[GRAB-KEYFRAMED] Setting up KEYFRAMED physics mode");

                if (state.originalParent)
                {
                    spdlog::debug("[GRAB-KEYFRAMED] Original parent: '{}' at world ({:.1f},{:.1f},{:.1f})",
                        state.originalParent->name.c_str(),
                        state.originalParent->world.translate.x,
                        state.originalParent->world.translate.y,
                        state.originalParent->world.translate.z);
                }

                if (state.collisionObject && state.node)
                {
                    RE::bhkWorld* bhkWorld = GetBhkWorldFromRefr(refr);
                    state.savedState.savedBhkWorld = bhkWorld;
                    state.savedState.motionType = RE::hknpMotionPropertiesId::Preset::DYNAMIC;
                    state.savedState.collisionObjectFlags = state.collisionObject->flags.flags;
                    spdlog::debug("[GRAB-KEYFRAMED] Saved collision object flags: {:X}",
                                 state.savedState.collisionObjectFlags);

                    // Capture every body in the shared reference physics
                    // system before changing motion type. The selected visible
                    // wrapper remains placement authority, while hidden
                    // alternates are driven in lockstep so they cannot reset
                    // the reference root to its pre-grab pose.
                    CaptureHeldCollisionBodyFrames(state);
                    const bool selectedWrapperScope =
                        UsesSelectedCollisionWrapperMotionScope(state);
                    if (selectedWrapperScope) {
                        SetMotionTypeLocked(
                            state.collisionObject,
                            RE::hknpMotionPropertiesId::Preset::KEYFRAMED,
                            bhkWorld);
                    } else {
                        bhkWorld_SetMotionLocked(
                            state.node.get(),
                            RE::hknpMotionPropertiesId::Preset::KEYFRAMED,
                            true,
                            true,
                            true,
                            bhkWorld);
                    }
                    spdlog::debug(
                        "[GRAB-KEYFRAMED] Set motion type to KEYFRAMED "
                        "(scope={} bhkWorld={:X})",
                        selectedWrapperScope
                            ? "active collision wrapper"
                            : "reference subtree",
                        reinterpret_cast<uintptr_t>(bhkWorld));

                    // A visibly split child cannot become root authority while
                    // it is dynamic: Havok would immediately restore the stale
                    // pose. Apply the staged rebase only now, after the selected
                    // wrapper is keyframed, then move only its captured body
                    // frame onto the unchanged visible pose.
                    if (state.pendingVisibleRootRebase) {
                        const RE::NiTransform previousRootWorld =
                            state.node->world;
                        const RE::NiPoint3 visibleBefore =
                            state.physicsNode
                                ? state.physicsNode->world.translate
                                : state.pendingVisibleRootWorld.translate;
                        Utils::UpdateKeyframedNode(
                            state.node.get(),
                            state.pendingVisibleRootWorld);

                        bool physicsSynced =
                            state.capturedHeldBodyCount > 0 &&
                            SyncCapturedHeldBodyFrames(
                                state,
                                state.pendingVisibleRootWorld,
                                bhkWorld);
                        if (!physicsSynced &&
                            state.physicsNode &&
                            IsCollisionObjectValid(
                                state.collisionObject)) {
                            RE::hkTransformf activeBodyTransform{};
                            activeBodyTransform.rotation =
                                state.physicsNode->world.rotate;
                            activeBodyTransform.translation =
                                RE::NiPoint4(
                                    state.physicsNode->world.translate.x *
                                        HAVOK_WORLD_SCALE,
                                    state.physicsNode->world.translate.y *
                                        HAVOK_WORLD_SCALE,
                                    state.physicsNode->world.translate.z *
                                        HAVOK_WORLD_SCALE,
                                    0.0f);
                            physicsSynced = SetTransformLocked(
                                state.collisionObject,
                                activeBodyTransform,
                                bhkWorld);
                        }

                        if (physicsSynced) {
                            if (refr) {
                                refr->data.location.x =
                                    state.node->world.translate.x;
                                refr->data.location.y =
                                    state.node->world.translate.y;
                                refr->data.location.z =
                                    state.node->world.translate.z;
                            }
                            const float visibleMove =
                                state.physicsNode
                                    ? (state.physicsNode->world.translate -
                                       visibleBefore)
                                          .Length()
                                    : 0.0f;
                            spdlog::info(
                                "[GRAB-ROOT-AUTHORITY] Applied staged active-"
                                "variant rebase for {:08X}; root=({:.1f},"
                                "{:.1f},{:.1f}) visibleMove={:.3f}gu bodies={}",
                                refr ? refr->formID : 0,
                                state.node->world.translate.x,
                                state.node->world.translate.y,
                                state.node->world.translate.z,
                                visibleMove,
                                state.capturedHeldBodyCount);
                        } else {
                            Utils::UpdateKeyframedNode(
                                state.node.get(),
                                previousRootWorld);
                            spdlog::warn(
                                "[GRAB-ROOT-AUTHORITY] Cancelled staged rebase "
                                "for {:08X}: selected collision body could not "
                                "be synchronized",
                                refr ? refr->formID : 0);
                        }
                        state.pendingVisibleRootRebase = false;
                    }

                    if (heisenberg::g_config.heldObjectCollidable) {
                        // REVERTED (2026-05-29): Layer-43 experiment broke thrown-object damage
                        // because SetLayerLocked also overwrites the filter's group/system bits
                        // with layer-43 presets, and the saved "original filter info" captured
                        // AFTER that call carried the wrong metadata back into the restored body.
                        // Throws then landed but registered no contacts on NPCs.
                        //
                        // Original working approach: leave the body on its native layer (4
                        // clutter / whatever the refr already was) and rely ONLY on OR'ing
                        // the 0x000B hand-group bits into the filter info. The engine's pair
                        // filter rejects player-attached pairs by GROUP, not layer, so the
                        // player-push-back avoidance still works without a layer change.
                        if (TryDisablePlayerHeldObjectCollision(state)) {
                            state.heldPlayerFilterApplied = true;
                        }
                        spdlog::info("[GRAB-KEYFRAMED] {:08X} held object kept COLLIDABLE (native layer + 0x000B group bits) — will push other objects",
                                     refr ? refr->formID : 0);
                    } else {
                        CaptureHeldObjectLayerBeforeChange(state);  // save REAL pre-grab layer (audit rank 7)
                        bhkUtilFunctions_SetLayerLocked(state.node.get(), 15, bhkWorld);
                        spdlog::debug("[GRAB-KEYFRAMED] Set collision layer to kNonCollidable (15)");
                    }

                    if (Utils::HasPhysicsBodyOffset(state.collisionObject, state.node.get()))
                    {
                        RE::NiTransform bodyOffset = Utils::GetPhysicsBodyOffset(state.collisionObject, state.node.get());
                        spdlog::debug("[GRAB-KEYFRAMED] Detected physics body offset: pos=({:.2f},{:.2f},{:.2f})",
                                    bodyOffset.translate.x, bodyOffset.translate.y, bodyOffset.translate.z);
                    }

                    state.savedState.wasDeactivated = false;
                }
                else
                {
                    // SMOKING GUN for "held objects don't push others": if the grabbed item has no
                    // collision body, the per-frame SetTransform+ApplyHardKeyframe drive is skipped
                    // (guarded by `if (state.collisionObject)`), so it can't shove other clutter.
                    spdlog::info("[GRAB-KEYFRAMED] {:08X} has NO collision body (collisionObject={} node={}) — held object CANNOT push other objects (item-item collision needs a physics body)",
                                 refr ? refr->formID : 0,
                                 reinterpret_cast<void*>(state.collisionObject),
                                 reinterpret_cast<void*>(state.node.get()));
                }

                state.keyframedSetupComplete = true;
                spdlog::debug("[GRAB-KEYFRAMED] Setup complete: will update transforms each frame");
            }
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

                // POST-PULL POSITION DIAGNOSTIC — how far is the object from the
                // palm once the pull-in animation has settled? Answers "does the
                // object actually end up at the hand?" Log palm, object pivot,
                // and absolute distance between them.
                {
                    auto* playerNodesPP = f4cf::f4vr::getPlayerNodes();
                    RE::NiNode* wandNodePP = playerNodesPP ? heisenberg::GetWandNode(playerNodesPP, isLeft) : nullptr;
                    // Skip the triangle extraction for saved-curl items (geometry irrelevant there).
                    if (wandNodePP && state.node && !HasStoredFingerCurls(state)) {
                        RE::NiPoint3 palmPosPP = GetPalmPosition(wandNodePP, isLeft);
                        RE::NiPoint3 objPosPP = state.node->world.translate;
                        float pivotDist = (objPosPP - palmPosPP).Length();
                        std::vector<heisenberg::TriangleData> trisPP;
                        trisPP.reserve(256);
                        heisenberg::GetTriangles(state.node.get(), trisPP);
                        RE::NiPoint3 meshPt;
                        float meshDist = -1.0f;
                        if (!trisPP.empty()) {
                            heisenberg::GetClosestMeshPointToPoint(trisPP, palmPosPP, meshPt, meshDist);
                        }
                        spdlog::warn("[POST-PULL] '{}' palm=({:.1f},{:.1f},{:.1f}) pivot=({:.1f},{:.1f},{:.1f}) pivotDist={:.2f}cm meshDist={:.2f}cm meshPt=({:.1f},{:.1f},{:.1f}) tris={}",
                                     state.node->name.c_str(),
                                     palmPosPP.x, palmPosPP.y, palmPosPP.z,
                                     objPosPP.x, objPosPP.y, objPosPP.z,
                                     pivotDist, meshDist,
                                     meshPt.x, meshPt.y, meshPt.z,
                                     trisPP.size());
                    }
                }

            }
        }

        // Keep a collidable held object from shoving the player. The pair filter is applied
        // when the body ids resolve, then re-applied at a fixed cadence (NOT every frame —
        // throttle even on failure, otherwise an SEH-caught failure inside disableCollisionsBetween
        // would re-fire every frame and spam the log). Cadence-frame counter ticks regardless of
        // success/failure; we attempt at most one disable per cadence-tick.
        if (g_config.heldObjectCollidable && !state.isPulling && state.keyframedSetupComplete &&
            state.collisionObject)
        {
            const bool attempt = (state.heldPlayerFilterFrames++ % 30 == 0);
            if (attempt) {
                // Drop the cached player body so resolution re-walks the proxy-first chain.
                // Recovers if a previous success latched the wrong (ragdoll) body.
                heisenberg::Physics::InvalidatePlayerBodyId();
                if (TryDisablePlayerHeldObjectCollision(state)) {
                    state.heldPlayerFilterApplied = true;
                }
            }
        }

        // =====================================================================
        // PER-FRAME FINGER CURL RE-APPLICATION
        // FRIK's controller tracking maps trigger input to finger curl, which
        // overrides our SetHandPoseFingerPositions. Re-apply every frame to
        // keep the correct grab pose regardless of trigger state.
        // =====================================================================
        if (!state.isPulling && (!state.isTelekinesis || state.naturalFingerPosing) && !state.pendingFingerCurls &&
            !(configMode.IsRepositionModeActive() && state.stickyGrab))
        {
            // Reapply the acquisition-time pose, but do not resolve it again.
            // The held object-to-rendered-hand transform is rigid; repeated
            // mesh solves merely sample animation/physics at different phases
            // and were the source of changing curls and rest jitter.
            if (HasConfiguredFingerCurls(state)) {
                if (ApplyConfiguredFingerCurls(state, isLeft)) {
                    static int curlLogCount = 0;
                    if (curlLogCount++ < 5) {
                        spdlog::debug("[GRAB] Per-frame curl re-apply: hand={}", isLeft ? "L" : "R");
                    }
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
                RE::NiPoint3 localOffset;
                RE::NiMatrix3 localRotation;
                GetEffectiveGrabPlacement(state, localOffset, localRotation);
                RE::NiPoint3 capturePos;
                RE::NiMatrix3 captureRot;
                ResolveGrabParent(state, isLeft, handPos, handRot, capturePos, captureRot);
                RE::NiPoint3 rotatedOffset = captureRot.Transpose() * localOffset;
                RE::NiPoint3 currentWorldPos = capturePos + rotatedOffset;
                RE::NiMatrix3 currentWorldRot = localRotation * captureRot;
                // Store both item world position AND hand transform for relative offset calculation.
                // Freeze using the resolved parent (skinned hand / FRIK / wand) so reposition
                // stays consistent with how the offset is normally applied.
                configMode.SetFrozenWorldTransform(currentWorldPos, currentWorldRot, capturePos, captureRot);
            }
            
            // Use frozen world position - item stays still while adjustments are made
            targetPos = configMode.GetFrozenWorldPos();
            targetRot = configMode.GetFrozenWorldRot();
        } else {
            // Normal mode - calculate from hand/weapon position + offset.
            // ResolveGrabParent picks the skinned-hand/FRIK/wand parent that
            // matches how the stored localOffset was computed.
            RE::NiPoint3 parentPos;
            RE::NiMatrix3 parentRot;
            ResolveGrabParent(state, isLeft, handPos, handRot, parentPos, parentRot);

            // F4VR row-vector apply: world.rotate = local.rotate * parent.rotate
            RE::NiPoint3 localOffset;
            RE::NiMatrix3 localRotation;
            GetEffectiveGrabPlacement(state, localOffset, localRotation);
            RE::NiMatrix3 finalRot = localRotation * parentRot;

            // Power armor glove compensation — push objects outward so they aren't hidden
            if (Utils::IsPlayerInPowerArmor()) {
                localOffset.x += g_config.paGrabOffsetX;
                localOffset.y += g_config.paGrabOffsetY;
                localOffset.z += g_config.paGrabOffsetZ;
            }

            // F4VR row-vector apply: world.translate = parent.translate + parent.rotate.Transpose() * local.translate
            RE::NiPoint3 rotatedOffset = parentRot.Transpose() * localOffset;
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
        // Physics updates happen in PostPhysicsGrabUpdate() from the post-physics hook.
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
            // v2: baseline must use the SAME signal as the steady-state check (player's
            // parentCell, not the held object's — see the WORLD-CHANGE RESYNC comment).
            if (auto* worldPlayer2 = RE::PlayerCharacter::GetSingleton()) {
                if (auto* worldPlayerCell2 = worldPlayer2->GetParentCell()) {
                    state.lastSyncedBhkWorld = worldPlayerCell2->GetbhkWorld();
                }
            }
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
                const bool selectedWrapperScope =
                    UsesSelectedCollisionWrapperMotionScope(state);
                if (selectedWrapperScope) {
                    SetMotionTypeLocked(
                        state.collisionObject,
                        RE::hknpMotionPropertiesId::Preset::KEYFRAMED,
                        bhkWorld);
                } else {
                    bhkWorld_SetMotionLocked(
                        state.node.get(),
                        RE::hknpMotionPropertiesId::Preset::KEYFRAMED,
                        true,
                        true,
                        true,
                        bhkWorld);
                }
                spdlog::warn(
                    "[POST-PHYSICS] Re-applied KEYFRAMED "
                    "(lag={:.0f}cm scope={})",
                    lag,
                    selectedWrapperScope
                        ? "active collision wrapper"
                        : "reference subtree");
            }
        }

        static int postPhysDebug = 0;
        if (++postPhysDebug >= 10) {
            postPhysDebug = 0;
            spdlog::debug("[POST-PHYSICS] Object at ({:.1f},{:.1f},{:.1f}) target=({:.1f},{:.1f},{:.1f}) lag={:.2f}cm",
                objPos.x, objPos.y, objPos.z, targetPos.x, targetPos.y, targetPos.z, lag);
        }

        // Periodic held-object-to-palm distance log — surfaces "object is far
        // from the hand" cases independent of pull animation. Once per second
        // at 60 Hz. Only log while not pulling (so we see steady-state only).
        // Skip for saved-curl items — the mesh extraction it does is pure overhead there.
        if (!state.isPulling && !HasStoredFingerCurls(state)) {
            static int heldDistCounter = 0;
            if (++heldDistCounter >= 60) {
                heldDistCounter = 0;
                auto* playerNodesHD = f4cf::f4vr::getPlayerNodes();
                RE::NiNode* wandNodeHD = playerNodesHD ? heisenberg::GetWandNode(playerNodesHD, isLeft) : nullptr;
                if (wandNodeHD) {
                    RE::NiPoint3 palmPosHD = GetPalmPosition(wandNodeHD, isLeft);
                    float pivotDistHD = (objPos - palmPosHD).Length();
                    std::vector<heisenberg::TriangleData> trisHD;
                    trisHD.reserve(256);
                    heisenberg::GetTriangles(state.node.get(), trisHD);
                    RE::NiPoint3 meshPtHD;
                    float meshDistHD = -1.0f;
                    if (!trisHD.empty()) {
                        heisenberg::GetClosestMeshPointToPoint(trisHD, palmPosHD, meshPtHD, meshDistHD);
                    }
                    spdlog::warn("[HELD-DIST] '{}' palm=({:.1f},{:.1f},{:.1f}) pivot=({:.1f},{:.1f},{:.1f}) pivotDist={:.2f}cm meshDist={:.2f}cm tris={}",
                                 state.node->name.c_str(),
                                 palmPosHD.x, palmPosHD.y, palmPosHD.z,
                                 objPos.x, objPos.y, objPos.z,
                                 pivotDistHD, meshDistHD, trisHD.size());
                }
            }
        }
    }

    void GrabManager::EndGrab(bool isLeft, const RE::NiPoint3* throwVelocity, bool forStorage,
                              const RE::NiPoint3* throwAngularVelocity)
    {
        // Get state for this hand first
        GrabState& state = isLeft ? _leftGrab : _rightGrab;

        // Two-handed grab cleanup:
        // (a) If THIS hand is the secondary aim hand, it owns no physics — just clear the
        //     marker and return; the primary keeps holding (its next drive sees no partner).
        if (state.coHeldSecondary) {
            spdlog::info("[GRAB] Two-handed: {} aim hand released — primary keeps the object", isLeft ? "Left" : "Right");
            state.Clear();
            return;
        }
        // (b) If THIS (primary) hand is releasing while the OTHER hand is its secondary aim hand,
        //     clear that marker too so it doesn't linger pointing at a dropped object.
        {
            GrabState& partner = isLeft ? _rightGrab : _leftGrab;
            if (partner.active && partner.coHeldSecondary && partner.GetRefr() == state.GetRefr()) {
                spdlog::info("[GRAB] Two-handed: primary released — clearing {} aim-hand marker", isLeft ? "Right" : "Left");
                partner.Clear();
            }
        }

        if (!state.active)
            return;
        
        // CRITICAL: Validate reference via handle lookup BEFORE any method calls!
        // This prevents crashes when the game deletes objects we're holding.
        bool refrValid = state.HasValidRefr();

        // ROCK coexistence: release any claim placed at grab start so ROCK may
        // own the object again, and re-enable ROCK's hand collision rig on this
        // hand (it was disabled during the grab). No-op when ROCK inactive.
        if (RockBridge::GetSingleton().IsActive()) {
            if (refrValid) {
                if (auto* heldRefr = state.GetRefr()) {
                    RockBridge::GetSingleton().ReleaseObject(heldRefr);
                }
            }
            RockBridge::GetSingleton().EnablePhysicsHand(isLeft);
        }

        // EMBEDDED ROCK engine hand-collision release is handled LEVEL-triggered in
        // PostPhysicsGrabUpdate: once this EndGrab clears state.active, the next frame's push
        // sends active=false, and the engine arms its config-delayed collider restore
        // (rockGrabReleaseHandCollisionDelaySeconds) so the just-released, now-dynamic object
        // is never shoved by the still-armed hand suite (audit rank 2).

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
        
        RE::TESObjectREFR* refr = state.GetRefr();

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
                    spdlog::info("[WEAPON] Release — isInEquipZone={} zone='{}' weaponEquipMode={} vhSlot={}",
                                state.isInEquipZone, state.currentZoneName,
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
                        RE::TESObjectWEAP* weapon = playerPCh && playerPCh->GetWeaponMagicDrawn()
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
                    // Gated on its OWN MCM toggle — this shared block is entered via
                    // bEnableDropToCompanion, but the MCM exposes a separate
                    // "Drop To Container" switcher that was previously wired to nothing
                    // (review-confirmed dead entry), so container drops ignored it.
                    else if (!containerRef && g_config.enableDropToContainer) {
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
                // Use the current itemOffset which was calculated during grab.
                // If automatic hand placement is active, save the live runtime placement instead.
                ItemOffset offsetToSave = state.itemOffset;
                if (g_config.enableAutomaticHandPlacement && state.hasRuntimeHandPlacement && !state.isTelekinesis)
                {
                    offsetToSave.position = state.runtimeHandPlacementPosition;
                    offsetToSave.rotation = state.runtimeHandPlacementRotation;
                }
                
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

        // Fire dropped callback for normal releases (not stash/consume)
        if (!forStorage && refr) {
            HeisenbergPluginAPI::InvokeDroppedCallbacks(isLeft, refr);
            // EndGrabKeyframed restores this original body and its native
            // collision below. Suppress only ROCK's additional push impulse
            // briefly, so a quiet ammo-box release cannot be interpreted as a
            // fresh high-speed hand push and launched across the room.
            rock::HostNotifyExternalRelease(isLeft, refr);
        }

        // ── KEYFRAMED MODE RELEASE ──
        // Restores motion type to DYNAMIC and applies throw velocity
        if (state.node)
        {
            EndGrabKeyframed(state, throwVelocity, isLeft, throwAngularVelocity);
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
                
                RE::NiTransform targetTransform;
                ComputeHeldGrabTargetTransform(state, isLeft, handPos, handRot, targetTransform);
                RE::NiPoint3 targetPos = targetTransform.translate;
                RE::NiMatrix3 targetRot = targetTransform.rotate;
                
                // Disable pull animation - item goes directly to hand
                state.isPulling = false;
                state.pullProgress = 1.0f;
                RE::NiMatrix3 effectiveRotation;
                GetEffectiveGrabPlacement(state, state.grabOffsetLocal, effectiveRotation);
                state.initialObjectPos = targetPos;

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

                    auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                    RE::NiNode* wandNode = playerNodes ? heisenberg::GetWandNode(playerNodes, isLeft) : nullptr;
                    ResolvePendingFingerCurls(state, wandNode, isLeft, "Instant grab");
                    
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
    

    void GrabManager::PostPhysicsGrabUpdate()
    {
        // [REL-DIAG v2] complete any pending pair-cache pokes (see EndGrab).
        {
            void* tickWorld = nullptr;
            if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->parentCell) {
                if (auto* bhk = player->parentCell->GetbhkWorld()) {
                    tickWorld = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
                }
            }
            if (tickWorld) { TickDeferredFilterRestores(tickWorld); }
        }

        // EMBEDDED ROCK engine (audit rank 2): publish the per-hand grab state LEVEL-triggered
        // every frame. The embedded engine leases the grabbing hand's collider suite + forearm
        // chain while held (so the hand capsules never shove the held object) and restores them
        // via ROCK's config-delayed window on release. Driving from the raw grabState.active
        // makes every teardown path (normal, mid-hold abort, world change) clear it for free.
        // Cheap atomic stores; runs before the menu/early-return logic below so the state is
        // always current. _leftGrab/_rightGrab are the physical-hand grab slots.
        if (heisenberg::IsRockEngineHosted()) {
            rock::HostNotifyExternalGrab(true,  _leftGrab.active);
            rock::HostNotifyExternalGrab(false, _rightGrab.active);
        }

        // Compute real frame delta time
        static auto lastFrameTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float frameDeltaTime = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        // Clamp to sane range (0.001 to 0.1 sec = 10-1000 fps)
        if (frameDeltaTime < 0.001f) frameDeltaTime = 0.001f;
        if (frameDeltaTime > 0.1f) frameDeltaTime = 0.1f;

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

            // Two-handed secondary aim hand is a marker only — it never drives physics
            // (the primary hand does, reading this hand's wand for the aim swing).
            if (state.coHeldSecondary)
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

            // ── KEYFRAMED MODE: Direct visual positioning ──
            RE::NiPoint3 targetPos;
            RE::NiMatrix3 targetRot;
            RE::NiNode* wandNode = nullptr;

            if (repositionActive && state.stickyGrab) {
                // Reposition mode: use frozen world position (adjusted by thumbsticks)
                targetPos = configMode.GetFrozenWorldPos();
                targetRot = configMode.GetFrozenWorldRot();
            } else {
                // Normal mode: calculate from hand/weapon position + offset
                wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
                if (!wandNode)
                    return;

                RE::NiPoint3 parentPos;
                RE::NiMatrix3 parentRot;
                ResolveGrabParent(state, isLeft, wandNode->world.translate, wandNode->world.rotate,
                                  parentPos, parentRot);

                // Calculate target: parent + active grab placement
                RE::NiPoint3 localOffset;
                RE::NiMatrix3 localRotation;
                GetEffectiveGrabPlacement(state, localOffset, localRotation);
                targetRot = localRotation * parentRot;

                // Power armor glove compensation (same as UpdateGrab)
                if (Utils::IsPlayerInPowerArmor()) {
                    localOffset.x += g_config.paGrabOffsetX;
                    localOffset.y += g_config.paGrabOffsetY;
                    localOffset.z += g_config.paGrabOffsetZ;
                }

                RE::NiPoint3 rotatedOffset = parentRot.Transpose() * localOffset;
                targetPos = parentPos + rotatedOffset;

                // Two-handed aim: if the OTHER hand is co-holding this same object as the
                // secondary aim hand, swing the object's forward axis toward the line between
                // the two hands (the HIGGS gun-style two-handed hold). Position stays anchored
                // to this (primary) hand. TUNING NOTE: primaryFwd uses the parent's local +Y
                // (row 1) as the object's forward — if the held object aims along a different
                // axis in-game, change which parentRot row is read here.
                if (g_config.enableTwoHandedGrab && !state.isPulling) {
                    GrabState& aimPartner = isLeft ? _rightGrab : _leftGrab;
                    if (aimPartner.active && aimPartner.coHeldSecondary &&
                        aimPartner.GetRefr() == stateRefr) {
                        if (RE::NiNode* aimWand = heisenberg::GetWandNode(playerNodes, !isLeft)) {
                            const RE::NiPoint3 primaryFwd(parentRot.entry[1][0], parentRot.entry[1][1], parentRot.entry[1][2]);
                            const RE::NiPoint3 aimVec = aimWand->world.translate - parentPos;
                            if (Utils::VectorLength(aimVec) > 5.0f) {  // hands ≥5cm apart
                                // MakeVectorAlignmentRotation builds a standard COLUMN-vector
                                // Rodrigues rotation. Composing it directly into this function's
                                // row-vector world (targetRot = targetRot * swing) makes each
                                // world axis transform as row*S = S^T*row = R(-theta)*row - the
                                // object swings the WRONG way (mirrored, growing with angle).
                                // Right-compose the transpose to get the intended R(+theta).
                                const RE::NiMatrix3 swing = MakeVectorAlignmentRotation(primaryFwd, aimVec);
                                targetRot = targetRot * swing.Transpose();  // F4VR row-vector compose
                            }
                        }
                    }
                }

                // Handle pull animation
                if (state.isPulling && state.pullProgress < 1.0f)
                {
                    float t = state.pullProgress;
                    RE::NiPoint3 finalPos = parentPos + (parentRot.Transpose() * localOffset);
                    targetPos = state.initialObjectPos * (1.0f - t) + finalPos * t;
                }
            }

            // =====================================================================
            // WALL / NPC CLAMP (keyframed grab — additive, does not affect tracking)
            // =====================================================================
            // The keyframed held object would otherwise pass through static geometry and
            // NPCs. Sweep the object's bounding radius from its CURRENT position toward the
            // desired target (ignoring the held object itself, so no self-hit). If something
            // is in the way, clamp the target to the contact point so the object STOPS at the
            // wall/NPC instead of clipping through. When the path is clear, targetPos is left
            // exactly as computed — so normal hand-following is untouched. Skipped during the
            // pull-in animation so the object can be pulled in through the air.
            if (g_config.heldObjectWallClamp && state.node && !state.isPulling)
            {
                RE::TESObjectREFR* clampRefr = state.GetRefr();
                const RE::NiPoint3 curPos = state.node->world.translate;
                RE::NiPoint3 toTarget = targetPos - curPos;
                const float moveDist = toTarget.Length();
                if (clampRefr && moveDist > 1.0f)
                {
                    const RE::NiPoint3 dir = toTarget * (1.0f / moveDist);
                    float objR = state.node->worldBound.fRadius;
                    if (!(objR > 2.0f && objR < 30.0f)) objR = (objR < 2.0f) ? 2.0f : 30.0f;
                    auto hit = heisenberg::Physics::CastSphere(curPos, dir, objR, moveDist, clampRefr);
                    // [WALLCLAMP-DIAG Jul 18] sampled probe: shows whether the sweep runs, what it
                    // hits, and whether the clamp engages - user reports held objects pass walls.
                    {
                        static std::uint32_t s_wcLog = 0;
                        if ((++s_wcLog % 30) == 0 || hit.hit) {
                            spdlog::debug("[WALLCLAMP-DIAG] cur=({:.0f},{:.0f},{:.0f}) move={:.1f} r={:.1f} hit={} frac={:.2f}",
                                          curPos.x, curPos.y, curPos.z, moveDist, objR,
                                          hit.hit ? "YES" : "no", hit.hit ? hit.hitFraction : 0.0f);
                        }
                    }
                    if (hit.hit)
                    {
                        // Ignore hits on OUR OWN collider bodies (layer 43 — the ROCK hand/body/
                        // weapon physics rigs). Without this the clamp false-hits the hand colliders
                        // next to the held object and the player's 23 body capsules, snapping the
                        // object short so it "drifts behind" the hand while carrying. Only real
                        // walls/NPCs (other layers) should stop it.
                        bool ownCollider = false;
                        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                            if (player->parentCell) {
                                if (auto* bhk = player->parentCell->GetbhkWorld()) {
                                    void* hknpWorld = *reinterpret_cast<void**>(
                                        reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
                                    std::uint32_t bodyIdRaw = 0;
                                    std::memcpy(&bodyIdRaw, &hit.bodyId, sizeof(bodyIdRaw));
                                    std::uint32_t filter = 0;
                                    if (hknpWorld &&
                                        heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, bodyIdRaw, filter)) {
                                        const std::uint32_t hitLayer = filter & 0x7Fu;
                                        if (hitLayer == 43u) {
                                            ownCollider = true;
                                        }
                                        // PLAYER-BODY EXCLUSION (Jul 24, "springy coin"): moving a
                                        // held object DOWN toward yourself sweeps it into the
                                        // player's own character-controller capsule (kCharController
                                        // layer 30 / biped 8) — the clamp then stops the object
                                        // short of the hand every frame, so it visibly lags and the
                                        // fingers clip through it. Your own body must never wall-
                                        // clamp what your own hand carries.
                                        if (hitLayer == 30u || hitLayer == 8u) {
                                            ownCollider = true;
                                        }
                                        // Also exclude by body id: the player's proxy body,
                                        // whatever layer it reports.
                                        const std::uint32_t playerBodyId = heisenberg::Physics::GetPlayerBodyId();
                                        if (playerBodyId != 0 && playerBodyId != 0x7FFFFFFFu && bodyIdRaw == playerBodyId) {
                                            ownCollider = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (!ownCollider) {
                            const float hitDist = hit.hitFraction * moveDist;
                            if (hitDist >= 0.0f && hitDist < moveDist)
                            {
                                targetPos = curPos + dir * hitDist;  // stop AT the wall / NPC
                                spdlog::debug("[WALLCLAMP-DIAG] CLAMPED at dist={:.1f}/{:.1f}", hitDist, moveDist);
                            }
                        }
                    }
                }
            }

            {
                // =================================================================
                // VISUAL UPDATE - Update node and propagate to children (keyframed)
                // =================================================================
                RE::NiTransform desiredTransform;
                desiredTransform.translate = targetPos;
                desiredTransform.rotate = targetRot;
                desiredTransform.scale = state.node->local.scale > 0.0f ? state.node->local.scale : 1.0f;

                // WORLD-CHANGE RESYNC (Jul 19, "carried an ammo box through Vault 111's exit,
                // hand/gun stopped colliding with it, still broken several cells later at
                // Diamond City"): state.node above tracks correctly across a cell/worldspace
                // load (script-space, engine-driven every frame regardless of Havok world
                // identity) but the held object's Havok collision presence does not self-heal
                // the same way.
                //
                // v2 (same day, first attempt didn't fire): v1 read GetBhkWorldFromRefr(stateRefr)
                // — the HELD OBJECT's OWN parentCell. The very next test proved that's the wrong
                // signal: ROCK's own detector (getPlayerBhkWorld(), player->GetParentCell())
                // logged "bhkWorld changed (cell transition)" for this exact transition, but our
                // check never fired. A grabbed object is detached and reparented under the
                // player by our own grab code, so the engine has no reason to keep updating ITS
                // parentCell once it's no longer really "in" a cell from the engine's point of
                // view — refr->parentCell for a held object goes stale/frozen, so comparing
                // against it can never detect a real transition. The PLAYER's parentCell is
                // always kept live (the player is never detached), so mirror ROCK's own
                // getPlayerBhkWorld() pattern instead: the world identity check reads the
                // PLAYER's current bhkWorld; stateRefr is still used (correctly) to locate and
                // resync the held object's OWN collision object once a change is detected.
                if (stateRefr) {
                    RE::bhkWorld* currentHeldWorld = nullptr;
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        if (auto* playerCell = player->GetParentCell()) {
                            currentHeldWorld = playerCell->GetbhkWorld();
                        }
                    }
                    if (currentHeldWorld) {
                        const bool heldWorldChanged = state.lastSyncedBhkWorld && currentHeldWorld != state.lastSyncedBhkWorld;
                        if (heldWorldChanged) {
                            spdlog::info("[GRAB] {} hand: bhkWorld changed while holding (cell/worldspace transition) - resyncing held object collision",
                                         isLeft ? "Left" : "Right");
                            state.collisionObject = GetCollisionObject(stateRefr);
                            if (state.collisionObject && state.collisionObject->spSystem) {
                                if (void* heldWorldRaw = AccessWorld(state.collisionObject)) {
                                    std::uint32_t heldBodyId = 0x7FFFFFFF;
                                    heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
                                        state.collisionObject->spSystem.get(), &heldBodyId, state.collisionObject->systemBodyIdx);
                                    if (heldBodyId != 0x7FFFFFFF) {
                                        RebuildBodyCollisionCachesNative(heldWorldRaw, heldBodyId);
                                        heisenberg::Physics::TrySetBodyCollisionLookAhead(heldWorldRaw, heldBodyId, 0.25f);
                                    }
                                }
                            }
                        }
                        state.lastSyncedBhkWorld = currentHeldWorld;
                    }
                }

                // Capture where the held object IS before we move it, so we can derive this
                // frame's motion and shove clutter the object sweeps into. The native
                // keyframed-body sync does NOT reliably push neighbours in our build, so we
                // drive the push manually (this is what made held->object collision work).
                const RE::NiPoint3 heldPrevPos = state.node->world.translate;

                Utils::UpdateKeyframedNode(state.node.get(), desiredTransform);
                // PHANTOM-BODY FIX: during the pull-in the visual mesh teleports toward the
                // hand every frame; dragging the captured keyframed (infinite-mass) bodies
                // along that sweep is what batted world clutter over. Park the bodies for the
                // pull — they snap to the held pose on the first post-pull frame here, and
                // EndGrabKeyframed's release path syncs them even if the grab ends mid-pull.
                if (state.capturedHeldBodyCount > 0 && !state.isPulling) {
                    RE::bhkWorld* heldBodyWorld =
                        state.savedState.savedBhkWorld;
                    if (stateRefr) {
                        if (auto* currentWorld =
                                GetBhkWorldFromRefr(stateRefr)) {
                            heldBodyWorld = currentWorld;
                        }
                    }
                    if (!SyncCapturedHeldBodyFrames(
                            state,
                            desiredTransform,
                            heldBodyWorld)) {
                        static std::uint32_t
                            s_multibodySyncFailureLog = 0;
                        if ((++s_multibodySyncFailureLog % 60) == 1) {
                            spdlog::warn(
                                "[GRAB-MULTIBODY] Failed to sync {} "
                                "captured body frame(s) for {:08X}",
                                state.capturedHeldBodyCount,
                                stateRefr
                                    ? stateRefr->formID
                                    : 0);
                        }
                    }
                }

                // Held object pushes other clutter it moves toward (skipped during pull-in).
                if (!state.isPulling)
                {
                    const float pushDelta = (frameDeltaTime > 0.001f) ? frameDeltaTime : (1.0f / 90.0f);
                    const RE::NiPoint3 heldVel = (targetPos - heldPrevPos) * (1.0f / pushDelta);
                    float objR = state.node->worldBound.fRadius;
                    if (!(objR > 2.0f && objR < 40.0f)) objR = (objR < 2.0f) ? 2.0f : 40.0f;
                    HandCollision::GetSingleton().PushObjectsToward(
                        targetPos, heldVel, objR + 6.0f, stateRefr);
                }

                // Also update refr->data.location to prevent ghosting
                if (stateRefr)
                {
                    stateRefr->data.location.x = targetPos.x;
                    stateRefr->data.location.y = targetPos.y;
                    stateRefr->data.location.z = targetPos.z;
                }
            }

            bool suppressFingerReapply = repositionActive && state.stickyGrab;
            if ((!state.isTelekinesis || state.naturalFingerPosing) && !suppressFingerReapply) {
                if (!ResolvePendingFingerCurls(state, wandNode, isLeft, "Keyframed post-physics")) {
                    if (!state.isPulling && HasConfiguredFingerCurls(state)) {
                        ApplyConfiguredFingerCurls(state, isLeft);
                    }
                }
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
        // NOTE: This function is currently NOT called — the pre-render hook (0x1C21156)
        // is disabled due to scene graph crashes. Kept for future restoration.
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

                // Resolve parent frame (skinned hand / FRIK / wand) so visual
                // update uses the same parent the stored offset was computed in.
                RE::NiPoint3 parentPos;
                RE::NiMatrix3 parentRot;
                ResolveGrabParent(state, isLeft, wandNode->world.translate, wandNode->world.rotate,
                                  parentPos, parentRot);

                // Calculate target transform (same formula as PostPhysicsGrabUpdate but NO prediction)
                RE::NiPoint3 localOffset;
                RE::NiMatrix3 localRotation;
                GetEffectiveGrabPlacement(state, localOffset, localRotation);
                targetRot = localRotation * parentRot;

                // Power armor glove compensation (same as UpdateGrab)
                if (Utils::IsPlayerInPowerArmor()) {
                    localOffset.x += g_config.paGrabOffsetX;
                    localOffset.y += g_config.paGrabOffsetY;
                    localOffset.z += g_config.paGrabOffsetZ;
                }

                RE::NiPoint3 rotatedOffset = parentRot.Transpose() * localOffset;
                targetPos = parentPos + rotatedOffset;
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

// Two-handed support-hand finger pose driver (Jul 19). While the embedded ROCK engine
// holds a support grip, FRIK still renders the controller grip-fist (ROCK's v5
// finger-pose API does not exist on pre-v5 FRIK) — fingers clip the foregrip. Drive
// FRIK's BASE finger API (v3, present on every FRIK) instead:
//   mode 1: ROCK's own mesh-solved grip pose (support-grip triangles, curl-disk solver)
//   mode 2: Heisenberg's HIGGS-table geometry solver against the whole weapon mesh,
//           solved once at grip capture (retried while the solve fails, max 30 frames)
// Cleared the frame the grip disengages.
            // mode 2: one-shot geometry solve against the weapon mesh at grip capture
void heisenberg::UpdateTwoHandedSupportFingerPose()
{
        const int mode = g_config.twoHandedFingerPoseMode;
        static bool s_active[2] = { false, false };
        // Scope-mode exit (Jul 19): tracked independent of the finger-pose mode so it works
        // even with iTwoHandedFingerPoseMode=0. On the support grip's falling edge, exit
        // scope mode (BetterScopesVR zoom and/or the vanilla ScopeMenu).
        static bool s_prevSupportEngaged[2] = { false, false };
        static bool s_solved[2] = { false, false };
        static int s_attempts[2] = { 0, 0 };
        static float s_curls[2][5] = {};

        auto& frik = FRIKInterface::GetSingleton();
        if (!frik.IsAvailable()) {
            return;
        }

        for (int hi = 0; hi < 2; ++hi) {
            const bool isLeft = (hi == 1);
            const bool supportEngaged = IsRockEngineHosted() && rock::HostIsWeaponSupportEngaged(isLeft);
            if (s_prevSupportEngaged[hi] && !supportEngaged) {
                ExitScopeModeOnGripRelease();
            }
            s_prevSupportEngaged[hi] = supportEngaged;
            const bool engaged = mode > 0 && supportEngaged;
            if (!engaged) {
                if (s_active[hi]) {
                    frik.ClearHandPoseFingerPositions(isLeft);
                    spdlog::debug("[THG-FINGER] {} support grip released - finger pose cleared", isLeft ? "L" : "R");
                }
                s_active[hi] = false;
                s_solved[hi] = false;
                s_attempts[hi] = 0;
                continue;
            }

            if (mode == 1) {
                float curls[5];
                if (rock::HostGetWeaponSupportFingerCurls(isLeft, curls)) {
                    // CONVENTION (Jul 19, settled by A/B in-game): ROCK grip-pose joints use
                    // the SAME scale as FRIK scalars (1=open) — an inverted send made the
                    // fingers fist THROUGH the gun. Pass through unmodified.
                    frik.SetHandPoseFingerPositions(isLeft, curls[0], curls[1], curls[2], curls[3], curls[4]);
                    if (!s_active[hi]) {
                        spdlog::info("[THG-FINGER] {} mode1 (ROCK grip pose): {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}",
                                     isLeft ? "L" : "R", curls[0], curls[1], curls[2], curls[3], curls[4]);
                    }
                    s_active[hi] = true;
                }
                continue;
            }

            // mode 2: one-shot geometry solve against the weapon mesh at grip capture
            if (!s_solved[hi] && s_attempts[hi] < 30) {
                ++s_attempts[hi];
                auto* player = f4cf::f4vr::getPlayer();
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                RE::NiNode* wandNode = playerNodes ? heisenberg::GetWandNode(playerNodes, isLeft) : nullptr;
                RE::NiAVObject* weaponObj = (player && player->firstPerson3D.get())
                    ? FindNodeDirectChildren(player->firstPerson3D.get(), "Weapon", 20)
                    : nullptr;
                if (wandNode && weaponObj) {
                    RE::NiNode* handNode = GetSkinnedHandNode(isLeft);
                    if (!handNode) {
                        handNode = wandNode;
                    }
                    const RE::NiPoint3 palmPos = GetPalmPosition(wandNode, isLeft);
                    const RE::NiPoint3 palmDir = GetPalmDirection(wandNode, isLeft);
                    auto r = heisenberg::CalculateFingerCurlFromGeometry(weaponObj, handNode, palmPos, palmDir, isLeft, 1.0f);
                    if (r.success) {
                        s_curls[hi][0] = r.thumb;
                        s_curls[hi][1] = r.index;
                        s_curls[hi][2] = r.middle;
                        s_curls[hi][3] = r.ring;
                        s_curls[hi][4] = r.pinky;
                        s_solved[hi] = true;
                        spdlog::info("[THG-FINGER] {} mode2 (weapon-mesh solve, attempt {}): {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}",
                                     isLeft ? "L" : "R", s_attempts[hi],
                                     s_curls[hi][0], s_curls[hi][1], s_curls[hi][2], s_curls[hi][3], s_curls[hi][4]);
                    }
                }
            }
            if (s_solved[hi]) {
                frik.SetHandPoseFingerPositions(isLeft, s_curls[hi][0], s_curls[hi][1], s_curls[hi][2], s_curls[hi][3], s_curls[hi][4]);
                s_active[hi] = true;
            }
        }
}
