#include "Grab.h"
#include "Config.h"
#include "rock/RockBridge.h"
#include "../external/ROCK/src/ROCKMain.h"  // rock::HostNotifyExternalGrab — embedded-engine grab-lifecycle seam (audit rank 2)
#include "../external/ROCK/src/physics-interaction/grab/TouchGrabBridge.h"
#include "../external/ROCK/src/physics-interaction/native/HavokRuntime.h"
#include "../external/ROCK/src/physics-interaction/TransformMath.h"
#include "../external/ROCK/src/physics-interaction/weapon/LooseWeaponGripZone.h"
#include "../external/ROCK/src/physics-interaction/weapon/NativeScopeReentryPolicy.h"
#include "DropToHand.h"
#include "ConsumableUsePolicy.h"
#include "DualWieldAPI.h"
#include "F4VROffsets.h"
#include "FingerCurves.h"
#include "FRIKInterface.h"
#include "GrabConstraint.h"
#include "GrabOwnershipPolicy.h"
#include "GrabPosePolicy.h"
#include "SmartGrabHandler.h"
#include "SS2Integration.h"
#include "NpcInjectionPolicy.h"
#include "HeldCollisionBodySetPolicy.h"
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
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <sstream>

namespace
{
    constexpr const char* OBJECT_COHOLD_HAND_TAG =
        "Heisenberg_ObjectCoHold";
    constexpr int OBJECT_COHOLD_HAND_PRIORITY = 95;
}

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

    static constexpr std::size_t kMaxCollisionWrappers = 64;
    struct CollisionWrapperSet
    {
        std::array<
            RE::bhkNPCollisionObject*,
            kMaxCollisionWrappers>
            wrappers{};
        std::array<bool, kMaxCollisionWrappers> visible{};
        std::uint32_t count = 0;
        std::uint32_t totalWrapperCount = 0;
        bool overflowed = false;

        [[nodiscard]] bool Contains(
            const RE::bhkNPCollisionObject* collision) const
        {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (wrappers[i] == collision) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool IsVisible(
            const RE::bhkNPCollisionObject* collision) const
        {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (wrappers[i] == collision) {
                    return visible[i];
                }
            }
            return false;
        }
    };

    static CollisionWrapperSet CollectCollisionWrappers(
        RE::NiAVObject* referenceRoot);
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
        if (state.rockRichFingerPosePublished) {
            // The ROCK_Grab registry winner is persistent and is re-applied
            // after FRIK each frame. Re-sending the lossy curl-only API here
            // would erase splay and local surface corrections on 0.77.12.
            return true;
        }
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

        // CALIBRATED local -> world uses the row-vector convention:
        //   world = rotate.Transpose() * (local * scale) + translate.
        // The calibration that produced palmLocal/dirLocal now measures in the
        // true hand-local basis (FingerCurves InverseTransformPoint), so this
        // side must transpose. Previously BOTH sides used the row basis and
        // round-tripped, which hid the error until a re-measure at a different
        // wrist orientation returned a different "hand-local" palm.
        const auto& xform = skinned->world;
        outPos = xform.rotate.Transpose() * (palmLocal * xform.scale) + xform.translate;
        outDir = heisenberg::VectorNormalized(xform.rotate.Transpose() * dirLocal);
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

        // Row-vector convention: local -> world is rotate.Transpose() * local.
        // Using `rotate * local` here does NOT describe a fixed hand-local
        // direction — it is off by R^2, so it swung with the wrist and no INI
        // value could have made it stable (which is why the config palm never
        // felt tunable). See the note in GetCalibratedPalmWorld.
        return heisenberg::VectorNormalized(handNode->world.rotate.Transpose() * palmVectorLocal);
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

        // Row-vector local -> world (see GetPalmDirection).
        const auto& xform = handNode->world;
        return xform.rotate.Transpose() * (palmPosLocal * xform.scale) + xform.translate;
    }

    // Palm input for any solver that also publishes rendered-hand authority.
    // HostGetCleanPreAuthorityHandWorld is captured after FRIK pass 1 but
    // before authority
    // writers, so it cannot contain this solver's previous output.  If that
    // same-frame seam or finger calibration is unavailable, deliberately use
    // the controller/wand frame rather than reading the skinned hand.
    bool GetCleanTrackedPalmPositionImpl(
        RE::NiAVObject* wandNode,
        bool isLeft,
        RE::NiPoint3& outPos)
    {
        RE::NiPoint3 palmLocal{};
        RE::NiPoint3 directionLocal{};
        RE::NiTransform cleanHandWorld{};
        if (rock::HostGetCleanPreAuthorityHandWorld(
                isLeft,
                cleanHandWorld) &&
            heisenberg::GetCalibratedPalmLocal(
                isLeft,
                palmLocal,
                directionLocal)) {
            const float depth =
                heisenberg::g_config.palmDepthOffset;
            if (depth != 0.0f) {
                RE::NiPoint3 palmarLocal{};
                if (heisenberg::GetCalibratedPalmarLocal(
                        isLeft,
                        palmarLocal)) {
                    palmLocal =
                        palmLocal + palmarLocal * depth;
                }
            }

            // Calibrated local -> world: transpose (see GetCalibratedPalmWorld).
            outPos =
                cleanHandWorld.rotate.Transpose() *
                    (palmLocal * cleanHandWorld.scale) +
                cleanHandWorld.translate;
            if (std::isfinite(outPos.x) &&
                std::isfinite(outPos.y) &&
                std::isfinite(outPos.z)) {
                return true;
            }
        }

        if (!wandNode) {
            return false;
        }

        RE::NiPoint3 wandPalmLocal(
            heisenberg::g_config.palmPositionX,
            heisenberg::g_config.palmPositionY,
            heisenberg::g_config.palmPositionZ);
        if (isLeft) {
            wandPalmLocal.x *= -1.0f;
        }
        const auto& wandWorld = wandNode->world;
        outPos =
            wandWorld.rotate.Transpose() *
                (wandPalmLocal * wandWorld.scale) +
            wandWorld.translate;
        return std::isfinite(outPos.x) &&
               std::isfinite(outPos.y) &&
               std::isfinite(outPos.z);
    }

    // The knuckle centroid does not move when the fingers curl, so a cached
    // calibration whose anchor no longer matches a LIVE knuckle sample was
    // taken against a different skeleton state and describes a palm that does
    // not exist. Measured Jul 30: a LEFT calibration committed 14 ms after a
    // load sat 5.3 units (and 65 degrees) away from what the same hand
    // measured after the next load, which laid grabbed armor flat against a
    // plane nowhere near the hand. This check works at grab time, when the
    // hand is gripping and the idle-only re-calibration cannot run.
    inline constexpr float kCalibratedPalmLiveDriftLimit = 2.5f;

    bool CalibratedPalmFrameMatchesLiveHand(
        bool isLeft,
        RE::NiAVObject* skinnedHand,
        const RE::NiPoint3& calibratedPalmLocal)
    {
        RE::NiPoint3 liveKnuckleCentroidLocal{};
        if (!heisenberg::GetLiveKnuckleCentroidLocal(
                isLeft,
                skinnedHand,
                liveKnuckleCentroidLocal)) {
            // No live witness this frame — keep the cached frame rather than
            // discarding a good calibration over a transient snapshot miss.
            return true;
        }

        const float drift =
            heisenberg::Utils::VectorLength(
                liveKnuckleCentroidLocal - calibratedPalmLocal);
        if (!std::isfinite(drift) ||
            drift <= kCalibratedPalmLiveDriftLimit) {
            return true;
        }

        spdlog::warn(
            "[FINGER-CAL] {} hand palm anchor disagrees with the live "
            "knuckles by {:.2f} units (cached=({:.2f},{:.2f},{:.2f}) "
            "live=({:.2f},{:.2f},{:.2f})) — dropping the stale calibration "
            "and seating from the configured wand palm",
            isLeft ? "LEFT" : "RIGHT",
            drift,
            calibratedPalmLocal.x,
            calibratedPalmLocal.y,
            calibratedPalmLocal.z,
            liveKnuckleCentroidLocal.x,
            liveKnuckleCentroidLocal.y,
            liveKnuckleCentroidLocal.z);
        heisenberg::InvalidateFingerCalibration(isLeft);
        return false;
    }

    bool GetPalmSurfaceSeatFrame(
        RE::NiAVObject* wandNode,
        bool isLeft,
        RE::NiTransform& outParentWorld,
        RE::NiPoint3& outPalmWorld,
        RE::NiPoint3& outPalmarWorld,
        bool& outUsesSkinnedHand)
    {
        outUsesSkinnedHand = false;

        RE::NiPoint3 palmLocal{};
        RE::NiPoint3 fingerForwardLocal{};
        RE::NiPoint3 palmarLocal{};
        if (RE::NiNode* skinned = GetSkinnedHandNode(isLeft);
            skinned &&
            heisenberg::GetCalibratedPalmLocal(
                isLeft,
                palmLocal,
                fingerForwardLocal) &&
            heisenberg::GetCalibratedPalmarLocal(
                isLeft,
                palmarLocal) &&
            CalibratedPalmFrameMatchesLiveHand(isLeft, skinned, palmLocal)) {
            const float depth =
                heisenberg::g_config.palmDepthOffset;
            palmLocal += palmarLocal * depth;
            outParentWorld = skinned->world;
            // Calibrated local -> world: transpose (see GetCalibratedPalmWorld).
            outPalmWorld =
                outParentWorld.rotate.Transpose() *
                    (palmLocal * outParentWorld.scale) +
                outParentWorld.translate;
            outPalmarWorld =
                heisenberg::VectorNormalized(
                    outParentWorld.rotate.Transpose() * palmarLocal);
            outUsesSkinnedHand = true;
        } else if (wandNode) {
            // The wand fallback uses the SAME row-vector convention. Its
            // "local" is the INI constants palmPosition*/palmVector*, and it is
            // tempting to argue those were eyeball-tuned against the old
            // untransposed form and should be left alone. That argument does not
            // survive the algebra: `rotate * p_const` is not a fixed wand-local
            // point at all — matching a fixed point would require p_const to be
            // R^2-corrected, i.e. to change with the wrist. So the configured
            // palm was never a stable offset that tuning could pin down; with
            // the defaults (0, 6.0, -2.4), |p| = 6.46, it wandered by up to ~13
            // units with wrist pose. This branch is the one MOST grabs take
            // (13 of 18 unauthored seats in the Jul 30 log), so leaving it
            // inverted would have left the dominant path broken.
            outParentWorld = wandNode->world;
            palmLocal = RE::NiPoint3(
                heisenberg::g_config.palmPositionX,
                heisenberg::g_config.palmPositionY,
                heisenberg::g_config.palmPositionZ);
            palmarLocal = RE::NiPoint3(
                heisenberg::g_config.palmVectorX,
                heisenberg::g_config.palmVectorY,
                heisenberg::g_config.palmVectorZ);
            if (isLeft) {
                palmLocal.x *= -1.0f;
                palmarLocal.x *= -1.0f;
            }
            outPalmWorld =
                outParentWorld.rotate.Transpose() *
                    (palmLocal * outParentWorld.scale) +
                outParentWorld.translate;
            outPalmarWorld =
                heisenberg::VectorNormalized(
                    outParentWorld.rotate.Transpose() * palmarLocal);
        } else {
            return false;
        }

        return std::isfinite(outPalmWorld.x) &&
               std::isfinite(outPalmWorld.y) &&
               std::isfinite(outPalmWorld.z) &&
               std::isfinite(outPalmarWorld.x) &&
               std::isfinite(outPalmarWorld.y) &&
               std::isfinite(outPalmarWorld.z) &&
               heisenberg::Utils::VectorLength(outPalmarWorld) > 0.5f;
    }

    void StoreRuntimeWorldPlacement(
        heisenberg::GrabState& state,
        const RE::NiTransform& parentWorld,
        const RE::NiPoint3& desiredPivotWorld,
        const RE::NiMatrix3& desiredRotationWorld,
        bool usesSkinnedHand)
    {
        const RE::NiPoint3 localPosition =
            parentWorld.rotate *
            (desiredPivotWorld - parentWorld.translate);
        const RE::NiMatrix3 localRotation =
            desiredRotationWorld *
            parentWorld.rotate.Transpose();

        state.itemOffset.position = localPosition;
        state.itemOffset.rotation = localRotation;
        state.grabOffsetLocal = localPosition;
        state.hasItemOffset = false;
        state.isFRIKOffset = false;
        state.SetRuntimeHandPlacement(
            localPosition,
            localRotation,
            usesSkinnedHand);
    }

    // Replacement for the geometry-blind palm-snap constant.
    //
    // The old formula was pos = (0, 1 + longestAuthoredDim * 0.5, 3) in WAND
    // space. It pushed the object's PIVOT half its longest authored dimension
    // in front of the wand, assumed the pivot sits at the object's centre, and
    // knew nothing about where the palm actually is. Every assumption fails on
    // real meshes: Maxson's Battlecoat came to rest with its visible geometry
    // 14 units from the palm, floating with nothing touching the hand.
    //
    // This seats from the object's live world bound instead. The bound centre
    // supplies the pivot->centre correction the constant could not make, and
    // the palm frame supplies the real palm position and normal. The result is
    // returned in WAND-local space because that is the frame this offset is
    // applied in. Callers with mesh triangles available should prefer the
    // broad/mesh palm seat, which is exact; this is the no-triangle path.
    bool TryCalculateBoundPalmSnapLocal(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        const RE::NiTransform& objectWorld,
        float itemLength,
        float itemWidth,
        float itemHeight,
        RE::NiPoint3& outWandLocalPosition)
    {
        if (!wandNode) {
            return false;
        }

        RE::NiTransform parentWorld{};
        RE::NiPoint3 palmWorld{};
        RE::NiPoint3 palmarWorld{};
        bool usesSkinnedHand = false;
        if (!GetPalmSurfaceSeatFrame(
                wandNode,
                isLeft,
                parentWorld,
                palmWorld,
                palmarWorld,
                usesSkinnedHand)) {
            return false;
        }

        // Thinnest authored dimension is the side an object most plausibly
        // presents once it comes to rest in the palm. Erring toward the palm
        // is deliberate: slight overlap reads as "held", a gap reads as
        // "floating" — the defect this replaces.
        float halfThickness = 0.0f;
        {
            float smallest = 0.0f;
            bool haveSmallest = false;
            for (const float dimension : { itemLength, itemWidth, itemHeight }) {
                if (std::isfinite(dimension) &&
                    dimension > 0.1f &&
                    (!haveSmallest || dimension < smallest)) {
                    smallest = dimension;
                    haveSmallest = true;
                }
            }
            if (haveSmallest) {
                halfThickness = smallest * 0.5f;
            }
        }

        const RE::NiPoint3 targetCentreWorld =
            palmWorld +
            palmarWorld *
                (heisenberg::grab_pose_policy::kPalmSurfaceSkin +
                 halfThickness);

        // Without a usable bound the pivot has to stand in for the centre —
        // the one assumption of the old constant that cannot be avoided here.
        RE::NiPoint3 desiredPivotWorld = targetCentreWorld;
        if (state.node) {
            const float boundRadius = state.node->worldBound.fRadius;
            const RE::NiPoint3 boundCentre = state.node->worldBound.center;
            if (std::isfinite(boundRadius) &&
                boundRadius > 0.01f &&
                boundRadius < 10000.0f &&
                std::isfinite(boundCentre.x) &&
                std::isfinite(boundCentre.y) &&
                std::isfinite(boundCentre.z)) {
                desiredPivotWorld =
                    objectWorld.translate +
                    (targetCentreWorld - boundCentre);
            }
        }

        // The palm frame may be the skinned hand, but this offset is consumed
        // in wand space, so express the result there in every case.
        (void)parentWorld;
        (void)usesSkinnedHand;
        outWandLocalPosition =
            wandNode->world.rotate *
            (desiredPivotWorld - wandNode->world.translate);
        return std::isfinite(outWandLocalPosition.x) &&
               std::isfinite(outWandLocalPosition.y) &&
               std::isfinite(outWandLocalPosition.z);
    }

    bool TryCalculateCanonicalLooseWeaponPlacement(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        RE::TESObjectREFR* weaponRef,
        const char*& outReason)
    {
        outReason = "canonicalHoldUnavailable";
        if (!state.node || !wandNode || !weaponRef) {
            outReason = "missingPlacementInput";
            return false;
        }

        RE::NiTransform canonicalHandWorld{};
        RE::NiTransform handWeaponLocal{};
        if (!rock::loose_weapon_grip_zone::
                tryResolveLooseWeaponFiringHandHold(
                    isLeft,
                    weaponRef,
                    canonicalHandWorld,
                    handWeaponLocal,
                    &outReason)) {
            return false;
        }

        // The ROCK resolver returns the authored firing hand in WEAPON-root
        // space. Invert that relation to obtain the deterministic loose weapon
        // root world transform, then express it in the exact rendered-hand
        // frame Heisenberg will use for the held-object drive.
        const RE::NiTransform desiredWeaponWorld =
            rock::transform_math::composeTransforms(
                canonicalHandWorld,
                rock::transform_math::invertTransform(
                    handWeaponLocal));

        RE::NiTransform placementParent = wandNode->world;
        bool usesSkinnedHand = false;
        if (RE::NiNode* renderedHand =
                GetSkinnedHandNode(isLeft)) {
            placementParent = renderedHand->world;
            usesSkinnedHand = true;
        }

        StoreRuntimeWorldPlacement(
            state,
            placementParent,
            desiredWeaponWorld.translate,
            desiredWeaponWorld.rotate,
            usesSkinnedHand);
        state.usedSnapMode = true;
        return true;
    }

    bool TryCalculateKickballPalmPlacement(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        const RE::NiTransform& objectWorld)
    {
        if (!state.node || !wandNode) {
            return false;
        }

        RE::NiTransform parentWorld{};
        RE::NiPoint3 palmWorld{};
        RE::NiPoint3 palmarWorld{};
        bool usesSkinnedHand = false;
        if (!GetPalmSurfaceSeatFrame(
                wandNode,
                isLeft,
                parentWorld,
                palmWorld,
                palmarWorld,
                usesSkinnedHand)) {
            return false;
        }

        float radius = state.node->worldBound.fRadius;
        const RE::NiPoint3 boundCenter =
            state.node->worldBound.center;
        if (!std::isfinite(radius) ||
            radius < 1.0f ||
            radius > 1000.0f ||
            !std::isfinite(boundCenter.x) ||
            !std::isfinite(boundCenter.y) ||
            !std::isfinite(boundCenter.z)) {
            spdlog::warn(
                "[GRAB-ROCK-POSE] Kickball bound is invalid "
                "(radius={:.2f}); refusing an arbitrary one-unit seat",
                radius);
            return false;
        }
        const float centerDistance =
            heisenberg::grab_pose_policy::
                SpherePalmCenterDistance(radius);
        const RE::NiPoint3 targetCenter =
            palmWorld + palmarWorld * centerDistance;
        // Compensate NIFs whose pivot is not at their bound centre. Moving the
        // pivot by this delta seats the actual sphere surface, not an arbitrary
        // origin, at ROCK's 0.5-unit palm skin.
        const RE::NiPoint3 desiredPivot =
            objectWorld.translate +
            (targetCenter - boundCenter);
        if (!std::isfinite(desiredPivot.x) ||
            !std::isfinite(desiredPivot.y) ||
            !std::isfinite(desiredPivot.z)) {
            return false;
        }
        StoreRuntimeWorldPlacement(
            state,
            parentWorld,
            desiredPivot,
            objectWorld.rotate,
            usesSkinnedHand);
        state.usedSnapMode = true;
        return true;
    }

    bool TryCalculateSelectedSurfacePalmPlacement(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        const RE::NiTransform& objectWorld)
    {
        if (!state.node || !wandNode) {
            return false;
        }

        RE::NiTransform parentWorld{};
        RE::NiPoint3 palmWorld{};
        RE::NiPoint3 palmarWorld{};
        bool usesSkinnedHand = false;
        if (!GetPalmSurfaceSeatFrame(
                wandNode,
                isLeft,
                parentWorld,
                palmWorld,
                palmarWorld,
                usesSkinnedHand)) {
            return false;
        }

        std::vector<heisenberg::TriangleData> triangles;
        triangles.reserve(512);
        heisenberg::GetTriangles(
            state.node.get(),
            triangles,
            4096);
        RE::NiPoint3 currentSurfaceWorld{};
        float selectedPointMeshDistance = -1.0f;
        if (!heisenberg::GetClosestMeshPointToPoint(
                triangles,
                palmWorld,
                currentSurfaceWorld,
                selectedPointMeshDistance) ||
            !std::isfinite(selectedPointMeshDistance) ||
            selectedPointMeshDistance > 5.0f) {
            return false;
        }

        const RE::NiPoint3 targetSurfaceWorld =
            palmWorld +
            palmarWorld *
                heisenberg::grab_pose_policy::kPalmSurfaceSkin;
        const RE::NiPoint3 desiredPivot =
            objectWorld.translate +
            (targetSurfaceWorld - currentSurfaceWorld);
        StoreRuntimeWorldPlacement(
            state,
            parentWorld,
            desiredPivot,
            objectWorld.rotate,
            usesSkinnedHand);
        state.usedSnapMode = true;
        return true;
    }

    RE::NiMatrix3 MakeVectorAlignmentRotation(
        const RE::NiPoint3& fromVector,
        const RE::NiPoint3& toVector);

    bool TryCalculateBroadObjectPalmPlacement(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        const RE::NiTransform& objectWorld,
        bool isHolotape,
        bool hasAuthoredPlacement,
        bool directTouchPlacement,
        bool replacingGeometryBlindSeat)
    {
        if (!state.node || !wandNode ||
            !std::isfinite(objectWorld.scale) ||
            std::abs(objectWorld.scale) < 1.0e-4f) {
            return false;
        }

        RE::NiTransform parentWorld{};
        RE::NiPoint3 palmWorld{};
        RE::NiPoint3 palmarWorld{};
        bool usesSkinnedHand = false;
        if (!GetPalmSurfaceSeatFrame(
                wandNode,
                isLeft,
                parentWorld,
                palmWorld,
                palmarWorld,
                usesSkinnedHand)) {
            return false;
        }

        std::vector<heisenberg::TriangleData> triangles;
        triangles.reserve(512);
        heisenberg::GetTriangles(
            state.node.get(),
            triangles,
            4096);
        if (triangles.empty()) {
            return false;
        }

        const float infinity =
            (std::numeric_limits<float>::infinity)();
        RE::NiPoint3 localMin{ infinity, infinity, infinity };
        RE::NiPoint3 localMax{ -infinity, -infinity, -infinity };
        std::size_t finiteVertexCount = 0;
        const auto includeVertex =
            [&](const RE::NiPoint3& worldVertex) {
                if (!std::isfinite(worldVertex.x) ||
                    !std::isfinite(worldVertex.y) ||
                    !std::isfinite(worldVertex.z)) {
                    return;
                }
                const RE::NiPoint3 localVertex =
                    rock::transform_math::worldPointToLocal(
                        objectWorld,
                        worldVertex);
                if (!std::isfinite(localVertex.x) ||
                    !std::isfinite(localVertex.y) ||
                    !std::isfinite(localVertex.z)) {
                    return;
                }
                localMin.x = (std::min)(localMin.x, localVertex.x);
                localMin.y = (std::min)(localMin.y, localVertex.y);
                localMin.z = (std::min)(localMin.z, localVertex.z);
                localMax.x = (std::max)(localMax.x, localVertex.x);
                localMax.y = (std::max)(localMax.y, localVertex.y);
                localMax.z = (std::max)(localMax.z, localVertex.z);
                ++finiteVertexCount;
            };
        for (const auto& triangle : triangles) {
            includeVertex(triangle.v0);
            includeVertex(triangle.v1);
            includeVertex(triangle.v2);
        }
        if (finiteVertexCount < 3) {
            return false;
        }

        const float worldScale = std::abs(objectWorld.scale);
        const RE::NiPoint3 localSpans{
            localMax.x - localMin.x,
            localMax.y - localMin.y,
            localMax.z - localMin.z,
        };
        const RE::NiPoint3 actualSpans{
            localSpans.x * worldScale,
            localSpans.y * worldScale,
            localSpans.z * worldScale,
        };
        const bool broadPalmSeat =
            heisenberg::grab_pose_policy::
                ShouldApplyBroadPalmSeat(
                    isHolotape,
                    hasAuthoredPlacement,
                    directTouchPlacement,
                    actualSpans.x,
                    actualSpans.y,
                    actualSpans.z);
        const bool meshSeatFallback =
            !broadPalmSeat &&
            heisenberg::grab_pose_policy::
                ShouldApplyMeshPalmSeatFallback(
                    isHolotape,
                    hasAuthoredPlacement,
                    directTouchPlacement,
                    replacingGeometryBlindSeat,
                    actualSpans.x,
                    actualSpans.y,
                    actualSpans.z);
        if (!broadPalmSeat && !meshSeatFallback) {
            // Log the reject with its measured spans. A silent false here sent
            // Maxson's Battlecoat to the geometry-blind constant formula and
            // left it floating 14 units off the palm with no trace in the log.
            spdlog::info(
                "[GRAB-BROAD-PALM] '{}' not seated: spans=({:.2f},{:.2f},{:.2f}) "
                "triangles={} holotape={} authored={} directTouch={} "
                "snapConstantSeat={} — placement left to the caller",
                state.node->name.c_str(),
                actualSpans.x,
                actualSpans.y,
                actualSpans.z,
                triangles.size(),
                isHolotape,
                hasAuthoredPlacement,
                directTouchPlacement,
                replacingGeometryBlindSeat);
            return false;
        }

        // Center the complete visible object across the palm. First rotate its
        // thinnest object-local axis onto the palm normal so a slab cannot
        // remain edge-on, then put its broad rear face exactly one skin-width
        // in front of the palm. Unlike first-contact attachment, no individual
        // fingertip can become the object's pivot.
        const RE::NiPoint3 localGeometryCenter{
            (localMin.x + localMax.x) * 0.5f,
            (localMin.y + localMax.y) * 0.5f,
            (localMin.z + localMax.z) * 0.5f,
        };

        int thinAxisIndex = 0;
        RE::NiPoint3 thinAxisLocal{ 1.0f, 0.0f, 0.0f };
        float thinSpan = actualSpans.x;
        if (actualSpans.y < thinSpan) {
            thinAxisIndex = 1;
            thinAxisLocal = RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
            thinSpan = actualSpans.y;
        }
        if (actualSpans.z < thinSpan) {
            thinAxisIndex = 2;
            thinAxisLocal = RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
            thinSpan = actualSpans.z;
        }

        RE::NiPoint3 currentThinAxisWorld =
            heisenberg::VectorNormalized(
                rock::transform_math::localVectorToWorld(
                    objectWorld,
                    thinAxisLocal));
        float thinAxisPalmDot =
            heisenberg::DotProduct(
                currentThinAxisWorld,
                palmarWorld);
        if (thinAxisPalmDot < 0.0f) {
            thinAxisLocal = thinAxisLocal * -1.0f;
            currentThinAxisWorld =
                currentThinAxisWorld * -1.0f;
            thinAxisPalmDot = -thinAxisPalmDot;
        }
        if (broadPalmSeat &&
            heisenberg::Utils::VectorLength(
                currentThinAxisWorld) < 0.5f) {
            return false;
        }

        // The broad seat turns the mesh's thin axis onto the palm normal so a
        // slab cannot stay edge-on. The fallback seat has no broad face to
        // present, so it keeps the object's own orientation and only moves it.
        RE::NiMatrix3 desiredRotationWorld = objectWorld.rotate;
        if (broadPalmSeat) {
            const RE::NiMatrix3 broadFaceAlignment =
                MakeVectorAlignmentRotation(
                    currentThinAxisWorld,
                    palmarWorld);
            desiredRotationWorld =
                objectWorld.rotate *
                broadFaceAlignment.Transpose();
        }
        RE::NiTransform desiredObjectWorld = objectWorld;
        desiredObjectWorld.rotate = desiredRotationWorld;
        desiredObjectWorld.translate =
            RE::NiPoint3{ 0.0f, 0.0f, 0.0f };

        // How deep the mesh reaches toward the palm once rotated, measured
        // from its own centre. For the broad seat this is exactly -thinSpan/2;
        // deriving it from the corners instead keeps the fallback (arbitrary
        // orientation) correct with the same line of code, so the SURFACE —
        // never the pivot — is what lands one skin-width off the palm.
        float rearPlaneProjection = 0.0f;
        bool haveRearPlane = false;
        for (int corner = 0; corner < 8; ++corner) {
            const RE::NiPoint3 cornerLocal{
                (corner & 1) ? localMax.x : localMin.x,
                (corner & 2) ? localMax.y : localMin.y,
                (corner & 4) ? localMax.z : localMin.z,
            };
            const RE::NiPoint3 cornerOffsetWorld =
                rock::transform_math::localVectorToWorld(
                    desiredObjectWorld,
                    cornerLocal - localGeometryCenter);
            const float projection =
                heisenberg::DotProduct(cornerOffsetWorld, palmarWorld);
            if (!std::isfinite(projection)) {
                continue;
            }
            if (!haveRearPlane || projection < rearPlaneProjection) {
                rearPlaneProjection = projection;
                haveRearPlane = true;
            }
        }
        if (!haveRearPlane) {
            return false;
        }

        const RE::NiPoint3 targetGeometryCenter =
            palmWorld +
            palmarWorld *
                (heisenberg::grab_pose_policy::kPalmSurfaceSkin -
                 rearPlaneProjection);
        const RE::NiPoint3 rotatedCenterOffset =
            rock::transform_math::localVectorToWorld(
                desiredObjectWorld,
                localGeometryCenter);
        const RE::NiPoint3 desiredPivot =
            targetGeometryCenter - rotatedCenterOffset;
        if (!std::isfinite(desiredPivot.x) ||
            !std::isfinite(desiredPivot.y) ||
            !std::isfinite(desiredPivot.z)) {
            return false;
        }

        StoreRuntimeWorldPlacement(
            state,
            parentWorld,
            desiredPivot,
            desiredRotationWorld,
            usesSkinnedHand);
        state.usedSnapMode = true;
        state.itemOffset.hasFingerCurls = false;
        state.itemOffset.hasJointCurls = false;
        state.ClearRuntimeFingerCurls();

        spdlog::info(
            "[GRAB-BROAD-PALM] '{}' seat={} spans=({:.2f},{:.2f},{:.2f}) "
            "triangles={} thinAxis={} thinSpan={:.2f} "
            "alignmentDot={:.3f} rearPlane={:.2f} skin={:.2f} "
            "centerLocal=({:.2f},{:.2f},{:.2f}) "
            "placementLocal=({:.2f},{:.2f},{:.2f}) frame={} "
            "source={} — centered on palm, not first-contact finger",
            state.node->name.c_str(),
            broadPalmSeat ? "broad-face" : "mesh-fallback",
            actualSpans.x,
            actualSpans.y,
            actualSpans.z,
            triangles.size(),
            thinAxisIndex,
            thinSpan,
            thinAxisPalmDot,
            rearPlaneProjection,
            heisenberg::grab_pose_policy::kPalmSurfaceSkin,
            localGeometryCenter.x,
            localGeometryCenter.y,
            localGeometryCenter.z,
            state.runtimeHandPlacementPosition.x,
            state.runtimeHandPlacementPosition.y,
            state.runtimeHandPlacementPosition.z,
            usesSkinnedHand ? "skinned" : "wand",
            directTouchPlacement ? "direct-touch" : "unauthored-snap");
        return true;
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

    // Port of HIGGS Hand::TransitionHeld placement (hand.cpp:1301-1547),
    // translation only: the object keeps its world rotation and is moved so
    // that the closest mesh point to the palm line lands on the palm.
    //
    //   objectWorld  - pose to seat FROM. For a close grab this is the live
    //                  node transform. For a remote pull it is the object's
    //                  virtual ARRIVAL pose (bound centre at the palm), which
    //                  is where HIGGS's pulled object physically is when its
    //                  TransitionHeld runs. Only the translation may differ
    //                  from state.node->world; rotation must match.
    //   pulledArrival- HIGGS `shouldMoveHandBack` (hand.cpp:1330-1337 and
    //                  1407-1409): the object arrived by pull, so before the
    //                  closest-point search it is moved back out of the hand
    //                  by palmDir * pulledGrabHandAdjustDistance. A close
    //                  grab does NOT get this shift in HIGGS.
    //
    // Palm anchor and palm normal come from GetPalmSurfaceSeatFrame: the
    // skinned-hand palm (knuckle centroid + palmar depth) with its PALMAR
    // direction, or the wand-frame palmPosition/palmVector constants. The
    // palmar direction is the equivalent of HIGGS's palmVector (out of the
    // palm). The finger-forward calibrated direction that used to drive this
    // ray points along the fingers, not out of the palm, so the search ran
    // parallel to the hand surface.
    bool TryCalculateRuntimeHandPlacementFromGeometry(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        const RE::NiTransform& objectWorld,
        bool pulledArrival)
    {
        state.ClearRuntimeHandPlacement();

        if (!state.node || !wandNode) {
            spdlog::debug("[GEOM-PLACE] Failed: node={:p} wand={:p}", (void*)state.node.get(), (void*)wandNode);
            return false;
        }

        RE::NiTransform parentWorld{};
        RE::NiPoint3 palmPos{};
        RE::NiPoint3 palmDir{};
        bool usesSkinnedHand = false;
        if (!GetPalmSurfaceSeatFrame(
                wandNode,
                isLeft,
                parentWorld,
                palmPos,
                palmDir,
                usesSkinnedHand)) {
            spdlog::info("[GEOM-PLACE] FALLBACK: no palm frame for '{}'", state.node->name.c_str());
            return false;
        }

        // HIGGS gathers every triangle of the object root (hand.cpp:1414-1415).
        std::vector<heisenberg::TriangleData> triangles;
        triangles.reserve(256);
        heisenberg::GetTriangles(state.node.get(), triangles);
        if (triangles.empty()) {
            spdlog::info("[GEOM-PLACE] FALLBACK: no triangles for '{}' - will use stored item offset", state.node->name.c_str());
            return false;
        }

        // HIGGS (hand.cpp:1372-1382, 1417-1425):
        //   originalTransform = collidableNode->m_worldTransform;
        //   adjustedTransform = originalTransform;
        //   if (shouldMoveHandBack) adjustedTransform.pos += palmDirection * pulledGrabHandAdjustDistance;
        //   triangles.ApplyTransform(adjustedTransform * inverse(originalTransform));
        // Rotation is identical on both sides, so the triangle adjustment is
        // the pure translation (adjusted.translate - live.translate).
        RE::NiTransform adjustedTransform = objectWorld;
        adjustedTransform.rotate = state.node->world.rotate;
        if (pulledArrival) {
            const float moveBack = heisenberg::g_config.pulledGrabHandAdjustDistance;
            if (std::abs(moveBack) > 1e-4f) {
                adjustedTransform.translate += palmDir * moveBack;
            }
        }
        const RE::NiPoint3 triangleShift =
            adjustedTransform.translate - state.node->world.translate;
        if (heisenberg::VectorLengthSquared(triangleShift) > 1e-8f) {
            for (auto& tri : triangles) {
                tri.v0 += triangleShift;
                tri.v1 += triangleShift;
                tri.v2 += triangleShift;
            }
        }

        // HIGGS GetClosestPointOnGraphicsGeometryToLine (hand.cpp:1432).
        RE::NiPoint3 closestPoint;
        RE::NiPoint3 closestNormal;
        int closestTriIndex = -1;
        float distToLine = 0.0f;
        const bool found = heisenberg::GetClosestPointOnGeometryToLine(
            triangles, palmPos, palmDir,
            closestPoint, closestNormal, closestTriIndex, distToLine);
        if (!found) {
            spdlog::info("[GEOM-PLACE] FALLBACK: no closest-point hit for '{}' - will use stored item offset", state.node->name.c_str());
            return false;
        }

        // HIGGS (hand.cpp:1545-1546):
        //   desiredNodeTransform = adjustedTransform;
        //   desiredNodeTransform.pos += palmPos - ptPos;
        RE::NiTransform desiredWorldTransform = adjustedTransform;
        desiredWorldTransform.translate += (palmPos - closestPoint);
        if (heisenberg::g_config.enableAxialPlacement &&
            std::abs(heisenberg::g_config.axialPlacementClearance) > 1.0e-4f) {
            desiredWorldTransform.translate += palmDir * heisenberg::g_config.axialPlacementClearance;
        }
        if (!std::isfinite(desiredWorldTransform.translate.x) ||
            !std::isfinite(desiredWorldTransform.translate.y) ||
            !std::isfinite(desiredWorldTransform.translate.z)) {
            spdlog::warn("[GEOM-PLACE] non-finite seat for '{}' - ignored", state.node->name.c_str());
            return false;
        }

        // hand.cpp:1547 `desiredNodeTransformHandSpace = inverseHand * desiredNodeTransform`,
        // stored against the same parent frame the palm was measured in.
        StoreRuntimeWorldPlacement(
            state,
            parentWorld,
            desiredWorldTransform.translate,
            desiredWorldTransform.rotate,
            usesSkinnedHand);

        spdlog::info(
            "[GEOM-PLACE] SURFACE-SNAP '{}': tris={} palm=({:.1f},{:.1f},{:.1f}) "
            "palmDir=({:.2f},{:.2f},{:.2f}) surface=({:.1f},{:.1f},{:.1f}) "
            "lineDist={:.2f} tri={} pulled={} moveBack={:.1f} frame={} "
            "local=({:.2f},{:.2f},{:.2f})",
            state.node->name.c_str(),
            triangles.size(),
            palmPos.x, palmPos.y, palmPos.z,
            palmDir.x, palmDir.y, palmDir.z,
            closestPoint.x, closestPoint.y, closestPoint.z,
            distToLine,
            closestTriIndex,
            pulledArrival,
            pulledArrival ? heisenberg::g_config.pulledGrabHandAdjustDistance : 0.0f,
            usesSkinnedHand ? "SKINNED" : "WAND",
            state.runtimeHandPlacementPosition.x,
            state.runtimeHandPlacementPosition.y,
            state.runtimeHandPlacementPosition.z);

        // Oracle comparison: what the offset database would have used for
        // this item. Only meaningful when the offset was authored in the same
        // (wand) frame; a skinned-frame seat is logged as such.
        {
            std::optional<heisenberg::ItemOffset> baseline;
            if (auto* refr = state.GetRefr()) {
                baseline = heisenberg::ItemOffsetManager::GetSingleton().GetOffsetWithFallback(refr, isLeft);
            }
            if (baseline.has_value()) {
                const auto& b = baseline.value();
                spdlog::info("[GEOM-PLACE] COMPARE baseline '{}': pos=({:.2f},{:.2f},{:.2f}) matched='{}' seatFrame={} d=({:+.2f},{:+.2f},{:+.2f})",
                            state.node->name.c_str(),
                            b.position.x, b.position.y, b.position.z,
                            b.matchedName,
                            usesSkinnedHand ? "SKINNED" : "WAND",
                            state.runtimeHandPlacementPosition.x - b.position.x,
                            state.runtimeHandPlacementPosition.y - b.position.y,
                            state.runtimeHandPlacementPosition.z - b.position.z);
            }
        }
        return true;
    }

    // Close-grab convenience: seat from the live node pose, no pull shift.
    bool TryCalculateRuntimeHandPlacementFromGeometry(heisenberg::GrabState& state, RE::NiAVObject* wandNode, bool isLeft)
    {
        if (!state.node) {
            state.ClearRuntimeHandPlacement();
            return false;
        }
        return TryCalculateRuntimeHandPlacementFromGeometry(
            state, wandNode, isLeft, state.node->world, /*pulledArrival=*/false);
    }

    // Remote pull: the object is still at arm's length when the grab starts,
    // but HIGGS seats a pulled object when it ARRIVES at the hand. Build that
    // arrival pose (bound centre at the palm, rotation unchanged) and seat
    // from it, so the pull animation's target is the surface-snapped seat and
    // not a multi-meter translate of the distant surface point.
    bool TryCalculatePulledArrivalPlacementFromGeometry(
        heisenberg::GrabState& state,
        RE::NiAVObject* wandNode,
        bool isLeft,
        const RE::NiTransform& objectWorld)
    {
        if (!state.node || !wandNode) {
            return false;
        }

        RE::NiTransform parentWorld{};
        RE::NiPoint3 palmPos{};
        RE::NiPoint3 palmDir{};
        bool usesSkinnedHand = false;
        if (!GetPalmSurfaceSeatFrame(
                wandNode,
                isLeft,
                parentWorld,
                palmPos,
                palmDir,
                usesSkinnedHand)) {
            return false;
        }

        RE::NiPoint3 centreWorld = objectWorld.translate;
        const float boundRadius = state.node->worldBound.fRadius;
        const RE::NiPoint3 boundCentre = state.node->worldBound.center;
        if (std::isfinite(boundRadius) &&
            boundRadius > 0.01f &&
            boundRadius < 10000.0f &&
            std::isfinite(boundCentre.x) &&
            std::isfinite(boundCentre.y) &&
            std::isfinite(boundCentre.z)) {
            // worldBound is measured on the live node; re-express its centre
            // relative to the pose we were asked to seat from.
            centreWorld = objectWorld.translate +
                          (boundCentre - state.node->world.translate);
        }

        RE::NiTransform arrivalWorld = objectWorld;
        arrivalWorld.translate = objectWorld.translate + (palmPos - centreWorld);
        return TryCalculateRuntimeHandPlacementFromGeometry(
            state, wandNode, isLeft, arrivalWorld, /*pulledArrival=*/true);
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
        if (rock::touch_grab_bridge::
                SolveAndPublishTouchGrabFingerPose(
                    state.node.get(),
                    handNode->world,
                    isLeft,
                    palmPos,
                    palmPos,
                    0.0f,
                    true,
                    &jointCurls)) {
            state.SetRuntimeJointCurls(jointCurls);
            state.rockRichFingerPosePublished = true;
            return true;
        }
        state.rockRichFingerPosePublished = false;
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
                // DEFERRED path - the counterpart to the IMMEDIATE one in StartGrab. Same
                // authored curls, applied post-physics once the object is seated at its final
                // placement. Tagged identically so the two are directly comparable in a log.
                spdlog::info(
                    "[GRAB-POSE] {} applied paired authored offset curls "
                    "(curls=DEFERRED, post-physics)",
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
        RE::NiMatrix3 renderedHandRotOrtho = renderedHand->world.rotate;
        heisenberg::OrthoNormalize(renderedHandRotOrtho);
        const RE::NiMatrix3 renderedLocalRot =
            desiredWorldRot *
            renderedHandRotOrtho.Transpose();

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
        return heisenberg::grab_ownership_policy::resolve(
                   heisenberg::g_config.grabMode,
                   heisenberg::g_config.delegateWorldGrabToRock,
                   heisenberg::IsRockEngineHosted())
            .effectiveGrabMode;
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

    static void ClearCapturedHeldBodyFrames(
        heisenberg::GrabState& state)
    {
        state.capturedHeldBodies = {};
        state.capturedHeldBodyCount = 0;
        state.capturedHeldBodySetValid = false;
        state.capturedHeldBodiesWorld = nullptr;
        state.instantPreTeleportBodyCaptureAttempted = false;
        state.capturedHeldBodiesExcludeAlternateWrappers = false;
        state.capturedHeldBodiesIncludeAlternateWrappers = false;
    }

    static RE::bhkWorld* ResolveMatchingHeldBhkWorld(
        const heisenberg::GrabState& state)
    {
        if (!state.collisionObject) {
            return nullptr;
        }
        void* liveHknpWorld = AccessWorld(state.collisionObject);
        if (!liveHknpWorld) {
            return nullptr;
        }

        const auto matches = [liveHknpWorld](RE::bhkWorld* candidate) {
            return candidate &&
                   heisenberg::Physics::GetHknpWorldFromBhk(candidate) ==
                       liveHknpWorld;
        };
        if (matches(state.lastSyncedBhkWorld)) {
            return state.lastSyncedBhkWorld;
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell()) {
                if (auto* playerWorld = cell->GetbhkWorld();
                    matches(playerWorld)) {
                    return playerWorld;
                }
            }
        }
        if (auto* refr = state.GetRefr()) {
            if (auto* referenceWorld = GetBhkWorldFromRefr(refr);
                matches(referenceWorld)) {
                return referenceWorld;
            }
        }
        return matches(state.savedState.savedBhkWorld)
            ? state.savedState.savedBhkWorld
            : nullptr;
    }

    static bool LockedWorldMatchesHeldCollision(
        const heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld,
        RE::hknpWorld* expectedHknpWorld)
    {
        return state.collisionObject &&
               bhkWorld &&
               expectedHknpWorld &&
               heisenberg::Physics::GetHknpWorldFromBhk(bhkWorld) ==
                   expectedHknpWorld &&
               AccessWorld(state.collisionObject) == expectedHknpWorld;
    }

    static bool CapturedHeldBodyStillOwned(
        const heisenberg::GrabState& state,
        RE::hknpWorld* world,
        const heisenberg::GrabState::CapturedHeldBody& captured)
    {
        if (!state.collisionObject ||
            !world ||
            state.capturedHeldBodiesWorld != world ||
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
        if (rock::havok_runtime::getOwnerNodeFromCollisionObject(
                captured.ownerCollisionObject) !=
            captured.ownerNode.get()) {
            return false;
        }
        if (body->motionIndex != captured.motionId ||
            !rock::havok_runtime::getMotion(
                world,
                captured.motionId)) {
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

    static bool CapturedHeldBodySetStillOwned(
        const heisenberg::GrabState& state)
    {
        if (!state.collisionObject ||
            !state.capturedHeldBodySetValid ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!world ||
            state.capturedHeldBodiesWorld != world) {
            return false;
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
                return false;
            }
        }
        return true;
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

    // Recursive subtree motion is safe only when a complete body-frame set was
    // captured. Any rigid-object capture failure stays on the selected wrapper
    // for setup, lag recovery, and release so uncaptured bodies are never left
    // KEYFRAMED or restored through a mismatched scope.
    static bool RequiresSelectedCollisionWrapperMotionScope(
        const heisenberg::GrabState& state)
    {
        const auto* refr = state.GetRefr();
        return refr &&
               refr->GetFormType() != RE::ENUM_FORM_ID::kACHR &&
               state.collisionObject &&
               !state.capturedHeldBodySetValid;
    }

    // Capture the union of every collision wrapper's physics system under this
    // reference root, including hidden variants on independent systems. A
    // multipart weapon can expose several simultaneously rendered wrappers,
    // while an ammo variant can keep alternate bodies hidden. Body identity is
    // unique for filter/cache ownership; motion identity is unique for
    // transform/velocity writes.
    //
    // Capture is transactional. Any unreadable wrapper/system/body clears the
    // complete set, forcing the selected-wrapper-only motion fallback. A
    // partial set must never be paired with recursive subtree keyframing.
    // Ragdoll/actor proxies remain outside this rigid clutter path.
    static bool CaptureHeldCollisionBodyFrames(heisenberg::GrabState& state)
    {
        ClearCapturedHeldBodyFrames(state);
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
        auto* bhkWorld =
            ResolveMatchingHeldBhkWorld(state);
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }
        heisenberg::Physics::WorldReadLock readLock(bhkWorld);
        if (!readLock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }

        const auto collisionWrappers =
            heisenberg::CollectCollisionWrappers(
                state.node.get());
        if (collisionWrappers.overflowed) {
            spdlog::warn(
                "[GRAB-MULTIBODY] Collision-wrapper capture skipped for "
                "{:08X}: wrapperOccurrences={} unique=>{} cap={}",
                state.GetRefr() ? state.GetRefr()->formID : 0,
                collisionWrappers.totalWrapperCount,
                collisionWrappers.count,
                heisenberg::kMaxCollisionWrappers);
            return false;
        }

        // Physical-touch acquisition can legitimately nominate a wrapper that
        // the geometry visibility walk did not classify. Scan it as placement
        // authority without falsely labelling it as rendered.
        std::array<
            RE::bhkNPCollisionObject*,
            heisenberg::kMaxCollisionWrappers + 1>
            scanWrappers{};
        std::uint32_t scanWrapperCount = 0;
        for (std::uint32_t i = 0;
             i < collisionWrappers.count;
             ++i) {
            scanWrappers[scanWrapperCount++] =
                collisionWrappers.wrappers[i];
        }
        if (!collisionWrappers.Contains(state.collisionObject)) {
            scanWrappers[scanWrapperCount++] =
                state.collisionObject;
        }

        struct CaptureContext
        {
            heisenberg::GrabState* state = nullptr;
            RE::hknpWorld* world = nullptr;
            const heisenberg::CollisionWrapperSet*
                collisionWrappers = nullptr;
            RE::NiTransform rootInverse{};
            std::uint32_t eligibleBodyCount = 0;
            std::uint32_t visibleWrapperBodyCount = 0;
            std::uint32_t hiddenWrapperBodyCount = 0;
            std::uint32_t duplicateBodySkips = 0;
            bool eligibleCaptureFailed = false;
        } context{
            &state,
            world,
            &collisionWrappers,
            rock::transform_math::invertTransform(state.node->world),
        };

        auto visitor = [](std::uint32_t bodyId, void* rawContext) {
            auto* context = static_cast<CaptureContext*>(rawContext);
            if (!context || !context->state || !context->world) {
                return false;
            }
            auto& state = *context->state;

            // Several visible wrappers can point at one shared physics system.
            // Keep exactly one record for each world-local body ID.
            for (std::uint32_t i = 0;
                 i < state.capturedHeldBodyCount;
                 ++i) {
                if (state.capturedHeldBodies[i].bodyId == bodyId) {
                    ++context->duplicateBodySkips;
                    return true;
                }
            }

            auto* body = rock::havok_runtime::getBody(
                context->world,
                RE::hknpBodyId{ bodyId });
            if (!body ||
                !rock::havok_runtime::getMotion(
                    context->world,
                    body->motionIndex)) {
                context->eligibleCaptureFailed = true;
                return false;
            }

            auto* ownerCollision =
                rock::havok_runtime::getCollisionObjectFromBody(body);
            if (!ownerCollision) {
                context->eligibleCaptureFailed = true;
                return false;
            }
            auto* resolvedOwnerCollision =
                heisenberg::TryResolveNpcCollisionObjectFromRaw(
                    ownerCollision);

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
            captured.isActiveWrapper =
                resolvedOwnerCollision == state.collisionObject;
            captured.isVisibleWrapper =
                context->collisionWrappers &&
                context->collisionWrappers->IsVisible(
                    resolvedOwnerCollision);
            if (captured.isVisibleWrapper) {
                ++context->visibleWrapperBodyCount;
            } else {
                ++context->hiddenWrapperBodyCount;
            }
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

        constexpr std::uint32_t kMaxPhysicsSystemBodiesToInspect = 64;
        std::array<
            const void*,
            heisenberg::kMaxCollisionWrappers + 1>
            scannedSystems{};
        std::uint32_t uniqueSystemCount = 0;
        std::uint32_t duplicateSystemSkips = 0;
        std::uint32_t totalSystemBodySlots = 0;
        std::uint32_t totalVisitedBodies = 0;
        std::uint32_t totalInvalidBodySkips = 0;
        std::uint32_t failedScanStatus =
            (std::numeric_limits<std::uint32_t>::max)();
        bool systemScanFailed = scanWrapperCount == 0;

        for (std::uint32_t wrapperIndex = 0;
             wrapperIndex < scanWrapperCount &&
             !systemScanFailed;
             ++wrapperIndex) {
            auto* wrapper = scanWrappers[wrapperIndex];
            auto* system = wrapper
                ? wrapper->spSystem.get()
                : nullptr;
            if (!wrapper || !system) {
                systemScanFailed = true;
                continue;
            }

            bool alreadyScanned = false;
            for (std::uint32_t i = 0;
                 i < uniqueSystemCount;
                 ++i) {
                if (scannedSystems[i] == system) {
                    alreadyScanned = true;
                    break;
                }
            }
            if (alreadyScanned) {
                ++duplicateSystemSkips;
                continue;
            }
            scannedSystems[uniqueSystemCount++] = system;

            const auto scan =
                rock::havok_runtime::
                    forEachPhysicsSystemBodyIdDetailed(
                        static_cast<RE::NiCollisionObject*>(
                            wrapper),
                        world,
                        kMaxPhysicsSystemBodiesToInspect,
                        visitor,
                        &context);
            if (scan.bodyCount > 0) {
                totalSystemBodySlots +=
                    static_cast<std::uint32_t>(
                        scan.bodyCount);
            }
            totalVisitedBodies += scan.visitedBodies;
            totalInvalidBodySkips +=
                scan.skippedInvalidBodies;
            if (!scan.enumerated() ||
                scan.bodyCount >
                    static_cast<std::int32_t>(
                        kMaxPhysicsSystemBodiesToInspect) ||
                scan.skippedInvalidBodies > 0) {
                failedScanStatus =
                    static_cast<std::uint32_t>(
                        scan.status);
                systemScanFailed = true;
            }
        }

        if (systemScanFailed ||
            context.eligibleCaptureFailed ||
            context.eligibleBodyCount >
                heisenberg::GrabState::kMaxCapturedHeldBodies ||
            state.capturedHeldBodyCount !=
                context.eligibleBodyCount ||
            state.capturedHeldBodyCount == 0) {
            spdlog::warn(
                "[GRAB-MULTIBODY] All-visible capture skipped: status={} "
                "wrappers={}/{} systems={} sharedSystemSkips={} "
                "systemBodies={} visited={} invalidIds={} eligible={} "
                "captured={} duplicateBodies={} visibleBodies={} "
                "hiddenBodies={} cap={}",
                failedScanStatus,
                collisionWrappers.count,
                collisionWrappers.totalWrapperCount,
                uniqueSystemCount,
                duplicateSystemSkips,
                totalSystemBodySlots,
                totalVisitedBodies,
                totalInvalidBodySkips,
                context.eligibleBodyCount,
                state.capturedHeldBodyCount,
                context.duplicateBodySkips,
                context.visibleWrapperBodyCount,
                context.hiddenWrapperBodyCount,
                heisenberg::GrabState::kMaxCapturedHeldBodies);
            ClearCapturedHeldBodyFrames(state);
            return false;
        }

        // setBodyTransformDeferred/setBodyVelocityDeferred both address a body
        // but mutate its motion. The pure policy chooses one placement
        // authority per shared motion and rejects duplicate body identity.
        std::array<
            heisenberg::held_collision_body_set_policy::BodyCandidate,
            heisenberg::GrabState::kMaxCapturedHeldBodies>
            motionCandidates{};
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured =
                state.capturedHeldBodies[i];
            motionCandidates[i] = {
                captured.bodyId,
                captured.motionId,
                captured.isActiveWrapper,
                captured.isVisibleWrapper,
            };
        }
        const auto motionSelection =
            heisenberg::held_collision_body_set_policy::
                SelectMotionRepresentatives(
                    motionCandidates,
                    state.capturedHeldBodyCount);
        if (!motionSelection.valid) {
            ClearCapturedHeldBodyFrames(state);
            return false;
        }
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            state.capturedHeldBodies[i]
                .isMotionRepresentative =
                motionSelection.representatives[i];
        }
        const auto uniqueMotionCount =
            motionSelection.uniqueMotionCount;

        state.capturedHeldBodiesWorld = world;
        state.capturedHeldBodySetValid = true;
        state.capturedHeldBodiesExcludeAlternateWrappers = false;
        state.capturedHeldBodiesIncludeAlternateWrappers =
            context.hiddenWrapperBodyCount > 0;
        // Each captured body owns its byte-exact originalFilterInfo. Do not seed
        // the single-body fallback from capturedHeldBodies[0]: enumeration order
        // is not the selected/primary wrapper and those scalar fields are never
        // consumed while a valid captured set exists.
        spdlog::info(
            "[GRAB-MULTIBODY] Captured all-visible set for {:08X} "
            "root='{}': wrappers={}/{} systems={} bodies={} motions={} "
            "visibleBodies={} hiddenBodies={} duplicateBodies={} "
            "sharedSystemSkips={}",
            state.GetRefr() ? state.GetRefr()->formID : 0,
            state.node->name.c_str(),
            collisionWrappers.count,
            collisionWrappers.totalWrapperCount,
            uniqueSystemCount,
            state.capturedHeldBodyCount,
            uniqueMotionCount,
            context.visibleWrapperBodyCount,
            context.hiddenWrapperBodyCount,
            context.duplicateBodySkips,
            duplicateSystemSkips);
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& body = state.capturedHeldBodies[i];
            spdlog::info(
                "[GRAB-MULTIBODY] body[{}]={} motion={} filter=0x{:08X} "
                "owner='{}' selected={} visible={} motionRep={} "
                "ownerRootLocal=({:.2f},{:.2f},{:.2f}) "
                "bodyOwnerLocal=({:.2f},{:.2f},{:.2f})",
                i,
                body.bodyId,
                body.motionId,
                body.originalFilterInfo,
                body.ownerNode
                    ? body.ownerNode->name.c_str()
                    : "NULL",
                body.isActiveWrapper,
                body.isVisibleWrapper,
                body.isMotionRepresentative,
                body.ownerRootLocal.translate.x,
                body.ownerRootLocal.translate.y,
                body.ownerRootLocal.translate.z,
                body.bodyOwnerLocal.translate.x,
                body.bodyOwnerLocal.translate.y,
                body.bodyOwnerLocal.translate.z);
        }
        return state.capturedHeldBodyCount > 0;
    }

    static void QueueDeferredFilterRestore(
        void* world,
        std::uint32_t bodyId,
        std::uint32_t filter,
        int frames);

    static bool ApplyCapturedHeldBodyFilters(
        heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld,
        bool keepRenderedWrappersCollidable)
    {
        const bool firstHeldFilterApply =
            !state.heldPlayerFilterApplied;
        if (!state.collisionObject ||
            !state.capturedHeldBodySetValid ||
            !bhkWorld ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }

        std::array<
            std::uint32_t,
            heisenberg::GrabState::kMaxCapturedHeldBodies>
            currentFilters{};
        std::array<
            std::uint32_t,
            heisenberg::GrabState::kMaxCapturedHeldBodies>
            desiredFilters{};

        // Validate and read the complete set before the first mutation.
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            auto& captured = state.capturedHeldBodies[i];
            if (!captured.valid ||
                !CapturedHeldBodyStillOwned(
                    state,
                    world,
                    captured)) {
                return false;
            }
            if (!heisenberg::Physics::TryReadBodyFilterInfo(
                    world,
                    captured.bodyId,
                    currentFilters[i])) {
                return false;
            }
            // PHANTOM-BODY FIX (Jul 25, user-reported knockover): bodies of alternate/
            // hidden wrappers (the invisible second Havok body of dual-body NIFs like
            // the ammo box) are teleport-dragged along the visible mesh every frame at
            // keyframed authority — on their native clutter layer they batted world
            // objects over invisibly. Park them on the non-collidable layer (15, the
            // same constant the held-object non-collidable path uses) for the capture
            // lifetime; the raw filter write preserves group/system bits, dodging the
            // known SetLayerLocked preset-clobber bug. Every rendered wrapper
            // keeps the intended bHeldObjectCollidable behavior; acquisition
            // authority remains collidable defensively even if the visibility
            // walk did not classify it. Restore is byte-exact from
            // originalFilterInfo on release. When the entire held object is
            // configured non-collidable, every captured body uses the same raw
            // layer-only rewrite so mixed native filters are still preserved.
            const bool renderedCollisionAuthority =
                keepRenderedWrappersCollidable &&
                heisenberg::held_collision_body_set_policy::
                    ShouldKeepNativeCollision(
                        captured.isActiveWrapper,
                        captured.isVisibleWrapper);
            desiredFilters[i] =
                heisenberg::held_collision_body_set_policy::
                    HeldBodyFilter(
                        currentFilters[i],
                        renderedCollisionAuthority);
        }

        // Commit transactionally. If one native write fails, restore every
        // earlier body to the exact filter observed at function entry.
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            if (desiredFilters[i] != currentFilters[i] &&
                !heisenberg::Physics::TryWriteBodyFilterInfo(
                    world,
                    state.capturedHeldBodies[i].bodyId,
                    desiredFilters[i])) {
                for (std::uint32_t rollback = 0;
                     rollback < i;
                     ++rollback) {
                    if (desiredFilters[rollback] !=
                        currentFilters[rollback]) {
                        const auto rollbackBodyId =
                            state.capturedHeldBodies[rollback]
                                .bodyId;
                        if (!heisenberg::Physics::
                                TryWriteBodyFilterInfo(
                                    world,
                                    rollbackBodyId,
                                    currentFilters[rollback])) {
                            QueueDeferredFilterRestore(
                                world,
                                rollbackBodyId,
                                currentFilters[rollback],
                                1);
                            spdlog::warn(
                                "[GRAB-MULTIBODY] Deferred failed "
                                "transaction rollback for body {} -> "
                                "0x{:08X}",
                                rollbackBodyId,
                                currentFilters[rollback]);
                        }
                    }
                }
                return false;
            }
        }

        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            auto& captured = state.capturedHeldBodies[i];
            // filterChanged must reflect the FULL divergence from the original filter
            // (the old hand-bits-only test under-reported once the layer changes too),
            // so RestoreCapturedHeldBodyFilters knows a byte-exact restore is needed.
            captured.filterChanged =
                desiredFilters[i] != captured.originalFilterInfo;
            if ((desiredFilters[i] & 0x7Fu) !=
                (currentFilters[i] & 0x7Fu)) {
                // A real layer transition must invalidate all cached pair
                // verdicts immediately.
                RebuildBodyCollisionCachesNative(world, captured.bodyId);
            }
        }
        if (firstHeldFilterApply) {
            std::uint32_t noCharacterControllerBodies = 0;
            std::uint32_t hiddenNonCollidableBodies = 0;
            for (std::uint32_t i = 0;
                 i < state.capturedHeldBodyCount;
                 ++i) {
                const auto layer =
                    desiredFilters[i] &
                    heisenberg::held_collision_body_set_policy::
                        kCollisionLayerMask;
                noCharacterControllerBodies +=
                    layer ==
                    heisenberg::held_collision_body_set_policy::
                        kHeldNoCharacterControllerLayer;
                hiddenNonCollidableBodies +=
                    layer ==
                    heisenberg::held_collision_body_set_policy::
                        kNonCollidableLayer;
            }
            spdlog::info(
                "[GRAB-FILTER] Leased held body set world={:p} "
                "bodies={} BIPED_NO_CC={} NONCOLLIDABLE={} "
                "(CHARCONTROLLER<->BIPED_NO_CC matrix pair disabled)",
                static_cast<void*>(world),
                state.capturedHeldBodyCount,
                noCharacterControllerBodies,
                hiddenNonCollidableBodies);
        }
        return true;
    }

    static bool RestoreCapturedHeldBodyFilters(
        heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld)
    {
        if (!state.collisionObject ||
            !bhkWorld ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
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
                captured.filterChanged = false;
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
            } else {
                // Do not discard the byte-exact restore ledger on a transient
                // native write failure. The deferred queue retries after the
                // current physics step, when release/world bookkeeping has
                // settled, and owns this entry from here.
                QueueDeferredFilterRestore(
                    world,
                    captured.bodyId,
                    captured.originalFilterInfo,
                    1);
                captured.filterChanged = false;
                spdlog::warn(
                    "[GRAB-MULTIBODY] Deferred failed filter restore for "
                    "body {} -> 0x{:08X}",
                    captured.bodyId,
                    captured.originalFilterInfo);
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
            !state.capturedHeldBodySetValid ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }

        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }

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

        std::uint32_t writtenMotionCount = 0;
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            if (!captured.isMotionRepresentative) {
                continue;
            }
            const RE::NiTransform desiredOwnerWorld =
                rock::transform_math::composeTransforms(
                    rootWorld,
                    captured.ownerRootLocal);
            const RE::NiTransform desiredBodyWorld =
                rock::transform_math::composeTransforms(
                    desiredOwnerWorld,
                    captured.bodyOwnerLocal);
            if (!rock::havok_runtime::setBodyTransformDeferred(
                    world,
                    captured.bodyId,
                    desiredBodyWorld,
                    1)) {
                return false;
            }
            ++writtenMotionCount;
        }
        return writtenMotionCount > 0;
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
            !state.capturedHeldBodySetValid ||
            state.capturedHeldBodyCount == 0) {
            return false;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return false;
        }

        // Release velocity must also be all-or-nothing by captured set, but a
        // shared motion receives exactly one native write. Writing every body
        // in that group would repeatedly overwrite the same hknp motion.
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

        std::uint32_t writtenMotionCount = 0;
        for (std::uint32_t i = 0;
             i < state.capturedHeldBodyCount;
             ++i) {
            const auto& captured = state.capturedHeldBodies[i];
            if (!captured.isMotionRepresentative) {
                continue;
            }
            if (!rock::havok_runtime::setBodyVelocityDeferred(
                    world,
                    captured.bodyId,
                    linear,
                    angular)) {
                return false;
            }
            ++writtenMotionCount;
        }
        return writtenMotionCount > 0;
    }

    static void RebuildCapturedHeldBodyCollisionCaches(
        heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld,
        float lookAheadHavok)
    {
        if (!state.collisionObject ||
            !bhkWorld ||
            state.capturedHeldBodyCount == 0) {
            return;
        }
        auto* world = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
            return;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                world)) {
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

    static void ClearExternalHeldBodiesForPlayerSuppression(
        heisenberg::GrabState& state,
        const bool isLeft)
    {
        // GrabState moves between hand slots during an atomic two-hand
        // promotion, so clear the slot recorded by the state rather than
        // assuming that its current array slot still owns the publication.
        // Once cleared, repeated failed refreshes remain no-ops.
        if (state.externalHeldBodiesPublished) {
            rock::HostClearExternalHeldBodies(
                state.externalHeldBodiesPublishedForLeft);
        }
        state.externalHeldBodiesPublished = false;
        state.externalHeldBodiesPublishedForLeft = isLeft;
    }

    // Publish exact host-owned body identities before KEYFRAMED authority is
    // enabled. The character-controller hook matches both hknpWorld and body
    // ID, so an authored native collision group can never be mistaken for a
    // held object. This publication is independent of the optional filter-word
    // rewrite below: even if that native write fails, the exact solver filter
    // still prevents the held body from displacing the player.
    static bool PublishExternalHeldBodiesForPlayerSuppression(
        heisenberg::GrabState& state,
        const bool isLeft)
    {
        const auto failWithoutReplacing = []() {
            /*
             * Publication is a replace-on-success transaction. A transient
             * validation/read failure must not erase the last complete exact
             * snapshot: during a handover that old hand slot still protects
             * the same keyframed body. Proven invalidation sites (world
             * rollover, body-sync failure, release/abort) clear explicitly.
             */
            return false;
        };

        if (!state.collisionObject ||
            !state.collisionObject->spSystem) {
            return failWithoutReplacing();
        }

        auto* const liveWorld =
            static_cast<RE::hknpWorld*>(
                AccessWorld(state.collisionObject));
        if (!liveWorld) {
            return failWithoutReplacing();
        }

        std::array<
            std::uint32_t,
            heisenberg::GrabState::kMaxCapturedHeldBodies>
            bodyIds{};
        std::uint32_t bodyCount = 0;

        if (state.capturedHeldBodySetValid &&
            state.capturedHeldBodyCount > 0) {
            if (state.capturedHeldBodiesWorld != liveWorld ||
                !CapturedHeldBodySetStillOwned(state)) {
                return failWithoutReplacing();
            }
            for (std::uint32_t i = 0;
                 i < state.capturedHeldBodyCount;
                 ++i) {
                const auto& captured =
                    state.capturedHeldBodies[i];
                if (!captured.valid ||
                    captured.bodyId == 0x7FFFFFFFu ||
                    captured.bodyId == 0xFFFFFFFFu) {
                    return failWithoutReplacing();
                }
                bodyIds[bodyCount++] = captured.bodyId;
            }
        } else {
            std::uint32_t selectedBodyId = 0x7FFFFFFFu;
            heisenberg::ConstraintFunctions::
                BhkPhysicsSystemGetBodyId(
                    state.collisionObject->spSystem.get(),
                    &selectedBodyId,
                    state.collisionObject->systemBodyIdx);
            if (selectedBodyId == 0x7FFFFFFFu ||
                selectedBodyId == 0xFFFFFFFFu) {
                return failWithoutReplacing();
            }
            bodyIds[bodyCount++] = selectedBodyId;
        }

        if (bodyCount == 0) {
            return failWithoutReplacing();
        }

        const bool firstPublication =
            !state.externalHeldBodiesPublished;
        const bool publicationChangedHand =
            state.externalHeldBodiesPublished &&
            state.externalHeldBodiesPublishedForLeft != isLeft;
        const bool clearPreviousHandAfterPublish =
            state.externalHeldBodiesPublished &&
            state.externalHeldBodiesPublishedForLeft != isLeft;
        const bool previousPublishedForLeft =
            state.externalHeldBodiesPublishedForLeft;

        // HostPublish swaps a complete double-buffered snapshot. During a
        // handover, publish the destination first and only then clear the old
        // source slot so the still-keyframed body is never unprotected between
        // the two operations.
        rock::HostPublishExternalHeldBodies(
            isLeft,
            liveWorld,
            bodyIds.data(),
            bodyCount);
        if (clearPreviousHandAfterPublish) {
            rock::HostClearExternalHeldBodies(
                previousPublishedForLeft);
        }
        state.externalHeldBodiesPublished = true;
        state.externalHeldBodiesPublishedForLeft = isLeft;
        if (firstPublication || publicationChangedHand) {
            spdlog::info(
                "[GRAB-FILTER] Published exact held-body registry "
                "world={:p} hand={} bodies={} firstBody={}",
                static_cast<void*>(liveWorld),
                isLeft ? "left" : "right",
                bodyCount,
                bodyIds[0]);
        }
        return true;
    }

    // Move only the exact held body to the BIPED_NO_CC layer while it is
    // keyframed. Fallout's vendored Havok wrapper documents this as the
    // grabbed-object path that avoids character-controller bumping. Bits 7-31
    // remain byte-identical and the complete original filter is restored on
    // release.
    static bool TryDisablePlayerHeldObjectCollision(
        heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld)
    {
        if (!bhkWorld) {
            return false;
        }
        if (state.capturedHeldBodyCount > 0 &&
            state.capturedHeldBodySetValid) {
            auto* liveWorld = state.collisionObject
                ? static_cast<RE::hknpWorld*>(AccessWorld(state.collisionObject))
                : nullptr;
            if (liveWorld &&
                state.capturedHeldBodiesWorld == liveWorld &&
                CapturedHeldBodySetStillOwned(state)) {
                if (ApplyCapturedHeldBodyFilters(
                        state,
                        bhkWorld,
                        true)) {
                    return true;
                }

                // Filter writes can fail transiently while every captured body
                // frame is still valid. Keep the transform/release authority:
                // discarding it here would make release restore only one body
                // even though setup recursively keyframed the whole subtree.
                spdlog::warn(
                    "[GRAB-MULTIBODY] Held-filter apply failed for an "
                    "otherwise valid {}-body set; retaining motion/release "
                    "authority for retry",
                    state.capturedHeldBodyCount);
                return false;
            }

            // Keep the per-body original filters as a release ledger even when
            // transform authority becomes unusable. Restore everything still
            // owned/readable now, then let the selected-body scalar path carry
            // collision suppression for the remainder of the hold.
            (void)RestoreCapturedHeldBodyFilters(
                state,
                bhkWorld);
            state.capturedHeldBodySetValid = false;
            spdlog::warn(
                "[GRAB-MULTIBODY] Retaining unusable captured set as a "
                "filter-restore ledger before "
                "held-filter apply (capturedWorld={:p} liveWorld={:p} bodies={}); "
                "falling back to selected body",
                state.capturedHeldBodiesWorld,
                static_cast<void*>(liveWorld),
                state.capturedHeldBodyCount);
        }
        const bool firstApply = !state.heldScalarFilterApplied;
        if (!state.collisionObject || !state.collisionObject->spSystem) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: collObj/spSystem null");
            return false;
        }
        auto* hknpWorld = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                hknpWorld)) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: AccessWorld null");
            return false;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                hknpWorld)) {
            if (firstApply) {
                spdlog::warn(
                    "[GRAB-FILTER] skip: held world changed or write "
                    "lock failed");
            }
            return false;
        }
        std::uint32_t objBodyId = 0x7FFFFFFF;
        heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
            state.collisionObject->spSystem.get(), &objBodyId, state.collisionObject->systemBodyIdx);
        if (objBodyId == 0x7FFFFFFF) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: GetBodyId returned invalid id");
            return false;
        }

        std::uint32_t cur = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objBodyId, cur)) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: TryReadBodyFilterInfo failed for body 0x{:08X}", objBodyId);
            return false;
        }
        if (firstApply) {
            state.heldOriginalFilterInfo = cur;
        }
        const std::uint32_t heldFilter =
            heisenberg::held_collision_body_set_policy::
                HeldBodyFilter(cur, true);
        if (heldFilter == cur) {
            state.heldScalarFilterApplied = true;
            if (firstApply) {
                spdlog::info(
                    "[GRAB-FILTER] body 0x{:08X} already uses "
                    "BIPED_NO_CC (filter=0x{:08X})",
                    objBodyId,
                    cur);
            }
            return true;
        }
        if (!heisenberg::Physics::TryWriteBodyFilterInfo(
                hknpWorld,
                objBodyId,
                heldFilter)) {
            if (firstApply) spdlog::warn("[GRAB-FILTER] skip: TryWriteBodyFilterInfo failed for body 0x{:08X}", objBodyId);
            return false;
        }
        RebuildBodyCollisionCachesNative(hknpWorld, objBodyId);
        state.heldScalarFilterApplied = true;
        if (firstApply) {
            spdlog::info(
                "[GRAB-FILTER] body 0x{:08X} filter 0x{:08X} -> "
                "0x{:08X} (temporary BIPED_NO_CC layer)",
                objBodyId,
                cur,
                heldFilter);
        }
        return true;
    }

    // Restore the exact pre-grab filter so released/thrown bodies regain their
    // authored layer and continue to hit actors and world objects normally.
    // Jul 18 v3: engine primitive that destroys+rebuilds a body's collision caches so stale
    // pair verdicts die immediately. hknpWorld::rebuildBodyCollisionCaches(hknpBodyId), PDB
    // VA 0x14153C5A0 (F4VR 1.2.72). SEH rule: Relocation lives outside any __try.
    static void RebuildBodyCollisionCachesNative(void* hknpWorld, std::uint32_t bodyId)
    {
        using Fn = void (*)(void*, std::uint32_t);
        static REL::Relocation<Fn> s_fn{ REL::Offset(0x153C5A0) };
        s_fn(hknpWorld, bodyId);
    }

    // Deferred byte-exact restore queue. A multipart object can expose up to
    // 64 bodies per hand, so reserve both hands' worst case; dropping a failed
    // restore would strand a temporary hold filter in the world.
    struct DeferredFilterRestore
    {
        void* world = nullptr;
        std::uint32_t bodyId = 0x7FFFFFFF;
        std::uint32_t filter = 0;
        int framesLeft = 0;
        int attemptsLeft = 0;
    };
    static DeferredFilterRestore g_deferredFilterRestores[128];
    static std::atomic<bool> g_hasDeferredFilterRestores{ false };

    static void QueueDeferredFilterRestore(void* world, std::uint32_t bodyId, std::uint32_t filter, int frames)
    {
        if (!world ||
            bodyId == 0x7FFFFFFFu ||
            bodyId == 0xFFFFFFFFu) {
            return;
        }
        for (auto& e : g_deferredFilterRestores) {
            if (e.framesLeft > 0 &&
                e.world == world &&
                e.bodyId == bodyId) {
                e.filter = filter;
                e.framesLeft = (std::max)(frames, 1);
                e.attemptsLeft = 8;
                g_hasDeferredFilterRestores.store(
                    true,
                    std::memory_order_release);
                return;
            }
        }
        for (auto& e : g_deferredFilterRestores) {
            if (e.framesLeft <= 0) {
                e = {
                    world,
                    bodyId,
                    filter,
                    (std::max)(frames, 1),
                    8
                };
                g_hasDeferredFilterRestores.store(
                    true,
                    std::memory_order_release);
                return;
            }
        }
        // Queue exhaustion should be impossible for two 64-body holds. Still
        // make one immediate recovery attempt and report a failure loudly.
        if (!heisenberg::Physics::TryWriteBodyFilterInfo(
                world,
                bodyId,
                filter)) {
            spdlog::error(
                "[GRAB-FILTER] Deferred restore queue full and immediate "
                "restore failed for body 0x{:08X}",
                bodyId);
        }
    }

    void TickDeferredFilterRestores(void* liveHknpWorld)
    {
        bool anyPending = false;
        for (auto& e : g_deferredFilterRestores) {
            if (e.framesLeft <= 0) continue;
            // Do not drop on world-pointer mismatch: the queue owns the exact
            // hknpWorld captured with the body ID, and body IDs are meaningful
            // only in that namespace.
            (void)liveHknpWorld;
            if (--e.framesLeft == 0) {
                if (heisenberg::Physics::TryWriteBodyFilterInfo(
                        e.world,
                        e.bodyId,
                        e.filter)) {
                    RebuildBodyCollisionCachesNative(
                        e.world,
                        e.bodyId);
                    spdlog::debug(
                        "[REL-DIAG] deferred filter restore body=0x{:08X} "
                        "-> 0x{:08X}",
                        e.bodyId,
                        e.filter);
                    e = {};
                } else if (--e.attemptsLeft > 0) {
                    e.framesLeft = 1;
                    anyPending = true;
                } else {
                    spdlog::error(
                        "[GRAB-FILTER] Exhausted deferred filter restore "
                        "retries for body 0x{:08X}",
                        e.bodyId);
                    e = {};
                }
            } else {
                anyPending = true;
            }
        }
        g_hasDeferredFilterRestores.store(
            anyPending,
            std::memory_order_release);
    }

    static void TryRestoreHeldObjectCollision(
        heisenberg::GrabState& state,
        RE::bhkWorld* bhkWorld)
    {
        if (!state.heldPlayerFilterApplied || !bhkWorld) return;
        if (state.capturedHeldBodyCount > 0) {
            (void)RestoreCapturedHeldBodyFilters(
                state,
                bhkWorld);
        }
        if (!state.heldScalarFilterApplied) return;
        if (!state.collisionObject || !state.collisionObject->spSystem) return;
        auto* hknpWorld = static_cast<RE::hknpWorld*>(
            AccessWorld(state.collisionObject));
        if (!LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                hknpWorld)) {
            return;
        }
        heisenberg::Physics::WorldWriteLock lock(bhkWorld);
        if (!lock.IsLocked() ||
            !LockedWorldMatchesHeldCollision(
                state,
                bhkWorld,
                hknpWorld)) {
            return;
        }
        std::uint32_t objBodyId = 0x7FFFFFFF;
        heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(
            state.collisionObject->spSystem.get(), &objBodyId, state.collisionObject->systemBodyIdx);
        if (objBodyId == 0x7FFFFFFF) return;

        std::uint32_t cur = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objBodyId, cur)) return;
        const std::uint32_t restored = state.heldOriginalFilterInfo;
        if (restored != cur) {
            if (heisenberg::Physics::TryWriteBodyFilterInfo(
                    hknpWorld,
                    objBodyId,
                    restored)) {
                RebuildBodyCollisionCachesNative(
                    hknpWorld,
                    objBodyId);
                spdlog::info("[GRAB-KEYFRAMED] Held object 0x{:08X} filter restored 0x{:08X} -> 0x{:08X} (layer={})",
                             objBodyId, cur, restored, restored & 0x7Fu);
            } else {
                QueueDeferredFilterRestore(
                    hknpWorld,
                    objBodyId,
                    restored,
                    1);
                spdlog::warn(
                    "[GRAB-KEYFRAMED] Deferred failed scalar filter "
                    "restore for body 0x{:08X}",
                    objBodyId);
            }
        }
        state.heldScalarFilterApplied = false;
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
    bool GetCleanTrackedPalmPosition(
        RE::NiAVObject* wandNode,
        bool isLeft,
        RE::NiPoint3& outPos)
    {
        return GetCleanTrackedPalmPositionImpl(
            wandNode,
            isLeft,
            outPos);
    }

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

    RE::TESObjectWEAP* GetPlayerEquippedRealWeapon()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) {
            return nullptr;
        }

        const RE::BSAutoReadLock inventoryLock{
            player->inventoryList->rwLock };
        for (auto& inventoryItem : player->inventoryList->data) {
            auto* weapon = inventoryItem.object ?
                inventoryItem.object->As<RE::TESObjectWEAP>() :
                nullptr;
            if (!weapon) {
                continue;
            }

            const auto weaponType = weapon->weaponData.type.get();
            if (weaponType == RE::WEAPON_TYPE::kGrenade ||
                weaponType == RE::WEAPON_TYPE::kMine) {
                continue;
            }

            for (auto* stack = inventoryItem.stackData.get(); stack;
                 stack = stack->nextStack.get()) {
                if (stack->IsEquipped()) {
                    return weapon;
                }
            }
        }
        return nullptr;
    }

    bool IsHandGrabbingRealWeapon(const bool isLeft)
    {
        auto isRealWeaponReference = [](RE::TESObjectREFR* refr) {
            auto* baseForm = refr ? refr->GetObjectReference() : nullptr;
            auto* weapon = baseForm ?
                baseForm->As<RE::TESObjectWEAP>() :
                nullptr;
            if (!weapon) {
                return false;
            }
            const auto weaponType = weapon->weaponData.type.get();
            return weaponType != RE::WEAPON_TYPE::kGrenade &&
                   weaponType != RE::WEAPON_TYPE::kMine;
        };

        auto& grabManager = GrabManager::GetSingleton();
        if (grabManager.IsGrabbing(isLeft) &&
            isRealWeaponReference(
                grabManager.GetGrabState(isLeft).GetRefr())) {
            return true;
        }

        // Full-dynamic mode owns loose world objects inside ROCK rather than
        // GrabManager, so query its exact held-object snapshot as the fallback.
        rock::HostHeldObjectSnapshot snapshot{};
        return rock::HostGetHeldObjectSnapshot(isLeft, snapshot) &&
               isRealWeaponReference(snapshot.ref);
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
    bool StoreGrabbedItem(
        RE::TESObjectREFR* refr,
        bool showHudMessage = true,
        RE::TESForm** outStoredBaseForm = nullptr)
    {
        if (!refr)
            return false;

        // ActivateRef may synchronously remove the world reference. Keep the
        // object alive for the duration of this function, and capture every
        // value needed after activation before calling into the engine.
        RE::NiPointer<RE::TESObjectREFR> refrHolder(refr);
        refr = refrHolder.get();
        const RE::TESFormID storedRefID = refr->GetFormID();
        RE::TESForm* baseForm = refr->GetObjectReference();
        if (outStoredBaseForm) {
            *outStoredBaseForm = baseForm;
        }
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return false;
        
        // IMPORTANT: Get item name BEFORE ActivateRef - the reference may be deleted/cleaned up after!
        // Use base form name (TESFullName) to avoid HUD frameworks appending component/material tags
        // to GetDisplayFullName() (e.g. "Desk Fan [Steel, Screw]" instead of "Desk Fan")
        std::string itemName;
        if (showHudMessage) {
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
                         storeFormType == RE::ENUM_FORM_ID::kBOOK ? "Book/magazine" : "Holotape", storedRefID);
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
            spdlog::info("[STORE] Stored item {:08X} '{}' x{} to inventory", storedRefID, itemName, extraCount);

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
            spdlog::warn("[STORE] ActivateRef failed for {:08X} '{}'", storedRefID, itemName);
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

        // Programmatic loot appears at the controller without a deliberate
        // hand motion. Give it a longer grace so seated hands resting together
        // cannot immediately self-inject the new item.
        float elapsed = static_cast<float>(heisenberg::Utils::GetTime()) - state.grabStartTime;
        if (!heisenberg::consumable_use_policy::mayUseHeldConsumable(
                elapsed,
                state.isFromLootDrop,
                state.consumeZoneExitRequired)) {
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

        // Use the same delivery-aware grace as the mouth path. This is the
        // seated-play case: loot materializes in one lap hand while the other
        // hand is already inside the injection sphere.
        float elapsed = static_cast<float>(heisenberg::Utils::GetTime()) - state.grabStartTime;
        if (!heisenberg::consumable_use_policy::mayUseHeldConsumable(
                elapsed,
                state.isFromLootDrop,
                state.consumeZoneExitRequired))
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

    // Helper: Check if held item is an injectable (syringe-type chem).
    //
    // The English display-name whitelist this used to be was silently broken in every
    // localised game: a Spanish Stimpak reads "Estimulante" and matched nothing, so
    // injection simply did not work. It also missed every mod-added medical item, which is
    // how the Sim Settlements 2 disease cure ("Cura de enfermedades") fell through.
    //
    // SmartGrabHandler::CategorizeItem already solves this properly: it reads the item's
    // MAGIC EFFECT ARCHETYPES (kStimpak, kCureDisease, kCureAddiction, ...) plus keywords,
    // which are language-independent and mod-independent. The name test is kept only as a
    // last-resort fallback for items whose effects do not classify.
    bool IsInjectable(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || !baseForm->IsAlchemyItem()) return false;

        if (auto* bound = static_cast<RE::TESBoundObject*>(baseForm)) {
            const auto cat = heisenberg::SmartGrabHandler::GetSingleton().CategorizeItem(bound);
            using C = heisenberg::SmartGrabCategory;
            // SYRINGE-DELIVERED ONLY. This set decides which items are wrist-injected, and
            // it is exclusive: anything listed here is BLOCKED from mouth consumption while
            // hand injection is on. Including the whole chem family was wrong — Rad-X is a
            // swallowed pill and Jet is an inhaler, so tagging them CombatChem/RadResistance
            // made them refuse the mouth zone while only accepting a wrist gesture the player
            // has no reason to try. Keep this list to items that really are injectors.
            const auto injectables = C::Stimpack | C::RadAway | C::Antibiotic | C::Addictol;
            if (static_cast<std::uint32_t>(cat.categories & injectables) != 0) {
                return true;
            }
        }

        // Per-item injectors that no CATEGORY can pick out. Psycho is a syringe but shares the
        // CombatChem category with Jet, an inhaler — so the category is the wrong granularity
        // and only an item-level test works. Editor IDs are non-localised (unlike display
        // names, which is the bug this whole classifier replaced) and stable across the base
        // game plus most mod-added variants, which conventionally keep the vanilla stem.
        if (const char* editorId = baseForm->GetFormEditorID()) {
            static constexpr const char* kInjectorEditorIdStems[] = {
                "Psycho",   // covers PsychoJet, PsychoBuff, PsychoTats, ...
            };
            for (const char* stem : kInjectorEditorIdStems) {
                if (heisenberg::ContainsCI(editorId, stem)) {
                    return true;
                }
            }
        }

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

    // =========================================================================
    // NPC INJECTION - SS2 DISEASE CURES + NATIVE COMPANION STIMPAKS
    // =========================================================================
    // Scope is deliberately narrow (owner directive): this exists so a sick settler can be
    // cured by physically pressing the cure against them instead of opening the trade menu.
    // It is NOT a general "hand any item to any NPC" gesture. The only second
    // route is an exact native Stimpak touching a wounded current companion;
    // arbitrary chems and ordinary NPCs still do nothing.
    //
    // SS2 authors this particular cure with script-archetype effects. Resolve
    // its identity from the live manager property, then call SS2's own
    // menuless disease-cure function rather than trying to imitate only half
    // of a ContainerMenu trade.
    //
    // Deliberately NOT a proximity-only test: the cure must be close to the actor's actual
    // rendered body, so walking past a settler holding one cannot donate it.
    // SS2's generic Disease Cure uses script-archetype effects, so the normal
    // kCureDisease/Antibiotic classifier cannot identify it. Compare exact form
    // identity with NPC_RPGManager's live DiseaseCureForm property. This also
    // follows SS2 patches that redirect the property to a compatible item and
    // rejects similarly named per-disease internal potions.
    bool IsDiseaseCureItemImpl(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || !baseForm->IsAlchemyItem()) return false;

        return heisenberg::ss2::IsDiseaseCureForm(baseForm);
    }

    bool IsCompanionStimpakItemImpl(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        auto* baseForm = refr->GetObjectReference();
        if (!baseForm || !baseForm->IsAlchemyItem()) return false;

        // Do not infer Stimpaks from localized names, keywords, models, or
        // broad injectable categories. This is the same native classifier the
        // companion activation path uses.
        return heisenberg::MenuGameSettings_IsStimpak.get()(*baseForm);
    }

    heisenberg::npc_injection_policy::CompanionEvidence
        GetPlayerCompanionEvidence(
            RE::Actor* actor,
            RE::PlayerCharacter* player)
    {
        heisenberg::npc_injection_policy::CompanionEvidence evidence{};
        if (!actor || !player) return evidence;

        evidence.commandedFlag =
            actor->boolFlags.all(
                RE::Actor::BOOL_FLAGS::kIsCommandedActor);

        if (actor->currentProcess &&
            actor->currentProcess->middleHigh) {
            auto& commandingActor =
                actor->currentProcess->middleHigh->commandingActor;
            if (commandingActor) {
                const auto commandingActorPtr = commandingActor.get();
                evidence.commandedByPlayer =
                    commandingActorPtr &&
                    commandingActorPtr.get() ==
                        reinterpret_cast<RE::Actor*>(player);
            }
        }

        auto* playerActor = reinterpret_cast<RE::Actor*>(player);
        if (playerActor->currentProcess &&
            playerActor->currentProcess->middleHigh) {
            for (std::uint32_t i = 0;
                 i < playerActor->currentProcess->middleHigh->
                         commandedActors.size();
                 ++i) {
                auto& commandData =
                    playerActor->currentProcess->middleHigh->
                        commandedActors[i];
                const auto commandedActor =
                    commandData.commandedActor.get();
                if (commandedActor &&
                    commandedActor.get() == actor) {
                    evidence.inPlayerCommandList = true;
                    break;
                }
            }
        }

        // Base-game CurrentCompanionFaction. This catches ordinary followers
        // such as Codsworth, whose current-companion state is not necessarily
        // represented by one of the commanded-actor process flags.
        if (auto* factionForm =
                RE::TESForm::GetFormByID(0x0001C21C)) {
            if (auto* faction = factionForm->As<RE::TESFaction>()) {
                evidence.inCurrentCompanionFaction =
                    actor->IsInFaction(faction);
            }
        }
        return evidence;
    }

    bool IsWoundedCurrentPlayerCompanion(
        RE::Actor* actor,
        RE::PlayerCharacter* player)
    {
        if (!actor || !player ||
            actor == reinterpret_cast<RE::Actor*>(player) ||
            actor->GetFormID() == player->GetFormID()) {
            return false;
        }

        const auto evidence =
            GetPlayerCompanionEvidence(actor, player);
        const bool inBleedoutAnimation =
            actor->boolFlags.all(
                RE::Actor::BOOL_FLAGS::kInBleedoutAnimation);
        return heisenberg::npc_injection_policy::
            AllowsCompanionStimpakInjection(
                true,
                true,
                evidence,
                actor->lifeState,
                inBleedoutAnimation);
    }

    enum class InjectionTargetKind : std::uint8_t
    {
        DiseaseCure,
        WoundedCompanion,
    };

    RE::Actor* FindInjectionTargetActor(
        bool isLeft,
        RE::NiAVObject* heldNode,
        InjectionTargetKind targetKind,
        float& outDistance)
    {
        outDistance = -1.0f;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        auto* procLists = heisenberg::GetProcessListsSingleton();
        if (!player || !playerNodes || !procLists) return nullptr;

        RE::NiNode* holdingWand = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!holdingWand) return nullptr;
        // The reverse-engineered ProcessLists ABI takes its point by mutable
        // reference even though it is logically an input.
        RE::NiPoint3 tip = holdingWand->world.translate;

        const auto toPolicyPoint = [](const RE::NiPoint3& point) {
            return heisenberg::npc_injection_policy::Point3{
                point.x,
                point.y,
                point.z,
            };
        };

        // The wand is permanent evidence. A valid nearby held-object bound is
        // an optional SECOND probe, never a replacement: an unloaded/stale
        // scene bound must not make contact at the controller disappear. The
        // pure policy rejects non-finite, negative, oversized, and implausibly
        // far bounds instead of clamping them into a remote cure sphere.
        bool hasHeldBound = false;
        RE::NiPoint3 heldBoundCenter{};
        float heldBoundRadius = 0.0f;
        if (heldNode) {
            const auto& bound = heldNode->worldBound;
            hasHeldBound = true;
            heldBoundCenter = bound.center;
            heldBoundRadius = bound.fRadius;
        }
        auto contactProbes =
            heisenberg::npc_injection_policy::MakeContactProbeSet(
                toPolicyPoint(tip),
                hasHeldBound,
                toPolicyPoint(heldBoundCenter),
                heldBoundRadius);
        if (targetKind == InjectionTargetKind::WoundedCompanion) {
            // Companion revival is object-contact driven. The permanent wand
            // probe is useful for the older Disease Cure gesture, but must not
            // revive someone merely because the controller is nearby while
            // the visible Stimpak itself is not touching them.
            if (contactProbes.count < 2) {
                return nullptr;
            }
            contactProbes.probes[0] = contactProbes.probes[1];
            contactProbes.count = 1;
        }

        // Cast a generous net, then rank every candidate by rendered torso
        // distance. The old closest-actor helper usually returned PlayerRef
        // first (or a seated actor's foot marker), rejected that one result,
        // and never considered the sick settler actually being touched.
        RE::BSScrapArray<RE::NiPointer<RE::Actor>> actors;
        const float searchRadius = heisenberg::g_config.npcInjectionRadius + 150.0f;
        heisenberg::ProcessLists_GetActorsWithinRangeOfPoint(
            procLists,
            tip,
            searchRadius,
            actors);

        RE::NiPointer<RE::Actor> nearest;
        // SS2's older disease-cure gesture intentionally retains its generous
        // configurable proximity radius.  Companion revival is different: the
        // held Stimpak sphere must reach the rendered body surface.  Its probe
        // radius is already subtracted by the distance helpers, so only a tiny
        // numerical/one-frame tolerance belongs here.
        constexpr float kCompanionSurfaceToleranceGameUnits = 1.5f;
        float nearestDistance =
            targetKind == InjectionTargetKind::WoundedCompanion ?
                kCompanionSurfaceToleranceGameUnits :
                heisenberg::g_config.npcInjectionRadius;
        for (auto& candidate : actors) {
            RE::Actor* actor = candidate.get();
            if (!actor) {
                continue;
            }
            const bool eligible =
                targetKind == InjectionTargetKind::DiseaseCure ?
                    heisenberg::npc_injection_policy::IsEligibleNpcTarget(
                        reinterpret_cast<std::uintptr_t>(actor),
                        reinterpret_cast<std::uintptr_t>(player),
                        actor->formID,
                        actor->IsDead(true)) :
                    IsWoundedCurrentPlayerCompanion(actor, player);
            if (!eligible) {
                continue;
            }

            auto* root = actor->Get3D(false);
            if (!root) {
                continue;
            }

            // Measure against connected torso/arm capsules rather than isolated
            // bone points. Touching the midpoint of a forearm or the space
            // between spine joints is real body contact even when neither
            // endpoint lies inside the configured radius.
            float actorDistance = std::numeric_limits<float>::infinity();
            struct BodyPoint
            {
                RE::NiPoint3 point{};
                bool valid = false;
            };
            const auto resolveBodyPoint = [&](const char* boneName) {
                BodyPoint result{};
                if (auto* bone = root->GetObjectByName(boneName)) {
                    result.point = bone->world.translate;
                    result.valid = std::isfinite(result.point.x) &&
                                   std::isfinite(result.point.y) &&
                                   std::isfinite(result.point.z);
                }
                return result;
            };

            const std::array<BodyPoint, 6> torsoPoints{
                resolveBodyPoint("Head"),
                resolveBodyPoint("Neck"),
                resolveBodyPoint("Chest"),
                resolveBodyPoint("SPINE2"),
                resolveBodyPoint("SPINE1"),
                resolveBodyPoint("Pelvis"),
            };
            const std::array<BodyPoint, 6> leftArmPoints{
                resolveBodyPoint("LArm_Collarbone"),
                resolveBodyPoint("LArm_UpperArm"),
                resolveBodyPoint("LArm_ForeArm1"),
                resolveBodyPoint("LArm_ForeArm2"),
                resolveBodyPoint("LArm_ForeArm3"),
                resolveBodyPoint("LArm_Hand"),
            };
            const std::array<BodyPoint, 6> rightArmPoints{
                resolveBodyPoint("RArm_Collarbone"),
                resolveBodyPoint("RArm_UpperArm"),
                resolveBodyPoint("RArm_ForeArm1"),
                resolveBodyPoint("RArm_ForeArm2"),
                resolveBodyPoint("RArm_ForeArm3"),
                resolveBodyPoint("RArm_Hand"),
            };
            const std::array<BodyPoint, 5> leftLegPoints{
                resolveBodyPoint("Pelvis"),
                resolveBodyPoint("LLeg_Thigh"),
                resolveBodyPoint("LLeg_Calf"),
                resolveBodyPoint("LLeg_Foot"),
                resolveBodyPoint("LLeg_Toe1"),
            };
            const std::array<BodyPoint, 5> rightLegPoints{
                resolveBodyPoint("Pelvis"),
                resolveBodyPoint("RLeg_Thigh"),
                resolveBodyPoint("RLeg_Calf"),
                resolveBodyPoint("RLeg_Foot"),
                resolveBodyPoint("RLeg_Toe1"),
            };

            bool foundBodyCapsule = false;
            const auto measureChain = [&](const auto& points, float radius) {
                const BodyPoint* previous = nullptr;
                std::size_t validCount = 0;
                for (const auto& point : points) {
                    if (!point.valid) {
                        continue;
                    }
                    ++validCount;
                    if (previous) {
                        foundBodyCapsule = true;
                        actorDistance = (std::min)(
                            actorDistance,
                            heisenberg::npc_injection_policy::
                                MinimumProbeDistanceToCapsule(
                                    contactProbes,
                                    {
                                        .start = toPolicyPoint(previous->point),
                                        .end = toPolicyPoint(point.point),
                                        .radius = radius,
                                    }));
                    }
                    previous = &point;
                }
                if (validCount == 1 && previous) {
                    foundBodyCapsule = true;
                    const auto point = toPolicyPoint(previous->point);
                    actorDistance = (std::min)(
                        actorDistance,
                        heisenberg::npc_injection_policy::
                            MinimumProbeDistanceToCapsule(
                                contactProbes,
                                {
                                    .start = point,
                                    .end = point,
                                    .radius = radius,
                                }));
                }
            };
            measureChain(torsoPoints, 7.0f);
            measureChain(leftArmPoints, 4.0f);
            measureChain(rightArmPoints, 4.0f);
            if (targetKind ==
                InjectionTargetKind::WoundedCompanion) {
                measureChain(leftLegPoints, 4.0f);
                measureChain(rightLegPoints, 4.0f);
            }

            // Companion revival accepts a Stimpak touching the rendered actor
            // anywhere, not just the historical torso/arm injection capsules.
            // Bone capsules above are cheap, animation-current coverage for
            // ordinary humanoids. If they did not already prove contact, use
            // the actor's actual scene geometry so clothing, feet, creatures,
            // and nonstandard skeletons remain valid touch targets.
            bool hasActorMeshCoverage = false;
            bool probesNearActorBound = true;
            const auto& actorBound = root->worldBound;
            if (std::isfinite(actorBound.center.x) &&
                std::isfinite(actorBound.center.y) &&
                std::isfinite(actorBound.center.z) &&
                std::isfinite(actorBound.fRadius) &&
                actorBound.fRadius >= 0.0f &&
                actorBound.fRadius <= 200.0f) {
                const auto boundCenter =
                    toPolicyPoint(actorBound.center);
                const float boundDistance =
                    heisenberg::npc_injection_policy::
                        MinimumProbeDistanceToCapsule(
                            contactProbes,
                            {
                                .start = boundCenter,
                                .end = boundCenter,
                                .radius = actorBound.fRadius,
                            });
                probesNearActorBound =
                    std::isfinite(boundDistance) &&
                    boundDistance <= nearestDistance + 10.0f;
            }
            if (targetKind == InjectionTargetKind::WoundedCompanion &&
                probesNearActorBound &&
                (!std::isfinite(actorDistance) ||
                 actorDistance > nearestDistance)) {
                std::vector<heisenberg::TriangleData> actorTriangles;
                constexpr std::size_t kActorMeshTriangleBudget = 50000;
                heisenberg::GetTriangles(
                    root,
                    actorTriangles,
                    kActorMeshTriangleBudget);
                hasActorMeshCoverage = !actorTriangles.empty();
                if (hasActorMeshCoverage) {
                    const std::size_t probeCount = (std::min)(
                        contactProbes.count,
                        contactProbes.probes.size());
                    for (std::size_t probeIndex = 0;
                         probeIndex < probeCount;
                         ++probeIndex) {
                        const auto& probe =
                            contactProbes.probes[probeIndex];
                        RE::NiPoint3 probePoint{
                            probe.center.x,
                            probe.center.y,
                            probe.center.z,
                        };
                        RE::NiPoint3 meshPoint{};
                        float meshDistance =
                            std::numeric_limits<float>::infinity();
                        if (heisenberg::GetClosestMeshPointToPoint(
                                actorTriangles,
                                probePoint,
                                meshPoint,
                                meshDistance) &&
                            std::isfinite(meshDistance)) {
                            actorDistance = (std::min)(
                                actorDistance,
                                (std::max)(
                                    0.0f,
                                    meshDistance - probe.radius));
                        }
                    }
                }
            }

            const bool needsFallbackCoverage =
                targetKind == InjectionTargetKind::WoundedCompanion ?
                    (!foundBodyCapsule && !hasActorMeshCoverage) :
                    !foundBodyCapsule;
            if (needsFallbackCoverage) {
                // If exact scene geometry is unavailable, use the complete
                // actor 3D bound only for an unusual companion that exposes
                // neither recognized skeleton capsules nor mesh triangles.
                // Never let that broad bound replace real coverage. Disease
                // cures retain their narrower historical fallback.
                RE::NiPoint3 bodyPoint = root->worldBound.center;
                if (!std::isfinite(bodyPoint.x) ||
                    !std::isfinite(bodyPoint.y) ||
                    !std::isfinite(bodyPoint.z)) {
                    bodyPoint = root->world.translate;
                }
                const bool fullActorFallback =
                    targetKind ==
                    InjectionTargetKind::WoundedCompanion;
                const float fallbackBodyRadius =
                    fullActorFallback ? 35.0f : 8.0f;
                const float maximumFallbackBodyRadius =
                    fullActorFallback ? 100.0f : 15.0f;
                const float rawRadius = root->worldBound.fRadius;
                const float bodyRadius =
                    std::isfinite(rawRadius) && rawRadius >= 0.0f &&
                            rawRadius <= 200.0f ?
                        (std::min)(rawRadius,
                                   maximumFallbackBodyRadius) :
                        fallbackBodyRadius;
                const auto point = toPolicyPoint(bodyPoint);
                actorDistance = (std::min)(
                    actorDistance,
                    heisenberg::npc_injection_policy::
                        MinimumProbeDistanceToCapsule(
                            contactProbes,
                            {
                                .start = point,
                                .end = point,
                                .radius = bodyRadius,
                            }));
            }

            if (std::isfinite(actorDistance) &&
                actorDistance <= nearestDistance) {
                nearest = candidate;
                nearestDistance = actorDistance;
            }
        }

        if (!nearest) {
            return nullptr;
        }
        outDistance = nearestDistance;
        return nearest.get();
    }

    // SS2's OnItemRemoved handler only records the trade target; it actually
    // treats that actor when ContainerMenu later closes. A physical gesture has
    // no menu-close event, so invoke NPC_RPGManager's authoritative menuless
    // path and let SS2 perform disease checks, consume one cure, or refund it.
    heisenberg::NpcInjectionResult InjectHeldItemIntoActor(
        RE::TESObjectREFR* refr,
        RE::Actor* target,
        const std::function<bool()>& prepareInventoryCommit)
    {
        if (!refr || !target) {
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* baseForm = refr->GetObjectReference();
        if (!player || !baseForm) {
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        if (!heisenberg::npc_injection_policy::IsEligibleNpcTarget(
                reinterpret_cast<std::uintptr_t>(target),
                reinterpret_cast<std::uintptr_t>(player),
                target->formID,
                target->IsDead(true))) {
            spdlog::error(
                "[INJECT-NPC] Refused invalid/player injection target {:08X}",
                target->formID);
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        auto* bound = baseForm->As<RE::TESBoundObject>();
        if (!bound) {
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        std::int32_t count = 1;
        if (refr->extraList) {
            if (auto* extraData = refr->extraList->GetByType(RE::EXTRA_DATA_TYPE::kCount)) {
                count = *reinterpret_cast<std::int16_t*>(
                    reinterpret_cast<std::uint8_t*>(extraData) + 0x18);
            }
        }
        if (count < 1) count = 1;

        std::string targetName = "settler";
        if (auto* npcBase = target->GetObjectReference()) {
            auto nameView = RE::TESFullName::GetFullName(*npcBase, false);
            if (!nameView.empty()) targetName = std::string(nameView);
        }
        const std::string itemName = heisenberg::ItemOffsetManager::GetItemName(refr);

        const auto result = heisenberg::ss2::DispatchDiseaseCure(
            target,
            baseForm,
            [&]() {
                // Dynamic ROCK must surrender the live body/constraint before
                // this world ref is placed in inventory and disabled. A false
                // return aborts the SS2 call without mutating either owner.
                if (prepareInventoryCommit &&
                    !prepareInventoryCommit()) {
                    return false;
                }
                heisenberg::Hooks::SetSuppressHUDMessages(true);
                try {
                    RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                    heisenberg::AddObjectToContainer(
                        player,
                        bound,
                        &nullExtra,
                        count,
                        nullptr,
                        0);
                    SafeDisableRef(refr);
                    heisenberg::Hooks::SetSuppressHUDMessages(false);
                    return true;
                } catch (...) {
                    heisenberg::Hooks::SetSuppressHUDMessages(false);
                    throw;
                }
            });

        if (result ==
            heisenberg::ss2::DiseaseCureDispatchResult::PreflightFailed) {
            spdlog::warn(
                "[INJECT-NPC] SS2 preflight failed for {:08X}; keeping '{}' in hand",
                target->formID,
                itemName);
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        if (result == heisenberg::ss2::DiseaseCureDispatchResult::Accepted) {
            // This is intentionally warning-level: the owner's active log
            // level is warning, and a one-line target audit is essential for
            // proving that medicine went to the settler rather than PlayerRef.
            spdlog::warn(
                "[INJECT-NPC] DISPATCH QUEUED: SS2 accepted one '{}' for target {:08X} '{}'; "
                "held stack x{} was transferred to PlayerRef for TryToUseCureItemOnActor (Papyrus decides cure/refund asynchronously)",
                itemName,
                target->formID,
                targetName,
                count);
            if (heisenberg::g_config.showInjectionMessages) {
                const std::string msg =
                    std::format("Treatment queued for {}", targetName);
                heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
            }
            return heisenberg::NpcInjectionResult::Accepted;
        }

        spdlog::error(
            "[INJECT-NPC] SS2 rejected the cure call for {:08X}; '{}' remains in player inventory",
            target->formID,
            itemName);
        if (heisenberg::g_config.showInjectionMessages) {
            const std::string msg = std::format(
                "Injection failed; {} returned to inventory",
                itemName);
            heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
        }
        return heisenberg::NpcInjectionResult::ReturnedToInventory;
    }

    heisenberg::NpcInjectionResult InjectHeldStimpakIntoCompanion(
        RE::TESObjectREFR* refr,
        RE::Actor* target,
        const std::function<bool()>& prepareInventoryCommit)
    {
        if (!refr || !target) {
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* baseForm = refr->GetObjectReference();
        if (!player || !baseForm ||
            !heisenberg::g_config.enableCompanionStimpakInjection ||
            !IsCompanionStimpakItemImpl(refr) ||
            !IsWoundedCurrentPlayerCompanion(target, player)) {
            spdlog::warn(
                "[INJECT-COMPANION] Commit revalidation failed for target {:08X}; keeping held Stimpak in hand",
                target->GetFormID());
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        auto* bound = baseForm->As<RE::TESBoundObject>();
        if (!bound) {
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        std::int32_t count = 1;
        if (refr->extraList) {
            if (auto* extraData = refr->extraList->GetByType(
                    RE::EXTRA_DATA_TYPE::kCount)) {
                count = *reinterpret_cast<std::int16_t*>(
                    reinterpret_cast<std::uint8_t*>(extraData) + 0x18);
            }
        }
        if (count < 1) count = 1;

        std::string targetName = "companion";
        if (auto* npcBase = target->GetObjectReference()) {
            const auto nameView =
                RE::TESFullName::GetFullName(*npcBase, false);
            if (!nameView.empty()) targetName = std::string(nameView);
        }
        const std::string itemName =
            heisenberg::ItemOffsetManager::GetItemName(refr);

        // Dynamic ROCK owns a live body/constraint until this exact boundary.
        // If it cannot surrender that ownership, abort before either inventory
        // or actor state can change.
        if (prepareInventoryCommit && !prepareInventoryCommit()) {
            spdlog::error(
                "[INJECT-COMPANION] Exact held-object ownership handoff failed for target {:08X}; keeping item in hand",
                target->GetFormID());
            return heisenberg::NpcInjectionResult::FailedKeptInHand;
        }

        // The normal companion activation expects a Stimpak in PlayerRef's
        // inventory. Return the held world stack first, without consuming it.
        // From this point on a failure is intentionally inventory-safe: the
        // item remains in PlayerRef and we never attempt a manual rollback or
        // a second consumption path.
        heisenberg::Hooks::SetSuppressHUDMessages(true);
        try {
            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
            heisenberg::AddObjectToContainer(
                player,
                bound,
                &nullExtra,
                count,
                nullptr,
                0);
            SafeDisableRef(refr);
            heisenberg::DropToHand::GetSingleton().
                MarkAsRecentlyStored(baseForm->GetFormID());
            heisenberg::Hooks::SetSuppressHUDMessages(false);
        } catch (...) {
            heisenberg::Hooks::SetSuppressHUDMessages(false);
            throw;
        }

        if (!heisenberg::ActorUtils_PlayerCanStimpak.get()(*target)) {
            spdlog::warn(
                "[INJECT-COMPANION] Native PlayerCanStimpak rejected {:08X} '{}'; '{}' remains in player inventory",
                target->GetFormID(),
                targetName,
                itemName);
            if (heisenberg::g_config.showInjectionMessages) {
                const std::string message = std::format(
                    "Cannot revive {}; {} returned to inventory",
                    targetName,
                    itemName.empty() ? "Stimpak" : itemName);
                heisenberg::Hooks::ShowHUDMessageDirect(message.c_str());
            }
            return heisenberg::NpcInjectionResult::ReturnedToInventory;
        }

        // This is the normal A/X companion activation. The internal-activation
        // scope bypasses only Heisenberg's held-object input guard; Fallout
        // still owns the revive, one-Stimpak consumption, animation, dialogue,
        // and every native/Papyrus observer of that action.
        bool activated = false;
        heisenberg::Hooks::SetInternalActivation(true);
        try {
            activated = target->ActivateRef(
                player,
                nullptr,
                1,
                false,
                false,
                false);
            heisenberg::Hooks::SetInternalActivation(false);
        } catch (...) {
            heisenberg::Hooks::SetInternalActivation(false);
            throw;
        }

        if (!activated) {
            spdlog::error(
                "[INJECT-COMPANION] Native ActivateRef rejected {:08X} '{}'; '{}' remains in player inventory",
                target->GetFormID(),
                targetName,
                itemName);
            if (heisenberg::g_config.showInjectionMessages) {
                const std::string message = std::format(
                    "Revive failed; {} returned to inventory",
                    itemName.empty() ? "Stimpak" : itemName);
                heisenberg::Hooks::ShowHUDMessageDirect(message.c_str());
            }
            return heisenberg::NpcInjectionResult::ReturnedToInventory;
        }

        spdlog::warn(
            "[INJECT-COMPANION] NATIVE REVIVE ACCEPTED: target {:08X} '{}', held '{}' stack x{} returned to PlayerRef before ActivateRef; engine owns one-Stimpak consumption",
            target->GetFormID(),
            targetName,
            itemName,
            count);
        if (heisenberg::g_config.showInjectionMessages) {
            const std::string message =
                std::format("Revived {}", targetName);
            heisenberg::Hooks::ShowHUDMessageDirect(message.c_str());
        }
        return heisenberg::NpcInjectionResult::Accepted;
    }

    heisenberg::NpcInjectionAttempt TryInjectHeldDiseaseCureImpl(
        bool isLeft,
        RE::TESObjectREFR* refr,
        RE::NiAVObject* heldNode,
        float heldSeconds,
        float handSpeedMetersPerSecond,
        const std::function<bool()>& prepareInventoryCommit)
    {
        heisenberg::NpcInjectionAttempt attempt{};
        if (!heisenberg::g_config.enableNpcInjection) {
            attempt.gate = heisenberg::NpcInjectionGate::FeatureDisabled;
            return attempt;
        }
        if (!IsDiseaseCureItemImpl(refr)) {
            attempt.gate = heisenberg::NpcInjectionGate::NotDiseaseCure;
            return attempt;
        }

        float injectDistance = -1.0f;
        RE::Actor* target = FindInjectionTargetActor(
            isLeft,
            heldNode,
            InjectionTargetKind::DiseaseCure,
            injectDistance);
        if (!target) {
            attempt.gate = heisenberg::NpcInjectionGate::NoTargetContact;
            return attempt;
        }

        attempt.targetFormID = target->formID;
        attempt.targetDistanceGameUnits = injectDistance;
        if (!std::isfinite(heldSeconds) || heldSeconds < 0.5f) {
            attempt.gate = heisenberg::NpcInjectionGate::HoldAge;
            return attempt;
        }
        if (!std::isfinite(handSpeedMetersPerSecond) ||
            handSpeedMetersPerSecond >=
                heisenberg::g_config.mouthVelocityThreshold) {
            attempt.gate = heisenberg::NpcInjectionGate::HandSpeed;
            return attempt;
        }

        attempt.gate = heisenberg::NpcInjectionGate::Dispatch;
        spdlog::warn(
            "[INJECT-NPC] Disease Cure pressed against {:08X} at {:.1f}gu "
            "(hand={}, held={:.2f}s speed={:.2f}m/s) — dispatching SS2 cure",
            target->formID,
            injectDistance,
            isLeft ? "left" : "right",
            heldSeconds,
            handSpeedMetersPerSecond);
        attempt.result = InjectHeldItemIntoActor(
            refr,
            target,
            prepareInventoryCommit);
        if (attempt.result ==
            heisenberg::NpcInjectionResult::FailedKeptInHand) {
            spdlog::warn(
                "[INJECT-NPC] Injection dispatch failed for {:08X}; keeping item in hand",
                target->formID);
        }
        return attempt;
    }

    heisenberg::NpcInjectionAttempt TryInjectHeldCompanionStimpakImpl(
        bool isLeft,
        RE::TESObjectREFR* refr,
        RE::NiAVObject* heldNode,
        float heldSeconds,
        float handSpeedMetersPerSecond,
        const std::function<bool()>& prepareInventoryCommit)
    {
        heisenberg::NpcInjectionAttempt attempt{};
        if (!heisenberg::g_config.enableCompanionStimpakInjection) {
            attempt.gate = heisenberg::NpcInjectionGate::FeatureDisabled;
            return attempt;
        }
        if (!IsCompanionStimpakItemImpl(refr)) {
            attempt.gate =
                heisenberg::NpcInjectionGate::NotCompanionStimpak;
            return attempt;
        }
        if (!heldNode && refr) {
            heldNode = refr->Get3D();
        }

        float injectDistance = -1.0f;
        RE::Actor* target = FindInjectionTargetActor(
            isLeft,
            heldNode,
            InjectionTargetKind::WoundedCompanion,
            injectDistance);
        if (!target) {
            attempt.gate =
                heisenberg::NpcInjectionGate::NoTargetContact;
            return attempt;
        }

        attempt.targetFormID = target->GetFormID();
        attempt.targetDistanceGameUnits = injectDistance;

        attempt.gate = heisenberg::NpcInjectionGate::Dispatch;
        spdlog::warn(
            "[INJECT-COMPANION] Stimpak pressed against wounded companion {:08X} at {:.1f}gu (hand={}, held={:.2f}s speed={:.2f}m/s) - invoking native revive activation",
            target->GetFormID(),
            injectDistance,
            isLeft ? "left" : "right",
            heldSeconds,
            handSpeedMetersPerSecond);
        attempt.result = InjectHeldStimpakIntoCompanion(
            refr,
            target,
            prepareInventoryCommit);
        return attempt;
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
    
    // Weapon-equip contact rule (Jul 30, user): a weapon held in the OFF hand
    // equips when its collision actually TOUCHES the weapon hand's collision —
    // not when their origins come within some radius of each other. The old
    // 15-unit fingertip proximity test fired across a visible gap (and fired
    // instantly on a Pip-Boy drop, where the item materialises next to the
    // weapon hand), so contact is both the stricter and the more intuitive rule.
    //
    // "Touching" is evaluated against the SAME generated hand hull Havok
    // collides with, via HostCopyHandCollisionSamples — the boundary spheres of
    // the weapon hand's colliders — measured against the held weapon's visible
    // mesh. A small skin absorbs the gap between the render mesh and the hull.
    constexpr std::uint32_t kWeaponEquipHandSampleLimit = 384;
    constexpr float kWeaponEquipContactSkin = 1.0f;
    constexpr std::uint32_t kWeaponEquipTouchEvidenceMaxAgeFrames = 4;

    // Minimum separation between the weapon hand's generated collision hull and
    // the held weapon's visible mesh, in game units. Negative means the hull is
    // inside the mesh. Returns false when the measurement cannot be made at all
    // (no hull published yet, or no triangles) — callers must NOT treat that as
    // a touch.
    bool TryMeasureWeaponHandHullSeparation(
        heisenberg::GrabState& state,
        bool weaponHandIsLeft,
        float& outSeparation)
    {
        if (!state.node) {
            return false;
        }

        static std::array<RE::NiPoint3, kWeaponEquipHandSampleLimit> handPoints;
        static std::array<float, kWeaponEquipHandSampleLimit> handRadii;
        const std::uint32_t handSampleCount =
            rock::HostCopyHandCollisionSamples(
                weaponHandIsLeft,
                handPoints.data(),
                handRadii.data(),
                kWeaponEquipHandSampleLimit);
        if (handSampleCount == 0) {
            return false;
        }

        // Broadphase: the hull is a hand, the mesh can be a rifle. Reject on the
        // world bound before transforming thousands of vertices every frame.
        const float boundRadius = state.node->worldBound.fRadius;
        const RE::NiPoint3 boundCentre = state.node->worldBound.center;
        const bool boundUsable =
            std::isfinite(boundRadius) &&
            boundRadius > 0.0f &&
            boundRadius < 10000.0f &&
            std::isfinite(boundCentre.x) &&
            std::isfinite(boundCentre.y) &&
            std::isfinite(boundCentre.z);
        if (boundUsable) {
            bool anySampleInRange = false;
            for (std::uint32_t i = 0; i < handSampleCount; ++i) {
                const float reach =
                    boundRadius + handRadii[i] + kWeaponEquipContactSkin;
                if (heisenberg::Utils::VectorLength(handPoints[i] - boundCentre) <= reach) {
                    anySampleInRange = true;
                    break;
                }
            }
            if (!anySampleInRange) {
                outSeparation = (std::numeric_limits<float>::max)();
                return true;
            }
        }

        std::vector<heisenberg::TriangleData> triangles;
        triangles.reserve(1024);
        heisenberg::GetTriangles(state.node.get(), triangles, 4096);
        if (triangles.empty()) {
            return false;
        }

        float best = (std::numeric_limits<float>::max)();
        for (std::uint32_t i = 0; i < handSampleCount; ++i) {
            const RE::NiPoint3& sample = handPoints[i];
            if (!std::isfinite(sample.x) ||
                !std::isfinite(sample.y) ||
                !std::isfinite(sample.z)) {
                continue;
            }
            RE::NiPoint3 closest{};
            float distance = -1.0f;
            if (!heisenberg::GetClosestMeshPointToPoint(
                    triangles,
                    sample,
                    closest,
                    distance) ||
                !std::isfinite(distance)) {
                continue;
            }
            const float radius =
                (std::isfinite(handRadii[i]) && handRadii[i] > 0.0f) ? handRadii[i] : 0.0f;
            const float separation = distance - radius;
            if (separation < best) {
                best = separation;
            }
        }
        if (best == (std::numeric_limits<float>::max)()) {
            return false;
        }
        outSeparation = best;
        return true;
    }

    // Decide whether a weapon held in the OFF hand should be equipped, using the
    // contact rule described above: the weapon's collision must be TOUCHING the
    // weapon hand's collision. outMeasuredSeparation receives the measured hull
    // separation in game units when one was computed (stays at max otherwise).
    bool CheckWeaponEquipByHandContact(
        heisenberg::GrabState& state,
        float handSpeed,
        bool holdingInLeftHand,
        float* outMeasuredSeparation = nullptr)
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
        float separation = (std::numeric_limits<float>::max)();
        bool measured = false;
        bool havokContact = false;
        bool result = false;
        const char* reason = "";

        if (holdingInPrimaryHand) {
            // You must grab the weapon with the OFF-hand and bring it to the WEAPON hand —
            // you can't hand a weapon to the hand already holding it.
            reason = "BLOCKED: weapon grabbed with PRIMARY (weapon) hand — grab with the OFF-hand";
        } else if (handSpeed >= speedMax) {
            reason = "BLOCKED: hand moving too fast";
        } else {
            // 1) Havok's own contact evidence for the weapon hand, when the
            //    engine already reports it touching this exact reference.
            if (auto* heldRef = state.GetRefr()) {
                RE::TESObjectREFR* touchedRef = nullptr;
                std::uint32_t touchedBodyId = 0;
                std::uint32_t touchedAgeFrames = 0;
                RE::NiPoint3 touchedPoint{};
                bool hasTouchedPoint = false;
                if (rock::HostGetHandTouchEvidence(
                        weaponHandIsLeft,
                        kWeaponEquipTouchEvidenceMaxAgeFrames,
                        &touchedRef,
                        &touchedBodyId,
                        &touchedAgeFrames,
                        &touchedPoint,
                        &hasTouchedPoint) &&
                    touchedRef == heldRef) {
                    havokContact = true;
                }
            }

            // 2) Exact hull-vs-mesh separation. This is the primary test: a held
            //    body is keyframed onto a temporary layer while held, so Havok
            //    contact events for it cannot be relied on as the only signal.
            measured = TryMeasureWeaponHandHullSeparation(
                state,
                weaponHandIsLeft,
                separation);
            if (outMeasuredSeparation && measured) {
                *outMeasuredSeparation = separation;
            }

            if (havokContact) {
                result = true;
                reason = "DETECTED (havok contact) — will equip on release";
            } else if (!measured) {
                reason = "BLOCKED: no hand collision hull or mesh to measure";
            } else if (separation <= kWeaponEquipContactSkin) {
                result = true;
                reason = "DETECTED (hull touch) — will equip on release";
            } else {
                reason = "not touching the weapon hand's collision";
            }
        }

        static int weqLogCtr = 0;
        if (result || ++weqLogCtr >= 30) {
            weqLogCtr = 0;
            spdlog::info("[WEAP-EQUIP] grabHand={} usingOffHand={} speed={:.2f}(max{:.2f}) havokContact={} measured={} separation={:.2f}gu(need<={:.2f}) => {}",
                         holdingInLeftHand ? "LEFT" : "RIGHT", !holdingInPrimaryHand, handSpeed, speedMax,
                         havokContact, measured,
                         measured ? separation : -1.0f, kWeaponEquipContactSkin, reason);
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

        // Defense in depth: the SS2 cure is NPC-only. Even if a caller bypasses
        // the zone router, never return the held unit to PlayerRef's inventory
        // and never invoke EquipObject/DrinkPotion on the player for this form.
        if (heisenberg::ss2::IsDiseaseCureForm(baseForm)) {
            spdlog::warn(
                "[CONSUME] Blocked player self-use of SS2 Disease Cure {:08X}; keeping it held",
                baseForm->GetFormID());
            if (heisenberg::g_config.showConsumeMessages) {
                heisenberg::Hooks::ShowHUDMessageDirect(
                    "Disease Cure can only be used on a settler");
            }
            return false;
        }
        
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
        
        // Inventory count BEFORE anything happens, with the held unit still out in the world.
        // Kept as a number, not a bool: the accounting check at the end needs the magnitude to
        // prove exactly one unit was used rather than merely that some were.
        int countBefore = 0;
        bool hasInInventory = false;
        auto* inventory = player->inventoryList;
        if (inventory) {
            for (auto& item : inventory->data) {
                if (item.object && item.object->GetFormID() == baseForm->GetFormID()) {
                    countBefore = item.GetCount();
                    hasInInventory = countBefore > 0;
                    break;
                }
            }
        }
        
        // ── THE HELD UNIT ALWAYS GOES INTO INVENTORY FIRST ──────────────────────────
        // Consuming what is in your hand must reduce your total possession by exactly ONE.
        // The held world reference IS one unit, so there are only two self-consistent
        // recipes: (a) put it in inventory, then have the game consume one from inventory,
        // or (b) destroy the world ref and apply effects WITHOUT consuming from inventory.
        //
        // The old code branched on whether a spare existed and picked (b) when it did -
        // Disable() the held ref, then DrinkPotion, which removes nothing. That was correct
        // for DrinkPotion. It became a DOUBLE consume the moment consumption moved to
        // EquipObject, because EquipObject genuinely consumes a unit from inventory: the
        // held one was destroyed AND an inventory one was eaten. Two Jets for one use.
        //
        // Why it looked Pip-Boy-dependent: consume-to-hand takes the item FROM inventory, so
        // with 2+ of something a spare always remains and the buggy branch is taken. Grab a
        // loose one off the world with only that one to your name and there is no spare, so
        // the correct branch ran. The Pip-Boy is just how items come from inventory - the
        // real predictor was "do you own a spare", which is why the last one always worked.
        //
        // Recipe (a) for every case: uniform, and it is exactly what the game does when you
        // pick an item up and use it.
        {
            // Mark as recently stored so harvest-to-hand doesn't re-grab after ActivateRef
            heisenberg::DropToHand::GetSingleton().MarkAsRecentlyStored(baseForm->GetFormID());

            heisenberg::Hooks::SetSuppressHUDMessages(true);
            heisenberg::Hooks::SetInternalActivation(true);
            bool activateResult = refr->ActivateRef(player, nullptr, 1, false, false, false);
            heisenberg::Hooks::SetInternalActivation(false);
            heisenberg::Hooks::SetSuppressHUDMessages(false);
            spdlog::debug("[CONSUME] Held unit returned to inventory (ActivateRef={}) for {:08X}, hadSpare={}",
                activateResult, refr->formID, hasInInventory);
        }

        // ── CONSUME VIA THE NATIVE "USE ITEM" PATH ──────────────────────────────────
        // DrinkPotion applies the alchemy effects and nothing else. That is enough for
        // health/rads/addiction, but NOT for Survival: hunger and thirst are driven by a
        // script layer that watches the EQUIP pipeline, so a drink performed with
        // DrinkPotion is invisible to it. Live evidence - a Parched player drank Purified
        // Water, '[CONSUME] Successfully consumed' logged, and in the following 90 seconds
        // the Survival system did not equip a single status token, while 11 seconds EARLIER
        // it had equipped five of them in 100ms. It was alive; it simply never saw the drink.
        //
        // EquipObject is what the game itself calls when you use an item from the Pip-Boy,
        // so it produces exactly the events Survival expects, and it consumes the item.
        // g_internalEquip stops our own consumable-to-hand hook from bouncing it back to
        // the hand. DrinkPotion stays as a fallback: if the equip path fails for any reason
        // the item must still be eaten, or the player loses it for nothing.
        bool drinkResult = false;
        bool consumedViaEquip = false;
        if (auto* equipMgr = RE::ActorEquipManager::GetSingleton()) {
            // Same shape the armour/weapon equip paths use. Declared locally because the
            // engine's BGSObjectInstance layout is what EquipObject expects and CommonLib's
            // definition is not directly constructible here.
            // Layout must match the armour/weapon equip paths exactly (see ~5527) - the
            // static_assert is what guarantees we hand EquipObject the shape it expects.
            struct LocalObjectInstance {
                RE::TESForm* object{ nullptr };
                RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
            };
            static_assert(sizeof(LocalObjectInstance) == 0x10);

            LocalObjectInstance instance;
            instance.object = alchemyItem;

            heisenberg::Hooks::SetInternalEquip(true);
            consumedViaEquip = heisenberg::ActorEquipManager_EquipObject(
                equipMgr,
                player,
                reinterpret_cast<RE::BGSObjectInstance*>(&instance),
                0,        // stackID
                1,        // count
                nullptr,  // slot - the game resolves it for a consumable
                false,    // queueEquip: consume now, not next frame
                false,    // forceEquip
                true,     // playSounds
                true,     // applyNow
                false);   // locked
            heisenberg::Hooks::SetInternalEquip(false);
            spdlog::debug("[CONSUME] EquipObject returned {} for '{}'", consumedViaEquip, itemName);
        }

        if (!consumedViaEquip) {
            drinkResult = heisenberg::Actor_DrinkPotion(player, alchemyItem, 1);
            spdlog::warn("[CONSUME] EquipObject path unavailable/failed for '{}' - fell back to "
                         "DrinkPotion. Effects apply, but Survival hunger/thirst will NOT update.",
                         itemName);
        } else {
            drinkResult = true;
        }

        // ONLY on the DrinkPotion fallback. The held unit is now always in inventory (above),
        // so exactly one unit must leave it. EquipObject already removes one itself - removing
        // here too would eat TWO. DrinkPotion applies effects without removing anything, so on
        // that path this removal is what balances the books.
        if (!consumedViaEquip) {
            auto* boundObj = baseForm->As<RE::TESBoundObject>();
            if (boundObj) {
                RE::TESObjectREFR::RemoveItemData removeData(boundObj, 1);
                removeData.reason = RE::ITEM_REMOVE_REASON::kNone;
                heisenberg::Hooks::SetSuppressHUDMessages(true);
                player->RemoveItem(removeData);
                heisenberg::Hooks::SetSuppressHUDMessages(false);
                spdlog::debug("[CONSUME] Removed 1x '{}' from inventory (DrinkPotion fallback path)", itemName);
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

        // Count the item AFTER the whole sequence. Net possession must have fallen by exactly
        // one: countBefore was taken with the held unit still OUT of inventory, so the correct
        // end state is countBefore (the held unit came in, one was consumed). Anything lower
        // means a second unit was eaten - the double-consume returning.
        {
            int countAfter = 0;
            if (auto* inv = player->inventoryList) {
                for (auto& item : inv->data) {
                    if (item.object && item.object->GetFormID() == baseForm->GetFormID()) {
                        countAfter = item.GetCount();
                        break;
                    }
                }
            }
            const int expected = countBefore;   // held unit added, one consumed
            if (countAfter != expected) {
                spdlog::warn("[CONSUME] ACCOUNTING MISMATCH for '{}': inventory before={} after={} "
                             "expected={} (viaEquip={}) — more than one unit was used",
                             itemName, countBefore, countAfter, expected, consumedViaEquip);
            } else {
                spdlog::info("[CONSUME] Successfully consumed '{}' (inventory {} -> {}, viaEquip={})",
                             itemName, countBefore, countAfter, consumedViaEquip);
            }
        }
        
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
        struct ClearExternalHeldBodiesOnExit
        {
            bool isLeft;
            ~ClearExternalHeldBodiesOnExit() noexcept
            {
                rock::HostClearExternalHeldBodies(isLeft);
            }
        } clearExternalHeldBodiesOnExit{ isLeft };

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
        // the temporary held-object layer (or CSR suppression) forever and state.Clear()
            // wipes heldOriginalFilterInfo so it can never be undone. The helper is internally
            // guarded (null + SEH) so it's safe even if the body is partway gone.
            if (auto* restoreWorld =
                    ResolveMatchingHeldBhkWorld(state)) {
                TryRestoreHeldObjectCollision(
                    state,
                    restoreWorld);
            }
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

        // Restore the exact pre-grab filter before release. The temporary
        // BIPED_NO_CC layer stops the held body from shoving the player and must
        // not survive into a normal drop or throw.
        //
        // WORLD LOCK (Jul 20): this file's own invariant (see the *Locked wrapper comments
        // below) is that all physics modifications must go through a world write lock -
        // this cluster (filter restore + native cache rebuild + CCD look-ahead) was the one
        // release-path mutation that skipped it, racing background cell streaming or other
        // in-flight Havok tasks that hold the SAME lock while mutating the same hknpWorld.
        // The held reference's parentCell can remain stale after it is detached
        // for a grab. lastSyncedBhkWorld is driven from the player's live cell
        // and is therefore the authority after a mid-hold transition.
        RE::bhkWorld* poserLockWorld =
            ResolveMatchingHeldBhkWorld(state);
        if (poserLockWorld) {
            TryRestoreHeldObjectCollision(
                state,
                poserLockWorld);

            // PAIR-CACHE POKE (Jul 18): even with byte-exact filter restore through the native
            // setter, the hand<->object narrowphase pair CREATED while the hold suppressed it
            // keeps its cached no-collide verdict as long as the two stay overlapping — the
            // "must pull my hand back a few cm before it collides" symptom. Force the pair to be
            // destroyed+rebuilt after restoring the exact authored filter.
            if (state.capturedHeldBodyCount > 0) {
                RebuildCapturedHeldBodyCollisionCaches(
                    state,
                    poserLockWorld,
                    0.25f);
                spdlog::debug(
                    "[REL-DIAG] rebuilt collision caches for {} "
                    "captured held bodies",
                    state.capturedHeldBodyCount);
            } else if (state.collisionObject &&
                       state.collisionObject->spSystem) {
                auto* pokeWorld = static_cast<RE::hknpWorld*>(
                    AccessWorld(state.collisionObject));
                heisenberg::Physics::WorldWriteLock pokeLock(
                    poserLockWorld);
                if (pokeLock.IsLocked() &&
                    LockedWorldMatchesHeldCollision(
                        state,
                        poserLockWorld,
                        pokeWorld)) {
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
        } else {
            spdlog::warn(
                "[GRAB-KEYFRAMED] Skipped filter/cache restoration: "
                "no bhkWorld matches the held collision object's hknpWorld");
        }

        // Restore physics state
        if (state.collisionObject && IsCollisionObjectValid(state.collisionObject))
        {
            RE::bhkWorld* bhkWorld =
                ResolveMatchingHeldBhkWorld(state);
            
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
            
            // Restore the exact motion scope used at setup. This persisted bit
            // is independent of the live capture/filter ledger, so actors and
            // a multipart set invalidated mid-hold still restore symmetrically.
            const bool selectedWrapperScope =
                !state.heldMotionScopeIsReferenceSubtree;
            if (selectedWrapperScope) {
                SetMotionTypeLocked(
                    state.collisionObject,
                    RE::hknpMotionPropertiesId::Preset::DYNAMIC,
                    bhkWorld);
            } else {
                bhkWorld_SetMotionLocked(
                    state.node.get(),
                    RE::hknpMotionPropertiesId::Preset::DYNAMIC,
                    true,
                    true,
                    true,
                    bhkWorld);
            }
            spdlog::debug(
                "[GRAB-KEYFRAMED] Restored motion type to DYNAMIC "
                "(scope={})",
                selectedWrapperScope
                    ? "active collision wrapper"
                    : "reference subtree");
            
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
            
            // Restore collision layer through the legacy preset path ONLY when
            // that path changed it (kNonCollidable=15). FAITHFULNESS FIX
            // (2026-07-05 audit rank 7): this used to run
            // unconditionally with a never-captured default of 4, clobbering the byte-exact
            // filter restore above (TryRestoreHeldObjectCollision) — SetLayerLocked rewrites
            // the filter from layer presets, wiping group/system bits, and mis-layered any
            // object whose native layer wasn't 4 (weapons=5, debris=19/20, clutter-large=29).
            // audit rank 7c: also skip when the exact held-object filter was applied — the
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
        if (state.lastSyncedBhkWorld) {
            return state.lastSyncedBhkWorld;
        }
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
        // WORLD scale — outTransform is a WORLD transform (see the two lines above). Reading
        // local.scale here and letting UpdateKeyframedNode divide it by the parent's world
        // scale again compounds the error every time the parent changes.
        outTransform.scale = state.node->world.scale > 0.0f ? state.node->world.scale : 1.0f;
        return true;
    }

    // (Two-scenario cleanup: the deferred-HeldBody activation helpers that lived here
    // were removed with the HeldBodyGrab system. ComputeHeldGrabTargetTransform above
    // stays — the instant-grab path still uses it.)
}

namespace heisenberg
{
    bool IsPlayerConsumableItem(RE::TESObjectREFR* refr)
    {
        return IsConsumable(refr);
    }

    bool IsPlayerInjectableItem(RE::TESObjectREFR* refr)
    {
        return IsInjectable(refr);
    }

    bool IsDiseaseCureItem(RE::TESObjectREFR* refr)
    {
        return IsDiseaseCureItemImpl(refr);
    }

    bool IsCompanionStimpakItem(RE::TESObjectREFR* refr)
    {
        return IsCompanionStimpakItemImpl(refr);
    }

    bool HasHeldCompanionStimpakTarget(
        bool isLeft,
        RE::TESObjectREFR* refr,
        RE::NiAVObject* heldNode)
    {
        if (!g_config.enableCompanionStimpakInjection ||
            !IsCompanionStimpakItemImpl(refr)) {
            return false;
        }
        if (!heldNode && refr) {
            heldNode = refr->Get3D();
        }
        float targetDistance = -1.0f;
        return FindInjectionTargetActor(
                   isLeft,
                   heldNode,
                   InjectionTargetKind::WoundedCompanion,
                   targetDistance) != nullptr;
    }

    NpcInjectionAttempt TryInjectHeldDiseaseCure(
        bool isLeft,
        RE::TESObjectREFR* refr,
        RE::NiAVObject* heldNode,
        float heldSeconds,
        float handSpeedMetersPerSecond,
        const std::function<bool()>& prepareInventoryCommit)
    {
        return TryInjectHeldDiseaseCureImpl(
            isLeft,
            refr,
            heldNode,
            heldSeconds,
            handSpeedMetersPerSecond,
            prepareInventoryCommit);
    }

    NpcInjectionAttempt TryInjectHeldCompanionStimpak(
        bool isLeft,
        RE::TESObjectREFR* refr,
        RE::NiAVObject* heldNode,
        float heldSeconds,
        float handSpeedMetersPerSecond,
        const std::function<bool()>& prepareInventoryCommit)
    {
        return TryInjectHeldCompanionStimpakImpl(
            isLeft,
            refr,
            heldNode,
            heldSeconds,
            handSpeedMetersPerSecond,
            prepareInventoryCommit);
    }

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

    static CollisionWrapperSet CollectCollisionWrappers(
        RE::NiAVObject* referenceRoot)
    {
        CollisionWrapperSet result{};
        if (!referenceRoot) {
            return result;
        }

        std::function<void(RE::NiAVObject*, std::uint32_t)> visit =
            [&](RE::NiAVObject* node, std::uint32_t depth) {
                if (!node || depth > 64) {
                    return;
                }

                if (auto* collision =
                        TryResolveNpcCollisionObjectFromRaw(
                            node->collisionObject.get())) {
                    ++result.totalWrapperCount;
                    const bool wrapperVisible =
                        CollisionOwnerHasVisibleGeometry(
                            node,
                            node,
                            collision,
                            0);
                    bool found = false;
                    for (std::uint32_t i = 0;
                         i < result.count;
                         ++i) {
                        if (result.wrappers[i] == collision) {
                            result.visible[i] =
                                result.visible[i] ||
                                wrapperVisible;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (result.count >= result.wrappers.size()) {
                            result.overflowed = true;
                        } else {
                            result.wrappers[result.count] = collision;
                            result.visible[result.count] =
                                wrapperVisible;
                            ++result.count;
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
        return result;
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
        
        // STEALING CHECK: Block grab if picking up this item would be stealing (unless config allows it)
        RE::TESObjectREFR* selRefr = selection.GetRefr();

        // SIZE CEILING (car fix, #219/#220). Keep this at the centralized grab
        // commit instead of Physics::IsGrabbable: the latter is also the shared
        // eligibility predicate for proximity push, haptics and physical-contact
        // publication. Oversized bodies must remain tactile while every route
        // (including loot-to-hand / DropToHand) is rejected here.
        if (selRefr) {
            float oversizeMaxAxis = 0.0f;
            if (heisenberg::Physics::IsOversizedForPlayerGrab(selRefr, &oversizeMaxAxis)) {
                spdlog::debug("[GRAB] Item {:08X} is oversized ({:.0f} game units) - cannot grab", selRefr->formID, oversizeMaxAxis);
                return false;
            }
        }

        if (!g_config.allowGrabbingOwnedItems && selRefr && TESObjectREFR_IsCrimeToActivate(selRefr)) {
            spdlog::debug("[GRAB] Item {:08X} is owned - cannot grab (would be stealing)", selRefr->formID);
            g_vrInput.TriggerHaptic(isLeft, 500);  // Feedback that grab was blocked
            return false;
        }
        
        // POWER ARMOR WEAPON CHECK: historically weapons could not be grabbed at
        // all while in Power Armor ("mechanics interfere with native game
        // behavior, causing infinite loading screens"). That guard dates to
        // v0.7.0 and predates keyframed mode, the embedded ROCK engine, the
        // held-body lease and the on-release filter restore, so it is now opt-in
        // via bAllowWeaponGrabInPowerArmor=false rather than unconditional.
        // The dual-wield provider escape hatch is kept for when it IS enabled:
        // only an explicit per-hand candidate acceptance bypasses it; merely
        // registering is insufficient.
        if (selRefr && !g_config.allowWeaponGrabInPowerArmor && Utils::IsPlayerInPowerArmor()) {
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
                    // Info, not debug: this silently refuses a grab the player
                    // is actively attempting, and at the default log level the
                    // old debug line left no trace of why nothing happened.
                    spdlog::info("[GRAB] Weapon grab blocked in Power Armor by bAllowWeaponGrabInPowerArmor=false - using native game behavior");
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
        (void)rock::touch_grab_bridge::
            ClearTouchGrabFingerPose(isLeft);
        state.rockRichFingerPosePublished = false;

        // Clear any existing grab on THIS hand
        if (state.active)
        {
            spdlog::debug("[GRAB] {} hand: Clearing existing grab", isLeft ? "Left" : "Right");
            EndGrab(isLeft, nullptr);
        }
        // A rejected grab can populate handles/nodes before it reaches the
        // final active=true commit. Never carry that inactive partial state
        // into the next attempt.
        if (!state.active) {
            rock::HostClearExternalHeldBodies(isLeft);
            state.Clear();
        }
        
        // Note: We unequipped weapon earlier if needed (unequippedWeaponForThisGrab),
        // but we don't track it anymore since the player manually re-equips if desired
        
        // Check if the OTHER hand is holding this object - hand-to-hand transfer
        bool hasTransferContactPoint = false;
        bool hasTransferHandFrame = false;
        RE::NiPoint3 transferPalmPoint{};
        RE::NiPoint3 transferContactPoint{};
        RE::NiTransform transferHandWorld{};
        float transferContactMeshDistance = -1.0f;
        selRefr = selection.GetRefr();
        if (otherState.active && otherState.GetRefr() == selRefr)
        {
            const bool trustedSelectionContact =
                !otherState.coHeldSecondary &&
                !otherState.isPulling &&
                grab_pose_policy::
                    ShouldAcceptSameObjectTransferContact(
                        selection.isPhysicalTouch,
                        selection.physicalTouchAgeFrames,
                        selection.isHeldObjectTransferContact,
                        selection.distance);
            bool closeRangeTransferAttempt =
                trustedSelectionContact;
            if (trustedSelectionContact) {
                hasTransferContactPoint = true;
                transferContactPoint = selection.hitPoint;
                transferContactMeshDistance =
                    (std::max)(0.0f, selection.distance);
            }
            // Resolve the receiving palm against the visible mesh first. A
            // valid grip-edge contact always permits hand-to-hand transfer.
            // The query below enriches the pose and remains the fallback for
            // ordinary selections, but it must not revoke a fresh native
            // contact or Hand::TryStartGrab's exact held-mesh result merely
            // because the clean palm and controller frames have different
            // authored offsets.
            if (!otherState.coHeldSecondary && !otherState.isPulling) {
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                RE::NiNode* secondaryWand =
                    playerNodes ? heisenberg::GetWandNode(playerNodes, isLeft) : nullptr;
                RE::NiNode* primaryWand =
                    playerNodes ? heisenberg::GetWandNode(playerNodes, !isLeft) : nullptr;
                RE::NiNode* secondaryRenderedHand = GetSkinnedHandNode(isLeft);
                RE::NiAVObject* objectRoot =
                    otherState.node ? otherState.node.get() : selection.node.get();

                if (secondaryWand && objectRoot) {
                    RE::NiPoint3 secondaryPalm{};
                    if (!GetCleanTrackedPalmPosition(
                            secondaryWand,
                            isLeft,
                            secondaryPalm)) {
                        spdlog::warn(
                            "[GRAB] Same-object transfer unavailable: receiving "
                            "clean tracked palm could not be resolved");
                    } else {
                        std::vector<TriangleData> triangles;
                        triangles.reserve(512);
                        GetTriangles(objectRoot, triangles, 4096);

                        RE::NiPoint3 secondaryAnchorWorld{};
                        float secondaryMeshDistance = -1.0f;
                        const bool haveSecondaryAnchor =
                            GetClosestMeshPointToPoint(
                                triangles,
                                secondaryPalm,
                                secondaryAnchorWorld,
                                secondaryMeshDistance);

                        constexpr float kTransferMeshTolerance =
                            grab_pose_policy::
                                kSameObjectTransferCandidateDistance;

                        // CO-HOLD TOLERANCE MUST SCALE WITH THE OBJECT'S THICKNESS.
                        //
                        // A flat 5.0 units is generous slop on a rifle and nonsense on a
                        // screwdriver: that shaft measures 2.81 units ACROSS, so a palm 3.9
                        // units from the metal - nearly three shaft-radii away, not touching
                        // anything - still qualified as a two-handed grip. The second hand then
                        // curled its fingers in mid-air beside the shaft. Live evidence, ten
                        // joins on one screwdriver: secondaryMesh ranged 0.01 (fingers wrap the
                        // metal) to 3.92 (fingers close on nothing), all accepted identically.
                        //
                        // Derive the grip envelope from the object itself. Of the three AABB
                        // spans the LARGEST is the long axis (33.5 for the screwdriver); the
                        // MIDDLE one is the cross-section you actually close your hand around.
                        // Half of it is the grip radius, plus a fixed allowance for palm flesh
                        // and tracking noise. Never wider than the old constant, so chunky
                        // objects keep exactly today's behaviour and only thin ones tighten.
                        float coHoldMeshTolerance = 5.0f;
                        {
                            constexpr float kCoHoldHandSlop = 1.5f;
                            RE::NiPoint3 lo{ FLT_MAX, FLT_MAX, FLT_MAX };
                            RE::NiPoint3 hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
                            const auto include = [&](const RE::NiPoint3& v) {
                                lo.x = (std::min)(lo.x, v.x); hi.x = (std::max)(hi.x, v.x);
                                lo.y = (std::min)(lo.y, v.y); hi.y = (std::max)(hi.y, v.y);
                                lo.z = (std::min)(lo.z, v.z); hi.z = (std::max)(hi.z, v.z);
                            };
                            for (const auto& tri : triangles) {
                                include(tri.v0); include(tri.v1); include(tri.v2);
                            }
                            if (!triangles.empty()) {
                                float spans[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
                                std::sort(spans, spans + 3);
                                const float gripSpan = spans[1];   // middle = cross-section
                                if (std::isfinite(gripSpan) && gripSpan > 0.0f) {
                                    coHoldMeshTolerance =
                                        (std::min)(coHoldMeshTolerance,
                                                   gripSpan * 0.5f + kCoHoldHandSlop);
                                }
                            }
                        }
                        const float kCoHoldMeshTolerance = coHoldMeshTolerance;
                        const bool palmWithinTransferEnvelope =
                            haveSecondaryAnchor &&
                            secondaryMeshDistance <=
                                kTransferMeshTolerance;
                        closeRangeTransferAttempt =
                            trustedSelectionContact ||
                            palmWithinTransferEnvelope;
                        if (closeRangeTransferAttempt) {
                            hasTransferHandFrame = true;
                            transferPalmPoint = secondaryPalm;
                            transferHandWorld =
                                secondaryRenderedHand
                                    ? secondaryRenderedHand->world
                                    : secondaryWand->world;
                            if (!hasTransferContactPoint &&
                                haveSecondaryAnchor) {
                                hasTransferContactPoint = true;
                                transferContactPoint =
                                    secondaryAnchorWorld;
                            }
                            if (!trustedSelectionContact) {
                                transferContactMeshDistance =
                                    secondaryMeshDistance;
                            }
                        }

                        // Two-handed CO-HOLD still requires two distinct
                        // visible-mesh contacts. When that stricter test does
                        // not pass, the close receiving-hand contact above
                        // falls through to a normal ownership transfer.
                        if (g_config.enableTwoHandedGrab &&
                            closeRangeTransferAttempt &&
                            primaryWand)
                        {
                            RE::NiPoint3 primaryPalm{};
                            RE::NiPoint3 primaryAnchorWorld{};
                            float primaryMeshDistance = -1.0f;
                            const bool havePrimaryPalm =
                                GetCleanTrackedPalmPosition(
                                    primaryWand,
                                    !isLeft,
                                    primaryPalm);
                            const bool havePrimaryAnchor =
                                havePrimaryPalm &&
                                GetClosestMeshPointToPoint(
                                    triangles,
                                    primaryPalm,
                                    primaryAnchorWorld,
                                    primaryMeshDistance);
                            const float anchorSeparation =
                                havePrimaryAnchor
                                    ? (secondaryAnchorWorld -
                                          primaryAnchorWorld)
                                          .Length()
                                    : -1.0f;

                            if (havePrimaryAnchor &&
                                secondaryMeshDistance <=
                                    kCoHoldMeshTolerance &&
                                primaryMeshDistance <=
                                    kCoHoldMeshTolerance &&
                                anchorSeparation >= 6.0f)
                            {
                                const RE::NiTransform objectWorld =
                                    objectRoot->world;
                                const RE::NiTransform
                                    secondaryHandWorld =
                                        transferHandWorld;

                                otherState.coHoldPrimaryAnchorObjectLocal =
                                    rock::transform_math::worldPointToLocal(
                                        objectWorld,
                                        primaryAnchorWorld);
                                otherState.coHoldSecondaryAnchorObjectLocal =
                                    rock::transform_math::worldPointToLocal(
                                        objectWorld,
                                        secondaryAnchorWorld);
                                otherState.coHoldSecondaryHandObjectLocal =
                                    rock::transform_math::composeTransforms(
                                        rock::transform_math::invertTransform(
                                            objectWorld),
                                        secondaryHandWorld);
                                otherState.coHoldAnchorsValid = true;

                                state.SetRefr(selRefr);
                                state.node =
                                    otherState.node
                                        ? otherState.node
                                        : selection.node;
                                state.active = true;
                                state.coHeldSecondary = true;
                                state.naturalFingerPosing = true;

                                std::array<float, 15>
                                    secondaryJointCurls{};
                                const bool richSecondaryPose =
                                    rock::touch_grab_bridge::
                                        SolveAndPublishTouchGrabFingerPose(
                                            objectRoot,
                                            secondaryHandWorld,
                                            isLeft,
                                            secondaryPalm,
                                            secondaryAnchorWorld,
                                            0.0f,
                                            true,
                                            &secondaryJointCurls);
                                const bool solvedSecondaryPose =
                                    richSecondaryPose ||
                                    rock::touch_grab_bridge::
                                        SolveTouchGrabFingerPose(
                                            objectRoot,
                                            secondaryHandWorld,
                                            isLeft,
                                            secondaryPalm,
                                            secondaryAnchorWorld,
                                            secondaryJointCurls);
                                if (solvedSecondaryPose) {
                                    state.SetRuntimeJointCurls(
                                        secondaryJointCurls);
                                    state.rockRichFingerPosePublished =
                                        richSecondaryPose;
                                    ApplyRuntimeFingerCurls(
                                        state,
                                        isLeft);
                                } else {
                                    state.pendingFingerCurls = true;
                                }

                                spdlog::info(
                                    "[GRAB] Two-handed CO-HOLD: {} hand joined ref "
                                    "{:08X} at a distinct mesh spot "
                                    "(secondaryMesh={:.2f} primaryMesh={:.2f} "
                                    "anchorSep={:.2f} tol={:.2f})",
                                    isLeft ? "Left" : "Right",
                                    selRefr->formID,
                                    secondaryMeshDistance,
                                    primaryMeshDistance,
                                    anchorSeparation,
                                    kCoHoldMeshTolerance);
                                // This is an internal support-hand marker, not
                                // a second ownership grab.
                                return true;
                            }

                            spdlog::info(
                                "[GRAB] Two-handed CO-HOLD unavailable at this "
                                "contact (secondaryMesh={:.2f} primaryMesh={:.2f} "
                                "anchorSep={:.2f}); close-range transfer={}",
                                secondaryMeshDistance,
                                primaryMeshDistance,
                                anchorSeparation,
                                closeRangeTransferAttempt);
                        }
                    }
                } else {
                    if (trustedSelectionContact) {
                        spdlog::debug(
                            "[GRAB] Same-object transfer contact accepted "
                            "without a receiving pose frame; finger solve "
                            "will be deferred");
                    } else {
                        spdlog::warn(
                            "[GRAB] Same-object transfer unavailable: missing "
                            "receiving wand/object frame (secondary={} object={})",
                            static_cast<void*>(secondaryWand),
                            static_cast<void*>(objectRoot));
                    }
                }
            }

            if (!closeRangeTransferAttempt) {
                spdlog::info(
                    "[GRAB] {} hand: same-object grip ignored because "
                    "the receiving palm is not within the close-range mesh "
                    "envelope; {} hand keeps ownership",
                    isLeft ? "Left" : "Right",
                    isLeft ? "Right" : "Left");
                return false;
            }
            spdlog::info(
                "[GRAB] Hand-to-hand TRANSFER detected! {} -> {} hand "
                "(receivingMesh={:.2f}, trustedGripContact={})",
                isLeft ? "Right" : "Left",
                isLeft ? "Left" : "Right",
                transferContactMeshDistance,
                trustedSelectionContact);
            // Promote ownership in place instead of releasing the live body and
            // starting a second grab. Besides causing a visible one-frame drop,
            // the old EndGrab path could run equip/storage/companion-release
            // side effects merely because the holding hand happened to be in a
            // body zone during the handoff. The co-held promotion path already
            // transfers the complete keyframed/physics state without restoring
            // it, so use a short-lived secondary marker for this atomic handoff.
            state.SetRefr(selRefr);
            state.node =
                otherState.node ? otherState.node : selection.node;
            state.active = true;
            state.coHeldSecondary = true;
            state.naturalFingerPosing = true;
            state.physicalTouchGrab = true;
            otherState.coHoldAnchorsValid = false;

            std::array<float, 15> transferJointCurls{};
            RE::NiAVObject* transferObjectRoot =
                otherState.node
                    ? otherState.node.get()
                    : selection.node.get();
            bool richTransferPose = false;
            bool solvedTransferPose = false;
            if (transferObjectRoot &&
                hasTransferHandFrame &&
                hasTransferContactPoint) {
                richTransferPose =
                    rock::touch_grab_bridge::
                        SolveAndPublishTouchGrabFingerPose(
                            transferObjectRoot,
                            transferHandWorld,
                            isLeft,
                            transferPalmPoint,
                            transferContactPoint,
                            0.0f,
                            true,
                            &transferJointCurls);
                solvedTransferPose =
                    richTransferPose ||
                    rock::touch_grab_bridge::
                        SolveTouchGrabFingerPose(
                            transferObjectRoot,
                            transferHandWorld,
                            isLeft,
                            transferPalmPoint,
                            transferContactPoint,
                            transferJointCurls);
            }
            if (solvedTransferPose) {
                state.SetRuntimeJointCurls(
                    transferJointCurls);
                state.rockRichFingerPosePublished =
                    richTransferPose;
                ApplyRuntimeFingerCurls(state, isLeft);
            } else {
                // The promotion path requests a post-physics mesh solve when
                // calibration or geometry is temporarily unavailable.
                state.pendingFingerCurls = true;
            }

            EndGrab(!isLeft);
            const bool transferCommitted =
                state.active &&
                !state.coHeldSecondary &&
                state.GetRefr() == selRefr;
            if (!transferCommitted) {
                spdlog::error(
                    "[GRAB] Atomic hand-to-hand transfer failed for "
                    "{:08X}; ownership state was not promoted",
                    selRefr ? selRefr->formID : 0);
                return false;
            }

            spdlog::info(
                "[GRAB] Atomic hand-to-hand transfer committed to {} "
                "hand (fingerPose={} rich={})",
                isLeft ? "left" : "right",
                solvedTransferPose ? "mesh" : "deferred",
                richTransferPose);
            return true;
        }

        if (!selRefr || !selection.node)
        {
            spdlog::warn("[GRAB] {} hand: Invalid selection (refr={}, node={})",
                         isLeft ? "Left" : "Right",
                         selRefr ? "valid" : "null",
                         selection.node ? "valid" : "null");
            return false;
        }
        bool validatedPhysicalTouch =
            selection.isPhysicalTouch;
        float currentTouchMeshDistance = -1.0f;
        if (selection.isPhysicalTouch &&
            selection.physicalTouchAgeFrames >
                grab_pose_policy::kFreshTouchMaxAgeFrames) {
            std::vector<TriangleData> currentTouchTriangles;
            currentTouchTriangles.reserve(512);
            GetTriangles(
                selection.node.get(),
                currentTouchTriangles,
                4096);
            RE::NiPoint3 currentTouchMeshPoint{};
            const bool haveCurrentTouchMesh =
                GetClosestMeshPointToPoint(
                    currentTouchTriangles,
                    handPos,
                    currentTouchMeshPoint,
                    currentTouchMeshDistance);
            validatedPhysicalTouch =
                haveCurrentTouchMesh &&
                grab_pose_policy::
                    ShouldAcceptPhysicalTouchEvidence(
                        selection.physicalTouchAgeFrames,
                        currentTouchMeshDistance);
            if (!validatedPhysicalTouch) {
                spdlog::info(
                    "[GRAB-TOUCH] {} ref={:08X} rejected stale "
                    "physical flag age={}f currentMeshDist={:.1f}; "
                    "using normal close/remote selection rules",
                    isLeft ? "L" : "R",
                    selRefr->formID,
                    selection.physicalTouchAgeFrames,
                    currentTouchMeshDistance);
            }
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
        if (validatedPhysicalTouch) {
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
        const bool bypassSavedPlacement =
            grab_pose_policy::ShouldBypassSavedPlacement(baseFormID);
        const bool bypassSavedFingerPose =
            grab_pose_policy::ShouldBypassSavedFingerPose(baseFormID);
        state.rockRichFingerPosePublished = false;
        spdlog::debug("[GRAB] {} hand: Grabbing ref {:08X} (BaseFormID={:08X})",
                      isLeft ? "Left" : "Right", selRefr->formID, baseFormID);
        if (bypassSavedPlacement || bypassSavedFingerPose) {
            spdlog::info(
                "[GRAB-ROCK-POSE] '{}' base={:08X}: saved placement={} "
                "saved fingers={} (ROCK mesh pose test policy)",
                ItemOffsetManager::GetItemName(selRefr),
                baseFormID,
                bypassSavedPlacement ? "BYPASSED" : "kept",
                bypassSavedFingerPose ? "BYPASSED" : "kept");
        }

        // Store grab info
        state.SetRefr(selRefr);
        state.node = selection.node;  // NiPointer assignment
        state.collisionObject = nullptr;  // Will be set later
        state.physicalTouchGrab = validatedPhysicalTouch;
        state.physicalTouchBodyId =
            selection.physicalTouchBodyId;
        state.initialHandPos = grabHandPos;
        state.initialHandRot = grabHandRot;

        const auto abandonPendingGrab = [&]() {
            (void)rock::touch_grab_bridge::ClearTouchGrabFingerPose(isLeft);
            FRIKInterface::GetSingleton().ClearHandPoseFingerPositions(isLeft);
            state.Clear();
        };
        
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
        if (validatedPhysicalTouch) {
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
        
        // Calculate distance to SURFACE.
        // Measured from the palm to the closest point of the rendered mesh.
        // The previous sphere metric (centre distance minus bound radius)
        // reported "touching" for a hand anywhere inside the bounding sphere
        // of a long or flat object, which let the touch-preserve and
        // telekinesis paths lock objects 10+ units away from the hand. The
        // sphere metric stays as the fallback when no mesh can be read.
        float distToSurface = distToCenter - boundRadius;
        if (distToSurface < 0.0f) distToSurface = 0.0f;  // Hand is inside bounding sphere = touching
        const char* surfaceDistanceSource = "bound-sphere";
        if (!forceUseOffset && state.node) {
            RE::NiPoint3 surfaceProbe = grabHandPos;
            if (auto* probeNodes = f4cf::f4vr::getPlayerNodes()) {
                RE::NiTransform palmParent{};
                RE::NiPoint3 palmProbe{};
                RE::NiPoint3 palmarProbe{};
                bool palmSkinned = false;
                if (GetPalmSurfaceSeatFrame(
                        heisenberg::GetWandNode(probeNodes, isLeft),
                        isLeft,
                        palmParent,
                        palmProbe,
                        palmarProbe,
                        palmSkinned)) {
                    surfaceProbe = palmProbe;
                }
            }
            std::vector<TriangleData> surfaceTriangles;
            surfaceTriangles.reserve(512);
            GetTriangles(state.node.get(), surfaceTriangles, 4096);
            RE::NiPoint3 closestSurfacePoint{};
            float meshDistance = -1.0f;
            if (GetClosestMeshPointToPoint(
                    surfaceTriangles,
                    surfaceProbe,
                    closestSurfacePoint,
                    meshDistance) &&
                std::isfinite(meshDistance) &&
                meshDistance >= 0.0f) {
                distToSurface = meshDistance;
                surfaceDistanceSource = "palm-to-mesh";
            }
        }

        // Use surface distance for NATURAL GRAB decision (hand touching object)
        // Use center distance for PALM SNAP decision (pointing at object from distance)
        float distToObject = distToSurface;

        spdlog::debug("[GRAB] Distance: center={:.1f}cm surface={:.1f}cm ({}) boundR={:.1f}cm (natural if <= {:.1f}cm)",
                     distToCenter, distToSurface, surfaceDistanceSource, boundRadius, heisenberg::g_config.naturalGrabDistance);
        
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
        bool hasExactOffsetMatch =
            offsetMgr.HasExactMatch(selRefr) &&
            !bypassSavedPlacement;
        
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
            (validatedPhysicalTouch ||
             distToObject <= kTouchGrabMaxDistance);

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
                abandonPendingGrab();
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
                abandonPendingGrab();
                return false;
            }
        }
        
        // Prefer the integrated/saved offset for this item when we have an
        // exact match (FormID or name). Geometry-based placement acts only
        // as a fallback when no exact match exists. Telekinesis grabs still
        // ignore saved offsets (world position is preserved below).
        std::optional<ItemOffset> customOffset = std::nullopt;
        bool hasAuthoritativeIdentityOffset = false;
        if (!useTelekinesis) {
            if (!bypassSavedPlacement) {
                customOffset = offsetMgr.GetExactOffset(selRefr, isLeft);
                hasAuthoritativeIdentityOffset =
                    customOffset.has_value();
                // No FormID/name match? Fall back to a 100% EXACT-dimensions match. Items that reuse
                // another item's model (Addictol↔Jet, Buffout↔Bufftats) share identical bounds, so they
                // inherit that item's hand-tuned offset instead of floating via geometry placement.
                // Strictly exact dims — no fuzzy/similar matching.
                if (!customOffset.has_value()) {
                    customOffset = offsetMgr.GetExactDimensionsOffset(selRefr, isLeft);
                }
                // ARMOR DIMENSIONAL DONOR (Jul 30). Garments are the one item
                // class where a near-identical neighbour is genuinely
                // interchangeable: they are all authored around the same body
                // origin, so an authored garment pose transfers. The strict
                // dims match above misses them because it also compares HEIGHT,
                // and a folded garment's height varies with its drape (BoS
                // Uniform 34x42x10 vs the authored Black Vest and Slacks
                // 34x42x12 — identical footprint, different thickness).
                //
                // Without this the item falls to generated placement, and
                // generated placement is NOT trustworthy for garments: the
                // measured object-local mesh AABB is not even reproducible
                // between grabs of the same item (live: thinSpan 10.5 -> 34.8
                // on one uniform), and the calibrated palm frame it seats
                // against can itself be unreliable. Three uniforms tested live
                // all had a donor within 2.0 XZ units — one at 0.00 — and all
                // three were placed wrongly by the generated path.
                // MESH IDENTITY FIRST. Borrowing the pose of an item that
                // renders the SAME NIF is exact, not a similarity guess, so it
                // outranks the dimensional armor donor below.
                if (!customOffset.has_value()) {
                    auto meshDonor = offsetMgr.GetSharedModelDonorOffset(selRefr, isLeft);
                    if (meshDonor.has_value()) {
                        customOffset = meshDonor;
                        spdlog::info(
                            "[GRAB] '{}' using authored SHARED-MESH donor '{}' "
                            "— identical model means identical pivot, so the "
                            "authored hold applies verbatim",
                            itemName,
                            customOffset->matchedName);
                    }
                }
                if (!customOffset.has_value()) {
                    auto donorOffset = offsetMgr.GetArmorDimensionalDonorOffset(selRefr, isLeft);
                    if (donorOffset.has_value()) {
                        customOffset = donorOffset;
                        spdlog::info(
                            "[GRAB] '{}' using authored ARMOR donor '{}' "
                            "(XZ distance {:.2f}) — authored garment poses beat "
                            "generated placement",
                            itemName,
                            customOffset->matchedName,
                            customOffset->armorDonorXZDistance);
                    }
                }
                // A donor is authored data, so it must outrank the generated
                // palm seats exactly as an identity match does — otherwise the
                // broad-palm seat overrides it and we are back to the
                // unreproducible mesh measurement.
                hasAuthoritativeIdentityOffset =
                    hasAuthoritativeIdentityOffset || customOffset.has_value();
            }
        } else if (!bypassSavedFingerPose) {
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

        bool canonicalWeaponRuntimePlacement = false;
        const char* canonicalWeaponPlacementReason =
            "policySkipped";
        if (wandNode &&
            grab_pose_policy::
                ShouldUseCanonicalLooseWeaponHold(
                    customOffset.has_value(),
                    forceUseOffset,
                    usePullToHand,
                    IsWeapon(selRefr),
                    IsThrowableWeapon(selRefr))) {
            canonicalWeaponRuntimePlacement =
                TryCalculateCanonicalLooseWeaponPlacement(
                    state,
                    wandNode,
                    isLeft,
                    selRefr,
                    canonicalWeaponPlacementReason);
            if (canonicalWeaponRuntimePlacement) {
                spdlog::info(
                    "[GRAB-ROCK-WEAPON] {} '{}' canonical firing hold "
                    "selected for {} arrival (source={} local=({:.2f},"
                    "{:.2f},{:.2f}))",
                    isLeft ? "L" : "R",
                    itemName,
                    forceUseOffset ? "DropToHand" : "remote-pull",
                    canonicalWeaponPlacementReason,
                    state.runtimeHandPlacementPosition.x,
                    state.runtimeHandPlacementPosition.y,
                    state.runtimeHandPlacementPosition.z);
            } else {
                spdlog::debug(
                    "[GRAB-ROCK-WEAPON] {} '{}' canonical firing hold "
                    "unavailable for {} arrival (reason={}); retaining "
                    "the normal placement fallback",
                    isLeft ? "L" : "R",
                    itemName,
                    forceUseOffset ? "DropToHand" : "remote-pull",
                    canonicalWeaponPlacementReason);
            }
        }

        // Set when the no-offset branch below seated the object with the
        // HIGGS mesh surface snap. Consumed by the runtime-placement gates
        // after the touch/pull stage so the seat is not cleared as a stale
        // geometry result.
        bool geometryFallbackSeat = false;

        if (customOffset.has_value())
        {
            // Use saved profile offset
            state.itemOffset = customOffset.value();
            state.hasItemOffset = true;
            if (bypassSavedFingerPose) {
                state.itemOffset.hasFingerCurls = false;
                state.itemOffset.hasJointCurls = false;
                spdlog::info(
                    "[GRAB-ROCK-POSE] '{}' retained its saved placement but "
                    "will fit fingers to the final mesh",
                    itemName);
            }
            
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
        else if (canonicalWeaponRuntimePlacement)
        {
            // The runtime transform already contains both the canonical
            // firing-grip translation and rotation. Keep it separate from
            // persisted ItemOffsets so a generated ROCK hold is never saved
            // as a user-authored profile implicitly.
            state.hasItemOffset = false;
            state.isFRIKOffset = false;
        }
        else if (wandNode)
        {
            // No profile - use palm snap or natural grab based on config and distance
            state.itemOffset = ItemOffset();  // Start fresh
            
            // Get item dimensions first (needed for positioning calculations)
            float itemLength = 0, itemWidth = 0, itemHeight = 0;
            ItemOffsetManager::GetItemDimensions(selRefr, itemLength, itemWidth, itemHeight);
            
            // distToObject and withinSnapDistance already calculated above

            // NATURAL GRAB: preserve the object's current world pose relative
            // to the wand. Used for telekinesis and, when everything else is
            // unavailable, as the last resort.
            // F4VR row-vector convention:
            //   local.pos    = parent.rotate * (world - parent.translate)
            //   local.rotate = world.rotate * parent.rotate.Transpose()
            const auto applyNaturalPreserve = [&]() {
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
            };

            if (!withinSnapDistance &&
                !grab_pose_policy::ShouldBypassSavedPlacement(baseFormID))
            {
                // HIGGS-faithful fallback (Hand::TransitionHeld surface snap)
                // for every non-touching grab that has no exact, exact-dims
                // or donor offset. A close grab seats from the live pose; a
                // remote pull seats from its virtual arrival pose so the pull
                // animation's target is the snapped seat.
                geometryFallbackSeat = usePullToHand
                    ? TryCalculatePulledArrivalPlacementFromGeometry(
                          state, wandNode, isLeft, worldTransform)
                    : TryCalculateRuntimeHandPlacementFromGeometry(
                          state, wandNode, isLeft, worldTransform, /*pulledArrival=*/false);
                if (geometryFallbackSeat) {
                    state.hasItemOffset = false;
                    state.usedSnapMode = true;
                    spdlog::info(
                        "[GRAB] MESH SURFACE SNAP for '{}': dist={:.1f}cm pull={} frame={} local=({:.2f}, {:.2f}, {:.2f})",
                        itemName,
                        distToObject,
                        usePullToHand,
                        state.runtimePlacementSkinnedHand ? "skinned" : "wand",
                        state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z);
                }
            }

            if (geometryFallbackSeat)
            {
                // Seated above.
            }
            else if (heisenberg::g_config.enablePalmSnap && !withinSnapDistance)
            {
                // PALM SNAP (bound-based): only reached when the mesh surface
                // snap above could not run (no triangles, no palm frame, or a
                // policy-bypassed object).
                //
                // The geometry-blind constant this replaces —
                // (0, 1 + itemLength * 0.5, 3) in wand space — assumed the
                // pivot was the object's centre and that the palm sat at the
                // wand origin. Both are false for most meshes, which is how
                // large clothing ended up floating beside the hand.
                RE::NiPoint3 boundSnapLocal{};
                const bool boundSnapped =
                    TryCalculateBoundPalmSnapLocal(
                        state,
                        wandNode,
                        isLeft,
                        worldTransform,
                        itemLength,
                        itemWidth,
                        itemHeight,
                        boundSnapLocal);
                if (boundSnapped) {
                    state.itemOffset.position = boundSnapLocal;
                } else {
                    // No palm frame available at all (no calibration, no wand
                    // config). Keep the object at the wand with a clearance
                    // taken from its THINNEST dimension rather than its
                    // longest, so a failure here cannot reproduce the float.
                    float smallestDimension = itemLength;
                    if (itemWidth > 0.1f && itemWidth < smallestDimension) {
                        smallestDimension = itemWidth;
                    }
                    if (itemHeight > 0.1f && itemHeight < smallestDimension) {
                        smallestDimension = itemHeight;
                    }
                    if (!std::isfinite(smallestDimension) || smallestDimension < 0.1f) {
                        smallestDimension = 2.0f;
                    }
                    state.itemOffset.position = RE::NiPoint3(
                        0.0f,
                        heisenberg::grab_pose_policy::kPalmSurfaceSkin +
                            smallestDimension * 0.5f,
                        3.0f);
                    spdlog::warn(
                        "[GRAB] PALM SNAP for '{}' has no usable palm frame - "
                        "using a thin-dimension wand clearance",
                        itemName);
                }

                spdlog::info("[GRAB] PALM SNAP for '{}': dist={:.1f}cm, dims=({:.1f}x{:.1f}x{:.1f}), source={}, offset=({:.2f}, {:.2f}, {:.2f})",
                             itemName,
                             distToObject, itemLength, itemWidth, itemHeight,
                             boundSnapped ? "bound-centre-palm-seat" : "no-palm-frame-fallback",
                             state.itemOffset.position.x, state.itemOffset.position.y, state.itemOffset.position.z);

                // F4VR row-vector convention: local.rotate = world.rotate * parent.rotate.Transpose()
                state.itemOffset.rotation = worldTransform.rotate * wandNode->world.rotate.Transpose();

                state.hasItemOffset = false;
                state.usedSnapMode = true;  // Used snap positioning - will open hand fully
            }
            else
            {
                // Telekinesis range, or no seat could be computed with pull disabled.
                applyNaturalPreserve();
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
                abandonPendingGrab();
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
                    validatedPhysicalTouch && state.physicsNode
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
                            validatedPhysicalTouch &&
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
                                validatedPhysicalTouch &&
                                        selection.hasPhysicalTouchPoint
                                    ? 5.0f
                                    : 14.0f;
                        contactNearVisibleMesh =
                            contactMeshDistance <=
                            visibleContactToleranceGameUnits;
                    }
                }
                const bool richFingerPose =
                    rock::touch_grab_bridge::
                        SolveAndPublishTouchGrabFingerPose(
                            contactGeometryNode,
                            physicalTouchHandNode ? physicalTouchHandNode->world : wandNode->world,
                            isLeft,
                            grabHandPos,
                            validatedPhysicalTouch &&
                                    selection.hasPhysicalTouchPoint
                                ? selection.hitPoint
                                : grabHandPos,
                            0.0f,
                            true,
                            &jointCurls);
                const bool solvedFingerPose =
                    richFingerPose ||
                    rock::touch_grab_bridge::SolveTouchGrabFingerPose(
                        contactGeometryNode,
                        physicalTouchHandNode ? physicalTouchHandNode->world : wandNode->world,
                        isLeft,
                        grabHandPos,
                        validatedPhysicalTouch &&
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
                        state.rockRichFingerPosePublished =
                            richFingerPose;
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

        // A mesh surface seat counts as object-specific placement: it must
        // survive the gates below exactly like a canonical weapon hold. A
        // touch grab that ran afterwards has already replaced it.
        bool objectSpecificRuntimePlacement =
            canonicalWeaponRuntimePlacement ||
            (geometryFallbackSeat && !touchGrabApplied);
        if (!state.isTelekinesis &&
            wandNode &&
            !objectSpecificRuntimePlacement) {
            if (!touchGrabApplied &&
                baseFormID ==
                    grab_pose_policy::kKickballBaseFormID) {
                objectSpecificRuntimePlacement =
                    TryCalculateKickballPalmPlacement(
                        state,
                        wandNode,
                        isLeft,
                        worldTransform);
                if (objectSpecificRuntimePlacement) {
                    spdlog::info(
                        "[GRAB-ROCK-POSE] Kickball seated by bound "
                        "centre/radius at {:.2f}u palm clearance "
                        "(frame={})",
                        grab_pose_policy::kPalmSurfaceSkin,
                        state.runtimePlacementSkinnedHand
                            ? "skinned"
                            : "wand");
                }
            } else if (
                !touchGrabApplied &&
                baseFormID ==
                grab_pose_policy::kShovelBaseFormID) {
                objectSpecificRuntimePlacement =
                    TryCalculateSelectedSurfacePalmPlacement(
                        state,
                        wandNode,
                        isLeft,
                        worldTransform);
                if (objectSpecificRuntimePlacement) {
                    spdlog::info(
                        "[GRAB-ROCK-POSE] Shovel selected shaft "
                        "surface seated at the primary palm "
                        "(frame={})",
                        state.runtimePlacementSkinnedHand
                            ? "skinned"
                            : "wand");
                }
            }

            // Shape-driven broad seating is deliberately below explicit
            // sphere/shaft policies and above the generic one-point geometry
            // snap.  A direct touch may replace its first-contact relation;
            // a normal authored snap remains authoritative.
            if (!objectSpecificRuntimePlacement &&
                g_config.enableAutomaticHandPlacement &&
                !IsWeapon(selRefr) &&
                !IsThrowableWeapon(selRefr) &&
                baseFormID !=
                    grab_pose_policy::kKickballBaseFormID &&
                baseFormID !=
                    grab_pose_policy::kShovelBaseFormID) {
                // usedSnapMode is set only by the far-grab PALM SNAP branch
                // above — the geometry-blind constant that leaves large
                // unauthored meshes floating. A natural grab clears it, and
                // every object-specific seat sets objectSpecificRuntimePlacement
                // (checked above), so inside this block the flag means exactly
                // "the constant was applied and may be replaced".
                const bool replacingGeometryBlindSeat = state.usedSnapMode;
                const bool broadPalmPlacement =
                    TryCalculateBroadObjectPalmPlacement(
                        state,
                        wandNode,
                        isLeft,
                        worldTransform,
                        isHolotape,
                        hasAuthoritativeIdentityOffset,
                        touchGrabApplied,
                        replacingGeometryBlindSeat);
                if (broadPalmPlacement) {
                    objectSpecificRuntimePlacement = true;
                    if (touchGrabApplied) {
                        (void)rock::touch_grab_bridge::
                            ClearTouchGrabFingerPose(isLeft);
                        state.rockRichFingerPosePublished = false;
                        touchGrabApplied = false;
                    }
                }
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
            !objectSpecificRuntimePlacement &&
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
        } else if (
            !touchGrabApplied &&
            !objectSpecificRuntimePlacement) {
            state.ClearRuntimeHandPlacement();
            spdlog::debug("[GRAB-POS] Using saved exact offset for '{}' (no geometry fallback)", itemName);
        } else if (objectSpecificRuntimePlacement) {
            spdlog::debug(
                "[GRAB-POS] Preserving object-specific ROCK "
                "placement for '{}'",
                itemName);
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
                    // IMMEDIATE path: curls applied here, at grab time, BEFORE the object is
                    // seated at its final placement, and pendingFingerCurls is left false - so
                    // the rendered-hand rebase can commit as soon as the animator reports
                    // Holding. The alternative DEFERRED path (see ~9030 -> ResolvePendingFingerCurls)
                    // applies the same authored curls post-physics instead. Which of the two runs
                    // decides how long the hand has to settle before the rebase freezes the
                    // object's seat, and the owner reports the screwdriver sometimes ending up
                    // beside the finger curl instead of inside it. Logged at INFO on both paths
                    // so a bad grab can be attributed to one of them from the log alone.
                    spdlog::info("[GRAB-FINGERS] {} '{}' curls=IMMEDIATE (applied at grab time, "
                                 "pendingFingerCurls=false) joints={} thumb={:.2f}",
                        isLeft ? "L" : "R", itemName,
                        state.itemOffset.hasJointCurls, state.itemOffset.thumbCurl);
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

    bool GrabManager::CommitHeldConsumableConsumption(
        bool isLeft,
        RE::TESObjectREFR* refr,
        const char* zoneLabel)
    {
        RE::TESForm* baseForm =
            refr ? refr->GetObjectReference() : nullptr;
        const bool isDiseaseCureItem = IsDiseaseCureItem(refr);
        const bool consumed =
            heisenberg::npc_injection_policy::TryPlayerConsumption(
                isDiseaseCureItem,
                [&]() { return ConsumeGrabbedItem(refr); });

        if (consumed) {
            spdlog::debug(
                "[GRAB] Consume succeeded via {} zone",
                zoneLabel);
            HeisenbergPluginAPI::InvokeConsumedCallbacks(
                isLeft,
                baseForm);
            g_vrInput.TriggerHaptic(isLeft, 2000);
            g_heisenberg.BeginPostConsumeActivationSuppression(isLeft);
            EndGrab(isLeft, nullptr, true);
            return true;
        }

        spdlog::warn(
            "[GRAB] Consume FAILED via {} zone - keeping in hand",
            zoneLabel);
        g_vrInput.TriggerHaptic(isLeft, 500);
        return false;
    }

    bool GrabManager::TryConsumeHeldConsumableOnRelease(bool isLeft)
    {
        GrabState& state = isLeft ? _leftGrab : _rightGrab;
        if (!state.active || state.coHeldSecondary || state.isPulling) {
            return false;
        }

        if (ItemPositionConfigMode::GetSingleton().IsRepositionModeActive()) {
            return false;
        }

        RE::TESObjectREFR* refr = state.GetRefr();

        // Companion treatment owns the Stimpak before the player self-use
        // route. This matters on the exact frame a grip is released while the
        // held Stimpak is touching a wounded companion: never turn that gesture
        // into a wrist self-injection merely because both zones overlap.
        if (HasHeldCompanionStimpakTarget(
                isLeft,
                refr,
                state.node.get())) {
            const float heldSeconds =
                static_cast<float>(Utils::GetTime()) -
                state.grabStartTime;
            const NpcInjectionAttempt injection =
                TryInjectHeldCompanionStimpak(
                    isLeft,
                    refr,
                    state.node.get(),
                    heldSeconds,
                    state.handSpeed);
            if (injection.result !=
                NpcInjectionResult::NotAttempted) {
                state.consumeAttemptedThisVisit = true;
                if (injection.result !=
                    NpcInjectionResult::FailedKeptInHand) {
                    g_vrInput.TriggerHaptic(
                        isLeft,
                        injection.result ==
                                NpcInjectionResult::Accepted ?
                            2000 :
                            500);
                    EndGrab(isLeft, nullptr, true);
                    return true;
                }
                g_vrInput.TriggerHaptic(isLeft, 500);
            }

            // Contact was unambiguously aimed at a wounded companion, but
            // target revalidation or the pre-commit ownership handoff did not
            // complete. Let the ordinary release drop the object; do not fall
            // through to player consumption.
            return false;
        }

        if (!IsConsumable(refr) || IsDiseaseCureItem(refr)) {
            return false;
        }
        if (g_config.blockConsumptionInPA &&
            Utils::IsPlayerInPowerArmor()) {
            return false;
        }

        const bool usesWristRoute =
            IsInjectable(refr) && g_config.enableHandInjection;
        const bool inValidZone = usesWristRoute ?
            IsInHandInjectionZone(isLeft) :
            IsInMouthZone(isLeft);
        if (!inValidZone) {
            return false;
        }

        // A physical release is the explicit commit gesture. Unlike optional
        // auto-consume it has no speed, dwell, grab-age, or per-visit gate.
        // Mark the visit before committing so a failed inventory transaction
        // cannot be retried automatically every frame while grip is up.
        state.consumeAttemptedThisVisit = true;
        const char* zoneLabel =
            usesWristRoute ? "hand injection release" : "mouth release";
        spdlog::debug(
            "[GRAB] Consumable released in {} zone - attempting consume",
            usesWristRoute ? "hand injection" : "mouth");
        (void)CommitHeldConsumableConsumption(
            isLeft,
            refr,
            zoneLabel);
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
                (void)rock::touch_grab_bridge::
                    ClearTouchGrabFingerPose(isLeft);
                HandAuthority::Clear(
                    OBJECT_COHOLD_HAND_TAG,
                    isLeft);
                auto& primary =
                    isLeft ? _rightGrab : _leftGrab;
                primary.coHoldAnchorsValid = false;
                FRIKInterface::GetSingleton().
                    ClearHandPoseFingerPositions(isLeft);
                rock::HostClearExternalHeldBodies(isLeft);
                state.Clear();
                Heisenberg::GetSingleton().OnGrabEnded(isLeft);
            } else {
                // The marker returns before the primary object-drive path.
                // Keep its fallback (non-rich) FRIK finger pose alive and
                // allow one deferred mesh solve if acquisition happened while
                // calibration/geometry was temporarily unavailable.
                auto* playerNodes =
                    f4cf::f4vr::getPlayerNodes();
                RE::NiNode* wandNode =
                    playerNodes
                        ? heisenberg::GetWandNode(
                              playerNodes,
                              isLeft)
                        : nullptr;
                if (wandNode) {
                    if (!ResolvePendingFingerCurls(
                            state,
                            wandNode,
                            isLeft,
                            "Co-hold support")) {
                        (void)ApplyConfiguredFingerCurls(
                            state,
                            isLeft);
                    }
                }
            }
            return;
        }

        // CRITICAL: Validate reference via handle lookup BEFORE any method calls!
        // This prevents crashes when the game deletes objects we're holding.
        if (!state.HasValidRefr()) {
            spdlog::debug("[GRAB] UpdateGrab: {} hand reference invalid (object deleted?)", isLeft ? "Left" : "Right");
            (void)rock::touch_grab_bridge::
                ClearTouchGrabFingerPose(isLeft);
            GrabState& partner =
                isLeft ? _rightGrab : _leftGrab;
            if (partner.active && partner.coHeldSecondary) {
                HandAuthority::Clear(
                    OBJECT_COHOLD_HAND_TAG,
                    !isLeft);
                (void)rock::touch_grab_bridge::
                    ClearTouchGrabFingerPose(!isLeft);
                FRIKInterface::GetSingleton().
                    ClearHandPoseFingerPositions(!isLeft);
                rock::HostClearExternalHeldBodies(!isLeft);
                partner.Clear();
                Heisenberg::GetSingleton().
                    OnGrabEnded(!isLeft);
            }
            // Clean up grab state
            rock::HostClearExternalHeldBodies(isLeft);
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
            (void)rock::touch_grab_bridge::
                ClearTouchGrabFingerPose(isLeft);
            GrabState& partner =
                isLeft ? _rightGrab : _leftGrab;
            if (partner.active && partner.coHeldSecondary) {
                HandAuthority::Clear(
                    OBJECT_COHOLD_HAND_TAG,
                    !isLeft);
                (void)rock::touch_grab_bridge::
                    ClearTouchGrabFingerPose(!isLeft);
                FRIKInterface::GetSingleton().
                    ClearHandPoseFingerPositions(!isLeft);
                rock::HostClearExternalHeldBodies(!isLeft);
                partner.Clear();
                Heisenberg::GetSingleton().
                    OnGrabEnded(!isLeft);
            }
            rock::HostClearExternalHeldBodies(isLeft);
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

            (void)rock::touch_grab_bridge::
                ClearTouchGrabFingerPose(isLeft);
            GrabState& partner =
                isLeft ? _rightGrab : _leftGrab;
            if (partner.active && partner.coHeldSecondary) {
                HandAuthority::Clear(
                    OBJECT_COHOLD_HAND_TAG,
                    !isLeft);
                (void)rock::touch_grab_bridge::
                    ClearTouchGrabFingerPose(!isLeft);
                FRIKInterface::GetSingleton().
                    ClearHandPoseFingerPositions(!isLeft);
                rock::HostClearExternalHeldBodies(!isLeft);
                partner.Clear();
                Heisenberg::GetSingleton().
                    OnGrabEnded(!isLeft);
            }

            // Clear state safely (don't try to restore physics on deleted object)
            rock::HostClearExternalHeldBodies(isLeft);
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
                    RE::TESForm* storedBaseForm = nullptr;
                    bool stored =
                        StoreGrabbedItem(
                            refr,
                            showMessage,
                            &storedBaseForm);
                    
                    if (stored)
                    {
                        spdlog::debug("[GRAB] Auto-storage succeeded after {:.1f}s", storageHoldTime);
                        HeisenbergPluginAPI::InvokeStashedCallbacks(
                            isLeft,
                            storedBaseForm);
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
                        const float readFaceDist =
                            isReadableBook ? 20.0f : 10.0f;
                        // Test the held readable mesh, not the controller. A
                        // comic's saved hand offset can put its page 20+ gu
                        // closer to the face than the wrist, which made the
                        // intended gesture impossible despite the comic being
                        // visibly in front of the player's eyes.
                        RE::NiPoint3 readPoint =
                            state.node
                                ? state.node->worldBound.center
                                : handPos;
                        float meshDistance = -1.0f;
                        const float boundRadius =
                            state.node
                                ? std::clamp(
                                      state.node->worldBound.fRadius,
                                      0.0f,
                                      30.0f)
                                : 0.0f;
                        const bool meshCanReachReadZone =
                            (readPoint - hmdPos).Length() <=
                            readFaceDist + boundRadius + 3.0f;
                        if (state.node && meshCanReachReadZone) {
                            std::vector<TriangleData> readTriangles;
                            readTriangles.reserve(256);
                            GetTriangles(
                                state.node.get(),
                                readTriangles,
                                4096);
                            RE::NiPoint3 closestMeshPoint{};
                            if (GetClosestMeshPointToPoint(
                                    readTriangles,
                                    hmdPos,
                                    closestMeshPoint,
                                    meshDistance)) {
                                readPoint = closestMeshPoint;
                            }
                        }
                        const RE::NiPoint3 toNote = readPoint - hmdPos;
                        const float dist = toNote.Length();
                        // F4VR HMD forward is the negative Z column (the same
                        // convention used by Hand's visibility test). Require
                        // the note in FRONT of the face so
                        // the behind-head storage zone never counts as "brought to face".
                        const float inFront =
                            -(toNote.x * hmdRot.entry[0][2] +
                              toNote.y * hmdRot.entry[1][2] +
                              toNote.z * hmdRot.entry[2][2]);
                        // Paper notes must be brought RIGHT UP to the face (~14cm) — a deliberate
                        // gesture — so a note that just dropped to hand isn't read the instant you
                        // glance at it (the "notes go to the world on pickup" report). Magazines /
                        // comics are read at a comfortable ~28cm: you hold the comic up and look at
                        // it rather than mashing it into your nose. Reading a kBOOK grants its perk
                        // and pops the vanilla "collected an issue" splash — which is exactly the
                        // bring-to-face collect the user wants (and the ONLY place it should happen;
                        // the behind-head storage zone is gated off for books below).
                        const float grabAge =
                            static_cast<float>(Utils::GetTime()) -
                            state.grabStartTime;
                        const bool inReadZone =
                            grabAge >= 0.35f &&
                            dist < readFaceDist &&
                            inFront > 0.0f;
                        if (inReadZone) {
                            state.faceReadTimer += deltaTime;
                            if (state.faceReadTimer <= deltaTime * 1.5f) {
                                spdlog::debug(
                                    "[GRAB] {} entered face-read zone "
                                    "(meshDist={:.1f} front={:.1f} age={:.2f})",
                                    isReadableBook ? "Magazine" : "Note",
                                    meshDistance >= 0.0f
                                        ? meshDistance
                                        : dist,
                                    inFront,
                                    grabAge);
                            }
                        } else {
                            state.faceReadTimer = 0.0f;
                        }
                        constexpr float kFaceReadDwellSeconds = 0.20f;
                        if (state.faceReadTimer >=
                            kFaceReadDwellSeconds)
                        {
                            RE::NiPointer<RE::TESObjectREFR> keep(refr);
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            RE::TESForm* baseForm =
                                keep ? keep->GetObjectReference() : nullptr;
                            bool activated = false;
                            if (player && keep) {
                                // ActivateRef can emit a container-changed
                                // event synchronously. Mark it first so
                                // loot-to-hand cannot immediately re-grab the
                                // comic we are deliberately collecting.
                                if (baseForm) {
                                    heisenberg::DropToHand::GetSingleton().
                                        MarkAsRecentlyStored(
                                            baseForm->GetFormID());
                                }
                                // Bypass our activate hook — we WANT the native note overlay /
                                // magazine read here (the deliberate face gesture).
                                heisenberg::Hooks::SetInternalActivation(true);
                                activated = keep->ActivateRef(
                                    player,
                                    nullptr,
                                    1,
                                    false,
                                    false,
                                    false);
                                heisenberg::Hooks::SetInternalActivation(false);
                            }

                            if (activated) {
                                // Match the normal storage ordering: consume
                                // while the hold is still intact, then use the
                                // storage teardown which intentionally skips
                                // restoring a disappearing reference.
                                EndGrab(isLeft, nullptr, true);
                                // ActivateRef fires the read/collect but does NOT reliably remove
                                // the placed world reference for kNOTE/kBOOK (the exact reason
                                // StoreGrabbedItem uses AddObjectToContainer + SafeDisableRef).
                                // Without this the note/magazine survives as a frozen world ref at
                                // the last hand position → it floats. Disable it like every other
                                // storage path (SafeDisableRef defers for behavior-graph items).
                                SafeDisableRef(keep.get());
                                HeisenbergPluginAPI::InvokeStashedCallbacks(
                                    isLeft,
                                    baseForm);
                                spdlog::info(
                                    "[GRAB] {} read at face "
                                    "(dist={:.1f}cm) - collected/opened + taken",
                                    isReadableBook
                                        ? "Magazine"
                                        : "Note",
                                    dist);
                                return;  // grab is over
                            }

                            // A script or activation condition can reject the
                            // read. Keep the object held and retain its physics
                            // state so a failed activation never strands a
                            // keyframed, collision-suppressed world reference.
                            state.faceReadTimer = 0.0f;
                            spdlog::warn(
                                "[GRAB] {} face-read activation failed for "
                                "{:08X}; keeping object held",
                                isReadableBook ? "Magazine" : "Note",
                                keep ? keep->formID : 0);
                        }
                    } else {
                        state.faceReadTimer = 0.0f;
                    }
                } else {
                    state.faceReadTimer = 0.0f;
                }
            }
            else {
                state.faceReadTimer = 0.0f;
            }
        }

        // =====================================================================
        // VH HOLSTER ZONE CHECK — haptic when grabbed weapon enters VH zone
        // =====================================================================
        // Throwables are WEAP records and pass IsWeapon(), so they must be excluded
        // explicitly or grenades holster on VH zones alongside guns.
        const bool throwableHolsterBlocked =
            !heisenberg::g_config.enableThrowableHolstering && IsThrowableWeapon(refr);
        if (!configMode.IsRepositionModeActive() && !state.isPulling && IsWeapon(refr) && !throwableHolsterBlocked)
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
        // CONSUME ZONE TRACKING / OPTIONAL AUTO-CONSUME
        // =====================================================================
        // Mouth zone: bring consumable to face (food, drinks, chems)
        // Hand injection zone: bring consumable to opposite hand (syringes)
        // Normal consumption commits on physical grip release in Hand::Release.
        // The legacy in-zone commit remains available only through the two
        // explicit, default-off auto-consume settings.
        if (!configMode.IsRepositionModeActive() && !state.isPulling)
        {
            // Injectables (stimpak, RadAway, med-x, etc.) are wrist-injected, not
            // eaten — when hand injection is enabled they must NOT mouth-consume.
            // (Falls back to mouth if hand injection is disabled, so they're still usable.)
            const bool isDiseaseCureItem = IsDiseaseCureItem(refr);
            const bool isCompanionStimpakItem =
                IsCompanionStimpakItem(refr);
            const bool isInjectableItem = IsInjectable(refr);
            bool inMouthZone = !isDiseaseCureItem && IsInMouthZone(isLeft) &&
                               !(isInjectableItem && heisenberg::g_config.enableHandInjection);
            // Hand injection zone only activates for injectables (chems, stimpaks — not food/drinks)
            bool inHandInjectionZone = !isDiseaseCureItem &&
                                       isInjectableItem &&
                                       IsInHandInjectionZone(isLeft);
            bool inAnyConsumeZone = inMouthZone || inHandInjectionZone;

            // Keep the public zone-query state live even when auto-consume is
            // disabled. CheckMouthConsume/CheckHandInjectionConsume may refine
            // these while their respective auto mode is enabled.
            state.isInMouthZone = inMouthZone;
            state.isInHandInjectionZone = inHandInjectionZone;

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

            // A programmatically delivered item that appeared between seated
            // hands is intentionally unarmed until the player separates it
            // from both consume zones once. This prevents a stationary lap
            // pose from merely waiting out the time grace and auto-consuming.
            if (!inAnyConsumeZone) {
                consumeAttemptedThisVisit = false;
                state.consumeZoneExitRequired = false;
            }

            // COMPANION STIMPAK INJECTION — checked before either SS2 treatment
            // or self-use. A valid contact follows Fallout's normal companion
            // ActivateRef route; without a wounded current companion contact,
            // the Stimpak remains eligible for ordinary player use below.
            if (!consumeAttemptedThisVisit &&
                heisenberg::g_config.enableCompanionStimpakInjection &&
                isCompanionStimpakItem)
            {
                const float elapsedSinceGrab =
                    static_cast<float>(heisenberg::Utils::GetTime()) -
                    state.grabStartTime;
                const NpcInjectionAttempt injection =
                    TryInjectHeldCompanionStimpak(
                        isLeft,
                        refr,
                        state.node.get(),
                        elapsedSinceGrab,
                        state.handSpeed);
                if (injection.result !=
                    NpcInjectionResult::NotAttempted) {
                    consumeAttemptedThisVisit = true;
                    if (injection.result !=
                        NpcInjectionResult::FailedKeptInHand) {
                        g_vrInput.TriggerHaptic(
                            isLeft,
                            injection.result ==
                                    NpcInjectionResult::Accepted ?
                                2000 :
                                500);
                        EndGrab(isLeft, nullptr, true);
                        return;
                    }
                    g_vrInput.TriggerHaptic(isLeft, 500);
                }
            }

            // NPC INJECTION (disease cures only) — checked BEFORE the self-consume zones,
            // because the self-injection zone sits on the opposite wand and can easily be near
            // another actor while you reach toward them. SS2's manager owns
            // the final eligibility check, item consumption/refund, and cure.
            if (!consumeAttemptedThisVisit &&
                heisenberg::g_config.enableNpcInjection &&
                isDiseaseCureItem)
            {
                const float elapsedSinceGrab =
                    static_cast<float>(heisenberg::Utils::GetTime()) -
                    state.grabStartTime;
                const NpcInjectionAttempt injection =
                    TryInjectHeldDiseaseCure(
                        isLeft,
                        refr,
                        state.node.get(),
                        elapsedSinceGrab,
                        state.handSpeed);
                if (injection.result != NpcInjectionResult::NotAttempted) {
                    consumeAttemptedThisVisit = true;
                    if (injection.result !=
                        NpcInjectionResult::FailedKeptInHand) {
                        g_vrInput.TriggerHaptic(
                            isLeft,
                            injection.result == NpcInjectionResult::Accepted
                                ? 2000
                                : 500);
                        EndGrab(isLeft, nullptr, true);
                        return;
                    }
                    g_vrInput.TriggerHaptic(isLeft, 500);
                }
            }

            // Optional auto-consumption: mouth zone OR hand injection zone.
            // These keep the existing cooldown/exit/speed safety policy; the
            // physical release path intentionally bypasses those readiness
            // gates and checks only the current valid spatial route.
            // Blocked in Power Armor — PA helmet prevents eating/drinking/injecting
            bool shouldConsume = false;
            const char* zoneLabel = "";
            if (!consumeAttemptedThisVisit && IsConsumable(refr) && !(g_config.blockConsumptionInPA && Utils::IsPlayerInPowerArmor()))
            {
                if (g_config.autoConsumeInMouthArea &&
                    inMouthZone &&
                    CheckMouthConsume(isLeft, state)) {
                    shouldConsume = true;
                    zoneLabel = "mouth auto";
                } else if (g_config.autoConsumeInWristArea &&
                           inHandInjectionZone &&
                           CheckHandInjectionConsume(isLeft, state)) {
                    shouldConsume = true;
                    zoneLabel = "hand injection auto";
                }
            }

            if (shouldConsume)
            {
                consumeAttemptedThisVisit = true;
                spdlog::debug("[GRAB] Consumable entered {} zone - attempting consume (one-shot)!", zoneLabel);
                (void)CommitHeldConsumableConsumption(
                    isLeft,
                    refr,
                    zoneLabel);
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
                            float measuredSeparation = (std::numeric_limits<float>::max)();
                            const bool touchingWeaponHand =
                                CheckWeaponEquipByHandContact(
                                    state,
                                    state.handSpeed,
                                    isLeft,
                                    &measuredSeparation);
                            if (!state.weaponEquipZoneArmed) {
                                // Edge-trigger arming: a Pip-Boy drop-to-hand can
                                // materialise the weapon already intersecting the
                                // weapon hand, which would equip it the instant the
                                // player lets go. Require one confirmed CLEAR-of-the-hand
                                // measurement, with the Pip-Boy closed, before a touch
                                // can arm the equip. An unmeasurable frame leaves the
                                // separation at max and does NOT arm.
                                if (measuredSeparation > kWeaponEquipContactSkin &&
                                    measuredSeparation < (std::numeric_limits<float>::max)() &&
                                    !heisenberg::MenuChecker::GetSingleton().IsPipboyOpen()) {
                                    state.weaponEquipZoneArmed = true;
                                    spdlog::info("[WEAP-EQUIP] armed — held weapon is clear of the weapon hand (separation={:.2f}gu)", measuredSeparation);
                                }
                            } else {
                                weaponNearFingertip = touchingWeaponHand;
                            }
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
                    RE::bhkWorld* bhkWorld =
                        ResolveMatchingHeldBhkWorld(state);
                    state.savedState.savedBhkWorld = bhkWorld;
                    state.savedState.motionType = RE::hknpMotionPropertiesId::Preset::DYNAMIC;
                    state.savedState.collisionObjectFlags = state.collisionObject->flags.flags;
                    spdlog::debug("[GRAB-KEYFRAMED] Saved collision object flags: {:X}",
                                 state.savedState.collisionObjectFlags);

                    // Instant DropToHand placement captured before moving the
                    // visual root. Reuse that exact set when it is still live;
                    // recapturing now would freeze the artificial
                    // spawn-to-hand bodyOwnerLocal delta. A failed/stale
                    // pre-capture deliberately falls back to the selected
                    // wrapper rather than retaining a partial body set.
                    const bool instantPreCaptureAttempted =
                        state.instantPreTeleportBodyCaptureAttempted;
                    bool reusedInstantPreCapture = false;
                    if (instantPreCaptureAttempted) {
                        reusedInstantPreCapture =
                            CapturedHeldBodySetStillOwned(state);
                        if (!reusedInstantPreCapture) {
                            const auto rejectedBodyCount =
                                state.capturedHeldBodyCount;
                            ClearCapturedHeldBodyFrames(state);
                            spdlog::warn(
                                "[GRAB-INSTANT-BODY] {:08X} rejected "
                                "pre-teleport capture (bodies={}); not "
                                "recapturing after visual teleport",
                                refr ? refr->formID : 0,
                                rejectedBodyCount);
                        } else {
                            spdlog::info(
                                "[GRAB-INSTANT-BODY] {:08X} reusing {} "
                                "pre-teleport body frame(s)",
                                refr ? refr->formID : 0,
                                state.capturedHeldBodyCount);
                        }
                    } else {
                        CaptureHeldCollisionBodyFrames(state);
                    }
                    state.instantPreTeleportBodyCaptureAttempted = false;
                    if (!PublishExternalHeldBodiesForPlayerSuppression(
                            state,
                            isLeft)) {
                        spdlog::warn(
                            "[GRAB-FILTER] Could not publish exact held-body "
                            "identity before KEYFRAMED authority for {:08X}",
                            refr ? refr->formID : 0);
                    }
                    const bool selectedWrapperScope =
                        RequiresSelectedCollisionWrapperMotionScope(state);
                    state.heldMotionScopeIsReferenceSubtree =
                        !selectedWrapperScope;
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
                        // Preserve bits 7-31 and replace only the low layer bits
                        // with BIPED_NO_CC. Unlike the old SetLayerLocked
                        // experiment, this cannot clobber authored group/system
                        // metadata and release restores the full original word.
                        if (TryDisablePlayerHeldObjectCollision(
                                state,
                                bhkWorld)) {
                            state.heldPlayerFilterApplied = true;
                        }
                        spdlog::info("[GRAB-KEYFRAMED] {:08X} held object kept COLLIDABLE on temporary BIPED_NO_CC layer — will push other objects",
                                     refr ? refr->formID : 0);
                    } else {
                        if (state.capturedHeldBodySetValid) {
                            if (ApplyCapturedHeldBodyFilters(
                                    state,
                                    bhkWorld,
                                    false)) {
                                state.heldPlayerFilterApplied = true;
                                spdlog::debug(
                                    "[GRAB-KEYFRAMED] Set every captured "
                                    "body to kNonCollidable (15) with "
                                    "byte-exact per-body restore");
                            } else {
                                spdlog::warn(
                                    "[GRAB-KEYFRAMED] Could not apply "
                                    "multipart non-collidable filters; "
                                    "leaving native filters intact rather "
                                    "than collapsing per-body metadata");
                            }
                        } else {
                            CaptureHeldObjectLayerBeforeChange(state);
                            bhkUtilFunctions_SetLayerLocked(
                                state.node.get(),
                                15,
                                bhkWorld);
                            spdlog::debug(
                                "[GRAB-KEYFRAMED] Set collision subtree "
                                "to kNonCollidable (15) through scalar "
                                "fallback");
                        }
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
                    if (wandNodePP && state.node &&
                        !HasStoredFingerCurls(state) &&
                        spdlog::should_log(spdlog::level::warn)) {
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

        // Keep a collidable, keyframed host-held object from shoving the player.
        // Exact body publication is level-triggered: a transient setup failure
        // is retried and a successful snapshot is revalidated/replaced at the
        // same low cadence as the secondary filter word. This registry path
        // remains active during pull-in because the body can already be
        // keyframed then; only the optional group-filter rewrite waits for pull
        // completion.
        if (g_config.heldObjectCollidable &&
            state.keyframedSetupComplete &&
            state.collisionObject)
        {
            const bool cadence =
                (state.heldPlayerFilterFrames++ % 30 == 0);
            bool exactBodiesPublished = !cadence;
            if (cadence) {
                exactBodiesPublished =
                    PublishExternalHeldBodiesForPlayerSuppression(
                        state,
                        isLeft);
            }

            if (!state.isPulling && cadence) {
                // Drop the cached player body so resolution re-walks the proxy-first chain.
                // Recovers if a previous success latched the wrong (ragdoll) body.
                heisenberg::Physics::InvalidatePlayerBodyId();
                if (auto* filterWorld =
                        ResolveMatchingHeldBhkWorld(state);
                    filterWorld &&
                    TryDisablePlayerHeldObjectCollision(
                        state,
                        filterWorld)) {
                    state.heldPlayerFilterApplied = true;
                }

                /*
                 * TryDisablePlayerHeldObjectCollision can discover that a
                 * multipart capture is no longer usable and fall back to the
                 * selected live body. If the first publication failed for that
                 * reason, publish the fallback now instead of leaving the
                 * character-controller filter unprotected for another
                 * 30-frame cadence.
                 */
                if (!exactBodiesPublished && state.active) {
                    exactBodiesPublished =
                        PublishExternalHeldBodiesForPlayerSuppression(
                            state,
                            isLeft);
                }
            }

            if (cadence &&
                !exactBodiesPublished &&
                state.active) {
                spdlog::warn(
                    "[GRAB-FILTER] Exact held-body publication is not "
                    "ready for active {}-hand grab {:08X}; preserving the "
                    "last complete snapshot and retrying",
                    isLeft ? "left" : "right",
                    state.GetRefr()
                        ? state.GetRefr()->GetFormID()
                        : 0);
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
            // Seed it only for a brand-new grab. During a transition, leaving the old
            // identity in place is what makes PostPhysicsGrabUpdate recapture the
            // newly appeared wrapper instead of silently declaring it synchronized.
            if (!state.lastSyncedBhkWorld) {
                if (auto* worldPlayer2 = RE::PlayerCharacter::GetSingleton()) {
                    if (auto* worldPlayerCell2 = worldPlayer2->GetParentCell()) {
                        state.lastSyncedBhkWorld =
                            worldPlayerCell2->GetbhkWorld();
                    }
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
            auto* bhkWorld =
                ResolveMatchingHeldBhkWorld(state);
            if (bhkWorld) {
                const bool selectedWrapperScope =
                    !state.heldMotionScopeIsReferenceSubtree;
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

        // Periodic held-object-to-palm distance diagnostic. GetTriangles plus
        // the full closest-point walk is expensive on a high-poly held item, so
        // this must never run merely because warning logging is enabled in an
        // ordinary release session. Require explicit debug logging, sample each
        // hand independently, cap traversal, and retain scratch capacity.
        if (!state.isPulling &&
            !HasStoredFingerCurls(state) &&
            spdlog::should_log(spdlog::level::debug)) {
            static std::array<std::uint16_t, 2>
                heldDistCounters{};
            auto& heldDistCounter =
                heldDistCounters[isLeft ? 0u : 1u];
            if (++heldDistCounter >= 180) {
                heldDistCounter = 0;
                auto* playerNodesHD = f4cf::f4vr::getPlayerNodes();
                RE::NiNode* wandNodeHD = playerNodesHD ? heisenberg::GetWandNode(playerNodesHD, isLeft) : nullptr;
                if (wandNodeHD) {
                    RE::NiPoint3 palmPosHD = GetPalmPosition(wandNodeHD, isLeft);
                    float pivotDistHD = (objPos - palmPosHD).Length();
                    static thread_local
                        std::vector<heisenberg::TriangleData>
                            trisHD;
                    trisHD.clear();
                    if (trisHD.capacity() < 256) {
                        trisHD.reserve(256);
                    }
                    heisenberg::GetTriangles(
                        state.node.get(),
                        trisHD,
                        4096);
                    RE::NiPoint3 meshPtHD;
                    float meshDistHD = -1.0f;
                    if (!trisHD.empty()) {
                        heisenberg::GetClosestMeshPointToPoint(trisHD, palmPosHD, meshPtHD, meshDistHD);
                    }
                    spdlog::debug(
                        "[HELD-DIST] '{}' palm=({:.1f},{:.1f},{:.1f}) "
                        "pivot=({:.1f},{:.1f},{:.1f}) pivotDist={:.2f}cm "
                        "meshDist={:.2f}cm tris={}",
                        state.node->name.c_str(),
                        palmPosHD.x,
                        palmPosHD.y,
                        palmPosHD.z,
                        objPos.x,
                        objPos.y,
                        objPos.z,
                        pivotDistHD,
                        meshDistHD,
                        trisHD.size());
                }
            }
        }
    }

    void GrabManager::EndGrab(bool isLeft, const RE::NiPoint3* throwVelocity, bool forStorage,
                              const RE::NiPoint3* throwAngularVelocity)
    {
        // Get state for this hand first
        GrabState& state = isLeft ? _leftGrab : _rightGrab;
        (void)rock::touch_grab_bridge::
            ClearTouchGrabFingerPose(isLeft);
        state.rockRichFingerPosePublished = false;

        // Two-handed grab cleanup:
        // (a) If THIS hand is the secondary aim hand, it owns no physics — just clear the
        //     marker and return; the primary keeps holding (its next drive sees no partner).
        if (state.coHeldSecondary) {
            GrabState& primary =
                isLeft ? _rightGrab : _leftGrab;
            const bool endStoredPrimary =
                forStorage &&
                primary.active &&
                primary.GetRefr() == state.GetRefr();
            primary.coHoldAnchorsValid = false;
            HandAuthority::Clear(
                OBJECT_COHOLD_HAND_TAG,
                isLeft);
            auto& frik = FRIKInterface::GetSingleton();
            frik.ClearHandPoseFingerPositions(isLeft);
            Heisenberg::GetSingleton().
                SetFingerCurlValue(isLeft, 1.0f);
            rock::HostClearExternalHeldBodies(isLeft);
            state.Clear();
            Heisenberg::GetSingleton().OnGrabEnded(isLeft);

            if (endStoredPrimary) {
                spdlog::info(
                    "[GRAB] Two-handed: {} support initiated storage — ending primary ownership too",
                    isLeft ? "left" : "right");
                EndGrab(!isLeft, nullptr, true);
            } else {
                spdlog::info(
                    "[GRAB] Two-handed: {} aim hand released — primary keeps the object",
                    isLeft ? "Left" : "Right");
            }
            return;
        }
        // (b) If THIS (primary) hand releases while the other hand is still
        // gripping the same object, transfer the complete physics ownership to
        // that hand instead of dropping the object. The object is rebased from
        // its current world pose, so the handoff cannot snap even though the
        // old grab placement was expressed in the releasing hand's frame.
        {
            GrabState& partner = isLeft ? _rightGrab : _leftGrab;
            if (partner.active && partner.coHeldSecondary && partner.GetRefr() == state.GetRefr()) {
                if (forStorage) {
                    // Inventory activation consumes the shared world object.
                    // Do not promote the support marker into ownership of a
                    // reference that is already being stored/consumed.
                    HandAuthority::Clear(
                        OBJECT_COHOLD_HAND_TAG,
                        !isLeft);
                    auto& frik = FRIKInterface::GetSingleton();
                    frik.ClearHandPoseFingerPositions(!isLeft);
                    Heisenberg::GetSingleton().
                        SetFingerCurlValue(!isLeft, 1.0f);
                    rock::HostClearExternalHeldBodies(!isLeft);
                    partner.Clear();
                    Heisenberg::GetSingleton().
                        OnGrabEnded(!isLeft);
                    spdlog::info(
                        "[GRAB] Two-handed: storage ended {} support marker before primary cleanup",
                        isLeft ? "right" : "left");
                } else {
                const bool promotedIsLeft = !isLeft;
                const RE::NiTransform objectWorld =
                    state.node ? state.node->world : RE::NiTransform{};

                // Preserve the support hand's solved pose; the primary state
                // contains the physics/lifetime data but its finger curls
                // belong to the hand that is releasing.
                const bool supportHasFingerCurls =
                    partner.hasRuntimeFingerCurls;
                const bool supportHasJointCurls =
                    partner.hasRuntimeJointCurls;
                const auto supportJointCurls =
                    partner.runtimeJointCurls;
                const float supportThumb = partner.runtimeThumbCurl;
                const float supportIndex = partner.runtimeIndexCurl;
                const float supportMiddle = partner.runtimeMiddleCurl;
                const float supportRing = partner.runtimeRingCurl;
                const float supportPinky = partner.runtimePinkyCurl;
                const bool supportRichPose =
                    partner.rockRichFingerPosePublished;
                const bool supportNeedsFingerSolve =
                    partner.pendingFingerCurls ||
                    !supportHasFingerCurls;

                std::swap(state, partner);
                partner.coHeldSecondary = false;
                partner.coHoldAnchorsValid = false;
                partner.coHoldPrimaryAnchorObjectLocal = {};
                partner.coHoldSecondaryAnchorObjectLocal = {};
                partner.coHoldSecondaryHandObjectLocal = {};
                partner.coHoldSecondaryHandObjectLocal.rotate.MakeIdentity();
                partner.coHoldSecondaryHandObjectLocal.scale = 1.0f;

                partner.ClearRuntimeFingerCurls();
                if (supportHasFingerCurls) {
                    if (supportHasJointCurls) {
                        partner.SetRuntimeJointCurls(supportJointCurls);
                    } else {
                        partner.SetRuntimeFingerCurls(
                            supportThumb,
                            supportIndex,
                            supportMiddle,
                            supportRing,
                            supportPinky);
                    }
                }
                partner.rockRichFingerPosePublished =
                    supportRichPose;
                // A promoted support/receiving hand owns the object at its
                // current visible-mesh contact, not at the releasing hand's
                // authored offset. Keep mesh posing authoritative and request
                // one final-pose solve if acquisition-time calibration was not
                // available.
                partner.isPulling = false;
                partner.isNaturalGrab = true;
                partner.isTelekinesis = true;
                partner.naturalFingerPosing = true;
                partner.physicalTouchGrab = true;
                partner.pendingFingerCurls =
                    supportNeedsFingerSolve;

                RE::NiNode* promotedHand =
                    GetSkinnedHandNode(promotedIsLeft);
                if (!promotedHand) {
                    if (auto* playerNodes =
                            f4cf::f4vr::getPlayerNodes()) {
                        promotedHand =
                            heisenberg::GetWandNode(
                                playerNodes,
                                promotedIsLeft);
                    }
                }
                if (promotedHand && partner.node) {
                    RE::NiPoint3 paLocal{};
                    if (Utils::IsPlayerInPowerArmor()) {
                        paLocal.x = g_config.paGrabOffsetX;
                        paLocal.y = g_config.paGrabOffsetY;
                        paLocal.z = g_config.paGrabOffsetZ;
                    }
                    // ORTHONORMALIZE BEFORE INVERTING. Transpose is only the inverse of an
                    // orthonormal matrix; on a drifted one it leaves an M^T*M factor that
                    // stretches the object. This runs on EVERY hand-to-hand promotion and the
                    // result is captured back into objectWorld on the next one, so the factor
                    // COMPOUNDS: a few handovers visibly grow the object (owner: bottle, watch,
                    // ammo box, desk fan). Proven by the [GRAB-SCALE] witness, which showed
                    // local/world/parent scale all pinned at exactly 1.0000 through every
                    // promotion while the object still grew - so it was never the scale field.
                    RE::NiMatrix3 promotedHandRot = promotedHand->world.rotate;
                    heisenberg::OrthoNormalize(promotedHandRot);

                    RE::NiPoint3 promotedLocalPos =
                        promotedHandRot *
                        (objectWorld.translate -
                         promotedHand->world.translate);
                    promotedLocalPos -= paLocal;
                    RE::NiMatrix3 promotedLocalRot =
                        objectWorld.rotate *
                        promotedHandRot.Transpose();
                    heisenberg::OrthoNormalize(promotedLocalRot);
                    partner.SetRigidRenderedHandPlacement(
                        promotedLocalPos,
                        promotedLocalRot);
                    partner.grabOffsetLocal = promotedLocalPos;
                    partner.lastHandPos =
                        promotedHand->world.translate;
                    partner.velocityTrackingInit = false;
                }

                HandAuthority::Clear(
                    OBJECT_COHOLD_HAND_TAG,
                    promotedIsLeft);

                // Transfer the exact held-body publication before clearing the
                // releasing hand's slot. At every instant at least one complete
                // registry snapshot therefore protects the still-keyframed
                // object from the player solver.
                const bool promotedBodiesPublished =
                    PublishExternalHeldBodiesForPlayerSuppression(
                        partner,
                        promotedIsLeft);
                if (!promotedBodiesPublished) {
                    spdlog::error(
                        "[GRAB-FILTER] Could not transfer exact held-body "
                        "identity to the promoted {} hand for {:08X}; "
                        "preserving the previous hand snapshot for retry",
                        promotedIsLeft ? "left" : "right",
                        partner.GetRefr()
                            ? partner.GetRefr()->GetFormID()
                            : 0);
                }

                // `state` now contains the old marker only. Clearing it must
                // not restore physics or emit a Dropped callback.
                state.Clear();
                Heisenberg::GetSingleton().OnGrabEnded(isLeft);

                auto& configMode =
                    ItemPositionConfigMode::GetSingleton();
                configMode.OnGrabEnded(isLeft);
                configMode.OnGrabStarted(
                    &partner,
                    promotedIsLeft);

                // SCALE WITNESS. The object grows across repeated hand-to-hand promotions and
                // three fixed local/world conflations did not stop it, so capture the actual
                // numbers rather than reason about them: if node world.scale climbs across
                // promotions the writer is downstream of here; if local.scale climbs while
                // world.scale holds, the parent scale is the multiplier; if the parent's scale
                // is not 1.0 the object is parented to something scaled.
                if (partner.node) {
                    // Row norms, not scale. The scale field was proven innocent (it read
                    // exactly 1.0000 through every promotion while the object still grew);
                    // the real drift is in the rotation basis, where a row norm of r stretches
                    // that axis by r^2. Anything other than 1.000 here is the bug returning.
                    const auto rowNorm = [](const RE::NiMatrix3& m, int r) {
                        return std::sqrt(m.entry[r][0] * m.entry[r][0] +
                                         m.entry[r][1] * m.entry[r][1] +
                                         m.entry[r][2] * m.entry[r][2]);
                    };
                    const RE::NiMatrix3& w = partner.node->world.rotate;
                    spdlog::warn(
                        "[GRAB-SCALE] promotion: node='{}' scale={:.4f} "
                        "worldRotRowNorms=({:.4f},{:.4f},{:.4f})",
                        partner.node->name.c_str(),
                        partner.node->world.scale,
                        rowNorm(w, 0), rowNorm(w, 1), rowNorm(w, 2));
                }

                spdlog::info(
                    "[GRAB] Two-handed: {} primary released — "
                    "{} hand promoted without dropping ref {:08X}",
                    isLeft ? "left" : "right",
                    promotedIsLeft ? "left" : "right",
                    partner.GetRefr()
                        ? partner.GetRefr()->GetFormID()
                        : 0);
                return;
                }
            }
        }
        state.coHoldAnchorsValid = false;

        if (!state.active) {
            rock::HostClearExternalHeldBodies(isLeft);
            return;
        }
        
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
            rock::HostClearExternalHeldBodies(isLeft);
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
            rock::HostClearExternalHeldBodies(isLeft);
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
            (g_config.enableDropToCompanion ||
             g_config.enableDropToContainer) &&
            refr
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
                    if (g_config.enableDropToCompanion &&
                        targetRefr->GetFormType() == RE::ENUM_FORM_ID::kACHR)
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
                        
                        spdlog::debug("[COMPANION] Actor {:08X}: cmdFlag={}, cmdByPlayer={}, inPlayerList={}, inCompFaction={}",
                                    actor->formID, hasCommandedFlag, commandedByPlayer, inPlayerCommandList, inCompanionFaction);
                        
                        // Inventory transfer is destructive to the world
                        // reference. Only actual commanded/current companions
                        // are valid targets; a merely neutral vendor or settler
                        // must never silently receive the item.
                        if (hasCommandedFlag || commandedByPlayer ||
                            inPlayerCommandList || inCompanionFaction)
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
            rock::HostClearExternalHeldBodies(isLeft);
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
        rock::HostClearExternalHeldBodies(isLeft);
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

        // Direct state resets bypass EndGrab, so explicitly release every
        // external pose/authority writer first. Otherwise a load/reset can
        // leave a ROCK_Grab finger pose or object co-hold hand target active.
        HandAuthority::Clear(OBJECT_COHOLD_HAND_TAG, true);
        HandAuthority::Clear(OBJECT_COHOLD_HAND_TAG, false);
        (void)rock::touch_grab_bridge::
            ClearTouchGrabFingerPose(true);
        (void)rock::touch_grab_bridge::
            ClearTouchGrabFingerPose(false);
        auto& frik = FRIKInterface::GetSingleton();
        
        // Clear left hand grab state
        if (_leftGrab.active) {
            spdlog::debug("[GRAB] Clearing active left grab");
            frik.ClearHandPoseFingerPositions(true);
            Heisenberg::GetSingleton().SetFingerCurlValue(true, 1.0f);
            Heisenberg::GetSingleton().OnGrabEnded(true);
            ItemPositionConfigMode::GetSingleton().OnGrabEnded(true);
            _leftGrab.Clear();
        }
        
        // Clear right hand grab state
        if (_rightGrab.active) {
            spdlog::debug("[GRAB] Clearing active right grab");
            frik.ClearHandPoseFingerPositions(false);
            Heisenberg::GetSingleton().SetFingerCurlValue(false, 1.0f);
            Heisenberg::GetSingleton().OnGrabEnded(false);
            ItemPositionConfigMode::GetSingleton().OnGrabEnded(false);
            _rightGrab.Clear();
        }

        if (heisenberg::IsRockEngineHosted()) {
            rock::HostNotifyExternalGrab(true, false);
            rock::HostNotifyExternalGrab(false, false);
        }
        rock::HostClearExternalHeldBodies(true);
        rock::HostClearExternalHeldBodies(false);
        
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

                // Capture while the rendered owner nodes and dynamic Havok
                // bodies still describe the same spawn pose. The visual
                // teleport below intentionally does not move physics; a first
                // capture after that move would preserve the artificial
                // spawn-to-hand delta as bodyOwnerLocal for the whole hold.
                const bool capturedBeforeVisualTeleport =
                    state.collisionObject &&
                    state.node &&
                    CaptureHeldCollisionBodyFrames(state);
                state.instantPreTeleportBodyCaptureAttempted = true;
                spdlog::info(
                    "[GRAB-INSTANT-BODY] {:08X} pre-teleport capture "
                    "{} (bodies={}); setup will {}",
                    refr->formID,
                    capturedBeforeVisualTeleport
                        ? "succeeded"
                        : "failed",
                    state.capturedHeldBodyCount,
                    capturedBeforeVisualTeleport
                        ? "reuse this body set"
                        : "use selected-wrapper fallback");

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
                    // WORLD scale. UpdateKeyframedNode treats this as a world transform and
                    // derives local by dividing by the parent's world scale, so feeding it
                    // local.scale means local <- local / parentScale. This is the instant
                    // placement used by hand-to-hand transfer, so the object was rescaled on
                    // EVERY handover, compounding each time (owner: bottle, watch, ammo box
                    // all grew when passed between hands).
                    targetTransform.scale = state.node->world.scale > 0.0f
                        ? state.node->world.scale
                        : 1.0f;
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
        if (g_hasDeferredFilterRestores.load(
                std::memory_order_acquire)) {
            void* tickWorld = nullptr;
            if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->parentCell) {
                if (auto* bhk = player->parentCell->GetbhkWorld()) {
                    tickWorld = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
                }
            }
            if (tickWorld) { TickDeferredFilterRestores(tickWorld); }
        }

        // A live config reload may turn two-hand object grabs off while a
        // support marker is active. End the marker immediately so neither its
        // rendered-hand authority nor ROCK's external-grab lease can linger.
        if (!g_config.enableTwoHandedGrab) {
            if (_leftGrab.active && _leftGrab.coHeldSecondary) {
                spdlog::info(
                    "[GRAB] Two-handed object grab disabled - releasing left "
                    "support hand");
                EndGrab(true, nullptr);
            }
            if (_rightGrab.active && _rightGrab.coHeldSecondary) {
                spdlog::info(
                    "[GRAB] Two-handed object grab disabled - releasing right "
                    "support hand");
                EndGrab(false, nullptr);
            }
            _leftGrab.coHoldAnchorsValid = false;
            _rightGrab.coHoldAnchorsValid = false;
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

        // Check genuinely blocking menus, but continue if we have an active grab.
        // Favorites and Pip-Boy are live-world overlays and are not part of this gate.
        // Use cached menu state from MenuChecker for thread safety (avoids race conditions)
        auto& menuChecker = MenuChecker::GetSingleton();
        bool menuBlocking = menuChecker.IsPaused() || menuChecker.IsLoading() ||
                            menuChecker.IsMainMenu();
        
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

                // Two-point co-hold: swing the captured object-local vector
                // between the two visible-mesh grab spots toward the tracked
                // palm line, then translate about the primary mesh anchor.
                // This works for a shovel grabbed anywhere along its shaft;
                // it does not assume the object's +Y axis is its useful axis.
                if (g_config.enableTwoHandedGrab && !state.isPulling) {
                    GrabState& aimPartner = isLeft ? _rightGrab : _leftGrab;
                    if (aimPartner.active && aimPartner.coHeldSecondary &&
                        aimPartner.GetRefr() == stateRefr) {
                        if (RE::NiNode* aimWand = heisenberg::GetWandNode(playerNodes, !isLeft)) {
                            if (state.coHoldAnchorsValid) {
                                RE::NiTransform baseTarget{};
                                baseTarget.rotate = targetRot;
                                baseTarget.translate = targetPos;
                                baseTarget.scale =
                                    state.node->world.scale > 0.0f
                                        ? state.node->world.scale
                                        : 1.0f;
                                const RE::NiPoint3 primaryAnchorWorld =
                                    rock::transform_math::localPointToWorld(
                                        baseTarget,
                                        state.coHoldPrimaryAnchorObjectLocal);
                                const RE::NiPoint3 secondaryAnchorWorld =
                                    rock::transform_math::localPointToWorld(
                                        baseTarget,
                                        state.coHoldSecondaryAnchorObjectLocal);
                                RE::NiPoint3 secondaryPalmWorld{};
                                if (!GetCleanTrackedPalmPosition(
                                        aimWand,
                                        !isLeft,
                                        secondaryPalmWorld)) {
                                    spdlog::debug(
                                        "[GRAB] Two-point co-hold held this "
                                        "frame: clean support palm unavailable; "
                                        "continuing primary drive");
                                } else {
                                    const RE::NiPoint3 currentAnchorVector =
                                        secondaryAnchorWorld - primaryAnchorWorld;
                                    const RE::NiPoint3 trackedPalmVector =
                                        secondaryPalmWorld - primaryAnchorWorld;

                                    if (Utils::VectorLength(currentAnchorVector) >
                                            5.0f &&
                                        Utils::VectorLength(trackedPalmVector) >
                                            5.0f) {
                                        const RE::NiMatrix3 swing =
                                            MakeVectorAlignmentRotation(
                                                currentAnchorVector,
                                                trackedPalmVector);
                                        targetRot =
                                            targetRot * swing.Transpose();

                                        RE::NiTransform rotatedTarget =
                                            baseTarget;
                                        rotatedTarget.rotate = targetRot;
                                        const RE::NiPoint3 movedPrimaryAnchor =
                                            rock::transform_math::
                                                localPointToWorld(
                                                    rotatedTarget,
                                                    state.
                                                        coHoldPrimaryAnchorObjectLocal);
                                        targetPos +=
                                            primaryAnchorWorld -
                                            movedPrimaryAnchor;
                                    }
                                }
                            } else {
                                // Compatibility fallback for a marker captured
                                // by an older state: retain the former +Y aim.
                                const RE::NiPoint3 primaryFwd(
                                    parentRot.entry[1][0],
                                    parentRot.entry[1][1],
                                    parentRot.entry[1][2]);
                                const RE::NiPoint3 aimVec =
                                    aimWand->world.translate - parentPos;
                                if (Utils::VectorLength(aimVec) > 5.0f) {
                                    const RE::NiMatrix3 swing =
                                        MakeVectorAlignmentRotation(
                                            primaryFwd,
                                            aimVec);
                                    targetRot =
                                        targetRot * swing.Transpose();
                                }
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
                // Keep the rendered support hand welded to the exact secondary
                // mesh spot. Object motion is still solved exclusively from
                // clean wand/palm input above, so this visual publication can
                // never feed back into the two-point object solve.
                GrabState& supportState =
                    isLeft ? _rightGrab : _leftGrab;
                if (g_config.enableTwoHandedGrab &&
                    state.coHoldAnchorsValid &&
                    supportState.active &&
                    supportState.coHeldSecondary &&
                    supportState.GetRefr() == stateRefr) {
                    RE::NiTransform objectTarget{};
                    objectTarget.rotate = targetRot;
                    objectTarget.translate = targetPos;
                    objectTarget.scale =
                        state.node->world.scale > 0.0f
                            ? state.node->world.scale
                            : 1.0f;
                    const RE::NiTransform supportHandTarget =
                        rock::transform_math::composeTransforms(
                            objectTarget,
                            state.coHoldSecondaryHandObjectLocal);
                    HandAuthority::Apply(
                        OBJECT_COHOLD_HAND_TAG,
                        !isLeft,
                        supportHandTarget,
                        OBJECT_COHOLD_HAND_PRIORITY);
                }

                // =================================================================
                // VISUAL UPDATE - Update node and propagate to children (keyframed)
                // =================================================================
                // FINAL GUARD. targetRot is built from several transpose-as-inverse chains
                // upstream; if any of them drifted, the object is multiplied by M^T*M and
                // renders stretched or squashed. Normalising here catches all of them at the
                // single point the visual transform is committed. No-op when already clean.
                heisenberg::OrthoNormalize(targetRot);

                RE::NiTransform desiredTransform;
                desiredTransform.translate = targetPos;
                desiredTransform.rotate = targetRot;
                // WORLD, not local. This is a WORLD-space target, and UpdateKeyframedNode
                // divides by the parent's world scale to derive local. Feeding local back in
                // made local decay (or grow) by 1/parentScale every single frame whenever the
                // parent's world scale was not exactly 1. The co-hold targets at ~11914 and
                // ~12098 already read world.scale; this site was the outlier.
                desiredTransform.scale = state.node->world.scale > 0.0f ? state.node->world.scale : 1.0f;

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
                        bool heldWorldResynced = !heldWorldChanged;
                        if (heldWorldChanged) {
                            const void* previousCapturedWorld =
                                state.capturedHeldBodiesWorld;
                            const std::uint32_t previousCapturedCount =
                                state.capturedHeldBodyCount;
                            spdlog::info(
                                "[GRAB] {} hand: bhkWorld changed while holding "
                                "(cell/worldspace transition) - invalidating {} "
                                "captured body id(s) from {:p} and recapturing",
                                isLeft ? "Left" : "Right",
                                previousCapturedCount,
                                previousCapturedWorld);

                            // hknp body IDs are scoped to one exact world. Clear the
                            // old set before replacing collisionObject so no helper can
                            // accidentally interpret an old ID in the new body's buffer.
                            // The old world is being torn down; attempting to restore its
                            // filters here would dereference precisely the stale state we
                            // are defending against.
                            ClearExternalHeldBodiesForPlayerSuppression(
                                state,
                                isLeft);
                            ClearCapturedHeldBodyFrames(state);
                            state.heldPlayerFilterApplied = false;
                            state.heldPlayerFilterFrames = 0;
                            state.heldOriginalFilterInfo = 0;
                            state.heldScalarFilterApplied = false;
                            state.savedState.collisionLayerChanged = false;

                            // Preserve the wrapper that won acquisition when its
                            // visual owner survived the transition. Falling back
                            // straight to scene traversal can select a hidden
                            // alternate wrapper and invert active/phantom roles.
                            state.collisionObject = state.physicsNode
                                ? TryResolveNpcCollisionObjectFromRaw(
                                      state.physicsNode->collisionObject.get())
                                : nullptr;
                            if (!state.collisionObject) {
                                state.collisionObject =
                                    GetCollisionObject(stateRefr);
                            }
                            void* expectedHeldWorldRaw =
                                heisenberg::Physics::GetHknpWorldFromBhk(
                                    currentHeldWorld);
                            void* actualHeldWorldRaw = nullptr;
                            if (state.collisionObject &&
                                state.collisionObject->spSystem &&
                                state.node) {
                                actualHeldWorldRaw =
                                    AccessWorld(state.collisionObject);
                                if (void* heldWorldRaw = actualHeldWorldRaw;
                                    heldWorldRaw &&
                                    heldWorldRaw == expectedHeldWorldRaw) {
                                    // Capture in the new identity namespace before
                                    // changing motion/filter state. If multipart
                                    // enumeration fails, the selected-body fallback
                                    // below remains authoritative.
                                    CaptureHeldCollisionBodyFrames(state);
                                    if (!PublishExternalHeldBodiesForPlayerSuppression(
                                            state,
                                            isLeft)) {
                                        spdlog::warn(
                                            "[GRAB-FILTER] Could not publish "
                                            "new-world held-body identity "
                                            "before KEYFRAMED authority for "
                                            "{:08X}",
                                            stateRefr->formID);
                                    }
                                    const bool selectedWrapperScope =
                                        RequiresSelectedCollisionWrapperMotionScope(
                                            state);
                                    state.heldMotionScopeIsReferenceSubtree =
                                        !selectedWrapperScope;
                                    if (selectedWrapperScope) {
                                        SetMotionTypeLocked(
                                            state.collisionObject,
                                            RE::hknpMotionPropertiesId::Preset::KEYFRAMED,
                                            currentHeldWorld);
                                    } else {
                                        bhkWorld_SetMotionLocked(
                                            state.node.get(),
                                            RE::hknpMotionPropertiesId::Preset::KEYFRAMED,
                                            true,
                                            true,
                                            true,
                                            currentHeldWorld);
                                    }

                                    if (heisenberg::g_config.heldObjectCollidable) {
                                        state.heldPlayerFilterApplied =
                                            TryDisablePlayerHeldObjectCollision(
                                                state,
                                                currentHeldWorld);
                                    } else {
                                        if (state.capturedHeldBodySetValid) {
                                            if (ApplyCapturedHeldBodyFilters(
                                                    state,
                                                    currentHeldWorld,
                                                    false)) {
                                                state.heldPlayerFilterApplied =
                                                    true;
                                            } else {
                                                spdlog::warn(
                                                    "[GRAB-MULTIBODY] "
                                                    "Non-collidable filter "
                                                    "apply failed after world "
                                                    "recapture; native "
                                                    "filters retained");
                                            }
                                        } else {
                                            CaptureHeldObjectLayerBeforeChange(
                                                state);
                                            bhkUtilFunctions_SetLayerLocked(
                                                state.node.get(),
                                                15,
                                                currentHeldWorld);
                                        }
                                    }

                                    bool haveLiveBody =
                                        state.capturedHeldBodySetValid;
                                    if (haveLiveBody) {
                                        RebuildCapturedHeldBodyCollisionCaches(
                                            state,
                                            currentHeldWorld,
                                            0.25f);
                                    } else {
                                        // Preserve the selected-body path when
                                        // multipart capture cannot enumerate this
                                        // NIF. This is also the readiness check used
                                        // before committing the new world identity.
                                        heisenberg::Physics::WorldWriteLock
                                            selectedBodyLock(
                                                currentHeldWorld);
                                        if (selectedBodyLock.IsLocked() &&
                                            heisenberg::Physics::
                                                    GetHknpWorldFromBhk(
                                                        currentHeldWorld) ==
                                                heldWorldRaw &&
                                            AccessWorld(
                                                state.collisionObject) ==
                                                heldWorldRaw) {
                                            std::uint32_t heldBodyId =
                                                0x7FFFFFFF;
                                            heisenberg::ConstraintFunctions::
                                                BhkPhysicsSystemGetBodyId(
                                                    state.collisionObject
                                                        ->spSystem.get(),
                                                    &heldBodyId,
                                                    state.collisionObject
                                                        ->systemBodyIdx);
                                            if (heldBodyId != 0x7FFFFFFF &&
                                                heldBodyId != 0xFFFFFFFF) {
                                                RebuildBodyCollisionCachesNative(
                                                    heldWorldRaw,
                                                    heldBodyId);
                                                heisenberg::Physics::
                                                    TrySetBodyCollisionLookAhead(
                                                        heldWorldRaw,
                                                        heldBodyId,
                                                        0.25f);
                                                haveLiveBody = true;
                                            }
                                        }
                                    }

                                    if (haveLiveBody) {
                                        state.savedState.savedBhkWorld =
                                            currentHeldWorld;
                                        heldWorldResynced = true;
                                        spdlog::info(
                                            "[GRAB] {} hand: recaptured {} "
                                            "held body frame(s) in hknpWorld {:p} "
                                            "(fallback={})",
                                            isLeft ? "Left" : "Right",
                                            state.capturedHeldBodyCount,
                                            heldWorldRaw,
                                            !state.capturedHeldBodySetValid
                                                ? "selected-body"
                                                : "multipart");
                                    }
                                }
                            }

                            if (!heldWorldResynced) {
                                // Keep lastSyncedBhkWorld on the old identity so
                                // this block retries next frame; collision wrappers
                                // can appear a few frames after the player enters the
                                // new cell. The captured set remains empty, which
                                // keeps all per-frame paths on their safe fallback.
                                ClearCapturedHeldBodyFrames(state);
                                static std::uint32_t
                                    s_worldRecaptureFailureLog = 0;
                                if ((++s_worldRecaptureFailureLog % 60) == 1) {
                                    spdlog::warn(
                                        "[GRAB] {} hand: held collision is not "
                                        "ready in new bhkWorld {:p}; will retry "
                                        "(collisionObject={:p} actualHknp={:p} "
                                        "expectedHknp={:p})",
                                        isLeft ? "Left" : "Right",
                                        static_cast<void*>(currentHeldWorld),
                                        static_cast<void*>(state.collisionObject),
                                        actualHeldWorldRaw,
                                        expectedHeldWorldRaw);
                                }
                            }
                        }
                        if (heldWorldResynced) {
                            state.lastSyncedBhkWorld = currentHeldWorld;
                        }
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
                if (state.capturedHeldBodySetValid &&
                    !state.isPulling) {
                    RE::bhkWorld* heldBodyWorld =
                        ResolveMatchingHeldBhkWorld(state);
                    if (!SyncCapturedHeldBodyFrames(
                            state,
                            desiredTransform,
                            heldBodyWorld)) {
                        // The captured IDs no longer describe a body set we can
                        // safely drive. Drop the exact controller-suppression
                        // snapshot immediately; the cadence refresh will
                        // republish only after ownership validates again.
                        ClearExternalHeldBodiesForPlayerSuppression(
                            state,
                            isLeft);
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
                // A skeleton/Power-Armor rebuild clears FRIK's authority
                // registry. The grab state survives that rebuild, so detect a
                // vanished ROCK_Grab writer and solve once against the current
                // final mesh to rebuild skeleton-specific joint/splay/local
                // corrections. If the rich solve cannot recover, the stored
                // joint curls below remain a safe scalar fallback.
                if (!state.isPulling &&
                    state.rockRichFingerPosePublished &&
                    !rock::touch_grab_bridge::
                        IsTouchGrabFingerPoseActive(isLeft)) {
                    state.rockRichFingerPosePublished = false;
                    if (!wandNode) {
                        wandNode = heisenberg::GetWandNode(
                            playerNodes,
                            isLeft);
                    }
                    if (wandNode &&
                        TryCalculateRuntimeFingerCurlsFromGeometry(
                            state,
                            wandNode,
                            isLeft)) {
                        spdlog::info(
                            "[GRAB-FINGERS] {} rich mesh pose republished "
                            "after FRIK skeleton/PA authority reset",
                            isLeft ? "Left" : "Right");
                    } else {
                        spdlog::warn(
                            "[GRAB-FINGERS] {} rich mesh pose could not be "
                            "republished; retaining scalar joint fallback",
                            isLeft ? "Left" : "Right");
                    }
                }
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
            
        // Check genuinely blocking menus, but continue if we have an active grab.
        // Favorites and Pip-Boy are live-world overlays and are not part of this gate.
        // Use cached menu state from MenuChecker for thread safety
        auto& menuChecker = MenuChecker::GetSingleton();
        bool menuBlocking = menuChecker.IsPaused() || menuChecker.IsLoading() ||
                            menuChecker.IsMainMenu();
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

// Two-handed support-hand finger pose driver (Jul 19):
//   mode 1: ROCK owns its complete mesh-solved support pose. Native FRIK v5
//           consumes it directly; Heisenberg renders the same rich payload on
//           stock FRIK 0.77.12 after FRIK's skeleton pass.
//   mode 2: Heisenberg's HIGGS-table geometry solver against the whole weapon mesh,
//           solved once at grip capture (retried while the solve fails, max 30 frames).
//           The old five-scalar v3 call remains only as a fail-open fallback.
// Cleared the frame the grip disengages.
void heisenberg::UpdateTwoHandedSupportFingerPose()
{
        const int mode = g_config.twoHandedFingerPoseMode;
        static bool s_active[2] = { false, false };
        // Scope-mode exit/re-entry gating is tracked independent of the
        // finger-pose mode so it works even with
        // iTwoHandedFingerPoseMode=0. Track GRIPPED rather than the broader
        // engaged state: ROCK deliberately remains Touching after release,
        // which would delay the falling edge indefinitely. Acquiring support
        // while already scoped must preserve ScopeMenu; only releasing an
        // established support grip closes it and arms leave/re-enter gating.
        static bool s_prevAnySupportGripped = false;
        static bool s_solved[2] = { false, false };
        static int s_attempts[2] = { 0, 0 };
        static int s_retryCooldown[2] = { 0, 0 };
        static float s_curls[2][5] = {};
        static bool s_richMode2Active[2] = { false, false };
        static constexpr const char* kMode2PoseTag =
            "Heisenberg_TwoHandMode2";

        const bool rockHosted = IsRockEngineHosted();
        const bool anySupportGripped =
            rockHosted &&
            (rock::HostIsWeaponSupportGripped(false) ||
                rock::HostIsWeaponSupportGripped(true));
        if (rockHosted &&
            rock::native_scope_reentry_policy::
                shouldExitForSupportGripTransition(
                    s_prevAnySupportGripped,
                    anySupportGripped)) {
            ExitScopeModeOnGripRelease();
        }
        s_prevAnySupportGripped = anySupportGripped;

        auto& frik = FRIKInterface::GetSingleton();
        if (!frik.IsAvailable()) {
            return;
        }

        for (int hi = 0; hi < 2; ++hi) {
            const bool isLeft = (hi == 1);
            const bool supportGripped =
                rockHosted &&
                rock::HostIsWeaponSupportGripped(isLeft);

            const bool supportEngaged =
                rockHosted &&
                rock::HostIsWeaponSupportEngaged(isLeft);
            const bool engaged = mode > 0 && supportEngaged;

            // The old five-scalar driver existed only because stock FRIK had
            // no way to consume ROCK's complete pose. Once either native v5
            // or Heisenberg's v3 full-finger host backend is present, another
            // scalar write here would overwrite the fail-open baseline after
            // ROCK's duplicate-publish cache has intentionally gone quiet.
            // Keep the scope-release edge above. Mode 1 then leaves pose
            // ownership to the rich ROCK -> FRIK/host bridge; mode 2 remains
            // the user's explicit Heisenberg-solver choice below.
            const auto* const hostFingerAuthority =
                rock::getHostFingerPoseAuthority();
            const bool completeHostFingerAuthority =
                hostFingerAuthority &&
                hostFingerAuthority->applyPose &&
                hostFingerAuthority->buildPoseLocalTransforms &&
                hostFingerAuthority->applyLocalTransforms &&
                hostFingerAuthority->clear &&
                hostFingerAuthority->isActive;
            const bool rockOwnsFullFingerPose =
                IsRockEngineHosted() &&
                (rock::frikHasVisualAuthority() ||
                    completeHostFingerAuthority);
            // Mode 2 is an explicit request for Heisenberg's separate
            // whole-weapon geometry solver. Preserve that user selection;
            // only retire the old scalar adapter used by mode 1.
            if (rockOwnsFullFingerPose && mode != 2) {
                if (s_richMode2Active[hi]) {
                    (void)rock::HostClearFingerPose(
                        kMode2PoseTag,
                        isLeft);
                } else if (s_active[hi]) {
                    frik.ClearHandPoseFingerPositions(isLeft);
                }
                s_richMode2Active[hi] = false;
                s_active[hi] = false;
                s_solved[hi] = false;
                s_attempts[hi] = 0;
                s_retryCooldown[hi] = 0;
                continue;
            }

            if (!engaged) {
                if (s_richMode2Active[hi]) {
                    (void)rock::HostClearFingerPose(
                        kMode2PoseTag,
                        isLeft);
                } else if (s_active[hi]) {
                    frik.ClearHandPoseFingerPositions(isLeft);
                    spdlog::debug("[THG-FINGER] {} support grip released - finger pose cleared", isLeft ? "L" : "R");
                }
                s_richMode2Active[hi] = false;
                s_active[hi] = false;
                s_solved[hi] = false;
                s_attempts[hi] = 0;
                s_retryCooldown[hi] = 0;
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

            // Mode 2: solve against the whole weapon mesh at grip capture.
            // A temporarily unavailable skeleton used to trigger this full
            // traversal on 30 consecutive frames. Space retries out so a
            // transient readiness miss cannot create a sustained frame-time
            // spike; a normal first-attempt success is unchanged.
            if (!s_solved[hi] &&
                s_attempts[hi] < 30 &&
                s_retryCooldown[hi] == 0) {
                ++s_attempts[hi];
                s_retryCooldown[hi] = 4;
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
            } else if (!s_solved[hi] &&
                       s_retryCooldown[hi] > 0) {
                --s_retryCooldown[hi];
            }
            if (s_solved[hi]) {
                bool published = false;
                if (rockOwnsFullFingerPose) {
                    published = rock::HostPublishUniformFingerPose(
                        kMode2PoseTag,
                        isLeft,
                        s_curls[hi][0],
                        s_curls[hi][1],
                        s_curls[hi][2],
                        s_curls[hi][3],
                        s_curls[hi][4],
                        110);
                    s_richMode2Active[hi] = published;
                }
                if (!published) {
                    published = frik.SetHandPoseFingerPositions(
                        isLeft,
                        s_curls[hi][0],
                        s_curls[hi][1],
                        s_curls[hi][2],
                        s_curls[hi][3],
                        s_curls[hi][4]);
                    s_richMode2Active[hi] = false;
                }
                s_active[hi] = published;
            }
        }
}
