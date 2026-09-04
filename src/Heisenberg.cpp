#include "Heisenberg.h"
#include "PostConsumeActivationPolicy.h"
#include "HandAuthority.h"
#include "FrikArmGoalHook.h"
#include "LegacyFrikFingerPoseAuthority.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <SimpleIni.h>

#include "ActivatorHandler.h"
#include "Config.h"
#include "CookingHandler.h"
#include "DropToHand.h"
#include "F4VROffsets.h"
#include "FRIKInterface.h"
#include "Grab.h"
#include "GrabOwnershipPolicy.h"
#include "NpcInjectionPolicy.h"
#include "Hand.h"
#include "HeisenbergInterface001.h"
#include "Highlight.h"
#include "InputRecovery.h"
#include "HandCollision.h"
#include "FingerCurves.h"
#include "WandNodeHelper.h"
#include "HandBumpHook.h"
#include "HavokTimingFix.h"
#include "Hooks.h"
#include "ItemInsertHandler.h"
#include "ItemOffsets.h"
#include "ItemPositionConfigMode.h"
#include "IntroCeremonyState.h"
#include "MenuChecker.h"
#include "NodeCaptureMode.h"
#include "OpenVRHook.h"
#include "PickpocketHandler.h"
#include "PipboyInteraction.h"
#include "WaterInteraction.h"
#include "PlayerCharacterProxyListener.h"
#include "rock/RockBridge.h"
#include "../external/ROCK/src/ROCKMain.h"  // rock::HostLoad — embedded-engine host entry (lightweight fwd-decl header only)
#include "../external/ROCK/src/physics-interaction/native/HavokOffsets.h"
#include "../external/ROCK/src/physics-interaction/weapon/WeaponEquipTransfer.h"
#include "SmartGrabHandler.h"
#include "ThrownObjectTracker.h"
#include "Utils.h"
#include "VRInput.h"
#include "WandNodeHelper.h"

#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"

namespace heisenberg
{
    // True once the embedded ROCK engine has been hosted (bUseRockEngineArchitecture=1 + HostLoad OK).
    // Gates whether OnF4SEMessage forwards F4SE messages into ROCK: ROCK cannot register its own
    // F4SE-core listener in this single DLL (F4SE keeps Heisenberg's first-registered one), so ROCK
    // only receives kGameLoaded/kPostLoadGame/kNewGame via that forward. Must stay false when the
    // toggle is off, or forwarding would run ROCK's FRIK/physics init despite the engine being dormant.
    static bool s_rockEngineHosted = false;

    struct RockNpcInjectionDiagnostics
    {
        std::uint64_t grabTraceId = 0;
        NpcInjectionGate lastGate = NpcInjectionGate::FeatureDisabled;
        double nextGateLogTime = 0.0;
        double nextDispatchRetryTime = 0.0;
        bool dispatchRetryThrottled = false;
        bool identityLogged = false;
    };
    static std::array<npc_injection_policy::HostedDispatchGuard, 2>
        s_rockNpcInjectionGuards{};
    static std::array<RockNpcInjectionDiagnostics, 2>
        s_rockNpcInjectionDiagnostics{};

    static bool BlockRockPlayerConsume(RE::TESObjectREFR* ref)
    {
        // Exact same identity resolver as the NPC dispatch path. ROCK asks
        // before it creates a mouth candidate, so a cure remains physically
        // held instead of being detached/picked up and only then blocked by
        // ActorEquipManager's final PlayerRef-use guard.
        return IsDiseaseCureItem(ref);
    }

    static bool ResolveRockPlayerConsumeProfile(
        RE::TESObjectREFR* ref,
        bool isLeft,
        rock::HostPlayerConsumeProfile& outProfile)
    {
        // A registered host resolver owns the complete delivery policy.  Start
        // fail-closed, then open exactly one spatial route for an eligible
        // player consumable.
        outProfile = {};
        if (!ref) {
            return false;
        }

        if (IsDiseaseCureItem(ref) ||
            (g_config.blockConsumptionInPA &&
             Utils::IsPlayerInPowerArmor())) {
            return true;
        }

        // Companion contact has priority over both mouth and opposite-wrist
        // self-use.  Require ROCK's exact current held-node snapshot: the hand
        // or permanent wand being near the actor is not sufficient evidence.
        if (g_config.enableCompanionStimpakInjection &&
            IsCompanionStimpakItem(ref)) {
            rock::HostHeldObjectSnapshot snapshot{};
            if (rock::HostGetHeldObjectSnapshot(isLeft, snapshot) &&
                snapshot.ref == ref && snapshot.heldNode &&
                HasHeldCompanionStimpakTarget(
                    isLeft,
                    ref,
                    snapshot.heldNode)) {
                return true;
            }
        }

        if (!IsPlayerConsumableItem(ref)) {
            return true;
        }

        if (IsPlayerInjectableItem(ref) &&
            g_config.enableHandInjection) {
            outProfile.route =
                rock::HostPlayerConsumeRoute::OppositeWrist;
            outProfile.autoConsumeWhileHeld =
                g_config.autoConsumeInWristArea;
            outProfile.zoneOffsetXGameUnits =
                g_config.handInjectionOffsetX;
            outProfile.zoneOffsetYGameUnits =
                g_config.handInjectionOffsetY;
            outProfile.zoneOffsetZGameUnits =
                g_config.handInjectionOffsetZ;
            outProfile.zoneRadiusGameUnits =
                g_config.handInjectionRadius;
            return true;
        }

        if (g_config.consumableActivationZone != 0) {
            outProfile.route = rock::HostPlayerConsumeRoute::Mouth;
            outProfile.autoConsumeWhileHeld =
                g_config.autoConsumeInMouthArea;
            outProfile.zoneOffsetXGameUnits = g_config.mouthOffsetX;
            outProfile.zoneOffsetYGameUnits = g_config.mouthOffsetY;
            outProfile.zoneOffsetZGameUnits = g_config.mouthOffsetZ;
            outProfile.zoneRadiusGameUnits = g_config.mouthRadius;
        }
        return true;
    }

    static void ServiceRockHeldNpcInjection()
    {
        const double now = Utils::GetTime();
        for (std::size_t handIndex = 0; handIndex < 2; ++handIndex) {
            const bool isLeft = handIndex == 1;
            rock::HostHeldObjectSnapshot snapshot{};
            if (!rock::HostGetHeldObjectSnapshot(isLeft, snapshot)) {
                s_rockNpcInjectionDiagnostics[handIndex] = {};
                continue;
            }
            auto& diagnostics =
                s_rockNpcInjectionDiagnostics[handIndex];
            if (diagnostics.grabTraceId != snapshot.grabTraceId) {
                diagnostics = {};
                diagnostics.grabTraceId = snapshot.grabTraceId;
            }
            const bool isDiseaseCure =
                IsDiseaseCureItem(snapshot.ref);
            const bool isCompanionStimpak =
                g_config.enableCompanionStimpakInjection &&
                IsCompanionStimpakItem(snapshot.ref);
            if (!diagnostics.identityLogged) {
                auto* baseForm = snapshot.ref ?
                    snapshot.ref->GetObjectReference() :
                    nullptr;
                const char* editorID = baseForm ?
                    baseForm->GetFormEditorID() :
                    nullptr;
                spdlog::info(
                    "[INJECT-NPC][ROCK] Held-form identity checked: ref={:08X} base={:08X} type={} editor='{}' diseaseCure={} companionStimpak={} hand={} trace={}",
                    snapshot.ref ? snapshot.ref->GetFormID() : 0u,
                    baseForm ? baseForm->formID : 0u,
                    baseForm ?
                        static_cast<std::uint32_t>(
                            baseForm->GetFormType()) :
                        0u,
                    editorID ? editorID : "(none)",
                    isDiseaseCure ? "yes" : "no",
                    isCompanionStimpak ? "yes" : "no",
                    isLeft ? "left" : "right",
                    snapshot.grabTraceId);
                diagnostics.identityLogged = true;
            }
            if (!isDiseaseCure && !isCompanionStimpak) {
                continue;
            }
            const RE::ObjectRefHandle heldRefHandle =
                snapshot.ref->GetHandle();
            RE::NiPointer<RE::TESObjectREFR> heldRefKeepAlive =
                heldRefHandle.get();
            if (heldRefKeepAlive.get() != snapshot.ref) {
                spdlog::warn(
                    "[INJECT-NPC][ROCK] Held medicine snapshot could not be retained for the pre-commit ownership handoff (hand={} trace={})",
                    isLeft ? "left" : "right",
                    snapshot.grabTraceId);
                continue;
            }

            auto& dispatchGuard = s_rockNpcInjectionGuards[handIndex];
            if (!npc_injection_policy::AllowsHostedDispatch(
                    dispatchGuard,
                    snapshot.grabTraceId)) {
                continue;
            }

            if (diagnostics.dispatchRetryThrottled &&
                now < diagnostics.nextDispatchRetryTime) {
                continue;
            }
            diagnostics.dispatchRetryThrottled = false;

            bool backendHandoffAttempted = false;
            bool backendHandoffSucceeded = false;
            const auto prepareInventoryCommit = [&]() {
                // The selected treatment invokes this only after its actor/
                // runtime preflight has succeeded and immediately before
                // inventory mutation. Release while the ref/body is still
                // live; the ROCK seam also releases a peer co-hold here.
                if (backendHandoffAttempted) {
                    return false;
                }
                backendHandoffAttempted = true;
                backendHandoffSucceeded =
                    rock::HostReleaseHeldObjectForInventory(
                        isLeft,
                        heldRefKeepAlive.get(),
                        snapshot.grabTraceId);
                if (!backendHandoffSucceeded) {
                    spdlog::error(
                        "[INJECT-NPC][ROCK] Exact pre-commit release failed (hand={} ref={:08X} trace={}); inventory/treatment dispatch will be aborted",
                        isLeft ? "left" : "right",
                        heldRefKeepAlive->GetFormID(),
                        snapshot.grabTraceId);
                    return false;
                }
                npc_injection_policy::MarkHostedDispatchCommitted(
                    dispatchGuard,
                    snapshot.grabTraceId);
                return true;
            };

            // If an SS2 installation intentionally aliases DiseaseCureForm to
            // a Stimpak, companion contact wins; otherwise preserve the SS2
            // route. An ordinary Stimpak with no contact still runs the
            // companion attempt for useful gate diagnostics, but never commits.
            const bool dispatchCompanionStimpak =
                isCompanionStimpak &&
                (!isDiseaseCure ||
                 HasHeldCompanionStimpakTarget(
                     isLeft,
                     heldRefKeepAlive.get(),
                     snapshot.heldNode));
            const NpcInjectionAttempt attempt =
                dispatchCompanionStimpak ?
                    TryInjectHeldCompanionStimpak(
                        isLeft,
                        heldRefKeepAlive.get(),
                        snapshot.heldNode,
                        snapshot.heldSeconds,
                        snapshot.handSpeedMetersPerSecond,
                        prepareInventoryCommit) :
                    TryInjectHeldDiseaseCure(
                        isLeft,
                        heldRefKeepAlive.get(),
                        snapshot.heldNode,
                        snapshot.heldSeconds,
                        snapshot.handSpeedMetersPerSecond,
                        prepareInventoryCommit);
            const char* treatmentKind =
                dispatchCompanionStimpak ?
                    "Companion Stimpak" :
                    "Disease Cure";

            const bool diagnosticGate =
                attempt.gate == NpcInjectionGate::NoTargetContact ||
                attempt.gate == NpcInjectionGate::HoldAge ||
                attempt.gate == NpcInjectionGate::HandSpeed;
            if (diagnosticGate &&
                (attempt.gate != diagnostics.lastGate ||
                    now >= diagnostics.nextGateLogTime)) {
                diagnostics.lastGate = attempt.gate;
                diagnostics.nextGateLogTime = now + 2.0;
                if (attempt.gate == NpcInjectionGate::NoTargetContact) {
                    spdlog::warn(
                        "[INJECT-NPC][ROCK] {} {:08X} recognized in {} hand, but no eligible target body contact was inside {:.1f}gu (trace={})",
                        treatmentKind,
                        snapshot.ref->GetFormID(),
                        isLeft ? "left" : "right",
                        g_config.npcInjectionRadius,
                        snapshot.grabTraceId);
                } else if (attempt.gate == NpcInjectionGate::HoldAge) {
                    spdlog::warn(
                        "[INJECT-NPC][ROCK] {} target {:08X} found at {:.1f}gu, but hold age {:.2f}s is below 0.50s (hand={} trace={})",
                        treatmentKind,
                        attempt.targetFormID,
                        attempt.targetDistanceGameUnits,
                        snapshot.heldSeconds,
                        isLeft ? "left" : "right",
                        snapshot.grabTraceId);
                } else {
                    spdlog::warn(
                        "[INJECT-NPC][ROCK] {} target {:08X} found at {:.1f}gu, but hand speed {:.2f}m/s is at/above {:.2f}m/s (hand={} trace={})",
                        treatmentKind,
                        attempt.targetFormID,
                        attempt.targetDistanceGameUnits,
                        snapshot.handSpeedMetersPerSecond,
                        g_config.mouthVelocityThreshold,
                        isLeft ? "left" : "right",
                        snapshot.grabTraceId);
                }
            }

            if (attempt.result == NpcInjectionResult::NotAttempted) {
                continue;
            }
            if (attempt.result == NpcInjectionResult::FailedKeptInHand) {
                if (backendHandoffSucceeded) {
                    // Ownership has already left ROCK and the trace is terminal;
                    // do not describe or schedule this as a held-item retry.
                    spdlog::error(
                        "[INJECT-NPC][ROCK] Backend handoff succeeded but {} inventory/dispatch then failed for target {:08X}; trace={} remains latched",
                        treatmentKind,
                        attempt.targetFormID,
                        snapshot.grabTraceId);
                    g_vrInput.TriggerHaptic(isLeft, 500);
                    continue;
                }
                // SS2 can be temporarily unavailable while its VM finishes
                // binding after load, and a native target can change between
                // contact and transaction revalidation. Permit retry, but
                // never at frame rate.
                diagnostics.dispatchRetryThrottled = true;
                diagnostics.nextDispatchRetryTime = now + 1.0;
                g_vrInput.TriggerHaptic(isLeft, 500);
                continue;
            }

            if (!backendHandoffAttempted || !backendHandoffSucceeded) {
                spdlog::error(
                    "[INJECT-NPC][ROCK] {} reported an inventory-committed result for target {:08X} without a successful exact ROCK handoff (hand={} trace={})",
                    treatmentKind,
                    attempt.targetFormID,
                    isLeft ? "left" : "right",
                    snapshot.grabTraceId);
            }
            g_vrInput.TriggerHaptic(
                isLeft,
                attempt.result == NpcInjectionResult::Accepted ?
                    2000 :
                    500);
        }
    }

    // One post-FRIK tail owns all hosted scene writes. Whole-hand authority
    // lands first; finger locals then inherit that final wrist/weapon world.
    static void ApplyRockHostVisualAuthorities()
    {
        static bool benchmarkVisualAuthoritiesReset = false;
        if (rock::HostIsPerformanceBenchmarkBaseline()) {
            if (!benchmarkVisualAuthoritiesReset) {
                HandAuthority::Reset();
                LegacyFrikFingerPoseAuthority::Reset();
                benchmarkVisualAuthoritiesReset = true;
            }
            return;
        }
        benchmarkVisualAuthoritiesReset = false;
        HandAuthority::ApplyWinners();
        LegacyFrikFingerPoseAuthority::ApplyWinners();
        ServiceRockHeldNpcInjection();
    }

    // Public accessor (audit rank 10): ownership gates elsewhere (e.g. Hooks.cpp
    // UpdateHandCollisionBodies) must treat the EMBEDDED engine like a running ROCK.dll.
    // RockBridge::IsRunning() binds via GetModuleHandleA("ROCK.dll") and is structurally
    // always false in the embed, so gates keyed only on it never yield to the hosted engine.
    bool IsRockEngineHosted() { return s_rockEngineHosted; }

    // ---- Scope-mode exit/re-entry policy for support grips ----
    // Covers BOTH scope systems:
    //  - BetterScopesVR zoom: state tracked from its msg-15 broadcast; exit = msg-16 toggle
    //    (only sent while its own state says zoomed, so it can never toggle zoom ON).
    //  - Native FO4VR world scope: exit through its verified native state
    //    transition. The ROCK host hook then consumes repeated inside-cone
    //    activation until the optic leaves the cone once.
    //  - Vanilla ScopeMenu: also receives an idempotent UI kHide fallback.
    static bool s_lookingThroughScope = false;

    static bool HostObservedScopePresentation()
    {
        return s_lookingThroughScope ||
               MenuChecker::GetSingleton().IsScopeOpen();
    }

