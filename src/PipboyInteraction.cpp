#include "PipboyInteraction.h"
#include "ActivatorHandler.h"
#include "Config.h"
#include "MenuChecker.h"
#include "DropToHand.h"
#include "Hooks.h"
#include "FRIKInterface.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "IntroCeremonyState.h"
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
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <filesystem>

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

static bool s_projOverrideActive   = false;
static bool s_projSavedAlways      = false;
static bool s_projSavedHMD         = false;
static bool s_projOverrideSawPipOpen = false;  // Pipboy was open during the override
static bool s_projOverridePlaybackPending = false; // Staged Holo root swap has not queued playback yet
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
    s_projOverridePlaybackPending = false;
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
    if (s_termAppliedWristOverride || s_projOverridePlaybackPending) return;
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
                if (blurCount > 0) {
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

    void PipboyInteraction::ResetIntroWristTransition()
    {
        _introWristTransitionFormID = 0;
        _introWristPreFlipRoot = nullptr;
        _introWristCandidateRoot = nullptr;
        _introWristStableFrames = 0;
        _introWristTimeoutFrames = 0;
        s_projOverridePlaybackPending = false;
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

        if (_introWristTransitionFormID != 0) {
            spdlog::debug("[PIPBOY] Intro wrist transition already pending for {:08X}",
                         _introWristTransitionFormID);
            return;
        }

        auto* noteForm = RE::TESForm::GetFormByID(formID);
        if (!noteForm || noteForm->GetFormType() != RE::ENUM_FORM_ID::kNOTE) return;
        auto* holotape = static_cast<RE::BGSNote*>(noteForm);
        holotape->type = RE::BGSNote::NOTE_TYPE::kProgram;
        holotape->programFile = "Heisenberg";

        const bool projected = IsProjectedPipboyNow();
        const bool stagedHolo = projected
            && g_config.holotapeWristOverrideInProjected
            && IsFrikHoloPipboyEnabled();

        if (stagedHolo) {
            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            _introWristPreFlipRoot = playerNodes ? playerNodes->PipboyRoot_nif_only_node : nullptr;

            // Only mode flags change here. Screen activation and SWF queueing are deferred
            // until FRIK has installed and stabilized its replacement Holo root.
            if (BeginWristOverrideForHolotape(true)) {
                _introWristTransitionFormID = formID;
                _introWristCandidateRoot = nullptr;
                _introWristStableFrames = 0;
                _introWristTimeoutFrames = 120;
                s_projOverridePlaybackPending = true;
                spdlog::info("[PIPBOY] Intro {:08X}: staged Holo wrist transition started (oldRoot={} timeout=120)",
                             formID, static_cast<void*>(_introWristPreFlipRoot));
                return;
            }
        } else if (projected && g_config.holotapeWristOverrideInProjected) {
            BeginWristOverrideForHolotape();
        }

        QueueProgramHolotapePlayback(formID);
    }

    void PipboyInteraction::UpdateIntroWristTransition()
    {
        if (_introWristTransitionFormID == 0) return;

        const auto formID = _introWristTransitionFormID;
        if (!_holotapeLoaded || _loadedHolotapeFormID != formID || Utils::IsPlayerInPowerArmor()) {
            spdlog::info("[PIPBOY] Intro Holo wrist transition cancelled — tape unavailable or power armor entered");
            ResetIntroWristTransition();
            EndWristOverrideForHolotape();
            RestoreIntroHolotapeType(formID);
            return;
        }

        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        RE::NiNode* currentRoot = playerNodes ? playerNodes->PipboyRoot_nif_only_node : nullptr;
        RE::NiNode* currentScreen = playerNodes ? playerNodes->ScreenNode : nullptr;
        const bool replacementReady = currentRoot
            && currentRoot != _introWristPreFlipRoot
            && currentScreen
            && IsAncestorOf(currentRoot, currentScreen);

        if (replacementReady) {
            if (currentRoot == _introWristCandidateRoot) {
                ++_introWristStableFrames;
            } else {
                _introWristCandidateRoot = currentRoot;
                _introWristStableFrames = 1;
                spdlog::debug("[PIPBOY] Intro Holo wrist transition found replacement root {}",
                             static_cast<void*>(currentRoot));
            }
        } else {
            _introWristCandidateRoot = nullptr;
            _introWristStableFrames = 0;
        }

        if (_introWristStableFrames >= 2) {
            spdlog::info("[PIPBOY] Intro Holo wrist root stable for 2 frames — activating wrist playback");
            ResetIntroWristTransition();
            QueueProgramHolotapePlayback(formID);
            return;
        }

        if (--_introWristTimeoutFrames <= 0) {
            spdlog::warn("[PIPBOY] Intro Holo wrist transition timed out — restoring HMD mode and falling back to projected playback");
            ResetIntroWristTransition();
            EndWristOverrideForHolotape();
            QueueProgramHolotapePlayback(formID);
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Main update (called from Heisenberg.cpp every frame)
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::OnFrameUpdate(float deltaTime)
    {
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

        // FRIK Holo mode replaces PlayerNodes->PipboyRoot_nif_only_node only after
        // the projected/HMD flags change. Wait for that replacement to settle before
        // lighting the screen or queueing the intro SWF.
        UpdateIntroWristTransition();

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

                // Sound will be played right before console opens (in the per-frame block)
                _terminalSoundPending = true;

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

                // Per-frame: force FRIK ScreenNode visible + scaled
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
                    _consoleOpenDelayFrames = 0;
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

                // Sound will be played right before console opens (in the per-frame block)
                _terminalSoundPending = true;
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

        // Tape deck physical interactions — disabled in power armor
        if (!inPowerArmor) {
            OperateEjectButton(deltaTime);
            CheckHandPush();             // Hand-tracked deck pushing (replaces slam)
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
                            _holotapeGrabCooldown = 1.0f;  // 1s cooldown on new holotape grab
                            spdlog::debug("[PIPBOY] New holotape grab {:08X} — 1s insertion cooldown", refr->formID);
                        }
                    }
                }
                if (!holotapeHeld) _lastHeldHolotapeRefID = 0;
            }

            CheckHolotapeInsertion();
            UpdateHolotapeFingerPose();
        }

        // Re-draw weapon when holotape is no longer held in either hand.
        // We holstered the weapon at holotape removal so it wouldn't conflict with the grab.
        if (_holsteredWeaponForHolotape) {
            auto& grabMgr = GrabManager::GetSingleton();
            bool eitherHandHoldsHolotape = false;
            for (int h = 0; h < 2; h++) {
                bool isLeft = (h == 0);
                if (!grabMgr.IsGrabbing(isLeft)) continue;
                auto& gs = grabMgr.GetGrabState(isLeft);
                auto* refr = gs.GetRefr();
                if (!refr) continue;
                auto* baseObj = refr->GetObjectReference();
                if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                    eitherHandHoldsHolotape = true;
                    break;
                }
            }
            if (!eitherHandHoldsHolotape) {
                auto* player2 = RE::PlayerCharacter::GetSingleton();
                if (player2) {
                    player2->DrawWeaponMagicHands(true);
                    spdlog::debug("[PIPBOY] Re-drawing weapon — holotape no longer held");
                }
                _holsteredWeaponForHolotape = false;
            }
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

    RE::NiAVObject* PipboyInteraction::GetPipboyArmNode()
    {
        if (_frameCacheValid && _cachedArmNode) return _cachedArmNode;

        // CRITICAL: Must use the 3rd-person skeleton (unkF0->rootNode), NOT firstPersonSkeleton.
        // The renderer draws from the 3rd-person tree. Writes to firstPersonSkeleton nodes are invisible.
        // FRIK's Skeleton uses getRootNode() (3rd-person BSFlattenedBoneTree) and searches from getCommonNode() ("COM").
        // VirtualHolsters also uses unkF0->rootNode exclusively.
        auto* commonNode = f4vr::getCommonNode();
        if (!commonNode) {
            return nullptr;
        }

        // Pipboy is always on the LEFT wrist (doesn't swap in LH mode)
        const char* forearmNodeName = "LArm_ForeArm3";

        RE::NiAVObject* node = SafeFindAVObject(commonNode, forearmNodeName);

        static bool loggedOnce = false;
        if (!loggedOnce) {
            if (node) {
                spdlog::debug("[PIPBOY] Found arm node on 3rd-person skeleton: {} addr={}", forearmNodeName, (void*)node);
            } else {
                spdlog::warn("[PIPBOY] Arm node {} NOT found on 3rd-person skeleton!", forearmNodeName);
            }
            loggedOnce = true;
        }

        _cachedArmNode = node;
        _frameCacheValid = true;
        return node;
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
        _fingerPosCached = false;
    }

    RE::NiAVObject* PipboyInteraction::GetCachedTapeDeckNode()
    {
        if (_cachedTapeDeckNode) return _cachedTapeDeckNode;

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return nullptr;

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
        auto handle = BSPlayGameSound(GetGameSoundPath(filename).c_str(), 0.75f);
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

        // The EjectButton node is an animation pivot, not a physical contact point.
        // FRIK uses TapeDeck01 as the vanilla mesh's eject detection zone because the
        // model has no dedicated EjectDetect node. Keep the button node only for visuals.
        RE::NiAVObject* ejectContactTarget = GetCachedTapeDeckNode();
        if (!ejectContactTarget) return;

        // Don't detect eject button when weapon is drawn or scoping — hand moves near
        // Pipboy during aiming and causes false triggers
        {
            auto* vrPlayer = f4vr::getPlayer();
            if (vrPlayer && vrPlayer->GetWeaponMagicDrawn()) return;
            auto& menuChecker = MenuChecker::GetSingleton();
            if (menuChecker.IsScopeOpen()) return;
        }

        RE::NiPoint3 fingerPos = GetFingerPosition();
        if (fingerPos.x == 0.0f && fingerPos.y == 0.0f && fingerPos.z == 0.0f) {
            // During intro playback, FRIK finger tracking may be unavailable.
            // Fall back to controller (wand node) position.
            if (_introPlaybackActive) {
                auto* playerNodes2 = f4cf::f4vr::getPlayerNodes();
                bool isLeft = VRInput::GetSingleton().IsLeftHandedMode();
                // SecondaryWandNode = LEFT hand, primaryWandNode = RIGHT hand
                // For eject button on left-wrist Pipboy, we need the RIGHT hand (index finger)
                auto* wand = playerNodes2 ? (isLeft
                    ? playerNodes2->SecondaryWandNode   // left-handed: right wrist has Pipboy, left hand presses
                    : playerNodes2->primaryWandNode)    // right-handed: left wrist has Pipboy, right hand presses
                    : nullptr;
                if (wand) {
                    fingerPos = wand->world.translate;
                    spdlog::debug("[PIPBOY] Wand fallback: pos=({:.1f},{:.1f},{:.1f})",
                                 fingerPos.x, fingerPos.y, fingerPos.z);
                }
            }
            if (fingerPos.x == 0.0f && fingerPos.y == 0.0f && fingerPos.z == 0.0f) {
                return;
            }
        }

        RE::NiPoint3 diff = fingerPos - ejectContactTarget->world.translate;
        float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

        // Periodic distance log (every ~5 seconds at 90fps)
        static int distanceLogCounter = 0;
        if (++distanceLogCounter >= 450) {
            distanceLogCounter = 0;
            spdlog::debug("[PIPBOY-DIAG] finger=({:.1f},{:.1f},{:.1f}) deckTarget=({:.1f},{:.1f},{:.1f}) dist={:.1f} enter={:.1f} exit={:.1f} latched={} cooldown={:.2f} state={}",
                fingerPos.x, fingerPos.y, fingerPos.z,
                ejectContactTarget->world.translate.x, ejectContactTarget->world.translate.y, ejectContactTarget->world.translate.z,
                distance, EJECT_CONTACT_ENTER, EJECT_CONTACT_EXIT, _ejectContactLatched,
                _ejectCooldown, static_cast<int>(_tapeDeckState));
        }

        // Sticky contact with release hysteresis prevents one touch from toggling more than once.
        if (distance > EJECT_CONTACT_EXIT) {
            _ejectContactLatched = false;
            if (animateNode) animateNode->local.translate.z = _buttonOriginalZ;
            return;
        }

        // The 5..7 zone retains the latch but is outside the press surface.
        if (distance > EJECT_CONTACT_ENTER) {
            if (animateNode) animateNode->local.translate.z = _buttonOriginalZ;
            return;
        }

        // Preserve the small visual depression without using the animation pivot for collision.
        if (animateNode && _buttonOriginalZSet) {
            const float press = std::clamp(
                (EJECT_CONTACT_ENTER - distance) / EJECT_CONTACT_ENTER, 0.0f, 1.0f);
            animateNode->local.translate.z = _buttonOriginalZ + EJECT_Z_MIN * press;
        }

        // Latch on entry even during cooldown; a rejected touch must leave the 7-unit
        // release zone before it can become a new press.
        if (!_ejectContactLatched) {
            _ejectContactLatched = true;
            if (_ejectCooldown > 0.0f) return;

            spdlog::info("[PIPBOY] Eject button pressed via TapeDeck01 contact (dist={:.2f})", distance);
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
                _holotapeGrabCooldown = 0.5f;  // Prevent immediate insertion when deck opens

                if (_introWristTransitionFormID != 0) {
                    spdlog::debug("[PIPBOY] Cancelled staged intro wrist transition (deck opened)");
                    ResetIntroWristTransition();
                    EndWristOverrideForHolotape();
                    RestoreIntroHolotapeType(_introHolotapeFormID);
                }

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

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        // Use cached nodes to avoid per-frame tree searches
        RE::NiAVObject* tapeDeckNode = GetCachedTapeDeckNode();
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
        // This is OUTSIDE the interpolation block because the push system can set
        // _tapeDeckAnimProgress to 1.0 directly (finger moved beyond PUSH_ENTER_DISTANCE
        // during Pushing), then transition to Opening — which would skip the interpolation
        // block entirely (|1.0 - 1.0| < 0.001) and leave the state stuck in Opening.
        if (std::fabs(_tapeDeckAnimProgress - 1.0f) < 0.001f) {
            if (_tapeDeckState == TapeDeckState::Opening) {
                _tapeDeckState = TapeDeckState::Open;
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
        if (_holotapeGrabCooldown > 0.0f) return;

        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        RE::NiAVObject* tapeDeck = GetCachedTapeDeckNode();
        if (!tapeDeck) return;

        // The grabbing hand is the non-pipboy hand. Pipboy is always on the left
        // wrist (doesn't swap in LH mode), so the grab hand is always the right hand.
        bool grabHandIsLeft = false;

        // Check VR grip input directly (bypasses Hand state machine which may
        // have already consumed the grip press for a different grab this frame)
        bool gripPressed = VRInput::GetSingleton().IsPressed(grabHandIsLeft, VRButton::Grip);
        if (!gripPressed) return;

        // Use VR controller position (not fingertip) for removal detection.
        // Controller position is more predictable and matches where the player
        // perceives their hand to be when gripping near the tape deck.
        auto& heisenberg = Heisenberg::GetSingleton();
        Hand* grabHand = grabHandIsLeft ? heisenberg.GetLeftHand() : heisenberg.GetRightHand();
        if (!grabHand) return;

        RE::NiPoint3 handPos = grabHand->GetPosition();

        // Measure distance to TapREF (where the holotape visually appears),
        // not to the tape deck tray which rotates away when the deck opens.
        RE::NiPoint3 targetPos = _cachedTapeRef ? _cachedTapeRef->world.translate : tapeDeck->world.translate;
        RE::NiPoint3 diff = handPos - targetPos;
        float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

        auto& grabMgr = GrabManager::GetSingleton();
        bool alreadyGrabbing = grabMgr.IsGrabbing(grabHandIsLeft);

        spdlog::debug("[PIPBOY] Holotape removal check: grip=true dist={:.1f} radius={:.1f} alreadyGrab={} "
                     "hand=({:.1f},{:.1f},{:.1f}) target=({:.1f},{:.1f},{:.1f})",
                     distance, TAPE_GRAB_RADIUS, alreadyGrabbing,
                     handPos.x, handPos.y, handPos.z,
                     targetPos.x, targetPos.y, targetPos.z);

        if (distance > TAPE_GRAB_RADIUS) return;

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
        StopIntroPlayback();  // Cancel intro audio if playing
        RestoreIntroHolotapeType(_introHolotapeFormID);
        spdlog::debug("[PIPBOY] Player grabbing holotape out of deck! FormID={:08X} dist={:.1f} alreadyGrabbing={}",
            _loadedHolotapeFormID, distance, alreadyGrabbing);

        std::uint32_t holotapeFormID = _loadedHolotapeFormID;

        // Tape removed — restore projected mode if we overrode it for playback.
        ResetIntroWristTransition();
        EndWristOverrideForHolotape();

        // Hide TapREF and clear loaded state
        _holotapeLoaded = false;
        _loadedHolotapeFormID = 0;
        _pendingHolotapeRefrID = 0;  // Clear pending ref to prevent stale playback
        _holotapeGrabCooldown = TAPE_GRAB_COOLDOWN;
        _tapREFForceHidden = true;   // Persistent flag — overrides per-frame visibility
        _insertionOpenHandActive = false;  // Player is now holding the holotape — let grab system handle curls
        SetTapREFVisible(false);

        // Holster weapon if drawn — weapon on primary wand conflicts with holotape grab.
        // The holotape is always dropped to the primary (weapon) wand, so weapon must be put away.
        auto* vrPlayer = f4vr::getPlayer();
        auto* rePlayer = RE::PlayerCharacter::GetSingleton();
        if (vrPlayer && rePlayer && vrPlayer->GetWeaponMagicDrawn()) {
            rePlayer->DrawWeaponMagicHands(false);
            _holsteredWeaponForHolotape = true;
            spdlog::debug("[PIPBOY] Holstered weapon for holotape removal");
        }

        // Drop the holotape from inventory to the grabbing hand (primary wand).
        // Skip if the grab system already picked up an object this frame —
        // the player already has something in hand, DropToHand would conflict.
        if (!alreadyGrabbing) {
            DropToHand::GetSingleton().QueueDropToHand(holotapeFormID, grabHandIsLeft, 1, false, false);
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

            // Re-draw weapon if we holstered it for holotape removal
            if (_holsteredWeaponForHolotape) {
                auto* player2 = RE::PlayerCharacter::GetSingleton();
                if (player2) {
                    player2->DrawWeaponMagicHands(true);
                    spdlog::debug("[PIPBOY] Re-drawing weapon after holotape insertion");
                }
                _holsteredWeaponForHolotape = false;
            }

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
            _holotapeGrabCooldown = 0.5f;  // Brief cooldown to prevent still-held grip from re-triggering removal
            _slamCooldown = 0.5f;   // Prevent hand push from immediately closing deck after insertion
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
    // Hand push / slam close — track hand distance to deck for physical push.
    // When the hand enters the push zone, the deck rotation follows the hand.
    // If pushed past the commit threshold, auto-close. Otherwise spring back.
    // Fast hand motion (slam) bypasses push tracking and closes immediately.
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::CheckHandPush()
    {
        RE::NiAVObject* arm = GetPipboyArmNode();
        if (!arm) return;

        RE::NiAVObject* tapeDeck = GetCachedTapeDeckNode();
        if (!tapeDeck) return;

        // Use the same finger position as the eject button (matches FRIK's light button pattern).
        // Previously this used pushHand->GetPosition() (VR controller world pos) which gave
        // distances of 25-35 units — far too large for meaningful interaction.
        RE::NiPoint3 fingerPos = GetFingerPosition();
        if (fingerPos.x == 0.0f && fingerPos.y == 0.0f && fingerPos.z == 0.0f) {
            return;
        }
        RE::NiPoint3 deckPos = tapeDeck->world.translate;
        RE::NiPoint3 diff = fingerPos - deckPos;
        float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

        // The pushing hand is the non-pipboy hand. Pipboy always on left wrist,
        // so push hand is always right.
        bool pushHandIsLeft = false;

        // Don't push if hand is actively grabbing something.
        // Use GrabManager (authoritative) rather than Hand::IsHolding() because
        // EndGrab-for-storage clears GrabState but doesn't reset Hand._state,
        // which would leave IsHolding() stuck at true after holotape insertion.
        auto& grabMgr = GrabManager::GetSingleton();
        if (grabMgr.IsGrabbing(pushHandIsLeft)) {
            if (_tapeDeckState == TapeDeckState::Pushing) {
                // Hand started holding while pushing — spring back
                _tapeDeckState = TapeDeckState::Opening;
                _tapeDeckOpen = true;
            }
            return;
        }

        // Don't push when grip is pressed and a holotape is loaded — user wants removal, not push-close.
        // CheckHandPush runs before CheckHolotapeRemoval, so without this guard the push can
        // commit to Closing before removal gets a chance to fire.
        if (_holotapeLoaded && VRInput::GetSingleton().IsPressed(pushHandIsLeft, VRButton::Grip)) {
            if (_tapeDeckState == TapeDeckState::Pushing) {
                _tapeDeckState = TapeDeckState::Open;
                _tapeDeckAnimProgress = 1.0f;
                _tapeDeckOpen = true;
            }
            return;
        }

        // ── Currently in Pushing state ──
        if (_tapeDeckState == TapeDeckState::Pushing) {
            if (distance > PUSH_EXIT_DISTANCE) {
                // Finger left push zone — spring back to open
                spdlog::debug("[PIPBOY] Finger left push zone (dist={:.1f}) — returning to Open", distance);
                _tapeDeckState = TapeDeckState::Opening;
                _tapeDeckOpen = true;
                return;
            }

            // Map finger distance to deck progress: at PUSH_ENTER_DISTANCE → 1.0 (open),
            // at PUSH_CLOSED_DISTANCE → 0.0 (closed).  Like FRIK's fz = 0 - (range - dist).
            float t = (distance - PUSH_CLOSED_DISTANCE) / (PUSH_ENTER_DISTANCE - PUSH_CLOSED_DISTANCE);
            _pushProgress = std::clamp(t, 0.0f, 1.0f);
            _tapeDeckAnimProgress = _pushProgress;  // Directly drive the animation

            // Check if pushed past commit threshold
            if (_pushProgress <= PUSH_COMMIT_PROGRESS) {
                _tapeDeckOpen = false;
                _tapeDeckState = TapeDeckState::Closing;
                _closeAnimSpeed = 0.15f;  // Gentle auto-close from commit
                PlayWavSound("Slam close.wav");
                VRInput::GetSingleton().TriggerHaptic(pushHandIsLeft, 5000);
                spdlog::debug("[PIPBOY] Push commit! progress={:.2f} dist={:.1f}", _pushProgress, distance);
            }
            return;
        }

        // ── Not currently pushing — check for entry into push zone ──
        if (_tapeDeckState != TapeDeckState::Open) return;
        if (_slamCooldown > 0.0f) return;

        // Periodic debug log
        if (_logCooldown <= 0) {
            _logCooldown = 60;
            spdlog::debug("[PIPBOY] Push check: fingerDist={:.1f} (enter<{:.1f})",
                distance, PUSH_ENTER_DISTANCE);
        }

        // Finger close enough to enter push tracking
        if (distance > PUSH_ENTER_DISTANCE) return;

        // Enter push tracking — finger is touching the deck area
        _tapeDeckState = TapeDeckState::Pushing;
        float initT = (distance - PUSH_CLOSED_DISTANCE) / (PUSH_ENTER_DISTANCE - PUSH_CLOSED_DISTANCE);
        _pushProgress = std::clamp(initT, 0.0f, 1.0f);
        _tapeDeckAnimProgress = _pushProgress;

        VRInput::GetSingleton().TriggerHaptic(pushHandIsLeft, 2000);
        spdlog::debug("[PIPBOY] Entering push mode fingerDist={:.1f} progress={:.2f}", distance, _pushProgress);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Tape deck state helpers
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::CloseTapeDeck()
    {
        if (_tapeDeckState != TapeDeckState::Closed && _tapeDeckState != TapeDeckState::Closing) {
            _tapeDeckOpen = false;
            _tapeDeckState = TapeDeckState::Closing;
            _insertionOpenHandActive = false;
            spdlog::debug("[PIPBOY] Force-closing tape deck");
        }
    }

    void PipboyInteraction::EjectCurrentHolotape()
    {
        if (_holotapeLoaded) {
            if (_introWristTransitionFormID != 0) {
                ResetIntroWristTransition();
                EndWristOverrideForHolotape();
            }
            StopIntroPlayback();  // Cancel intro audio if playing
            RestoreIntroHolotapeType(_introHolotapeFormID);
            spdlog::debug("[PIPBOY] Ejecting holotape (FormID {:08X})", _loadedHolotapeFormID);
            _holotapeLoaded = false;
            _loadedHolotapeFormID = 0;
            _pendingHolotapeRefrID = 0;
            SetTapREFVisible(false);
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
        ResetIntroWristTransition();
        EndWristOverrideForHolotape();
        RestoreIntroHolotapeType(formID);

        _holotapeLoaded = false;
        _loadedHolotapeFormID = 0;
        _pendingHolotapeRefrID = 0;
        _tapREFForceHidden = true;
        _holotapeGrabCooldown = TAPE_GRAB_COOLDOWN;
        _insertionOpenHandActive = false;
        _deckOpenedByEject = false;
        SetTapREFVisible(false);

        auto* boundObj = form->As<RE::TESBoundObject>();
        if (boundObj) player->PlayPickUpSound(boundObj, true, false);
        DropToHand::GetSingleton().QueueDropToHand(formID, false, 1, true, false);
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
    // Holotape insertion zone
    // ════════════════════════════════════════════════════════════════════════

    bool PipboyInteraction::IsInTapeDeckSlotZone(const RE::NiPoint3& pos) const
    {
        RE::NiAVObject* arm = const_cast<PipboyInteraction*>(this)->GetPipboyArmNode();
        if (!arm) return false;

        RE::NiAVObject* tapeDeck = const_cast<PipboyInteraction*>(this)->GetCachedTapeDeckNode();
        if (!tapeDeck) return false;

        RE::NiPoint3 slotPos = tapeDeck->world.translate;
        RE::NiPoint3 diff = pos - slotPos;
        float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

        return distance < TAPE_SLOT_RADIUS;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Holotape insertion
    // ════════════════════════════════════════════════════════════════════════

    bool PipboyInteraction::InsertHolotape(RE::TESObjectREFR* holotapeRef)
    {
        if (!holotapeRef) {
            spdlog::warn("[PIPBOY] InsertHolotape: null ref");
            return false;
        }

        auto* baseForm = holotapeRef->GetObjectReference();
        if (!baseForm || baseForm->GetFormType() != RE::ENUM_FORM_ID::kNOTE) {
            spdlog::warn("[PIPBOY] InsertHolotape: not a holotape");
            return false;
        }

        std::uint32_t formID = baseForm->GetFormID();
        std::string name(RE::TESFullName::GetFullName(*baseForm, false));

        spdlog::info("[PIPBOY] Holotape inserted: '{}' ({:08X})", name.empty() ? "unknown" : name, formID);

        // Disable the world object so it can't be re-grabbed.
        // Do NOT call SetDelete — see CheckHolotapeInsertion comment for details.
        heisenberg::SafeDisable(holotapeRef);

        // Add holotape to inventory NOW so it's available for removal via DropToHand
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                heisenberg::AddObjectToContainer(player, static_cast<RE::TESBoundObject*>(baseForm),
                                                 &nullExtra, 1, nullptr, 0);
                spdlog::debug("[PIPBOY] Added holotape {:08X} to inventory at insertion", formID);
            }
        }

        _holotapeLoaded = true;
        _loadedHolotapeFormID = formID;

        SetTapREFVisible(true);

        // Keep deck open — player must close it

        VRInput::GetSingleton().TriggerHaptic(false, 3000);
        VRInput::GetSingleton().TriggerHaptic(true, 3000);

        spdlog::info("[PIPBOY] Holotape inserted, deck stays open");
        PlayWavSound("Holotape place inside tray.wav");
        return true;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Reset all state on save/load — prevents stale transforms and ghost holotapes
    // ════════════════════════════════════════════════════════════════════════

    void PipboyInteraction::ClearState()
    {
        spdlog::info("[PIPBOY] ClearState — resetting all tape deck state for load");

        // Safety: never leave projected mode overridden across a load.
        ResetIntroWristTransition();
        EndWristOverrideForHolotape();

        _tapeDeckState          = TapeDeckState::Closed;
        _tapeDeckOpen           = false;
        _tapeDeckAnimProgress   = 0.0f;
        _ejectCooldown          = 0.0f;
        _insertionOpenHandActive = false;
        _deckOpenedByEject      = false;
        _holotapeLoaded         = false;
        _loadedHolotapeFormID   = 0;
        _pendingHolotapeRefrID  = 0;
        _logCooldown            = 0;
        _buttonOriginalZ        = 0.0f;
        _buttonOriginalZSet     = false;
        _ejectContactLatched    = false;
        _tapeRefInitialHideDone = false;
        _holotapeGrabCooldown   = 0.0f;
        _closeAnimSpeed         = ANIM_SPEED;
        _slamCooldown           = 0.0f;
        _pushStartDistance      = 0.0f;
        _pushProgress           = 1.0f;
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
        _fingerPosCached        = false;
        _holsteredWeaponForHolotape = false;
        _pendingPlaybackDelay       = 0.0f;
        _pendingPlaybackFormID      = 0;
        _pendingAudioFormID         = 0;
        _pendingAudioWaitFrames     = 0;
        _pendingProgramFormID       = 0;
        _pendingProgramWaitFrames   = 0;
        _holotapePauseClearFrames   = 0;
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
        _introSWFCloseStep2Done     = false;
        _introBootSoundPlayed       = false;
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
        _consoleOpenDelayFrames     = 0;
        _terminalSoundPending       = false;
        _terminalSoundDelay         = 0;
        _pendingTerminalRedirect    = false;
        _terminalRedirectActive     = false;
        _isWorldTerminalRedirect    = false;
        _terminalPatchesSuspended   = false;
        // Seed the "left the vault" latch: an established character (intro already played) is
        // well past Vault 111, so don't gate their terminal redirect. A fresh playthrough starts
        // false and flips true the moment the player first reaches an exterior cell.
        _hasReachedExterior         = g_config.introHolotapePlayed;
        // QueueIntroHolotapeDelivery seeds this true for a post-load save that already owns
        // the Pip-Boy; pre-pickup/new-game paths leave it false until the boot open+close.
        _pipboyOpenedSinceAcquire   = false;
        _terminalScreenNode         = nullptr;
        _savedDiffuseSRV            = 0;
        _savedRendererPtr           = nullptr;
        _savedOrigWorldRoot         = 0;
        _savedHmdScreenNode         = nullptr;
        _savedHmdScreenParent       = nullptr;
        _worldTerminalChecked       = false;
        _wtWaitFrames               = 0;
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

        // Sheathe any drawn weapon so the player's hands are free for the intro.
        player->DrawWeaponMagicHands(false);

        PlayWavSound("Eject with holotape inside.wav");
        IntroCeremonyState::MarkComplete();
        spdlog::info("[INTRO] Intro holotape delivered — deck open, weapon sheathed");
    }

    void PipboyInteraction::StartIntroPlayback()
    {
        if (_introLines.empty()) {
            spdlog::warn("[INTRO] No intro lines to play");
            return;
        }

        // Stop any previous playback
        StopIntroPlayback();

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
        heisenberg::HideSubtitle();
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
