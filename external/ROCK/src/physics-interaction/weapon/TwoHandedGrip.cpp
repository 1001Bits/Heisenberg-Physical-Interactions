#include "physics-interaction/weapon/TwoHandedGrip.h"
#include "ROCKMain.h"

#include "api/ROCKProviderApi.h"
#include "physics-interaction/actor/ActorEquipmentGrab.h"
#include "physics-interaction/hand/HandSkeleton.h"
#include "physics-interaction/hand/HandVisual.h"
#include "physics-interaction/grab/GrabFinger.h"
#include "physics-interaction/hand/HandFrame.h"
#include "physics-interaction/core/RockRuntimeState.h"
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "RockConfig.h"
#include "RockUtils.h"
#include "physics-interaction/TransformMath.h"
#include "physics-interaction/weapon/WeaponAuthority.h"
#include "physics-interaction/weapon/WeaponCollision.h"
#include "physics-interaction/weapon/WeaponGeometry.h"
#include "physics-interaction/weapon/WeaponSupport.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace rock
{
    namespace
    {
        constexpr const char* PRIMARY_GRIP_TAG = "ROCK_WeaponPrimaryGrip";
        constexpr const char* PRIMARY_DETACH_TAG = "ROCK_WeaponPrimaryDetach";
        constexpr const char* SUPPORT_GRIP_TAG = "ROCK_WeaponSupportGrip";
        // Distinct tag tells the old-FRIK host seam that this is an exact rigid
        // hand-to-weapon target. The final host-side support-arm solve applies its
        // own modest visual reach allowance without moving the weapon.
        constexpr const char* REACH_LIMITED_SUPPORT_GRIP_TAG = "ROCK_WeaponSupportGripRigid";
        constexpr int GRIP_HAND_POSE_PRIORITY = 100;
        constexpr float SUPPORT_NORMAL_TWIST_FACTOR = 0.5f;

        constexpr std::array<float, 15> BARREL_WRAP_POSE = { 0.85f, 0.80f, 0.75f, 0.35f, 0.30f, 0.25f, 0.30f, 0.25f, 0.20f, 0.35f, 0.30f, 0.25f, 0.40f, 0.35f, 0.30f };
        constexpr std::array<float, 15> HANDGUARD_CLAMP_POSE = { 0.75f, 0.72f, 0.68f, 0.45f, 0.42f, 0.38f, 0.46f, 0.42f, 0.38f, 0.48f, 0.44f, 0.40f, 0.54f, 0.48f, 0.42f };
        constexpr std::array<float, 15> FOREGRIP_POSE = { 0.90f, 0.86f, 0.82f, 0.70f, 0.66f, 0.60f, 0.74f, 0.68f, 0.62f, 0.72f, 0.66f, 0.60f, 0.66f, 0.58f, 0.50f };
        constexpr std::array<float, 15> PUMP_GRIP_POSE = { 0.82f, 0.78f, 0.72f, 0.58f, 0.54f, 0.48f, 0.60f, 0.56f, 0.50f, 0.62f, 0.56f, 0.50f, 0.58f, 0.50f, 0.44f };
        constexpr std::array<float, 15> MAGWELL_HOLD_POSE = { 0.58f, 0.52f, 0.46f, 0.40f, 0.36f, 0.32f, 0.42f, 0.38f, 0.34f, 0.42f, 0.38f, 0.34f, 0.44f, 0.38f, 0.32f };
        constexpr std::array<float, 15> RECEIVER_SUPPORT_POSE = { 0.46f, 0.40f, 0.34f, 0.34f, 0.30f, 0.26f, 0.36f, 0.32f, 0.28f, 0.36f, 0.32f, 0.28f, 0.36f, 0.30f, 0.24f };

        const std::array<float, 15>& poseValuesForGrip(WeaponGripPoseId poseId)
        {
            switch (poseId) {
            case WeaponGripPoseId::HandguardClamp:
                return HANDGUARD_CLAMP_POSE;
            case WeaponGripPoseId::VerticalForegrip:
            case WeaponGripPoseId::AngledForegrip:
                return FOREGRIP_POSE;
            case WeaponGripPoseId::PumpGrip:
                return PUMP_GRIP_POSE;
            case WeaponGripPoseId::MagwellHold:
                return MAGWELL_HOLD_POSE;
            case WeaponGripPoseId::ReceiverSupport:
                return RECEIVER_SUPPORT_POSE;
            case WeaponGripPoseId::BarrelWrap:
            case WeaponGripPoseId::None:
            default:
                return BARREL_WRAP_POSE;
            }
        }

        /*
         * The part-carry two-anchor solve feeds its own rotation back as the
         * next frame's base, chaining several float matrix products per frame.
         * Without re-orthonormalization the rotation's row norms decay and the
         * matrix acquires shear, which visibly stretches the weapon mesh and
         * collapses the grip geometry (telemetry: rigid grip separation decayed
         * ~0.05% per frame). Rows are the stored local axes.
         */
        RE::NiMatrix3 orthonormalizeStoredRotation(const RE::NiMatrix3& rotation)
        {
            const RE::NiPoint3 row0{ rotation.entry[0][0], rotation.entry[0][1], rotation.entry[0][2] };
            const RE::NiPoint3 row1{ rotation.entry[1][0], rotation.entry[1][1], rotation.entry[1][2] };

            const RE::NiPoint3 axis0 = weaponSolverNormalize(row0);
            RE::NiPoint3 axis2 = weaponSolverCross(axis0, row1);
            axis2 = weaponSolverNormalize(axis2);
            const RE::NiPoint3 axis1 = weaponSolverCross(axis2, axis0);

            RE::NiMatrix3 result = rotation;
            result.entry[0][0] = axis0.x;
            result.entry[0][1] = axis0.y;
            result.entry[0][2] = axis0.z;
            result.entry[1][0] = axis1.x;
            result.entry[1][1] = axis1.y;
            result.entry[1][2] = axis1.z;
            result.entry[2][0] = axis2.x;
            result.entry[2][1] = axis2.y;
            result.entry[2][2] = axis2.z;
            return result;
        }

        RE::NiNode* sourceRootNodeOrFallback(RE::NiAVObject* sourceRoot, RE::NiNode* fallback)
        {
            if (sourceRoot) {
                if (auto* sourceNode = sourceRoot->IsNode()) {
                    return sourceNode;
                }
            }
            return fallback;
        }

        // CRASH FIX (Jul 6): validate a cached weapon sub-node against the LIVE weapon node each
        // frame. The game/FRIK frees + rebuilds the first-person "Weapon" subtree (equip / ADS /
        // anim-graph reload / 1st<->3rd) WITHOUT advancing ROCK's weapon-body generation key, so a
        // cached _activeWeaponNode can dangle and the (now post-FRIK, live) authority write recurses
        // freed memory -> hard AV. Walk the LIVE subtree comparing raw POINTER VALUES only — NEVER
        // dereference `target`, which may already be freed. Depth-bounded.
        bool isPointerInLiveSubtree(const RE::NiAVObject* target, RE::NiAVObject* liveRoot, int depth = 0)
        {
            if (!target || !liveRoot) {
                return false;
            }
            if (liveRoot == target) {
                return true;
            }
            if (depth > 16) {
                return false;
            }
            if (auto* node = liveRoot->IsNode()) {
                for (const auto& child : node->children) {
                    if (child && isPointerInLiveSubtree(target, child.get(), depth + 1)) {
                        return true;
                    }
                }
            }
            return false;
        }

        RE::NiPoint3 lerpPoint(const RE::NiPoint3& from, const RE::NiPoint3& to, float alpha)
        {
            const float t = (std::max)(0.0f, (std::min)(1.0f, alpha));
            return RE::NiPoint3{ from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t };
        }

        constexpr std::uint16_t SUPPORT_THUMB_LOCAL_TRANSFORM_MASK = 0x0007;
        constexpr float MIN_THUMB_OPPOSITION_DISTANCE = 0.001f;

        bool isFiniteRotation(const RE::NiMatrix3& rotation)
        {
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if (!std::isfinite(rotation.entry[row][column])) {
                        return false;
                    }
                }
            }
            return true;
        }

        bool isFiniteTransform(const RE::NiTransform& transform)
        {
            return isFiniteRotation(transform.rotate) && std::isfinite(transform.translate.x) && std::isfinite(transform.translate.y) &&
                   std::isfinite(transform.translate.z) && std::isfinite(transform.scale);
        }

        RE::NiTransform providerSnapTransformToNi(
            const WeaponProviderPartSnapTransform& source)
        {
            RE::NiTransform result{};
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    result.rotate.entry[row][column] = source.rotate[row * 3 + column];
                }
            }
            result.translate = RE::NiPoint3{
                source.translate[0],
                source.translate[1],
                source.translate[2],
            };
            result.scale = source.scale;
            return result;
        }

        float lengthSquared(const RE::NiPoint3& value)
        {
            return value.x * value.x + value.y * value.y + value.z * value.z;
        }

        RE::NiPoint3 normalizeOrFallback(const RE::NiPoint3& value, const RE::NiPoint3& fallback)
        {
            const float valueLengthSquared = lengthSquared(value);
            if (std::isfinite(valueLengthSquared) && valueLengthSquared > 0.000001f) {
                const float invLength = 1.0f / std::sqrt(valueLengthSquared);
                return RE::NiPoint3{ value.x * invLength, value.y * invLength, value.z * invLength };
            }

            const float fallbackLengthSquared = lengthSquared(fallback);
            if (std::isfinite(fallbackLengthSquared) && fallbackLengthSquared > 0.000001f) {
                const float invLength = 1.0f / std::sqrt(fallbackLengthSquared);
                return RE::NiPoint3{ fallback.x * invLength, fallback.y * invLength, fallback.z * invLength };
            }

            return RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
        }

        bool remapFingerSnapshotToHandFrame(
            const root_flattened_finger_skeleton_runtime::Snapshot& source,
            const RE::NiTransform& sourceHandWorld,
            const RE::NiTransform& targetHandWorld,
            root_flattened_finger_skeleton_runtime::Snapshot& out)
        {
            if (!source.valid || !isFiniteTransform(sourceHandWorld) || !isFiniteTransform(targetHandWorld) ||
                std::abs(sourceHandWorld.scale) <= 0.000001f) {
                return false;
            }

            out = source;
            for (auto& finger : out.fingers) {
                if (!finger.valid) {
                    continue;
                }
                for (auto& point : finger.points) {
                    const RE::NiPoint3 handLocal = transform_math::worldPointToLocal(sourceHandWorld, point);
                    point = transform_math::localPointToWorld(targetHandWorld, handLocal);
                }
            }
            if (out.palmNormalValid) {
                const RE::NiPoint3 handLocalNormal = transform_math::worldVectorToLocal(sourceHandWorld, source.palmNormalWorld);
                out.palmNormalWorld = normalizeOrFallback(
                    transform_math::localVectorToWorld(targetHandWorld, handLocalNormal),
                    source.palmNormalWorld);
            }
            return true;
        }

        struct LiveThumbTransform
        {
            RE::NiTransform world{};
            RE::NiTransform parentWorld{};
            RE::NiTransform local{};
            bool valid = false;
        };

        DirectSkeletonBoneReader& rootFlattenedTwoHandedReader()
        {
            static DirectSkeletonBoneReader reader;
            return reader;
        }

        const DirectSkeletonBoneEntry* findSnapshotBoneByTreeIndex(const DirectSkeletonBoneSnapshot& snapshot, int treeIndex)
        {
            if (treeIndex < 0) {
                return nullptr;
            }

            for (const auto& bone : snapshot.bones) {
                if (bone.treeIndex == treeIndex) {
                    return &bone;
                }
            }
            return nullptr;
        }

        // FIX B (power-armor jitter, bone-lookup disambiguation): a bare name-only scan
        // has zero protection against seating on a wrong-topology node that merely shares
        // a bone name. DirectSkeletonBoneReader.cpp's Jul-19 "PA-DIAG" note documents this
        // exact class of failure -- a name-only lookup silently winning against the wrong
        // node while the player is in power armor, producing "random points on the gun."
        // When expectedParentName is supplied, scope the match to the correct sub-hierarchy
        // by preferring the first candidate whose immediate parent (via the snapshot's own
        // parentTreeIndex chain, already captured alongside inPowerArmor) is that name;
        // fall back to a bare first match only if no candidate satisfies it, so a capture
        // still succeeds rather than failing outright. expectedParentName defaults to null
        // so every other existing caller (e.g. the thumb chain lookups below) keeps its
        // original unscoped first-match behavior unchanged.
        const DirectSkeletonBoneEntry* findSnapshotBone(
            const DirectSkeletonBoneSnapshot& snapshot,
            std::string_view name,
            const char* expectedParentName = nullptr)
        {
            const DirectSkeletonBoneEntry* firstMatch = nullptr;
            for (const auto& bone : snapshot.bones) {
                if (bone.name != name) {
                    continue;
                }
                if (!firstMatch) {
                    firstMatch = &bone;
                }
                if (!expectedParentName) {
                    return &bone;
                }
                const auto* parent = findSnapshotBoneByTreeIndex(snapshot, bone.parentTreeIndex);
                if (parent && parent->name == expectedParentName) {
                    return &bone;
                }
            }
            return firstMatch;
        }

        bool resolveLiveThumbTransforms(bool isLeft, std::array<LiveThumbTransform, 3>& outNodes)
        {
            outNodes = {};

            DirectSkeletonBoneSnapshot snapshot{};
            if (!rootFlattenedTwoHandedReader().capture(skeleton_bone_debug_math::DebugSkeletonBoneMode::HandsAndForearmsOnly,
                    skeleton_bone_debug_math::DebugSkeletonBoneSource::GameRootFlattenedBoneTree,
                    snapshot)) {
                return false;
            }

            for (std::size_t segment = 0; segment < outNodes.size(); ++segment) {
                const char* boneName = root_flattened_finger_skeleton_runtime::fingerBoneName(isLeft, 0, segment);
                const auto* node = boneName ? findSnapshotBone(snapshot, boneName) : nullptr;
                const auto* parent = node ? findSnapshotBoneByTreeIndex(snapshot, node->parentTreeIndex) : nullptr;
                if (!node || !parent || !isFiniteTransform(node->world) || !isFiniteTransform(parent->world)) {
                    return false;
                }

                const RE::NiTransform local = transform_math::composeTransforms(transform_math::invertTransform(parent->world), node->world);
                if (!isFiniteTransform(local)) {
                    return false;
                }

                outNodes[segment] = LiveThumbTransform{
                    .world = node->world,
                    .parentWorld = parent->world,
                    .local = local,
                    .valid = true,
                };
            }
            return true;
        }

        bool buildAlternateThumbLocalTransforms(
            bool isLeft,
            const RE::NiPoint3& supportGripPivotWorldPoint,
            const RE::NiPoint3& gripWorldPoint,
            float thumbScalarValue,
            std::array<RE::NiTransform, 15>& outLocalTransforms,
            std::uint16_t& outMask)
        {
            /*
             * ROCK switches the support thumb to an alternate local-transform
             * target when the weapon mesh solve requires thumb opposition. FRIK
             * only exposes scalar curls by default, so derive local thumb targets
             * from the root-flattened chain and publish them through the local
             * pose API.
             */
            outLocalTransforms = {};
            outMask = 0;

            std::array<LiveThumbTransform, 3> thumbNodes{};
            if (!resolveLiveThumbTransforms(isLeft, thumbNodes)) {
                return false;
            }

            const float sanitizedThumbValue = std::isfinite(thumbScalarValue) ? std::clamp(thumbScalarValue, 0.0f, 1.0f) : 1.0f;
            const float oppositionStrength = std::clamp(0.45f + (1.0f - sanitizedThumbValue) * 0.55f, 0.45f, 1.0f);

            for (std::size_t segment = 0; segment < thumbNodes.size(); ++segment) {
                const auto& node = thumbNodes[segment];
                if (!node.valid) {
                    return false;
                }

                const RE::NiPoint3 currentAxisWorld = normalizeOrFallback(
                    transform_math::rotateLocalVectorToWorld(node.world.rotate, RE::NiPoint3{ 1.0f, 0.0f, 0.0f }),
                    RE::NiPoint3{ 1.0f, 0.0f, 0.0f });
                const RE::NiPoint3 toGrip = weapon_support_thumb_pose_policy::vectorToGripFromPredictedThumbNode(
                    node.world.translate,
                    supportGripPivotWorldPoint,
                    gripWorldPoint);
                if (lengthSquared(toGrip) <= MIN_THUMB_OPPOSITION_DISTANCE * MIN_THUMB_OPPOSITION_DISTANCE) {
                    return false;
                }

                const RE::NiPoint3 targetAxisWorld = normalizeOrFallback(toGrip, currentAxisWorld);
                const float dotToTarget = std::clamp(weaponSolverDot(currentAxisWorld, targetAxisWorld), -1.0f, 1.0f);
                const float angle = std::acos(dotToTarget) * oppositionStrength;
                if (!std::isfinite(angle)) {
                    return false;
                }

                RE::NiMatrix3 rotationDelta = transform_math::makeIdentityRotation<RE::NiMatrix3>();
                if (angle > 0.0001f) {
                    RE::NiPoint3 axis = weaponSolverCross(currentAxisWorld, targetAxisWorld);
                    if (lengthSquared(axis) <= 0.000001f) {
                        axis = weaponSolverOrthogonalAxis(currentAxisWorld);
                    }
                    rotationDelta = weaponSolverAxisAngleStored<RE::NiMatrix3, RE::NiPoint3>(axis, angle);
                }

                const RE::NiMatrix3 targetWorldRotation =
                    weaponSolverApplyWorldRotationToStoredBasis<RE::NiMatrix3, RE::NiPoint3>(rotationDelta, node.world.rotate);
                RE::NiTransform localTransform = node.local;
                localTransform.rotate = transform_math::multiplyStoredRotations(targetWorldRotation, transform_math::transposeRotation(node.parentWorld.rotate));
                if (!isFiniteTransform(localTransform)) {
                    return false;
                }

                outLocalTransforms[segment] = localTransform;
                outMask = static_cast<std::uint16_t>(outMask | (1U << segment));
            }

            return outMask == SUPPORT_THUMB_LOCAL_TRANSFORM_MASK;
        }

        bool buildFullHandLocalTransformsForMeshPose(
            bool isLeft,
            const grab_finger_pose_runtime::SolvedGrabFingerPose& meshFingerPose,
            const frik_visual_authority::HandPoseData& handPose,
            std::array<RE::NiTransform, 15>& outLocalTransforms,
            std::uint16_t& outMask)
        {
            auto* api = frik_visual_authority::api();
            const bool canPublish =
                grab_finger_local_transform_math::shouldPublishLocalTransformPose(
                    g_rockConfig.rockGrabMeshLocalTransformPoseEnabled,
                    meshFingerPose.solved,
                    true,
                    api && api->getHandPoseLocalTransformsForPose != nullptr,
                    api && api->setHandPoseCustomLocalTransformsWithPriority != nullptr);
            if (!canPublish) {
                ROCK_LOG_DEBUG(Weapon,
                    "TwoHandedGrip: full-hand local transform override skipped hand={} enabled={} api={} baselineApi={} publishApi={}",
                    isLeft ? "left" : "right",
                    g_rockConfig.rockGrabMeshLocalTransformPoseEnabled ? "yes" : "no",
                    api ? "yes" : "no",
                    (api && api->getHandPoseLocalTransformsForPose) ? "yes" : "no",
                    (api && api->setHandPoseCustomLocalTransformsWithPriority) ? "yes" : "no");
                return false;
            }

            frik_visual_authority::FingerLocalTransformOverride baseline{};
            if (!frik_visual_authority::getHandPoseLocalTransformsForPose(handFromBool(isLeft), handPose, &baseline)) {
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: full-hand local transform override failed hand={} reason=baseline-query", isLeft ? "left" : "right");
                return false;
            }

            frik_visual_authority::FingerLocalTransformOverride corrected{};
            const char* failureReason = "unknown";
            if (!grab_finger_local_transform_runtime::buildSurfaceCorrectedLocalTransforms(isLeft,
                    meshFingerPose,
                    baseline,
                    grab_finger_local_transform_runtime::Options{
                        .enabled = g_rockConfig.rockGrabMeshLocalTransformPoseEnabled,
                        .smoothingSpeed = g_rockConfig.rockGrabFingerLocalTransformSmoothingSpeed,
                        .maxCorrectionDegrees = g_rockConfig.rockGrabFingerLocalTransformMaxCorrectionDegrees,
                        .surfaceAimStrength = g_rockConfig.rockGrabFingerSurfaceAimStrength,
                        .thumbOppositionStrength = g_rockConfig.rockGrabThumbOppositionStrength,
                        .thumbAlternateCurveStrength = g_rockConfig.rockGrabThumbAlternateCurveStrength,
                        .thumbSurfaceSafetyEnabled = g_rockConfig.rockGrabThumbSurfaceSafetyEnabled,
                        .thumbSurfaceSafetyMarginGameUnits = g_rockConfig.rockGrabThumbSurfaceSafetyMarginGameUnits,
                    },
                    corrected,
                    &failureReason)) {
                ROCK_LOG_WARN(Weapon,
                    "TwoHandedGrip: full-hand local transform override failed hand={} reason={}",
                    isLeft ? "left" : "right",
                    failureReason ? failureReason : "unknown");
                return false;
            }

            outMask = corrected.enabledMask;
            for (std::size_t i = 0; i < outLocalTransforms.size(); ++i) {
                outLocalTransforms[i] = corrected.localTransforms[i];
            }
            return outMask == grab_finger_local_transform_math::kFullFingerLocalTransformMask;
        }

    }

    static bool tryGetHandBoneTransform(bool isLeft, RE::NiTransform& outTransform)
    {
        outTransform = {};
        // EMBED (Jul 19, frame-order audit): prefer the host's PRE-AUTHORITY hand snapshot
        // (FRIK's clean controller-tracking output, captured post-FRIK before any authority
        // write this frame). The flattened-tree read below can contain LAST frame's
        // authority-written hand — feeding it into the two-handed weapon solve makes the
        // solve chase its own output (contamination loop). Fresh-this-frame only.
        if (rock::HostGetPreAuthorityHandWorld(isLeft, outTransform)) {
            return true;
        }
        DirectSkeletonBoneSnapshot snapshot{};
        if (!rootFlattenedTwoHandedReader().capture(skeleton_bone_debug_math::DebugSkeletonBoneMode::HandsAndForearmsOnly,
                skeleton_bone_debug_math::DebugSkeletonBoneSource::GameRootFlattenedBoneTree,
                snapshot)) {
            return false;
        }

        // FIX B: validate the resolved node's immediate parent is the expected forearm
        // segment for this side, so a node that merely shares the "LArm_Hand"/"RArm_Hand"
        // name (the PA-DIAG failure mode) cannot silently win a first-match scan while
        // power armor is equipped.
        const auto* handBone = findSnapshotBone(
            snapshot,
            isLeft ? "LArm_Hand" : "RArm_Hand",
            isLeft ? "LArm_ForeArm3" : "RArm_ForeArm3");
        if (!handBone || !isFiniteTransform(handBone->world)) {
            return false;
        }
        if (snapshot.inPowerArmor) {
            const auto* parent = findSnapshotBoneByTreeIndex(snapshot, handBone->parentTreeIndex);
            const bool ancestryValid = parent && parent->name == (isLeft ? "LArm_ForeArm3" : "RArm_ForeArm3");
            // Log only on a state transition (this runs every frame while gripping) --
            // matches the rate-limiting pattern DirectSkeletonBoneReader.cpp already uses
            // for its own PA-DIAG detection logging.
            static bool s_prevAncestryValidLeft = true;
            static bool s_prevAncestryValidRight = true;
            bool& prevValid = isLeft ? s_prevAncestryValidLeft : s_prevAncestryValidRight;
            if (ancestryValid != prevValid) {
                prevValid = ancestryValid;
                if (!ancestryValid) {
                    ROCK_LOG_WARN(Weapon,
                        "TwoHandedGrip: hand bone ancestry check failed in power armor hand={} — falling back to unscoped match, grip may seat on wrong node",
                        isLeft ? "left" : "right");
                }
            }
        }

        outTransform = handBone->world;
        return true;
    }

    bool TwoHandedGrip::tryCaptureRootFlattenedPalmWorld(bool isLeft, RE::NiPoint3& outPalmWorld, RE::NiTransform& outHandWorld)
    {
        outPalmWorld = {};
        if (!tryGetHandBoneTransform(isLeft, outHandWorld)) {
            return false;
        }
        outPalmWorld = computeGrabLegacyPalmPivotAWorldFromHandBasis(outHandWorld, isLeft);
        return true;
    }

    RE::NiPoint3 TwoHandedGrip::worldToWeaponLocal(const RE::NiPoint3& worldPos, const RE::NiAVObject* weaponNode)
    {
        if (!weaponNode) {
            return {};
        }
        return weapon_collision_geometry_math::worldPointToLocal(weaponNode->world.rotate, weaponNode->world.translate, weaponNode->world.scale, worldPos);
    }

    RE::NiPoint3 TwoHandedGrip::weaponLocalToWorld(const RE::NiPoint3& localPos, const RE::NiAVObject* weaponNode)
    {
        if (!weaponNode) {
            return {};
        }
        return weapon_collision_geometry_math::localPointToWorld(weaponNode->world.rotate, weaponNode->world.translate, weaponNode->world.scale, localPos);
    }

    RE::NiPoint3 TwoHandedGrip::resolvePartGripWorld(const WeaponPartGrip& grip, RE::NiNode* weaponNode) const
    {
        if (auto* supportAttachmentRoot = resolveCurrentSupportAttachmentRoot(grip, weaponNode)) {
            return transform_math::localPointToWorld(supportAttachmentRoot->world, grip.gripSourceLocal);
        }
        return weaponLocalToWorld(grip.gripLocal, weaponNode);
    }

    RE::NiPoint3 TwoHandedGrip::resolvePartGripWeaponLocal(const WeaponPartGrip& grip, RE::NiNode* weaponNode) const
    {
        return worldToWeaponLocal(resolvePartGripWorld(grip, weaponNode), weaponNode);
    }

    RE::NiPoint3 TwoHandedGrip::resolvePartGripNormalWeaponLocal(const WeaponPartGrip& grip, RE::NiNode* weaponNode) const
    {
        if (auto* supportAttachmentRoot = resolveCurrentSupportAttachmentRoot(grip, weaponNode)) {
            const RE::NiPoint3 supportNormalWorld = transform_math::localVectorToWorld(supportAttachmentRoot->world, grip.normalSourceLocal);
            return transform_math::worldVectorToLocal(weaponNode->world, supportNormalWorld);
        }
        return grip.normalLocal;
    }

    RE::NiTransform TwoHandedGrip::resolvePartGripHandWorld(const WeaponPartGrip& grip, RE::NiNode* weaponNode) const
    {
        if (auto* supportAttachmentRoot = resolveCurrentSupportAttachmentRoot(grip, weaponNode)) {
            return transform_math::composeTransforms(supportAttachmentRoot->world, grip.handSourceLocal);
        }
        if (!weaponNode) {
            return RE::NiTransform{};
        }
        return weapon_support_authority_policy::buildVisualOnlySupportHandWorld(weaponNode->world, grip.handWeaponLocal);
    }

    RE::NiTransform TwoHandedGrip::resolvePartGripHandWorldForWeaponWorld(
        const WeaponPartGrip& grip,
        RE::NiNode* weaponNode,
        const RE::NiTransform& weaponWorld) const
    {
        // Freeze the source frame's CURRENT weapon-local relation for this transaction.
        // The candidate weapon, its collision, and the published hand then all use the
        // same attachment pose even when a provider animates that part.
        if (auto* supportAttachmentRoot = resolveCurrentSupportAttachmentRoot(grip, weaponNode)) {
            const RE::NiTransform attachmentWeaponLocal = transform_math::composeTransforms(
                transform_math::invertTransform(weaponNode->world), supportAttachmentRoot->world);
            const RE::NiTransform candidateAttachmentWorld =
                transform_math::composeTransforms(weaponWorld, attachmentWeaponLocal);
            return transform_math::composeTransforms(candidateAttachmentWorld, grip.handSourceLocal);
        }
        return weapon_support_authority_policy::buildVisualOnlySupportHandWorld(
            weaponWorld, grip.handWeaponLocal);
    }

    RE::NiAVObject* TwoHandedGrip::resolveCurrentSupportAttachmentRoot(const WeaponPartGrip& grip, RE::NiNode* weaponNode) const
    {
        if (!grip.hasSourceFrames || !grip.attachmentRoot || !weaponNode) {
            return nullptr;
        }
        return actor_equipment_grab::nodeContainsNode(weaponNode, grip.attachmentRoot, 64) ? grip.attachmentRoot : nullptr;
    }

    void TwoHandedGrip::update(
        RE::NiNode* weaponNode,
        const WeaponInteractionContact& leftWeaponContact,
        const WeaponInteractionContact& rightWeaponContact,
        const EquippedWeaponGripFrameInput& frameInput,
        float dt,
        std::uint64_t currentWeaponGenerationKey,
        const WeaponCollision& weaponCollision,
        const WeaponInteractionRuntimeState& leftRuntimeState,
        const WeaponInteractionRuntimeState& rightRuntimeState,
        weapon_support_authority_policy::WeaponSupportAuthorityMode supportAuthorityMode,
        bool primaryDetachEnabled)
    {
        _hasSolvedWeaponTransform = false;
        _firingGripReattachHoverInsideRadius = false;

        // HOST API (Jul 19, Virtual Reloads): external off-hand grip block — release any
        // engaged part grip; the capture gate in capturePartGrip refuses new ones while
        // the lease is active.
        //
        // PROVIDER EXEMPTION (Jul 24, Virtual Reloads slide-grab): AttachOnly grips that
        // the provider consumer ITSELF whitelisted survive the block. The lease exists to
        // suppress DEFAULT off-hand weapon gripping during a scripted reload — but the
        // provider-authorized part grab (the whitelisted slide/magazine) IS that reload.
        // Without the exemption, "block off-hand gripping + whitelist the slide" — the
        // exact intended recipe — kills the consumer's own grab. FullTwoHandAuthority
        // provider grips are still torn down: they steer the whole weapon, which is
        // precisely what the block is meant to prevent.
        if (rock::HostIsTwoHandedGripBlocked()) {
            const auto gripSurvivesBlock = [this](bool isLeft) {
                const WeaponPartGrip& g = partGrip(isLeft);
                return g.active && g.providerPartAuthority.active && g.attachOnly;
            };
            if (partGrip(true).active && !gripSurvivesBlock(true)) {
                releasePartGrip(true, "host-api-block");
            }
            if (partGrip(false).active && !gripSurvivesBlock(false)) {
                releasePartGrip(false, "host-api-block");
            }
            // AUDIT FIX (Jul 19): releasing only the part grips left the state machine in
            // Gripping/PartCarry — the continue-check below then ran against an inactive
            // grip and could route through the manual-release action, which DROPS the
            // equipped weapon when the primary grip is not held (exactly a reload posture,
            // exactly when VirtualReloads engages this block). Tear down fully when no
            // exempt provider grip remains; the capturePartGrip gate keeps default
            // re-entry blocked while the block holds.
            if (!partGrip(true).active && !partGrip(false).active &&
                _state != TwoHandedState::Inactive) {
                transitionToInactive(ownsWeaponTransform());
            }
        }

        if (!runtime_state::isLocalSkeletonReady() || !weaponNode) {
            if (_state != TwoHandedState::Inactive) {
                transitionToInactive(false);
            }
            return;
        }

        const bool supportHandIsLeft = !_firingHandIsLeft;
        // FIX A1 (trigger-finger clip through gun during two-handed steering): the primary
        // hand's own weapon contact + runtime state, same per-hand selection pattern
        // updatePartCarryGrip already uses for its firingHandContact, threaded through to
        // transitionToGripping so it can capture the primary hand's own finger pose too.
        const WeaponInteractionContact& primaryWeaponContact = _firingHandIsLeft ? leftWeaponContact : rightWeaponContact;
        const WeaponInteractionRuntimeState& primaryRuntimeState = _firingHandIsLeft ? leftRuntimeState : rightRuntimeState;
        const WeaponInteractionDecision decision = routeWeaponInteraction(leftWeaponContact, leftRuntimeState);
        const bool leftTouchingSupport = decision.kind == WeaponInteractionKind::SupportGrip;
        RE::NiNode* interactionWeaponNode = sourceRootNodeOrFallback(decision.interactionRoot, weaponNode);
        const bool leftGripPressed = frameInput.leftGripHeld;
        const bool supportHandHoldingObject = frameInput.leftHandHoldingObject;

        // CAPTURE EDGE (Jul 19, user feedback): starting a support grip requires a grip
        // PRESS near the weapon — a grip held down since long before touching (walked
        // into the weapon) must not two-hand. A short press-to-touch grace window keeps
        // "press a beat before contact" natural; releasing clears it, so releasing and
        // re-pressing while touching re-grips as before. Single instance, game thread —
        // function-local statics are safe. Continue/release logic still uses the level.
        static bool s_prevSupportGripHeld = false;
        static int s_supportGripPressGraceFrames = 0;
        constexpr int kSupportGripPressGraceFrames = 20;  // ~220ms @90fps
        if (leftGripPressed && !s_prevSupportGripHeld) {
            s_supportGripPressGraceFrames = kSupportGripPressGraceFrames;
        } else if (!leftGripPressed) {
            s_supportGripPressGraceFrames = 0;
        } else if (s_supportGripPressGraceFrames > 0) {
            --s_supportGripPressGraceFrames;
        }
        s_prevSupportGripHeld = leftGripPressed;
        const bool gripPressForCapture = g_rockConfig.rockSupportGripRequiresPressEdge
            ? (leftGripPressed && s_supportGripPressGraceFrames > 0)
            : leftGripPressed;
        const EquippedWeaponPrimaryGripInput& primaryGripInput = frameInput.primaryGripInput;

        switch (_state) {
        case TwoHandedState::Inactive:
            if (leftTouchingSupport && !supportHandHoldingObject) {
                transitionToTouching(interactionWeaponNode, decision);
            }
            break;

        case TwoHandedState::Touching:
            if (supportHandHoldingObject) {
                _state = TwoHandedState::Inactive;
                break;
            }
            if (leftTouchingSupport) {
                _touchFrames = 0;
            } else {
                _touchFrames++;
                if (_touchFrames > TOUCH_TIMEOUT_FRAMES) {
                    _state = TwoHandedState::Inactive;
                    break;
                }
            }
            if (weapon_two_handed_grip_math::canStartSupportGrip(leftTouchingSupport, gripPressForCapture, supportHandHoldingObject)) {
                transitionToGripping(interactionWeaponNode, decision, weaponCollision, supportAuthorityMode, leftRuntimeState.providerPartAuthority, primaryWeaponContact, primaryRuntimeState);
            }
            break;

        case TwoHandedState::Gripping:
            if (!_activeWeaponNode) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing authority because active weapon source root is unavailable");
                transitionToInactive(false);
            } else if (!weapon_authority_lifecycle_policy::isWeaponContactGenerationCurrent(_activeWeaponGenerationKey, currentWeaponGenerationKey)) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing authority because weapon generation changed during support grip");
                transitionToInactive(false);
            } else if (!isPointerInLiveSubtree(_activeWeaponNode, weaponNode)) {
                // CRASH FIX (Jul 6, kept through the Jul-8 upstream merge): the cached authority
                // node is no longer part of the live weapon subtree (the game/FRIK rebuilt it
                // without advancing ROCK's generation key). Drop the grip instead of dereferencing
                // freed memory in updateGripping/updateTransformsDown.
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing authority — cached weapon node 0x{:x} no longer in the live weapon subtree (subtree rebuilt, generation key stale)",
                    reinterpret_cast<std::uintptr_t>(_activeWeaponNode));
                transitionToInactive(false);
            } else if (!providerPartAuthorityStillCurrent(partGrip(supportHandIsLeft), currentWeaponGenerationKey)) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing authority because provider weapon-part target is no longer current");
                transitionToInactive(false);
            } else if (providerPartTargetNewlyMatchesGrip(partGrip(supportHandIsLeft), currentWeaponGenerationKey)) {
                // The still-held grab recaptures next frame under the new
                // provider resolution (e.g. an AttachOnly whitelist armed
                // mid-hold).
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: releasing support grip to recapture under newly matched provider weapon-part target");
                transitionToInactive(ownsWeaponTransform());
            } else if (!leftRuntimeState.supportGripAllowed) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing authority because offhand reservation disabled support grip");
                transitionToInactive(false);
            } else if (!weapon_two_handed_grip_math::shouldContinueSupportGrip(leftGripPressed, supportHandHoldingObject)) {
                // DIAG (Jul 19, tester grip-flap): the bare "grip released" line hid WHICH input
                // ended the grip — say it explicitly so the next tester log is decisive.
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: support grip continue check failed (gripHeld={} handHoldingObject={})",
                    leftGripPressed, supportHandHoldingObject);
                const auto releaseAction = weapon_two_handed_grip_math::resolveSupportReleaseManualAction(primaryDetachEnabled, primaryGripInput.held);
                if (releaseAction == weapon_two_handed_grip_math::SupportReleaseManualAction::KeepPrimaryOwnership) {
                    transitionToPrimaryOnly(_activeWeaponNode, currentWeaponGenerationKey, "support-released-primary-held");
                } else if (releaseAction == weapon_two_handed_grip_math::SupportReleaseManualAction::DropEquippedWeapon) {
                    requestEquippedWeaponDrop(
                        "support-released-primary-not-held",
                        equipped_weapon_drop_policy::sourceForSupportRelease(primaryGripInput.released));
                } else {
                    transitionToInactive(ownsWeaponTransform());
                }
            } else if (primaryDetachEnabled &&
                       _authorityMode == weapon_support_authority_policy::WeaponSupportAuthorityMode::FullTwoHandedSolver &&
                       !primaryGripInput.held) {
                if (transitionToPartCarry()) {
                    updatePartCarryGrip(
                        _activeWeaponNode,
                        dt,
                        frameInput,
                        leftWeaponContact,
                        rightWeaponContact,
                        weaponCollision,
                        currentWeaponGenerationKey,
                        leftRuntimeState,
                        rightRuntimeState);
                }
            } else {
                updateGripping(_activeWeaponNode, dt);
            }
            break;

        case TwoHandedState::PartCarry:
            if (!_activeWeaponNode) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing part-carry authority because active weapon source root is unavailable");
                transitionToInactive(false);
            } else if (!weapon_authority_lifecycle_policy::isWeaponContactGenerationCurrent(_activeWeaponGenerationKey, currentWeaponGenerationKey)) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing part-carry authority because weapon generation changed");
                transitionToInactive(false);
            } else if (!isPointerInLiveSubtree(_activeWeaponNode, weaponNode)) {
                // Same liveness guard as the Gripping case above (Jul 6 crash fix) - the
                // generation key can stay unchanged while the game/FRIK rebuilds the fp
                // Weapon subtree without advancing it, and updatePartCarryGrip performs
                // unguarded reads of _activeWeaponNode.
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing part-carry authority — cached weapon node 0x{:x} no longer in the live weapon subtree",
                    reinterpret_cast<std::uintptr_t>(_activeWeaponNode));
                transitionToInactive(false);
            } else if (!primaryDetachEnabled) {
                transitionToInactive(ownsWeaponTransform());
            } else {
                updatePartCarryGrip(
                    _activeWeaponNode,
                    dt,
                    frameInput,
                    leftWeaponContact,
                    rightWeaponContact,
                    weaponCollision,
                    currentWeaponGenerationKey,
                    leftRuntimeState,
                    rightRuntimeState);
            }
            break;

        case TwoHandedState::PrimaryOnly:
            if (!_activeWeaponNode) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing primary-only authority because active weapon source root is unavailable");
                transitionToInactive(false);
            } else if (!weapon_authority_lifecycle_policy::isWeaponContactGenerationCurrent(_activeWeaponGenerationKey, currentWeaponGenerationKey)) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing primary-only authority because weapon generation changed");
                transitionToInactive(false);
            } else if (!isPointerInLiveSubtree(_activeWeaponNode, weaponNode)) {
                // Same liveness guard as the Gripping/PartCarry cases (Jul 6 crash fix) -
                // updatePrimaryOnlyGrip also performs unguarded reads of _activeWeaponNode.
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing primary-only authority — cached weapon node 0x{:x} no longer in the live weapon subtree",
                    reinterpret_cast<std::uintptr_t>(_activeWeaponNode));
                transitionToInactive(false);
            } else if (!primaryDetachEnabled) {
                transitionToInactive(false);
            } else if (leftTouchingSupport && weapon_two_handed_grip_math::canStartSupportGrip(leftTouchingSupport, gripPressForCapture, supportHandHoldingObject)) {
                transitionToGripping(interactionWeaponNode, decision, weaponCollision, supportAuthorityMode, leftRuntimeState.providerPartAuthority, primaryWeaponContact, primaryRuntimeState);
            } else {
                updatePrimaryOnlyGrip(_activeWeaponNode, currentWeaponGenerationKey, primaryGripInput);
            }
            break;
        }
    }

    void TwoHandedGrip::reset()
    {
        _equippedWeaponDropRequest = {};
        _hapticEvents = {};
        _firingGripReattachHoverInsideRadius = false;
        clearPrimaryGripPose(_firingHandIsLeft);
        clearPrimaryDetachVisualAuthority(_firingHandIsLeft);
        clearSupportGripPose(true);
        clearSupportGripPose(false);
        restoreFrikPrimaryWeaponPose();
        if (_state != TwoHandedState::Inactive) {
            transitionToInactive(false);
            return;
        }
        _state = TwoHandedState::Inactive;
        _touchFrames = 0;
        _rotationBlend = 0.0f;
        _hasSupportAimSmoothedDirection = false;
        _partGrips = {};
        _partCarryPivotIsLeft = true;
        _partCarryGripSeparationWorld = 0.0f;
        _primaryGripLocal = {};
        _lockedGripSeparationWorld = 0.0f;
        _authorityMode = weapon_support_authority_policy::WeaponSupportAuthorityMode::FullTwoHandedSolver;
        _hasSolvedWeaponTransform = false;
        _activeWeaponNode = nullptr;
        _activeWeaponGenerationKey = 0;
        _weaponNodeLocalBaseline = {};
        _hasWeaponNodeLocalBaseline = false;
        _primaryHandWeaponLocal = {};
        _hasFiringHandWeaponLocal = false;
        _hasPublishedFiringHandWorld = false;
        _hasPrimaryRollWeaponLocal = false;
        _primaryGripConfidence = 0.0f;
        resetLockedHandVisualLerp();
    }

    bool TwoHandedGrip::ownsWeaponTransform() const
    {
        return (_state == TwoHandedState::Gripping || _state == TwoHandedState::PartCarry) &&
               weapon_support_authority_policy::supportGripOwnsWeaponTransform(_authorityMode);
    }

    bool TwoHandedGrip::getSolvedWeaponTransform(RE::NiTransform& outTransform) const
    {
        if (!_hasSolvedWeaponTransform) {
            return false;
        }
        outTransform = _lastSolvedWeaponTransform;
        return true;
    }

    bool TwoHandedGrip::getDebugAuthoritySnapshot(TwoHandedGripDebugSnapshot& outSnapshot) const
    {
        const auto& leftGrip = partGrip(true);
        const auto& rightGrip = partGrip(false);
        if (!_hasSolvedWeaponTransform || !_activeWeaponNode) {
            return false;
        }
        if (!_hasFiringHandWeaponLocal && !leftGrip.active && !rightGrip.active) {
            return false;
        }

        outSnapshot.weaponWorld = _lastSolvedWeaponTransform;
        if (rightGrip.active) {
            outSnapshot.rightRequestedHandWorld = resolvePartGripHandWorld(rightGrip, _activeWeaponNode);
            outSnapshot.rightGripWorld = resolvePartGripWorld(rightGrip, _activeWeaponNode);
        } else {
            outSnapshot.rightRequestedHandWorld = transform_math::composeTransforms(_lastSolvedWeaponTransform, _primaryHandWeaponLocal);
            outSnapshot.rightGripWorld = transform_math::localPointToWorld(_lastSolvedWeaponTransform, _primaryGripLocal);
        }
        if (leftGrip.active) {
            outSnapshot.leftRequestedHandWorld = resolvePartGripHandWorld(leftGrip, _activeWeaponNode);
            outSnapshot.leftGripWorld = resolvePartGripWorld(leftGrip, _activeWeaponNode);
        } else {
            outSnapshot.leftRequestedHandWorld = RE::NiTransform{};
            outSnapshot.leftGripWorld = RE::NiPoint3{};
        }
        return true;
    }

    void TwoHandedGrip::resetLockedHandVisualLerp()
    {
        _primaryHandVisualLerp = {};
        partGrip(true).visualLerp = {};
        partGrip(false).visualLerp = {};
    }

    RE::NiTransform TwoHandedGrip::resolveLockedHandVisualTarget(
        const RE::NiTransform& targetWorld,
        const RE::NiTransform* liveHandWorld,
        float dt,
        LockedHandVisualLerpState& state,
        bool forceExact)
    {
        // A rigid two-hand solve has already positioned the complete weapon and both
        // captured hand frames as one transaction. Blending either hand independently
        // would temporarily break that weld (and is visible as the palm sliding over the
        // gun), so full-authority grips publish the exact derived frame immediately.
        if (forceExact) {
            state = {};
            state.lastAlpha = 1.0f;
            return targetWorld;
        }

        /*
         * Two-handed weapon smoothing is visual-only. The weapon solver keeps
         * immediate aim authority; this only eases the FRIK external hand target
         * into the locked hand-to-weapon relation captured at grip start.
         */
        if (!g_rockConfig.rockWeaponSupportGripHandLerpEnabled) {
            state = {};
            return targetWorld;
        }

        if (!state.active) {
            const RE::NiTransform startWorld = (liveHandWorld && isFiniteTransform(*liveHandWorld)) ? *liveHandWorld : targetWorld;
            const float initialDistance =
                hand_visual_lerp_math::distanceGameUnits(startWorld.translate, targetWorld.translate);
            const float durationSeconds =
                hand_visual_lerp_math::computeDistanceMappedDurationGameUnits(
                    initialDistance,
                    g_rockConfig.rockWeaponSupportGripHandLerpTimeMin,
                    g_rockConfig.rockWeaponSupportGripHandLerpTimeMax,
                    g_rockConfig.rockWeaponSupportGripHandLerpMinDistance,
                    g_rockConfig.rockWeaponSupportGripHandLerpMaxDistance);
            if (durationSeconds <= 0.0f) {
                state = {};
                state.lastAlpha = 1.0f;
                return targetWorld;
            }

            state.active = true;
            state.startWorld = startWorld;
            state.elapsedSeconds = 0.0f;
            state.durationSeconds = durationSeconds;
            state.lastAlpha = 0.0f;
        }

        state.elapsedSeconds =
            hand_visual_lerp_math::advanceTimedBlendElapsed(state.elapsedSeconds, dt, state.durationSeconds);
        const auto blended =
            hand_visual_lerp_math::blendTransformOverDuration(state.startWorld, targetWorld, state.elapsedSeconds, state.durationSeconds);
        state.lastAlpha = hand_visual_lerp_math::timedBlendAlpha(state.elapsedSeconds, state.durationSeconds);
        return blended.transform;
    }

    void TwoHandedGrip::transitionToTouching(RE::NiNode* weaponNode, const WeaponInteractionDecision& decision)
    {
        if (!weaponNode) {
            _state = TwoHandedState::Inactive;
            return;
        }

        _state = TwoHandedState::Touching;
        _touchFrames = 0;
        ROCK_LOG_DEBUG(Weapon,
            "TwoHandedGrip: touching weapon='{}' bodyId={} partKind={} pose={} interactionRoot={:x} sourceRoot={:x} generation={:016X}",
            weaponNode->name.c_str(),
            decision.bodyId,
            static_cast<int>(decision.partKind),
            static_cast<int>(decision.gripPose),
            reinterpret_cast<std::uintptr_t>(decision.interactionRoot),
            reinterpret_cast<std::uintptr_t>(decision.sourceRoot),
            decision.weaponGenerationKey);
    }

    bool TwoHandedGrip::capturePartGrip(
        bool isLeft,
        RE::NiNode* weaponNode,
        const WeaponInteractionDecision& decision,
        const WeaponCollision& weaponCollision,
        const WeaponProviderPartAuthority& providerPartAuthority)
    {
        if (!weaponNode) {
            return false;
        }
        if (rock::HostIsTwoHandedGripBlocked()) {
            // PROVIDER EXEMPTION (Jul 24): a capture the provider consumer itself
            // whitelisted in AttachOnly mode is allowed through the block — the block
            // suppresses DEFAULT off-hand weapon gripping during a scripted reload, and
            // the whitelisted part grab (slide/magazine) IS the reload interaction the
            // consumer wants. FullTwoHandAuthority provider targets stay blocked (they
            // steer the whole weapon — exactly what the lease prevents).
            const bool providerAttachOnlyCapture = weapon_part_grip_report_policy::providerGrabModeIsAttachOnly(
                providerPartAuthority.active, providerPartAuthority.grabMode);
            if (!providerAttachOnlyCapture) {
                return false;  // host API (Virtual Reloads): off-hand weapon gripping blocked
            }
        }
        if (rock::HostIsHandHoldingObject(isLeft)) {
            return false;  // hand is holding a grabbed object — no support grip (Jul 19 user rule)
        }
        if (!weapon_authority_lifecycle_policy::isWeaponContactGenerationCurrent(decision.weaponGenerationKey, _activeWeaponGenerationKey)) {
            ROCK_LOG_DEBUG(Weapon, "TwoHandedGrip: part grip capture skipped because contact generation is stale hand={}", isLeft ? "left" : "right");
            return false;
        }

        RE::NiTransform handTransform{};
        if (!tryGetHandBoneTransform(isLeft, handTransform)) {
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: part grip capture skipped because root flattened hand transforms are unavailable hand={}", isLeft ? "left" : "right");
            return false;
        }

        WeaponPartGrip& grip = partGrip(isLeft);
        grip = {};
        RE::NiAVObject* supportAttachmentRoot = decision.sourceRoot ? decision.sourceRoot : static_cast<RE::NiAVObject*>(weaponNode);
        const bool providerZoneActive =
            providerPartAuthority.active &&
            providerPartAuthority.interactionZoneActive;
        const bool providerAttachOnly =
            weapon_part_grip_report_policy::providerGrabModeIsAttachOnly(
                providerPartAuthority.active,
                providerPartAuthority.grabMode);
        RE::NiAVObject* providerControlledRoot =
            reinterpret_cast<RE::NiAVObject*>(providerPartAuthority.controlledRoot);
        if ((providerZoneActive || providerAttachOnly) &&
            providerControlledRoot &&
            providerControlledRoot != weaponNode &&
            isPointerInLiveSubtree(providerControlledRoot, weaponNode)) {
            supportAttachmentRoot = providerControlledRoot;
        } else {
            providerControlledRoot = nullptr;
        }

        const auto providerSnapMode =
            static_cast<::rock::provider::RockProviderWeaponPartInteractionZoneSnapModeV1>(
                providerPartAuthority.interactionZoneSnapMode);
        const std::uint32_t handedTransformFlag = static_cast<std::uint32_t>(
            isLeft ?
                ::rock::provider::RockProviderWeaponPartInteractionZoneFlagV1::LeftHandTransformValid :
                ::rock::provider::RockProviderWeaponPartInteractionZoneFlagV1::RightHandTransformValid);
        const bool providerFullHandSnap =
            providerZoneActive &&
            providerControlledRoot &&
            providerSnapMode ==
                ::rock::provider::RockProviderWeaponPartInteractionZoneSnapModeV1::FullHandTransform &&
            (providerPartAuthority.interactionZoneFlags & handedTransformFlag) != 0;
        if (providerFullHandSnap) {
            const auto& authoredLocal = isLeft ?
                providerPartAuthority.leftHandPartLocal :
                providerPartAuthority.rightHandPartLocal;
            const RE::NiTransform authoredHandWorld = transform_math::composeTransforms(
                providerControlledRoot->world,
                providerSnapTransformToNi(authoredLocal));
            if (isFiniteTransform(authoredHandWorld)) {
                handTransform = authoredHandWorld;
            }
        }
        grip.gripPose = decision.gripPose != WeaponGripPoseId::None ? decision.gripPose : WeaponGripPoseId::BarrelWrap;
        grip.partKind = decision.partKind;
        grip.attachmentRoot = supportAttachmentRoot;
        grip.providerPartAuthority = providerPartAuthority.active ? providerPartAuthority : WeaponProviderPartAuthority{};
        grip.attachOnly = weapon_part_grip_report_policy::providerGrabModeIsAttachOnly(
            grip.providerPartAuthority.active,
            grip.providerPartAuthority.grabMode);
        grip.contactBodyId = decision.bodyId;
        grip.contactSourceRoot =
            decision.sourceRoot ?
                decision.sourceRoot :
                reinterpret_cast<RE::NiAVObject*>(
                    providerPartAuthority.sourceRoot);
        if (!grip.contactSourceRoot) {
            grip.contactSourceRoot = supportAttachmentRoot;
        }
        grip.reloadRole = decision.reloadRole;
        grip.socketRole = decision.socketRole;
        grip.actionRole = decision.actionRole;
        grip.weaponGenerationKey = decision.weaponGenerationKey;
        grip.gripSequence = ++_gripCaptureSequence;
        {
            // The routing decision carries no support role or authored source
            // name; both come from the evidence descriptor keyed by the
            // contact body, matching the provider target-query construction.
            WeaponCollisionProfileEvidenceDescriptor descriptor{};
            RE::NiAVObject* descriptorSourceNode = nullptr;
            if (weaponCollision.tryGetProfileEvidenceDescriptorForBodyId(decision.bodyId, descriptor, descriptorSourceNode) &&
                descriptor.weaponGenerationKey == decision.weaponGenerationKey) {
                grip.supportRole = descriptor.semantic.supportGripRole;
                grip.omodFormId = descriptor.omodFormId;
                grip.attachPointFormId = descriptor.semantic.attachPointFormId;
                grip.classificationSource = descriptor.semantic.classificationSource;
                const std::size_t copyLength = (std::min)(descriptor.sourceName.size(), grip.sourceName.size() - 1);
                std::memcpy(grip.sourceName.data(), descriptor.sourceName.data(), copyLength);
                grip.sourceName[copyLength] = '\0';
            } else if (grip.providerPartAuthority.active) {
                grip.supportRole = static_cast<WeaponSupportGripRole>(grip.providerPartAuthority.supportRole);
                grip.sourceName = grip.providerPartAuthority.sourceName;
                grip.sourceName[grip.sourceName.size() - 1] = '\0';
            }
        }

        RE::NiPoint3 palmPos = computeGrabLegacyPalmPivotAWorldFromHandBasis(handTransform, isLeft);
        RE::NiPoint3 palmDir = computePalmNormalFromHandBasis(handTransform, isLeft);
        const bool providerAnchorSnap =
            providerZoneActive &&
            providerControlledRoot &&
            providerSnapMode ==
                ::rock::provider::RockProviderWeaponPartInteractionZoneSnapModeV1::AnchorPosition &&
            (providerPartAuthority.interactionZoneFlags &
                static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderWeaponPartInteractionZoneFlagV1::SnapAnchorValid)) != 0;
        const bool providerClosestMeshSnap =
            providerZoneActive &&
            providerSnapMode ==
                ::rock::provider::RockProviderWeaponPartInteractionZoneSnapModeV1::ClosestTargetMeshSurface;
        RE::NiPoint3 providerAnchorWorld{};
        if (providerAnchorSnap) {
            providerAnchorWorld = transform_math::localPointToWorld(
                providerControlledRoot->world,
                RE::NiPoint3{
                    providerPartAuthority.interactionZoneSnapAnchor[0],
                    providerPartAuthority.interactionZoneSnapAnchor[1],
                    providerPartAuthority.interactionZoneSnapAnchor[2],
                });
        }

        std::vector<TriangleData> triangles;
        const bool cachedTrianglesFound = weaponCollision.tryBuildSupportGripEvidenceTriangles(decision.bodyId, weaponNode, triangles);
        const std::size_t evidenceTriangleCount = triangles.size();
        std::size_t seatTriangleCount = triangles.size();
        bool seatLocalityFallback = false;

        GrabPoint grabPoint;
        bool meshFound = false;
        bool seatRescued = false;
        // SEAT LOCALITY (Jul 19, user log "hand suddenly slid down closer to me"): with the
        // palm aiming down the barrel, the directional weighting elected a surface 30+gu
        // away along the weapon (captured gripLocal y=-1.8 while the palm hovered at y~36 —
        // the hand SNAPS from muzzle to receiver at capture). The grip must seat on the
        // surface UNDER the palm: keep only triangles whose closest SURFACE point is within
        // the seat radius. Testing vertices alone drops a large handguard triangle when the
        // palm is over its middle, leaving small rail/receiver details to steal the seat.
        // Falls back to the full set if the filter empties (tiny parts).
        if (!providerClosestMeshSnap &&
            !providerAnchorSnap &&
            !providerFullHandSnap &&
            !triangles.empty() &&
            g_rockConfig.rockSupportGripSeatRadiusGameUnits > 0.0f) {
            const float r2 = g_rockConfig.rockSupportGripSeatRadiusGameUnits *
                             g_rockConfig.rockSupportGripSeatRadiusGameUnits;
            std::vector<TriangleData> nearTriangles;
            nearTriangles.reserve(triangles.size());
            for (const auto& tri : triangles) {
                float surfaceDistanceSquared = 0.0f;
                (void)closestPointOnTriangleToPoint(palmPos, tri, surfaceDistanceSquared);
                if (std::isfinite(surfaceDistanceSquared) && surfaceDistanceSquared <= r2) {
                    nearTriangles.push_back(tri);
                }
            }
            if (!nearTriangles.empty()) {
                triangles.swap(nearTriangles);
            } else {
                seatLocalityFallback = true;
            }
            seatTriangleCount = triangles.size();
        }
        if (providerClosestMeshSnap && !triangles.empty()) {
            float nearestDistanceSquared = (std::numeric_limits<float>::max)();
            for (std::size_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex) {
                const auto& triangle = triangles[triangleIndex];
                float distanceSquared = 0.0f;
                const RE::NiPoint3 candidate =
                    closestPointOnTriangleToPoint(palmPos, triangle, distanceSquared);
                if (!std::isfinite(distanceSquared) || distanceSquared >= nearestDistanceSquared) {
                    continue;
                }
                nearestDistanceSquared = distanceSquared;
                grabPoint.position = candidate;
                grabPoint.normal = normalizeOrFallback(
                    weaponSolverCross(
                        sub(triangle.v1, triangle.v0),
                        sub(triangle.v2, triangle.v0)),
                    RE::NiPoint3{ -palmDir.x, -palmDir.y, -palmDir.z });
                grabPoint.triangleIndex = static_cast<int>(triangleIndex);
                grabPoint.distance = std::sqrt((std::max)(0.0f, distanceSquared));
                meshFound = true;
            }
        } else if (!providerAnchorSnap && !providerFullHandSnap && !triangles.empty()) {
            meshFound = findClosestGrabPoint(triangles,
                palmPos,
                palmDir,
                g_rockConfig.rockGrabLateralWeight,
                g_rockConfig.rockGrabDirectionalWeight,
                grabPoint,
                g_rockConfig.rockGrabSurfaceBehindPalmToleranceGameUnits);
            if (!meshFound) {
                // SEAT RESCUE (Jul 19 seat audit, HIGH x2): a palm buried deeper than the
                // behind-palm tolerance (~1.5gu) rejects the ENTRY surface — so exactly the
                // hands most in need of reseating captured the RAW penetrating transform,
                // pinning the hand inside thick weapons (sniper stock/receiver) for the
                // whole grip. Retry with unlimited behind tolerance: backface culling still
                // discards exit-side walls, so the winner is the entry surface and
                // alignHandFrameToGripPoint pulls the hand back OUT onto it.
                meshFound = findClosestGrabPoint(triangles,
                    palmPos,
                    palmDir,
                    g_rockConfig.rockGrabLateralWeight,
                    g_rockConfig.rockGrabDirectionalWeight,
                    grabPoint,
                    1.0e6f);
                seatRescued = meshFound;
            }
        }

        // SEAT CLAMP (Jul 19, user log: mid-gun grip captured at stock y=-20.8 while the palm
        // was ~36gu away — the locality filter's fall-back-to-full-set escape re-admitted the
        // far-surface election). HARD INVARIANT: the seat may never land farther than the seat
        // radius from the palm; when it would, attach at the palm itself — the grip anchors at
        // the exact point the player pressed grab.
        if (meshFound &&
            !providerClosestMeshSnap &&
            g_rockConfig.rockSupportGripSeatRadiusGameUnits > 0.0f) {
            const RE::NiPoint3 seatDelta = sub(grabPoint.position, palmPos);
            const float r = g_rockConfig.rockSupportGripSeatRadiusGameUnits;
            if (dot(seatDelta, seatDelta) > r * r) {
                ROCK_LOG_INFO(Weapon,
                    "TwoHandedGrip: seat clamp rejected far surface point ({:.1f}gu from palm > {:.1f}gu) — attaching at palm",
                    std::sqrt(dot(seatDelta, seatDelta)), r);
                meshFound = false;
                seatRescued = false;
            }
        }

        if (providerAnchorSnap) {
            grip.gripLocal = worldToWeaponLocal(providerAnchorWorld, weaponNode);
            grip.grabNormalWorld = palmDir;
        } else if (meshFound) {
            grip.gripLocal = worldToWeaponLocal(grabPoint.position, weaponNode);
            grip.grabNormalWorld = grabPoint.normal;
        } else {
            grip.gripLocal = worldToWeaponLocal(palmPos, weaponNode);
            grip.grabNormalWorld = palmDir;
        }
        const RE::NiPoint3 gripWorldPoint =
            providerAnchorSnap ?
                providerAnchorWorld :
                (meshFound ? grabPoint.position : palmPos);
        // PALM CLEARANCE (Jul 19 seat audit): the seat is zero-clearance by design — the
        // palm PIVOT lands exactly on the surface, so the hand model's volume beyond the
        // pivot (~palm thickness) always intersects the mesh, worst on thin muzzle/barrel
        // surfaces. Pull the HAND alignment target outward along the seat surface normal
        // (oriented against the palm approach); grip point + finger targets stay on the
        // true surface so fingers still reach the mesh.
        RE::NiPoint3 seatTarget = gripWorldPoint;
        float appliedPalmClearance = 0.0f;
        if (meshFound && g_rockConfig.rockSupportGripPalmClearanceGameUnits > 0.0f) {
            RE::NiPoint3 outwardN = normalizeOrFallback(
                grip.grabNormalWorld,
                RE::NiPoint3{ -palmDir.x, -palmDir.y, -palmDir.z });
            if (dot(outwardN, palmDir) > 0.0f) {
                outwardN.x = -outwardN.x;
                outwardN.y = -outwardN.y;
                outwardN.z = -outwardN.z;
            }
            const float c = g_rockConfig.rockSupportGripPalmClearanceGameUnits;
            appliedPalmClearance = c;
            seatTarget.x = gripWorldPoint.x + outwardN.x * c;
            seatTarget.y = gripWorldPoint.y + outwardN.y * c;
            seatTarget.z = gripWorldPoint.z + outwardN.z * c;
        }
        const RE::NiTransform adjustedHandTransform =
            weapon_two_handed_grip_math::alignHandFrameToGripPoint(handTransform, palmPos, seatTarget);
        const RE::NiPoint3 adjustedPalmPos =
            computeGrabLegacyPalmPivotAWorldFromHandBasis(adjustedHandTransform, isLeft);
        const float palmSeatError = std::sqrt(lengthSquared(sub(adjustedPalmPos, seatTarget)));
        grip.handWeaponLocal = transform_math::composeTransforms(transform_math::invertTransform(weaponNode->world), adjustedHandTransform);
        grip.hasHandWeaponLocal = true;
        grip.normalLocal = transform_math::worldVectorToLocal(weaponNode->world, palmDir);
        if (supportAttachmentRoot) {
            grip.gripSourceLocal = transform_math::worldPointToLocal(supportAttachmentRoot->world, gripWorldPoint);
            grip.normalSourceLocal = transform_math::worldVectorToLocal(supportAttachmentRoot->world, palmDir);
            grip.handSourceLocal = transform_math::composeTransforms(transform_math::invertTransform(supportAttachmentRoot->world), adjustedHandTransform);
            grip.attachmentWeaponLocal = transform_math::composeTransforms(transform_math::invertTransform(weaponNode->world), supportAttachmentRoot->world);
            grip.hasSourceFrames = true;
            grip.hasAttachmentWeaponLocal = true;
        }

        grab_finger_pose_runtime::SolvedGrabFingerPose meshFingerPose{};
        const grab_finger_pose_runtime::SolvedGrabFingerPose* meshFingerPosePtr = nullptr;
        if (g_rockConfig.rockGrabMeshFingerPoseEnabled) {
            auto fingerPoseTargets = grab_finger_pose_runtime::makeSharedGripPoseTarget(gripWorldPoint, grip.grabNormalWorld);
            fingerPoseTargets.useSeatPointForMissingTargets = false;
            fingerPoseTargets.useWholeMeshForMissingTargets = true;
            root_flattened_finger_skeleton_runtime::Snapshot liveFingerSnapshot{};
            root_flattened_finger_skeleton_runtime::Snapshot seatedFingerSnapshot{};
            const root_flattened_finger_skeleton_runtime::Snapshot* liveFingerSnapshotPtr = nullptr;
            if (root_flattened_finger_skeleton_runtime::resolveLiveFingerSkeletonSnapshot(isLeft, liveFingerSnapshot)) {
                // Finger geometry must be solved in the same palm-clearance frame that is
                // ultimately rendered. Solving against the pre-seat hand and then moving the
                // hand outward made the curl endpoint disagree with the visible mesh border.
                liveFingerSnapshotPtr = remapFingerSnapshotToHandFrame(
                    liveFingerSnapshot,
                    handTransform,
                    adjustedHandTransform,
                    seatedFingerSnapshot) ?
                    &seatedFingerSnapshot :
                    &liveFingerSnapshot;
            }
            auto solvedFingerPose = grab_finger_pose_runtime::solveGrabFingerPoseFromTriangles(
                triangles, adjustedHandTransform, isLeft, adjustedPalmPos, fingerPoseTargets, g_rockConfig.rockGrabFingerMinValue,
                g_rockConfig.rockGrabMaxTriangleDistance, true, liveFingerSnapshotPtr,
                g_rockConfig.rockGrabFingerRejectBacksideHits, g_rockConfig.rockGrabFingerSurfacePlaneToleranceGameUnits);
            if (solvedFingerPose.solved) {
                std::array<grab_finger_pose_runtime::FingerPadSurfaceEvidence, 5> padEvidence{};
                if (liveFingerSnapshotPtr) {
                    grab_finger_pose_runtime::FingerPadProbeOptions padOptions{};
                    padOptions.openBiasStrength = 1.0f;
                    (void)grab_finger_pose_runtime::refineGrabFingerPoseWithPadProbes(
                        solvedFingerPose,
                        triangles,
                        fingerPoseTargets,
                        *liveFingerSnapshotPtr,
                        weaponNode->world,
                        true,
                        true,
                        padEvidence,
                        true,
                        true,
                        padOptions);
                }

                bool meshBorderSafetyOpenedFinger = false;
                for (std::size_t finger = 0; finger < solvedFingerPose.values.size(); ++finger) {
                    const bool frontSurfaceHit = solvedFingerPose.hitKind[finger] ==
                        grab_finger_pose_math::FingerCurlValue::HitKind::FrontValid;
                    const float safeOpenValue = two_handed_weapon_policy::meshBorderSafeFingerOpenValue(
                        solvedFingerPose.values[finger],
                        frontSurfaceHit,
                        padEvidence[finger].padMayBeInsideSurface);
                    if (safeOpenValue > solvedFingerPose.values[finger] + 0.0001f) {
                        solvedFingerPose.values[finger] = safeOpenValue;
                        meshBorderSafetyOpenedFinger = true;
                    }
                }
                if (meshBorderSafetyOpenedFinger) {
                    solvedFingerPose.jointValues =
                        grab_finger_pose_math::expandFingerCurlsToJointValues(solvedFingerPose.values);
                    solvedFingerPose.hasJointValues = true;
                }

                meshFingerPose = solvedFingerPose;
                meshFingerPosePtr = &meshFingerPose;
                ROCK_LOG_DEBUG(Weapon,
                    "TwoHandedGrip: mesh-border finger pose hand={} values=({:.2f},{:.2f},{:.2f},{:.2f},{:.2f}) hits={} candidateTris={} safetyOpened={} altThumb={} thumbLane={}",
                    isLeft ? "left" : "right",
                    meshFingerPose.values[0],
                    meshFingerPose.values[1],
                    meshFingerPose.values[2],
                    meshFingerPose.values[3],
                    meshFingerPose.values[4],
                    solvedFingerPose.hitCount,
                    solvedFingerPose.candidateTriangleCount,
                    meshBorderSafetyOpenedFinger ? "yes" : "no",
                    solvedFingerPose.usedAlternateThumbCurve ? "yes" : "no",
                    grab_finger_pose_math::thumbLaneName(solvedFingerPose.selectedThumbLane));
                if (solvedFingerPose.hasThumbCurveDiagnostics) {
                    ROCK_LOG_DEBUG(Weapon,
                        "TwoHandedGrip: thumb curve primary(hit={} value={:.2f} behind={}) opposition(hit={} value={:.2f} behind={}) sidePad(hit={} value={:.2f} behind={}) selected={}",
                        solvedFingerPose.thumbPrimaryCurve.hit ? "yes" : "no",
                        solvedFingerPose.thumbPrimaryCurve.value,
                        solvedFingerPose.thumbPrimaryCurve.openedByBehindContact ? "yes" : "no",
                        solvedFingerPose.thumbAlternateCurve.hit ? "yes" : "no",
                        solvedFingerPose.thumbAlternateCurve.value,
                        solvedFingerPose.thumbAlternateCurve.openedByBehindContact ? "yes" : "no",
                        solvedFingerPose.thumbSidePadCurve.hit ? "yes" : "no",
                        solvedFingerPose.thumbSidePadCurve.value,
                        solvedFingerPose.thumbSidePadCurve.openedByBehindContact ? "yes" : "no",
                        grab_finger_pose_math::thumbLaneName(solvedFingerPose.selectedThumbLane));
                }

                const bool canPublishAlternateThumb = !g_rockConfig.rockGrabMeshLocalTransformPoseEnabled &&
                    weapon_support_thumb_pose_policy::shouldPublishAlternateThumbLocalOverride(
                    solvedFingerPose.solved,
                    solvedFingerPose.usedAlternateThumbCurve,
                    frik_visual_authority::api() && frik_visual_authority::api()->setHandPoseCustomLocalTransformsWithPriority);
                if (canPublishAlternateThumb) {
                    std::array<RE::NiTransform, 15> localTransforms{};
                    std::uint16_t localTransformMask = 0;
                    if (buildAlternateThumbLocalTransforms(isLeft, palmPos, gripWorldPoint, meshFingerPose.values[0], localTransforms, localTransformMask)) {
                        grip.fingerLocalTransforms = localTransforms;
                        grip.fingerLocalTransformMask = localTransformMask;
                        grip.hasFingerLocalTransforms = true;
                        ROCK_LOG_DEBUG(Weapon,
                            "TwoHandedGrip: alternate thumb local transform override prepared hand={} mask=0x{:04X}",
                            isLeft ? "left" : "right",
                            grip.fingerLocalTransformMask);
                    } else {
                        ROCK_LOG_WARN(Weapon, "TwoHandedGrip: alternate thumb selected but local transform override could not be built");
                    }
                }
            }
        }

        setSupportGripPose(isLeft, grip.gripPose, meshFingerPosePtr);
        if (meshFingerPosePtr && grip.hasFingerPose) {
            std::array<RE::NiTransform, 15> localTransforms{};
            std::uint16_t localTransformMask = 0;
            const auto handPose = grip.hasFingerSplay ?
                frik_visual_authority::makeHandPoseDataFromJointValues(grip.fingerPose, grip.fingerSplayRadians) :
                frik_visual_authority::makeHandPoseDataFromJointValues(grip.fingerPose);
            if (buildFullHandLocalTransformsForMeshPose(
                    isLeft,
                    *meshFingerPosePtr,
                    handPose,
                    localTransforms,
                    localTransformMask)) {
                grip.fingerLocalTransforms = localTransforms;
                grip.fingerLocalTransformMask = localTransformMask;
                grip.hasFingerLocalTransforms = true;
                ROCK_LOG_DEBUG(Weapon,
                    "TwoHandedGrip: full-hand local transform override prepared hand={} mask=0x{:04X}",
                    isLeft ? "left" : "right",
                    grip.fingerLocalTransformMask);
            }
        }

        grip.visualLerp = {};
        grip.active = true;
        if (isLeft) {
            _hapticEvents.leftPartGripCaptured = true;
        } else {
            _hapticEvents.rightPartGripCaptured = true;
        }

        ROCK_LOG_INFO(Weapon,
            "TwoHandedGrip: part grip captured hand={} weapon='{}' "
            "gripLocal=({:.3f},{:.3f},{:.3f}) meshGrab={} triangles={}/{} localityFallback={} "
            "palm=({:.2f},{:.2f},{:.2f}) surface=({:.2f},{:.2f},{:.2f}) "
            "handSeat=({:.2f},{:.2f},{:.2f}) normalDot={:.3f} clearance={:.2f} seatError={:.4f} "
            "cachedTriangles={} partKind={} pose={} generation={:016X}",
            isLeft ? "left" : "right",
            weaponNode->name.c_str(),
            grip.gripLocal.x,
            grip.gripLocal.y,
            grip.gripLocal.z,
            seatRescued ? "RESCUED" : (meshFound ? "YES" : "FALLBACK"),
            seatTriangleCount,
            evidenceTriangleCount,
            seatLocalityFallback ? "yes" : "no",
            palmPos.x,
            palmPos.y,
            palmPos.z,
            gripWorldPoint.x,
            gripWorldPoint.y,
            gripWorldPoint.z,
            seatTarget.x,
            seatTarget.y,
            seatTarget.z,
            dot(grip.grabNormalWorld, palmDir),
            appliedPalmClearance,
            palmSeatError,
            cachedTrianglesFound ? "yes" : "no",
            static_cast<int>(grip.partKind),
            static_cast<int>(grip.gripPose),
            _activeWeaponGenerationKey);
        return true;
    }

    namespace
    {
        // Proper roll about the hand's local X (distal) axis, in the project's STORED-ROW
        // convention. Deliberately a rotation and not a mirror: a determinant -1 matrix would
        // invert the skinning and FRIK's arm solve downstream of composeTransforms.
        RE::NiMatrix3 makeRollAboutLocalXStored(float degrees)
        {
            const float r = degrees * 0.017453292519943295f;
            const float c = std::cos(r);
            const float s = std::sin(r);
            RE::NiMatrix3 m{};
            m.entry[0][0] = 1.0f; m.entry[0][1] = 0.0f; m.entry[0][2] = 0.0f;
            m.entry[1][0] = 0.0f; m.entry[1][1] = c;    m.entry[1][2] = s;
            m.entry[2][0] = 0.0f; m.entry[2][1] = -s;   m.entry[2][2] = c;
            return m;
        }

        WeaponGripPoseId clampWeaponGripPoseId(int value)
        {
            constexpr int kMax = static_cast<int>(WeaponGripPoseId::ReceiverSupport);
            const int clamped = (value < 0) ? 0 : ((value > kMax) ? kMax : value);
            return static_cast<WeaponGripPoseId>(clamped);
        }
    }

    void TwoHandedGrip::lockPartGripToWeaponRoot(bool isLeft)
    {
        /*
         * Part-carry feeds its own solved weapon transform back as the next
         * frame's base, so grips must resolve exclusively through the captured
         * weapon-root frames while it is active. Following live part-node
         * chains lets any per-frame part animation integrate into a steady
         * carry drift and pulls the locked hand visuals apart (verified by
         * telemetry: rigid-weapon grip separation grew frame over frame).
         */
        WeaponPartGrip& grip = partGrip(isLeft);
        grip.hasSourceFrames = false;
        grip.hasAttachmentWeaponLocal = false;
    }

    void TwoHandedGrip::releasePartGrip(bool isLeft, const char* reason)
    {
        WeaponPartGrip& grip = partGrip(isLeft);
        if (!grip.active) {
            return;
        }
        clearSupportGripPose(isLeft);
        grip = {};
        ROCK_LOG_INFO(Weapon, "TwoHandedGrip: part grip released hand={} reason={}", isLeft ? "left" : "right", reason ? reason : "unknown");
    }

    void TwoHandedGrip::transitionToGripping(
        RE::NiNode* weaponNode,
        const WeaponInteractionDecision& decision,
        const WeaponCollision& weaponCollision,
        weapon_support_authority_policy::WeaponSupportAuthorityMode supportAuthorityMode,
        const WeaponProviderPartAuthority& providerPartAuthority,
        const WeaponInteractionContact& primaryWeaponContact,
        const WeaponInteractionRuntimeState& primaryRuntimeState)
    {
        performance_profiler::ScopedTimer profilerTimer(performance_profiler::Scope::TwoHandedGripStart);

        if (!weaponNode) {
            transitionToInactive(false);
            return;
        }

        const bool supportHandIsLeft = !_firingHandIsLeft;
        const bool primaryHandIsLeft = _firingHandIsLeft;

        _authorityMode = supportAuthorityMode;
        _activeWeaponNode = weaponNode;
        _activeWeaponGenerationKey = decision.weaponGenerationKey;
        _weaponNodeLocalBaseline = weaponNode->local;
        _hasWeaponNodeLocalBaseline = true;
        _primaryGripConfidence = 0.0f;
        _hasFiringHandWeaponLocal = false;
        _hasPublishedFiringHandWorld = false;
        _hasPrimaryRollWeaponLocal = false;
        resetLockedHandVisualLerp();
        clearPrimaryGripPose(primaryHandIsLeft);
        clearSupportGripPose(supportHandIsLeft);
        // EDIT A3 cleanup: also drop any stale SUPPORT_GRIP_TAG-published pose left over
        // on the primary hand's own part-grip slot (see the capture below and the tag-leak
        // note on publishGripHandPoses) before this fresh session starts.
        clearSupportGripPose(primaryHandIsLeft);

        killFrikOffhandGrip();

        RE::NiTransform primaryTransform{};
        if (!tryGetHandBoneTransform(primaryHandIsLeft, primaryTransform)) {
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: support grip start skipped because root flattened hand transforms are unavailable");
            restoreFrikOffhandGrip();
            return;
        }

        const RE::NiPoint3 primaryPalmPos = computeGrabLegacyPalmPivotAWorldFromHandBasis(primaryTransform, primaryHandIsLeft);
        _primaryGripLocal = worldToWeaponLocal(primaryPalmPos, weaponNode);
        _primaryGripConfidence = 1.0f;
        _primaryHandWeaponLocal = transform_math::composeTransforms(transform_math::invertTransform(weaponNode->world), primaryTransform);
        _hasFiringHandWeaponLocal = true;
        _firingGripSequence = ++_gripCaptureSequence;
        _hasSupportAimSmoothedDirection = false;

        /*
         * EDIT A1 (trigger-finger clip through gun during two-handed steering): capture
         * the primary/firing hand's own mesh-solved finger pose at grip start too, using
         * the exact same capturePartGrip() machinery the PartCarry free-hand capture uses
         * below in updatePartCarryGrip (capturePartGrip(firingHandIsLeft, ...)). Without
         * this the firing hand's wrist rigidly follows the weapon every frame
         * (applyFiringHandLockedVisual) while its fingers stay on FRIK's weapon-agnostic
         * canned pose -- the visible clip-through once the support hand starts
         * steering/rotating the weapon. primaryDecision is resolved the same way
         * updatePartCarryGrip resolves its own firingHandContact/rightRuntimeState pair,
         * just threaded in from update() instead of re-read here. A failed capture (e.g.
         * the primary hand's palm is not currently touching the weapon's own collision)
         * degrades to the pre-fix behavior for this session rather than aborting the grip.
         */
        const WeaponInteractionDecision primaryDecision = routeWeaponInteraction(primaryWeaponContact, primaryRuntimeState);
        (void)capturePartGrip(primaryHandIsLeft, weaponNode, primaryDecision, weaponCollision, primaryRuntimeState.providerPartAuthority);
        /*
         * capturePartGrip() unconditionally marks the slot active=true, which is the
         * correct meaning for a real part/support grip but NOT for the primary hand:
         * several other consumers in this file (and the host-facing
         * computeLiveGripHandWorld seam PhysicsInteraction calls every frame for its
         * anti-rubber-band hand authority) branch on partGrip(isLeft).active to decide
         * between "use this captured mesh-seat-adjusted part-grip frame" and "use
         * _primaryHandWeaponLocal, the firing grip's own live-recomposed wrist frame."
         * The firing hand's wrist is, and must stay, driven by
         * applyFiringHandLockedVisual()/_primaryHandWeaponLocal (already correct, no
         * lag) -- only the finger-pose fields captured above (read solely by
         * publishGripHandPoses, which never checks .active) are meant to be used from
         * this capture. Force the slot back to inactive so every other consumer keeps
         * treating this hand as the firing grip, exactly as before this fix.
         */
        partGrip(primaryHandIsLeft).active = false;
        // capturePartGrip() also queues a part-grip-captured haptic on the captured hand —
        // correct for a real support/part grip, but the primary hand's capture here is
        // finger-pose-only bookkeeping: without clearing it, every two-handed grip start
        // buzzed BOTH controllers, including when the support-hand capture then failed and
        // no grip engaged at all (review-confirmed). Clear only the primary hand's event;
        // the support hand's own capture below still buzzes as before.
        if (primaryHandIsLeft) {
            _hapticEvents.leftPartGripCaptured = false;
        } else {
            _hapticEvents.rightPartGripCaptured = false;
        }

        if (!capturePartGrip(supportHandIsLeft, weaponNode, decision, weaponCollision, providerPartAuthority)) {
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: support grip start skipped because part grip capture failed");
            restoreFrikOffhandGrip();
            return;
        }

        WeaponPartGrip& capturedSupportGrip = partGrip(supportHandIsLeft);
        /*
         * Fail-safe invariant: AttachOnly is output-only hand glue plus an
         * optional part constraint.  Even if an upstream weapon-class or
         * provider-mode decision is stale, it can never enter the whole-weapon
         * solver and steer the gun away from the firing hand.
         */
        if (capturedSupportGrip.attachOnly) {
            _authorityMode =
                weapon_support_authority_policy::WeaponSupportAuthorityMode::VisualOnlySupport;
        }
        if (_authorityMode == weapon_support_authority_policy::WeaponSupportAuthorityMode::FullTwoHandedSolver &&
            !capturedSupportGrip.attachOnly) {
            // A shooting support grip is a rigid weapon-local weld. Following a live part
            // node here feeds its per-frame animation/flattening back into the next weapon
            // solve and makes the captured palm creep along the gun while locomoting.
            lockPartGripToWeaponRoot(supportHandIsLeft);
        }

        /*
         * SIDEARM TWO-HANDED PISTOL HOLD (Jul 27, user: "treat any offhanding on a 1 handed gun as
         * holding the gun grip with 2 hands ... bring the offhand to the gun grip").
         *
         * By default the support hand is published wherever the player's palm happened to touch the
         * gun. Live telemetry on one 10mm across four consecutive grips: supportLocal Y landed at
         * +9.90, +7.65, +2.85 and -3.43 (gripSeparation 13.3 / 15.5 / 7.1 / 9.2) while the FIRING
         * frame repeated to 0.01gu. For a pistol that scatter IS the bug — there is no foregrip to
         * seat on, so the "seat" is noise.
         *
         * Reseat the support hand as a rigid offset off the FIRING hand's own captured frame. Since
         * a sidearm now runs VisualOnlySupport, ROCK writes neither the weapon nor the firing hand,
         * so the weapon stays welded to the firing hand by FRIK and this offset is effectively
         * firing-hand-local: the off-hand rides the grip and the firing hand is provably untouched.
         *
         * Offsets are in the firing hand's RAW local basis (HandColliderTypes.h:655-658):
         * raw X = distal/toward fingertips, raw +Y = back of hand (-Y = palm face), raw Z =
         * cross-palm. Both bone bases are proper right-handed rotations, so raw +Z is PINKY-ward on
         * the RIGHT hand and THUMB-ward on the LEFT — hence the Z (and roll) sign flips for a
         * left-handed firing grip, matching this codebase's own precedent of negating authored Z to
         * express one anatomical point on both hands (RockConfig.cpp palm pivot +0.2 / -0.2).
         *
         * The rotation is a proper ROLL, never a mirror: a reflection has determinant -1 and would
         * propagate through composeTransforms into the published NiTransform, inverting both the
         * skinning and FRIK's arm solve.
         */
        if (g_rockConfig.rockSidearmTwoHandedGripReseat &&
            _authorityMode == weapon_support_authority_policy::WeaponSupportAuthorityMode::VisualOnlySupport &&
            !capturedSupportGrip.attachOnly &&
            _hasFiringHandWeaponLocal) {
            WeaponPartGrip& reseat = partGrip(supportHandIsLeft);
            const float mirror = _firingHandIsLeft ? -1.0f : 1.0f;

            RE::NiTransform offsetLocal{};
            offsetLocal.scale = 1.0f;
            offsetLocal.translate = RE::NiPoint3{
                g_rockConfig.rockSidearmSupportGripOffsetFingers,
                g_rockConfig.rockSidearmSupportGripOffsetPalmDepth,
                g_rockConfig.rockSidearmSupportGripOffsetCrossPalm * mirror,
            };
            offsetLocal.rotate = makeRollAboutLocalXStored(
                g_rockConfig.rockSidearmSupportGripRollDegrees * mirror);

            reseat.handWeaponLocal =
                transform_math::composeTransforms(_primaryHandWeaponLocal, offsetLocal);
            reseat.hasHandWeaponLocal = true;

            /*
             * MANDATORY, and the reason a naive version of this is a silent per-frame no-op: every
             * consumer that re-derives the support hand — resolvePartGripHandWorld,
             * resolvePartGripHandWorldForWeaponWorld and sehComputePartGripHandWorldRaw (the
             * HandAuthority::ApplyWinners seam) — prefers grip.handSourceLocal whenever
             * hasSourceFrames is set, and capturePartGrip sets it on EVERY capture because the
             * attachment root is never null. Drop to the weapon-root frame so the override is the
             * only surviving source. lockPartGripToWeaponRoot above only runs for the full solver.
             */
            reseat.hasSourceFrames = false;
            reseat.hasAttachmentWeaponLocal = false;

            // The mesh-solved per-joint finger overrides were solved for the DISCARDED touch point
            // and are published unconditionally by publishGripHandPoses; keeping them would pose the
            // fingers for geometry the hand is no longer anywhere near.
            reseat.hasFingerLocalTransforms = false;
            reseat.fingerLocalTransformMask = 0;

            setSupportGripPose(
                supportHandIsLeft,
                clampWeaponGripPoseId(g_rockConfig.rockSidearmSupportGripPoseId),
                nullptr);

            ROCK_LOG_INFO(Weapon,
                "TwoHandedGrip: sidearm two-handed hold — support hand reseated onto the firing "
                "grip (offset=({:.2f},{:.2f},{:.2f})gu roll={:.1f}deg firingLeft={} pose={})",
                offsetLocal.translate.x,
                offsetLocal.translate.y,
                offsetLocal.translate.z,
                g_rockConfig.rockSidearmSupportGripRollDegrees * mirror,
                _firingHandIsLeft,
                static_cast<int>(g_rockConfig.rockSidearmSupportGripPoseId));
        }

        const RE::NiPoint3 supportGripWorldPoint = resolvePartGripWorld(partGrip(supportHandIsLeft), weaponNode);
        // Full weapon authority rotates around the exact captured firing-hand
        // frame origin, not the legacy palm estimate. Rotating around the palm
        // made the wrist orbit by several game units at steep offhand angles;
        // the arm solver then clamped that artificial translation and the hand
        // visibly left the trigger grip. The complete hand-to-weapon transform
        // captured above makes its own origin the definitionally rigid anchor.
        const RE::NiPoint3 primaryAuthorityAnchorWorld =
            _authorityMode ==
                    weapon_support_authority_policy::
                        WeaponSupportAuthorityMode::
                            FullTwoHandedSolver
                ? primaryTransform.translate
                : primaryPalmPos;
        const RE::NiPoint3 primaryToSupportWorld =
            sub(supportGripWorldPoint, primaryAuthorityAnchorWorld);
        _lockedGripSeparationWorld = std::sqrt(dot(primaryToSupportWorld, primaryToSupportWorld));

        /*
         * LEVER-ARM AUTHORITY GATE (Jul 25, pistol grip fix A3). The full solver's
         * angular gain is ~1/leverArm: a pistol cup measures ~13gu grip separation
         * (live log) vs >= 22.4gu on every observed long gun, so the same off-hand
         * motion swings a pistol's solved orientation ~2x faster — divergence the
         * firing hand's delivery cannot follow on FRIK v3, which is the reported
         * "off-hand disturbs the pistol grip". Below the min lever arm, demote to
         * VisualOnlySupport — the proven cup behavior: the off-hand glues to the
         * gun, the weapon and firing hand are never touched. Same demotion pattern
         * as the AttachOnly fail-safe above; capture-time only, so no per-frame
         * flapping (_lockedGripSeparationWorld is fixed for weld-locked grips).
         */
        if (g_rockConfig.rockTwoHandedLeverArmAuthorityGate &&
            _authorityMode == weapon_support_authority_policy::WeaponSupportAuthorityMode::FullTwoHandedSolver &&
            !capturedSupportGrip.attachOnly &&
            two_handed_weapon_policy::effectiveSupportSteeringWeight(
                _lockedGripSeparationWorld,
                g_rockConfig.rockTwoHandedMinSteeringLeverArmGameUnits,
                g_rockConfig.rockTwoHandedFullSteeringLeverArmGameUnits,
                g_rockConfig.rockTwoHandedMinSteeringAuthority) <= 0.0f) {
            _authorityMode = weapon_support_authority_policy::WeaponSupportAuthorityMode::VisualOnlySupport;
            ROCK_LOG_INFO(Weapon,
                "TwoHandedGrip: steering authority demoted to visual-only separation={:.1f} min={:.1f} full={:.1f}",
                _lockedGripSeparationWorld,
                g_rockConfig.rockTwoHandedMinSteeringLeverArmGameUnits,
                g_rockConfig.rockTwoHandedFullSteeringLeverArmGameUnits);
        }

        /*
         * FIRING-HAND ROLL REFERENCE (Jul 25, pistol grip fix B1): capture the firing
         * palm normal in weapon-local space so the solver can reference roll about the
         * aim axis to the LIVE firing controller instead of the support palm normal.
         */
        _primaryRollWeaponLocal = transform_math::worldVectorToLocal(
            weaponNode->world,
            computePalmNormalFromHandBasis(primaryTransform, primaryHandIsLeft));
        _hasPrimaryRollWeaponLocal = true;

        _state = TwoHandedState::Gripping;
        _rotationBlend = 0.0f;
        _gripLogCounter = 0;

        const WeaponPartGrip& supportGrip = partGrip(supportHandIsLeft);
        ROCK_LOG_INFO(Weapon,
            "TwoHandedGrip: grip active weapon='{}', "
            "primaryLocal=({:.3f},{:.3f},{:.3f}), supportLocal=({:.3f},{:.3f},{:.3f}), "
            "gripSeparation={:.3f}, primaryGripSource={}, primaryGripConfidence={:.2f}, partKind={}, pose={}, authorityMode={}, generation={:016X}",
            weaponNode->name.c_str(),
            _primaryGripLocal.x,
            _primaryGripLocal.y,
            _primaryGripLocal.z,
            supportGrip.gripLocal.x,
            supportGrip.gripLocal.y,
            supportGrip.gripLocal.z,
            _lockedGripSeparationWorld,
            "root-flattened",
            _primaryGripConfidence,
            static_cast<int>(supportGrip.partKind),
            static_cast<int>(supportGrip.gripPose),
            static_cast<int>(_authorityMode),
            _activeWeaponGenerationKey);
    }

    void TwoHandedGrip::transitionToInactive(bool publishRestoredWeaponTransform)
    {
        clearPrimaryGripPose(_firingHandIsLeft);
        clearPrimaryDetachVisualAuthority(_firingHandIsLeft);
        clearSupportGripPose(true);
        clearSupportGripPose(false);
        restoreFrikOffhandGrip();
        restoreFrikPrimaryWeaponPose();
        bool restoredWeaponTransformAvailable = false;
        RE::NiTransform restoredWeaponTransform{};
        if (publishRestoredWeaponTransform && _hasWeaponNodeLocalBaseline && _activeWeaponNode) {
            if (_activeWeaponNode->parent) {
                restoredWeaponTransform = transform_math::composeTransforms(_activeWeaponNode->parent->world, _weaponNodeLocalBaseline);
            } else {
                restoredWeaponTransform = _weaponNodeLocalBaseline;
            }
            restoredWeaponTransformAvailable = true;
        }

        _state = TwoHandedState::Inactive;
        _touchFrames = 0;
        _rotationBlend = 0.0f;
        _hasSupportAimSmoothedDirection = false;
        _partGrips = {};
        _partCarryPivotIsLeft = true;
        _partCarryGripSeparationWorld = 0.0f;
        _primaryGripLocal = {};
        _lockedGripSeparationWorld = 0.0f;
        _authorityMode = weapon_support_authority_policy::WeaponSupportAuthorityMode::FullTwoHandedSolver;
        _hasSolvedWeaponTransform = publishRestoredWeaponTransform && restoredWeaponTransformAvailable;
        if (_hasSolvedWeaponTransform) {
            _lastSolvedWeaponTransform = restoredWeaponTransform;
        }
        _primaryHandWeaponLocal = {};
        _hasFiringHandWeaponLocal = false;
        _hasPublishedFiringHandWorld = false;
        _hasPrimaryRollWeaponLocal = false;
        _primaryGripConfidence = 0.0f;
        _activeWeaponNode = nullptr;
        _activeWeaponGenerationKey = 0;
        _weaponNodeLocalBaseline = {};
        _hasWeaponNodeLocalBaseline = false;
        resetLockedHandVisualLerp();

        ROCK_LOG_INFO(Weapon, "TwoHandedGrip: grip released");
    }

    void TwoHandedGrip::updateGripping(RE::NiNode* weaponNode, float dt)
    {
        const WeaponPartGrip& supportGrip = partGrip(!_firingHandIsLeft);
        if (supportGrip.active && supportGrip.attachOnly) {
            _authorityMode =
                weapon_support_authority_policy::WeaponSupportAuthorityMode::VisualOnlySupport;
        }
        if (_authorityMode == weapon_support_authority_policy::WeaponSupportAuthorityMode::VisualOnlySupport) {
            updateVisualOnlySupportGrip(weaponNode, dt);
            return;
        }

        updateFullWeaponAuthorityGrip(weaponNode, dt);
    }

    bool TwoHandedGrip::providerPartAuthorityStillCurrent(const WeaponPartGrip& grip, std::uint64_t currentWeaponGenerationKey) const
    {
        if (!grip.providerPartAuthority.active) {
            return true;
        }
        if (currentWeaponGenerationKey == 0 || currentWeaponGenerationKey != grip.providerPartAuthority.weaponGenerationKey) {
            return false;
        }

        ::rock::provider::RockProviderWeaponPartTargetQueryV1 query{};
        query.weaponGenerationKey = grip.providerPartAuthority.weaponGenerationKey;
        query.bodyId = grip.providerPartAuthority.bodyId;
        query.partKind = grip.providerPartAuthority.partKind;
        query.reloadRole = grip.providerPartAuthority.reloadRole;
        query.supportRole = grip.providerPartAuthority.supportRole;
        query.socketRole = grip.providerPartAuthority.socketRole;
        query.actionRole = grip.providerPartAuthority.actionRole;
        query.sourceRoot = grip.providerPartAuthority.sourceRoot;
        std::memcpy(query.sourceName, grip.providerPartAuthority.sourceName.data(), grip.providerPartAuthority.sourceName.size());
        query.sourceName[sizeof(query.sourceName) - 1] = '\0';

        ::rock::provider::RockProviderWeaponPartTargetResolutionV1 resolution{};
        if (!::rock::provider::resolveWeaponPartTargetV1(query, resolution)) {
            return false;
        }
        return resolution.matched != 0 &&
               resolution.ownerToken == grip.providerPartAuthority.ownerToken &&
               resolution.groupId == grip.providerPartAuthority.groupId &&
               static_cast<std::uint32_t>(resolution.grabMode) == grip.providerPartAuthority.grabMode;
    }

    bool TwoHandedGrip::providerPartTargetNewlyMatchesGrip(const WeaponPartGrip& grip, std::uint64_t currentWeaponGenerationKey) const
    {
        /*
         * Upgrade twin of providerPartAuthorityStillCurrent: a support grip
         * captured WITHOUT provider authority whose own part NOW resolves to
         * a matched provider target — a consumer armed its whitelist while
         * the hand was already holding the part (PAPER_Redux: pulling the
         * trigger mid-hold switches an authority grab to attach-only). The
         * caller releases the grip; the still-held grab recaptures within a
         * couple of frames under the new resolution, through the same
         * re-resolve path the downgrade direction uses when a target
         * disappears mid-grip. The query is built from the grip's own
         * captured contact identity, not the live contact, so a flickering
         * contact cannot convert against the wrong part.
         */
        if (!grip.active || grip.providerPartAuthority.active) {
            return false;
        }
        if (currentWeaponGenerationKey == 0 || currentWeaponGenerationKey != grip.weaponGenerationKey) {
            return false;
        }

        ::rock::provider::RockProviderWeaponPartTargetQueryV1 query{};
        query.weaponGenerationKey = grip.weaponGenerationKey;
        query.bodyId = grip.contactBodyId;
        query.partKind = static_cast<std::uint32_t>(grip.partKind);
        query.reloadRole = static_cast<std::uint32_t>(grip.reloadRole);
        query.supportRole = static_cast<std::uint32_t>(grip.supportRole);
        query.socketRole = static_cast<std::uint32_t>(grip.socketRole);
        query.actionRole = static_cast<std::uint32_t>(grip.actionRole);
        std::memcpy(query.sourceName, grip.sourceName.data(), grip.sourceName.size());
        query.sourceName[sizeof(query.sourceName) - 1] = '\0';

        ::rock::provider::RockProviderWeaponPartTargetResolutionV1 resolution{};
        return ::rock::provider::resolveWeaponPartTargetV1(query, resolution) && resolution.matched != 0;
    }

    void TwoHandedGrip::updateFullWeaponAuthorityGrip(RE::NiNode* weaponNode, float dt)
    {
        const bool supportHandIsLeft = !_firingHandIsLeft;
        const bool primaryHandIsLeft = _firingHandIsLeft;
        const WeaponPartGrip& supportGrip = partGrip(supportHandIsLeft);

        _rotationBlend = (std::min)(1.0f, _rotationBlend + dt * ROTATION_BLEND_SPEED);

        RE::NiTransform primaryTransform{};
        RE::NiTransform supportTransform{};
        if (!tryGetHandBoneTransform(primaryHandIsLeft, primaryTransform) || !tryGetHandBoneTransform(supportHandIsLeft, supportTransform)) {
            _hasSolvedWeaponTransform = false;
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing support grip because root flattened hand transforms are unavailable");
            transitionToInactive(false);
            return;
        }

        // The immutable firing-grip anchor is the exact wrist/hand frame
        // origin captured in _primaryHandWeaponLocal.  A legacy palm pivot is
        // useful for touch/re-attach distance tests, but is not a rigid-body
        // pivot: changing weapon orientation around it makes the wrist orbit
        // and can push the primary arm outside its reachable sphere.
        const RE::NiPoint3 primaryController =
            primaryTransform.translate;
        const RE::NiPoint3 primaryAuthorityAnchorLocal =
            _primaryHandWeaponLocal.translate;
        RE::NiPoint3 supportController = computeGrabLegacyPalmPivotAWorldFromHandBasis(supportTransform, supportHandIsLeft);

        const RE::NiPoint3 currentSupportWorld = resolvePartGripWorld(supportGrip, weaponNode);
        const RE::NiPoint3 currentPrimaryGripWorld =
            transform_math::localPointToWorld(
                weaponNode->world,
                primaryAuthorityAnchorLocal);
        const float currentGripSeparationWorld = std::sqrt(dot(sub(currentSupportWorld, currentPrimaryGripWorld), sub(currentSupportWorld, currentPrimaryGripWorld)));
        const float lockedGripSeparationWorld = supportGrip.hasSourceFrames ? currentGripSeparationWorld : _lockedGripSeparationWorld;
        const RE::NiPoint3 supportGripLocal = resolvePartGripWeaponLocal(supportGrip, weaponNode);
        // FIX C (power-armor jitter insurance): low-pass the tracked support direction
        // across frames instead of latching a raw single-frame sample straight into the
        // aim target -- cheap insurance against upstream discontinuities (including a
        // Fix-B-class bone-lookup mismatch) showing up as unattenuated weapon-aim wobble.
        const RE::NiPoint3 lockedSupportControllerTarget = makeSmoothedLockedSupportGripTarget(
            primaryController,
            supportController,
            currentSupportWorld,
            lockedGripSeparationWorld,
            0.001f,
            _supportAimSmoothedDirectionWorld,
            _hasSupportAimSmoothedDirection,
            dt);
        RE::NiPoint3 blendedSupportTarget = lerpPoint(currentSupportWorld, lockedSupportControllerTarget, _rotationBlend);

        /*
         * LEVER-ARM STEERING BLEND (Jul 25, pistol grip fix A4). In the 15-22gu band
         * (SMG/sawn-off levers) the support hand keeps only PARTIAL steering authority:
         * blend the support target between where the FIRING controller's own aim would
         * place the seat (weight 0 — gun tracks one-handed aim exactly, off-hand rides
         * along) and the steered target (weight 1 — today's exact behavior). Because the
         * solve is seeded incrementally, scaling the per-frame rotation would only
         * low-pass authority, not reduce it — the blend must be between two ABSOLUTE
         * references. Rifles measure >= 22.4gu → weight 1.0 → bit-exact legacy path.
         */
        float steeringWeight = 1.0f;
        if (g_rockConfig.rockTwoHandedLeverArmAuthorityGate) {
            steeringWeight = two_handed_weapon_policy::effectiveSupportSteeringWeight(
                lockedGripSeparationWorld,
                g_rockConfig.rockTwoHandedMinSteeringLeverArmGameUnits,
                g_rockConfig.rockTwoHandedFullSteeringLeverArmGameUnits,
                g_rockConfig.rockTwoHandedMinSteeringAuthority);
            // The floor is applied INSIDE effectiveSupportSteeringWeight so that this site and the
            // capture-time demotion in transitionToGripping can never disagree again — that
            // divergence is precisely what kept the pistol off-hand dead after the first attempt.
        }
        if (steeringWeight < 1.0f) {
            // Weapon orientation the live firing controller alone implies (captured weld).
            // Convention check: handLocal L was captured as compose(inv(W), H) = H·W⁻¹,
            // i.e. H = L·W (row-vector). Solving for W: W = L⁻¹·H = compose(H, inv(L)) —
            // same convention as compose(parentWorld, childLocal) = childWorld used at
            // the snapshot sites in this file.
            const RE::NiTransform controllerAnchoredWeaponWorld = transform_math::composeTransforms(
                primaryTransform,
                transform_math::invertTransform(_primaryHandWeaponLocal));
            // Where the support seat would sit under pure controller aim:
            const RE::NiPoint3 ctrlSeatWorld = transform_math::localPointToWorld(
                controllerAnchoredWeaponWorld, supportGripLocal);
            const RE::NiPoint3 dirCtrl = sub(ctrlSeatWorld, primaryController);
            const RE::NiPoint3 dirSteered = sub(blendedSupportTarget, primaryController);
            const float lenCtrl = std::sqrt(dot(dirCtrl, dirCtrl));
            const float lenSteered = std::sqrt(dot(dirSteered, dirSteered));
            if (lenCtrl > 0.001f && lenSteered > 0.001f) {
                // nlerp of the unit directions, re-projected to the locked separation:
                const RE::NiPoint3 blendedDir{
                    (dirCtrl.x / lenCtrl) * (1.0f - steeringWeight) + (dirSteered.x / lenSteered) * steeringWeight,
                    (dirCtrl.y / lenCtrl) * (1.0f - steeringWeight) + (dirSteered.y / lenSteered) * steeringWeight,
                    (dirCtrl.z / lenCtrl) * (1.0f - steeringWeight) + (dirSteered.z / lenSteered) * steeringWeight };
                const float blendedLen = std::sqrt(dot(blendedDir, blendedDir));
                if (blendedLen > 0.001f) {
                    const float scale = lockedGripSeparationWorld / blendedLen;
                    blendedSupportTarget = RE::NiPoint3{
                        primaryController.x + blendedDir.x * scale,
                        primaryController.y + blendedDir.y * scale,
                        primaryController.z + blendedDir.z * scale };
                }
            }
        }

        WeaponTwoHandedSolverInput<RE::NiTransform, RE::NiPoint3> solverInput{};
        solverInput.weaponWorldTransform = weaponNode->world;
        // ROLL CONTINUITY (Jul 24, workflow-confirmed wrist-flip root cause): re-basing
        // the solve on the live weaponNode->world every frame means every frame performs
        // one large shortest-arc rotation from the CONTROLLER's one-handed aim to the
        // hand-to-hand axis. Shortest-arc leaves roll unconstrained (it parallel-
        // transports the controller's roll), and its antiparallel branch flips 180°
        // about an arbitrary axis — at high/low aim the palm-normal twist correction
        // degenerates too (both projections collapse near the aim axis), producing the
        // reported hard WRIST FLIP when the off-hand pushes the gun far up/down. Seeding
        // the rotation from last frame's SOLVED transform makes the solve incremental
        // (small per-frame arcs, roll carried forward continuously), so the antiparallel
        // branch is unreachable in practice and roll can no longer invert crossing
        // vertical. Translation stays live from the weapon node.
        if (_hasSolvedWeaponTransform) {
            solverInput.weaponWorldTransform.rotate = _lastSolvedWeaponTransform.rotate;
        }
        solverInput.primaryGripLocal = primaryAuthorityAnchorLocal;
        solverInput.supportGripLocal = supportGripLocal;
        solverInput.primaryTargetWorld = primaryController;
        solverInput.supportTargetWorld = blendedSupportTarget;
        solverInput.supportNormalLocal = resolvePartGripNormalWeaponLocal(supportGrip, weaponNode);
        solverInput.supportNormalTargetWorld = computePalmNormalFromHandBasis(supportTransform, supportHandIsLeft);
        solverInput.useSupportNormalTwist = true;
        solverInput.supportNormalTwistFactor = SUPPORT_NORMAL_TWIST_FACTOR;
        // FIRING-HAND ROLL AUTHORITY (Jul 25, pistol grip fix B3): reference roll about
        // the aim axis to the LIVE firing controller at full strength instead of the
        // support palm normal at half strength — the trigger hand owns roll, so the gun
        // cannot roll/pitch out of the captured firing grip while the off-hand steers.
        // Takes precedence inside the solver; factor 0.0 in ROCK.ini = exact legacy path.
        solverInput.usePrimaryRollTwist =
            _hasPrimaryRollWeaponLocal && g_rockConfig.rockTwoHandedPrimaryRollAuthorityFactor > 0.0f;
        solverInput.primaryRollLocal = _primaryRollWeaponLocal;
        solverInput.primaryRollTargetWorld = computePalmNormalFromHandBasis(primaryTransform, primaryHandIsLeft);
        solverInput.primaryRollTwistFactor = g_rockConfig.rockTwoHandedPrimaryRollAuthorityFactor;

        const auto solved = solveTwoHandedWeaponTransformFrikPivot(solverInput);
        if (!solved.solved) {
            return;
        }

        RE::NiTransform solvedWeaponWorld = solved.weaponWorldTransform;
        // The firing grip is the immutable weapon pivot. The old embedded reach
        // correction translated the complete gun toward the support shoulder after
        // this solve, which pulled the trigger hand out of its captured grip and made
        // the muzzle shift as the offhand approached full extension. Keep the gun on
        // the controller, clean accumulated basis error, and reassert the pivot exactly.
        solvedWeaponWorld.rotate = orthonormalizeStoredRotation(solvedWeaponWorld.rotate);

        /*
         * ANGULAR RATE LIMIT (Jul 27, pistol off-hand authority).
         *
         * The firing grip is rigid in POSITION unconditionally (reanchorAtPrimaryGrip below pins
         * the captured grip point onto the controller every frame). Its ORIENTATION, though, is
         * only rigid if the firing WRIST tracks the weapon's rotation — which is exactly what
         * applyLockedHandVisualAuthority(applyPrimaryHandAuthority=true) does, and which only runs
         * in FullTwoHandedSolver mode. Solver angular gain goes as ~1/leverArm, so a pistol's short
         * cup rotates the gun about twice as fast per unit of off-hand travel as a rifle, and FRIK
         * v3's wrist delivery cannot keep up — that lag IS the "off-hand disturbs the weapon grip"
         * report. Capping the SPEED (not the range) of the solved rotation keeps the demanded wrist
         * motion inside what the hand can actually deliver, so the off-hand can still steer the gun
         * anywhere, just never faster than the grip can follow.
         */
        const float maxDegreesPerSecond = g_rockConfig.rockTwoHandedMaxSteeringDegreesPerSecond;
        if (_hasSolvedWeaponTransform && maxDegreesPerSecond > 0.0f && dt > 0.0f) {
            const auto& previous = _lastSolvedWeaponTransform.rotate;
            const auto& proposed = solvedWeaponWorld.rotate;
            // Angle of the relative rotation, via trace(R_new * R_prev^T).
            float trace = 0.0f;
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    trace += proposed.entry[i][k] * previous.entry[i][k];
                }
            }
            const float cosAngle = (std::clamp)((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
            const float angle = std::acos(cosAngle);
            const float maxAngle =
                maxDegreesPerSecond * (3.14159265358979323846f / 180.0f) * dt;
            if (std::isfinite(angle) && angle > maxAngle && angle > 1.0e-5f) {
                const float t = maxAngle / angle;
                RE::NiMatrix3 limited{};
                for (int i = 0; i < 3; ++i) {
                    for (int k = 0; k < 3; ++k) {
                        limited.entry[i][k] =
                            previous.entry[i][k] +
                            (proposed.entry[i][k] - previous.entry[i][k]) * t;
                    }
                }
                // Element-wise blend leaves the basis slightly non-orthonormal; the existing
                // cleanup restores it (per-frame angles here are small, so this tracks a true
                // slerp closely).
                solvedWeaponWorld.rotate = orthonormalizeStoredRotation(limited);
            }
        }

        solvedWeaponWorld = two_handed_weapon_policy::reanchorAtPrimaryGrip(
            solvedWeaponWorld,
            primaryAuthorityAnchorLocal,
            primaryController);

        if (!applyWeaponVisualAuthority(weaponNode, solvedWeaponWorld)) {
            _hasSolvedWeaponTransform = false;
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing support grip because ROCK visual weapon authority failed");
            transitionToInactive(false);
            return;
        }

        // EDIT A2: hoisted above the publish calls so it can also gate the primary hand's
        // finger-pose publish immediately below, right alongside the existing wrist
        // authority gate it already governs a few lines down.
        const bool applyPrimaryHandAuthority = weapon_support_authority_policy::supportGripAppliesPrimaryHandAuthority(_authorityMode);

        static_assert(weapon_visual_authority_math::handPosePrecedesLockedHandAuthority());
        static_assert(weapon_visual_authority_math::weaponVisualPrecedesLockedHandAuthority());
        publishGripHandPoses(supportHandIsLeft);
        // EDIT A2 — DISABLED (2026-07-24, regression). The primary hand's capturePartGrip
        // (EDIT A1) has no real per-part collision evidence to search — unlike the support
        // hand's genuine touch/grab contact, the primary hand's synthesized decision resolves
        // to a body/part classification with zero registered support-grip evidence triangles
        // (confirmed live: "meshGrab=FALLBACK triangles=0/0 partKind=22" on every single
        // capture, 100% reproducible, never once resolved real geometry). setSupportGripPose
        // sets hasFingerPose=true unconditionally even on the fallback path, so there's no
        // cheap signal here to gate publish on real-vs-fallback — the fallback canned pose
        // was being published as if it were correct, every frame. Net effect: worse than
        // doing nothing — it neither fixed the original trigger-clip (still reproduces,
        // confirmed) nor left FRIK's native pose alone (which is what previously "worked",
        // if imperfectly). Publish disabled until the primary hand has a real mesh-evidence
        // source to solve against; EDIT A1's capture is left in place (harmless — unused
        // while unpublished) and EDIT A3's tag cleanup is left in place (still correct
        // regardless of whether this publish ever fires).
        // if (applyPrimaryHandAuthority) {
        //     publishGripHandPoses(primaryHandIsLeft);
        // }

        if (!applyLockedHandVisualAuthority(
                weaponNode,
                applyPrimaryHandAuthority,
                true,
                dt,
                &primaryTransform,
                &supportTransform,
                true)) {
            _hasSolvedWeaponTransform = false;
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing support grip because ROCK locked hand authority failed");
            transitionToInactive(false);
            return;
        }

        _lastSolvedWeaponTransform = weaponNode->world;
        _hasSolvedWeaponTransform = true;

        RE::NiPoint3 primaryGripFinal =
            transform_math::localPointToWorld(
                _lastSolvedWeaponTransform,
                primaryAuthorityAnchorLocal);
        RE::NiPoint3 offhandGripFinal = resolvePartGripWorld(supportGrip, weaponNode);

        if (++_gripLogCounter >= 90) {
            _gripLogCounter = 0;
            float separation = std::sqrt(dot(sub(primaryGripFinal, offhandGripFinal), sub(primaryGripFinal, offhandGripFinal)));
            ROCK_LOG_DEBUG(Weapon,
                "TwoHandedGrip: blend={:.2f}, separation={:.1f}gu, aimW={:.2f}, rollTwistDeg={:+.1f}, "
                "primaryGrip=({:.1f},{:.1f},{:.1f}), offhandGrip=({:.1f},{:.1f},{:.1f}), "
                "handLerp=({:.2f}/{:.3f}s,{:.2f}/{:.3f}s), solverError=({:.4f},{:.4f})gu",
                _rotationBlend,
                separation,
                steeringWeight,
                solved.appliedTwistRadians * 57.2957795f,
                primaryGripFinal.x,
                primaryGripFinal.y,
                primaryGripFinal.z,
                offhandGripFinal.x,
                offhandGripFinal.y,
                offhandGripFinal.z,
                _primaryHandVisualLerp.lastAlpha,
                _primaryHandVisualLerp.durationSeconds,
                supportGrip.visualLerp.lastAlpha,
                supportGrip.visualLerp.durationSeconds,
                solved.primaryError,
                solved.supportError);
        }
    }

    bool TwoHandedGrip::transitionToPartCarry()
    {
        if (_state == TwoHandedState::PartCarry) {
            return true;
        }

        clearPrimaryGripPose(_firingHandIsLeft);
        // EDIT A3 cleanup: the firing hand's own SUPPORT_GRIP_TAG-published finger pose
        // (published every frame by EDIT A2 while FullTwoHandedSolver Gripping was
        // active) is not cleared anywhere else on this path -- the part-carry free-hand
        // slot for this same hand identity starts inactive (see the capture in
        // transitionToGripping) rather than self-healing through a stale-grip release,
        // so a stale pose would otherwise ride along into part carry's open/ungripped
        // firing hand until (if ever) a fresh part grip capture overwrites it.
        clearSupportGripPose(_firingHandIsLeft);
        if (!blockFrikPrimaryWeaponPose()) {
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: primary detach skipped because hFRIK primary weapon-pose blocker is unavailable");
            return false;
        }
        _primaryHandVisualLerp = {};
        partGrip(!_firingHandIsLeft).visualLerp = {};
        lockPartGripToWeaponRoot(!_firingHandIsLeft);
        _rotationBlend = 1.0f;
        _partCarryPivotIsLeft = !_firingHandIsLeft;
        _partCarryGripSeparationWorld = 0.0f;
        _state = TwoHandedState::PartCarry;
        _hapticEvents.firingGripDetached = true;
        _hapticEvents.firingGripDetachedHandIsLeft = _firingHandIsLeft;
        ROCK_LOG_INFO(Weapon, "TwoHandedGrip: firing hand detached; part grips own equipped weapon authority");
        return true;
    }

    bool TwoHandedGrip::republishPartCarryWeaponTransform(RE::NiNode* weaponNode)
    {
        if (_state != TwoHandedState::PartCarry) {
            return false;
        }
        return republishOwnedWeaponTransform(weaponNode);
    }

    bool TwoHandedGrip::republishOwnedWeaponTransform(RE::NiNode* weaponNode)
    {
        if (!ownsWeaponTransform() || !_hasSolvedWeaponTransform || !weaponNode ||
            weaponNode != _activeWeaponNode) {
            return false;
        }
        return applyWeaponVisualAuthority(weaponNode, _lastSolvedWeaponTransform);
    }

    bool TwoHandedGrip::beginPrimaryOnlyGrip(RE::NiNode* weaponNode, std::uint64_t currentWeaponGenerationKey)
    {
        if (!weaponNode || currentWeaponGenerationKey == 0 || _state != TwoHandedState::Inactive) {
            return false;
        }

        if (!transitionToPrimaryOnly(weaponNode, currentWeaponGenerationKey, "primary-grip-start")) {
            return false;
        }
        // Only a fresh grab pulses; transitionToPrimaryOnly is also reached
        // from support-release paths where the firing grip never changed.
        _hapticEvents.firingGripAttached = true;
        _hapticEvents.firingGripAttachedHandIsLeft = _firingHandIsLeft;
        _firingGripSequence = ++_gripCaptureSequence;
        return true;
    }

    EquippedWeaponManualDropRequest TwoHandedGrip::consumeEquippedWeaponDropRequest()
    {
        const EquippedWeaponManualDropRequest request = _equippedWeaponDropRequest;
        _equippedWeaponDropRequest = {};
        return request;
    }

    void TwoHandedGrip::getHandGripReport(bool isLeft, HandGripReport& outReport) const
    {
        outReport = {};
        const bool isFiringHand = isLeft == _firingHandIsLeft;
        const WeaponPartGrip& grip = partGrip(isLeft);
        const auto kind = weapon_part_grip_report_policy::resolveHandGripKind(
            _state == TwoHandedState::Gripping,
            _state == TwoHandedState::PartCarry,
            _state == TwoHandedState::PrimaryOnly,
            isFiringHand,
            grip.active,
            grip.attachOnly,
            _authorityMode == weapon_support_authority_policy::WeaponSupportAuthorityMode::VisualOnlySupport);
        outReport.kind = kind;
        if (kind == weapon_part_grip_report_policy::HandGripKind::None) {
            return;
        }

        outReport.active = true;
        if (kind == weapon_part_grip_report_policy::HandGripKind::FiringGrip) {
            // In PrimaryOnly the weapon rides the FRIK-native hand attach and
            // ROCK holds no captured hand-to-weapon frame; hasHandPartLocal
            // stays false there by design.
            outReport.gripSequence = _firingGripSequence;
            outReport.weaponGenerationKey = _activeWeaponGenerationKey;
            outReport.sourceRoot = reinterpret_cast<std::uintptr_t>(_activeWeaponNode);
            outReport.hasHandPartLocal = _hasFiringHandWeaponLocal;
            outReport.handPartLocal = _primaryHandWeaponLocal;
            return;
        }

        outReport.attachOnly = grip.attachOnly;
        outReport.gripSequence = grip.gripSequence;
        outReport.weaponGenerationKey = grip.weaponGenerationKey != 0 ? grip.weaponGenerationKey : _activeWeaponGenerationKey;
        outReport.bodyId = grip.contactBodyId;
        outReport.partKind = static_cast<std::uint32_t>(grip.partKind);
        outReport.reloadRole = static_cast<std::uint32_t>(grip.reloadRole);
        outReport.supportRole = static_cast<std::uint32_t>(grip.supportRole);
        outReport.socketRole = static_cast<std::uint32_t>(grip.socketRole);
        outReport.actionRole = static_cast<std::uint32_t>(grip.actionRole);
        outReport.contactSourceRoot =
            reinterpret_cast<std::uintptr_t>(
                grip.contactSourceRoot ? grip.contactSourceRoot : grip.attachmentRoot);
        outReport.sourceRoot = reinterpret_cast<std::uintptr_t>(grip.attachmentRoot);
        if (grip.providerPartAuthority.active) {
            outReport.providerOwnerToken = grip.providerPartAuthority.ownerToken;
            outReport.providerGroupId = grip.providerPartAuthority.groupId;
            outReport.providerGrabMode = grip.providerPartAuthority.grabMode;
        }
        outReport.hasHandPartLocal = grip.hasSourceFrames || grip.hasHandWeaponLocal;
        outReport.handPartLocalIsSourceLocal = grip.hasSourceFrames;
        outReport.handPartLocal = grip.hasSourceFrames ? grip.handSourceLocal : grip.handWeaponLocal;
        outReport.sourceName = grip.sourceName;
        outReport.omodFormId = grip.omodFormId;
        outReport.attachPointFormId = grip.attachPointFormId;
        outReport.classificationSource = static_cast<std::uint32_t>(grip.classificationSource);
    }

    TwoHandedGripHapticEvents TwoHandedGrip::consumeHapticEvents()
    {
        const TwoHandedGripHapticEvents events = _hapticEvents;
        _hapticEvents = {};
        return events;
    }

    bool TwoHandedGrip::transitionToPrimaryOnly(RE::NiNode* weaponNode, std::uint64_t currentWeaponGenerationKey, const char* reason)
    {
        if (!weaponNode || currentWeaponGenerationKey == 0) {
            return false;
        }

        const bool primaryHandIsLeft = _firingHandIsLeft;
        const bool supportHandIsLeft = !_firingHandIsLeft;

        if (_state == TwoHandedState::Inactive) {
            _activeWeaponNode = weaponNode;
            _activeWeaponGenerationKey = currentWeaponGenerationKey;
            _weaponNodeLocalBaseline = weaponNode->local;
            _hasWeaponNodeLocalBaseline = true;

            RE::NiTransform primaryTransform{};
            if (tryGetHandBoneTransform(primaryHandIsLeft, primaryTransform)) {
                _primaryGripLocal = worldToWeaponLocal(computeGrabLegacyPalmPivotAWorldFromHandBasis(primaryTransform, primaryHandIsLeft), weaponNode);
                _primaryGripConfidence = 1.0f;
            } else {
                _primaryGripLocal = {};
                _primaryGripConfidence = 0.0f;
            }
        }

        clearPrimaryGripPose(primaryHandIsLeft);
        clearSupportGripPose(supportHandIsLeft);
        clearSupportGripPose(primaryHandIsLeft);
        clearPrimaryDetachVisualAuthority(primaryHandIsLeft);
        restoreFrikOffhandGrip();
        restoreFrikPrimaryWeaponPose();
        _partGrips = {};
        _partCarryPivotIsLeft = true;
        _partCarryGripSeparationWorld = 0.0f;
        _hasSolvedWeaponTransform = false;
        _primaryHandWeaponLocal = {};
        _hasFiringHandWeaponLocal = false;
        _hasPublishedFiringHandWorld = false;
        _hasPrimaryRollWeaponLocal = false;
        _primaryHandVisualLerp = {};
        _state = TwoHandedState::PrimaryOnly;

        ROCK_LOG_INFO(Weapon,
            "TwoHandedGrip: primary-only equipped weapon ownership active reason={} generation={:016X}",
            reason ? reason : "unknown",
            _activeWeaponGenerationKey);
        return true;
    }

    void TwoHandedGrip::requestEquippedWeaponDrop(const char* reason, equipped_weapon_drop_policy::SourceHand sourceHand)
    {
        if (_equippedWeaponDropRequest.requested) {
            transitionToInactive(false);
            return;
        }

        _equippedWeaponDropRequest = EquippedWeaponManualDropRequest{
            .requested = true,
            .sourceHand = sourceHand,
        };
        ROCK_LOG_INFO(Weapon,
            "TwoHandedGrip: equipped weapon drop requested reason={} sourceHand={} generation={:016X}",
            reason ? reason : "unknown",
            equipped_weapon_drop_policy::sourceHandName(sourceHand),
            _activeWeaponGenerationKey);
        transitionToInactive(false);
    }

    void TwoHandedGrip::updatePrimaryOnlyGrip(
        RE::NiNode* weaponNode,
        std::uint64_t currentWeaponGenerationKey,
        const EquippedWeaponPrimaryGripInput& primaryGripInput)
    {
        equipped_weapon_manual_ownership_policy::RuntimeState manualState{
            .active = true,
            .weaponGenerationKey = _activeWeaponGenerationKey,
        };
        const auto manualDecision = equipped_weapon_manual_ownership_policy::update(manualState,
            equipped_weapon_manual_ownership_policy::Input{
                .weaponEquipped = weaponNode != nullptr,
                .weaponGenerationKey = currentWeaponGenerationKey,
                .startRequested = false,
                .primaryGripRetained = primaryGripInput.held,
                .supportGripRetained = false,
            });

        if (manualDecision.dropRequested) {
            requestEquippedWeaponDrop("primary-only-grip-released",
                _firingHandIsLeft ? equipped_weapon_drop_policy::SourceHand::Left : equipped_weapon_drop_policy::SourceHand::Right);
            return;
        }

        if (manualDecision.cleared) {
            transitionToInactive(false);
            return;
        }

        _hasSolvedWeaponTransform = false;
    }

    bool TwoHandedGrip::firingGripContactMatchesCapturedGrip(
        RE::NiNode* weaponNode,
        const WeaponInteractionContact& firingHandWeaponContact,
        const RE::NiTransform& firingHandTransform) const
    {
        if (!weaponNode || !firingHandWeaponContact.valid) {
            return false;
        }

        if (!weapon_authority_lifecycle_policy::isWeaponContactGenerationCurrent(firingHandWeaponContact.weaponGenerationKey, _activeWeaponGenerationKey)) {
            return false;
        }

        const RE::NiPoint3 firingPalm = computeGrabLegacyPalmPivotAWorldFromHandBasis(firingHandTransform, _firingHandIsLeft);
        const RE::NiPoint3 firingGripWorld = weaponLocalToWorld(_primaryGripLocal, weaponNode);
        const RE::NiPoint3 delta = sub(firingPalm, firingGripWorld);
        const float distance = std::sqrt(dot(delta, delta));
        return std::isfinite(distance) && distance <= g_rockConfig.rockWeaponFiringGripReattachRadius;
    }

    bool TwoHandedGrip::tryComputeFiringPalmToGripDistance(RE::NiNode* weaponNode, float& outDistance) const
    {
        if (!weaponNode) {
            return false;
        }
        RE::NiTransform firingHandTransform{};
        if (!tryGetHandBoneTransform(_firingHandIsLeft, firingHandTransform)) {
            return false;
        }
        const RE::NiPoint3 firingPalm = computeGrabLegacyPalmPivotAWorldFromHandBasis(firingHandTransform, _firingHandIsLeft);
        const RE::NiPoint3 firingGripWorld = weaponLocalToWorld(_primaryGripLocal, weaponNode);
        const RE::NiPoint3 delta = sub(firingPalm, firingGripWorld);
        const float distance = std::sqrt(dot(delta, delta));
        if (!std::isfinite(distance)) {
            return false;
        }
        outDistance = distance;
        return true;
    }

    bool TwoHandedGrip::tryReattachFiringGrip(RE::NiNode* weaponNode, const WeaponInteractionContact& firingHandWeaponContact)
    {
        if (!weaponNode) {
            return false;
        }

        const bool firingHandIsLeft = _firingHandIsLeft;
        RE::NiTransform firingHandTransform{};
        if (!tryGetHandBoneTransform(firingHandIsLeft, firingHandTransform)) {
            return false;
        }

        if (!firingGripContactMatchesCapturedGrip(weaponNode, firingHandWeaponContact, firingHandTransform)) {
            return false;
        }

        const RE::NiPoint3 firingPalm = computeGrabLegacyPalmPivotAWorldFromHandBasis(firingHandTransform, firingHandIsLeft);
        const RE::NiPoint3 firingGripWorld = weaponLocalToWorld(_primaryGripLocal, weaponNode);
        const RE::NiTransform adjustedFiringHandTransform =
            weapon_two_handed_grip_math::alignHandFrameToGripPoint(firingHandTransform, firingPalm, firingGripWorld);
        _primaryHandWeaponLocal = transform_math::composeTransforms(transform_math::invertTransform(weaponNode->world), adjustedFiringHandTransform);
        _hasFiringHandWeaponLocal = true;
        _firingGripSequence = ++_gripCaptureSequence;
        _primaryHandVisualLerp = {};
        clearPrimaryDetachVisualAuthority(firingHandIsLeft);
        restoreFrikPrimaryWeaponPose();
        _hapticEvents.firingGripAttached = true;
        _hapticEvents.firingGripAttachedHandIsLeft = firingHandIsLeft;
        ROCK_LOG_INFO(Weapon, "TwoHandedGrip: firing hand reattached at configured grip");
        return true;
    }

    void TwoHandedGrip::updatePartCarryGrip(
        RE::NiNode* weaponNode,
        float dt,
        const EquippedWeaponGripFrameInput& frameInput,
        const WeaponInteractionContact& leftWeaponContact,
        const WeaponInteractionContact& rightWeaponContact,
        const WeaponCollision& weaponCollision,
        std::uint64_t currentWeaponGenerationKey,
        const WeaponInteractionRuntimeState& leftRuntimeState,
        const WeaponInteractionRuntimeState& rightRuntimeState)
    {
        const bool supportHandIsLeft = !_firingHandIsLeft;
        const bool firingHandIsLeft = _firingHandIsLeft;
        const WeaponInteractionContact& firingHandContact = firingHandIsLeft ? leftWeaponContact : rightWeaponContact;

        /*
         * Firing-grip reattach is the squeeze gesture: a held grab with the
         * free firing palm inside the reattach radius re-takes the grip.
         * Nothing attaches to an open hand, and the gesture cannot re-capture
         * a fresh detach because the detach requires the grab to be open. A
         * hand already part-gripping is never converted; open it first, then
         * squeeze the grip.
         */
        bool reattachRequested = false;
        if (frameInput.reattachEligible && !partGrip(firingHandIsLeft).active) {
            float palmToGripDistance = 0.0f;
            if (tryComputeFiringPalmToGripDistance(weaponNode, palmToGripDistance)) {
                reattachRequested = weapon_two_handed_grip_math::shouldReattachFiringGripOnGrab(
                    frameInput.primaryGripInput.held,
                    palmToGripDistance,
                    g_rockConfig.rockWeaponFiringGripReattachRadius);
                _firingGripReattachHoverInsideRadius = weapon_two_handed_grip_math::isFiringGripReattachHoverCandidate(
                    frameInput.primaryGripInput.held,
                    palmToGripDistance,
                    g_rockConfig.rockWeaponFiringGripReattachRadius);
            }
        }

        if (reattachRequested && tryReattachFiringGrip(weaponNode, firingHandContact)) {
            if (partGrip(supportHandIsLeft).active) {
                _state = TwoHandedState::Gripping;
                updateFullWeaponAuthorityGrip(weaponNode, dt);
            } else {
                transitionToPrimaryOnly(weaponNode, currentWeaponGenerationKey, "part-carry-reattached-firing-grip");
            }
            return;
        }

        bool lastReleaseWasSupportHand = true;

        WeaponPartGrip& supportGrip = partGrip(supportHandIsLeft);
        if (supportGrip.active) {
            if (!providerPartAuthorityStillCurrent(supportGrip, currentWeaponGenerationKey) || !leftRuntimeState.supportGripAllowed) {
                /*
                 * Provider revocation and offhand reservation are policy
                 * changes, not a player release: return the weapon to
                 * FRIK-native carry instead of dropping it.
                 */
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: clearing part-carry authority because provider revoked the support part grip");
                transitionToInactive(false);
                return;
            }
            if (!weapon_two_handed_grip_math::shouldContinueSupportGrip(frameInput.leftGripHeld, frameInput.leftHandHoldingObject)) {
                releasePartGrip(supportHandIsLeft, "support-grip-released");
                lastReleaseWasSupportHand = true;
            }
        }

        WeaponPartGrip& freeHandGrip = partGrip(firingHandIsLeft);
        if (freeHandGrip.active) {
            if (!providerPartAuthorityStillCurrent(freeHandGrip, currentWeaponGenerationKey)) {
                releasePartGrip(firingHandIsLeft, "provider-part-authority-lost");
                lastReleaseWasSupportHand = false;
            } else if (!weapon_two_handed_grip_math::shouldContinueSupportGrip(frameInput.primaryGripInput.held, frameInput.rightHandHoldingObject)) {
                releasePartGrip(firingHandIsLeft, "free-hand-grip-released");
                lastReleaseWasSupportHand = false;
            }
        }

        /*
         * Mid-hold conversion, part-carry flavor: a part grip captured
         * WITHOUT provider authority whose own part now resolves to a
         * matched provider target (consumer armed its whitelist while the
         * hand was already holding — per-hand trigger arming) recaptures
         * immediately under the new resolution. Gated on the OTHER grip
         * holding carry authority: converting the last carry grip to
         * attach-only glue would drop the weapon through the fail-closed
         * all-grips check below. The free hand recaptures in the same
         * update rather than release-to-recapture because its capture path
         * is press-edged; the support hand gets the same treatment for
         * symmetry (no one-frame glue gap).
         */
        if (freeHandGrip.active && !freeHandGrip.providerPartAuthority.active &&
            rightRuntimeState.providerPartAuthority.active &&
            rightRuntimeState.providerPartAuthority.bodyId == freeHandGrip.contactBodyId &&
            weapon_part_grip_report_policy::partGripCountsAsCarry(supportGrip.active, supportGrip.attachOnly)) {
            const WeaponInteractionDecision freeHandDecision = routeWeaponInteraction(firingHandContact, rightRuntimeState);
            if (freeHandDecision.kind == WeaponInteractionKind::SupportGrip) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: recapturing free-hand part grip under newly matched provider weapon-part target");
                releasePartGrip(firingHandIsLeft, "provider-part-target-newly-matched");
                (void)capturePartGrip(firingHandIsLeft, weaponNode, freeHandDecision, weaponCollision, rightRuntimeState.providerPartAuthority);
            }
        }
        if (supportGrip.active && !supportGrip.providerPartAuthority.active &&
            leftRuntimeState.providerPartAuthority.active &&
            leftRuntimeState.providerPartAuthority.bodyId == supportGrip.contactBodyId &&
            weapon_part_grip_report_policy::partGripCountsAsCarry(freeHandGrip.active, freeHandGrip.attachOnly)) {
            const WeaponInteractionDecision supportDecision = routeWeaponInteraction(leftWeaponContact, leftRuntimeState);
            if (supportDecision.kind == WeaponInteractionKind::SupportGrip) {
                ROCK_LOG_INFO(Weapon, "TwoHandedGrip: recapturing support part grip under newly matched provider weapon-part target");
                releasePartGrip(supportHandIsLeft, "provider-part-target-newly-matched");
                (void)capturePartGrip(supportHandIsLeft, weaponNode, supportDecision, weaponCollision, leftRuntimeState.providerPartAuthority);
            }
        }

        if (!freeHandGrip.active && frameInput.primaryGripInput.pressed) {
            const WeaponInteractionDecision freeHandDecision = routeWeaponInteraction(firingHandContact, rightRuntimeState);
            if (weapon_two_handed_grip_math::canStartFreeHandPartGrip(
                    freeHandDecision.kind == WeaponInteractionKind::SupportGrip,
                    frameInput.primaryGripInput.pressed,
                    frameInput.rightHandHoldingObject,
                    freeHandGrip.active)) {
                if (capturePartGrip(firingHandIsLeft, weaponNode, freeHandDecision, weaponCollision, rightRuntimeState.providerPartAuthority)) {
                    /*
                     * AttachOnly keeps its authored source frames so the glued
                     * hand follows provider-driven part motion; it never joins
                     * the carry solve, so it cannot feed part animation back
                     * into the carry (the drift lockPartGripToWeaponRoot
                     * prevents). Separation/blend only matter for a two-anchor
                     * carry, which needs both grips to hold carry authority.
                     */
                    if (freeHandGrip.attachOnly) {
                        ROCK_LOG_INFO(Weapon, "TwoHandedGrip: free-hand part grip captured as provider attach-only glue");
                    } else {
                        lockPartGripToWeaponRoot(firingHandIsLeft);
                        if (weapon_part_grip_report_policy::partGripCountsAsCarry(supportGrip.active, supportGrip.attachOnly)) {
                            _partCarryGripSeparationWorld = partCarryGripSeparation(weaponNode);
                            _rotationBlend = 0.0f;
                        }
                    }
                }
            }
        }

        if (!supportGrip.active) {
            const WeaponInteractionDecision supportDecision = routeWeaponInteraction(leftWeaponContact, leftRuntimeState);
            if (supportDecision.kind == WeaponInteractionKind::SupportGrip &&
                weapon_two_handed_grip_math::canStartSupportGrip(true, frameInput.leftGripHeld, frameInput.leftHandHoldingObject)) {
                if (capturePartGrip(supportHandIsLeft, weaponNode, supportDecision, weaponCollision, leftRuntimeState.providerPartAuthority)) {
                    // Symmetric to the free-hand capture above: attach-only
                    // glue keeps source frames and stays out of the carry.
                    if (supportGrip.attachOnly) {
                        ROCK_LOG_INFO(Weapon, "TwoHandedGrip: support-hand part grip captured as provider attach-only glue");
                    } else {
                        lockPartGripToWeaponRoot(supportHandIsLeft);
                        if (weapon_part_grip_report_policy::partGripCountsAsCarry(freeHandGrip.active, freeHandGrip.attachOnly)) {
                            _partCarryGripSeparationWorld = partCarryGripSeparation(weaponNode);
                            _rotationBlend = 0.0f;
                        }
                    }
                }
            }
        }

        /*
         * Only carry-authority grips can hold the weapon. When the last carry
         * grip releases, a remaining AttachOnly glue cannot inherit pivot
         * authority (never upgrade), so it releases with the carry and the
         * normal manual-drop request proceeds (fail closed).
         */
        if (!weapon_part_grip_report_policy::partGripCountsAsCarry(supportGrip.active, supportGrip.attachOnly) &&
            !weapon_part_grip_report_policy::partGripCountsAsCarry(freeHandGrip.active, freeHandGrip.attachOnly)) {
            releasePartGrip(supportHandIsLeft, "carry-authority-lost");
            releasePartGrip(firingHandIsLeft, "carry-authority-lost");
            requestEquippedWeaponDrop(
                "part-carry-all-grips-released",
                lastReleaseWasSupportHand ?
                    (supportHandIsLeft ? equipped_weapon_drop_policy::SourceHand::Left : equipped_weapon_drop_policy::SourceHand::Right) :
                    (firingHandIsLeft ? equipped_weapon_drop_policy::SourceHand::Left : equipped_weapon_drop_policy::SourceHand::Right));
            return;
        }

        // The pivot must always be a carry-authority grip; after the check
        // above, the other hand is guaranteed to hold one.
        if (!weapon_part_grip_report_policy::partGripCountsAsCarry(
                partGrip(_partCarryPivotIsLeft).active,
                partGrip(_partCarryPivotIsLeft).attachOnly)) {
            _partCarryPivotIsLeft = !_partCarryPivotIsLeft;
        }

        (void)solvePartCarryWeaponAuthority(weaponNode, dt);
    }

    float TwoHandedGrip::partCarryGripSeparation(RE::NiNode* weaponNode) const
    {
        const RE::NiPoint3 leftGripWorld = resolvePartGripWorld(partGrip(true), weaponNode);
        const RE::NiPoint3 rightGripWorld = resolvePartGripWorld(partGrip(false), weaponNode);
        const RE::NiPoint3 delta = sub(leftGripWorld, rightGripWorld);
        const float separation = std::sqrt(dot(delta, delta));
        return std::isfinite(separation) ? separation : 0.0f;
    }

    bool TwoHandedGrip::solvePartCarryWeaponAuthority(RE::NiNode* weaponNode, float dt)
    {
        const bool pivotIsLeft = _partCarryPivotIsLeft;
        const WeaponPartGrip& pivotGrip = partGrip(pivotIsLeft);
        const WeaponPartGrip& aimGrip = partGrip(!pivotIsLeft);
        // An AttachOnly glue never aims the weapon; the carry solves
        // single-anchor around the pivot and the glue publishes afterwards.
        const bool aimGripCarries = weapon_part_grip_report_policy::partGripCountsAsCarry(aimGrip.active, aimGrip.attachOnly);
        if (!pivotGrip.active || !pivotGrip.hasHandWeaponLocal) {
            _hasSolvedWeaponTransform = false;
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because captured hand frames are unavailable");
            transitionToInactive(false);
            return false;
        }

        RE::NiTransform pivotHandTransform{};
        if (!tryGetHandBoneTransform(pivotIsLeft, pivotHandTransform)) {
            _hasSolvedWeaponTransform = false;
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because root flattened hand transforms are unavailable");
            transitionToInactive(false);
            return false;
        }

        if (aimGripCarries) {
            RE::NiTransform aimHandTransform{};
            if (!tryGetHandBoneTransform(!pivotIsLeft, aimHandTransform)) {
                _hasSolvedWeaponTransform = false;
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because aim hand transform is unavailable");
                transitionToInactive(false);
                return false;
            }

            _rotationBlend = (std::min)(1.0f, _rotationBlend + dt * ROTATION_BLEND_SPEED);

            /*
             * The two-anchor solve must be closed over the captured
             * weapon-root-local grip points. Part-carry feeds its own solved
             * transform back as the next frame's base, so re-resolving grips
             * through live part-node chains lets any per-frame part animation
             * integrate into a steady carry drift (verified by telemetry:
             * rigid-weapon grip separation grew frame over frame). The
             * trade-off is that a two-anchor carry does not follow externally
             * driven part motion; provider part revocation releases the grip
             * in that case.
             */
            const RE::NiPoint3 pivotPalm = computeGrabLegacyPalmPivotAWorldFromHandBasis(pivotHandTransform, pivotIsLeft);
            const RE::NiPoint3 aimPalm = computeGrabLegacyPalmPivotAWorldFromHandBasis(aimHandTransform, !pivotIsLeft);
            const RE::NiPoint3 currentAimGripWorld = weaponLocalToWorld(aimGrip.gripLocal, weaponNode);
            const RE::NiPoint3 currentPivotGripWorld = weaponLocalToWorld(pivotGrip.gripLocal, weaponNode);
            const RE::NiPoint3 currentSeparationDelta = sub(currentAimGripWorld, currentPivotGripWorld);
            const float currentSeparation = std::sqrt(dot(currentSeparationDelta, currentSeparationDelta));
            const float lockedSeparation = _partCarryGripSeparationWorld > 0.0f ? _partCarryGripSeparationWorld : currentSeparation;
            const RE::NiPoint3 lockedAimTarget = makeLockedSupportGripTarget(
                pivotPalm,
                aimPalm,
                currentAimGripWorld,
                lockedSeparation,
                0.001f);
            const RE::NiPoint3 blendedAimTarget = lerpPoint(currentAimGripWorld, lockedAimTarget, _rotationBlend);

            WeaponTwoHandedSolverInput<RE::NiTransform, RE::NiPoint3> solverInput{};
            solverInput.weaponWorldTransform = weaponNode->world;
            solverInput.primaryGripLocal = pivotGrip.gripLocal;
            solverInput.supportGripLocal = aimGrip.gripLocal;
            solverInput.primaryTargetWorld = pivotPalm;
            solverInput.supportTargetWorld = blendedAimTarget;
            solverInput.supportNormalLocal = aimGrip.normalLocal;
            solverInput.supportNormalTargetWorld = computePalmNormalFromHandBasis(aimHandTransform, !pivotIsLeft);
            solverInput.useSupportNormalTwist = true;
            solverInput.supportNormalTwistFactor = SUPPORT_NORMAL_TWIST_FACTOR;

            const auto solved = solveTwoHandedWeaponTransformFrikPivot(solverInput);
            if (!solved.solved) {
                return true;
            }

            // Break the rotation feedback loop's orthonormality decay before
            // the solved transform becomes next frame's base.
            RE::NiTransform stabilizedWeaponWorld = solved.weaponWorldTransform;
            stabilizedWeaponWorld.rotate = orthonormalizeStoredRotation(stabilizedWeaponWorld.rotate);

            if (!applyWeaponVisualAuthority(weaponNode, stabilizedWeaponWorld)) {
                _hasSolvedWeaponTransform = false;
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because ROCK visual weapon authority failed");
                transitionToInactive(false);
                return false;
            }

            static_assert(weapon_visual_authority_math::handPosePrecedesLockedHandAuthority());
            static_assert(weapon_visual_authority_math::weaponVisualPrecedesLockedHandAuthority());
            publishGripHandPoses(pivotIsLeft);
            publishGripHandPoses(!pivotIsLeft);

            if (!applyPartGripLockedVisual(pivotIsLeft, weaponNode, dt, &pivotHandTransform) ||
                !applyPartGripLockedVisual(!pivotIsLeft, weaponNode, dt, &aimHandTransform)) {
                _hasSolvedWeaponTransform = false;
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because ROCK part grip hand authority failed");
                transitionToInactive(false);
                return false;
            }
        } else {
            RE::NiTransform solvedWeaponWorld{};
            if (pivotGrip.hasSourceFrames && pivotGrip.hasAttachmentWeaponLocal && resolveCurrentSupportAttachmentRoot(pivotGrip, weaponNode)) {
                const RE::NiTransform solvedSourceWorld =
                    transform_math::composeTransforms(pivotHandTransform, transform_math::invertTransform(pivotGrip.handSourceLocal));
                solvedWeaponWorld = transform_math::composeTransforms(solvedSourceWorld, transform_math::invertTransform(pivotGrip.attachmentWeaponLocal));
            } else {
                solvedWeaponWorld = transform_math::composeTransforms(pivotHandTransform, transform_math::invertTransform(pivotGrip.handWeaponLocal));
            }
            if (!isFiniteTransform(solvedWeaponWorld)) {
                _hasSolvedWeaponTransform = false;
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because single-anchor weapon solve produced invalid transform");
                transitionToInactive(false);
                return false;
            }

            if (!applyWeaponVisualAuthority(weaponNode, solvedWeaponWorld)) {
                _hasSolvedWeaponTransform = false;
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because ROCK visual weapon authority failed");
                transitionToInactive(false);
                return false;
            }

            static_assert(weapon_visual_authority_math::handPosePrecedesLockedHandAuthority());
            publishGripHandPoses(pivotIsLeft);
            if (!applyPartGripLockedVisual(pivotIsLeft, weaponNode, dt, &pivotHandTransform)) {
                _hasSolvedWeaponTransform = false;
                ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing part-carry grip because ROCK part grip hand authority failed");
                transitionToInactive(false);
                return false;
            }
        }

        /*
         * AttachOnly glue publishes after the weapon solve so it composes from
         * this frame's part transforms (including provider part drives applied
         * earlier in the frame). A glue visual failure only loses the attach;
         * the carry pivot must survive it.
         */
        if (aimGrip.active && aimGrip.attachOnly) {
            RE::NiTransform attachHandTransform{};
            const RE::NiTransform* liveAttachHandWorld =
                tryGetHandBoneTransform(!pivotIsLeft, attachHandTransform) ? &attachHandTransform : nullptr;
            publishGripHandPoses(!pivotIsLeft);
            if (!applyPartGripLockedVisual(!pivotIsLeft, weaponNode, dt, liveAttachHandWorld)) {
                releasePartGrip(!pivotIsLeft, "attach-only-visual-authority-failed");
            }
        }

        _lastSolvedWeaponTransform = weaponNode->world;
        _hasSolvedWeaponTransform = true;

        if (++_gripLogCounter >= 90) {
            _gripLogCounter = 0;
            const RE::NiPoint3 pivotGripFinal = resolvePartGripWorld(pivotGrip, weaponNode);
            ROCK_LOG_DEBUG(Weapon,
                "TwoHandedGrip: part-carry authority pivot={} anchors={} pivotGrip=({:.1f},{:.1f},{:.1f}) handLerp=({:.2f}/{:.3f}s,{:.2f}/{:.3f}s)",
                pivotIsLeft ? "left" : "right",
                aimGrip.active ? 2 : 1,
                pivotGripFinal.x,
                pivotGripFinal.y,
                pivotGripFinal.z,
                pivotGrip.visualLerp.lastAlpha,
                pivotGrip.visualLerp.durationSeconds,
                aimGrip.visualLerp.lastAlpha,
                aimGrip.visualLerp.durationSeconds);
        }
        return true;
    }

    void TwoHandedGrip::updateVisualOnlySupportGrip(RE::NiNode* weaponNode, float dt)
    {
        const bool supportHandIsLeft = !_firingHandIsLeft;

        static_assert(weapon_visual_authority_math::handPosePrecedesLockedHandAuthority());
        publishGripHandPoses(supportHandIsLeft);

        RE::NiTransform supportTransform{};
        const RE::NiTransform* liveSupportTransform = tryGetHandBoneTransform(supportHandIsLeft, supportTransform) ? &supportTransform : nullptr;
        if (!applyLockedHandVisualAuthority(weaponNode, false, true, dt, nullptr, liveSupportTransform)) {
            _hasSolvedWeaponTransform = false;
            ROCK_LOG_WARN(Weapon, "TwoHandedGrip: clearing visual-only support grip because ROCK support hand authority failed");
            transitionToInactive(false);
            return;
        }

        _lastSolvedWeaponTransform = weaponNode ? weaponNode->world : RE::NiTransform{};
        _hasSolvedWeaponTransform = false;

        if (weaponNode && ++_gripLogCounter >= 90) {
            _gripLogCounter = 0;
            const WeaponPartGrip& supportGrip = partGrip(supportHandIsLeft);
            const RE::NiPoint3 offhandGripFinal = resolvePartGripWorld(supportGrip, weaponNode);
            ROCK_LOG_DEBUG(Weapon,
                "TwoHandedGrip: visual-only support follows weapon='{}', offhandGrip=({:.1f},{:.1f},{:.1f}), handLerp={:.2f}/{:.3f}s",
                weaponNode->name.c_str(),
                offhandGripFinal.x,
                offhandGripFinal.y,
                offhandGripFinal.z,
                supportGrip.visualLerp.lastAlpha,
                supportGrip.visualLerp.durationSeconds);
        }
    }

    void TwoHandedGrip::setSupportGripPose(bool isLeft, WeaponGripPoseId poseId, const grab_finger_pose_runtime::SolvedGrabFingerPose* meshFingerPose)
    {
        WeaponPartGrip& grip = partGrip(isLeft);
        if (meshFingerPose && meshFingerPose->solved) {
            grip.fingerPose = meshFingerPose->hasJointValues ? meshFingerPose->jointValues : grab_finger_pose_math::expandFingerCurlsToJointValues(meshFingerPose->values);
            grip.fingerSplayRadians = {};
            grip.hasFingerSplay = grab_finger_pose_runtime::resolveSurfaceContactSplayValues(isLeft, *meshFingerPose, grip.fingerSplayRadians);
            grip.hasFingerPose = true;
            return;
        }

        const auto& poseValues = poseValuesForGrip(poseId);
        grip.fingerPose = poseValues;
        grip.fingerSplayRadians = {};
        grip.hasFingerPose = true;
        grip.hasFingerSplay = false;
    }

    void TwoHandedGrip::clearSupportGripPose(bool isLeft)
    {
        WeaponPartGrip& grip = partGrip(isLeft);
        grip.fingerPose = {};
        grip.fingerSplayRadians = {};
        grip.hasFingerPose = false;
        grip.hasFingerSplay = false;
        grip.fingerLocalTransforms = {};
        grip.fingerLocalTransformMask = 0;
        grip.hasFingerLocalTransforms = false;

        (void)frik_visual_authority::clearHandPose(SUPPORT_GRIP_TAG, handFromBool(isLeft));
        (void)frik_visual_authority::clearExternalHandWorldTransform(SUPPORT_GRIP_TAG, handFromBool(isLeft));
        (void)frik_visual_authority::clearExternalHandWorldTransform(REACH_LIMITED_SUPPORT_GRIP_TAG, handFromBool(isLeft));
    }

    // SEH backstop (Jul 6, kept through the Jul-8 upstream merge): the liveness check in update()
    // is the primary crash fix; this contains the raw scene-graph writes so a subtree free that
    // RACES this write (worker thread) or slips between the check and the write degrades to a
    // caught failure (caller drops the grip) instead of a hard AV inside updateTransformsDown's
    // child walk. Per the project SEH rule, the __try body holds NO destructor-bearing locals
    // (NiTransform is trivially destructible).
    static bool sehApplyWeaponVisualAuthorityRaw(RE::NiNode* weaponNode, const RE::NiTransform& solvedWeaponWorld)
    {
        __try {
            if (weaponNode->parent) {
                weaponNode->local = weapon_visual_authority_math::worldTargetToParentLocal(weaponNode->parent->world, solvedWeaponWorld);
                f4vr::updateTransformsDown(weaponNode, true);
            } else {
                weaponNode->local = solvedWeaponWorld;
                weaponNode->world = solvedWeaponWorld;
                f4vr::updateTransformsDown(weaponNode, false);
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // EMBED (Jul 19): raw SEH body for computeLiveGripHandWorld — no destructor-bearing
    // locals inside __try (project SEH rule; NiTransform is trivially destructible).
    static bool sehComputeGripHandWorldRaw(const RE::NiNode* weaponNode, const RE::NiTransform* handWeaponLocal, RE::NiTransform* out)
    {
        __try {
            *out = weapon_visual_authority_math::weaponLocalFrameToWorld(weaponNode->world, *handWeaponLocal);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Source-frame twin of the helper above. The attachment pointer is cached across
    // frames, so both the subtree membership walk and the selected node-world read must
    // live inside the same SEH leaf. A weapon rebuild racing this query then fails the
    // live refresh instead of taking the game down.
    static bool sehComputePartGripHandWorldRaw(
        RE::NiNode* weaponNode,
        bool hasSourceFrames,
        RE::NiAVObject* attachmentRoot,
        const RE::NiTransform* handSourceLocal,
        const RE::NiTransform* handWeaponLocal,
        RE::NiTransform* out)
    {
        __try {
            if (hasSourceFrames && attachmentRoot &&
                actor_equipment_grab::nodeContainsNode(weaponNode, attachmentRoot, 64)) {
                *out = transform_math::composeTransforms(attachmentRoot->world, *handSourceLocal);
            } else {
                *out = weapon_support_authority_policy::buildVisualOnlySupportHandWorld(
                    weaponNode->world, *handWeaponLocal);
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool TwoHandedGrip::getSupportFingerCurls(bool isLeft, float outCurls[5]) const
    {
        const auto& grip = partGrip(isLeft);
        if (!grip.active) {
            return false;
        }
        bool any = false;
        for (int f = 0; f < 5; ++f) {
            const float prox = grip.fingerPose[f * 3 + 0];
            const float mid = grip.fingerPose[f * 3 + 1];
            const float dist = grip.fingerPose[f * 3 + 2];
            outCurls[f] = (prox + mid + dist) / 3.0f;
            if (outCurls[f] != 0.0f) {
                any = true;
            }
        }
        return any;  // all-zero pose = never populated (cleared on release)
    }

    bool TwoHandedGrip::computeLiveGripHandWorld(bool isLeft, RE::NiTransform& out) const
    {
        if (!_activeWeaponNode) {
            return false;
        }

        const auto applyPublishedLerp = [](const RE::NiTransform& freshTarget,
                                            const LockedHandVisualLerpState& state) {
            // The ROCK update already advanced this state. Recompose against the current
            // weapon/source frame using the published alpha without advancing time again.
            if (g_rockConfig.rockWeaponSupportGripHandLerpEnabled && state.active) {
                return hand_visual_lerp_math::interpolateTransform(
                    state.startWorld, freshTarget, state.lastAlpha);
            }
            return freshTarget;
        };

        // Support/part grip for this hand?
        const WeaponPartGrip& grip = partGrip(isLeft);
        if (grip.active && grip.hasHandWeaponLocal) {
            RE::NiTransform freshTarget{};
            if (!sehComputePartGripHandWorldRaw(
                    _activeWeaponNode,
                    grip.hasSourceFrames,
                    grip.attachmentRoot,
                    &grip.handSourceLocal,
                    &grip.handWeaponLocal,
                    &freshTarget)) {
                return false;
            }
            if (!isFiniteTransform(freshTarget)) {
                return false;
            }
            out = applyPublishedLerp(freshTarget, grip.visualLerp);
            return isFiniteTransform(out);
        }
        // Firing/primary hand?
        if (_hasFiringHandWeaponLocal && _firingHandIsLeft == isLeft) {
            // Return the transform that was ACTUALLY published this frame rather than recomputing
            // the raw rigid weld. ApplyWinners re-derives through here for "ROCK_Weapon*" tags, so
            // recomputing would discard the wrist-follow blend applied in
            // applyFiringHandLockedVisual and silently restore the 1:1 weld every frame.
            if (_hasPublishedFiringHandWorld && isFiniteTransform(_publishedFiringHandWorld)) {
                out = _publishedFiringHandWorld;
                return true;
            }
            RE::NiTransform freshTarget{};
            if (!sehComputeGripHandWorldRaw(_activeWeaponNode, &_primaryHandWeaponLocal, &freshTarget)) {
                return false;
            }
            out = applyPublishedLerp(freshTarget, _primaryHandVisualLerp);
            return isFiniteTransform(out);
        }
        return false;
    }

    bool TwoHandedGrip::rebindAttachOnlyGripToControlledRoot(
        bool isLeft,
        RE::NiNode* weaponNode,
        RE::NiAVObject* controlledRoot)
    {
        WeaponPartGrip& grip = partGrip(isLeft);
        if (!weaponNode ||
            weaponNode != _activeWeaponNode ||
            !controlledRoot ||
            !controlledRoot->parent ||
            !grip.active ||
            !grip.attachOnly ||
            !grip.hasHandWeaponLocal ||
            !actor_equipment_grab::nodeContainsNode(
                weaponNode,
                controlledRoot,
                64)) {
            return false;
        }

        if (grip.hasSourceFrames && grip.attachmentRoot == controlledRoot) {
            return true;
        }

        /*
         * Preserve the exact current hand/seat pose while changing reference
         * frames. The constraint has not moved controlledRoot yet, so the
         * first non-zero rail displacement moves the node and glued hand by
         * the same delta instead of snapping either one during this rebind.
         */
        const RE::NiTransform currentHandWorld =
            resolvePartGripHandWorld(grip, weaponNode);
        const RE::NiPoint3 currentGripWorld =
            resolvePartGripWorld(grip, weaponNode);
        const RE::NiPoint3 currentNormalWeaponLocal =
            resolvePartGripNormalWeaponLocal(grip, weaponNode);
        const RE::NiPoint3 currentNormalWorld =
            transform_math::localVectorToWorld(
                weaponNode->world,
                currentNormalWeaponLocal);
        if (!isFiniteTransform(currentHandWorld) ||
            !std::isfinite(currentGripWorld.x) ||
            !std::isfinite(currentGripWorld.y) ||
            !std::isfinite(currentGripWorld.z) ||
            !std::isfinite(currentNormalWorld.x) ||
            !std::isfinite(currentNormalWorld.y) ||
            !std::isfinite(currentNormalWorld.z)) {
            return false;
        }

        RE::NiAVObject* previousAttachmentRoot = grip.attachmentRoot;
        grip.attachmentRoot = controlledRoot;
        grip.gripSourceLocal =
            transform_math::worldPointToLocal(
                controlledRoot->world,
                currentGripWorld);
        grip.normalSourceLocal =
            transform_math::worldVectorToLocal(
                controlledRoot->world,
                currentNormalWorld);
        grip.handSourceLocal =
            transform_math::composeTransforms(
                transform_math::invertTransform(controlledRoot->world),
                currentHandWorld);
        grip.attachmentWeaponLocal =
            transform_math::composeTransforms(
                transform_math::invertTransform(weaponNode->world),
                controlledRoot->world);
        grip.hasSourceFrames = true;
        grip.hasAttachmentWeaponLocal = true;
        grip.providerPartAuthority.controlledRoot =
            reinterpret_cast<std::uintptr_t>(controlledRoot);

        ROCK_LOG_INFO(Weapon,
            "Provider AttachOnly hand rebound to controlledRoot hand={} "
            "bodyId={} previousRoot=0x{:X} controlledRoot=0x{:X}",
            isLeft ? "left" : "right",
            grip.contactBodyId,
            reinterpret_cast<std::uintptr_t>(previousAttachmentRoot),
            reinterpret_cast<std::uintptr_t>(controlledRoot));
        return true;
    }

    bool TwoHandedGrip::republishAttachOnlyHandAfterControlledRootMove(
        bool isLeft)
    {
        const WeaponPartGrip& grip = partGrip(isLeft);
        if (!grip.active ||
            !grip.attachOnly ||
            !grip.hasHandWeaponLocal ||
            !frik_visual_authority::isAvailable()) {
            return false;
        }

        RE::NiTransform postMotionHandWorld{};
        if (!computeLiveGripHandWorld(isLeft, postMotionHandWorld) ||
            !isFiniteTransform(postMotionHandWorld)) {
            return false;
        }

        /*
         * Overwrite the pre-constraint writer published by
         * updateVisualOnlySupportGrip(). In the embedded pre-v5 bridge this is
         * consumed by HandAuthority::ApplyWinners later in the same frame; in
         * native FRIK v5 it refreshes the external hand target directly.
         */
        return frik_visual_authority::applyExternalHandWorldTransform(
            SUPPORT_GRIP_TAG,
            handFromBool(isLeft),
            postMotionHandWorld,
            GRIP_HAND_POSE_PRIORITY);
    }

    bool TwoHandedGrip::applyWeaponVisualAuthority(RE::NiNode* weaponNode, const RE::NiTransform& solvedWeaponWorld)
    {
        if (!weaponNode) {
            return false;
        }
        return sehApplyWeaponVisualAuthorityRaw(weaponNode, solvedWeaponWorld);
    }

    bool TwoHandedGrip::applyFiringHandLockedVisual(
        RE::NiNode* weaponNode,
        float dt,
        const RE::NiTransform* liveHandWorld,
        bool forceExact)
    {
        if (!weaponNode || !_hasFiringHandWeaponLocal) {
            return false;
        }
        if (!frik_visual_authority::isAvailable()) {
            return false;
        }

        RE::NiTransform firingHandWorld =
            weapon_visual_authority_math::weaponLocalFrameToWorld(weaponNode->world, _primaryHandWeaponLocal);

        // Relax the 1:1 weapon->wrist weld toward the player's real controller orientation. Only
        // possible when we actually know where the live hand is; otherwise keep the legacy weld.
        if (liveHandWorld) {
            firingHandWorld = two_handed_weapon_policy::blendFiringWristTowardController(
                firingHandWorld,
                *liveHandWorld,
                g_rockConfig.rockTwoHandedFiringWristFollowFactor);
            firingHandWorld.rotate = orthonormalizeStoredRotation(firingHandWorld.rotate);
        }

        const RE::NiTransform appliedFiringHandWorld =
            resolveLockedHandVisualTarget(firingHandWorld, liveHandWorld, dt, _primaryHandVisualLerp, forceExact);

        // SINGLE SOURCE OF TRUTH. HandAuthority::ApplyWinners discards the transform published here
        // for any "ROCK_Weapon*" tag and re-derives it via HostGetLiveGripHandWorld ->
        // computeLiveGripHandWorld, which recomputed the RAW weld — so a change made only here was
        // silently overwritten every frame (the same two-sites-disagree failure that made an
        // earlier fix a no-op). Cache what was actually published and hand that exact transform
        // back on re-derivation instead of recomputing it.
        _publishedFiringHandWorld = appliedFiringHandWorld;
        _hasPublishedFiringHandWorld = true;

        return frik_visual_authority::applyExternalHandWorldTransform(
            PRIMARY_GRIP_TAG, handFromBool(_firingHandIsLeft), appliedFiringHandWorld, GRIP_HAND_POSE_PRIORITY);
    }

    bool TwoHandedGrip::applyPartGripLockedVisual(
        bool isLeft,
        RE::NiNode* weaponNode,
        float dt,
        const RE::NiTransform* liveHandWorld,
        bool forceExact)
    {
        WeaponPartGrip& grip = partGrip(isLeft);
        if (!weaponNode || !grip.active || !grip.hasHandWeaponLocal) {
            return false;
        }
        if (!frik_visual_authority::isAvailable()) {
            return false;
        }

        const RE::NiTransform partGripHandWorld = resolvePartGripHandWorld(grip, weaponNode);
        const RE::NiTransform appliedHandWorld =
            resolveLockedHandVisualTarget(partGripHandWorld, liveHandWorld, dt, grip.visualLerp, forceExact);
        const char* authorityTag = forceExact ? REACH_LIMITED_SUPPORT_GRIP_TAG : SUPPORT_GRIP_TAG;
        return frik_visual_authority::applyExternalHandWorldTransform(
            authorityTag, handFromBool(isLeft), appliedHandWorld, GRIP_HAND_POSE_PRIORITY);
    }

    bool TwoHandedGrip::applyLockedHandVisualAuthority(
        RE::NiNode* weaponNode,
        bool applyPrimaryHand,
        bool applySupportHand,
        float dt,
        const RE::NiTransform* livePrimaryHandWorld,
        const RE::NiTransform* liveSupportHandWorld,
        bool forceExact)
    {
        if (!weaponNode) {
            return false;
        }

        if (!frik_visual_authority::isAvailable()) {
            return false;
        }

        if (!applyPrimaryHand && !applySupportHand) {
            return true;
        }

        const bool supportHandIsLeft = !_firingHandIsLeft;
        bool primaryApplied = true;
        bool supportApplied = true;
        if (applyPrimaryHand) {
            primaryApplied = applyFiringHandLockedVisual(weaponNode, dt, livePrimaryHandWorld, forceExact);
        }
        if (applySupportHand) {
            supportApplied = applyPartGripLockedVisual(
                supportHandIsLeft, weaponNode, dt, liveSupportHandWorld, forceExact);
        }
        if (primaryApplied && supportApplied) {
            return true;
        }

        if (applyPrimaryHand && primaryApplied) {
            (void)frik_visual_authority::clearExternalHandWorldTransform(PRIMARY_GRIP_TAG, handFromBool(_firingHandIsLeft));
        }
        if (applySupportHand && supportApplied) {
            const char* authorityTag = forceExact ? REACH_LIMITED_SUPPORT_GRIP_TAG : SUPPORT_GRIP_TAG;
            (void)frik_visual_authority::clearExternalHandWorldTransform(authorityTag, handFromBool(supportHandIsLeft));
        }
        return false;
    }

    void TwoHandedGrip::publishGripHandPoses(bool isLeft)
    {
        if (!frik_visual_authority::isAvailable()) {
            return;
        }

        const WeaponPartGrip& grip = partGrip(isLeft);
        if (weapon_visual_authority_math::shouldPublishTwoHandedGripPose(weapon_visual_authority_math::LockedHandRole::Support) && grip.hasFingerPose) {
            const auto handPose = grip.hasFingerSplay ?
                frik_visual_authority::makeHandPoseDataFromJointValues(grip.fingerPose, grip.fingerSplayRadians) :
                frik_visual_authority::makeHandPoseDataFromJointValues(grip.fingerPose);
            (void)frik_visual_authority::setHandPoseCustomWithPriority(
                SUPPORT_GRIP_TAG,
                handFromBool(isLeft),
                handPose,
                GRIP_HAND_POSE_PRIORITY);
        }

        if (grip.hasFingerLocalTransforms) {
            frik_visual_authority::FingerLocalTransformOverride overrideData{};
            overrideData.enabledMask = grip.fingerLocalTransformMask;
            for (std::size_t i = 0; i < grip.fingerLocalTransforms.size(); ++i) {
                overrideData.localTransforms[i] = grip.fingerLocalTransforms[i];
            }
            (void)frik_visual_authority::setHandPoseCustomLocalTransformsWithPriority(SUPPORT_GRIP_TAG, handFromBool(isLeft), &overrideData, GRIP_HAND_POSE_PRIORITY);
        }
    }

    void TwoHandedGrip::clearPrimaryGripPose(bool isLeft)
    {
        (void)frik_visual_authority::clearHandPose(PRIMARY_GRIP_TAG, handFromBool(isLeft));
        (void)frik_visual_authority::clearExternalHandWorldTransform(PRIMARY_GRIP_TAG, handFromBool(isLeft));
    }

    void TwoHandedGrip::clearPrimaryDetachVisualAuthority(bool isLeft)
    {
        (void)frik_visual_authority::clearHandPose(PRIMARY_DETACH_TAG, handFromBool(isLeft));
        (void)frik_visual_authority::clearExternalHandWorldTransform(PRIMARY_DETACH_TAG, handFromBool(isLeft));
    }

    void TwoHandedGrip::killFrikOffhandGrip()
    {
        if (frik_visual_authority::blockOffHandWeaponGripping("ROCK_TwoHanded", true)) {
            ROCK_LOG_DEBUG(Weapon, "FRIK offhand grip suppressed");
        }
    }

    void TwoHandedGrip::restoreFrikOffhandGrip()
    {
        if (frik_visual_authority::blockOffHandWeaponGripping("ROCK_TwoHanded", false)) {
            ROCK_LOG_DEBUG(Weapon, "FRIK offhand grip restored");
        }
    }

    bool TwoHandedGrip::blockFrikPrimaryWeaponPose()
    {
        if (frik_visual_authority::blockPrimaryHandWeaponPose("ROCK_PrimaryDetach", true)) {
            ROCK_LOG_DEBUG(Weapon, "FRIK primary weapon pose suppressed");
            return true;
        }
        return false;
    }

    void TwoHandedGrip::restoreFrikPrimaryWeaponPose()
    {
        if (frik_visual_authority::blockPrimaryHandWeaponPose("ROCK_PrimaryDetach", false)) {
            ROCK_LOG_DEBUG(Weapon, "FRIK primary weapon pose restored");
        }
    }

}
