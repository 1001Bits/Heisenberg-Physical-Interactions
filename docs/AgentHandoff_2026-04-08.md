# Agent Handoff Overview (2026-04-08)

This document is for the next coding agent working in the Heisenberg repo.

It reflects the live tree as of 2026-04-08 and supersedes the older assessment that said hand collision was effectively stubbed. That older statement is no longer accurate: the repo now contains active hand-collision, HeldBody, and geometry finger-curve work, although several paths are still experimental.

## Scope

This handoff is focused on:

- automatic hand placement and finger curls
- HeldBody grabbing
- hand collision
- the recent ROCK-derived architecture decisions that are still relevant

I did not build or run the mod while preparing this handoff.

## Repo Context

- Project root: `c:\Development\higgs-master\Heisenberg - Interactive Activators main mod source`
- Main runtime code: `src`
- Docs folder: `docs`
- Config load order in `src/Config.cpp`:
  1. embedded defaults
  2. `Data/F4SE/Plugins/Heisenberg_F4VR.ini`
  3. `Data/MCM/Settings/Heisenberg.ini`

Important implication: `Config.h` field defaults are not always the actual runtime defaults. The embedded INI text in `Config.cpp` overrides some of them.

## High-Level Current State

### 1. Automatic hand placement and finger curls are live work, not just theory

Relevant files:

- `src/FingerCurves.cpp`
- `src/FingerCurves.h`
- `src/FingerCurveData.cpp`
- `src/FingerCurveData.h`
- `src/Grab.cpp`

Current state:

- A full HIGGS-style geometry finger-curve system was added.
- `Grab.cpp` still contains runtime geometry helpers such as `TryCalculateRuntimeHandPlacementFromGeometry(...)` and `TryCalculateRuntimeFingerCurlsFromGeometry(...)`.
- `CalculateFingerCurlFromGeometry(...)` is now wired into the grab flow.
- Embedded config defaults now enable both automatic hand placement and automatic finger curls.

Practical read on this:

- The repo is no longer only using saved offsets for everything.
- There is now real triangle extraction and finger-curve evaluation in-tree.
- This path still needs normal verification on real objects, especially odd meshes and skinned content.

### 2. HeldBody is actively integrated

Relevant files:

- `src/HeldBodyGrab.cpp`
- `src/HeldBodyGrab.h`
- `src/Grab.cpp`
- `src/Grab.h`
- `src/GrabConstraint.cpp`
- `src/GrabConstraint.h`
- `src/Heisenberg.cpp`

Current state:

- `HeldBodyGrabManager` exists and is initialized from `Heisenberg.cpp` when config enables it.
- `Grab.cpp` now routes grabs through `HeldBodyGrabManager::StartGrab`, `UpdateGrab`, and `EndGrab`.
- `GrabState` has HeldBody-specific fields such as `usingHeldBodyGrab`, `constraintId`, `handBodyId`, and tau state.
- `GrabConstraint.cpp` is no longer just dead stub code; major constraint code was re-enabled and several function signatures were corrected.

Runtime/config notes:

- Embedded defaults currently set `bUseHeldBodyGrab = true`.
- Embedded defaults currently set `iHeldBodyMode = 0`, which means spring HeldBody mode.
- Embedded defaults currently set `bUseSimpleHandBodyCreation = true`.
- `Config.cpp` explicitly logs that `iGrabMode=0` still forces the original keyframed backend, even if HeldBody is enabled.

What that means in practice:

- The preferred dynamic-grab direction in the tree is now HeldBody, not just the original keyframed grab.
- There are multiple HeldBody sub-modes in play: spring, custom 6-DOF, and native constraint/ragdoll.
- The current default is still the spring-flavored HeldBody path, not the full constraint path.

Important caveats:

- `HeldBodyGrab.cpp` is large and heavily uses manual Havok allocations, SEH wrappers, and reverse-engineered calling conventions.
- There is at least one explicit TODO left in that file about reading actual object mass.
- Treat the custom-constraint and native-ragdoll branches as more experimental than the spring path.

### 3. Hand collision is now active again

Relevant files:

- `src/HandCollision.cpp`
- `src/HandCollision.h`
- `src/Hooks.cpp`
- `src/Heisenberg.cpp`
- `src/Physics.cpp`
- `src/PlayerCharacterProxyListener.cpp`

Current state:

- `Heisenberg.cpp` initializes `HandCollision` when `enableHandCollision` is true.
- Actual per-frame hand-collision management is now driven from `Hooks.cpp`, not from `Heisenberg.cpp` update code.
- `HookPlayerCharacterUpdate(...)` calls a local `UpdateHandCollisionBodies()` helper.
- That helper calls:
  - `HandCollision::CreateBodiesIfNeeded(...)`
  - `HandCollision::ApplyPlayerPairFilterIfNeeded()`
  - `HandCollision::Update(...)`

This is the current live hook timing model:

- create bodies in the pre-physics-style player update hook
- apply pair filtering there
- update hand-body positions there as well
- flush queued haptics later from the post-physics hook

### 4. The earlier "HandCollision is just stubbed" summary is obsolete

That older summary was true for the earlier snapshot that was inspected, but it is no longer true for the current tree.

The file now has both:

- an older disabled implementation block at the top under `#if 0`
- a later active implementation below that block

This matters because:

- grep/search results can hit disabled code first
- there are duplicate function names inside the dead block and the live block
- an agent can easily read the wrong occurrence and misdiagnose the current state

When inspecting `src/HandCollision.cpp`, make sure you are reading the active section below the top disabled block.

## Hand Collision Architecture Right Now

### Active pattern

The current hand-collision approach is:

