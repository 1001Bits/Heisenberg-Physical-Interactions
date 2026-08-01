#include "GrabPosePolicy.h"
#include "../external/ROCK/src/physics-interaction/grab/FrikWeaponOffsetKeyPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace heisenberg::grab_pose_policy;

    using rock::frik_weapon_offset_key_policy::
        UniqueNormalizedIndex;
    using rock::frik_weapon_offset_key_policy::
        normalizeEditorIdKey;

    Require(
        normalizeEditorIdKey("AssaultRifle") ==
            normalizeEditorIdKey("Assault Rifle"),
        "editor-ID normalization must bridge omitted FRIK-key spaces");
    Require(
        normalizeEditorIdKey("PipeBoltAction") ==
            normalizeEditorIdKey("Pipe Bolt-Action"),
        "editor-ID normalization must bridge FRIK-key punctuation");
    Require(
        normalizeEditorIdKey("Cryolator") !=
            normalizeEditorIdKey("Plasma Rifle"),
        "normalization must retain alphanumeric weapon identity");

    UniqueNormalizedIndex<int> normalizedWeaponKeys;
    normalizedWeaponKeys.add("Assault Rifle", 11);
    Require(
        normalizedWeaponKeys.find("AssaultRifle") ==
            std::optional<int>{ 11 },
        "a unique normalized editor ID must resolve its spaced FRIK key");
    normalizedWeaponKeys.add("Assault-Rifle", 22);
    Require(
        !normalizedWeaponKeys.find("AssaultRifle").has_value(),
        "punctuation-colliding FRIK keys must fail closed as ambiguous");

    Require(
        ShouldBypassSavedPlacement(kKickballBaseFormID),
        "the regular Kickball must bypass its embedded placement for the ROCK pose test");
    Require(
        ShouldBypassSavedPlacement(kShovelBaseFormID),
        "the shovel must bypass the legacy placement that leaves its shaft away from the primary palm");
    Require(
        !ShouldBypassSavedPlacement(0x0002D9ABu),
        "the separate unfilled Kickball record must not inherit the test override");
    Require(
        ShouldBypassSavedFingerPose(kKickballBaseFormID),
        "the Kickball must not reuse its all-open saved curls");
    Require(
        ShouldBypassSavedFingerPose(kShovelBaseFormID),
        "the shovel must mesh-fit instead of using its legacy uniform curls");
    Require(
        !ShouldBypassSavedFingerPose(0x000822CCu),
        "nearby form IDs must not inherit either narrow exception");

    Require(
        ShouldAcceptPhysicalTouchEvidence(kFreshTouchMaxAgeFrames, 500.0f),
        "fresh native contact remains authoritative during its live four-frame window");
    Require(
        ShouldAcceptPhysicalTouchEvidence(178u, 13.9f),
        "recent same-reference evidence may bridge a squeeze when the current mesh is still reachable");
    Require(
        !ShouldAcceptPhysicalTouchEvidence(178u, 79.1f),
        "stale same-reference evidence must not turn a distant selection into a physical touch");
    Require(
        !ShouldAcceptPhysicalTouchEvidence(181u, 0.0f),
        "same-reference evidence must expire after its bounded recent window");

    Require(
        ShouldAcceptSameObjectTransferContact(
            true,
            kFreshTouchMaxAgeFrames,
            false,
            500.0f),
        "fresh native contact must commit a same-object transfer even when the palm-center frame is offset");
    Require(
        ShouldAcceptSameObjectTransferContact(
            false,
            0xFFFF'FFFFu,
            true,
            4.0f),
        "the exact held-object mesh result from the grip edge must remain authoritative in StartGrab");
    Require(
        !ShouldAcceptSameObjectTransferContact(
            false,
            0xFFFF'FFFFu,
            true,
            5.1f),
        "a held-object transfer marker must not widen the five-unit contact envelope");
    Require(
        !ShouldAcceptSameObjectTransferContact(
            true,
            kFreshTouchMaxAgeFrames + 1u,
            false,
            0.0f),
        "an aged native-contact identity alone must not authorize a transfer");

    Require(
        CanRejectCoHoldMeshQuery(31.0f, 10.0f, 5.0f),
        "a hand clearly outside the held mesh bound and guarded contact range may skip extraction");
    Require(
        !CanRejectCoHoldMeshQuery(20.0f, 10.0f, 5.0f),
        "the guarded co-hold broadphase boundary must retain exact mesh testing");
    Require(
        !CanRejectCoHoldMeshQuery(31.0f, 0.0f, 5.0f),
        "a degenerate held-object world bound must fail open to exact extraction");
    Require(
        !CanRejectCoHoldMeshQuery(
            31.0f,
            std::numeric_limits<float>::quiet_NaN(),
            5.0f),
        "an invalid held-object world bound must fail open to exact extraction");

    Require(
        SpherePalmCenterDistance(13.6f) > 14.09f &&
            SpherePalmCenterDistance(13.6f) < 14.11f,
        "a radius-13.6 sphere must retain the 0.5-unit ROCK palm skin");
    Require(
        SpherePalmCenterDistance(0.2f) == 1.5f,
        "degenerate sphere bounds must use the one-unit minimum radius");

    Require(
        ShouldUseBroadPalmSeat(16.0f, 13.0f, 3.0f),
        "a folded-uniform slab must be centered on a broad palm seat");
    Require(
        ShouldUseBroadPalmSeat(3.0f, 16.0f, 13.0f),
        "broad-seat classification must be independent of object-local axis order");
    Require(
        ShouldUseBroadPalmSeat(42.0f, 34.0f, 10.0f),
        "the BOS Knight uniform's observed mesh bounds must select a broad palm seat");
    Require(
        ShouldUseBroadPalmSeat(20.0f, 12.0f, 0.0f),
        "a genuinely planar magazine must remain eligible with zero thickness");
    Require(
        !ShouldUseBroadPalmSeat(12.0f, 11.0f, 10.0f),
        "a compact cube must keep ordinary whole-object contact placement");
    Require(
        !ShouldUseBroadPalmSeat(38.0f, 5.0f, 3.0f),
        "a long narrow handle must keep shaft/contact placement");
    Require(
        !ShouldUseBroadPalmSeat(120.0f, 60.0f, 20.0f),
        "large scenery must not be centered wholesale on the hand");
    Require(
        !ShouldUseBroadPalmSeat(6.0f, 5.0f, 1.0f),
        "a prop narrower than the palm must keep precise contact placement");
    Require(
        ShouldApplyBroadPalmSeat(
            false,
            false,
            false,
            16.0f,
            13.0f,
            3.0f),
        "an unauthored broad prop must use the palm seat");
    Require(
        !ShouldApplyBroadPalmSeat(
            false,
            true,
            false,
            16.0f,
            13.0f,
            3.0f),
        "a normal authored snap must retain its saved placement");
    Require(
        ShouldApplyBroadPalmSeat(
            false,
            true,
            true,
            16.0f,
            13.0f,
            3.0f),
        "a direct broad touch must not remain pinned to one fingertip");
    Require(
        !ShouldApplyBroadPalmSeat(
            true,
            false,
            true,
            16.0f,
            13.0f,
            3.0f),
        "holotapes must retain their shared inventory/deck placement");

    // Jul 30: a mesh the slab classifier rejects must still be seated against
    // the palm rather than dropped onto the geometry-blind snap constant that
    // left Maxson's Battlecoat floating 14 units off the hand.
    Require(
        ShouldApplyMeshPalmSeatFallback(
            false,
            false,
            false,
            true,
            26.0f,
            11.0f,
            9.0f),
        "a pulled unauthored mesh that fails the slab test must still be seated on the palm");
    Require(
        !ShouldApplyMeshPalmSeatFallback(
            false,
            false,
            false,
            false,
            26.0f,
            11.0f,
            9.0f),
        "a close natural or geometry-solved grab must never be snapped to the palm by the fallback");
    Require(
        !ShouldApplyMeshPalmSeatFallback(
            false,
            false,
            false,
            true,
            16.0f,
            13.0f,
            3.0f),
        "a slab that the broad seat already handles must not also take the fallback");
    Require(
        !ShouldApplyMeshPalmSeatFallback(
            false,
            true,
            false,
            true,
            26.0f,
            11.0f,
            9.0f),
        "an authored placement stays authoritative for the fallback seat too");
    Require(
        !ShouldApplyMeshPalmSeatFallback(
            true,
            false,
            false,
            true,
            26.0f,
            11.0f,
            9.0f),
        "holotapes keep their shared placement contract in the fallback seat");
    Require(
        !ShouldApplyMeshPalmSeatFallback(
            false,
            false,
            false,
            true,
            120.0f,
            60.0f,
            20.0f),
        "scenery-sized meshes must stay out of the palm seat entirely");

    Require(
        UsesAuthoredPose(true, false, true),
        "a saved placement and its saved curls must remain one authored pose");
    Require(
        !UsesAuthoredPose(false, true, true),
        "a natural touch placement must not reuse curls authored for a snap pose");
    Require(
        !UsesAuthoredPose(true, true, true),
        "natural posing must override an otherwise available saved profile");
    Require(
        !UsesAuthoredPose(true, false, false),
        "a saved transform without saved curls still needs a geometry pose");

    Require(
        !ShouldRecalculateGeometryCurls(true, false, true),
        "geometry must not overwrite a complete authored pose");
    Require(
        ShouldRecalculateGeometryCurls(false, true, true),
        "natural placement must continue to follow live mesh geometry");

    Require(
        ShouldUseCanonicalLooseWeaponHold(false, true, false, true, false),
        "a no-offset DropToHand firearm must use the canonical firing hold");
    Require(
        ShouldUseCanonicalLooseWeaponHold(false, false, true, true, false),
        "a no-offset remote firearm pull must converge on the canonical firing hold");
    Require(
        !ShouldUseCanonicalLooseWeaponHold(true, true, true, true, false),
        "a saved Heisenberg placement must remain authoritative");
    Require(
        !ShouldUseCanonicalLooseWeaponHold(false, false, false, true, false),
        "a close weapon grab must remain a free mesh/touch hold");
    Require(
        !ShouldUseCanonicalLooseWeaponHold(false, true, false, true, true),
        "grenades and mines must preserve their throwable hand pose");
    Require(
        !ShouldUseCanonicalLooseWeaponHold(false, true, false, false, false),
        "programmatic clutter arrivals must not enter the weapon firing-hold path");

    Require(
        !ShouldCommitRenderedHandRebase(true, true, true),
        "an authored pose must not freeze its hand relation during pull");
    Require(
        !ShouldCommitRenderedHandRebase(true, false, false),
        "an authored pose must not freeze against a transitioning finger pose");
    Require(
        ShouldCommitRenderedHandRebase(true, false, true),
        "a settled authored transform/curl pair may be attached to the rendered hand");
    Require(
        ShouldCommitRenderedHandRebase(false, true, false),
        "live geometry/touch placement keeps its immediate rebase behavior");

    return 0;
}
