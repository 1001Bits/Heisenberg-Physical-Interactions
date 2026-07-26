# ROCK provider API through Heisenberg

Heisenberg embeds ROCK in `Heisenberg_F4VR.dll`; a separate `ROCK.dll` and a
Virtual Reloads helper header are not required. Include the matching
`ROCKProviderApi.h`, call the API directly, and check every returned
`RockProviderResultV1`.

## What the three structures do

- `RockProviderWeaponPartTargetV1` selects the exact collision body the hand
  is allowed to grab.
- `RockProviderWeaponPartMotionConstraintV1` defines how a grabbed part may
  move.
- `RockProviderWeaponPartDriveTargetV1` writes an explicit pose without hand
  movement.

`motion` and `partConstraint` are only local variable names. Both examples
refer to the same `RockProviderWeaponPartMotionConstraintV1` type.

There are also two node identities:

- The evidence `bodyId` identifies the physical mesh the hand must touch.
- `controlledRoot` identifies the authored scene node ROCK must move.

For Cylon's current code, the controlled node value is:

```cpp
reinterpret_cast<std::uintptr_t>(weap->primaryNode01)
```

It is **not** `CurrentWeapon::primaryNode01`. `controlledRoot` does not make a
part grabbable; the exact evidence target does that.

## Connect once

```cpp
#include "ROCKProviderApi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

const rock::provider::RockProviderApi* g_rockApi = nullptr;
std::uint64_t g_rockOwnerToken = 0;

bool connectToRock()
{
    using namespace rock::provider;

    if (RockProviderApi::initialize(
            ROCK_PROVIDER_API_VERSION,
            ROCK_PROVIDER_API_V1_WEAPON_PART_MOTION_CONSTRAINT_TABLE_BYTES) != 0) {
        return false;
    }

    g_rockApi = RockProviderApi::inst;
    if (!g_rockApi || !g_rockApi->isProviderReady()) {
        return false;
    }

    RockProviderLimitsV1 limits{};
    if (!g_rockApi->getProviderLimitsV1(&limits) ||
        !supportsWeaponPartInteractionV1(limits) ||
        !supportsWeaponPartMotionConstraintV1(limits) ||
        !supportsWeaponPartExclusiveExactContactV1(limits) ||
        !supportsWeaponPartControlledRootV1(limits)) {
        return false;
    }

    RockProviderConsumerRegistrationV1 registration{};
    std::snprintf(
        registration.modName,
        sizeof(registration.modName),
        "%s",
        "CylonVirtualReloads");
    registration.requestedCapabilities =
        static_cast<std::uint32_t>(
            RockProviderConsumerCapabilityV1::WeaponPartInteraction) |
        static_cast<std::uint32_t>(
            RockProviderConsumerCapabilityV1::WeaponPartMotionConstraint);

    RockProviderConsumerHandleV1 handle{};
    if (g_rockApi->registerConsumerV1(&registration, &handle) !=
        RockProviderResultV1::Ok) {
        g_rockApi = nullptr;
        return false;
    }

    const auto required = registration.requestedCapabilities;
    if ((handle.grantedCapabilities & required) != required) {
        g_rockApi->unregisterConsumerV1(handle.ownerToken);
        g_rockApi = nullptr;
        return false;
    }

    g_rockOwnerToken = handle.ownerToken;
    return true;
}
```

Retry connection on a later game tick if ROCK is not ready yet.

## Cylon's exact dry-fire slide setup

The lifecycle supplied by Cylon is:

1. `dryPositionSlide` runs when ammunition reaches zero and moves the bolt to
   `reloadSlidePos`.
2. The player presses reload, which calls `slideReloadSetup`.
3. `slideReloadSetup` calls `setupSlideConstraint` only while the bolt is
   already in the dry-fire position.

Therefore the position captured at the instant of the grab is rail coordinate
zero. Pressing grip without moving the controller must not move the bolt.
Moving the controller along local negative Y pulls it from `reloadSlidePos`
toward `maxBoltPosition`, and moving the controller back returns it toward
the dry-fire position.

This is a complete raw-API setup with every local defined:

