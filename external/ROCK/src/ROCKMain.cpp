
#include "ROCKMain.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>

#include "api/FRIKApi.h"
#define ROCK_API_EXPORTS
#include "RockConfig.h"
#include "api/ROCKProviderApi.h"
#include "physics-interaction/debug/DebugBodyOverlay.h"
#include "physics-interaction/core/PhysicsCreationGatePolicy.h"
#include "physics-interaction/core/PhysicsHooks.h"
#include "physics-interaction/core/RockRuntimeState.h"
#include "physics-interaction/grenade/LooseGrenadeRuntime.h"
#include "physics-interaction/native/HavokOffsets.h"
#include "physics-interaction/native/HavokRuntime.h"
#include "physics-interaction/native/NativeMemory.h"
#include "physics-interaction/input/DebugControllerRuntime.h"
#include "physics-interaction/input/InputRemapRuntime.h"
#include "physics-interaction/core/PhysicsInteraction.h"
#include "physics-interaction/grab/ExternalHeldBodyRegistry.h"
#include "physics-interaction/grab/FrikWeaponOffsetCache.h"
#include "physics-interaction/grab/SavedGrabOffsetStore.h"
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "physics-interaction/visual/FrikCompatibilityPolicy.h"
#include "physics-interaction/visual/FrikVisualAuthorityBridge.h"
#include "physics-interaction/visual/PreAuthorityHandFramePolicy.h"
#include "physics-interaction/weapon/SeeThroughScopesCompatibility.h"
#include "physics-interaction/weapon/NativeScopeReentryPolicy.h"
#include "physics-interaction/RockLoggingPolicy.h"
#include "../../../src/ItemOffsets.h"
#include "../../../src/WandNodeHelper.h"

#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"

// AFTER every RE/CommonLibF4 header on purpose: this header pulls in <Windows.h>, and from
// higher up in the block the Win32 macros (GetObject, near/far, min/max) would reach them.
#include "physics-interaction/native/HookAddressDiagnostics.h"

namespace
{
    using namespace rock;

    const F4SE::MessagingInterface* s_messaging = nullptr;

    PhysicsInteraction* s_physicsInteraction = nullptr;
    bool s_physicsPublished = false;

    bool s_frikAvailable = false;
    std::uint32_t s_frikApiVersion = 0;                        // latched loaded FRIK API version
    const rock::HostHandAuthority* s_hostHandAuthority = nullptr;  // plugin-side hand placement (old FRIK)
    const rock::HostFingerPoseAuthority* s_hostFingerPoseAuthority = nullptr;  // full finger posing (FRIK v3)
    std::atomic<int> s_hostTwoHandedFingerPoseMode{ -1 };
    std::atomic<bool> s_nativeScopeReentryBlocked{ false };
    std::atomic<bool> s_nativeScopeGeometryHookInstalled{ false };
    rock::external_held_body_registry::Registry<64>
        s_externalHeldBodyRegistry;
    // Retained only across the chained host update immediately preceding
    // ROCK's update. Cleared at the start of every hook invocation so a host
    // early-return can never leave a stale ViewCaster target behind.
    RE::NiPointer<RE::TESObjectREFR> s_hostViewCasterGrabCandidates[2]{};

    bool s_pluginLoaded = false;
    // NOTE (Jul 20 audit): a per-frame SEH fault-containment flag (s_rockFrameFaulted) and
    // this comment describing it used to live here, but onGameFrameUpdateHook has never
    // actually contained a __try/__except - the flag was declared, documented, and never
    // read or written anywhere else. Removed rather than wired up: onFrameUpdate() has its
    // own RAII locals (e.g. performance_profiler::FrameScope) deep in its call graph, and
    // this project's own rule is that /EHsc __except skips C++ destructors on unwind, so a
    // catch-all SEH here would risk leaking whatever those hold on a fault - worse than the
    // current honest hard-CTD-on-fault behavior. A real fix needs a narrow guard around
    // specific raw-pointer leaf calls (the project's established pattern elsewhere), not a
    // blanket wrap of the whole per-frame pipeline.
    // EMBED (Jul 19, frame-order audit): host pre-authority hand snapshot + post-update seam.
    RE::NiTransform s_preAuthHandWorld[2]{};   // [0]=Right, [1]=Left
    rock::pre_authority_hand_frame_policy::Provenance
        s_preAuthHandProvenance =
            rock::pre_authority_hand_frame_policy::Provenance::Unavailable;
    RE::NiPoint3 s_preAuthShoulderWorld[2]{};
    float s_preAuthMaxReach[2] = { 0.0f, 0.0f };
    bool s_preAuthArmReachValid[2] = { false, false };
    rock::HostPostUpdateFn s_hostPostUpdateFn = nullptr;
    rock::HostPlayerConsumeBlockFn s_hostPlayerConsumeBlockFn = nullptr;
    rock::HostPlayerConsumeProfileFn s_hostPlayerConsumeProfileFn = nullptr;
    std::atomic<std::uint32_t> s_providerGeneration{ 1 };
    std::atomic<std::uint32_t> s_skeletonGeneration{ 1 };
    std::atomic<bool> s_physicsCreationRequested{ false };
    std::atomic<std::uint32_t> s_physicsCreationReadyDeferralFrames{ 0 };
    physics_creation_gate_policy::WorldStabilityState s_physicsCreationWorldStability{};
    std::uint32_t s_physicsCreationGateLogCounter = 0;

    struct PolledSkeletonObservation
    {
        bool ready = false;
        std::uintptr_t skeleton = 0;
        std::uintptr_t boneTree = 0;
        bool inPowerArmor = false;
    };
    PolledSkeletonObservation s_polledSkeleton{};

    struct PlayerPhysicsWorlds
    {
        RE::bhkWorld* bhk = nullptr;
        RE::hknpWorld* hknp = nullptr;
    };

    const char* physicsCreationBlockReasonName(physics_creation_gate_policy::CreationBlockReason reason)
    {
        using Reason = physics_creation_gate_policy::CreationBlockReason;
        switch (reason) {
        case Reason::None:
            return "none";
        case Reason::RockDisabled:
            return "rock-disabled";
        case Reason::ProviderUnavailable:
            return "provider-unavailable";
        case Reason::SkeletonNotReady:
            return "skeleton-not-ready";
        case Reason::ReadyEventDeferred:
            return "ready-event-deferred";
        case Reason::MenuBlocked:
            return "menu-blocked";
        case Reason::WorldUnavailable:
            return "world-unavailable";
        case Reason::WorldUnstable:
            return "world-unstable";
        default:
            return "unknown";
        }
    }

    void resetPhysicsCreationGate()
    {
        physics_creation_gate_policy::resetWorldStability(s_physicsCreationWorldStability);
        s_physicsCreationGateLogCounter = 0;
    }

    void resetPolledSkeletonObservation()
    {
        s_polledSkeleton = {};
    }

    void requestDeferredPhysicsCreation()
    {
        s_physicsCreationRequested.store(true, std::memory_order_release);
        s_physicsCreationReadyDeferralFrames.store(
            physics_creation_gate_policy::kSkeletonReadyCreateDeferralFrames,
            std::memory_order_release);
        resetPhysicsCreationGate();
    }

