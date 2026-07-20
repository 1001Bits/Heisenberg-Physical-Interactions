#include "Heisenberg.h"
#include "HandAuthority.h"
#include "FrikArmGoalHook.h"

#include <chrono>
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
#include "GrabConstraint.h"
#include "Hand.h"
#include "HeisenbergInterface001.h"
#include "Highlight.h"
#include "HandCollision.h"
#include "FingerCurves.h"
#include "WandNodeHelper.h"
#include "HeldBodyGrab.h"
#include "HandBumpHook.h"
#include "HavokTimingFix.h"
#include "rock_integration/grab/RockGrabCoreManager.h"
// ROCK integration subsystems (toggled in [RockIntegration] INI section)
#include "rock_integration/HandBoneColliderSet.h"
#include "rock_integration/BodyBoneColliderSet.h"
#include "rock_integration/WeaponCollision.h"
#include "rock_integration/TwoHandedGrip.h"
#include "HandWallPushback.h"
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

    // Public accessor (audit rank 10): ownership gates elsewhere (e.g. Hooks.cpp
    // UpdateHandCollisionBodies) must treat the EMBEDDED engine like a running ROCK.dll.
    // RockBridge::IsRunning() binds via GetModuleHandleA("ROCK.dll") and is structurally
    // always false in the embed, so gates keyed only on it never yield to the hosted engine.
    bool IsRockEngineHosted() { return s_rockEngineHosted; }

    // ---- Scope-mode exit on support-grip release (Jul 19, user request) ----
    // Covers BOTH scope systems:
    //  - BetterScopesVR zoom: state tracked from its msg-15 broadcast; exit = msg-16 toggle
    //    (only sent while its own state says zoomed, so it can never toggle zoom ON).
    //  - Vanilla ScopeMenu: tracked by MenuChecker; exit = UIMessageQueue kHide.
    static bool s_lookingThroughScope = false;

    void SetLookingThroughScope(bool a_looking)
    {
        if (s_lookingThroughScope != a_looking) {
            spdlog::debug("[SCOPE] BetterScopesVR looking-through-scope = {}", a_looking);
        }
        s_lookingThroughScope = a_looking;
    }

    void ExitScopeModeOnGripRelease()
    {
        if (s_lookingThroughScope) {
            s_lookingThroughScope = false;  // optimistic; its next msg 15 re-syncs
            if (const auto* messaging = F4SE::GetMessagingInterface()) {
                messaging->Dispatch(16, nullptr, 0, "FO4VRBETTERSCOPES");
                spdlog::info("[SCOPE] support grip released while zoomed — sent scope-mode exit toggle to BetterScopesVR");
            }
        }
        if (MenuChecker::GetSingleton().IsScopeOpen()) {
            if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                static RE::BSFixedString scopeMenuName("ScopeMenu");
                msgQueue->AddMessage(scopeMenuName, RE::UI_MESSAGE_TYPE::kHide);
                spdlog::info("[SCOPE] support grip released while vanilla ScopeMenu open — sent kHide");
            }
        }
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
            spdlog::info("[Heisenberg] API interface request from '{}'",
                a_msg->sender ? a_msg->sender : "?");
            HeisenbergPluginAPI::HandleInterfaceRequest(
                static_cast<HeisenbergPluginAPI::HeisenbergMessage*>(a_msg->data));
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
            // (e.g. RockConfig::load throwing when its embedded ROCK.ini resource is missing on a
            // clean install) so it can't std::terminate the whole DLL.
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
            // Shutdown HeldBody grab system - constraints/hand bodies become invalid
            heisenberg::HeldBodyGrabManager::GetSingleton().Shutdown();
            // Shutdown ROCK collider modules - their cached bhk/hknp world + bodies become
            // invalid on load. Without this they'd drive bodies in a freed world (UAF).
            heisenberg::rock_hand_collider::Shutdown();
            heisenberg::rock_body_collider::Shutdown();
            heisenberg::rock_weapon_collision::Shutdown();
            heisenberg::rock_grab_core::RockGrabCoreManager::GetSingleton().Shutdown();
            // Release any active FRIK hand-pushback overrides + reset tracking.
            heisenberg::hand_wall_pushback::Reset();
            heisenberg::HandAuthority::Reset();
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
            // Shutdown HeldBody grab system - constraints/hand bodies become invalid
            heisenberg::HeldBodyGrabManager::GetSingleton().Shutdown();
            // Shutdown ROCK collider modules - their cached bhk/hknp world + bodies become
            // invalid on load. Without this they'd drive bodies in a freed world (UAF).
            heisenberg::rock_hand_collider::Shutdown();
            heisenberg::rock_body_collider::Shutdown();
            heisenberg::rock_weapon_collision::Shutdown();
            heisenberg::rock_grab_core::RockGrabCoreManager::GetSingleton().Shutdown();
            // Release any active FRIK hand-pushback overrides + reset tracking.
            heisenberg::hand_wall_pushback::Reset();
            heisenberg::HandAuthority::Reset();
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

        // Default to INFO so startup/config messages are visible until
        // Config::Load() applies the user's iLogLevel setting.
        spdlog::set_level(spdlog::level::info);
        // PERF (Jul 5): flush per WARN, not per INFO — flushing on every info line is an
        // fflush write syscall per log call on the frame thread (~1000/s under load = real
        // FPS cost). WER minidumps + dmp.py/sym.py cover crash forensics; bump to info/trace
        // temporarily when chasing a CTD whose last line matters.
        spdlog::flush_on(spdlog::level::warn);

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
            const bool useRockEngine = bootIni.GetBoolValue("RockEngine", "bUseRockEngineArchitecture", false);
            if (useRockEngine) {
                spdlog::info("[RockEngine] bUseRockEngineArchitecture=1 — hosting embedded ROCK engine via rock::HostLoad()");
                if (rock::HostLoad(a_f4se)) {
                    s_rockEngineHosted = true;  // enable F4SE-message forwarding into ROCK (see OnF4SEMessage)
                    // Register the plugin-side hand-authority table so ROCK's two-handing / wall-stop
                    // route into our own hand placement when the loaded FRIK lacks the v5 API.
                    rock::HostSetHandAuthority(reinterpret_cast<const rock::HostHandAuthority*>(&heisenberg::HandAuthority::HostTable()));
                    // Jul 19 (frame-order audit): apply hand authority at the tail of ROCK's
                    // frame — after its weapon write, before the bone-tree flatten — so the
                    // rendered hand tracks the rendered weapon with zero lag.
                    rock::HostSetPostUpdateCallback(&heisenberg::HandAuthority::ApplyWinners);
                    // iGrabMode=9 (RockNativeGrab): cede grab+selection ownership to the embedded
                    // engine for the whole session (read from the boot ini — g_config loads later).
                    // The seam forces the embed's bGrabEnabled/bSelectionEnabled ON across every
                    // ROCK.ini (re)load; Heisenberg's grip handlers stand down via GetEffectiveGrabMode.
                    const long bootGrabMode = bootIni.GetLongValue("ObjectPickup", "iGrabMode", 0);
                    if (bootGrabMode == 9) {
                        rock::HostSetGrabOwnership(true);
                        spdlog::info("[RockEngine] iGrabMode=9 — embedded ROCK owns grab+selection this session");
                    }
                    spdlog::info("[RockEngine] Embedded ROCK engine load OK — driven by ROCK.ini");
                } else {
                    spdlog::error("[RockEngine] rock::HostLoad() FAILED — embedded ROCK engine disabled this session");
                }
            } else {
                spdlog::info("[RockEngine] bUseRockEngineArchitecture=0 — embedded ROCK engine dormant (default)");
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

        // Apply grip weapon draw patch based on config (like STUF VR)
        Hooks::SetGripWeaponDrawDisabled(g_config.disableGripWeaponDraw);

        // Arm the player char-proxy bump guard whenever physics hand bodies are active
        // (they generate the player-proxy bumps that fault without it), or when forced on.
        // Integration audit C4: the embedded ROCK engine spawns its OWN hand bodies (HBCS)
        // independent of Heisenberg's hand-body flags, and ROCK's own installBumpHook is a
        // dead patch (we hooked 0x1E24980 first, so its validation memcmp fails). Force our
        // guard on whenever the engine is hosted, else the char-proxy bump hits the known CTD.
        HandBumpHook::SetEnabled(g_config.rockHandBumpGuard
                                 || g_config.usePhysicsHandBodies
                                 || g_config.rockHandBoneColliderSet
                                 || s_rockEngineHosted);

        // Enable the ROCK Havok timing-fix substep override per config.
        HavokTimingFix::SetEnabled(g_config.havokTimingFix);

        // Initialize the ROCK grab-core host (idempotent). Selectable as iGrabMode=8 once
        // bUseRockGrabCore is on; harmless otherwise.
        rock_grab_core::RockGrabCoreManager::GetSingleton().Initialize();

        // Apply terminal-on-Pipboy patches based on config (after MCM settings loaded)
        Hooks::ApplyTerminalPatches(g_config.forceTerminalOnWrist);

        // Create InputEnableLayer for controlling kFighting (like FRIK/VH do)
        auto* inputManager = RE::BSInputEnableManager::GetSingleton();
        if (inputManager && !_inputLayerInitialized) {
            if (inputManager->AllocateNewLayer(_inputLayer, "HeisenbergGrab")) {
                _inputLayerID = _inputLayer->layerID;
                _inputLayerInitialized = true;
                spdlog::info("Created InputEnableLayer 'HeisenbergGrab' with ID {}", _inputLayerID);
            } else {
                spdlog::error("Failed to create InputEnableLayer");
            }
        }

        // Detect Virtual Holsters mod for compatibility mode.
        // Storage zone is a Heisenberg behavior (behind-head zone) distinct from VH's
        // hip/shoulder holster zones — keep storage-zone unequip active either way.
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
        } else {
            spdlog::warn("FRIK not available or incompatible version - Heisenberg requires FRIK 0.77+");
            spdlog::warn("Heisenberg will use fallback hand tracking (finger poses may not work)");
        }

        // Initialize constraint grab manager for physics-based grabbing
        // (Motor constraints not yet implemented - uses keyframe mode)
        auto& constraintMgr = ConstraintGrabManager::GetSingleton();
        constraintMgr.Initialize();

        // Hand collision system — physics bodies for VR hands (push/touch detection)
        if (g_config.enableHandCollision) {
            auto& handCollision = HandCollision::GetSingleton();
            if (handCollision.Initialize()) {
                spdlog::info("Hand collision initialized");
            }
        }

        // ROCK integration subsystems — each gated by its own INI toggle
        // (bRockCollisionSuppressionRegistry / bRockHandBoneColliderSet / etc.).
        // Init is harmless when toggle is off (logs and returns).
        heisenberg::rock_hand_collider::Init();
        heisenberg::rock_body_collider::Init();
        heisenberg::rock_weapon_collision::Init();
        heisenberg::rock_two_handed_grip::Init();
        // CollisionSuppressionRegistry has no init — it's lazy-constructed on first acquire.

        // HeldBody grab system — HIGGS Skyrim-style dynamic grab backend
        if (g_config.UseHeldBodyManagedGrab()) {
            auto& heldBodyMgr = HeldBodyGrabManager::GetSingleton();
            if (heldBodyMgr.Initialize()) {
                spdlog::info("HeldBody grab system initialized");
            }
        }

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
                uint64_t allowMask = 0xFFFFFFFFFFFFFFFF;  // Start with all buttons allowed

                // THREAD SAFETY LOGGING: Track callback entry for deadlock diagnosis
                // This helps identify if we got stuck inside the callback
                static std::atomic<int> callbackEntryCount{0};
                static std::atomic<int> callbackExitCount{0};
                int entryNum = ++callbackEntryCount;

                // Log every 300th callback to show we're still processing (roughly every 3-5 seconds)
                if (entryNum % 300 == 0) {
                    spdlog::debug("[CB HEARTBEAT] OpenVR callback #{} (exits: {}), hand: {}",
                                 entryNum, callbackExitCount.load(), isLeft ? "L" : "R");
                }

                // =====================================================================
                // EARLY NULL CHECKS - Singletons may not be ready during startup/shutdown
                // =====================================================================
                auto* heisenbergPtr = &Heisenberg::GetSingleton();
                if (!heisenbergPtr || !heisenbergPtr->IsInitialized()) {
                    ++callbackExitCount;
                    return allowMask;  // Not ready yet
                }
                auto& modInst = *heisenbergPtr;

                // GrabManager singleton - should always be valid if Heisenberg is initialized
                auto& grabMgr = GrabManager::GetSingleton();

                // =====================================================================
                // EITHER HAND: Track physical grip state (for main thread finger logic)
                // =====================================================================
                // Just set the atomic flag - main thread decides finger animation
                {
                    constexpr uint64_t gripMask = 1ULL << vr::k_EButton_Grip;
                    bool physicalGripPressed = (state->ulButtonPressed & gripMask) != 0;

                    if (isLeft) {
                        modInst._physicalGripPressedLeft.store(physicalGripPressed, std::memory_order_relaxed);
                    } else {
                        modInst._physicalGripPressedRight.store(physicalGripPressed, std::memory_order_relaxed);
                    }
                }

                // Get state for THIS hand
                bool thisHandGrabbing = isLeft ? grabMgr.GetGrabState(true).active
                                               : grabMgr.GetGrabState(false).active;

                // Determine if this is the primary (weapon) hand
                // Primary = right hand normally, left hand in left-handed mode
                bool isLeftHandedMode = VRInput::GetSingleton().IsLeftHandedMode();
                bool isPrimaryHand = (isLeftHandedMode ? isLeft : !isLeft);

                // Check if grenade handling is enabled at all
                // If enableGrenadeHandling=false: mod doesn't touch grenade input at all (game handles natively)
                // If enableGrenadeHandling=true: throwable state machine lower in the function decides.
                // Grenades are handled by the PRIMARY hand (weapon hand)
                bool inGrenadeZone = isPrimaryHand && modInst.IsInChestPocketZone();

                // =====================================================================
                // MENU CHECK — don't modify grip/A in menus
                // =====================================================================
                bool inMenu = false;
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->menuMode) {
                    inMenu = true;
                } else {
                    auto& menuChecker = MenuChecker::GetSingleton();
                    inMenu = menuChecker.IsPipboyOpen() ||
                             menuChecker.IsPaused() ||
                             menuChecker.IsInventoryOpen() ||
                             menuChecker.IsContainerOpen() ||
                             menuChecker.IsFavoritesOpen() ||
                             menuChecker.IsWorkshopOpen() ||
                             menuChecker.IsGameStopped();
                }

                // =====================================================================
                // BLOCK A+GRIP DURING GRAB + POST-DROP COOLDOWN
                // When this hand is actively grabbing, block A+Grip so the game
                // doesn't see button presses (important for SteamVR Grip<>A remaps).
                // After grab ends, continue blocking for 0.5s to prevent the
                // button release from activating the dropped object.
                // Skip when a menu is open — grip must pass through to close containers
                // =====================================================================
                {
                    // Per-hand post-drop cooldown (frames remaining)
                    static int s_postDropCooldownL = 0;
                    static int s_postDropCooldownR = 0;
                    static bool s_wasGrabbingL = false;
                    static bool s_wasGrabbingR = false;

                    bool& wasGrabbing = isLeft ? s_wasGrabbingL : s_wasGrabbingR;
                    int& postDropCooldown = isLeft ? s_postDropCooldownL : s_postDropCooldownR;

                    // Detect grab-end transition → start cooldown (~0.5s at 90fps)
                    if (wasGrabbing && !thisHandGrabbing) {
                        postDropCooldown = 45;  // ~0.5 seconds
                    }
                    wasGrabbing = thisHandGrabbing;

                    if (postDropCooldown > 0) {
                        postDropCooldown--;
                    }

                    bool shouldBlock = (thisHandGrabbing || postDropCooldown > 0) && !inMenu;
                    if (shouldBlock) {
                        constexpr uint64_t aButtonMask = 1ULL << vr::k_EButton_A;
                        constexpr uint64_t gripMask = 1ULL << vr::k_EButton_Grip;
                        allowMask &= ~aButtonMask;  // Block A
                        allowMask &= ~gripMask;     // Block Grip
                    }

                    // ============================================================
                    // A/X HOLD-TO-GRAB callback interceptor.
                    // X on the left controller and A on the right controller share
                    // bit 7 (vr::k_EButton_A), so the same logic handles both hands.
                    // When no target: A/X passes through for native game actions
                    // (activate, loot). When target or grabbing: hold = grab, tap =
                    // inject native action so the game still sees a quick press.
                    // ============================================================
                    bool handUsesAButton = (!isLeft && g_config.useAForRightGrab)
                                        || (isLeft  && g_config.useXForLeftGrab);
                    if (handUsesAButton && !inMenu) {
                        constexpr uint64_t axMask = 1ULL << vr::k_EButton_A;
                        constexpr float holdThreshold = 0.20f;

                        bool hasTarget = isLeft ? modInst._hasGrabTargetLeft.load(std::memory_order_relaxed)
                                                : modInst._hasGrabTargetRight.load(std::memory_order_relaxed);
                        bool shouldIntercept = hasTarget || thisHandGrabbing || postDropCooldown > 0;

                        auto& pressTime = isLeft ? modInst._cb_axGrabPressTimeL  : modInst._cb_axGrabPressTimeR;
                        auto& wasPressed = isLeft ? modInst._cb_axGrabWasPressedL : modInst._cb_axGrabWasPressedR;
                        auto& heldLong   = isLeft ? modInst._cb_axGrabHeldLongL   : modInst._cb_axGrabHeldLongR;
                        auto& injectTap  = isLeft ? modInst._cb_axGrabInjectTapL  : modInst._cb_axGrabInjectTapR;

                        if (shouldIntercept) {
                            bool axPressed = (state->ulButtonPressed & axMask) != 0;
                            bool prevPressed = wasPressed.load(std::memory_order_relaxed);
                            bool prevHeldLong = heldLong.load(std::memory_order_relaxed);

                            if (axPressed && !prevPressed) {
                                pressTime.store(Utils::GetTime(), std::memory_order_relaxed);
                                heldLong.store(false, std::memory_order_relaxed);
                            }
                            else if (axPressed && prevPressed && !prevHeldLong) {
                                double held = Utils::GetTime() - pressTime.load(std::memory_order_relaxed);
                                if (held >= holdThreshold) {
                                    heldLong.store(true, std::memory_order_relaxed);
                                }
                            }
                            else if (!axPressed && prevPressed) {
                                // Quick release before threshold = tap; queue 2 frames of injection
                                if (!prevHeldLong) {
                                    injectTap.store(2, std::memory_order_relaxed);
                                }
                                heldLong.store(false, std::memory_order_relaxed);
                            }
                            wasPressed.store(axPressed, std::memory_order_relaxed);

                            // While holding (not yet confirmed-long), block A from
                            // the game so the native activate doesn't fire during
                            // the hold-confirmation window.
                            if (axPressed && !heldLong.load(std::memory_order_relaxed) && hasTarget) {
                                allowMask &= ~axMask;
                            }
                        }

                        // Tap injection: set the A bit for 2 frames after a quick
                        // release so the game still sees the tap action.
                        int tapFrames = injectTap.load(std::memory_order_relaxed);
                        if (tapFrames > 0) {
                            constexpr uint64_t axMaskInject = 1ULL << vr::k_EButton_A;
                            state->ulButtonPressed |= axMaskInject;
                            // Don't strip via allowMask — we want the game to see it.
                            allowMask |= axMaskInject;
                            injectTap.store(tapFrames - 1, std::memory_order_relaxed);
                        }
                    }
                }

                // =====================================================================
                // BLOCK GRIP DURING STICKY GRAB COOLDOWN (per-hand)
                // =====================================================================
                // After releasing a sticky grab (DropToHand item) or storing an item,
                // there's a 1 second cooldown to prevent accidental re-grab.
                // Only blocks grip on the specific hand that triggered the cooldown.
                if (modInst.IsInStickyGrabCooldown(isLeft)) {
                    constexpr uint64_t gripMask = 1ULL << vr::k_EButton_Grip;
                    bool gripPressed = (state->ulButtonPressed & gripMask) != 0;
                    if (gripPressed) {
                        state->ulButtonPressed &= ~gripMask;  // Strip grip from state

                        // Log periodically
                        int cooldownBlockCounter = modInst._cb_cooldownBlockCounter.load(std::memory_order_relaxed);
                        if (++cooldownBlockCounter % 30 == 1) {
                            spdlog::debug("[COOLDOWN] Blocking grip on {} hand - in cooldown",
                                         isLeft ? "LEFT" : "RIGHT");
                        }
                        modInst._cb_cooldownBlockCounter.store(cooldownBlockCounter, std::memory_order_relaxed);
                    }
                }

                // =====================================================================
                // BLOCK RIGHT CONTROLLER DURING HOLOTAPE GAME PLAYBACK
                // When a game holotape SWF is active on the Pipboy, block the
                // Pipboy-operating hand (primary/weapon hand) so its laser pointer
                // can't click Pipboy tabs behind the game.
                // =====================================================================
                if (isPrimaryHand && PipboyInteraction::GetSingleton().IsProgramSWFActive()) {
                    // Zero all axes (trigger, thumbstick) so nothing registers
                    for (auto& axis : state->rAxis) {
                        axis.x = 0.0f;
                        axis.y = 0.0f;
                    }
                    allowMask = 0;  // Block all buttons
                    ++callbackExitCount;
                    return allowMask;
                }

                // =====================================================================
                // THROWABLE ACTIVATION STATE MACHINE (8 cases)
                // =====================================================================
                //   alt = bRemapGrenadeButtonToA ("Throwables Alternate Control")
                //   binding = SteamVR Grip→A binding (detected at runtime — see below)
                //
                //   Fire conditions:
                //     alt=ON  + binding=YES + physical Grip squeeze → fire (case 1)
                //     alt=ON  + binding=NO  + physical A press      → fire (case 2)
                //     alt=OFF + binding=NO  + physical Grip squeeze → fire (case 3, native)
                //     alt=OFF + binding=YES + physical A press      → fire (case 5)
                //   Block in all other combinations, including "both held simultaneously".
                //
                // Input signatures we use to distinguish physical inputs:
                //   physical Grip, NO binding   →  G=1, a>0.5  (analog grip squeezed,
                //                                              digital grip bit set)
                //   physical Grip, WITH binding →  A=1, a>0.5, G=0  (binding routes
                //                                              digital signal to A)
                //   physical A press            →  A=1, a≈0,  G=0
                //
                // Binding detection: once we observe (A=1, a>0.5, G=0), we know a
                // Grip→A binding is active. The flag is sticky for the session.
                //
                // Fallback: controllers without an analog grip axis (digital-only
                // grip) skip the analog gate entirely and use the digital A bit.
                // =====================================================================
                double aButtonPressTime = modInst._cb_aButtonPressTime.load(std::memory_order_relaxed);
                bool aButtonHeldLongEnough = modInst._cb_aButtonHeldLongEnough.load(std::memory_order_relaxed);
                bool aButtonWasPressed = modInst._cb_aButtonWasPressed.load(std::memory_order_relaxed);
                bool gripABindingDetected = modInst._cb_gripABindingDetected.load(std::memory_order_relaxed);

                // Forced arm from a VH unholster (kMessage_ArmThrowable): inject grip
                // for a few frames so the just-equipped throwable arms instantly,
                // skipping the hold-to-arm delay.
                {
                    int forceFrames = modInst._cb_forceArmThrowableFrames.load(std::memory_order_relaxed);
                    if (forceFrames > 0) {
                        aButtonHeldLongEnough = true;
                        modInst._cb_forceArmThrowableFrames.store(forceFrames - 1, std::memory_order_relaxed);
                    }
                }

                const float holdThresholdSeconds = g_config.throwableHoldDuration;

                // Snapshot input signals once
                constexpr uint64_t aButtonMaskTW = 1ULL << vr::k_EButton_A;
                constexpr uint64_t gripMaskTW   = 1ULL << vr::k_EButton_Grip;
                bool aBitTW = (state->ulButtonPressed & aButtonMaskTW) != 0;
                bool gBitTW = (state->ulButtonPressed & gripMaskTW)   != 0;
                const auto& vri = VRInput::GetSingleton();
                float analogTW = vri.GetGripValue(isLeft);
                bool hasAnalogTW = vri.HasAnalogGrip(isLeft);
                bool gripSqueezed = hasAnalogTW && analogTW > 0.5f;

                // Detect Grip→A binding. Per-frame check; set sticky flag once seen.
                if (isPrimaryHand && hasAnalogTW && aBitTW && !gBitTW && gripSqueezed) {
                    if (!gripABindingDetected) {
                        gripABindingDetected = true;
                        modInst._cb_gripABindingDetected.store(true, std::memory_order_relaxed);
                        spdlog::info("[GRENADE] Detected SteamVR Grip→A binding (sticky)");
                    }
                }

                // Classify the current physical input on the primary hand
                bool isGripPhysicalNoBinding   = isPrimaryHand && gBitTW && gripSqueezed;
                bool isGripPhysicalWithBinding = isPrimaryHand && aBitTW && !gBitTW && gripSqueezed;
                bool isAPhysical               = isPrimaryHand && aBitTW && !gBitTW
                                                  && (!hasAnalogTW || analogTW <= 0.5f);
                bool bothBitsHeld              = isPrimaryHand && aBitTW && gBitTW;

                // Decide if the throwable activator is currently pressed.
                // The alt toggle is a *functional* "use this button" choice — NOT
                // a SteamVR-binding proxy (per saved feedback). So:
                //   altOn  = true  → throwable fires on physical A button press
                //   altOn  = false → throwable fires on physical Grip squeeze
                // The Grip→A binding is handled transparently: when active, a
                // physical grip squeeze produces (A=1, !G, analog>0.5) which is
                // accepted as a grip squeeze; without the binding it produces
                // (G=1, analog>0.5). Either signature counts as "physical grip".
                bool altOn = g_config.remapGrenadeButtonToA && g_config.enableGrenadeHandling;
                bool activatorPressed = false;
                if (isPrimaryHand && !bothBitsHeld) {
                    // alt = "use the *alternate* button compared to your default".
                    // A SteamVR Grip→A binding inverts what counts as default:
                    //
                    //                    | binding active | no binding
                    //   alt=OFF (default)| A press        | grip squeeze
                    //   alt=ON  (alt)    | grip squeeze   | A press
                    //
                    // With the binding active grip already routes to A, so "stock"
                    // becomes A press (alt=OFF) and "alternate" becomes literal grip
                    // squeeze distinguished via the analog axis (alt=ON). Without the
                    // binding, stock is grip (Bethesda default) and alternate is A.
                    if (gripABindingDetected) {
                        activatorPressed = altOn
                            ? isGripPhysicalWithBinding   // physical grip squeezed (analog>0.5)
                            : isAPhysical;                // physical A pressed (analog≤0.5)
                    } else {
                        activatorPressed = altOn
                            ? isAPhysical                 // physical A pressed
                            : isGripPhysicalNoBinding;    // physical grip squeezed
                    }
                }

                // Mod handles the timing+injection in every case EXCEPT
                // "alt=OFF, no binding" (native game uses fThrowDelay there).
                bool modHandlesThrowable = isPrimaryHand && g_config.enableGrenadeHandling
                    && (g_config.throwableActivationZone == 0 || inGrenadeZone)
                    && (altOn || gripABindingDetected);

                if (modHandlesThrowable && !thisHandGrabbing) {
                    if (activatorPressed && !aButtonWasPressed) {
                        aButtonPressTime = Utils::GetTime();
                        aButtonHeldLongEnough = false;
                        spdlog::info("[GRENADE] Activator press DOWN (alt={} binding={} A={} G={} analog={:.2f}) — timer started, threshold={:.2f}s",
                                     altOn, gripABindingDetected, isAPhysical, isGripPhysicalNoBinding || isGripPhysicalWithBinding, analogTW, holdThresholdSeconds);
                    } else if (activatorPressed && aButtonWasPressed) {
                        double now = Utils::GetTime();
                        float heldSeconds = static_cast<float>(now - aButtonPressTime);
                        if (heldSeconds >= holdThresholdSeconds && !aButtonHeldLongEnough) {
                            aButtonHeldLongEnough = true;
                            spdlog::info("[GRENADE] Hold threshold reached ({:.2f}s) — injecting grip on next frame",
                                         heldSeconds);
                        }
                    } else if (!activatorPressed && aButtonWasPressed) {
                        aButtonHeldLongEnough = false;
                        spdlog::info("[GRENADE] Activator released before threshold");
                    } else if (!activatorPressed) {
                        aButtonHeldLongEnough = false;
                    }
                    aButtonWasPressed = activatorPressed;
                } else if (isPrimaryHand) {
                    // Diagnostic: log why we're NOT running the throwable timer.
                    static double lastSkipLog = 0.0;
                    double now = Utils::GetTime();
                    if (now - lastSkipLog > 5.0 && (aBitTW || gBitTW)) {
                        spdlog::info("[GRENADE] Timer skipped: modHandles={} grabbing={} (zone={} inZone={} altOn={} bindingDet={} enableGren={})",
                                     modHandlesThrowable, thisHandGrabbing,
                                     g_config.throwableActivationZone, inGrenadeZone,
                                     altOn, gripABindingDetected, g_config.enableGrenadeHandling);
                        lastSkipLog = now;
                    }
                }

                constexpr uint64_t gripMask = 1ULL << vr::k_EButton_Grip;

                // THROWABLE continuous grip-STRIP removed — grenade readying is now controlled at the
                // GAME level (HookMeleeThrowOnButtonEvent, [ObjectPickup] bUseGrenadeReadyHook), so we
                // no longer strip grip from the input stream every frame (that broke VirtualHolsters
                // equip/holster, grip-reload, and mod secondary actions).
                //
                // A-LONG-PRESS GRIP INJECT KEPT (remap-to-A): when the grenade button is remapped to A
                // and the user holds A long enough, synthesize a grip press so the game's throw event
                // fires (the game hook then lets it through for the remap-to-A path). This fires only on
                // a deliberate A-long-press, not continuously, so it doesn't disturb VH/reload/secondary.
                if (modHandlesThrowable && !inMenu && aButtonHeldLongEnough) {
                    state->ulButtonPressed |= gripMask;
                    state->ulButtonPressed &= ~aButtonMaskTW;
                }

                // =====================================================================
                // ROCK DYNAMIC HANDOFF — synthetic grip release->press edge for ROCK
                // =====================================================================
                // After Heisenberg places a grabbed object and hands it to ROCK, force
                // grip OFF for a few frames on the handoff hand. ROCK (edge-triggered)
                // sees release; once these frames elapse the real (still-held) grip
                // forms the press edge ROCK needs to grab the placed object. The suppress
                // counter (decremented here once/frame for this hand) blocks Heisenberg
                // re-grabbing on that same edge.
                {
                    int hoHand = modInst._rockHandoffHand.load(std::memory_order_relaxed);
                    if (hoHand == (isLeft ? 1 : 0)) {
                        int off = modInst._rockHandoffGripOffFrames.load(std::memory_order_relaxed);
                        if (off > 0) {
                            // Release phase: force grip OFF so ROCK sees a release.
                            state->ulButtonPressed &= ~gripMask;
                            modInst._rockHandoffGripOffFrames.store(off - 1, std::memory_order_relaxed);
                        } else {
                            int on = modInst._rockHandoffGripOnFrames.load(std::memory_order_relaxed);
                            if (on > 0) {
                                // Press phase: force grip ON. This OVERRIDES the primary-hand
                                // grip strip above (which otherwise hides grip from ROCK on the
                                // right hand), giving ROCK a clean press edge to grab on.
                                state->ulButtonPressed |= gripMask;
                                state->ulButtonPressed &= ~aButtonMaskTW;
                                modInst._rockHandoffGripOnFrames.store(on - 1, std::memory_order_relaxed);
                                if (on - 1 == 0) {
                                    spdlog::info("[ROCK-HANDOFF] forced press window done ({} hand) — ROCK should have grabbed", isLeft ? "L" : "R");
                                }
                            }
                        }
                        int sup = modInst._rockHandoffSuppressGrabFrames.load(std::memory_order_relaxed);
                        if (sup > 0) {
                            modInst._rockHandoffSuppressGrabFrames.store(sup - 1, std::memory_order_relaxed);
                            if (sup - 1 == 0) {
                                modInst._rockHandoffHand.store(-1, std::memory_order_relaxed);
                            }
                        }
                    }
                }

                // (Removed redundant "block grip while grabbing": the A+Grip block earlier
                // already masks grip for the grabbing hand via allowMask when thisHandGrabbing,
                // so this duplicated it; its grenade-while-grabbing grip re-inject was also
                // defeated by that same allowMask strip. Grenade-while-grabbing is now the game
                // hook's concern.)

                // Write back A-button state to atomics (PRIMARY hand only)
                if (isPrimaryHand) {
                    modInst._cb_aButtonPressTime.store(aButtonPressTime, std::memory_order_relaxed);
                    modInst._cb_aButtonHeldLongEnough.store(aButtonHeldLongEnough, std::memory_order_relaxed);
                    modInst._cb_aButtonWasPressed.store(aButtonWasPressed, std::memory_order_relaxed);
                }

                // THREAD SAFETY LOGGING: Track callback exit
                ++callbackExitCount;

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

        // NOTE: grenade gating is now done entirely by the MeleeThrowHandler hook
        // (Hooks.cpp), which caps the button hold-duration below the throw threshold
        // when readying should be blocked. fThrowDelay:Controls is left at the game
        // default — the old system's 999999 override is gone (it permanently disabled
        // throwing once the zone was enabled).

        InitHands();
        _initialized = true;
        spdlog::info("Heisenberg initialized");
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
    }

    void Heisenberg::ProcessPendingWeaponUnequip()
    {
        if (!_pendingUnequipForm) return;

        auto* form = _pendingUnequipForm;
        auto name = std::move(_pendingUnequipName);
        _pendingUnequipForm = nullptr;
        _pendingUnequipName.clear();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Use ActorEquipManager::UnequipObject with queueEquip=true
        // so the game defers the actual unequip to a safe phase.
        // The raw UnEquipItem(0xe707b0) crashes even in EndUpdate context.
        RE::ActorEquipManager** equipMgrPtr = heisenberg::g_ActorEquipManager.get();
        if (!equipMgrPtr || !*equipMgrPtr) {
            spdlog::error("[GRAB] ActorEquipManager not available for weapon unequip");
            return;
        }

        RE::ActorEquipManager* equipMgr = *equipMgrPtr;

        // Construct BGSObjectInstance (same pattern as armor equip in Grab.cpp)
        struct LocalObjectInstance {
            RE::TESForm* object{ nullptr };
            RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
        };
        static_assert(sizeof(LocalObjectInstance) == 0x10);

        LocalObjectInstance instance;
        instance.object = form;
        instance.instanceData = nullptr;

        heisenberg::ActorEquipManager_UnequipObject(
            reinterpret_cast<std::uint64_t>(equipMgr),
            reinterpret_cast<RE::Actor*>(player),
            reinterpret_cast<RE::BGSObjectInstance*>(&instance),
            1,          // number
            nullptr,    // slot - let game determine
            -1,         // stackID - any stack
            false,      // queueEquip - apply now (we're in EndUpdate, safe context)
            true,       // playSounds - plays native unequip sound
            true,       // applyNow - process immediately so sound plays and state updates
            false,      // locked
            nullptr     // slotBeingReplaced
        );

        // Remember this weapon for re-equip on next grip in storage zone
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

        // MAIN THREAD HEARTBEAT: Log every ~5 seconds to verify main thread is running.
        // If deadlock occurs, this will stop appearing while OpenVR callback heartbeat continues.
        static int mainThreadHeartbeat = 0;
        static uint64_t mainThreadTotalFrames = 0;
        ++mainThreadTotalFrames;
        if (++mainThreadHeartbeat >= 450) {  // ~5 seconds at 90fps
            mainThreadHeartbeat = 0;
            spdlog::debug("[MAIN THREAD HEARTBEAT] OnInputUpdate running, total frames={}", mainThreadTotalFrames);
        }

        // NOTE: kFighting toggle code removed - was causing issues

        // Check for MCM settings changes (throttled internally to every 2 seconds)
        g_config.ReloadIfMCMChanged();

        // Sync grip weapon draw patch with current config (no-op if unchanged)
        Hooks::SetGripWeaponDrawDisabled(g_config.disableGripWeaponDraw);

        // HUD rollover button hiding is now handled entirely by vtable hooks
        // (NullRolloverButtons / NullAllRolloverFields). Binary patches are no longer
        // used — they caused an asymmetry where the right wand's ShowRollover
        // implementation would suppress item names when ShowActivateButton was RET'd.
        Hooks::ApplyHUDRolloverButtonPatches(false);

        // Update sticky grab cooldowns (1 second cooldown after storage/release)
        // Uses approximate delta time based on 90fps VR headset
        constexpr float deltaTime = 1.0f / 90.0f;
        UpdateStickyGrabCooldowns(deltaTime);
        UpdateWeaponUnequipCooldown(deltaTime);

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
                    auto* inputMgr = RE::BSInputEnableManager::GetSingleton();
                    if (inputMgr) {
                        inputMgr->ForceUserEventEnabled(RE::UEFlag::kFighting, false);
                        _postGrabFightingSuppressed = true;
                        spdlog::debug("[INPUT] Suppressed kFighting (unarmed trigger press)");
                    }
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
        // Touch NPC while sneaking + grip to steal items
        {
            constexpr float pickpocketDelta = 1.0f / 90.0f;
            PickpocketHandler::GetSingleton().Update(pickpocketDelta);
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
            constexpr float fingerDeltaTime = 1.0f / 90.0f;
            _leftFingerAnimator.Update(true, fingerDeltaTime);
            _rightFingerAnimator.Update(false, fingerDeltaTime);

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
                    leftStuckTimer += fingerDeltaTime;
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
                    rightStuckTimer += fingerDeltaTime;
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
            constexpr float poseDeltaTime = 1.0f / 90.0f;

            bool leftThumbPressed = g_vrInput.IsPressed(true, VRButton::ThumbstickPress);
            bool rightThumbPressed = g_vrInput.IsPressed(false, VRButton::ThumbstickPress);

            if (leftThumbPressed) {
                // Open hands (towards 1.0)
                _leftHandPoseValue += poseSpeed * poseDeltaTime;
                if (_leftHandPoseValue > 1.0f) _leftHandPoseValue = 1.0f;
                _rightHandPoseValue += poseSpeed * poseDeltaTime;
                if (_rightHandPoseValue > 1.0f) _rightHandPoseValue = 1.0f;
                _handPoseOverrideActive = true;
            }

            if (rightThumbPressed) {
                // Close hands (towards 0.0)
                _leftHandPoseValue -= poseSpeed * poseDeltaTime;
                if (_leftHandPoseValue < 0.0f) _leftHandPoseValue = 0.0f;
                _rightHandPoseValue -= poseSpeed * poseDeltaTime;
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
        constexpr float deltaTime = 1.0f / 90.0f;  // Assume 90fps VR
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
        auto* inputManager = RE::BSInputEnableManager::GetSingleton();
        if (_postGrabFightingSuppressed) {
            bool weapDrawn2 = _cachedWeaponDrawn.load(std::memory_order_relaxed);
            if (!weapDrawn2) {
                if (inputManager) {
                    inputManager->ForceUserEventEnabled(RE::UEFlag::kFighting, true);
                    spdlog::debug("Heisenberg: Restored kFighting (unarmed sheathed)");
                }
                _postGrabFightingSuppressed = false;
            }
        }

        // === PART 2.2: Block native weapon-draw (trigger auto-ready) while holding an object ===
        // Fallout VR auto-readies/draws the equipped weapon when the trigger is pressed while
        // holstered. With an object in hand (e.g. a holotape — always held in the right/weapon
        // hand), that trigger unsheathes the weapon THROUGH the held item. The grip path is
        // already blocked (ReadyWeaponHandler hook); the trigger path is only gated by kFighting,
        // so suppress kFighting for the whole hold. Forced every frame while holding (overrides
        // any stray re-enable) and self-correcting: once nothing is held we restore it (unless the
        // post-grab unarmed path still owns it, in which case that path restores it).
        {
            auto& grabMgr = heisenberg::GrabManager::GetSingleton();
            const bool holdingObject = grabMgr.IsGrabbing(true) || grabMgr.IsGrabbing(false);
            if (holdingObject) {
                if (inputManager) {
                    inputManager->ForceUserEventEnabled(RE::UEFlag::kFighting, false);
                }
                _holdFightingSuppressed = true;
            } else if (_holdFightingSuppressed) {
                if (inputManager && !_postGrabFightingSuppressed) {
                    inputManager->ForceUserEventEnabled(RE::UEFlag::kFighting, true);
                    spdlog::debug("Heisenberg: Restored kFighting (object released — weapon draw re-enabled)");
                }
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
        // Reuse inputManager from PART 2
        if (inputManager && !_zKeySuppressed) {
            inputManager->ForceOtherEventEnabledVR(RE::OEFlag::kZKey, false);
            _zKeySuppressed = true;
            spdlog::debug("Heisenberg: Suppressed native Z-key spring mode");
        }

        // === PART 4: Suppress activation when holding objects ===
        // kActivation = A/X button activate prompt
        bool shouldSuppressActivate = IsHoldingAnything();

        if (shouldSuppressActivate && !_inputSuppressed) {
            if (inputManager) {
                inputManager->ForceOtherEventEnabledVR(RE::OEFlag::kActivation, false);
                spdlog::debug("Heisenberg: Suppressing native activation while holding");
            }
            _inputSuppressed = true;
        }
        else if (!shouldSuppressActivate && _inputSuppressed) {
            if (inputManager) {
                inputManager->ForceOtherEventEnabledVR(RE::OEFlag::kActivation, true);
                spdlog::debug("Heisenberg: Restoring native activation");
            }
            _inputSuppressed = false;
        }
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
            _storageConfigCooldown -= 0.016f;
            if (_storageConfigCooldown < 0.0f) _storageConfigCooldown = 0.0f;
        }

        // Helper lambda to enable/disable player controls
        auto setPlayerControlsEnabled = [](bool enabled) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                f4cf::f4vr::SetActorRestrained(player, !enabled);
            }

            auto* inputManager = RE::BSInputEnableManager::GetSingleton();
            if (inputManager) {
                using UEFlag = RE::UserEvents::USER_EVENT_FLAG;
                // NOTE: kFighting intentionally excluded — always suppressed to prevent unarmed
                UEFlag flagsToControl = UEFlag::kMainFour | UEFlag::kVATS | UEFlag::kActivate | UEFlag::kMenu;
                inputManager->ForceUserEventEnabled(flagsToControl, enabled);

                using OEFlag = RE::OtherInputEvents::OTHER_EVENT_FLAG;
                OEFlag otherFlags = OEFlag::kVATS | OEFlag::kActivation | OEFlag::kFavorites;
                inputManager->ForceOtherEventEnabledVR(otherFlags, enabled);
            }
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

                    // Calculate local offset using yaw-only rotation (inverse/transpose)
                    RE::NiPoint3 worldOffset = handPos - hmdPos;
                    RE::NiPoint3 localOffset;
                    localOffset.x = yawRot.entry[0][0] * worldOffset.x + yawRot.entry[1][0] * worldOffset.y + yawRot.entry[2][0] * worldOffset.z;
                    localOffset.y = yawRot.entry[0][1] * worldOffset.x + yawRot.entry[1][1] * worldOffset.y + yawRot.entry[2][1] * worldOffset.z;
                    localOffset.z = yawRot.entry[0][2] * worldOffset.x + yawRot.entry[1][2] * worldOffset.y + yawRot.entry[2][2] * worldOffset.z;
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
        auto* inputMgr = RE::BSInputEnableManager::GetSingleton();
        if (inputMgr) {
            inputMgr->ForceUserEventEnabled(RE::UEFlag::kFighting, false);
            _postGrabFightingSuppressed = true;
        }
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
