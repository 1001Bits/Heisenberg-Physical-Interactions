#pragma once

#include <cstdint>

namespace F4SE
{
    class MessagingInterface;
}

namespace F4SE
{
    class LoadInterface;
}

namespace RE
{
    class Actor;
    class BGSObjectInstance;
    class NiAVObject;
    class NiPoint3;
    class NiTransform;
    class TESObjectREFR;
}

namespace rock
{
    const F4SE::MessagingInterface* getROCKMessaging();
    // Broadcast an optional ROCK event through the native F4SE messaging proxy.
    // Native F4SE returns false when nobody listens to a sender; unlike
    // CommonLibF4's convenience wrapper, this seam deliberately does not turn
    // that expected result into a warning.
    bool dispatchOptionalROCKMessage(
        std::uint32_t a_messageType,
        void* a_data,
        std::uint32_t a_dataLen,
        const char* a_receiver = nullptr);

    // Embedded-host load entry — call from Heisenberg's F4SEPlugin_Load after its own init,
    // gated on bUseRockEngineArchitecture. Installs ROCK's frame hook + engine lifecycle.
    bool HostLoad(const F4SE::LoadInterface* a_f4se);

    // Forward an F4SE message from the host (Heisenberg's OnF4SEMessage) into ROCK's lifecycle
    // handler. REQUIRED in the embed: ROCK can't register its own F4SE-core listener (this DLL
    // already has Heisenberg's), so Heisenberg must call this for EVERY F4SE message once the
    // engine is hosted. Param is void* to keep this header dependency-free; impl casts to
    // F4SE::MessagingInterface::Message*.
    void HostOnF4SEMessage(void* a_msg);

    // Forward a FRIK ("F4VRBody") lifecycle message from the host into ROCK's onFRIKMessage. REQUIRED
    // in the embed for the same reason as HostOnF4SEMessage: ROCK's own {plugin,"F4VRBody"} listener is
    // dead-on-arrival in a shared DLL, so Heisenberg must call this for every "F4VRBody"-sender message
    // once the engine is hosted. This is how the engine receives kSkeletonReady and builds its
    // hand/weapon colliders. Param is void*; impl casts to F4SE::MessagingInterface::Message*.
    void HostOnFRIKMessage(void* a_msg);

    // True only while the embedded ROCK physics-interaction runtime exists and
    // completed initialization. Hosts should use this as the gate for disabling
    // their body-less fallback collision path; loading ROCK alone is insufficient
    // because the runtime can remain dormant or be torn down with the world.
    bool HostIsPhysicsInteractionReady();
    // Diagnostic A/B mode: the embedded DLL and Heisenberg remain loaded, but
    // ROCK's live PhysicsInteraction is intentionally absent so frame pacing
    // can be compared against the full engine without activating a replacement
    // Heisenberg collision workload.
    bool HostIsPerformanceBenchmarkBaseline();

    // EMBEDDED-HOST SEAM (audit rank 2): notify the engine of the HOST's grab lifecycle so the
    // grabbing hand's collider suite + forearm chain are collision-leased for the whole hold and
    // restored via ROCK's own config-delayed release path (rockGrabReleaseHandCollisionDelaySeconds)
    // — exactly like ROCK's native grabs. Call with active=true when the host commits a grab on
    // that hand, active=false on release. Safe no-op while the engine is absent/uninitialized.
    void HostNotifyExternalGrab(bool a_isLeft, bool a_active);
    // One-shot ownership handoff for a physical grip edge consumed by an
    // already-active host grab. ROCK drains its own copy of that edge without
    // acting on it, preventing same-frame release/re-grab under mode 9.
    void HostConsumeExternalGrabInputEdge(bool a_isLeft);
    // Publishes the exact Havok bodies owned by a host-side grab. Body IDs are
    // scoped to their hknpWorld; the physics callback matches both values and
    // removes only those pairs from the player's character-controller solve.
    // Unlike collision-group tagging, this cannot mistake a native object's
    // authored group for host ownership. Publication is allocation-free and
    // safe to read from the physics thread.
    void HostPublishExternalHeldBodies(
        bool a_isLeft,
        const void* a_hknpWorld,
        const std::uint32_t* a_bodyIds,
        std::uint32_t a_count);
    void HostClearExternalHeldBodies(bool a_isLeft);
    bool HostHasExternalHeldBodies();
    bool HostIsExternalHeldBody(
        const void* a_hknpWorld,
        std::uint32_t a_bodyId);
    // Bit 0 = host right hand, bit 1 = host left hand. This preserves exact
    // held-object semantics in ROCK's contact router after the temporary
    // BIPED_NO_CC layer would otherwise look like a generic actor layer.
    std::uint8_t HostExternalHeldBodyOwnerMask(
        const void* a_hknpWorld,
        std::uint32_t a_bodyId);
    // Marks the actual ref restored to dynamic motion on a host release. ROCK
    // briefly withholds only its additive scripted push impulse from that ref;
    // native Havok collision remains active and can still be pushed normally.
    void HostNotifyExternalRelease(bool a_isLeft, RE::TESObjectREFR* a_releasedRef);