    PlayerPhysicsWorlds samplePlayerPhysicsWorlds()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return {};
        }

        auto* cell = player->GetParentCell();
        if (!cell) {
            return {};
        }

        auto* bhk = cell->GetbhkWorld();
        if (!bhk) {
            return {};
        }

        return {
            .bhk = bhk,
            .hknp = havok_runtime::getHknpWorldFromBhk(bhk),
        };
    }

    std::uint32_t bumpGeneration(std::atomic<std::uint32_t>& generation)
    {
        const auto next = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        return next == 0 ? generation.fetch_add(1, std::memory_order_acq_rel) + 1 : next;
    }

    void publishPhysicsInteractionIfReady()
    {
        if (!s_physicsInteraction || !s_physicsInteraction->isInitialized() || s_physicsPublished) {
            return;
        }

        PhysicsInteraction::s_hooksEnabled.store(true, std::memory_order_release);
        rock::provider::setPhysicsInteractionInstance(s_physicsInteraction);
        s_physicsPublished = true;
        logger::info("ROCK: PhysicsInteraction initialized and published.");
    }

    void createPhysicsInteraction()
    {
        logger::info("ROCK: Creating PhysicsInteraction (skeleton became ready)...");

        s_physicsInteraction = new PhysicsInteraction(
            s_skeletonGeneration.load(std::memory_order_acquire),
            s_providerGeneration.load(std::memory_order_acquire));
        s_physicsInteraction->init();

        publishPhysicsInteractionIfReady();
        if (!s_physicsPublished) {
            logger::warn("ROCK: PhysicsInteraction init deferred; hooks/API remain disabled until lazy init succeeds.");
        }
    }

    void destroyPhysicsInteraction(rock::provider::RockProviderLifecycleReason reason);

    void ensurePhysicsInteractionForReadySkeleton(const runtime_state::RuntimeFrameSnapshot& runtime)
    {
        /*
         * ROCK creation is event-driven when FRIK first announces skeleton
         * readiness, but config hot reload can disable the module during that
         * event and re-enable it later. The frame loop is the only place that
         * sees the current config and live FRIK readiness together, so it owns
         * this narrow recovery path instead of forcing users to reload a save
         * to receive a second skeleton-ready message.
         */
        const bool interactionExists = s_physicsInteraction != nullptr;
        const bool interactionInitialized =
            interactionExists && s_physicsInteraction->isInitialized();
        if (!s_physicsCreationRequested.load(std::memory_order_acquire) &&
            frik_compatibility_policy::shouldRequestCollisionCreation({
                .rockEnabled = g_rockConfig.rockEnabled,
                .frikProviderAvailable = s_frikAvailable,
                .localSkeletonReady =
                    runtime.localSkeletonReady &&
                    runtime.visualSkeletonReadyHint,
                .interactionExists = interactionExists,
                .interactionInitialized = interactionInitialized,
            })) {
            s_physicsCreationRequested.store(true, std::memory_order_release);
            s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
        }

        if (!s_physicsCreationRequested.load(std::memory_order_acquire)) {
            return;
        }

        const auto worlds = samplePlayerPhysicsWorlds();
        const auto readyDeferralFrames = s_physicsCreationReadyDeferralFrames.load(std::memory_order_acquire);
        const physics_creation_gate_policy::CreationGateInput gateInput{
            .rockEnabled = g_rockConfig.rockEnabled,
            .providerAvailable = s_frikAvailable,
            .skeletonReady =
                runtime.localSkeletonReady &&
                runtime.visualSkeletonReadyHint,
            .runtimeMenuBlocking = runtime.localMenuBlocking,
            .compatibilityConfigBlocking = runtime.compatibilityConfigBlocking,
            .bhkWorld = reinterpret_cast<std::uintptr_t>(worlds.bhk),
            .hknpWorld = reinterpret_cast<std::uintptr_t>(worlds.hknp),
            .readyDeferralFrames = readyDeferralFrames,
        };
        const auto decision = physics_creation_gate_policy::evaluateCreationGate(s_physicsCreationWorldStability, gateInput);
        if (readyDeferralFrames > 0) {
            s_physicsCreationReadyDeferralFrames.fetch_sub(1, std::memory_order_acq_rel);
        }

        if (!decision.keepRequestPending) {
            s_physicsCreationRequested.store(false, std::memory_order_release);
        }

        if (!decision.canCreate) {
            if ((s_physicsCreationGateLogCounter++ % 90u) == 0u) {
                logger::debug(
                    "ROCK: Physics creation deferred reason={} stableFrames={} bhk={} hknp={} localMenu={} compatibilityConfig={} readyDeferral={} skel[rootNode={} rootAttached={} flattenedValid={} handBones={} captured={} reqResolved={}]",
                    physicsCreationBlockReasonName(decision.blockReason),
                    decision.stableWorldFrames,
                    static_cast<const void*>(worlds.bhk),
                    static_cast<const void*>(worlds.hknp),
                    runtime.localMenuBlocking ? "yes" : "no",
                    runtime.compatibilityConfigBlocking ? "yes" : "no",
                    readyDeferralFrames,
                    runtime.localSkeletonRootNodeAvailable ? "yes" : "no",
                    runtime.localSkeletonRootAttached ? "yes" : "no",
                    runtime.localSkeletonFlattenedTreeValid ? "yes" : "no",
                    runtime.localSkeletonRequiredHandBonesReady ? "yes" : "no",
                    runtime.localSkeletonCapturedBoneCount,
                    runtime.localSkeletonRequiredResolvedCount);
            }
            return;
        }

        if (s_physicsInteraction) {
            logger::info("ROCK: Recreating PhysicsInteraction after deferred skeleton-ready gate.");
            destroyPhysicsInteraction(rock::provider::RockProviderLifecycleReason::SkeletonReady);
        }

        createPhysicsInteraction();
        s_physicsCreationRequested.store(false, std::memory_order_release);
        s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
        resetPhysicsCreationGate();
    }

    void destroyPhysicsInteraction(
        rock::provider::RockProviderLifecycleReason reason = rock::provider::RockProviderLifecycleReason::ProviderLost)
    {
        if (!s_physicsInteraction) {
            return;
        }

        logger::info("ROCK: Destroying PhysicsInteraction (skeleton released)...");

        PhysicsInteraction::s_hooksEnabled.store(false, std::memory_order_release);
        // Drain in-flight physics-thread callbacks (hookedProcessConstraintsCallback,
        // native contact events) before lifecycle teardown mutates the instance
        // they dereference. Callbacks that observe hooks disabled exit without
        // entering; callbacks that passed the gate first remain counted here.
        {
            int spinCount = 0;
            while (PhysicsInteraction::s_inFlightCallbacks.load(std::memory_order_acquire) > 0) {
                std::this_thread::yield();
                if (++spinCount % 100000 == 0) {
                    logger::warn("ROCK: destroyPhysicsInteraction still draining in-flight callbacks (spins={})", spinCount);
                }
            }
        }

        /*
         * Close the provider read gate before mutating PhysicsInteraction.
         * setPhysicsInteractionInstance(nullptr) also drains API readers that
         * already acquired a lease.
         */
        rock::provider::setPhysicsInteractionInstance(nullptr);
        s_physicsPublished = false;

        s_physicsInteraction->noteProviderLifecycle(
            s_providerGeneration.load(std::memory_order_acquire),
            reason);
        s_physicsInteraction->shutdown(reason);
        rock::provider::dispatchFrameCallbacks(*s_physicsInteraction);
        rock::provider::clearExternalBodiesForProviderLoss();

        delete s_physicsInteraction;
        s_physicsInteraction = nullptr;

        logger::info("ROCK: PhysicsInteraction destroyed.");
    }

    void observePolledSkeletonLifecycle(
        const runtime_state::RuntimeFrameSnapshot& runtime)
    {
        const bool ready =
            runtime.visualSkeletonReadyHint &&
            runtime.localSkeletonReady &&
            runtime.localSkeletonIdentity != 0 &&
            runtime.localSkeletonBoneTreeIdentity != 0;

        if (!ready) {
            if (s_polledSkeleton.ready) {
                const auto generation = bumpGeneration(s_skeletonGeneration);
                logger::info(
                    "ROCK: Polled FRIK skeleton became unavailable; invalidating generation {}.",
                    generation);
                s_physicsCreationRequested.store(false, std::memory_order_release);
                s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
                resetPhysicsCreationGate();
                if (s_physicsInteraction) {
                    s_physicsInteraction->noteSkeletonLifecycle(
                        generation,
                        rock::provider::RockProviderLifecycleReason::SkeletonDestroying);
                }
                destroyPhysicsInteraction(
                    rock::provider::RockProviderLifecycleReason::SkeletonDestroying);
            }
            resetPolledSkeletonObservation();
            return;
        }

        const PolledSkeletonObservation next{
            .ready = true,
            .skeleton = runtime.localSkeletonIdentity,
            .boneTree = runtime.localSkeletonBoneTreeIdentity,
            .inPowerArmor = runtime.localSkeletonInPowerArmor,
        };
        if (!s_polledSkeleton.ready) {
            s_polledSkeleton = next;
            logger::debug(
                "ROCK: Polled FRIK skeleton ready skeleton={} boneTree={} powerArmor={}.",
                reinterpret_cast<const void*>(next.skeleton),
                reinterpret_cast<const void*>(next.boneTree),
                next.inPowerArmor ? "yes" : "no");
            return;
        }

        const bool identityChanged =
            next.skeleton != s_polledSkeleton.skeleton ||
            next.boneTree != s_polledSkeleton.boneTree;
        const bool powerArmorChanged =
            next.inPowerArmor != s_polledSkeleton.inPowerArmor;
        if (!identityChanged && !powerArmorChanged) {
            return;
        }

        const auto generation = bumpGeneration(s_skeletonGeneration);
        logger::info(
            "ROCK: Polled FRIK skeleton replacement detected (identityChanged={} powerArmorChanged={}); rebuilding generation {}.",
            identityChanged ? "yes" : "no",
            powerArmorChanged ? "yes" : "no",
            generation);
        const auto reason =
            powerArmorChanged ?
            rock::provider::RockProviderLifecycleReason::PowerArmorChanged :
            rock::provider::RockProviderLifecycleReason::SkeletonDestroying;
        if (s_physicsInteraction) {
            s_physicsInteraction->noteSkeletonLifecycle(generation, reason);
        }
        destroyPhysicsInteraction(reason);
        s_polledSkeleton = next;
        requestDeferredPhysicsCreation();
    }

    void onFrameUpdate()
    {
        if (s_pluginLoaded && s_frikAvailable) {
            g_rockConfig.processPendingConfigReload();
        }
        performance_profiler::refreshSettings(
            g_rockConfig.rockPerformanceProfilerEnabled,
            g_rockConfig.rockPerformanceProfilerLogIntervalFrames,
            g_rockConfig.rockPerformanceProfilerWarmupFrames,
            g_rockConfig.rockPerformanceProfilerOverlayText);
        performance_profiler::refreshBenchmarkSettings(
            g_rockConfig.rockPerformanceBenchmarkMode,
            g_rockConfig.rockPerformanceProfilerLogIntervalFrames,
            g_rockConfig.rockPerformanceProfilerWarmupFrames);
        performance_profiler::FrameScope profilerFrame;

        if (!s_pluginLoaded || !s_frikAvailable) {
            input_remap_runtime::setGameplayInputAllowed(false);
            input_remap_runtime::setWeaponDrawn(false);
            input_remap_runtime::setHandHeldWeapon(false, false);
            input_remap_runtime::setEquippedWeaponFiringGripInputActive(false);
            input_remap_runtime::setEquippedWeaponPrimaryDetached(false);
            return;
        }

        input_remap_runtime::installInputRemapHooks();

        const bool performanceBenchmarkBaseline =
            g_rockConfig.rockPerformanceBenchmarkMode == 2;
        const bool menuInputActive = input_remap_runtime::isMenuInputActive();
        runtime_state::updateFrame(runtime_state::RuntimeFrameInput{
            .menuInputBlocking = menuInputActive,
            .visualAuthorityAvailable = frik_visual_authority::isAvailable(),
            .visualSkeletonReadyHint = frik_visual_authority::isSkeletonReadyHint(),
            .compatibilityConfigBlocking = frik_visual_authority::isCompatibilityConfigBlocking(),
        });
        const auto& runtime = runtime_state::currentFrame();
        observePolledSkeletonLifecycle(runtime);
        const bool gameplayInputAllowed =
            g_rockConfig.rockEnabled &&
            !performanceBenchmarkBaseline &&
            runtime.localSkeletonReady &&
            !runtime.localMenuBlocking &&
            !runtime.inputMenuBlocking &&
            !runtime.compatibilityConfigBlocking;
        input_remap_runtime::setWeaponDrawn(runtime.weaponDrawn);
        input_remap_runtime::setGameplayInputAllowed(gameplayInputAllowed);
        // DEVELOPER MODE ONLY. This polls XInput and maps BARE A/B/X/Y to debug-collider
        // draw and live grab-pivot authoring - and persists those flags into the user's INI
        // (persistPhysicsBool). Ungated, a player with an Xbox pad connected for desktop use
        // could put permanent debug wireframes and edited grab pivots into their config from
        // one stray press, with no in-game way to undo it. bDeveloperModeEnabled already
        // existed and defaulted false; it simply was not consulted here.
        if (g_rockConfig.rockDeveloperModeEnabled) {
            debug_controller_runtime::update(gameplayInputAllowed, runtime.deltaSeconds);
        }

        if (!g_rockConfig.rockEnabled ||
            performanceBenchmarkBaseline) {
            s_physicsCreationRequested.store(false, std::memory_order_release);
            s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
            resetPhysicsCreationGate();
            if (s_physicsInteraction) {
                destroyPhysicsInteraction();
            }
        } else {
            ensurePhysicsInteractionForReadySkeleton(runtime);

            if (s_physicsInteraction) {
                s_physicsInteraction->update();
                publishPhysicsInteractionIfReady();
            }
        }

        // EMBED (Jul 19, frame-order audit): the weapon node now holds ROCK's final solved
        // transform for this frame — the one the renderer consumes. Invoke the host's hand
        // authority HERE so the rendered hand is solved against the rendered weapon (zero
        // lag). Then retire the pre-authority hand snapshot (per-frame freshness).
        //
        // EMBED (Jul 20 audit fix): this used to sit behind an early `return` in the
        // `!rockEnabled` branch above. The host's OWN gate for calling ApplyWinners
        // (Hooks.cpp) is the session-static IsRockEngineHosted() flag, NOT a per-frame
        // "did ROCK run its tail callback" check - so a session with
        // bUseRockEngineArchitecture=1 that sets [PhysicsInteraction] bEnabled=false at runtime (hot
        // reload) silently killed EVERY hand-authority writer (wall pushback, etc.) with
        // no log hint, because NEITHER side ever called ApplyWinners once rockEnabled
        // went false. Now runs every frame this hook fires, regardless of rockEnabled.
        if (s_hostPostUpdateFn) {
            s_hostPostUpdateFn();
        }
        if (s_physicsInteraction) {
            s_physicsInteraction->finalizeWeaponAuthorityAfterHostHands();
        }
        // Scope/reticle alignment must sample the same post-host transform that the
        // renderer and muzzle use. This stays an immediate per-frame sample: no motion
        // smoothing is introduced into the already-stable walking/aim path.
        see_through_scopes::updateFrame();
        s_preAuthHandProvenance =
            rock::pre_authority_hand_frame_policy::Provenance::Unavailable;
        s_preAuthArmReachValid[0] = false;
        s_preAuthArmReachValid[1] = false;
    }

    using GameLoopFunc = void (*)(std::uint64_t rcx);
    GameLoopFunc s_originalGameLoopFunc = nullptr;
    using NativeScopeStateTransitionFunc =
        void (*)(RE::PlayerCharacter*, bool);
    NativeScopeStateTransitionFunc s_originalNativeScopeStateTransition =
        nullptr;

    bool tryReadNativeScopeRequestState(bool& outActive)
    {
        using GetScopeRequestState = bool (*)(const void*);
        static REL::Relocation<GetScopeRequestState> getScopeRequestState{
            REL::Offset(
                rock::offsets::kFunc_NativeScopeRequestStateGet) };
        static REL::Relocation<std::uintptr_t> rendererState{
            REL::Offset(
                rock::offsets::kData_NativeScopeRendererState) };
        if (!getScopeRequestState.address() ||
            !rendererState.address()) {
            return false;
        }

        outActive = getScopeRequestState(
            reinterpret_cast<const void*>(
                rendererState.address()));
        return true;
    }

    bool onNativeScopeGeometryDecision(
        RE::PlayerCharacter* player,
        const bool nativeGeometryDecision)
    {
        const auto decision =
            rock::native_scope_reentry_policy::filter(
                s_nativeScopeReentryBlocked.load(
                    std::memory_order_acquire),
                nativeGeometryDecision);
        if (decision.rearmedThisFrame) {
            s_nativeScopeReentryBlocked.store(
                false,
                std::memory_order_release);
            logger::info(
                "ROCK(host): native scope re-entry rearmed after "
                "the optic left its activation cone");
        }

        if (s_originalNativeScopeStateTransition) {
            s_originalNativeScopeStateTransition(
                player,
                decision.nativeScopeRequested);
        }
        /*
         * The caller's adjacent TEST is patched to consume AL. In Bethesda's
         * original routine that TEST does not change scope state a second
         * time: it selects the near-eye approach-blackout path. While an armed
         * latch consumes an inside-cone activation we deliberately return true
         * here, despite sending false to the state transition above. That
         * keeps the scope closed without leaving the normal world black until
         * the physical optic moves away. The first real outside-cone sample
         * still returns false and rearms exactly as before.
         */
        return decision.bypassNativeApproachFade;
    }

    bool hookNativeScopeGeometryDecision()
    {
        REL::Relocation<std::uintptr_t> callSite{
            REL::Offset(
                rock::offsets::
                    kHookSite_NativeScopeGeometryDecision) };
        const auto callSiteAddress = callSite.address();
        const auto* callBytes =
            reinterpret_cast<const std::uint8_t*>(
                callSiteAddress);
        if (!callBytes || callBytes[0] != 0xE8) {
            logger::critical(
                "ROCK(host): native scope geometry hook validation "
                "failed at 0x{:X}: expected CALL rel32, found "
                "0x{:02X}",
                callSiteAddress,
                callBytes ? callBytes[0] : 0u);
            return false;
        }

        const auto relativeTarget =
            *reinterpret_cast<const std::int32_t*>(
                callBytes + 1);
        const auto decodedTarget =
            callSiteAddress + 5u + relativeTarget;
        const auto expectedTarget =
            REL::Offset(
                rock::offsets::
                    kFunc_NativeScopeStateTransition)
                .address();
        if (decodedTarget != expectedTarget) {
            logger::critical(
                "ROCK(host): native scope geometry hook validation "
                "failed at 0x{:X}: target 0x{:X}, expected 0x{:X}",
                callSiteAddress,
                decodedTarget,
                expectedTarget);
            return false;
        }

        REL::Relocation<std::uintptr_t> postDecisionTest{
            REL::Offset(
                rock::offsets::
                    kPatchSite_NativeScopePostDecisionTest) };
        const auto postDecisionTestAddress =
            postDecisionTest.address();
        const auto* postDecisionTestBytes =
            reinterpret_cast<const std::uint8_t*>(
                postDecisionTestAddress);
        constexpr std::array<std::uint8_t, 2>
            kExpectedNativeDecisionTest{ 0x84, 0xDB };
        if (!postDecisionTestBytes ||
            postDecisionTestBytes[0] !=
                kExpectedNativeDecisionTest[0] ||
            postDecisionTestBytes[1] !=
                kExpectedNativeDecisionTest[1]) {
            logger::critical(
                "ROCK(host): native scope fade-decision "
                "validation failed at 0x{:X}: expected TEST BL,BL",
                postDecisionTestAddress);
            return false;
        }

        auto& trampoline = F4SE::GetTrampoline();
        const auto original = trampoline.write_call<5>(
            callSiteAddress,
            &onNativeScopeGeometryDecision);
        s_originalNativeScopeStateTransition =
            reinterpret_cast<
                NativeScopeStateTransitionFunc>(original);
        if (!s_originalNativeScopeStateTransition) {
            logger::critical(
                "ROCK(host): native scope geometry hook original "
                "target is null");
            return false;
        }

        // The wrapper returns an independently filtered approach-fade decision
        // in AL. It normally mirrors BL, but blocked-inside scope requests must
        // close scope state while bypassing the near-eye blackout calculation.
        constexpr std::array<std::uint8_t, 2>
            kRockDecisionTest{ 0x84, 0xC0 };
        REL::safe_write(
            postDecisionTestAddress,
            kRockDecisionTest.data(),
            kRockDecisionTest.size());

        logger::info(
            "ROCK(host): native scope geometry/re-entry hook "
            "installed at 0x{:X}, original 0x{:X}",
            callSiteAddress,
            original);
        s_nativeScopeGeometryHookInstalled.store(
            true,
            std::memory_order_release);
        return true;
    }

    // ROCK applies weapon visual/collision authority after the chained frame update
    // so FRIK finishes its skeleton and weapon pass before ROCK writes final state.
    void onGameFrameUpdateHook(const std::uint64_t rcx)
    {
        for (auto& candidate : s_hostViewCasterGrabCandidates) {
            candidate.reset();
        }

        if (s_originalGameLoopFunc) {
            s_originalGameLoopFunc(rcx);
        }

        onFrameUpdate();

        const int benchmarkMode =
            performance_profiler::benchmarkMode();
        const auto& runtime = runtime_state::currentFrame();
        const bool commonBenchmarkEligibility =
            s_pluginLoaded &&
            s_frikAvailable &&
            runtime.localSkeletonReady &&
            runtime.visualSkeletonReadyHint &&
            !runtime.localMenuBlocking &&
            !runtime.inputMenuBlocking &&
            !runtime.compatibilityConfigBlocking;
        const bool benchmarkEligible =
            commonBenchmarkEligibility &&
            ((benchmarkMode == 1 &&
                 g_rockConfig.rockEnabled &&
                 s_physicsInteraction &&
                 s_physicsInteraction->isInitialized()) ||
                (benchmarkMode == 2 &&
                    s_physicsInteraction == nullptr));
        performance_profiler::observeFrameBoundary(
            benchmarkEligible);
    }

    bool hookMainLoop()
    {
        REL::Relocation hookCallSite{ REL::Offset(rock::offsets::kHookSite_MainLoop) };

        logger::info("ROCK: Hooking main loop at (0x{:X})...", hookCallSite.address());

        // Validate the call-site prefix before patching, matching the project rule every
        // OTHER raw-offset hook in this bucket already follows (the grenade hook memcmps a
        // 17-byte prefix; the input-remap vtable hooks check the current target). This is a
        // SHARED FRIK-framework hook site - if another mod (or a framework version change)
        // patches it with anything other than a 5-byte rel32 call before HostLoad runs,
        // write_call<5> would overwrite 5 of ITS bytes and decode a garbage displacement
        // into s_originalGameLoopFunc (almost never null, so the !s_originalGameLoopFunc
        // check below would not catch it) - the first frame's call through that garbage
        // pointer jumps into unmapped/misaligned code, an immediate CTD at game start with
        // no diagnostic.
        const auto* siteBytes = reinterpret_cast<const std::uint8_t*>(hookCallSite.address());
        if (siteBytes[0] != 0xE8) {
            // Findings only: address + owning module + the bytes actually present.
            // The previous wording ("another mod or framework version likely changed
            // this shared hook site") asserted a cause nothing had checked.
            logger::critical("ROCK: main loop hook site 0x{:X} ({}) prefix validation FAILED: expected a CALL rel32 (opcode 0xE8), found bytes [{}]; aborting ROCK init",
                hookCallSite.address(),
                rock::hook_diagnostics::describeAddress(hookCallSite.address()),
                rock::hook_diagnostics::formatBytes(siteBytes, 5));
            return false;
        }

        auto& trampoline = F4SE::GetTrampoline();
        const auto original = trampoline.write_call<5>(hookCallSite.address(), &onGameFrameUpdateHook);
        s_originalGameLoopFunc = reinterpret_cast<GameLoopFunc>(original);

        if (!s_originalGameLoopFunc) {
            logger::critical("ROCK: Failed to hook main loop — original function pointer is null!");
            return false;
        }

        logger::info("ROCK: Main loop hook installed, original: (0x{:X}).", original);
        return true;
    }

    void onFRIKMessage(F4SE::MessagingInterface::Message* msg)
    {
        if (!msg || !s_frikAvailable) {
            return;
        }

        using LE = frik::api::FRIKApi::LifecycleEvent;

        switch (static_cast<LE>(msg->type)) {
        case LE::kSkeletonReady:
            logger::info("ROCK: Received kSkeletonReady from FRIK.");
            resetPolledSkeletonObservation();
            bumpGeneration(s_skeletonGeneration);
            if (!g_rockConfig.rockEnabled) {
                logger::info("ROCK: Physics disabled in config, skipping creation.");
                break;
            }
            if (s_physicsInteraction) {
                logger::warn("ROCK: PhysicsInteraction already exists on kSkeletonReady; deferring recreation to ROCK frame gate.");
            }
            requestDeferredPhysicsCreation();
            break;

        case LE::kSkeletonDestroying:
            logger::info("ROCK: Received kSkeletonDestroying from FRIK.");
            resetPolledSkeletonObservation();
            bumpGeneration(s_skeletonGeneration);
            s_physicsCreationRequested.store(false, std::memory_order_release);
            s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
            resetPhysicsCreationGate();
            if (s_physicsInteraction) {
                s_physicsInteraction->noteSkeletonLifecycle(
                    s_skeletonGeneration.load(std::memory_order_acquire),
                    rock::provider::RockProviderLifecycleReason::SkeletonDestroying);
            }
            destroyPhysicsInteraction(rock::provider::RockProviderLifecycleReason::SkeletonDestroying);
            break;

        case LE::kPowerArmorChanged:
            resetPolledSkeletonObservation();
            bumpGeneration(s_skeletonGeneration);
            if (msg->data && msg->dataLen >= sizeof(bool)) {
                const bool isInPA = *static_cast<const bool*>(msg->data);
                logger::info("ROCK: Power Armor state changed: {}", isInPA ? "IN PA" : "NOT IN PA");
            }
            if (s_physicsInteraction) {
                s_physicsInteraction->noteSkeletonLifecycle(
                    s_skeletonGeneration.load(std::memory_order_acquire),
                    rock::provider::RockProviderLifecycleReason::PowerArmorChanged);
            }
            break;

        default:

            break;
        }
    }

    void onF4SEMessage(F4SE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }

        if (msg->type == F4SE::MessagingInterface::kGameLoaded) {
            logger::info("ROCK: GameLoaded -- initializing FRIKApi and loading config...");
            resetPolledSkeletonObservation();
            const auto providerGeneration = bumpGeneration(s_providerGeneration);
            if (s_physicsInteraction) {
                s_physicsInteraction->noteProviderLifecycle(
                    providerGeneration,
                    rock::provider::RockProviderLifecycleReason::GameLoaded);
            }

            // Stock FRIK 0.77.12 ships API v3. Retain that exact, known-compatible
            // prefix for readiness/config/scalar-pose operations. The appended
            // visual-authority table is read only after a v5 version check; on v3,
            // hand-world operations route through Heisenberg's host callbacks.
            const int frikErr = frik::api::FRIKApi::initialize(
                frik_compatibility_policy::kStockFrik07712ApiVersion);
            if (frikErr != 0) {
                switch (frikErr) {
                case 1: logger::critical("ROCK: FRIKApi init FAILED (1). FRIK.dll not loaded. ROCK DISABLED."); break;
                case 2: logger::critical("ROCK: FRIKApi init FAILED (2). FRIKAPI_GetApi export missing. ROCK DISABLED."); break;
                case 3: logger::critical("ROCK: FRIKApi init FAILED (3). FRIKAPI_GetApi returned null. ROCK DISABLED."); break;
                case 4: logger::critical("ROCK: FRIKApi init FAILED (4). API v3 or newer is required. ROCK DISABLED."); break;
                default: logger::critical("ROCK: FRIKApi init FAILED ({}). ROCK DISABLED.", frikErr); break;
                }
                s_frikAvailable = false;
                return;
            }

            // Latch the loaded FRIK API version once (getVersion is member 0, always present).
            s_frikApiVersion = frik::api::FRIKApi::inst->getVersion();
            // Never null the v3 core pointer: doing so made visualAuthorityAvailable
            // false and permanently blocked PhysicsInteraction creation. All reads
            // beyond the v3 table boundary are centralized behind the version-aware
            // bridge.
            const bool nativeAuthority =
                frik_compatibility_policy::mayReadNativeVisualAuthorityTail(
                    true,
                    s_frikApiVersion);
            logger::info("ROCK: FRIK API v{} initialized. Native visual-authority {} "
                         "(host hand-world shim {}, host full-finger-pose shim {}).",
                         s_frikApiVersion,
                         nativeAuthority ? "AVAILABLE" : "ABSENT",
                         nativeAuthority ? "idle" : "ACTIVE",
                         nativeAuthority ? "idle" :
                             (s_hostFingerPoseAuthority ? "ACTIVE" : "MISSING"));

            // Upstream Jul-6: canonical 22-float hand-pose contract check. EMBED ADAPTATION: only
            // enforce when the NATIVE v5 authority is in use — on older FRIK the
            // host plugin-side hand authority replaces these functions, so ROCK must keep running.
            if (nativeAuthority) {
                const auto* frikApi = frik::api::FRIKApi::inst;
                const bool hasCanonicalHandPoseContract =
                    frikApi &&
                    frikApi->setHandPoseCustomWithPriority != nullptr &&
                    frikApi->getHandPoseLocalTransformsForPose != nullptr &&
                    frikApi->setHandPoseCustomLocalTransformsWithPriority != nullptr &&
                    frikApi->applyExternalHandWorldTransform != nullptr &&
                    frikApi->clearExternalHandWorldTransform != nullptr;
                if (!hasCanonicalHandPoseContract) {
                    logger::error(
                        "ROCK: FRIKApi v5 contract mismatch. Optional native hand-pose/visual operations will fail closed or use the host authority; collision remains enabled.");
                }
            }

            g_rockConfig.load();
            rock::frik_weapon_offset_cache::preload();
            rock::saved_grab_offset::preload();
            rock::installHavokTimingFixHook();
            runtime_state::initialize();
            see_through_scopes::refreshRuntimeState();
            logger::info("ROCK: Config loaded (rockEnabled={}).", g_rockConfig.rockEnabled);
            rock::input_remap_runtime::installInputRemapHooks();
            rock::debug::Install();

            s_frikAvailable = true;

            // EMBED: do NOT register our own {plugin,"F4VRBody"} FRIK listener here. This shared DLL
            // already registered Heisenberg's null-sender external listener; F4SE will not also deliver
            // to a second listener under the same plugin handle, so this registration was
            // dead-on-arrival — kSkeletonReady never arrived, the engine sat "waiting for skeleton",
            // and no hand/weapon colliders were ever built. Instead Heisenberg forwards every FRIK
            // "F4VRBody" message to rock::HostOnFRIKMessage() (see Heisenberg.cpp
            // OnExternalPluginMessage). Stock 0.77.12 sends no lifecycle messages,
            // so local/API readiness polling below is authoritative and forwarded
            // events from newer builds are only a fast path.
            logger::info("ROCK: FRIK lifecycle uses authoritative readiness polling plus optional host-forwarded events, sender '{}'.",
                frik::api::FRIKApi::FRIK_F4SE_MOD_NAME);

            logger::info("ROCK: Initialization complete. Waiting for skeleton...");
        }

        if (msg->type == F4SE::MessagingInterface::kPostLoadGame || msg->type == F4SE::MessagingInterface::kNewGame) {
            logger::info("ROCK: New game session -- resetting PhysicsInteraction...");
            resetPolledSkeletonObservation();
            const auto providerGeneration = bumpGeneration(s_providerGeneration);
            s_physicsCreationRequested.store(false, std::memory_order_release);
            s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
            resetPhysicsCreationGate();
            runtime_state::resetTransientState();
            s_externalHeldBodyRegistry.clear(true);
            s_externalHeldBodyRegistry.clear(false);
            if (s_physicsInteraction) {
                s_physicsInteraction->noteProviderLifecycle(
                    providerGeneration,
                    rock::provider::RockProviderLifecycleReason::ProviderLost);
            }

            destroyPhysicsInteraction();

            if (s_frikAvailable) {
                see_through_scopes::resetRuntimeState();
                g_rockConfig.reload();
                see_through_scopes::refreshRuntimeState();
                logger::info("ROCK: Config reloaded for new session.");
            }
        }
    }
}

