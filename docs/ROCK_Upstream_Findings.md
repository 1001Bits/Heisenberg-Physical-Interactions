# ROCK — findings from the Heisenberg fork

Hi Bruno,

Heisenberg (Physical Interactions VR) embeds ROCK as its physics engine. In the course of
chasing our own bugs we ended up reading a lot of your code closely, and a handful of the
things we fixed look like they are still present in the public tree. This is that list.

**Comparison basis:** our vendored copy against `github.com/brunocatani/ROCK` at `4c4034e`
(2026-07-30, "0.6 ROCK Framework"). Of the shared source files: **194 byte-identical, 94
modified by us, 41 we have that the public tree does not.**

**How this was produced.** Every claim below was read directly in *both* trees and then
adversarially re-checked by a second pass whose explicit job was to kill it. 8 of 44 candidate
findings died that way — including two of our own melee claims and a contact-subscription one —
and are not in this document. Where confidence is less than solid I say so in the item itself
rather than burying it.

**Nothing here needs anything from us.** No credit, no attribution, no reciprocity. If any of
it is useful, take it; if you've already considered and rejected it, that's genuinely useful for
us to know too.

---

## Read this first: our FRIK differs from yours

Your tree calls `FRIKAPI_GetApiStructSize` (`api/FRIKApi.h:281`). Ours has zero occurrences of
it — we're on stock FRIK 77.x with the versioned `FRIKAPI_GetApi` only. So the FRIK-facing
surface is genuinely different between our builds, and anything that depends on hand/arm
authority, node naming, or the FRIK API shape may not map cleanly onto pFRIK.

I've therefore split the findings:

- **Tier A — engine level.** Havok, native FO4VR functions, CommonLib, pure transform math.
  These don't touch FRIK at all, so the fork difference is irrelevant. Items 1–4 and 6.
- **Tier B — may not apply.** Touches the hand-pose publish path, so pFRIK's shape may already
  differ. Item 5, flagged inline.

---

## 1. Physics-thread callbacks are never drained before `PhysicsInteraction` is deleted

**Tier A · use-after-free · standalone · high confidence**

`destroyPhysicsInteraction()` sets `s_hooksEnabled = false` and runs straight through to
`delete s_physicsInteraction` (`ROCKMain.cpp:235-247`) without waiting on anything. Both hot
physics-thread callbacks do check-then-use across that window:

- `hookedProcessConstraintsCallback` — tests `s_hooksEnabled` at `PhysicsHooks.cpp:1580`, loads
  `s_instance` at `:1597`, dereferences at `:1598-1600`
- `onContactCallbackUnsafe` — tests at `PhysicsInteractionContacts.inl:430`, loads at `:437`,
  calls `handleContactEvent` at `:458`

A Havok-thread callback that passed the gate one instruction before the main-thread store runs
its whole body on freed memory. `s_instance` is only cleared in `~PhysicsInteraction`
(`PhysicsInteraction.cpp:1360`) — after `delete` has already begun — so it can't close the
window. The trampoline is installed once behind a static flag and never removed, so
`s_hooksEnabled` is the only gate there is.

**Why it matters beyond process exit:** `destroyPhysicsInteraction` also fires mid-session at
`ROCKMain.cpp:217` (recreate), `:306`, `:648` (SkeletonDestroying) and `:755` (provider lost) —
so it's reachable on cell transitions, `coc`, race/skeleton swaps and provider loss. The SEH
`__try` around the callback body turns the access violation into a silent swallow at best, and
a corrupted-solver crash at worst.

**You already have the right primitive.** `PhysicsCallbackQuiescenceGate.h`
(`tryEnterCallback` / `pauseForMutation` / `waitForCallbacks`) does exactly this job — its only
users are `PhysicsStepDriveCoordinator.cpp:65,174,191`. Neither of these two callbacks takes a
lease, and `destroyPhysicsInteraction` never pauses the gate. Extending the existing gate is
probably cleaner than what we did.

**What we shipped** (the smaller version): an `std::atomic<int> s_inFlightCallbacks`,
increment/decrement in a thin wrapper *outside* the `__try` (an RAII guard inside a `__try`
whose `__except` can fire will leak the count on unwind), and a yield-spin in
`destroyPhysicsInteraction` before teardown. Two traps we hit ourselves:

- The increment must come **before** the `s_hooksEnabled` check. Check-then-increment
  reintroduces the same race.
- Keep the counting wrapper and the impl as separate functions, and make sure the **wrapper**
  is what's installed as the trampoline target. An early version of ours collapsed the two and
  silently turned the drain loop into a permanent no-op, with no symptom to notice.

