#include "GrabOwnershipPolicy.h"
#include "StorageZoneWeaponPolicy.h"
#include "physics-interaction/input/HostGrabInputEdgePolicy.h"
#include "physics-interaction/input/InputRemapPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using heisenberg::storage_zone_weapon_policy::Input;
    using heisenberg::storage_zone_weapon_policy::isPrimaryHand;
    using heisenberg::storage_zone_weapon_policy::offhandIsLeft;
    using heisenberg::storage_zone_weapon_policy::
        shouldUnequipPrimaryWeapon;
    Require(
        shouldUnequipPrimaryWeapon(Input{
            .featureEnabled = true,
            .primaryHand = true,
            .insideStorageZone = true,
            .offhandGrabbingWeapon = true,
            .playerHasWeaponEquipped = true,
        }),
        "StorageZone must own the primary grip for an explicit offhand-weapon swap");
    Require(
        !isPrimaryHand(true, false) &&
            isPrimaryHand(false, false) &&
            offhandIsLeft(false),
        "right-handed mode must map right to primary and left to offhand");
    Require(
        isPrimaryHand(true, true) &&
            !isPrimaryHand(false, true) &&
            !offhandIsLeft(true),
        "left-handed mode must map left to primary and right to offhand");
    Require(
        !shouldUnequipPrimaryWeapon(Input{
            .featureEnabled = true,
            .primaryHand = true,
            .insideStorageZone = true,
            .offhandGrabbingWeapon = false,
            .playerHasWeaponEquipped = true,
        }),
        "ordinary primary-hand shoulder holstering must remain available when the offhand holds no weapon");
    Require(
        !shouldUnequipPrimaryWeapon(Input{
            .featureEnabled = true,
            .primaryHand = true,
            .insideStorageZone = true,
            .offhandGrabbingWeapon = true,
            .playerHasWeaponEquipped = false,
        }),
        "StorageZone must not equip or re-equip a primary weapon when none is equipped");
    Require(
        !shouldUnequipPrimaryWeapon(Input{
            .featureEnabled = true,
            .primaryHand = true,
            .insideStorageZone = true,
            .offhandGrabbingWeapon = false,
            .playerHasWeaponEquipped = false,
        }),
        "the false/false predicate pair must leave primary-hand holster input untouched");
    Require(
        !shouldUnequipPrimaryWeapon(Input{
            .featureEnabled = true,
            .primaryHand = false,
            .insideStorageZone = true,
            .offhandGrabbingWeapon = true,
            .playerHasWeaponEquipped = true,
        }),
        "the offhand must never mutate the primary equipped weapon through StorageZone");
    Require(
        !shouldUnequipPrimaryWeapon(Input{
            .featureEnabled = false,
            .primaryHand = true,
            .insideStorageZone = true,
            .offhandGrabbingWeapon = true,
            .playerHasWeaponEquipped = true,
        }) &&
            !shouldUnequipPrimaryWeapon(Input{
                .featureEnabled = true,
                .primaryHand = true,
                .insideStorageZone = false,
                .offhandGrabbingWeapon = true,
                .playerHasWeaponEquipped = true,
            }),
        "the config and zone boundary must remain mandatory");

    {
        using namespace rock::input_remap_policy;
        const auto gripMask = buttonMask(kOpenVrGripButtonId);
        const auto acceptMask = buttonMask(kOpenVrAcceptButtonId);
        const auto analogPressed =
            normalizeGripPressedMask(acceptMask, 0, 0.75f);
        Require(
            (analogPressed & gripMask) != 0,
            "an analog-only grip squeeze must produce ROCK's grip button state");
        Require(
            (analogPressed & acceptMask) != 0,
            "grip normalization must preserve unrelated digital buttons");

        const auto pressTransition =
            evaluateEdgeTransition(true, acceptMask, analogPressed);
        Require(
            (pressTransition.pressedEdges & gripMask) != 0 &&
                (pressTransition.releasedEdges & gripMask) == 0,
            "an analog-only grip squeeze must derive exactly one press edge");

        const auto analogHeld =
            normalizeGripPressedMask(acceptMask, analogPressed, 0.40f);
        Require(
            (analogHeld & gripMask) != 0,
            "grip-axis hysteresis must retain a held left grip through threshold noise");
        const auto heldTransition =
            evaluateEdgeTransition(true, analogPressed, analogHeld);
        Require(
            (heldTransition.pressedEdges & gripMask) == 0 &&
                (heldTransition.releasedEdges & gripMask) == 0,
            "grip-axis hysteresis must not chatter new edges while held");

        const auto analogReleased =
            normalizeGripPressedMask(acceptMask, analogHeld, 0.10f);
        Require(
            (analogReleased & gripMask) == 0,
            "an analog grip release must clear ROCK's normalized grip state");
        const auto releaseTransition =
            evaluateEdgeTransition(true, analogHeld, analogReleased);
        Require(
            (releaseTransition.pressedEdges & gripMask) == 0 &&
                (releaseTransition.releasedEdges & gripMask) != 0,
            "an analog grip release must derive exactly one release edge");

        Require(
            normalizeGripPressedMask(
                gripMask | acceptMask,
                0,
                0.0f) == (gripMask | acceptMask),
            "controllers with a digital grip bit must retain their native state");
        Require(
            (normalizeGripPressedMask(
                 0,
                 0,
                 std::numeric_limits<float>::quiet_NaN()) &
                gripMask) == 0,
            "a NaN grip-axis sample must not manufacture a grip press");
        Require(
            (normalizeGripPressedMask(
                 0,
                 0,
                 std::numeric_limits<float>::infinity()) &
                gripMask) == 0,
            "an infinite grip-axis sample must not manufacture a grip press");
    }

    using namespace heisenberg::grab_ownership_policy;

    Require(
        !shouldInterceptAlternateGrabPress(false, false),
        "a selection without an active grab must never intercept Grip/A");
    Require(
        shouldInterceptAlternateGrabPress(true, false) &&
            shouldInterceptAlternateGrabPress(false, true),
        "active same-hand ownership and its post-drop guard must remain intercepted");

    const auto shippedDynamic = resolve(
        kFullDynamicGrabMode,
        false,
        true);
    Require(
        shippedDynamic.embeddedRockOwnsGrab &&
            shippedDynamic.effectiveGrabMode ==
                kFullDynamicGrabMode,
        "mode 9 with a hosted engine must have exactly one embedded ROCK grab owner");

    const auto legacyAlias = resolve(
        kKeyframedGrabMode,
        true,
        true);
    Require(
        legacyAlias.embeddedRockOwnsGrab &&
            legacyAlias.effectiveGrabMode ==
                kFullDynamicGrabMode,
        "the legacy delegate flag must cede to the hosted embedded engine instead of becoming a dead switch");

    const auto unavailableEngine = resolve(
        kFullDynamicGrabMode,
        false,
        false);
    Require(
        !unavailableEngine.embeddedRockOwnsGrab &&
            unavailableEngine.fellBackToKeyframed &&
            unavailableEngine.effectiveGrabMode ==
                kKeyframedGrabMode,
        "a failed embedded engine load must retain a working keyframed fallback");

    const auto explicitKeyframed = resolve(
        kKeyframedGrabMode,
        false,
        true);
    Require(
        !explicitKeyframed.dynamicRequested &&
            !explicitKeyframed.embeddedRockOwnsGrab &&
            explicitKeyframed.effectiveGrabMode ==
                kKeyframedGrabMode,
        "mode 0 without delegation must remain an explicit keyframed choice");

    const auto removedHistoricalMode = resolve(4, false, true);
    Require(
        !removedHistoricalMode.embeddedRockOwnsGrab &&
            removedHistoricalMode.effectiveGrabMode ==
                kKeyframedGrabMode,
        "removed historical modes must never create two grab owners");

    Require(
        resolveGripInputOwner(
            kFullDynamicGrabMode,
            false,
            true,
            false) == GripInputOwner::EmbeddedRockWorldGrab,
        "an idle mode-9 hand must cede world-grab input to embedded ROCK");
    Require(
        resolveGripInputOwner(
            kFullDynamicGrabMode,
            false,
            true,
            true) == GripInputOwner::HostActiveGrab,
        "an active DropToHand grab must retain its release edge in mode 9");
    Require(
        resolveGripInputOwner(
            kKeyframedGrabMode,
            true,
            true,
            true) == GripInputOwner::HostActiveGrab,
        "an active host grab must outrank the legacy ROCK-delegation alias");
    Require(
        resolveGripInputOwner(
            kKeyframedGrabMode,
            false,
            true,
            false) == GripInputOwner::HostWorldGrab,
        "an idle keyframed hand must retain normal host input");
    Require(
        resolveGripInputOwner(
            kFullDynamicGrabMode,
            false,
            false,
            true) == GripInputOwner::HostActiveGrab,
        "a host grab must remain releasable while embedded ROCK is unavailable");

    const auto activeHostAndSupport = resolveGripInput(
        kFullDynamicGrabMode,
        false,
        true,
        true,
        true);
    Require(
        activeHostAndSupport.owner == GripInputOwner::HostActiveGrab &&
            activeHostAndSupport.consumeEmbeddedRockEdge,
        "an active host grab must outrank mode 9 and a weapon-support gate, then consume ROCK's copy of the edge");

    const auto supportWithoutHostGrab = resolveGripInput(
        kFullDynamicGrabMode,
        false,
        true,
        false,
        true);
    Require(
        supportWithoutHostGrab.owner ==
                GripInputOwner::EmbeddedRockWeaponSupport &&
            !supportWithoutHostGrab.consumeEmbeddedRockEdge,
        "weapon support must retain the edge only when no host grab is active");

    const auto unavailableRockHostGrab = resolveGripInput(
        kFullDynamicGrabMode,
        false,
        false,
        true,
        true);
    Require(
        unavailableRockHostGrab.owner == GripInputOwner::HostActiveGrab &&
            !unavailableRockHostGrab.consumeEmbeddedRockEdge,
        "the host must not queue a phantom ROCK edge consumption while the engine is unavailable");

    const auto hostedKeyframedHostGrab = resolveGripInput(
        kKeyframedGrabMode,
        false,
        true,
        true,
        false);
    Require(
        hostedKeyframedHostGrab.owner == GripInputOwner::HostActiveGrab &&
            hostedKeyframedHostGrab.consumeEmbeddedRockEdge,
        "a hosted mode-0 host grab must still mask ROCK's independent equipped-weapon consumers");

    Require(
        shouldReleaseActiveHostGrab(true, GripEdge::Pressed) &&
            !shouldReleaseActiveHostGrab(true, GripEdge::Released),
        "a sticky DropToHand item must toggle off on press, not release");
    Require(
        !shouldReleaseActiveHostGrab(false, GripEdge::Pressed) &&
            shouldReleaseActiveHostGrab(false, GripEdge::Released),
        "a non-sticky programmatic grab must remain held through press and drop on release");

    using rock::host_grab_input_edge_policy::Action;
    using rock::host_grab_input_edge_policy::ConsumptionStage;
    using rock::host_grab_input_edge_policy::resolve;
    using rock::host_grab_input_edge_policy::resolveWeaponGripHeldLevel;
    using rock::host_grab_input_edge_policy::shouldDrainRawStateAtStage;
    using rock::host_grab_input_edge_policy::
        shouldRetainPendingEdgeForNextConsumableFrame;
    const auto noHostEdge = resolve(false, false, false);
    Require(
        noHostEdge.action == Action::None &&
            !noHostEdge.discardEdge,
        "ROCK must not discard an edge without an explicit host signal");
    const auto hostOnlyEdge = resolve(true, false, false);
    Require(
        hostOnlyEdge.action == Action::DiscardHostOwnedEdge &&
            hostOnlyEdge.discardEdge &&
            hostOnlyEdge.suppressWeaponInputThisFrame &&
            hostOnlyEdge.suppressNormalGrabInputThisFrame,
        "ROCK must mask both weapon and normal grab consumers for the one-shot host-owned edge");
    Require(
        shouldDrainRawStateAtStage(
            hostOnlyEdge,
            ConsumptionStage::BeforeWeaponOwnership) &&
            !shouldDrainRawStateAtStage(
                hostOnlyEdge,
                ConsumptionStage::NormalGrab),
        "the host-owned raw edge must drain before weapon ownership and never drain again in normal grab");
    Require(
        shouldRetainPendingEdgeForNextConsumableFrame(true, false) &&
            !shouldRetainPendingEdgeForNextConsumableFrame(true, true) &&
            !shouldRetainPendingEdgeForNextConsumableFrame(false, false),
        "a host ownership token must survive only frames that cannot reach ROCK's raw-edge drain");
    Require(
        !resolveWeaponGripHeldLevel(true, false, true),
        "a masked host-held level must not acquire a new firing/support weapon grip");
    Require(
        resolveWeaponGripHeldLevel(true, true, false),
        "a masked host edge must not release an already-active firing/support weapon grip");
    Require(
        resolveWeaponGripHeldLevel(false, false, true) &&
            !resolveWeaponGripHeldLevel(false, true, false),
        "unmasked weapon input must continue to follow the raw held level");
    const auto rockHeldConflict = resolve(true, true, false);
    Require(
        rockHeldConflict.action == Action::PreserveActiveRockOwner &&
            !rockHeldConflict.discardEdge &&
            rockHeldConflict.suppressWeaponInputThisFrame &&
            !rockHeldConflict.suppressNormalGrabInputThisFrame,
        "a dual-owner fail-safe must mask weapon input while preserving an existing ROCK object's raw release");
    const auto touchGrabConflict = resolve(true, false, true);
    Require(
        touchGrabConflict.action == Action::PreserveActiveRockOwner &&
            !touchGrabConflict.discardEdge &&
            touchGrabConflict.suppressWeaponInputThisFrame &&
            !touchGrabConflict.suppressNormalGrabInputThisFrame,
        "a dual-owner fail-safe must mask weapon input while preserving an existing provider touch-grab release");
    std::cout << "Grab ownership policy tests passed\n";
    return 0;
}
