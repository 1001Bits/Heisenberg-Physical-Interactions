
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
#include "physics-interaction/grab/FrikWeaponOffsetCache.h"
#include "physics-interaction/grab/SavedGrabOffsetStore.h"
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "physics-interaction/visual/FrikVisualAuthorityBridge.h"
#include "physics-interaction/weapon/SeeThroughScopesCompatibility.h"
#include "physics-interaction/RockLoggingPolicy.h"

#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"

namespace
{
    using namespace rock;

    const F4SE::MessagingInterface* s_messaging = nullptr;

    PhysicsInteraction* s_physicsInteraction = nullptr;
    bool s_physicsPublished = false;

    bool s_frikAvailable = false;
    std::uint32_t s_frikApiVersion = 0;                        // latched loaded FRIK API version
    const rock::HostHandAuthority* s_hostHandAuthority = nullptr;  // plugin-side hand placement (old FRIK)

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
    bool s_preAuthHandFresh = false;           // set by host, cleared at onFrameUpdate tail
    RE::NiPoint3 s_preAuthShoulderWorld[2]{};
    float s_preAuthMaxReach[2] = { 0.0f, 0.0f };
    bool s_preAuthArmReachValid[2] = { false, false };
    rock::HostPostUpdateFn s_hostPostUpdateFn = nullptr;
    std::atomic<std::uint32_t> s_providerGeneration{ 1 };
    std::atomic<std::uint32_t> s_skeletonGeneration{ 1 };
    std::atomic<bool> s_physicsCreationRequested{ false };
    std::atomic<std::uint32_t> s_physicsCreationReadyDeferralFrames{ 0 };
    physics_creation_gate_policy::WorldStabilityState s_physicsCreationWorldStability{};
    std::uint32_t s_physicsCreationGateLogCounter = 0;

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
        if (!s_physicsCreationRequested.load(std::memory_order_acquire) &&
            !s_physicsInteraction &&
            g_rockConfig.rockEnabled &&
            runtime.visualAuthorityAvailable &&
            runtime.localSkeletonReady) {
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
            .providerAvailable = s_frikAvailable && runtime.visualAuthorityAvailable,
            .skeletonReady = runtime.localSkeletonReady,
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
        s_physicsInteraction->noteProviderLifecycle(
            s_providerGeneration.load(std::memory_order_acquire),
            reason);
        s_physicsInteraction->shutdown(reason);
        rock::provider::dispatchFrameCallbacks(*s_physicsInteraction);

        rock::provider::setPhysicsInteractionInstance(nullptr);
        rock::provider::clearExternalBodiesForProviderLoss();
        s_physicsPublished = false;

        // Drain in-flight physics-thread callbacks (hookedProcessConstraintsCallback,
        // native contact events) before freeing the instance they dereference. Both
        // already checked s_hooksEnabled==false above will exit without ever incrementing
        // s_inFlightCallbacks; any that passed the check a moment earlier are still
        // counted and this spins until they finish. Bounded: physics-thread callbacks are
        // single, bounded pieces of work (no blocking calls), so this is a short spin in
        // practice, never an unbounded wait.
        {
            int spinCount = 0;
            while (PhysicsInteraction::s_inFlightCallbacks.load(std::memory_order_acquire) > 0) {
                std::this_thread::yield();
                if (++spinCount % 100000 == 0) {
                    logger::warn("ROCK: destroyPhysicsInteraction still draining in-flight callbacks (spins={})", spinCount);
                }
            }
        }

        delete s_physicsInteraction;
        s_physicsInteraction = nullptr;

        logger::info("ROCK: PhysicsInteraction destroyed.");
    }