    static void ExitScopePresentation(const char* reason)
    {
        if (s_lookingThroughScope) {
            s_lookingThroughScope = false;  // optimistic; its next msg 15 re-syncs
            if (const auto* messaging = F4SE::GetMessagingInterface()) {
                messaging->Dispatch(16, nullptr, 0, "FO4VRBETTERSCOPES");
                spdlog::info(
                    "[SCOPE] {} while zoomed — sent scope-mode exit "
                    "toggle to BetterScopesVR",
                    reason);
            }
        }

        // Raw-disassembly-verified FO4VR ABI: (PlayerCharacter*, bool).
        // Calling the false transition is idempotent when no native scope is
        // active and closes the WSScope presentation even though the game
        // never reported an exact "ScopeMenu" open event in the failing logs.
        using NativeScopeStateTransition =
            void (*)(RE::PlayerCharacter*, bool);
        static REL::Relocation<NativeScopeStateTransition>
            nativeScopeStateTransition{
                REL::Offset(
                    rock::offsets::
                        kFunc_NativeScopeStateTransition) };
        if (auto* player =
                RE::PlayerCharacter::GetSingleton();
            player &&
            nativeScopeStateTransition.address()) {
            nativeScopeStateTransition(player, false);
            spdlog::info(
                "[SCOPE] {} — requested native FO4VR scope state "
                "exit",
                reason);
        }

        // Do not gate this on MenuChecker::IsScopeOpen(): Fallout 4 VR can
        // keep WSScope active without ever publishing an exact ScopeMenu event.
        // kHide on a closed/unregistered menu is harmless.
        if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
            static RE::BSFixedString scopeMenuName("ScopeMenu");
            msgQueue->AddMessage(
                scopeMenuName,
                RE::UI_MESSAGE_TYPE::kHide);
            spdlog::info(
                "[SCOPE] {} — sent idempotent vanilla ScopeMenu "
                "kHide",
                reason);
        }
    }

    void SetLookingThroughScope(bool a_looking)
    {
        if (s_lookingThroughScope != a_looking) {
            spdlog::debug(
                "[SCOPE] BetterScopesVR looking-through-scope = {}",
                a_looking);
        }
        s_lookingThroughScope = a_looking;

        // BetterScopes can publish a new zoom-on state while Bethesda's
        // ocular is still inside the activation cone. Keep the presentation
        // closed for the same leave-then-re-enter episode as native WSScope.
        if (a_looking &&
            IsRockEngineHosted() &&
            rock::HostIsNativeScopeReentryBlocked()) {
            ExitScopePresentation(
                "scope re-entry remained blocked");
        }
    }

    void ExitScopeModeOnGripRelease()
    {
        if (IsRockEngineHosted()) {
            // Preserve the existing release exit, and give it the same
            // anti-reopen behavior when release itself occurs while scoped.
            (void)rock::HostArmNativeScopeReentryBlockIfActive(
                HostObservedScopePresentation());
        }
        ExitScopePresentation(
            "support grip released");
    }

    // Destructor - defined here where Hand is complete
    Heisenberg::~Heisenberg() = default;

    // SEH-safe helper for writing to displacement flags (no C++ objects allowed)
    static void ForceDisplacementFlags_SEH(uintptr_t settingsAddr, uintptr_t masterAddr)
    {
        __try
        {
            auto* pSettings = reinterpret_cast<uint8_t*>(settingsAddr);
            auto* pMaster = reinterpret_cast<uint8_t*>(masterAddr);
            if (*pSettings == 0) *pSettings = 1;
            if (*pMaster == 0) *pMaster = 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Force water displacement settings flag ON before cells load.
    // F4VR defaults bUseWaterDisplacements to 0 (no [Water] section in default INI).
    // Without this, water planes lack displacement geometry and the AddRipple
    // ring buffer data has no render target to display on.
    static void ForceWaterDisplacementEnabled()
    {
        static REL::Relocation<uint8_t*> settingsFlag{ REL::Offset(0x3772c80) };
        static REL::Relocation<uint8_t*> masterEnable{ REL::Offset(0x37729c8) };
        ForceDisplacementFlags_SEH(
            reinterpret_cast<uintptr_t>(settingsFlag.get()),
            reinterpret_cast<uintptr_t>(masterEnable.get()));
    }

    // F4SE message handler
    // Messages dispatched to us by OTHER plugins. These do NOT arrive on the
    // "F4SE" listener below — F4SE routes a Dispatch(receiver) only to listeners
    // the receiver registered with sender == the *dispatching* plugin. So we
    // register this handler with a null sender (all plugins) separately.
    static void OnExternalPluginMessage(F4SE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }

        // FRIK ("F4VRBody") lifecycle events (kSkeletonReady / kSkeletonDestroying / ...) must reach the
        // embedded ROCK engine so it builds+destroys its hand/weapon colliders. ROCK cannot own a
        // {plugin,"F4VRBody"} listener slot in this shared DLL (F4SE keeps our null-sender listener,
        // registered first), so — exactly like the F4SE-core forward in OnF4SEMessage — we forward every
        // FRIK message here. Without this the engine stays parked at "waiting for skeleton" and no ROCK
        // hand/weapon collision ever occurs. Gated on s_rockEngineHosted so a dormant engine is untouched.
        // "F4VRBody" is FRIK's F4SE plugin name (frik::api::FRIKApi::FRIK_F4SE_MOD_NAME).
        // BetterScopesVR ("FO4VRBETTERSCOPES") broadcasts msg 15 with looking-through-scope
        // state (data pointer used as the bool, mirroring FRIK's reading of the protocol).
        // Tracked so releasing the two-handed support grip can exit scope mode (msg 16 toggle).
        if (a_msg->sender && std::strcmp(a_msg->sender, "FO4VRBETTERSCOPES") == 0) {
            if (a_msg->type == 15) {
                heisenberg::SetLookingThroughScope(static_cast<bool>(a_msg->data));
            }
            return;
        }

        if (s_rockEngineHosted && a_msg->sender && std::strcmp(a_msg->sender, "F4VRBody") == 0) {
            using FrikLifecycle =
                frik::host_api::FRIKApi::LifecycleEvent;
            const auto lifecycle =
                static_cast<FrikLifecycle>(a_msg->type);
            if (lifecycle == FrikLifecycle::kSkeletonReady ||
                lifecycle == FrikLifecycle::kSkeletonDestroying ||
                lifecycle == FrikLifecycle::kPowerArmorChanged) {
                // A rich local pose contains skeleton- and PA-specific authored
                // translations. Drop host publications at every FRIK lifecycle
                // boundary so ROCK's active-tag cache must rebuild them.
                heisenberg::LegacyFrikFingerPoseAuthority::Reset();
            }

            // Host-adapter isolation (integration audit §3.1 / C1): a fault inside the embedded
            // engine must never std::terminate Heisenberg's own message handling.
            try {
                rock::HostOnFRIKMessage(a_msg);
            } catch (const std::exception& e) {
                spdlog::error("[Heisenberg] embedded ROCK threw in HostOnFRIKMessage (type={}): {}", a_msg->type, e.what());
            } catch (...) {
                spdlog::error("[Heisenberg] embedded ROCK threw (non-std) in HostOnFRIKMessage (type={})", a_msg->type);
            }
            return;
        }

        // ROCK physics provider messages (touch/grab/release/lifecycle). Routed to
        // the RockBridge; only acted on when ROCK integration is active.
        if (a_msg->sender && std::strcmp(a_msg->sender, rock::api::ROCKApi::ROCK_F4SE_MOD_NAME) == 0) {
            RockBridge::GetSingleton().OnRockMessage(a_msg->type, a_msg->data, a_msg->dataLen);
            return;
        }

        // Public API: another plugin requests the IHeisenbergInterface001 vtable.
        if (a_msg->type == HeisenbergPluginAPI::HeisenbergMessage::kMessage_GetInterface)
        {
            constexpr auto kLegacyMessageSize =
                offsetof(HeisenbergPluginAPI::HeisenbergMessage, structSize);
            if (!a_msg->data || a_msg->dataLen < kLegacyMessageSize) {
                spdlog::warn(
                    "[Heisenberg] Rejected malformed API request from '{}' (data={}, dataLen={})",
                    a_msg->sender ? a_msg->sender : "?",
                    a_msg->data != nullptr,
                    a_msg->dataLen);
                return;
            }
            spdlog::info("[Heisenberg] API interface request from '{}'",
                a_msg->sender ? a_msg->sender : "?");
            HeisenbergPluginAPI::HandleInterfaceRequest(
                static_cast<HeisenbergPluginAPI::HeisenbergMessage*>(a_msg->data),
                a_msg->dataLen);
            return;
        }

        // Public API: suppress item-to-hand routing for a window so a scripted
        // bulk transfer lands directly in inventory. Optional uint32 ms payload.
        if (a_msg->type == HeisenbergPluginAPI::kMessage_SuppressItemToHand)
        {
            std::uint64_t windowMs = 1500;
            if (a_msg->data && a_msg->dataLen >= sizeof(std::uint32_t)) {
                std::uint32_t v = *static_cast<std::uint32_t*>(a_msg->data);
                if (v != 0) windowMs = v;
            }
            DropToHand::SuppressItemToHand(windowMs);
            spdlog::info("[Heisenberg] Item-to-hand suppressed for {}ms (from '{}')",
                windowMs, a_msg->sender ? a_msg->sender : "?");
            return;
        }

        // A throwable was unholstered from Virtual Holsters — arm it immediately
        // instead of waiting out the hold-to-arm delay.
        if (a_msg->type == HeisenbergPluginAPI::kMessage_ArmThrowable)
        {
            // Hold the synthetic grip for a handful of frames so the throwable arm
            // reliably triggers right after VH equips it.
            g_heisenberg.ForceArmThrowable();
            spdlog::info("[Heisenberg] ArmThrowable signal from '{}' — readying throwable immediately",
                a_msg->sender ? a_msg->sender : "?");
            return;
        }
    }

    static void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }

        // Forward every F4SE-core message into the embedded ROCK engine. ROCK can't register its own
        // {plugin,"F4SE"} listener (this DLL already has ours), so this is the ONLY way it receives
        // kGameLoaded -> FRIKApi::initialize -> PhysicsInteraction. Gated on s_rockEngineHosted so a
        // dormant engine (toggle off / HostLoad failed) is never driven.
        if (s_rockEngineHosted) {
            // Host-adapter isolation (integration audit §3.1 / C1): swallow+log any engine fault
            // so it can't std::terminate the whole DLL.
            try {
                rock::HostOnF4SEMessage(a_msg);
            } catch (const std::exception& e) {
                spdlog::error("[Heisenberg] embedded ROCK threw in HostOnF4SEMessage (type={}): {}", a_msg->type, e.what());
            } catch (...) {
                spdlog::error("[Heisenberg] embedded ROCK threw (non-std) in HostOnF4SEMessage (type={})", a_msg->type);
            }
        }

        switch (a_msg->type) {
        case F4SE::MessagingInterface::kPostLoad:
            // All F4SE plugins (incl. ROCK) have loaded — safe to bind ROCK's API.
            heisenberg::RockBridge::GetSingleton().Init();
            break;
        case F4SE::MessagingInterface::kGameLoaded:
            spdlog::info("Game loaded, initializing Heisenberg...");
            ForceWaterDisplacementEnabled();
            spdlog::info("[Water] Forced displacement settings at kGameLoaded");
            g_heisenberg.OnGameLoad();
            // FRIK-GOAL seam (v5-equivalent hand authority): install AFTER FRIK is loaded
            // and initialized. Stands down automatically if the loaded FRIK has API v5+.
            if (heisenberg::FRIKInterface::GetSingleton().GetApiVersion() >= 5) {
                spdlog::info("[FRIK-GOAL] loaded FRIK reports API v5+ — native authority path, seam not needed");
            } else {
                heisenberg::FrikArmGoalHook::Install();
            }
            break;
        case F4SE::MessagingInterface::kPreSaveGame:
            spdlog::info("[SAVE] kPreSaveGame - force-releasing all grabbed objects");
            // CRITICAL: Properly release grabs BEFORE saving!
            // Without this, objects are saved at the hand position with KEYFRAMED physics.
            // On load they float in the sky because the grab state is cleared but the
            // object retains its hand-held position with frozen physics.
            heisenberg::GrabManager::GetSingleton().ForceReleaseAll();
            break;
        case F4SE::MessagingInterface::kPreLoadGame:
            ForceWaterDisplacementEnabled();
            spdlog::info("[Water] Forced displacement settings at kPreLoadGame");
            // Clear the previous save's ceremony bit before F4SE attempts to load the next
            // co-save. Legacy saves may have no .f4se file at all, in which case F4SE never
            // calls our load callback and this explicit reset is the only safe boundary.
            heisenberg::IntroCeremonyState::PrepareForLoad();
            // Disable all "to hand" interception immediately — engine transfers during
            // load must not be intercepted (survival effects, perk items, etc.)
            heisenberg::DropToHand::SetSessionNotReady();
            // Reset finger calibration — skeleton is rebuilt on load
            heisenberg::ResetFingerCalibration();
            // CRITICAL: Force-release grabs before load to restore physics state.
            // ClearAllState alone doesn't restore KEYFRAMED→DYNAMIC or collision layers.
            heisenberg::GrabManager::GetSingleton().ForceReleaseAll();
            // v0.5.4 behavior: just log, no loading state
            // Highlighter disabled - no need to clear
            // heisenberg::Highlighter::GetSingleton().ClearAllHighlights();
            // Clear cached menu state on load
            heisenberg::MenuChecker::GetSingleton().ClearState();
            // Clear activator tracking - refs and cell pointer become invalid
            heisenberg::ActivatorHandler::GetSingleton().ClearState();
            // Clear node capture mode
            heisenberg::NodeCaptureMode::GetSingleton().ClearState();
            // Destroy hand collision bodies - world/body pointers become invalid on load
            heisenberg::HandCollision::GetSingleton().Shutdown();
            heisenberg::HandAuthority::Reset();
            heisenberg::LegacyFrikFingerPoseAuthority::Reset();
            // Clear thrown-object tracking - tracked bodies, pending impacts, and the cached
            // hknpWorld pointer all become stale across a load (UAF on first post-load throw).
            heisenberg::ThrownObjectTracker::GetSingleton().Reset();
            heisenberg::ThrownObjectTracker::GetSingleton().SetWorld(nullptr);
            // Clear pending drops/loots - form IDs become invalid
            heisenberg::DropToHand::GetSingleton().ClearState();
            // Clear cooking handler state - refs become invalid
            heisenberg::CookingHandler::GetSingleton().ClearState();
            // Clear smart grab state - inventory state becomes invalid
            heisenberg::SmartGrabHandler::GetSingleton().ClearState();
            // Reset pipboy tape deck state on load — prevents stale open/closed transforms
            heisenberg::PipboyInteraction::GetSingleton().ClearState();
            // Clear pickpocket state - NPC handles become invalid
            heisenberg::PickpocketHandler::GetSingleton().ClearState();
            // Clear water hand-submersion / player-position tracking - stale across loads
            heisenberg::WaterInteraction::GetSingleton().ClearState();
            // Unregister proxy listener before player is unloaded
            heisenberg::PlayerCharacterProxyListener::GetSingleton().UnregisterFromPlayer();
            // Clear last-unequipped weapon tracking - form becomes invalid
            heisenberg::g_heisenberg.ClearLastUnequippedWeapon();
            // Clear recently-dropped ring buffer - stale form IDs from a save made
            // shortly after a drop must not block activation in the next session
            heisenberg::Hooks::ClearRecentDrops();
            break;
        case F4SE::MessagingInterface::kNewGame:
        case F4SE::MessagingInterface::kPostLoadGame:
            ForceWaterDisplacementEnabled();
            spdlog::info("[Water] Forced displacement settings at kNewGame/kPostLoadGame");
            spdlog::info("Game session started - clearing all grab state");
            heisenberg::GrabManager::GetSingleton().ClearAllState();
            heisenberg::ItemPositionConfigMode::GetSingleton().ClearAllState();
            // Highlighter disabled - no need to clear
            // heisenberg::Highlighter::GetSingleton().ClearAllHighlights();
            // Clear cached menu state
            heisenberg::MenuChecker::GetSingleton().ClearState();
            // Clear activator tracking - refs and cell pointer become invalid
            heisenberg::ActivatorHandler::GetSingleton().ClearState();
            // Clear node capture mode
            heisenberg::NodeCaptureMode::GetSingleton().ClearState();
            // Destroy hand collision bodies - world/body pointers become invalid on load
            heisenberg::HandCollision::GetSingleton().Shutdown();
            heisenberg::HandAuthority::Reset();
            heisenberg::LegacyFrikFingerPoseAuthority::Reset();
            // Clear thrown-object tracking - tracked bodies, pending impacts, and the cached
            // hknpWorld pointer all become stale across a load (UAF on first post-load throw).
            heisenberg::ThrownObjectTracker::GetSingleton().Reset();
            heisenberg::ThrownObjectTracker::GetSingleton().SetWorld(nullptr);
            // Clear pending drops/loots - form IDs become invalid
            heisenberg::DropToHand::GetSingleton().ClearState();
            // Clear cooking handler state - refs become invalid
            heisenberg::CookingHandler::GetSingleton().ClearState();
            // Clear smart grab state - inventory state becomes invalid
            heisenberg::SmartGrabHandler::GetSingleton().ClearState();
            // Reset pipboy tape deck state on load — prevents stale open/closed transforms
            heisenberg::PipboyInteraction::GetSingleton().ClearState();
            // Clear pickpocket state - NPC handles become invalid
            heisenberg::PickpocketHandler::GetSingleton().ClearState();
            // Clear water hand-submersion / player-position tracking - stale across loads
            heisenberg::WaterInteraction::GetSingleton().ClearState();
            // Intro holotape: on new game, wait for vault exit; on load, deliver after 3s
            if (a_msg->type == F4SE::MessagingInterface::kNewGame) {
                heisenberg::PipboyInteraction::GetSingleton().SetNewGame();
            } else {
                heisenberg::PipboyInteraction::GetSingleton().QueueIntroHolotapeDelivery();
            }
            // Explicitly re-assert NOT ready here (kPreLoadGame already did so for
            // loads, but not for kNewGame). The gate is OPENED by MenuChecker when
            // LoadingMenu closes — NOT here. kPostLoadGame fires while LoadingMenu
            // is still up (observed: ~17s before close), and engine perk/gamesetting
            // work happens in that window which we must not intercept.
            heisenberg::DropToHand::SetSessionNotReady();
            // Clear hand references to prevent dangling pointers
            g_heisenberg.ClearHandStates();
            // Reset grenade zone/callback state after load
            g_heisenberg.ResetGrenadeZoneState();
            break;
        }
    }

    bool Heisenberg::OnF4SEQuery(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
    {
        // DO NOT initialize logging here. F4SE::log::init() reads
        // F4SE::GetPluginName() / GetSaveFolderName() which are only populated by
        // F4SE::Init() during F4SEPlugin_Load. Calling it now produces a log file
        // at "Documents/My Games//F4SE/.log" with empty plugin name. F4SE::Init()
        // in OnF4SELoad re-runs log::init() with the correct names, so logging is
        // set up properly there.

        // Fill plugin info
        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = "Heisenberg_F4VR";
        a_info->version = Version::MAJOR;

        // Runtime checks — must reject in Query so F4SE doesn't call Load.
        // Cannot use spdlog yet (no sinks attached), so failures are silent here;
        // they'll show up in F4SE's own loader log.
        if (a_f4se->IsEditor()) {
            return false;
        }

        const auto ver = a_f4se->RuntimeVersion();
        if (ver < F4SE::RUNTIME_VR_1_2_72) {
            return false;
        }

        return true;
    }

    bool Heisenberg::OnF4SELoad(const F4SE::LoadInterface* a_f4se)
    {
        // F4SE::Init populates plugin name/save folder from PluginVersionData,
        // then calls F4SE::log::init() with the correct values, then logs the
        // banner "{plugin} v{version}". After this point spdlog routes to the
        // proper Documents/My Games/Fallout4VR/F4SE/HeisenbergF4VR.log.
        F4SE::Init(a_f4se);

        // Release bootstrap is error-only, but users diagnosing launch/config/hook
        // negotiation need those pre-Config::Load lines. Read the one early escape
        // hatch directly; Config::Load later applies the steady-state iLogLevel.
        CSimpleIniA launchIni;
        launchIni.SetUnicode();
        launchIni.LoadFile("Data/F4SE/Plugins/Heisenberg_F4VR.ini");
        const bool verboseLaunch =
            launchIni.GetBoolValue("Debug", "bVerboseLaunch", false);
        spdlog::set_level(
            verboseLaunch ? spdlog::level::debug : spdlog::level::err);
        // PERF (Jul 5): flush per WARN, not per INFO — flushing on every info line is an
        // fflush write syscall per log call on the frame thread (~1000/s under load = real
        // FPS cost). WER minidumps + dmp.py/sym.py cover crash forensics; bump to info/trace
        // temporarily when chasing a CTD whose last line matters.
        spdlog::flush_on(spdlog::level::warn);

        if (verboseLaunch) {
            spdlog::info(
                "[Debug] bVerboseLaunch=1 — launch/config/hook diagnostics enabled "
                "until Config::Load applies iLogLevel");
        }
        spdlog::info("Heisenberg F4VR loading...");

        // Get messaging interface
        _messaging = F4SE::GetMessagingInterface();
        if (!_messaging) {
            spdlog::critical("Failed to get messaging interface");
            return false;
        }

        // Register message listener for F4SE core messages (kGameLoaded, etc.)
        if (!_messaging->RegisterListener(OnF4SEMessage)) {
            spdlog::critical("Failed to register message listener");
            return false;
        }

        // The opening ceremony is save-local state, not a global INI preference.
        // Register before any game/new-game messages can arrive. Failure is non-fatal:
        // legacy holotape-presence inference still prevents ordinary re-delivery.
        heisenberg::IntroCeremonyState::Initialize();

        // Register for messages from ALL other plugins (null sender). This is the
        // path that delivers the public Heisenberg plugin API: kMessage_GetInterface
        // and kMessage_SuppressItemToHand. A "F4SE"-sender listener never receives
        // external dispatches, so this separate registration is required.
        if (!_messaging->RegisterListener(OnExternalPluginMessage, std::string_view{})) {
            spdlog::warn("Failed to register external-plugin message listener (API unavailable to other mods)");
        }

        // Install hooks
        Hooks::Install();

        // Initialize OpenVR hook for controller input interception
        // This must be done before the game calls VR_GetGenericInterface
        auto& openvrHook = OpenVRHook::GetSingleton();
        if (openvrHook.Initialize()) {
            spdlog::info("OpenVR hook initialized - controller input interception enabled");
        } else {
            spdlog::warn("OpenVR hook failed to initialize - input blocking disabled");
        }
        // F4SE plugins are process-lifetime in normal play, but the OpenVR
        // bridge still owns IAT/vtable entries and callback registrations.
        // Register after constructing the singleton so this runs before its
        // own static destructor. Consumers must not assume a cross-DLL atexit
        // order; process-exit cleanup in Controls Config avoids calling back
        // through this DLL's bridge API. Shutdown's callback drains are bounded
        // to 500 ms each, so a thread already terminated by ExitProcess cannot
        // leave Fallout4VR.exe stuck as a zombie.
        static std::once_flag shutdownRegistration;
        std::call_once(shutdownRegistration, [] {
            std::atexit([] {
                OpenVRHook::GetSingleton().Shutdown();
            });
        });

        // ─── SECOND ARCHITECTURE (embedded ROCK engine) ─────────────────────────────
        // Read ONLY the master toggle directly from the INI here. The full Config::Load()
        // runs later (kGameLoaded), but ROCK's host must register its own F4SE lifecycle
        // listener NOW — before kGameLoaded fires — so it sees the same kGameLoaded that
        // drives its FRIK init / config load / skeleton wait. When the toggle is off the
        // linked engine stays completely dormant (no hook, no listener) = identical to today.
        {
            CSimpleIniA bootIni;
            bootIni.SetUnicode();
            bootIni.LoadFile("Data/F4SE/Plugins/Heisenberg_F4VR.ini");
            const bool useRockEngine = bootIni.GetBoolValue(
                "RockEngine",
                "bUseRockEngineArchitecture",
                true);
            if (useRockEngine) {
                spdlog::info("[RockEngine] bUseRockEngineArchitecture=1 — hosting embedded ROCK engine via rock::HostLoad()");
                if (rock::HostLoad(a_f4se)) {
                    s_rockEngineHosted = true;  // enable F4SE-message forwarding into ROCK (see OnF4SEMessage)
                    // Register the plugin-side hand-authority table so ROCK's two-handing / wall-stop
                    // route into our own hand placement when the loaded FRIK lacks the v5 API.
                    rock::HostSetHandAuthority(reinterpret_cast<const rock::HostHandAuthority*>(&heisenberg::HandAuthority::HostTable()));
                    // Stock FRIK 0.77.12 cannot carry ROCK's independent joints,
                    // splay, palm motion, or exact mesh-contact locals. Register a
                    // distinct Heisenberg backend for that payload; native FRIK v5
                    // remains preferred by the ROCK bridge.
                    rock::HostSetFingerPoseAuthority(
                        &heisenberg::LegacyFrikFingerPoseAuthority::HostTable());
                    // Jul 19 (frame-order audit): apply hand authority at the tail of ROCK's
                    // frame — after its weapon write, before the bone-tree flatten — so the
                    // rendered hand tracks the rendered weapon with zero lag.
                    rock::HostSetPostUpdateCallback(
                        &heisenberg::ApplyRockHostVisualAuthorities);
                    rock::HostSetPlayerConsumeBlockCallback(
                        &heisenberg::BlockRockPlayerConsume);
                    rock::HostSetPlayerConsumeProfileCallback(
                        &heisenberg::ResolveRockPlayerConsumeProfile);
                    // Resolve grab+selection ownership for the whole session from the boot INI
                    // (g_config loads later). iGrabMode=9 is canonical; the old external-ROCK
                    // delegation flag remains a compatibility alias while this engine is hosted.
                    // The seam forces the embed's bGrabEnabled/bSelectionEnabled ON across every
                    // shared Heisenberg INI reload; Heisenberg's grip handlers stand down via
                    // GetEffectiveGrabMode.
                    const long bootGrabMode = bootIni.GetLongValue(
                        "ObjectPickup",
                        "iGrabMode",
                        grab_ownership_policy::kFullDynamicGrabMode);
                    const bool bootLegacyDelegate = bootIni.GetBoolValue(
                        "ROCK",
                        "bDelegateWorldGrabToRock",
                        false);
                    const auto grabOwnership =
                        grab_ownership_policy::resolve(
                            static_cast<int>(bootGrabMode),
                            bootLegacyDelegate,
                            true);
                    const bool rockOwnsGrab =
                        grabOwnership.embeddedRockOwnsGrab;
                    rock::HostSetGrabOwnership(rockOwnsGrab);
                    if (rockOwnsGrab) {
                        spdlog::info(
                            "[RockEngine] Dynamic grab requested (iGrabMode={}, legacyDelegate={}) — embedded ROCK exclusively owns grab+selection this session",
                            bootGrabMode,
                            bootLegacyDelegate);
                    } else {
                        spdlog::info(
                            "[RockEngine] Keyframed grab selected (iGrabMode={}, legacyDelegate={}) — Heisenberg owns grab+selection; embedded ROCK supplies collision",
                            bootGrabMode,
                            bootLegacyDelegate);
                    }
                    spdlog::info("[RockEngine] Embedded ROCK engine load OK — settings share Heisenberg_F4VR.ini");
                } else {
                    spdlog::error("[RockEngine] rock::HostLoad() FAILED — embedded ROCK engine disabled this session");
                }
            } else {
                spdlog::info("[RockEngine] bUseRockEngineArchitecture=0 — embedded ROCK engine explicitly disabled");
            }
        }

        spdlog::info("Heisenberg F4VR loaded successfully");
        return true;
    }

    void Heisenberg::OnGameLoad()
    {
        if (_initialized) {
            spdlog::debug("Heisenberg already initialized");
            return;
        }

        // Load configuration from INI file FIRST (before anything else uses it)
        g_config.Load();
        spdlog::info("Configuration loaded from INI");

        // Reconcile the early boot decision with the fully merged config (external INI plus
        // any higher-priority MCM settings). Without this second application, a stale manual
        // MCM override could make Heisenberg's Hand handlers and ROCK's input reader both
        // believe they own grip input. HostSetGrabOwnership is idempotent and also preserves
        // the decision across ROCK's shared-INI reload.
        if (IsRockEngineHosted()) {
            const auto grabOwnership =
                grab_ownership_policy::resolve(
                    g_config.grabMode,
                    g_config.delegateWorldGrabToRock,
                    true);
            rock::HostSetGrabOwnership(
                grabOwnership.embeddedRockOwnsGrab);
            spdlog::info(
                "[RockEngine] Reconciled merged grab ownership: configuredMode={} legacyDelegate={} effectiveMode={} owner={}",
                g_config.grabMode,
                g_config.delegateWorldGrabToRock,
                grabOwnership.effectiveGrabMode,
                grabOwnership.embeddedRockOwnsGrab
                    ? "embedded-ROCK"
                    : "Heisenberg-keyframed");
        }

        // Apply grip weapon draw patch based on config (like STUF VR)
        Hooks::SetGripWeaponDrawDisabled(g_config.disableGripWeaponDraw);

        // Arm the player char-proxy bump guard when forced on, or when the embedded engine
        // is hosted. Integration audit C4: the embedded ROCK engine spawns its OWN hand
        // bodies (HBCS) and ROCK's own installBumpHook is a dead patch (we hooked 0x1E24980
        // first, so its validation memcmp fails). Force our guard on whenever the engine is
        // hosted, else the char-proxy bump hits the known CTD.
        HandBumpHook::SetEnabled(g_config.rockHandBumpGuard
                                 || s_rockEngineHosted);

        // Enable the ROCK Havok timing-fix substep override per config.
        HavokTimingFix::SetEnabled(g_config.havokTimingFix);

        // Apply terminal-on-Pipboy patches based on config (after MCM settings loaded)
        Hooks::ApplyTerminalPatches(g_config.forceTerminalOnWrist);

        // Create InputEnableLayer for controlling kFighting (like FRIK/VH do)
        auto* inputManager = RE::BSInputEnableManager::GetSingleton();
        if (inputManager && !_inputLayerInitialized) {
            if (inputManager->AllocateNewLayer(_inputLayer, "HeisenbergGrab")) {
                _inputLayerID = _inputLayer->layerID;
                _inputLayerInitialized = true;
                RefreshOwnedInputLayer();
                spdlog::info("Created InputEnableLayer 'HeisenbergGrab' with ID {}", _inputLayerID);
            } else {
                spdlog::error("Failed to create InputEnableLayer");
            }
        }

        // Detect Virtual Holsters mod for compatibility mode. StorageZone weapon
        // mutation is independently restricted to an explicit offhand-weapon
        // grab, so ordinary primary-hand shoulder gestures remain VH-owned.
        HMODULE vhModule = GetModuleHandleA("VirtualHolsters.dll");
        if (vhModule) {
            _virtualHolstersDetected = true;
            spdlog::info("Virtual Holsters detected - VH compatibility mode active");
        }

        // Initialize FRIK interface for hand tracking
        // Requires FRIK 0.77+ (API version 2)
        auto& frik = FRIKInterface::GetSingleton();
        if (frik.Initialize()) {
            spdlog::info("FRIK interface connected (version: {}) - using FRIK for hand tracking", frik.GetModVersion());
            const std::uint32_t frikApiVersion = frik.GetApiVersion();
            if (s_rockEngineHosted && frikApiVersion > 0 && frikApiVersion < 5) {
                if (!frik.ReconcileLegacyOffHandGrippingIni()) {
                    spdlog::warn("[FRIK-COMPAT] Automatic legacy offhand-grip suppression failed; FRIK and embedded ROCK may both steer the weapon");
                }
            } else if (s_rockEngineHosted && frikApiVersion >= 5) {
                spdlog::info("[FRIK-COMPAT] FRIK API v{} supports ROCK's runtime grip blocker; live FRIK INI left unchanged",
                    frikApiVersion);
            }
        } else {
            spdlog::warn("FRIK not available or incompatible version - Heisenberg requires FRIK 0.77+");
            spdlog::warn("Heisenberg will use fallback hand tracking (finger poses may not work)");
        }

        // Hand collision system — physics bodies for VR hands (push/touch detection)
        if (g_config.enableHandCollision) {
            auto& handCollision = HandCollision::GetSingleton();
            if (handCollision.Initialize()) {
                spdlog::info("Hand collision initialized");
            }
        }

        // (Two-scenario cleanup: the [RockIntegration] hand-ported subsystem inits —
        // rock_hand_collider / rock_body_collider / rock_weapon_collision /
        // rock_two_handed_grip — were removed with those modules.)


        // Initialize item offset system for per-item grab positioning
        auto& itemOffsets = ItemOffsetManager::GetSingleton();
        itemOffsets.Initialize();

        // NOTE: We cannot use F4VRCommonFramework's UIManager because it depends on g_mod (ModBase)
        // which Heisenberg doesn't use. A custom NIF-based UI will be needed for item repositioning.

        // Initialize item position config mode
        auto& itemConfigMode = ItemPositionConfigMode::GetSingleton();
        itemConfigMode.Initialize();

        // Initialize item insert handler (Port-A-Diner, Nuka machines, etc.)
        auto& itemInsertHandler = ItemInsertHandler::GetSingleton();
        itemInsertHandler.Initialize();

        // Initialize cooking handler (heat-based cooking with grabbed items)
        auto& cookingHandler = CookingHandler::GetSingleton();
        cookingHandler.Initialize();

        // Initialize smart grab handler (context-aware inventory retrieval)
        auto& smartGrabHandler = SmartGrabHandler::GetSingleton();
        smartGrabHandler.Initialize();

        // Initialize water interaction system (ripples/splashes for VR hands)
        auto& waterInteraction = WaterInteraction::GetSingleton();
        waterInteraction.Initialize();

        // Register OpenVR controller state callback to block buttons in certain situations:
        // 1. Block grip when weapon is holstered (prevents native unholster)
        // 2. Block buttons on grabbing hand (prevents Virtual Holsters interference)
        // 3. In grenade zone: Remap A→Grip for grenades, block VH's grip
        auto& openvrHook = OpenVRHook::GetSingleton();
        if (openvrHook.IsHooked()) {
            openvrHook.RegisterControllerStateCallback([](bool isLeft, vr::VRControllerState_t* state) -> uint64_t {
                constexpr std::uint64_t kAllowAll = ~std::uint64_t{0};
                constexpr std::uint64_t kGripMask = 1ULL << vr::k_EButton_Grip;
                constexpr std::uint64_t kAcceptMask = 1ULL << vr::k_EButton_A;
                constexpr double kGrabHoldSeconds = 0.20;
                constexpr std::uint64_t kTapInjectMilliseconds = 60;
                constexpr std::uint64_t kPostDropBlockMilliseconds = 500;

                if (!state) {
                    return kAllowAll;
                }

                auto& modInst = Heisenberg::GetSingleton();
                const std::uint32_t policy =
                    modInst._controllerPolicyFlags.load(std::memory_order_acquire);
                if ((policy & kPolicyInitialized) == 0) {
                    return kAllowAll;
                }

                // A press/release gesture spans several fields (edge, start
                // time, long-hold state, deferred synthetic tap, throwable
                // state). Atomics prevent data races but do not make that
                // collection one state transition. Serialize each hand's
                // complete callback so simultaneous OpenVR consumers cannot
                // observe or publish a half-updated A/X gesture.
                std::mutex& callbackMutex =
                    isLeft
                        ? modInst._controllerCallbackMutexLeft
                        : modInst._controllerCallbackMutexRight;
                std::lock_guard callbackStateLock(callbackMutex);

                std::uint64_t allowMask = kAllowAll;
                const std::uint64_t nowMs = GetTickCount64();
                const double nowSeconds = Utils::GetTime();
                const std::uint64_t rawPressed = state->ulButtonPressed;
                const bool physicalGripPressed = (rawPressed & kGripMask) != 0;
                (isLeft ? modInst._physicalGripPressedLeft
                        : modInst._physicalGripPressedRight)
                    .store(physicalGripPressed, std::memory_order_relaxed);

                const bool thisHandGrabbing =
                    (policy & (isLeft ? kPolicyGrabbingLeft
                                      : kPolicyGrabbingRight)) != 0;
                const bool leftHanded =
                    (policy & kPolicyLeftHanded) != 0;
                const bool isPrimaryHand =
                    leftHanded ? isLeft : !isLeft;
                const bool inMenu = (policy & kPolicyMenuOpen) != 0;
                const bool inGrenadeZone =
                    isPrimaryHand && (policy & kPolicyChestZone) != 0;

                auto& wasGrabbing =
                    isLeft ? modInst._cb_wasGrabbingLeft
                           : modInst._cb_wasGrabbingRight;
                auto& postDropUntil =
                    isLeft ? modInst._cb_postDropBlockUntilLeft
                           : modInst._cb_postDropBlockUntilRight;
                const bool previouslyGrabbing =
                    wasGrabbing.exchange(
                        thisHandGrabbing,
                        std::memory_order_acq_rel);
                if (previouslyGrabbing && !thisHandGrabbing) {
                    postDropUntil.store(
                        nowMs + kPostDropBlockMilliseconds,
                        std::memory_order_release);
                }
                const bool inPostDropBlock =
                    nowMs < postDropUntil.load(std::memory_order_acquire);

                if (!inMenu &&
                    (thisHandGrabbing || inPostDropBlock)) {
                    allowMask &= ~kAcceptMask;
                    allowMask &= ~kGripMask;
                }

                // X/A hold-to-grab must hide a press for its entire grabbed-object
                // arbitration window. A quick release becomes a real-time synthetic
                // press long enough for Fallout's poll, independent of how many other
                // OpenVR consumers queried the same packet.
                const bool handUsesAccept =
                    (isLeft && (policy & kPolicyUseXForLeftGrab) != 0) ||
                    (!isLeft && (policy & kPolicyUseAForRightGrab) != 0);
                auto& acceptPressTime =
                    isLeft ? modInst._cb_axGrabPressTimeL
                           : modInst._cb_axGrabPressTimeR;
                auto& acceptWasPressed =
                    isLeft ? modInst._cb_axGrabWasPressedL
                           : modInst._cb_axGrabWasPressedR;
                auto& acceptHeldLong =
                    isLeft ? modInst._cb_axGrabHeldLongL
                           : modInst._cb_axGrabHeldLongR;
                auto& acceptIntercepted =
                    isLeft ? modInst._cb_axGrabInterceptedL
                           : modInst._cb_axGrabInterceptedR;
                auto& acceptInjectUntil =
                    isLeft ? modInst._cb_axGrabInjectUntilL
                           : modInst._cb_axGrabInjectUntilR;

                if (handUsesAccept && !inMenu) {
                    const bool physicalAccept =
                        (rawPressed & kAcceptMask) != 0;
                    const bool previousAccept =
                        acceptWasPressed.exchange(
                            physicalAccept,
                            std::memory_order_acq_rel);
                    if (physicalAccept && !previousAccept) {
                        const bool intercept =
                            grab_ownership_policy::
                                shouldInterceptAlternateGrabPress(
                                    thisHandGrabbing,
                                    inPostDropBlock);
                        acceptPressTime.store(
                            nowSeconds,
                            std::memory_order_relaxed);
                        acceptHeldLong.store(
                            false,
                            std::memory_order_relaxed);
                        acceptIntercepted.store(
                            intercept,
                            std::memory_order_release);
                    } else if (physicalAccept && previousAccept) {
                        if (acceptIntercepted.load(
                                std::memory_order_acquire) &&
                            nowSeconds -
                                    acceptPressTime.load(
                                        std::memory_order_relaxed) >=
                                kGrabHoldSeconds) {
                            acceptHeldLong.store(
                                true,
                                std::memory_order_relaxed);
                        }
                    } else if (!physicalAccept && previousAccept) {
                        const bool intercepted =
                            acceptIntercepted.exchange(
                                false,
                                std::memory_order_acq_rel);
                        const bool wasLong =
                            acceptHeldLong.exchange(
                                false,
                                std::memory_order_acq_rel);
                        if (intercepted && !wasLong) {
                            acceptInjectUntil.store(
                                nowMs + kTapInjectMilliseconds,
                                std::memory_order_release);
                        }
                    }

                    if (physicalAccept &&
                        acceptIntercepted.load(
                            std::memory_order_acquire)) {
                        // Keep native activation hidden for a confirmed long hold too.
                        allowMask &= ~kAcceptMask;
                    }
                    if (!physicalAccept &&
                        nowMs < acceptInjectUntil.load(
                                    std::memory_order_acquire)) {
                        state->ulButtonPressed |= kAcceptMask;
                        state->ulButtonTouched |= kAcceptMask;
                        allowMask |= kAcceptMask;
                    }
                } else {
                    acceptWasPressed.store(false, std::memory_order_relaxed);
                    acceptHeldLong.store(false, std::memory_order_relaxed);
                    acceptIntercepted.store(false, std::memory_order_relaxed);
                    acceptInjectUntil.store(0, std::memory_order_relaxed);
                }

                const bool stickyCooldown =
                    (policy & (isLeft ? kPolicyStickyLeft
                                      : kPolicyStickyRight)) != 0;
                if (stickyCooldown) {
                    state->ulButtonPressed &= ~kGripMask;
                    state->ulButtonTouched &= ~kGripMask;
                }

                if (isPrimaryHand &&
                    (policy & kPolicyProgramSWF) != 0) {
                    for (auto& axis : state->rAxis) {
                        axis.x = 0.0f;
                        axis.y = 0.0f;
                    }
                    return 0;
                }

                // Throwable classification uses the original physical packet, not
                // the synthetic tap above. All game/config state was published by
                // the main thread; this polling path touches no engine singleton.
                const bool aBit = (rawPressed & kAcceptMask) != 0;
                const bool gripBit = (rawPressed & kGripMask) != 0;
                const float analogGrip = state->rAxis[2].x;
                auto& hasAnalogAtomic =
                    isLeft ? modInst._cb_hasAnalogGripLeft
                           : modInst._cb_hasAnalogGripRight;
                if (analogGrip > 0.1f) {
                    hasAnalogAtomic.store(
                        true,
                        std::memory_order_relaxed);
                }
                const bool hasAnalog =
                    hasAnalogAtomic.load(std::memory_order_relaxed);
                const bool gripSqueezed =
                    hasAnalog ? analogGrip > 0.5f : gripBit;

                bool bindingDetected =
                    modInst._cb_gripABindingDetected.load(
                        std::memory_order_relaxed);
                if (isPrimaryHand &&
                    hasAnalog &&
                    aBit &&
                    !gripBit &&
                    gripSqueezed) {
                    bindingDetected = true;
                    modInst._cb_gripABindingDetected.store(
                        true,
                        std::memory_order_relaxed);
                }

                const bool physicalGripNative =
                    isPrimaryHand && gripBit && gripSqueezed;
                const bool physicalGripRemapped =
                    isPrimaryHand && aBit && !gripBit && gripSqueezed;
                const bool physicalAccept =
                    isPrimaryHand &&
                    aBit &&
                    !gripBit &&
                    (!hasAnalog || analogGrip <= 0.5f);
                const bool bothBits =
                    isPrimaryHand && aBit && gripBit;
                const bool grenadeHandling =
                    (policy & kPolicyGrenadeHandling) != 0;
                const bool alternate =
                    grenadeHandling &&
                    (policy & kPolicyGrenadeRemapToA) != 0;

                bool activatorPressed = false;
                if (isPrimaryHand && !bothBits) {
                    activatorPressed = bindingDetected
                        ? (alternate
                              ? physicalGripRemapped
                              : physicalAccept)
                        : (alternate
                              ? physicalAccept
                              : physicalGripNative);
                }

                const bool zoneAllows =
                    (policy & kPolicyGrenadeZoneDisabled) != 0 ||
                    inGrenadeZone;
                const bool modHandlesThrowable =
                    isPrimaryHand &&
                    grenadeHandling &&
                    zoneAllows &&
                    (alternate || bindingDetected);

                double pressTime =
                    modInst._cb_aButtonPressTime.load(
                        std::memory_order_relaxed);
                bool heldLong =
                    modInst._cb_aButtonHeldLongEnough.load(
                        std::memory_order_relaxed);
                bool wasPressed =
                    modInst._cb_aButtonWasPressed.load(
                        std::memory_order_relaxed);

                if (isPrimaryHand &&
                    nowMs <
                        modInst._cb_forceArmThrowableUntil.load(
                            std::memory_order_acquire)) {
                    heldLong = true;
                }

                if (modHandlesThrowable && !thisHandGrabbing) {
                    if (activatorPressed && !wasPressed) {
                        pressTime = nowSeconds;
                        heldLong = false;
                    } else if (activatorPressed && wasPressed) {
                        const float threshold =
                            modInst._controllerThrowableHoldSeconds.load(
                                std::memory_order_relaxed);
                        if (nowSeconds - pressTime >= threshold) {
                            heldLong = true;
                        }
                    } else if (!activatorPressed) {
                        heldLong = false;
                    }
                    wasPressed = activatorPressed;
                } else if (isPrimaryHand) {
                    wasPressed = false;
                    if (nowMs >=
                        modInst._cb_forceArmThrowableUntil.load(
                            std::memory_order_acquire)) {
                        heldLong = false;
                    }
                }

                if (modHandlesThrowable && !inMenu && heldLong) {
                    state->ulButtonPressed |= kGripMask;
                    state->ulButtonPressed &= ~kAcceptMask;
                }

                if (isPrimaryHand) {
                    modInst._cb_aButtonPressTime.store(
                        pressTime,
                        std::memory_order_relaxed);
                    modInst._cb_aButtonHeldLongEnough.store(
                        heldLong,
                        std::memory_order_relaxed);
                    modInst._cb_aButtonWasPressed.store(
                        wasPressed,
                        std::memory_order_relaxed);
                }
                return allowMask;
            });
            spdlog::info("Registered button blocking callback (A→Grip remap in grenade zone)");
        }

        // Initialize drop-to-hand feature
        auto& dropToHand = DropToHand::GetSingleton();
        dropToHand.Initialize();
        spdlog::info("Drop-to-hand feature initialized");

        // Initialize activator handler for touch-based button/switch activation
        if (g_config.enableInteractiveActivators) {
            ActivatorHandler::GetSingleton().Initialize();
            spdlog::info("Interactive activator handler initialized");

            // Proxy listener disabled — pair collision filter handles capsule collision.
            // The addListener approach caused deadlocks in hknp.
            spdlog::info("Player capsule collision handled via pair filter (proxy listener disabled)");
        }

        // Initialize object highlighter
        if (g_config.enableHighlighting) {
            auto& highlighter = Highlighter::GetSingleton();
            if (highlighter.Initialize()) {
                spdlog::info("Object highlighter initialized");
            } else {
                spdlog::warn("Object highlighter failed to initialize - highlighting disabled");
            }
        }

        // Initialize event-based menu checker (safer than calling UI::GetMenuOpen from hooks)
        // This caches menu states via MenuOpenCloseEvent to avoid race conditions
        if (MenuChecker::GetSingleton().Initialize()) {
            spdlog::info("Event-based menu checker initialized");
        } else {
            spdlog::warn("Menu checker failed to initialize - using direct UI calls (less safe)");
        }

        // NOTE (Jul 31): a TESHitEvent damage-observer sink was removed here —
        // RE::TESHitEvent::GetEventSource() resolves via REL::RelocationID
        // (989868, 1411899), which are the Skyrim-SE / FO4-AE address-library
        // IDs and have NO Fallout4VR mapping. Calling it at init threw
        // "Failed to find the id within the address library: 989868" and
        // aborted the whole plugin at launch. VR-safe hit/damage observation
        // needs a VR address-library offset for the FO4VR hit-event source (or
        // a native ProcessHit hook), not this CommonLib helper.

        // NOTE: grenade gating is now done entirely by the MeleeThrowHandler hook
        // (Hooks.cpp), which caps the button hold-duration below the throw threshold
        // when readying should be blocked. fThrowDelay:Controls is left at the game
        // default — the old system's 999999 override is gone (it permanently disabled
        // throwing once the zone was enabled).

        InitHands();
        _initialized = true;
        PublishControllerPolicy();
        spdlog::info("Heisenberg initialized");
    }

    void Heisenberg::SetInputSuppression(
        InputSuppressionReason reason,
        bool suppressed)
    {
        const auto bit = static_cast<std::uint32_t>(reason);
        const auto before = _inputSuppressionReasons;
        if (suppressed) {
            _inputSuppressionReasons |= bit;
        } else {
            _inputSuppressionReasons &= ~bit;
        }
        if (before != _inputSuppressionReasons) {
            RefreshOwnedInputLayer();
        }
    }

    void Heisenberg::RefreshOwnedInputLayer()
    {
        if (!_inputLayerInitialized) {
            return;
        }
        auto* manager = RE::BSInputEnableManager::GetSingleton();
        if (!manager) {
            return;
        }

        using Reason = InputSuppressionReason;
        const auto has = [this](Reason reason) {
            return (_inputSuppressionReasons &
                    static_cast<std::uint32_t>(reason)) != 0;
        };

        RE::UEFlag disabledUser = static_cast<RE::UEFlag>(0);
        if (has(Reason::PostGrabFighting) ||
            has(Reason::HeldObjectFighting)) {
            disabledUser = disabledUser | RE::UEFlag::kFighting;
        }
        if (has(Reason::ItemPositionConfig) ||
            has(Reason::StorageZoneConfig)) {
            disabledUser = disabledUser |
                RE::UEFlag::kMainFour |
                RE::UEFlag::kVATS |
                RE::UEFlag::kActivate |
                RE::UEFlag::kMenu;
        }
        if (has(Reason::HeldObjectActivation) ||
            has(Reason::PostConsumeActivation)) {
            disabledUser = disabledUser | RE::UEFlag::kActivate;
        }

        RE::OEFlag disabledOther = static_cast<RE::OEFlag>(0);
        if (has(Reason::NativeZKey)) {
            disabledOther = disabledOther | RE::OEFlag::kZKey;
        }
        if (has(Reason::HeldObjectActivation) ||
            has(Reason::PostConsumeActivation)) {
            disabledOther = disabledOther | RE::OEFlag::kActivation;
        }
        if (has(Reason::ItemPositionConfig) ||
            has(Reason::StorageZoneConfig)) {
            disabledOther = disabledOther |
                RE::OEFlag::kVATS |
                RE::OEFlag::kActivation |
                RE::OEFlag::kFavorites;
        }

        constexpr auto sender = RE::UserEvents::SENDER_ID::kGameplay;
        // Reset only our named layer, then apply the union of Heisenberg's live
        // owners. Other mods' layers and the engine force words are untouched.
        manager->EnableUserEvent(
            _inputLayerID,
            RE::UEFlag::kAll,
            true,
            sender);
        manager->EnableOtherEvent(
            _inputLayerID,
            RE::OEFlag::kAll,
            true,
            sender);
        if (static_cast<std::uint32_t>(disabledUser) != 0) {
            manager->EnableUserEvent(
                _inputLayerID,
                disabledUser,
                false,
                sender);
        }
        if (static_cast<std::uint32_t>(disabledOther) != 0) {
            manager->EnableOtherEvent(
                _inputLayerID,
                disabledOther,
                false,
                sender);
        }
    }

    void Heisenberg::PublishControllerPolicy()
    {
        std::uint32_t flags = 0;
        if (_initialized) {
            flags |= kPolicyInitialized;
        }
        if (VRInput::GetSingleton().IsLeftHandedMode()) {
            flags |= kPolicyLeftHanded;
        }

        const auto& menus = MenuChecker::GetSingleton();
        if (menus.IsPaused() ||
            menus.IsInventoryOpen() ||
            menus.IsContainerOpen() ||
            menus.IsWorkshopOpen() ||
            menus.IsGameStopped()) {
            flags |= kPolicyMenuOpen;
        }
        if (_isInChestPocketZone) {
            flags |= kPolicyChestZone;
        }

        const auto& grabManager = GrabManager::GetSingleton();
        if (grabManager.GetGrabState(true).active) {
            flags |= kPolicyGrabbingLeft;
        }
        if (grabManager.GetGrabState(false).active) {
            flags |= kPolicyGrabbingRight;
        }
        if (_leftStickyGrabCooldown > 0.0f) {
            flags |= kPolicyStickyLeft;
        }
        if (_rightStickyGrabCooldown > 0.0f) {
            flags |= kPolicyStickyRight;
        }
        if (PipboyInteraction::GetSingleton().IsProgramSWFActive()) {
            flags |= kPolicyProgramSWF;
        }
        if (g_config.useXForLeftGrab) {
            flags |= kPolicyUseXForLeftGrab;
        }
        if (g_config.useAForRightGrab) {
            flags |= kPolicyUseAForRightGrab;
        }
        if (g_config.enableGrenadeHandling) {
            flags |= kPolicyGrenadeHandling;
        }
        if (g_config.remapGrenadeButtonToA) {
            flags |= kPolicyGrenadeRemapToA;
        }
        if (g_config.throwableActivationZone == 0) {
            flags |= kPolicyGrenadeZoneDisabled;
        }
        _controllerThrowableHoldSeconds.store(
            g_config.throwableHoldDuration,
            std::memory_order_relaxed);
        OpenVRHook::GetSingleton().SetSuppressThumbstickTouch(
            g_config.suppressThumbstickTouch);
        _controllerPolicyFlags.store(flags, std::memory_order_release);
    }

    void Heisenberg::ForceArmThrowable()
    {
        // Real-time ownership prevents unrelated OpenVR consumers from consuming
        // a frame counter before Fallout polls the primary controller.
        _cb_forceArmThrowableUntil.store(
            GetTickCount64() + 120,
            std::memory_order_release);
    }

    void Heisenberg::ResetGrenadeZoneState()
    {
        // Reset zone tracking so transitions are detected fresh after load.
        // (Grenade gating is the MeleeThrowHandler hook's job now; no fThrowDelay
        // manipulation here.)
        _isInChestPocketZone = false;
        _wasInChestPocketZone = false;
        _wasInChest = false;
        _doubleTapHoldActive = false;
        // Reset callback atomics to prevent stale A-button state
        _cb_aButtonPressTime.store(0.0, std::memory_order_relaxed);
        _cb_aButtonHeldLongEnough.store(false, std::memory_order_relaxed);
        _cb_aButtonWasPressed.store(false, std::memory_order_relaxed);
        _cb_forceArmThrowableUntil.store(0, std::memory_order_relaxed);
        _cb_axGrabWasPressedL.store(false, std::memory_order_relaxed);
        _cb_axGrabWasPressedR.store(false, std::memory_order_relaxed);
        _cb_axGrabHeldLongL.store(false, std::memory_order_relaxed);
        _cb_axGrabHeldLongR.store(false, std::memory_order_relaxed);
        _cb_axGrabInterceptedL.store(false, std::memory_order_relaxed);
        _cb_axGrabInterceptedR.store(false, std::memory_order_relaxed);
        _cb_axGrabInjectUntilL.store(0, std::memory_order_relaxed);
        _cb_axGrabInjectUntilR.store(0, std::memory_order_relaxed);
        _cb_wasGrabbingLeft.store(false, std::memory_order_relaxed);
        _cb_wasGrabbingRight.store(false, std::memory_order_relaxed);
        _cb_postDropBlockUntilLeft.store(0, std::memory_order_relaxed);
        _cb_postDropBlockUntilRight.store(0, std::memory_order_relaxed);
        PublishControllerPolicy();
    }

    void Heisenberg::ProcessPendingWeaponUnequip()
    {
        if (!_pendingUnequipForm) return;

        auto* form = _pendingUnequipForm;
        auto name = std::move(_pendingUnequipName);
        const bool offhandIsLeft =
            _pendingUnequipOffhandIsLeft;
        _pendingUnequipForm = nullptr;
        _pendingUnequipName.clear();

        // Queueing happens during the input/physics callback and the actual
        // ActorEquipManager mutation happens here. Revalidate the complete
        // ownership condition at this transaction boundary so releasing the
        // offhand weapon or changing equipment in between cancels the request.
        auto* currentlyEquipped = GetPlayerEquippedRealWeapon();
        const bool offhandStillGrabbingWeapon =
            IsHandGrabbingRealWeapon(offhandIsLeft);
        if (!g_config.enableStorageZoneWeaponEquip ||
            !offhandStillGrabbingWeapon ||
            currentlyEquipped != form) {
            spdlog::info(
                "[GRAB] Cancelled deferred StorageZone weapon unequip: "
                "offhand={} stillGrabbingWeapon={} expected={:08X} current={:08X}",
                offhandIsLeft ? "left" : "right",
                offhandStillGrabbingWeapon,
                form ? form->GetFormID() : 0u,
                currentlyEquipped ? currentlyEquipped->GetFormID() : 0u);
            return;
        }

        // Resolve the exact currently equipped stack/instance at commit time.
        // The old null-instance/stack=-1 call could unequip the wrong customized
        // weapon when multiple stacks shared one base form.
        const auto unequipResult =
            rock::weapon_equip_transfer::
                unequipEquippedWeaponFromPlayer(
                    rock::weapon_equip_transfer::
                        EquippedUnequipInput{
                            .playSounds = true,
                        });
        if (!unequipResult.success ||
            unequipResult.weapon != form) {
            spdlog::warn(
                "[GRAB] StorageZone exact-stack unequip failed: "
                "expected={:08X} observed={:08X} reason={} attempted={}",
                form ? form->GetFormID() : 0u,
                unequipResult.formID,
                rock::weapon_equip_transfer::unequipReasonName(
                    unequipResult.reason),
                unequipResult.attempted);
            return;
        }

        // Retain the historical record for API/save compatibility. StorageZone
        // itself no longer consumes it to re-equip a primary-hand weapon.
        _lastUnequippedWeapon = form;
        _lastUnequippedWeaponName = name;

        // Block smart retrieval briefly so the same hand doesn't immediately
        // smart-grab a new item right after the unequip.
        StartWeaponUnequipCooldown();

        spdlog::debug("[GRAB] Unequipped weapon '{}' via ActorEquipManager (storage zone)", name);
        if (g_config.showUnequipMessages)
            heisenberg::Hooks::ShowHUDMessageDirect(
                std::format("{} was unequipped", name).c_str());
    }

    void Heisenberg::ProcessPendingWeaponReequip()
    {
        if (!_pendingReequipForm) return;

        auto* form = _pendingReequipForm;
        auto name = std::move(_pendingReequipName);
        _pendingReequipForm = nullptr;
        _pendingReequipName.clear();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        RE::ActorEquipManager** equipMgrPtr = heisenberg::g_ActorEquipManager.get();
        if (!equipMgrPtr || !*equipMgrPtr) {
            spdlog::error("[GRAB] ActorEquipManager not available for weapon re-equip");
            return;
        }

        RE::ActorEquipManager* equipMgr = *equipMgrPtr;

        struct LocalObjectInstance {
            RE::TESForm* object{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
        };
        static_assert(sizeof(LocalObjectInstance) == 0x10);

        LocalObjectInstance instance;
        instance.object = form;
        instance.instanceData = nullptr;

        // Get equip slot from weapon
        RE::BGSEquipSlot* equipSlot = nullptr;
        if (form->IsWeapon()) {
            equipSlot = static_cast<RE::TESObjectWEAP*>(form)->GetEquipSlot(nullptr);
        }

        bool equipped = heisenberg::ActorEquipManager_EquipObject(
            equipMgr,
            player,
            reinterpret_cast<RE::BGSObjectInstance*>(&instance),
            0,          // stackID
            1,          // number
            equipSlot,  // equip slot
            true,       // queue equip
            false,      // don't force equip
            true,       // play sounds
            false,      // DON'T apply now - defer to safe game phase
            false       // not locked
        );

        if (equipped) {
            spdlog::debug("[GRAB] Re-equipped weapon '{}' via storage zone grip", name);
            if (g_config.showUnequipMessages)
                heisenberg::Hooks::ShowHUDMessageDirect(
                    std::format("{} equipped", name).c_str());
        } else {
            spdlog::warn("[GRAB] Failed to re-equip weapon '{}'", name);
        }

        // Clear the last-unequipped tracking since we re-equipped
        _lastUnequippedWeapon = nullptr;
        _lastUnequippedWeaponName.clear();
    }

    // ────────────────────────────────────────────────────────────────────────
    // SCOPE EXIT: recover a skeleton FRIK left collapsed
    //
    // FRIK's Skeleton::hideHands() (Skeleton.cpp:2265) does NOT just hide hands - it sets
    // the whole third-person skeleton root to `local.scale = 0.00001f` and shoves it 10
    // units behind the camera, so the body cannot render in front of the scope camera. It
    // is called unconditionally from Skeleton::onFrameUpdate whenever isInScopeMenu().
    //
    // FRIK's ONLY restore is `_root->local.scale = 1.0f` in setBodyUnderHMD - and that line
    // sits AFTER an early return:
    //
    //     if (!tryGetRotationFromVectors(back, bodyDir, bodyFacing)) { return; }
    //     ...
    //     _root->local.scale = 1.0f;        // never reached on that frame
    //
    // `back` is the normalized planar HMD forward. If it degenerates (near-vertical gaze,
    // a momentarily unusable head pose) the function bails and the collapse is never undone
    // - the player leaves the scope with an invisible body and a floating weapon, and it
    // persists until some later frame happens to succeed. Reported live by a tester.
    //
    // This is FRIK's bug to fix properly; the guard below just stops it being ours to
    // suffer. Deliberately narrow: it only acts while the scope is CLOSED, so it can never
    // fight the collapse FRIK legitimately wants during a scope view, and it waits out a
    // short grace period first so FRIK's own restore gets the first attempt on the normal
    // path. Restoring the scale is enough - FRIK recomputes local rotation and translation
    // itself on the next frame setBodyUnderHMD succeeds; scale is the part that makes the
    // body invisible.
    // ────────────────────────────────────────────────────────────────────────
    // SURVIVAL STATUS-TOKEN AUDIT
    //
    // Fallout 4's Survival needs (hunger/thirst/fatigue) are implemented as hidden ALCH
    // items sitting in the player's inventory - Peckish, Hungry, Ravenous, Starving,
    // Parched, Dehydrated, Weary and so on. The game holds exactly ONE per need at a time
    // and swaps it when the tier changes.
    //
    // Heisenberg's drop-to-hand watches container-changed events, and a tier swap
    // (old removed from player, new added) is indistinguishable from a real player drop.
    // If one of these is ever intercepted, the drop times out waiting for 3D that an
    // invisible item never gets, and the token is re-added - RE-APPLYING ITS EFFECT. That
    // is the "stuck starving, eating changes nothing" report: the tier can never fall
    // because something keeps putting the token back.
    //
    // DropToHand already carries THREE stacked guards against this (non-playable, HC_/omod
    // editor-ID prefix, model-less ALCH) - each added after a previous recurrence. Every one
    // of them enumerates what a token LOOKS like, so a token shaped slightly differently
    // slips through and the bug returns. This audit exists to stop guessing: it reports what
    // is actually in the inventory so the next recurrence is diagnosed from data instead of
    // from another round of pattern-matching.
    //
    // Read-only. It never removes anything - a wrong auto-removal here would corrupt a
    // legitimate survival state, which is worse than the bug.
    void Heisenberg::AuditSurvivalStatusTokens()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) {
            return;
        }

        int distinctTokens = 0;
        int totalStacked = 0;

        for (auto& invItem : player->inventoryList->data) {
            auto* obj = invItem.object;
            if (!obj || obj->GetFormType() != RE::ENUM_FORM_ID::kALCH) {
                continue;
            }

            const char* editorId = obj->GetFormEditorID();
            const bool hcPrefix = editorId && std::strncmp(editorId, "HC_", 3) == 0;

            const char* modelPath = nullptr;
            if (auto* asModel = obj->As<RE::TESModel>()) {
                modelPath = asModel->GetModel();
            }
            const bool modelLess = (!modelPath || modelPath[0] == '\0');

            if (!hcPrefix && !modelLess) {
                continue;  // an ordinary edible/chem
            }

            const int count = invItem.GetCount();
            std::string name;
            {
                const auto view = RE::TESFullName::GetFullName(*obj, false);
                name = view.empty() ? "<no name>" : std::string(view);
            }

            ++distinctTokens;
            totalStacked += count;

            // count > 1 is the smoking gun. The game keeps exactly one token per need, so
            // a stack means something re-added it - almost certainly us.
            spdlog::warn("[SURVIVAL-AUDIT] {:08X} '{}' edID='{}' count={} hcPrefix={} modelLess={}{}",
                         obj->GetFormID(),
                         name,
                         editorId ? editorId : "<none>",
                         count,
                         hcPrefix,
                         modelLess,
                         count > 1 ? "  <-- STACKED: re-added, this is the bug" : "");
        }

        if (distinctTokens > 0) {
            spdlog::warn("[SURVIVAL-AUDIT] {} status token(s) in inventory, {} total. "
                         "One per active need is NORMAL; any count>1 is not.",
                         distinctTokens, totalStacked);
        } else {
            spdlog::info("[SURVIVAL-AUDIT] No survival status tokens in inventory "
                         "(either Survival is off, or all needs are satisfied)");
        }
    }

    void Heisenberg::RecoverSkeletonCollapsedByScopeExit(float deltaTime)
    {
        // Well below any legitimate skeleton scale, far above FRIK's 0.00001 sentinel.
        constexpr float kCollapsedScaleThreshold = 0.01f;
        constexpr float kPostScopeGraceSeconds = 0.25f;

        static float s_grace = 0.0f;
        static std::uint32_t s_recoveries = 0;

        if (MenuChecker::GetSingleton().IsScopeOpen()) {
            s_grace = kPostScopeGraceSeconds;   // FRIK owns the collapse while scoped
            return;
        }
        if (s_grace > 0.0f) {
            s_grace -= deltaTime;               // let FRIK restore it first
            return;
        }

        RE::NiNode* skeletonRoot = f4vr::getRootNode();
        if (!skeletonRoot || skeletonRoot->local.scale > kCollapsedScaleThreshold) {
            return;                             // healthy - the overwhelmingly common path
        }

        // Capture BEFORE the write - reporting the threshold instead of what was actually
        // there would make every occurrence look identical and hide a different root cause.
        const float observedScale = skeletonRoot->local.scale;

        skeletonRoot->local.scale = 1.0f;
        f4vr::updateTransformsDown(skeletonRoot, true);

        ++s_recoveries;
        spdlog::warn("[SKELETON] Restored collapsed skeleton root after scope exit "
                     "(observed scale={:.5f}; FRIK's setBodyUnderHMD early-returned before its "
                     "own restore. 0.00001 = FRIK hideHands; anything else is a DIFFERENT "
                     "cause worth investigating. n={})",
                     observedScale, s_recoveries);
    }

    void Heisenberg::OnFrameUpdate()
    {
        // If mod is disabled (incompatible FRIK), do nothing
        if (_modDisabled) {
            return;
        }

        // Legacy single update - calls both phases for backwards compatibility
        OnInputUpdate();
        OnGrabUpdate();
    }

    void Heisenberg::OnInputUpdate()
    {
        // PRE-PHYSICS UPDATE: Runs BEFORE the engine's physics step
        // Good for: input processing, grab detection, starting grabs
        // BAD for: positioning grabbed objects (will be overwritten by physics)

        // Pre-warm BSFixedString entries for HUD rollover hooks (once).
        // Cannot be done during F4SEPlugin_Load (string pool not ready) or inside
        // vtable hooks (corrupts HUD). First frame update is the safe window.
        Hooks::InitRolloverStrings();

        // Tick deferred HUD unsuppress (catches native messages after ActivateRef)
        Hooks::UpdateDeferredHUDUnsuppress();

        // Tick weapon draw block cooldown (storage zone unequip)
        Hooks::TickWeaponDrawBlock();

        // Tick deferred disable queue (weapons/armor with behavior graphs)
        heisenberg::TickDeferredDisables();

        if (_modDisabled || !_initialized) {
            return;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        const auto frameNow = std::chrono::steady_clock::now();
        if (_lastFrameTick.time_since_epoch().count() != 0) {
            _frameDeltaSeconds = std::clamp(
                std::chrono::duration<float>(
                    frameNow - _lastFrameTick)
                    .count(),
                1.0f / 240.0f,
                0.10f);
        }
        _lastFrameTick = frameNow;
        const float deltaTime = _frameDeltaSeconds;

        // MAIN THREAD HEARTBEAT: Log every ~5 seconds to verify main thread is running.
        // If deadlock occurs, this will stop appearing while OpenVR callback heartbeat continues.
        static int mainThreadHeartbeat = 0;
        static uint64_t mainThreadTotalFrames = 0;
        ++mainThreadTotalFrames;
        if (++mainThreadHeartbeat >= 450) {  // ~5 seconds at 90fps
            mainThreadHeartbeat = 0;
            spdlog::debug("[MAIN THREAD HEARTBEAT] OnInputUpdate running, total frames={}", mainThreadTotalFrames);
        }

        // STALL WATCHDOG (Jul 31): a 12.4s all-thread log silence was observed
        // live with no way to tell an external suspension (SteamVR dashboard,
        // alt-tab, driver) from an in-process hang. Log the wall-clock gap on
        // the first tick AFTER any >2s main-loop pause, tagged with the menu
        // state so loading screens (which legitimately gap the main thread for
        // tens of seconds) stay interpretable rather than alarming.
        {
            static ULONGLONG s_lastTickMs = 0;
            const ULONGLONG nowMs = GetTickCount64();
            if (s_lastTickMs != 0 && nowMs - s_lastTickMs > 2000) {
                const auto& menus = MenuChecker::GetSingleton();
                spdlog::warn(
                    "[STALL] Main loop did not tick for {} ms (loading={} paused={} gameStopped={}) — "
                    "external suspension (dashboard/alt-tab) if menus are clear, otherwise investigate",
                    nowMs - s_lastTickMs,
                    menus.IsLoading(),
                    menus.IsPaused(),
                    menus.IsGameStopped());
            }
            s_lastTickMs = nowMs;
        }

        // NOTE: kFighting toggle code removed - was causing issues

        // Post-load input watchdog: dumps the engine's input-enable layers and re-asserts movement
        // at a few checkpoints after a save load. Disarmed (single atomic load) the rest of the time.
        InputRecovery::Tick();

        // Check for MCM settings changes (throttled internally to every 2 seconds)
        g_config.ReloadIfMCMChanged();

        // Sync grip weapon draw patch with current config (no-op if unchanged)
        Hooks::SetGripWeaponDrawDisabled(g_config.disableGripWeaponDraw);

        // HUD rollover button hiding is now handled entirely by vtable hooks
        // (NullRolloverButtons / NullAllRolloverFields). Binary patches are no longer
        // used — they caused an asymmetry where the right wand's ShowRollover
        // implementation would suppress item names when ShowActivateButton was RET'd.
        Hooks::ApplyHUDRolloverButtonPatches(false);

        // Update time-based state using measured frame time (clamped above).
        UpdateStickyGrabCooldowns(deltaTime);
        UpdateWeaponUnequipCooldown(deltaTime);

        // Entering or leaving Power Armor REPLACES the skeleton, so a palm frame
        // measured against the old one describes a hand that no longer exists.
        // Only kPreLoadGame used to reset it, which left the PA session running
        // on the pre-PA calibration (live: a 2.97-unit palm/knuckle disagreement
        // the moment PA was entered). Re-measure across the transition instead.
        {
            static bool lastInPowerArmor = false;
            static bool powerArmorStateKnown = false;
            const bool inPowerArmor = heisenberg::Utils::IsPlayerInPowerArmor();
            if (!powerArmorStateKnown) {
                powerArmorStateKnown = true;
                lastInPowerArmor = inPowerArmor;
            } else if (inPowerArmor != lastInPowerArmor) {
                lastInPowerArmor = inPowerArmor;
                heisenberg::ResetFingerCalibration();
                spdlog::info(
                    "[FINGER-CAL] Power Armor {} — skeleton replaced, re-measuring the palm frame",
                    inPowerArmor ? "entered" : "exited");
            }
        }

        // Opportunistic finger calibration — no-op once calibrated. Uses a
        // straightness gate so curled-finger frames abort without committing.
        // Runs every frame while either hand is still un-calibrated.
        heisenberg::TryCalibrateFingerDataIfIdle(true);
        heisenberg::TryCalibrateFingerDataIfIdle(false);

        // Update menu close cooldown (1 second after closing pipboy, inventory, etc.)
        MenuChecker::GetSingleton().UpdateMenuCloseCooldown(deltaTime);

        // Cache weapon state for thread-safe access from OpenVR callback
        UpdateCachedWeaponState();

        // === Trigger press: deactivate unarmed + force open hand ===
        // Trigger press while not grabbing:
        // 1. Unarmed fist active (weaponDrawn + no real weapon) → sheathe weapon
        // 2. Always clear any stale FRIK hand pose override → ensures open hand
        // This acts as a reliable "open hand" reset, like Virtual Holsters.
        {
            static bool lastTriggerRight = false;
            static bool lastTriggerLeft = false;
            bool triggerRight = g_vrInput.IsPressed(false, VRButton::Trigger);
            bool triggerLeft = g_vrInput.IsPressed(true, VRButton::Trigger);

            bool justPressedRight = triggerRight && !lastTriggerRight;
            bool justPressedLeft = triggerLeft && !lastTriggerLeft;

            // Log trigger state for debugging hand open issues
            if (justPressedRight || justPressedLeft) {
                bool holding = IsHoldingAnything();
                bool weaponDrawn = _cachedWeaponDrawn.load(std::memory_order_relaxed);
                bool hasRealWeapon = _cachedHasRealWeapon.load(std::memory_order_relaxed);
                // Hand state: 0=Idle, 1=SelectedClose, 2=Pulling, 3=Held
                // FingerAnimator state: 0=Idle, 1=Closing, 2=Holding, 3=Opening
                int rightHandState = _rightHand ? static_cast<int>(_rightHand->GetState()) : -1;
                int leftHandState = _leftHand ? static_cast<int>(_leftHand->GetState()) : -1;
                spdlog::debug("[TRIGGER-DEBUG] {} trigger pressed: holdingAny={} weaponDrawn={} hasRealWeapon={} handState=L{}/R{} fingerState=L{}/R{}",
                             justPressedRight ? "Right" : "Left", holding, weaponDrawn, hasRealWeapon,
                             leftHandState, rightHandState,
                             static_cast<int>(_leftFingerAnimator.GetState()),
                             static_cast<int>(_rightFingerAnimator.GetState()));
            }

            if ((justPressedRight || justPressedLeft) && !IsHoldingAnything())
            {
                bool weapDrawn = _cachedWeaponDrawn.load(std::memory_order_relaxed);
                bool hasReal   = _cachedHasRealWeapon.load(std::memory_order_relaxed);

                // Unarmed fists (drawn + no real weapon) → sheathe to open hand
                if (weapDrawn && !hasReal && g_config.enableUnarmedAutoUnequip)
                {
                    SetInputSuppression(
                        InputSuppressionReason::PostGrabFighting,
                        true);
                    _postGrabFightingSuppressed = true;
                    spdlog::debug("[INPUT] Suppressed kFighting (unarmed trigger press)");
                    player->DrawWeaponMagicHands(false);
                    spdlog::debug("[INPUT] Trigger: sheathing unarmed fists");
                }

                // Force open hand + clear FRIK override, but only if the animator
                // is actively overriding (closing/holding/opening from a grab).
                // When idle, ForceReset interferes with the game's native hand
                // animation (e.g., unarmed fist pose in LH mode).
                if (justPressedRight && _rightFingerAnimator.IsActive()) {
                    spdlog::debug("[INPUT] Right trigger -> ForceReset right hand");
                    _rightFingerAnimator.ForceReset(false);
                }
                if (justPressedLeft && _leftFingerAnimator.IsActive()) {
                    spdlog::debug("[INPUT] Left trigger -> ForceReset left hand");
                    _leftFingerAnimator.ForceReset(true);
                }
            }

            lastTriggerRight = triggerRight;
            lastTriggerLeft = triggerLeft;
        }

        // Heartbeat disabled for release - enable debugLogging in config if needed

        // Process input and hand state - but don't update grab positions yet
        // Hand::Update() handles input processing and grab detection
        UpdateHands();

        // Update finger close state AFTER hands (needs current selection state)
        UpdateFingerCloseState();

        UpdateInputSuppression();
        UpdateChestPocketZone();
        UpdateStorageZoneConfig();

        // FRIK can leave the whole skeleton collapsed after a scope exit (see the function).
        RecoverSkeletonCollapsedByScopeExit(deltaTime);

        // SKELETON STATE WITNESS.
        //
        // "My body disappeared" has now been reported for two DIFFERENT underlying causes, and
        // the recovery guard above is silent whenever it decides NOT to act - so a log with no
        // [SKELETON] line cannot distinguish "the body is fine" from "the body is gone for a
        // reason I do not handle". This reports the actual state periodically so the next
        // occurrence is diagnosed from numbers instead of from a theory.
        //
        // hideHands() collapses BOTH scale and position (scale 0.00001 and a 10-unit shove
        // behind the camera), so position is reported too: a healthy scale with the root far
        // from the player is a different failure from a collapsed scale, and only this line
        // tells them apart.
        {
            static float s_skelWitness = 0.0f;
            s_skelWitness -= deltaTime;
            if (s_skelWitness <= 0.0f) {
                s_skelWitness = 5.0f;
                if (RE::NiNode* root = f4vr::getRootNode()) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    const RE::NiPoint3 rp = root->world.translate;
                    float distFromPlayer = -1.0f;
                    if (player) {
                        const RE::NiPoint3 pp = player->GetPosition();
                        const RE::NiPoint3 d{ rp.x - pp.x, rp.y - pp.y, rp.z - pp.z };
                        distFromPlayer = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    }
                    const bool collapsed = root->local.scale < 0.01f;
                    const bool displaced = distFromPlayer > 200.0f;
                    if (collapsed || displaced) {
                        spdlog::warn("[SKELETON-STATE] root localScale={:.5f} worldScale={:.5f} "
                                     "distFromPlayer={:.1f} scopeOpen={} -- {}{}",
                                     root->local.scale, root->world.scale, distFromPlayer,
                                     MenuChecker::GetSingleton().IsScopeOpen(),
                                     collapsed ? "COLLAPSED " : "",
                                     displaced ? "DISPLACED" : "");
                    } else {
                        spdlog::info("[SKELETON-STATE] healthy: localScale={:.3f} "
                                     "distFromPlayer={:.1f} scopeOpen={}",
                                     root->local.scale, distFromPlayer,
                                     MenuChecker::GetSingleton().IsScopeOpen());
                    }
                } else {
                    spdlog::warn("[SKELETON-STATE] getRootNode() returned NULL - the player has "
                                 "no skeleton root at all, which no scale/position fix can address");
                }
            }
        }

        // Survival status-token audit. Runs every 10s rather than once at load, because the
        // failure mode is a token being RE-ADDED over time - a single snapshot at load would
        // miss exactly the thing we need to see. Cheap: one inventory walk, and it only emits
        // when something is actually there.
        {
            static float s_survivalAuditTimer = 0.0f;
            s_survivalAuditTimer -= deltaTime;
            if (s_survivalAuditTimer <= 0.0f) {
                s_survivalAuditTimer = 10.0f;
                AuditSurvivalStatusTokens();
            }
        }

        // Rollover-hook snapshot + witness drain. UNCONDITIONAL on purpose: it clears the
        // tracked-activator snapshot when the feature is off (an MCM toggle mid-session must
        // not leave it latched) and it is the main-thread drain point for the rollover hook's
        // log witnesses.
        Hooks::RefreshWandActivatorTargets();

        // Update activator handler (checks for cell changes and rescans)
        if (g_config.enableInteractiveActivators) {
            ActivatorHandler::GetSingleton().Update();

            // NOTE: Proxy listener registration disabled - charController is always null in VR
            // The hand pushback issue is NOT from physics collision - it's from VR hand reach limits
            // TODO: Find and hook the VR roomscale/hand reach constraint system
        }

        // ==== Node Capture Mode ====
        // Hold right thumbstick to enter capture mode.
        // While in mode, right hand shows pointing pose.
        // Hold thumbstick again to capture finger position relative to nearest activator.
        if (g_config.enableInteractiveActivators && g_config.enableActivatorDiscoveryMode) {
            NodeCaptureMode::GetSingleton().Update();
        }

        // ==== Pickpocket / Stealing ====
        // Touch NPC while sneaking + grip to steal items.
        // PARKED 2026-09-04: enablePickpocket is forced off in Config::Load().
        if (g_config.enablePickpocket) {
            PickpocketHandler::GetSingleton().Update(deltaTime);
        }

        // ==== Hand Pose Management ====
        // The FingerAnimator is the SOLE authority for hand pose during grabs:
        //   Closing: lerps fingers to grab pose, sends to FRIK each frame
        //   Holding: re-sends grab pose each frame (prevents FRIK override)
        //   Opening: lerps fingers to open (1.0), clears FRIK override when done
        //   Idle: does nothing (FRIK controller tracking handles fingers)
        // No separate per-frame grab curl code needed - FingerAnimator handles it all.

        // ==== Per-frame finger animation update ====
        {
            _leftFingerAnimator.Update(true, deltaTime);
            _rightFingerAnimator.Update(false, deltaTime);

            // Failsafe: auto-reset stuck FingerAnimator after 2 seconds if not grabbing
            bool holding = IsHoldingAnything();
            static float leftStuckTimer = 0.0f;
            static float rightStuckTimer = 0.0f;

            // Periodic logging of finger animator state
            static int failsafeLogCounter = 0;
            failsafeLogCounter++;
            if (failsafeLogCounter % 90 == 0) {  // Log every ~1 second
                spdlog::debug("[FingerAnim-STATUS] holding={} L:state={} R:state={} L:timer={:.1f}s R:timer={:.1f}s",
                             holding,
                             static_cast<int>(_leftFingerAnimator.GetState()),
                             static_cast<int>(_rightFingerAnimator.GetState()),
                             leftStuckTimer, rightStuckTimer);
            }

            if (!holding) {
                if (_leftFingerAnimator.IsActive()) {
                    leftStuckTimer += deltaTime;
                    if (leftStuckTimer > 2.0f) {
                        spdlog::warn("[FingerAnim] Left hand stuck in state {} for >2s without grab - force resetting",
                                     static_cast<int>(_leftFingerAnimator.GetState()));
                        _leftFingerAnimator.ForceReset(true);
                        leftStuckTimer = 0.0f;
                    }
                } else {
                    leftStuckTimer = 0.0f;
                }

                if (_rightFingerAnimator.IsActive()) {
                    rightStuckTimer += deltaTime;
                    if (rightStuckTimer > 2.0f) {
                        spdlog::warn("[FingerAnim] Right hand stuck in state {} for >2s without grab - force resetting",
                                     static_cast<int>(_rightFingerAnimator.GetState()));
                        _rightFingerAnimator.ForceReset(false);
                        rightStuckTimer = 0.0f;
                    }
                } else {
                    rightStuckTimer = 0.0f;
                }
            } else {
                // Reset timers while holding
                leftStuckTimer = 0.0f;
                rightStuckTimer = 0.0f;
            }
        }

        // ==== Hand Pose Debug Control ====
        // Only active in reposition mode - used to adjust finger curl for positioning
        // Hold left thumbstick click = smoothly open hands
        // Hold right thumbstick click = smoothly close hands
        auto& frik = FRIKInterface::GetSingleton();
        auto& configMode = ItemPositionConfigMode::GetSingleton();
        if (configMode.IsRepositionModeActive())
        {
            constexpr float poseSpeed = 2.0f;  // Full open/close in 0.5 seconds
            bool leftThumbPressed = g_vrInput.IsPressed(true, VRButton::ThumbstickPress);
            bool rightThumbPressed = g_vrInput.IsPressed(false, VRButton::ThumbstickPress);

            if (leftThumbPressed) {
                // Open hands (towards 1.0)
                _leftHandPoseValue += poseSpeed * deltaTime;
                if (_leftHandPoseValue > 1.0f) _leftHandPoseValue = 1.0f;
                _rightHandPoseValue += poseSpeed * deltaTime;
                if (_rightHandPoseValue > 1.0f) _rightHandPoseValue = 1.0f;
                _handPoseOverrideActive = true;
            }

            if (rightThumbPressed) {
                // Close hands (towards 0.0)
                _leftHandPoseValue -= poseSpeed * deltaTime;
                if (_leftHandPoseValue < 0.0f) _leftHandPoseValue = 0.0f;
                _rightHandPoseValue -= poseSpeed * deltaTime;
                if (_rightHandPoseValue < 0.0f) _rightHandPoseValue = 0.0f;
                _handPoseOverrideActive = true;
            }

            // Apply finger positions when override is active (using per-joint values)
            if (_handPoseOverrideActive) {
                float leftJoints[15], rightJoints[15];
                ExpandFingerToJointValues(_leftHandPoseValue, _leftHandPoseValue, _leftHandPoseValue,
                                         _leftHandPoseValue, _leftHandPoseValue, leftJoints);
                ExpandFingerToJointValues(_rightHandPoseValue, _rightHandPoseValue, _rightHandPoseValue,
                                         _rightHandPoseValue, _rightHandPoseValue, rightJoints);
                frik.SetHandPoseJointPositions(true, leftJoints);
                frik.SetHandPoseJointPositions(false, rightJoints);
                // Sync FingerAnimator so saved profiles capture the adjusted curl values
                _leftFingerAnimator.SetCurrentValues(leftJoints);
                _rightFingerAnimator.SetCurrentValues(rightJoints);
            }
        }
        PublishControllerPolicy();
    }

    void Heisenberg::OnGrabUpdate()
    {
        // POST-PHYSICS UPDATE: Runs AFTER the engine's physics step
        // This is the same timing as FRIK's main update.
        // Good for: positioning grabbed objects (our changes won't be overwritten)

        if (_modDisabled || !_initialized) {
            return;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        // NOTE: PostPhysicsGrabUpdate() is called directly from HookPostPhysics in Hooks.cpp.
        // This function (OnGrabUpdate) handles non-physics grab-related work.
        auto& grabMgr = GrabManager::GetSingleton();

        // NOTE: ProcessPendingHolster moved to HookEndUpdate (Hooks.cpp)
        // VH's displayWeapon() does cloneNode/AttachChild/loadNifFromFile which deadlocks
        // during post-physics. EndUpdate runs after ALL animation/skeleton processing,
        // so the scene graph is fully available for VH's NIF cloning.

        // Update item position config mode
        auto& itemConfigMode = ItemPositionConfigMode::GetSingleton();
        const float deltaTime = _frameDeltaSeconds;
        itemConfigMode.OnFrameUpdate(deltaTime);

        // Update drop-to-hand feature (grabs items dropped from inventory)
        auto& dropToHand = DropToHand::GetSingleton();
        dropToHand.OnFrameUpdate(deltaTime);

        // Update cooking handler (heat-based cooking with grabbed items)
        auto& cookingHandler = CookingHandler::GetSingleton();
        cookingHandler.Update(deltaTime);

        // Update Pipboy tape deck interaction (eject button, holotape insertion)
        auto& pipboyInteraction = PipboyInteraction::GetSingleton();
        pipboyInteraction.OnFrameUpdate(deltaTime);
        PublishControllerPolicy();

        // Update water interaction (ripples/splashes when VR hands touch water)
        auto& waterInteraction = WaterInteraction::GetSingleton();
        if (g_config.enableWaterInteraction) {
            auto& wCfg = waterInteraction.GetConfig();
            wCfg.enabled = g_config.enableWaterInteraction;
            wCfg.splashScale = g_config.waterSplashScale;  // was hardcoded 0.1f — now forwards the fSplashScale INI/MCM slider (default 0.1, unchanged)
            wCfg.wakeEnabled = g_config.enableWakeRipples;
            wCfg.wakeAmt = g_config.wakeRippleAmount;
            wCfg.wakeIntervalMs = g_config.wakeRippleIntervalMs;
            wCfg.wakeMinDistance = g_config.wakeMinDistance;
            wCfg.wakeMaxMultiplier = g_config.wakeMaxMultiplier;
            wCfg.enableSplashEffects = g_config.enableWaterSplashEffects;
            wCfg.splashEffectEntryMagnitude = g_config.splashEffectEntryMagnitude;
            wCfg.splashEffectExitMagnitude = g_config.splashEffectExitMagnitude;
            wCfg.enableSplashNif = g_config.enableWaterSplashNif;
            wCfg.splashNifScale = g_config.waterSplashNifScale;
            waterInteraction.Update(deltaTime);
        }

        // Smart retrieval: haptic pulse when empty hand enters storage zone
        if (g_config.enableSmartGrab) {
            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            if (playerNodes) {
                for (int h = 0; h < 2; ++h) {
                    bool isLeft = (h == 0);
                    RE::NiNode* wand = heisenberg::GetWandNode(playerNodes, isLeft);
                    if (!wand) continue;

                    const auto& gs = grabMgr.GetGrabState(isLeft);
                    bool handEmpty = !gs.active;
                    bool inZone = false;
                    if (handEmpty) {
                        auto zr = CheckItemStorageZone(wand->world.translate);
                        inZone = zr.isInZone;
                    }

                    if (inZone && !_emptyHandInStorageZone[h]) {
                        g_vrInput.TriggerHaptic(isLeft, 30000);
                    }
                    _emptyHandInStorageZone[h] = inZone;
                }
            }
        }

        // Update item insert handler (Port-A-Diner, interactive discovery mode, etc.)
        // Gated by the same master toggle as the sibling ActivatorHandler.
        if (g_config.enableInteractiveActivators) {
            ItemInsertHandler::GetSingleton().Update();
        }
    }

    bool Heisenberg::IsHoldingAnything() const
    {
        bool leftHolding = _leftHand && _leftHand->IsHolding();
        bool rightHolding = _rightHand && _rightHand->IsHolding();
        return leftHolding || rightHolding;
    }

    bool Heisenberg::IsPrimaryHandBusy() const
    {
        // Primary = weapon/grenade hand (right normally, left in left-handed mode).
        bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
        const Hand* primary = isLeftHandedMode ? _leftHand.get() : _rightHand.get();
        if (!primary) return false;
        if (primary->IsHolding()) return true;
        // Selection match: primary hand has a valid grab target
        return (isLeftHandedMode ? _hasGrabTargetLeft : _hasGrabTargetRight)
                .load(std::memory_order_relaxed);
    }

    void Heisenberg::UpdateInputSuppression()
    {
        // === PART 1: Chest pocket zone + grip = native throwables ===
        // When PRIMARY hand is in chest pocket zone AND grip is pressed, enable native throwables
        // This is simpler than the old double-tap system

        // Primary hand = right normally, left in left-handed mode
        bool isLeftHandedMode = VRInput::GetSingleton().IsLeftHandedMode();
        bool primaryHandIsLeft = isLeftHandedMode;
        bool primaryGripPressed = g_vrInput.IsPressed(primaryHandIsLeft, VRButton::Grip);

        // Check if we should enable native throwables:
        // - Primary hand must be in chest pocket zone
        // - Grip must be pressed
        bool shouldEnableThrowables = _isInChestPocketZone && primaryGripPressed;

        // Detect state changes
        if (shouldEnableThrowables && !_doubleTapHoldActive) {
            _doubleTapHoldActive = true;
            spdlog::debug("[INPUT] Primary grip in chest zone - enabling native throwables");
        }
        else if (!shouldEnableThrowables && _doubleTapHoldActive) {
            _doubleTapHoldActive = false;
            spdlog::debug("[INPUT] Left chest zone or released grip - disabling native throwables");
        }

        _lastRightGripPressed = primaryGripPressed;

        // === PART 2: Restore kFighting once unarmed is sheathed ===
        // kFighting is suppressed in trigger handler when unarmed is active.
        // Restore it once weapon is no longer drawn (sheathe completed).
        if (_postGrabFightingSuppressed) {
            bool weapDrawn2 = _cachedWeaponDrawn.load(std::memory_order_relaxed);
            if (!weapDrawn2) {
                SetInputSuppression(
                    InputSuppressionReason::PostGrabFighting,
                    false);
                spdlog::debug("Heisenberg: Restored kFighting (unarmed sheathed)");
                _postGrabFightingSuppressed = false;
            }
        }

        // === PART 2.2: Block native weapon-draw (trigger auto-ready) while holding an object ===
        // Fallout VR auto-readies/draws the equipped weapon when the trigger is pressed while
        // holstered. With an object in hand (e.g. a holotape — always held in the right/weapon
        // hand), that trigger unsheathes the weapon THROUGH the held item. The grip path is
        // already blocked (ReadyWeaponHandler hook); the trigger path is only gated by kFighting,
        // so suppress kFighting for the hold — but ONLY while the weapon is holstered. Disabling
        // fighting controls through the input-enable layer makes the engine force-sheathe a DRAWN
        // weapon (the DisablePlayerControls path), so engaging this with the weapon out holsters
        // it ~1s after every grab. The suppression's sole purpose is preventing a draw, which is
        // moot when the weapon is already drawn. Self-correcting per frame: if the weapon comes
        // out mid-hold the suppression lifts; once nothing is held we restore it (unless the
        // post-grab unarmed path still owns it, in which case that path restores it).
        {
            auto& grabMgr = heisenberg::GrabManager::GetSingleton();
            const bool holdingObject = grabMgr.IsGrabbing(true) || grabMgr.IsGrabbing(false);
            const bool weapDrawnHold = _cachedWeaponDrawn.load(std::memory_order_relaxed);
            if (holdingObject && !weapDrawnHold) {
                SetInputSuppression(
                    InputSuppressionReason::HeldObjectFighting,
                    true);
                _holdFightingSuppressed = true;
            } else if (_holdFightingSuppressed) {
                SetInputSuppression(
                    InputSuppressionReason::HeldObjectFighting,
                    false);
                spdlog::debug("Heisenberg: Restored kFighting (object released or weapon drawn — draw block lifted)");
                _holdFightingSuppressed = false;
            }
        }

        // === PART 2.5: REMOVED ===
        // We only sheathe weapon once on grab start (in HasRealWeaponEquipped).
        // After that, native behavior handles trigger to unholster.
        // No continuous forcing needed.

        // === PART 3: Always suppress Z-key (native spring mode) ===
        // kZKey = Z key grab/spring in flat mode, also mapped to A/X in VR
        // We ALWAYS want this disabled since Heisenberg handles all grabbing
        if (!_zKeySuppressed) {
            SetInputSuppression(
                InputSuppressionReason::NativeZKey,
                true);
            _zKeySuppressed = true;
            spdlog::debug("Heisenberg: Suppressed native Z-key spring mode");
        }

        // === PART 4: Suppress activation when holding objects ===
        // kActivation = A/X button activate prompt
        bool shouldSuppressActivate = IsHoldingAnything();

        if (shouldSuppressActivate && !_inputSuppressed) {
            SetInputSuppression(
                InputSuppressionReason::HeldObjectActivation,
                true);
            spdlog::debug("Heisenberg: Suppressing native activation while holding");
            _inputSuppressed = true;
        }
        else if (!shouldSuppressActivate && _inputSuppressed) {
            SetInputSuppression(
                InputSuppressionReason::HeldObjectActivation,
                false);
            spdlog::debug("Heisenberg: Restoring native activation");
            _inputSuppressed = false;
        }

        // A consumed reference disappears before this update. Without a
        // separate tail, HeldObjectActivation is cleared in that exact frame
        // and a queued Grip->Activate reaches a companion/container. Retain
        // the block for at least 500 ms and until the consuming grip is up.
        const auto now = std::chrono::steady_clock::now();
        for (const bool isLeft : { true, false }) {
            const std::size_t index = isLeft ? 0u : 1u;
            if (!_postConsumeActivationSuppressed[index].load(
                    std::memory_order_acquire)) {
                continue;
            }
            const bool minimumTailElapsed =
                now >= _postConsumeActivationMinimumUntil[index];
            const Hand* hand = isLeft ?
                _leftHand.get() : _rightHand.get();
            const bool grabInputHeld =
                hand && hand->IsGrabPressed();
            if (!post_consume_activation_policy::shouldKeepSuppressed(
                    minimumTailElapsed,
                    grabInputHeld)) {
                _postConsumeActivationSuppressed[index].store(
                    false,
                    std::memory_order_release);
            }
        }
        const bool anyPostConsumeSuppression =
            IsPostConsumeActivationSuppressed();
        SetInputSuppression(
            InputSuppressionReason::PostConsumeActivation,
            anyPostConsumeSuppression);
    }

    void Heisenberg::BeginPostConsumeActivationSuppression(
        const bool isLeft)
    {
        const std::size_t index = isLeft ? 0u : 1u;
        _postConsumeActivationMinimumUntil[index] =
            std::chrono::steady_clock::now() +
            POST_CONSUME_ACTIVATION_MINIMUM_TAIL;
        _postConsumeActivationSuppressed[index].store(
            true,
            std::memory_order_release);
        SetInputSuppression(
            InputSuppressionReason::PostConsumeActivation,
            true);
    }

    void Heisenberg::UpdateChestPocketZone()
    {
        // Check hand-to-zone distance to gate the A→Grip remap for grenades.
        // When hand is inside the zone, grenadeRemapActive becomes true in the
        // OpenVR callback, allowing held-A to inject grip for grenade readying.

        // --- Grenade cooldown after grab release ---
        // Track transition from holding→not-holding to start cooldown timer.
        // Prevents grip-hold from immediately readying a grenade after consuming/dropping an item.
        bool holdingNow = IsHoldingAnything();
        if (holdingNow) {
            _wasHoldingForGrenadeCooldown = true;
        } else if (_wasHoldingForGrenadeCooldown) {
            _lastGrabReleaseTime = std::chrono::steady_clock::now();
            _wasHoldingForGrenadeCooldown = false;
            spdlog::debug("[GRENADE] Grab released - cooldown started (0.5s)");
        }

        auto elapsed = std::chrono::steady_clock::now() - _lastGrabReleaseTime;
        bool inGrenadeCooldown = _lastGrabReleaseTime.time_since_epoch().count() > 0 &&
                                 elapsed < std::chrono::milliseconds(500);

        // Grenade zone disabled → never in-zone. The MeleeThrowHandler hook does all
        // grenade gating now; here we only maintain the _isInChestPocketZone flag it
        // reads via IsInChestPocketZone(). No fThrowDelay manipulation.
        if (g_config.throwableActivationZone == 0) {
            _isInChestPocketZone = false;
            _wasInChestPocketZone = false;
            _wasInChest = false;
            return;
        }

        // Don't treat as in-zone while holding an object or during the post-release
        // cooldown (so a grip release right after a grab can't read as "in zone").
        if (holdingNow || inGrenadeCooldown) {
            if (_wasInChestPocketZone) {
                spdlog::debug("[GRENADE] Holding/cooldown - grenade zone deactivated");
                _wasInChestPocketZone = false;
            }
            _isInChestPocketZone = false;
            return;
        }

        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->primaryWandNode || !playerNodes->SecondaryWandNode || !playerNodes->HmdNode) {
            return;
        }

        // Get PRIMARY hand (weapon hand) position
        // The game auto-swaps primaryWandNode to always track the dominant/weapon hand,
        // so primaryWandNode is correct regardless of left-handed mode.
        bool isLeftHandedMode = VRInput::GetSingleton().IsLeftHandedMode();
        RE::NiNode* weaponWand = playerNodes->primaryWandNode;
        RE::NiPoint3 handPos = weaponWand->world.translate;
        RE::NiPoint3 hmdPos = playerNodes->HmdNode->world.translate;
        RE::NiMatrix3 hmdRot = playerNodes->HmdNode->world.rotate;
        float hmdScale = playerNodes->HmdNode->world.scale;

        // Extract yaw-only rotation from HMD (same as storage zone)
        float forwardX = hmdRot.entry[0][1];
        float forwardY = hmdRot.entry[1][1];
        float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (length < 0.001f) length = 1.0f;
        forwardX /= length;
        forwardY /= length;
        float rightX = forwardY;
        float rightY = -forwardX;

        // Build yaw-only rotation matrix
        RE::NiMatrix3 yawRot;
        yawRot.entry[0][0] = rightX;   yawRot.entry[0][1] = forwardX; yawRot.entry[0][2] = 0.0f;
        yawRot.entry[1][0] = rightY;   yawRot.entry[1][1] = forwardY; yawRot.entry[1][2] = 0.0f;
        yawRot.entry[2][0] = 0.0f;     yawRot.entry[2][1] = 0.0f;     yawRot.entry[2][2] = 1.0f;

        // Helper to transform a local offset to world space using yaw-only rotation
        auto transformPoint = [&](float offsetX, float offsetY, float offsetZ) -> RE::NiPoint3 {
            RE::NiPoint3 scaled(offsetX * hmdScale, offsetY * hmdScale, offsetZ * hmdScale);
            RE::NiPoint3 rotated;
            rotated.x = yawRot.entry[0][0] * scaled.x + yawRot.entry[1][0] * scaled.y + yawRot.entry[2][0] * scaled.z;
            rotated.y = yawRot.entry[0][1] * scaled.x + yawRot.entry[1][1] * scaled.y + yawRot.entry[2][1] * scaled.z;
            rotated.z = yawRot.entry[0][2] * scaled.x + yawRot.entry[1][2] * scaled.y + yawRot.entry[2][2] * scaled.z;
            return rotated + hmdPos;
        };

        // === Calculate single throwable zone position from config ===
        RE::NiPoint3 zonePos = transformPoint(
            g_config.throwableZoneOffsetX,
            g_config.throwableZoneOffsetY,
            g_config.throwableZoneOffsetZ);

        // Calculate distance to zone
        float zoneDist = (handPos - zonePos).Length();
        float radius = g_config.throwableZoneRadius;

        // Check if we're in the zone
        bool inZone = zoneDist < radius;

        // Update zone state
        _isInChestPocketZone = (g_config.throwableActivationZone != 0) && inZone;

        // Haptic feedback on zone entry (on weapon hand's physical controller)
        if (inZone && !_wasInChest) {
            g_vrInput.TriggerHaptic(isLeftHandedMode, 40000);
        }

        // Update previous state (reusing _wasInChest for backward compat)
        _wasInChest = inZone;

        if (_isInChestPocketZone && !_wasInChestPocketZone) {
            spdlog::debug("[GRENADE] Entered chest-pocket zone");
        } else if (!_isInChestPocketZone && _wasInChestPocketZone) {
            spdlog::debug("[GRENADE] Left chest-pocket zone");
        }

        _wasInChestPocketZone = _isInChestPocketZone;
    }

    void Heisenberg::UpdateStorageZoneConfig()
    {
        // ===== ITEM STORAGE ZONE CONFIGURATION MODE =====
        // Similar to throwable zone config but for item storage zones
        // Hold A button for 1 second to enter config mode
        // Controls: L-Stick up/down = cycle zones, R-Stick up/down = adjust radius, B = save, A = exit

        if (!g_config.enableStorageZoneConfigMode) {
            return;
        }

        // Update cooldown timer
        if (_storageConfigCooldown > 0.0f) {
            _storageConfigCooldown -= _frameDeltaSeconds;
            if (_storageConfigCooldown < 0.0f) _storageConfigCooldown = 0.0f;
        }

        // Helper lambda to enable/disable player controls
        auto setPlayerControlsEnabled = [](bool enabled) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                f4cf::f4vr::SetActorRestrained(player, !enabled);
            }
            Heisenberg::GetSingleton().SetInputSuppression(
                InputSuppressionReason::StorageZoneConfig,
                !enabled);
        };

        bool aButtonPressed = g_vrInput.IsPressed(false, VRButton::A);  // A button on right controller

        if (_storageConfigModeActive) {
            // Already in config mode - handle input
            setPlayerControlsEnabled(false);

            // === TRIGGER TO SET ZONE POSITION IN REAL-TIME ===
            static bool lastTrigger = false;
            bool triggerPressed = g_vrInput.IsPressed(false, VRButton::Trigger);  // Right trigger
            if (triggerPressed && !lastTrigger) {
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                if (playerNodes && playerNodes->primaryWandNode && playerNodes->HmdNode) {
                    RE::NiPoint3 handPos = playerNodes->primaryWandNode->world.translate;
                    RE::NiPoint3 hmdPos = playerNodes->HmdNode->world.translate;
                    RE::NiMatrix3 hmdRot = playerNodes->HmdNode->world.rotate;
                    float hmdScale = playerNodes->HmdNode->world.scale;

                    // Extract yaw-only rotation from HMD to make zone body-relative
                    // (not affected by head tilt/pitch)
                    float forwardX = hmdRot.entry[0][1];  // Y column, X row
                    float forwardY = hmdRot.entry[1][1];  // Y column, Y row
                    float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
                    if (length < 0.001f) length = 1.0f;
                    forwardX /= length;
                    forwardY /= length;
                    float rightX = forwardY;
                    float rightY = -forwardX;

                    // Build yaw-only rotation matrix
                    RE::NiMatrix3 yawRot;
                    yawRot.entry[0][0] = rightX;   yawRot.entry[0][1] = forwardX; yawRot.entry[0][2] = 0.0f;
                    yawRot.entry[1][0] = rightY;   yawRot.entry[1][1] = forwardY; yawRot.entry[1][2] = 0.0f;
                    yawRot.entry[2][0] = 0.0f;     yawRot.entry[2][1] = 0.0f;     yawRot.entry[2][2] = 1.0f;

                    // Calculate local offset using yaw-only rotation (inverse/transpose).
                    // Grab.cpp's TransformPoint (the CHECK side: local -> world) uses
                    // rotated.x = e[0][0]*local.x + e[1][0]*local.y + e[2][0]*local.z (dot
                    // against COLUMNS). This capture must be its true inverse - for an
                    // orthogonal rotation matrix that's the TRANSPOSE, i.e. dot against ROWS
                    // - but this used the IDENTICAL column-dot pattern as TransformPoint, so
                    // capture and check were the SAME linear map applied twice instead of
                    // inverses: the captured local offset was rotated 180 degrees from the
                    // true inverse, placing the zone check point on the opposite side of
                    // wherever the player was facing when they set it (correct only when
                    // facing world +-Y at the moment of capture).
                    RE::NiPoint3 worldOffset = handPos - hmdPos;
                    RE::NiPoint3 localOffset;
                    localOffset.x = yawRot.entry[0][0] * worldOffset.x + yawRot.entry[0][1] * worldOffset.y + yawRot.entry[0][2] * worldOffset.z;
                    localOffset.y = yawRot.entry[1][0] * worldOffset.x + yawRot.entry[1][1] * worldOffset.y + yawRot.entry[1][2] * worldOffset.z;
                    localOffset.z = yawRot.entry[2][0] * worldOffset.x + yawRot.entry[2][1] * worldOffset.y + yawRot.entry[2][2] * worldOffset.z;
                    localOffset.x /= hmdScale;
                    localOffset.y /= hmdScale;
                    localOffset.z /= hmdScale;

                    // Set single storage zone position
                    g_config.storageZoneOffsetX = localOffset.x;
                    g_config.storageZoneOffsetY = localOffset.y;
                    g_config.storageZoneOffsetZ = localOffset.z;

                    spdlog::debug("=== STORAGE ZONE POSITION SET ===");
                    spdlog::debug("NEW POSITION: X={:.2f}, Y={:.2f}, Z={:.2f}",
                                 localOffset.x, localOffset.y, localOffset.z);

                    char msg[256];
                    snprintf(msg, sizeof(msg), "Storage Zone SET: X=%.1f Y=%.1f Z=%.1f | B=save",
                             localOffset.x, localOffset.y, localOffset.z);
                    heisenberg::ShowHUDMessage_VR(msg, nullptr, false, false);
                    g_vrInput.TriggerHaptic(false, 30000);
                }
            }
            lastTrigger = triggerPressed;

            // A button to exit (without saving)
            static bool lastA = false;
            if (_storageConfigJustEntered) {
                if (!aButtonPressed) {
                    _storageConfigJustEntered = false;
                }
                lastA = aButtonPressed;
            } else if (aButtonPressed && !lastA) {
                setPlayerControlsEnabled(true);
                heisenberg::ShowHUDMessage_VR("Storage Zone Config EXITED", nullptr, false, false);
                g_vrInput.TriggerHaptic(false, 30000);
                _storageConfigModeActive = false;
                _storageConfigCooldown = 2.0f;
                spdlog::debug("[STORAGE CONFIG] Exited without saving");
                lastA = aButtonPressed;
                return;
            }
            lastA = aButtonPressed;

            // B button to save and exit
            static bool lastB = false;
            bool bPressed = g_vrInput.IsPressed(false, VRButton::B);
            if (bPressed && !lastB) {
                g_config.Save();
                setPlayerControlsEnabled(true);
                heisenberg::ShowHUDMessage_VR("Storage zone SAVED to INI!", nullptr, false, false);
                g_vrInput.TriggerHaptic(false, 50000);
                _storageConfigModeActive = false;
                _storageConfigCooldown = 2.0f;
                spdlog::debug("[STORAGE CONFIG] Saved and exited");
                lastB = bPressed;
                return;
            }
            lastB = bPressed;

            // Left stick Y to adjust radius
            float leftStickY = g_vrInput.GetThumbstickY(true);  // Left thumbstick
            if (std::abs(leftStickY) > 0.3f) {
                g_config.itemStorageZoneRadius += leftStickY * 0.5f;
                if (g_config.itemStorageZoneRadius < 5.0f) g_config.itemStorageZoneRadius = 5.0f;
                if (g_config.itemStorageZoneRadius > 50.0f) g_config.itemStorageZoneRadius = 50.0f;

                static float radiusMsgTimer = 0.0f;
                radiusMsgTimer += 0.016f;
                if (radiusMsgTimer > 0.1f) {
                    radiusMsgTimer = 0.0f;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Storage Radius: %.0f cm", g_config.itemStorageZoneRadius * 1.4f);
                    heisenberg::ShowHUDMessage_VR(msg, nullptr, false, false);
                }
            }

        } else {
            // Not in config mode - check for activation (long press A button)
            static bool lastAEntry = false;
            if (_storageConfigCooldown > 0.0f) {
                lastAEntry = aButtonPressed;
            } else if (aButtonPressed) {
                if (!lastAEntry) {
                    _storageConfigModeHoldTimer = 0.0f;
                }
                _storageConfigModeHoldTimer += 0.016f;

                if (_storageConfigModeHoldTimer > 1.0f) {
                    _storageConfigModeActive = true;
                    _storageConfigModeHoldTimer = 0.0f;
                    _storageConfigCooldown = 2.0f;
                    _storageConfigJustEntered = true;

                    // Block controls immediately on entry
                    setPlayerControlsEnabled(false);

                    char msg[128];
                    snprintf(msg, sizeof(msg), "STORAGE ZONE CONFIG | Radius: %.0fcm | Pos: (%.0f, %.0f, %.0f)",
                             g_config.itemStorageZoneRadius * 1.4f,
                             g_config.storageZoneOffsetX, g_config.storageZoneOffsetY, g_config.storageZoneOffsetZ);
                    heisenberg::ShowHUDMessage_VR(msg, nullptr, false, false);
                    g_vrInput.TriggerHaptic(false, 50000);  // Right hand (where A button is)
                    spdlog::debug("[STORAGE CONFIG] Entered storage zone configuration mode");

                    heisenberg::ShowHUDMessage_VR("R-Trigger: set position | L-Stick: radius | B: save | A: exit", nullptr, false, false);
                }
            } else {
                _storageConfigModeHoldTimer = 0.0f;
            }
            lastAEntry = aButtonPressed;
        }
    }

    void Heisenberg::OnGrabStarted(bool isLeft)
    {
        // NOTE: Previously tracked weapon state for Unarmed/holster workarounds.
        // STUF VR mod now handles preventing Unarmed equip on grip, so this is minimal.
        spdlog::debug("[GRAB START] {} hand", isLeft ? "Left" : "Right");
    }

    void Heisenberg::OnGrabEnded(bool isLeft)
    {
        spdlog::debug("[GRAB END] {} hand - clearing FRIK override as safety net", isLeft ? "Left" : "Right");

        // CRITICAL: Always clear FRIK hand pose override on grab end.
        // Grab.cpp sets finger positions directly via FRIK API during grabs.
        // Even though EndGrab paths now call ClearHandPoseFingerPositions,
        // this is a safety net to ensure the override is ALWAYS released.
        auto& frik = FRIKInterface::GetSingleton();
        frik.ClearHandPoseFingerPositions(isLeft);

        // Also reset the FingerAnimator to Idle (in case it was somehow active)
        if (isLeft) {
            _leftFingerAnimator.ForceReset(true);
        } else {
            _rightFingerAnimator.ForceReset(false);
        }
    }

    void Heisenberg::DeactivateUnarmedForGrab()
    {
        // Only sheathe bare fists (unarmed). A real weapon must be holstered by the
        // player first — the grab is blocked in StartGrab, this is a no-op then.
        bool weapDrawn = _cachedWeaponDrawn.load(std::memory_order_relaxed);
        bool hasReal   = _cachedHasRealWeapon.load(std::memory_order_relaxed);
        if (!weapDrawn || hasReal) return;  // not in unarmed mode

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Sheathe unarmed fists so the hand opens for the grabbed item
        player->DrawWeaponMagicHands(false);
        spdlog::debug("[GRAB] Unarmed active at grab start — sheathing fists");

        // Suppress kFighting briefly so the game doesn't immediately re-equip fists
        SetInputSuppression(
            InputSuppressionReason::PostGrabFighting,
            true);
        _postGrabFightingSuppressed = true;
    }

    // =========================================================================
    // STICKY GRAB COOLDOWN - Prevents accidental re-grab after storage/release
    // =========================================================================

    void Heisenberg::StartStickyGrabCooldown(bool isLeft)
    {
        constexpr float COOLDOWN_DURATION = 1.0f;  // 1 second cooldown

        if (isLeft) {
            _leftStickyGrabCooldown = COOLDOWN_DURATION;
            spdlog::debug("[COOLDOWN] Started 1s grip cooldown for LEFT hand");
        } else {
            _rightStickyGrabCooldown = COOLDOWN_DURATION;
            spdlog::debug("[COOLDOWN] Started 1s grip cooldown for RIGHT hand");
        }
    }

    bool Heisenberg::IsInStickyGrabCooldown(bool isLeft) const
    {
        return isLeft ? (_leftStickyGrabCooldown > 0.0f) : (_rightStickyGrabCooldown > 0.0f);
    }

    void Heisenberg::UpdateStickyGrabCooldowns(float deltaTime)
    {
        if (_leftStickyGrabCooldown > 0.0f) {
            _leftStickyGrabCooldown -= deltaTime;
            if (_leftStickyGrabCooldown <= 0.0f) {
                _leftStickyGrabCooldown = 0.0f;
                spdlog::debug("[COOLDOWN] LEFT hand cooldown expired");
            }
        }

        if (_rightStickyGrabCooldown > 0.0f) {
            _rightStickyGrabCooldown -= deltaTime;
            if (_rightStickyGrabCooldown <= 0.0f) {
                _rightStickyGrabCooldown = 0.0f;
                spdlog::debug("[COOLDOWN] RIGHT hand cooldown expired");
            }
        }
    }

    void Heisenberg::UpdateCachedWeaponState()
    {
        // THREAD SAFETY: This function runs on the main thread and caches weapon state
        // so that the OpenVR callback thread can read it safely without accessing
        // game data structures that might be modified by other threads (e.g., job system)

        auto* f4vrPlayer = f4vr::getPlayer();
        if (!f4vrPlayer) {
            _cachedWeaponDrawn.store(false, std::memory_order_relaxed);
            _cachedHasRealWeapon.store(false, std::memory_order_relaxed);
            return;
        }

        // Cache weaponDrawn
        bool weaponDrawn = f4vrPlayer->GetWeaponMagicDrawn();
        const bool wasWeaponDrawn = _cachedWeaponDrawn.load(std::memory_order_relaxed);
        _cachedWeaponDrawn.store(weaponDrawn, std::memory_order_relaxed);

        // CRASH GUARD: drawing/equipping a weapon into a hand that is currently
        // holding a Heisenberg-grabbed object makes two systems co-own that hand
        // node. The keyframed held body can be orphaned or reparented out from
        // under the per-frame held-object update and fault a few frames later
        // (observed: silent crash ~3s after a weapon was drawn into the hand
        // holding a grabbed component). The weapon always occupies the PRIMARY
        // hand, so on the rising edge of "weapon drawn" release any active grab
        // on that hand. The off-hand grab (if any) is untouched.
        if (weaponDrawn && !wasWeaponDrawn) {
            auto& grabMgr = GrabManager::GetSingleton();
            const bool holdingAny = grabMgr.IsGrabbing(true) || grabMgr.IsGrabbing(false);
            if (holdingAny) {
                // User rule: a weapon must NEVER draw while the player is holding anything.
                // This used to release the primary-hand grab (which dropped the held object,
                // e.g. a holotape mid to-hand). Instead, suppress the draw by sheathing the
                // weapon immediately and KEEP the grab. Sheathing also removes the weapon+grab
                // same-hand node co-ownership that the old release was guarding against.
                spdlog::info("[GRAB] Weapon drawn while holding an object — sheathing to suppress draw (grab kept)");
                if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                    pc->DrawWeaponMagicHands(false);
                }
            }
        }

        // Cache HasRealWeaponEquipped result
        bool hasRealWeapon = HasRealWeaponEquipped();
        _cachedHasRealWeapon.store(hasRealWeapon, std::memory_order_relaxed);

        // Periodic logging of cached weapon state (every ~5 seconds at 90fps = 450 frames)
        static int cacheLogCounter = 0;
        if (++cacheLogCounter >= 450) {
            cacheLogCounter = 0;
            spdlog::debug("[WEAPON CACHE] drawn={} realWeapon={}",
                         weaponDrawn, hasRealWeapon);
        }
    }

    void Heisenberg::UpdateFingerCloseState()
    {
        // =====================================================================
        // FINGER CLOSE LOGIC (runs on main thread)
        // =====================================================================
        // Decide whether to close fingers based on grip state, selection, and weapon state.
        // This replaces the logic that was in the OpenVR callback.

        auto& grabMgr = GrabManager::GetSingleton();

        // Never close into a fist on empty grip - it looks like unarmed is activating.
        // Finger curls during actual grabs are handled by the grab system separately.
        // Just ensure both flags are always false.
        _leftHandGripHeld = false;
        _rightHandGripHeld = false;
    }

    void Heisenberg::InitHands()
    {
        _leftHand = std::make_unique<Hand>(true);
        _rightHand = std::make_unique<Hand>(false);
    }

    void Heisenberg::UpdateHands()
    {
        if (_leftHand) {
            _leftHand->Update();
        }
        if (_rightHand) {
            _rightHand->Update();
        }

        // Hand collision position updates moved to HookPlayerCharacterUpdate (pre-physics).
        // HIGGS pattern: all hand body operations happen pre-physics on the same thread.
        // Post-physics applyHardKeyFrame deadlocks against the physics thread's world lock.
    }
}

