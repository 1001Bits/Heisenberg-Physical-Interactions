#include "physics-interaction/grab/TouchGrabBridge.h"

#include "physics-interaction/grab/MeshGrab.h"
#include "physics-interaction/grab/GrabFinger.h"
#include "physics-interaction/hand/HandSkeleton.h"
#include "physics-interaction/PhysicsLog.h"
#include "physics-interaction/visual/FrikVisualAuthorityBridge.h"
#include "RockConfig.h"

namespace rock::touch_grab_bridge
{
    namespace
    {
        constexpr const char* TOUCH_GRAB_HAND_POSE_TAG = "ROCK_Grab";
        constexpr int TOUCH_GRAB_HAND_POSE_PRIORITY = 100;

        struct RichTouchGrabPublishState
        {
            std::array<float, 15> currentJointPose{};
            bool hasCurrentJointPose = false;
            grab_finger_local_transform_runtime::State localTransforms{};
            RE::NiAVObject* objectRoot = nullptr;
            bool posePublished = false;
        };

        std::array<RichTouchGrabPublishState, 2> g_richTouchGrabPublishStates{};

        [[nodiscard]] RichTouchGrabPublishState& richPublishState(bool isLeft)
        {
            return g_richTouchGrabPublishStates[isLeft ? 0 : 1];
        }

        [[nodiscard]] bool clearRichTouchGrabFingerPose(bool isLeft)
        {
            auto& state = richPublishState(isLeft);
            grab_finger_local_transform_runtime::clearLocalTransformOverride(
                TOUCH_GRAB_HAND_POSE_TAG,
                frik_visual_authority::handFromBool(isLeft),
                TOUCH_GRAB_HAND_POSE_PRIORITY,
                state.localTransforms);
            state = {};
            return frik_visual_authority::clearHandPose(
                TOUCH_GRAB_HAND_POSE_TAG,
                frik_visual_authority::handFromBool(isLeft));
        }

        [[nodiscard]] bool solveRichTouchGrabFingerPose(
            RE::NiAVObject* objectRoot,
            const RE::NiTransform& handWorld,
            bool isLeft,
            const RE::NiPoint3& grabAnchorWorld,
            const RE::NiPoint3& grabContactWorld,
            grab_finger_pose_runtime::SolvedGrabFingerPose& outPose)
        {
            outPose = {};
            if (!objectRoot) {
                return false;
            }

            std::vector<TriangleData> triangles;
            extractAllTriangles(objectRoot, triangles);
            if (triangles.empty()) {
                return false;
            }

            root_flattened_finger_skeleton_runtime::Snapshot liveSnapshot{};
            if (!root_flattened_finger_skeleton_runtime::resolveLiveFingerSkeletonSnapshot(isLeft, liveSnapshot)) {
                return false;
            }

            // Preserve the established touch-grab geometry contract: the
            // contact proves acquisition, while each calibrated finger sweeps
            // from its own live base against the complete visible mesh.
            (void)grabContactWorld;
            grab_finger_pose_runtime::GrabFingerPoseTargetSet targets{};
            targets.seatPointWorld = grabAnchorWorld;
            targets.seatPointValid = true;
            targets.useSeatPointForMissingTargets = false;
            targets.useWholeMeshForMissingTargets = true;

            outPose = grab_finger_pose_runtime::solveGrabFingerPoseFromTriangles(
                triangles,
                handWorld,
                isLeft,
                grabAnchorWorld,
                targets,
                0.0f,
                900.0f,
                true,
                &liveSnapshot,
                false,
                0.0f,
                true,
                g_rockConfig.rockGrabFingerSweepContactRadiusGameUnits,
                -1.0f,
                g_rockConfig.rockGrabThumbSweepMaxOpenValue,
                g_rockConfig.rockGrabFingerSweepMaxOpenValue);

            if (!outPose.solved || !outPose.hasJointValues || outPose.hitCount == 0) {
                return false;
            }

            // Feed the post-solve pad evidence into the same contact targets
            // used by surface splay and thumb-local correction publication.
            std::array<grab_finger_pose_runtime::FingerPadSurfaceEvidence, 5> padEvidence{};
            (void)grab_finger_pose_runtime::refineGrabFingerPoseWithPadProbes(
                outPose,
                triangles,
                targets,
                liveSnapshot,
                objectRoot->world,
                true,
                true,
                padEvidence,
                true);
            grab_finger_pose_runtime::captureSurfaceAimObjectLocal(
                outPose,
                objectRoot->world);
            return true;
        }
    }

