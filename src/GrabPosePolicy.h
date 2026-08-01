#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace heisenberg::grab_pose_policy
{
    inline constexpr std::uint32_t kKickballBaseFormID = 0x000822D3u;
    inline constexpr std::uint32_t kShovelBaseFormID = 0x000822CBu;
    inline constexpr std::uint32_t kFreshTouchMaxAgeFrames = 4u;
    inline constexpr std::uint32_t kRecentTouchMaxAgeFrames = 180u;
    inline constexpr float kRecentTouchMaxMeshDistance = 14.0f;
    inline constexpr float kPalmSurfaceSkin = 0.5f;
    inline constexpr float kBroadPalmMinimumSecondSpan = 8.0f;
    inline constexpr float kBroadPalmMaximumAspectRatio = 2.25f;
    inline constexpr float kBroadPalmMaximumLargestSpan = 50.0f;
    inline constexpr float kBroadPalmMaximumThicknessRatio = 0.60f;

    // The regular Kickball is an explicit integration test for ROCK's current
    // mesh placement and rich finger pose. The shovel's legacy pose leaves its
    // visible shaft 42-55 game units from the primary palm, which prevents a
    // second hand from ever joining it. Keep both records available to the
    // offset index, but bypass their placement at acquisition.
    constexpr bool ShouldBypassSavedPlacement(std::uint32_t baseFormID)
    {
        return baseFormID == kKickballBaseFormID ||
               baseFormID == kShovelBaseFormID;
    }

    // The shovel's old uniform curl profile predates the mesh-contact solver
    // and was observed penetrating the handle. Preserve its useful placement,
    // but derive fingers from the final visible mesh. The Kickball bypasses
    // both halves of its saved pose.
    constexpr bool ShouldBypassSavedFingerPose(std::uint32_t baseFormID)
    {
        return baseFormID == kKickballBaseFormID ||
               baseFormID == kShovelBaseFormID;
    }

    // A same-reference contact cache is only identity evidence. Once the
    // native contact is older than the four-frame live window, the controller
    // must still be inside the reachable hand envelope of the CURRENT visible
    // mesh before it can be promoted back to a physical-touch grab.
    inline bool ShouldAcceptPhysicalTouchEvidence(
        std::uint32_t ageFrames,
        float currentMeshDistance)
    {
        if (ageFrames <= kFreshTouchMaxAgeFrames) {
            return true;
        }
        return ageFrames <= kRecentTouchMaxAgeFrames &&
               std::isfinite(currentMeshDistance) &&
               currentMeshDistance >= 0.0f &&
               currentMeshDistance <= kRecentTouchMaxMeshDistance;
    }

    // Same-object transfer eligibility is established at the grip edge. A
    // fresh native contact is authoritative, and Hand::TryStartGrab may also
    // provide a narrowly-scoped exact visible-mesh result for the currently
    // held reference. Requiring StartGrab to perform another query from a
    // differently offset palm made the two tests disagree and allowed the
    // source hand's following release to drop the object.
    inline bool ShouldAcceptSameObjectTransferContact(
        bool isPhysicalTouch,
        std::uint32_t physicalTouchAgeFrames,
        bool isHeldObjectTransferContact,
        float selectionMeshDistance,
        float heldObjectContactLimit = 5.0f) noexcept
    {
        if (isPhysicalTouch &&
            physicalTouchAgeFrames <= kFreshTouchMaxAgeFrames) {
            return true;
        }

        return isHeldObjectTransferContact &&
               std::isfinite(selectionMeshDistance) &&
               selectionMeshDistance >= 0.0f &&
               std::isfinite(heldObjectContactLimit) &&
               heldObjectContactLimit >= 0.0f &&
               selectionMeshDistance <= heldObjectContactLimit;
    }

    // The rendered world bound contains every mesh point. Therefore a hand
    // farther from its center than radius + accepted mesh distance cannot be a
    // co-hold candidate. Keep a generous extra guard for one-frame held-object
    // transform/bound slack. Invalid or degenerate bounds fail open so the
    // existing exact triangle query remains the compatibility fallback.
    inline bool CanRejectCoHoldMeshQuery(
        float handDistanceToBoundCenter,
        float worldBoundRadius,
        float acceptedMeshDistance,
        float safetyGuard = 5.0f) noexcept
    {
        if (!std::isfinite(handDistanceToBoundCenter) ||
            handDistanceToBoundCenter < 0.0f ||
            !std::isfinite(worldBoundRadius) ||
            worldBoundRadius < 0.5f ||
            worldBoundRadius > 10000.0f ||
            !std::isfinite(acceptedMeshDistance) ||
            acceptedMeshDistance < 0.0f ||
            !std::isfinite(safetyGuard) ||
            safetyGuard < 0.0f) {
            return false;
        }

        const float rejectionDistance =
            worldBoundRadius +
            acceptedMeshDistance +
            safetyGuard;
        return std::isfinite(rejectionDistance) &&
               handDistanceToBoundCenter > rejectionDistance;
    }

    constexpr float SpherePalmCenterDistance(
        float radius,
        float skin = kPalmSurfaceSkin)
    {
        return (radius > 1.0f ? radius : 1.0f) +
               (skin > 0.0f ? skin : 0.0f);
    }

    // A contact on a broad prop must describe WHAT was touched, not WHERE the
    // whole object remains attached.  Once two object-local axes span at least
    // the palm, a one-finger contact is an unstable pivot: folded clothing,
    // books, magazines, plates and small boxes hang from whichever fingertip
    // happened to report the first manifold.  Classify those shapes from
    // object-local spans so a long handle cannot become "broad" merely because
    // it lies diagonally in world space.  Very large scenery stays on the
    // ordinary contact path; centering a car or furniture piece on the palm
    // would be worse than retaining the touched point.
    inline bool ShouldUseBroadPalmSeat(
        float spanX,
        float spanY,
        float spanZ) noexcept
    {
        if (!std::isfinite(spanX) ||
            !std::isfinite(spanY) ||
            !std::isfinite(spanZ) ||
            spanX < 0.0f ||
            spanY < 0.0f ||
            spanZ < 0.0f) {
            return false;
        }

        const float largest =
            (std::max)(spanX, (std::max)(spanY, spanZ));
        const float smallest =
            (std::min)(spanX, (std::min)(spanY, spanZ));
        const float middle = spanX + spanY + spanZ -
                             largest - smallest;
        return middle >= kBroadPalmMinimumSecondSpan &&
               middle > 0.0f &&
               largest <= kBroadPalmMaximumLargestSpan &&
               largest / middle <=
                   kBroadPalmMaximumAspectRatio &&
               smallest / middle <=
                   kBroadPalmMaximumThicknessRatio;
    }

    // Explicit authored placements remain user authority for a normal snap.
    // A direct touch already opts out of authored placement in Grab.cpp, so a
    // broad touch may be promoted from a fingertip pivot to the palm seat.
    // Holotapes retain their shared deck/inventory placement contract.
    inline bool ShouldApplyBroadPalmSeat(
        bool isHolotape,
        bool hasAuthoredPlacement,
        bool directTouchPlacement,
        float spanX,
        float spanY,
        float spanZ) noexcept
    {
        return !isHolotape &&
               (!hasAuthoredPlacement || directTouchPlacement) &&
               ShouldUseBroadPalmSeat(spanX, spanY, spanZ);
    }

    // A mesh that fails the slab test still has to END UP IN THE HAND. When a
    // far/pulled grab has no authored offset and no geometry solve, placement
    // fell through to a formula that parked the object's PIVOT half its
    // longest BOUND dimension in front of the palm and assumed pivot ==
    // centre. Live result for Maxson's Battlecoat: the visible mesh came to
    // rest 14 units from the palm, floating with nothing touching the hand.
    // Seat those meshes from their measured geometry too — but WITHOUT the
    // broad seat's thin-axis rotation, since a non-slab shape has no
    // meaningful broad face to present. Scenery-sized meshes stay out, exactly
    // as for the broad seat: centring a car on the palm would be worse.
    inline bool ShouldUseMeshPalmSeatFallback(
        float spanX,
        float spanY,
        float spanZ) noexcept
    {
        if (!std::isfinite(spanX) ||
            !std::isfinite(spanY) ||
            !std::isfinite(spanZ) ||
            spanX < 0.0f ||
            spanY < 0.0f ||
            spanZ < 0.0f) {
            return false;
        }

        const float largest =
            (std::max)(spanX, (std::max)(spanY, spanZ));
        return largest > 0.0f &&
               largest <= kBroadPalmMaximumLargestSpan;
    }

    // The fallback seat exists to REPLACE that geometry-blind constant, so it
    // may only run where the constant was actually applied — a far/pulled snap
    // with no authored placement. A close natural grab keeps its preserved
    // world pose and a close solved grab keeps its live geometry contact;
    // neither may be snapped to the palm by this path.
    inline bool ShouldApplyMeshPalmSeatFallback(
        bool isHolotape,
        bool hasAuthoredPlacement,
        bool directTouchPlacement,
        bool replacingGeometryBlindSeat,
        float spanX,
        float spanY,
        float spanZ) noexcept
    {
        return replacingGeometryBlindSeat &&
               !isHolotape &&
               (!hasAuthoredPlacement || directTouchPlacement) &&
               !ShouldUseBroadPalmSeat(spanX, spanY, spanZ) &&
               ShouldUseMeshPalmSeatFallback(spanX, spanY, spanZ);
    }

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

    // Programmatic arrivals and remote pulls need one deterministic firing
    // relation for loose guns. Close grabs deliberately remain free mesh/touch
    // holds, while a user-authored Heisenberg offset stays the highest
    // placement authority. Throwables are hand-thrown objects and must never
    // inherit a firearm attach transform.
    constexpr bool ShouldUseCanonicalLooseWeaponHold(
        bool hasSavedPlacement,
        bool forcedArrival,
        bool remotePull,
        bool isWeapon,
        bool isThrowable)
    {
        return !hasSavedPlacement &&
               (forcedArrival || remotePull) &&
               isWeapon &&
               !isThrowable;
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
