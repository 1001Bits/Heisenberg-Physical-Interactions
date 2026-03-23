#pragma once

#include "RE/Fallout.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heisenberg
{
    struct CookingIngredient
    {
        RE::TESFormID formID = 0;
        std::uint32_t count = 1;
    };

    struct CookingRecipe
    {
        RE::TESFormID cookedFormID = 0;
        std::uint16_t cookedCount = 1;
        std::vector<CookingIngredient> ingredients;
    };

    struct CookingState
    {
        float cookTimer = 0.0f;
        float cooldownTimer = 0.0f;  // Post-cook cooldown to prevent immediate re-cooking
        bool isCooking = false;
        bool notifiedStart = false;
        int debugCounter = 0;        // Frame counter for periodic debug logging

        void Reset()
        {
            cookTimer = 0.0f;
            isCooking = false;
            notifiedStart = false;
        }
    };

    /**
     * Cooking handler — hold raw food near a cooking station to cook it.
     *
     * Detection: Uses the game's ViewCaster to find cooking stations.
     * When either hand's ViewCaster points at a cooking station, that
     * station is cached. If the player holds cookable food near it,
     * cooking begins.
     *
     * Multi-ingredient recipes: when the timer completes, all other
     * required ingredients are consumed from inventory. If missing,
     * falls back to a simpler recipe or shows what's lacking.
     */
    class CookingHandler
    {
    public:
        static CookingHandler& GetSingleton()
        {
            static CookingHandler instance;
            return instance;
        }

        void Initialize();
        void Update(float deltaTime);
        void ClearState();

        // Smart grab: grip in storage zone near cooking station → pull cookable food from inventory
        bool TrySmartGrab(bool isLeft);
        // Smart release: grip released in storage zone → store item back to inventory
        bool TrySmartRelease(bool isLeft);
        // Check if current grab was initiated by smart grab
        bool IsSmartGrabActive(bool isLeft) const;
        // Clear smart grab state (called when grab ends normally)
        void ClearSmartGrab(bool isLeft);

        // Returns true if a cooking station has been detected nearby
        bool HasNearbyCookingStation() const { return _activeCookingStation.operator bool(); }

    private:
        CookingHandler() = default;

        void BuildRecipeMap();
        bool IsCookingStation(RE::TESObjectREFR* refr) const;
        void UpdateViewCasterDetection();
        void UpdateHand(bool isLeft, float deltaTime);
        void CookItem(bool isLeft, RE::TESObjectREFR* heldItem, RE::TESFormID heldFormID);

        // Find the best recipe for a held ingredient
        // Returns recipe index or -1 if none satisfiable
        // If none satisfiable, outLackingMsg is set with what's missing
        int FindBestRecipe(RE::TESFormID heldFormID, std::string& outLackingMsg) const;

        // Check if player has required ingredients (excluding the held item)
        bool HasIngredients(const CookingRecipe& recipe, RE::TESFormID heldFormID) const;

        // Master recipe list
        std::vector<CookingRecipe> _allRecipes;

        // ingredientFormID → indices into _allRecipes
        std::unordered_map<RE::TESFormID, std::vector<size_t>> _ingredientToRecipes;

        // Cooking workbench keyword form IDs (discovered from COBJ records)
        std::unordered_set<RE::TESFormID> _cookingKeywordIDs;

        // Base form IDs of furniture/activators that serve as cooking stations
        std::unordered_set<RE::TESFormID> _cookingStationBaseIDs;

        // Cached cooking station detected via ViewCaster
        RE::ObjectRefHandle _activeCookingStation;
        RE::NiPoint3 _activeStationPos;

        // Per-hand cooking state
        CookingState _leftState;
        CookingState _rightState;

        bool _initialized = false;

        // True THIS frame if a ViewCaster is actively pointing at a cooking station
        bool _viewCasterPointingAtStation = false;

        // Heat source detection cooldown (prevents message spam)
        float _heatSourceMsgCooldown = 0.0f;

        // Pending cook result message (delayed to avoid overlapping with "Cooking..." HUD text)
        std::string _pendingCookMessage;
        float _pendingCookMessageTimer = 0.0f;

        // Smart grab state
        bool _smartGrabActive[2] = { false, false };   // [0]=left, [1]=right
        RE::TESFormID _lastSmartGrabFormID = 0;         // Last item given (to cycle)
    };
}