    bool SolveTouchGrabFingerPose(
        RE::NiAVObject* objectRoot,
        const RE::NiTransform& handWorld,
        bool isLeft,
        const RE::NiPoint3& grabAnchorWorld,
        const RE::NiPoint3& grabContactWorld,
        std::array<float, 15>& outJointCurls)
    {
        outJointCurls = {};
        if (!objectRoot) {
            return false;
        }

        std::vector<TriangleData> triangles;
        extractAllTriangles(objectRoot, triangles);
        if (triangles.empty()) {
            return false;
        }

        root_flattened_finger_skeleton_runtime::Snapshot liveSnapshot{};
        if (!root_flattened_finger_skeleton_runtime::resolveLiveFingerSkeletonSnapshot(isLeft, liveSnapshot)) {
            return false;
        }

        // The object remains at its visible world pose for a touch grab, so the
        // live finger bases must not all be shifted to one shared contact point.
        // Use that point only as acquisition evidence; solve each calibrated
        // finger path against the complete visible mesh from its real base.
        (void)grabContactWorld;
        grab_finger_pose_runtime::GrabFingerPoseTargetSet targets{};
        targets.seatPointWorld = grabAnchorWorld;
        targets.seatPointValid = true;
        targets.useSeatPointForMissingTargets = false;
        targets.useWholeMeshForMissingTargets = true;

        const auto solved = grab_finger_pose_runtime::solveGrabFingerPoseFromTriangles(
            triangles, handWorld, isLeft,
            grabAnchorWorld,
            targets,
            0.0f,                               // minValue — allow the full open-to-closed curl range
            900.0f,                             // candidate radius; whole-mesh mode ranks the nearest triangles
            true,
            &liveSnapshot,
            false,                              // triangle winding/backside does not invalidate physical contact
            0.0f,
            false,                              // no surface-aim bone correction after the verified stop
            true);                              // swept visible-envelope first contact

        // A candidate mesh alone is not a successful grip pose. At least one
        // calibrated finger envelope must actually reach it; otherwise callers
        // can fall back to a saved/authored placement (e.g. loose clothing).
        if (!solved.solved || !solved.hasJointValues || solved.hitCount == 0) {
            return false;
        }

        outJointCurls = solved.jointValues;
        return true;
    }