```cpp
bool setupSlideConstraint(CurrentWeapon* weap)
{
    using namespace rock::provider;

    if (!g_rockApi || !g_rockOwnerToken ||
        !weap || !weap->primaryNode01) {
        return false;
    }

    const char* wantedSource =
        weap->primaryNode01->m_name.c_str();
    if (!wantedSource || !wantedSource[0]) {
        return false;
    }

    const std::uint32_t requestedCount =
        g_rockApi->getWeaponEvidenceDetailCountV1();
    if (requestedCount == 0 ||
        requestedCount > ROCK_PROVIDER_MAX_WEAPON_BODIES) {
        return false;
    }

    std::vector<RockProviderWeaponEvidenceDetailV1> details(
        requestedCount);
    const std::uint32_t copiedCount =
        g_rockApi->copyWeaponEvidenceDetailsV1(
            details.data(),
            requestedCount);
    details.resize(copiedCount);

    // One authored source can have multiple generated collision hulls. Select
    // every body for exactly primaryNode01 and no other source/part.
    std::vector<RockProviderWeaponEvidenceDetailV1> boltBodies;
    for (const auto& detail : details) {
        const bool exactSource =
            std::strcmp(detail.sourceName, wantedSource) == 0;
        const bool isBolt =
            detail.partKind ==
            static_cast<std::uint32_t>(
                RockProviderWeaponPartKindV1::Bolt);

        if (exactSource && isBolt) {
            boltBodies.push_back(detail);
        }
    }

    if (boltBodies.empty()) {
        // Log the evidence list here. Do not fall back to MatchPartKind,
        // barrel, receiver, proximity, or the legacy whole-weapon grab.
        return false;
    }

    const float travel =
        weap->reloadSlidePos - weap->maxBoltPosition;
    if (!std::isfinite(travel) || travel <= 0.0f) {
        return false;
    }

    std::vector<RockProviderWeaponPartTargetV1> targets;
    std::vector<RockProviderWeaponPartMotionConstraintV1> constraints;
    targets.reserve(boltBodies.size());
    constraints.reserve(boltBodies.size());

    for (const auto& boltEvidence : boltBodies) {
        RockProviderWeaponPartTargetV1 target{};
        target.flags = static_cast<std::uint32_t>(
            RockProviderWeaponPartTargetFlagV1::MatchBodyId);
        target.grabMode =
            RockProviderWeaponPartGrabModeV1::AttachOnly;
        target.weaponGenerationKey =
            boltEvidence.weaponGenerationKey;
        target.bodyId = boltEvidence.bodyId;
        targets.push_back(target);

        RockProviderWeaponPartMotionConstraintV1 motion{};
        motion.flags = static_cast<std::uint32_t>(
            RockProviderWeaponPartTargetFlagV1::MatchBodyId);
        motion.kind =
            RockProviderWeaponPartMotionKindV1::Linear;
        motion.axisSpace =
            RockProviderWeaponPartDriveSpaceV1::
                ControlledRootParentLocal;
        motion.weaponGenerationKey =
            boltEvidence.weaponGenerationKey;
        motion.bodyId = boltEvidence.bodyId;
        motion.axisDirection[0] = 0.0f;
        motion.axisDirection[1] = -1.0f;
        motion.axisDirection[2] = 0.0f;
        motion.minValue = 0.0f;
        motion.maxValue = travel;
        motion.controlledRoot =
            reinterpret_cast<std::uintptr_t>(
                weap->primaryNode01);
        constraints.push_back(motion);
    }

    const auto targetResult =
        g_rockApi->setWeaponPartTargetsV1(
            g_rockOwnerToken,
            targets.data(),
            static_cast<std::uint32_t>(targets.size()));
    if (targetResult != RockProviderResultV1::Ok) {
        return false;
    }

    const auto constraintResult =
        g_rockApi->setWeaponPartMotionConstraintsV1(
            g_rockOwnerToken,
            constraints.data(),
            static_cast<std::uint32_t>(constraints.size()));
    if (constraintResult != RockProviderResultV1::Ok) {
        g_rockApi->clearWeaponPartTargetsV1(
            g_rockOwnerToken);
        return false;
    }

    return true;
}
```

The example intentionally does not set `axisOrigin`: ROCK linear constraints
measure hand displacement relative to the part and clean tracked hand pose
captured on the grab frame. `axisOrigin` is used for rotational constraints.

`axisDirection = (0,-1,0)` defines the increasing rail coordinate; it is not
a one-way input lock. With `minValue = 0` and `maxValue = travel`, the player
can pull back and push forward anywhere inside that interval.

`weaponGenerationKey` comes from the same evidence record as `bodyId`. It
prevents a numeric body ID from accidentally selecting a different weapon
after an equip or collision rebuild.

## Detecting when the bolt reaches maximum travel

`RockProviderWeaponPartGripStateV1::handPartLocal` is the hand-to-part
transform captured when the grip starts. It does not change as the constrained
part moves and therefore cannot be used as the bolt's current rail coordinate.

Because the example uses `ControlledRootParentLocal`, ROCK writes
`weap->primaryNode01->m_localTransform`. Save that node's local position on the
first grabbed frame, then project its live displacement onto the same normalized
axis supplied to the constraint:

```cpp
NiPoint3 boltLocalPositionAtGrab{};
bool haveBoltGrabPosition = false;

void updateBoltTravel(CurrentWeapon* weap, bool isGrabbing)
{
    if (!weap || !weap->primaryNode01) {
        haveBoltGrabPosition = false;
        return;
    }

    const NiPoint3 current =
        weap->primaryNode01->m_localTransform.pos;

    if (!isGrabbing) {
        haveBoltGrabPosition = false;
        return;
    }

    if (!haveBoltGrabPosition) {
        boltLocalPositionAtGrab = current;
        haveBoltGrabPosition = true;
    }

    // axisDirection is (0,-1,0), so dot(current - start, axis)
    // simplifies to start.y - current.y.
    const float currentTravel =
        boltLocalPositionAtGrab.y - current.y;
    const float maximumTravel =
        weap->reloadSlidePos - weap->maxBoltPosition;
    constexpr float endpointTolerance = 0.02f;

    if (currentTravel >= maximumTravel - endpointTolerance) {
        weap->maxBoltReached = true;
    }
}
```