// ============================================================================
// F4SE PLUGIN VERSION DATA
// ============================================================================
// This struct is required for F4SE::log::init() to properly name the log file.
// It exports a "F4SEPlugin_Version" symbol that CommonLibF4 looks up.

extern "C" DLLEXPORT constinit F4SE::PluginVersionData F4SEPlugin_Version = []() {
    F4SE::PluginVersionData v{};
    v.PluginVersion({ Version::MAJOR, Version::MINOR, Version::PATCH, 0 });
    v.PluginName("HeisenbergF4VR");
    v.AuthorName("FeverDream");
    v.UsesAddressLibrary(true);
    v.IsLayoutDependent(true);
    v.CompatibleVersions({ F4SE::RUNTIME_VR_1_2_72 });
    return v;
}();

// F4SE plugin entry points
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    return heisenberg::g_heisenberg.OnF4SEQuery(a_f4se, a_info);
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    return heisenberg::g_heisenberg.OnF4SELoad(a_f4se);
}

// ============================================================================
// HEISENBERG API EXPORTS
// ============================================================================
// These exports allow other mods (like Virtual Holsters) to query our state.
// This enables cross-mod compatibility without modifying the other mods' code.
// They can LoadLibrary("Heisenberg_F4VR.dll") and GetProcAddress to check state.

