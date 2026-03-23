#include "MenuChecker.h"
#include "Utils.h"

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
        _isLoading.store(false, std::memory_order_relaxed);
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
        _isGameStopped.store(false, std::memory_order_relaxed);
        _menuCloseCooldown.store(0.0f, std::memory_order_relaxed);
        _favoritesCloseTime.store(0.0, std::memory_order_relaxed);
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
