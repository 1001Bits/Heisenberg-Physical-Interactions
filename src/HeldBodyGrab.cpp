#include "HeldBodyGrab.h"
#include "Config.h"
#include "ContactImpulseListener.h"
#include "Grab.h"
#include "Physics.h"
#include "PlayerCharacterProxyListener.h"
#include "ThrownObjectTracker.h"
#include "Utils.h"
#include "VRInput.h"
#include "f4vr/PlayerNodes.h"
#include "f4vr/F4VRUtils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr float PI = 3.14159265358979323846f;

    static bool SafeSetBodyVelocity(void* hknpWorld, std::uint32_t bodyId,
                                     const RE::NiPoint4& linear, const RE::NiPoint4& angular);

    // Havok world scale - 1 game unit = 0.0142875 Havok units (1/70)
    constexpr float HAVOK_WORLD_SCALE = 0.0142875f;
    
    // Hand box dimensions in Havok units (matching HIGGS Skyrim)
    constexpr float HAND_HALF_X = 0.06f;  // ~4 game units
    constexpr float HAND_HALF_Y = 0.06f;
    constexpr float HAND_HALF_Z = 0.06f;
    
    // Collision filter for clutter objects (layer 4)
    constexpr std::uint32_t CLUTTER_FILTER = 0x02EF0004;
    // Collision filter for non-collidable layer 15 - hand body won't collide with player
    // Layer is in lower 7 bits: 0x0F = layer 15 (kNonCollidable)
    constexpr std::uint32_t NONCOLLIDABLE_FILTER = 0x02EF000F;
    constexpr std::uint32_t COLLISION_FLAG_DISABLED = (1 << 14);
    
    // Motion property IDs
    constexpr std::uint8_t MOTION_KEYFRAMED = 2;
    constexpr std::uint8_t MOTION_DYNAMIC = 1;

    // ROCK rockGrabMaxInertiaRatio default — clamp held-object inverse-inertia spread.
    constexpr float kHeldBodyMaxInertiaRatio = 10.0f;
    // ROCK rockGrabVelocityDamping default (0.25) — each frame the held body keeps
    // (1-damping) of its velocity, bleeding off the orbital/chase velocity so it settles
    // to the target instead of "swirling on a rope".
    constexpr float kHeldBodyVelocityDamping = 0.25f;
    // Max release/throw speed (game units/s ~= ROCK maxVelocityHavok 12 m/s) — anti-fling.
    constexpr float kHeldBodyMaxThrowSpeed = 840.0f;

    // Forward declaration (defined later in this file)
    static bool SafeSetBodyVelocity(void* hknpWorld, std::uint32_t bodyId,
                                     const RE::NiPoint4& linear, const RE::NiPoint4& angular);

    struct ConstraintTransformsPrefix
    {
        void* vtable;
        std::uint16_t memSizeAndFlags;
        std::uint16_t referenceCount;
        std::uint32_t pad0C;
        std::uint64_t userData;
        std::uint32_t constraintType;
        std::uint8_t pad1C[4];
        heisenberg::hkpSetLocalTransformsConstraintAtom transforms;
    };
    static_assert(offsetof(ConstraintTransformsPrefix, transforms) == 0x20);

    RE::NiPoint3 ScaleLocalPoint(const RE::NiPoint3& point, float scale)
    {
        return RE::NiPoint3(point.x * scale, point.y * scale, point.z * scale);
    }

    RE::NiPoint3 ComputeHeldBodyPredictedPlayerMovement(heisenberg::GrabState& state)
    {
        RE::NiPoint3 predictedPlayerMovement(0.0f, 0.0f, 0.0f);
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->roomnode) {
            state.roomTrackingInitialized = false;
            state.smoothedRoomDelta = RE::NiPoint3(0.0f, 0.0f, 0.0f);
            return predictedPlayerMovement;
        }

        RE::NiPoint3 currentRoomPos = playerNodes->roomnode->world.translate;
        if (state.roomTrackingInitialized) {
            RE::NiPoint3 roomDelta = currentRoomPos - state.lastRoomPos;

            constexpr float maxDelta = 30.0f;
            if (roomDelta.Length() < maxDelta) {
                constexpr float smoothingFactor = 0.3f;
                state.smoothedRoomDelta = state.smoothedRoomDelta * (1.0f - smoothingFactor) + roomDelta * smoothingFactor;
                predictedPlayerMovement = state.smoothedRoomDelta;
            } else {
                state.smoothedRoomDelta = RE::NiPoint3(0.0f, 0.0f, 0.0f);
            }
        }

        state.lastRoomPos = currentRoomPos;
        state.roomTrackingInitialized = true;
        return predictedPlayerMovement;
    }

    bool GetFRIKWeaponParentTransform(bool isLeft, RE::NiPoint3& outPos, RE::NiMatrix3& outRot)
    {
        auto* player = f4cf::f4vr::getPlayer();
        if (!player || !player->firstPersonSkeleton) {
            return false;
        }

        const char* handNodeName = isLeft ? "LArm_Hand" : "RArm_Hand";
        RE::NiAVObject* handNodeObj = heisenberg::Utils::FindNode(player->firstPersonSkeleton, handNodeName, 15);
        RE::NiNode* handNode = handNodeObj ? handNodeObj->IsNode() : nullptr;
        if (handNode) {
            outPos = handNode->world.translate;
            outRot = handNode->world.rotate;
            return true;
        }

        RE::NiAVObject* weaponObj = heisenberg::Utils::FindNode(player->firstPersonSkeleton, "Weapon", 15);
        RE::NiNode* weaponNode = weaponObj ? weaponObj->IsNode() : nullptr;
        if (!weaponNode || !weaponNode->parent) {
            return false;
        }

        bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
        bool isPrimaryHand = (isLeftHandedMode ? isLeft : !isLeft);
        if (!isPrimaryHand) {
            return false;
        }

        outPos = weaponNode->parent->world.translate;
        outRot = weaponNode->parent->world.rotate;
        return true;
    }

    // Find the COM-tree skinned hand (same node used by Grab.cpp's
    // GetSkinnedHandNode). This is the bone with the real finger children,
    // not the firstPersonSkeleton weapon-attach wrapper.
    static RE::NiNode* FindSkinnedHandForHeldBody(bool isLeft)
    {
        const char* handName = isLeft ? "LArm_Hand" : "RArm_Hand";
        if (auto* commonNode = f4cf::f4vr::getCommonNode()) {
            if (auto* handObj = heisenberg::Utils::FindNode(commonNode, handName, 20)) {
                if (auto* asNode = handObj->IsNode()) return asNode;
            }
        }
        return nullptr;
    }

    void ResolveHeldBodyHandReference(const heisenberg::GrabState& state,
                                      bool isLeft,
                                      const RE::NiPoint3& handPos,
                                      const RE::NiMatrix3& handRot,
                                      RE::NiPoint3& outPos,
                                      RE::NiMatrix3& outRot)
    {
        outPos = handPos;
        outRot = handRot;

        // Runtime placement computed in the skinned-hand frame takes priority
        // — fingers live in that frame, so object + fingers share a parent.
        if (state.runtimePlacementSkinnedHand) {
            if (RE::NiNode* skinned = FindSkinnedHandForHeldBody(isLeft)) {
                outPos = skinned->world.translate;
                outRot = skinned->world.rotate;
                return;
            }
        }

        if (!state.isFRIKOffset) {
            return;
        }

        RE::NiPoint3 frikParentPos;
        RE::NiMatrix3 frikParentRot;
        if (GetFRIKWeaponParentTransform(isLeft, frikParentPos, frikParentRot)) {
            outPos = frikParentPos;
            outRot = frikParentRot;
        }
    }

    void ResolveHeldBodyHandDriveTransform(heisenberg::GrabState& state,
                                           bool isLeft,
                                           const RE::NiPoint3& handPos,
                                           const RE::NiMatrix3& handRot,
                                           RE::NiPoint3& outPos,
                                           RE::NiMatrix3& outRot)
    {
        ResolveHeldBodyHandReference(state, isLeft, handPos, handRot, outPos, outRot);
        outPos = outPos + ComputeHeldBodyPredictedPlayerMovement(state);
    }

    bool ComputeHeldBodyTargetTransformsFromParent(const heisenberg::GrabState& state,
                                                   const RE::NiPoint3& parentPos,
                                                   const RE::NiMatrix3& parentRot,
                                                   RE::NiTransform& desiredVisualTransform,
                                                   RE::NiTransform& desiredPhysicsTransform,
                                                   RE::NiAVObject*& syncNode)
    {
        if (!state.node) {
            return false;
        }

        RE::NiPoint3 localOffset;
        RE::NiMatrix3 localRotation;
        heisenberg::GetEffectiveGrabPlacement(state, localOffset, localRotation);
        if (heisenberg::Utils::IsPlayerInPowerArmor()) {
            const auto& config = heisenberg::Config::GetSingleton();
            localOffset.x += config.paGrabOffsetX;
            localOffset.y += config.paGrabOffsetY;
            localOffset.z += config.paGrabOffsetZ;
        }

        desiredVisualTransform.rotate = localRotation * parentRot;
        desiredVisualTransform.translate = parentPos + (parentRot.Transpose() * localOffset);
        desiredVisualTransform.scale = state.node->local.scale > 0.0f ? state.node->local.scale : 1.0f;

        syncNode = state.physicsNode ? state.physicsNode.get() : state.node.get();
        desiredPhysicsTransform = desiredVisualTransform;

        if (syncNode && syncNode != state.node.get()) {
            desiredPhysicsTransform.scale = syncNode->local.scale > 0.0f ? syncNode->local.scale : desiredVisualTransform.scale;

            if (state.collisionObject && heisenberg::Utils::HasPhysicsBodyOffset(state.collisionObject, state.node.get())) {
                RE::NiTransform bodyOffset = heisenberg::Utils::GetPhysicsBodyOffset(state.collisionObject, state.node.get());
                desiredPhysicsTransform = heisenberg::Utils::ApplyPhysicsBodyOffset(desiredVisualTransform, bodyOffset);
            }
        }

        return true;
    }

    RE::NiPoint3 WorldPointToLocalPoint(const RE::NiTransform& transform, const RE::NiPoint3& worldPoint)
    {
        float invScale = (transform.scale != 0.0f) ? (1.0f / transform.scale) : 1.0f;
        // Havok body-local space uses forward rotation for pivot computation
        return (transform.rotate * (worldPoint - transform.translate)) * invScale;
    }

    void UpdateConstraintTransformB(void* constraintData, const RE::NiMatrix3& rotation, const RE::NiPoint3& pivotB)
    {
        if (!constraintData) {
            return;
        }

        auto* prefix = reinterpret_cast<ConstraintTransformsPrefix*>(constraintData);
        float* transformB = reinterpret_cast<float*>(prefix->transforms.transformB);
        transformB[0] = rotation.entry[0][0];
        transformB[1] = rotation.entry[1][0];
        transformB[2] = rotation.entry[2][0];
        transformB[3] = 0.0f;
        transformB[4] = rotation.entry[0][1];
        transformB[5] = rotation.entry[1][1];
        transformB[6] = rotation.entry[2][1];
        transformB[7] = 0.0f;
        transformB[8] = rotation.entry[0][2];
        transformB[9] = rotation.entry[1][2];
        transformB[10] = rotation.entry[2][2];
        transformB[11] = 0.0f;
        transformB[12] = pivotB.x * HAVOK_WORLD_SCALE;
        transformB[13] = pivotB.y * HAVOK_WORLD_SCALE;
        transformB[14] = pivotB.z * HAVOK_WORLD_SCALE;
        transformB[15] = 0.0f;
    }

    bool ComputeHeldBodyTargetTransforms(const heisenberg::GrabState& state,
                                         bool isLeft,
                                         const RE::NiPoint3& handPos,
                                         const RE::NiMatrix3& handRot,
                                         RE::NiTransform& desiredVisualTransform,
                                         RE::NiTransform& desiredPhysicsTransform,
                                         RE::NiAVObject*& syncNode)
    {
        if (!state.node) {
            return false;
        }

        RE::NiPoint3 parentPos;
        RE::NiMatrix3 parentRot;
        ResolveHeldBodyHandReference(state, isLeft, handPos, handRot, parentPos, parentRot);

        return ComputeHeldBodyTargetTransformsFromParent(state, parentPos, parentRot,
                                                         desiredVisualTransform, desiredPhysicsTransform, syncNode);
    }

    bool ComputeNativeRagdollConstraintTargetFromParent(const heisenberg::GrabState& state,
                                                        const RE::NiPoint3& parentPos,
                                                        const RE::NiMatrix3& parentRot,
                                              RE::NiTransform& desiredPhysicsTransform,
                                              RE::NiAVObject*& syncNode,
                                              RE::NiMatrix3& frameB,
                                              RE::NiPoint3& pivotB)
    {
        RE::NiTransform desiredVisualTransform;
        if (!ComputeHeldBodyTargetTransformsFromParent(state, parentPos, parentRot,
                                                       desiredVisualTransform, desiredPhysicsTransform, syncNode)) {
            return false;
        }

        frameB = parentRot * desiredPhysicsTransform.rotate.Transpose();
        pivotB = WorldPointToLocalPoint(desiredPhysicsTransform, parentPos);
        return true;
    }

    RE::NiPoint4 MatrixToQuaternion(const RE::NiMatrix3& rotation)
    {
        RE::NiPoint4 orientation;
        float trace = rotation.entry[0][0] + rotation.entry[1][1] + rotation.entry[2][2];
        if (trace > 0.0f) {
            float scale = 0.5f / sqrtf(trace + 1.0f);
            orientation.w = 0.25f / scale;
            orientation.x = (rotation.entry[2][1] - rotation.entry[1][2]) * scale;
            orientation.y = (rotation.entry[0][2] - rotation.entry[2][0]) * scale;
            orientation.z = (rotation.entry[1][0] - rotation.entry[0][1]) * scale;
        } else if (rotation.entry[0][0] > rotation.entry[1][1] && rotation.entry[0][0] > rotation.entry[2][2]) {
            float scale = 2.0f * sqrtf(1.0f + rotation.entry[0][0] - rotation.entry[1][1] - rotation.entry[2][2]);
            orientation.w = (rotation.entry[2][1] - rotation.entry[1][2]) / scale;
            orientation.x = 0.25f * scale;
            orientation.y = (rotation.entry[0][1] + rotation.entry[1][0]) / scale;
            orientation.z = (rotation.entry[0][2] + rotation.entry[2][0]) / scale;
        } else if (rotation.entry[1][1] > rotation.entry[2][2]) {
            float scale = 2.0f * sqrtf(1.0f + rotation.entry[1][1] - rotation.entry[0][0] - rotation.entry[2][2]);
            orientation.w = (rotation.entry[0][2] - rotation.entry[2][0]) / scale;
            orientation.x = (rotation.entry[0][1] + rotation.entry[1][0]) / scale;
            orientation.y = 0.25f * scale;
            orientation.z = (rotation.entry[1][2] + rotation.entry[2][1]) / scale;
        } else {
            float scale = 2.0f * sqrtf(1.0f + rotation.entry[2][2] - rotation.entry[0][0] - rotation.entry[1][1]);
            orientation.w = (rotation.entry[1][0] - rotation.entry[0][1]) / scale;
            orientation.x = (rotation.entry[0][2] + rotation.entry[2][0]) / scale;
            orientation.y = (rotation.entry[1][2] + rotation.entry[2][1]) / scale;
            orientation.z = 0.25f * scale;
        }
        return orientation;
    }

    RE::NiPoint3 ClampVectorMagnitude(const RE::NiPoint3& vector, float maxMagnitude)
    {
        float magnitude = sqrtf(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
        if (magnitude <= maxMagnitude || magnitude <= 0.0001f) {
            return vector;
        }

        float scale = maxMagnitude / magnitude;
        return RE::NiPoint3(vector.x * scale, vector.y * scale, vector.z * scale);
    }

    heisenberg::HeldBodyGrabConstraint* FindHeldBodyConstraintForState(
        const heisenberg::GrabState& state,
        heisenberg::HeldBodyGrabConstraint& leftConstraint,
        heisenberg::HeldBodyGrabConstraint& rightConstraint)
    {
        auto matchesState = [&](heisenberg::HeldBodyGrabConstraint& constraint) {
            if (!constraint.IsValid()) {
                return false;
            }
            if (state.constraintId != 0x7FFFFFFF && constraint.constraintId.m_value == state.constraintId) {
                return true;
            }
            return state.handBodyId != 0x7FFFFFFF && constraint.handBodyId == state.handBodyId;
        };

        if (matchesState(leftConstraint)) {
            return &leftConstraint;
        }
        if (matchesState(rightConstraint)) {
            return &rightConstraint;
        }
        return nullptr;
    }

    static float GetMaxForceForFPS(float fps, float fps72, float fps90, float fps120, float fps144)
    {
        struct FPSPoint { float fps; float mult; };
        FPSPoint points[4] = { {72, fps72}, {90, fps90}, {120, fps120}, {144, fps144} };

        if (fps <= points[0].fps) return points[0].mult;
        if (fps >= points[3].fps) return points[3].mult;

        for (int i = 0; i < 3; i++) {
            if (fps <= points[i + 1].fps) {
                float t = (fps - points[i].fps) / (points[i + 1].fps - points[i].fps);
                return points[i].mult + t * (points[i + 1].mult - points[i].mult);
            }
        }
        return points[3].mult;
    }

    bool UpdateHeldBodySpringDrive(heisenberg::GrabState& state,
                                   const heisenberg::HeldBodyGrabConstraint& constraint,
                                   bool isLeft,
                                   const RE::NiPoint3& handParentPos,
                                   const RE::NiMatrix3& handParentRot,
                                   float deltaTime,
                                   float& outDistance)
    {
        if (!state.node || constraint.objectBodyId == 0x7FFFFFFF || !constraint.hknpWorld) {
            return false;
        }

        RE::NiTransform desiredVisualTransform;
        RE::NiTransform desiredPhysicsTransform;
        RE::NiAVObject* syncNode = nullptr;
        if (!ComputeHeldBodyTargetTransformsFromParent(state, handParentPos, handParentRot,
                                                       desiredVisualTransform, desiredPhysicsTransform, syncNode)) {
            return false;
        }

        RE::NiAVObject* currentNode = syncNode ? syncNode : state.node.get();
        if (!currentNode) {
            return false;
        }

        RE::NiPoint3 currentPosition = currentNode->world.translate;
        RE::NiMatrix3 currentRotation = currentNode->world.rotate;
        outDistance = (desiredPhysicsTransform.translate - currentPosition).Length();

        float safeDeltaTime = deltaTime > 0.0001f ? deltaTime : (1.0f / 90.0f);
        float physicsFPS = 1.0f / safeDeltaTime;
        float linearFPSMult = GetMaxForceForFPS(physicsFPS, 0.7f, 1.0f, 1.6f, 2.0f);
        float angularFPSMult = GetMaxForceForFPS(physicsFPS, 0.5f, 1.0f, 1.375f, 1.5f);

        RE::NiPoint3 currentLinearVelocity(0.0f, 0.0f, 0.0f);
        RE::NiPoint3 currentHandVelocity(0.0f, 0.0f, 0.0f);
        if (state.velocityTrackingInit) {
            float invDeltaTime = 1.0f / safeDeltaTime;
            currentLinearVelocity = (currentPosition - state.prevObjectPos) * invDeltaTime;
            currentHandVelocity = (handParentPos - state.prevWandPos) * invDeltaTime;
        }

        const auto& config = heisenberg::Config::GetSingleton();
        RE::NiPoint3 positionError = desiredPhysicsTransform.translate - currentPosition;
        // Stable velocity-follow controller.
        //   desiredVel = handVelocity (feed-forward, so the object MOVES WITH the hand)
        //              + positionError * gain (proportional pull to correct drift)
        // The previous law subtracted currentLinearVelocity * 1.6 — but since we SET the
        // body velocity every frame, the measured velocity ≈ last command, so subtracting
        // it with gain > 1 is positive feedback that overshoots and ejects the object
        // ("flies everywhere"). That term is removed. Hand velocity is now fed forward at
        // full weight (was 0.15) so free-space motion tracks the hand instead of lagging.
        (void)config;
        (void)currentLinearVelocity;
        float linearGain = (std::max)(1.5f, state.currentLinearTau * 12.0f * linearFPSMult);
        RE::NiPoint3 desiredLinearVelocity = currentHandVelocity + (positionError * linearGain);
        constexpr float kBaseMaxLinearVelocity = 1200.0f;
        const float maxLinearVelocity = kBaseMaxLinearVelocity * linearFPSMult;
        float desiredLinearVelocityMagnitude = std::sqrt(
            desiredLinearVelocity.x * desiredLinearVelocity.x +
            desiredLinearVelocity.y * desiredLinearVelocity.y +
            desiredLinearVelocity.z * desiredLinearVelocity.z);
        bool linearVelocityClamped = desiredLinearVelocityMagnitude > maxLinearVelocity;
        desiredLinearVelocity = ClampVectorMagnitude(desiredLinearVelocity, maxLinearVelocity);

        RE::NiMatrix3 deltaRotation = desiredPhysicsTransform.rotate * currentRotation.Transpose();
        RE::NiPoint4 deltaQuaternion = MatrixToQuaternion(deltaRotation);
        if (deltaQuaternion.w < 0.0f) {
            deltaQuaternion.x = -deltaQuaternion.x;
            deltaQuaternion.y = -deltaQuaternion.y;
            deltaQuaternion.z = -deltaQuaternion.z;
            deltaQuaternion.w = -deltaQuaternion.w;
        }

        RE::NiPoint3 desiredAngularVelocity(0.0f, 0.0f, 0.0f);
        float sinHalfAngle = sqrtf(deltaQuaternion.x * deltaQuaternion.x +
                                   deltaQuaternion.y * deltaQuaternion.y +
                                   deltaQuaternion.z * deltaQuaternion.z);
        if (sinHalfAngle > 0.0001f) {
            float clampedW = (std::clamp)(deltaQuaternion.w, -1.0f, 1.0f);
            float angle = 2.0f * atan2f(sinHalfAngle, clampedW);
            if (angle > PI) {
                angle -= 2.0f * PI;
            }

            float invSinHalfAngle = 1.0f / sinHalfAngle;
            RE::NiPoint3 axis(deltaQuaternion.x * invSinHalfAngle,
                              deltaQuaternion.y * invSinHalfAngle,
                              deltaQuaternion.z * invSinHalfAngle);
            float angularGain = (std::max)(2.0f, state.currentAngularTau * 12.0f * angularFPSMult);
            desiredAngularVelocity = axis * (angle * angularGain);
            desiredAngularVelocity = ClampVectorMagnitude(desiredAngularVelocity, 18.0f * angularFPSMult);
        }

        RE::NiPoint4 hkLinearVelocity(
            desiredLinearVelocity.x * HAVOK_WORLD_SCALE,
            desiredLinearVelocity.y * HAVOK_WORLD_SCALE,
            desiredLinearVelocity.z * HAVOK_WORLD_SCALE,
            0.0f);
        RE::NiPoint4 hkAngularVelocity(
            desiredAngularVelocity.x,
            desiredAngularVelocity.y,
            desiredAngularVelocity.z,
            0.0f);

        if (!SafeSetBodyVelocity(constraint.hknpWorld, constraint.objectBodyId, hkLinearVelocity, hkAngularVelocity)) {
            spdlog::error("[HELDBODY] Spring drive failed for body 0x{:08X}", constraint.objectBodyId);
            return false;
        }

        if (outDistance > 40.0f || linearVelocityClamped) {
            static int s_springDiagnosticCounter = 0;
            ++s_springDiagnosticCounter;
            if ((s_springDiagnosticCounter % 30) == 1) {
                spdlog::warn(
                    "[HELDBODY] Spring tracking error: dist={:.1f}, clamped={}, target=({:.1f},{:.1f},{:.1f}), current=({:.1f},{:.1f},{:.1f}), desiredVel=({:.1f},{:.1f},{:.1f}), handVel=({:.1f},{:.1f},{:.1f})",
                    outDistance,
                    linearVelocityClamped ? 1 : 0,
                    desiredPhysicsTransform.translate.x,
                    desiredPhysicsTransform.translate.y,
                    desiredPhysicsTransform.translate.z,
                    currentPosition.x,
                    currentPosition.y,
                    currentPosition.z,
                    desiredLinearVelocity.x,
                    desiredLinearVelocity.y,
                    desiredLinearVelocity.z,
                    currentHandVelocity.x,
                    currentHandVelocity.y,
                    currentHandVelocity.z);
            }
        }

        state.lastTargetPos = desiredPhysicsTransform.translate;
        state.lastTargetRot = desiredPhysicsTransform.rotate;
        state.lastDeltaTime = safeDeltaTime;
        state.prevObjectPos = currentPosition;
        state.prevWandPos = handParentPos;
        state.objectVelocity = currentLinearVelocity;
        state.wandVelocity = currentHandVelocity;
        state.velocityError = desiredLinearVelocity - currentLinearVelocity;
        state.velocityTrackingInit = true;

        return true;
    }

    // SEH helper for CreateBody
    static volatile bool g_sehExceptionCaught = false;
    
    // SEH-safe wrappers (must be separate functions with no C++ objects on stack)
    static bool SafeHknpPhysicsSystemCtor(void* hknpSystemMem, void* systemDataMem, 
                                           void* hknpWorld, void* identityTransform,
                                           int additionMode, int additionFlags, int flags)
    {
        __try {
            heisenberg::ConstraintFunctions::PhysicsSystemCtor(
                hknpSystemMem, systemDataMem, hknpWorld,
                identityTransform, additionMode, static_cast<std::uint8_t>(additionFlags), flags);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // Flag to track SEH exception
    static volatile bool g_createBodySehException = false;
    
    // Function pointer for hknpWorld::createBody (moved outside SEH function)
    // CORRECT signature from Ghidra decompilation (FUN_141543ff0):
    //   int* hknpWorld_createBody(void* world, int* outBodyId, hknpBodyCinfo* cinfo, int additionMode, char flags)
    // param_2 is an OUTPUT parameter - the bodyId is written there!
    using hknpWorld_createBody_t = std::int32_t*(__fastcall*)(void* world, std::int32_t* outBodyId, void* bodyCinfo, int additionMode, unsigned char flags);
    static REL::Relocation<hknpWorld_createBody_t> g_hknpWorld_createBody{ REL::Offset(0x1543ff0) };
    
    // SEH-safe wrapper for hknpWorld::createBody (NO C++ objects allowed in this function!)
    // Returns bodyId on success, 0x7FFFFFFE on exception
    static std::uint32_t SafeCreateBody(void* hknpWorld, heisenberg::hknpBodyCinfo* cinfo)
    {
        g_createBodySehException = false;
        __try {
            // AdditionMode: 0=ADD_ACTIVE, 1=ADD_INACTIVE, 2=ADD_ASLEEP
            // flags: 0 = default
            std::int32_t outBodyId = 0x7FFFFFFF;  // Initialize to invalid
            g_hknpWorld_createBody(hknpWorld, &outBodyId, cinfo, 0, 0);
            return static_cast<std::uint32_t>(outBodyId);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            g_createBodySehException = true;
            return 0x7FFFFFFE;  // Different from 0x7FFFFFFF to indicate exception
        }
    }
    
    static bool SafeApplyHardKeyFrame(void* hknpWorld, std::uint32_t bodyId,
                                       const RE::NiPoint4& position,
                                       const RE::NiPoint4& orientation,
                                       float invDeltaTime)
    {
        __try {
            heisenberg::ConstraintFunctions::ApplyHardKeyFrameBodyId(
                hknpWorld, bodyId, position, orientation, invDeltaTime);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    static bool SafeDestroyBodies(void* hknpWorld, std::uint32_t* bodyIds, int count)
    {
        __try {
            heisenberg::ConstraintFunctions::DestroyBodies(hknpWorld, bodyIds, count, 0);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for hknpWorld::setBodyLinearVelocity
    static bool SafeSetBodyLinearVelocity(void* hknpWorld, std::uint32_t bodyId,
                                           const RE::NiPoint4& velocity)
    {
        __try {
            heisenberg::ConstraintFunctions::hknpWorld_setBodyLinearVelocity(
                hknpWorld, bodyId, velocity);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-safe wrapper for hknpWorld::setBodyVelocity (linear + angular)
    static bool SafeSetBodyVelocity(void* hknpWorld, std::uint32_t bodyId,
                                     const RE::NiPoint4& linear, const RE::NiPoint4& angular)
    {
        __try {
            heisenberg::ConstraintFunctions::hknpWorld_setBodyVelocity(
                hknpWorld, bodyId, linear, angular);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-safe wrapper for bhkNPCollisionObject::AddToWorld
    static bool SafeAddToWorld(void* collisionObjMem, void* bhkWorld)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkNPCollisionObjectAddToWorld(collisionObjMem, bhkWorld);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for bhkNPCollisionObject::SetMotionType
    static bool SafeSetMotionType(void* collisionObjMem, int motionType)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkNPCollisionObjectSetMotionType(collisionObjMem, motionType);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for hknpWorld::createConstraint
    static bool SafeCreateConstraint(void* hknpWorld, heisenberg::hknpConstraintId* outId, 
                                      const heisenberg::hknpConstraintCinfo* cinfo)
    {
        __try {
            heisenberg::ConstraintFunctions::CreateConstraint(hknpWorld, outId, cinfo);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for BallSocketCtor
    static bool SafeBallSocketCtor(heisenberg::hkpBallAndSocketConstraintData* data)
    {
        __try {
            heisenberg::ConstraintFunctions::BallSocketCtor(data);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for BallSocketSetInBodySpace
    static bool SafeBallSocketSetInBodySpace(heisenberg::hkpBallAndSocketConstraintData* data,
                                              const RE::NiPoint4& pivotA,
                                              const RE::NiPoint4& pivotB)
    {
        __try {
            heisenberg::ConstraintFunctions::BallSocketSetInBodySpace(data, pivotA, pivotB);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // Create a simple ball-socket constraint (for testing - no motors)
    // Returns allocated constraint data (caller owns it)
    heisenberg::hkpBallAndSocketConstraintData* CreateBallSocketConstraintDataLocal(
        const RE::NiPoint3& pivotA,  // In body A local space (hand)
        const RE::NiPoint3& pivotB)  // In body B local space (object)
    {
        using namespace heisenberg;
        
        // Allocate ALIGNED memory for the ball-socket constraint data
        // Havok structures require 16-byte alignment
        void* mem = _aligned_malloc(sizeof(hkpBallAndSocketConstraintData), 16);
        if (!mem) {
            spdlog::error("[HELDBODY] CreateBallSocketConstraintData: Failed to allocate memory");
            return nullptr;
        }
        
        std::memset(mem, 0, sizeof(hkpBallAndSocketConstraintData));
        auto* data = reinterpret_cast<hkpBallAndSocketConstraintData*>(mem);
        
        // Call the native constructor to properly initialize the structure
        spdlog::debug("[HELDBODY] Calling BallSocketCtor at {:p}", (void*)ConstraintFunctions::BallSocketCtor.address());
        if (!SafeBallSocketCtor(data)) {
            spdlog::error("[HELDBODY] BallSocketCtor crashed!");
            _aligned_free(mem);
            return nullptr;
        }
        spdlog::debug("[HELDBODY] BallSocketCtor succeeded");
        
        // Set the pivot points in body-local space
        // Convert to Havok scale - use ALIGNED vectors for SIMD operations
        alignas(16) RE::NiPoint4 hkPivotA(
            pivotA.x * HAVOK_WORLD_SCALE,
            pivotA.y * HAVOK_WORLD_SCALE,
            pivotA.z * HAVOK_WORLD_SCALE,
            0.0f);
        alignas(16) RE::NiPoint4 hkPivotB(
            pivotB.x * HAVOK_WORLD_SCALE,
            pivotB.y * HAVOK_WORLD_SCALE,
            pivotB.z * HAVOK_WORLD_SCALE,
            0.0f);
        
        spdlog::debug("[HELDBODY] Calling BallSocketSetInBodySpace: pivotA=({:.4f},{:.4f},{:.4f}), pivotB=({:.4f},{:.4f},{:.4f})",
                     hkPivotA.x, hkPivotA.y, hkPivotA.z, hkPivotB.x, hkPivotB.y, hkPivotB.z);
        if (!SafeBallSocketSetInBodySpace(data, hkPivotA, hkPivotB)) {
            spdlog::error("[HELDBODY] BallSocketSetInBodySpace crashed!");
            _aligned_free(mem);
            return nullptr;
        }
        spdlog::debug("[HELDBODY] BallSocketSetInBodySpace succeeded");
        
        return data;
    }
    
    // =========================================================================
    // NATIVE RAGDOLL CONSTRAINT (uses game's hkpRagdollConstraintData)
    // This has proper atom layout that hknp understands, plus angular motors
    // =========================================================================
    
    // Size of hkpRagdollConstraintData based on constructor analysis
    // Constructor initializes up to offset 0x180
    constexpr size_t RAGDOLL_CONSTRAINT_DATA_SIZE = 0x200;  // Add some safety margin
    
    // SEH-safe wrapper for RagdollConstraintData constructor
    static bool SafeRagdollCtor(void* data)
    {
        __try {
            heisenberg::ConstraintFunctions::RagdollConstraintData_ctor(data);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for RagdollSetInBodySpace
    static bool SafeRagdollSetInBodySpace(void* data,
                                          const RE::NiPoint4& pivotA, const RE::NiPoint4& pivotB,
                                          const RE::NiPoint4& planeA, const RE::NiPoint4& planeB,
                                          const RE::NiPoint4& twistA, const RE::NiPoint4& twistB)
    {
        __try {
            heisenberg::ConstraintFunctions::RagdollSetInBodySpace(
                data, &pivotA, &pivotB, &planeA, &planeB, &twistA, &twistB);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    // SEH-safe wrapper for RagdollSetMotor
    static bool SafeRagdollSetMotor(void* data, int motorIndex, heisenberg::hkpConstraintMotor* motor)
    {
        __try {
            heisenberg::ConstraintFunctions::RagdollSetMotor(data, motorIndex, motor);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    using RagdollSetMotorsEnabled_t = void(*)(void* ragdollConstraintData, void* runtime, char enable);
    static REL::Relocation<RagdollSetMotorsEnabled_t> g_RagdollSetMotorsEnabled{ REL::Offset(0x19b2640) };

    struct ConstraintManagerPrefix
    {
        void* vtable;
        void* constraintBuffer;
        std::uint32_t capacity;
        std::uint32_t pad14;
    };

    struct HknpConstraintEntry
    {
        std::uint32_t bodyIdA;
        std::uint32_t bodyIdB;
        void* constraintData;
        std::uint32_t constraintId;
        std::uint16_t pad14;
        std::uint8_t flags;
        std::uint8_t constraintType;
        std::uint64_t solverAtomsCache;
        std::uint16_t atomsField;
        std::int16_t runtimeAtoms;
        std::uint8_t runtimeMeta;
        std::uint8_t pad25;
        std::int16_t runtimeSize;
        void* runtime;
        std::uint64_t pad30;
    };
    static_assert(sizeof(HknpConstraintEntry) == 0x38);

    constexpr std::ptrdiff_t HKNP_WORLD_CONSTRAINT_MANAGER_OFFSET = 0x120;

    static heisenberg::hkpPositionConstraintMotor* CreateHavokPositionMotor(
        float tau,
        float damping,
        float maxForce,
        float proportionalVelocity,
        float constantVelocity)
    {
        using namespace heisenberg;

        auto* motor = ConstraintFunctions::HkAllocReferencedObject<hkpPositionConstraintMotor>();
        if (!motor) {
            spdlog::error("[HELDBODY] Failed to allocate native hkpPositionConstraintMotor on Havok heap");
            return nullptr;
        }

        std::memset(motor, 0, sizeof(hkpPositionConstraintMotor));
        motor->vtable = reinterpret_cast<void*>(REL::Module::get().base() + ConstraintFunctions::PositionConstraintMotorVtable);
        motor->memSizeAndFlags = sizeof(hkpPositionConstraintMotor);
        motor->referenceCount = 1;
        motor->type = hkpConstraintMotor::TYPE_POSITION;
        motor->minForce = -maxForce;
        motor->maxForce = maxForce;
        motor->tau = tau;
        motor->damping = damping;
        motor->proportionalRecoveryVelocity = proportionalVelocity;
        motor->constantRecoveryVelocity = constantVelocity;

        return motor;
    }

    static heisenberg::hkpPositionConstraintMotor* CreateCustomPositionMotor(
        float tau,
        float damping,
        float maxForce,
        float proportionalVelocity,
        float constantVelocity)
    {
        using namespace heisenberg;

        auto* motor = new hkpPositionConstraintMotor();
        if (!motor) {
            spdlog::error("[HELDBODY] Failed to allocate custom hkpPositionConstraintMotor");
            return nullptr;
        }

        std::memset(motor, 0, sizeof(hkpPositionConstraintMotor));
        motor->vtable = reinterpret_cast<void*>(REL::Module::get().base() + ConstraintFunctions::PositionConstraintMotorVtable);
        motor->memSizeAndFlags = sizeof(hkpPositionConstraintMotor);
        motor->referenceCount = 1;
        motor->type = hkpConstraintMotor::TYPE_POSITION;
        motor->minForce = -maxForce;
        motor->maxForce = maxForce;
        motor->tau = tau;
        motor->damping = damping;
        motor->proportionalRecoveryVelocity = proportionalVelocity;
        motor->constantRecoveryVelocity = constantVelocity;

        return motor;
    }

    static void ReleaseNativeRagdollMotorRefs(heisenberg::hkpPositionConstraintMotor* motor)
    {
        if (!motor) {
            return;
        }

        for (int i = 0; i < 3; ++i) {
            heisenberg::ConstraintFunctions::hkReferencedObject_removeReference(motor);
        }
    }

    static void* GetConstraintRuntime(void* hknpWorld, heisenberg::hknpConstraintId constraintId)
    {
        if (!hknpWorld || !constraintId.IsValid()) {
            return nullptr;
        }

        auto* manager = reinterpret_cast<ConstraintManagerPrefix*>(
            reinterpret_cast<std::uintptr_t>(hknpWorld) + HKNP_WORLD_CONSTRAINT_MANAGER_OFFSET);
        if (!manager || !manager->constraintBuffer || constraintId.m_value >= manager->capacity) {
            return nullptr;
        }

        auto* buffer = reinterpret_cast<HknpConstraintEntry*>(manager->constraintBuffer);
        return buffer[constraintId.m_value].runtime;
    }

    static bool SafeRagdollSetMotorsEnabled(void* data, void* runtime, bool enable)
    {
        __try {
            g_RagdollSetMotorsEnabled(data, runtime, enable ? 1 : 0);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    /**
     * Create native hkpRagdollConstraintData using game's constructor
     * 
     * Ragdoll constraint includes:
     * - Ball-socket for position constraint (keeps pivot points together)
     * - 3 angular motors for rotation control
     * - Twist/cone limits (we set wide limits for grabbing)
     * 
     * @param pivotA Pivot point in body A local space (hand) 
     * @param pivotB Pivot point in body B local space (object)
     * @param axisA Primary axis in body A local space (hand forward direction)
     * @param axisB Primary axis in body B local space (corresponding object axis)
     * @param motor Position constraint motor to use for angular control (can be null)
     * @return Allocated constraint data (caller owns it), or nullptr on failure
     */
    void* CreateRagdollConstraintDataLocal(
        const RE::NiPoint3& pivotA,
        const RE::NiPoint3& pivotB,
        const RE::NiMatrix3& frameA,
        const RE::NiMatrix3& frameB,
        heisenberg::hkpPositionConstraintMotor* motor)
    {
        using namespace heisenberg;
        
        // Allocate ALIGNED memory for ragdoll constraint data
        void* mem = _aligned_malloc(RAGDOLL_CONSTRAINT_DATA_SIZE, 16);
        if (!mem) {
            spdlog::error("[HELDBODY] CreateRagdollConstraintData: Failed to allocate memory");
            return nullptr;
        }
        
        std::memset(mem, 0, RAGDOLL_CONSTRAINT_DATA_SIZE);
        
        // Call the native constructor
        spdlog::debug("[HELDBODY] Calling RagdollConstraintData_ctor at {:p}",
                     (void*)ConstraintFunctions::RagdollConstraintData_ctor.address());
        
        if (!SafeRagdollCtor(mem)) {
            spdlog::error("[HELDBODY] RagdollConstraintData_ctor crashed!");
            _aligned_free(mem);
            return nullptr;
        }
        spdlog::debug("[HELDBODY] RagdollConstraintData_ctor succeeded");
        
        // Set up constraint frames using setInBodySpace
        // We need to define:
        // - Pivot points (where bodies are attached)
        // - Twist axis (primary rotation axis)
        // - Plane axis (secondary axis perpendicular to twist)
        
        // The caller passes local frames for each body. Those local frames define
        // the desired relative orientation when the constraint is at rest.
        
        // Pivot in Havok scale
        alignas(16) RE::NiPoint4 hkPivotA(
            pivotA.x * HAVOK_WORLD_SCALE,
            pivotA.y * HAVOK_WORLD_SCALE,
            pivotA.z * HAVOK_WORLD_SCALE,
            0.0f);
        alignas(16) RE::NiPoint4 hkPivotB(
            pivotB.x * HAVOK_WORLD_SCALE,
            pivotB.y * HAVOK_WORLD_SCALE,
            pivotB.z * HAVOK_WORLD_SCALE,
            0.0f);
        
        // Twist axis (Y axis of local frame A)
        alignas(16) RE::NiPoint4 twistA(
            frameA.entry[0][1], frameA.entry[1][1], frameA.entry[2][1], 0.0f);
        
        // Plane axis (X axis of local frame A)
        alignas(16) RE::NiPoint4 planeA(
            frameA.entry[0][0], frameA.entry[1][0], frameA.entry[2][0], 0.0f);
        
        // Matching axes for body B in its own local space.
        alignas(16) RE::NiPoint4 twistB(
            frameB.entry[0][1], frameB.entry[1][1], frameB.entry[2][1], 0.0f);
        alignas(16) RE::NiPoint4 planeB(
            frameB.entry[0][0], frameB.entry[1][0], frameB.entry[2][0], 0.0f);
        
        spdlog::debug("[HELDBODY] Calling RagdollSetInBodySpace:");
        spdlog::debug("[HELDBODY]   pivotA=({:.4f},{:.4f},{:.4f}), pivotB=({:.4f},{:.4f},{:.4f})",
                     hkPivotA.x, hkPivotA.y, hkPivotA.z, hkPivotB.x, hkPivotB.y, hkPivotB.z);
        spdlog::debug("[HELDBODY]   twistA=({:.4f},{:.4f},{:.4f}), twistB=({:.4f},{:.4f},{:.4f})",
                     twistA.x, twistA.y, twistA.z, twistB.x, twistB.y, twistB.z);
        
        if (!SafeRagdollSetInBodySpace(mem, hkPivotA, hkPivotB, planeA, planeB, twistA, twistB)) {
            spdlog::error("[HELDBODY] RagdollSetInBodySpace crashed!");
            _aligned_free(mem);
            return nullptr;
        }
        spdlog::debug("[HELDBODY] RagdollSetInBodySpace succeeded");
        
        // Set angular motors if provided
        // Ragdoll has 3 motors: MOTOR_TWIST(0), MOTOR_PLANE(1), MOTOR_CONE(2)
        if (motor) {
            spdlog::debug("[HELDBODY] Setting ragdoll motors");
            for (int i = 0; i < 3; ++i) {
                if (!SafeRagdollSetMotor(mem, i, motor)) {
                    spdlog::warn("[HELDBODY] RagdollSetMotor({}) failed, continuing...", i);
                }
            }
            spdlog::debug("[HELDBODY] Ragdoll motors set");
        }
        
        return mem;
    }
    
    // =========================================================================
    // LOCAL GrabConstraintData helper functions
    // These are local implementations since the main code in GrabConstraint.cpp
    // is in a disabled #if 0 block
    // =========================================================================
    
    heisenberg::GrabConstraintData* CreateGrabConstraintDataLocal(
        const RE::NiTransform& transformA,
        const RE::NiTransform& transformB,
        heisenberg::hkpPositionConstraintMotor* angularMotor,
        heisenberg::hkpPositionConstraintMotor* linearMotor)
    {
        using namespace heisenberg;
        
        if (!angularMotor || !linearMotor) {
            spdlog::error("[HELDBODY] CreateGrabConstraintData: Motors cannot be null");
            return nullptr;
        }
        
        // Use aligned allocation for Havok structures (16-byte alignment required)
        void* mem = _aligned_malloc(sizeof(GrabConstraintData), 16);
        if (!mem) {
            spdlog::error("[HELDBODY] CreateGrabConstraintData: Failed to allocate");
            return nullptr;
        }
        
        std::memset(mem, 0, sizeof(GrabConstraintData));
        auto* data = reinterpret_cast<GrabConstraintData*>(mem);
        
        // Initialize header
        // Use OUR CUSTOM VTABLE since our Atoms layout differs from real hkpRagdollConstraintData
        // The custom vtable implements getConstraintInfo(), getRuntimeInfo(), etc. for our structure
        data->vtable = GrabConstraintVtable::GetVtable();
        data->memSizeAndFlags = sizeof(GrabConstraintData);
        data->referenceCount = 1;
        data->userData = 0;
        data->constraintType = GrabConstraintVtable::CONSTRAINT_TYPE_CUSTOM;  // Custom constraint type
        
        spdlog::info("[HELDBODY] CreateGrabConstraintData: Using custom vtable at {:p}", data->vtable);
        
        // =====================================================================
        // Initialize transform atoms
        // =====================================================================
        data->atoms.transforms.type = hkpConstraintAtom::TYPE_SET_LOCAL_TRANSFORMS;
        
        // Copy transformA to hkTransform format (column-major)
        float* transA = reinterpret_cast<float*>(data->atoms.transforms.transformA);
        transA[0] = transformA.rotate.entry[0][0];
        transA[1] = transformA.rotate.entry[1][0];
        transA[2] = transformA.rotate.entry[2][0];
        transA[3] = 0.0f;
        transA[4] = transformA.rotate.entry[0][1];
        transA[5] = transformA.rotate.entry[1][1];
        transA[6] = transformA.rotate.entry[2][1];
        transA[7] = 0.0f;
        transA[8] = transformA.rotate.entry[0][2];
        transA[9] = transformA.rotate.entry[1][2];
        transA[10] = transformA.rotate.entry[2][2];
        transA[11] = 0.0f;
        transA[12] = transformA.translate.x * HAVOK_WORLD_SCALE;
        transA[13] = transformA.translate.y * HAVOK_WORLD_SCALE;
        transA[14] = transformA.translate.z * HAVOK_WORLD_SCALE;
        transA[15] = 0.0f;
        
        // Copy transformB
        float* transB = reinterpret_cast<float*>(data->atoms.transforms.transformB);
        transB[0] = transformB.rotate.entry[0][0];
        transB[1] = transformB.rotate.entry[1][0];
        transB[2] = transformB.rotate.entry[2][0];
        transB[3] = 0.0f;
        transB[4] = transformB.rotate.entry[0][1];
        transB[5] = transformB.rotate.entry[1][1];
        transB[6] = transformB.rotate.entry[2][1];
        transB[7] = 0.0f;
        transB[8] = transformB.rotate.entry[0][2];
        transB[9] = transformB.rotate.entry[1][2];
        transB[10] = transformB.rotate.entry[2][2];
        transB[11] = 0.0f;
        transB[12] = transformB.translate.x * HAVOK_WORLD_SCALE;
        transB[13] = transformB.translate.y * HAVOK_WORLD_SCALE;
        transB[14] = transformB.translate.z * HAVOK_WORLD_SCALE;
        transB[15] = 0.0f;
        
        // =====================================================================
        // Initialize stabilization atom
        // =====================================================================
        // 1:1 with ROCK (writeSetupStabilizationDefaults): stabilization is DISABLED
        // and all three clamps are HK_REAL_MAX so it never fights the motors. The old
        // code enabled it with maxAngle=0.1 and left maxLinearImpulse/maxAngularImpulse
        // at 0 (from memset) — enabled stabilization with zero impulse clamps clamps the
        // solver impulses toward zero, which destabilized / killed the grab.
        data->atoms.setupStabilization.type = hkpConstraintAtom::TYPE_SETUP_STABILIZATION;
        data->atoms.setupStabilization.enabled = false;
        data->atoms.setupStabilization.maxLinearImpulse = 3.402823466e+38f;   // HK_REAL_MAX
        data->atoms.setupStabilization.maxAngularImpulse = 3.402823466e+38f;  // HK_REAL_MAX
        data->atoms.setupStabilization.maxAngle = 3.402823466e+38f;           // HK_REAL_MAX
        
        // =====================================================================
        // Initialize ragdoll motor atom (3-axis angular control)
        // =====================================================================
        data->atoms.ragdollMotors.type = hkpConstraintAtom::TYPE_RAGDOLL_MOTOR;
        data->atoms.ragdollMotors.enabled = true;
        // Runtime offsets relative to runtime cursor when ragdoll motor atom is processed.
        // Cursor starts at 0x00; initialized[3] at Runtime+0x60, angles[3] at Runtime+0x64.
        data->atoms.ragdollMotors.initializedOffset = offsetof(heisenberg::GrabConstraintData::Runtime, initialized);
        data->atoms.ragdollMotors.previousTargetAnglesOffset = offsetof(heisenberg::GrabConstraintData::Runtime, previousTargetAngles);
        
        // Set identity target rotation
        data->atoms.ragdollMotors.target_bRca[0] = 1.0f;
        data->atoms.ragdollMotors.target_bRca[1] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[2] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[3] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[4] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[5] = 1.0f;
        data->atoms.ragdollMotors.target_bRca[6] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[7] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[8] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[9] = 0.0f;
        data->atoms.ragdollMotors.target_bRca[10] = 1.0f;
        data->atoms.ragdollMotors.target_bRca[11] = 0.0f;
        
        // Assign angular motor to all 3 axes
        data->atoms.ragdollMotors.motors[0] = angularMotor;
        data->atoms.ragdollMotors.motors[1] = angularMotor;
        data->atoms.ragdollMotors.motors[2] = angularMotor;
        
        // =====================================================================
        // Initialize linear motor atoms (3-axis position control)
        // =====================================================================
        // Linear motor offsets: cursor at 0x30 after ragdoll motor, +0x10 per linear motor
        using Rt = heisenberg::GrabConstraintData::Runtime;
        data->atoms.linearMotor0.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        data->atoms.linearMotor0.isEnabled = true;
        data->atoms.linearMotor0.motorAxis = 0;
        data->atoms.linearMotor0.initializedOffset = offsetof(Rt, initializedLinear) - 0x30;
        data->atoms.linearMotor0.previousTargetPosOffset = offsetof(Rt, previousTargetPositions) - 0x30;
        data->atoms.linearMotor0.targetPosition = 0.0f;
        data->atoms.linearMotor0.motor = linearMotor;

        data->atoms.linearMotor1.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        data->atoms.linearMotor1.isEnabled = true;
        data->atoms.linearMotor1.motorAxis = 1;
        data->atoms.linearMotor1.initializedOffset = static_cast<std::int16_t>(offsetof(Rt, initializedLinear) + 1 - 0x40);
        data->atoms.linearMotor1.previousTargetPosOffset = static_cast<std::int16_t>(offsetof(Rt, previousTargetPositions) + 4 - 0x40);
        data->atoms.linearMotor1.targetPosition = 0.0f;
        data->atoms.linearMotor1.motor = linearMotor;

        data->atoms.linearMotor2.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        data->atoms.linearMotor2.isEnabled = true;
        data->atoms.linearMotor2.motorAxis = 2;
        data->atoms.linearMotor2.initializedOffset = static_cast<std::int16_t>(offsetof(Rt, initializedLinear) + 2 - 0x50);
        data->atoms.linearMotor2.previousTargetPosOffset = static_cast<std::int16_t>(offsetof(Rt, previousTargetPositions) + 8 - 0x50);
        data->atoms.linearMotor2.targetPosition = 0.0f;
        data->atoms.linearMotor2.motor = linearMotor;
        
        spdlog::debug("[HELDBODY] Created GrabConstraintData with 6-DOF motors");
        
        return data;
    }
    
    void DestroyGrabConstraintDataLocal(heisenberg::GrabConstraintData* data)
    {
        if (data) {
            // Note: Motors are NOT destroyed here - caller owns them
            // Use _aligned_free to match _aligned_malloc for Havok structures
            _aligned_free(data);
        }
    }
}

namespace heisenberg
{
    // =========================================================================
    // HeldBodyHandPhysics
    // =========================================================================
    
    void HeldBodyHandPhysics::Invalidate()
    {
        bodyId = 0x7FFFFFFF;
        shape = nullptr;
        hknpWorld = nullptr;
        bhkWorld = nullptr;
        physicsSystem = nullptr;
        hknpPhysicsSystem = nullptr;
        collisionObject = nullptr;
        // Note: Don't free aligned memory here - done in DestroyHandBody
        alignedSystemDataMem = nullptr;
        alignedPhysicsSystemMem = nullptr;
        alignedHknpSystemMem = nullptr;
        alignedBodyCinfoMem = nullptr;
        alignedMaterialMem = nullptr;
        alignedCollisionObjMem = nullptr;
        valid = false;
        createdTime = 0.0;
        prevPosition = RE::NiPoint3{};
        prevRotation = RE::NiMatrix3{};
    }

    // =========================================================================
    // HeldBodyGrabConstraint
    // =========================================================================
    
    void HeldBodyGrabConstraint::Invalidate()
    {
        constraintId.Invalidate();
        handBodyId = 0x7FFFFFFF;
        objectBodyId = 0x7FFFFFFF;
        hknpWorld = nullptr;
        driveMode = HeldBodyDriveMode::None;
        active = false;
        angularMotor = nullptr;
        linearMotor = nullptr;
        constraintData = nullptr;
    }

    // =========================================================================
    // HeldBodyGrabManager - Initialization
    // =========================================================================

    bool HeldBodyGrabManager::Initialize()
    {
        if (_initialized) {
            return true;
        }

        spdlog::info("[HELDBODY] Initializing HeldBody grab system...");
        
        _leftHandBody.Invalidate();
        _rightHandBody.Invalidate();
        _leftConstraint.Invalidate();
        _rightConstraint.Invalidate();
        
        _initialized = true;
        spdlog::info("[HELDBODY] HeldBody grab system initialized (hand bodies created on first grab)");
        return true;
    }

    void HeldBodyGrabManager::Shutdown()
    {
        spdlog::info("[HELDBODY] Shutting down HeldBody grab system...");
        
        // Destroy any active constraints first
        GrabState dummyState;
        dummyState.constraintId = _leftConstraint.constraintId.m_value;
        dummyState.handBodyId = _leftConstraint.handBodyId;
        if (_leftConstraint.IsValid()) {
            DestroyGrabConstraint(dummyState);
        }
        dummyState.constraintId = _rightConstraint.constraintId.m_value;
        dummyState.handBodyId = _rightConstraint.handBodyId;
        if (_rightConstraint.IsValid()) {
            DestroyGrabConstraint(dummyState);
        }
        
        // Then destroy hand bodies
        DestroyHandBody(_leftHandBody);
        DestroyHandBody(_rightHandBody);
        
        _initialized = false;
    }

    // =========================================================================
    // World Access
    // =========================================================================

    void* HeldBodyGrabManager::GetCurrentHknpWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }
        
        auto* cell = player->parentCell;
        
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        void* bhkWorld = GetbhkWorld(cell);
        if (!bhkWorld) {
            return nullptr;
        }
        
        // hknpBSWorld* is at offset 0x60 in bhkWorld
        void* hknpWorld = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkWorld) + 0x60);
        return hknpWorld;
    }

    void* HeldBodyGrabManager::GetCurrentBhkWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }
        
        auto* cell = player->parentCell;
        
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        return GetbhkWorld(cell);
    }

    // =========================================================================
    // Hand Body Creation - Full Bethesda Wrapper Chain
    // 
    // CRITICAL: This follows the VERIFIED WORKING approach from HandCollision.cpp backups:
    // 1. Create hknpPhysicsSystemData with body cinfo
    // 2. Create bhkPhysicsSystem from the data  
    // 3. Create hknpPhysicsSystem via constructor
    // 4. Create bhkNPCollisionObject from the physics system (THIS WAS MISSING!)
    // 5. Call AddToWorld to set up backpointer and register with world (THIS WAS MISSING!)
    // 6. Set motion type to KEYFRAMED via Bethesda wrapper
    // 7. Commit and activate body
    // 
    // The PREVIOUS approach skipped steps 4-6, causing crashes after body creation.
    // =========================================================================

    bool HeldBodyGrabManager::CreateHandBody(HeldBodyHandPhysics& handBody, bool isLeft,
                                              const RE::NiPoint3& position, 
                                              void* hknpWorld, void* bhkWorld)
    {
        if (!hknpWorld || !bhkWorld) {
            spdlog::error("[HELDBODY] CreateHandBody: No world");
            return false;
        }
        
        spdlog::info("[HELDBODY] Creating {} hand body (FULL Bethesda wrapper chain)...", 
                     isLeft ? "LEFT" : "RIGHT");
        spdlog::info("[HELDBODY] Initial position: ({:.2f}, {:.2f}, {:.2f}) -> Havok ({:.4f}, {:.4f}, {:.4f})",
                     position.x, position.y, position.z,
                     position.x * HAVOK_WORLD_SCALE, position.y * HAVOK_WORLD_SCALE, position.z * HAVOK_WORLD_SCALE);
        
        // =====================================================================
        // STEP 1: Create box shape for hand
        // =====================================================================
        
        alignas(16) ConstraintFunctions::hkAabb aabb;
        aabb.min = RE::NiPoint4(-HAND_HALF_X, -HAND_HALF_Y, -HAND_HALF_Z, 0.0f);
        aabb.max = RE::NiPoint4(HAND_HALF_X, HAND_HALF_Y, HAND_HALF_Z, 0.0f);
        
        alignas(16) ConstraintFunctions::hknpConvexShapeBuildConfig buildConfig;
        ConstraintFunctions::BuildConfigCtor(&buildConfig);
        
        void* shape = ConstraintFunctions::CreateConvexShapeFromAabb(aabb, 0.0f, &buildConfig);
        if (!shape) {
            spdlog::error("[HELDBODY] Failed to create box shape");
            return false;
        }
        
        spdlog::info("[HELDBODY] STEP 1: Created box shape at {:p}", shape);
        
        // =====================================================================
        // STEP 2: Create hknpPhysicsSystemData with body cinfo
        // =====================================================================
        
        // Allocate system data with proper alignment
        void* systemDataMem = _aligned_malloc(sizeof(hknpPhysicsSystemData), 16);
        if (!systemDataMem) {
            spdlog::error("[HELDBODY] Failed to allocate hknpPhysicsSystemData");
            return false;
        }
        std::memset(systemDataMem, 0, sizeof(hknpPhysicsSystemData));
        
        // Call Havok constructor to properly initialize
        ConstraintFunctions::PhysicsSystemDataCtor(systemDataMem);
        spdlog::info("[HELDBODY] Created hknpPhysicsSystemData at {:p}", systemDataMem);
        
        // Create material descriptor (REQUIRED - body creation indexes into materials array)
        void* materialMem = _aligned_malloc(sizeof(hknpMaterialDescriptor), 16);
        if (!materialMem) {
            spdlog::error("[HELDBODY] Failed to allocate hknpMaterialDescriptor");
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(materialMem, 0, sizeof(hknpMaterialDescriptor));
        
        // Create body cinfo
        void* bodyCinfoMem = _aligned_malloc(sizeof(hknpBodyCinfo), 16);
        if (!bodyCinfoMem) {
            spdlog::error("[HELDBODY] Failed to allocate hknpBodyCinfo");
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        hknpBodyCinfo* bodyCinfo = reinterpret_cast<hknpBodyCinfo*>(bodyCinfoMem);
        ConstraintFunctions::BodyCinfoCtor(bodyCinfo);
        
        // Set body properties
        bodyCinfo->shape = shape;
        bodyCinfo->qualityId = MOTION_KEYFRAMED;  // KEYFRAMED = 2
        bodyCinfo->materialId = 0;
        bodyCinfo->collisionFilterInfo = NONCOLLIDABLE_FILTER | COLLISION_FLAG_DISABLED;  // Layer 15, start disabled
        bodyCinfo->flags = 0;
        bodyCinfo->position = RE::NiPoint4(
            position.x * HAVOK_WORLD_SCALE,
            position.y * HAVOK_WORLD_SCALE,
            position.z * HAVOK_WORLD_SCALE,
            0.0f
        );
        bodyCinfo->orientation = RE::NiPoint4(0.0f, 0.0f, 0.0f, 1.0f);  // Identity quaternion
        
        spdlog::info("[HELDBODY] bodyCinfo: shape={:p}, pos=({:.4f},{:.4f},{:.4f}), qualityId={}, filter=0x{:08X}",
                     bodyCinfo->shape,
                     bodyCinfo->position.x, bodyCinfo->position.y, bodyCinfo->position.z,
                     bodyCinfo->qualityId, bodyCinfo->collisionFilterInfo);
        
        // Set up arrays in system data
        std::uint8_t* sdBytes = reinterpret_cast<std::uint8_t*>(systemDataMem);
        
        // Set materials array at 0x10
        *reinterpret_cast<void**>(sdBytes + 0x10) = materialMem;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x18) = 1;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x1C) = 0x80000001;
        
        // Set bodyCinfos array at 0x40
        *reinterpret_cast<void**>(sdBytes + 0x40) = bodyCinfo;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x48) = 1;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x4C) = 0x80000001;
        
        // =====================================================================
        // STEP 3: Create bhkPhysicsSystem from the data
        // =====================================================================
        
        void* physicsSystemMem = _aligned_malloc(0x30, 16);
        if (!physicsSystemMem) {
            spdlog::error("[HELDBODY] Failed to allocate bhkPhysicsSystem");
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(physicsSystemMem, 0, 0x30);
        
        ConstraintFunctions::BhkPhysicsSystemCtor(physicsSystemMem, systemDataMem);
        spdlog::info("[HELDBODY] Created bhkPhysicsSystem at {:p}", physicsSystemMem);
        
        // =====================================================================
        // STEP 4: Create hknpPhysicsSystem directly
        // =====================================================================
        
        constexpr size_t HKNP_PHYSICS_SYSTEM_SIZE = 0x60;
        void* hknpSystemMem = _aligned_malloc(HKNP_PHYSICS_SYSTEM_SIZE, 16);
        if (!hknpSystemMem) {
            spdlog::error("[HELDBODY] Failed to allocate hknpPhysicsSystem");
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(hknpSystemMem, 0, HKNP_PHYSICS_SYSTEM_SIZE);
        
        // Create identity transform
        alignas(16) std::uint8_t identityTransform[0x40];
        std::memset(identityTransform, 0, sizeof(identityTransform));
        reinterpret_cast<float*>(identityTransform)[0] = 1.0f;   // Col0: (1,0,0,0)
        reinterpret_cast<float*>(identityTransform)[5] = 1.0f;   // Col1: (0,1,0,0)
        reinterpret_cast<float*>(identityTransform)[10] = 1.0f;  // Col2: (0,0,1,0)
        reinterpret_cast<float*>(identityTransform)[15] = 1.0f;  // Col3: (0,0,0,1)
        
        spdlog::info("[HELDBODY] Calling HknpPhysicsSystemCtor...");
        
        // Call hknpPhysicsSystem constructor with SEH protection
        // NOTE: Do NOT lock world - HknpPhysicsSystemCtor handles its own locking!
        bool ctorSuccess = SafeHknpPhysicsSystemCtor(
            hknpSystemMem,      // Output
            systemDataMem,      // hknpPhysicsSystemData*
            hknpWorld,          // hknpWorld*
            identityTransform,  // hkTransformf (identity)
            1,                  // AdditionMode::Immediate
            0,                  // AdditionFlags
            0                   // Flags
        );
        
        if (!ctorSuccess) {
            spdlog::error("[HELDBODY] HknpPhysicsSystemCtor CRASHED");
            _aligned_free(hknpSystemMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        spdlog::info("[HELDBODY] HknpPhysicsSystemCtor completed, hknpSystem={:p}", hknpSystemMem);
        
        // Store hknpPhysicsSystem in bhkPhysicsSystem at offset 0x18
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physicsSystemMem) + 0x18) = hknpSystemMem;
        
        spdlog::info("[HELDBODY] STEP 4: Stored hknpPhysicsSystem in bhkPhysicsSystem");
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 5: Create bhkNPCollisionObject from the physics system
        // THIS WAS THE MISSING STEP THAT CAUSED CRASHES!
        // =====================================================================
        
        // bhkNPCollisionObject is ~0x30 bytes based on Ghidra
        void* collisionObjMem = _aligned_malloc(0x30, 16);
        if (!collisionObjMem) {
            spdlog::error("[HELDBODY] Failed to allocate bhkNPCollisionObject");
            _aligned_free(hknpSystemMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(collisionObjMem, 0, 0x30);
        
        // Call bhkNPCollisionObject constructor
        // Parameters: this, bodyIndex (0 for first body), bhkPhysicsSystem*
        spdlog::info("[HELDBODY] Calling BhkNPCollisionObjectCtor...");
        spdlog::default_logger()->flush();
        ConstraintFunctions::BhkNPCollisionObjectCtor(collisionObjMem, 0, physicsSystemMem);
        spdlog::info("[HELDBODY] STEP 5: Created bhkNPCollisionObject at {:p}", collisionObjMem);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 6: Add to world - THIS sets up the backpointer and registers properly!
        // =====================================================================
        
        spdlog::info("[HELDBODY] Calling BhkNPCollisionObjectAddToWorld...");
        spdlog::default_logger()->flush();
        
        bool addSuccess = SafeAddToWorld(collisionObjMem, bhkWorld);
        
        if (!addSuccess) {
            spdlog::error("[HELDBODY] SafeAddToWorld CRASHED - exception caught");
            _aligned_free(collisionObjMem);
            _aligned_free(hknpSystemMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        spdlog::info("[HELDBODY] STEP 6: AddToWorld completed");
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 7: Get body ID from physics system
        // =====================================================================
        
        std::uint32_t bodyId = 0x7FFFFFFF;
        ConstraintFunctions::BhkPhysicsSystemGetBodyId(physicsSystemMem, &bodyId, 0);
        
        if (bodyId == 0x7FFFFFFF) {
            spdlog::error("[HELDBODY] Failed to get body ID from physics system");
            _aligned_free(collisionObjMem);
            _aligned_free(hknpSystemMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        spdlog::info("[HELDBODY] STEP 7: Got body ID 0x{:08X} from physics system", bodyId);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 8: Set motion type to KEYFRAMED via Bethesda wrapper
        // =====================================================================
        
        spdlog::info("[HELDBODY] Setting motion type to KEYFRAMED...");
        spdlog::default_logger()->flush();
        
        bool motionSuccess = SafeSetMotionType(collisionObjMem, 2);  // 2 = KEYFRAMED
        if (!motionSuccess) {
            spdlog::warn("[HELDBODY] SetMotionType SEH exception - continuing anyway");
        } else {
            spdlog::info("[HELDBODY] STEP 8: SetMotionType(KEYFRAMED) completed");
        }
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 9: Commit added bodies and activate
        // =====================================================================
        
        spdlog::info("[HELDBODY] Calling commitAddBodies and activateBody...");
        spdlog::default_logger()->flush();
        
        ConstraintFunctions::hknpWorld_commitAddBodies(hknpWorld);
        spdlog::info("[HELDBODY] commitAddBodies done");
        spdlog::default_logger()->flush();
        
        ConstraintFunctions::hknpWorld_activateBody(hknpWorld, bodyId);
        spdlog::info("[HELDBODY] STEP 9: activateBody done");
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 10: Store results
        // =====================================================================
        
        spdlog::info("[HELDBODY] STEP 10: Storing results in handBody struct...");
        spdlog::default_logger()->flush();
        
        handBody.bodyId = bodyId;
        handBody.shape = shape;
        handBody.hknpWorld = hknpWorld;
        handBody.bhkWorld = bhkWorld;
        handBody.physicsSystem = physicsSystemMem;
        handBody.hknpPhysicsSystem = hknpSystemMem;
        handBody.collisionObject = collisionObjMem;  // NEW: Store collision object!
        handBody.alignedSystemDataMem = systemDataMem;
        handBody.alignedPhysicsSystemMem = physicsSystemMem;
        handBody.alignedHknpSystemMem = hknpSystemMem;
        handBody.alignedBodyCinfoMem = bodyCinfoMem;
        handBody.alignedMaterialMem = materialMem;
        handBody.alignedCollisionObjMem = collisionObjMem;  // NEW: Track for cleanup
        handBody.valid = true;
        handBody.prevPosition = position;
        
        spdlog::info("[HELDBODY] Successfully created {} hand body with ID 0x{:08X}",
                     isLeft ? "LEFT" : "RIGHT", bodyId);
        spdlog::default_logger()->flush();
        
        spdlog::info("[HELDBODY] About to return true from CreateHandBody...");
        spdlog::default_logger()->flush();
        
        return true;
    }

    void HeldBodyGrabManager::DestroyHandBody(HeldBodyHandPhysics& handBody)
    {
        if (!handBody.IsValid()) {
            return;
        }
        
        spdlog::info("[HELDBODY] Destroying hand body ID: 0x{:08X}", handBody.bodyId);
        
        // Destroy the body in the world
        if (handBody.hknpWorld && handBody.bodyId != 0x7FFFFFFF) {
            std::uint32_t ids[1] = { handBody.bodyId };
            SafeDestroyBodies(handBody.hknpWorld, ids, 1);
        }
        
        // Free aligned memory (including NEW collision object)
        if (handBody.alignedCollisionObjMem) _aligned_free(handBody.alignedCollisionObjMem);
        if (handBody.alignedHknpSystemMem) _aligned_free(handBody.alignedHknpSystemMem);
        if (handBody.alignedPhysicsSystemMem) _aligned_free(handBody.alignedPhysicsSystemMem);
        if (handBody.alignedBodyCinfoMem) _aligned_free(handBody.alignedBodyCinfoMem);
        if (handBody.alignedMaterialMem) _aligned_free(handBody.alignedMaterialMem);
        if (handBody.alignedSystemDataMem) _aligned_free(handBody.alignedSystemDataMem);
        
        handBody.Invalidate();
    }

    // =========================================================================
    // CreateHandBodySimple - Alternative HIGGS Skyrim-style approach
    // 
    // This simpler approach:
    // 1. Create shape
    // 2. Create bhkPhysicsSystem with body data (constructor)
    // 3. Call CreateInstance to create internal hknpPhysicsSystem
    // 4. Create bhkNPCollisionObject wrapper
    // 5. Call AddToWorld
    // 
    // Toggle USE_SIMPLE_HAND_BODY_CREATION to test this vs the full chain approach.
    // =========================================================================
    
    bool HeldBodyGrabManager::CreateHandBodySimple(HeldBodyHandPhysics& handBody, bool isLeft,
                                                    const RE::NiPoint3& position, 
                                                    void* hknpWorld, void* bhkWorld)
    {
        if (!hknpWorld || !bhkWorld) {
            spdlog::error("[HELDBODY-SIMPLE] CreateHandBodySimple: No world");
            return false;
        }
        
        spdlog::info("[HELDBODY-SIMPLE] Creating {} hand body (SIMPLE approach)...", 
                     isLeft ? "LEFT" : "RIGHT");
        spdlog::info("[HELDBODY-SIMPLE] Position: ({:.2f}, {:.2f}, {:.2f})",
                     position.x, position.y, position.z);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 1: Create box shape for hand
        // =====================================================================
        
        alignas(16) ConstraintFunctions::hkAabb aabb;
        aabb.min = RE::NiPoint4(-HAND_HALF_X, -HAND_HALF_Y, -HAND_HALF_Z, 0.0f);
        aabb.max = RE::NiPoint4(HAND_HALF_X, HAND_HALF_Y, HAND_HALF_Z, 0.0f);
        
        alignas(16) ConstraintFunctions::hknpConvexShapeBuildConfig buildConfig;
        ConstraintFunctions::BuildConfigCtor(&buildConfig);
        
        void* shape = ConstraintFunctions::CreateConvexShapeFromAabb(aabb, 0.0f, &buildConfig);
        if (!shape) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to create box shape");
            return false;
        }
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 1: Created box shape at {:p}", shape);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 2: Create hknpPhysicsSystemData with body cinfo
        // =====================================================================
        
        void* systemDataMem = _aligned_malloc(sizeof(hknpPhysicsSystemData), 16);
        if (!systemDataMem) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to allocate hknpPhysicsSystemData");
            return false;
        }
        std::memset(systemDataMem, 0, sizeof(hknpPhysicsSystemData));
        ConstraintFunctions::PhysicsSystemDataCtor(systemDataMem);
        
        // Create material descriptor
        void* materialMem = _aligned_malloc(sizeof(hknpMaterialDescriptor), 16);
        if (!materialMem) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to allocate material");
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(materialMem, 0, sizeof(hknpMaterialDescriptor));
        
        // Create body cinfo
        void* bodyCinfoMem = _aligned_malloc(sizeof(hknpBodyCinfo), 16);
        if (!bodyCinfoMem) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to allocate bodyCinfo");
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        hknpBodyCinfo* bodyCinfo = reinterpret_cast<hknpBodyCinfo*>(bodyCinfoMem);
        ConstraintFunctions::BodyCinfoCtor(bodyCinfo);
        
        bodyCinfo->shape = shape;
        bodyCinfo->qualityId = MOTION_KEYFRAMED;
        bodyCinfo->materialId = 0;
        bodyCinfo->collisionFilterInfo = NONCOLLIDABLE_FILTER;  // Layer 15 - no player collision
        bodyCinfo->flags = 0;
        bodyCinfo->position = RE::NiPoint4(
            position.x * HAVOK_WORLD_SCALE,
            position.y * HAVOK_WORLD_SCALE,
            position.z * HAVOK_WORLD_SCALE,
            0.0f
        );
        bodyCinfo->orientation = RE::NiPoint4(0.0f, 0.0f, 0.0f, 1.0f);
        
        // Set up arrays in system data
        std::uint8_t* sdBytes = reinterpret_cast<std::uint8_t*>(systemDataMem);
        *reinterpret_cast<void**>(sdBytes + 0x10) = materialMem;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x18) = 1;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x1C) = 0x80000001;
        *reinterpret_cast<void**>(sdBytes + 0x40) = bodyCinfo;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x48) = 1;
        *reinterpret_cast<std::uint32_t*>(sdBytes + 0x4C) = 0x80000001;
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 2: Created systemData with bodyCinfo");
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 3: Create bhkPhysicsSystem from data
        // =====================================================================
        
        void* physicsSystemMem = _aligned_malloc(0x30, 16);
        if (!physicsSystemMem) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to allocate bhkPhysicsSystem");
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(physicsSystemMem, 0, 0x30);
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 3: Calling BhkPhysicsSystemCtor...");
        spdlog::default_logger()->flush();
        
        ConstraintFunctions::BhkPhysicsSystemCtor(physicsSystemMem, systemDataMem);
        
        spdlog::info("[HELDBODY-SIMPLE] Created bhkPhysicsSystem at {:p}", physicsSystemMem);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 4: Call CreateInstance to create internal hknpPhysicsSystem
        // This is the key difference - let Bethesda wrapper handle hknpPhysicsSystem
        // =====================================================================
        
        // Create identity transform for placement
        alignas(16) std::uint8_t identityTransform[0x40];
        std::memset(identityTransform, 0, sizeof(identityTransform));
        reinterpret_cast<float*>(identityTransform)[0] = 1.0f;   // Col0
        reinterpret_cast<float*>(identityTransform)[5] = 1.0f;   // Col1
        reinterpret_cast<float*>(identityTransform)[10] = 1.0f;  // Col2
        reinterpret_cast<float*>(identityTransform)[15] = 1.0f;  // Col3
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 4: Calling BhkPhysicsSystemCreateInstance...");
        spdlog::default_logger()->flush();
        
        ConstraintFunctions::BhkPhysicsSystemCreateInstance(physicsSystemMem, bhkWorld, identityTransform);
        
        spdlog::info("[HELDBODY-SIMPLE] CreateInstance completed");
        spdlog::default_logger()->flush();
        
        // Verify hknpPhysicsSystem was created at offset 0x18
        void* hknpSystem = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physicsSystemMem) + 0x18);
        if (!hknpSystem) {
            spdlog::error("[HELDBODY-SIMPLE] CreateInstance did NOT create hknpPhysicsSystem!");
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        spdlog::info("[HELDBODY-SIMPLE] hknpPhysicsSystem created at {:p}", hknpSystem);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 5: Create bhkNPCollisionObject wrapper
        // =====================================================================
        
        void* collisionObjMem = _aligned_malloc(0x30, 16);
        if (!collisionObjMem) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to allocate bhkNPCollisionObject");
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(collisionObjMem, 0, 0x30);
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 5: Calling BhkNPCollisionObjectCtor(idx=0, system)...");
        spdlog::default_logger()->flush();
        
        // bodyIdx=0 for first body in system - mirrors HIGGS Skyrim pattern
        ConstraintFunctions::BhkNPCollisionObjectCtor(collisionObjMem, 0, physicsSystemMem);
        
        spdlog::info("[HELDBODY-SIMPLE] Created bhkNPCollisionObject at {:p}", collisionObjMem);
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 6: Add to world
        // =====================================================================
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 6: Calling AddToWorld...");
        spdlog::default_logger()->flush();
        
        bool addSuccess = SafeAddToWorld(collisionObjMem, bhkWorld);
        if (!addSuccess) {
            spdlog::error("[HELDBODY-SIMPLE] AddToWorld CRASHED");
            _aligned_free(collisionObjMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        spdlog::info("[HELDBODY-SIMPLE] AddToWorld completed");
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 7: Get body ID and set motion type
        // =====================================================================
        
        std::uint32_t bodyId = 0x7FFFFFFF;
        ConstraintFunctions::BhkPhysicsSystemGetBodyId(physicsSystemMem, &bodyId, 0);
        
        if (bodyId == 0x7FFFFFFF) {
            spdlog::error("[HELDBODY-SIMPLE] Failed to get body ID");
            _aligned_free(collisionObjMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        
        spdlog::info("[HELDBODY-SIMPLE] STEP 7: Got body ID 0x{:08X}", bodyId);
        spdlog::default_logger()->flush();
        
        // Set motion type to KEYFRAMED
        spdlog::info("[HELDBODY-SIMPLE] Setting motion type to KEYFRAMED...");
        SafeSetMotionType(collisionObjMem, 2);  // 2 = KEYFRAMED
        
        // Commit and activate
        ConstraintFunctions::hknpWorld_commitAddBodies(hknpWorld);
        ConstraintFunctions::hknpWorld_activateBody(hknpWorld, bodyId);
        
        spdlog::info("[HELDBODY-SIMPLE] Committed and activated body");
        spdlog::default_logger()->flush();
        
        // =====================================================================
        // STEP 8: Store results
        // =====================================================================
        
        handBody.bodyId = bodyId;
        handBody.shape = shape;
        handBody.hknpWorld = hknpWorld;
        handBody.bhkWorld = bhkWorld;
        handBody.physicsSystem = physicsSystemMem;
        handBody.hknpPhysicsSystem = hknpSystem;
        handBody.collisionObject = collisionObjMem;
        handBody.alignedSystemDataMem = systemDataMem;
        handBody.alignedPhysicsSystemMem = physicsSystemMem;
        handBody.alignedHknpSystemMem = nullptr;  // Not separately allocated in this approach
        handBody.alignedBodyCinfoMem = bodyCinfoMem;
        handBody.alignedMaterialMem = materialMem;
        handBody.alignedCollisionObjMem = collisionObjMem;
        handBody.valid = true;
        handBody.prevPosition = position;
        
        spdlog::info("[HELDBODY-SIMPLE] Successfully created {} hand body with ID 0x{:08X}",
                     isLeft ? "LEFT" : "RIGHT", bodyId);
        spdlog::default_logger()->flush();
        
        return true;
    }

    void HeldBodyGrabManager::UpdateHandBodyPosition(HeldBodyHandPhysics& handBody,
                                                      const RE::NiPoint3& position,
                                                      const RE::NiMatrix3& rotation,
                                                      float deltaTime)
    {
        if (!handBody.IsValid()) {
            spdlog::warn("[HELDBODY] UpdateHandBodyPosition: hand body not valid");
            return;
        }

        // Convert position to Havok units
        RE::NiPoint4 hkPosition(
            position.x * HAVOK_WORLD_SCALE,
            position.y * HAVOK_WORLD_SCALE,
            position.z * HAVOK_WORLD_SCALE,
            0.0f
        );

        RE::NiPoint4 hkOrientation = MatrixToQuaternion(rotation);

        float invDeltaTime = deltaTime > 0.0001f ? 1.0f / deltaTime : 60.0f;

        // Apply hard keyframe (velocity-based movement)
        SafeApplyHardKeyFrame(handBody.hknpWorld, handBody.bodyId,
                              hkPosition, hkOrientation, invDeltaTime);

        // =====================================================================
        // VELOCITY CLAMPING (HIGGS: ApplyHardKeyframeVelocityClamped)
        // If the hand moved too fast, the velocity-based approach can't keep up
        // (Havok clamps internal body velocity). Teleport the body directly.
        // =====================================================================
        if (handBody.prevPosition.x != 0.0f || handBody.prevPosition.y != 0.0f || handBody.prevPosition.z != 0.0f) {
            RE::NiPoint3 delta = position - handBody.prevPosition;
            float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
            // Max velocity ~20 m/s in game units (~1400 units/sec). If moved more than
            // maxVel*dt, the keyframe velocity was clamped internally.
            float maxDist = 1400.0f * deltaTime;
            if (distSq > maxDist * maxDist) {
                // Teleport: directly set body position instead of velocity
                __try {
                    ConstraintFunctions::hknpWorld_setBodyPosition(
                        handBody.hknpWorld, handBody.bodyId, hkPosition, 0);  // 0 = DO_NOT_ACTIVATE
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {
                    // Silently ignore
                }
            }
        }

        handBody.prevPosition = position;
        handBody.prevRotation = rotation;
    }

    // =========================================================================
    // Grab Start/Update/End
    // =========================================================================

    bool HeldBodyGrabManager::StartGrab(GrabState& state, bool isLeft,
                                         const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                                         RE::TESObjectREFR* refr, RE::NiAVObject* node,
                                         RE::bhkNPCollisionObject* collisionObject)
    {
        spdlog::info("[HELDBODY] StartGrab: {} hand grabbing {:p}",
                     isLeft ? "LEFT" : "RIGHT", (void*)refr);
        
        // Get current worlds
        void* hknpWorld = GetCurrentHknpWorld();
        void* bhkWorld = GetCurrentBhkWorld();
        if (!hknpWorld || !bhkWorld) {
            spdlog::error("[HELDBODY] StartGrab: No physics world");
            return false;
        }
        
        // NOTE: Proxy listener registration moved to HookEndUpdate (Hooks.cpp)
        // to avoid deadlock — addListener modifies the proxy during/near the physics step.

        HeldBodyHandPhysics& handBody = isLeft ? _leftHandBody : _rightHandBody;
        RE::NiPoint3 resolvedHandPos = handPos;
        RE::NiMatrix3 resolvedHandRot = handRot;
        ResolveHeldBodyHandDriveTransform(state, isLeft, handPos, handRot, resolvedHandPos, resolvedHandRot);

        // Create hand body if needed
        if (!handBody.IsValid() || handBody.hknpWorld != hknpWorld) {
            if (handBody.IsValid()) {
                DestroyHandBody(handBody);
            }
            
            // Toggle between creation approaches based on config
            bool success = false;
            if (Config::GetSingleton().useSimpleHandBodyCreation) {
                spdlog::info("[HELDBODY] Using SIMPLE hand body creation (CreateInstance approach)");
                success = CreateHandBodySimple(handBody, isLeft, resolvedHandPos, hknpWorld, bhkWorld);
            } else {
                spdlog::info("[HELDBODY] Using FULL hand body creation (manual hknpPhysicsSystem)");
                success = CreateHandBody(handBody, isLeft, resolvedHandPos, hknpWorld, bhkWorld);
            }
            
            if (!success) {
                spdlog::error("[HELDBODY] StartGrab: Failed to create hand body");
                return false;
            }
            spdlog::info("[HELDBODY] Hand body creation returned SUCCESS (success={})", success);
            spdlog::default_logger()->flush();
            spdlog::info("[HELDBODY] After flush, handBody.valid={}, handBody.bodyId=0x{:08X}", 
                         handBody.valid, handBody.bodyId);
            spdlog::default_logger()->flush();
        }
        
        spdlog::info("[HELDBODY] About to get object body ID...");
        spdlog::default_logger()->flush();
        
        // Get object body ID from collision object
        std::uint32_t objectBodyId = 0x7FFFFFFF;
        spdlog::info("[HELDBODY] Getting object body ID from collision object...");
        
        if (collisionObject && collisionObject->spSystem) {
            // Get the actual body ID using bhkPhysicsSystem::GetBodyId
            std::uint32_t systemIndex = collisionObject->systemBodyIdx;
            void* physicsSystem = collisionObject->spSystem.get();
            
            spdlog::info("[HELDBODY] physicsSystem={:p}, systemIdx={}", physicsSystem, systemIndex);
            
            if (physicsSystem) {
                ConstraintFunctions::BhkPhysicsSystemGetBodyId(physicsSystem, &objectBodyId, systemIndex);
            }
            
            // Fallback to systemIndex if GetBodyId fails
            if (objectBodyId == 0x7FFFFFFF) {
                objectBodyId = systemIndex;
            }
            
            spdlog::info("[HELDBODY] StartGrab: Object body ID = 0x{:08X} (systemIdx={})", 
                         objectBodyId, systemIndex);
        } else {
            spdlog::warn("[HELDBODY] collisionObject={:p}, spSystem valid={}", 
                         (void*)collisionObject, 
                         collisionObject ? (collisionObject->spSystem ? true : false) : false);
        }
        
        if (objectBodyId == 0x7FFFFFFF) {
            spdlog::error("[HELDBODY] StartGrab: Could not get object body ID");
            return false;
        }

        // Task #1 — DYNAMIC guard. The HIGGS-style motor drives a DYNAMIC rigid
        // body. If the object was left KEYFRAMED (e.g. a previous fallback grab
        // ended without restoring) the solver will silently do nothing and the
        // grab will feel dead. Force DYNAMIC before wiring the constraint.
        bool forcedDynamic = false;
        if (Config::GetSingleton().grabConstraintForceDynamicHeldBody && collisionObject) {
            if (SafeSetMotionType(collisionObject, MOTION_DYNAMIC)) {
                forcedDynamic = true;
                spdlog::info("[HELDBODY] StartGrab: Forced object 0x{:08X} to DYNAMIC motion type",
                             objectBodyId);
            } else {
                spdlog::warn("[HELDBODY] StartGrab: SafeSetMotionType(DYNAMIC) threw — object 0x{:08X}",
                             objectBodyId);
            }
        }

        if (!CreateGrabConstraint(state, isLeft, objectBodyId, resolvedHandPos, resolvedHandRot)) {
            spdlog::error("[HELDBODY] StartGrab: Failed to create HeldBody constraint");
            return false;
        }

        HeldBodyGrabConstraint& constraint = isLeft ? _leftConstraint : _rightConstraint;
        constraint.objectCollisionObject = collisionObject;
        constraint.forcedObjectDynamic = forcedDynamic;

        // ROCK parity: sample the real object mass for the motor force cap, and clamp the
        // inverse-inertia ratio so long/thin held objects don't spin wildly under the motor.
        // Done AFTER forcing DYNAMIC (mass/inertia are only meaningful on a dynamic motion).
        constraint.cachedObjectMass = heisenberg::Physics::GetHeldObjectMass(collisionObject);
        constraint.inertiaModified = heisenberg::Physics::NormalizeHeldObjectInertia(
            collisionObject, kHeldBodyMaxInertiaRatio, constraint.savedInertia);
        // Snapshot the object's collision filter so EndGrab can guarantee it's restored
        // (prevents a dropped object being left non-collidable → falls through geometry).
        constraint.objectFilterSaved = false;
        if (hknpWorld && objectBodyId != 0x7FFFFFFF) {
            std::uint32_t curFilter = 0;
            if (heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objectBodyId, curFilter)) {
                constraint.objectOriginalFilter = curFilter;
                constraint.objectFilterSaved = true;
            }
            spdlog::info("[HELDBODY] StartGrab: object filter=0x{:08X} (layer {})", curFilter, curFilter & 0x7F);
        }
        spdlog::info("[HELDBODY] StartGrab: object mass={:.2f}kg, inertiaNormalized={}",
                     constraint.cachedObjectMass, constraint.inertiaModified);

        // ROCK item 2: zero the object's velocity at grab onset so the motor starts from
        // rest instead of fighting whatever momentum the object had.
        if (hknpWorld && objectBodyId != 0x7FFFFFFF) {
            RE::NiPoint4 zero4(0.0f, 0.0f, 0.0f, 0.0f);
            SafeSetBodyVelocity(hknpWorld, objectBodyId, zero4, zero4);
        }

        // Mark grab state
        state.usingHeldBodyGrab = true;
        state.handBodyId = handBody.bodyId;
        state.heldBodyGrabTime = 0.0;
        state.heldBodyConstraintActive = true;
        state.constraintId = constraint.constraintId.m_value;

        for (int i = 0; i < MAX_DISTANCE_SAMPLES; ++i) {
            if (isLeft) {
                _leftDistSamples[i] = 0.0f;
            } else {
                _rightDistSamples[i] = 0.0f;
            }
        }
        if (isLeft) {
            _leftDistIdx = 0;
        } else {
            _rightDistIdx = 0;
        }
        _playerPosInitialized = false;

        // Register grabbed body with proxy listener
        PlayerCharacterProxyListener::GetSingleton().RegisterGrabbedBodyId(objectBodyId);

        // Task #5 — subscribe grabbed body to CONTACT_STARTED so
        // UpdateConstraintMotors can soften tau during contact, HIGGS-style.
        ContactImpulseListener::GetSingleton().SubscribeForGrabbedBody(objectBodyId, isLeft);

        // Disable collision between grabbed object and player capsule using pair filter.
        std::uint32_t playerBodyId = Physics::GetPlayerBodyId();
        if (playerBodyId != 0x7FFFFFFF && hknpWorld) {
            Physics::DisableCollisionBetween(hknpWorld, objectBodyId, playerBodyId);
            spdlog::info("[HELDBODY] Disabled player↔object collision via pair filter: player=0x{:08X}, object=0x{:08X}",
                         playerBodyId, objectBodyId);
        } else {
            spdlog::warn("[HELDBODY] Player body not found — capsule collision not suppressed. Grab.cpp will set kNonCollidable as fallback.");
        }

        const char* backendName = constraint.IsSpringMode() ? "spring" :
            (constraint.linearMotor ? "custom 6-DOF" : "native ragdoll");
        spdlog::info("[HELDBODY] StartGrab: SUCCESS ({}) - hand 0x{:08X}, object 0x{:08X}, constraint 0x{:08X}",
                     backendName, handBody.bodyId, objectBodyId, constraint.constraintId.m_value);

        return true;
    }

    void HeldBodyGrabManager::UpdateGrab(GrabState& state, bool isLeft,
                                          const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                                          float deltaTime)
    {
        if (!state.usingHeldBodyGrab || !state.heldBodyConstraintActive) {
            return;
        }

        HeldBodyHandPhysics& handBody = isLeft ? _leftHandBody : _rightHandBody;
        HeldBodyGrabConstraint& constraint = isLeft ? _leftConstraint : _rightConstraint;

        if (!constraint.IsValid()) {
            spdlog::warn("[HELDBODY] UpdateGrab: backend became invalid for {} hand", isLeft ? "LEFT" : "RIGHT");
            state.heldBodyConstraintActive = false;
            return;
        }

        // Throttled logging - only every ~60 frames
        static int s_updateGrabCount = 0;
        s_updateGrabCount++;
        bool shouldLog = (s_updateGrabCount % 60 == 1);

        RE::NiPoint3 resolvedHandPos = handPos;
        RE::NiMatrix3 resolvedHandRot = handRot;
        ResolveHeldBodyHandDriveTransform(state, isLeft, handPos, handRot, resolvedHandPos, resolvedHandRot);

        // Update hand body position (with real rotation)
        UpdateHandBodyPosition(handBody, resolvedHandPos, resolvedHandRot, deltaTime);

        // Update grab time (for tau lerp)
        state.heldBodyGrabTime += deltaTime;

        UpdateConstraintMotors(state, deltaTime);

        float distance = 0.0f;
        if (constraint.IsSpringMode()) {
            if (!UpdateHeldBodySpringDrive(state, constraint, isLeft, resolvedHandPos, resolvedHandRot, deltaTime, distance)) {
                state.heldBodyConstraintActive = false;
                return;
            }
        } else if (constraint.linearMotor && constraint.constraintData) {
            auto* grabData = static_cast<GrabConstraintData*>(constraint.constraintData);
            grabData->SetMotorsActive(true);

            // ROCK parity: refresh BOTH the angular target (ragdoll motor) AND transformB
            // (object pivot frame, which the linear motors drive toward) EVERY frame, from
            // ONE consistent target frame. The old code refreshed ONLY the angular target —
            // the linear/pivot frame stayed frozen at the grab pose, so the object settled to
            // a fixed wrong offset and didn't track the hand. ROCK's note: transformA tracks
            // the live hand, so a frozen transformB makes the linear+angular goals disagree.
            if (state.node) {
                RE::NiTransform desiredPhysicsTransform;
                RE::NiAVObject* syncNode = nullptr;
                RE::NiMatrix3 nativeFrameB;
                RE::NiPoint3 nativePivotB;
                if (ComputeNativeRagdollConstraintTargetFromParent(state, resolvedHandPos, resolvedHandRot,
                                                                   desiredPhysicsTransform, syncNode,
                                                                   nativeFrameB, nativePivotB)) {
                    // transformB: object pivot frame (rotation + translation) — drives linear motors.
                    UpdateConstraintTransformB(constraint.constraintData, nativeFrameB, nativePivotB);

                    // Angular ragdoll-motor target from the SAME frame (column-major for Havok).
                    alignas(16) float bRa[12];
                    bRa[0]  = nativeFrameB.entry[0][0]; bRa[1]  = nativeFrameB.entry[1][0]; bRa[2]  = nativeFrameB.entry[2][0]; bRa[3]  = 0.0f;
                    bRa[4]  = nativeFrameB.entry[0][1]; bRa[5]  = nativeFrameB.entry[1][1]; bRa[6]  = nativeFrameB.entry[2][1]; bRa[7]  = 0.0f;
                    bRa[8]  = nativeFrameB.entry[0][2]; bRa[9]  = nativeFrameB.entry[1][2]; bRa[10] = nativeFrameB.entry[2][2]; bRa[11] = 0.0f;
                    grabData->setTargetRelativeOrientationOfBodies(bRa);

                    // RESIDUAL VELOCITY DAMPING (ROCK item 1 + 3, hand-referenced).
                    // ROCK damps velocity MINUS the player/room velocity so it bleeds the
                    // orbital overshoot without fighting locomotion. We reference the HAND
                    // velocity instead (the hand already carries player motion + arm swing,
                    // and is exactly what the object should follow): keep the hand-following
                    // velocity in full, damp only the part BEYOND it (the orbit). So a perfect
                    // tracker (objVel == handVel) is untouched, while overshoot decays. This is
                    // what stops the "swirl on a rope" WITHOUT the tracking loss the naive
                    // full-velocity damp caused. Linear only — angular is left to the motor.
                    if (constraint.hknpWorld && constraint.objectBodyId != 0x7FFFFFFF && constraint.hasLastHandPos) {
                        RE::NiPoint3 linH, angH;
                        if (heisenberg::Physics::GetHeldObjectVelocity(
                                static_cast<RE::bhkNPCollisionObject*>(constraint.objectCollisionObject), linH, angH)) {
                            const float dt = (deltaTime > 0.0001f) ? deltaTime : (1.0f / 90.0f);
                            const float k = HAVOK_WORLD_SCALE / dt;
                            // Hand velocity in HAVOK units.
                            const RE::NiPoint3 handVelH{
                                (resolvedHandPos.x - constraint.lastHandPos.x) * k,
                                (resolvedHandPos.y - constraint.lastHandPos.y) * k,
                                (resolvedHandPos.z - constraint.lastHandPos.z) * k };
                            const float keep = 1.0f - kHeldBodyVelocityDamping;  // 0.75
                            RE::NiPoint4 lin4(
                                handVelH.x + (linH.x - handVelH.x) * keep,
                                handVelH.y + (linH.y - handVelH.y) * keep,
                                handVelH.z + (linH.z - handVelH.z) * keep, 0.0f);
                            SafeSetBodyLinearVelocity(constraint.hknpWorld, constraint.objectBodyId, lin4);
                        }
                    }
                    constraint.lastHandPos = resolvedHandPos;
                    constraint.hasLastHandPos = true;

                    // DIAGNOSTIC every ~60 frames: is the object tracking the hand or stuck?
                    static int dr_diag = 0;
                    if (++dr_diag >= 60) {
                        dr_diag = 0;
                        const RE::NiPoint3 objPos = state.node->world.translate;
                        const float gap = (objPos - desiredPhysicsTransform.translate).Length();
                        spdlog::info("[HELDBODY-DIAG] obj=({:.1f},{:.1f},{:.1f}) target=({:.1f},{:.1f},{:.1f}) hand=({:.1f},{:.1f},{:.1f}) gap={:.1f}u",
                                     objPos.x, objPos.y, objPos.z,
                                     desiredPhysicsTransform.translate.x, desiredPhysicsTransform.translate.y, desiredPhysicsTransform.translate.z,
                                     resolvedHandPos.x, resolvedHandPos.y, resolvedHandPos.z, gap);
                    }
                }
            }
        } else if (constraint.objectBodyId != 0x7FFFFFFF && state.node) {
            RE::NiTransform desiredPhysicsTransform;
            RE::NiAVObject* syncNode = nullptr;
            RE::NiMatrix3 nativeFrameB;
            RE::NiPoint3 nativePivotB;
            if (!ComputeNativeRagdollConstraintTargetFromParent(state, resolvedHandPos, resolvedHandRot,
                                                                desiredPhysicsTransform, syncNode,
                                                                nativeFrameB, nativePivotB)) {
                return;
            }

            UpdateConstraintTransformB(constraint.constraintData, nativeFrameB, nativePivotB);
        }

        if (!constraint.IsSpringMode() && !constraint.linearMotor && state.node) {
            RE::NiTransform desiredPhysicsTransform;
            RE::NiAVObject* syncNode = nullptr;
            RE::NiMatrix3 nativeFrameB;
            RE::NiPoint3 nativePivotB;
            if (ComputeNativeRagdollConstraintTargetFromParent(state, resolvedHandPos, resolvedHandRot,
                                                               desiredPhysicsTransform, syncNode,
                                                               nativeFrameB, nativePivotB)) {
                RE::NiAVObject* distanceNode = syncNode ? syncNode : state.node.get();
                distance = (distanceNode->world.translate - desiredPhysicsTransform.translate).Length();
            }
        } else if (!constraint.IsSpringMode() && state.node) {
            RE::NiTransform desiredPhysicsTransform;
            RE::NiAVObject* syncNode = nullptr;
            RE::NiMatrix3 nativeFrameB;
            RE::NiPoint3 nativePivotB;
            if (ComputeNativeRagdollConstraintTargetFromParent(state, resolvedHandPos, resolvedHandRot,
                                                               desiredPhysicsTransform, syncNode,
                                                               nativeFrameB, nativePivotB)) {
                RE::NiAVObject* distanceNode = syncNode ? syncNode : state.node.get();
                distance = (distanceNode->world.translate - desiredPhysicsTransform.translate).Length();
            }
        }

        // Keep the TESObjectREFR location aligned with the currently rendered node.
        // Without this, the game can render/cull the grabbed ref at its stale location
        // while the scene graph node is moving with HeldBody, producing a ghost copy.
        if (RE::TESObjectREFR* stateRefr = state.GetRefr()) {
            RE::NiAVObject* visualNode = state.node ? state.node.get() : nullptr;
            if (!visualNode) {
                visualNode = constraint.objectBodyId != 0x7FFFFFFF ? (state.physicsNode ? state.physicsNode.get() : nullptr) : nullptr;
            }
            if (visualNode) {
                stateRefr->data.location.x = visualNode->world.translate.x;
                stateRefr->data.location.y = visualNode->world.translate.y;
                stateRefr->data.location.z = visualNode->world.translate.z;
            }
        }

        float* samples = isLeft ? _leftDistSamples : _rightDistSamples;
        int& idx = isLeft ? _leftDistIdx : _rightDistIdx;
        samples[idx % MAX_DISTANCE_SAMPLES] = distance;
        idx++;
        if (idx >= MAX_DISTANCE_SAMPLES) {
            float avg = 0.0f;
            for (int i = 0; i < MAX_DISTANCE_SAMPLES; ++i) {
                avg += samples[i];
            }
            avg /= MAX_DISTANCE_SAMPLES;
            if (avg > 200.0f && state.heldBodyGrabTime > 1.0f) {
                spdlog::warn("[HELDBODY] Auto-drop: avg distance {:.1f} > 200 for {} hand",
                             avg, isLeft ? "LEFT" : "RIGHT");
                state.heldBodyConstraintActive = false;
                return;
            }
        }

        if (shouldLog) {
            spdlog::info("[HELDBODY] {}: dist={:.1f}, angularTau={:.2f}, linearTau={:.2f}, constraint=0x{:08X}",
                         constraint.IsSpringMode() ? "Spring" : "Constraint",
                         distance,
                         state.currentAngularTau,
                         state.currentLinearTau,
                         constraint.constraintId.m_value);
        }
    }

    void HeldBodyGrabManager::EndGrab(GrabState& state, bool isLeft, const RE::NiPoint3* throwVelocity)
    {
        spdlog::info("[HELDBODY] EndGrab: {} hand releasing", isLeft ? "LEFT" : "RIGHT");

        if (!state.usingHeldBodyGrab) {
            return;
        }

        // Get constraint info before destroying (need objectBodyId and world)
        HeldBodyGrabConstraint& constraint = isLeft ? _leftConstraint : _rightConstraint;
        std::uint32_t objectBodyId = constraint.objectBodyId;
        void* hknpWorld = constraint.hknpWorld;

        // Unregister grabbed body from proxy listener
        if (objectBodyId != 0x7FFFFFFF) {
            PlayerCharacterProxyListener::GetSingleton().UnregisterGrabbedBodyId(objectBodyId);
        }

        // Task #5 — drop the motor-soften subscription so stray callbacks
        // can't reach a stale object.
        ContactImpulseListener::GetSingleton().UnsubscribeGrabbedBody(isLeft);

        // ROCK parity: restore the object's original packed inertia (undo the grab-time
        // ratio clamp) BEFORE tearing down, while the body/motion is still resolvable.
        if (constraint.inertiaModified && constraint.objectCollisionObject) {
            heisenberg::Physics::RestoreHeldObjectInertia(
                static_cast<RE::bhkNPCollisionObject*>(constraint.objectCollisionObject),
                constraint.savedInertia);
        }

        // Guarantee the dropped object is collidable again. Log the current filter first so
        // we can see whether the grab left it non-collidable (cause of falling through floors),
        // then restore the snapshot taken at StartGrab. Also undo the player-pair disable.
        if (hknpWorld && objectBodyId != 0x7FFFFFFF) {
            std::uint32_t curFilter = 0;
            if (heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, objectBodyId, curFilter)) {
                spdlog::info("[HELDBODY] EndGrab: object filter at release=0x{:08X} (layer {}), saved=0x{:08X}",
                             curFilter, curFilter & 0x7F, constraint.objectOriginalFilter);
            }
            if (constraint.objectFilterSaved) {
                heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, objectBodyId, constraint.objectOriginalFilter);
            }
            std::uint32_t playerBodyId = Physics::GetPlayerBodyId();
            if (playerBodyId != 0x7FFFFFFF) {
                Physics::EnableCollisionBetween(hknpWorld, objectBodyId, playerBodyId);
            }
        }

        // Destroy constraint and clean up struct
        DestroyGrabConstraint(state);
        constraint.Invalidate();

        // ROCK item 4: proper release velocity. Clamp the throw so a swirl/overshoot can't
        // fling the object through walls; on a gentle drop, ZERO the velocity so leftover
        // constraint/orbital momentum doesn't carry it through the floor (the "clips through
        // when dropped" bug).
        if (hknpWorld && objectBodyId != 0x7FFFFFFF) {
            float velMag = throwVelocity ? sqrtf(throwVelocity->x * throwVelocity->x +
                                                 throwVelocity->y * throwVelocity->y +
                                                 throwVelocity->z * throwVelocity->z)
                                         : 0.0f;
            if (throwVelocity && velMag > 0.1f) {
                // Clamp to max throw speed.
                RE::NiPoint3 v = *throwVelocity;
                if (velMag > kHeldBodyMaxThrowSpeed) {
                    const float s = kHeldBodyMaxThrowSpeed / velMag;
                    v.x *= s; v.y *= s; v.z *= s;
                    velMag = kHeldBodyMaxThrowSpeed;
                }
                alignas(16) RE::NiPoint4 hkVel(v.x * HAVOK_WORLD_SCALE, v.y * HAVOK_WORLD_SCALE, v.z * HAVOK_WORLD_SCALE, 0.0f);
                if (SafeSetBodyLinearVelocity(hknpWorld, objectBodyId, hkVel)) {
                    spdlog::info("[HELDBODY] EndGrab: throw velocity ({:.1f},{:.1f},{:.1f}) mag={:.1f}", v.x, v.y, v.z, velMag);
                }
                RE::TESObjectREFR* thrownRefr = state.GetRefr();
                if (thrownRefr) {
                    ThrownObjectTracker::GetSingleton().OnThrown(thrownRefr, objectBodyId, v);
                }
            } else {
                // Gentle drop: kill residual linear + angular velocity so it settles in place.
                RE::NiPoint4 zero4(0.0f, 0.0f, 0.0f, 0.0f);
                SafeSetBodyVelocity(hknpWorld, objectBodyId, zero4, zero4);
            }
        }

        // Clear HeldBody state
        state.usingHeldBodyGrab = false;
        state.handBodyId = 0x7FFFFFFF;
        state.constraintId = 0x7FFFFFFF;
        state.heldBodyConstraintActive = false;

        // Note: We keep the hand body alive for the next grab
    }

    bool HeldBodyGrabManager::IsGrabActive(bool isLeft) const
    {
        const HeldBodyGrabConstraint& constraint = isLeft ? _leftConstraint : _rightConstraint;
        return constraint.IsValid();
    }

    const HeldBodyHandPhysics& HeldBodyGrabManager::GetHandBody(bool isLeft) const
    {
        return isLeft ? _leftHandBody : _rightHandBody;
    }

    // =========================================================================
    // Constraint Management
    // =========================================================================

    bool HeldBodyGrabManager::CreateGrabConstraint(GrabState& state, bool isLeft,
                                                    std::uint32_t objectBodyId,
                                                    const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot)
    {
        HeldBodyHandPhysics& handBody = isLeft ? _leftHandBody : _rightHandBody;
        HeldBodyGrabConstraint& constraint = isLeft ? _leftConstraint : _rightConstraint;
        
        if (!handBody.IsValid()) {
            spdlog::error("[HELDBODY] CreateGrabConstraint: Hand body not valid");
            return false;
        }
        
        void* hknpWorld = handBody.hknpWorld;
        auto& config = Config::GetSingleton();

        // Keep the anchor body aligned with the controller before we compute
        // body-space pivots and frames for the new constraint.
        // StartGrab already passes the drive transform that includes HeldBody's
        // locomotion compensation. Re-resolving here can strip that adjustment
        // back to the raw FRIK parent transform and skew the initial target.
        RE::NiPoint3 resolvedHandPos = handPos;
        RE::NiMatrix3 resolvedHandRot = handRot;
        UpdateHandBodyPosition(handBody, resolvedHandPos, resolvedHandRot, 1.0f / 90.0f);

        RE::NiPoint3 pivotA(0.0f, 0.0f, 0.0f);  // Hand body center
        RE::NiTransform desiredPhysicsTransform;
        RE::NiAVObject* syncNode = nullptr;
        RE::NiMatrix3 frameB;
        RE::NiPoint3 pivotB;
        if (!ComputeNativeRagdollConstraintTargetFromParent(state, resolvedHandPos, resolvedHandRot,
                                                            desiredPhysicsTransform, syncNode,
                                                            frameB, pivotB)) {
            spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to compute native ragdoll target");
            return false;
        }

        RE::NiMatrix3 frameA{};
        frameA.entry[0][0] = 1.0f;
        frameA.entry[0][1] = 0.0f;
        frameA.entry[0][2] = 0.0f;
        frameA.entry[1][0] = 0.0f;
        frameA.entry[1][1] = 1.0f;
        frameA.entry[1][2] = 0.0f;
        frameA.entry[2][0] = 0.0f;
        frameA.entry[2][1] = 0.0f;
        frameA.entry[2][2] = 1.0f;

        spdlog::debug("[HELDBODY] Constraint pivots: A=({:.2f},{:.2f},{:.2f}), B=({:.2f},{:.2f},{:.2f})",
                     pivotA.x, pivotA.y, pivotA.z, pivotB.x, pivotB.y, pivotB.z);

        RE::NiTransform transformA;
        transformA.rotate = frameA;
        transformA.translate = pivotA;
        transformA.scale = 1.0f;

        RE::NiTransform transformB;
        transformB.rotate = frameB;
        transformB.translate = pivotB;
        transformB.scale = 1.0f;

        if (config.UseHeldBodySpringMode()) {
            spdlog::info("[HELDBODY] Using dynamic spring backend");
            constraint.constraintId.Invalidate();
            constraint.handBodyId = handBody.bodyId;
            constraint.objectBodyId = objectBodyId;
            constraint.hknpWorld = hknpWorld;
            constraint.driveMode = HeldBodyDriveMode::Spring;
            constraint.active = true;
            constraint.constraintData = nullptr;
            constraint.angularMotor = nullptr;
            constraint.linearMotor = nullptr;
            state.constraintId = 0x7FFFFFFF;
            return true;
        }

        if (config.UseHeldBodyNativeConstraint()) {
            spdlog::info("[HELDBODY] Using native ragdoll constraint");

            auto* angularMotor = CreateHavokPositionMotor(
                config.grabConstraintAngularTau,
                config.grabConstraintAngularDamping,
                config.grabConstraintAngularMaxForce,
                config.grabConstraintAngularProportionalRecoveryVelocity,
                config.grabConstraintAngularConstantRecoveryVelocity);
            if (!angularMotor) {
                spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to allocate native angular motor");
                return false;
            }

            void* ragdollData = CreateRagdollConstraintDataLocal(
                pivotA,
                pivotB,
                frameA,
                frameB,
                angularMotor);
            if (!ragdollData) {
                spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to allocate native ragdoll constraint data");
                ConstraintFunctions::hkReferencedObject_removeReference(angularMotor);
                return false;
            }

            // The ragdoll constraint now owns three references, one per motor axis.
            // Drop the creator reference so cleanup only needs to release the slot refs.
            ConstraintFunctions::hkReferencedObject_removeReference(angularMotor);

            spdlog::debug("[HELDBODY] Native ragdoll motor: angular tau={:.3f}", angularMotor->tau);
            
            hknpConstraintCinfo cinfo{};
            cinfo.bodyIdA = handBody.bodyId;
            cinfo.bodyIdB = objectBodyId;
            cinfo.constraintData = ragdollData;
            
            spdlog::info("[HELDBODY] About to create native ragdoll constraint: bodyA=0x{:08X}, bodyB=0x{:08X}",
                         handBody.bodyId, objectBodyId);
            spdlog::default_logger()->flush();
            
            hknpConstraintId constraintId;
            constraintId.m_value = 0x7FFFFFFF;
            
            bool createSuccess = SafeCreateConstraint(hknpWorld, &constraintId, &cinfo);
            
            if (!createSuccess) {
                spdlog::error("[HELDBODY] CRASH in CreateConstraint! Exception caught.");
                ReleaseNativeRagdollMotorRefs(angularMotor);
                _aligned_free(ragdollData);
                return false;
            }
            
            spdlog::info("[HELDBODY] CreateConstraint returned, constraintId=0x{:08X}, valid={}",
                         constraintId.m_value, constraintId.IsValid());
            
            if (constraintId.IsValid()) {
                ConstraintFunctions::AddConstraintBodyMap(hknpWorld, constraintId, &cinfo);
                spdlog::info("[HELDBODY] AddConstraintBodyMap completed");
            }
            
            if (!constraintId.IsValid()) {
                spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to create constraint");
                ReleaseNativeRagdollMotorRefs(angularMotor);
                _aligned_free(ragdollData);
                return false;
            }

            void* runtime = GetConstraintRuntime(hknpWorld, constraintId);
            if (runtime) {
                if (!SafeRagdollSetMotorsEnabled(ragdollData, runtime, true)) {
                    spdlog::warn("[HELDBODY] Failed to enable native ragdoll motors at runtime");
                } else {
                    spdlog::debug("[HELDBODY] Native ragdoll motors enabled (runtime={:p})", runtime);
                }
            } else {
                spdlog::warn("[HELDBODY] Native ragdoll constraint runtime was null after creation");
            }
            
            constraint.constraintId = constraintId;
            constraint.handBodyId = handBody.bodyId;
            constraint.objectBodyId = objectBodyId;
            constraint.hknpWorld = hknpWorld;
            constraint.driveMode = HeldBodyDriveMode::NativeConstraint;
            constraint.active = true;
            constraint.constraintData = ragdollData;
            constraint.angularMotor = angularMotor;
            constraint.linearMotor = nullptr;
            
            state.constraintId = constraintId.m_value;
            
            spdlog::info("[HELDBODY] CreateGrabConstraint: Created native ragdoll constraint ID 0x{:08X}",
                         constraintId.m_value);
            
            return true;
        }

        spdlog::info("[HELDBODY] Using custom 6-DOF constraint");

        auto* angularMotor = CreateCustomPositionMotor(
            config.grabConstraintAngularTau,
            config.grabConstraintAngularDamping,
            config.grabConstraintAngularMaxForce,
            config.grabConstraintAngularProportionalRecoveryVelocity,
            config.grabConstraintAngularConstantRecoveryVelocity);
        if (!angularMotor) {
            spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to allocate custom angular motor");
            return false;
        }

        auto* linearMotor = CreateCustomPositionMotor(
            config.grabConstraintLinearTau,
            config.grabConstraintLinearDamping,
            config.grabConstraintLinearMaxForce,
            config.grabConstraintLinearProportionalRecoveryVelocity,
            config.grabConstraintLinearConstantRecoveryVelocity);
        if (!linearMotor) {
            spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to allocate custom linear motor");
            delete angularMotor;
            return false;
        }

        auto* grabData = CreateGrabConstraintDataLocal(transformA, transformB, angularMotor, linearMotor);
        if (!grabData) {
            spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to allocate custom 6-DOF constraint data");
            delete angularMotor;
            delete linearMotor;
            return false;
        }

        hknpConstraintCinfo cinfo{};
        cinfo.bodyIdA = handBody.bodyId;
        cinfo.bodyIdB = objectBodyId;
        cinfo.constraintData = grabData;
        cinfo.flags = 1;  // Bit 0 = wantRuntime: forces hknp to allocate constraint runtime buffer

        spdlog::info("[HELDBODY] About to create custom 6-DOF constraint: bodyA=0x{:08X}, bodyB=0x{:08X}",
                     handBody.bodyId, objectBodyId);
        spdlog::default_logger()->flush();

        hknpConstraintId constraintId;
        constraintId.m_value = 0x7FFFFFFF;

        bool createSuccess = SafeCreateConstraint(hknpWorld, &constraintId, &cinfo);
        if (!createSuccess) {
            spdlog::error("[HELDBODY] CRASH in custom 6-DOF CreateConstraint! Exception caught.");
            DestroyGrabConstraintDataLocal(grabData);
            delete angularMotor;
            delete linearMotor;
            return false;
        }

        spdlog::info("[HELDBODY] CreateConstraint returned, constraintId=0x{:08X}, valid={}",
                     constraintId.m_value, constraintId.IsValid());

        if (constraintId.IsValid()) {
            ConstraintFunctions::AddConstraintBodyMap(hknpWorld, constraintId, &cinfo);
            spdlog::info("[HELDBODY] AddConstraintBodyMap completed for custom 6-DOF constraint");
        }

        if (!constraintId.IsValid()) {
            spdlog::error("[HELDBODY] CreateGrabConstraint: Failed to create custom 6-DOF constraint");
            DestroyGrabConstraintDataLocal(grabData);
            delete angularMotor;
            delete linearMotor;
            return false;
        }

        constraint.constraintId = constraintId;
        constraint.handBodyId = handBody.bodyId;
        constraint.objectBodyId = objectBodyId;
        constraint.hknpWorld = hknpWorld;
        constraint.driveMode = HeldBodyDriveMode::Custom6DOF;
        constraint.active = true;
        constraint.constraintData = grabData;
        constraint.angularMotor = angularMotor;
        constraint.linearMotor = linearMotor;

        state.constraintId = constraintId.m_value;

        // =====================================================================
        // SNAP PIVOTS TO ACTUAL BODY POSITIONS
        // The computed pivots may not match where the bodies actually are right now
        // (due to pull animation, physics settling, etc.). Overwrite transformB
        // with the hand's actual position in the object's actual local space.
        // This anchors the constraint at exactly where the object currently sits.
        // =====================================================================
        if (state.node) {
            RE::NiPoint3 objectWorldPos = state.node->world.translate;
            RE::NiMatrix3 objectWorldRot = state.node->world.rotate;
            float objectScale = state.node->world.scale > 0.0f ? state.node->world.scale : 1.0f;

            // Hand position in object's local space
            float invScale = (objectScale != 0.0f) ? (1.0f / objectScale) : 1.0f;
            RE::NiPoint3 actualPivotB = (objectWorldRot.Transpose() * (handPos - objectWorldPos)) * invScale;

            // Hand rotation in object's local space
            RE::NiMatrix3 actualFrameB = handRot * objectWorldRot.Transpose();

            UpdateConstraintTransformB(grabData, actualFrameB, actualPivotB);

            spdlog::info("[HELDBODY] Snapped pivotB to actual positions: B=({:.2f},{:.2f},{:.2f})",
                         actualPivotB.x, actualPivotB.y, actualPivotB.z);
        }

        spdlog::info("[HELDBODY] CreateGrabConstraint: Created custom 6-DOF constraint ID 0x{:08X}",
                     constraintId.m_value);

        return true;
    }

    void HeldBodyGrabManager::DestroyGrabConstraint(GrabState& state)
    {
        // Find which hand's constraint to destroy
        HeldBodyGrabConstraint* constraint = FindHeldBodyConstraintForState(state, _leftConstraint, _rightConstraint);
        
        if (!constraint || !constraint->IsValid()) {
            return;
        }

        if (!constraint->UsesConstraint()) {
            spdlog::info("[HELDBODY] DestroyGrabConstraint: Clearing dynamic spring backend for body 0x{:08X}",
                         constraint->objectBodyId);
            constraint->Invalidate();
            return;
        }
        
        spdlog::info("[HELDBODY] DestroyGrabConstraint: Destroying HeldBody constraint ID 0x{:08X}",
                     constraint->constraintId.m_value);
        
        // Remove from Bethesda tracking
        // CRITICAL: RemoveConstraintBodyMap takes (world, 0, constraintId.m_value), NOT (world, constraintId)!
        ConstraintFunctions::RemoveConstraintBodyMap(constraint->hknpWorld, 0, constraint->constraintId.m_value);
        
        // Destroy the constraint
        hknpConstraintId ids[1] = { constraint->constraintId };
        ConstraintFunctions::DestroyConstraints(constraint->hknpWorld, ids, 1);
        
        if (constraint->linearMotor) {
            if (constraint->constraintData) {
                DestroyGrabConstraintDataLocal(static_cast<GrabConstraintData*>(constraint->constraintData));
                constraint->constraintData = nullptr;
            }
            delete constraint->angularMotor;
            delete constraint->linearMotor;
        } else {
            if (constraint->angularMotor) {
                ReleaseNativeRagdollMotorRefs(constraint->angularMotor);
            }
            if (constraint->constraintData) {
                _aligned_free(constraint->constraintData);
                constraint->constraintData = nullptr;
            }
        }

        constraint->angularMotor = nullptr;
        constraint->linearMotor = nullptr;
        
        constraint->Invalidate();
    }

    void HeldBodyGrabManager::UpdateConstraintMotors(GrabState& state, float deltaTime)
    {
        // Find constraint for this state
        HeldBodyGrabConstraint* constraint = FindHeldBodyConstraintForState(state, _leftConstraint, _rightConstraint);

        if (!constraint || !constraint->IsValid()) {
            return;
        }

        auto& config = Config::GetSingleton();

        // =====================================================================
        // HIGGS-style tau lerp: start at TauBodyStart, lerp to TauBody over
        // heldBodyTauLerpTime seconds.
        // =====================================================================
        float angularTauTarget = config.grabConstraintAngularTauBody;
        float linearTauTarget = config.grabConstraintLinearTauBody;

        // Task #13: Approach/Contact/Grip ramp. When enabled, the ramp duration
        // is driven by grabApproachRampSeconds (a separate, typically shorter knob
        // for the "feel" of grip settling) and starts from a softer tau so the
        // hand eases into the grip instead of snapping with full stiffness at
        // frame 1. The existing heldBodyTauLerpTime behaviour is retained as the
        // default when the substate feature is off.
        float rampDuration = config.heldBodyTauLerpTime;
        float angularStart = config.grabConstraintAngularTauBodyStart;
        float linearStart  = config.grabConstraintLinearTauBodyStart;
        if (config.enableGrabApproachSubstates && config.grabApproachRampSeconds > 0.0001f) {
            rampDuration = config.grabApproachRampSeconds;
            // Approach phase: start at 25% of the configured start tau so the
            // first ~third of the ramp feels like the object is being "pulled
            // in", the middle like contact, the end like full grip.
            angularStart *= 0.25f;
            linearStart  *= 0.25f;
        }

        float elapsedRatio = static_cast<float>(state.heldBodyGrabTime / rampDuration);
        if (elapsedRatio > 1.0f) elapsedRatio = 1.0f;

        float currentAngularTau = angularStart + elapsedRatio * (angularTauTarget - angularStart);
        float currentLinearTau  = linearStart  + elapsedRatio * (linearTauTarget  - linearStart);

        // =====================================================================
        // HIGGS motor-soften on collision (Task #5)
        // =====================================================================
        // When the grabbed body has just collided with the world, lerp tau
        // toward a much softer "colliding" value so the motor doesn't try to
        // pull the body through the contact. The effect decays back to the
        // normal tau as recent contact ages — kCollisionSoftWindow is the time
        // over which we blend from full soften back to zero soften.
        {
            bool isLeft = false;
            if (constraint == &_leftConstraint) {
                isLeft = true;
            } else if (constraint == &_rightConstraint) {
                isLeft = false;
            }
            const auto& listener = ContactImpulseListener::GetSingleton();
            const double timeSinceHit = listener.GetGrabbedBodyTimeSinceCollision(isLeft);
            constexpr double kCollisionSoftWindow = 0.20;  // 200 ms
            if (timeSinceHit < kCollisionSoftWindow) {
                const float softBlend = 1.0f - static_cast<float>(timeSinceHit / kCollisionSoftWindow);
                const float angularSoft = config.grabConstraintCollidingAngularTau;
                const float linearSoft  = config.grabConstraintCollidingLinearTau;
                currentAngularTau = currentAngularTau + softBlend * (angularSoft - currentAngularTau);
                currentLinearTau  = currentLinearTau  + softBlend * (linearSoft  - currentLinearTau);
            }
        }

        state.currentAngularTau = currentAngularTau;
        state.currentLinearTau = currentLinearTau;

        // =====================================================================
        // FPS-ADAPTIVE FORCE SCALING (HIGGS pattern)
        // At lower FPS, the solver runs fewer iterations per second, so each
        // iteration needs more force to achieve the same effective stiffness.
        // HIGGS multiplier maps: {72Hz: 0.7, 90Hz: 1.0, 120Hz: 1.6, 144Hz: 2.0}
        // =====================================================================
        float physicsFPS = (deltaTime > 0.001f) ? (1.0f / deltaTime) : 90.0f;

        // Linear force: base * FPS multiplier
        float linearFPSMult = GetMaxForceForFPS(physicsFPS, 0.7f, 1.0f, 1.6f, 2.0f);
        float angularFPSMult = GetMaxForceForFPS(physicsFPS, 0.5f, 1.0f, 1.375f, 1.5f);

        float linearMaxForce = config.grabConstraintLinearMaxForce * linearFPSMult;
        float angularToLinearRatio = config.grabConstraintAngularToLinearForceRatio;

        // Fade-in: during first grabConstraintFadeInTime seconds, reduce angular
        // force ratio to prevent initial rotation kick
        if (state.heldBodyGrabTime < config.grabConstraintFadeInTime) {
            float fadeRatio = static_cast<float>(state.heldBodyGrabTime / config.grabConstraintFadeInTime);
            angularToLinearRatio = config.grabConstraintFadeInStartAngularMaxForceRatio +
                                   fadeRatio * (config.grabConstraintAngularToLinearForceRatio -
                                                config.grabConstraintFadeInStartAngularMaxForceRatio);
        }

        float angularMaxForce = (config.grabConstraintLinearMaxForce * angularFPSMult) / angularToLinearRatio;

        // MASS-PROPORTIONAL FORCE (fixes heavy objects not tracking the hand).
        // The motor must scale force WITH mass so force/mass (== achievable acceleration)
        // is consistent across objects. Diagnostics showed a 2kg object tracked perfectly
        // with ~1000 force while 5/30kg objects lagged badly with the same ~2000 — because
        // the old code only ever *reduced* force (clamp-down), never raised it for heavy
        // objects. ROCK uses mass*ratio (default 500) as the force target. We target it
        // directly, clamped to a sane band (light objects not over-forced -> no jitter;
        // heavy objects not given runaway force -> no instability), then apply the FPS
        // multiplier. Uses the REAL sampled mass (5kg fallback if unread).
        if (config.grabConstraintMaxForceToMassRatio > 0.0f) {
            float objMass = (constraint && constraint->cachedObjectMass > 0.0f)
                                ? constraint->cachedObjectMass : 5.0f;
            float massForce = objMass * config.grabConstraintMaxForceToMassRatio;  // ROCK mass*500
            if (massForce < 600.0f)   massForce = 600.0f;
            if (massForce > 12000.0f) massForce = 12000.0f;
            linearMaxForce = massForce * linearFPSMult;
            angularMaxForce = linearMaxForce / angularToLinearRatio;
        }

        // Update motors
        if (constraint->angularMotor) {
            constraint->angularMotor->tau = currentAngularTau;
            constraint->angularMotor->damping = config.grabConstraintAngularDamping;
            constraint->angularMotor->maxForce = angularMaxForce;
            constraint->angularMotor->minForce = -angularMaxForce;
            constraint->angularMotor->proportionalRecoveryVelocity = config.grabConstraintAngularProportionalRecoveryVelocity;
            constraint->angularMotor->constantRecoveryVelocity = config.grabConstraintAngularConstantRecoveryVelocity;
        }
        if (constraint->linearMotor) {
            constraint->linearMotor->tau = currentLinearTau;
            constraint->linearMotor->damping = config.grabConstraintLinearDamping;
            constraint->linearMotor->maxForce = linearMaxForce;
            constraint->linearMotor->minForce = -linearMaxForce;
            constraint->linearMotor->proportionalRecoveryVelocity = config.grabConstraintLinearProportionalRecoveryVelocity;
            constraint->linearMotor->constantRecoveryVelocity = config.grabConstraintLinearConstantRecoveryVelocity;
        }
    }

}  // namespace heisenberg
