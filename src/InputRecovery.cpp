#include "InputRecovery.h"

#include "Config.h"
#include "ItemPositionConfigMode.h"
#include "MenuChecker.h"
#include "f4vr/F4VRUtils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

/*
 * POST-LOAD INPUT RECOVERY (Jul 26).
 *
 * Symptom being chased: adding/updating Heisenberg on an existing save (including a save that
 * already serialized a prior Heisenberg/menu input state) can leave the player unable to move or
 * jump, while looking/turning, menus, and VR tracking still work. Movement+jump dead with looking
 * alive is the signature of the engine's player-control gating (USER_EVENT_FLAG kMovement /
 * kJumping), NOT of a dead controller axis and NOT of FRIK's thumbstick deadzone trick (a deadzone
 * cannot disable a jump BUTTON).
 *
 * Fallout 4 gates player input through BSInputEnableManager, which owns an array of input-enable
 * LAYERS (each with a debug name) plus force-enable override words. Scripts disable controls by
 * allocating a layer and clearing flags on it; that layer state travels in the savegame. If a save
 * captured such a layer and the script that would have removed it has already run, the layer is
 * restored on load and nothing ever clears it.
 *
 * Heisenberg already had a partial workaround (MenuChecker.cpp, LoadingMenu close ->
 * SetActorRestrained(player,false)) for a related FRIK bug. That covers only the actor RESTRAINED
 * flag, which is a different mechanism, and it fires exactly once at a moment when the engine may
 * still be finishing its save-load restore (BSInputEnableManager even carries an
 * isCurrentlyInSaveLoad flag). This module therefore does two things:
 *
 *   1. DIAGNOSE. Dump the full input-enable state at several post-load checkpoints: the cached and
 *      force flag words, every layer with its engine debug name and flags, and a raw dword dump of
 *      the manager. The raw dump matters because CommonLibF4's struct offsets are known to be
 *      partially wrong for VR (its own header carries "TODO: in VR this affects the other input
 *      events instead" comments), so the typed reads below are corroborated by, not trusted over,
 *      the raw words. The layer debug names should name the culprit outright.
 *
 *   2. HEAL. Release only positively identified input layers whose owning menu is no longer open,
 *      then perform the legacy one-shot post-load actor-unrestrain used for old saves. We never
 *      write BSInputEnableManager's process-global force-enable words: those have no ownership
 *      model, survive after this watchdog disarms, and can override another mod or menu.
 *
 * SEH rule for this file: every raw/engine read happens inside a wrapper function whose __try
 * frame constructs no C++ objects (MSVC C2712). Formatting and logging happen after the guarded
 * capture returns.
 */

namespace heisenberg::InputRecovery
{
    namespace
    {
        using UEFlag = RE::UserEvents::USER_EVENT_FLAG;

        // VERIFIED-LIVE raw offsets into BSInputEnableManager (see CaptureImpl for the proof).
        constexpr std::size_t kOffCachedUser = 0x118;
        constexpr std::size_t kOffCachedOther = 0x11C;
        constexpr std::size_t kOffForceUser = 0x120;
        constexpr std::size_t kOffForceOther = 0x124;
        constexpr std::uint32_t kMovementJumpMask = (1u << 0) | (1u << 10);

        // The user-event bits a stale control-disable layer strips. Measured from the failing
        // save: a leftover 'PauseMenu Input Layer' carried 0xFFFFFEBA, i.e. Movement, Activate,
        // Fighting and MainFour cleared, everything else (Looking, Menu, Jumping...) left alone —
        // which is exactly "cannot move, cannot act, CAN turn and open menus".
        constexpr std::uint32_t kAllUserEvents = 0xFFFFFFFFu;

        // Offsets/size of the region we raw-dump out of BSInputEnableManager (0x110..0x180).
        constexpr std::size_t kDumpBase = 0x110;
        constexpr std::size_t kDumpWords = 28;
        constexpr std::uint32_t kMaxLayers = 12;
        constexpr std::size_t kMaxNameLen = 48;

