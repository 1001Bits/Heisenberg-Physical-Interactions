## Background: what you're connecting to

Heisenberg embeds the full ROCK physics/grab engine directly inside `Heisenberg_F4VR.dll` — there
is no separate `ROCK.dll` on disk or loaded in the process. ROCK ships a "provider API"
specifically meant for other mods to consume: weapon-part classification (which node is the
magazine, the bolt, the charging handle...), a whitelist mechanism to make specific parts
grabbable, live grip-state reporting, and the ability to drive a part's transform yourself. It's
built for exactly the "reload mod grabs the real weapon part" use case.

## Step 1 — Get the header

You need one file: `ROCKProviderApi.h`. Pull it from this project's
`external/ROCK/src/api/ROCKProviderApi.h` — it's vendored unmodified from upstream ROCK, so it's
the same file you'd get from a standalone ROCK.dll build. Copy it into your own project's include
path; you don't need any other ROCK source files, just this one header (it's self-contained aside
from standard headers).

## Step 2 — Find the API at runtime

As of 2026-07-21 the header itself does the module lookup for you —
`rock::provider::RockProviderApi::initialize()` tries `ROCK.dll` first, then falls back to
`Heisenberg_F4VR.dll`, so the same call works whether the player has standalone ROCK or Heisenberg
with ROCK embedded:

```cpp
#include "ROCKProviderApi.h"

bool AcquireRockProviderApi()
{
    using namespace rock::provider;
    const int status = RockProviderApi::initialize(); // 0 == success
    return status == 0; // RockProviderApi::inst now points at the live table
}

// ... later ...
const rock::provider::RockProviderApi* api = rock::provider::RockProviderApi::inst;
```

Call this after your plugin receives F4SE's `kGameLoaded` message — the host DLL needs to have
finished its own init first. A non-zero status isn't a hard error (host not installed yet, or an
older build below the version/table-size you asked for) — retry later or disable the integration
for this session. If you'd rather do the module/proc lookup yourself (e.g. to log which module
answered), `GetProcAddress(module, "ROCKAPI_GetProviderApi")` against either module name still
works exactly as before — `initialize()` is just that same lookup, done for you.

## Step 3 — Register as a consumer

Most of the interesting functions (anything that lets you change state, like declaring grabbable
parts) require an `ownerToken`, which you get by registering:

```cpp
using namespace rock::provider;

const RockProviderApi* g_rockApi = nullptr;
std::uint64_t g_rockOwnerToken = 0;
std::uint32_t g_rockGrantedCapabilities = 0;

bool ConnectToRock()
{
    g_rockApi = AcquireRockProviderApi();
    if (!g_rockApi || !g_rockApi->isProviderReady())
        return false;

    RockProviderConsumerRegistrationV1 registration{};
    std::snprintf(registration.modName, sizeof(registration.modName), "VirtualReloads");
    registration.requestedCapabilities =
        static_cast<std::uint32_t>(RockProviderConsumerCapabilityV1::WeaponPartInteraction);

    RockProviderConsumerHandleV1 handle{};
    if (g_rockApi->registerConsumerV1(&registration, &handle) != RockProviderResultV1::Ok)
        return false;

    g_rockOwnerToken = handle.ownerToken;
    g_rockGrantedCapabilities = handle.grantedCapabilities;

    // Confirm the parts of the table you need actually exist at this ABI size before calling
    // into them — a running host may be an older build with a smaller struct.
    RockProviderLimitsV1 limits{};
    if (!g_rockApi->getProviderLimitsV1(&limits) || !supportsWeaponPartInteractionV1(limits))
        return false;

    return true;
}
```

Keep `g_rockOwnerToken` — every weapon-part call below takes it as the first argument, and it's
how ROCK knows which whitelist/drive entries belong to you versus another consumer.

## Step 4 — The actual reload flow

This is the part that gets you "the same parts a reload animation would reference, with full
collision":

**4a. Find the real parts.** When the player has a weapon equipped and you're about to start a
reload, enumerate its parts:

```cpp
std::uint32_t count = g_rockApi->getWeaponEvidenceDetailCountV1();
std::vector<RockProviderWeaponEvidenceDetailV1> details(count);
g_rockApi->copyWeaponEvidenceDetailsV1(details.data(), count);

for (const auto& part : details)
{
    // part.partKind: Magazine, Bolt, Slide, ChargingHandle, Cylinder, ...
    // part.reloadRole / part.actionRole: which reload step this part belongs to
    // part.omodFormId: the ACTUAL installed magazine/mod occupying this slot (0 if vanilla) —
    // this is what makes it "the same part the reload animation references" rather than a
    // guess from the NIF name.
}
```

**4b. Make the right part grabbable for this reload step.** When your reload state machine
reaches "player should grab the magazine," whitelist just that part:

```cpp
RockProviderWeaponPartTargetV1 target{};
target.flags = static_cast<std::uint32_t>(RockProviderWeaponPartTargetFlagV1::MatchPartKind);
target.grabMode = RockProviderWeaponPartGrabModeV1::FullTwoHandAuthority; // or AttachOnly
target.partKind = static_cast<std::uint32_t>(RockProviderWeaponPartKindV1::Magazine);

g_rockApi->setWeaponPartTargetsV1(g_rockOwnerToken, &target, 1);
```

While this whitelist is active, only parts matching your target(s) grab normally — everything
else behaves as it would without your mod involved.

**4c. Know when the player grabs it.** Poll every frame (or on your own tick):

```cpp
RockProviderWeaponPartGripStateV1 state{};
if (g_rockApi->getWeaponPartGripStateV1(RockProviderHand::Right, &state) && state.active)
{
    // state.partKind / state.reloadRole tell you what got grabbed;
    // state.gripSequence increments on every fresh grab, so compare it to the last value you
    // saw to detect a new attach without needing a callback.
}
```

This is real Havok contact — the player is physically touching and holding the actual part body,
not a scripted proxy, so it collides with the rest of the world normally while held.

**4d. Optional — drive the part yourself.** If part of your reload sequence should animate
before or independent of the player's hand (e.g. the mag auto-ejecting partway before the player
takes it), you can write the part's transform directly while ROCK keeps the grabbing hand glued
to it:

```cpp
RockProviderWeaponPartDriveTargetV1 drive{};
drive.bodyId = /* from the evidence detail you found in 4a */;
drive.driveSpace = RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal;
drive.targetTransform = /* your animated transform */;
drive.leaseFrames = 1; // short finite lease; re-send while this animation advances

g_rockApi->setWeaponPartDriveTargetsV1(g_rockOwnerToken, &drive, 1);
```

**4e. Clean up.** When your reload finishes (or the player cancels), clear both:

```cpp
g_rockApi->clearWeaponPartTargetsV1(g_rockOwnerToken);
g_rockApi->clearWeaponPartDriveTargetsV1(g_rockOwnerToken);
```

## Step 5 — Your own node-name mapping, with real collision and correct constrained motion