    // ── PLUGIN-SIDE HAND AUTHORITY SEAM (Jul 6) ──────────────────────────────────────────────
    // Lets ROCK's hand-placement (two-handing off-hand, wall-stop) work on an OLDER FRIK that lacks
    // the v5 applyExternalHandWorldTransform API. When the loaded FRIK is < v5 the bridge routes hand
    // placement into the HOST's plugin-side implementation via this POD callback table (mirror of the
    // grab seam, reversed direction). hand: 0=Right, 1=Left; worldXform/outWorldXform = RE::NiTransform*.
    struct HostHandAuthority
    {
        bool (*apply)(const char* tag, int hand, const void* worldXform, int priority);
        bool (*clear)(const char* tag, int hand);
        bool (*get)(int hand, void* outWorldXform);
    };
    void HostSetHandAuthority(const HostHandAuthority* a_cbs);

    // ── PLUGIN-SIDE FULL FINGER-POSE SEAM (FRIK 0.77.12 / API v3) ────────────────
    // Stock FRIK can consume only five per-finger curl scalars. The embedded
    // ROCK solver produces a canonical 22-float pose and, when mesh contact is
    // available, fifteen exact finger-bone local transforms. On FRIK < v5 the
    // bridge routes that richer result into Heisenberg through this POD table.
    //
    // The pointed-to payloads are deliberately opaque here to keep the host
    // boundary dependency-free:
    //   pose             = frik::api::FRIKApi::HandPoseData
    //   localTransforms  = frik::api::FRIKApi::FingerLocalTransformOverride
    // hand uses the same physical callback ABI as HostHandAuthority:
    // 0=Right, 1=Left.
    struct HostFingerPoseAuthority
    {
        bool (*applyPose)(const char* tag, int hand, const void* pose, int priority);
        bool (*buildPoseLocalTransforms)(int hand, const void* pose, void* outLocalTransforms);
        bool (*applyLocalTransforms)(const char* tag, int hand, const void* localTransforms, int priority);
        bool (*clear)(const char* tag, int hand);
        bool (*isActive)(const char* tag, int hand);
    };
    void HostSetFingerPoseAuthority(const HostFingerPoseAuthority* a_cbs);

    // Heisenberg's iTwoHandedFingerPoseMode owns support-finger publication
    // policy in the embedded build: 0=off, 1=ROCK rich mesh pose,
    // 2=Heisenberg whole-weapon geometry pose. -1 leaves standalone ROCK
    // behavior unchanged.
    void HostSetTwoHandedFingerPoseMode(int a_mode);
    int HostGetTwoHandedFingerPoseMode();
    bool HostPublishUniformFingerPose(
        const char* a_tag,
        bool a_isLeft,
        float a_thumb,
        float a_index,
        float a_middle,
        float a_ring,
        float a_pinky,
        int a_priority);
    bool HostClearFingerPose(const char* a_tag, bool a_isLeft);

