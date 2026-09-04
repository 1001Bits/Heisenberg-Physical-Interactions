#include "PipboyInteraction.h"
#include "ActivatorHandler.h"
#include "Config.h"
#include "MenuChecker.h"
#include "DropToHand.h"
#include "Hooks.h"
#include "FRIKInterface.h"
#include "FingerCurves.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "IntroCeremonyState.h"
#include "PipboyMeshContactPolicy.h"
#include "../external/ROCK/src/ROCKMain.h"  // rock::HostIsWeaponSupportGripped — reliable two-handed check (FRIK's own flag gets killed by ROCK's TwoHandedGrip once engaged)
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "VRInput.h"
#include "Utils.h"
#include "RE/Fallout.h"
#include "F4VROffsets.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include "f4vr/F4VROffsets.h"
#include "common/MatrixUtils.h"

#include <RE/Bethesda/PipboyManager.h>
#include <SimpleIni.h>
#include <ShlObj.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <array>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <filesystem>
#include <limits>

#include <windows.h>

#include <d3d11.h>
#include <dxgiformat.h>

using namespace f4cf;  // So f4vr::getPlayer() works without f4cf:: prefix
using common::MatrixUtils;

// Cached BSFixedString menu names — lazy-init to avoid calling game string pool during DLL static init
static const RE::BSFixedString& MenuMain()           { static RE::BSFixedString s("MainMenu");            return s; }
static const RE::BSFixedString& MenuLoading()        { static RE::BSFixedString s("LoadingMenu");         return s; }
static const RE::BSFixedString& MenuTerminal()       { static RE::BSFixedString s("TerminalMenu");        return s; }
static const RE::BSFixedString& MenuTerminalButtons(){ static RE::BSFixedString s("TerminalMenuButtons"); return s; }
static const RE::BSFixedString& MenuPipboy()         { static RE::BSFixedString s("PipboyMenu");          return s; }
static const RE::BSFixedString& MenuHolotape()       { static RE::BSFixedString s("HolotapeMenu");        return s; }
static const RE::BSFixedString& MenuPipboyHolotape() { static RE::BSFixedString s("PipboyHolotapeMenu");  return s; }
static const RE::BSFixedString& MenuConsole()         { static RE::BSFixedString s("Console");             return s; }

// VR-compatible PipboyManager singleton accessor
// CommonLibF4's GetSingleton() uses REL::ID(4799238) which is NOT in VR address library.
// Use the raw VR offset instead (verified in fo4_database.csv: ID 553234 → 0x5940758).
static RE::PipboyManager* GetPipboyManagerVR() {
    static REL::Relocation<RE::PipboyManager**> singleton{ REL::Offset(0x5940758) };
    return *singleton;
}

// Cached at first call: F4VR's bAlwaysUseProjectedPipboy:VRPipboy INI flag.
// Read once because F4VR transiently flips this during pause/terminal flows,
// which would break gates if checked live per-frame.
static bool IsProjectedPipboyAtLoad() {
    static const bool cached = []() {
        auto* s = f4cf::f4vr::getIniSetting("bAlwaysUseProjectedPipboy:VRPipboy");
        const bool v = s ? s->GetBinary() : false;
        spdlog::info("[PIPBOY] Cached bAlwaysUseProjectedPipboy:VRPipboy = {}", v);
        return v;
    }();
    return cached;
}

// ── Temporary projected→wrist override for holotape playback ────────────────
//
// The two raw game globals below are exactly what f4vr::isPipboyOnWrist() reads
// (bAlwaysUseProjectedPipboy at 0x37B4280, attach-to-HMD at 0x37B4298). FRIK
// reads isPipboyOnWrist() live too, so flipping these overrides projected mode
// for BOTH the game and FRIK in one stroke. We use this so a terminal tape or
// the Heisenberg holotape, inserted while projected mode is active, plays on the
// handheld wrist Pipboy instead of doing nothing / waiting for a projected open.
// The original values are restored on eject or cell/game load.
static bool* PipboyAlwaysProjectedFlag() {
    static auto* p = reinterpret_cast<bool*>(REL::Offset(0x37B4280).address());
    return p;
}
static bool* PipboyAttachToHMDFlag() {
    static auto* p = reinterpret_cast<bool*>(REL::Offset(0x37B4298).address());
    return p;
}
static bool IsProjectedPipboyNow() {
    return *PipboyAlwaysProjectedFlag() || *PipboyAttachToHMDFlag();
}

// Reliable "the player has actually RECEIVED the Pip-Boy" check. The TapeDeck01 node lives on
// the body skeleton from the very start (just hidden until acquired), so its mere presence is
// NOT a valid acquisition signal — relying on it let the intro ceremony fire AND the terminal-
// hacking patches arm before the player even picks up the Pip-Boy in Vault 111, disturbing the
// vanilla pickup. The Pip-Boy is FormID 0x00021B3B (Fallout4.esm) and is added to inventory on
// pickup, so an inventory check is the authoritative "has the Pip-Boy" gate.
static bool PlayerHasReceivedPipboy()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;
    auto* pipboy = RE::TESForm::GetFormByID(0x00021B3B);
    if (!pipboy) return false;
    return player->GetInventoryObjectCount(static_cast<RE::TESBoundObject*>(pipboy)) > 0;
}

// The pickup quest adds the Pip-Boy armor to inventory before its ordinary
// equip/model-attach work finishes.  The boot renderer can therefore become
// visible several seconds before the physical PipboyLowPlayer armor does.
// Complete the same verified VR equip operation immediately on the observed
// inventory 0->1 transition. This helper runs from PipboyInteraction::Update on
// the main thread, not from the inventory callback. A queued equip was not
// processed until the 15-second boot menu closed, so the boot screen appeared
// on an empty wrist.
static bool QueuePhysicalPipboyArmorEquipForBoot()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* form = RE::TESForm::GetFormByID(0x00021B3B);
    if (!player || !form ||
        form->GetFormType() != RE::ENUM_FORM_ID::kARMO) {
        return false;
    }

    RE::ActorEquipManager** equipManagerPtr =
        heisenberg::g_ActorEquipManager.get();
    if (!equipManagerPtr || !*equipManagerPtr) {
        return false;
    }

    struct LocalObjectInstance
    {
        RE::TESForm* object{ nullptr };
        RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
    };
    static_assert(sizeof(LocalObjectInstance) == 0x10);

    LocalObjectInstance instance{};
    instance.object = form;
    const bool queued =
        heisenberg::ActorEquipManager_EquipObject(
            *equipManagerPtr,
            player,
            reinterpret_cast<RE::BGSObjectInstance*>(&instance),
            0,
            1,
            nullptr,
            false,  // already on the main update thread; do not defer past boot
            true,   // pickup armor must be present for the complete boot
            false,  // vanilla pickup/boot owns its sounds
            true,   // attach PipboyLowPlayer before the boot screen is shown
            false);
    spdlog::log(
        queued ? spdlog::level::info : spdlog::level::warn,
        "[PIPBOY] Physical Pip-Boy armor immediate equip {} at pickup transition",
        queued ? "completed" : "returned false (vanilla equip may already own it)");
    // A false return can mean the same armor is already in the vanilla equip
    // queue.  The request reached a valid manager, so do not submit it every
    // frame and race the pickup's own work.
    return true;
}

static bool s_projOverrideActive   = false;
static bool s_projSavedAlways      = false;
static bool s_projSavedHMD         = false;
static bool s_projOverrideSawPipOpen = false;  // Pipboy was open during the override
static int  s_projRestoreDelayFrames = 0;          // Let FRIK settle before changing back to HMD mode
// Saved VRPipboy scale angles, captured the first time ActivatePipboyScreen() zeroes them and
// restored by DeactivatePipboyScreen(). Previously close wrote hardcoded 20/5/20/5, which is wrong
// for in-front/projected mode and left the Pip-Boy mis-scaled too far forward (you look INSIDE it).
// Order: fHMDToPipboyScaleOuterAngle, fHMDToPipboyScaleInnerAngle, fPipboyScaleOuterAngle, fPipboyScaleInnerAngle.
static float s_savedPipAngles[4] = { 20.0f, 5.0f, 20.0f, 5.0f };
static bool  s_pipAnglesSaved    = false;
// True while a TERMINAL redirect owns the projected->wrist override (so the
// holotape auto-restore below leaves it alone; the terminal restores it itself).
static bool s_termAppliedWristOverride = false;
// Safety countdown: when we pre-flip to wrist on terminal ACTIVATION, restore if no
// terminal redirect actually starts within this many frames (e.g. a failed/locked
// activation that never opens TerminalMenu). 0 = inactive.
static int s_termEarlyOverrideFrames = 0;

static bool BeginWristOverrideForHolotape(bool allowStagedHolo = false) {
    if (s_projOverrideActive) return true;
    // Direct activation during FRIK's Holo root replacement is unsafe. The intro-tape path
    // is the sole exception: it flips only the mode flags here, then waits for the replacement
    // root to change and settle before it activates the screen. All other callers stay gated.
    const bool frikHolo = heisenberg::PipboyInteraction::GetSingleton().IsFrikHoloPipboyEnabled();
    if (frikHolo && !allowStagedHolo) {
        spdlog::info("[PIPBOY] Wrist override skipped — FRIK Holo requires staged root transition");
        return false;
    }
    bool* always = PipboyAlwaysProjectedFlag();
    bool* hmd    = PipboyAttachToHMDFlag();
    if (!*always && !*hmd) return false;  // already on wrist — nothing to override
    s_projSavedAlways = *always;
    s_projSavedHMD    = *hmd;
    *always = false;
    *hmd    = false;
    s_projOverrideActive = true;
    s_projOverrideSawPipOpen = false;
    s_projRestoreDelayFrames = 0;
    spdlog::info("[PIPBOY] Projected->wrist override ON for holotape (saved always={} hmd={} stagedHolo={})",
                 s_projSavedAlways, s_projSavedHMD, frikHolo && allowStagedHolo);
    return true;
}

static void EndWristOverrideForHolotape() {
    if (!s_projOverrideActive) return;
    *PipboyAlwaysProjectedFlag() = s_projSavedAlways;
    *PipboyAttachToHMDFlag()     = s_projSavedHMD;
    s_projOverrideActive = false;
    s_projOverrideSawPipOpen = false;
    s_projRestoreDelayFrames = 0;
    spdlog::info("[PIPBOY] Projected->wrist override OFF — restored (always={} hmd={})",
                 s_projSavedAlways, s_projSavedHMD);
}

// Restore projected mode once the holotape display has ended. The override is
// stamped on insert; playback then opens the Pipboy. When the player closes the
// Pipboy after viewing, the display is done — restore their projected preference
// (even if the tape stays in the deck). Must wait for the open→closed transition
// so we don't restore before playback has opened the Pipboy.
static void UpdateWristOverrideRestore() {
    if (!s_projOverrideActive) return;
    // A terminal redirect owns the override right now — let it restore on terminal
    // close, don't restore here on a transient Pipboy open/close.
    if (s_termAppliedWristOverride) return;
    bool pipOpen = heisenberg::MenuChecker::GetSingleton().IsPipboyOpen();
    if (pipOpen) {
        s_projOverrideSawPipOpen = true;
        s_projRestoreDelayFrames = 0;
    } else if (s_projOverrideSawPipOpen) {
        if (s_projRestoreDelayFrames == 0) {
            s_projRestoreDelayFrames = 2;
            spdlog::debug("[PIPBOY] Holotape display ended — delaying projected-mode restore by 2 frames");
        } else if (--s_projRestoreDelayFrames == 0) {
            spdlog::info("[PIPBOY] Holotape display ended — restoring projected mode after settle delay");
            EndWristOverrideForHolotape();
        }
    }
}

// Clear kPausesGame from a menu, decrement menuMode, and undo audio counter
// increments that MenuModeCounterListener applied when the menu opened.
// Each kPausesGame menu open increments: 0x5acd700 (SFX), 0x5acd704 (aux),
// 0x5acd780 (master gate, atomic). We reverse these with state-transition
// detection so unpause functions only fire when counters reach 0 (matching
// the game's own close behavior).
static bool ClearMenuPauseFlag(const RE::BSFixedString& menuName) {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;

    auto menu = ui->GetMenu(menuName);
    if (!menu) return false;

    auto* flags = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(menu.get()) + 0x58);
    if (*flags & 1) {  // kPausesGame = bit 0
        *flags &= ~1u;
        if (ui->menuMode > 0) ui->menuMode--;

        // Audio counter addresses (MenuModeCounterListener targets)
        static auto addr700 = REL::Offset(0x5acd700).address();  // Main SFX
        static auto addr704 = REL::Offset(0x5acd704).address();  // Auxiliary category
        static auto addr708 = REL::Offset(0x5acd708).address();  // Voice/event counter
        static auto addr780 = REL::Offset(0x5acd780).address();  // Master gate (atomic)

        auto& sfxCounter   = *reinterpret_cast<int32_t*>(addr700);
        auto& auxCounter   = *reinterpret_cast<int32_t*>(addr704);
        auto& voiceCounter = *reinterpret_cast<int32_t*>(addr708);

        // Decrement main SFX counter; if it hits 0, unpause SFX + decrement master gate
        bool wasSfxPaused = (sfxCounter > 0);
        if (sfxCounter > 0) sfxCounter--;

        if (wasSfxPaused && sfxCounter == 0) {
            // SFX counter reached 0 — call PauseUnpauseSFXCategories(false) to restore audio.
            // Call with BOTH mute and normal params to cover all audio categories.
            using PauseUnpauseSFXFn = void(*)(bool, uint16_t);
            static auto sfxFnAddr    = REL::Offset(0xcd73d0).address();
            static auto normalAddr   = REL::Offset(0x5acd710).address();
            static auto muteAddr     = REL::Offset(0x5acd70c).address();
            uint16_t normalParam = *reinterpret_cast<uint16_t*>(normalAddr);
            uint16_t muteParam   = *reinterpret_cast<uint16_t*>(muteAddr);
            auto sfxFn = reinterpret_cast<PauseUnpauseSFXFn>(sfxFnAddr);
            sfxFn(false, muteParam);
            sfxFn(false, normalParam);

            // Atomic decrement of master audio gate (game uses LOCK prefix)
            InterlockedDecrement(reinterpret_cast<volatile long*>(addr780));
        }

        // Decrement auxiliary counter; if it hits 0, unpause audio category
        bool wasAuxPaused = (auxCounter > 0);
        if (auxCounter > 0) auxCounter--;

        if (wasAuxPaused && auxCounter == 0) {
            static auto audioBaseAddr = REL::Offset(0x5ab97b8).address();
            static auto audioFlagAddr = REL::Offset(0x5ab9614).address();
            if (*reinterpret_cast<uint8_t*>(audioFlagAddr) != 0) {
                auto audioBase = *reinterpret_cast<uintptr_t*>(audioBaseAddr);
                if (audioBase) {
                    auto* soundCategory = reinterpret_cast<void*>(audioBase + 0x30);
                    using PauseCategoryFn = void(*)(bool, void*, bool, uint16_t);
                    static auto catFnAddr = REL::Offset(0xcd6850).address();
                    reinterpret_cast<PauseCategoryFn>(catFnAddr)(false, soundCategory, true, 0);
                }
            }
        }

        // Decrement voice/event counter; if it hits 0, fire pause change event
        // (radio and other systems may listen to this event to resume)
        bool wasVoicePaused = (voiceCounter > 0);
        if (voiceCounter > 0) voiceCounter--;

        if (wasVoicePaused && voiceCounter == 0) {
            // Fire the MenuPausedChangeEvent (same as MenuModeCounterListener does)
            // FUN_140cdb720(&DAT_1437bfe10, ...) with unpause=false
            static auto eventSourceAddr = REL::Offset(0x37bfe10).address();
            bool unpauseState = false;  // voiceCounter == 0 means unpaused
            void* eventData[2];
            eventData[0] = &unpauseState;
            using FireEventFn = void(*)(uintptr_t, void*);
            static auto fireEventAddr = REL::Offset(0xcdb720).address();
            reinterpret_cast<FireEventFn>(fireEventAddr)(eventSourceAddr, eventData);
        }

        spdlog::debug("[PIPBOY] Cleared kPausesGame from {} (menuMode={}, sfx={}, aux={}, voice={})",
                     menuName.c_str(), ui->menuMode, sfxCounter, auxCounter, voiceCounter);
        return true;
    }
    return false;
}

// Play a UI sound by editor ID (e.g. "UITerminalCharScroll").
// Uses UIUtils::PlayMenuSound(char*) which routes through the game's sound system.
static void PlayMenuSoundByName(const char* editorID) {
    using PlayMenuSoundFn = void(*)(const char*);
    static REL::Relocation<PlayMenuSoundFn> fn{ REL::Offset(0x133d7d0) };
    fn(editorID);
}

// Check if player radio is currently enabled.
static bool IsPlayerRadioEnabled() {
    using Fn = bool(*)();
    static auto fnAddr = REL::Offset(0xd0a9d0).address();
    return reinterpret_cast<Fn>(fnAddr)();
}

// Enable or disable player radio.
static void SetPlayerRadioEnabled(bool enable, bool playSound) {
    using Fn = void(*)(bool, bool);
    static auto fnAddr = REL::Offset(0xd0a870).address();
    reinterpret_cast<Fn>(fnAddr)(enable, playSound);
}

// Disable input event processing on a menu during holotape boot.
// Sets BSInputEventUser::inputEventHandlingEnabled = false so Scaleform
// doesn't call ProcessUserEvent (thumbstick/buttons are ignored by the SWF).
// Does NOT touch kOnStack (menu must stay on stack for AdvanceMovie to be called).
static bool DisableMenuInput(const RE::BSFixedString& menuName) {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;

    auto menu = ui->GetMenu(menuName);
    if (!menu) return false;

    auto menuPtr = reinterpret_cast<uintptr_t>(menu.get());

    // BSInputEventUser::inputEventHandlingEnabled at IMenu+0x18
    // (IMenu inherits BSInputEventUser at +0x10, member at +0x08 within that)
    auto* inputEnabled = reinterpret_cast<bool*>(menuPtr + 0x18);
    if (*inputEnabled) {
        *inputEnabled = false;
        spdlog::debug("[PIPBOY] Disabled input event handling on {}", menuName.c_str());
        return true;
    }
    return false;
}

// Re-enable input event processing on a menu (reverses DisableMenuInput).
static bool EnableMenuInput(const RE::BSFixedString& menuName) {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return false;

    auto menu = ui->GetMenu(menuName);
    if (!menu) return false;

    auto menuPtr = reinterpret_cast<uintptr_t>(menu.get());
    auto* inputEnabled = reinterpret_cast<bool*>(menuPtr + 0x18);
    if (!*inputEnabled) {
        *inputEnabled = true;
        spdlog::debug("[PIPBOY] Restored input event handling on {}", menuName.c_str());
        return true;
    }
    return false;
}

namespace heisenberg
{
    static bool IsUsableWorldPoint(const RE::NiPoint3& point);
    static RE::NiAVObject* SafeFindAVObject(RE::NiAVObject* root, const std::string& name);
    static RE::NiAVObject* FindNamedNodeOutsideSubtree(
        RE::NiAVObject* root,
        const char* targetName,
        RE::NiAVObject* excludedSubtree,
        int remainingDepth);

    float PipboyInteraction::GetFrikPipboyScale()
    {
        if (_frikPipboyScale < 0.0f) {
            _frikPipboyScale = 1.0f;  // default
            wchar_t* buffer = nullptr;
            HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &buffer);
            if (SUCCEEDED(hr) && buffer) {
                std::filesystem::path frikPath(buffer);
                CoTaskMemFree(buffer);
                frikPath /= "My Games/Fallout4VR/FRIK_Config/FRIK.ini";
                CSimpleIniA frikIni;
                frikIni.SetUnicode();
                if (frikIni.LoadFile(frikPath.string().c_str()) >= 0) {
                    _frikPipboyScale = static_cast<float>(frikIni.GetDoubleValue("Fallout4VRBody", "PipboyScale", 1.0));
                    spdlog::info("[PIPBOY] Read FRIK PipboyScale = {:.3f} from {}", _frikPipboyScale, frikPath.string());
                } else {
                    spdlog::warn("[PIPBOY] Could not read {}, using PipboyScale=1.0", frikPath.string());
                }
            } else {
                if (buffer) CoTaskMemFree(buffer);
                spdlog::warn("[PIPBOY] Could not resolve Documents folder, using PipboyScale=1.0");
            }
        }
        return _frikPipboyScale;
    }

    bool PipboyInteraction::IsFrikHoloPipboyEnabled()
    {
        if (_frikHoloPipboy < 0) {
            _frikHoloPipboy = 0;  // default: not holo
            wchar_t* buffer = nullptr;
            HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &buffer);
            if (SUCCEEDED(hr) && buffer) {
                std::filesystem::path frikPath(buffer);
                CoTaskMemFree(buffer);
                frikPath /= "My Games/Fallout4VR/FRIK_Config/FRIK.ini";
                CSimpleIniA frikIni;
                frikIni.SetUnicode();
                if (frikIni.LoadFile(frikPath.string().c_str()) >= 0) {
                    _frikHoloPipboy = frikIni.GetBoolValue("Fallout4VRBody", "HoloPipBoyEnabled", false) ? 1 : 0;
                    spdlog::info("[PIPBOY] Read FRIK HoloPipBoyEnabled = {} from {}", _frikHoloPipboy != 0, frikPath.string());
                } else {
                    spdlog::warn("[PIPBOY] Could not read {}, assuming HoloPipBoyEnabled=false", frikPath.string());
                }
            } else {
                if (buffer) CoTaskMemFree(buffer);
            }
        }
        return _frikHoloPipboy != 0;
    }

    static bool LoadFrikNormalPipboyScreenOffset(
        RE::NiTransform& outTransform)
    {
        wchar_t* documentsBuffer = nullptr;
        const HRESULT result = SHGetKnownFolderPath(
            FOLDERID_Documents,
            0,
            nullptr,
            &documentsBuffer);
        if (FAILED(result) || !documentsBuffer) {
            if (documentsBuffer) {
                CoTaskMemFree(documentsBuffer);
            }
            return false;
        }

        std::filesystem::path offsetPath(documentsBuffer);
        CoTaskMemFree(documentsBuffer);
        offsetPath /=
            "My Games/Fallout4VR/FRIK_Config/Pipboy_Offsets/"
            "PipboyPosition_v2.json";

        try {
            std::ifstream stream(offsetPath);
            if (!stream) {
                spdlog::warn(
                    "[PIPBOY] Normal FRIK screen offset not found at {}; "
                    "using the NIF-authored transform",
                    offsetPath.string());
                return false;
            }

            nlohmann::json document;
            stream >> document;
            const auto& position = document.at("PipboyPosition");
            const auto& rotation = position.at("rotation");
            if (!rotation.is_array() || rotation.size() < 12) {
                throw std::runtime_error(
                    "PipboyPosition.rotation must contain 12 values");
            }

            RE::NiTransform parsed{};
            parsed.translate.x = position.at("x").get<float>();
            parsed.translate.y = position.at("y").get<float>();
            parsed.translate.z = position.at("z").get<float>();
            parsed.scale = position.at("scale").get<float>();
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    parsed.rotate.entry[row][column] =
                        rotation.at(row * 4 + column).get<float>();
                }
            }

            const bool finite =
                std::isfinite(parsed.translate.x) &&
                std::isfinite(parsed.translate.y) &&
                std::isfinite(parsed.translate.z) &&
                std::isfinite(parsed.scale) &&
                parsed.scale > 0.001f;
            if (!finite) {
                throw std::runtime_error(
                    "PipboyPosition contains a non-finite/invalid transform");
            }

            outTransform = parsed;
            spdlog::info(
                "[PIPBOY] Loaded normal FRIK screen offset from {}: "
                "pos=({:.3f},{:.3f},{:.3f}) scale={:.3f}",
                offsetPath.string(),
                parsed.translate.x,
                parsed.translate.y,
                parsed.translate.z,
                parsed.scale);
            return true;
        } catch (const std::exception& error) {
            spdlog::warn(
                "[PIPBOY] Invalid normal FRIK screen offset {}: {}; "
                "using the NIF-authored transform",
                offsetPath.string(),
                error.what());
            return false;
        }
    }

    bool PipboyInteraction::BeginTemporaryNormalPipboyModel()
    {
        if (_temporaryNormalPipboyActive) {
            return true;
        }

        auto* playerNodes = f4vr::getPlayerNodes();
        if (!playerNodes ||
            !playerNodes->PipboyRoot_nif_only_node ||
            !playerNodes->ScreenNode) {
            spdlog::debug(
                "[PIPBOY] Cannot install temporary normal model: "
                "stock FRIK PlayerNodes are not ready");
            return false;
        }

        auto* originalRoot = playerNodes->PipboyRoot_nif_only_node;
        auto* originalScreen = playerNodes->ScreenNode;
        auto* attachParent = originalRoot->parent;
        if (!attachParent) {
            spdlog::warn(
                "[PIPBOY] Cannot install temporary normal model: "
                "current FRIK root has no attach parent");
            return false;
        }

        // FRIK/PipboyVR.nif is the normal UI/screen replacement.  The physical
        // wrist casing is the equipped 0x21B3B Pip-Boy armor and can appear as
        // a sibling in the actor tree after the queued pickup equip settles.
        // It is useful compatibility state when present, but is not the screen
        // root assigned to PlayerNodes.
        auto* physicalCasingObject =
            FindNamedNodeOutsideSubtree(
                attachParent,
                "PipboyRoot_NIF_ONLY",
                originalRoot,
                16);
        auto* physicalCasingRoot =
            physicalCasingObject
                ? physicalCasingObject->IsNode()
                : nullptr;
        // The casing is OPTIONAL, and the whole rest of this function already treats it that way:
        // every read of physicalCasingRoot below is null-guarded, EndTemporaryNormalPipboyModel
        // restores only the pieces it captured, and the success log calls it "optional armor
        // casing". This used to `return false` here instead, which permanently deferred the boot
        // on any skeleton where the armor casing never appears as a separate PipboyRoot_NIF_ONLY
        // sibling — confirmed live at the Vault 111 pickup, where it logged this same line 5213
        // times in a row and the RobCo boot screen never played. The screen the boot renders
        // through is FRIK's own loaded normal root, not the casing, so a missing casing costs only
        // the cosmetic emitter/screen swap on the physical arm model.
        if (!physicalCasingRoot) {
            spdlog::debug(
                "[PIPBOY] No separate armor-tree casing present; "
                "booting through the FRIK normal root alone");
        }

        RE::NiNode* normalRoot = nullptr;
        try {
            normalRoot = f4vr::loadNifFromFile("FRIK/PipboyVR.nif");
        } catch (const std::exception& error) {
            spdlog::error(
                "[PIPBOY] Failed to load FRIK/PipboyVR.nif: {}",
                error.what());
            return false;
        }
        if (!normalRoot) {
            spdlog::error(
                "[PIPBOY] Failed to load FRIK/PipboyVR.nif: null root");
            return false;
        }
        RE::NiPointer<RE::NiNode> loadedNormalRootHold;
        loadedNormalRootHold.reset(normalRoot);

        auto* normalScreenObject =
            SafeFindAVObject(normalRoot, "Screen");
        auto* normalScreen =
            normalScreenObject ? normalScreenObject->IsNode() : nullptr;
        if (!normalScreen) {
            spdlog::error(
                "[PIPBOY] FRIK/PipboyVR.nif has no Screen node");
            return false;
        }

        auto* armHoloEmitter = physicalCasingRoot
            ? SafeFindAVObject(physicalCasingRoot, "HoloEmitter")
            : nullptr;
        auto* armPhysicalScreen = physicalCasingRoot
            ? SafeFindAVObject(physicalCasingRoot, "Screen")
            : nullptr;

        _temporaryOriginalPipboyRoot = originalRoot;
        _temporaryOriginalPipboyScreen = originalScreen;
        _temporaryPipboyAttachParent = attachParent;
        _temporaryPhysicalCasingRoot = physicalCasingRoot;
        _temporaryNormalPipboyRootHold.reset(normalRoot);
        _temporaryOriginalPipboyRootHold.reset(originalRoot);
        _temporaryPipboyAttachParentHold.reset(attachParent);
        if (physicalCasingRoot) {
            _temporaryPhysicalCasingRootHold.reset(physicalCasingRoot);
        }
        _temporaryNormalPipboyRootOwned = true;
        _temporaryNormalPipboyRoot = normalRoot;
        _temporaryNormalPipboyScreen = normalScreen;
        _temporaryArmHoloEmitter = armHoloEmitter;
        _temporaryArmPhysicalScreen = armPhysicalScreen;
        _temporaryOriginalRootScale = originalRoot->local.scale;
        _temporaryOriginalScreenFlags = originalScreen->flags.flags;
        _temporaryNormalRootScale = normalRoot->local.scale;
        _temporaryNormalScreenLocal = normalScreen->local;
        LoadFrikNormalPipboyScreenOffset(
            _temporaryNormalScreenLocal);
        // BOOT FRAMING (Jul 27, user: "the upper 5% of the screen is not visible during the
        // startup animation"). The boot SWF renders to the full screen quad, but the temporary
        // normal wrist model sits slightly high in the visible aperture, clipping the top band.
        // Nudge the screen DOWN its own local -Z so the whole animation fits. Applied only to the
        // temporary boot screen; FRIK's own offsets and the normal Pip-Boy are untouched, and the
        // value is restored wholesale with the rest of the temporary state on teardown.
        _temporaryNormalScreenLocal.translate.z -=
            g_config.pipboyBootScreenDropGameUnits;
        if (physicalCasingRoot) {
            _temporaryPhysicalCasingRootScale =
                physicalCasingRoot->local.scale;
        }

        if (armHoloEmitter) {
            _temporaryHoloEmitterScale = armHoloEmitter->local.scale;
            _temporaryHoloEmitterFlags = armHoloEmitter->flags.flags;
        }
        if (armPhysicalScreen) {
            _temporaryArmPhysicalScreenScale =
                armPhysicalScreen->local.scale;
            _temporaryArmPhysicalScreenFlags =
                armPhysicalScreen->flags.flags;
        }

        // Keep FRIK's owned Holo scene attached and intact, but make it
        // impossible for its projector or screen to render during the boot.
        originalRoot->local.scale = 0.0f;
        originalScreen->flags.flags |= static_cast<std::uint64_t>(0x1);

        // If this skeleton also has a separate armor casing, preserve the old
        // compatibility behavior. PlayerNodes still points only at the loaded
        // complete FRIK model and its native Screen render root.
        if (physicalCasingRoot &&
            !(physicalCasingRoot->local.scale > 0.0f)) {
            physicalCasingRoot->local.scale = 1.0f;
        }
        if (armHoloEmitter) {
            armHoloEmitter->local.scale = 0.0f;
            armHoloEmitter->flags.flags |=
                static_cast<std::uint64_t>(0x1);
        }
        if (armPhysicalScreen) {
            if (!(armPhysicalScreen->local.scale > 0.0f)) {
                armPhysicalScreen->local.scale = 1.0f;
            }
            armPhysicalScreen->flags.flags &=
                ~static_cast<std::uint64_t>(0x1);
        }

        // The loaded normal FRIK root is the only root the game may render UI
        // through. Apply FRIK's normal-screen offset rather than the currently
        // selected Holo offset, which FRIK will otherwise write every frame.
        if (!(normalRoot->local.scale > 0.0f)) {
            normalRoot->local.scale = 1.0f;
        }
        normalScreen->local = _temporaryNormalScreenLocal;
        normalScreen->flags.flags &=
            ~static_cast<std::uint64_t>(0x1);
        if (!(normalScreen->local.scale > 0.0f)) {
            normalScreen->local.scale = 1.0f;
        }
        attachParent->AttachChild(normalRoot, false);
        playerNodes->PipboyRoot_nif_only_node = normalRoot;
        playerNodes->ScreenNode = normalScreen;
        f4vr::updateTransformsDown(attachParent, true);

        _temporaryNormalPipboyActive = true;
        spdlog::info(
            "[PIPBOY] Installed complete temporary FRIK normal wrist root {} "
            "over Holo root {} (optional armor casing={} UI screen={} casing "
            "screen={} emitter={} screenLocal=({:.2f},{:.2f},{:.2f}) "
            "scale={:.3f})",
            static_cast<const void*>(normalRoot),
            static_cast<const void*>(originalRoot),
            static_cast<const void*>(physicalCasingRoot),
            static_cast<const void*>(normalScreen),
            static_cast<const void*>(armPhysicalScreen),
            static_cast<const void*>(armHoloEmitter),
            normalScreen->local.translate.x,
            normalScreen->local.translate.y,
            normalScreen->local.translate.z,
            normalScreen->local.scale);
        return true;
    }

    bool PipboyInteraction::EndTemporaryNormalPipboyModel()
    {
        if (!_temporaryNormalPipboyActive) {
            return true;
        }

        auto* playerNodes = f4vr::getPlayerNodes();
        const bool stillOwnsCurrentRoot =
            playerNodes &&
            playerNodes->PipboyRoot_nif_only_node ==
                _temporaryNormalPipboyRoot;

        if (stillOwnsCurrentRoot) {
            playerNodes->PipboyRoot_nif_only_node =
                _temporaryOriginalPipboyRoot;
            playerNodes->ScreenNode =
                _temporaryOriginalPipboyScreen;
            spdlog::info(
                "[PIPBOY] Restored stock FRIK 77.12 Holo root {} and "
                "detached temporary normal root",
                static_cast<const void*>(
                    _temporaryOriginalPipboyRoot));
        } else {
            // A FRIK skeleton rebuild invalidates the old tree and installs
            // its own current pointers. Never dereference the stale saved
            // nodes in that case; simply relinquish the temporary state.
            spdlog::warn(
                "[PIPBOY] Temporary normal root no longer owns PlayerNodes; "
                "assuming stock FRIK rebuilt the Pip-Boy tree");
        }

        // The normal UI root is always ours. Detach it even if FRIK rebuilt
        // PlayerNodes while it was active; otherwise its screen can remain
        // visibly orphaned on the wrist.
        if (_temporaryNormalPipboyRootOwned &&
            _temporaryNormalPipboyRoot &&
            _temporaryNormalPipboyRoot->parent ==
                _temporaryPipboyAttachParent) {
            _temporaryPipboyAttachParent->DetachChild(
                _temporaryNormalPipboyRoot);
        }

        // Strong references make these restores safe even if an acquisition
        // rebuild detached the old tree. Only PlayerNodes assignment is
        // conditional: never overwrite a newer root installed by FRIK.
        if (_temporaryOriginalPipboyRoot) {
            _temporaryOriginalPipboyRoot->local.scale =
                _temporaryOriginalRootScale;
        }
        if (_temporaryOriginalPipboyScreen) {
            _temporaryOriginalPipboyScreen->flags.flags =
                _temporaryOriginalScreenFlags;
        }
        if (_temporaryArmHoloEmitter) {
            _temporaryArmHoloEmitter->local.scale =
                _temporaryHoloEmitterScale;
            _temporaryArmHoloEmitter->flags.flags =
                _temporaryHoloEmitterFlags;
        }
        if (_temporaryArmPhysicalScreen) {
            _temporaryArmPhysicalScreen->local.scale =
                _temporaryArmPhysicalScreenScale;
            _temporaryArmPhysicalScreen->flags.flags =
                _temporaryArmPhysicalScreenFlags;
        }
        if (_temporaryPhysicalCasingRoot) {
            _temporaryPhysicalCasingRoot->local.scale =
                _temporaryPhysicalCasingRootScale;
        }
        if (_temporaryNormalPipboyRoot) {
            _temporaryNormalPipboyRoot->local.scale =
                _temporaryNormalRootScale;
        }
        if (_temporaryPipboyAttachParent) {
            f4vr::updateTransformsDown(
                _temporaryPipboyAttachParent,
                true);
        }

        _temporaryNormalPipboyActive = false;
        _temporaryNormalPipboyRootOwned = false;
        _temporaryNormalPipboyRoot = nullptr;
        _temporaryNormalPipboyScreen = nullptr;
        _temporaryOriginalPipboyRoot = nullptr;
        _temporaryOriginalPipboyScreen = nullptr;
        _temporaryPipboyAttachParent = nullptr;
        _temporaryPhysicalCasingRoot = nullptr;
        _temporaryArmHoloEmitter = nullptr;
        _temporaryArmPhysicalScreen = nullptr;
        _temporaryNormalPipboyRootHold.reset();
        _temporaryOriginalPipboyRootHold.reset();
        _temporaryPipboyAttachParentHold.reset();
        _temporaryPhysicalCasingRootHold.reset();
        return true;
    }

    // ── BSAudioManager helpers — plays sounds through the game's own audio system.
    // This avoids creating a competing WASAPI session (which caused muffled audio). ──

    // VR offsets (Ghidra-verified):
    // FUN_141b4c3f0 → BSAudioManager::GetSingleton() returns BSAudioManager*
    // BSResource::ID::GenerateFromPath @ 141bee5f0
    // BSAudioManager::GetSoundHandleByFile @ 141b4cd50
    // BSSoundHandle::Play (FUN_141b4a9c0) @ 141b4a9c0
    // BSSoundHandle::FadeOutAndRelease @ 141b4b3e0

    using BSAudioManagerGetterFn  = RE::BSAudioManager*(*)();
    using GenerateFromPathFn      = void(*)(RE::BSResource::ID*, const char*);
    using GetSoundHandleByFileFn  = void(*)(RE::BSAudioManager*, RE::BSSoundHandle*,
                                            const RE::BSResource::ID*, std::uint32_t, std::uint8_t,
                                            const char*);
    using BSSoundHandlePlayFn     = bool(*)(RE::BSSoundHandle*);
    using BSSoundHandleFadeOutFn  = bool(*)(RE::BSSoundHandle*, std::uint16_t);
    using BSSoundHandleFadeVolFn  = bool(*)(RE::BSSoundHandle*, float, std::uint16_t, std::uint16_t, std::uint16_t);

    static REL::Relocation<BSAudioManagerGetterFn>  s_getAudioManager{ REL::Offset(0x1b4c3f0) };
    static REL::Relocation<GenerateFromPathFn>      s_generateFromPath{ REL::Offset(0x1bee5f0) };
    static REL::Relocation<GetSoundHandleByFileFn>  s_getSoundHandleByFile{ REL::Offset(0x1b4cd50) };
    static REL::Relocation<BSSoundHandlePlayFn>     s_bsSoundPlay{ REL::Offset(0x1b4a9c0) };
    static REL::Relocation<BSSoundHandleFadeOutFn>  s_bsSoundFadeOut{ REL::Offset(0x1b4b3e0) };
    static REL::Relocation<BSSoundHandleFadeVolFn>  s_bsSoundFadeVol{ REL::Offset(0x1b4ae20) };

    // Play a file through the game's BSAudioManager.
    // gameRelPath is relative to Data/, e.g. "Sound\\FX\\Heisenberg\\Eject button press.wav"
    // volume: 1.0 = full, 0.75 = 75%, etc. Applied immediately via FadeVolume(v,0,0,0).
    static RE::BSSoundHandle BSPlayGameSound(const char* gameRelPath, float volume = 1.0f)
    {
        RE::BSSoundHandle handle = { static_cast<std::uint32_t>(-1), false, 0 };
        RE::BSAudioManager* mgr = s_getAudioManager();
        if (!mgr) { spdlog::warn("[BSAUDIO] No audio manager for: {}", gameRelPath); return handle; }
        RE::BSResource::ID fileID{};
        s_generateFromPath(&fileID, gameRelPath);
        s_getSoundHandleByFile(mgr, &handle, &fileID, 0x12, 0x80, gameRelPath);
        if (handle.soundID != static_cast<std::uint32_t>(-1)) {
            s_bsSoundPlay(&handle);
            if (volume != 1.0f) s_bsSoundFadeVol(&handle, volume, 0, 0, 0);
            spdlog::debug("[BSAUDIO] Playing: {} (vol={:.0f}%)", gameRelPath, volume * 100.0f);
        } else {
            spdlog::warn("[BSAUDIO] Sound not found: {}", gameRelPath);
        }
        return handle;
    }

    static void BSStopSound(RE::BSSoundHandle& handle)
    {
        if (handle.soundID != static_cast<std::uint32_t>(-1)) {
            s_bsSoundFadeOut(&handle, 0);
            handle.soundID = static_cast<std::uint32_t>(-1);
        }
    }

    // Game-relative path for a sound file: "Sound\\FX\\Heisenberg\\<filename>"
    static std::string GetGameSoundPath(const char* filename)
    {
        return std::string("Sound\\FX\\Heisenberg\\") + filename;
    }

    // Persistent handle for the currently-playing narration line.
    // BSSoundHandle has no game-pool constructor so it is safe as a static.
    // Initialized to the invalid state (soundID = -1).
    static RE::BSSoundHandle s_narrationHandle = { static_cast<std::uint32_t>(-1), false, 0 };

    // Play a narration line through BSAudioManager (same pipeline as all other Heisenberg SFX).
    // Using PlaySoundA (WinMM) was found to open a competing WASAPI session on the HMD audio
    // device, which caused SteamVR's HRTF plugin to reset and corrupt spatial audio permanently.
    // BSAudioManager with flags 0x12 (same as UIUtils::PlayMenuSound_PreESM) is safe.
    static void PlayNarrationWav(const std::string& gameRelPath)
    {
        spdlog::debug("[INTRO] PlayNarrationWav: {}", gameRelPath);
        // Stop any previous narration line before starting the next
        BSStopSound(s_narrationHandle);
        s_narrationHandle = BSPlayGameSound(gameRelPath.c_str(), 0.75f);
        if (s_narrationHandle.soundID == static_cast<std::uint32_t>(-1)) {
            spdlog::warn("[INTRO] BSPlayGameSound failed for: {}", gameRelPath);
        }
    }

    static void StopNarrationWav()
    {
        BSStopSound(s_narrationHandle);
    }

    // ─────────────────────────────────────────────────────────────────────────

    // Resolve sound directory: game exe dir + Data\Sound\FX\Heisenberg\
    // Used for file-system existence checks and ParseWavDuration.
    // Files are deployed under MO2 as Sound/FX/Heisenberg/ and MO2 VFS makes
    // them visible at this path to the game process.
    static std::string GetSoundDir()
    {
        static std::string soundDir;
        if (!soundDir.empty()) return soundDir;

        char exePath[MAX_PATH];
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
            std::filesystem::path gamePath(exePath);
            auto dir = gamePath.parent_path() / "Data" / "Sound" / "FX" / "Heisenberg";
            soundDir = dir.string() + "\\";
            spdlog::debug("[PIPBOY] Sound directory: {}", soundDir);
        }
        if (soundDir.empty()) {
            soundDir = ".\\Data\\Sound\\FX\\Heisenberg\\";
        }
        return soundDir;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Interface3D::Renderer helpers (terminal redirect)
    // ════════════════════════════════════════════════════════════════════════

    using I3DGetByName_t = void* (*)(const RE::BSFixedString&);

    static void* I3D_GetByName(const char* name) {
        static auto fn = reinterpret_cast<I3DGetByName_t>(REL::Offset(0xb00270).address());
        RE::BSFixedString bsName(name);
        return fn(bsName);
    }

    static void I3D_LogRenderer(const char* label, void* rend) {
        if (!rend) {
            spdlog::debug("[I3D] {} = nullptr", label);
            return;
        }
        auto b = reinterpret_cast<uintptr_t>(rend);
        auto enabled     = *reinterpret_cast<uint8_t*>(b + 0x05C);
        auto offscr3D    = *reinterpret_cast<uint8_t*>(b + 0x05D);
        auto postfx      = *reinterpret_cast<int32_t*>(b + 0x070);
        auto screenmode  = *reinterpret_cast<int32_t*>(b + 0x078);
        auto worldRoot   = *reinterpret_cast<uintptr_t*>(b + 0x088);
        auto screenRoot  = *reinterpret_cast<uintptr_t*>(b + 0x090);
        auto offscreenEl = *reinterpret_cast<uintptr_t*>(b + 0x098);
        auto customRT    = *reinterpret_cast<int32_t*>(b + 0x1D8);
        auto customSwap  = *reinterpret_cast<int32_t*>(b + 0x1DC);

        spdlog::debug("[I3D] {} @{:X}: enabled={} offscr3D={} postfx={} "
                     "screenMode={} worldRoot={:X} screenRoot={:X} "
                     "offscreenEl={:X} RT={} Swap={}",
                     label, b, enabled, offscr3D, postfx,
                     screenmode, worldRoot, screenRoot,
                     offscreenEl, customRT, customSwap);

        if (worldRoot > 0x10000) {
            auto* node = reinterpret_cast<RE::NiAVObject*>(worldRoot);
            spdlog::debug("[I3D]   worldRoot name='{}' pos=({:.1f},{:.1f},{:.1f}) scale={:.3f}",
                         node->name.c_str() ? node->name.c_str() : "(null)",
                         node->world.translate.x, node->world.translate.y, node->world.translate.z,
                         node->world.scale);
        }
    }

    static bool IsAncestorOf(RE::NiAVObject* ancestor, RE::NiAVObject* node) {
        if (!ancestor || !node) return false;
        auto* p = node->parent;
        while (p) {
            if (p == ancestor) return true;
            p = p->parent;
        }
        return false;
    }

    static RE::NiAVObject* FindNamedNodeOutsideSubtree(
        RE::NiAVObject* root,
        const char* targetName,
        RE::NiAVObject* excludedSubtree,
        int remainingDepth)
    {
        if (!root ||
            !targetName ||
            remainingDepth < 0 ||
            root == excludedSubtree) {
            return nullptr;
        }

        const char* currentName = root->name.c_str();
        if (currentName &&
            _stricmp(currentName, targetName) == 0) {
            return root;
        }

        auto* node = root->IsNode();
        if (!node) {
            return nullptr;
        }
        for (const auto& child : node->children) {
            if (!child) {
                continue;
            }
            if (auto* match = FindNamedNodeOutsideSubtree(
                    child.get(),
                    targetName,
                    excludedSubtree,
                    remainingDepth - 1)) {
                return match;
            }
        }
        return nullptr;
    }

    struct PipboyRootInspection
    {
        RE::NiNode* root{ nullptr };
        RE::NiNode* screen{ nullptr };
        bool screenBelongsToRoot{ false };
        bool hasHoloMarker{ false };
        bool hasNormalMarker{ false };
    };

    static PipboyRootInspection InspectCurrentPipboyRoot()
    {
        PipboyRootInspection result{};
        auto* playerNodes = f4vr::getPlayerNodes();
        if (!playerNodes) {
            return result;
        }

        result.root = playerNodes->PipboyRoot_nif_only_node;
        result.screen = playerNodes->ScreenNode;
        if (!result.root || !result.screen) {
            return result;
        }

        result.screenBelongsToRoot =
            result.screen == result.root ||
            IsAncestorOf(result.root, result.screen);
        result.hasHoloMarker =
            SafeFindAVObject(result.root, "ScreenBlack01") != nullptr ||
            SafeFindAVObject(result.root, "ScreenBlack02") != nullptr ||
            SafeFindAVObject(result.root, "HoloScreen") != nullptr;
        result.hasNormalMarker =
            SafeFindAVObject(result.root, "ScreenGlowEffect01") != nullptr ||
            SafeFindAVObject(result.root, "ScreenGlowEffect01:0") != nullptr ||
            SafeFindAVObject(result.root, "ScreenDust:0") != nullptr;
        return result;
    }

    static void HideCurrentPipboyRootDuringModelTransition(
        const PipboyRootInspection& inspection)
    {
        if (inspection.root) {
            inspection.root->local.scale = 0.0f;
        }
        if (inspection.screen) {
            inspection.screen->flags.flags |= static_cast<std::uint64_t>(0x1);
        }
    }

    static bool IsVerifiedNormalPipboyRoot(
        const PipboyRootInspection& inspection,
        RE::NiNode* preSwitchRoot,
        bool rootMustChange)
    {
        return inspection.root != nullptr &&
               inspection.screen != nullptr &&
               inspection.screenBelongsToRoot &&
               inspection.hasNormalMarker &&
               !inspection.hasHoloMarker &&
               (!rootMustChange || inspection.root != preSwitchRoot);
    }

    // Leave the holo Pip-Boy in exactly the state a trigger close would leave it in.
    //
    // FRIK's Pipboy::openClose(false) (verified against FRIK source pipboy/Pipboy.cpp) does
    // three things: _isOpen = false, setNodeVisibility(ScreenNode,false), and
    // PipboyRoot_nif_only_node->local.scale = 0. FRIK ALREADY ran all three the instant the
    // terminal opened ("Close Pipboy due to terminal open..." in FRIK.log, 16ms before our
    // redirect activates). We then overrode the two scene-graph effects so the terminal could
    // render on the wrist. So reproducing a trigger close means putting those two back — the
    // state half is already correct — and FRIK cannot do it for us because openClose() early-
    // returns on `_isOpen == open`.
    //
    // STATE-GATED on FRIK's own isWristPipboyOpen(): if FRIK still considers the Pip-Boy open
    // (i.e. it kept it open for the terminal rather than closing it), the model is FRIK's to
    // manage — hiding it here would desync FRIK's state from the visuals, and since openClose()
    // early-returns on a matching flag the player could be left unable to bring the Pip-Boy
    // back at all. In that case we do nothing and let FRIK close it now that keep-open is off.
    // isWristPipboyOpen is the 8th entry of the API struct, inside the v2 ABI prefix that
    // FRIKInterface::Initialize already requires, so this needs no extra version gate.
    static bool RestoreFrikHoloHiddenStateAfterTerminal()
    {
        if (!f4vr::isPipboyOnWrist()) {
            return false;  // projected/HMD mode — FRIK never hid anything for us to restore
        }

        if (FRIKInterface::GetSingleton().IsWristPipboyOpen()) {
            spdlog::info("[PIPBOY] FRIK still reports its wrist Pip-Boy OPEN — leaving the "
                         "holo model to FRIK instead of hiding it behind its back");
            return false;
        }

        bool restored = false;

        // ScreenNode: prefer the same pointer the per-frame force used (player+0x7B8) so we
        // cannot restore a different node than the one we modified.
        RE::NiNode* frikScreen = nullptr;
        if (auto* player = f4vr::getPlayer()) {
            const auto screenPtr =
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(player) + 0x07B8);
            if (screenPtr > 0x10000) {
                frikScreen = reinterpret_cast<RE::NiNode*>(screenPtr);
            }
        }
        if (frikScreen) {
            frikScreen->flags.flags |= static_cast<std::uint64_t>(0x1);
            restored = true;
        }

        // PipboyRoot: ForceAncestorScales walked the parent chain restoring any zeroed scale,
        // so zero the root again exactly as FRIK does.
        if (auto* playerNodes = f4vr::getPlayerNodes()) {
            if (playerNodes->PipboyRoot_nif_only_node) {
                playerNodes->PipboyRoot_nif_only_node->local.scale = 0.0f;
                restored = true;
            }
        }

        return restored;
    }

    static void ForceAncestorScales(RE::NiAVObject* node) {
        auto* p = node->parent;
        while (p) {
            if (p->local.scale < 0.001f) {
                p->local.scale = 1.0f;  // Restore to neutral (don't override with pipScale)
            }
            p = p->parent;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Pipboy screen activation helpers (for holotape auto-playback)
    // Replicates FRIK's turnPipBoyOnOff — sets INI angles to 0/defaults
    // and forces ScreenNode + PipboyRoot visible/scaled.
    // ════════════════════════════════════════════════════════════════════════

    static void ActivatePipboyScreen() {
        // Only for wrist-mounted Pipboy — in projected/HMD mode the game engine handles display.
        // These INI angles control projected display scaling too, so overwriting them would break it.
        if (!f4vr::isPipboyOnWrist()) return;
        // Capture the player's real scale angles ONCE before zeroing, so close can restore them
        // instead of writing hardcoded 20/5 (which mis-scaled the in-front/projected Pip-Boy).
        if (!s_pipAnglesSaved) {
            if (auto* a = RE::GetINISetting("fHMDToPipboyScaleOuterAngle:VRPipboy")) s_savedPipAngles[0] = a->GetFloat();
            if (auto* b = RE::GetINISetting("fHMDToPipboyScaleInnerAngle:VRPipboy")) s_savedPipAngles[1] = b->GetFloat();
            if (auto* c = RE::GetINISetting("fPipboyScaleOuterAngle:VRPipboy"))      s_savedPipAngles[2] = c->GetFloat();
            if (auto* d = RE::GetINISetting("fPipboyScaleInnerAngle:VRPipboy"))      s_savedPipAngles[3] = d->GetFloat();
            s_pipAnglesSaved = true;
        }
        // Set INI angles to 0 so Pipboy screen is always "in view"
        RE::GetINISetting("fHMDToPipboyScaleOuterAngle:VRPipboy")->SetFloat(0.0f);
        RE::GetINISetting("fHMDToPipboyScaleInnerAngle:VRPipboy")->SetFloat(0.0f);
        RE::GetINISetting("fPipboyScaleOuterAngle:VRPipboy")->SetFloat(0.0f);
        RE::GetINISetting("fPipboyScaleInnerAngle:VRPipboy")->SetFloat(0.0f);
        // Make ScreenNode visible + PipboyRoot scaled.
        // Match FRIK's openClose(true): clear AppCulled on ScreenNode, set PipboyRoot scale to 1.
        // Don't override ScreenNode's local.scale — FRIK applies the user's configured
        // PipboyOffset (position/scale) every frame via updateSetupPipboyNodes(), and our
        // per-frame intro SWF forcing ensures ScreenNode stays visible (ancestors non-zero).
        auto* player = f4vr::getPlayer();
        if (player) {
            auto addr = reinterpret_cast<uintptr_t>(player);
            auto screenPtr = *reinterpret_cast<uintptr_t*>(addr + 0x07B8);
            auto rootPtr   = *reinterpret_cast<uintptr_t*>(addr + 0x07B0);
            if (screenPtr > 0x10000) {
                auto* screen = reinterpret_cast<RE::NiNode*>(screenPtr);
                screen->flags.flags &= ~static_cast<uint64_t>(0x1);  // Clear AppCulled
            }
            if (rootPtr > 0x10000)
                reinterpret_cast<RE::NiNode*>(rootPtr)->local.scale = 1.0f;
        }
    }

    static void DeactivatePipboyScreen() {
        if (!f4vr::isPipboyOnWrist()) return;
        // Restore the angles captured at activation (not hardcoded 20/5) so in-front/projected
        // scaling returns to the player's real values; the {20,5,20,5} initializer is the
        // fallback when nothing was ever captured.
        if (auto* a = RE::GetINISetting("fHMDToPipboyScaleOuterAngle:VRPipboy")) a->SetFloat(s_savedPipAngles[0]);
        if (auto* b = RE::GetINISetting("fHMDToPipboyScaleInnerAngle:VRPipboy")) b->SetFloat(s_savedPipAngles[1]);
        if (auto* c = RE::GetINISetting("fPipboyScaleOuterAngle:VRPipboy"))      c->SetFloat(s_savedPipAngles[2]);
        if (auto* d = RE::GetINISetting("fPipboyScaleInnerAngle:VRPipboy"))      d->SetFloat(s_savedPipAngles[3]);
        s_pipAnglesSaved = false;
    }

    // Pre-flip to wrist mode on terminal ACTIVATION (before TerminalMenu opens) so the
    // terminal initializes on the wrist Pipboy renderer instead of the projected VR overlay.
    // Mirrors how the holotape override is applied before playback opens the display.
    void PipboyInteraction::PrepareProjectedTerminalOnWrist() {
        if (!heisenberg::g_config.forceTerminalOnWrist) return;
        if (s_termAppliedWristOverride) return;          // already overriding
        if (!IsProjectedPipboyNow()) return;             // already on wrist — nothing to do
        if (!BeginWristOverrideForHolotape()) return;     // direct Holo transitions stay gated
        ActivatePipboyScreen();                          // un-cull + zero view angles
        s_termAppliedWristOverride = true;
        s_termEarlyOverrideFrames = 120;                 // ~2s safety: restore if no terminal opens
        spdlog::info("[PIPBOY] Terminal activation in projected mode — pre-flipped to wrist before menu opens");
    }

    // SEH helper — must be in a function with NO C++ objects (no REL::Offset, no std::string)
    static void SafeHideRollover_SEH(uintptr_t singletonAddr, uintptr_t fnAddr) {
        __try {
            auto singleton = *reinterpret_cast<uintptr_t*>(singletonAddr);
            if (singleton) {
                using HideRolloverFn = void(*)(uintptr_t);
                auto fn = reinterpret_cast<HideRolloverFn>(fnAddr);
                fn(singleton);
            }
        } __except(1) {}
    }

    static void SafeHideRollover() {
        if (!g_config.hideWandHUD) return;
        static auto singletonAddr = REL::Offset(0x37a1b48).address();
        static auto fnAddr = REL::Offset(0xab7590).address();

        static int s_hideCallCount = 0;
        if (++s_hideCallCount % 120 == 1) {
            auto singleton = *reinterpret_cast<uintptr_t*>(singletonAddr);
            spdlog::debug("[HUD_HIDE] SafeHideRollover call #{}, singleton={:X}, fnAddr={:X}",
                         s_hideCallCount, singleton, fnAddr);
        }

        SafeHideRollover_SEH(singletonAddr, fnAddr);
    }

    static void SuppressTerminalDarkening() {
        using StopIMOD_t = void(*)(uintptr_t);
        static auto stopIMOD = reinterpret_cast<StopIMOD_t>(
            REL::Offset(0x37d360).address());

        static bool loggedOnce = false;

        // 1+2: Terminal projected display + IMOD
        {
            static auto termDisplayMgrAddr = REL::Offset(0x5ac0f80).address();
            auto termDisplayMgr = *reinterpret_cast<uintptr_t*>(termDisplayMgrAddr);
            if (termDisplayMgr) {
                auto projectedDisplay = *reinterpret_cast<uintptr_t*>(termDisplayMgr + 0x100);
                if (projectedDisplay > 0x10000) {
                    reinterpret_cast<RE::NiAVObject*>(projectedDisplay)->flags.flags |= 0x1;
                }
                auto imodForm = *reinterpret_cast<uintptr_t*>(termDisplayMgr + 0xe8);
                if (imodForm) {
                    stopIMOD(imodForm);
                }
                if (!loggedOnce) {
                    spdlog::debug("[PIPBOY] SuppressTerminalDarkening: termDisplayMgr={:X} "
                                 "projDisp={:X} imod={:X}",
                                 termDisplayMgr, projectedDisplay, imodForm);
                }
            }
        }

        // 3: UIBlurManager blur IMOD
        {
            static auto uiBlurMgrAddr = REL::Offset(0x5ad5d68).address();
            auto uiBlurMgr = *reinterpret_cast<uintptr_t*>(uiBlurMgrAddr);
            if (uiBlurMgr) {
                auto blurImod = *reinterpret_cast<uintptr_t*>(uiBlurMgr + 0x18);
                auto& blurCount = *reinterpret_cast<int*>(uiBlurMgr + 0x20);
                if (blurImod) {
                    stopIMOD(blurImod);
                }
                // blurCount is the ENGINE's refcount of menus requesting blur.
                // Zeroing it while a terminal holds a reference means the engine's
                // matching decrement on close underflows to -1, and a manager that
                // only stops blurring when the count returns to 0 then never stops.
                // Clamp in BOTH directions so an already-underflowed count is
                // repaired rather than driven further negative.
                if (blurCount != 0) {
                    blurCount = 0;
                }
                if (!loggedOnce) {
                    spdlog::debug("[PIPBOY] SuppressTerminalDarkening: UIBlurMgr={:X} "
                                 "blurImod={:X}", uiBlurMgr, blurImod);
                }
            }
        }

        loggedOnce = true;
    }

    // Undo everything SuppressTerminalDarkening() and the per-frame terminal
    // loop force, at redirect teardown.
    //
    // The suppression runs EVERY FRAME while a terminal redirect is active and
    // was never unwound, so anything it forced stayed forced once the loop
    // stopped: a darkening/blur IMOD the engine restarted on close kept
    // running, the blur refcount kept whatever value our clamp left, and the
    // console/terminal-button menus kept the visibility we forced off them.
    // Symptom: exiting a terminal holotape with grip leaves a black screen
    // instead of the Pip-Boy inventory.
    static void RestoreTerminalDarkeningState()
    {
        // One last cancel, for an IMOD the engine started as the terminal closed.
        SuppressTerminalDarkening();

        int blurCountAfter = 0;
        {
            static auto uiBlurMgrAddr = REL::Offset(0x5ad5d68).address();
            auto uiBlurMgr = *reinterpret_cast<uintptr_t*>(uiBlurMgrAddr);
            if (uiBlurMgr) {
                auto& blurCount = *reinterpret_cast<int*>(uiBlurMgr + 0x20);
                // Hand the engine back a clean baseline: no menu is requesting
                // blur now, and a negative count would never recover on its own.
                if (blurCount != 0) {
                    blurCount = 0;
                }
                blurCountAfter = blurCount;
            }
        }

        // Give the menus we forced invisible their visibility back, so a later
        // terminal (or the console) is not permanently blank.
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto termBtnMenu = ui->GetMenu(MenuTerminalButtons())) {
                termBtnMenu->menuCanBeVisible = true;
            }
            if (auto consoleMenu = ui->GetMenu(MenuConsole());
                consoleMenu && consoleMenu->uiMovie) {
                consoleMenu->uiMovie->SetVisible(true);
            }
        }

        spdlog::info("[PIPBOY] Terminal darkening/blur state restored (blurCount={})", blurCountAfter);
    }

    // Restore the intro holotape's form type back to kVoice.
    // We temporarily set it to kProgram when the deck closes (for SWF playback),
    // but it MUST be reverted or the game crashes with R6025 (pure virtual call)
    // if the player later tries to open the holotape from inventory.
    static void RestoreIntroHolotapeType(std::uint32_t introFormID)
    {
        if (introFormID == 0) return;
        auto* form = RE::TESForm::GetFormByID(introFormID);
        if (!form || form->GetFormType() != RE::ENUM_FORM_ID::kNOTE) return;
        auto* note = static_cast<RE::BGSNote*>(form);
        if (note->type != RE::BGSNote::NOTE_TYPE::kVoice) {
            spdlog::debug("[PIPBOY] Restoring intro holotape {:08X} type to kVoice (was {})",
                         introFormID, note->type);
            note->type = RE::BGSNote::NOTE_TYPE::kVoice;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Vanilla Pip-Boy boot sequence (RobCo boot SWF)
    // ════════════════════════════════════════════════════════════════════════
    // Ghidra-verified 2026-07-24 (supersedes the Jul-22 animation-event attempt):
    // "PipboyBootSequence" is an event EMITTED BY the flat pickup animation's
    // behavior graph, not a graph INPUT — NotifyAnimationGraph sends events INTO
    // the graph, so the previous implementation was a legal no-op (fired cleanly
    // in the log, showed nothing). The real boot is a dedicated menu:
    // PipboyOpeningSequenceMenu, a GenericMenu that loads
    // Interface\pipboybootsequence.swf (present in the VR BA2) onto the Pip-Boy
    // SCREEN render target (PipboyManager::AddMenuToPipboy sets
    // customRendererName="PipboyMenu"), and the VR wrist-placement code lists it
    // among the wrist-screen menus natively. The one-call entry point — shared by
    // the vanilla handler AND the ShowPipboyBootSequence console command — is
    // GenericMenu::ShowPipboyBootSequence(BSFixedString&) @ VR 0x140b55470
    // (raw 0xB55470, VR AL ID 722966). It raises the Pip-Boy itself
    // (StartPipboyMode + InitPipboy), so PipboyMenu must NOT be pre-opened.

    static void FireNativePipboyBootEvent()
    {
        using ShowBootFn = void (*)(const RE::BSFixedString&);
        static REL::Relocation<ShowBootFn> showBoot{ REL::Offset(0xB55470) };
        // The anim-event arg is only consumed on the with-animation path; this
        // entry hardcodes noAnim internally — vanilla handler passes empty too.
        static const RE::BSFixedString noAnim("");
        showBoot(noAnim);
        spdlog::info("[PIPBOY] Invoked GenericMenu::ShowPipboyBootSequence (PipboyOpeningSequenceMenu)");
    }

    // True while the game's boot menu is on the Pip-Boy screen.
    static bool IsBootSequenceMenuOpen()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        static const RE::BSFixedString menuName("PipboyOpeningSequenceMenu");
        return ui->GetMenu(menuName) != nullptr;
    }

    // Show/hide PipboyMenu's Scaleform ROOT without touching the menu's lifecycle.
    // The game opens PipboyMenu ~100ms after the boot menu fires (log-confirmed), and
    // both menus render onto the same Pip-Boy screen texture — the user sees the
    // normal tabs blended with the boot SWF. Closing PipboyMenu ourselves is off
    // limits (FRIK tracks its lifecycle; a bypassed close leaves the player
    // restrained — see the eject-abort comments), so blank its movie instead while
    // the boot menu owns the screen and restore it afterwards. Returns true when the
    // visibility write landed (menu + movie + root all present).
    static bool SetPipboyMenuMovieVisible(bool visible)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        static const RE::BSFixedString menuName("PipboyMenu");
        auto menu = ui->GetMenu(menuName);
        if (!menu || !menu->uiMovie) return false;
        RE::Scaleform::GFx::Value root;
        if (!menu->uiMovie->GetVariable(&root, "root1") &&
            !menu->uiMovie->GetVariable(&root, "_root")) {
            return false;
        }
        RE::Scaleform::GFx::Value vis(visible);
        return root.SetMember("visible", vis);
    }

    // Enter through PipboyManager's real open lifecycle. A raw UI kShow creates
    // the Scaleform menu but skips camera/manager initialization and can be
    // closed by FRIK a frame later, leaving a lit but black render target.
    static bool RequestNativePipboyContentOpen()
    {
        auto* manager = GetPipboyManagerVR();
        if (!manager) {
            return false;
        }
        static const RE::BSFixedString noAnim("");
        manager->PlayPipboyGenericOpenAnim(
            MenuPipboy(),
            noAnim,
            true);
        return true;
    }

    void PipboyInteraction::BeginPipboyBootSequence()
    {
        if (_bootSequenceFired) return;

        const auto rootInspection = InspectCurrentPipboyRoot();
        if (!IsVerifiedNormalPipboyRoot(
                rootInspection,
                _bootPipboyPreSwitchRoot,
                _bootPipboyRootMustChange)) {
            spdlog::error(
                "[PIPBOY] Boot sequence blocked: current FRIK root is not the "
                "verified normal wrist model (root={} screen={} belongs={} "
                "normalMarker={} holoMarker={} mustChange={})",
                static_cast<const void*>(rootInspection.root),
                static_cast<const void*>(rootInspection.screen),
                rootInspection.screenBelongsToRoot,
                rootInspection.hasNormalMarker,
                rootInspection.hasHoloMarker,
                _bootPipboyRootMustChange);
            return;
        }

        // The transition gate keeps the replacement hidden while its scene tree
        // settles. Hand the verified normal root back to the native boot event;
        // ScreenNode itself stays culled until the real opening menu is observed.
        rootInspection.root->local.scale = 1.0f;

        if (IsProjectedPipboyNow()) {
            _bootWristOverrideOwned = BeginWristOverrideForHolotape();
            if (!_bootWristOverrideOwned) {
                // This is expected when the user's FRIK preference is Holo: the boot
                // path owns a staged temporary-normal-root transition instead, and the
                // direct flag override deliberately rejects Holo. Keep it visible at
                // the release error-only log level so the no-op is never mistaken for
                // a silently broken projected->wrist transition.
                spdlog::error(
                    "[PIPBOY] Boot direct wrist-flag override was not acquired; "
                    "FRIK Holo/projected mode remains under the staged normal-root transition");
            }
        }

        // Ghidra-verified redesign (Jul 24): do NOT pre-open PipboyMenu here. The real
        // boot entry (GenericMenu::ShowPipboyBootSequence, see FireNativePipboyBootEvent)
        // raises the Pip-Boy itself (StartPipboyMode + InitPipboy) and shows
        // PipboyOpeningSequenceMenu on the Pip-Boy screen render target; with PipboyMenu
        // already open, both menus would be routed onto the same screen texture at once.
        // The old ActivatePipboyScreen + cull + IsPipboyOpen-settle dance existed only to
        // satisfy the animation-event attempt that never actually displayed anything.
        FireNativePipboyBootEvent();
        _bootSequenceFired = true;
        _bootMenuSeen = false;
        _bootMenuFinished = false;
        _bootMenuAppearWaitSeconds = 3.0f;  // if the boot menu never appears, fall through to the finish path
        _bootPipboyCursorOverridden = false;
        _bootPipboyCursorWasEnabled = false;
        _bootContentOpenRequested = false;
        _bootContentOpenWaitSeconds = 0.0f;
        _bootContentMenuSeen = false;
        _bootContentCloseArmSeconds = 0.0f;
        _bootContentGripWasPressed = false;
        _bootContentGripCloseRequested = false;
        if (_bootHoloOverrideActive) {
            // Holo restore is now driven by the boot menu CLOSING (see the completion
            // watcher in the update loop) — this stays -1 until then. Ceiling only.
            _bootHoloRestoreSeconds = -1.0f;
        }
        spdlog::info("[PIPBOY] Boot sequence: fired PipboyOpeningSequenceMenu on the wrist Pip-Boy");
    }

    void PipboyInteraction::QueueProgramHolotapePlayback(std::uint32_t formID)
    {
        if (!_holotapeLoaded || _loadedHolotapeFormID != formID) {
            spdlog::debug("[PIPBOY] Program queue cancelled — holotape {:08X} is no longer loaded", formID);
            return;
        }

        auto* noteForm = RE::TESForm::GetFormByID(formID);
        if (!noteForm || noteForm->GetFormType() != RE::ENUM_FORM_ID::kNOTE) {
            spdlog::warn("[PIPBOY] Program queue cancelled — form {:08X} is not a NOTE", formID);
            return;
        }

        auto* holotape = static_cast<RE::BGSNote*>(noteForm);
        holotape->type = RE::BGSNote::NOTE_TYPE::kProgram;
        if (IsIntroHolotape(formID)) {
            holotape->programFile = "Heisenberg";
        }

        _pendingProgramFormID = formID;
        _pendingProgramWaitFrames = 0;
        ActivatePipboyScreen();

        // This is FRIK's non-pausing open signal. In the timeout fallback these
        // settings open the user's projected display; on a successful transition
        // ActivatePipboyScreen already made the settled wrist root visible.
        if (auto* s = RE::GetINISetting("fHMDToPipboyScaleOuterAngle:VRPipboy")) s->SetFloat(0.0f);
        if (auto* s = RE::GetINISetting("fHMDToPipboyScaleInnerAngle:VRPipboy")) s->SetFloat(0.0f);
        if (auto* s = RE::GetINISetting("fPipboyScaleOuterAngle:VRPipboy"))      s->SetFloat(0.0f);
        if (auto* s = RE::GetINISetting("fPipboyScaleInnerAngle:VRPipboy"))      s->SetFloat(0.0f);

        spdlog::info("[PIPBOY] Queued program holotape {:08X} on {} display (programFile='{}')",
                     formID, f4vr::isPipboyOnWrist() ? "wrist" : "projected",
                     holotape->GetNoteProgram().c_str());
    }

    void PipboyInteraction::BeginIntroProgramPlayback(std::uint32_t formID)
    {
        if (!_holotapeLoaded || _loadedHolotapeFormID != formID || !IsIntroHolotape(formID)) {
            spdlog::warn("[PIPBOY] Intro playback request rejected — {:08X} is not the loaded intro tape", formID);
            return;
        }
        if (Utils::IsPlayerInPowerArmor()) {
            spdlog::debug("[PIPBOY] Intro holotape {:08X} playback deferred — player in power armor", formID);
            return;
        }

        auto* noteForm = RE::TESForm::GetFormByID(formID);
        if (!noteForm || noteForm->GetFormType() != RE::ENUM_FORM_ID::kNOTE) return;
        auto* holotape = static_cast<RE::BGSNote*>(noteForm);
        holotape->type = RE::BGSNote::NOTE_TYPE::kProgram;
        holotape->programFile = "Heisenberg";

        // HEISENBERG INTRO — ALWAYS ON THE WRIST (user rule, Jul 24). The intro ceremony
        // plays on the wrist Pip-Boy no matter which display mode the player runs: Holo,
        // projected, and HMD-attached ("in front") are all temporarily overridden and
        // restored once the intro display ends.
        //  * Holo uses the same temporary normal scene root as the boot
        //    sequence. Stock FRIK's DLL and INI remain untouched.
        //  * Projected/HMD flip the game's live ini bools via the existing wrist override —
        //    unconditionally for the intro (the bHolotapeWristOverrideInProjected gate
        //    still governs ordinary holotapes only).
        if (IsFrikHoloPipboyEnabled() && !_introHoloIniOverrideActive) {
            if (BeginTemporaryNormalPipboyModel()) {
                _introHoloIniOverrideActive = true;
                _introHoloSawPipOpen = false;
                _introDisplayRestorePending = false;
                _introHoloWaitFrames = 2;
                _introHoloWaitFormID = formID;
                BeginWristOverrideForHolotape(true);
                spdlog::info("[PIPBOY] Intro {:08X}: temporary normal wrist root installed — deferring start {} frames",
                             formID, _introHoloWaitFrames);
                return;  // UpdateIntroHoloOverride() queues playback once the wait expires
            }
        }
        if (IsProjectedPipboyNow()) {
            BeginWristOverrideForHolotape();
        }

        QueueProgramHolotapePlayback(formID);
    }

    // Settle the temporary normal root and keep it for the complete intro
    // transaction: boot SWF plus spoken narration.  Restoring as soon as the
    // SWF menu closes exposes/activates FRIK's Holo display behind the audio.
    void PipboyInteraction::UpdateIntroHoloOverride()
    {
        using IntroCloseClock = std::chrono::steady_clock;
        static IntroCloseClock::time_point s_closeRetryStarted{};
        static IntroCloseClock::time_point s_nextCloseRetry{};
        static std::uint32_t s_closeRetryAttempts = 0;
        static bool s_closeRetryTimeoutLogged = false;

        if (!_introDisplayRestorePending) {
            s_closeRetryStarted = {};
            s_nextCloseRetry = {};
            s_closeRetryAttempts = 0;
            s_closeRetryTimeoutLogged = false;
        }

        if (_introHoloWaitFrames > 0) {
            if (--_introHoloWaitFrames == 0) {
                const auto formID = _introHoloWaitFormID;
                _introHoloWaitFormID = 0;
                if (_holotapeLoaded && _loadedHolotapeFormID == formID && !Utils::IsPlayerInPowerArmor()) {
                    spdlog::info("[PIPBOY] Intro {:08X}: normal-root settle done — starting wrist playback", formID);
                    QueueProgramHolotapePlayback(formID);
                } else {
                    spdlog::info("[PIPBOY] Intro {:08X}: cancelled during normal-root settle — restoring modes", formID);
                    if (_introHoloIniOverrideActive) {
                        EndTemporaryNormalPipboyModel();
                        _introHoloIniOverrideActive = false;
                    }
                    EndWristOverrideForHolotape();
                    RestoreIntroHolotapeType(formID);
                }
            }
            return;
        }

        const bool pipOpen = MenuChecker::GetSingleton().IsPipboyOpen();

        // Narration completion owns a strict close-before-restore handshake.
        // Never restore the Holo/projected preference while PipboyMenu is
        // still live: FRIK interprets that mode switch as a fresh open.
        if (_introDisplayRestorePending) {
            if (pipOpen) {
                const auto now = IntroCloseClock::now();
                if (s_closeRetryStarted == IntroCloseClock::time_point{}) {
                    s_closeRetryStarted = now;
                    s_nextCloseRetry = now;
                }

                constexpr auto kRetryInterval =
                    std::chrono::milliseconds(250);
                constexpr auto kRetryCeiling =
                    std::chrono::seconds(5);
                if (now - s_closeRetryStarted >= kRetryCeiling) {
                    if (!s_closeRetryTimeoutLogged) {
                        s_closeRetryTimeoutLogged = true;
                        spdlog::error(
                            "[PIPBOY] Intro display still open after {} bounded "
                            "ClosedownPipboy attempts over 5 s; stopping automatic "
                            "retries and waiting for a manual close before restoring modes",
                            s_closeRetryAttempts);
                    }
                    return;
                }

                if (now >= s_nextCloseRetry) {
                    DeactivatePipboyScreen();
                    EnableMenuInput(MenuPipboy());
                    EnableMenuInput(MenuHolotape());
                    EnableMenuInput(MenuPipboyHolotape());
                    if (auto* pbm = GetPipboyManagerVR()) {
                        pbm->ClosedownPipboy();
                    }
                    ++s_closeRetryAttempts;
                    s_nextCloseRetry = now + kRetryInterval;
                }
                return;
            }

            s_closeRetryStarted = {};
            s_nextCloseRetry = {};
            s_closeRetryAttempts = 0;
            s_closeRetryTimeoutLogged = false;
            DeactivatePipboyScreen();
            if (_introHoloIniOverrideActive) {
                EndTemporaryNormalPipboyModel();
            }
            EndWristOverrideForHolotape();
            _introHoloIniOverrideActive = false;
            _introHoloSawPipOpen = false;
            _introDisplayRestorePending = false;
            spdlog::info(
                "[PIPBOY] Intro narration ended with display closed — "
                "restored the user's Pip-Boy mode without reopening it");
            return;
        }

        if (!_introHoloIniOverrideActive) return;
        if (pipOpen) {
            _introHoloSawPipOpen = true;
        } else if (_introHoloSawPipOpen &&
                   !_introSWFActive &&
                   !_introPlaybackActive) {
            EndTemporaryNormalPipboyModel();
            EndWristOverrideForHolotape();
            _introHoloIniOverrideActive = false;
            _introHoloSawPipOpen = false;
            spdlog::info(
                "[PIPBOY] Intro display ended without narration — "
                "restored stock FRIK Holo root");
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Main update (called from Heisenberg.cpp every frame)
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::OnFrameUpdate(float deltaTime)
    {
        rock::performance_profiler::ScopedTimer profilerTimer(
            rock::performance_profiler::Scope::PipboyFrame);

        // A new main-update phase must never inherit HookEndUpdate's liveness
        // result. If this function returns before capture (no player/common
        // root), a later out-of-band accessor will lazily make one fresh
        // attempt rather than trusting the prior phase.
        _nodeSnapshotPhaseAttempted = false;

        // ROOT-CAUSE FIX for the intro-ceremony "execute at 0x0" CTD: the persistent node caches
        // (_cachedArmNode + the tape-deck sub-nodes) were assumed stable across frames, but the
        // Pip-Boy 3D subtree is torn down and rebuilt on equip / FRIK skeleton reset / cell change —
        // none of which fire a game-load (the only place ClearState ran). The cached pointers then
        // dangle at freed nodes, and findAVObject's virtual node->IsNode() dispatches through a dead
        // vtable. Detect the rebuild by watching the 3rd-person skeleton root pointer; when it
        // changes, invalidate the caches so every lookup re-resolves from the live, settled tree.
        {
            static RE::NiNode* s_lastCommonRoot = nullptr;
            RE::NiNode* curRoot = f4vr::getCommonNode();
            if (curRoot && curRoot != s_lastCommonRoot) {
                s_lastCommonRoot = curRoot;
                InvalidateFrameCache();           // _cachedArmNode + _frameCacheValid + finger cache
                _cachedEjectButton      = nullptr;
                _cachedEjectButtonMesh  = nullptr;
                _cachedTapeDeckLid      = nullptr;
                _cachedTapeRef          = nullptr;
                _cachedTapeDeckMesh1    = nullptr;
                _cachedTapeDeckLidMesh1 = nullptr;
                _nodesCached            = false;
                spdlog::debug("[PIPBOY] 3rd-person skeleton root changed — invalidated stale Pip-Boy node caches");
            }
        }

        // Finger POSITION must be refreshed every frame (it's a world-space coordinate, not a pointer).
        _fingerPosCached = false;

        // Bounded watchdog for the grip-exit Pip-Boy lowering (armed in the terminal-redirect
        // teardown below). ClosedownPipboy is known to sometimes blank the screen yet leave the
        // menu object alive (see the boot-content path), so re-issue every 250ms until
        // MenuChecker reports the menu gone; give up after 5s like the intro close retry.
        if (_terminalClosePipboyPending) {
            if (!heisenberg::MenuChecker::GetSingleton().IsPipboyOpen()) {
                _terminalClosePipboyPending = false;
                // The menu is down; NOW put back the hidden state we overrode for the terminal
                // render. Ordering matters: doing this while the menu was still up would blank
                // the display the player is still looking at.
                bool holoRestored = false;
                if (_terminalForcedFrikScreenVisible) {
                    holoRestored = RestoreFrikHoloHiddenStateAfterTerminal();
                    _terminalForcedFrikScreenVisible = false;
                }
                spdlog::info("[PIPBOY] Grip-exit lowering complete — menu closed, "
                             "FRIK holo model hidden={}", holoRestored);
            } else {
                _terminalClosePipboyDeadline  -= deltaTime;
                _terminalClosePipboyNextRetry -= deltaTime;
                if (_terminalClosePipboyDeadline <= 0.0f) {
                    _terminalClosePipboyPending = false;
                    // Deliberately do NOT restore the hidden state here: the menu is still up,
                    // so the player is looking at a live Pip-Boy and blanking its model would be
                    // worse than leaving it. Drop the debt instead and say so.
                    _terminalForcedFrikScreenVisible = false;
                    spdlog::error("[PIPBOY] Holo Pip-Boy still open 5s after grip-exit lowering; "
                                  "stopping bounded ClosedownPipboy retries and leaving the FRIK "
                                  "holo model visible (menu never closed)");
                } else if (_terminalClosePipboyNextRetry <= 0.0f) {
                    _terminalClosePipboyNextRetry = 0.25f;
                    EnableMenuInput(MenuPipboy());
                    if (auto* pbm = GetPipboyManagerVR()) {
                        pbm->ClosedownPipboy();
                    }
                }
            }
        }

        static bool firstCall = true;
        if (firstCall) {
            spdlog::debug("[PIPBOY] PipboyInteraction::OnFrameUpdate called for first time");
            firstCall = false;
        }

        // Restore projected Pipboy mode once a holotape display has ended (Pipboy
        // closed after playback). Runs every frame; no-op unless an override is active.
        UpdateWristOverrideRestore();

        // Safety for the terminal-activation pre-flip: if we flipped to wrist on a terminal
        // activation but no terminal redirect actually started (e.g. a locked/failed
        // activation), restore projected mode after the countdown so we don't get stuck.
        if (s_termEarlyOverrideFrames > 0) {
            if (_terminalRedirectActive || _pendingTerminalRedirect) {
                s_termEarlyOverrideFrames = 0;  // terminal took over — it restores on close
            } else if (--s_termEarlyOverrideFrames == 0 && s_termAppliedWristOverride) {
                spdlog::info("[PIPBOY] Terminal pre-flip timed out (no terminal opened) — restoring projected mode");
                DeactivatePipboyScreen();
                EndWristOverrideForHolotape();
                s_termAppliedWristOverride = false;
            }
        }

        // Stop intro if player exits to main menu (must run before player null check)
        if (_introPlaybackActive) {
            auto* ui = RE::UI::GetSingleton();
            if (ui && (ui->GetMenuOpen(MenuMain()) ||
                       ui->GetMenuOpen(MenuLoading()))) {
                spdlog::debug("[INTRO] Stopping playback — menu/loading detected (early check)");
                StopIntroPlayback();
            }
        }

        auto* player = f4vr::getPlayer();
        if (!player || !f4vr::getCommonNode()) {
            return;
        }

        // Capture once for this main-update phase. Every persistent Pip-Boy
        // pointer used below is checked against this same live-tree snapshot,
        // replacing several independent recursive scene-graph walks.
        BeginNodeValidationPhase();

        // FRIK refreshes PlayerNodes->ScreenNode->local every frame from the
        // selected Holo offset profile. During the temporary normal handoff that
        // would move the physical screen away from its authored casing position.
        // Reassert only while PlayerNodes still explicitly belongs to our strongly
        // held outer root. If FRIK installed a newer root, leave it untouched; the
        // transition verifier will fail closed and EndTemporaryNormalPipboyModel
        // will relinquish the old tree without replacing FRIK's newer pointers.
        if (_temporaryNormalPipboyActive) {
            auto* playerNodes = f4vr::getPlayerNodes();
            const bool stillOwnsCurrentRoot =
                playerNodes &&
                playerNodes->PipboyRoot_nif_only_node ==
                    _temporaryNormalPipboyRoot &&
                playerNodes->ScreenNode ==
                    _temporaryNormalPipboyScreen;
            if (stillOwnsCurrentRoot) {
                _temporaryNormalPipboyRoot->local.scale = 1.0f;
                _temporaryNormalPipboyScreen->local =
                    _temporaryNormalScreenLocal;
                _temporaryNormalPipboyScreen->flags.flags &=
                    ~static_cast<std::uint64_t>(0x1);
                if (_temporaryPhysicalCasingRoot) {
                    _temporaryPhysicalCasingRoot->local.scale = 1.0f;
                }
                if (_temporaryArmHoloEmitter) {
                    _temporaryArmHoloEmitter->local.scale = 0.0f;
                    _temporaryArmHoloEmitter->flags.flags |=
                        static_cast<std::uint64_t>(0x1);
                }
                if (_temporaryArmPhysicalScreen) {
                    _temporaryArmPhysicalScreen->local.scale = 1.0f;
                    _temporaryArmPhysicalScreen->flags.flags &=
                        ~static_cast<std::uint64_t>(0x1);
                }
            }
        }

        // ── Suppress game-opened menus when we redirected a holotape to hand ──
        // The EquipObject hook intercepts holotape activation and queues drop-to-hand,
        // but the game's UI may also open HolotapeMenu or TerminalMenu through a separate
        // code path. Close any that appeared within 500ms of our redirect.
        if (Hooks::WasHolotapeJustRedirected()) {
            auto* ui = RE::UI::GetSingleton();
            if (ui) {
                if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                    if (ui->GetMenuOpen(MenuHolotape())) {
                        msgQueue->AddMessage(MenuHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                        spdlog::debug("[PIPBOY] Suppressed game-opened HolotapeMenu (holotape redirected to hand)");
                    }
                    if (ui->GetMenuOpen(MenuPipboyHolotape())) {
                        msgQueue->AddMessage(MenuPipboyHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                        spdlog::debug("[PIPBOY] Suppressed game-opened PipboyHolotapeMenu (holotape redirected to hand)");
                    }
                    // Terminal holotapes (kTerminal type) open TerminalMenu instead of HolotapeMenu
                    if (ui->GetMenuOpen(MenuTerminal())) {
                        msgQueue->AddMessage(MenuTerminal(), RE::UI_MESSAGE_TYPE::kForceHide);
                        msgQueue->AddMessage(MenuTerminalButtons(), RE::UI_MESSAGE_TYPE::kForceHide);
                        spdlog::debug("[PIPBOY] Suppressed game-opened TerminalMenu (terminal holotape redirected to hand)");
                    }
                }
            }
        } else {
            // Redirect window expired — restore temporarily changed holotape type
            Hooks::RestoreRedirectedHolotapeType();
        }

        // ── Resolve the intro holotape INDEPENDENTLY of the one-time ceremony ──
        // The tape remains replayable after the opening ceremony, so its form and audio lines
        // must resolve even when this save's ceremony-complete bit is already set.
        // Self-guarded: returns early until the data handler/ESP is ready; the formID==0
        // check makes this a one-shot once it resolves.
        if (_introHolotapeFormID == 0) {
            InitIntroHolotape();
        }

        // One-time migration for saves created before the co-save record existed. The intro
        // tape in THIS save's inventory is the only reliable evidence that its ceremony ran;
        // the plugin INI is global and may describe a completely different character/save.
        if (_introHolotapeFormID != 0 &&
            IntroCeremonyState::NeedsLegacyInference() &&
            !MenuChecker::GetSingleton().IsLoading()) {
            auto* introForm = RE::TESForm::GetFormByID(_introHolotapeFormID);
            const bool hasIntroTape = introForm &&
                player->GetInventoryObjectCount(static_cast<RE::TESBoundObject*>(introForm)) > 0;
            IntroCeremonyState::ResolveLegacyFromHolotapePresence(hasIntroTape);
        }

        // A completed save still resolves the tape above (so manual replay works), but never
        // arms the physical opening ceremony again.
        if (_introDeliveryQueued && IntroCeremonyState::IsResolved() && IntroCeremonyState::IsComplete()) {
            _introDeliveryQueued = false;
            spdlog::debug("[INTRO] Ceremony already complete for this save — delivery queue cleared");
        }

        // New-game delivery is armed immediately in SetNewGame() (no exterior wait) — the timer
        // below defers it until the Pip-Boy/tape-deck is on the wrist, so the ceremony pops right
        // after the player receives the Pip-Boy inside Vault 111.

        // ── Intro holotape delivery timer ──
        // Delay delivery while in power armor (Pipboy is projected, not wrist-mounted)
        if (_introDeliveryQueued) {
            // The ceremony (tape-deck open + weapon sheathe) must NOT fire during the vanilla
            // Pip-Boy pickup/bootup — doing so corrupts the green RobCo boot + Pip-Boy screen state.
            // For a fresh pickup, deliver only AFTER the Pip-Boy boot/open has been observed and
            // then CLOSED, so the tape never lands over the RobCo boot screen. QueueIntroHolotapeDelivery
            // seeds this latch true when a loaded save already owned its Pip-Boy; no pickup boot is
            // running in that case, so a closed Pip-Boy plus the normal settle delay is sufficient.
            auto& introMenus = heisenberg::MenuChecker::GetSingleton();
            const bool hasPipboy = PlayerHasReceivedPipboy();
            if (hasPipboy && introMenus.IsPipboyOpen()) {
                _pipboyOpenedSinceAcquire = true;  // latch the first post-acquire Pip-Boy open
            }
            const bool skelReady   = FRIKInterface::GetSingleton().IsAvailable();
            const bool tapeDeckOk  = GetCachedTapeDeckNode() != nullptr;
            const bool loadingNow  = introMenus.IsLoading();
            const bool pipOpenNow  = introMenus.IsPipboyOpen();
            const bool inPA        = Utils::IsPlayerInPowerArmor();

            // kPostLoadGame can arrive while the world is still streaming. Re-check the loaded
            // inventory on the first settled frame so a post-Pip-Boy save never depends on the
            // exact instant at which the message-time inventory query became valid. If it is
            // still absent here, this is genuinely a pre-pickup save and the later boot must be
            // observed normally.
            if (_introPostLoadBaselinePending && !loadingNow) {
                _introPostLoadBaselinePending = false;
                if (hasPipboy) {
                    _pipboyOpenedSinceAcquire = true;
                }
                spdlog::debug("[INTRO] Post-load Pip-Boy baseline settled: hasPipboy={} openedLatch={}",
                              hasPipboy, _pipboyOpenedSinceAcquire);
            }

            const bool introNotReady =
                inPA
                || !hasPipboy
                || !skelReady                         // FRIK skeleton fully READY (3D not mid-reload)
                || !tapeDeckOk
                || loadingNow                         // never during a loadscreen
                || !_pipboyOpenedSinceAcquire         // wait for the first Pip-Boy open (the bootup)…
                || pipOpenNow;                        // …and require it CLOSED again before delivering
            // Gate diagnostic — debug level so it's silent in release (info) but available if needed.
            static float s_introGateDiagT = 0.0f;
            s_introGateDiagT += deltaTime;
            if (introNotReady && s_introGateDiagT >= 2.0f) {
                s_introGateDiagT = 0.0f;
                spdlog::debug("[INTRO-GATE] queued=1 hasPipboy={} skelReady={} tapeDeck={} loading={} openedSinceAcquire={} pipOpen={} PA={} delay={:.1f}",
                              hasPipboy, skelReady, tapeDeckOk, loadingNow, _pipboyOpenedSinceAcquire,
                              pipOpenNow, inPA, _introDeliveryDelay);
            }
            if (introNotReady) {
                _introDeliveryDelay = INTRO_DELIVERY_DELAY;  // restart the short settle after close
            } else {
                _introDeliveryDelay -= deltaTime;
                if (_introDeliveryDelay <= 0.0f) {
                    TryDeliverIntroHolotape();
                }
            }
        }

        // ── All tape deck + intro functionality disabled in power armor ──
        // PA uses projected Pipboy, not wrist-mounted — tape deck is inaccessible.
        // Intro ceremony is deferred via the delivery timer PA guard above.
        const bool inPowerArmor = Utils::IsPlayerInPowerArmor();

        // ── Vanilla Pip-Boy boot sequence — force it to fire on VR too ──
        // Runs independent of _introDeliveryQueued/power-armor-for-intro gating above;
        // this must observe the FIRST real pickup regardless of Heisenberg's own intro
        // ceremony state. Deliberately does NOT run in power armor (vanilla boot is a
        // physical wrist-Pipboy event; PA always uses projected, matching how the tape
        // deck/intro ceremony are gated above).
        if (!_bootSequenceFired && !inPowerArmor) {
            // Phase 0: observe the real 0->1 inventory transition, then ask FRIK
            // to perform its own live model-swap lifecycle when the player's
            // preference is Holo. An INI write is insufficient here: it changes
            // configuration but leaves the already-attached Holo NIF alive.
            if (!_bootHoloModeHandled) {
                const bool hasPipboy = PlayerHasReceivedPipboy();
                // ACQUISITION GATE (Jul 24): fire only on the 0 -> 1 transition observed
                // THIS session. Inventory count alone re-fired the boot on every save load
                // once the Pip-Boy was owned (log: two "boot menu never appeared" re-fires
                // after loads) — the boot is a once-per-pickup ceremony, not a per-load one.
                if (!hasPipboy) {
                    _bootSawPipboyAbsent = true;
                } else if (_bootSawPipboyAbsent) {
                    // Inventory ownership is visible before the vanilla pickup
                    // finishes equipping PipboyLowPlayer.nif.  Queue that armor
                    // now, before installing/firing the boot screen, so the
                    // physical wrist Pip-Boy and its boot image appear together.
                    if (!_bootPipboyArmorEquipQueued) {
                        _bootPipboyArmorEquipQueued =
                            QueuePhysicalPipboyArmorEquipForBoot();
                        if (!_bootPipboyArmorEquipQueued) {
                            // The equip singleton/actor may still be settling
                            // on the exact inventory transition frame.  Leave
                            // the latch clear and retry next frame.
                            spdlog::debug(
                                "[PIPBOY] Boot deferred until physical "
                                "Pip-Boy armor equip can be queued");
                            return;
                        }
                    }

                    const auto beforeSwitch = InspectCurrentPipboyRoot();
                    const bool rootMustChange =
                        IsFrikHoloPipboyEnabled();
                    bool transitionInitialized = !rootMustChange;

                    if (rootMustChange) {
                        if (BeginTemporaryNormalPipboyModel()) {
                            _bootHoloOverrideActive = true;
                            transitionInitialized = true;
                            spdlog::info(
                                "[PIPBOY] Boot sequence: temporary normal root "
                                "installed over stock FRIK Holo; validating");
                        } else {
                            // The inventory 0->1 event precedes FRIK attaching
                            // the physical wrist casing by a few frames. This
                            // is an expected acquisition race, not a terminal
                            // failure: leave _bootHoloModeHandled false so the
                            // next frame retries once the casing exists.
                            // Throttled: this is a per-frame retry, and when it did stick it
                            // produced 5213 identical lines (~2MB) in one session, burying the
                            // rest of the log.
                            static std::uint32_t s_bootDeferLog = 0;
                            if ((s_bootDeferLog++ % 120) == 0) {
                                spdlog::debug(
                                    "[PIPBOY] Boot sequence deferred: temporary "
                                    "normal wrist root is not ready yet (x{})",
                                    s_bootDeferLog);
                            }
                        }
                    }

                    if (transitionInitialized) {
                        _bootHoloModeHandled = true;
                        _bootPipboyPreSwitchRoot = beforeSwitch.root;
                        _bootPipboyRootMustChange = rootMustChange;
                        _bootPipboyRootTransitionFailed = false;
                        _bootPipboyRootTransitionPending = true;
                        _bootPipboyCandidateRoot = nullptr;
                        _bootPipboyRootStableFrames = 0;
                        _bootPipboyRootWaitSeconds = 3.0f;
                    }
                }
            }

            if (_bootHoloModeHandled &&
                !_bootPipboyRootTransitionFailed &&
                _bootPipboyRootTransitionPending) {
                const auto inspection = InspectCurrentPipboyRoot();

                const bool runtimeIsNormal =
                    !_bootPipboyRootMustChange ||
                    _temporaryNormalPipboyActive;
                const bool verifiedNormal = runtimeIsNormal &&
                    IsVerifiedNormalPipboyRoot(
                        inspection,
                        _bootPipboyPreSwitchRoot,
                        _bootPipboyRootMustChange);
                if (!verifiedNormal) {
                    HideCurrentPipboyRootDuringModelTransition(
                        inspection);
                }

                if (verifiedNormal) {
                    if (_bootPipboyCandidateRoot == inspection.root) {
                        ++_bootPipboyRootStableFrames;
                    } else {
                        _bootPipboyCandidateRoot = inspection.root;
                        _bootPipboyRootStableFrames = 1;
                    }
                } else {
                    _bootPipboyCandidateRoot = nullptr;
                    _bootPipboyRootStableFrames = 0;
                }

                _bootPipboyRootWaitSeconds -= deltaTime;
                if (_bootPipboyRootStableFrames >= 2) {
                    _bootPipboyRootTransitionPending = false;
                    spdlog::info(
                        "[PIPBOY] Boot sequence: verified normal FRIK root {} "
                        "for {} stable frames",
                        static_cast<const void*>(inspection.root),
                        _bootPipboyRootStableFrames);
                    BeginPipboyBootSequence();
                } else if (_bootPipboyRootWaitSeconds <= 0.0f) {
                    _bootPipboyRootTransitionPending = false;
                    _bootPipboyRootTransitionFailed = true;
                    if (_bootHoloOverrideActive) {
                        _bootHoloRestoreSeconds = 0.1f;
                    }
                    spdlog::error(
                        "[PIPBOY] Boot sequence suppressed after root-validation "
                        "timeout (root={} screen={} belongs={} normalMarker={} "
                        "holoMarker={} runtimeHolo={} mustChange={})",
                        static_cast<const void*>(inspection.root),
                        static_cast<const void*>(inspection.screen),
                        inspection.screenBelongsToRoot,
                        inspection.hasNormalMarker,
                        inspection.hasHoloMarker,
                        !runtimeIsNormal,
                        _bootPipboyRootMustChange);
                }
            }
        }

        // Boot-menu completion watcher: reveal-to-holo handoff is driven by the game's
        // own PipboyOpeningSequenceMenu closing (the SWF drives its own teardown). Kept
        // outside the !_bootSequenceFired gate above — it runs only after that flips true.
        if (_bootSequenceFired && !_bootMenuFinished) {
            const bool bootMenuOpen = IsBootSequenceMenuOpen();
            if (bootMenuOpen) {
                if (!_bootMenuSeen) {
                    _bootMenuSeen = true;
                    // The boot menu renders into the Pip-Boy SCREEN texture, but FRIK keeps
                    // the wrist screen itself dark until something lights it — user-confirmed:
                    // boot SOUNDS played over a dark screen. Light it now (un-cull + zero the
                    // view-angle gates; this does NOT open PipboyMenu, so the boot SWF is the
                    // only thing on the texture) and turn it back off when the boot ends.
                    ActivatePipboyScreen();
                    _bootScreenActivated = true;
                    spdlog::info("[PIPBOY] Boot sequence: PipboyOpeningSequenceMenu is showing — wrist screen lit");
                }
                // The game opens PipboyMenu onto the same screen texture ~100ms after the
                // boot fires (log-confirmed) — its tabs blend with the boot SWF. Keep its
                // movie root blanked EVERY frame while the boot owns the screen (it can
                // open at any point in this window); restored below on completion.
                if (SetPipboyMenuMovieVisible(false) && !_bootPipboyMenuBlanked) {
                    _bootPipboyMenuBlanked = true;
                    spdlog::info("[PIPBOY] Boot sequence: PipboyMenu movie blanked while boot SWF owns the screen");
                }
                // PipboyMenu is hidden, but its VR cursor/laser is rendered by
                // the menu layer independently of root1.visible. Suppress that
                // owner every frame so the boot screen has no white center dot.
                if (auto* ui = RE::UI::GetSingleton()) {
                    auto pipMenu = ui->GetMenu(MenuPipboy());
                    if (pipMenu) {
                        if (!_bootPipboyCursorOverridden) {
                            _bootPipboyCursorWasEnabled = pipMenu->UsesCursor();
                            _bootPipboyCursorOverridden = true;
                        }
                        pipMenu->menuFlags.reset(RE::UI_MENU_FLAGS::kUsesCursor);
                    }
                }
            } else if (_bootMenuSeen) {
                _bootMenuFinished = true;
                spdlog::info("[PIPBOY] Boot sequence: boot menu closed — sequence complete");
                if (_bootPipboyMenuBlanked) {
                    SetPipboyMenuMovieVisible(true);
                    _bootPipboyMenuBlanked = false;
                }
                // Keep the wrist render target lit through the display-mode
                // restoration and normal-menu handoff. Deactivating it here
                // produced the black interval that persisted until a manual
                // close/reopen.
                if (_bootHoloOverrideActive) {
                    _bootHoloRestoreSeconds = 0.5f;  // hand the display back to the player's Holo preference
                }
                if (_bootWristOverrideOwned) {
                    EndWristOverrideForHolotape();
                    _bootWristOverrideOwned = false;
                }
                // Vanilla flat behavior after the boot is that the Pip-Boy opens on the STATS
                // tab, and bPipboyBootOpensContent reproduces that. It is OFF by default because
                // that forced-open Pip-Boy could not be closed again (user-reported, log-confirmed
                // 2026-07-27): the content is opened through PipboyManager's native path, which
                // FRIK does not own, so a grip press does not close it. The fallback below then
                // called ClosedownPipboy() directly, which blanked the screen WITHOUT closing the
                // menu — leaving a black Pip-Boy stuck open (`pipOpen=true` for minutes after
                // "close requested"). Ending the boot with the Pip-Boy CLOSED hands open/close
                // back to FRIK, which works, and costs only the cosmetic auto-open.
                if (g_config.pipboyBootOpensContent) {
                    _bootContentOpenSeconds = 1.2f;
                } else {
                    if (auto* ui = RE::UI::GetSingleton()) {
                        if (auto pipMenu = ui->GetMenu(MenuPipboy());
                            pipMenu && _bootPipboyCursorOverridden) {
                            if (_bootPipboyCursorWasEnabled) {
                                pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
                            }
                        }
                    }
                    _bootPipboyCursorOverridden = false;
                    _bootPipboyCursorWasEnabled = false;
                    if (_bootScreenActivated) {
                        DeactivatePipboyScreen();
                        _bootScreenActivated = false;
                    }
                    spdlog::info(
                        "[PIPBOY] Boot sequence: leaving Pip-Boy closed after boot "
                        "(bPipboyBootOpensContent=false); FRIK owns open/close from here");
                }
            } else {
                _bootMenuAppearWaitSeconds -= deltaTime;
                if (_bootMenuAppearWaitSeconds <= 0.0f) {
                    _bootMenuFinished = true;
                    spdlog::warn("[PIPBOY] Boot sequence: boot menu never appeared — restoring display modes");
                    if (_bootHoloOverrideActive) {
                        _bootHoloRestoreSeconds = 0.5f;
                    }
                    if (_bootWristOverrideOwned) {
                        EndWristOverrideForHolotape();
                        _bootWristOverrideOwned = false;
                    }
                    if (_bootPipboyMenuBlanked) {
                        SetPipboyMenuMovieVisible(true);
                        _bootPipboyMenuBlanked = false;
                    }
                    if (_bootPipboyCursorOverridden) {
                        if (auto* ui = RE::UI::GetSingleton()) {
                            auto pipMenu = ui->GetMenu(MenuPipboy());
                            if (pipMenu && _bootPipboyCursorWasEnabled) {
                                pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
                            }
                        }
                        _bootPipboyCursorOverridden = false;
                        _bootPipboyCursorWasEnabled = false;
                    }
                    if (_bootScreenActivated) {
                        DeactivatePipboyScreen();
                        _bootScreenActivated = false;
                    }
                }
            }
        }

        // Post-boot content reveal. Use PipboyManager's native no-animation
        // open path, then wait for the actual menu object before handing over
        // the screen/cursor. A raw kShow was the black-tabs lifecycle bug.
        if (_bootContentOpenSeconds > 0.0f) {
            _bootContentOpenSeconds -= deltaTime;
            if (_bootContentOpenSeconds <= 0.0f) {
                auto* ui = RE::UI::GetSingleton();
                const bool alreadyOpen =
                    ui && ui->GetMenu(MenuPipboy()) != nullptr;
                const bool requested =
                    alreadyOpen || RequestNativePipboyContentOpen();
                if (requested) {
                    ActivatePipboyScreen();
                    _bootScreenActivated = true;
                    _bootContentOpenRequested = true;
                    _bootContentOpenWaitSeconds = 2.0f;
                    spdlog::info(
                        "[PIPBOY] Boot sequence: {} normal Pip-Boy content via native lifecycle",
                        alreadyOpen ? "adopting existing" : "opening");
                } else {
                    spdlog::warn(
                        "[PIPBOY] Boot sequence: native normal-content open unavailable");
                }
            }
        }

        if (_bootContentOpenRequested) {
            ActivatePipboyScreen();
            auto* ui = RE::UI::GetSingleton();
            auto pipMenu = ui ? ui->GetMenu(MenuPipboy()) : nullptr;
            if (pipMenu) {
                SetPipboyMenuMovieVisible(true);
                if (_bootPipboyCursorOverridden) {
                    if (_bootPipboyCursorWasEnabled) {
                        pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
                    }
                    _bootPipboyCursorOverridden = false;
                    _bootPipboyCursorWasEnabled = false;
                }
                _bootContentOpenRequested = false;
                _bootContentMenuSeen = true;
                _bootContentCloseArmSeconds = 0.5f;
                _bootContentGripWasPressed = false;
                _bootContentGripCloseRequested = false;
                spdlog::info(
                    "[PIPBOY] Boot sequence: normal Pip-Boy tabs confirmed visible");
            } else {
                _bootContentOpenWaitSeconds -= deltaTime;
                if (_bootContentOpenWaitSeconds <= 0.0f) {
                    _bootContentOpenRequested = false;
                    if (_bootPipboyCursorOverridden) {
                        _bootPipboyCursorOverridden = false;
                        _bootPipboyCursorWasEnabled = false;
                    }
                    if (_bootScreenActivated) {
                        DeactivatePipboyScreen();
                        _bootScreenActivated = false;
                    }
                    spdlog::warn(
                        "[PIPBOY] Boot sequence: native normal-content menu did not appear");
                }
            }
        }

        if (_bootContentMenuSeen && !_bootContentOpenRequested) {
            auto* ui = RE::UI::GetSingleton();
            const bool contentOpen =
                ui && ui->GetMenu(MenuPipboy()) != nullptr;
            const bool pipboyHandIsLeft =
                !g_vrInput.IsLeftHandedMode();
            const bool gripPressed =
                g_vrInput.IsPressed(
                    pipboyHandIsLeft,
                    VRButton::Grip);
            if (gripPressed) {
                _bootContentGripWasPressed = true;
            } else if (_bootContentGripWasPressed &&
                       !_bootContentGripCloseRequested &&
                       _bootContentCloseArmSeconds <= 0.0f &&
                       contentOpen) {
                _bootContentGripWasPressed = false;
                if (auto* manager = GetPipboyManagerVR()) {
                    EnableMenuInput(MenuPipboy());
                    manager->ClosedownPipboy();
                    _bootContentGripCloseRequested = true;
                    // Watchdog: ClosedownPipboy() does not always actually close the menu (it can
                    // blank the screen and leave the menu object alive). Without this the latch
                    // above stayed set forever and EVERY later grip press was ignored, stranding
                    // the player in front of a black Pip-Boy. Re-arm if the menu is still open.
                    _bootContentCloseRetrySeconds = 1.0f;
                    spdlog::info(
                        "[PIPBOY] Post-boot Holo Pip-Boy close requested by "
                        "{} grip release",
                        pipboyHandIsLeft ? "left" : "right");
                }
            }
            if (_bootContentGripCloseRequested &&
                _bootContentCloseRetrySeconds > 0.0f) {
                _bootContentCloseRetrySeconds -= deltaTime;
                if (_bootContentCloseRetrySeconds <= 0.0f && contentOpen) {
                    _bootContentGripCloseRequested = false;
                    _bootContentGripWasPressed = false;
                    spdlog::warn(
                        "[PIPBOY] Post-boot close did not take (menu still open) — "
                        "re-arming grip close");
                }
            }
            if (_bootContentCloseArmSeconds > 0.0f) {
                _bootContentCloseArmSeconds -= deltaTime;
                if (!contentOpen) {
                    // FRIK can close a menu during the immediate model handoff.
                    // Treat that as a race and re-enter through PipboyManager;
                    // a later close after this window is an intentional user close.
                    _bootContentMenuSeen = false;
                    _bootContentGripWasPressed = false;
                    _bootContentGripCloseRequested = false;
                    if (RequestNativePipboyContentOpen()) {
                        ActivatePipboyScreen();
                        _bootScreenActivated = true;
                        _bootContentOpenRequested = true;
                        _bootContentOpenWaitSeconds = 2.0f;
                        spdlog::info(
                            "[PIPBOY] Boot sequence: retried native content open after FRIK handoff close");
                    }
                }
            } else if (!contentOpen) {
                _bootContentMenuSeen = false;
                _bootContentGripWasPressed = false;
                _bootContentGripCloseRequested = false;
                if (_bootScreenActivated) {
                    DeactivatePipboyScreen();
                    _bootScreenActivated = false;
                }
                if (auto* playerNodes = f4vr::getPlayerNodes()) {
                    if (playerNodes->ScreenNode) {
                        playerNodes->ScreenNode->flags.flags |=
                            static_cast<std::uint64_t>(0x1);
                    }
                    if (playerNodes->PipboyRoot_nif_only_node) {
                        playerNodes->PipboyRoot_nif_only_node->local.scale =
                            0.0f;
                    }
                }
            }
        }

        // Restore FRIK's real Holo preference once the boot menu has finished. Kept
        // outside the !_bootSequenceFired gate above since it needs to keep running
        // AFTER that flips true.
        if (_bootHoloRestoreSeconds > 0.0f) {
            _bootHoloRestoreSeconds -= deltaTime;
            if (_bootHoloRestoreSeconds <= 0.0f) {
                if (EndTemporaryNormalPipboyModel()) {
                    _bootHoloOverrideActive = false;
                    _bootHoloRestoreSeconds = -1.0f;
                    spdlog::info(
                        "[PIPBOY] Boot sequence: restored stock FRIK Holo root");
                } else {
                    _bootHoloRestoreSeconds = 0.5f;
                    spdlog::warn(
                        "[PIPBOY] Boot sequence: stock FRIK root restore failed; retrying");
                }
            }
        }

        // FRIK Holo mode replaces PlayerNodes->PipboyRoot_nif_only_node only after
        // the projected/HMD flags change. Wait for that replacement to settle before
        // lighting the screen or queueing the intro SWF.
        UpdateIntroHoloOverride();

        // ── Intro holotape audio playback ──
        if (!inPowerArmor) UpdateIntroPlayback(deltaTime);

        // ── Delayed holotape playback (let slam sound finish) ──
        if (!inPowerArmor && _pendingPlaybackDelay > 0.0f) {
            _pendingPlaybackDelay -= deltaTime;
            if (_pendingPlaybackDelay <= 0.0f) {
                _pendingPlaybackDelay = 0.0f;
                auto formID = _pendingPlaybackFormID;
                _pendingPlaybackFormID = 0;

                if (formID != 0 && _holotapeLoaded && _loadedHolotapeFormID == formID) {
                    if (IsIntroHolotape(formID)) {
                        // Intro holotape uses custom WAV files (not in game's voice system)
                        StartIntroPlayback();
                    } else if (auto* noteForm = RE::TESForm::GetFormByID(formID);
                               noteForm && noteForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                        // Defer PlayPipboyLoadHolotapeAnim until PipboyMenu is open.
                        // The function expects PipboyMenu already running (vanilla: player
                        // selects holotape from Pipboy inventory). Open Pipboy first, then
                        // call PlayPipboyLoadHolotapeAnim once PipboyMenu is stable.
                        _pendingAudioFormID = formID;
                        _pendingAudioWaitFrames = 0;
                        ActivatePipboyScreen();
                        spdlog::debug("[PIPBOY] Audio holotape {:08X} — opening Pipboy, deferring playback",
                                     formID);
                    }
                }
            }
        }

        // ── Pending audio holotape — trigger PlayPipboyLoadHolotapeAnim when PipboyMenu is open ──
        if (!inPowerArmor && _pendingAudioFormID != 0) {
            auto& menuChecker = MenuChecker::GetSingleton();
            if (menuChecker.IsPipboyOpen()) {
                _pendingAudioWaitFrames++;
                if (_pendingAudioWaitFrames >= 3) {
                    auto formID = _pendingAudioFormID;
                    _pendingAudioFormID = 0;
                    _pendingAudioWaitFrames = 0;

                    if (_holotapeLoaded && _loadedHolotapeFormID == formID) {
                        if (auto* noteForm = RE::TESForm::GetFormByID(formID);
                            noteForm && noteForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                            auto* holotape = static_cast<RE::BGSNote*>(noteForm);
                            auto* pipMgr = GetPipboyManagerVR();
                            if (pipMgr) {
                                pipMgr->PlayPipboyLoadHolotapeAnim(holotape, true);
                                spdlog::info("[PIPBOY] PlayPipboyLoadHolotapeAnim {:08X} (type={} — PipboyMenu ready)",
                                             formID, holotape->type);
                            }
                        }
                    }
                }
            }
        }

        // ── Pending program holotape — trigger when PipboyMenu opens ──
        // Wait a few frames after PipboyMenu opens to ensure it's fully initialized
        // before opening the HolotapeMenu submenu.
        if (!inPowerArmor && _pendingProgramFormID != 0) {
            auto& menuChecker = MenuChecker::GetSingleton();
            if (menuChecker.IsPipboyOpen()) {
                _pendingProgramWaitFrames++;
                if (_pendingProgramWaitFrames < 2) {
                    // Still waiting for PipboyMenu to stabilize
                    if (_pendingProgramWaitFrames == 1) {
                        spdlog::debug("[PIPBOY] PipboyMenu open, waiting for HolotapeMenu open...");
                    }
                } else if (_pendingProgramWaitFrames == 2) {
                    // Frame 2: Close any existing HolotapeMenu so the SWF is fully destroyed.
                    // Must happen on a separate frame BEFORE opening a new one, otherwise
                    // the Scaleform system reuses the old movie clip and resumes mid-animation.
                    if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                        msgQueue->AddMessage(MenuHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                        spdlog::debug("[PIPBOY] Frame 2: Closed existing HolotapeMenu (kForceHide)");
                    }
                } else {
                    // Frame 3+: Old HolotapeMenu is gone, open a fresh one
                    auto formID = _pendingProgramFormID;
                    _pendingProgramFormID = 0;
                    _pendingProgramWaitFrames = 0;

                    if (auto* noteForm = RE::TESForm::GetFormByID(formID);
                        noteForm && noteForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                        auto* holotape = static_cast<RE::BGSNote*>(noteForm);

                        // Log holotape form details for diagnostics
                        spdlog::debug("[PIPBOY] Holotape {:08X}: type={} programFile='{}' isIntro={}",
                                     formID, holotape->type,
                                     holotape->programFile.c_str(),
                                     IsIntroHolotape(formID));

                        // Ensure type is kProgram (we deferred the switch for intro holotape)
                        if (holotape->type != RE::BGSNote::NOTE_TYPE::kProgram) {
                            spdlog::debug("[PIPBOY] Switching type from {} to kProgram(2)", holotape->type);
                            holotape->type = RE::BGSNote::NOTE_TYPE::kProgram;
                        }

                        auto* player = RE::PlayerCharacter::GetSingleton();
                        auto* pipMgr = GetPipboyManagerVR();

                        if (player && pipMgr) {
                            auto pipMgrAddr = reinterpret_cast<uintptr_t>(pipMgr);
                            auto playerAddr = reinterpret_cast<uintptr_t>(player);

                            // Store holotape at PipboyManager+0x190
                            *reinterpret_cast<RE::BGSNote**>(pipMgrAddr + 0x190) = holotape;
                            // Set active holotape on player+0xeb0
                            *reinterpret_cast<RE::TESBoundObject**>(playerAddr + 0xeb0) = holotape;

                            // Get program name — use GetNoteProgram() (checks type==kProgram)
                            RE::BSFixedString programName = holotape->GetNoteProgram();
                            const char* progCStr = programName.c_str();

                            spdlog::debug("[PIPBOY] Opening PipboyHolotapeMenu for {:08X} (programFile='{}' len={} ptr={:X})",
                                         formID, progCStr ? progCStr : "(null)",
                                         progCStr ? strlen(progCStr) : 0,
                                         reinterpret_cast<uintptr_t>(progCStr));

                            if (!progCStr || progCStr[0] == '\0') {
                                spdlog::error("[PIPBOY] Empty program name! Aborting HolotapeMenu open.");
                            } else {
                                // Call the game's PipboyHolotapeMenu opener (FUN_140b8e2c0)
                                using OpenPipboyHolotapeMenuFunc = void(*)(const RE::BSFixedString&);
                                static REL::Relocation<OpenPipboyHolotapeMenuFunc> openHolotapeMenu{
                                    REL::Offset(0xb8e2c0) };
                                openHolotapeMenu(programName);
                                spdlog::info("[PIPBOY] openHolotapeMenu() returned OK");

                                // Clear kPausesGame from holotape menus so the world keeps running
                                ClearMenuPauseFlag(MenuHolotape());
                                ClearMenuPauseFlag(MenuPipboyHolotape());
                                ClearMenuPauseFlag(MenuPipboy());
                                // Audio sync now handled inside ClearMenuPauseFlag via SFX counter
                                // Keep trying for a few frames (menu may be created async)
                                _holotapePauseClearFrames = 10;

                                // Mark intro SWF as active — audio starts when Pipboy closes
                                if (IsIntroHolotape(formID)) {
                                    _introSWFActive = true;
                                    // Disable input for intro SWF (non-interactive animation)
                                    DisableMenuInput(MenuHolotape());
                                    DisableMenuInput(MenuPipboyHolotape());
                                    DisableMenuInput(MenuPipboy());
                                    spdlog::info("[PIPBOY] Intro SWF active — audio will start when Pipboy closes");
                                } else {
                                    _programSWFActive = true;
                                    // Game holotapes: let native input system handle everything
                                    spdlog::info("[PIPBOY] Game holotape SWF active — native input enabled");
                                }
                            }
                        }
                    }
                }
            } else {
                // PipboyMenu not open yet — reset frame counter
                _pendingProgramWaitFrames = 0;
            }
        }

        // ── Intro SWF: sound events + audio start + Pipboy close ──
        // Uses std::chrono for real wall-clock time (matches SWF getTimer()).
        // Sound events are precomputed to match the SWF animation timeline exactly.
        // All sounds played via C++ PlayWavSound (game sounds via SWF playSound loop incorrectly).
        // Per-frame input clearing for holotape menus (catches async creation)
        if (_holotapePauseClearFrames > 0) {
            _holotapePauseClearFrames--;
            ClearMenuPauseFlag(MenuHolotape());
            ClearMenuPauseFlag(MenuPipboyHolotape());
            ClearMenuPauseFlag(MenuPipboy());
            // Audio sync now handled inside ClearMenuPauseFlag via SFX counter
            // Only disable input for intro SWF (non-interactive); game holotapes use native input
            if (!_programSWFActive) {
                DisableMenuInput(MenuHolotape());
                DisableMenuInput(MenuPipboyHolotape());
                DisableMenuInput(MenuPipboy());
            }

        }

        // Safety net: restore PipboyMenu input whenever holotape boot is done.
        // DisableMenuInput(MenuPipboy()) is called during boot to prevent tab-switching
        // from interfering with the SWF, but must be re-enabled afterward.
        if (_holotapePauseClearFrames == 0 && !_introSWFActive && !_programSWFActive) {
            EnableMenuInput(MenuPipboy());
        }

        // Game holotape: disable right-controller cursor on PipboyMenu each frame
        // so the VR laser pointer can't click Pipboy tabs behind the game.
        // Holotape game input comes through keyboard-style events (left controller).
        if (_programSWFActive) {
            auto* ui = RE::UI::GetSingleton();
            if (ui) {
                auto pipMenu = ui->GetMenu(MenuPipboy());
                if (pipMenu) {
                    pipMenu->menuFlags.reset(RE::UI_MENU_FLAGS::kUsesCursor);
                }
            }
            auto& mc = MenuChecker::GetSingleton();
            if (!mc.IsPipboyOpen()) {
                _programSWFActive = false;
                // Restore cursor flag
                if (ui) {
                    auto pipMenu = ui->GetMenu(MenuPipboy());
                    if (pipMenu) {
                        pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
                    }
                }
                spdlog::info("[PIPBOY] Pipboy closed — game holotape SWF ended");
            }
        }

        if (!inPowerArmor && _introSWFActive) {
            auto& menuChecker = MenuChecker::GetSingleton();
            bool pipboyOpen = menuChecker.IsPipboyOpen();

            // If tape deck was opened during boot sequence, stop everything
            if (_tapeDeckOpen) {
                spdlog::debug("[PIPBOY] Tape deck opened during intro SWF — aborting boot sequence");
                StopIntroPlayback();  // Stops intro handle; deck one-shots are fire-and-forget
                RestoreIntroHolotapeType(_introHolotapeFormID);
                _introSWFActive = false;

                _introSWFMenuSeen = false;
                _introSWFLastLogSec = 0;
                _introAudioStarted = false;
                _introSoundEvents.clear();
                _introSoundEventIndex = 0;
                _pendingProgramFormID = 0;

                // Close HolotapeMenu so the SWF is fully destroyed.
                // PipboyMenu stays open — FRIK controls its lifecycle.
                // Closing PipboyMenu ourselves bypasses FRIK's _isOpen flag,
                // leaving the player permanently restrained.
                if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                    msgQueue->AddMessage(MenuPipboyHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                    msgQueue->AddMessage(MenuHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                    spdlog::debug("[PIPBOY] Closed PipboyHolotapeMenu+HolotapeMenu on deck-open abort (kForceHide)");
                }
                // Fall through — _introSWFActive is now false, won't re-enter next frame
            }

            if (pipboyOpen && !_introSWFMenuSeen) {
                _introSWFMenuSeen = true;
                _introSWFStartTime = std::chrono::steady_clock::now();

                // Build sound event timeline (seconds from Pipboy open).
                // Timing derived from SWF constants: splash 3s, typing 33ms/char,
                // line delay 200ms, blink cycle 500ms×3, auto-advance 2s, loading blink 667ms.
                // Hard drive sounds play when each line starts typing.
                // ok_passgood.wav plays when [OK] appears after blink-OK.
                _introSoundEvents.clear();
                _introSoundEventIndex = 0;
                _introAudioStarted = false;
                // Timeline from frame-accurate 90fps simulation of SWF state machine.
                // Available HD sounds: A_01..A_08, A_11..A_15 (13 files, game skips 09/10).
                _introSoundEvents = {
                    {  0.30f, "robco x sped down.wav" },                 // Boot beep during splash
                    // ── Page 0 lines ──
                    {  3.00f, "UI_Terminal_HardDrive_A_01.wav" },     // ROBCO INDUSTRIES (TM)
                    {  3.93f, "UI_Terminal_HardDrive_A_02.wav" },     // TERMLINK PROTOCOL
                    {  4.72f, "UI_Terminal_HardDrive_A_03.wav" },     // ==============================
                    {  5.78f, "UI_Terminal_HardDrive_A_04.wav" },     // HEISENBERG PHYSICAL
                    {  6.28f, "UI_Terminal_HardDrive_A_05.wav" },     // INTERACTIONS SYSTEM v0.7
                    {  7.30f, "UI_Terminal_HardDrive_A_06.wav" },     // INITIALIZING VR SUBSYSTEMS...
                    // ── Page 1 status lines (type → blink 3× → [OK]) ──
                    {  9.89f, "UI_Terminal_HardDrive_A_07.wav" },     // ESTABLISHING VIEWCASTER LINK
                    { 12.36f, "ok_passgood.wav" },                    // [OK] after 3 blinks
                    { 12.55f, "UI_Terminal_HardDrive_A_08.wav" },     // LOADING 843 OFFSETS
                    { 14.52f, "ok_passgood.wav" },                    // [OK] after 3 blinks
                    { 14.72f, "UI_Terminal_HardDrive_A_11.wav" },     // WIRING UP INTERACTION ZONES
                    { 16.99f, "ok_passgood.wav" },                    // [OK] after 3 blinks
                    { 17.19f, "UI_Terminal_HardDrive_A_12.wav" },     // SETTING UP TERMINAL REDIRECT
                    { 19.45f, "ok_passgood.wav" },                    // [OK] after 3 blinks
                    { 19.66f, "UI_Terminal_HardDrive_A_13.wav" },     // INITIALISING SMART RETRIEVAL SYSTEM
                    { 20.66f, "ok_passgood.wav" },                    // [OK] immediate
                    // ── Page 1 remaining lines ──
                    { 21.05f, "UI_Terminal_HardDrive_A_14.wav" },     // ALL SYSTEMS NOMINAL
                    { 21.75f, "UI_Terminal_HardDrive_A_15.wav" },     // ==============================
                    // ── Loading blinks (phase 3 at 22.38s, cycle 667ms) ──
                    { 22.38f, "UI_Terminal_HardDrive_A_01.wav" },     // LOADING AUDIO... blink 1
                    { 23.04f, "UI_Terminal_HardDrive_A_02.wav" },     // LOADING AUDIO... blink 2
                    { 23.71f, "UI_Terminal_HardDrive_A_03.wav" },     // LOADING AUDIO... blink 3
                };
                spdlog::debug("[PIPBOY] Intro SWF started — {} sound events queued", _introSoundEvents.size());
            }

            // Per-frame: force ScreenNode visible during intro SWF (FRIK tries to hide it)
            // Only needed for wrist mode — in projected/HMD mode the game manages the screen.
            if (_introSWFMenuSeen && f4vr::isPipboyOnWrist()) {
                auto* player2 = f4vr::getPlayer();
                if (player2) {
                    auto playerAddr = reinterpret_cast<uintptr_t>(player2);
                    auto screenPtr = *reinterpret_cast<uintptr_t*>(playerAddr + 0x07B8);
                    if (screenPtr > 0x10000) {
                        auto* frikScreen = reinterpret_cast<RE::NiNode*>(screenPtr);
                        ForceAncestorScales(frikScreen);
                        frikScreen->flags.flags &= ~static_cast<uint64_t>(0x1);
                    }
                }
            }


            float realElapsed = 0.0f;
            if (_introSWFMenuSeen) {
                auto now = std::chrono::steady_clock::now();
                realElapsed = std::chrono::duration<float>(now - _introSWFStartTime).count();
            }

            // Play sound events from precomputed timeline (stop once intro audio starts)
            if (_introSWFMenuSeen && !_introAudioStarted) {
                while (_introSoundEventIndex < static_cast<int>(_introSoundEvents.size()) &&
                       realElapsed >= _introSoundEvents[_introSoundEventIndex].first) {
                    PlayWavSound(_introSoundEvents[_introSoundEventIndex].second.c_str());
                    spdlog::debug("[PIPBOY] Sound event {} at {:.2f}s: {}",
                        _introSoundEventIndex, realElapsed,
                        _introSoundEvents[_introSoundEventIndex].second);
                    _introSoundEventIndex++;
                }
            }

            // Log periodically for debugging (every ~5 seconds)
            int sec = static_cast<int>(realElapsed);
            if (sec > 0 && sec != _introSWFLastLogSec && sec % 5 == 0) {
                _introSWFLastLogSec = sec;
                spdlog::debug("[PIPBOY] Intro SWF: real={:.1f}s pipboy={} audioStarted={}",
                    realElapsed, pipboyOpen, _introAudioStarted);
            }

            // At ~24.9s: start intro audio playback (0.5s after closeHolotape at 24.38s)
            if (_introSWFMenuSeen && !_introAudioStarted && realElapsed > 24.9f) {
                spdlog::debug("[PIPBOY] Timer {:.1f}s — starting intro audio", realElapsed);
                _introAudioStarted = true;
                StartIntroPlayback();
            }

            // At ~25.9s: close Pipboy (1s after audio start, gives closeHolotape time to process)
            // Restore INI angles FIRST so the game doesn't immediately reopen the Pipboy,
            // then re-enable menu input (so close events can process), then close.
            if (_introSWFMenuSeen && _introAudioStarted && pipboyOpen && realElapsed > 25.9f) {
                DeactivatePipboyScreen();  // Restore INI angles before closing
                EnableMenuInput(MenuPipboy());
                EnableMenuInput(MenuHolotape());
                EnableMenuInput(MenuPipboyHolotape());
                auto* pbm = GetPipboyManagerVR();
                if (pbm) {
                    pbm->ClosedownPipboy();
                    spdlog::debug("[PIPBOY] ClosedownPipboy() retry at {:.1f}s", realElapsed);
                } else {
                    if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                        msgQueue->AddMessage(MenuPipboy(), RE::UI_MESSAGE_TYPE::kForceHide);
                    }
                }
            }

            // Pipboy closed after audio started → done, reset all intro state
            if (_introSWFMenuSeen && _introAudioStarted && !pipboyOpen) {
                spdlog::info("[PIPBOY] Intro complete — Pipboy closed, resetting state");
                RestoreIntroHolotapeType(_introHolotapeFormID);
                _introSWFActive = false;

                _introSWFMenuSeen = false;
                _introSWFLastLogSec = 0;
                _introAudioStarted = false;
                _introSoundEvents.clear();
                _introSoundEventIndex = 0;
                DeactivatePipboyScreen();  // Extra safety — ensure angles are restored
                {
                    static auto setKeepOpen = []() -> void(*)(bool) {
                        auto frikDll = GetModuleHandleA("FRIK.dll");
                        if (!frikDll) return nullptr;
                        return reinterpret_cast<void(*)(bool)>(
                            GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                    }();
                    if (setKeepOpen) setKeepOpen(false);
                }
            }

            // If user manually closes Pipboy before timer → start audio early
            if (_introSWFMenuSeen && !pipboyOpen && !_introAudioStarted) {
                spdlog::info("[PIPBOY] Pipboy manually closed at {:.1f}s — starting audio early", realElapsed);
                StartIntroPlayback();
                // Reset all state
                RestoreIntroHolotapeType(_introHolotapeFormID);
                _introSWFActive = false;

                _introSWFMenuSeen = false;
                _introSWFLastLogSec = 0;
                _introAudioStarted = false;
                _introSoundEvents.clear();
                _introSoundEventIndex = 0;
                DeactivatePipboyScreen();
                {
                    static auto setKeepOpen = []() -> void(*)(bool) {
                        auto frikDll = GetModuleHandleA("FRIK.dll");
                        if (!frikDll) return nullptr;
                        return reinterpret_cast<void(*)(bool)>(
                            GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                    }();
                    if (setKeepOpen) setKeepOpen(false);
                }
            }
        }

        // ── Terminal-on-Pipboy redirect ──
        // Note: We previously forced PipboyManager+0x1a=1 every frame here, but that
        // broke normal pipboy operation (projected pipboy appeared instead of wrist).
        // The binary patches in Hooks.cpp (JNZ→JMP at 0xc33d00, JZ→JMP at 0x2ede5e)
        // already force the non-projected rendering path for all terminals.
        // World terminal detection handles the WorldRoot swap when TerminalMenu opens.

        // One-way latch: the player must have LEFT Vault 111 (reached an exterior cell) before the
        // terminal-on-wrist redirect may engage. Activating it inside the vault — at Pip-Boy pickup,
        // during the vanilla green RobCo bootup — corrupts the Pip-Boy screen/menu state (bootup not
        // displayed, inventory overlapping). Seeded from introHolotapePlayed in ClearState so
        // established characters aren't gated; flips true the first frame the player is outdoors.
        if (!_hasReachedExterior) {
            auto* extPlayer = RE::PlayerCharacter::GetSingleton();
            auto* extCell = extPlayer ? extPlayer->GetParentCell() : nullptr;
            if (extCell && !extCell->IsInterior()) {
                _hasReachedExterior = true;
                spdlog::info("[PIPBOY] Player reached exterior — terminal-on-wrist redirect now permitted");
            }
        }

        // Dynamic toggle: revert terminal patches when in PA or projected mode
        // so terminals use the normal native game path (projected/fullscreen).
        if (g_config.forceTerminalOnWrist) {
            // Use the native (projected/fullscreen) terminal path until the player has LEFT the
            // vault AND the world is settled. Never flip the redirect byte-patches inside Vault 111,
            // during a loadscreen, or before the Pip-Boy exists — that's the "pipboy hacking
            // disturbs the bootup" report. IsSessionReady() gives a 5s grace after the loadscreen
            // closes. We do NOT gate on IsPipboyOpen() here — an actual on-wrist terminal redirect
            // can run with the Pip-Boy menu open, so that would break the feature later.
            bool needNativePath = Utils::IsPlayerInPowerArmor()
                || !_hasReachedExterior                          // still in / never left Vault 111
                || !PlayerHasReceivedPipboy()
                || GetCachedTapeDeckNode() == nullptr
                || !heisenberg::DropToHand::IsSessionReady()
                || heisenberg::MenuChecker::GetSingleton().IsLoading();
            if (needNativePath != _terminalPatchesSuspended) {
                Hooks::ApplyTerminalPatches(!needNativePath);
                _terminalPatchesSuspended = needNativePath;
                spdlog::info("[PIPBOY] Terminal patches {} (PA/projected={})",
                            needNativePath ? "suspended" : "restored", needNativePath);
            }
        }

        // Early blur/IMOD suppression: catch UIBlurManager on the very first frame
        // TerminalMenu opens, before any terminal state checks.
        if (g_config.forceTerminalOnWrist && !_terminalPatchesSuspended) {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GetMenuOpen(MenuTerminal())) {
                SuppressTerminalDarkening();
            }
        }

        // Pre-arm FRIK keepOpen flag so Pipboy stays visible when terminal opens
        if (g_config.forceTerminalOnWrist && !_terminalPatchesSuspended &&
            !_terminalRedirectActive && !_pendingTerminalRedirect) {
            static auto setKeepOpenPreArm = []() -> void(*)(bool) {
                auto frikDll = GetModuleHandleA("FRIK.dll");
                if (!frikDll) return nullptr;
                auto fn = reinterpret_cast<void(*)(bool)>(
                    GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                spdlog::debug("[PIPBOY] FRIK SetKeepPipboyOpenForTerminal: {}",
                             fn ? "FOUND" : "NOT FOUND (v0.77 compat mode)");
                return fn;
            }();
            if (setKeepOpenPreArm) setKeepOpenPreArm(true);
            // NOTE: Do NOT force +0x1a=1 every frame here — it breaks normal Pipboy
            // operation (Pipboy shows projected instead of wrist). The binary patches
            // in Hooks.cpp (JNZ→JMP at 0xc33d00, JZ→JMP at 0x2ede5e) already force
            // the non-projected wrist path for all terminal activations.
        }

        // Pending terminal redirect: wait for TerminalMenu to open, then do WorldRoot swap
        if (_pendingTerminalRedirect) {
            static int termWaitFrames = 0;
            SafeHideRollover();
            SuppressTerminalDarkening();

            auto* ui = RE::UI::GetSingleton();
            bool termMenuOpen = ui && ui->GetMenuOpen(MenuTerminal());

            if (termMenuOpen) {
                // Unlock render singletons
                static auto layerSingletonAddr3 = REL::Offset(0x5ac8eb0).address();
                static auto modeSingletonAddr3  = REL::Offset(0x5ac72b8).address();
                auto layerS = *reinterpret_cast<uintptr_t*>(layerSingletonAddr3);
                auto modeS  = *reinterpret_cast<uintptr_t*>(modeSingletonAddr3);
                if (layerS) *reinterpret_cast<int*>(layerS + 0x374) = _savedLayerLock;
                if (modeS)  *reinterpret_cast<int*>(modeS + 0x374) = _savedModeLock;

                int curLayer = layerS ? *reinterpret_cast<int*>(layerS + 0x36c) : -1;
                int curMode  = modeS  ? *reinterpret_cast<int*>(modeS + 0x36c) : -1;

                // Force FRIK ScreenNode visible (don't touch scale — FRIK manages it)
                RE::NiNode* frikScreen = nullptr;
                auto* player2 = f4vr::getPlayer();
                if (player2) {
                    auto playerAddr = reinterpret_cast<uintptr_t>(player2);
                    auto screenPtr = *reinterpret_cast<uintptr_t*>(playerAddr + 0x07B8);
                    if (screenPtr > 0x10000) {
                        frikScreen = reinterpret_cast<RE::NiNode*>(screenPtr);
                        ForceAncestorScales(frikScreen);
                        frikScreen->flags.flags &= ~static_cast<uint64_t>(0x1);
                    }
                }

                spdlog::debug("[PIPBOY] TerminalMenu ready (waited {} frames), unlocked. "
                             "layer={:#x} mode={:#x} screenScale={:.3f}",
                             termWaitFrames, curLayer, curMode,
                             frikScreen ? frikScreen->world.scale : -1.0f);

                _pendingTerminalRedirect = false;
                _terminalRedirectActive = true;
                termWaitFrames = 0;

                spdlog::info("[PIPBOY] Terminal redirect ACTIVATED (holotape path) — checking HUD vtable");
                heisenberg::Hooks::CheckHUDRolloverVtableIntegrity();

                // WorldRoot swap to FRIK's Pipboy ScreenNode
                void* pipRend = I3D_GetByName("PipboyMenu");
                if (pipRend && frikScreen) {
                    auto rendBase = reinterpret_cast<uintptr_t>(pipRend);
                    auto oldWorldRoot = *reinterpret_cast<uintptr_t*>(rendBase + 0x088);

                    I3D_LogRenderer("PipboyMenu-BEFORE", pipRend);

                    if (oldWorldRoot > 0x10000) {
                        auto* oldScreen = reinterpret_cast<RE::NiAVObject*>(oldWorldRoot);
                        if (IsAncestorOf(oldScreen, frikScreen)) {
                            ForceAncestorScales(frikScreen);
                            _savedHmdScreenNode = nullptr;
                            _savedHmdScreenParent = nullptr;
                            spdlog::debug("[PIPBOY] Old worldRoot '{}' is ancestor of ScreenNode — "
                                         "keeping attached",
                                         oldScreen->name.c_str() ? oldScreen->name.c_str() : "?");
                        } else {
                            oldScreen->flags.flags |= 0x1;
                            oldScreen->local.scale = 0.0f;
                            _savedHmdScreenParent = oldScreen->parent;
                            if (oldScreen->parent) {
                                oldScreen->parent->DetachChild(oldScreen);
                            }
                            _savedHmdScreenNode = oldScreen;
                            spdlog::debug("[PIPBOY] Detached+hidden old worldRoot '{}' (separate node)",
                                         oldScreen->name.c_str() ? oldScreen->name.c_str() : "?");
                        }
                    }

                    _savedRendererPtr = pipRend;
                    _savedOrigWorldRoot = oldWorldRoot;

                    *reinterpret_cast<uintptr_t*>(rendBase + 0x088) =
                        reinterpret_cast<uintptr_t>(frikScreen);

                    spdlog::debug("[PIPBOY] WorldRoot swapped to FRIK ScreenNode '{}' (scale={:.3f})",
                                 frikScreen->name.c_str() ? frikScreen->name.c_str() : "?",
                                 frikScreen->world.scale);
                    I3D_LogRenderer("PipboyMenu-AFTER", pipRend);
                }
            } else {
                termWaitFrames++;
                if (termWaitFrames > 300) {
                    static auto layerSingletonAddr4 = REL::Offset(0x5ac8eb0).address();
                    static auto modeSingletonAddr4  = REL::Offset(0x5ac72b8).address();
                    auto layerS4 = *reinterpret_cast<uintptr_t*>(layerSingletonAddr4);
                    auto modeS4  = *reinterpret_cast<uintptr_t*>(modeSingletonAddr4);
                    if (layerS4 && _savedLayerValue != -1)
                        *reinterpret_cast<int*>(layerS4 + 0x36c) = _savedLayerValue;
                    if (modeS4 && _savedModeValue != -1)
                        *reinterpret_cast<int*>(modeS4 + 0x36c) = _savedModeValue;
                    if (layerS4) *reinterpret_cast<int*>(layerS4 + 0x374) = _savedLayerLock;
                    if (modeS4)  *reinterpret_cast<int*>(modeS4 + 0x374) = _savedModeLock;
                    _pendingTerminalRedirect = false;
                    termWaitFrames = 0;
                    spdlog::warn("[PIPBOY] TerminalMenu never appeared after 300 frames, giving up");
                }
            }
        }

        // Active terminal redirect: per-frame forcing + close detection
        if (_terminalRedirectActive) {
            SafeHideRollover();

            // Open console to suppress terminal darkening.
            // The projected display 'Screen' NiNode has no parent — it's rendered via a
            // special VR overlay path that ignores AppCull, scale, and scene graph detach.
            // The only proven fix: opening the console pauses the game loop, which prevents
            // the overlay render from running. Terminal hacking is menu-driven and still works.
            // We hide the console UI per-frame so it's not visible to the player.
            // Open console to suppress projected overlay / terminal darkening.
            // Opening console triggers kPausesGame for 1 frame which swallows audio playing
            // at that moment, so we control the delay carefully:
            //   World terminals: open immediately (0-frame delay) so the 1-frame pause
            //     happens before the terminal intro audio starts — audio then plays uninterrupted.
            //   Holotape terminals: delay 30 frames so initial click sounds can play first.
            if (!_consoleOpenedForTerminal) {
                // Save radio state before Console opens (Console pauses audio including radio)
                _radioWasEnabled = IsPlayerRadioEnabled();

                auto* msgQueue = RE::UIMessageQueue::GetSingleton();
                if (msgQueue) {
                    msgQueue->AddMessage(MenuConsole(), RE::UI_MESSAGE_TYPE::kShow);
                    _consoleOpenedForTerminal = true;
                    spdlog::debug("[PIPBOY] Opened console to suppress terminal darkening (radio was {})",
                                 _radioWasEnabled ? "ON" : "OFF");
                }
            }

            // Clear kPausesGame from all terminal-related menus so the world keeps running.
            // ClearMenuPauseFlag also reverses audio counter increments from MenuModeCounterListener.
            bool consoleFlagCleared = ClearMenuPauseFlag(MenuConsole());
            ClearMenuPauseFlag(MenuTerminal());
            ClearMenuPauseFlag(MenuTerminalButtons());
            ClearMenuPauseFlag(MenuPipboy());

            // Schedule deferred radio restart after Console's kPausesGame is cleared.
            // The radio has its own enable flag separate from SFX counters.
            // Toggle off→on to force a full stream restart.
            if (consoleFlagCleared && _radioWasEnabled) {
                _radioRestoreFrames = 5;
                _radioWasEnabled = false;  // Consumed — don't re-schedule
                spdlog::debug("[PIPBOY] Scheduled radio restart in 5 frames");
            }

            // Deferred radio restart countdown
            if (_radioRestoreFrames > 0) {
                if (--_radioRestoreFrames == 0) {
                    // Toggle off→on forces a full restart of the radio stream
                    SetPlayerRadioEnabled(false, false);
                    SetPlayerRadioEnabled(true, false);
                    spdlog::debug("[PIPBOY] Restarted player radio (off→on toggle)");
                }
            }

            // Hide the console UI per-frame (so the text overlay isn't visible)
            {
                auto* uiCon = RE::UI::GetSingleton();
                if (uiCon) {
                    auto consoleMenu = uiCon->GetMenu(MenuConsole());
                    if (consoleMenu && consoleMenu->uiMovie) {
                        consoleMenu->uiMovie->SetVisible(false);
                    }
                }
            }

            // Hide terminal button bar ("[Grab] Exit" / "[X] Holotape")
            // VR renders menus to offscreen textures, so GFx SetVisible/alpha don't work.
            // Instead, set menuCanBeVisible=false on the IMenu itself.
            if (g_config.hideTerminalExitPrompt) {
                auto* ui2 = RE::UI::GetSingleton();
                if (ui2) {
                    auto termBtnMenu = ui2->GetMenu(MenuTerminalButtons());
                    if (termBtnMenu) {
                        termBtnMenu->menuCanBeVisible = false;
                    }
                }
            }

            SuppressTerminalDarkening();

            // Per-frame forcing for all terminal types (holotape + world)
            {
                // Per-frame: force render singletons to non-projected values
                {
                    static auto layerSingletonAddr = REL::Offset(0x5ac8eb0).address();
                    static auto modeSingletonAddr  = REL::Offset(0x5ac72b8).address();
                    auto layerS = *reinterpret_cast<uintptr_t*>(layerSingletonAddr);
                    auto modeS  = *reinterpret_cast<uintptr_t*>(modeSingletonAddr);
                    if (layerS) {
                        auto& val = *reinterpret_cast<int*>(layerS + 0x36c);
                        if (val != 0x3) val = 0x3;
                    }
                    if (modeS) {
                        auto& val = *reinterpret_cast<int*>(modeS + 0x36c);
                        if (val != 0x20) val = 0x20;
                    }
                }

                // Per-frame: force FRIK ScreenNode visible + scaled.
                //
                // FRIK hides its holo Pip-Boy the instant a terminal opens — its own log says
                // "Close Pipboy due to terminal open..." / "Turning Pipboy OFF", measured 16ms
                // BEFORE our redirect activates. Pipboy::openClose(false) does exactly two things
                // to the scene graph: setNodeVisibility(ScreenNode,false) and
                // PipboyRoot_nif_only_node->local.scale = 0. The two lines below undo both, which
                // is the only reason the terminal is visible on the wrist at all.
                //
                // Because we resurrect a model FRIK believes it already closed, FRIK can never
                // put it back: openClose() early-returns on `_isOpen == open`, and its _isOpen is
                // already false. So the teardown MUST restore the hidden state itself — see
                // RestoreFrikHoloHiddenStateAfterTerminal(). Without that the model stays visible
                // until the player manually opens and closes the Pip-Boy, which is precisely the
                // reported "grip doesn't close the holo Pip-Boy, I have to use trigger".
                _terminalForcedFrikScreenVisible = true;
                {
                    auto* player2 = f4vr::getPlayer();
                    if (player2) {
                        auto playerAddr = reinterpret_cast<uintptr_t>(player2);
                        auto screenPtr = *reinterpret_cast<uintptr_t*>(playerAddr + 0x07B8);
                        if (screenPtr > 0x10000) {
                            auto* frikScreen = reinterpret_cast<RE::NiNode*>(screenPtr);
                            ForceAncestorScales(frikScreen);
                            frikScreen->flags.flags &= ~static_cast<uint64_t>(0x1);
                        }
                    }
                }
            }

            // Per-frame SRV swap (holotape terminals with physical screen node)
            if (!_isWorldTerminalRedirect && _terminalScreenNode) {
                auto screenAddr = reinterpret_cast<uintptr_t>(_terminalScreenNode);
                auto spAddr = *reinterpret_cast<uintptr_t*>(screenAddr + 0x178);
                if (spAddr) {
                    auto matAddr = *reinterpret_cast<uintptr_t*>(spAddr + 0x58);
                    if (matAddr) {
                        auto diffuseTexPtr = *reinterpret_cast<uintptr_t*>(matAddr + 0x40);
                        if (diffuseTexPtr) {
                            auto rendTexPtr = *reinterpret_cast<uintptr_t*>(diffuseTexPtr + 0x38);
                            if (rendTexPtr) {
                                static auto rendererDataPtrAddr2 = REL::Offset(0x60f3ce8).address();
                                auto rendererData = *reinterpret_cast<uintptr_t*>(rendererDataPtrAddr2);
                                if (rendererData) {
                                    auto rt3fSRV = *reinterpret_cast<uintptr_t*>(
                                        rendererData + 0x0A58 + 0x3F * 0x30 + 0x18);
                                    if (rt3fSRV) {
                                        *reinterpret_cast<uintptr_t*>(rendTexPtr + 0x00) = rt3fSRV;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Detect session end: EITHER the terminal menu closed, OR the player exited the
            // Pipboy. In projected mode, loading a terminal holotape can leave TerminalMenu
            // flagged open after the player closes the Pipboy, so the old close-detection
            // (!termMenuOpen only) never fired — FRIK kept the Pipboy held open and player
            // controls stayed locked (= stuck, can't move). Treat a Pipboy close during the
            // redirect as session end too.
            auto* ui = RE::UI::GetSingleton();
            bool termMenuOpen = ui && ui->GetMenuOpen(MenuTerminal());
            bool pipOpen = heisenberg::MenuChecker::GetSingleton().IsPipboyOpen();
            if (!termMenuOpen || !pipOpen) {
                spdlog::info("[PIPBOY] Terminal redirect ending — termMenuOpen={} pipOpen={} menuMode={} (world={})",
                             termMenuOpen, pipOpen, ui ? ui->menuMode : -1, _isWorldTerminalRedirect);

                // If the player exited the Pipboy while the terminal was still flagged open,
                // close the terminal too so it doesn't linger and keep the game paused.
                if (termMenuOpen) {
                    if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                        mq->AddMessage(MenuTerminal(), RE::UI_MESSAGE_TYPE::kHide);
                        spdlog::info("[PIPBOY] Pipboy closed with terminal still open — force-closing TerminalMenu");
                    }
                }
                // Clear any residual holotape-menu pause that a terminal-loaded holotape left
                // behind (only acts if the menu is still open with kPausesGame set).
                ClearMenuPauseFlag(MenuPipboyHolotape());
                ClearMenuPauseFlag(MenuHolotape());

                // 1. Signal FRIK to stop holding Pipboy open
                {
                    static auto setKeepOpen = []() -> void(*)(bool) {
                        auto frikDll = GetModuleHandleA("FRIK.dll");
                        if (!frikDll) return nullptr;
                        return reinterpret_cast<void(*)(bool)>(
                            GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                    }();
                    if (setKeepOpen) setKeepOpen(false);
                }

                // 2. Close console
                if (_consoleOpenedForTerminal) {
                    auto* msgQueue = RE::UIMessageQueue::GetSingleton();
                    if (msgQueue) {
                        msgQueue->AddMessage(MenuConsole(), RE::UI_MESSAGE_TYPE::kHide);
                        spdlog::debug("[PIPBOY] Closed console (terminal redirect ended)");
                    }
                    _consoleOpenedForTerminal = false;
                }

                // 3. Restore SRV on screen node
                if (_terminalScreenNode && _savedDiffuseSRV) {
                    auto screenAddr = reinterpret_cast<uintptr_t>(_terminalScreenNode);
                    auto spAddr = *reinterpret_cast<uintptr_t*>(screenAddr + 0x178);
                    if (spAddr) {
                        auto matAddr = *reinterpret_cast<uintptr_t*>(spAddr + 0x58);
                        if (matAddr) {
                            auto diffuseTexPtr = *reinterpret_cast<uintptr_t*>(matAddr + 0x40);
                            if (diffuseTexPtr) {
                                auto rendTexPtr = *reinterpret_cast<uintptr_t*>(diffuseTexPtr + 0x38);
                                if (rendTexPtr) {
                                    *reinterpret_cast<uintptr_t*>(rendTexPtr + 0x00) = _savedDiffuseSRV;
                                }
                            }
                        }
                    }
                }

                // 4. Restore I3D worldRoot
                if (_savedRendererPtr && _savedOrigWorldRoot) {
                    auto rendBase = reinterpret_cast<uintptr_t>(_savedRendererPtr);
                    *reinterpret_cast<uintptr_t*>(rendBase + 0x088) = _savedOrigWorldRoot;
                }

                // 5. Restore render singleton values AND locks
                {
                    static auto layerSingletonAddrClose = REL::Offset(0x5ac8eb0).address();
                    static auto modeSingletonAddrClose  = REL::Offset(0x5ac72b8).address();
                    auto layerS = *reinterpret_cast<uintptr_t*>(layerSingletonAddrClose);
                    auto modeS  = *reinterpret_cast<uintptr_t*>(modeSingletonAddrClose);

                    int curLayer = layerS ? *reinterpret_cast<int*>(layerS + 0x36c) : -1;
                    int curMode  = modeS  ? *reinterpret_cast<int*>(modeS + 0x36c) : -1;

                    // CONSOLE FIX: a terminal opened in PROJECTED mode leaves the engine's render
                    // singletons at the projected-terminal layer/mode (0x24 / 0x14). The WORLD-
                    // terminal path captured THOSE as its "saved" baseline (the holotape path
                    // captures the clean Pipboy 0x3 / 0x20), so restoring them re-LOCKS the render
                    // layer to projected-world space (+0x374). That lock then forces every later
                    // flat menu — the Console especially — onto the world-projected layer, so it
                    // renders off the desktop view: you get the green cursor but no console body.
                    // For the world path, restore the clean menu defaults (what the working
                    // holotape path leaves behind) instead of the polluted captured values.
                    constexpr int kCleanLayer = 0x3;
                    constexpr int kCleanMode  = 0x20;
                    const int restoreLayerVal  = _isWorldTerminalRedirect ? kCleanLayer : _savedLayerValue;
                    const int restoreModeVal   = _isWorldTerminalRedirect ? kCleanMode  : _savedModeValue;
                    const int restoreLayerLock = _isWorldTerminalRedirect ? kCleanLayer : _savedLayerLock;
                    const int restoreModeLock  = _isWorldTerminalRedirect ? kCleanMode  : _savedModeLock;

                    // Restore values first, then locks
                    if (layerS && restoreLayerVal != -1)
                        *reinterpret_cast<int*>(layerS + 0x36c) = restoreLayerVal;
                    if (modeS && restoreModeVal != -1)
                        *reinterpret_cast<int*>(modeS + 0x36c) = restoreModeVal;
                    if (layerS) *reinterpret_cast<int*>(layerS + 0x374) = restoreLayerLock;
                    if (modeS)  *reinterpret_cast<int*>(modeS + 0x374) = restoreModeLock;

                    spdlog::info("[PIPBOY] Restored render singletons{}: layer {:#x}→{:#x} (lock {}), mode {:#x}→{:#x} (lock {})",
                                _isWorldTerminalRedirect ? " [world→clean]" : "",
                                curLayer, restoreLayerVal, restoreLayerLock,
                                curMode, restoreModeVal, restoreModeLock);
                }

                // Restore projected Pipboy mode if we flipped to wrist for this terminal.
                if (s_termAppliedWristOverride) {
                    // Restore wrist Pipboy view-angle scaling FIRST (while still in wrist mode),
                    // then flip back to the player's projected preference.
                    DeactivatePipboyScreen();
                    EndWristOverrideForHolotape();
                    s_termAppliedWristOverride = false;
                    spdlog::info("[PIPBOY] Terminal closed — restored projected Pipboy mode");
                }

                // 5b. Undo the per-frame darkening/blur/menu suppression. Must
                // run AFTER the terminal menu has actually closed (above) so a
                // dim the engine kicked off on close is cancelled too.
                RestoreTerminalDarkeningState();

                // 6. Reset state
                _terminalRedirectActive = false;
                _isWorldTerminalRedirect = false;
                _worldTerminalChecked = false;
                _terminalScreenNode = nullptr;
                _savedDiffuseSRV = 0;
                _savedRendererPtr = nullptr;
                _savedOrigWorldRoot = 0;
                _savedHmdScreenNode = nullptr;
                _savedHmdScreenParent = nullptr;

                spdlog::info("[PIPBOY] Terminal close complete — checking HUD vtable integrity");
                heisenberg::Hooks::CheckHUDRolloverVtableIntegrity();

                // Owner directive (Jul 31): grip on a redirected terminal closes EVERYTHING —
                // the terminal view AND the holo Pip-Boy. This fires only when the TERMINAL
                // closed while the Pip-Boy was still up (the grip-exit case); if the player
                // closed the Pip-Boy themselves (pipOpen==false) there is nothing to lower.
                // Without this, the engine/FRIK tears PipboyMenu down ~35ms later WITHOUT the
                // native lowering flow, and FRIK's holo model keeps rendering the destroyed
                // menu's render target — the reported "black screen until a trigger press".
                // Runs AFTER steps 1-6 + the vtable check on purpose: FRIK's keep-open flag is
                // already false, the I3D renderer is re-pointed at its original world root, and
                // the per-frame SRV loop is disarmed, so the close cannot race our writes into a
                // dying menu (the tape-deck stale-node CTD class). Deliberately NOT calling
                // DeactivatePipboyScreen() here: in wrist mode ActivatePipboyScreen never ran,
                // so a blind deactivate would write fallback angles over the player's INI.
                if (!termMenuOpen && pipOpen) {
                    EnableMenuInput(MenuPipboy());  // no-op if never disabled
                    if (auto* pbm = GetPipboyManagerVR()) {
                        pbm->ClosedownPipboy();
                    }
                    _terminalClosePipboyPending   = true;
                    _terminalClosePipboyDeadline  = 5.0f;
                    _terminalClosePipboyNextRetry = 0.25f;
                    spdlog::info("[PIPBOY] Grip-exit on redirected terminal — lowering the holo "
                                 "Pip-Boy (ClosedownPipboy + bounded watchdog)");
                } else if (_terminalForcedFrikScreenVisible) {
                    // The Pip-Boy is already down (the player closed it themselves while the
                    // terminal was up), so there is no menu to wait on — but we still owe the
                    // scene graph the hidden state we overrode, and FRIK still will not do it.
                    const bool holoRestored = RestoreFrikHoloHiddenStateAfterTerminal();
                    _terminalForcedFrikScreenVisible = false;
                    spdlog::info("[PIPBOY] Terminal ended with the Pip-Boy already closed — "
                                 "FRIK holo model hidden={}", holoRestored);
                }
            }
        }

        // World terminal detection — catch terminals activated directly (not via holotape)
        // Binary patches force non-projected path. We add WorldRoot swap so the
        // terminal SWF renders on the wrist pipboy, but do NOT lock render singletons
        // or force screenMode — those break things.
        // Skip in power armor / projected mode (Pipboy is projected, not wrist-mounted).
        if (!_pendingTerminalRedirect && !_terminalRedirectActive &&
            !_worldScreenRedirectActive && !g_config.enableTerminalOnWorldScreen &&
            g_config.forceTerminalOnWrist && !_terminalPatchesSuspended) {

            auto* ui = RE::UI::GetSingleton();
            bool termMenuOpen = ui && ui->GetMenuOpen(MenuTerminal());
            if (termMenuOpen && !_worldTerminalChecked) {
                _worldTerminalChecked = true;
                SuppressTerminalDarkening();

                // Projected-mode support: the redirect targets the FRIK wrist ScreenNode,
                // which isn't displayed while the Pipboy is in projected/HMD mode. If we're
                // projected, temporarily flip to wrist mode so the terminal actually shows
                // on the wrist Pipboy. Everything else stays projected; restored on close.
                if (IsProjectedPipboyNow()) {
                    BeginWristOverrideForHolotape();
                    // Force the wrist Pipboy screen visible AND set its view-angle scaling
                    // to 0 — otherwise the wrist Pipboy scales to nothing because the player
                    // is looking forward at the terminal, not down at their wrist. This is the
                    // same activation holotape playback uses. Runs now that the override has
                    // flipped isPipboyOnWrist() true.
                    ActivatePipboyScreen();
                    s_termAppliedWristOverride = true;
                    spdlog::info("[PIPBOY] Terminal opened in projected mode — wrist override + screen activation applied");
                }

                spdlog::debug("[PIPBOY] World terminal detected — performing WorldRoot swap");

                // Tell FRIK to keep Pipboy open
                {
                    static auto setKeepOpen = []() -> void(*)(bool) {
                        auto frikDll = GetModuleHandleA("FRIK.dll");
                        if (!frikDll) return nullptr;
                        return reinterpret_cast<void(*)(bool)>(
                            GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                    }();
                    if (setKeepOpen) setKeepOpen(true);
                }

                // WorldRoot swap — point PipboyMenu renderer at FRIK ScreenNode
                void* pipRend = I3D_GetByName("PipboyMenu");
                RE::NiNode* frikScreen = nullptr;
                auto* player3 = f4vr::getPlayer();
                if (player3) {
                    auto playerAddr = reinterpret_cast<uintptr_t>(player3);
                    auto screenPtr = *reinterpret_cast<uintptr_t*>(playerAddr + 0x07B8);
                    if (screenPtr > 0x10000) {
                        frikScreen = reinterpret_cast<RE::NiNode*>(screenPtr);
                    }
                }

                if (pipRend && frikScreen) {
                    auto rendBase = reinterpret_cast<uintptr_t>(pipRend);
                    auto oldWorldRoot = *reinterpret_cast<uintptr_t*>(rendBase + 0x088);

                    ForceAncestorScales(frikScreen);
                    frikScreen->flags.flags &= ~static_cast<uint64_t>(0x1);

                    if (oldWorldRoot > 0x10000) {
                        auto* oldScreen = reinterpret_cast<RE::NiAVObject*>(oldWorldRoot);
                        if (IsAncestorOf(oldScreen, frikScreen)) {
                            ForceAncestorScales(frikScreen);
                            _savedHmdScreenNode = nullptr;
                            _savedHmdScreenParent = nullptr;
                        } else {
                            oldScreen->flags.flags |= 0x1;
                            oldScreen->local.scale = 0.0f;
                            _savedHmdScreenParent = oldScreen->parent;
                            if (oldScreen->parent) {
                                oldScreen->parent->DetachChild(oldScreen);
                            }
                            _savedHmdScreenNode = oldScreen;
                        }
                    }

                    _savedOrigWorldRoot = oldWorldRoot;
                    _savedRendererPtr = pipRend;

                    *reinterpret_cast<uintptr_t*>(rendBase + 0x088) =
                        reinterpret_cast<uintptr_t>(frikScreen);

                    spdlog::debug("[PIPBOY] World terminal: worldRoot swapped to '{}' (scale={:.3f})",
                                 frikScreen->name.c_str() ? frikScreen->name.c_str() : "?",
                                 frikScreen->world.scale);
                } else {
                    spdlog::warn("[PIPBOY] World terminal: pipRend={} frikScreen={}",
                                 pipRend != nullptr, frikScreen != nullptr);
                }

                // Save render singleton values BEFORE per-frame forcing starts
                {
                    static auto layerSAddrW = REL::Offset(0x5ac8eb0).address();
                    static auto modeSAddrW  = REL::Offset(0x5ac72b8).address();
                    auto layerS = *reinterpret_cast<uintptr_t*>(layerSAddrW);
                    auto modeS  = *reinterpret_cast<uintptr_t*>(modeSAddrW);
                    _savedLayerValue = layerS ? *reinterpret_cast<int*>(layerS + 0x36c) : -1;
                    _savedModeValue  = modeS  ? *reinterpret_cast<int*>(modeS + 0x36c) : -1;
                    _savedLayerLock  = layerS ? *reinterpret_cast<int*>(layerS + 0x374) : 0;
                    _savedModeLock   = modeS  ? *reinterpret_cast<int*>(modeS + 0x374) : 0;
                    spdlog::info("[PIPBOY] Saved render state: layer={:#x} lock={}, mode={:#x} lock={}",
                                _savedLayerValue, _savedLayerLock, _savedModeValue, _savedModeLock);
                }

                _terminalRedirectActive = true;
                _isWorldTerminalRedirect = true;

                spdlog::info("[PIPBOY] Terminal redirect ACTIVATED (world terminal path) — checking HUD vtable");
                heisenberg::Hooks::CheckHUDRolloverVtableIntegrity();
            } else if (!termMenuOpen) {
                _worldTerminalChecked = false;
            }
        }

        // ════════════════════════════════════════════════════════════════════════
        // In-world terminal screen redirect (experimental — enableTerminalOnWorldScreen)
        // Renders terminal UI texture onto the in-world terminal's screen mesh via SRV swap.
        // Unlike the Pipboy wrist redirect, this targets the physical terminal object's NIF.
        // ════════════════════════════════════════════════════════════════════════

        // Detection: when TerminalMenu opens and world-screen mode is enabled
        if (g_config.enableTerminalOnWorldScreen &&
            !_worldScreenRedirectActive && !_worldScreenChecked &&
            !_pendingTerminalRedirect && !_terminalRedirectActive)
        {
            auto* ui = RE::UI::GetSingleton();
            bool termMenuOpen = ui && ui->GetMenuOpen(MenuTerminal());
            if (termMenuOpen) {
                _worldScreenChecked = true;
                SuppressTerminalDarkening();

                // Find the nearest terminal via ActivatorHandler
                auto* player2 = f4vr::getPlayer();
                RE::NiPoint3 playerPos{};
                if (player2) {
                    auto* player3d = reinterpret_cast<RE::Actor*>(player2)->Get3D();
                    if (player3d) playerPos = player3d->world.translate;
                }
                auto* nearestTerminal = ActivatorHandler::GetSingleton().GetNearestTerminal(playerPos, 300.0f);

                if (nearestTerminal) {
                    auto* termRefr = nearestTerminal->GetRefr();
                    if (termRefr) {
                        auto* termNode = termRefr->Get3D();
                        if (termNode) {
                            // Walk the NIF tree to find the screen mesh node
                            // Common names: "Screen:0", "ScreenGlass:0", "Screen"
                            RE::NiAVObject* screenMesh = nullptr;
                            static const char* screenNodeNames[] = {
                                "Screen:0", "ScreenGlass:0", "Screen",
                                "screen:0", "screenGlass:0", "screen"
                            };
                            for (const char* name : screenNodeNames) {
                                screenMesh = ActivatorHandler::GetSingleton().FindNodeRecursive(termNode, name);
                                if (screenMesh) break;
                            }

                            if (screenMesh) {
                                // Save original SRV for restoration
                                _worldScreenOrigSRV = GetMeshDiffuseSRV(screenMesh);
                                _worldScreenTerminalNode = screenMesh;
                                _worldScreenRedirectActive = true;

                                spdlog::debug("[WORLD_SCREEN] Terminal screen redirect active — "
                                             "terminal {:08X} screen node '{}' origSRV={:X}",
                                             termRefr->formID,
                                             screenMesh->name.c_str() ? screenMesh->name.c_str() : "?",
                                             _worldScreenOrigSRV);
                            } else {
                                // Log all child nodes for discovery
                                spdlog::warn("[WORLD_SCREEN] No screen mesh found on terminal {:08X} — "
                                             "dumping node tree for discovery:", termRefr->formID);
                                ActivatorHandler::GetSingleton().LogActivatorNodes(termRefr);
                            }
                        }
                    }
                } else {
                    spdlog::warn("[WORLD_SCREEN] No terminal found near player — "
                                 "ActivatorHandler may not have scanned terminals in this cell");
                }
            }
        }

        // Per-frame maintenance: SRV swap onto world terminal screen mesh
        if (_worldScreenRedirectActive) {
            SuppressTerminalDarkening();
            SafeHideRollover();

            // Per-frame: read RT3F (rendered terminal UI) and write onto world screen mesh
            if (_worldScreenTerminalNode) {
                static auto rendererDataPtrAddr3 = REL::Offset(0x60f3ce8).address();
                auto rendererData = *reinterpret_cast<uintptr_t*>(rendererDataPtrAddr3);
                if (rendererData) {
                    // RT index 0x3F contains the rendered terminal UI
                    auto rt3fSRV = *reinterpret_cast<uintptr_t*>(
                        rendererData + 0x0A58 + 0x3F * 0x30 + 0x18);
                    if (rt3fSRV) {
                        SetMeshDiffuseSRV(_worldScreenTerminalNode, rt3fSRV);
                    }
                }
            }

            // Detect terminal close
            auto* ui = RE::UI::GetSingleton();
            bool termMenuOpen = ui && ui->GetMenuOpen(MenuTerminal());
            if (!termMenuOpen) {
                spdlog::debug("[WORLD_SCREEN] TerminalMenu closed — restoring world screen SRV");

                // Restore original SRV
                if (_worldScreenTerminalNode && _worldScreenOrigSRV) {
                    SetMeshDiffuseSRV(_worldScreenTerminalNode, _worldScreenOrigSRV);
                    spdlog::debug("[WORLD_SCREEN] Restored original SRV {:X}", _worldScreenOrigSRV);
                }

                _worldScreenRedirectActive = false;
                _worldScreenChecked = false;
                _worldScreenTerminalNode = nullptr;
                _worldScreenOrigSRV = 0;

                spdlog::debug("[WORLD_SCREEN] Close complete — checking HUD vtable integrity");
                heisenberg::Hooks::CheckHUDRolloverVtableIntegrity();
            }
        }

        // Reset world screen check when terminal closes (even if redirect wasn't active)
        if (!_worldScreenRedirectActive && _worldScreenChecked) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->GetMenuOpen(MenuTerminal())) {
                _worldScreenChecked = false;
            }
        }

        if (_holotapeGrabCooldown > 0.0f) {
            _holotapeGrabCooldown -= deltaTime;
        }

        if (_holotapeRemovalCooldown > 0.0f) {
            _holotapeRemovalCooldown -= deltaTime;
        }

        if (_deckRemovalHandGuard > 0.0f) {
            _deckRemovalHandGuard -= deltaTime;
        }

        if (_deckPushCloseLockout > 0.0f) {
            _deckPushCloseLockout -= deltaTime;
        }

        if (_slamCooldown > 0.0f) {
            _slamCooldown -= deltaTime;
        }

        // Process deferred Disable() for inserted holotape world refs.
        // Waiting a few frames lets Inventory3DManager finish any pending 3D load tasks.
        if (_deferredDisableFrames > 0) {
            if (--_deferredDisableFrames == 0) {
                RE::NiPointer<RE::TESObjectREFR> deferredRef = _deferredDisableHandle.get();
                if (deferredRef) {
                    heisenberg::SafeDisable(deferredRef.get());
                    spdlog::debug("[PIPBOY] Deferred Disable() on {:08X}", deferredRef->formID);
                }
                _deferredDisableHandle.reset();
            }
        }

        // Tape deck physical interactions — disabled in power armor and while two-handing
        // a weapon (off-hand is busy supporting the grip, shouldn't also reach the deck).
        // FRIK's own off-hand-gripping flag gets explicitly killed (killFrikOffhandGrip())
        // once ROCK's TwoHandedGrip state machine takes over a two-handed hold, so it reads
        // false while the player is still very much two-handing via ROCK. Check ROCK's own
        // engagement state directly too — either signal being true means two-handing.
        const bool twoHandingWeapon = FRIKInterface::GetSingleton().IsOffHandGrippingWeapon()
            || rock::HostIsWeaponSupportGripped(true)
            || rock::HostIsWeaponSupportGripped(false);
        if (!inPowerArmor && !twoHandingWeapon) {
            // Resolve the visible fingertip once and share the identical motion segment between
            // eject, deck-push, and tape-removal tests. A missing tracking frame invalidates the
            // sweep so reacquisition cannot draw a phantom contact across the Pip-Boy.
            const RE::NiPoint3 sampledFinger = GetFingerPosition();
            if (IsUsableWorldPoint(sampledFinger)) {
                _previousInteractionFingerPos = _interactionFingerPos;
                _previousInteractionFingerValid = _interactionFingerValid;
                _interactionFingerPos = sampledFinger;
                _interactionFingerValid = true;
            } else {
                _interactionFingerValid = false;
                _previousInteractionFingerValid = false;
            }

            OperateEjectButton(deltaTime);
            CheckHandPush();             // Actual hand/weapon collider hulls against the live tray mesh
            CheckHolotapeRemoval();

            // Prevent immediate re-insertion of freshly grabbed holotapes.
            // When DropToHand grabs a holotape near the Pipboy (e.g., after dropping from
            // inventory while the deck is open), CheckHolotapeInsertion would immediately
            // re-insert it. Detect new holotape grabs and set a cooldown.
            {
                auto& grabMgr = GrabManager::GetSingleton();
                bool holotapeHeld = false;
                for (int h = 0; h < 2; h++) {
                    bool isLeft = (h == 0);
                    if (!grabMgr.IsGrabbing(isLeft)) continue;
                    const auto& gs = grabMgr.GetGrabState(isLeft);
                    auto* refr = gs.GetRefr();
                    if (!refr) continue;
                    auto* baseObj = refr->GetObjectReference();
                    if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                        holotapeHeld = true;
                        if (refr->formID != _lastHeldHolotapeRefID) {
                            _lastHeldHolotapeRefID = refr->formID;
                            // Never shorten the longer grace already established by a physical
                            // removal. This is insertion-only; it no longer delays taking a tape.
                            _holotapeGrabCooldown = (std::max)(_holotapeGrabCooldown, 1.0f);
                            spdlog::debug("[PIPBOY] New holotape grab {:08X} — 1s insertion cooldown", refr->formID);
                        }
                    }
                }
                if (!holotapeHeld) _lastHeldHolotapeRefID = 0;
            }

            CheckHolotapeInsertion();
            UpdateHolotapeFingerPose();
        } else {
            // The sampler above didn't run this frame, so the stored fingertip sample goes
            // stale. Without invalidating it, releasing a two-handed grip (or exiting power
            // armor) pairs a seconds-old position with the fresh one on re-entry — a phantom
            // multi-second sweep segment that can pass through the deck/eject contact bubbles
            // and fire spurious eject/close (review-confirmed). Matches the sampler's own
            // missing-frame rule: no continuous pair, no sweep.
            _interactionFingerValid = false;
            _previousInteractionFingerValid = false;
        }

        // NOTE: UpdateTapeDeckAnimation() is now called from HookEndUpdate (0xd84f2c)
        // which runs AFTER all animation/skeleton updates, so our transforms stick.
    }

    // ════════════════════════════════════════════════════════════════════════
    // Helpers
    // ════════════════════════════════════════════════════════════════════════

    // SEH-isolated findAVObject. The cache-invalidation in OnFrameUpdate fixes the STALE-pointer case
    // (re-resolve from the live skeleton root when it changes), but it cannot cover the engine
    // rebuilding the Pip-Boy SUBTREE in place: findAVObject then walks a half-built child and calls
    // its virtual node->IsNode() through a null/garbage vtable → "execute at 0x0" CTD (Buffout can't
    // even log it). You cannot validate a freed/half-built node without dereferencing it, so traversing
    // the game's mutating scene graph is the legitimate case for __try. NO C++ object needing unwinding
    // lives here (name is a const-ref, so the std::string temporary is built in the CALLER) → __try is
    // legal. On fault it returns null and the caller treats the node as not-ready (defer + retry).
    static RE::NiAVObject* SafeFindAVObject(RE::NiAVObject* root, const std::string& name)
    {
        if (!root) return nullptr;
        __try {
            return f4vr::findAVObject(root, name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    template <std::size_t Capacity>
    static void CapturePipboySubtree(
        RE::NiAVObject* object,
        int parentIndex,
        int depth,
        pipboy_node_snapshot::FixedSubtreeMembership<Capacity>& snapshot)
    {
        if (!object) {
            return;
        }

        const int objectIndex =
            snapshot.Append(object, parentIndex);
        if (objectIndex < 0) {
            return;
        }

        // The former arm-root liveness queries stopped at 24 levels, while a
        // nested deck/lid query could cover another 24. Capture the combined
        // envelope once so replacing the nested walks cannot reduce coverage.
        constexpr int kMaximumCombinedDepth = 48;
        if (depth >= kMaximumCombinedDepth) {
            return;
        }

        if (auto* node = object->IsNode()) {
            const std::uint32_t childCount =
                static_cast<std::uint32_t>(node->children.size());
            for (std::uint32_t i = 0; i < childCount; ++i) {
                auto* child = node->children[i].get();
                if (child) {
                    CapturePipboySubtree(
                        child,
                        objectIndex,
                        depth + 1,
                        snapshot);
                }
            }
        }
    }

    template <std::size_t Capacity>
    static bool CapturePipboySubtreeSafely(
        RE::NiAVObject* arm,
        pipboy_node_snapshot::FixedSubtreeMembership<Capacity>& snapshot)
    {
        // No local C++ owner/RAII object lives across this narrow SEH region.
        // A Pip-Boy subtree can be half-rebuilt even when the overall skeleton
        // root pointer is unchanged; on a raw child-array fault, discard the
        // partial snapshot and defer all node work until the next phase.
        __try {
            snapshot.Reset();
            CapturePipboySubtree(arm, -1, 0, snapshot);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            snapshot.Reset();
            snapshot.MarkIncomplete();
            return false;
        }
    }

    struct SweptMeshContact
    {
        bool meshAvailable = false;
        float currentDistance = (std::numeric_limits<float>::max)();
        float sweptDistance = (std::numeric_limits<float>::max)();
        RE::NiPoint3 closestPoint{};
    };

    static bool IsUsableWorldPoint(const RE::NiPoint3& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z)
            && (point.x != 0.0f || point.y != 0.0f || point.z != 0.0f);
    }

    static float PointDistance(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        const RE::NiPoint3 d = a - b;
        return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    }

    static float PointToSegmentDistance(
        const RE::NiPoint3& point,
        const RE::NiPoint3& start,
        const RE::NiPoint3& end)
    {
        const RE::NiPoint3 segment = end - start;
        const float lengthSq = segment.x * segment.x + segment.y * segment.y + segment.z * segment.z;
        if (lengthSq <= 1e-6f) return PointDistance(point, end);
        const RE::NiPoint3 fromStart = point - start;
        const float projection = fromStart.x * segment.x + fromStart.y * segment.y + fromStart.z * segment.z;
        const float t = (std::clamp)(projection / lengthSq, 0.0f, 1.0f);
        return PointDistance(point, start + segment * t);
    }

    static RE::NiPoint3 PipboyCrossProduct(
        const RE::NiPoint3& a,
        const RE::NiPoint3& b)
    {
        return RE::NiPoint3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x);
    }

    static float PipboyDotProduct(
        const RE::NiPoint3& a,
        const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static RE::NiPoint3 PipboyRotateDirection(
        const RE::NiMatrix3& rotation,
        const RE::NiPoint3& localDirection)
    {
        RE::NiPoint3 worldDirection(
            rotation.entry[0][0] * localDirection.x +
                rotation.entry[1][0] * localDirection.y +
                rotation.entry[2][0] * localDirection.z,
            rotation.entry[0][1] * localDirection.x +
                rotation.entry[1][1] * localDirection.y +
                rotation.entry[2][1] * localDirection.z,
            rotation.entry[0][2] * localDirection.x +
                rotation.entry[1][2] * localDirection.y +
                rotation.entry[2][2] * localDirection.z);
        const float length = PointDistance({}, worldDirection);
        if (!std::isfinite(length) || length <= 1.0e-4f) {
            return RE::NiPoint3(0.0f, 0.0f, 1.0f);
        }
        return worldDirection * (1.0f / length);
    }

    // Exact double-sided segment/triangle test. Point sampling alone can step
    // over a sub-unit button or tray during a fast controller movement.
    static bool SegmentIntersectsTriangle(
        const RE::NiPoint3& start,
        const RE::NiPoint3& end,
        const TriangleData& triangle,
        float& intersectionT,
        RE::NiPoint3& intersection)
    {
        constexpr float epsilon = 1.0e-6f;
        const RE::NiPoint3 direction = end - start;
        const RE::NiPoint3 edge1 = triangle.v1 - triangle.v0;
        const RE::NiPoint3 edge2 = triangle.v2 - triangle.v0;
        const RE::NiPoint3 p = PipboyCrossProduct(direction, edge2);
        const float determinant = PipboyDotProduct(edge1, p);
        if (std::fabs(determinant) <= epsilon) {
            return false;
        }

        const float inverseDeterminant = 1.0f / determinant;
        const RE::NiPoint3 fromVertex = start - triangle.v0;
        const float u = PipboyDotProduct(fromVertex, p) * inverseDeterminant;
        if (u < -epsilon || u > 1.0f + epsilon) {
            return false;
        }

        const RE::NiPoint3 q = PipboyCrossProduct(fromVertex, edge1);
        const float v = PipboyDotProduct(direction, q) * inverseDeterminant;
        if (v < -epsilon || u + v > 1.0f + epsilon) {
            return false;
        }

        const float t = PipboyDotProduct(edge2, q) * inverseDeterminant;
        if (t < -epsilon || t > 1.0f + epsilon) {
            return false;
        }

        intersectionT = (std::clamp)(t, 0.0f, 1.0f);
        intersection = start + direction * intersectionT;
        return true;
    }

    static bool FindFirstSegmentMeshIntersection(
        const std::vector<TriangleData>& triangles,
        const RE::NiPoint3& start,
        const RE::NiPoint3& end,
        RE::NiPoint3& intersection)
    {
        float firstT = (std::numeric_limits<float>::max)();
        bool found = false;
        for (const auto& triangle : triangles) {
            float t = 0.0f;
            RE::NiPoint3 candidate{};
            if (SegmentIntersectsTriangle(start, end, triangle, t, candidate) &&
                t < firstT) {
                firstT = t;
                intersection = candidate;
                found = true;
            }
        }
        return found;
    }

    // Query the visible triangle surface at the node's current world transform. Sampling the
    // previous->current fingertip segment at <= half-radius spacing prevents a quick VR motion
    // from tunnelling through the thin deck/button between two 90 Hz updates.
    static SweptMeshContact QuerySweptMeshContact(
        RE::NiAVObject* mesh,
        const RE::NiPoint3& current,
        const RE::NiPoint3& previous,
        bool previousValid,
        float contactRadius,
        float broadphaseInterestDistance = -1.0f)
    {
        SweptMeshContact result;
        if (!mesh || !IsUsableWorldPoint(current)) return result;

        // Eject-button callers provide the largest distance at which this
        // query can affect contact, latch, release, or animation. A valid
        // world bound encloses the visible mesh; expanding it by the
        // bound-center-to-node-pivot distance encloses the node-point fallback
        // as well. Reject only when the COMPLETE valid fingertip sweep is
        // outside that enlarged sphere. A fast crossing therefore still takes
        // the exact path. Tracking discontinuities and invalid bounds fail open
        // to extraction below.
        if (std::isfinite(broadphaseInterestDistance) &&
            broadphaseInterestDistance >= 0.0f) {
            bool canUseBroadphase = true;
            float pathDistanceToBoundCenter =
                PointDistance(mesh->worldBound.center, current);
            if (previousValid) {
                if (!IsUsableWorldPoint(previous)) {
                    canUseBroadphase = false;
                } else {
                    const float travel = PointDistance(previous, current);
                    // Keep the exact query's tracking-discontinuity behavior:
                    // it intentionally does not sweep a reacquisition jump.
                    if (!std::isfinite(travel) || travel > 120.0f) {
                        canUseBroadphase = false;
                    } else {
                        pathDistanceToBoundCenter =
                            PointToSegmentDistance(
                                mesh->worldBound.center,
                                previous,
                                current);
                    }
                }
            }

            const float meshToPivotDistance =
                PointDistance(
                    mesh->worldBound.center,
                    mesh->world.translate);
            if (canUseBroadphase &&
                pipboy_mesh_contact::CanRejectEjectMeshQuery(
                    pathDistanceToBoundCenter,
                    mesh->worldBound.fRadius,
                    meshToPivotDistance,
                    broadphaseInterestDistance)) {
                return result;
            }
        }

        std::vector<TriangleData> triangles;
        triangles.reserve(256);
        GetTriangles(mesh, triangles, 2048);
        if (triangles.empty()) return result;
        result.meshAvailable = true;

        RE::NiPoint3 meshPoint;
        if (!GetClosestMeshPointToPoint(triangles, current, meshPoint, result.currentDistance)) {
            result.meshAvailable = false;
            return result;
        }
        result.sweptDistance = result.currentDistance;
        result.closestPoint = meshPoint;

        if (!previousValid || !IsUsableWorldPoint(previous)) return result;
        const float travel = PointDistance(previous, current);
        // Treat a large tracking reacquisition/teleport as a discontinuity, not a physical sweep.
        // 120 game units is still far beyond a normal one-frame arm movement, but keeps
        // low-frame-rate fast swipes usable instead of silently dropping their contact.
        if (!std::isfinite(travel) || travel > 120.0f) return result;

        // A true crossing is continuous and cannot be missed regardless of sample
        // spacing. This is especially important for the eject button, whose fully
        // depressed trigger depth is much smaller than the general hand-contact radius.
        RE::NiPoint3 intersection{};
        if (FindFirstSegmentMeshIntersection(
                triangles, previous, current, intersection)) {
            result.sweptDistance = 0.0f;
            result.closestPoint = intersection;
            return result;
        }

        const int segments = pipboy_mesh_contact::SweepSegmentCount(travel, contactRadius);
        for (int i = 0; i < segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const RE::NiPoint3 sample = previous * (1.0f - t) + current * t;
            float distance = (std::numeric_limits<float>::max)();
            if (GetClosestMeshPointToPoint(triangles, sample, meshPoint, distance)
                && distance < result.sweptDistance) {
                result.sweptDistance = distance;
                result.closestPoint = meshPoint;
            }
        }
        return result;
    }

    RE::NiAVObject* PipboyInteraction::GetPipboyArmNode()
    {
        if (!_nodeSnapshotPhaseAttempted) {
            BeginNodeValidationPhase();
        }
        if (!_nodeSnapshotUsable ||
            !_frameCacheValid ||
            !_cachedArmNode ||
            _cachedArmNode != _nodeSnapshotArm) {
            return nullptr;
        }

        // CRITICAL: Must use the 3rd-person skeleton (unkF0->rootNode), NOT firstPersonSkeleton.
        // The renderer draws from the 3rd-person tree. Writes to firstPersonSkeleton nodes are invisible.
        // FRIK's Skeleton uses getRootNode() (3rd-person BSFlattenedBoneTree) and searches from getCommonNode() ("COM").
        // VirtualHolsters also uses unkF0->rootNode exclusively.
        return _nodeSnapshotUsable
            ? _nodeSnapshotArm
            : nullptr;
    }

    void PipboyInteraction::BeginNodeValidationPhase()
    {
        _nodeSnapshotPhaseAttempted = true;
        _nodeSnapshot.Reset();
        _nodeSnapshotArm = nullptr;
        _nodeSnapshotUsable = false;
        _cachedArmNode = nullptr;
        _frameCacheValid = false;

        auto* commonNode = f4vr::getCommonNode();
        if (!commonNode) {
            ValidatePersistentNodeCaches();
            return;
        }

        // Resolve from the CURRENT skeleton every phase. In particular,
        // HookEndUpdate must not reuse the arm pointer captured earlier by
        // OnFrameUpdate because FRIK can rebuild that subtree between hooks
        // without changing the top-level common-root address.
        RE::NiAVObject* arm =
            SafeFindAVObject(commonNode, "LArm_ForeArm3");
        if (!arm ||
            !CapturePipboySubtreeSafely(arm, _nodeSnapshot)) {
            ValidatePersistentNodeCaches();
            return;
        }

        _nodeSnapshotArm = arm;
        _nodeSnapshotUsable = true;
        _cachedArmNode = arm;
        _frameCacheValid = true;
        ValidatePersistentNodeCaches();
    }

    bool PipboyInteraction::CachedNodeIsLiveWithin(
        const RE::NiAVObject* root,
        const RE::NiAVObject* target) const
    {
        return _nodeSnapshotUsable &&
            root &&
            target &&
            _nodeSnapshot.ContainsWithin(root, target);
    }

    void PipboyInteraction::ValidatePersistentNodeCaches()
    {
        RE::NiAVObject* arm =
            _nodeSnapshotUsable ? _nodeSnapshotArm : nullptr;
        if (!arm) {
            _cachedEjectButton = nullptr;
            _cachedEjectButtonMesh = nullptr;
            _cachedTapeDeckNode = nullptr;
            _cachedTapeDeckLid = nullptr;
            _cachedTapeRef = nullptr;
            _cachedTapeDeckMesh1 = nullptr;
            _cachedTapeDeckLidMesh1 = nullptr;
            _nodesCached = false;
            return;
        }

        const bool ejectButtonLive =
            !_cachedEjectButton ||
            CachedNodeIsLiveWithin(
                arm,
                _cachedEjectButton);
        const bool ejectMeshLive =
            !_cachedEjectButtonMesh ||
            CachedNodeIsLiveWithin(
                arm,
                _cachedEjectButtonMesh);
        if (!ejectButtonLive || !ejectMeshLive) {
            _cachedEjectButton = nullptr;
            _cachedEjectButtonMesh = nullptr;
            _nodesCached = false;
        }

        if (_cachedTapeDeckNode &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedTapeDeckNode)) {
            _cachedTapeDeckNode = nullptr;
            _cachedTapeDeckMesh1 = nullptr;
        }

        if (_cachedTapeDeckLid &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedTapeDeckLid)) {
            _cachedTapeDeckLid = nullptr;
            _cachedTapeRef = nullptr;
            _cachedTapeDeckMesh1 = nullptr;
            _cachedTapeDeckLidMesh1 = nullptr;
        }

        if (_cachedTapeRef &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedTapeRef)) {
            _cachedTapeRef = nullptr;
        }

        if (_cachedTapeDeckMesh1 &&
            (!_cachedTapeDeckNode ||
             !CachedNodeIsLiveWithin(
                 _cachedTapeDeckNode,
                 _cachedTapeDeckMesh1))) {
            _cachedTapeDeckMesh1 = nullptr;
        }

        if (_cachedTapeDeckLidMesh1 &&
            (!_cachedTapeDeckLid ||
             !CachedNodeIsLiveWithin(
                 _cachedTapeDeckLid,
                 _cachedTapeDeckLidMesh1))) {
            _cachedTapeDeckLidMesh1 = nullptr;
        }

        static bool s_snapshotCapacityWarningLogged = false;
        if (!_nodeSnapshot.Complete() &&
            !s_snapshotCapacityWarningLogged) {
            s_snapshotCapacityWarningLogged = true;
            spdlog::warn(
                "[PIPBOY] Arm subtree exceeded the fixed {}-node "
                "liveness snapshot; uncaptured caches will be safely "
                "re-resolved",
                PIPBOY_NODE_SNAPSHOT_CAPACITY);
        }
    }

    RE::NiPoint3 PipboyInteraction::GetFingerPosition()
    {
        if (_fingerPosCached) return _cachedFingerPos;

        // Try FRIK API first (accurate finger tracking)
        // Pipboy is always on the LEFT wrist (doesn't swap in LH mode),
        // so the interaction finger is always the RIGHT hand index finger.
        auto& frik = FRIKInterface::GetSingleton();
        if (frik.IsAvailable()) {
            RE::NiPoint3 fingerPos;
            if (frik.GetIndexFingerTipPosition(false /*right hand*/, fingerPos)) {
                // Guard against NaN — skeleton can be temporarily invalid during 
                // consume/equip animations. NaN bypasses all distance comparisons
                // (NaN > x is always false), causing false eject button triggers.
                if (std::isnan(fingerPos.x) || std::isnan(fingerPos.y) || std::isnan(fingerPos.z)) {
                    return RE::NiPoint3();
                }
                _cachedFingerPos = fingerPos;
                _fingerPosCached = true;
                return _cachedFingerPos;
            }
        }

        // Fallback: skeleton bone lookup on the 3rd-person skeleton
        auto* commonNode = f4vr::getCommonNode();
        if (!commonNode) {
            return RE::NiPoint3();
        }

        // Pipboy always on left wrist — interaction finger is always right hand
        const char* fingerNodeName = "RArm_Finger23";

        RE::NiAVObject* fingerNode = f4vr::findAVObject(commonNode, fingerNodeName);
        if (fingerNode) {
            RE::NiPoint3 pos = fingerNode->world.translate;
            if (std::isnan(pos.x) || std::isnan(pos.y) || std::isnan(pos.z)) {
                return RE::NiPoint3();
            }
            _cachedFingerPos = pos;
            _fingerPosCached = true;
            return _cachedFingerPos;
        }

        return RE::NiPoint3();
    }

    void PipboyInteraction::InvalidateFrameCache()
    {
        _frameCacheValid = false;
        _cachedArmNode = nullptr;
        _cachedTapeDeckNode = nullptr;
        _nodeSnapshot.Reset();
        _nodeSnapshotArm = nullptr;
        _nodeSnapshotPhaseAttempted = false;
        _nodeSnapshotUsable = false;
        _fingerPosCached = false;
        _interactionFingerValid = false;
        _previousInteractionFingerValid = false;
        _ejectContactLatched = false;
    }

    RE::NiAVObject* PipboyInteraction::GetCachedTapeDeckNode()
    {
        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return nullptr;

        // Re-validate against the current phase's LIVE arm snapshot, same as the sibling
        // _cachedTapeDeckLid/_cachedEjectButton checks — this cache was previously
        // invalidated ONLY by InvalidateFrameCache() on a 3rd-person skeleton ROOT pointer
        // change, but a Pip-Boy pickup can rebuild just the TapeDeck01 sub-node via a partial
        // 3D-equip reload without the overall skeleton root pointer changing, leaving this
        // cache dangling at a freed node. Buffout-confirmed 2026-07-21 (Vault111Cryo):
        // EXECUTE at 0x0 inside findAVObject(tapeDeckNode, "TapeDeck01_mesh:1") called from
        // UpdateTapeDeckAnimation, at Pip-Boy pickup.
        if (_cachedTapeDeckNode &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedTapeDeckNode)) {
            _cachedTapeDeckNode = nullptr;
        }
        if (_cachedTapeDeckNode) return _cachedTapeDeckNode;

        _cachedTapeDeckNode = SafeFindAVObject(arm, "TapeDeck01");
        return _cachedTapeDeckNode;
    }

    void PipboyInteraction::PlaySound(std::uint32_t formID)
    {
        auto* form = RE::TESForm::GetFormByID(formID);
        if (!form) {
            spdlog::warn("[PIPBOY] Sound form {:08X} not found", formID);
            return;
        }
        auto* sound = reinterpret_cast<RE::BGSSoundDescriptorForm*>(form);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            heisenberg::PlaySoundAtActor(sound, player);
            spdlog::debug("[PIPBOY] Playing sound {:08X}", formID);
        }
    }

    void PipboyInteraction::PlayWavSound(const char* filename, std::uint32_t fallbackFormID)
    {
        // Route through the game's own BSAudioManager to avoid WASAPI conflicts.
        // Files must be deployed to Sound/FX/Heisenberg/ in the mod's Data folder.
        //
        // 0.5625 = the previous 0.75 lowered by 25% (owner request). Every tape-deck action
        // sound — eject press, tray open/close, tape in/out, slam — routes through here, so
        // this one constant is the deck's master volume.
        auto handle = BSPlayGameSound(GetGameSoundPath(filename).c_str(), 0.5625f);
        if (handle.soundID == static_cast<std::uint32_t>(-1) && fallbackFormID != 0) {
            PlaySound(fallbackFormID);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Eject button interaction (finger proximity detection)
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::OperateEjectButton(float deltaTime)
    {
        _logCooldown--;

        if (_ejectCooldown > 0.0f) {
            _ejectCooldown -= deltaTime;
        }

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        // Re-validate against the phase snapshot before trusting the persistent cache.
        // This catches an in-place Pip-Boy rebuild the coarse skeleton-root pointer
        // comparison misses, without recursively walking the same arm again.
        if (_nodesCached &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedEjectButton)) {
            _cachedEjectButton     = nullptr;
            _cachedEjectButtonMesh = nullptr;
            _nodesCached           = false;
        }

        // Use cached nodes to avoid per-frame tree searches
        if (!_nodesCached) {
            _cachedEjectButton     = f4vr::findAVObject(arm, "EjectButton");
            _cachedEjectButtonMesh = f4vr::findAVObject(arm, "EjectButton_mesh:0");
            if (_cachedEjectButton) _nodesCached = true;
        }
        RE::NiAVObject* ejectButton     = _cachedEjectButton;
        RE::NiAVObject* ejectButtonMesh = _cachedEjectButtonMesh;

        RE::NiAVObject* animateNode = ejectButtonMesh ? ejectButtonMesh : ejectButton;

        // Capture original Z from the actual node (not hardcoded)
        if (!_buttonOriginalZSet && animateNode) {
            _buttonOriginalZ = animateNode->local.translate.z;
            _buttonOriginalZSet = true;
            spdlog::debug("[PIPBOY] Captured button original Z={:.6f} from {}",
                _buttonOriginalZ, animateNode->name.c_str());
        }

        // One-time node dump
        static bool loggedSearchResult = false;
        if (!loggedSearchResult) {
            loggedSearchResult = true;
            if (ejectButton) {
                spdlog::debug("[PIPBOY] Found EjectButton (local.z={:.4f})", ejectButton->local.translate.z);
                if (ejectButtonMesh) {
                    spdlog::debug("[PIPBOY] Found EjectButton_mesh:0 (local.z={:.4f})", ejectButtonMesh->local.translate.z);
                }
            } else {
                spdlog::warn("[PIPBOY] EjectButton NOT found – dumping arm structure:");
                DumpNodesContaining(arm, "");
            }
        }

        if (!ejectButton && !_dumpedNodes) {
            _dumpedNodes = true;
            spdlog::warn("[PIPBOY] Dumping arm child nodes:");
            DumpNodesContaining(arm, "");
        }

        // Contact the rendered button itself. The old path measured a five-unit sphere
        // around TapeDeck01's animation pivot, so it could fire beside the button and miss
        // a quick movement through it. EjectButton_mesh:0 is the visible transformed mesh.
        RE::NiAVObject* ejectContactTarget = ejectButtonMesh ? ejectButtonMesh : ejectButton;
        if (!ejectContactTarget) return;

        // Scope view remains an unsuitable interaction context.  A merely drawn
        // weapon is not: the other hand can still physically press the wrist button,
        // and returning here made that visibly depressed press do nothing.
        {
            auto& menuChecker = MenuChecker::GetSingleton();
            if (menuChecker.IsScopeOpen()) {
                _ejectContactLatched = false;
                if (animateNode && _buttonOriginalZSet) {
                    animateNode->local.translate.z = _buttonOriginalZ;
                }
                return;
            }
        }

        RE::NiPoint3 fingerPos;
        bool haveFingerPos = _interactionFingerValid;
        bool usingWandFallbackPos = false;  // wand origin sits several gu behind the fingertip skin — the press-to-fire distance must not apply to it
        if (haveFingerPos) {
            fingerPos = _interactionFingerPos;
        } else if (_introPlaybackActive) {
            // FRIK finger tracking (both the FRIK API and the skeleton-bone fallback inside
            // GetFingerPosition()) can be unavailable during intro holotape playback. Without a
            // fallback here, the physical eject button silently stops responding for the whole
            // window. Fall back to the pointing-hand wand (controller) position for THIS
            // function only — the shared _interactionFingerValid sample is left untouched since
            // CheckHandPush()/CheckHolotapeRemoval() don't need this fallback.
            if (auto* playerNodes = f4cf::f4vr::getPlayerNodes()) {
                const bool isLeftHanded = VRInput::GetSingleton().IsLeftHandedMode();
                // SecondaryWandNode = LEFT hand, primaryWandNode = RIGHT hand.
                auto* wand = isLeftHanded ? playerNodes->SecondaryWandNode : playerNodes->primaryWandNode;
                if (wand && IsUsableWorldPoint(wand->world.translate)) {
                    fingerPos = wand->world.translate;
                    haveFingerPos = true;
                    usingWandFallbackPos = true;
                    spdlog::debug("[PIPBOY] Eject: finger tracking unavailable during intro playback, "
                                  "falling back to wand position=({:.1f},{:.1f},{:.1f})",
                                  fingerPos.x, fingerPos.y, fingerPos.z);
                }
            }
        }
        if (!haveFingerPos) {
            _ejectContactLatched = false;
            if (animateNode && _buttonOriginalZSet) animateNode->local.translate.z = _buttonOriginalZ;
            return;
        }
        const float contactRadius = (std::clamp)(
            g_config.handContactSlop, EJECT_FINGER_RADIUS, 4.0f);
        const float releaseRadius = contactRadius + CONTACT_RELEASE_PAD;

        SweptMeshContact contact = QuerySweptMeshContact(
            ejectContactTarget,
            fingerPos,
            _previousInteractionFingerPos,
            _previousInteractionFingerValid,
            contactRadius,
            releaseRadius);

        // Mesh extraction is supported by the vanilla button and remains the active path.
        // Retain a node-point fallback for replacement Pip-Boy NIFs whose geometry buffers
        // are GPU-only, so those mods do not lose the eject control entirely.
        if (!contact.meshAvailable) {
            contact.currentDistance = PointDistance(fingerPos, ejectContactTarget->world.translate);
            contact.sweptDistance = _previousInteractionFingerValid
                ? PointToSegmentDistance(
                    ejectContactTarget->world.translate,
                    _previousInteractionFingerPos,
                    fingerPos)
                : contact.currentDistance;
        }

        // The press used to fire the instant the fingertip entered the padded contact bubble
        // (contactRadius includes handContactSlop), i.e. visibly BEFORE the finger touched the
        // button. Model the real thing instead: the button starts moving exactly when the
        // fingertip SKIN reaches the mesh (tracked-point distance = EJECT_SKIN_RADIUS), rides
        // the finger 1:1 through the button's actual travel (|EJECT_Z_MIN|), and fires at full
        // depression.
        // Fallback paths are exempt from the tight fire distance: the node fallback (GPU-only
        // replacement NIFs) measures to the node CENTER, and the intro-playback wand fallback
        // trails the visible fingertip by several gu — either would make the button
        // unpressable (review-confirmed) — so both keep the old whole-radius fire.
        const bool preciseTouch = contact.meshAvailable && !usingWandFallbackPos;
        const float engageDistance = preciseTouch ? EJECT_SKIN_RADIUS : contactRadius;
        const float fireDistance = preciseTouch
            ? (std::max)(EJECT_SKIN_RADIUS + EJECT_Z_MIN, 0.05f)  // EJECT_Z_MIN is negative: skin contact minus full button travel
            : contactRadius;
        // RE-ARM FIX (Jul 25, user: "pressed multiple times, it pressed in, deck didn't
        // open"): the old release radius was the whole engage bubble + hysteresis pad — up
        // to ~6 gu (~8.5 cm) of retreat before the latch re-armed. Meanwhile the press
        // VISUAL below is purely distance-driven and kept animating, so repeat presses
        // looked valid while the latch silently stayed armed and never re-fired
        // (log-confirmed: whole test session recorded only 3 registered presses). On the
        // precise path the finger only needs to lift just clear of the button skin to
        // re-arm — one button-height above the engage point.
        const float rearmDistance = preciseTouch
            ? (EJECT_SKIN_RADIUS + 1.0f)
            : releaseRadius;
        const auto latch = pipboy_mesh_contact::UpdateLatch(
            _ejectContactLatched,
            contact.currentDistance,
            contact.sweptDistance,
            fireDistance,
            rearmDistance,
            _ejectCooldown <= 0.0f);
        _ejectContactLatched = latch.latched;

        // Periodic distance log (every ~5 seconds at 90fps)
        static int distanceLogCounter = 0;
        if (++distanceLogCounter >= 450) {
            distanceLogCounter = 0;
            // Print the thresholds the latch ACTUALLY uses (engage/fire/rearm),
            // not the fallback contact/release radii — the old line printed
            // enter=2.5/exit=4.5 while the precise mesh path fires at ~0.25,
            // which misled a log audit into flagging a working latch as broken.
            spdlog::debug("[PIPBOY-DIAG] eject mesh={} finger=({:.1f},{:.1f},{:.1f}) currentDist={:.2f} sweptDist={:.2f} engage={:.2f} fire={:.2f} rearm={:.2f} latched={} cooldown={:.2f} state={}",
                contact.meshAvailable,
                fingerPos.x, fingerPos.y, fingerPos.z,
                contact.currentDistance, contact.sweptDistance,
                engageDistance, fireDistance, rearmDistance,
                _ejectContactLatched,
                _ejectCooldown, static_cast<int>(_tapeDeckState));
        }

        // Visual depression starts at skin contact (engageDistance) and reaches FULL travel
        // exactly at the fire distance, so the button moves only while the finger is really
        // on it and the deck action lands the moment it looks fully pressed in. While the
        // latch is armed (fired, finger not yet lifted clear) the button HOLDS fully
        // depressed — a distance-driven visual here kept animating fresh-looking presses
        // that could not fire, which is exactly what confused repeat presses.
        if (animateNode && _buttonOriginalZSet) {
            const float travelRange = (std::max)(engageDistance - fireDistance, 0.01f);
            const float press = _ejectContactLatched
                ? 1.0f
                : (std::clamp)(
                      (engageDistance - contact.currentDistance) / travelRange, 0.0f, 1.0f);
            animateNode->local.translate.z = _buttonOriginalZ + EJECT_Z_MIN * press;
        }

        if (latch.triggered) {
            spdlog::info("[PIPBOY] Eject button pressed via {} contact (current={:.2f}, swept={:.2f})",
                contact.meshAvailable ? "mesh" : "node fallback",
                contact.currentDistance, contact.sweptDistance);
            _ejectCooldown = EJECT_COOLDOWN_TIME;

            // Haptic on the pointing hand (always right — Pipboy stays on left wrist)
            VRInput::GetSingleton().TriggerHaptic(false /*right hand*/, 5000);

            // Context-aware eject button behavior
            if (_tapeDeckState == TapeDeckState::Closed || _tapeDeckState == TapeDeckState::Closing) {

                // If terminal is active on wrist, close it first
                if (_terminalRedirectActive || _pendingTerminalRedirect) {
                    if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                        msgQueue->AddMessage(MenuTerminal(), RE::UI_MESSAGE_TYPE::kHide);
                        spdlog::debug("[PIPBOY] Eject: closing TerminalMenu");
                    }
                }

                // CLOSED → OPEN (via eject — player wants to remove holotape)
                _tapeDeckOpen = true;
                _tapeDeckState = TapeDeckState::Opening;
                _deckOpenedByEject = true;
                // The finger that pressed the button has not moved yet, and the button is
                // right beside the tray — without this it pushes the deck shut on the way out.
                _deckPushCloseLockout = DECK_PUSH_CLOSE_LOCKOUT;
                _holotapeGrabCooldown = 0.5f;  // Prevent immediate insertion when deck opens
                _holotapeRemovalCooldown = 0.0f; // Eject means the player may take the loaded tape immediately
                _holotapeRemovalRequiresGripRelease = false; // This is a deliberate new removal cycle

                // Cancel any pending delayed playback
                if (_pendingPlaybackFormID != 0) {
                    spdlog::debug("[PIPBOY] Cancelled pending playback for {:08X} (deck opened)", _pendingPlaybackFormID);
                    _pendingPlaybackFormID = 0;
                    _pendingPlaybackDelay = 0.0f;
                }
                if (_pendingAudioFormID != 0) {
                    spdlog::debug("[PIPBOY] Cancelled pending audio playback for {:08X} (deck opened)", _pendingAudioFormID);
                    _pendingAudioFormID = 0;
                    _pendingAudioWaitFrames = 0;
                }

                // If holotape is loaded and playing, stop playback so user can grab it out
                if (_holotapeLoaded && _loadedHolotapeFormID != 0) {
                    StopIntroPlayback();  // Stop intro audio if playing

                    // Stop intro SWF animation — Pipboy stays open so FRIK can close it naturally
                    if (_introSWFActive) {
                        spdlog::info("[PIPBOY] Eject during intro SWF — stopping animation");
                        StopNarrationWav();  // Stop intro audio if still playing (Win32 path)
                        RestoreIntroHolotapeType(_introHolotapeFormID);
                        _introSWFActive = false;

                        _introSWFMenuSeen = false;
                        _introSWFLastLogSec = 0;
                        _introAudioStarted = false;
                        _introSoundEvents.clear();
                        _introSoundEventIndex = 0;
                        _pendingProgramFormID = 0;

                        // Close HolotapeMenu so the SWF is fully destroyed.
                        // PipboyMenu stays open — FRIK controls its lifecycle.
                        // Closing PipboyMenu ourselves bypasses FRIK's _isOpen flag,
                        // leaving the player permanently restrained.
                        if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                            msgQueue->AddMessage(MenuPipboyHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                            msgQueue->AddMessage(MenuHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                            spdlog::debug("[PIPBOY] Closed PipboyHolotapeMenu+HolotapeMenu on eject abort (kForceHide)");
                        }
                    }
                    // Also handle non-intro game holotape SWF cleanup
                    if (_programSWFActive) {
                        _programSWFActive = false;
                        EnableMenuInput(MenuPipboy());
                        if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
                            msgQueue->AddMessage(MenuPipboyHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                            msgQueue->AddMessage(MenuHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
                        }
                        // Restore cursor flag on PipboyMenu
                        if (auto* ui = RE::UI::GetSingleton()) {
                            auto pipMenu = ui->GetMenu(MenuPipboy());
                            if (pipMenu) pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
                        }
                        spdlog::info("[PIPBOY] Game holotape SWF stopped on eject");
                    }
                    auto* noteForm = RE::TESForm::GetFormByID(_loadedHolotapeFormID);
                    if (noteForm && noteForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                        auto* holotape = static_cast<RE::BGSNote*>(noteForm);
                        auto* p = RE::PlayerCharacter::GetSingleton();
                        if (p && p->IsHolotapePlaying(holotape)) {
                            p->PauseHolotape(holotape);
                            spdlog::debug("[PIPBOY] Stopped holotape {:08X} on deck open", _loadedHolotapeFormID);
                        }
                    }
                    spdlog::debug("[PIPBOY] Tape deck → Opening (holotape inside, ready for removal)");
                } else {
                    spdlog::debug("[PIPBOY] Tape deck → Opening");
                }
                PlayWavSound("Eject button press.wav");
            } else {
                // OPEN — eject button does NOT close the deck (only slam gesture closes it)
                spdlog::debug("[PIPBOY] Eject button ignored — deck already open");
            }

            // Release FRIK's pointing hand pose so the finger curls back after pressing
            // Always right hand (Pipboy stays on left wrist)
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.IsAvailable()) {
                frik.ClearHandPoseFingerPositions(false /*right hand*/);
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Tape deck animation — per-frame lerp (matching FRIK PipboyPhysicalHandler)
    // Each frame, _tapeDeckAnimProgress moves toward target by ANIM_SPEED.
    // Absolute rotation is set via MatrixUtils::getMatrixFromEulerAngles.
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::UpdateTapeDeckAnimation()
    {
        // No tape deck animation in power armor (Pipboy is projected)
        if (Utils::IsPlayerInPowerArmor()) return;

        // The closed, empty deck uses the skeleton's identity/default pose.
        // Once that pose has been initialized there is nothing to rewrite, so
        // avoid rebuilding a second full arm-subtree liveness snapshot at the
        // end of every frame. Any interaction state immediately leaves this
        // fast path.
        if (_meshesInitialized &&
            _tapeDeckState == TapeDeckState::Closed &&
            !_tapeDeckOpen &&
            std::fabs(_tapeDeckAnimProgress) < 0.001f &&
            !_holotapeLoaded &&
            !_tapREFForceHidden &&
            !_insertionOpenHandActive)
        {
            return;
        }

        // HookEndUpdate is a distinct liveness phase. FRIK/game animation may
        // have replaced the arm subtree after OnFrameUpdate, so never reuse the
        // earlier phase's membership result here.
        BeginNodeValidationPhase();

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        // Use cached nodes after the phase snapshot has proved their liveness.
        RE::NiAVObject* tapeDeckNode = GetCachedTapeDeckNode();
        // Keep the local check so this use site retains its validation cadence;
        // it is now an allocation-free pointer lookup in the phase snapshot.
        if (_cachedTapeDeckLid &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedTapeDeckLid)) {
            _cachedTapeDeckLid      = nullptr;
            _cachedTapeRef          = nullptr;
            _cachedTapeDeckMesh1    = nullptr;
            _cachedTapeDeckLidMesh1 = nullptr;
        }
        if (!_cachedTapeDeckLid && tapeDeckNode) {
            _cachedTapeDeckLid     = f4vr::findAVObject(arm, "TapeDeckLid_mesh");
            _cachedTapeRef         = f4vr::findAVObject(arm, "TapREF");
            _cachedTapeDeckMesh1   = f4vr::findAVObject(tapeDeckNode, "TapeDeck01_mesh:1");
            _cachedTapeDeckLidMesh1= _cachedTapeDeckLid ? f4vr::findAVObject(_cachedTapeDeckLid, "TapeDeckLid_mesh:1") : nullptr;
        }
        RE::NiAVObject* tapeDeckLidNode = _cachedTapeDeckLid;
        RE::NiAVObject* tapeRef         = _cachedTapeRef;

        if (!tapeDeckNode && !tapeDeckLidNode) return;

        RE::NiAVObject* tapeDeckMesh    = _cachedTapeDeckMesh1;
        RE::NiAVObject* tapeDeckLidMesh = _cachedTapeDeckLidMesh1;

        static bool loggedNodes = false;
        if (!loggedNodes) {
            loggedNodes = true;
            spdlog::debug("[PIPBOY] Tape deck nodes (3rd-person skeleton): TapeDeck01={} @{}, TapeDeckLid_mesh={} @{}, TapeDeck01_mesh:1={} @{}, TapeDeckLid_mesh:1={} @{}, TapREF={}",
                tapeDeckNode    ? "FOUND" : "MISSING", (void*)tapeDeckNode,
                tapeDeckLidNode ? "FOUND" : "MISSING", (void*)tapeDeckLidNode,
                tapeDeckMesh    ? "FOUND" : "MISSING", (void*)tapeDeckMesh,
                tapeDeckLidMesh ? "FOUND" : "MISSING", (void*)tapeDeckLidMesh,
                tapeRef         ? "FOUND" : "MISSING");

            // Cross-check: log what firstPersonSkeleton would give (for comparison)
            auto* player = f4vr::getPlayer();
            if (player && player->firstPerson3D.get()) {
                auto* fpArm = f4vr::findAVObject(player->firstPerson3D.get(), "LArm_ForeArm3");
                auto* fpTapeDeck = fpArm ? f4vr::findAVObject(fpArm, "TapeDeck01_mesh:1") : nullptr;
                spdlog::debug("[PIPBOY] COMPARE: 1stPerson LArm_ForeArm3={} TapeDeck01_mesh:1={} | 3rdPerson arm={} mesh={}",
                    (void*)fpArm, (void*)fpTapeDeck, (void*)arm, (void*)tapeDeckMesh);
            }
        }

        // On first call (or after load), force meshes to closed rotation (save game may have stale open rotation)
        if (!_meshesInitialized) {
            _meshesInitialized = true;
            RE::NiMatrix3 identity;
            identity.entry[0][0] = 1; identity.entry[0][1] = 0; identity.entry[0][2] = 0;
            identity.entry[1][0] = 0; identity.entry[1][1] = 1; identity.entry[1][2] = 0;
            identity.entry[2][0] = 0; identity.entry[2][1] = 0; identity.entry[2][2] = 1;
            if (tapeDeckMesh) tapeDeckMesh->local.rotate = identity;
            if (tapeDeckLidMesh) tapeDeckLidMesh->local.rotate = identity;
            spdlog::debug("[PIPBOY] Initialized tape deck rotation to closed (identity)");
        }

        // Target progress based on open/closed state
        const float targetProgress = _tapeDeckOpen ? 1.0f : 0.0f;

        // Force TapREF visibility EVERY FRAME because the game's animation system
        // resets node transforms each frame (same reason we apply rotations every frame).
        // Hide TapREF when deck is closed, or when force-hidden after removal.
        if (tapeRef) {
            if (_tapREFForceHidden) {
                tapeRef->local.scale = 0.0f;
            } else if (_holotapeLoaded && _tapeDeckAnimProgress > 0.01f) {
                tapeRef->local.scale = 1.0f;  // TapREF is unscaled; only hand-held holotape uses pipboyScale
            } else {
                tapeRef->local.scale = 0.0f;
            }
        }

        // Smoothly interpolate toward target
        // Use ANIM_SPEED for opening, _closeAnimSpeed for closing (variable for slam)
        // In Pushing state, _tapeDeckAnimProgress is driven directly by hand position
        if (_tapeDeckState == TapeDeckState::Pushing) {
            // During pushing, the progress is set directly by CheckHandPush.
            // Guard against NaN (can happen if finger position was invalid) — NaN
            // propagates through rotation matrices and corrupts node transforms.
            if (std::isnan(_pushProgress)) {
                _pushProgress = 1.0f;  // Default to open
                _tapeDeckState = TapeDeckState::Open;
            }
            _tapeDeckAnimProgress = std::clamp(_pushProgress, 0.0f, 1.0f);
        } else if (std::fabs(_tapeDeckAnimProgress - targetProgress) > 0.001f) {
            if (_tapeDeckAnimProgress < targetProgress) {
                _tapeDeckAnimProgress = (std::min)(_tapeDeckAnimProgress + ANIM_SPEED, targetProgress);
            } else {
                _tapeDeckAnimProgress = (std::max)(_tapeDeckAnimProgress - _closeAnimSpeed, targetProgress);
            }
        }

        // Update tape deck state when animation reaches endpoints.
        // This is OUTSIDE the interpolation block because the push/removal paths can set
        // _tapeDeckAnimProgress to 1.0 directly, which would skip the interpolation
        // block entirely (|1.0 - 1.0| < 0.001) and leave the state stuck in Opening.
        if (std::fabs(_tapeDeckAnimProgress - 1.0f) < 0.001f) {
            if (_tapeDeckState == TapeDeckState::Opening) {
                _tapeDeckState = TapeDeckState::Open;
                // Start every newly opened cycle from one coherent mechanical
                // state. A completed close left _pushProgress at zero and the
                // prior stroke metadata live; although a later contact normally
                // recaptured progress, failures before recapture exposed that
                // stale state in both behavior and diagnostics.
                _pushProgress = 1.0f;
                _deckPushStrokeValid = false;
                _deckPushStrokeUsesWeapon = false;
                _deckPushStrokeOrigin = {};
                _deckPushStrokeDirection = {};
                _deckPushStrokeStartProgress = 1.0f;
                _deckPushStrokeRequiredTravel = 3.0f;
                _deckPushStrokeMaxTravel = 0.0f;
                spdlog::debug("[PIPBOY] Tape deck fully open");
            }
        } else if (std::fabs(_tapeDeckAnimProgress) < 0.001f) {
            if (_tapeDeckState == TapeDeckState::Closing) {
                _tapeDeckState = TapeDeckState::Closed;
                _ejectCooldown = 0.5f;  // Prevent accidental re-open right after closing
                if (_insertionOpenHandActive) {
                    auto& frikClear = FRIKInterface::GetSingleton();
                    if (frikClear.IsAvailable())
                        frikClear.ClearHandPoseFingerPositions(_insertionOpenHandIsLeft);
                    _insertionOpenHandActive = false;
                }
                _deckPushCloseLockout = 0.0f;
                _deckOpenedByEject = false;
                spdlog::debug("[PIPBOY] Tape deck closed (0.5s eject cooldown)");

                // Holotape was added to inventory at insertion time.
                // Now trigger playback.  Method depends on holotape type:
                //   kVoice  → delayed PlayPipboyLoadHolotapeAnim for full audio pipeline
                //   kProgram → immediate PlayPipboyLoadHolotapeAnim(noAnim=true) for SWF UI
                //   kTerminal → immediate terminal handler (opens TerminalMenu on Pipboy)
                // Intro holotape is kVoice — plays custom WAV audio via delay timer.
                if (_loadedHolotapeFormID != 0) {
                    if (auto* noteForm = RE::TESForm::GetFormByID(_loadedHolotapeFormID);
                        noteForm && noteForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                        auto* holotape = static_cast<RE::BGSNote*>(noteForm);
                        auto noteType = static_cast<RE::BGSNote::NOTE_TYPE>(holotape->type);
                        spdlog::debug("[PIPBOY] Holotape {:08X} type={} (0=Voice,1=Scene,2=Program,3=Terminal)",
                                     _loadedHolotapeFormID, holotape->type);

                        // Our intro holotape is stored as kVoice to prevent the game
                        // from auto-playing it on AddItem.  Switch to kProgram now for SWF playback.
                        // Also re-set programFile every time — game reverts form fields on save/load.
                        bool isOurHolotape = (_introHolotapeFormID != 0 &&
                                              _loadedHolotapeFormID == _introHolotapeFormID);
                        if (isOurHolotape) {
                            if (noteType == RE::BGSNote::NOTE_TYPE::kVoice) {
                                holotape->type = RE::BGSNote::NOTE_TYPE::kProgram;
                                noteType = RE::BGSNote::NOTE_TYPE::kProgram;
                                spdlog::debug("[PIPBOY] Switched intro holotape to kProgram for SWF playback");
                            }
                            holotape->programFile = "Heisenberg";
                            spdlog::debug("[PIPBOY] Ensured programFile='Heisenberg' for intro holotape");
                        }

                        // Terminal tapes retain the direct normal-Pip-Boy override. The intro
                        // program is handled below by BeginIntroProgramPlayback(), whose Holo
                        // path waits for FRIK's replacement root before activating anything.
                        if (noteType == RE::BGSNote::NOTE_TYPE::kTerminal && IsProjectedPipboyNow()) {
                            if (g_config.holotapeWristOverrideInProjected && !IsFrikHoloPipboyEnabled()) {
                                spdlog::info("[PIPBOY] Holotape {:08X} inserted in projected mode — forcing wrist playback",
                                             _loadedHolotapeFormID);
                                BeginWristOverrideForHolotape();
                            } else {
                                spdlog::info("[PIPBOY] Holotape {:08X} projected-mode wrist override SKIPPED (cfg={}, frikHolo={})",
                                             _loadedHolotapeFormID, g_config.holotapeWristOverrideInProjected, IsFrikHoloPipboyEnabled());
                            }
                        }

                        if (noteType == RE::BGSNote::NOTE_TYPE::kTerminal) {
                            // Terminal holotape — immediate, no audio to conflict with slam
                            // Skip redirect in PA / projected mode (no wrist to render to)
                            if (_terminalPatchesSuspended) {
                                spdlog::debug("[PIPBOY] Terminal holotape {:08X} skipped redirect — patches suspended (PA/projected)",
                                             _loadedHolotapeFormID);
                            } else if (_terminalRedirectActive) {
                                spdlog::warn("[PIPBOY] Terminal holotape {:08X} skipped — "
                                             "terminal redirect already active (world={})",
                                             _loadedHolotapeFormID, _isWorldTerminalRedirect);
                            } else {
                            auto* terminal = holotape->GetNoteTerminal();
                            spdlog::debug("[PIPBOY] Terminal {:08X}: BGSTerminal={:X} noteFormID={:X} forceWrist={}",
                                         _loadedHolotapeFormID,
                                         terminal ? reinterpret_cast<uintptr_t>(terminal) : uintptr_t(0),
                                         static_cast<std::uint32_t>(holotape->noteFormID),
                                         g_config.forceTerminalOnWrist);
                            spdlog::default_logger()->flush();

                            if (terminal) {
                                static auto pipMgrGlobal = REL::Offset(0x5940758).address();
                                auto* mgr = *reinterpret_cast<void**>(pipMgrGlobal);
                                auto mgrAddr = reinterpret_cast<uintptr_t>(mgr);

                                if (mgrAddr != 0) {
                                    *reinterpret_cast<uintptr_t*>(mgrAddr + 0x190) =
                                        reinterpret_cast<uintptr_t>(holotape);

                                    using TermHandlerFunc = void(*)(void*, void*);
                                    static REL::Relocation<TermHandlerFunc> termHandler{
                                        REL::Offset(0xc33c60) };

                                    if (g_config.forceTerminalOnWrist) {
                                        // Signal FRIK to keep Pipboy open
                                        static auto setKeepOpen = []() -> void(*)(bool) {
                                            auto frikDll = GetModuleHandleA("FRIK.dll");
                                            if (!frikDll) return nullptr;
                                            return reinterpret_cast<void(*)(bool)>(
                                                GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                                        }();
                                        if (setKeepOpen) setKeepOpen(true);

                                        // LOCK render singletons to prevent InitRenderer from changing them
                                        static auto layerSingletonAddr = REL::Offset(0x5ac8eb0).address();
                                        static auto modeSingletonAddr  = REL::Offset(0x5ac72b8).address();
                                        auto layerS = *reinterpret_cast<uintptr_t*>(layerSingletonAddr);
                                        auto modeS  = *reinterpret_cast<uintptr_t*>(modeSingletonAddr);

                                        _savedLayerValue = layerS ? *reinterpret_cast<int*>(layerS + 0x36c) : -1;
                                        _savedModeValue  = modeS  ? *reinterpret_cast<int*>(modeS + 0x36c) : -1;
                                        _savedLayerLock  = layerS ? *reinterpret_cast<int*>(layerS + 0x374) : 0;
                                        _savedModeLock   = modeS  ? *reinterpret_cast<int*>(modeS + 0x374) : 0;
                                        spdlog::info("[PIPBOY] Saved render state (holotape): layer={:#x} lock={}, mode={:#x} lock={}",
                                                    _savedLayerValue, _savedLayerLock, _savedModeValue, _savedModeLock);
                                        if (layerS) *reinterpret_cast<int*>(layerS + 0x374) = 1;
                                        if (modeS)  *reinterpret_cast<int*>(modeS + 0x374) = 1;

                                        // Set +0x1a=1 before handler (forces non-projected wrist path)
                                        *reinterpret_cast<char*>(mgrAddr + 0x1a) = 1;

                                        termHandler(mgr, terminal);
                                        _pendingTerminalRedirect = true;
                                    } else {
                                        termHandler(mgr, terminal);
                                    }

                                    spdlog::debug("[PIPBOY] Terminal handler called for {:08X}",
                                                 _loadedHolotapeFormID);

                                }
                            }
                            } // end else (guard against double redirect)
                        } else if (noteType == RE::BGSNote::NOTE_TYPE::kProgram) {
                            // Program holotape — skip in PA (wrist pipboy not available).
                            if (Utils::IsPlayerInPowerArmor()) {
                                spdlog::debug("[PIPBOY] Program holotape {:08X} skipped — player in power armor",
                                             _loadedHolotapeFormID);
                            } else {
                            if (isOurHolotape) {
                                BeginIntroProgramPlayback(_loadedHolotapeFormID);
                            } else {
                            // Program holotape — open Pipboy immediately, then defer SWF loading
                            _pendingProgramFormID = _loadedHolotapeFormID;
                            spdlog::debug("[PIPBOY] Queued program holotape {:08X} (programFile='{}') — opening Pipboy",
                                         _loadedHolotapeFormID, holotape->GetNoteProgram().c_str());
                            // Activate Pipboy screen so it lights up for the holotape SWF
                            ActivatePipboyScreen();

                            // Projected pipboy mode + Pipboy Hacking on: force the wrist
                            // render path so the holotape SWF displays on the wrist Pipboy
                            // instead of the projected display. Mirrors the kTerminal trick:
                            //   1. Pre-arm FRIK keepOpen so the wrist Pipboy stays visible.
                            //   2. Lock render singleton values so InitRenderer can't flip
                            //      back to projected.
                            //   3. PipboyManager+0x1a=1 selects the non-projected wrist path.
                            // Skip the projected->wrist render hack in FRIK HOLO mode: locking the
                            // render singletons (+0x374) + FRIK SetKeepPipboyOpen + PipboyManager poke
                            // deadlocks FRIK's holo Pip-Boy (and the lock left set by the 1st insert is
                            // why the 2nd insert hangs). Normal projected Pip-Boy is fine. In holo mode
                            // the program holotape just plays on the (holo) Pip-Boy natively.
                            const bool projectedHack =
                                IsProjectedPipboyAtLoad() && g_config.forceTerminalOnWrist
                                && !IsFrikHoloPipboyEnabled();
                            if (projectedHack) {
                                static auto setKeepOpen = []() -> void(*)(bool) {
                                    auto frikDll = GetModuleHandleA("FRIK.dll");
                                    if (!frikDll) return nullptr;
                                    return reinterpret_cast<void(*)(bool)>(
                                        GetProcAddress(frikDll, "FRIKAPI_SetKeepPipboyOpenForTerminal"));
                                }();
                                if (setKeepOpen) setKeepOpen(true);

                                static auto layerSingletonAddr = REL::Offset(0x5ac8eb0).address();
                                static auto modeSingletonAddr  = REL::Offset(0x5ac72b8).address();
                                auto layerS = *reinterpret_cast<uintptr_t*>(layerSingletonAddr);
                                auto modeS  = *reinterpret_cast<uintptr_t*>(modeSingletonAddr);
                                if (layerS) *reinterpret_cast<int*>(layerS + 0x374) = 1;
                                if (modeS)  *reinterpret_cast<int*>(modeS + 0x374) = 1;

                                if (auto* pipMgr = GetPipboyManagerVR()) {
                                    *reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(pipMgr) + 0x1a) = 1;
                                }
                                spdlog::info("[PIPBOY] Program holotape {:08X}: forced wrist path (projected+hacking)",
                                             _loadedHolotapeFormID);
                            }

                            // Open Pipboy the FRIK way — set INI angles to 0.
                            // This triggers the game's VR Pipboy system to open PipboyMenu
                            // without entering camera mode, so the world doesn't pause.
                            // (PlayPipboyLoadHolotapeAnim uses the flat-screen path which
                            // pauses the world via StartPipboyMode + OnPipboyOpenAnim.)
                            // Our pending program handler (above) will detect PipboyMenu is
                            // open and set up the holotape SWF from there.
                            if (auto* s = RE::GetINISetting("fHMDToPipboyScaleOuterAngle:VRPipboy")) s->SetFloat(0.0f);
                            if (auto* s = RE::GetINISetting("fHMDToPipboyScaleInnerAngle:VRPipboy")) s->SetFloat(0.0f);
                            if (auto* s = RE::GetINISetting("fPipboyScaleOuterAngle:VRPipboy"))      s->SetFloat(0.0f);
                            if (auto* s = RE::GetINISetting("fPipboyScaleInnerAngle:VRPipboy"))      s->SetFloat(0.0f);
                            spdlog::debug("[PIPBOY] Opened Pipboy via INI angles (FRIK-style, no world pause)");
                            }
                            }
                        } else {
                            // Voice/Scene holotape — delay playback so slam sound finishes
                            _pendingPlaybackFormID = _loadedHolotapeFormID;
                            _pendingPlaybackDelay = 0.7f;
                            spdlog::debug("[PIPBOY] Queued audio holotape {:08X} playback (0.7s delay for slam sound)",
                                         _loadedHolotapeFormID);
                        }
                    }
                }

                // Holotape remains loaded in the deck — TapREF stays visible
                // when the deck is reopened via eject. Only cleared on grip removal.

                // Reset close anim speed to default
                _closeAnimSpeed = ANIM_SPEED;
            }
        }

        // Apply rotations EVERY FRAME (not just during animation) because the
        // game's animation system resets node transforms each frame.

        // TapeDeck01_mesh:1: rotates 16 degrees negative X when open
        if (tapeDeckMesh) {
            const float angle1 = MatrixUtils::degreesToRads(-TAPE_DECK_OPEN_ANGLE * _tapeDeckAnimProgress);
            tapeDeckMesh->local.rotate = MatrixUtils::getMatrixFromEulerAngles(angle1, 0, 0);
        }

        // TapeDeckLid_mesh:1: rotates 18 degrees positive X when open
        if (tapeDeckLidMesh) {
            const float angle2 = MatrixUtils::degreesToRads(TAPE_LID_OPEN_ANGLE * _tapeDeckAnimProgress);
            tapeDeckLidMesh->local.rotate = MatrixUtils::getMatrixFromEulerAngles(angle2, 0, 0);
        }

        // TapREF is a child of TapeDeck01 (which does NOT rotate).
        // We manually orbit and rotate TapREF around TapeDeck01's local origin
        // to match the tray mesh rotation.  Hide when nearly closed to avoid clipping.
        if (tapeRef && _holotapeLoaded) {
            if (_tapeDeckAnimProgress < 0.15f) {
                tapeRef->local.scale = 0.0f;
            } else {
                tapeRef->local.scale = 1.0f;
                const float angle = MatrixUtils::degreesToRads(TAPE_DECK_OPEN_ANGLE * _tapeDeckAnimProgress);

                // Artist-placed TapREF position in TapeDeck01 local space
                static constexpr float refX = -0.0459f;
                static constexpr float refY =  1.9395f;
                static constexpr float refZ =  0.0071f;

                // Orbit around TapeDeck01 local origin (pivot = 0,0,0)
                float cosA = std::cos(angle);
                float sinA = std::sin(angle);

                float newY = refY * cosA - refZ * sinA;
                float newZ = refY * sinA + refZ * cosA;

                tapeRef->local.translate.x = refX;
                tapeRef->local.translate.y = newY;
                tapeRef->local.translate.z = newZ;
                tapeRef->local.rotate = MatrixUtils::getMatrixFromEulerAngles(-angle, 0, 0);
            }
        } else if (tapeRef && !_holotapeLoaded) {
            // Extra safety: force TapREF hidden when no holotape is loaded
            tapeRef->local.scale = 0.0f;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Holotape physical removal — detect grip near open tape deck
    // The non-pipboy hand (right in normal, left in left-handed mode) must
    // be gripping near the tape deck while it's open with a holotape loaded.
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::CheckHolotapeRemoval()
    {
        // Only check when deck is open (or opening/pushing) and a holotape is loaded.
        // Include Pushing state: player reaching toward the deck to grab the holotape
        // also triggers push mode (finger close to deck), so we must allow removal there too.
        if (!_holotapeLoaded) return;
        if (_tapeDeckState != TapeDeckState::Open && _tapeDeckState != TapeDeckState::Opening
            && _tapeDeckState != TapeDeckState::Pushing) return;
        // The grabbing hand is the non-pipboy hand. Pipboy is always on the left
        // wrist (doesn't swap in LH mode), so the grab hand is always the right hand.
        bool grabHandIsLeft = false;

        // Check VR grip input directly (bypasses Hand state machine which may
        // have already consumed the grip press for a different grab this frame)
        bool gripPressed = VRInput::GetSingleton().IsPressed(grabHandIsLeft, VRButton::Grip);

        // EndGrab during insertion does not release the physical controller button.
        // A timer alone therefore let that same held Grip remove the tape again as
        // soon as the timer expired. Require a real release before a later press can
        // start removal; update this gate even while the short debounce is running.
        const bool wasWaitingForRelease =
            _holotapeRemovalRequiresGripRelease;
        const auto releaseGate =
            pipboy_mesh_contact::UpdateGripReleaseGate(
                _holotapeRemovalRequiresGripRelease,
                gripPressed);
        _holotapeRemovalRequiresGripRelease =
            releaseGate.requiresRelease;
        if (wasWaitingForRelease &&
            !_holotapeRemovalRequiresGripRelease) {
            spdlog::debug(
                "[PIPBOY] Insert grip released — holotape removal re-armed");
        }
        if (releaseGate.blocksAction) return;
        if (_holotapeRemovalCooldown > 0.0f) return;
        if (!gripPressed) return;

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        RE::NiAVObject* tapeDeck = GetCachedTapeDeckNode();
        if (!tapeDeck) return;

        // Keep the controller as a compatibility fallback, but use the visible fingertip
        // against the visible tape mesh as the primary grab test. The controller origin sits
        // well behind the rendered hand; current runtime traces showed 26-40 units while the
        // fingers were at the deck, outside the old 15-unit controller sphere.
        auto& heisenberg = Heisenberg::GetSingleton();
        Hand* grabHand = grabHandIsLeft ? heisenberg.GetLeftHand() : heisenberg.GetRightHand();
        if (!grabHand) return;

        RE::NiPoint3 handPos = grabHand->GetPosition();

        // Measure against TapREF (the visible holotape), not the rotating tray pivot.
        RE::NiAVObject* tapeRef = _cachedTapeRef;
        if (!tapeRef ||
            !CachedNodeIsLiveWithin(
                arm,
                tapeRef)) {
            tapeRef = SafeFindAVObject(arm, "TapREF");
            _cachedTapeRef = tapeRef;
        }
        const RE::NiPoint3 targetPos = tapeRef ? tapeRef->world.translate : tapeDeck->world.translate;
        const float controllerDistance = PointDistance(handPos, targetPos);

        bool visibleHandContact = false;
        float fingerDistance = (std::numeric_limits<float>::max)();
        bool tapeMeshAvailable = false;
        if (_interactionFingerValid) {
            SweptMeshContact tapeContact = QuerySweptMeshContact(
                tapeRef,
                _interactionFingerPos,
                _previousInteractionFingerPos,
                _previousInteractionFingerValid,
                TAPE_REMOVAL_CONTACT_RADIUS);
            tapeMeshAvailable = tapeContact.meshAvailable;
            fingerDistance = tapeContact.meshAvailable
                ? tapeContact.sweptDistance
                : (_previousInteractionFingerValid
                    ? PointToSegmentDistance(
                        targetPos,
                        _previousInteractionFingerPos,
                        _interactionFingerPos)
                    : PointDistance(_interactionFingerPos, targetPos));
            visibleHandContact = fingerDistance <= (tapeContact.meshAvailable
                ? TAPE_REMOVAL_CONTACT_RADIUS
                : TAPE_GRAB_RADIUS);
        }
        const bool controllerContact = controllerDistance <= TAPE_GRAB_RADIUS;

        auto& grabMgr = GrabManager::GetSingleton();
        bool alreadyGrabbing = grabMgr.IsGrabbing(grabHandIsLeft);

        spdlog::debug("[PIPBOY] Holotape removal check: grip=true mesh={} fingerDist={:.2f} controllerDist={:.1f} alreadyGrab={} "
                     "hand=({:.1f},{:.1f},{:.1f}) target=({:.1f},{:.1f},{:.1f})",
                     tapeMeshAvailable, fingerDistance, controllerDistance, alreadyGrabbing,
                     handPos.x, handPos.y, handPos.z,
                     targetPos.x, targetPos.y, targetPos.z);

        if (!visibleHandContact && !controllerContact) return;

        // Don't remove the loaded holotape if the hand already holds something
        // (e.g. player bringing a different holotape toward the deck to insert)
        if (alreadyGrabbing) {
            spdlog::debug("[PIPBOY] Removal skipped — hand already holding something");
            return;
        }

        // Player is gripping near the open tape deck — remove the holotape!
        // If deck was in Pushing state (hand close to deck triggered push tracking),
        // return to Open so the deck stays open for the player to see the removal.
        if (_tapeDeckState == TapeDeckState::Pushing) {
            _tapeDeckState = TapeDeckState::Open;
            _tapeDeckAnimProgress = 1.0f;
            _tapeDeckOpen = true;
        }
        // CheckHandPush runs before this removal check. Discard any active
        // physical push stroke and hold the tray open while QueueDropToHand
        // transfers ownership to the extracting hand.  Otherwise _holotapeLoaded becomes
        // false one frame before GrabManager reports the tape as held, so the same
        // overlapping hand starts a fresh push and closes the tray.
        _deckPushStrokeValid = false;
        _deckPushStrokeMaxTravel = 0.0f;
        _deckRemovalHandGuard = TAPE_GRAB_COOLDOWN;
        StopIntroPlayback();  // Cancel intro audio if playing
        RestoreIntroHolotapeType(_introHolotapeFormID);
        spdlog::debug("[PIPBOY] Player grabbing holotape out of deck! FormID={:08X} fingerDist={:.2f} controllerDist={:.1f} alreadyGrabbing={}",
            _loadedHolotapeFormID, fingerDistance, controllerDistance, alreadyGrabbing);

        std::uint32_t holotapeFormID = _loadedHolotapeFormID;

        // Tape removed — restore projected mode if we overrode it for playback.
        EndWristOverrideForHolotape();

        // Hide TapREF and clear loaded state
        _holotapeLoaded = false;
        _loadedHolotapeFormID = 0;
        _holotapeGrabCooldown = TAPE_GRAB_COOLDOWN;
        _holotapeRemovalRequiresGripRelease = false;
        _tapREFForceHidden = true;   // Persistent flag — overrides per-frame visibility
        _insertionOpenHandActive = false;  // Player is now holding the holotape — let grab system handle curls
        SetTapREFVisible(false);

        // Drop the holotape from inventory to the grabbing hand (primary wand).
        // Skip if the grab system already picked up an object this frame —
        // the player already has something in hand, DropToHand would conflict.
        if (!alreadyGrabbing) {
            DropToHand::GetSingleton().QueueDropToHand(
                holotapeFormID, grabHandIsLeft, 1, false, false, true);
        } else {
            spdlog::debug("[PIPBOY] Skipping DropToHand — grab system already holding an object");
        }

        // Haptic feedback on the grabbing hand
        VRInput::GetSingleton().TriggerHaptic(grabHandIsLeft, 3000);

        // Play holotape eject sound
        PlayWavSound("take out holotape.wav");

        spdlog::debug("[PIPBOY] Holotape {:08X} {} {} hand", holotapeFormID,
            alreadyGrabbing ? "deck cleared for" : "queued to",
            grabHandIsLeft ? "left" : "right");
    }

    // ════════════════════════════════════════════════════════════════════════
    // Holotape insertion — detect held holotape near open tape deck
    // When the player brings a grabbed holotape close to the open deck,
    // release the grab, add it to inventory, show TapREF, and close deck.
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::CheckHolotapeInsertion()
    {
        // Only allow insertion when deck is open (or opening) and no holotape is loaded
        if (_holotapeLoaded) return;
        if (_tapeDeckState != TapeDeckState::Open && _tapeDeckState != TapeDeckState::Opening) return;
        if (_holotapeGrabCooldown > 0.0f) return;  // Prevent immediate re-insertion after removal

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        // Don't allow insertion if TapREF is visible (holotape already in deck)
        RE::NiAVObject* tapREF = _cachedTapeRef ? _cachedTapeRef : f4vr::findAVObject(arm, "TapREF");
        if (tapREF && tapREF->local.scale > 0.01f) return;

        RE::NiAVObject* tapeDeck = GetCachedTapeDeckNode();
        if (!tapeDeck) return;

        auto& grabMgr = GrabManager::GetSingleton();

        // Check both hands for a held holotape
        for (int hand = 0; hand < 2; hand++) {
            bool isLeft = (hand == 0);
            if (!grabMgr.IsGrabbing(isLeft)) continue;

            auto& state = grabMgr.GetGrabState(isLeft);
            RE::TESObjectREFR* refr = state.GetRefr();
            if (!refr) continue;

            auto* baseObj = refr->GetObjectReference();
            if (!baseObj || baseObj->GetFormType() != RE::ENUM_FORM_ID::kNOTE) continue;

            // It's a holotape — check distance to tape deck
            if (!state.node) {
                spdlog::debug("[PIPBOY] Insertion: holotape {:08X} in {} hand has null node — skipping",
                    baseObj->GetFormID(), isLeft ? "left" : "right");
                continue;
            }
            RE::NiPoint3 objPos = state.node->world.translate;
            RE::NiPoint3 slotPos = tapeDeck->world.translate;
            RE::NiPoint3 diff = objPos - slotPos;
            float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

            if (distance > TAPE_INSERT_RADIUS) {
                // Log distance periodically (every ~30 frames)
                static int s_insertLogCounter = 0;
                if (++s_insertLogCounter % 30 == 0) {
                    spdlog::debug("[PIPBOY] Insertion: holotape dist={:.1f} > radius={:.1f} (obj=({:.0f},{:.0f},{:.0f}) deck=({:.0f},{:.0f},{:.0f}))",
                        distance, TAPE_INSERT_RADIUS,
                        objPos.x, objPos.y, objPos.z, slotPos.x, slotPos.y, slotPos.z);
                }
                continue;
            }

            // Holotape is close to the open deck — insert it!
            std::uint32_t formID = baseObj->GetFormID();
            std::string name(RE::TESFullName::GetFullName(*baseObj, false));

            spdlog::info("[PIPBOY] Holotape '{}' ({:08X}) near deck dist={:.1f} — inserting!",
                name.empty() ? "unknown" : name, formID, distance);

            // Release the grab (forStorage=true so physics cleanup is minimal)
            grabMgr.EndGrab(isLeft, nullptr, true);

            // Defer Disable() by a few frames — Inventory3DManager may have an async
            // 3D load task for this ref.  Disabling immediately can race with
            // FinishItemLoadTask → TESObjectREFR::GetOnLocalMap(null) crash.
            _deferredDisableHandle = refr->GetHandle();
            _deferredDisableFrames = 5;

            // Add holotape to inventory NOW so it's available for removal via DropToHand
            // at any time (before or after closing the deck).  Playback is deferred to close.
            {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                    heisenberg::AddObjectToContainer(player, static_cast<RE::TESBoundObject*>(baseObj),
                                                     &nullExtra, 1, nullptr, 0);
                    spdlog::debug("[PIPBOY] Added holotape {:08X} to inventory at insertion", formID);
                }
            }

            // Mark holotape as loaded and show TapREF
            _holotapeLoaded = true;
            _loadedHolotapeFormID = formID;
            _tapREFForceHidden = false;  // Clear force-hide from previous removal
            _holotapeGrabCooldown = 0.5f;   // Insertion grace only
            _holotapeRemovalCooldown = 0.25f; // Prevent the still-held insertion grip from taking it straight back out
            _holotapeRemovalRequiresGripRelease = true; // The insertion press is consumed until Grip is physically released
            _slamCooldown = 0.0f;            // Grip-release gate prevents accidental removal; do not discard a fast close sweep
            _ejectCooldown = 1.0f;  // Prevent eject button from immediately closing deck after insertion
            _deckOpenedByEject = false;  // Insertion — hand stays open for push-close

            // Keep deck open — player must close it (eject button or slam gesture)

            // Haptic feedback on both hands
            VRInput::GetSingleton().TriggerHaptic(true, 3000);
            VRInput::GetSingleton().TriggerHaptic(false, 3000);

            // Force hand fully open after insertion — keep open until deck closes
            // (player will push deck closed next)
            _insertionOpenHandActive = true;
            _insertionOpenHandIsLeft = isLeft;
            auto& frik = FRIKInterface::GetSingleton();
            if (frik.IsAvailable()) {
                frik.SetHandPoseFingerPositions(isLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            }

            PlayWavSound("Holotape place inside tray.wav");
            spdlog::info("[PIPBOY] Holotape {:08X} inserted, deck stays open", formID);
            return;  // Only one insertion per frame
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Holotape finger pose — when holding a holotape and approaching the Pipboy,
    // extend only the index finger (pointing) while keeping other fingers at
    // their grab curl values (so the hand still looks like it's gripping the tape).
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::UpdateHolotapeFingerPose()
    {
        auto& grabMgr = GrabManager::GetSingleton();
        auto& frik = FRIKInterface::GetSingleton();
        if (!frik.IsAvailable()) return;

        // When deck is open/opening:
        // - Holding a holotape: never touch pose (grab system handles curls)
        // - Empty hand + holotape inside deck: holotape holding pose when near
        // - Empty hand + no holotape: fully open hand when near
        if (_tapeDeckState == TapeDeckState::Open || _tapeDeckState == TapeDeckState::Opening) {
            // Clear any pointing override left from when the deck was closed
            if (_holotapeFingerPoseActive) {
                bool isLeft = _holotapeFingerPoseIsLeft;
                // If hand is holding something, restore grab curls instead of clearing to default
                if (grabMgr.IsGrabbing(isLeft)) {
                    const auto& state = grabMgr.GetGrabState(isLeft);
                    if (state.hasRuntimeFingerCurls) {
                        frik.SetHandPoseFingerPositions(isLeft,
                            state.runtimeThumbCurl, state.runtimeIndexCurl,
                            state.runtimeMiddleCurl, state.runtimeRingCurl, state.runtimePinkyCurl);
                    } else {
                        float curl = Heisenberg::GetSingleton().GetFingerCurlValue(isLeft);
                        frik.SetHandPoseFingerPositions(isLeft, curl, curl, curl, curl, curl);
                    }
                    spdlog::debug("[PIPBOY] Deck opened: restored grab curls for {} hand", isLeft ? "left" : "right");
                } else {
                    frik.ClearHandPoseFingerPositions(isLeft);
                }
                _holotapeFingerPoseActive = false;
            }
            // Determine the interacting hand (non-pipboy hand = right in normal mode)
            bool isLeftHanded = VRInput::GetSingleton().IsLeftHandedMode();
            bool interactHandIsLeft = isLeftHanded;

            // If the interacting hand is already holding something, don't touch its pose
            // (grab system handles finger curls — must check before _insertionOpenHandActive)
            if (grabMgr.IsGrabbing(interactHandIsLeft)) {
                return;
            }

            // After insertion: keep hand fully open (player will push deck closed)
            if (_insertionOpenHandActive) {
                frik.SetHandPoseFingerPositions(_insertionOpenHandIsLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
                return;
            }

            // When holotape is loaded, measure distance to TapREF (actual holotape mesh)
            // Otherwise measure to eject button / tape deck node
            RE::NiPoint3 targetPos;
            if (_holotapeLoaded && _cachedTapeRef) {
                targetPos = _cachedTapeRef->world.translate;
            } else {
                RE::NiAVObject* tapeDeck2 = GetCachedTapeDeckNode();
                RE::NiAVObject* ejectBtn2 = _cachedEjectButton;
                if (!tapeDeck2 && !ejectBtn2) return;
                targetPos = ejectBtn2 ? ejectBtn2->world.translate : tapeDeck2->world.translate;
            }

            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            if (!playerNodes) return;
            auto* wand = isLeftHanded ? playerNodes->SecondaryWandNode : playerNodes->primaryWandNode;
            if (!wand) return;

            RE::NiPoint3 diff = wand->world.translate - targetPos;
            float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

            constexpr float POSE_START_DIST = 20.0f;
            constexpr float POSE_FULL_DIST  = 10.0f;
            constexpr float HOLOTAPE_CURL   = 0.73f;
            if (dist > POSE_START_DIST) return;  // Too far — don't touch pose

            if (_deckOpenedByEject && _holotapeLoaded) {
                // Eject opened deck — pose hand for holotape removal (grip curl)
                float blend = std::clamp((POSE_START_DIST - dist) / (POSE_START_DIST - POSE_FULL_DIST), 0.0f, 1.0f);
                float curl = 1.0f - blend * (1.0f - HOLOTAPE_CURL);
                frik.SetHandPoseFingerPositions(interactHandIsLeft, curl, curl, curl, curl, curl);
            } else {
                // Insertion/ceremony — hand stays fully open (player is pushing deck closed)
                frik.SetHandPoseFingerPositions(interactHandIsLeft, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            return;
        }

        RE::NiAVObject* tapeDeck = GetCachedTapeDeckNode();
        RE::NiAVObject* ejectButton = _cachedEjectButton;
        // Need at least one reference point on the Pipboy
        if (!tapeDeck && !ejectButton) return;

        // Use eject button position if available (more precise), otherwise tape deck
        RE::NiPoint3 pipboyTargetPos = ejectButton ? ejectButton->world.translate : tapeDeck->world.translate;

        // Check if either hand is holding a holotape
        bool foundHolotape = false;
        for (int hand = 0; hand < 2; hand++) {
            bool isLeft = (hand == 0);
            if (!grabMgr.IsGrabbing(isLeft)) continue;

            const auto& state = grabMgr.GetGrabState(isLeft);
            RE::TESObjectREFR* refr = state.GetRefr();
            if (!refr) continue;

            auto* baseObj = refr->GetObjectReference();
            if (!baseObj || baseObj->GetFormType() != RE::ENUM_FORM_ID::kNOTE) continue;

            // Found a held holotape — check distance to Pipboy
            if (!state.node) continue;
            RE::NiPoint3 diff = state.node->world.translate - pipboyTargetPos;
            float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

            if (distance <= TAPE_POINTING_RANGE && _tapeDeckState == TapeDeckState::Closed) {
                foundHolotape = true;

                // Get grab curl values — keep all fingers except index at their grip positions
                float thumb, middle, ring, pinky;
                if (state.hasRuntimeFingerCurls) {
                    thumb  = state.runtimeThumbCurl;
                    middle = state.runtimeMiddleCurl;
                    ring   = state.runtimeRingCurl;
                    pinky  = state.runtimePinkyCurl;
                } else {
                    float curl = Heisenberg::GetSingleton().GetFingerCurlValue(isLeft);
                    thumb = middle = ring = pinky = curl;
                }

                // Override only the index finger to extended (pointing at deck)
                frik.SetHandPoseFingerPositions(isLeft, thumb, 1.0f, middle, ring, pinky);

                if (!_holotapeFingerPoseActive || _holotapeFingerPoseIsLeft != isLeft) {
                    _holotapeFingerPoseActive = true;
                    _holotapeFingerPoseIsLeft = isLeft;
                    spdlog::debug("[PIPBOY] Holotape finger pose: index extended, dist={:.1f}", distance);
                }
                break;  // Only one holotape at a time
            }
        }

        // If no holotape is near the Pipboy but pose was active, restore original grab curls
        if (!foundHolotape && _holotapeFingerPoseActive) {
            bool isLeft = _holotapeFingerPoseIsLeft;
            const auto& state = grabMgr.GetGrabState(isLeft);
            if (grabMgr.IsGrabbing(isLeft)) {
                // Restore full grab finger curls (including index)
                if (state.hasRuntimeFingerCurls) {
                    frik.SetHandPoseFingerPositions(isLeft,
                        state.runtimeThumbCurl, state.runtimeIndexCurl,
                        state.runtimeMiddleCurl, state.runtimeRingCurl, state.runtimePinkyCurl);
                } else {
                    float curl = Heisenberg::GetSingleton().GetFingerCurlValue(isLeft);
                    frik.SetHandPoseFingerPositions(isLeft, curl, curl, curl, curl, curl);
                }
                spdlog::debug("[PIPBOY] Holotape finger pose: restored grab curls");
            }
            _holotapeFingerPoseActive = false;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // ════════════════════════════════════════════════════════════════════════
    // Hand push / slam close — swept contact against the visible tray mesh.
    // A touch begins the short close animation. The old pivot-distance mapping required
    // the fingertip to enter a tiny sphere at TapeDeck01's origin and could both trigger
    // beside the tray and tunnel through it during fast motion.
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::CheckHandPush()
    {
        rock::performance_profiler::ScopedTimer profilerTimer(
            rock::performance_profiler::Scope::PipboyDeckContactQuery);

        // Accept a fast closing swipe near the end of the opening animation, but
        // not on the same frame as the eject-button press. Without this threshold
        // the hand pressing the adjacent button could immediately reverse the tray.
        const bool openingPushWindow =
            _tapeDeckState == TapeDeckState::Opening &&
            _tapeDeckAnimProgress >= 0.75f;
        if (_tapeDeckState != TapeDeckState::Open &&
            _tapeDeckState != TapeDeckState::Pushing &&
            !openingPushWindow) {
            return;
        }

        // Deck just opened: refuse push-close until the lockout expires. The animation
        // threshold above is not enough on its own — the pressing finger is still parked on
        // the eject button when the tray reaches 75%, so the deck opened and immediately shut
        // in a single press. Time is the right unit here because the blocking condition is
        // "the hand has not withdrawn yet", which has nothing to do with tray travel.
        if (_deckPushCloseLockout > 0.0f) {
            return;
        }

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        RE::NiAVObject* tapeDeck = GetCachedTapeDeckNode();
        if (!tapeDeck) return;

        if (!_cachedTapeDeckMesh1 ||
            !CachedNodeIsLiveWithin(
                tapeDeck,
                _cachedTapeDeckMesh1)) {
            _cachedTapeDeckMesh1 = SafeFindAVObject(tapeDeck, "TapeDeck01_mesh:1");
        }
        RE::NiAVObject* deckMesh = _cachedTapeDeckMesh1;

        // TapeDeckLid_mesh:1 is a separately animated visible half of the open
        // mechanism. The old contact query only included TapeDeck01_mesh:1, so
        // pushing the lid was guaranteed to pass through unless the same hand
        // hull happened to reach the tray too.
        if (_cachedTapeDeckLid &&
            !CachedNodeIsLiveWithin(
                arm,
                _cachedTapeDeckLid)) {
            _cachedTapeDeckLid = nullptr;
            _cachedTapeDeckLidMesh1 = nullptr;
            // UpdateTapeDeckAnimation normally clears this sibling cache when
            // it observes the stale lid root. CheckHandPush can now repair the
            // lid first, which would hide that invalidation from the later
            // animation pass and leave an old TapREF pointer eligible for a
            // scale write. Preserve the cache-family invariant here too.
            _cachedTapeRef = nullptr;
        }
        if (!_cachedTapeDeckLid) {
            _cachedTapeDeckLid =
                SafeFindAVObject(arm, "TapeDeckLid_mesh");
        }
        if (_cachedTapeDeckLid &&
            (!_cachedTapeDeckLidMesh1 ||
             !CachedNodeIsLiveWithin(
                 _cachedTapeDeckLid,
                 _cachedTapeDeckLidMesh1))) {
            _cachedTapeDeckLidMesh1 =
                SafeFindAVObject(
                    _cachedTapeDeckLid,
                    "TapeDeckLid_mesh:1");
        }
        RE::NiAVObject* deckLidMesh =
            _cachedTapeDeckLidMesh1;

        // The pushing hand is the non-pipboy hand. Pipboy always on left wrist,
        // so push hand is always right.
        constexpr bool pushHandIsLeft = false;

        // Don't push if hand is actively grabbing something.
        // Use GrabManager (authoritative) rather than Hand::IsHolding() because
        // EndGrab-for-storage clears GrabState but doesn't reset Hand._state,
        // which would leave IsHolding() stuck at true after holotape insertion.
        auto& grabMgr = GrabManager::GetSingleton();
        const bool handOccupied = grabMgr.IsGrabbing(pushHandIsLeft);

        // Don't push when grip is pressed and a holotape is loaded — user wants removal, not push-close.
        // CheckHandPush runs before CheckHolotapeRemoval, so without this guard the push can
        // commit to Closing before removal gets a chance to fire.
        const bool removalIntent = _holotapeLoaded
            && VRInput::GetSingleton().IsPressed(pushHandIsLeft, VRButton::Grip);
        const bool removalTransferInProgress = _deckRemovalHandGuard > 0.0f;

        struct DeckHullContact
        {
            bool valid = false;
            float distance = (std::numeric_limits<float>::max)();
            float effectiveRadius = 0.0f;
            float penetration = -(std::numeric_limits<float>::max)();
            RE::NiPoint3 sample{};
            RE::NiPoint3 closest{};
            const char* source = "none";
            const char* surface = "none";
            RE::NiPoint3 hinge{};
            float openAngleRadians =
                TAPE_DECK_OPEN_ANGLE *
                3.14159265358979323846f / 180.0f;
            bool swept = false;
        };

        /*
         * The tray and lid are animated visual meshes rather than independently
         * simulated rigid body, so native Havok cannot deliver a body/body
         * contact callback for it. Sample the boundaries of ROCK's exact live
         * generated collision hulls instead. This makes the deck react to the
         * palm/finger boxes and to the equipped weapon collision, rather than
         * to one hidden index-bone point.
         */
        // The exported points are vertices/rings from ROCK's continuous convex
        // hulls, not a watertight triangle representation of those hulls. A
        // small skin closes the gaps between samples. Keep it well below the old
        // 1.0-gu bubble (which visibly triggered early); 0.5 gu matches ROCK's
        // normal world-contact skin and catches the observed 0.59-gu visible
        // touch with a 0.10-gu convex radius.
        constexpr float generatedHullContactSkin = 0.5f;

        RE::NiPoint3 handFrameDelta{};
        bool handSweepValid = false;
        if (_interactionFingerValid && _previousInteractionFingerValid) {
            handFrameDelta =
                _interactionFingerPos - _previousInteractionFingerPos;
            const float handTravel =
                PointDistance(
                    _interactionFingerPos,
                    _previousInteractionFingerPos);
            handSweepValid =
                std::isfinite(handTravel) &&
                handTravel > 1.0e-3f &&
                handTravel <= 120.0f;
        }

        constexpr std::uint32_t kMaxHandSamples = 384;
        constexpr std::uint32_t kMaxWeaponSamples = 512;
        // HostCopy* initializes exactly the returned prefix. Avoid zeroing about
        // 14 KiB of stack scratch on every open-deck frame before overwriting it.
        std::array<RE::NiPoint3, kMaxHandSamples> handPoints;
        std::array<float, kMaxHandSamples> handRadii;
        std::array<RE::NiPoint3, kMaxWeaponSamples> weaponPoints;
        std::array<float, kMaxWeaponSamples> weaponRadii;
        const std::uint32_t handSampleCount = rock::HostCopyHandCollisionSamples(
            pushHandIsLeft,
            handPoints.data(),
            handRadii.data(),
            kMaxHandSamples);
        const std::uint32_t weaponSampleCount = rock::HostCopyWeaponCollisionSamples(
            weaponPoints.data(),
            weaponRadii.data(),
            kMaxWeaponSamples);

        const bool globallyBlocked =
            _slamCooldown > 0.0f || removalIntent || removalTransferInProgress;
        const bool handContactEligible = !globallyBlocked && !handOccupied;
        const bool weaponContactEligible = !globallyBlocked;

        /*
         * PERF BROADPHASE:
         *
         * GetTriangles transforms every vertex into world space, then the exact
         * query can test hundreds of hand/weapon samples against every triangle.
         * Previously that work ran every open-deck frame even when both hands
         * were across the room. Cache only the transform-invariant radius
         * measured by the previous exact query and first ask whether a current
         * sample (or its validated hand sweep) can reach that surface.
         *
         * A new node/scale uses the exact query's maximum possible 48-gu radius
         * for its first gate, so this optimization cannot reject a contact the
         * old broadphase would admit. Rotation and translation do not change a
         * mesh's radius. Pushing is deliberately not special-cased: when no
         * sample can reach either surface the contacts remain invalid, allowing
         * the existing release/reopen path below to run on the same frame.
         */
        struct DeckSurfaceBroadphaseCache
        {
            const RE::NiAVObject* mesh = nullptr;
            float scale = 1.0f;
            float radius = 48.0f;
            bool valid = false;
        };
        static std::array<DeckSurfaceBroadphaseCache, 2>
            surfaceBroadphaseCache{};

        const std::array<RE::NiAVObject*, 2> surfaceMeshes = {
            deckMesh,
            deckLidMesh
        };
        auto cachedSurfaceRadius =
            [&](std::size_t surfaceIndex) {
                const auto* mesh = surfaceMeshes[surfaceIndex];
                auto& cache = surfaceBroadphaseCache[surfaceIndex];
                if (!mesh) {
                    cache = {};
                    return 0.0f;
                }

                const float scale = std::fabs(mesh->world.scale);
                if (!cache.valid ||
                    cache.mesh != mesh ||
                    !std::isfinite(scale) ||
                    std::fabs(cache.scale - scale) > 1.0e-4f) {
                    // makeSurface clamps its exact broadphase to 48 gu.
                    return 48.0f;
                }
                return cache.radius;
            };

        auto sampleCloudMayReachSurface =
            [&](std::size_t surfaceIndex,
                const RE::NiPoint3* points,
                const float* radii,
                std::uint32_t count,
                float contactSkin,
                bool includeHandSweep) {
                const auto* mesh = surfaceMeshes[surfaceIndex];
                if (!mesh || count == 0) {
                    return false;
                }

                const RE::NiPoint3 center = mesh->world.translate;
                if (!IsUsableWorldPoint(center)) {
                    // Preserve the old exact-query behavior for malformed
                    // transforms instead of letting a prefilter decide it.
                    return true;
                }
                const float surfaceRadius =
                    cachedSurfaceRadius(surfaceIndex);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const RE::NiPoint3& sample = points[i];
                    if (!IsUsableWorldPoint(sample)) {
                        continue;
                    }
                    const float effectiveRadius =
                        pipboy_mesh_contact::GeneratedDeckContactRadius(
                            radii[i],
                            contactSkin);
                    const float reach =
                        surfaceRadius + effectiveRadius;
                    if (PointDistance(sample, center) <= reach) {
                        return true;
                    }

                    if (!includeHandSweep || !handSweepValid) {
                        continue;
                    }
                    const RE::NiPoint3 previousSample =
                        sample - handFrameDelta;
                    if (IsUsableWorldPoint(previousSample) &&
                        PointToSegmentDistance(
                            center,
                            previousSample,
                            sample) <= reach) {
                        return true;
                    }
                }
                return false;
            };

        std::array<bool, 2> surfaceQueryNeeded{};
        for (std::size_t surfaceIndex = 0;
             surfaceIndex < surfaceMeshes.size();
             ++surfaceIndex) {
            if (!surfaceMeshes[surfaceIndex]) {
                continue;
            }

            if (handContactEligible) {
                if (handSampleCount > 0) {
                    surfaceQueryNeeded[surfaceIndex] =
                        sampleCloudMayReachSurface(
                            surfaceIndex,
                            handPoints.data(),
                            handRadii.data(),
                            handSampleCount,
                            generatedHullContactSkin,
                            true);
                } else if (_interactionFingerValid) {
                    const float fingertipRadius =
                        (std::clamp)(
                            g_config.tapeDeckPushCloseRadius,
                            0.3f,
                            8.0f);
                    surfaceQueryNeeded[surfaceIndex] =
                        sampleCloudMayReachSurface(
                            surfaceIndex,
                            &_interactionFingerPos,
                            &fingertipRadius,
                            1,
                            0.0f,
                            true);
                }
            }

            if (!surfaceQueryNeeded[surfaceIndex] &&
                weaponContactEligible &&
                weaponSampleCount > 0) {
                surfaceQueryNeeded[surfaceIndex] =
                    sampleCloudMayReachSurface(
                        surfaceIndex,
                        weaponPoints.data(),
                        weaponRadii.data(),
                        weaponSampleCount,
                        generatedHullContactSkin,
                        false);
            }
        }

        static std::vector<TriangleData> deckTriangles;
        static std::vector<TriangleData> lidTriangles;
        deckTriangles.clear();
        lidTriangles.clear();
        if (deckMesh && surfaceQueryNeeded[0]) {
            deckTriangles.reserve(256);
            GetTriangles(deckMesh, deckTriangles, 2048);
        }
        if (deckLidMesh && surfaceQueryNeeded[1]) {
            lidTriangles.reserve(256);
            GetTriangles(
                deckLidMesh,
                lidTriangles,
                2048);
        }

        struct DeckContactSurface
        {
            const std::vector<TriangleData>* triangles =
                nullptr;
            RE::NiPoint3 center{};
            RE::NiPoint3 outward{};
            float broadphaseRadius = 24.0f;
            float openAngleRadians = 0.0f;
            const char* name = "none";
        };

        auto makeSurface = [&](std::size_t surfaceIndex,
                               RE::NiAVObject* mesh,
                               const std::vector<TriangleData>& triangles,
                               float openAngleDegrees,
                               const char* name) {
            DeckContactSurface surface;
            surface.triangles = &triangles;
            surface.name = name;
            surface.openAngleRadians =
                openAngleDegrees *
                3.14159265358979323846f / 180.0f;
            if (!mesh) {
                return surface;
            }

            surface.center = mesh->world.translate;
            surface.outward = PipboyRotateDirection(
                mesh->world.rotate,
                RE::NiPoint3(0.0f, 0.0f, 1.0f));
            float meshRadius = 0.0f;
            for (const auto& triangle : triangles) {
                meshRadius = (std::max)(
                    meshRadius,
                    PointDistance(surface.center, triangle.v0));
                meshRadius = (std::max)(
                    meshRadius,
                    PointDistance(surface.center, triangle.v1));
                meshRadius = (std::max)(
                    meshRadius,
                    PointDistance(surface.center, triangle.v2));
            }
            if (std::isfinite(meshRadius) &&
                meshRadius > 0.0f) {
                surface.broadphaseRadius =
                    (std::clamp)(
                        meshRadius + 8.0f,
                        12.0f,
                        48.0f);
            }
            if (!triangles.empty()) {
                auto& cache =
                    surfaceBroadphaseCache[surfaceIndex];
                cache.mesh = mesh;
                cache.scale = std::fabs(mesh->world.scale);
                cache.radius = surface.broadphaseRadius;
                cache.valid =
                    std::isfinite(cache.scale) &&
                    std::isfinite(cache.radius);
            }
            return surface;
        };

        const std::array<DeckContactSurface, 2>
            contactSurfaces = {
                makeSurface(
                    0,
                    deckMesh,
                    deckTriangles,
                    TAPE_DECK_OPEN_ANGLE,
                    "tray"),
                makeSurface(
                    1,
                    deckLidMesh,
                    lidTriangles,
                    TAPE_LID_OPEN_ANGLE,
                    "lid")
            };
        const bool deckMeshAvailable =
            !deckTriangles.empty() ||
            !lidTriangles.empty();

        auto evaluateSamples = [&](const RE::NiPoint3* points,
                                   const float* radii,
                                   std::uint32_t count,
                                   const char* source,
                                   const char* sweptSource,
                                   bool sweepWithHand,
                                   float contactSkin) {
            DeckHullContact best{};
            best.source = source;
            if (!deckMeshAvailable) {
                return best;
            }

            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& sample = points[i];
                if (!IsUsableWorldPoint(sample)) {
                    continue;
                }
                const float effectiveRadius =
                    pipboy_mesh_contact::GeneratedDeckContactRadius(
                        radii[i],
                        contactSkin);
                const RE::NiPoint3 previousSample =
                    sample - handFrameDelta;

                for (const auto& surface :
                     contactSurfaces) {
                    if (!surface.triangles ||
                        surface.triangles->empty()) {
                        continue;
                    }

                    // Current-frame overlap remains the normal slow/steady
                    // contact path. Use each moving surface's own center and
                    // local +Z face; world-Z admission failed as soon as the
                    // player rotated their Pip-Boy wrist.
                    if (PointDistance(
                            sample,
                            surface.center) <=
                        surface.broadphaseRadius +
                            effectiveRadius) {
                        RE::NiPoint3 closest{};
                        float distance =
                            (std::numeric_limits<float>::max)();
                        if (GetClosestMeshPointToPoint(
                                *surface.triangles,
                                sample,
                                closest,
                                distance)) {
                            const RE::NiPoint3 toCollider =
                                sample - closest;
                            const float directionLength =
                                PointDistance(
                                    sample,
                                    closest);
                            const bool contactOnClosingFace =
                                directionLength <= 1.0e-3f
                                    ? _tapeDeckState ==
                                          TapeDeckState::Pushing
                                    : PipboyDotProduct(
                                          toCollider,
                                          surface.outward) >
                                          0.15f *
                                              directionLength;
                            const float penetration =
                                effectiveRadius -
                                distance;
                            if (contactOnClosingFace &&
                                (!best.valid ||
                                 penetration >
                                     best.penetration)) {
                                best.valid = true;
                                best.distance = distance;
                                best.effectiveRadius =
                                    effectiveRadius;
                                best.penetration =
                                    penetration;
                                best.sample = sample;
                                best.closest = closest;
                                best.source = source;
                                best.surface =
                                    surface.name;
                                best.hinge =
                                    surface.center;
                                best.openAngleRadians =
                                    surface.
                                        openAngleRadians;
                                best.swept = false;
                            }
                        }
                    }

                    if (!sweepWithHand ||
                        !handSweepValid ||
                        !IsUsableWorldPoint(
                            previousSample) ||
                        PointToSegmentDistance(
                            surface.center,
                            previousSample,
                            sample) >
                            surface.broadphaseRadius +
                                effectiveRadius) {
                        continue;
                    }

                    // Translate the live hull boundary sample back by the
                    // tracked hand motion and test its continuous path against
                    // both visible mechanism meshes.
                    RE::NiPoint3 intersection{};
                    if (!FindFirstSegmentMeshIntersection(
                            *surface.triangles,
                            previousSample,
                            sample,
                            intersection)) {
                        continue;
                    }

                    const RE::NiPoint3 approach =
                        previousSample - intersection;
                    const float approachLength =
                        PointDistance(
                            previousSample,
                            intersection);
                    const float approachFromOutside =
                        PipboyDotProduct(
                            approach,
                            surface.outward);
                    const float motionTowardSurface =
                        PipboyDotProduct(
                            sample - previousSample,
                            surface.outward);
                    const bool sweptFromClosingFace =
                        motionTowardSurface < 0.0f &&
                        (approachLength <= 1.0e-3f ||
                         approachFromOutside >
                             0.15f *
                                 approachLength);
                    if (!sweptFromClosingFace) {
                        continue;
                    }

                    // Preserve how far the validated sweep ended beyond the
                    // surface. Treating every crossing as only one 0.10-gu
                    // convex radius let a fast hand emerge behind the lid
                    // after advancing it by a tiny fixed step.
                    const float travelPastSurface =
                        (std::max)(
                            0.0f,
                            -PipboyDotProduct(
                                sample - intersection,
                                surface.outward));
                    const float sweptPenetration =
                        (std::max)(
                            effectiveRadius +
                                travelPastSurface,
                            0.01f);
                    if (!best.valid ||
                        sweptPenetration >
                            best.penetration) {
                        best.valid = true;
                        best.distance = 0.0f;
                        best.effectiveRadius =
                            effectiveRadius;
                        best.penetration =
                            sweptPenetration;
                        best.sample = intersection;
                        best.closest = intersection;
                        best.source = sweptSource;
                        best.surface = surface.name;
                        best.hinge = surface.center;
                        best.openAngleRadians =
                            surface.openAngleRadians;
                        best.swept = true;
                    }
                }
            }
            return best;
        };

        auto sampleCentroid = [](const RE::NiPoint3* points,
                                 std::uint32_t count,
                                 RE::NiPoint3& out) {
            out = {};
            std::uint32_t validCount = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (!IsUsableWorldPoint(points[i])) {
                    continue;
                }
                out = out + points[i];
                ++validCount;
            }
            if (validCount == 0) {
                return false;
            }
            out = out * (1.0f / static_cast<float>(validCount));
            return IsUsableWorldPoint(out);
        };

        RE::NiPoint3 handHullCentroid{};
        RE::NiPoint3 weaponHullCentroid{};
        const bool handHullCentroidValid =
            sampleCentroid(
                handPoints.data(),
                handSampleCount,
                handHullCentroid);
        const bool weaponHullCentroidValid =
            sampleCentroid(
                weaponPoints.data(),
                weaponSampleCount,
                weaponHullCentroid);

        DeckHullContact handContact{};
        if (handContactEligible) {
            handContact = evaluateSamples(
                handPoints.data(),
                handRadii.data(),
                handSampleCount,
                "hand hull",
                "hand hull sweep",
                true,
                generatedHullContactSkin);
        }
        DeckHullContact weaponContact{};
        if (weaponContactEligible) {
            weaponContact = evaluateSamples(
                weaponPoints.data(),
                weaponRadii.data(),
                weaponSampleCount,
                "weapon hull",
                "weapon hull sweep",
                false,
                generatedHullContactSkin);
        }

        // Compatibility fallback when generated hand collision is unavailable.
        // Retain the swept visible fingertip so old/fallback runtime
        // configurations can still close either visible mechanism mesh. Its
        // configured radius is already a complete compatibility envelope, so
        // do not add the generated-hull sample skin a second time.
        if (handContactEligible &&
            handSampleCount == 0 &&
            _interactionFingerValid) {
            const float fingertipRadius =
                (std::clamp)(g_config.tapeDeckPushCloseRadius, 0.3f, 8.0f);
            const std::array<RE::NiPoint3, 1>
                fallbackPoints = {
                    _interactionFingerPos
                };
            const std::array<float, 1>
                fallbackRadii = {
                    fingertipRadius
                };
            handContact = evaluateSamples(
                fallbackPoints.data(),
                fallbackRadii.data(),
                1,
                "fingertip fallback",
                "fingertip sweep",
                true,
                0.0f);
        }

        if (_tapeDeckState == TapeDeckState::Pushing && std::isnan(_pushProgress)) {
            _pushProgress = 1.0f;
            _tapeDeckState = TapeDeckState::Open;
            _deckPushStrokeValid = false;
            _deckPushStrokeMaxTravel = 0.0f;
        }

        constexpr float kPushingReleaseHysteresis = 0.3f;
        const auto contactContinuesPush =
            [&](const DeckHullContact& contact) {
                return contact.valid &&
                    pipboy_mesh_contact::
                        TapeDeckContactKeepsPushing(
                            contact.penetration,
                            contact.swept,
                            _tapeDeckState ==
                                TapeDeckState::Pushing,
                            kPushingReleaseHysteresis);
            };
        const bool handPushingNow =
            !globallyBlocked && !handOccupied &&
            contactContinuesPush(handContact);
        // An equipped weapon is not a GrabManager object. It remains a valid
        // physical pusher even while the right hand is otherwise "occupied".
        const bool weaponPushingNow =
            !globallyBlocked &&
            contactContinuesPush(weaponContact);
        const bool pushingNow = handPushingNow || weaponPushingNow;
        const bool activeUsesWeapon =
            weaponPushingNow &&
            (!handPushingNow ||
             weaponContact.penetration > handContact.penetration);
        const DeckHullContact& activeContact =
            activeUsesWeapon
                ? weaponContact
                : handContact;

        auto getStrokeAnchor = [&](bool useWeapon, RE::NiPoint3& out) {
            if (useWeapon) {
                if (!weaponHullCentroidValid) {
                    return false;
                }
                out = weaponHullCentroid;
                return true;
            }
            if (_interactionFingerValid) {
                out = _interactionFingerPos;
                return true;
            }
            if (handHullCentroidValid) {
                out = handHullCentroid;
                return true;
            }
            return false;
        };

        float strokeTravelForLog = 0.0f;
        float strokeLateralForLog = 0.0f;

        if ((_tapeDeckState == TapeDeckState::Open ||
             openingPushWindow ||
             (_tapeDeckState == TapeDeckState::Pushing &&
              !_deckPushStrokeValid)) &&
            pushingNow) {
            RE::NiPoint3 anchor{};
            if (!getStrokeAnchor(activeUsesWeapon, anchor)) {
                anchor = activeContact.sample;
            }

            RE::NiPoint3 closingDirection =
                activeContact.closest - activeContact.sample;
            float directionLength =
                PointDistance(activeContact.closest, activeContact.sample);
            // STROKE-DIRECTION FIX (Jul 25, user: "gradually close relative to my hand"):
            // sample→closest of the FIRST graze can point along the tray edge (a shallow
            // first touch), making later straight-down hand motion produce near-zero
            // axial travel — the tray stalled although the hand pushed correctly. When
            // the hand's own motion is known and agrees with the contact side (not
            // pulling away), use the HAND MOTION as the stroke axis: travel along it is
            // then literally "how far my hand pushed".
            if (!activeUsesWeapon && handSweepValid) {
                const float handTravelLen = PointDistance({}, handFrameDelta);
                if (handTravelLen > 1.0e-3f) {
                    const float agreement = directionLength > 1.0e-3f
                        ? PipboyDotProduct(handFrameDelta, closingDirection) /
                              (handTravelLen * directionLength)
                        : 1.0f;
                    if (agreement > 0.0f) {
                        closingDirection = handFrameDelta;
                        directionLength = handTravelLen;
                    }
                }
            }
            if (directionLength <= 1.0e-3f &&
                !activeUsesWeapon &&
                handSweepValid) {
                closingDirection = handFrameDelta;
                directionLength = PointDistance({}, handFrameDelta);
            }
            if (directionLength <= 1.0e-3f ||
                !std::isfinite(directionLength)) {
                // Compatibility fallback for an exact swept intersection where
                // closest==sample and visible fingertip tracking was unavailable.
                // Contact admission already required the pusher to be above the
                // tray, so downward world Z is the inward direction.
                closingDirection = RE::NiPoint3(0.0f, 0.0f, -1.0f);
                directionLength = 1.0f;
            }
            closingDirection =
                closingDirection * (1.0f / directionLength);

            _tapeDeckState = TapeDeckState::Pushing;
            _pushProgress = _tapeDeckAnimProgress;
            _deckPushStrokeValid = true;
            _deckPushStrokeUsesWeapon = activeUsesWeapon;
            _deckPushStrokeOrigin =
                activeContact.swept &&
                        !activeUsesWeapon &&
                        _previousInteractionFingerValid
                    ? _previousInteractionFingerPos
                    : anchor;
            _deckPushStrokeDirection = closingDirection;
            _deckPushStrokeStartProgress = _tapeDeckAnimProgress;
            _deckPushStrokeMaxTravel = 0.0f;

            const RE::NiPoint3 hinge =
                IsUsableWorldPoint(
                    activeContact.hinge)
                    ? activeContact.hinge
                    : tapeDeck->world.translate;
            const float contactRadiusFromHinge =
                PointDistance(activeContact.closest, hinge);
            const float openAngleRadians =
                std::isfinite(
                    activeContact.openAngleRadians) &&
                        activeContact.openAngleRadians >
                            1.0e-4f
                    ? activeContact.openAngleRadians
                    : TAPE_DECK_OPEN_ANGLE *
                          3.14159265358979323846f /
                          180.0f;
            _deckPushStrokeRequiredTravel =
                pipboy_mesh_contact::TapeDeckStrokeDistance(
                    contactRadiusFromHinge,
                    _deckPushStrokeStartProgress,
                    openAngleRadians);
            spdlog::debug(
                "[PIPBOY] Deck proportional push started by {} on {} "
                "(surfaceDist={:.2f}, hullRadius={:.2f}, "
                "startProgress={:.2f}, requiredTravel={:.2f})",
                activeContact.source,
                activeContact.surface,
                activeContact.distance,
                activeContact.effectiveRadius,
                _deckPushStrokeStartProgress,
                _deckPushStrokeRequiredTravel);
        }

        if (_tapeDeckState == TapeDeckState::Pushing) {
            RE::NiPoint3 anchor{};
            const bool anchorValid =
                _deckPushStrokeValid &&
                getStrokeAnchor(_deckPushStrokeUsesWeapon, anchor);

            float axialTravel = 0.0f;
            float lateralTravel = 0.0f;
            if (anchorValid) {
                const RE::NiPoint3 delta =
                    anchor - _deckPushStrokeOrigin;
                axialTravel =
                    PipboyDotProduct(
                        delta,
                        _deckPushStrokeDirection);
                const RE::NiPoint3 lateralDelta =
                    delta -
                    _deckPushStrokeDirection * axialTravel;
                lateralTravel = PointDistance({}, lateralDelta);
            }
            strokeTravelForLog = axialTravel;
            strokeLateralForLog = lateralTravel;

            /*
             * The visual tray retreats as it rotates, so direct overlap naturally
             * ends after first touch. Continue following the SAME stable hand or
             * weapon anchor instead of advancing by elapsed frames. More travel
             * along the captured inward direction means proportionally more
             * closure; holding still means exactly zero further motion.
             */
            // DEPENETRATION DRIVE (Jul 27, user: "it should smoothly close when the hand touches
            // it ... the hand is not allowed to penetrate the holotape deck and not close it").
            //
            // The old model integrated travel along a stroke axis captured at first touch, kept
            // the deepest value via a ratchet, and abandoned the stroke on lateral drift. Every
            // one of those could desynchronise the tray from the hand: a stroke captured along a
            // shallow graze, or broken by drift, left the tray still while the hand kept going —
            // i.e. the hand penetrated the deck without closing it.
            //
            // Replace it with the constraint the user actually described: the tray may never be
            // inside the pusher. Each frame, rotate the tray away by exactly the angle that
            // clears the CURRENT measured penetration (arc length -> angle at the contact's
            // radius from the hinge). This is self-correcting rather than integrated, so it needs
            // no anchor, no captured axis and no ratchet; it is inherently proportional to hand
            // speed (a fast hand penetrates further per frame and so rotates the tray further),
            // and it cannot be outrun, because any penetration is removed the frame it appears.
            const bool strokeReleased = globallyBlocked || !pushingNow;

            if (strokeReleased) {
                _deckPushStrokeValid = false;
                _deckPushStrokeMaxTravel = 0.0f;
                // RECAPTURE FIX (Jul 25, user: close feels unreliable): a broken stroke
                // (lateral drift past the tolerance, anchor loss) used to snap the tray
                // back toward fully open, and a continuing push could not re-engage until
                // the tray had re-opened past 75% — the deck visibly fought the player.
                // If the pusher is STILL in valid contact, stay in Pushing with the stroke
                // invalidated: the capture block above re-anchors a fresh stroke from the
                // current hand position next frame, so the tray just keeps following the
                // hand. Only spring back open when the contact is actually gone.
                if (pushingNow) {
                    _pushProgress = _tapeDeckAnimProgress;
                    spdlog::debug(
                        "[PIPBOY] Deck push stroke re-anchoring "
                        "(travel={:.2f}, lateral={:.2f}) — contact persists",
                        axialTravel,
                        lateralTravel);
                } else {
                    _tapeDeckOpen = true;
                    _tapeDeckState = TapeDeckState::Opening;
                    _pushProgress = _tapeDeckAnimProgress;
                    spdlog::debug(
                        "[PIPBOY] Deck proportional push released "
                        "(travel={:.2f}, lateral={:.2f}, blocked={}) — returning open",
                        axialTravel,
                        lateralTravel,
                        globallyBlocked);
                }
            } else if (activeContact.penetration > 0.0f) {
                const RE::NiPoint3 hingeNow =
                    IsUsableWorldPoint(
                        activeContact.hinge)
                        ? activeContact.hinge
                        : tapeDeck->world.translate;
                const float contactRadius =
                    PointDistance(activeContact.closest, hingeNow);
                const float openAngleRadiansNow =
                    std::isfinite(
                        activeContact.openAngleRadians) &&
                            activeContact.openAngleRadians >
                                1.0e-4f
                        ? activeContact.openAngleRadians
                        : TAPE_DECK_OPEN_ANGLE *
                              3.14159265358979323846f /
                              180.0f;
                // The policy helper owns the degenerate-hinge guard and the
                // distinction between capped slow overlap and a validated
                // sweep that must clear its measured crossing this frame.
                _pushProgress =
                    pipboy_mesh_contact::
                        TapeDeckProgressAfterPenetration(
                            _pushProgress,
                            activeContact.penetration,
                            contactRadius,
                            openAngleRadiansNow,
                            activeContact.swept,
                            0.20f);
                strokeTravelForLog = activeContact.penetration;
            }

            if (_tapeDeckState == TapeDeckState::Pushing &&
                pipboy_mesh_contact::TapeDeckReachedMechanicalLatch(
                    _pushProgress)) {
                _tapeDeckOpen = false;
                _tapeDeckState = TapeDeckState::Closing;
                _closeAnimSpeed = ANIM_SPEED;
                _slamCooldown = SLAM_COOLDOWN_TIME;
                _deckPushStrokeValid = false;
                PlayWavSound("Slam close.wav");
                VRInput::GetSingleton().TriggerHaptic(pushHandIsLeft, 5000);
                spdlog::info(
                    "[PIPBOY] Tape deck physically pushed closed by {} "
                    "(travel={:.2f}/{:.2f}, actual collider samples: "
                    "hand={}, weapon={})",
                    _deckPushStrokeUsesWeapon ? "weapon hull" : "hand hull",
                    _deckPushStrokeMaxTravel,
                    _deckPushStrokeRequiredTravel,
                    handSampleCount,
                    weaponSampleCount);
                _deckPushStrokeMaxTravel = 0.0f;
            }
        }

        // Logging (user directive: "log how close my hand is and when it pushed"):
        // dense while the hand is anywhere NEAR the tray (every 6th frame under 8gu),
        // sparse otherwise. The sparse-only version sampled every few seconds and
        // completely missed the pushes that failed to engage.
        {
            static std::uint32_t s_deckNearLog = 0;
            const DeckHullContact* nearest = nullptr;
            if (handContact.valid) nearest = &handContact;
            if (weaponContact.valid &&
                (!nearest || weaponContact.distance < nearest->distance)) {
                nearest = &weaponContact;
            }
            const float nearestDistance = nearest
                ? nearest->distance
                : (std::numeric_limits<float>::max)();
            const bool nearDeck =
                std::isfinite(nearestDistance) && nearestDistance < 8.0f;
            const bool denseTick = nearDeck && (++s_deckNearLog % 6) == 0;
            if (denseTick || _logCooldown <= 0) {
                if (_logCooldown <= 0) _logCooldown = 60;
                spdlog::debug(
                    "[PIPBOY] Deck collider contact: mesh={} tray={} lid={} handSamples={} "
                    "weaponSamples={} nearest={} surface={} dist={:.2f} pen={:.2f} "
                    "state={} pushProg={:.2f} stroke={} source={} "
                    "travel={:.2f}/{:.2f} lateral={:.2f} blocked={} occupied={}",
                    deckMeshAvailable,
                    !deckTriangles.empty(),
                    !lidTriangles.empty(),
                    handSampleCount,
                    weaponSampleCount,
                    nearest ? nearest->source : "none",
                    nearest ? nearest->surface : "none",
                    nearestDistance,
                    nearest ? nearest->penetration : 0.0f,
                    static_cast<int>(_tapeDeckState),
                    _pushProgress,
                    _deckPushStrokeValid,
                    _deckPushStrokeUsesWeapon ? "weapon" : "hand",
                    strokeTravelForLog,
                    _deckPushStrokeRequiredTravel,
                    strokeLateralForLog,
                    globallyBlocked,
                    handOccupied);
            }
        }
    }

    bool PipboyInteraction::TakeLoadedIntroHolotapeToRightHand(std::uint32_t formID)
    {
        if (!_holotapeLoaded || _loadedHolotapeFormID != formID || !IsIntroHolotape(formID)) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form = RE::TESForm::GetFormByID(formID);
        if (!player || !form || form->GetFormType() != RE::ENUM_FORM_ID::kNOTE) {
            spdlog::warn("[PIPBOY] Cannot take loaded intro tape {:08X} — player/form unavailable", formID);
            return false;
        }

        // Match normal menu-to-hand behavior: store any currently grabbed right-hand
        // object first, then holster a drawn weapon before queueing the tape.
        auto& grabMgr = GrabManager::GetSingleton();
        if (grabMgr.IsGrabbing(false)) {
            const auto& grabState = grabMgr.GetGrabState(false);
            auto* heldRefr = grabState.GetRefr();
            if (heldRefr) {
                RE::NiPointer<RE::TESObjectREFR> heldRef(heldRefr);
                auto* heldBase = heldRefr->GetObjectReference();
                if (heldBase) {
                    DropToHand::GetSingleton().MarkAsRecentlyStored(heldBase->formID);
                }

                grabMgr.EndGrab(false, nullptr, true);
                Hooks::SetSuppressHUDMessages(true);
                if (heldBase && heldBase->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                    RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                    heisenberg::AddObjectToContainer(
                        player, static_cast<RE::TESBoundObject*>(heldBase), &nullExtra, 1, nullptr, 0);
                    heisenberg::SafeDisable(heldRef.get());
                } else if (heldBase) {
                    Hooks::SetInternalActivation(true);
                    heldRef->ActivateRef(player, nullptr, 1, false, false, false);
                    Hooks::SetInternalActivation(false);
                }
                Hooks::SetSuppressHUDMessages(false);
                spdlog::debug("[PIPBOY] Stored right-hand item before taking loaded intro tape");
            }
        }

        auto* vrPlayer = f4vr::getPlayer();
        if (vrPlayer && vrPlayer->GetWeaponMagicDrawn()) {
            player->DrawWeaponMagicHands(false);
            spdlog::debug("[PIPBOY] Holstered weapon before taking loaded intro tape");
        }

        // Abort every playback stage before changing the physical deck state.
        StopIntroPlayback();
        StopNarrationWav();
        _pendingPlaybackFormID = 0;
        _pendingPlaybackDelay = 0.0f;
        _pendingAudioFormID = 0;
        _pendingAudioWaitFrames = 0;
        _pendingProgramFormID = 0;
        _pendingProgramWaitFrames = 0;
        _holotapePauseClearFrames = 0;
        _introSWFActive = false;
        _introSWFMenuSeen = false;
        _introSWFLastLogSec = 0;
        _introAudioStarted = false;
        _introSoundEvents.clear();
        _introSoundEventIndex = 0;

        if (auto* msgQueue = RE::UIMessageQueue::GetSingleton()) {
            msgQueue->AddMessage(MenuPipboyHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
            msgQueue->AddMessage(MenuHolotape(), RE::UI_MESSAGE_TYPE::kForceHide);
        }
        EnableMenuInput(MenuPipboy());
        EnableMenuInput(MenuHolotape());
        EnableMenuInput(MenuPipboyHolotape());
        if (auto* ui = RE::UI::GetSingleton()) {
            auto pipMenu = ui->GetMenu(MenuPipboy());
            if (pipMenu) pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
        }

        DeactivatePipboyScreen();
        EndWristOverrideForHolotape();
        RestoreIntroHolotapeType(formID);

        _holotapeLoaded = false;
        _loadedHolotapeFormID = 0;
        _tapREFForceHidden = true;
        _holotapeGrabCooldown = TAPE_GRAB_COOLDOWN;
        _holotapeRemovalRequiresGripRelease = false;
        _deckPushStrokeValid = false;
        _deckPushStrokeMaxTravel = 0.0f;
        _deckRemovalHandGuard = TAPE_GRAB_COOLDOWN;
        _insertionOpenHandActive = false;
        _deckOpenedByEject = false;
        SetTapREFVisible(false);

        auto* boundObj = form->As<RE::TESBoundObject>();
        if (boundObj) player->PlayPickUpSound(boundObj, true, false);
        DropToHand::GetSingleton().QueueDropToHand(formID, false, 1, true, false, true);
        VRInput::GetSingleton().TriggerHaptic(false, 3000);
        PlayWavSound("take out holotape.wav");

        spdlog::info("[PIPBOY] Loaded intro tape {:08X} removed from deck and queued to right hand", formID);
        return true;
    }

    void PipboyInteraction::SetTapREFVisible(bool visible)
    {
        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        RE::NiAVObject* tapREF = _cachedTapeRef ? _cachedTapeRef : f4vr::findAVObject(arm, "TapREF");
        if (tapREF) {
            float visScale = 1.0f;  // TapREF is unscaled
            tapREF->local.scale = visible ? visScale : 0.0f;
            spdlog::debug("[PIPBOY] TapREF visibility → {}", visible ? "VISIBLE" : "HIDDEN");
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Reset all state on save/load — prevents stale transforms and ghost holotapes
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::ClearState()
    {
        spdlog::info("[PIPBOY] ClearState — resetting all tape deck state for load");

        // Safety: never leave a temporary normal root or projected mode
        // overridden across a load.
        EndTemporaryNormalPipboyModel();
        EndWristOverrideForHolotape();

        _tapeDeckState          = TapeDeckState::Closed;
        _tapeDeckOpen           = false;
        _tapeDeckAnimProgress   = 0.0f;
        _ejectCooldown          = 0.0f;
        _insertionOpenHandActive = false;
        _deckPushCloseLockout      = 0.0f;
        _deckOpenedByEject      = false;
        _holotapeLoaded         = false;
        _loadedHolotapeFormID   = 0;
        _logCooldown            = 0;
        _buttonOriginalZ        = 0.0f;
        _buttonOriginalZSet     = false;
        _ejectContactLatched    = false;
        _holotapeGrabCooldown   = 0.0f;
        _holotapeRemovalCooldown = 0.0f;
        _holotapeRemovalRequiresGripRelease = false;
        _deckRemovalHandGuard   = 0.0f;
        _closeAnimSpeed         = ANIM_SPEED;
        _slamCooldown           = 0.0f;
        _pushProgress           = 1.0f;
        _deckPushStrokeValid       = false;
        _deckPushStrokeUsesWeapon  = false;
        _deckPushStrokeOrigin      = {};
        _deckPushStrokeDirection   = {};
        _deckPushStrokeStartProgress = 1.0f;
        _deckPushStrokeRequiredTravel = 3.0f;
        _deckPushStrokeMaxTravel   = 0.0f;
        _tapREFForceHidden      = false;
        _meshesInitialized      = false;   // Force re-init of mesh rotations on next frame
        // Invalidate persistent node caches (skeleton changes on load)
        _cachedEjectButton      = nullptr;
        _cachedEjectButtonMesh  = nullptr;
        _cachedTapeDeckLid      = nullptr;
        _cachedTapeRef          = nullptr;
        _cachedTapeDeckMesh1    = nullptr;
        _cachedTapeDeckLidMesh1 = nullptr;
        _cachedTapeDeckNode     = nullptr;
        _cachedArmNode          = nullptr;
        _nodesCached            = false;
        _frameCacheValid        = false;
        _nodeSnapshot.Reset();
        _nodeSnapshotArm        = nullptr;
        _nodeSnapshotPhaseAttempted = false;
        _nodeSnapshotUsable     = false;
        _fingerPosCached        = false;
        _interactionFingerPos = {};
        _previousInteractionFingerPos = {};
        _interactionFingerValid = false;
        _previousInteractionFingerValid = false;
        _pendingPlaybackDelay       = 0.0f;
        _pendingPlaybackFormID      = 0;
        _pendingAudioFormID         = 0;
        _pendingAudioWaitFrames     = 0;
        _pendingProgramFormID       = 0;
        _pendingProgramWaitFrames   = 0;
        _holotapePauseClearFrames   = 0;
        _deferredDisableHandle.reset();
        _deferredDisableFrames      = 0;
        // Restore PipboyMenu input and cursor that may have been disabled during holotape
        EnableMenuInput(MenuPipboy());
        if (auto* ui = RE::UI::GetSingleton()) {
            auto pipMenu = ui->GetMenu(MenuPipboy());
            if (pipMenu) pipMenu->menuFlags.set(RE::UI_MENU_FLAGS::kUsesCursor);
        }
        RestoreIntroHolotapeType(_introHolotapeFormID);
        _introSWFActive             = false;
        _programSWFActive           = false;
        _introSWFMenuSeen           = false;
        _introSWFLastLogSec         = 0;
        _introAudioStarted          = false;
        _introSoundEvents.clear();
        _introSoundEventIndex       = 0;
        // Terminal redirect state — restore function patch if active
        if (_consoleOpenedForTerminal) {
            auto* msgQueue = RE::UIMessageQueue::GetSingleton();
            if (msgQueue) {
                msgQueue->AddMessage(MenuConsole(), RE::UI_MESSAGE_TYPE::kHide);
            }
        }
        _radioWasEnabled            = false;
        _radioRestoreFrames         = 0;
        _consoleOpenedForTerminal   = false;
        _pendingTerminalRedirect    = false;
        _terminalRedirectActive     = false;
        _isWorldTerminalRedirect    = false;
        _terminalPatchesSuspended   = false;
        _terminalClosePipboyPending = false;
        _terminalForcedFrikScreenVisible = false;
        // Seed the "left the vault" latch: an established character (intro already played) is
        // well past Vault 111, so don't gate their terminal redirect. A fresh playthrough starts
        // false and flips true the moment the player first reaches an exterior cell.
        _hasReachedExterior         = g_config.introHolotapePlayed;
        // QueueIntroHolotapeDelivery seeds this true for a post-load save that already owns
        // the Pip-Boy; pre-pickup/new-game paths leave it false until the boot open+close.
        _pipboyOpenedSinceAcquire   = false;
        // Vanilla Pip-Boy boot sequence — same seeding rule as _pipboyOpenedSinceAcquire
        // (QueueIntroHolotapeDelivery seeds true for an established save; SetNewGame/here
        // leave false so a fresh pickup still fires the native event).
        // Safety net: if a save/load interrupts the sequence while the live Holo
        // override is active, ask FRIK to restore the player's model now.
        if (_bootHoloOverrideActive) {
            EndTemporaryNormalPipboyModel();
        }
        // And for the intro ceremony's Holo override — same never-strand-the-preference rule.
        if (_introHoloIniOverrideActive) {
            EndTemporaryNormalPipboyModel();
        }
        _introHoloIniOverrideActive = false;
        _introHoloWaitFrames        = 0;
        _introHoloWaitFormID        = 0;
        _introHoloSawPipOpen        = false;
        _bootSequenceFired          = false;
        _bootWristOverrideOwned     = false;
        _bootHoloModeHandled             = false;
        _bootHoloOverrideActive          = false;
        _bootPipboyRootTransitionPending = false;
        _bootPipboyRootTransitionFailed  = false;
        _bootPipboyRootMustChange        = false;
        _bootPipboyPreSwitchRoot          = nullptr;
        _bootPipboyCandidateRoot          = nullptr;
        _bootPipboyRootStableFrames      = 0;
        _bootPipboyRootWaitSeconds       = 0.0f;
        _bootHoloRestoreSeconds          = -1.0f;
        _bootMenuSeen               = false;
        _bootMenuFinished           = false;
        _bootMenuAppearWaitSeconds  = 0.0f;
        // Never strand the wrist screen lit (zeroed view angles) across an interrupted boot.
        if (_bootScreenActivated) {
            DeactivatePipboyScreen();
        }
        _bootScreenActivated        = false;
        // Nor PipboyMenu's movie blanked — restore its root before the state resets.
        if (_bootPipboyMenuBlanked) {
            SetPipboyMenuMovieVisible(true);
        }
        _bootPipboyMenuBlanked      = false;
        _bootPipboyCursorOverridden = false;
        _bootPipboyCursorWasEnabled = false;
        _bootContentOpenSeconds     = 0.0f;
        _bootContentOpenRequested   = false;
        _bootContentOpenWaitSeconds = 0.0f;
        _bootContentCloseRetrySeconds = 0.0f;
        _bootContentMenuSeen        = false;
        _bootContentCloseArmSeconds = 0.0f;
        _bootContentGripWasPressed  = false;
        _bootContentGripCloseRequested = false;
        _bootSawPipboyAbsent        = false;
        _bootPipboyArmorEquipQueued = false;
        _terminalScreenNode         = nullptr;
        _savedDiffuseSRV            = 0;
        _savedRendererPtr           = nullptr;
        _savedOrigWorldRoot         = 0;
        _savedHmdScreenNode         = nullptr;
        _savedHmdScreenParent       = nullptr;
        _worldTerminalChecked       = false;
        // World screen redirect state
        _worldScreenRedirectActive  = false;
        _worldScreenChecked         = false;
        _worldScreenTerminalNode    = nullptr;
        _worldScreenOrigSRV         = 0;
        // Holotape finger pose + grab tracking
        _holotapeFingerPoseActive   = false;
        _lastHeldHolotapeRefID      = 0;
        // Intro playback state (but NOT _introHolotapeFormID or _introLines — session-stable)
        StopIntroPlayback();
        _introDeliveryQueued    = false;
        _introDeliveryDelay     = 0.0f;
        _introPostLoadBaselinePending = false;
        _isNewGame              = false;
        _newGameExteriorReached = false;
        _frikHoloPipboy         = -1;  // Re-read FRIK mode after a game/cell load
        // NOTE: _frikPipboyScale and _dumpedNodes intentionally NOT reset (cached across loads)
    }

    // ════════════════════════════════════════════════════════════════════════
    // Mesh diffuse SRV helpers (used by terminal screen redirect)
    // ════════════════════════════════════════════════════════════════════════

    // File-scope static for rendTex stub (used by SetMeshDiffuseSRV when chain is partial)
    static uintptr_t s_rendTexStub = 0;

    uintptr_t PipboyInteraction::GetMeshDiffuseSRV(RE::NiAVObject* mesh)
    {
        if (!mesh) return 0;
        auto addr = reinterpret_cast<uintptr_t>(mesh);
        auto sp = *reinterpret_cast<uintptr_t*>(addr + 0x178);
        if (!sp) return 0;
        auto mat = *reinterpret_cast<uintptr_t*>(sp + 0x58);
        if (!mat) return 0;
        auto diffuse = *reinterpret_cast<uintptr_t*>(mat + 0x40);
        if (!diffuse) return 0;
        auto rendTex = *reinterpret_cast<uintptr_t*>(diffuse + 0x38);
        if (!rendTex) return 0;  // Partial chain — SetMeshDiffuseSRV will create a stub
        return *reinterpret_cast<uintptr_t*>(rendTex + 0x00);
    }

    void PipboyInteraction::SetMeshDiffuseSRV(RE::NiAVObject* mesh, uintptr_t srv)
    {
        if (!mesh) return;
        auto addr = reinterpret_cast<uintptr_t>(mesh);
        auto sp = *reinterpret_cast<uintptr_t*>(addr + 0x178);
        if (!sp) return;
        auto mat = *reinterpret_cast<uintptr_t*>(sp + 0x58);
        if (!mat) return;
        auto diffuse = *reinterpret_cast<uintptr_t*>(mat + 0x40);
        if (!diffuse) return;
        auto rendTex = *reinterpret_cast<uintptr_t*>(diffuse + 0x38);
        if (rendTex) {
            // Full chain — write SRV into existing rendTex
            *reinterpret_cast<uintptr_t*>(rendTex + 0x00) = srv;
        } else {
            // Partial chain — rendTex is null. Create a minimal stub and inject.
            // The stub only needs the SRV at offset 0x00.
            if (!s_rendTexStub) {
                s_rendTexStub = reinterpret_cast<uintptr_t>(VirtualAlloc(
                    nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (s_rendTexStub) {
                    memset(reinterpret_cast<void*>(s_rendTexStub), 0, 256);
                    spdlog::debug("[INTRO] Created rendTex stub at {:X}", s_rendTexStub);
                }
            }
            if (s_rendTexStub) {
                *reinterpret_cast<uintptr_t*>(s_rendTexStub + 0x00) = srv;
                *reinterpret_cast<uintptr_t*>(diffuse + 0x38) = s_rendTexStub;
            }
        }
    }

    // SEH-safe helper: check if a node has a valid shader chain for SRV swapping
    // Returns rendTex pointer (non-zero = valid), 0 = no valid chain
    static uintptr_t CheckNodeShaderChain_SEH(RE::NiAVObject* node)
    {
        uintptr_t result = 0;
        __try
        {
            auto addr = reinterpret_cast<uintptr_t>(node);
            auto sp = *reinterpret_cast<uintptr_t*>(addr + 0x178);
            if (!sp) return 0;
            auto mat = *reinterpret_cast<uintptr_t*>(sp + 0x58);
            if (!mat) return 0;
            auto diffuse = *reinterpret_cast<uintptr_t*>(mat + 0x40);
            if (!diffuse) return 0;
            auto rendTex = *reinterpret_cast<uintptr_t*>(diffuse + 0x38);
            result = rendTex;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { result = 0; }
        return result;
    }

    // Detailed chain dump (for debugging Screen:0 with partial chain)
    static void DumpShaderChain_SEH(RE::NiAVObject* node, const char* label)
    {
        __try
        {
            auto addr = reinterpret_cast<uintptr_t>(node);
            auto sp = *reinterpret_cast<uintptr_t*>(addr + 0x178);
            spdlog::debug("[INTRO] {} chain: sp={:X}", label, sp);
            if (!sp) return;
            auto mat = *reinterpret_cast<uintptr_t*>(sp + 0x58);
            spdlog::debug("[INTRO] {} chain: mat={:X}", label, mat);
            if (!mat) return;
            auto diffuse = *reinterpret_cast<uintptr_t*>(mat + 0x40);
            spdlog::debug("[INTRO] {} chain: diffuse={:X}", label, diffuse);
            if (!diffuse) return;
            auto rendTex = *reinterpret_cast<uintptr_t*>(diffuse + 0x38);
            spdlog::debug("[INTRO] {} chain: rendTex={:X}", label, rendTex);
            if (!rendTex) return;
            auto srv = *reinterpret_cast<uintptr_t*>(rendTex + 0x00);
            spdlog::debug("[INTRO] {} chain: SRV={:X}", label, srv);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[INTRO] {} chain: exception during walk", label);
        }
    }

    // SEH-safe helper: iterate NiNode children (up to maxChildren) looking for one with a valid shader chain.
    // Uses raw NiNode::children layout: children is an NiTObjectArray at a known offset within NiNode.
    // RE::NiNode inherits: NiAVObject (0x120 bytes) → NiNode adds children array.
    // In CommonLibF4 NiNode, children is: NiTObjectArray<NiPointer<NiAVObject>> children;
    // SEH-safe: find a named child node recursively (up to maxDepth)
    static RE::NiAVObject* FindNamedNodeInChildren_SEH(RE::NiNode* parent, const char* targetName,
                                                        int maxDepth = 4, int depth = 0)
    {
        if (!parent || depth > maxDepth) return nullptr;
        RE::NiAVObject* result = nullptr;
        __try
        {
            auto numChildren = parent->children.size();
            if (numChildren > 64) numChildren = 64;

            for (uint32_t i = 0; i < numChildren; ++i) {
                auto* child = parent->children[i].get();
                if (!child) continue;

                const char* childName = child->name.c_str();
                if (childName && strcmp(childName, targetName) == 0) {
                    auto rendTex = CheckNodeShaderChain_SEH(child);
                    if (rendTex) {
                        spdlog::debug("[INTRO] Found target mesh '{}' (rendTex={:X})", childName, rendTex);
                        return child;
                    }
                }

                // Recurse into child nodes
                auto* childAsNode = reinterpret_cast<RE::NiNode*>(child);
                __try
                {
                    auto childCount = childAsNode->children.size();
                    if (childCount > 0 && childCount <= 64) {
                        auto* deeper = FindNamedNodeInChildren_SEH(childAsNode, targetName, maxDepth, depth + 1);
                        if (deeper) return deeper;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return result;
    }

    // SEH-safe: find the first child with a valid shader chain, skipping dust/overlay nodes
    static RE::NiAVObject* FindShaderNodeInChildren_SEH(RE::NiNode* parent, int maxDepth = 3, int depth = 0)
    {
        if (!parent || depth > maxDepth) return nullptr;

        RE::NiAVObject* found = nullptr;
        __try
        {
            auto numChildren = parent->children.size();
            if (numChildren > 64) numChildren = 64;

            for (uint32_t i = 0; i < numChildren; ++i) {
                auto* child = parent->children[i].get();
                if (!child) continue;

                const char* childName = child->name.c_str();
                if (!childName) childName = "?";

                // Skip dust/overlay nodes — we want the actual screen
                if (strstr(childName, "Dust") || strstr(childName, "dust") ||
                    strstr(childName, "Overlay") || strstr(childName, "overlay")) {
                    spdlog::debug("[INTRO] Skipping overlay mesh '{}'", childName);
                    continue;
                }

                auto rendTex = CheckNodeShaderChain_SEH(child);
                if (rendTex) {
                    spdlog::debug("[INTRO] Found mesh '{}' with valid shader chain (rendTex={:X})",
                                 childName, rendTex);
                    return child;
                }

                auto* childAsNode = reinterpret_cast<RE::NiNode*>(child);
                __try
                {
                    auto childCount = childAsNode->children.size();
                    if (childCount > 0 && childCount <= 64) {
                        auto* deeper = FindShaderNodeInChildren_SEH(childAsNode, maxDepth, depth + 1);
                        if (deeper) return deeper;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::warn("[INTRO] Exception iterating children at depth {}", depth);
        }
        return found;
    }

    // Diagnostic: dump all children of a node with their shader chain status
    // NOTE: Cannot use std::string or other C++ objects inside __try blocks (C2712)
    static void DumpNodeChildren_SEH(RE::NiNode* parent, const char* label, int maxDepth = 3, int depth = 0)
    {
        if (!parent || depth > maxDepth) return;

        // Pre-build indent outside __try (no C++ objects inside SEH)
        char indent[16] = {};
        int indentLen = depth * 2;
        if (indentLen > 14) indentLen = 14;
        for (int j = 0; j < indentLen; ++j) indent[j] = ' ';
        indent[indentLen] = '\0';

        __try
        {
            auto numChildren = parent->children.size();
            if (numChildren > 64) numChildren = 64;

            for (uint32_t i = 0; i < numChildren; ++i) {
                auto* child = parent->children[i].get();
                if (!child) { spdlog::debug("[INTRO] {}[{}] null", indent, i); continue; }

                const char* name = "?";
                __try { name = child->name.c_str(); if (!name) name = "?"; }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                auto rendTex = CheckNodeShaderChain_SEH(child);
                auto addr = reinterpret_cast<uintptr_t>(child);
                auto sp = *reinterpret_cast<uintptr_t*>(addr + 0x178);

                spdlog::debug("[INTRO] {}[{}] '{}' sp={:X} rendTex={:X} flags={:X} scale={:.2f}",
                             indent, i, name, sp, rendTex,
                             child->flags.flags, child->local.scale);

                // Recurse
                auto* childAsNode = reinterpret_cast<RE::NiNode*>(child);
                __try {
                    auto cc = childAsNode->children.size();
                    if (cc > 0 && cc <= 64)
                        DumpNodeChildren_SEH(childAsNode, label, maxDepth, depth + 1);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }


    // ════════════════════════════════════════════════════════════════════════
    // Intro Holotape — first-load narrated introduction
    // ════════════════════════════════════════════════════════════════════════

    static float ParseWavDuration(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return 3.0f;

        char header[44];
        file.read(header, 44);
        if (file.gcount() < 44) return 3.0f;

        uint32_t byteRate = 0;
        std::memcpy(&byteRate, header + 28, 4);
        if (byteRate == 0) return 3.0f;

        // Find "data" chunk to get actual audio data size
        file.seekg(12);
        while (file) {
            char chunkID[4];
            uint32_t chunkSize = 0;
            file.read(chunkID, 4);
            file.read(reinterpret_cast<char*>(&chunkSize), 4);
            if (file.gcount() < 4) break;

            if (chunkID[0] == 'd' && chunkID[1] == 'a' && chunkID[2] == 't' && chunkID[3] == 'a') {
                return static_cast<float>(chunkSize) / static_cast<float>(byteRate);
            }
            file.seekg(chunkSize, std::ios::cur);
            if (chunkSize & 1) file.seekg(1, std::ios::cur);
        }
        return 3.0f;
    }

    void PipboyInteraction::InitIntroHolotape()
    {
        if (_introInitDone) return;
        _introInitDone = true;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            spdlog::debug("[INTRO] TESDataHandler not available yet");
            _introInitDone = false;
            return;
        }


        // LookupForm() fails in F4VR because GetCompiledFileCollection() returns null.
        // Bypass it: use LookupModByName to get the file, construct the FormID manually.
        auto* modFile = dataHandler->LookupModByName("Heisenberg.esp");
        if (!modFile) {
            spdlog::debug("[INTRO] Heisenberg.esp not loaded — intro holotape disabled");
            return;
        }

        const uint32_t localFormID = 0x800;  // First custom form in our ESP
        uint32_t fullFormID = 0;

        // Heisenberg.esp ships ESL/LIGHT-flagged. A light plugin's runtime FormID is
        //   0xFE000000 | (smallFileCompileIndex << 12) | (localID & 0xFFF)
        // NOT (compileIndex << 24) | localID. compileIndex on a light plugin reads as 0xFE, so the
        // old regular-ESP formula produced 0xFE000800 — which is a *base-game ActorValueInfo*, not
        // our NOTE. Writing our name/model onto that AVIF corrupted it and crashed the Pip-Boy 3D
        // preview (GetFormDetailedString → BSsprintf on the mangled form). Use IsLight() to pick the
        // right formula. (Matches CommonLibF4VR TESDataHandler::LookupForm.)
        if (modFile->IsLight()) {
            fullFormID = 0xFE000000u
                | (static_cast<uint32_t>(modFile->smallFileCompileIndex) << 12)
                | (localFormID & 0x00000FFFu);
            spdlog::info("[INTRO] Heisenberg.esp is LIGHT (smallIndex={:#x}) → FormID={:08X}",
                         modFile->smallFileCompileIndex, fullFormID);
        } else {
            fullFormID = (static_cast<uint32_t>(modFile->compileIndex) << 24) | (localFormID & 0x00FFFFFF);
            spdlog::info("[INTRO] Heisenberg.esp compileIndex={:02X} → FormID={:08X}",
                         modFile->compileIndex, fullFormID);
        }

        auto* form = RE::TESForm::GetFormByID(fullFormID);
        if (!form) {
            spdlog::warn("[INTRO] FormID {:08X} not found in game — ESP may be malformed", fullFormID);
            return;
        }

        // SAFETY GUARD: only touch the form if it really is our holotape NOTE. If the FormID math
        // ever resolves to the wrong form (e.g. an ESL index mismatch), writing fullName/model/
        // programFile onto it corrupts an unrelated form (this is exactly what caused the AVIF
        // crash). Bail out instead of corrupting anything.
        if (form->GetFormType() != RE::ENUM_FORM_ID::kNOTE) {
            spdlog::warn("[INTRO] FormID {:08X} resolved to type {} (not NOTE) — aborting intro setup to avoid corrupting another form",
                         fullFormID, static_cast<int>(form->GetFormType()));
            return;
        }

        _introHolotapeFormID = fullFormID;
        spdlog::info("[INTRO] Resolved intro holotape NOTE FormID={:08X}", _introHolotapeFormID);

        // Set display name (ESP has generic "Heisenberg" — override to full name)
        // Use direct member access instead of TESFullName::SetFullName (relies on REL::ID that may not resolve in VR)
        if (auto* fullNameComp = form->As<RE::TESFullName>()) {
            fullNameComp->fullName = "Heisenberg Instructions";
            spdlog::debug("[INTRO] Set holotape name to 'Heisenberg Instructions'");
        }

        // Pre-set programFile so it's ready when we switch to kProgram at playback time.
        // We do NOT change type here — keeping it kVoice prevents the game from
        // auto-playing the holotape when it's added to inventory during deck insertion.
        // (Model already ships valid in the ESP as Props\Holotape_Prop.nif — do not override it.)
        {
            auto* note = static_cast<RE::BGSNote*>(form);
            note->programFile = "Heisenberg";  // No .swf — path builder appends it for VR loose file check
            spdlog::debug("[INTRO] Set programFile='Heisenberg'");
        }

        _introLines.clear();
        for (int i = 1; i <= 20; ++i) {
            std::string filename = "intro\\Line " + std::to_string(i) + ".wav";
            std::string fullPath = GetSoundDir() + filename;
            if (!std::filesystem::exists(fullPath)) break;

            float duration = ParseWavDuration(fullPath);
            _introLines.push_back({ filename, duration, "" });
            spdlog::debug("[INTRO] Found {} ({:.1f}s)", filename, duration);
        }

        // Load subtitle text from subtitles.txt (one line per WAV)
        {
            std::string subPath = GetSoundDir() + "intro\\subtitles.txt";
            std::ifstream subFile(subPath);
            if (subFile.is_open()) {
                std::string line;
                int idx = 0;
                while (std::getline(subFile, line) && idx < static_cast<int>(_introLines.size())) {
                    // Trim trailing \r for Windows line endings
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    _introLines[idx].subtitle = line;
                    idx++;
                }
                spdlog::debug("[INTRO] Loaded {} subtitles from {}", idx, subPath);
            } else {
                spdlog::debug("[INTRO] No subtitles.txt found at {}", subPath);
            }
        }

        spdlog::debug("[INTRO] Loaded {} intro lines", _introLines.size());

    }

    void PipboyInteraction::QueueIntroHolotapeDelivery()
    {
        if (IntroCeremonyState::IsResolved() && IntroCeremonyState::IsComplete()) {
            _introDeliveryQueued = false;
            spdlog::debug("[INTRO] Post-load ceremony not queued — already complete for this save");
            return;
        }

        // This method is the kPostLoadGame path. If the loaded inventory already contains the
        // Pip-Boy, the vanilla pickup/boot sequence cannot be in progress, so delivery may run
        // automatically once loading is over and the Pip-Boy is closed. A pre-pickup save leaves
        // the latch false and must still observe the real pickup boot open+close.
        const bool loadedWithPipboy = PlayerHasReceivedPipboy();
        _pipboyOpenedSinceAcquire = loadedWithPipboy;
        // Same rule as _pipboyOpenedSinceAcquire: an established save already owning the
        // Pip-Boy never ran the vanilla boot this session (it ran, if ever, long before this
        // load) — don't fire the event retroactively for existing characters.
        _bootSequenceFired = loadedWithPipboy;
        _introPostLoadBaselinePending = true;
        _introDeliveryQueued = true;
        _introDeliveryDelay = INTRO_DELIVERY_DELAY;
        spdlog::debug("[INTRO] Queued intro holotape delivery ({:.0f}s delay, loadedWithPipboy={})",
                      INTRO_DELIVERY_DELAY, loadedWithPipboy);
    }

    void PipboyInteraction::SetNewGame()
    {
        _isNewGame = true;
        _newGameExteriorReached = false;
        _pipboyOpenedSinceAcquire = false;
        _bootSequenceFired = false;
        _bootHoloModeHandled = false;
        _bootHoloOverrideActive = false;
        _bootPipboyRootTransitionPending = false;
        _bootPipboyRootTransitionFailed = false;
        _bootPipboyRootMustChange = false;
        _bootPipboyPreSwitchRoot = nullptr;
        _bootPipboyCandidateRoot = nullptr;
        _bootPipboyRootStableFrames = 0;
        _bootPipboyRootWaitSeconds = 0.0f;
        _bootHoloRestoreSeconds = -1.0f;
        _bootPipboyArmorEquipQueued = false;
        _introPostLoadBaselinePending = false;
        _hasReachedExterior = false;
        IntroCeremonyState::ResetForNewGame();
        // Arm delivery immediately — do NOT wait for the player to leave the vault. The per-frame
        // delivery timer defers until the Pip-Boy/tape-deck node exists (i.e. the Pip-Boy is on
        // the wrist) and the player isn't in power armor, so the ceremony pops ~INTRO_DELIVERY_DELAY
        // seconds after the player receives the Pip-Boy inside Vault 111.
        _introDeliveryQueued = true;
        _introDeliveryDelay  = INTRO_DELIVERY_DELAY;
        spdlog::info("[INTRO] New game — intro queued; delivers ~{:.0f}s after the Pip-Boy is on the wrist",
                     INTRO_DELIVERY_DELAY);
    }

    void PipboyInteraction::TryDeliverIntroHolotape()
    {
        // Defensive check in addition to the queue-time/per-frame checks: a completed co-save
        // must never replay the physical opening ceremony.
        if (!IntroCeremonyState::IsResolved()) {
            _introDeliveryQueued = true;
            _introDeliveryDelay = INTRO_DELIVERY_DELAY;
            spdlog::debug("[INTRO] Per-save ceremony state unresolved — deferring delivery");
            return;
        }
        if (IntroCeremonyState::IsComplete()) {
            _introDeliveryQueued = false;
            spdlog::debug("[INTRO] Ceremony already complete for this save — skipping delivery");
            return;
        }

        // The tape-deck node + lid/mesh sub-nodes are cached once and never refreshed. Right after
        // the Pip-Boy is equipped the 3D is re-created, so any node cached during the unsettled
        // bootup is now STALE — driving the deck-open animation against a dead node (deck won't
        // open, finger can't open it). Invalidate the caches here so this ceremony re-resolves the
        // CURRENT Pip-Boy nodes. The gate already required FRIK's skeleton to be ready, so the
        // fresh lookup below resolves the real, settled nodes.
        // InvalidateFrameCache() also drops the cached ARM node — the crash log showed findAVObject
        // walking a STALE arm (freed on the post-equip 3D rebuild) and dispatching IsNode() through
        // a dead vtable. Re-resolving the arm fresh (under SafeFindAVObject) avoids that.
        InvalidateFrameCache();
        _cachedTapeDeckNode    = nullptr;
        _cachedTapeDeckLid     = nullptr;
        _cachedTapeDeckMesh1   = nullptr;
        _cachedTapeDeckLidMesh1= nullptr;

        // Don't run the ceremony until the player has the Pip-Boy. If it's not
        // present yet, keep the delivery queued so the timer retries.
        if (GetCachedTapeDeckNode() == nullptr) {
            spdlog::debug("[INTRO] Pip-Boy not present yet — deferring intro ceremony");
            _introDeliveryQueued = true;
            _introDeliveryDelay = INTRO_DELIVERY_DELAY;
            return;
        }

        _introDeliveryQueued = false;

        InitIntroHolotape();

        if (_introHolotapeFormID == 0) {
            spdlog::debug("[INTRO] No intro holotape form — skipping delivery");
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        auto* noteForm = RE::TESForm::GetFormByID(_introHolotapeFormID);
        if (!noteForm) {
            spdlog::warn("[INTRO] Could not resolve FormID {:08X}", _introHolotapeFormID);
            return;
        }

        // Add the tape if needed. A valid incomplete co-save is authoritative: if another mod or
        // console command already supplied the tape, use that copy and still run the ceremony once.
        const auto inv = player->GetInventoryObjectCount(static_cast<RE::TESBoundObject*>(noteForm));
        if (inv <= 0) {
            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
            heisenberg::AddObjectToContainer(player, static_cast<RE::TESBoundObject*>(noteForm),
                                             &nullExtra, 1, nullptr, 0);
            spdlog::debug("[INTRO] Added intro holotape to inventory");
        } else {
            spdlog::debug("[INTRO] Using existing intro holotape for first ceremony (count={})", inv);
        }

        // First and only delivery for this save — open the physical deck with the tape visible.
        _tapeDeckOpen = true;
        _tapeDeckState = TapeDeckState::Opening;
        _deckOpenedByEject = false;  // Ceremony — hand stays open for push-close
        _holotapeLoaded = true;
        _loadedHolotapeFormID = _introHolotapeFormID;
        _tapREFForceHidden = false;
        SetTapREFVisible(true);

        // No longer force-sheathes a drawn weapon here (removed 2026-07-22 — forcibly
        // unequipping the player's real weapon just because the ceremony started was
        // an unwanted interruption during active gameplay).

        PlayWavSound("Eject with holotape inside.wav");
        IntroCeremonyState::MarkComplete();
        spdlog::info("[INTRO] Intro holotape delivered — deck open");
    }

    void PipboyInteraction::StartIntroPlayback()
    {
        if (_introLines.empty()) {
            spdlog::warn("[INTRO] No intro lines to play");
            return;
        }

        // Transitioning from the intro SWF to narration is not an abort.  The
        // old StopIntroPlayback() call restored the stock Holo root here,
        // while the screen was closing, which visibly activated Holo during
        // the narration.  Stop only a prior audio handle and retain display
        // ownership until the final line completes.
        if (_introPlaybackActive) {
            StopNarrationWav();
        }
        heisenberg::HideSubtitle();
        _introDisplayRestorePending = false;

        _introPlaybackActive = true;
        _introCurrentLine = -1;   // Will advance to 0 on first UpdateIntroPlayback fire
        // Set end time to now so first line fires immediately
        _introLineEndTime = std::chrono::steady_clock::now();
        _lastDisplayedSubLine = -1;

        spdlog::info("[INTRO] Starting playback — {} lines (wall-clock timer, Win32 audio)", _introLines.size());
    }

    void PipboyInteraction::UpdateIntroPlayback(float /*deltaTime*/)
    {
        if (_introPlaybackActive) {
            // ── Subtitle display ──
            int curLine = _introCurrentLine;
            if (curLine >= 0 && curLine < static_cast<int>(_introLines.size())) {
                bool lineChanged = (curLine != _lastDisplayedSubLine);
                if (lineChanged) {
                    _lastDisplayedSubLine = curLine;
                    // Clear previous subtitle before showing new one (game doesn't auto-replace)
                    heisenberg::HideSubtitle();
                    const auto& sub = _introLines[curLine].subtitle;
                    if (!sub.empty()) {
                        heisenberg::ShowSubtitle(sub.c_str());
                        spdlog::debug("[INTRO] Subtitle {}/{}: '{}'", curLine + 1,
                                     _introLines.size(), sub);
                    }
                }
            }

            // ── Audio timing (wall-clock — independent of frame rate) ──
            // Use std::chrono so timing matches real time regardless of VR frame rate.
            auto now = std::chrono::steady_clock::now();
            if (now >= _introLineEndTime) {
                int nextLine = _introCurrentLine + 1;
                if (nextLine < static_cast<int>(_introLines.size())) {
                    _introCurrentLine = nextLine;
                    spdlog::debug("[INTRO] Playing line {}/{}", nextLine + 1, _introLines.size());
                    // Use BSAudioManager (same as all other Heisenberg SFX, flags 0x12).
                    // PlaySoundA (WinMM) opened a competing WASAPI session on the HMD device,
                    // causing SteamVR's HRTF plugin to reset and corrupt spatial audio.
                    std::string gameRelPath = GetGameSoundPath(_introLines[nextLine].filename.c_str());
                    PlayNarrationWav(gameRelPath);
                    float lineDuration = _introLines[nextLine].durationSeconds + INTRO_LINE_GAP;
                    _introLineEndTime = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(lineDuration));
                } else {
                    spdlog::info("[INTRO] Playback complete");
                    _introPlaybackActive = false;
                    _introCurrentLine = -1;
                    _introDisplayRestorePending = true;
                    // Mark intro as fully played — won't re-deliver on subsequent loads
                    if (!g_config.introHolotapePlayed) {
                        g_config.introHolotapePlayed = true;
                        g_config.Save();
                        spdlog::info("[INTRO] Set introHolotapePlayed=true");
                    }
                }
            }
        } else if (_lastDisplayedSubLine >= 0) {
            // Playback ended — clear subtitle
            heisenberg::HideSubtitle();
            _lastDisplayedSubLine = -1;
        }
    }

    void PipboyInteraction::StopIntroPlayback()
    {
        if (_introPlaybackActive) {
            spdlog::info("[INTRO] Stopping playback");
            StopNarrationWav();
        }
        _introPlaybackActive = false;
        _introCurrentLine = -1;
        _introDisplayRestorePending = false;
        heisenberg::HideSubtitle();
        // An interrupted/ejected intro must restore stock FRIK's Holo root too.
        if (_introHoloIniOverrideActive) {
            EndTemporaryNormalPipboyModel();
            _introHoloIniOverrideActive = false;
            _introHoloSawPipOpen = false;
            spdlog::info("[PIPBOY] Intro playback stopped — restored stock FRIK Holo root");
        }
        EndWristOverrideForHolotape();
    }

    bool PipboyInteraction::IsIntroHolotape(std::uint32_t formID) const
    {
        return _introHolotapeFormID != 0 && formID == _introHolotapeFormID;
    }

    // Debug: dump node tree
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::DumpNodesContaining(RE::NiAVObject* node, const std::string& indent)
    {
        if (!node) return;

        if (indent.length() < 12) {
            spdlog::warn("[PIPBOY] {}{} (local.z={:.3f})", indent, node->name.c_str(), node->local.translate.z);
        }

        if (auto* asNode = node->IsNode()) {
            for (auto& child : asNode->children) {
                if (child) {
                    DumpNodesContaining(child.get(), indent + "  ");
                }
            }
        }
    }
}
