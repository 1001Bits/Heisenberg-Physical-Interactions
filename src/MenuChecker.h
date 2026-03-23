#pragma once

#include "RE/Fallout.h"
#include <unordered_map>
#include <string>
#include <atomic>

namespace heisenberg
{
    /**
     * Event-based menu state tracking.
     * 
     * This is ported from HIGGS Skyrim's menu_checker system.
     * Instead of calling UI::GetMenuOpen() directly from hook contexts
     * (which can cause race conditions with the UI thread), we register
     * for MenuOpenCloseEvent and cache the menu states.
     * 
     * This is thread-safe because:
     * 1. Menu events are processed on the main thread
     * 2. Our hooks also run on the main thread
     * 3. The atomic bools ensure visibility across any potential threading issues
     */
    class MenuChecker
    {
    public:
        static MenuChecker& GetSingleton()
        {
            static MenuChecker instance;
            return instance;
        }

        /**
         * Initialize the menu checker and register for events.
         * Call this once after game data is loaded.
         */
        bool Initialize();

        /**
         * Check if any game-stopping menu is open.
         * This is the main check used to skip processing during menus.
         */
        bool IsGameStopped() const;

        /**
         * Check if a specific menu is open.
         * @param menuName The BSFixedString name of the menu
         */
        bool IsMenuOpen(const char* menuName) const;

        /**
         * Check if loading screen is active.
         * Separate check for quick access.
         */
        bool IsLoading() const { return _isLoading.load(std::memory_order_relaxed); }

        /**
         * Check if pause menu is open.
         */
        bool IsPaused() const { return _isPaused.load(std::memory_order_relaxed); }

        /**
         * Check if pipboy is open.
         */
        bool IsPipboyOpen() const { return _isPipboyOpen.load(std::memory_order_relaxed); }

        /**
         * Check if scope menu is open (player is looking through a scope).
         */
        bool IsScopeOpen() const { return _isScopeOpen.load(std::memory_order_relaxed); }

        // Direct atomic accessors — use these instead of IsMenuOpen("...") to avoid strcmp chains
        bool IsMainMenu() const { return _isMainMenu.load(std::memory_order_relaxed); }
        bool IsInventoryOpen() const { return _isInventoryOpen.load(std::memory_order_relaxed); }
        bool IsContainerOpen() const { return _isContainerOpen.load(std::memory_order_relaxed); }
        bool IsDialogueOpen() const { return _isDialogueOpen.load(std::memory_order_relaxed); }
        bool IsConsoleOpen() const { return _isConsoleOpen.load(std::memory_order_relaxed); }
        bool IsMessageBoxOpen() const { return _isMessageBoxOpen.load(std::memory_order_relaxed); }
        bool IsTerminalOpen() const { return _isTerminalOpen.load(std::memory_order_relaxed); }
        bool IsWorkshopOpen() const { return _isWorkshopOpen.load(std::memory_order_relaxed); }
        bool IsCookingOpen() const { return _isCookingOpen.load(std::memory_order_relaxed); }
        bool IsFavoritesOpen() const { return _isFavoritesOpen.load(std::memory_order_relaxed); }

        /**
         * Check if Favorites menu is open or was recently open (within 500ms).
         * Used to detect consumable equips triggered from the Favorites menu.
         */
        bool WasFavoritesRecentlyOpen() const;

        /**
         * Check if in menu close cooldown (1 second after any menu closes).
         * Used to prevent grabbing immediately after closing a menu.
         */
        bool IsInMenuCloseCooldown() const;

        /**
         * Update menu close cooldown timer.
         * Call from main update loop with deltaTime.
         */
        void UpdateMenuCloseCooldown(float deltaTime);

        /**
         * Clear all cached menu states.
         * Call on save/load to reset to clean state.
         */
        void ClearState();

    private:
        MenuChecker() = default;
        ~MenuChecker() = default;
        MenuChecker(const MenuChecker&) = delete;
        MenuChecker& operator=(const MenuChecker&) = delete;

        // Event sink for menu open/close events
        class MenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            MenuEventSink(MenuChecker& owner) : _owner(owner) {}

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent& a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source) override;

        private:
            MenuChecker& _owner;
        };

        MenuEventSink _eventSink{ *this };
        bool _initialized = false;

        // Quick-access atomic flags for commonly checked menus
        std::atomic<bool> _isLoading{ false };
        std::atomic<bool> _isPaused{ false };
        std::atomic<bool> _isPipboyOpen{ false };
        std::atomic<bool> _isMainMenu{ false };
        std::atomic<bool> _isInventoryOpen{ false };
        std::atomic<bool> _isContainerOpen{ false };
        std::atomic<bool> _isDialogueOpen{ false };
        std::atomic<bool> _isConsoleOpen{ false };
        std::atomic<bool> _isMessageBoxOpen{ false };
        std::atomic<bool> _isTerminalOpen{ false };
        std::atomic<bool> _isWorkshopOpen{ false };
        std::atomic<bool> _isCookingOpen{ false };
        std::atomic<bool> _isFavoritesOpen{ false };
        std::atomic<bool> _isScopeOpen{ false };

        // Combined flag for "any blocking menu" - updated when individual flags change
        std::atomic<bool> _isGameStopped{ false };

        // Favorites close timestamp (for detecting equips triggered from Favorites)
        std::atomic<double> _favoritesCloseTime{ 0.0 };

        // Menu close cooldown - prevents grabbing immediately after closing menus
        std::atomic<float> _menuCloseCooldown{ 0.0f };
        static constexpr float MENU_CLOSE_COOLDOWN_DURATION = 1.0f;

        void UpdateGameStopped();
    };

}  // namespace heisenberg