    // ── EQUIP-OBJECT DETOUR ARBITRATION (grenade fix, 2026-07-28) ───────────────────────────
    // ActorEquipManager::EquipObject (0x0E6FEA0) can hold exactly ONE raw entry detour, and
    // Heisenberg's own (src/Hooks.cpp HookEquipObject) is installed first. ROCK therefore
    // installs NOTHING there; the host's detour calls into the engine through this seam. Before
    // this existed, ROCK's second detour failed its prologue memcmp against the bytes the host
    // had already written, so loose-grenade equip interception had literally never executed —
    // the "grenades drop at my feet" report.
    enum class HostEquipInterception : int
    {
        // ROCK did not touch this call. Caller continues its normal path (including
        // calling the original EquipObject).
        NotIntercepted = 0,
        // ROCK consumed it and queued the grenade transaction. Caller must NOT call the
        // original and must return TRUE ("equip handled").
        ConsumedEquipped = 1,
        // ROCK consumed it as a grenade whose projectile/explosion/fuse data would not
        // resolve. Caller must NOT call the original and must return FALSE.
        ConsumedBlocked = 2,
    };

    // Invoke ROCK's loose-grenade equip interception. `object` may be null. Safe to call before
    // engine init and when the engine is dormant: it returns NotIntercepted.
    HostEquipInterception HostTryInterceptEquipObject(
        RE::Actor* a_actor,
        const RE::BGSObjectInstance* a_object,
        std::uint32_t a_stackID,
        std::uint32_t a_number);

    // The host reports whether ITS EquipObject detour is live. MUST be called on every path out
    // of the host's install attempt (installed, skipped by config, failed) — it is the gate on
    // ROCK's whole pending-grenade-equip queue, and a queue nobody can feed must be latched off
    // rather than left waiting. `a_reasonWhenAbsent` is logged verbatim when a_installed is
    // false; give the concrete reason (e.g. which config keys left the hook uninstalled).
    // Call order note: Heisenberg's Hooks::Install() runs BEFORE rock::HostLoad(), so this
    // normally lands before the engine exists. That is fine and intended — the flag is plain
    // constant-initialised static state, and HostLoad reads it back.
    void HostSetEquipObjectDetourInstalled(bool a_installed, const char* a_reasonWhenAbsent);

    // Re-entry bracket. The host detour must wrap its call to the ORIGINAL EquipObject in these
    // so an equip the game issues from inside that call is not intercepted a second time. This
    // reproduces the guard ROCK's own detour used to hold across its pass-through. Thread-local
    // and counted; every Begin needs its End (use an RAII wrapper).
    void HostEquipObjectPassThroughBegin();
    void HostEquipObjectPassThroughEnd();

    // EMBED: host selects grab+selection ownership from its resolved policy. Passing true
    // (mode 9) cedes both to ROCK; false keeps both in Heisenberg. The decision
    // overrides standalone bGrabEnabled/bSelectionEnabled values now and after
    // every shared Heisenberg_F4VR.ini reload.
    void HostSetGrabOwnership(bool a_rockOwnsGrab);

    // Current-frame, data-only hint from Fallout 4 VR's native ViewCaster.
    // Publishing never starts a grab and never bypasses ROCK selection policy;
    // ROCK must collision-query and validate the exact reference before use.
    void HostPublishViewCasterGrabCandidate(
        bool a_isLeft,
        RE::TESObjectREFR* a_target);
    RE::TESObjectREFR* HostGetViewCasterGrabCandidate(bool a_isLeft);

    // EMBED (Jul 18): true while the given hand is engaging the equipped weapon (two-handed
    // support touch/grip or part-grip). Host uses it to suppress its own grab on that hand.
    bool HostIsWeaponSupportEngaged(bool a_isLeft);

    // GRIPPED-only twin of HostIsWeaponSupportEngaged (Jul 20 audit fix): excludes the
    // Touching state, for callers whose contract documents "gripping" specifically (the
    // public plugin API's IsOffHandGrippingWeapon).
    bool HostIsWeaponSupportGripped(bool a_isLeft);