namespace rock
{
    const F4SE::MessagingInterface* getROCKMessaging() { return s_messaging; }

    bool dispatchOptionalROCKMessage(
        std::uint32_t a_messageType,
        void* a_data,
        std::uint32_t a_dataLen,
        const char* a_receiver)
    {
        if (!s_messaging) {
            return false;
        }

        // MessagingInterface is CommonLibF4's facade over this exact F4SE
        // proxy (its own GetProxy() performs the same reinterpret_cast).
        // Calling the native proxy preserves dispatch semantics while avoiding
        // the wrapper's warning for the documented, benign no-listener case.
        const auto& proxy =
            reinterpret_cast<const F4SE::detail::F4SEMessagingInterface&>(*s_messaging);
        if (!proxy.Dispatch) {
            return false;
        }

        return proxy.Dispatch(
            F4SE::GetPluginHandle(),
            a_messageType,
            a_data,
            a_dataLen,
            a_receiver);
    }

    // Host forwarder: Heisenberg owns the single F4SE-core listener for this DLL, so it forwards every
    // F4SE message here. Takes void* to keep ROCKMain.h dependency-free; cast back to the real type.
    // This is how ROCK receives kGameLoaded / kPostLoadGame / kNewGame in the embed.
    void HostOnF4SEMessage(void* a_msg)
    {
        onF4SEMessage(static_cast<F4SE::MessagingInterface::Message*>(a_msg));
    }

