#pragma once

namespace heisenberg::grab_pose_policy
{
    // A saved object transform and its saved finger curls are one authored pose.
    // They are authoritative only when that saved transform is actually driving
    // placement. Natural/touch placement must continue to solve against the
    // object's live mesh because its hand-relative transform is arbitrary.
    constexpr bool UsesAuthoredPose(
        bool hasSavedPlacement,
        bool naturalFingerPosing,
        bool hasStoredFingerCurls)
    {
        return hasSavedPlacement &&
               !naturalFingerPosing &&
               hasStoredFingerCurls;
    }

    constexpr bool ShouldRecalculateGeometryCurls(
        bool hasSavedPlacement,
        bool naturalFingerPosing,
        bool hasStoredFingerCurls)
    {
        return !UsesAuthoredPose(
            hasSavedPlacement,
            naturalFingerPosing,
            hasStoredFingerCurls);
    }

    // Saved offsets were authored in the controller/wand frame. Re-expressing
    // one in the rendered hand frame while the authored fingers are still
    // moving captures a transient wrist/controller relation and produces a
    // slightly different seat on every grab. Commit only after both the pull
    // and the saved finger animation have settled.
    constexpr bool ShouldCommitRenderedHandRebase(
        bool usesAuthoredPose,
        bool isPulling,
        bool authoredFingerPoseSettled)
    {
        return !usesAuthoredPose ||
               (!isPulling && authoredFingerPoseSettled);
    }
}