    // Scope re-entry gate used by the embedded host's support-grip policy.
    // Arms only when either the host observed a scope presentation or FO4VR's
    // native request state is active. Once armed, the native geometry hook
    // keeps scope activation false until Bethesda reports the scope outside
    // its activation cone; a later re-entry can then activate normally.
    bool HostArmNativeScopeReentryBlockIfActive(bool a_hostObservedScopeActive);
    bool HostIsNativeScopeReentryBlocked();

    // HOST API (Jul 19, Virtual Reloads): external control seams. The host pushes the
    // effective state every frame. Host-side these are plain LATCHES (set until the
    // caller explicitly clears them) — the old 5s lease TTL is gone for good (Jul 24
    // user directive: never again).
    void HostSetWeaponCollisionSuppressed(bool a_suppressed);
    bool HostIsWeaponCollisionSuppressed();
    void HostSetTwoHandedGripBlocked(bool a_blocked);
    bool HostIsTwoHandedGripBlocked();
    void HostSetHandCollisionSuppressed(bool a_isLeft, bool a_suppressed);
    bool HostIsHandCollisionSuppressed(bool a_isLeft);
    void HostSetHandHoldingObject(bool a_isLeft, bool a_holding);
    bool HostIsHandHoldingObject(bool a_isLeft);

    // EMBED HOST COEXISTENCE: suppress only ROCK's scripted hand DynamicPushAssist while
    // the host supplies its body-less proximity/depenetration push. This does NOT suppress
    // ROCK hand/arm collision bodies, contact evidence, haptics, or weapon DynamicPushAssist.
    // Defaults false so standalone ROCK and hosts that do not opt in retain normal behavior.
    void HostSetHandDynamicPushAssistSuppressed(bool a_suppressed);
    bool HostIsHandDynamicPushAssistSuppressed();

    // EMBED (Jul 24): host-driven additive hand/arm collider radius padding (game units,
    // clamped 0..1). Bridges Heisenberg's MCM-tunable fHandColliderRadiusPadding into the
    // engine's HandBoneColliderSet — a change flips the collider tuning signature and
    // rebuilds the capsules mid-session. Push every frame like the other HostSet seams.
    void HostSetHandColliderRadiusPadding(float a_padding);

    // EMBED (Jul 29): host-driven lower bound for support-hand steering authority.
    // Heisenberg exposes 0.35 (the existing balanced default) through 1.0 (full
    // authority) in MCM and pushes it every frame so hot reload reaches the engine.
    void HostSetTwoHandedMinSteeringAuthority(float a_authority);

    // ── LARGE-OBJECT POLICY (car fix, #219/#220) ────────────────────────────────────────
    // The size ceiling that stops the player grabbing a car has to agree with the
    // character-controller half of the same fix, so BOTH halves read the SAME shared-INI
    // keys ([PhysicsInteraction] in Heisenberg_F4VR.ini). Half B lives inside the engine;
    // half A lives in Heisenberg's own grab selection, so it reads the values back out
    // through these two seams instead of duplicating the keys in a second config object.
    // Safe before/without engine load: they return the compiled defaults (true / 150.0).
    bool HostIsLargeObjectGrabBlockEnabled();
    float HostGetLargeObjectBoundThresholdGameUnits();

    // EMBED (Jul 24): the object ref this hand's colliders are ACTIVELY touching (contact
    // evidence, few-frame staleness window), or null. For the host's touch-priority grab
    // selection — a raycast pick must not beat the object physically under the hand.
    // Host must re-validate the ref (IsDeleted/Get3D) before acting on it.
    RE::TESObjectREFR* HostGetHandTouchedRef(bool a_isLeft);

    // Extended touch evidence for host grab acquisition. Unlike
    // HostGetHandTouchedRef, the caller chooses the admissible age. This is
    // intended for a guarded "recent touch of the SAME currently-selected
    // reference" handoff, not for replacing an unrelated current selection.
    // The body/contact point identify the physics surface that was actually
    // under the generated hand collider.
    bool HostGetHandTouchEvidence(
        bool a_isLeft,
        std::uint32_t a_maxAgeFrames,
        RE::TESObjectREFR** a_outRef,
        std::uint32_t* a_outBodyId,
        std::uint32_t* a_outAgeFrames,
        RE::NiPoint3* a_outContactPointWorld,
        bool* a_outHasContactPoint);

