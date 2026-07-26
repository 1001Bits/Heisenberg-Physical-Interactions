#include "MenuChecker.h"
#include "DropToHand.h"
#include "InputRecovery.h"
#include "Utils.h"
#include "f4vr/F4VRUtils.h"

namespace heisenberg
{
    bool MenuChecker::Initialize()
    {
        if (_initialized) {
            return true;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            spdlog::warn("[MenuChecker] UI singleton not available - cannot register for events");
            return false;
        }

        // Register for menu open/close events
        ui->GetEventSource<RE::MenuOpenCloseEvent>()->RegisterSink(&_eventSink);
        
        _initialized = true;
        spdlog::debug("[MenuChecker] Registered for menu open/close events");
        return true;
    }

    bool MenuChecker::IsGameStopped() const
    {
        return _isGameStopped.load(std::memory_order_relaxed);
    }

    bool MenuChecker::IsMenuOpen(const char* menuName) const
    {
        // For commonly checked menus, use the cached atomic flags
        if (strcmp(menuName, "LoadingMenu") == 0) return _isLoading.load(std::memory_order_relaxed);
        if (strcmp(menuName, "PauseMenu") == 0) return _isPaused.load(std::memory_order_relaxed);
        if (strcmp(menuName, "PipboyMenu") == 0) return _isPipboyOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "MainMenu") == 0) return _isMainMenu.load(std::memory_order_relaxed);
        if (strcmp(menuName, "InventoryMenu") == 0) return _isInventoryOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "ContainerMenu") == 0) return _isContainerOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "DialogueMenu") == 0) return _isDialogueOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "Console") == 0) return _isConsoleOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "MessageBoxMenu") == 0) return _isMessageBoxOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "TerminalMenu") == 0) return _isTerminalOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "WorkshopMenu") == 0) return _isWorkshopOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "CookingMenu") == 0) return _isCookingOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "FavoritesMenu") == 0) return _isFavoritesOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "ScopeMenu") == 0) return _isScopeOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "ExamineMenu") == 0) return _isExamineOpen.load(std::memory_order_relaxed);
        if (strcmp(menuName, "ExamineConfirmMenu") == 0) return _isExamineConfirmOpen.load(std::memory_order_relaxed);
        
        // For other menus, fall back to direct UI check (less safe but works)
        auto* ui = RE::UI::GetSingleton();
        if (ui) {
            return ui->GetMenuOpen(menuName);
        }
        return false;
    }

    bool MenuChecker::WasFavoritesRecentlyOpen() const
    {
        if (_isFavoritesOpen.load(std::memory_order_relaxed)) {
            return true;  // Currently open
        }
        double closeTime = _favoritesCloseTime.load(std::memory_order_relaxed);
        if (closeTime <= 0.0) return false;
        double now = Utils::GetTime();
        return (now - closeTime) < 0.5;  // Within 500ms of closing
    }

    bool MenuChecker::IsInMenuCloseCooldown() const
    {
        return _menuCloseCooldown.load(std::memory_order_relaxed) > 0.0f;
    }

    void MenuChecker::UpdateMenuCloseCooldown(float deltaTime)
    {
        float current = _menuCloseCooldown.load(std::memory_order_relaxed);
        if (current > 0.0f) {
            current -= deltaTime;
            if (current <= 0.0f) {
                current = 0.0f;
                spdlog::debug("[MenuChecker] Menu close cooldown ended");
            }
            _menuCloseCooldown.store(current, std::memory_order_relaxed);
        }
    }

    void MenuChecker::ClearState()
    {
        // Re-derive _isLoading from the live UI rather than force-clearing it.
        // kNewGame/kPostLoadGame fire while LoadingMenu is still up (~17s before close),
        // and the MenuOpenCloseEvent sink only re-sets the flag on the CLOSE event.
        // Unconditionally storing false here defeated every IsLoading() hook guard for the
        // load tail, letting per-frame work (incl. hand-collision body creation) run against
        // a still-streaming world. kPreLoadGame fires before LoadingMenu opens, so this
        // yields false there, preserving prior behavior on that path.
        {
            bool loadingNow = false;
            if (auto* ui = RE::UI::GetSingleton()) {
                loadingNow = ui->GetMenuOpen("LoadingMenu");
            }
            _isLoading.store(loadingNow, std::memory_order_relaxed);
        }
        _isPaused.store(false, std::memory_order_relaxed);
        _isPipboyOpen.store(false, std::memory_order_relaxed);
        _isMainMenu.store(false, std::memory_order_relaxed);
        _isInventoryOpen.store(false, std::memory_order_relaxed);
        _isContainerOpen.store(false, std::memory_order_relaxed);
        _isDialogueOpen.store(false, std::memory_order_relaxed);
        _isConsoleOpen.store(false, std::memory_order_relaxed);
        _isMessageBoxOpen.store(false, std::memory_order_relaxed);
        _isTerminalOpen.store(false, std::memory_order_relaxed);
        _isWorkshopOpen.store(false, std::memory_order_relaxed);
        _isCookingOpen.store(false, std::memory_order_relaxed);
        _isFavoritesOpen.store(false, std::memory_order_relaxed);
        _isScopeOpen.store(false, std::memory_order_relaxed);
        _isExamineOpen.store(false, std::memory_order_relaxed);
        _isExamineConfirmOpen.store(false, std::memory_order_relaxed);
        _menuCloseCooldown.store(0.0f, std::memory_order_relaxed);
        _favoritesCloseTime.store(0.0, std::memory_order_relaxed);
        // Recompute the combined stopped flag AFTER all individual flags are set, so it stays
        // consistent with a still-true _isLoading during the load tail (instead of a flat false).
        UpdateGameStopped();
        spdlog::debug("[MenuChecker] Cleared all menu state");
    }

    void MenuChecker::UpdateGameStopped()
    {
        bool stopped =
            _isLoading.load(std::memory_order_relaxed) ||
            _isPaused.load(std::memory_order_relaxed) ||
            _isPipboyOpen.load(std::memory_order_relaxed) ||
            _isMainMenu.load(std::memory_order_relaxed) ||
            _isInventoryOpen.load(std::memory_order_relaxed) ||
            _isContainerOpen.load(std::memory_order_relaxed) ||
            _isDialogueOpen.load(std::memory_order_relaxed) ||
            _isConsoleOpen.load(std::memory_order_relaxed) ||
            _isMessageBoxOpen.load(std::memory_order_relaxed) ||
            _isTerminalOpen.load(std::memory_order_relaxed) ||
            _isWorkshopOpen.load(std::memory_order_relaxed) ||
            _isCookingOpen.load(std::memory_order_relaxed) ||
            _isFavoritesOpen.load(std::memory_order_relaxed);
        
        _isGameStopped.store(stopped, std::memory_order_relaxed);
    }

    RE::BSEventNotifyControl MenuChecker::MenuEventSink::ProcessEvent(
        const RE::MenuOpenCloseEvent& a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*a_source*/)
    {
        // Get the menu name
        const char* menuName = a_event.menuName.c_str();
        bool opening = a_event.opening;
        
        // Update the appropriate atomic flag
        if (strcmp(menuName, "LoadingMenu") == 0) {
            _owner._isLoading.store(opening, std::memory_order_relaxed);

            // Tie the to-hand session-ready gate to the REAL end of loading.
            // kPostLoadGame fires long before LoadingMenu closes (observed: 17s gap),
            // so we anchor the gate to the menu close event here and add a short
            // grace period for any tail-end engine transfers.
            if (opening) {
                heisenberg::DropToHand::SetSessionNotReady();
            } else {
                heisenberg::DropToHand::SetSessionReady(5000);

                // WORKAROUND (2026-07-22) for a FRIK bug (frik::PlayerControlsHandler,
                // PlayerControlsHandler.h): its enableControls()/disableControls() guard
                // themselves on a member flag (_disabledInput) that survives a save load,
                // but the native restrained/input-enable state does NOT reliably survive
                // one. If the save happened while FRIK's cached flag already believed
                // controls were enabled, its post-load onFrameUpdate hits the idempotent
                // early-return and never re-applies SetActorRestrained(false) - movement
                // (kMainFour) stays stuck disabled until the player opens (forcing FRIK's
                // real disableControls() body to run) and closes the Pip-Boy (forcing the
                // real enableControls() body to run), which is the reported "have to open
                // the Pip-Boy before I can move" symptom. Reported upstream; this directly
                // re-asserts the un-restrained state once real loading is done (mirrors
                // FRIK's own enableControls() call) so a fresh load can't leave movement
                // blocked even if FRIK's cache disagrees with the fresh native state. Safe
                // to call unconditionally here - a no-op if the player is legitimately
                // restrained for an unrelated reason (menus/config-mode reassert their own
                // restrained state every frame regardless of this one-shot call).
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    f4cf::f4vr::SetActorRestrained(player, false);
                }

                // The one-shot above fires the instant LoadingMenu closes, which can still be
                // before the engine finishes restoring save state, and it only covers the actor
                // RESTRAINED flag. Arm the watchdog so the engine's input-enable layers are dumped
                // and movement re-asserted at several checkpoints after the load settles.
                InputRecovery::OnLoadComplete();
            }
        }
        else if (strcmp(menuName, "PauseMenu") == 0) {
            _owner._isPaused.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "PipboyMenu") == 0) {
            _owner._isPipboyOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "MainMenu") == 0) {
            _owner._isMainMenu.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "InventoryMenu") == 0) {
            _owner._isInventoryOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "ContainerMenu") == 0) {
            _owner._isContainerOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "DialogueMenu") == 0) {
            _owner._isDialogueOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "Console") == 0) {
            _owner._isConsoleOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "MessageBoxMenu") == 0) {
            _owner._isMessageBoxOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "TerminalMenu") == 0) {
            _owner._isTerminalOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "WorkshopMenu") == 0) {
            _owner._isWorkshopOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "CookingMenu") == 0) {
            _owner._isCookingOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "ScopeMenu") == 0) {
            _owner._isScopeOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "FavoritesMenu") == 0) {
            _owner._isFavoritesOpen.store(opening, std::memory_order_relaxed);
            if (!opening) {
                _owner._favoritesCloseTime.store(Utils::GetTime(), std::memory_order_relaxed);
            }
        }
        else if (strcmp(menuName, "ExamineMenu") == 0) {
            // Weapon/armor workbench (and scrap/mod confirm flow parent).
            _owner._isExamineOpen.store(opening, std::memory_order_relaxed);
        }
        else if (strcmp(menuName, "ExamineConfirmMenu") == 0) {
            // Child confirm dialog inside ExamineMenu — the crafting output
            // container event fires in the single ms after this closes while
            // ExamineMenu is still up, so we track both independently.
            _owner._isExamineConfirmOpen.store(opening, std::memory_order_relaxed);
        }

        // Log ALL menu open/close events for debugging
        spdlog::debug("[MenuChecker] {} {}", menuName, opening ? "OPENED" : "CLOSED");

        // Update combined flag
        _owner.UpdateGameStopped();
        
        // Start menu close cooldown when a blocking menu closes
        // (prevents grabbing immediately after closing inventory, pipboy, etc.)
        if (!opening) {
            // Only start cooldown for menus that block gameplay
            bool isBlockingMenu = 
                strcmp(menuName, "PipboyMenu") == 0 ||
                strcmp(menuName, "InventoryMenu") == 0 ||
                strcmp(menuName, "ContainerMenu") == 0 ||
                strcmp(menuName, "WorkshopMenu") == 0 ||
                strcmp(menuName, "FavoritesMenu") == 0 ||
                strcmp(menuName, "CookingMenu") == 0;
            
            if (isBlockingMenu) {
                _owner._menuCloseCooldown.store(MENU_CLOSE_COOLDOWN_DURATION, std::memory_order_relaxed);
                spdlog::debug("[MenuChecker] {} closed - starting 1s grab cooldown", menuName);
            }
        }
        
        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace heisenberg
