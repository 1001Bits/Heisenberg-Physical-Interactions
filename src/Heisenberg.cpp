#include "Heisenberg.h"

#include <chrono>
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
#include "Hooks.h"
#include "ItemInsertHandler.h"
#include "ItemOffsets.h"
#include "ItemPositionConfigMode.h"
#include "MenuChecker.h"
#include "NodeCaptureMode.h"
#include "OpenVRHook.h"
#include "PickpocketHandler.h"
#include "PipboyInteraction.h"
#include "WaterInteraction.h"
#include "PlayerCharacterProxyListener.h"
#include "SmartGrabHandler.h"
#include "Utils.h"
#include "VRInput.h"
#include "WandNodeHelper.h"

#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"

namespace heisenberg
{
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
    static void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) {
            return;
        }

        // Check for API interface request from other plugins
        if (a_msg->type == HeisenbergPluginAPI::HeisenbergMessage::kMessage_GetInterface)
        {
            spdlog::info("[Heisenberg] Received API interface request from plugin");
            HeisenbergPluginAPI::HandleInterfaceRequest(
                static_cast<HeisenbergPluginAPI::HeisenbergMessage*>(a_msg->data));
            return;
        }

        switch (a_msg->type) {
        case F4SE::MessagingInterface::kGameLoaded:
            spdlog::info("Game loaded, initializing Heisenberg...");
            ForceWaterDisplacementEnabled();
            spdlog::info("[Water] Forced displacement settings at kGameLoaded");
            g_heisenberg.OnGameLoad();
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
            // Clear hand collision contacts - refs become invalid
            heisenberg::HandCollision::GetSingleton().ClearContacts();
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
            // Unregister proxy listener before player is unloaded
            heisenberg::PlayerCharacterProxyListener::GetSingleton().UnregisterFromPlayer();
            // Clear last-unequipped weapon tracking - form becomes invalid
            heisenberg::g_heisenberg.ClearLastUnequippedWeapon();
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
            // Clear hand collision contacts - refs become invalid
            heisenberg::HandCollision::GetSingleton().ClearContacts();
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
            // Intro holotape: on new game, wait for vault exit; on load, deliver after 3s
            if (a_msg->type == F4SE::MessagingInterface::kNewGame) {
                heisenberg::PipboyInteraction::GetSingleton().SetNewGame();
            } else {
                heisenberg::PipboyInteraction::GetSingleton().QueueIntroHolotapeDelivery();
            }
            // Clear hand references to prevent dangling pointers
            g_heisenberg.ClearHandStates();
            // Reset grenade zone/callback state after load
            g_heisenberg.ReapplyThrowDelay();
            break;
        }
    }

    bool Heisenberg::OnF4SEQuery(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
    {
        // Setup logging using F4SE's logger initialization
        // This uses F4SEPlugin_Version to get the log file name ("Heisenberg_F4VR.log")
        F4SE::log::init();

        // Set to ERROR for release - config can override via iLogLevel
        spdlog::set_level(spdlog::level::err);
        spdlog::flush_on(spdlog::level::err);

        // Set cleaner log pattern: [time] [Level] message (no thread ID)
        spdlog::set_pattern("[%T.%e] [%L] %v");

        spdlog::info("Heisenberg F4VR v{}.{}.{}", Version::MAJOR, Version::MINOR, Version::PATCH);

        // Fill plugin info
        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = "Heisenberg_F4VR";
        a_info->version = Version::MAJOR;

        // Check runtime
        if (a_f4se->IsEditor()) {
            spdlog::critical("Loaded in editor, aborting");
            return false;
        }

        const auto ver = a_f4se->RuntimeVersion();
        if (ver < F4SE::RUNTIME_VR_1_2_72) {
            spdlog::critical("Unsupported runtime version {}", ver.string());
            return false;
        }

        return true;
    }

    bool Heisenberg::OnF4SELoad(const F4SE::LoadInterface* a_f4se)
    {
        spdlog::info("Heisenberg F4VR loading...");

        F4SE::Init(a_f4se);

        // Get messaging interface
        _messaging = F4SE::GetMessagingInterface();
        if (!_messaging) {
            spdlog::critical("Failed to get messaging interface");
            return false;
        }

        // Register message listener
        if (!_messaging->RegisterListener(OnF4SEMessage)) {
            spdlog::critical("Failed to register message listener");
            return false;
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

        // Detect Virtual Holsters mod for compatibility mode
        HMODULE vhModule = GetModuleHandleA("VirtualHolsters.dll");
        if (vhModule) {
            _virtualHolstersDetected = true;
            spdlog::info("Virtual Holsters detected - VH compatibility mode active");

            // Auto-disable storage zone weapon equip when VH is present (VH handles holstering)
            // Only override if user hasn't explicitly set it in INI or MCM
            {
                bool explicitlySet = false;
                CSimpleIniA checkIni;
                checkIni.SetUnicode();
                if (checkIni.LoadFile("Data/F4SE/Plugins/Heisenberg_F4VR.ini") >= 0) {
                    if (checkIni.GetValue("ItemStorage", "bEnableStorageZoneWeaponEquip"))
                        explicitlySet = true;
                }
                if (!explicitlySet) {
                    CSimpleIniA checkMcm;
                    checkMcm.SetUnicode();
                    if (checkMcm.LoadFile("Data/MCM/Settings/Heisenberg.ini") >= 0) {
                        if (checkMcm.GetValue("ItemStorage", "bEnableStorageZoneWeaponEquip"))
                            explicitlySet = true;
                    }
                }
                if (!explicitlySet) {
                    g_config.enableStorageZoneWeaponEquip = false;
                    spdlog::debug("Storage zone weapon equip auto-disabled (Virtual Holsters detected)");
                }
            }
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

        // Hand collision system - DISABLED for now, will implement later
        // auto& handCollision = HandCollision::GetSingleton();
        // if (handCollision.Initialize()) {
        //     spdlog::info("Hand collision initialized");
        // }

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
                // If enableGrenadeHandling=true: check remap and zone settings
                // Grenades are handled by the PRIMARY hand (weapon hand)
                bool inGrenadeZone = isPrimaryHand && modInst.IsInChestPocketZone();
                bool grenadeRemapActive = isPrimaryHand && g_config.enableGrenadeHandling &&
                    g_config.remapGrenadeButtonToA &&
                    (g_config.throwableActivationZone == 0 || inGrenadeZone);

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
                // GRENADE REMAP: Hold A for 0.3s → Grip for native grenades
                // =====================================================================
                // When remapGrenadeButtonToA is enabled:
                // - Quick tap A = normal A function (looting, activating)
                // - Hold A for 0.3s = inject Grip (ready grenade)
                // - Physical Grip is blocked from grenades (but Heisenberg still sees it via unfiltered input)
                // This allows A to work normally while also supporting grenades
                double aButtonPressTime = modInst._cb_aButtonPressTime.load(std::memory_order_relaxed);
                bool aButtonHeldLongEnough = modInst._cb_aButtonHeldLongEnough.load(std::memory_order_relaxed);
                bool aButtonWasPressed = modInst._cb_aButtonWasPressed.load(std::memory_order_relaxed);

                if (grenadeRemapActive && !thisHandGrabbing) {
                    constexpr uint64_t aButtonMask = 1ULL << vr::k_EButton_A;
                    constexpr float holdThresholdSeconds = 0.3f;

                    bool aPressed = (state->ulButtonPressed & aButtonMask) != 0;

                    if (aPressed && !aButtonWasPressed) {
                        // A just pressed - start timing
                        aButtonPressTime = Utils::GetTime();
                        aButtonHeldLongEnough = false;
                    }
                    else if (aPressed && aButtonWasPressed) {
                        // A still held - check duration
                        double now = Utils::GetTime();
                        float heldSeconds = static_cast<float>(now - aButtonPressTime);

                        if (heldSeconds >= holdThresholdSeconds) {
                            aButtonHeldLongEnough = true;
                        }
                    }
                    else if (!aPressed) {
                        // A released - reset state
                        aButtonHeldLongEnough = false;
                    }

                    aButtonWasPressed = aPressed;

                    // NOTE: Grip injection happens below after we strip physical grip
                }

                constexpr uint64_t gripMask = 1ULL << vr::k_EButton_Grip;
                bool gripPressed = (state->ulButtonPressed & gripMask) != 0;

                // =====================================================================
                // A→GRIP INJECTION: Inject grip when A is held long enough
                // =====================================================================
                // Strip A so the game doesn't see both A (activate) and Grip,
                // which would cause the grenade to ready then immediately drop.
                if (grenadeRemapActive && !inMenu) {
                    if (aButtonHeldLongEnough) {
                        state->ulButtonPressed |= gripMask;
                        constexpr uint64_t aButtonMask = 1ULL << vr::k_EButton_A;
                        state->ulButtonPressed &= ~aButtonMask;
                    }
                }
                // SUSTAIN GRIP after leaving zone: keep injecting grip until A is released
                // so a mid-throw grenade doesn't drop when hand exits the zone.
                else if (!grenadeRemapActive && !inMenu && isPrimaryHand && !thisHandGrabbing && aButtonHeldLongEnough) {
                    constexpr uint64_t aButtonMask = 1ULL << vr::k_EButton_A;
                    bool aPressed = (state->ulButtonPressed & aButtonMask) != 0;
                    if (aPressed) {
                        state->ulButtonPressed |= gripMask;
                        state->ulButtonPressed &= ~aButtonMask;
                    } else {
                        aButtonHeldLongEnough = false;
                        aButtonWasPressed = false;
                    }
                }

                // =====================================================================
                // BLOCK GRIP WHILE ACTIVELY GRABBING AN OBJECT
                // =====================================================================
                // Prevents the game from processing grip as weapon draw/grenade while
                // carrying. Heisenberg uses GetControllerStateUnfiltered so it still sees grip.
                if (!inMenu && gripPressed && thisHandGrabbing) {
                    state->ulButtonPressed &= ~gripMask;

                    // Re-inject grip from A for PRIMARY hand (grenade while grabbing)
                    if (isPrimaryHand && aButtonHeldLongEnough) {
                        state->ulButtonPressed |= gripMask;
                    }
                }

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

            // NOTE: PlayerCharacterProxyListener disabled — charController is always null in VR
            // and it causes movement issues on some headsets (Pico). The hand pushback issue
            // is from VR roomscale/hand reach constraints, not physics collision.
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

        // Gate grenades via fThrowDelay: 999999 outside zone (disabled), 0.3 inside (enabled).
        // When zone is disabled (0): always 0.3 so native grenades work (grip long-press).
        // When zone is enabled (!0): 999999 to block grenades until hand enters zone.
        {
            auto* throwDelaySetting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
            if (throwDelaySetting) {
                if (g_config.throwableActivationZone == 0) {
                    throwDelaySetting->SetFloat(0.3f);
                    spdlog::debug("[GRENADE] Zone disabled - fThrowDelay=0.3 (native grenades enabled)");
                } else {
                    throwDelaySetting->SetFloat(999999.9f);
                    spdlog::debug("[GRENADE] Zone enabled - fThrowDelay=999999 (zone-gated)");
                }
            }
        }

        InitHands();
        _initialized = true;
        spdlog::info("Heisenberg initialized");
    }

    void Heisenberg::ReapplyThrowDelay()
    {
        // Re-apply fThrowDelay after save load (game resets INI on load)
        auto* throwDelaySetting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
        if (throwDelaySetting) {
            if (g_config.throwableActivationZone == 0) {
                throwDelaySetting->SetFloat(0.3f);
            } else {
                throwDelaySetting->SetFloat(999999.9f);
            }
        }
        // Reset zone tracking so transitions are detected fresh after load
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

        // MAIN THREAD HEARTBEAT: Log every ~5 seconds to verify main thread is running
        // If deadlock occurs, this will stop appearing while OpenVR callback heartbeat continues
        static int mainThreadHeartbeat = 0;
        if (++mainThreadHeartbeat >= 450) {  // ~5 seconds at 90fps
            mainThreadHeartbeat = 0;
            spdlog::debug("[MAIN THREAD HEARTBEAT] OnInputUpdate running, frame count={}", mainThreadHeartbeat);
        }

        // NOTE: kFighting toggle code removed - was causing issues

        // Check for MCM settings changes (throttled internally to every 2 seconds)
        static int lastThrowableZone = g_config.throwableActivationZone;
        g_config.ReloadIfMCMChanged();

        // If throwable activation zone setting changed via MCM, reapply fThrowDelay
        if (g_config.throwableActivationZone != lastThrowableZone) {
            spdlog::debug("[GRENADE] Zone setting changed ({} -> {}) - reapplying fThrowDelay",
                        lastThrowableZone, g_config.throwableActivationZone);
            lastThrowableZone = g_config.throwableActivationZone;
            ReapplyThrowDelay();
        }

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
#if 0
            // Try to register proxy listener with player if not yet done
            // (player may not be ready at OnGameLoad)
            auto& proxyListener = PlayerCharacterProxyListener::GetSingleton();
            if (!proxyListener.GetPlayerProxy()) {
                proxyListener.RegisterWithPlayer();
            }
#endif
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

        // Update grab positions - this runs after physics so our SetTransform/velocity
        // changes won't be overwritten by the physics simulation
        auto& grabMgr = GrabManager::GetSingleton();
        grabMgr.PostPhysicsUpdate();

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
            wCfg.splashScale = 0.1f;
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

        // DISABLED FOR TESTING: Update item insert handler (Port-A-Diner, interactive discovery mode, etc.)
        // ItemInsertHandler::GetSingleton().Update();
    }

    bool Heisenberg::IsHoldingAnything() const
    {
        bool leftHolding = _leftHand && _leftHand->IsHolding();
        bool rightHolding = _rightHand && _rightHand->IsHolding();
        return leftHolding || rightHolding;
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

        // If grenade zone is disabled (0), native grenades work everywhere (fThrowDelay=0.3).
        // Ensure fThrowDelay is correct if zone was previously enabled (would have been 999999).
        if (g_config.throwableActivationZone == 0) {
            if (_wasInChestPocketZone) {
                // Zone was active last frame but config just changed to disabled.
                // Reset fThrowDelay to allow native grenades.
                auto* setting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
                if (setting) setting->SetFloat(0.3f);
                spdlog::debug("[GRENADE] Zone disabled - reset fThrowDelay=0.3");
            }
            // During cooldown, suppress grenade activation even when zone is disabled
            if (holdingNow || inGrenadeCooldown) {
                auto* setting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
                if (setting && setting->GetFloat() < 1.0f) {
                    setting->SetFloat(999999.9f);
                    spdlog::debug("[GRENADE] Cooldown/holding - fThrowDelay=999999");
                }
            } else {
                auto* setting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
                if (setting && setting->GetFloat() > 1.0f) {
                    setting->SetFloat(0.3f);
                    spdlog::debug("[GRENADE] Cooldown ended - fThrowDelay=0.3");
                }
            }
            _isInChestPocketZone = false;
            _wasInChestPocketZone = false;
            _wasInChest = false;
            return;
        }

        // Don't allow new grenade readying while holding an object or during cooldown
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
            auto* setting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
            if (setting) setting->SetFloat(0.3f);
            spdlog::debug("[GRENADE] Entered zone - fThrowDelay=0.3");
        } else if (!_isInChestPocketZone && _wasInChestPocketZone) {
            auto* setting = f4cf::f4vr::getIniSetting("fThrowDelay:Controls");
            if (setting) setting->SetFloat(999999.9f);
            spdlog::debug("[GRENADE] Left zone - fThrowDelay=999999");
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
        bool weaponDrawn = f4vrPlayer->actorState.IsWeaponDrawn();
        _cachedWeaponDrawn.store(weaponDrawn, std::memory_order_relaxed);

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

        // Hand collision disabled - was causing unwanted object movement
        // The HandCollision system creates physics bodies that can push objects
        // If needed, can be re-enabled via INI: bEnableHandCollision=true
        // if (_leftHand && _rightHand && g_config.enableHandCollision) {
        //     auto& handCollision = HandCollision::GetSingleton();
        //     handCollision.Update(
        //         _leftHand->GetPosition(), _rightHand->GetPosition(),
        //         _leftHand->GetVelocity(), _rightHand->GetVelocity(),
        //         1.0f / 90.0f  // Approximate delta time
        //     );
        // }
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