    // Exact, same-main-thread view of a loose object currently owned by a ROCK
    // grab. The node/ref are borrowed only for the duration of the host's
    // post-update callback; grabTraceId must be supplied to the release call so
    // a stale snapshot cannot detach a later grab from the same hand.
    struct HostHeldObjectSnapshot
    {
        RE::TESObjectREFR* ref = nullptr;
        RE::NiAVObject* heldNode = nullptr;
        float heldSeconds = 0.0f;
        float handSpeedMetersPerSecond = 0.0f;
        std::uint64_t grabTraceId = 0;
    };
    bool HostGetHeldObjectSnapshot(
        bool a_isLeft,
        HostHeldObjectSnapshot& a_out);

    // Exact Heisenberg item-profile bridge. This intentionally exposes only
    // Priority 1/2 authored matches; ROCK must never turn a fuzzy or
    // dimension-donor pose into rigid grab authority.
    struct HostExactItemGrabProfile
    {
        RE::NiPoint3 localPosition{};
        RE::NiMatrix3 localRotation{};
        RE::NiTransform parentWorld{};
        std::array<float, 5> fingerCurls{};
        std::array<float, 15> jointCurls{};
        bool hasFingerCurls{ false };
        bool hasJointCurls{ false };
        bool parentWorldValid{ false };
    };
    bool HostGetExactItemGrabProfile(
        RE::TESObjectREFR* a_ref,
        bool a_isLeft,
        HostExactItemGrabProfile& a_out);
    bool HostReleaseHeldObjectForInventory(
        bool a_isLeft,
        RE::TESObjectREFR* a_expectedRef,
        std::uint64_t a_expectedGrabTraceId);

    // Optional host reservation for quest items that may be physically held
    // but must never enter ROCK's native player-mouth consume transfer. A true
    // result blocks the candidate before ROCK detaches or activates the ref.
    using HostPlayerConsumeBlockFn = bool (*)(RE::TESObjectREFR*);
    void HostSetPlayerConsumeBlockCallback(
        HostPlayerConsumeBlockFn a_fn);
    // Engine-side query; default false in standalone ROCK. Callback faults are
    // contained and fail closed for the current item.
    bool HostShouldBlockPlayerConsume(RE::TESObjectREFR* a_ref);

    // The embedded host owns game-specific delivery classification (including
    // mod-added disease cures), Power Armor policy, and the user-facing
    // Consumables settings.  ROCK owns only spatial detection and transfer.
    // Standalone ROCK keeps its legacy mouth fallback when no callback is
    // registered; a registered callback may fail closed by returning false.
    enum class HostPlayerConsumeRoute : std::uint8_t
    {
        Blocked = 0,
        Mouth,
        OppositeWrist,
    };

    struct HostPlayerConsumeProfile
    {
        HostPlayerConsumeRoute route{ HostPlayerConsumeRoute::Blocked };
        bool autoConsumeWhileHeld{ false };
        float zoneOffsetXGameUnits{ 0.0f };
        float zoneOffsetYGameUnits{ 0.0f };
        float zoneOffsetZGameUnits{ 0.0f };
        float zoneRadiusGameUnits{ 0.0f };
    };

    using HostPlayerConsumeProfileFn = bool (*)(
        RE::TESObjectREFR*,
        bool,
        HostPlayerConsumeProfile&);
    void HostSetPlayerConsumeProfileCallback(
        HostPlayerConsumeProfileFn a_fn);
    // a_inOut contains ROCK's standalone mouth fallback on entry.  Returns
    // false only when a registered host callback rejects/faults the item.
    bool HostResolvePlayerConsumeProfile(
        RE::TESObjectREFR* a_ref,
        bool a_holdingHandIsLeft,
        HostPlayerConsumeProfile& a_inOut);