    // Host forwarder for FRIK "F4VRBody" lifecycle messages (kSkeletonReady / kSkeletonDestroying /
    // kPowerArmorChanged / ...). Same constraint as HostOnF4SEMessage: ROCK cannot own a listener slot
    // in this shared DLL, so Heisenberg forwards every "F4VRBody"-sender message here. This is the ONLY
    // path by which the embedded engine learns the skeleton is ready and builds its hand/weapon
    // colliders — without it the engine stays parked at "waiting for skeleton".
    void HostOnFRIKMessage(void* a_msg)
    {
        onFRIKMessage(static_cast<F4SE::MessagingInterface::Message*>(a_msg));
    }

    bool HostIsPhysicsInteractionReady()
    {
        return s_physicsInteraction && s_physicsInteraction->isInitialized();
    }

    bool HostIsPerformanceBenchmarkBaseline()
    {
        return g_rockConfig.rockPerformanceBenchmarkMode == 2;
    }

    // EMBEDDED-HOST SEAM (audit rank 2) — see ROCKMain.h. Routes the host's grab lifecycle into
    // PhysicsInteraction's request slots; the engine applies them inside update() with a valid
    // world (suppress on grab start, config-delayed collider restore on release).
    void HostNotifyExternalGrab(bool a_isLeft, bool a_active)
    {
        if (s_physicsInteraction && s_physicsInteraction->isInitialized()) {
            s_physicsInteraction->hostNotifyExternalGrab(a_isLeft, a_active);
        }
    }