*Honest caveat:* our release-store + acquire-load isn't a full StoreLoad fence, so a
nanosecond-wide Dekker-style residual survives even in ours. `seq_cst` on both sides would
close it properly.

`OURS: PhysicsInteraction.h:99, PhysicsHooks.cpp:2944/2989 (install :3024), PhysicsInteractionContacts.inl:635/641, ROCKMain.cpp:300-313`

---

## 2. The player char-controller filter wipes every constraint row on a constraint-only manifold

**Tier A · correctness · standalone · high on code, medium on live frequency**

When `makeGeneratedContactBufferView` returns invalid with `missingManifoldEntries` and the
player-controller filter is active, `clearGeneratedConstraintOnlyContacts`
(`PhysicsHooks.cpp:1617-1630`) runs and you **still** forward to `g_originalProcessConstraints`
at `:1641-1644` — so the native listener and the downstream simplex solve both see the mutated
buffers.

The helper (`GrabHeldObject.h:963-986`) tests no body id, no layer, no ROCK ownership. Past its
shape guard it does exactly `*view.manifoldCountPtr = 0; *view.constraintCountPtr = 0;`. It
*structurally* cannot discriminate: body ids are only ever read off the manifold rows
(`:1009`), and the branch that reaches the clear is precisely the one where
`manifoldEntries == nullptr`.

The gate is also broader than a held-object situation — `playerControllerFilterActive`
(`:1595`) is just `playerController && playerHknpWorld`, config flag defaulting true
(`RockConfig.h:199`). No hand needs to be holding anything.

**Player-facing:** on any frame that hits this path, every constraint the player's char-proxy
was about to solve is discarded — ground and wall support included. Reads as sinking through
the floor, sliding, or walking through geometry for a frame or a run of frames.

**We fail open instead:** return without touching the buffers and let native solve everything.
We removed the helper entirely.

**This one may well be deliberate on your side** — `DynamicMovableStaticGrabPolicyTests.cpp:348-365`
explicitly asserts *"constraint-only player controller contacts can be cleared fail-closed"* and
*"remove every unidentified row"*, so you're presumably defending against a hazard we haven't
seen. If so, the thing worth knowing is just that the blast radius includes vanilla player
support. And if you want fail-closed behaviour for a specific hazard, it needs a discriminator
that survives into the constraint-only layout — there isn't one today.

*Two caveats:* we have no measurement of how often the degenerate state (null manifold pointer
with `constraintCount > 0`) actually occurs on 1.2.72 — if it's unreachable in practice this is
harmless dead code. And the layout constants it writes through
(`kGeneratedConstraintRowsOffset 0x48`, `kGeneratedConstraintCountOffset 0x50`,
`kGeneratedContactStride 0x40`, `GrabHeldObject.h:817-820`) look like inferred RE from here; if
any are off, the blind `*ptr = 0` writes are worse than described, not better.

`OURS: PhysicsHooks.cpp:2798-2816 (fail-open); helper absent from our tree`

---

## 3. `installNativeGrabHook` blind-writes three bytes and reports success unconditionally

**Tier A · robustness · standalone · high confidence**

`PhysicsHooks.cpp:1430-1450` is the only patcher in that file that validates nothing. It
`VirtualProtect`s 3 bytes at `module+0xF19250`, writes `31 C0 C3`, restores protection, and logs
*"native grab DISABLED"* regardless of what was actually there. No prologue `memcmp`, no check
for an existing foreign detour, no `FlushInstructionCache`.

The contrast inside the same file is sharp — `installEntryTrampolineHook` and siblings `memcmp`
at `:187` and `:1162`, and `FlushInstructionCache` at `:959, :997, :1197, :1241`.

This is live, not dead: `HavokOffsets.h:282` has `kFunc_VRGrabInitiate = 0xF19250` (non-zero)
and `PhysicsInteraction.cpp:1345` calls it unconditionally from the constructor.

**Player-facing:** if the constant ever drifts (different exe build, address-library mismatch)
or another mod already detoured that entry, ROCK corrupts three bytes of whatever is actually
there and *still reports success* — so the user gets a hard, undiagnosable crash and the log
actively points away from the cause.

**What we shipped:** readability probe → exact 16-byte prologue compare → early-out if the
suppressed state is already present → revalidate after `VirtualProtect` → single
`InterlockedCompareExchange` over a 4-byte aligned word (the fourth byte is identical in both
words, with a `static_assert` on alignment) → `FlushInstructionCache` → final-state readback →
explicit `bool` return so the caller knows whether suppression actually happened.

For the record, we confirmed the expected prologue against the loaded `Fallout4VR.exe` in
Ghidra — `0x140F19250` is `44 89 4C 24 20 89 54 24 10 55 53 56 57 41 54 41 …`, matching our
16-byte constant exactly, so the validating path proceeds rather than failing closed.