    // Live boundary samples from the exact generated hand/weapon collision
    // hulls. Used by host-owned animated meshes (Pip-Boy deck/button) that do
    // not expose a standalone Havok rigid body but still need to react to the
    // same physical hand and gun footprint.
    std::uint32_t HostCopyHandCollisionSamples(
        bool a_isLeft,
        RE::NiPoint3* a_outWorldPoints,
        float* a_outRadiiGame,
        std::uint32_t a_maxSamples);
    std::uint32_t HostCopyWeaponCollisionSamples(
        RE::NiPoint3* a_outWorldPoints,
        float* a_outRadiiGame,
        std::uint32_t a_maxSamples);

    // EMBED (Jul 19): live weapon-grip hand world (post-FRIK current weapon node), for the
    // host's hand-authority apply. False when the hand is not gripping the weapon.
    bool HostGetLiveGripHandWorld(bool a_isLeft, RE::NiTransform& a_out);

    // HOST SEAM (Jul 19): support-grip finger curls (5 values, 1=open/0=closed) for the
    // host's two-handed finger-pose driver. False when the hand is not support-gripping.
    bool HostGetWeaponSupportFingerCurls(bool a_isLeft, float a_outCurls[5]);

    // EMBED (Jul 19, frame-order audit): host publishes FRIK's clean post-solve skinned hand
    // worlds each frame (captured post-FRIK, BEFORE any authority write). TwoHandedGrip
    // consumes these as controller-truth inputs so the weapon solve never reads back
    // authority-written hands (loop-proof). Freshness is per-frame: ROCK clears the flag at
    // the end of its onFrameUpdate.
    void HostSetPreAuthorityHandWorlds(const RE::NiTransform& a_left, const RE::NiTransform& a_right);
    // Compatibility fallback captured from the final skinned graph. Existing
    // weapon consumers may read it through HostGetPreAuthorityHandWorld, but
    // hand-collision feedback must use HostGetCleanPreAuthorityHandWorld and
    // therefore rejects this potentially authority-written source.
    void HostSetFallbackPreAuthorityHandWorlds(
        const RE::NiTransform& a_left,
        const RE::NiTransform& a_right);
    // True only for the embedded HostLoad path, where flattened hand/arm
    // frames can contain the preceding external-authority result and therefore
    // must never be used as dynamic-collision intent without fresh clean truth.
    bool HostRequiresPreAuthorityHandWorld();
    bool HostGetPreAuthorityHandWorld(bool a_isLeft, RE::NiTransform& a_out);
    bool HostGetCleanPreAuthorityHandWorld(
        bool a_isLeft,
        RE::NiTransform& a_out);

    // Same-frame anatomical data captured beside the clean hand truth. The host clears
    // validity whenever it publishes a new hand pair, then fills each successfully resolved
    // arm. ROCK copies the values only; no skeleton node pointers cross this seam.
    void HostSetPreAuthorityArmReach(bool a_isLeft, const RE::NiPoint3& a_shoulderWorld,
        float a_maxReach);
    bool HostGetPreAuthorityArmReach(bool a_isLeft, RE::NiPoint3& a_shoulderWorld,
        float& a_maxReach);

    // EMBED (Jul 19, frame-order audit): host callback invoked at the TAIL of ROCK's
    // onFrameUpdate — after TwoHandedGrip has written the weapon node (the transform the
    // renderer consumes) and before the engine's bone-tree flatten. The host applies its
    // hand authority here so the rendered hand tracks the rendered weapon with zero lag.
    using HostPostUpdateFn = void (*)();
    void HostSetPostUpdateCallback(HostPostUpdateFn a_fn);
    const HostHandAuthority* getHostHandAuthority();
    const HostFingerPoseAuthority* getHostFingerPoseAuthority();

    // True when the loaded FRIK exposes the native v5 visual-authority API (getVersion >= 5). When
    // false, the bridge uses the host hand-authority table above. Latched once at init.
    bool frikHasVisualAuthority();
}