    void HostConsumeExternalGrabInputEdge(bool a_isLeft)
    {
        if (s_physicsInteraction && s_physicsInteraction->isInitialized()) {
            s_physicsInteraction->hostConsumeExternalGrabInputEdge(a_isLeft);
        }
    }

    void HostPublishExternalHeldBodies(
        const bool a_isLeft,
        const void* const a_hknpWorld,
        const std::uint32_t* const a_bodyIds,
        const std::uint32_t a_count)
    {
        s_externalHeldBodyRegistry.publish(
            a_isLeft,
            a_hknpWorld,
            a_bodyIds,
            a_count);
    }

    void HostClearExternalHeldBodies(const bool a_isLeft)
    {
        s_externalHeldBodyRegistry.clear(a_isLeft);
    }

    bool HostHasExternalHeldBodies()
    {
        return s_externalHeldBodyRegistry.any();
    }

    bool HostIsExternalHeldBody(
        const void* const a_hknpWorld,
        const std::uint32_t a_bodyId)
    {
        return s_externalHeldBodyRegistry.contains(
            a_hknpWorld,
            a_bodyId);
    }

    std::uint8_t HostExternalHeldBodyOwnerMask(
        const void* const a_hknpWorld,
        const std::uint32_t a_bodyId)
    {
        return s_externalHeldBodyRegistry.ownerMask(
            a_hknpWorld,
            a_bodyId);
    }

    void HostNotifyExternalRelease(bool a_isLeft, RE::TESObjectREFR* a_releasedRef)
    {
        if (s_physicsInteraction && s_physicsInteraction->isInitialized()) {
            s_physicsInteraction->hostNotifyExternalRelease(
                a_isLeft,
                a_releasedRef);
        }
    }

    // Plugin-side hand-authority seam (see ROCKMain.h). The bridge uses these when FRIK lacks v5.
    void HostSetHandAuthority(const HostHandAuthority* a_cbs) { s_hostHandAuthority = a_cbs; }
    const HostHandAuthority* getHostHandAuthority() { return s_hostHandAuthority; }
    void HostSetFingerPoseAuthority(const HostFingerPoseAuthority* a_cbs)
    {
        s_hostFingerPoseAuthority = a_cbs;
        logger::info(
            "ROCK(host): legacy full-finger-pose authority {}",
            a_cbs ? "registered" : "cleared");
    }
    const HostFingerPoseAuthority* getHostFingerPoseAuthority()
    {
        return s_hostFingerPoseAuthority;
    }
    void HostSetTwoHandedFingerPoseMode(const int a_mode)
    {
        const int sanitized = a_mode >= 0 && a_mode <= 2 ?
            a_mode :
            -1;
        s_hostTwoHandedFingerPoseMode.store(
            sanitized,
            std::memory_order_release);
    }
    int HostGetTwoHandedFingerPoseMode()
    {
        return s_hostTwoHandedFingerPoseMode.load(
            std::memory_order_acquire);
    }
    bool HostPublishUniformFingerPose(
        const char* const a_tag,
        const bool a_isLeft,
        const float a_thumb,
        const float a_index,
        const float a_middle,
        const float a_ring,
        const float a_pinky,
        const int a_priority)
    {
        return frik_visual_authority::setHandPoseCustomWithPriority(
            a_tag,
            frik_visual_authority::handFromBool(a_isLeft),
            frik_visual_authority::makeUniformHandPoseData(
                a_thumb,
                a_index,
                a_middle,
                a_ring,
                a_pinky),
            a_priority);
    }
    bool HostClearFingerPose(
        const char* const a_tag,
        const bool a_isLeft)
    {
        return frik_visual_authority::clearHandPose(
            a_tag,
            frik_visual_authority::handFromBool(a_isLeft));
    }
    bool frikHasVisualAuthority() { return s_frikApiVersion >= 5; }