        // Checkpoints measured from LoadingMenu close. The first is immediate; the later ones exist
        // because the engine can still be restoring state when the loading menu closes, and because
        // FRIK re-applies its own control state on its first frames after the skeleton rebuild.
        // The window runs out to 30s deliberately: the reported symptom points at the actor
        // RESTRAINED flag, and whatever sets it does so at an unknown time after the load, so a
        // short window could re-assert movement before the restrain lands and then stop watching.
        // Each pass is an idempotent no-op when the player is already free.
        constexpr std::uint64_t kCheckpointsMs[] = {
            0, 750, 1500, 3000, 5000, 8000, 12000, 17000, 23000, 30000
        };
        constexpr std::size_t kCheckpointCount = std::size(kCheckpointsMs);

        std::atomic<std::uint64_t> s_loadCompleteTick{ 0 };
        std::atomic<std::uint32_t> s_nextCheckpoint{ kCheckpointCount };
        // Retain the old-save movement recovery, but make it a one-shot instead of repeatedly
        // clearing actor restraint for the full 30-second watchdog window.
        std::atomic<bool> s_postLoadUnrestrainPending{ false };

        struct Snapshot
        {
            std::uint32_t words[kDumpWords]{};
            std::uint32_t cachedUser = 0;
            std::uint32_t cachedOther = 0;
            std::uint32_t forceUser = 0;
            std::uint32_t forceOther = 0;
            std::uint32_t layerCount = 0;
            std::uint32_t nameCount = 0;
            std::uint32_t layerUser[kMaxLayers]{};
            std::uint32_t layerOther[kMaxLayers]{};
            char          layerNames[kMaxLayers][kMaxNameLen]{};
            std::uint8_t  inSaveLoad = 0;
        };

        // Full C++ is fine here - this is called from inside a __try in CaptureGuarded, but it is a
        // separate frame, so no unwinding object lives in the guarded frame itself.
        void CaptureImpl(RE::BSInputEnableManager* mgr, Snapshot& out)
        {
            const auto* base = reinterpret_cast<const std::uint8_t*>(mgr);
            std::memcpy(out.words, base + kDumpBase, sizeof(out.words));

            // RAW offsets, NOT the typed members. Measured live 2026-07-27: the compiler places
            // these four EnumSets 4 bytes EARLIER than the offsets CommonLibF4's own header
            // documents, so `mgr->cachedInputUserEventsFlags` actually reads 0x114 and
            // `mgr->forceEnableInputUserEventsFlags` reads 0x11C. Proof from the same dump: the
            // AND of every layer's inputUserEvents equalled the word at raw 0x118 exactly
            // (0xFFFFFEBA with a stale PauseMenu layer, 0xFFFFFFFF without), which identifies
            // 0x118 as the real cached user-event mask. This is what CommonLibF4's own
            // "TODO: in VR this affects the other input events instead" comments are describing.
            std::memcpy(&out.cachedUser, base + kOffCachedUser, sizeof(out.cachedUser));
            std::memcpy(&out.cachedOther, base + kOffCachedOther, sizeof(out.cachedOther));
            std::memcpy(&out.forceUser, base + kOffForceUser, sizeof(out.forceUser));
            std::memcpy(&out.forceOther, base + kOffForceOther, sizeof(out.forceOther));
            out.inSaveLoad = mgr->isCurrentlyInSaveLoad ? 1u : 0u;

            out.layerCount = static_cast<std::uint32_t>(mgr->layers.size());
            const std::uint32_t layerCap = out.layerCount < kMaxLayers ? out.layerCount : kMaxLayers;
            for (std::uint32_t i = 0; i < layerCap; ++i) {
                out.layerUser[i] = mgr->layers[i].inputUserEvents.underlying();
                out.layerOther[i] = mgr->layers[i].otherInputEvents.underlying();
            }

            out.nameCount = static_cast<std::uint32_t>(mgr->debugNames.size());
            const std::uint32_t nameCap = out.nameCount < kMaxLayers ? out.nameCount : kMaxLayers;
            for (std::uint32_t i = 0; i < nameCap; ++i) {
                if (const char* name = mgr->debugNames[i].c_str()) {
                    std::strncpy(out.layerNames[i], name, kMaxNameLen - 1);
                }
            }
        }