*One sub-claim we walked back:* you **do** have re-entry protection (`static bool installed` at
`:1432-1434`), so ROCK won't re-patch itself. What's missing is validation of what's there,
foreign-detour detection, and the cache flush.

`OURS: PhysicsHooks.cpp:2165-2362, signature at PhysicsHooks.h:21`

---

## 4. `transform_math::invertTransform` transposes without an orthonormality precondition

**Tier A · latent correctness · standalone · code certain, consequence unmeasured upstream**

This is the one we most wanted to check, and it's a negative result: no, the public tree doesn't
normalize. `invertTransform` (`TransformMath.h:198-207`) unconditionally does
`result.rotate = transposeRotation(transform.rotate)` and inverts only the uniform `scale`
float. `transposeRotation` (`:100-109`) is a bare index swap with no guard and no comment
recording the precondition.

Roughly twenty object/grab consumers inherit it — `GrabNodeInfoMath.h:32,34,45`;
`GrabCore.h:968,1066,1323,1379`; `GrabContact.h:1859`; `GrabThreePhase.h:577`; `CustomOGA.h:157`;
`HandVisual.h:360`; `GrabConstraintMath.h:57,102,108`; `HandGrab.cpp:2971,2977,3160`;
`PhysicsInteraction.cpp:7191,8203`; `WeaponAuthority.h:152`. `WeaponGeometry.h:72-83` is a second
independent copy of the same bare transpose on the weapon-collision hull path.

On a drifted basis the forward/inverse pair stops cancelling and the residual is
`MᵀM = diag(|r₀|², |r₁|², |r₂|²)` — a **per-axis** stretch that `NiTransform::scale`, being a
single uniform float, can never represent or reveal.

**You already know this failure mode.** `TwoHandedGrip.cpp:57-62` has
`orthonormalizeStoredRotation`, with your comment measuring ~0.05%/frame row-norm decay. That's
the only normalization primitive in the tree, and it's used at exactly one site (`:5022`), on
the weapon path. Nothing on the object path.

**Player-facing:** a held object that visibly deforms — flattens or grows — while every
diagnostic insists scale is fine, because scale genuinely still reads `1.0000`. We hit this
with a Nuka-Cola bottle: flattened on grab, ~3× mid-hold, snapped back on release once Havok
re-drove the node from a clean quaternion. Compose-invert-compose chains inside a single call
(e.g. `GrabNodeInfoMath.h:32-34`) bite hardest.

**Highest-leverage fix** is a Gram-Schmidt normalize inside `invertTransform` itself — that
covers all ~20 consumers at once — plus the separate copy at `WeaponGeometry.h:72-83`. Ours is
small enough to lift verbatim: Gram-Schmidt on rows, with a `1e-6` degenerate-row bail that
returns **without writing** (so a degenerate basis is left alone rather than replaced by an
invented one), and a no-op to float precision on a clean matrix.

One detail that mattered: normalize a **copy** of the parent basis and use that *same* clean
basis for both the rotation line and the translation line, or the pair goes inconsistent and you
trade a stretch for a drift.

**The diagnostic is the transferable part:** log rotation **row norms**, not scale. We burned a
day on scale before realising it can't represent the defect. A row norm of `r` stretches that
axis by `r²` — 1.73 gives exactly 3×.

*Honest scope:* `TransformMath.h` is byte-identical across our tree, yours, and our 2026-07-06
pin, so this isn't something you fixed differently. But our runtime evidence is host-side, and
we have no measurement showing the object path drifts far enough to deform a mesh in a
standalone build. Raising it because you already measured the decay rate on the weapon side, so
you're better placed than we are to judge whether the object side reaches it.

`OURS (host-side, not vendored ROCK): src/Utils.h:27-59, applied at src/Utils.cpp:355-366 and src/Grab.cpp:1922, 10660, 10670, 12169`

---

## 5. `publishLocalTransformPose` discards a `[[nodiscard]]` result and reports success

**Tier B — touches the hand-pose publish path, so pFRIK may already differ · minor**

The finger local-transform publish is `[[nodiscard]]`, and its result is dropped; the function
reports success either way. Small and bounded: the visible case is the window where the
preceding scalar publish was cache-skipped (same pose bits, same priority, tag still active)
while the smoothed locals had changed — joints hold a stale pose for a frame or two.

Low severity, and genuinely may not apply to your FRIK. Mentioned only because the compiler is
already telling you about it.

---

## 6. Native melee: generated colliders arm the global cooldown

**Tier A · gameplay · standalone · Ghidra-verified against 1.2.72**