    // ── EMBEDDED HOST ENTRY ──────────────────────────────────────────────────
    // Heisenberg owns the F4SE plugin entry point (F4SEPlugin_Query/Load) and calls this after
    // its own init, gated on the bUseRockEngineArchitecture toggle. Same setup as the standalone
    // F4SEPlugin_Load MINUS F4SE::Init (Heisenberg already called it). The original
    // F4SEPlugin_Query/Load exports are removed so they don't collide with Heisenberg's exports.
    bool HostLoad(const F4SE::LoadInterface* /*a_f4se*/)
    {
        // CRITICAL: every ROCK engine TU does `using namespace f4cf`, so all ROCK `logger::*`
        // calls route through f4cf::logger::internal::loggerInstance — a shared_ptr that is NULL until
        // f4cf::logger::init() runs. Heisenberg sets up its OWN spdlog logger via F4SE::Init and
        // never inits the framework logger, so the standalone ROCK's `logger::init("ROCK")` (which
        // lived in the F4SEPlugin_Query we deleted) is still required here — otherwise the very
        // first ROCK logger::info() below dereferences a null logger and crashes the game.
        // logger::init() also calls spdlog::set_default_logger(); capture + restore Heisenberg's
        // default so its own spdlog::info() keeps writing to HeisenbergF4VR.log (ROCK logs to
        // ROCK.log via its own internal::_logger, independent of the default logger).
        {
            auto prevDefault = spdlog::default_logger();
            // The host reads the launch-time logging policy before entering
            // HostLoad. Inherit that decision instead of silently promoting
            // the embedded engine to Debug. This also leaves one deliberate
            // escape hatch for verbose launch diagnostics: the host can select
            // Debug before calling HostLoad and both loggers will agree.
            const auto startupLogLevel =
                prevDefault ? prevDefault->level() : spdlog::level::err;
            logger::init("ROCK");
            // Right after init, the ROCK logger is the default. Apply the
            // host-selected launch level while retaining warn-and-higher
            // flushing for useful crash tails without per-line frame cost.
            // ROCK's own internal logger is this same object, so both policies
            // persist after the Heisenberg default logger is restored.
            if (auto rockLogger = spdlog::default_logger()) {
                rockLogger->set_level(startupLogLevel);
                // PERF (Jul 5): was flush_on(trace) — an fflush per LINE, a deliberate
                // CTD-diagnostic so the last ROCK.log line marks the crash point. The embed
                // now creates cleanly and WER minidump + dmp.py/sym.py cover crashes, so trade
                // it for frame time. Revert to trace temporarily when chasing a new CTD.
                rockLogger->flush_on(spdlog::level::warn);
            }
            if (prevDefault) {
                spdlog::set_default_logger(prevDefault);
            }

            // SINGLE LOG (Jul 19, user directive): route the embedded engine's framework
            // logger into Heisenberg's own log file — ONE HeisenbergF4VR.log for everything
            // (testers only ever send one file; ROCK.log kept going unread). Same DLL, same
            // spdlog registry: rebuild internal::_logger on the HOST logger's sinks so both
            // write through one file sink (no dual-handle interleaving). Engine lines are
            // already self-tagging ([ROCK::...] / ROCK(host):). Keep the engine's level and
            // the warn-flush CTD policy from above; do NOT set a pattern (the host sink's
            // formatter stays authoritative).
            if (prevDefault && !prevDefault->sinks().empty() && logger::internal::loggerInstance) {
                auto merged = std::make_shared<spdlog::logger>(
                    "ROCK", prevDefault->sinks().begin(), prevDefault->sinks().end());
                merged->set_level(startupLogLevel);
                merged->flush_on(spdlog::level::warn);
                logger::internal::loggerInstance = merged;
                // BUGFIX (Jul 21): "do NOT set a pattern" above only means this block itself
                // skips calling setLogLevelAndPattern - it does NOT stop RockConfig::load()
                // from calling it moments later (onF4SEMessage, right after HostLoad returns).
                // internal::_logPattern is a framework-wide inline global that still holds its
                // compile-time default ("%H:%M:%S.%e %l: %v") here, which does not textually
                // match logging_policy::DefaultLogPattern - so that first load's
                // `_logPattern != logPattern` check was unconditionally true, and
                // spdlog::set_formatter() is registry-wide: it restamps every REGISTERED
                // logger's sinks. `merged` is never registered (only set_default_logger()/
                // register_logger() register a logger, and neither runs on it), but
                // `prevDefault` IS registered (restored as default just above) and shares
                // these exact sink objects with `merged` - so the restamp landed on the
                // shared file sink anyway, silently defeating "host sink stays authoritative"
                // the moment kGameLoaded's g_rockConfig.load() ran. Seed the in-memory pattern
                // to ROCK's own default so that near-certain first load is a true no-op; a
                // genuine user sLogPattern= override in Heisenberg_F4VR.ini still differs and
                // still applies normally.
                logger::internal::logPattern = logging_policy::DefaultLogPattern;
                logger::info("ROCK(host): engine log merged into HeisenbergF4VR.log (single-log mode)");
            }
        }
        logger::info("ROCK(host): HostLoad - initializing embedded engine...");
        s_messaging = F4SE::GetMessagingInterface();
        if (!s_messaging) {
            logger::critical("ROCK(host): Failed to get F4SE MessagingInterface. Cannot continue.");
            return false;
        }
        // NOTE: ROCK does NOT register its own F4SE-core listener in the embed. This is a single DLL
        // that already registered one (Heisenberg's, first) and F4SE keeps only one listener per
        // {plugin,"F4SE"} — ROCK's would be dead-on-arrival (kGameLoaded never arrives → FRIK never
        // inits → no physics). Instead, Heisenberg forwards every F4SE message here via
        // rock::HostOnF4SEMessage(). FRIK "F4VRBody" lifecycle messages (kSkeletonReady etc.) are
        // forwarded the SAME way via rock::HostOnFRIKMessage() — ROCK's own {plugin,"F4VRBody"} listener
        // is likewise dead-on-arrival in this shared DLL, so we do NOT register it (see kGameLoaded).
        // DO NOT call F4SE::AllocTrampoline here in the embed. Heisenberg already allocated a single
        // shared trampoline (Hooks.cpp, sized to include ROCK's budget) BEFORE HostLoad runs. A second
        // AllocTrampoline calls set_trampoline()->release(), which FREES Heisenberg's trampoline and
        // leaves its already-installed hook stubs dangling → execute-AV the moment a per-frame hook
        // (e.g. a PlayerUpdateEvent sink) first fires after the player spawns. ROCK's hooks below
        // append to the existing shared trampoline via F4SE::GetTrampoline().
        // Upstream Jul-5 (grenade-grab): loose grenade equip interception.
        //
        // 2026-07-28 — ARBITRATION, NOT A SECOND HOOK. This used to install ROCK's own raw entry
        // detour on ActorEquipManager::EquipObject (0x0E6FEA0). That could never work in the embed:
        // Heisenberg's HookEquipObject already owns that entry (Hooks::Install runs before
        // HostLoad), an entry holds exactly ONE raw detour, and ROCK's installer then memcmp'd the
        // prologue the host had itself overwritten and failed — so grenade interception had never
        // once executed. That is the "grenades drop at my feet" report. Heisenberg's detour now
        // CALLS rock::HostTryInterceptEquipObject; installEquipHook() only registers that seam and
        // reports what is on the entry. Order-swapping is NOT an alternative: the host's installer
        // only recognises pristine prologue patterns, so if ROCK patched first the host would log
        // "Unknown prologue pattern! Hook NOT installed" and holotape/consumable-to-hand would break.
        //
        // EMBED (Jul 10): NON-FATAL. Standalone ROCK aborts load when this fails; in the embed that
        // killed the ENTIRE engine (HostLoad FAILED → fallback physics). Degrade instead:
        // installEquipHook() latches only the interception feature OFF and everything else runs.
        //
        // The message that used to live here asserted "another mod likely hooks EquipObject".
        // NOTHING in this path had probed for another mod — it was a hardcoded guess, and it was
        // twice mistaken for evidence, sending investigations after a plugin that was not loaded on
        // either the dev or the tester machine. installEquipHook() now reports the address, the
        // found bytes, the expected pristine bytes and the module the entry branches into; this call
        // site adds no theory of its own about who or what changed them.
        if (!rock::loose_grenade_runtime::installEquipHook()) {
            ROCK_LOG_WARN(Init,
                "Continuing WITHOUT loose-grenade equip interception. The preceding report states the target address, the bytes "
                "found there, the bytes a pristine entry has, and the module the entry branches into. User-visible consequence: "
                "grenades equipped from the Pip-Boy or favourites use the game's native equip instead of becoming a physical "
                "loose grenade in the hand; the rest of the engine is unaffected.");
        }
        if (!hookMainLoop()) {
            return false;
        }
        if (!hookNativeScopeGeometryDecision()) {
            // Scope re-entry suppression is isolated from the physics engine.
            // Preserve every other interaction feature if another scope mod
            // already owns this verified call site.
            logger::warn(
                "ROCK(host): continuing without native scope "
                "re-entry suppression");
        }
        // See-Through-Scopes late-culling hook: ONLY install when the STS mod is actually present.
        // It is a RENDER-path hook (late culling pass) whose sole purpose is scope-rendering compat;
        // ROCK's own STS logic no-ops without FO4VR_better_scopes.dll. Installing the branch anyway
        // plants an extra hook in the main render/update dispatch that overlaps other render mods
        // (e.g. Inventory3DFix) for zero benefit — observed as a CTD on save-load in the embed.
        // Mirroring ROCK's own detection at the hook-install level keeps behavior identical when STS
        // IS present and removes all render-path risk when it isn't.
        // NOTE (embed): the pinned F4VRCommonFramework no longer exports
        // f4cf::common::isDLLModLoaded. Same semantics via the module table, which
        // is what the rest of the tree already uses for DLL-presence probes.
        if (GetModuleHandleA("FO4VR_better_scopes.dll") != nullptr) {
            if (!rock::see_through_scopes::installLateCullingHook()) {
                return false;
            }
        } else {
            logger::info("ROCK(host): See-Through-Scopes mod (FO4VR_better_scopes.dll) not detected; skipping late-culling render hook.");
        }
        s_pluginLoaded = true;
        logger::info("ROCK(host): embedded engine load complete. Waiting for GameLoaded event...");
        return true;
    }

    // ── EQUIP-OBJECT DETOUR ARBITRATION (grenade fix, 2026-07-28) ────────────────────────
    // Thin forwarding to loose_grenade_runtime. Declared in ROCKMain.h so the host does not
    // have to include an internal physics-interaction header. See that header for why ROCK
    // installs no detour of its own at ActorEquipManager::EquipObject.
    HostEquipInterception HostTryInterceptEquipObject(
        RE::Actor* a_actor,
        const RE::BGSObjectInstance* a_object,
        std::uint32_t a_stackID,
        std::uint32_t a_number)
    {
        switch (rock::loose_grenade_runtime::tryInterceptEquipObject(a_actor, a_object, a_stackID, a_number)) {
        case rock::loose_grenade_runtime::EquipInterceptionResult::ConsumedEquipped:
            return HostEquipInterception::ConsumedEquipped;
        case rock::loose_grenade_runtime::EquipInterceptionResult::ConsumedBlocked:
            return HostEquipInterception::ConsumedBlocked;
        case rock::loose_grenade_runtime::EquipInterceptionResult::NotIntercepted:
        default:
            return HostEquipInterception::NotIntercepted;
        }
    }

    void HostSetEquipObjectDetourInstalled(bool a_installed, const char* a_reasonWhenAbsent)
    {
        rock::loose_grenade_runtime::setHostEquipObjectDetourInstalled(a_installed, a_reasonWhenAbsent);
    }

    void HostEquipObjectPassThroughBegin()
    {
        rock::loose_grenade_runtime::beginHostEquipPassThrough();
    }

    void HostEquipObjectPassThroughEnd()
    {
        rock::loose_grenade_runtime::endHostEquipPassThrough();
    }

    void HostSetGrabOwnership(bool a_rockOwnsGrab)
    {
        auto configMutation = g_rockConfig.pauseNativeReadsForMutation();
        g_rockConfig.rockHostGrabOwnershipConfigured = true;
        g_rockConfig.rockHostGrabOwnershipForced = a_rockOwnsGrab;
        g_rockConfig.rockGrabEnabled = a_rockOwnsGrab;
        g_rockConfig.rockSelectionEnabled = a_rockOwnsGrab;
        // Embedded ROCK consumes the host ViewCaster candidate.  Its own VATS
        // glow and curved beam are deliberately visual-only and must not
        // compete with Fallout VR's native selection feedback.
        if (a_rockOwnsGrab) {
            g_rockConfig.rockHighlightEnabled = false;
            g_rockConfig.rockSelectionBeamEnabled = false;
        }
        if (!a_rockOwnsGrab) {
            for (auto& candidate : s_hostViewCasterGrabCandidates) {
                candidate.reset();
            }
        }
        logger::info("ROCK(host): grab+selection ownership {} (Heisenberg resolved ownership seam)",
            a_rockOwnsGrab ? "ceded to embedded ROCK" : "retained by host");
    }