    bool SolveAndPublishTouchGrabFingerPose(
        RE::NiAVObject* objectRoot,
        const RE::NiTransform& handWorld,
        bool isLeft,
        const RE::NiPoint3& grabAnchorWorld,
        const RE::NiPoint3& grabContactWorld,
        float deltaTime,
        bool snapJointPoseToTarget,
        std::array<float, 15>* outJointCurls)
    {
        if (outJointCurls) {
            *outJointCurls = {};
        }
        if (!frik_visual_authority::isAvailable() ||
            !g_rockConfig.rockGrabMeshFingerPoseEnabled) {
            (void)clearRichTouchGrabFingerPose(isLeft);
            return false;
        }

        grab_finger_pose_runtime::SolvedGrabFingerPose fingerPose{};
        if (!solveRichTouchGrabFingerPose(
                objectRoot,
                handWorld,
                isLeft,
                grabAnchorWorld,
                grabContactWorld,
                fingerPose)) {
            (void)clearRichTouchGrabFingerPose(isLeft);
            if (g_rockConfig.rockDebugGrabFrameLogging) {
                ROCK_LOG_DEBUG(
                    Grab,
                    "{} hand rich touch-grab pose unavailable",
                    isLeft ? "Left" : "Right");
            }
            return false;
        }

        auto& state = richPublishState(isLeft);
        if (state.objectRoot && state.objectRoot != objectRoot) {
            (void)clearRichTouchGrabFingerPose(isLeft);
        }
        auto& activeState = richPublishState(isLeft);
        activeState.objectRoot = objectRoot;

        const auto targetJointPose = fingerPose.hasJointValues ?
            fingerPose.jointValues :
            grab_finger_pose_math::expandFingerCurlsToJointValues(fingerPose.values);
        if (outJointCurls) {
            *outJointCurls = targetJointPose;
        }
        if (!activeState.hasCurrentJointPose || snapJointPoseToTarget) {
            activeState.currentJointPose = targetJointPose;
            activeState.hasCurrentJointPose = true;
        } else {
            activeState.currentJointPose = grab_finger_pose_math::advanceJointValues(
                activeState.currentJointPose,
                targetJointPose,
                g_rockConfig.rockGrabFingerPoseSmoothingSpeed,
                deltaTime);
        }

        std::array<float, 5> splayRadians{};
        const bool usedSurfaceSplay =
            grab_finger_pose_runtime::resolveSurfaceContactSplayValues(
                isLeft,
                fingerPose,
                splayRadians);
        const auto hand = frik_visual_authority::handFromBool(isLeft);
        const auto handPose =
            frik_visual_authority::makeHandPoseDataFromJointValues(
                activeState.currentJointPose,
                splayRadians);
        if (!frik_visual_authority::setHandPoseCustomWithPriority(
                TOUCH_GRAB_HAND_POSE_TAG,
                hand,
                handPose,
                TOUCH_GRAB_HAND_POSE_PRIORITY)) {
            (void)clearRichTouchGrabFingerPose(isLeft);
            return false;
        }
        activeState.posePublished = true;

        const bool localTransformsRequested =
            g_rockConfig.rockGrabMeshLocalTransformPoseEnabled;
        const bool localTransformsPublished =
            grab_finger_local_transform_runtime::publishLocalTransformPose(
                TOUCH_GRAB_HAND_POSE_TAG,
                hand,
                isLeft,
                fingerPose,
                handPose,
                grab_finger_local_transform_runtime::Options{
                    .enabled = localTransformsRequested,
                    .smoothingSpeed =
                        g_rockConfig.rockGrabFingerLocalTransformSmoothingSpeed,
                    .maxCorrectionDegrees =
                        g_rockConfig.rockGrabFingerLocalTransformMaxCorrectionDegrees,
                    .surfaceAimStrength =
                        g_rockConfig.rockGrabFingerSurfaceAimStrength,
                    .thumbOppositionStrength =
                        g_rockConfig.rockGrabThumbOppositionStrength,
                    .thumbAlternateCurveStrength =
                        g_rockConfig.rockGrabThumbAlternateCurveStrength,
                    .thumbSurfaceSafetyEnabled =
                        g_rockConfig.rockGrabThumbSurfaceSafetyEnabled,
                    .thumbSurfaceSafetyMarginGameUnits =
                        g_rockConfig.rockGrabThumbSurfaceSafetyMarginGameUnits,
                },
                deltaTime,
                TOUCH_GRAB_HAND_POSE_PRIORITY,
                activeState.localTransforms);

        // On FRIK 0.77.12 this checks the Heisenberg host winner, not the
        // stock scalar tag-state stub. It therefore prevents a scalar-only
        // publication from being mistaken for the requested rich pose.
        const bool richPoseActive =
            frik_visual_authority::handPosePublicationStillActive(
                TOUCH_GRAB_HAND_POSE_TAG,
                hand);
        if (!richPoseActive) {
            (void)clearRichTouchGrabFingerPose(isLeft);
            return false;
        }
        // Local corrections enrich an already-valid joint+splay pose. A mesh
        // without a trustworthy correction target must not discard that pose
        // and fall back to the older curl-only API; upstream ROCK likewise
        // keeps the joint publication when local correction returns false.
        if (localTransformsRequested &&
            !localTransformsPublished &&
            g_rockConfig.rockDebugGrabFrameLogging) {
            ROCK_LOG_DEBUG(
                Grab,
                "{} hand rich touch-grab pose retained without local "
                "surface corrections",
                isLeft ? "Left" : "Right");
        }

        if (g_rockConfig.rockDebugGrabFrameLogging) {
            ROCK_LOG_DEBUG(
                Grab,
                "{} hand rich touch-grab pose: hits={} triangles={} splay={} localTransforms={} mask=0x{:04X}",
                isLeft ? "Left" : "Right",
                fingerPose.hitCount,
                fingerPose.candidateTriangleCount,
                usedSurfaceSplay ? "yes" : "no",
                localTransformsPublished ? "yes" : "no",
                activeState.localTransforms.currentMask);
        }
        return true;
    }

    bool IsTouchGrabFingerPoseActive(bool isLeft)
    {
        const auto& state = richPublishState(isLeft);
        return state.posePublished &&
            frik_visual_authority::handPosePublicationStillActive(
                TOUCH_GRAB_HAND_POSE_TAG,
                frik_visual_authority::handFromBool(isLeft));
    }

    bool ClearTouchGrabFingerPose(bool isLeft)
    {
        return clearRichTouchGrabFingerPose(isLeft);
    }
}