/**
 * IsHeisenbergGrabbing - Check if Heisenberg is currently grabbing an object
 *
 * Returns true if either hand is actively holding an object.
 * Other mods should check this before triggering their own grab/holster actions.
 *
 * Usage from other mods:
 *   typedef bool (*IsHeisenbergGrabbing_t)();
 *   HMODULE hMod = GetModuleHandleA("Heisenberg_F4VR.dll");
 *   if (hMod) {
 *       auto func = (IsHeisenbergGrabbing_t)GetProcAddress(hMod, "IsHeisenbergGrabbing");
 *       if (func && func()) { ... Heisenberg is grabbing, skip holster action ... }
 *   }
 */
extern "C" DLLEXPORT bool IsHeisenbergGrabbing()
{
    return heisenberg::g_heisenberg.IsHoldingAnything();
}

/**
 * IsHeisenbergGrabbingLeft - Check if the left hand is grabbing
 */
extern "C" DLLEXPORT bool IsHeisenbergGrabbingLeft()
{
    auto& grabMgr = heisenberg::GrabManager::GetSingleton();
    return grabMgr.GetGrabState(true).active;
}

/**
 * IsHeisenbergGrabbingRight - Check if the right hand is grabbing
 */
extern "C" DLLEXPORT bool IsHeisenbergGrabbingRight()
{
    auto& grabMgr = heisenberg::GrabManager::GetSingleton();
    return grabMgr.GetGrabState(false).active;
}

/**
 * GetHeisenbergAPI - Get the full Heisenberg API interface via DLL export.
 *
 * This bypasses F4SE messaging entirely, providing direct access to the
 * IHeisenbergInterface001 pointer including all methods and callback registration.
 *
 * Usage from other mods:
 *   typedef void* (*GetHeisenbergAPI_t)(unsigned int);
 *   HMODULE hMod = GetModuleHandleA("Heisenberg_F4VR.dll");
 *   if (hMod) {
 *       auto getApi = (GetHeisenbergAPI_t)GetProcAddress(hMod, "GetHeisenbergAPI");
 *       if (getApi) {
 *           auto* iface = static_cast<HeisenbergPluginAPI::IHeisenbergInterface001*>(getApi(1));
 *           if (iface) { ... use full API ... }
 *       }
 *   }
 *
 * @param revisionNumber API revision (1 for IHeisenbergInterface001)
 * @return Pointer to the interface, or nullptr if revision not supported
 */
extern "C" DLLEXPORT void* GetHeisenbergAPI(unsigned int revisionNumber)
{
    return HeisenbergPluginAPI::GetApi(revisionNumber);
}