    void HostPublishViewCasterGrabCandidate(
        bool a_isLeft,
        RE::TESObjectREFR* a_target)
    {
        s_hostViewCasterGrabCandidates[a_isLeft ? 1u : 0u].reset(a_target);
    }

    RE::TESObjectREFR* HostGetViewCasterGrabCandidate(bool a_isLeft)
    {
        return s_hostViewCasterGrabCandidates[a_isLeft ? 1u : 0u].get();
    }

    namespace
    {
        bool s_hostWeaponCollisionSuppressed = false;
        bool s_hostTwoHandedGripBlocked = false;
    }

    void HostSetWeaponCollisionSuppressed(bool a_suppressed)
    {
        if (s_hostWeaponCollisionSuppressed != a_suppressed) {
            logger::info("ROCK(host): weapon collision {} via host API", a_suppressed ? "SUPPRESSED" : "restored");
        }
        s_hostWeaponCollisionSuppressed = a_suppressed;
    }

    bool HostIsWeaponCollisionSuppressed() { return s_hostWeaponCollisionSuppressed; }

    void HostSetTwoHandedGripBlocked(bool a_blocked)
    {
        if (s_hostTwoHandedGripBlocked != a_blocked) {
            logger::info("ROCK(host): off-hand weapon gripping {} via host API", a_blocked ? "BLOCKED" : "unblocked");
        }
        s_hostTwoHandedGripBlocked = a_blocked;
    }

    bool HostIsTwoHandedGripBlocked() { return s_hostTwoHandedGripBlocked; }

    namespace
    {
        bool s_hostHandCollisionSuppressed[2] = { false, false };  // [0]=Right [1]=Left
    }

    void HostSetHandCollisionSuppressed(bool a_isLeft, bool a_suppressed)
    {
        bool& slot = s_hostHandCollisionSuppressed[a_isLeft ? 1 : 0];
        if (slot != a_suppressed) {
            logger::info("ROCK(host): {} hand collision {} via host API",
                a_isLeft ? "left" : "right", a_suppressed ? "SUPPRESSED" : "restored");
        }
        slot = a_suppressed;
    }

    bool HostIsHandCollisionSuppressed(bool a_isLeft) { return s_hostHandCollisionSuppressed[a_isLeft ? 1 : 0]; }

    namespace
    {
        bool s_hostHandHoldingObject[2] = { false, false };  // [0]=Right [1]=Left
        std::atomic_bool s_hostHandDynamicPushAssistSuppressed{ false };
    }

    // Jul 19 (user rule): a hand that is holding a grabbed object must not capture a
    // two-handed support grip. Pushed per-frame by the host from its grab state.
    void HostSetHandHoldingObject(bool a_isLeft, bool a_holding)
    {
        s_hostHandHoldingObject[a_isLeft ? 1 : 0] = a_holding;
    }

    bool HostIsHandHoldingObject(bool a_isLeft) { return s_hostHandHoldingObject[a_isLeft ? 1 : 0]; }

    void HostSetHandDynamicPushAssistSuppressed(bool a_suppressed)
    {
        const bool previous = s_hostHandDynamicPushAssistSuppressed.exchange(
            a_suppressed, std::memory_order_relaxed);
        if (previous != a_suppressed) {
            logger::info("ROCK(host): scripted hand DynamicPushAssist {} (host proximity-push coexistence)",
                a_suppressed ? "SUPPRESSED" : "restored");
        }
    }

    bool HostIsHandDynamicPushAssistSuppressed()
    {
        return s_hostHandDynamicPushAssistSuppressed.load(std::memory_order_relaxed);
    }

    RE::TESObjectREFR* HostGetHandTouchedRef(bool a_isLeft)
    {
        return s_physicsInteraction ? s_physicsInteraction->hostGetHandTouchedRef(a_isLeft) : nullptr;
    }

    bool HostGetHandTouchEvidence(
        bool a_isLeft,
        std::uint32_t a_maxAgeFrames,
        RE::TESObjectREFR** a_outRef,
        std::uint32_t* a_outBodyId,
        std::uint32_t* a_outAgeFrames,
        RE::NiPoint3* a_outContactPointWorld,
        bool* a_outHasContactPoint)
    {
        if (a_outRef) {
            *a_outRef = nullptr;
        }
        if (a_outBodyId) {
            *a_outBodyId = 0x7FFF'FFFFu;
        }
        if (a_outAgeFrames) {
            *a_outAgeFrames = 0xFFFF'FFFFu;
        }
        if (a_outContactPointWorld) {
            *a_outContactPointWorld = {};
        }
        if (a_outHasContactPoint) {
            *a_outHasContactPoint = false;
        }
        if (!s_physicsInteraction || !a_outRef || !a_outBodyId ||
            !a_outAgeFrames || !a_outContactPointWorld ||
            !a_outHasContactPoint) {
            return false;
        }
        return s_physicsInteraction->hostGetHandTouchEvidence(
            a_isLeft,
            a_maxAgeFrames,
            *a_outRef,
            *a_outBodyId,
            *a_outAgeFrames,
            *a_outContactPointWorld,
            *a_outHasContactPoint);
    }

    bool HostGetHeldObjectSnapshot(
        bool a_isLeft,
        HostHeldObjectSnapshot& a_out)
    {
        a_out = {};
        if (!s_physicsInteraction) {
            return false;
        }
        return s_physicsInteraction->hostGetHeldObjectSnapshot(
            a_isLeft,
            a_out.ref,
            a_out.heldNode,
            a_out.heldSeconds,
            a_out.handSpeedMetersPerSecond,
            a_out.grabTraceId);
    }

    bool HostGetExactItemGrabProfile(
        RE::TESObjectREFR* a_ref,
        bool a_isLeft,
        HostExactItemGrabProfile& a_out)
    {
        a_out = {};
        if (!a_ref) {
            return false;
        }
        auto& offsetManager =
            heisenberg::ItemOffsetManager::GetSingleton();
        auto resolved = offsetManager.GetExactOffset(a_ref, a_isLeft);
        if (!resolved.has_value()) {
            /*
             * Bounds in TESBoundObject are integral model-space extents. An
             * exact L/W/H triple is therefore a useful donor key for objects
             * that reuse the same geometry but have a different form/name.
             * GetExactDimensionsOffset remains strict (no tolerance), prefers
             * the same form type, and resolves ties deterministically; fuzzy
             * shape matching is deliberately excluded from this bridge.
             */
            resolved = offsetManager.GetExactDimensionsOffset(
                a_ref, a_isLeft);
        }
        if (!resolved.has_value()) {
            return false;
        }

        auto profile = *resolved;
        const bool namedLeft =
            profile.matchedName.size() >= 2 &&
            profile.matchedName.compare(profile.matchedName.size() - 2, 2, "_L") == 0;
        const bool namedRight =
            profile.matchedName.size() >= 2 &&
            profile.matchedName.compare(profile.matchedName.size() - 2, 2, "_R") == 0;
        if (profile.isRightHandSpace) {
            if (a_isLeft && !profile.isLeftHanded && !namedLeft) {
                profile.position.z = -profile.position.z;
                profile.rotation.entry[0][1] = -profile.rotation.entry[0][1];
                profile.rotation.entry[1][0] = -profile.rotation.entry[1][0];
                profile.rotation.entry[1][2] = -profile.rotation.entry[1][2];
                profile.rotation.entry[2][1] = -profile.rotation.entry[2][1];
            }
        } else if (!a_isLeft && !namedRight) {
            profile.position.x = -profile.position.x;
            profile.rotation.entry[0][1] = -profile.rotation.entry[0][1];
            profile.rotation.entry[0][2] = -profile.rotation.entry[0][2];
            profile.rotation.entry[1][0] = -profile.rotation.entry[1][0];
            profile.rotation.entry[2][0] = -profile.rotation.entry[2][0];
        }

        a_out.localPosition = profile.position;
        a_out.localRotation = profile.rotation;
        a_out.fingerCurls = { profile.thumbCurl, profile.indexCurl,
            profile.middleCurl, profile.ringCurl, profile.pinkyCurl };
        std::copy(std::begin(profile.jointCurls), std::end(profile.jointCurls),
            a_out.jointCurls.begin());
        a_out.hasFingerCurls = profile.hasFingerCurls;
        a_out.hasJointCurls = profile.hasJointCurls;
        if (profile.isFRIKOffset) {
            a_out.parentWorldValid =
                HostGetPreAuthorityHandWorld(a_isLeft, a_out.parentWorld);
        } else if (auto* nodes = f4cf::f4vr::getPlayerNodes()) {
            if (auto* wand = heisenberg::GetWandNode(nodes, a_isLeft)) {
                a_out.parentWorld = wand->world;
                a_out.parentWorldValid = true;
            }
        }
        if (!a_out.parentWorldValid) {
            a_out.parentWorld = {};
        }
        return true;
    }

    bool HostReleaseHeldObjectForInventory(
        bool a_isLeft,
        RE::TESObjectREFR* a_expectedRef,
        std::uint64_t a_expectedGrabTraceId)
    {
        return s_physicsInteraction &&
               s_physicsInteraction->hostReleaseHeldObjectForInventory(
                   a_isLeft,
                   a_expectedRef,
                   a_expectedGrabTraceId);
    }

    void HostSetPlayerConsumeBlockCallback(
        HostPlayerConsumeBlockFn a_fn)
    {
        s_hostPlayerConsumeBlockFn = a_fn;
        logger::info(
            "ROCK(host): player-consume reservation callback {}",
            a_fn ? "registered" : "cleared");
    }