Ghidra-verified: `VRMeleeImpact` (`module+0xEFF000`) arms a **global** cooldown at
`PlayerCharacter+0x908` on any contact it processes, and early-returns at its first check while
that value is > 0 — before any target inspection. Generated keyframed colliders (hand 43,
weapon 44, body 47, proxy 48) are keyframed onto the same first-person weapon nodes the native
melee body is registered on, are legal partners (flags `0x2|0x4`, static bit clear), and burn
the cooldown at swing onset. The real NPC contact then arrives with the cooldown already armed
and dispatches nothing. Vanilla has no such extra bodies.

Why they're not skipped as "player self": `BethesdaPhysicsBody::createNiNode` builds owner nodes
with **no scene-graph parent**, so `FindReferenceFor3D` finds no ref and the `a_actor == player`
self-hit filter never triggers. Worth knowing on its own — if generated owner nodes ever get
parented under the player skeleton, this interference mode silently changes character.

**Player-facing:** vanilla lands the first bat swing on an NPC; with the mod it deals no damage.

**Where your tree stands:** you *do* hook this (`PhysicsHooks.cpp:858`), with a prologue check —
better validated than the grab hook in item 3 — and your own comment notes the native path
"owns … melee cooldown writes". But the hook is all-or-nothing: full suppression so SCISSORS can
own replacement damage, or forward everything. There's no per-contact filter (0 matches for a
partner-filter in your tree; 4 in ours). So with native melee *not* suppressed — the common
case, since SCISSORS is optional — the colliders still eat the first hit.

**What we shipped:** in the hook, for the player only, decode the other contact body's filter and
return without forwarding when its layer is one of ours. The weapon hull stays collidable, so a
bat still clanks off walls. Verified chain:

```
world   = *(uintptr*)contactEvent
slot    = *(uint*)(contactEvent + 0x20)              // expect 0/1
otherId = *(int*)(collisionEvent + 8 + (1-slot)*4)
body    = *(uintptr*)(world + 0x20) + otherId*0x90
filter  = *(uint*)(body + 0x44)                      // layer = filter & 0x7F
```

All reads SEH-guarded with plausibility checks; any fault or odd value falls through to native,
i.e. vanilla behaviour for that contact.

This one is additive to what you have rather than a correction — you clearly know the handler;
what's missing is the per-contact discriminator.

---

## Things we deliberately are **not** raising

- **The `dt` vs `1/dt` keyframe-drive inversion.** We had this recorded as a ROCK bug. It is
  **closed** — `ComputeHardKeyFrame` is called with `driveDeltaSeconds` in both trees. Flagging
  so you can ignore any older report of it from us.
- Weapon-hull push assist vs the native melee hit test — our change there is host-motivated, not
  a correction; the defect narrative didn't survive checking.
- Contact-subscription slot retention — the recycled-world-address concern is real in your tree,
  but our change doesn't actually fix it either.
- Char-proxy support body not cleared before a generated collider is freed — textual difference
  is real, the defect isn't present in your tree today.
- All host seams (`HostLoad`, `HostSetHandAuthority`, the host grab pump), the old-FRIK path, and
  the RE-API adaptations we made because the embed has no F4SEVR private SDK. Integration
  scaffolding, not improvements.

## Questions, if you're willing

1. Was `PhysicsCallbackQuiescenceGate` deliberately scoped to the step-drive path only, or is
   extending it to the two main callbacks just something that hasn't been done yet?
2. How often does `clearGeneratedConstraintOnlyContacts` actually fire on FO4VR? Your test
   asserts fail-closed deliberately, so there's presumably a hazard behind it we haven't seen.
3. Are the generated-contact buffer layout constants (`0x48`, `0x50`, `0x40`) RE-confirmed
   against 1.2.72, or inferred?
4. Is the `VRGrabInitiate` prologue at `module+0xF19250` stable across every build you target,
   and are you aware of other mods detouring it?
5. Would you take a normalize inside `invertTransform`, or do you consider orthonormality a
   documented precondition of the call sites?
6. Is the pre-0.6 history recoverable? The public repo now has two commits and our vendor pin
   (`edbaed3b`, 2026-07-06) isn't in it — we can still diff, it just makes tracking your changes
   harder for us.
7. Why were `held_player_space_math` (commit `6452acd`) and the soft-contact
   `NativeContactEvidence` producer (`9b7c7ee`) removed? We deliberately kept both — the first
   because smooth/snap-turn handling of held objects depends on it unless the host supplies its
   own, the second because `SoftContactRuntime` still consumes the cache. Happy to converge if
   there's a better replacement we've missed.

---

*Compiled from a file-level diff of both trees, 2026-08-02. Every item was read in both trees
and independently re-verified. If any of this is wrong I'd rather hear it than not — several of
our own claims died in verification, and the ones above are just the survivors.*