    void onFrameUpdate()
    {
        performance_profiler::refreshSettings(
            g_rockConfig.rockPerformanceProfilerEnabled,
            g_rockConfig.rockPerformanceProfilerLogIntervalFrames,
            g_rockConfig.rockPerformanceProfilerWarmupFrames,
            g_rockConfig.rockPerformanceProfilerOverlayText);
        performance_profiler::FrameScope profilerFrame;

        if (!s_pluginLoaded || !s_frikAvailable) {
            input_remap_runtime::setGameplayInputAllowed(false);
            input_remap_runtime::setWeaponDrawn(false);
            input_remap_runtime::setRightHandHeldWeapon(false);
            input_remap_runtime::setEquippedWeaponPrimaryDetachInputActive(false);
            input_remap_runtime::setEquippedWeaponPrimaryDetached(false);
            return;
        }

        g_rockConfig.processPendingConfigReload();
        input_remap_runtime::installInputRemapHooks();

        const bool menuInputActive = input_remap_runtime::isMenuInputActive();
        runtime_state::updateFrame(runtime_state::RuntimeFrameInput{
            .menuInputBlocking = menuInputActive,
            .visualAuthorityAvailable = frik_visual_authority::isAvailable(),
            .visualSkeletonReadyHint = frik_visual_authority::isSkeletonReadyHint(),
            .compatibilityConfigBlocking = frik_visual_authority::isCompatibilityConfigBlocking(),
        });
        const auto& runtime = runtime_state::currentFrame();
        const bool gameplayInputAllowed =
            g_rockConfig.rockEnabled &&
            runtime.localSkeletonReady &&
            !runtime.localMenuBlocking &&
            !runtime.compatibilityConfigBlocking;
        input_remap_runtime::setWeaponDrawn(runtime.weaponDrawn);
        input_remap_runtime::setGameplayInputAllowed(gameplayInputAllowed);
        debug_controller_runtime::update(gameplayInputAllowed, runtime.deltaSeconds);

        if (!g_rockConfig.rockEnabled) {
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
        // bUseRockEngineArchitecture=1 that sets ROCK.ini bEnabled=false at runtime (hot
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
        s_preAuthHandFresh = false;
        s_preAuthArmReachValid[0] = false;
        s_preAuthArmReachValid[1] = false;
    }

    using GameLoopFunc = void (*)(std::uint64_t rcx);
    GameLoopFunc s_originalGameLoopFunc = nullptr;

    // ROCK applies weapon visual/collision authority after the chained frame update
    // so FRIK finishes its skeleton and weapon pass before ROCK writes final state.
    void onGameFrameUpdateHook(const std::uint64_t rcx)
    {
        if (s_originalGameLoopFunc) {
            s_originalGameLoopFunc(rcx);
        }

        onFrameUpdate();
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
            logger::critical("ROCK: main loop hook site 0x{:X} prefix validation failed (expected 0xE8 call, found 0x{:02X}) - another mod or framework version likely changed this shared hook site; aborting ROCK init",
                hookCallSite.address(), siteBytes[0]);
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
            const auto providerGeneration = bumpGeneration(s_providerGeneration);
            if (s_physicsInteraction) {
                s_physicsInteraction->noteProviderLifecycle(
                    providerGeneration,
                    rock::provider::RockProviderLifecycleReason::GameLoaded);
            }

            // Jul 6: require only the v2 CORE (was FRIK_API_VERSION=5). The v5-only visual-authority
            // functions (applyExternalHandWorldTransform etc.) are routed to the host's plugin-side
            // HandAuthority when the loaded FRIK is < v5 (see frikHasVisualAuthority + the bridge), so
            // ROCK now RUNS on older FRIK instead of disabling entirely. CAUTION: on old FRIK the
            // v5-tail fn-ptrs are past the end of the returned struct (OOB) — NEVER call them; the
            // bridge gates on the latched version below, not a null-check.
            // Jul 7: require any FRIK API version (initialize(0)) — the v5-only visual-authority
            // functions are routed to the host's plugin-side HandAuthority when FRIK is < v5 (see
            // frikHasVisualAuthority + the bridge), so ROCK RUNS on ANY FRIK (even pre-v5 / no-API-
            // features) instead of disabling. On a too-old FRIK the v5-tail fn-ptrs are OOB — the
            // bridge gates on the latched version below (frikHasVisualAuthority), NOT a null-check, so
            // they are never called; only errors 1-3 (FRIK.dll genuinely absent) disable ROCK.
            const int frikErr = frik::api::FRIKApi::initialize(0);
            if (frikErr != 0) {
                // frikErr 1/2/3 = FRIK.dll not loaded / export missing / null api → the body itself
                // won't render; ROCK cannot run. (Version-too-old can't occur at min=0.)
                switch (frikErr) {
                case 1: logger::critical("ROCK: FRIKApi init FAILED (1). FRIK.dll not loaded. ROCK DISABLED."); break;
                case 2: logger::critical("ROCK: FRIKApi init FAILED (2). FRIKAPI_GetApi export missing. ROCK DISABLED."); break;
                case 3: logger::critical("ROCK: FRIKApi init FAILED (3). FRIKAPI_GetApi returned null. ROCK DISABLED."); break;
                default: logger::critical("ROCK: FRIKApi init FAILED ({}). ROCK DISABLED.", frikErr); break;
                }
                s_frikAvailable = false;
                return;
            }

            // Latch the loaded FRIK API version once (getVersion is member 0, always present).
            s_frikApiVersion = frik::api::FRIKApi::inst->getVersion();
            // SAFETY (Jul 7): if the API is < 5, NULL out inst. The FRIKApi struct is append-only, so
            // an older FRIK returns a SHORTER struct — dereferencing any v5-tail fn-ptr (finger poses,
            // hand-world) is an out-of-bounds read. With inst==nullptr every frik_visual_authority
            // bridge function is null-safe: the 3 hand-placement functions route to the host plugin-side
            // authority (which needs no FRIK API), and the finger-pose functions return false (default
            // curl). Physics/collision/two-handing/dynamic grab do NOT use the FRIK API and run fully.
            const bool nativeAuthority = (s_frikApiVersion >= 5);
            if (!nativeAuthority) {
                frik::api::FRIKApi::inst = nullptr;
            }
            logger::info("ROCK: FRIK API v{} initialized. Native visual-authority {} (host hand-authority shim {}).",
                         s_frikApiVersion,
                         nativeAuthority ? "AVAILABLE" : "ABSENT",
                         nativeAuthority ? "idle" : "ACTIVE");

            // Upstream Jul-6: canonical 22-float hand-pose contract check. EMBED ADAPTATION: only
            // enforce when the NATIVE v5 authority is in use — on older FRIK (inst nulled above) the
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
                    logger::critical(
                        "ROCK: FRIKApi v5 contract mismatch. Loaded FRIK.dll does not expose the canonical 22-float hand-pose contract required by this ROCK build. Deploy the matching rebuilt FRIK.dll. ROCK is now DISABLED.");
                    s_frikAvailable = false;
                    return;
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
            // "F4VRBody" message to rock::HostOnFRIKMessage() (see Heisenberg.cpp OnExternalPluginMessage).
            logger::info("ROCK: FRIK lifecycle events arrive via host forward (rock::HostOnFRIKMessage), sender '{}'.",
                frik::api::FRIKApi::FRIK_F4SE_MOD_NAME);

            logger::info("ROCK: Initialization complete. Waiting for skeleton...");
        }

        if (msg->type == F4SE::MessagingInterface::kPostLoadGame || msg->type == F4SE::MessagingInterface::kNewGame) {
            logger::info("ROCK: New game session -- resetting PhysicsInteraction...");
            const auto providerGeneration = bumpGeneration(s_providerGeneration);
            s_physicsCreationRequested.store(false, std::memory_order_release);
            s_physicsCreationReadyDeferralFrames.store(0, std::memory_order_release);
            resetPhysicsCreationGate();
            runtime_state::resetTransientState();
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

    // EMBEDDED-HOST SEAM (audit rank 2) — see ROCKMain.h. Routes the host's grab lifecycle into
    // PhysicsInteraction's request slots; the engine applies them inside update() with a valid
    // world (suppress on grab start, config-delayed collider restore on release).
    void HostNotifyExternalGrab(bool a_isLeft, bool a_active)
    {
        if (s_physicsInteraction && s_physicsInteraction->isInitialized()) {
            s_physicsInteraction->hostNotifyExternalGrab(a_isLeft, a_active);
        }
    }

    // Plugin-side hand-authority seam (see ROCKMain.h). The bridge uses these when FRIK lacks v5.
    void HostSetHandAuthority(const HostHandAuthority* a_cbs) { s_hostHandAuthority = a_cbs; }
    const HostHandAuthority* getHostHandAuthority() { return s_hostHandAuthority; }
    bool frikHasVisualAuthority() { return s_frikApiVersion >= 5; }

    // ── EMBEDDED HOST ENTRY ──────────────────────────────────────────────────
    // Heisenberg owns the F4SE plugin entry point (F4SEPlugin_Query/Load) and calls this after
    // its own init, gated on the bUseRockEngineArchitecture toggle. Same setup as the standalone
    // F4SEPlugin_Load MINUS F4SE::Init (Heisenberg already called it). The original
    // F4SEPlugin_Query/Load exports are removed so they don't collide with Heisenberg's exports.
    bool HostLoad(const F4SE::LoadInterface* /*a_f4se*/)
    {
        // CRITICAL: every ROCK engine TU does `using namespace f4cf`, so all ROCK `logger::*`
        // calls route through f4cf::logger::internal::_logger — a shared_ptr that is NULL until
        // f4cf::logger::init() runs. Heisenberg sets up its OWN spdlog logger via F4SE::Init and
        // never inits the framework logger, so the standalone ROCK's `logger::init("ROCK")` (which
        // lived in the F4SEPlugin_Query we deleted) is still required here — otherwise the very
        // first ROCK logger::info() below dereferences a null logger and crashes the game.
        // logger::init() also calls spdlog::set_default_logger(); capture + restore Heisenberg's
        // default so its own spdlog::info() keeps writing to HeisenbergF4VR.log (ROCK logs to
        // ROCK.log via its own internal::_logger, independent of the default logger).
        {
            auto prevDefault = spdlog::default_logger();
            logger::init("ROCK");
            // DIAGNOSTIC: right after init, the ROCK logger is the default. Force flush-on-every-
            // line so a hard CTD (which bypasses the crash logger) can't swallow the tail — the
            // last ROCK.log line then reliably marks the crash point. ROCK's own internal::_logger
            // is this same object, so the policy persists after we restore Heisenberg's default.
            if (auto rockLogger = spdlog::default_logger()) {
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
            if (prevDefault && !prevDefault->sinks().empty() && logger::internal::_logger) {
                auto merged = std::make_shared<spdlog::logger>(
                    "ROCK", prevDefault->sinks().begin(), prevDefault->sinks().end());
                merged->set_level(logger::internal::_logger->level());
                merged->flush_on(spdlog::level::warn);
                logger::internal::_logger = merged;
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
                // genuine user sLogPattern= override in ROCK.ini still differs from this and
                // still applies normally.
                logger::internal::_logPattern = logging_policy::DefaultLogPattern;
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
        // Upstream Jul-5 (grenade-grab): loose grenade equip hook — appends to the shared trampoline.
        // EMBED (Jul 10): NON-FATAL. The hook's byte-prefix validation fails when another mod
        // (e.g. VirtualReloads) already patched ActorEquipManager::EquipObject — observed live:
        // "native bytes changed, hook not installed". Standalone ROCK aborts load; in the embed
        // that killed the ENTIRE engine (HostLoad FAILED → fallback physics). Degrade instead:
        // grenade-grab interception stays off, everything else runs.
        if (!rock::loose_grenade_runtime::installEquipHook()) {
            ROCK_LOG_WARN(Init, "Loose-grenade equip hook not installed (prefix validation failed — another mod likely hooks EquipObject); continuing WITHOUT grenade-grab interception");
        }
        if (!hookMainLoop()) {
            return false;
        }
        // See-Through-Scopes late-culling hook: ONLY install when the STS mod is actually present.
        // It is a RENDER-path hook (late culling pass) whose sole purpose is scope-rendering compat;
        // ROCK's own STS logic no-ops without FO4VR_better_scopes.dll. Installing the branch anyway
        // plants an extra hook in the main render/update dispatch that overlaps other render mods
        // (e.g. Inventory3DFix) for zero benefit — observed as a CTD on save-load in the embed.
        // Mirroring ROCK's own detection at the hook-install level keeps behavior identical when STS
        // IS present and removes all render-path risk when it isn't.
        if (f4cf::common::isDLLModLoaded("FO4VR_better_scopes.dll")) {
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

    void HostSetGrabOwnership(bool a_rockOwnsGrab)
    {
        g_rockConfig.rockHostGrabOwnershipForced = a_rockOwnsGrab;
        if (a_rockOwnsGrab) {
            g_rockConfig.rockGrabEnabled = true;
            g_rockConfig.rockSelectionEnabled = true;
        }
        logger::info("ROCK(host): grab+selection ownership {} (iGrabMode=9 host seam)",
            a_rockOwnsGrab ? "ceded to embedded ROCK" : "retained by host");
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

    void HostSetHandColliderRadiusPadding(float a_padding)
    {
        if (!std::isfinite(a_padding)) {
            return;
        }
        const float clamped = std::clamp(a_padding, 0.0f, 1.0f);
        if (g_rockConfig.rockHandBoneColliderRadiusPadding != clamped) {
            logger::info("ROCK(host): hand collider radius padding {} -> {} via host API (colliders will rebuild)",
                g_rockConfig.rockHandBoneColliderRadiusPadding, clamped);
        }
        g_rockConfig.rockHandBoneColliderRadiusPadding = clamped;
    }

    bool HostIsWeaponSupportEngaged(bool a_isLeft)
    {
        return s_physicsInteraction && s_physicsInteraction->hostIsWeaponSupportEngaged(a_isLeft);
    }

    bool HostIsWeaponSupportGripped(bool a_isLeft)
    {
        return s_physicsInteraction && s_physicsInteraction->hostIsWeaponSupportGripped(a_isLeft);
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
        s_preAuthHandFresh = true;
        // Reach is paired with this exact clean-hand publication. Invalidate the old
        // pair first; FrikArmGoalHook fills whichever current arms resolve safely.
        s_preAuthArmReachValid[0] = false;
        s_preAuthArmReachValid[1] = false;
    }

    bool HostGetPreAuthorityHandWorld(bool a_isLeft, RE::NiTransform& a_out)
    {
        if (!s_preAuthHandFresh) {
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
        if (!s_preAuthHandFresh || !finite || a_maxReach <= 1.0f || a_maxReach >= 200.0f) {
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
        if (!s_preAuthHandFresh || !s_preAuthArmReachValid[i]) {
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