    bool HostShouldBlockPlayerConsume(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref || !s_hostPlayerConsumeBlockFn) {
            return false;
        }
        try {
            return s_hostPlayerConsumeBlockFn(a_ref);
        } catch (...) {
            logger::error(
                "ROCK(host): player-consume reservation callback faulted; blocking the current held item");
            return true;
        }
    }

    void HostSetPlayerConsumeProfileCallback(
        HostPlayerConsumeProfileFn a_fn)
    {
        s_hostPlayerConsumeProfileFn = a_fn;
        logger::info(
            "ROCK(host): player-consume route/profile callback {}",
            a_fn ? "registered" : "cleared");
    }

    bool HostResolvePlayerConsumeProfile(
        RE::TESObjectREFR* a_ref,
        bool a_holdingHandIsLeft,
        HostPlayerConsumeProfile& a_inOut)
    {
        if (!a_ref) {
            return false;
        }
        if (!s_hostPlayerConsumeProfileFn) {
            return true;
        }
        try {
            return s_hostPlayerConsumeProfileFn(
                a_ref,
                a_holdingHandIsLeft,
                a_inOut);
        } catch (...) {
            logger::error(
                "ROCK(host): player-consume route/profile callback faulted; blocking the current held item");
            return false;
        }
    }

    std::uint32_t HostCopyHandCollisionSamples(
        bool a_isLeft,
        RE::NiPoint3* a_outWorldPoints,
        float* a_outRadiiGame,
        std::uint32_t a_maxSamples)
    {
        return s_physicsInteraction
            ? s_physicsInteraction->hostCopyHandCollisionSamples(
                  a_isLeft,
                  a_outWorldPoints,
                  a_outRadiiGame,
                  a_maxSamples)
            : 0;
    }

    std::uint32_t HostCopyWeaponCollisionSamples(
        RE::NiPoint3* a_outWorldPoints,
        float* a_outRadiiGame,
        std::uint32_t a_maxSamples)
    {
        return s_physicsInteraction
            ? s_physicsInteraction->hostCopyWeaponCollisionSamples(
                  a_outWorldPoints,
                  a_outRadiiGame,
                  a_maxSamples)
            : 0;
    }

    void HostSetHandColliderRadiusPadding(float a_padding)
    {
        if (!std::isfinite(a_padding)) {
            return;
        }
        // Ceiling is 3.0, not 1.0: the host raised its own clamp (src/Config.cpp
        // fHandColliderRadiusPadding) from 1.0 to 3.0 on Jul 27 because measured
        // finger-vs-token intersections reached 1.26gu, i.e. DEEPER than the largest
        // padding a 1.0 cap would accept - the symptom was untunable by construction.
        // Keep this in step with src/Config.cpp, RockConfig.cpp's INI clamp, the
        // in-function clamps in HandBoneColliderSet/BodyBoneColliderSet, and the MCM
        // slider max, or the host's ceiling silently cannot reach the engine.
        const float clamped = std::clamp(a_padding, 0.0f, 3.0f);
        auto configMutation = g_rockConfig.pauseNativeReadsForMutation();
        if (g_rockConfig.rockHandBoneColliderRadiusPadding != clamped) {
            logger::info("ROCK(host): hand collider radius padding {} -> {} via host API (colliders will rebuild)",
                g_rockConfig.rockHandBoneColliderRadiusPadding, clamped);
        }
        g_rockConfig.rockHandBoneColliderRadiusPadding = clamped;
    }

    void HostSetTwoHandedMinSteeringAuthority(float a_authority)
    {
        if (!std::isfinite(a_authority)) {
            return;
        }
        // The standalone engine accepts 0..1. Heisenberg deliberately starts at 0.35,
        // making the bottom of its slider identical to the pre-slider behavior.
        const float clamped = std::clamp(a_authority, 0.35f, 1.0f);
        auto configMutation = g_rockConfig.pauseNativeReadsForMutation();
        if (g_rockConfig.rockTwoHandedMinSteeringAuthority != clamped) {
            logger::info("ROCK(host): minimum off-hand steering authority {} -> {} via host API",
                g_rockConfig.rockTwoHandedMinSteeringAuthority, clamped);
        }
        g_rockConfig.rockTwoHandedMinSteeringAuthority = clamped;
    }

    bool HostIsLargeObjectGrabBlockEnabled()
    {
        return g_rockConfig.rockLargeObjectPlayerBlockEnabled && g_rockConfig.rockLargeObjectGrabBlockEnabled;
    }

    float HostGetLargeObjectBoundThresholdGameUnits()
    {
        return g_rockConfig.rockLargeObjectBoundThresholdGameUnits;
    }

    bool HostIsWeaponSupportEngaged(bool a_isLeft)
    {
        return s_physicsInteraction && s_physicsInteraction->hostIsWeaponSupportEngaged(a_isLeft);
    }

    bool HostIsWeaponSupportGripped(bool a_isLeft)
    {
        return s_physicsInteraction && s_physicsInteraction->hostIsWeaponSupportGripped(a_isLeft);
    }

    bool HostArmNativeScopeReentryBlockIfActive(
        const bool a_hostObservedScopeActive)
    {
        bool nativeScopeActive = false;
        const bool nativeStateAvailable =
            tryReadNativeScopeRequestState(
                nativeScopeActive);
        if (!a_hostObservedScopeActive &&
            (!nativeStateAvailable || !nativeScopeActive)) {
            return false;
        }

        if (!s_nativeScopeGeometryHookInstalled.load(
                std::memory_order_acquire)) {
            // Still report an active presentation so the host performs the
            // immediate close. Do not create a block that has no geometry
            // hook available to observe its rearm condition.
            return true;
        }

        const bool wasBlocked =
            s_nativeScopeReentryBlocked.exchange(
                true,
                std::memory_order_acq_rel);
        if (!wasBlocked) {
            logger::info(
                "ROCK(host): native scope re-entry blocked until "
                "the optic leaves its activation cone "
                "(hostObserved={}, nativeActive={})",
                a_hostObservedScopeActive,
                nativeScopeActive);
        }
        return true;
    }

    bool HostIsNativeScopeReentryBlocked()
    {
        return s_nativeScopeReentryBlocked.load(
            std::memory_order_acquire);
    }

    bool HostGetLiveGripHandWorld(bool a_isLeft, RE::NiTransform& a_out)
    {
        return s_physicsInteraction && s_physicsInteraction->hostComputeLiveGripHandWorld(a_isLeft, a_out);
    }

    bool HostGetWeaponSupportFingerCurls(bool a_isLeft, float a_outCurls[5])
    {
        return s_physicsInteraction && s_physicsInteraction->hostGetWeaponSupportFingerCurls(a_isLeft, a_outCurls);
    }


    void HostSetPreAuthorityHandWorlds(const RE::NiTransform& a_left, const RE::NiTransform& a_right)
    {
        s_preAuthHandWorld[1] = a_left;
        s_preAuthHandWorld[0] = a_right;
        s_preAuthHandProvenance =
            pre_authority_hand_frame_policy::Provenance::CleanPass1;
        // Reach is paired with this exact clean-hand publication. Invalidate the old
        // pair first; FrikArmGoalHook fills whichever current arms resolve safely.
        s_preAuthArmReachValid[0] = false;
        s_preAuthArmReachValid[1] = false;
    }

    void HostSetFallbackPreAuthorityHandWorlds(
        const RE::NiTransform& a_left,
        const RE::NiTransform& a_right)
    {
        s_preAuthHandWorld[1] = a_left;
        s_preAuthHandWorld[0] = a_right;
        s_preAuthHandProvenance =
            pre_authority_hand_frame_policy::Provenance::SkinnedFallback;
        // The fallback pair can still serve legacy weapon consumers, but any
        // old reach sample belonged to a different hand publication.
        s_preAuthArmReachValid[0] = false;
        s_preAuthArmReachValid[1] = false;
    }

    bool HostRequiresPreAuthorityHandWorld()
    {
        return s_pluginLoaded;
    }

    bool HostGetPreAuthorityHandWorld(bool a_isLeft, RE::NiTransform& a_out)
    {
        if (!pre_authority_hand_frame_policy::isAvailable(
                s_preAuthHandProvenance)) {
            return false;
        }
        a_out = s_preAuthHandWorld[a_isLeft ? 1 : 0];
        return true;
    }

    bool HostGetCleanPreAuthorityHandWorld(
        bool a_isLeft,
        RE::NiTransform& a_out)
    {
        if (!pre_authority_hand_frame_policy::isTrueCleanPass1(
                s_preAuthHandProvenance)) {
            return false;
        }
        a_out = s_preAuthHandWorld[a_isLeft ? 1 : 0];
        return true;
    }

    void HostSetPreAuthorityArmReach(bool a_isLeft, const RE::NiPoint3& a_shoulderWorld,
        float a_maxReach)
    {
        const int i = a_isLeft ? 1 : 0;
        const bool finite = std::isfinite(a_shoulderWorld.x) &&
            std::isfinite(a_shoulderWorld.y) && std::isfinite(a_shoulderWorld.z) &&
            std::isfinite(a_maxReach);
        if (!pre_authority_hand_frame_policy::isAvailable(
                s_preAuthHandProvenance) ||
            !finite || a_maxReach <= 1.0f || a_maxReach >= 200.0f) {
            s_preAuthArmReachValid[i] = false;
            return;
        }
        s_preAuthShoulderWorld[i] = a_shoulderWorld;
        s_preAuthMaxReach[i] = a_maxReach;
        s_preAuthArmReachValid[i] = true;
    }

    bool HostGetPreAuthorityArmReach(bool a_isLeft, RE::NiPoint3& a_shoulderWorld,
        float& a_maxReach)
    {
        const int i = a_isLeft ? 1 : 0;
        if (!pre_authority_hand_frame_policy::isAvailable(
                s_preAuthHandProvenance) ||
            !s_preAuthArmReachValid[i]) {
            return false;
        }
        a_shoulderWorld = s_preAuthShoulderWorld[i];
        a_maxReach = s_preAuthMaxReach[i];
        return true;
    }

    void HostSetPostUpdateCallback(HostPostUpdateFn a_fn)
    {
        s_hostPostUpdateFn = a_fn;
        logger::info("ROCK(host): post-update hand-authority callback {}", a_fn ? "registered" : "cleared");
    }
}