1. Create actual KEYFRAMED hand bodies through Bethesda wrappers.
2. Avoid player self-collision using pair filtering against the player body.
3. Keep the proxy-listener path disabled.
4. Keep proximity fallback and queued haptics active alongside the native-body path.

### What is deliberately disabled

The custom player proxy listener path is still intentionally disabled.

Reason captured in code:

- synthetic listener registration causes vtable corruption or save/load instability
- until the full VR proxy listener ABI is mapped, the safer path is pair filtering

That means the current hand-collision path is not using the synthetic proxy callback design as the primary solution.

### Body creation pattern now used

The active hand-body creation code follows the Bethesda-wrapper path:

1. build `hknpPhysicsSystemData`
2. allocate material/body cinfo
3. construct `bhkPhysicsSystem`
4. call `BhkPhysicsSystemCreateInstance`
5. construct `bhkNPCollisionObject`
6. call `AddToWorld`
7. get body id from `bhkPhysicsSystem`
8. set motion type to KEYFRAMED
9. commit and activate

This is consistent with the safer direction from the earlier review and with the best parts of the ROCK findings.

### Step listener status

`HandCollision.h` still exposes step-listener related API:

- `RegisterStepListener()`
- `UnregisterStepListener()`
- `OnPrePhysicsStep()`
- `HandCollisionStepListener`

But in the current tree I did not find a live runtime registration path for that listener. The active flow is the `HookPlayerCharacterUpdate(...)` path in `Hooks.cpp`.

Treat the step-listener API as dormant or unfinished unless you confirm otherwise before editing it.

## Automatic Placement / Finger Curl Notes

The geometry-based path is now worth treating as a real subsystem.

Important facts:

- `Config.h` still shows `enableAutomaticHandPlacement` and `enableAutomaticFingerCurls` as false by struct default.
- Embedded config in `Config.cpp` overrides those to true.
- `Grab.cpp` contains both hand-placement and finger-curl runtime geometry logic.
- `FingerCurves.cpp` contains real triangle extraction and curve intersection work, not just placeholders.

Practical caution:

- Do not assume the feature is off just because `Config.h` shows false defaults.
- Do not assume the feature is complete just because the runtime path exists.
- Validate behavior against real in-game meshes before simplifying or refactoring it.

## ROCK Findings That Still Matter

External repo: `https://github.com/brunocatani/ROCK`

The most useful ROCK takeaways for this repo are still:

- Prefer Bethesda-backed body creation and wrapper correctness over orphan raw `hknp` bodies.
- For real held-object collision, the strongest direction is still DYNAMIC held objects driven by constraints, not pure visual keyframing.
- Mesh/surface/skinned-mesh extraction is the right source of truth for better hand placement and finger curl.
- Pair filtering is currently safer than reviving the synthetic proxy-listener path.
- Back-pointer/wrapper correctness and body-map registration details matter a lot in FO4VR Havok.

ROCK areas to copy carefully, not blindly:

- hybrid wrapper/body creation code
- unfinished contact/signal paths
- any path that assumes its reverse-engineered calling convention is already verified in this repo

## Working Tree State On 2026-04-08

The working tree is dirty. Do not assume you can freely refactor or revert.

Current files visibly in active flux include:

- `CMakeLists.txt`
- `src/Config.cpp`
- `src/Config.h`
- `src/Grab.cpp`
- `src/Grab.h`
- `src/GrabConstraint.cpp`
- `src/GrabConstraint.h`
- `src/HandCollision.cpp`
- `src/HandCollision.h`
- `src/HeldBodyGrab.cpp`
- `src/HeldBodyGrab.h`
- `src/Hooks.cpp`
- `src/Heisenberg.cpp`
- `src/FingerCurves.cpp`
- `src/FingerCurves.h`
- `src/FingerCurveData.cpp`
- `src/FingerCurveData.h`

There are also untracked additions right now, including:

- `src/ContactImpulseListener.cpp`
- `src/ContactImpulseListener.h`
- `src/WeaponCollision.cpp`
- `src/WeaponCollision.h`
- `src/api/api/`
- `src/cmake/cmake/`

Implication for the next agent:

- read before editing
- avoid broad cleanup passes
- do not trust older summaries over current file contents

## Suggested Reading Order For The Next Agent

1. `src/Hooks.cpp`
   - confirm the live hand-collision call chain
2. `src/HandCollision.cpp`
   - read the active implementation below the top disabled block
3. `src/HandCollision.h`
   - understand public API and dormant step-listener surface
4. `src/Grab.cpp`
   - inspect HeldBody routing and geometry placement/curl hooks
5. `src/HeldBodyGrab.cpp`
   - inspect current default HeldBody backend and hand-body creation approach
6. `src/GrabConstraint.cpp` and `src/GrabConstraint.h`
   - inspect corrected signatures and constraint implementations
7. `src/Config.cpp` and `src/Config.h`
   - verify real runtime defaults before changing behavior

## Likely Next Tasks

If continuing this line of work, the most sensible next steps are:

1. Verify the active hand-collision path end-to-end across game load, cell change, and save/load.
2. Treat HeldBody spring mode as the primary stability target before spending more time on custom 6-DOF or native ragdoll modes.
3. Validate geometry-based finger curl and automatic hand placement on a representative set of clutter, weapons, and skinned meshes.
4. Only revisit the proxy-listener path if you are explicitly mapping the full VR listener ABI.
5. Clean out dead duplicate code only after the live path is proven stable.

## Short Version

If you only remember four things, remember these:

- hand collision is active again in the current tree
- the live hand-collision path is hook-driven and pair-filter based, not proxy-listener based
- HeldBody is actively integrated and now the main dynamic-grab direction
- old summaries are stale if they say these systems are still mostly stubbed