        bool CaptureGuarded(RE::BSInputEnableManager* mgr, Snapshot& out)
        {
            __try {
                CaptureImpl(mgr, out);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        std::string DescribeUserFlags(std::uint32_t value)
        {
            static constexpr struct
            {
                std::uint32_t bit;
                const char*   name;
            } kBits[] = {
                { 1u << 0, "Movement" }, { 1u << 1, "Looking" }, { 1u << 2, "Activate" },
                { 1u << 3, "Menu" }, { 1u << 4, "Console" }, { 1u << 5, "POVSwitch" },
                { 1u << 6, "Fighting" }, { 1u << 7, "Sneaking" }, { 1u << 8, "MainFour" },
                { 1u << 9, "WheelZoom" }, { 1u << 10, "Jumping" }, { 1u << 11, "VATS" },
            };
            std::string result;
            for (const auto& entry : kBits) {
                if ((value & entry.bit) != 0) {
                    if (!result.empty()) {
                        result += '|';
                    }
                    result += entry.name;
                }
            }
            return result.empty() ? std::string("none") : result;
        }

        // Something that legitimately owns player controls right now. While one of these holds we
        // observe but never heal, so a real scripted lock (dialogue, terminal, workshop, a loading
        // screen) is left exactly as the game intends.
        //
        // ItemPositionConfigMode is in this list because it restrains the player ON PURPOSE
        // (ItemPositionConfigMode.cpp:39) and re-asserts that every frame; without this guard the
        // watchdog and the config mode would fight each other over the same flags.
        bool BlockingMenuOpen()
        {
            if (ItemPositionConfigMode::GetSingleton().IsRepositionModeActive()) {
                return true;
            }
            const auto& menus = MenuChecker::GetSingleton();
            return menus.IsMenuOpen("LoadingMenu") ||
                menus.IsMenuOpen("DialogueMenu") ||
                menus.IsMenuOpen("TerminalMenu") ||
                menus.IsMenuOpen("WorkshopMenu") ||
                menus.IsMenuOpen("PipboyMenu") ||
                menus.IsMenuOpen("PauseMenu") ||
                menus.IsMenuOpen("MainMenu");
        }
    }

    namespace
    {
        // A layer whose owning menu is closed but whose disable mask is still applied. Keyed by the
        // engine's own debug name (BSInputEnableManager::debugNames), so we only ever release a
        // layer we can positively identify AND whose menu we can positively observe as shut.
        struct StrandedLayerRule
        {
            const char* namePrefix;
            const char* menuName;
        };
        constexpr StrandedLayerRule kStrandedLayerRules[] = {
            { "PauseMenu Input Layer", "PauseMenu" },
        };

        // Returns the number of layers repaired. Full C++ body; the caller SEH-wraps it.
        int ReleaseStrandedMenuLayersImpl(RE::BSInputEnableManager* mgr, const char* tag)
        {
            const auto& menus = MenuChecker::GetSingleton();
            const std::uint32_t layerCount = static_cast<std::uint32_t>(mgr->layers.size());
            const std::uint32_t nameCount = static_cast<std::uint32_t>(mgr->debugNames.size());
            const std::uint32_t count = (std::min)(layerCount, nameCount);

            int repaired = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                const char* name = mgr->debugNames[i].c_str();
                if (!name || name[0] == '\0') {
                    continue;
                }
                for (const auto& rule : kStrandedLayerRules) {
                    if (std::strncmp(name, rule.namePrefix, std::strlen(rule.namePrefix)) != 0) {
                        continue;
                    }
                    if (menus.IsMenuOpen(rule.menuName)) {
                        continue;  // legitimately held — the menu really is open
                    }
                    const std::uint32_t user = mgr->layers[i].inputUserEvents.underlying();
                    const std::uint32_t other = mgr->layers[i].otherInputEvents.underlying();
                    if (user == kAllUserEvents && other == kAllUserEvents) {
                        continue;  // already permissive, nothing stranded
                    }
                    mgr->layers[i].inputUserEvents = static_cast<UEFlag>(kAllUserEvents);
                    mgr->layers[i].otherInputEvents = static_cast<RE::OEFlag>(kAllUserEvents);
                    ++repaired;
                    spdlog::warn(
                        "[INPUT-DIAG] {}: released STRANDED layer[{}] '{}' ({} is closed) "
                        "user 0x{:08X}->0xFFFFFFFF other 0x{:08X}->0xFFFFFFFF",
                        tag, i, name, rule.menuName, user, other);
                }
            }

            if (repaired > 0) {
                // The engine caches the effective mask; recompute it the way the engine does (AND
                // across every layer) and write it through the VERIFIED raw offsets, otherwise the
                // repair would not take effect until something else happened to trigger a rebuild.
                std::uint32_t user = kAllUserEvents;
                std::uint32_t other = kAllUserEvents;
                for (std::uint32_t i = 0; i < layerCount; ++i) {
                    user &= mgr->layers[i].inputUserEvents.underlying();
                    other &= mgr->layers[i].otherInputEvents.underlying();
                }
                auto* base = reinterpret_cast<std::uint8_t*>(mgr);
                std::memcpy(base + kOffCachedUser, &user, sizeof(user));
                std::memcpy(base + kOffCachedOther, &other, sizeof(other));
                spdlog::warn(
                    "[INPUT-DIAG] {}: recomputed cached masks user=0x{:08X} other=0x{:08X} "
                    "after releasing {} stranded layer(s)",
                    tag, user, other, repaired);
            }
            return repaired;
        }

        int ReleaseStrandedMenuLayers(RE::BSInputEnableManager* mgr, const char* tag)
        {
            if (!mgr) {
                return 0;
            }
            __try {
                return ReleaseStrandedMenuLayersImpl(mgr, tag);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }
    }

    void LogInputEnableState(const char* tag)
    {
        // Layer-name copies, flag descriptions, and the raw manager dump are
        // forensic diagnostics. Avoid all of that work in normal release
        // logging; targeted recovery below still runs at every checkpoint.
        if (!spdlog::should_log(spdlog::level::debug)) {
            return;
        }

        auto* mgr = RE::BSInputEnableManager::GetSingleton();
        if (!mgr) {
            spdlog::warn("[INPUT-DIAG] {}: BSInputEnableManager singleton is null", tag);
            return;
        }

        Snapshot snap{};
        if (!CaptureGuarded(mgr, snap)) {
            spdlog::warn("[INPUT-DIAG] {}: capture faulted reading BSInputEnableManager", tag);
            return;
        }

        spdlog::debug(
            "[INPUT-DIAG] {}: cachedUser=0x{:08X} ({}) cachedOther=0x{:08X} "
            "forceUser=0x{:08X} ({}) forceOther=0x{:08X} layers={} names={} inSaveLoad={}",
            tag,
            snap.cachedUser,
            DescribeUserFlags(snap.cachedUser),
            snap.cachedOther,
            snap.forceUser,
            DescribeUserFlags(snap.forceUser),
            snap.forceOther,
            snap.layerCount,
            snap.nameCount,
            snap.inSaveLoad);

        const std::uint32_t layerCap = snap.layerCount < kMaxLayers ? snap.layerCount : kMaxLayers;
        for (std::uint32_t i = 0; i < layerCap; ++i) {
            spdlog::debug(
                "[INPUT-DIAG] {}:   layer[{}] '{}' user=0x{:08X} ({}) other=0x{:08X}",
                tag,
                i,
                (i < snap.nameCount && snap.layerNames[i][0] != '\0') ? snap.layerNames[i] : "(unnamed)",
                snap.layerUser[i],
                DescribeUserFlags(snap.layerUser[i]),
                snap.layerOther[i]);
        }

        // Raw words: ground truth if the typed offsets above are wrong for VR.
        std::string raw;
        raw.reserve(kDumpWords * 14);
        for (std::size_t i = 0; i < kDumpWords; ++i) {
            char chunk[24]{};
            std::snprintf(chunk, sizeof(chunk), "%03zX=%08X ", kDumpBase + i * 4, snap.words[i]);
            raw += chunk;
        }
        spdlog::debug("[INPUT-DIAG] {}:   raw {}", tag, raw);
    }

    void OnLoadComplete()
    {
        s_loadCompleteTick.store(GetTickCount64(), std::memory_order_release);
        s_nextCheckpoint.store(0, std::memory_order_release);
        s_postLoadUnrestrainPending.store(true, std::memory_order_release);
        spdlog::info("[INPUT-DIAG] post-load input watchdog armed ({} checkpoints)", kCheckpointCount);
    }

    void Tick()
    {
        const std::uint32_t next = s_nextCheckpoint.load(std::memory_order_acquire);
        if (next >= kCheckpointCount) {
            return;  // disarmed - the overwhelmingly common path
        }
        const std::uint64_t armedAt = s_loadCompleteTick.load(std::memory_order_acquire);
        if (armedAt == 0) {
            return;
        }
        if (GetTickCount64() - armedAt < kCheckpointsMs[next]) {
            return;
        }
        s_nextCheckpoint.store(next + 1, std::memory_order_release);

        char tag[48]{};
        std::snprintf(tag, sizeof(tag), "postload+%llums", static_cast<unsigned long long>(kCheckpointsMs[next]));
        LogInputEnableState(tag);

        if (!g_config.postLoadInputRecovery) {
            return;
        }
        if (BlockingMenuOpen()) {
            spdlog::debug("[INPUT-DIAG] {}: heal skipped - a control-owning menu is open", tag);
            return;
        }

        // Preserve the old-save movement repair, but perform it once at the first checkpoint at
        // which no legitimate control-owning menu is open. Repeating this for 30 seconds could
        // continually defeat a scripted restraint that begins after load.
        bool unrestrained = false;
        if (s_postLoadUnrestrainPending.load(std::memory_order_acquire)) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                f4cf::f4vr::SetActorRestrained(player, false);
                s_postLoadUnrestrainPending.store(false, std::memory_order_release);
                unrestrained = true;
            }
        }