If you already have your own curated per-archetype node-name maps (built from real-world testing —
"a slide lives in one of these 4 specific nodes for this archetype," break-actions use the barrel
node instead of P-Mag, etc.), you don't need ROCK's own classification heuristics at all. Target
the exact node by name (`MatchSourceName` is an exact, case-sensitive match against the live NIF
node's real name — no fuzzy matching, so whatever string is in your map is what to send):

```cpp
RockProviderWeaponPartTargetV1 target{};
target.flags = static_cast<std::uint32_t>(RockProviderWeaponPartTargetFlagV1::MatchSourceName);
target.grabMode = RockProviderWeaponPartGrabModeV1::AttachOnly; // required for a motion constraint - see below
std::snprintf(target.sourceName, sizeof(target.sourceName), "%s", yourMappedNodeName.c_str());

g_rockApi->setWeaponPartTargetsV1(g_rockOwnerToken, &target, 1);
```

`AttachOnly` grabs give the player real Havok collision on the part (it's a normal generated body,
same as any other ROCK weapon-part hull) but the hand only glues to wherever the part's node
currently sits — nothing makes the part itself move correctly along a slide rail or a hinge on its
own. That's what a **motion constraint** is for: it tells ROCK to project the grabbing hand's
motion onto an axis you define, instead of letting the part follow the hand freely in 3D.

Check support once (this feature may not exist on an older host build), then set a constraint for
the same part you just whitelisted — match it by the same identity fields (exact node name here
again is the simplest and most reliable), before or the same frame the player is expected to grab
it:

```cpp
RockProviderLimitsV1 limits{};
const bool haveMotionConstraints = g_rockApi->getProviderLimitsV1(&limits) &&
    supportsWeaponPartMotionConstraintV1(limits);

if (haveMotionConstraints)
{
    RockProviderWeaponPartMotionConstraintV1 constraint{};
    constraint.kind = RockProviderWeaponPartMotionKindV1::Linear;      // or Rotational
    constraint.axisSpace = RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal;
    std::snprintf(constraint.sourceName, sizeof(constraint.sourceName), "%s", yourMappedNodeName.c_str());

    // Linear: axisOrigin is the path start point, axisDirection is the slide direction (unit
    // vector), min/maxValue are game units of travel from axisOrigin.
    // Rotational: axisOrigin is the hinge pivot, axisDirection is the hinge axis (unit vector),
    // min/maxValue are degrees swept around it.
    constraint.axisOrigin[0] = 0.0f;  constraint.axisOrigin[1] = 0.0f;  constraint.axisOrigin[2] = 0.0f;
    constraint.axisDirection[0] = 0.0f; constraint.axisDirection[1] = 0.0f; constraint.axisDirection[2] = 1.0f;
    constraint.minValue = 0.0f;
    constraint.maxValue = 3.2f; // e.g. slide travel in game units - these are entirely yours to set,
                                // per weapon/archetype, from whatever you measure or author

    g_rockApi->setWeaponPartMotionConstraintsV1(g_rockOwnerToken, &constraint, 1);
}
```

Once both are set: the player grabs the real part with real collision (identified by your own
mapping, not ROCK's), and as their hand moves, the part slides or swings exactly along the axis you
specified — clamped to your min/max — instead of following the hand's raw 3D position. The part's
rotation stays fixed for a Linear constraint (pure translation, correct for a slide/detach path);
for Rotational, the part's translation sweeps through the same arc as its rotation around
`axisOrigin`, so a part whose own origin isn't exactly at the pivot still swings through a real arc
rather than spinning in place.

Clear it alongside the target when your reload step ends:

```cpp
g_rockApi->clearWeaponPartMotionConstraintsV1(g_rockOwnerToken);
g_rockApi->clearWeaponPartTargetsV1(g_rockOwnerToken);
```

A constraint only ever applies to an `AttachOnly` grip — `FullTwoHandAuthority` already steers the
whole weapon and has no single part to constrain independently, so setting one for a
`FullTwoHandAuthority` target is simply ignored (no error, `getWeaponPartGripStateV1` just won't
show constrained motion for it).

## Step 6 — Positioning a part WITHOUT a grab (dry-fire slide lock, bolt hold-open, hammer states)

`setWeaponPartDriveTargetsV1` is not just for mid-grab animation — it is the general "set this
node's transform" call, and it works with **no whitelist, no grip, and no collision body** on the
part. Verified against the engine implementation (`PhysicsInteraction::applyProviderWeaponPartDrives`):

- **Node resolution**: `MatchSourceName` matches the exact NIF node name first against collision
  evidence, then **falls back to a live search of the equipped weapon's node tree** — so a slide
  that was never whitelisted or touched still resolves.
- **What it writes**: the node's actual `local` transform (then `updateTransformsDown`), re-applied
  every frame while the drive is alive — it survives the game/FRIK re-posing the tree each frame.
- **Spaces**: `SourceParentLocal` = the transform you supply IS the node's new local transform
  relative to its own parent (what you want for "slide at rest, translated back 2.4 units along
  its rail"). `WeaponRootLocal` = relative to the weapon root node instead.
- **Automatic restore**: the engine snapshots the node's original local on first drive and
  **restores it automatically** when the drive lease expires, when you call
  `clearWeaponPartDriveTargetsV1`, or when the weapon regenerates (holster/re-equip). You never
  have to put the slide back yourself.

Dry-fire example — hold the slide locked back until the state ends:

```cpp
RockProviderWeaponPartDriveTargetV1 drive{};
drive.flags = static_cast<std::uint32_t>(RockProviderWeaponPartTargetFlagV1::MatchSourceName);
drive.driveSpace = RockProviderWeaponPartDriveSpaceV1::SourceParentLocal;
std::snprintf(drive.sourceName, sizeof(drive.sourceName), "%s", slideNodeName.c_str());
drive.targetTransform = slideNode->m_localTransform;          // legacy F4SE NiTransform converts directly
drive.targetTransform.translate[Y] -= slideLockBackTravel;    // ...offset along the rail axis
drive.leaseFrames = ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1;
g_rockApi->setWeaponPartDriveTargetsV1(g_rockOwnerToken, &drive, 1);

// chamber a round / reload complete:
g_rockApi->clearWeaponPartDriveTargetsV1(g_rockOwnerToken);   // slide snaps home automatically
```

The published `0xFFFFFF` value is the persistent sentinel, so existing consumers using that
literal remain source- and behavior-compatible; the named constant makes the intent explicit.
Persistent drives are still removed by `clearWeaponPartDriveTargetsV1`, owner unregister, or
provider lifecycle loss. New consumers that also support older ROCK hosts can query
`supportsWeaponPartDrivePersistentLeaseV1()` and fall back to refreshing a short lease when it
returns false. A small `leaseFrames` value remains useful for fail-safe, continuously updated
animation.

Notes: the direct assignment adapter accepts the legacy F4SE `NiTransform` layout used by
`m_localTransform` (`rot.data`, `pos`, and `scale`) without exposing that engine type in ROCK's ABI.
If two drives target the same node, the higher `priority` wins. Set `weaponGenerationKey` from the
frame snapshot when the drive must be restricted to the currently equipped weapon generation;
leaving it zero intentionally permits the matcher to resolve on later generations too. The drive
requires an equipped weapon with a live generation key (i.e. ROCK's weapon collision has seen the
weapon — always true in normal play with the embed).

## Export stability

`ROCKAPI_GetProviderApi` is a documented, intentional public export as of 2026-07-21 (see the
comment on its declaration in `ROCKProviderApi.h` and its definition in `ROCKProviderApi.cpp`) —
not an accident of a leftover build flag. It's expected to stay put; the API widens by bumping
`ROCK_PROVIDER_API_VERSION` and appending new function-table members (check
`getProviderLimitsV1`/`supportsXV1` before calling anything new, same as today), never by changing
this entry point's signature.
