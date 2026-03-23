#pragma once

#include "RE/Fallout.h"
#include <string>
#include <unordered_set>
#include <vector>

namespace heisenberg
{
    // Categories for contextual item selection
    enum class SmartGrabCategory : std::uint32_t
    {
        None            = 0,
        Stimpack        = 1 << 0,   // Restores health
        RadAway         = 1 << 1,   // Removes radiation
        RadResistance   = 1 << 2,   // Boosts rad resistance (Rad-X)
        Antibiotic      = 1 << 3,   // Cures disease
        Addictol        = 1 << 4,   // Cures addiction
        Food            = 1 << 5,   // Food items
        Drink           = 1 << 6,   // Drinks (water, nuka cola)
        CombatChem      = 1 << 7,   // Combat chems (Psycho, Jet, etc.)
        CarryWeightAid  = 1 << 8,   // Carry weight boosters
        SleepAid        = 1 << 9,   // Caffeine/sleep prevention
        Ammo            = 1 << 10,  // Ammunition matching equipped weapon
    };

    inline SmartGrabCategory operator|(SmartGrabCategory a, SmartGrabCategory b)
    {
        return static_cast<SmartGrabCategory>(
            static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
    }

    inline SmartGrabCategory operator&(SmartGrabCategory a, SmartGrabCategory b)
    {
        return static_cast<SmartGrabCategory>(
            static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
    }

    inline bool HasCategory(SmartGrabCategory flags, SmartGrabCategory test)
    {
        return (flags & test) != SmartGrabCategory::None;
    }

    // Snapshot of player state used for priority selection
    struct PlayerNeeds
    {
        float healthPercent = 1.0f;     // 0.0-1.0
        float radsLevel = 0.0f;         // 0-1000 raw
        float radsPercent = 0.0f;       // 0.0-1.0
        bool  takingRadDamage = false;  // Currently in radiation area
        bool  inCombat = false;         // In active combat
        bool  hasDisease = false;       // Has active disease effect
        bool  hasAddiction = false;     // Has active addiction
        bool  isHungry = false;         // Survival: hunger detected
        bool  isThirsty = false;        // Survival: thirst detected
        bool  isOverencumbered = false; // Carry weight exceeded
        bool  isFatigued = false;       // Survival: sleep deprivation
        bool  hasWeaponDrawn = false;   // Has a ranged weapon drawn
        bool  lowAmmo = false;          // Magazine below threshold
        float ammoClipPercent = 1.0f;   // 0.0-1.0 magazine fill level
        RE::TESFormID weaponAmmoFormID = 0; // FormID of ammo the weapon uses (0 = none/melee)
    };

    // An inventory item with its categorization and sub-priority score
    struct CategorizedItem
    {
        RE::TESBoundObject* object = nullptr;
        RE::TESFormID formID = 0;
        SmartGrabCategory categories = SmartGrabCategory::None;
        int subPriority = 0;  // Higher = prefer within same category
        std::string displayName;  // ETTD name or base name for HUD display
    };

    /**
     * Smart grab handler - context-aware item selection when gripping behind the head.
     *
     * Assesses player state (health, rads, hunger, thirst, disease, combat, etc.)
     * and selects the most useful item from inventory based on priority:
     *   1. Health + Radiation (critical)
     *   2. Disease + Addiction
     *   3. Hunger + Thirst (survival)
     *   4. Combat chems
     *   5. Utility (carry weight, fatigue)
     */
    class SmartGrabHandler
    {
    public:
        static SmartGrabHandler& GetSingleton()
        {
            static SmartGrabHandler instance;
            return instance;
        }

        void Initialize();
        void ClearState();

        // Smart grab: grip in storage zone → pull contextual item from inventory
        bool TrySmartGrab(bool isLeft);
        // Smart release: grip released in storage zone → store item back to inventory
        bool TrySmartRelease(bool isLeft);
        // Check if current grab was initiated by smart grab
        bool IsSmartGrabActive(bool isLeft) const;
        // Clear smart grab state (called when grab ends normally)
        void ClearSmartGrab(bool isLeft);

    private:
        SmartGrabHandler() = default;

        // Assess current player needs
        PlayerNeeds AssessPlayerNeeds() const;

        // Categorize a single AlchemyItem
        CategorizedItem CategorizeItem(RE::TESBoundObject* obj) const;

        // Select the best item from inventory based on player needs
        // Returns nullptr if no suitable item found
        CategorizedItem SelectBestItem(const PlayerNeeds& needs, bool isLeft);

        // Get sub-priority score for stimpack-category items (stimpak > other healing > blood pack)
        int GetStimpackSubPriority(RE::TESBoundObject* obj) const;
        // Get sub-priority score for food items (cooked > prepared > raw)
        int GetFoodSubPriority(RE::TESBoundObject* obj) const;
        // Get sub-priority score for drink items (purified water > clean > nuka > other)
        int GetDrinkSubPriority(RE::TESBoundObject* obj) const;

        // Spawn item from inventory and grab it (count = number of items to remove from inventory)
        bool SpawnAndGrab(RE::TESBoundObject* obj, RE::TESFormID formID, bool isLeft, int count = 1);

        bool _initialized = false;

        // Smart grab state per hand
        bool _smartGrabActive[2] = { false, false };           // [0]=left, [1]=right
        RE::TESFormID _lastSmartGrabFormID[2] = { 0, 0 };     // Per-hand cycling
        SmartGrabCategory _lastCategory[2] = { SmartGrabCategory::None, SmartGrabCategory::None };
    };
}