        int releasedLayers = 0;
        if (auto* mgr = RE::BSInputEnableManager::GetSingleton()) {
            // PRIMARY REPAIR: release input-enable layers that a menu stranded in the savegame.
            //
            // Root cause, measured 2026-07-27 on the failing save: layer[0] came back named
            // 'PauseMenu Input Layer' with user=0xFFFFFEBA (Movement/Activate/Fighting/MainFour
            // cleared) while NO pause menu was open — LoadingMenu closed at 00:44:32.999 and the
            // next PauseMenu did not open until 00:46:36. You save FROM the pause menu, so that
            // layer is serialised in the disabled state and, because the menu is never closed
            // again after the load, nothing ever releases it. The effective mask (raw 0x118)
            // equalled that layer exactly, so it alone was gating locomotion.
            //
            // Restoring the layer to fully-permissive is precisely what closing the menu would
            // have done. Only layers whose owning menu is provably NOT open are touched.
            releasedLayers = ReleaseStrandedMenuLayers(mgr, tag);

            Snapshot after{};
            if (CaptureGuarded(mgr, after) &&
                (after.cachedUser & kMovementJumpMask) != kMovementJumpMask) {
                // Unknown/script-owned layers are diagnostic only. Guessing at their ownership
                // is more dangerous than leaving a real quest/cutscene lock intact.
                spdlog::warn(
                    "[INPUT-DIAG] {}: movement remains gated after targeted recovery "
                    "(cachedUser=0x{:08X}, missing=0x{:08X}); no global override applied",
                    tag,
                    after.cachedUser,
                    kMovementJumpMask & ~after.cachedUser);
            }
        }
        if (unrestrained || releasedLayers > 0) {
            spdlog::info(
                "[INPUT-DIAG] {}: targeted recovery applied (actorUnrestrained={}, "
                "strandedLayersReleased={}, globalForceOverride=no)",
                tag,
                unrestrained ? "yes" : "no",
                releasedLayers);
        }
    }
}
