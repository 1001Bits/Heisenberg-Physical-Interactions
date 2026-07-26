#pragma once

namespace F4SE
{
    class MessagingInterface;
}

namespace F4SE
{
    class LoadInterface;
}

namespace rock
{
    const F4SE::MessagingInterface* getROCKMessaging();

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

    // EMBEDDED-HOST SEAM (audit rank 2): notify the engine of the HOST's grab lifecycle so the
    // grabbing hand's collider suite + forearm chain are collision-leased for the whole hold and
    // restored via ROCK's own config-delayed release path (rockGrabReleaseHandCollisionDelaySeconds)
    // — exactly like ROCK's native grabs. Call with active=true when the host commits a grab on
    // that hand, active=false on release. Safe no-op while the engine is absent/uninitialized.
    void HostNotifyExternalGrab(bool a_isLeft, bool a_active);
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

    // EMBED: host selects grab+selection ownership from iGrabMode. Passing true
    // (mode 9) cedes both to ROCK; false keeps both in Heisenberg. The decision
    // overrides standalone bGrabEnabled/bSelectionEnabled values now and after
    // every shared Heisenberg_F4VR.ini reload.
    void HostSetGrabOwnership(bool a_rockOwnsGrab);

    // EMBED (Jul 18): true while the given hand is engaging the equipped weapon (two-handed
    // support touch/grip or part-grip). Host uses it to suppress its own grab on that hand.
    bool HostIsWeaponSupportEngaged(bool a_isLeft);

    // GRIPPED-only twin of HostIsWeaponSupportEngaged (Jul 20 audit fix): excludes the
    // Touching state, for callers whose contract documents "gripping" specifically (the
    // public plugin API's IsOffHandGrippingWeapon).
    bool HostIsWeaponSupportGripped(bool a_isLeft);

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
    bool HostGetPreAuthorityHandWorld(bool a_isLeft, RE::NiTransform& a_out);

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

    // True when the loaded FRIK exposes the native v5 visual-authority API (getVersion >= 5). When
    // false, the bridge uses the host hand-authority table above. Latched once at init.
    bool frikHasVisualAuthority();
}