If `primaryNode01->m_localTransform` does not change, first verify that the
submitted constraint sets:

```cpp
motion.axisSpace =
    RockProviderWeaponPartDriveSpaceV1::ControlledRootParentLocal;
motion.controlledRoot =
    reinterpret_cast<std::uintptr_t>(weap->primaryNode01);
```

Without `controlledRoot`, ROCK moves the exact matched collision source. That
source may be a generated mesh leaf rather than the authored
`primaryNode01` that Virtual Reloads polls.

## Extending the rail to the closed position while still gripping

The consumer may call `setWeaponPartMotionConstraintsV1` again with the same
exact body matcher while the grip remains active. Heisenberg refreshes
`minValue` and `maxValue` in place while preserving the original at-grab
reference, so this does not require an API change, a release, or a re-grab.

For Cylon's negative-Y rail, the initial dry-fire-to-cocked interval is:

```cpp
motion.minValue = 0.0f;
motion.maxValue =
    weap->reloadSlidePos - weap->maxBoltPosition;
```

After the bolt reaches the cocked endpoint, resubmit every exact-body
constraint with:

```cpp
motion.minValue =
    weap->reloadSlidePos - weap->originalBoltPosition;
motion.maxValue =
    weap->reloadSlidePos - weap->maxBoltPosition;
```

The new minimum is negative when the closed/rest Y position is forward of the
dry-fire Y position. The same held hand can then push the bolt through
coordinate zero to the closed endpoint. Only the legal range is updated
mid-grip; changing the motion kind, axis space, or axis direction takes effect
on the next fresh grab to avoid reinterpreting the existing hand displacement
and snapping the part.

## Holding the dry-fire pose

If Virtual Reloads wants a set-and-forget drive, use the persistent lease
constant and target the same authored node:

```cpp
bool holdDryPos(CurrentWeapon* weap)
{
    using namespace rock::provider;

    if (!g_rockApi || !g_rockOwnerToken ||
        !weap || !weap->primaryNode01) {
        return false;
    }

    RockProviderWeaponPartDriveTargetV1 drive{};
    drive.flags = static_cast<std::uint32_t>(
        RockProviderWeaponPartTargetFlagV1::MatchSourceName);
    drive.driveSpace =
        RockProviderWeaponPartDriveSpaceV1::
            ControlledRootParentLocal;
    std::snprintf(
        drive.sourceName,
        sizeof(drive.sourceName),
        "%s",
        weap->primaryNode01->m_name.c_str());

    drive.targetTransform =
        weap->primaryNode01->m_localTransform;
    drive.targetTransform.translate[1] =
        weap->reloadSlidePos;
    drive.controlledRoot =
        reinterpret_cast<std::uintptr_t>(
            weap->primaryNode01);
    drive.leaseFrames =
        ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1;

    return g_rockApi->setWeaponPartDriveTargetsV1(
               g_rockOwnerToken,
               &drive,
               1) == RockProviderResultV1::Ok;
}
```

`drive.targetTransform = node->m_localTransform;` is supported directly by
the header adapter; manual copying of the matrix, position, and scale is not
required.

ROCK applies a matching hand constraint after a drive in the same frame, so
the dry-fire hold keeps the part open before it is grabbed while the
constraint has authority during the grab.

## Cleanup

```cpp
void clearSlideConstraint()
{
    using namespace rock::provider;
    if (!g_rockApi || !g_rockOwnerToken) {
        return;
    }

    (void)g_rockApi->clearWeaponPartTargetsV1(
        g_rockOwnerToken);
    (void)g_rockApi->clearWeaponPartMotionConstraintsV1(
        g_rockOwnerToken);
}

void clearDryPose()
{
    if (g_rockApi && g_rockOwnerToken) {
        (void)g_rockApi->clearWeaponPartDriveTargetsV1(
            g_rockOwnerToken);
    }
}

void disconnectFromRock()
{
    if (g_rockApi && g_rockOwnerToken) {
        clearSlideConstraint();
        clearDryPose();
        (void)g_rockApi->unregisterConsumerV1(
            g_rockOwnerToken);
    }
    g_rockOwnerToken = 0;
    g_rockApi = nullptr;
}
```

Clear the target, constraint, and drive on success, cancellation, holster,
weapon change, and plugin shutdown.

## Exact contact behavior in this build

Exclusive targets never fall back to another weapon part or the legacy
whole-weapon selector. A target attaches only when a generated hand collider
actually overlaps the selected body's mesh; ROCK can reconstruct that exact
mesh contact when Havok omits the callback for a tiny keyframed part.

The stock 10 mm `Pistol10mmBoltRelease:0` is no longer classified as a Bolt,
so `MatchPartKind::Bolt` cannot accidentally select the release button.
Exact `MatchBodyId` remains the recommended selector.

`ROCKAPI_GetProviderApi` is the stable export. `RockProviderApi::initialize`
checks standalone `ROCK.dll` first for compatibility, then the embedded
`Heisenberg_F4VR.dll`.
