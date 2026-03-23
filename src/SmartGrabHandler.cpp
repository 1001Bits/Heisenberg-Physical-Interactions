#include "SmartGrabHandler.h"
#include "Config.h"
#include "CookingHandler.h"
#include "DropToHand.h"
#include "Grab.h"
#include "Hooks.h"
#include "F4VROffsets.h"
#include "WandNodeHelper.h"
#include "f4vr/PlayerNodes.h"
#include "VirtualHolstersAPI.h"
#include "SharedUtils.h"

#include <cfloat>

namespace heisenberg
{

    static std::string GetItemName(RE::TESBoundObject* obj)
    {
        if (!obj) return "?";
        auto nameView = RE::TESFullName::GetFullName(*obj, false);
        return nameView.empty() ? "?" : std::string(nameView);
    }

    // Get display name from inventory item (checks ETTD first for custom/modded names)
    static std::string GetInventoryItemName(RE::BGSInventoryItem& invItem)
    {
        if (!invItem.object) return "?";

        // Check stack ExtraDataList for ExtraTextDisplayData
        auto* stack = invItem.stackData.get();
        while (stack) {
            if (stack->extra) {
                auto* ettd = stack->extra->GetByType<RE::ExtraTextDisplayData>();
                if (ettd && !ettd->displayName.empty()) {
                    return std::string(ettd->displayName.c_str());
                }
            }
            stack = stack->nextStack.get();
        }

        // Fallback to base form name
        return GetItemName(invItem.object);
    }

    // Get cached Virtual Holsters container references
    static std::vector<RE::TESObjectREFR*>& GetHolsterContainers()
    {
        static std::vector<RE::TESObjectREFR*> containers;
        static bool initialized = false;

        if (!initialized) {
            initialized = true;
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (handler) {
                // Form IDs from VirtualHolsters.esp (indices 1-7)
                static constexpr RE::TESFormID kHolsterRawIDs[] = {
                    0x001022, 0x001021, 0x001020, 0x00101F, 0x001023, 0x001024, 0x006B86
                };
                for (auto rawID : kHolsterRawIDs) {
                    auto* form = handler->LookupForm(rawID, "VirtualHolsters.esp");
                    if (form) {
                        auto* refr = form->As<RE::TESObjectREFR>();
                        if (refr) containers.push_back(refr);
                    }
                }
                if (!containers.empty()) {
                    spdlog::debug("[SMARTGRAB] Found {} Virtual Holster containers", containers.size());
                }
            }
        }

        return containers;
    }

    // Check if a weapon name is holstered in any Virtual Holsters container
    static bool IsWeaponHolstered(const std::string& weaponName, const std::vector<RE::TESObjectREFR*>& holsters)
    {
        if (weaponName.empty() || holsters.empty()) return false;

        for (auto* container : holsters) {
            if (!container || !container->inventoryList) continue;

            for (auto& item : container->inventoryList->data) {
                if (!item.object) continue;
                if (item.object->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;

                auto* stack = item.stackData.get();
                while (stack) {
                    if (stack->extra) {
                        auto* ettd = stack->extra->GetByType<RE::ExtraTextDisplayData>();
                        if (ettd && !ettd->displayName.empty()) {
                            if (std::string(ettd->displayName.c_str()) == weaponName) {
                                return true;
                            }
                        }
                    }
                    stack = stack->nextStack.get();
                }
            }
        }

        return false;
    }

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    void SmartGrabHandler::Initialize()
    {
        if (_initialized) return;
        _initialized = true;
        spdlog::debug("[SMARTGRAB] Initialized");
    }

    void SmartGrabHandler::ClearState()
    {
        for (int i = 0; i < 2; ++i) {
            _smartGrabActive[i] = false;
            _lastSmartGrabFormID[i] = 0;
            _lastCategory[i] = SmartGrabCategory::None;
        }
    }

    bool SmartGrabHandler::IsSmartGrabActive(bool isLeft) const
    {
        return _smartGrabActive[isLeft ? 0 : 1];
    }

    void SmartGrabHandler::ClearSmartGrab(bool isLeft)
    {
        _smartGrabActive[isLeft ? 0 : 1] = false;
    }

    // =========================================================================
    // PLAYER STATE ASSESSMENT
    // =========================================================================

    PlayerNeeds SmartGrabHandler::AssessPlayerNeeds() const
    {
        PlayerNeeds needs;

        // Use VR player (F4SEVR layout) for actorState / activeEffects access
        auto* vrPlayer = f4cf::f4vr::getPlayer();
        if (!vrPlayer) return needs;

        // RE::PlayerCharacter* for RE API calls (GetActorValue, inventory, etc.)
        auto* player = reinterpret_cast<RE::PlayerCharacter*>(vrPlayer);

        auto* av = RE::ActorValue::GetSingleton();
        if (!av) return needs;

        spdlog::debug("[SMARTGRAB] AssessPlayerNeeds: checkpoint 1 - basics OK");

        // Read radiation first — needed to compute effective max HP
        if (av->rads) {
            needs.radsLevel = player->GetActorValue(*av->rads);
            needs.radsPercent = needs.radsLevel / 1000.0f;
            needs.radsPercent = std::clamp(needs.radsPercent, 0.0f, 1.0f);
        }

        // Health — compute effective max HP accounting for radiation
        // In FO4, rads reduce max HP: effectiveMax = trueMax * (1 - rads/1000)
        // Use effectiveMax so a "full" health bar with rads doesn't trigger stimpak
        float currentHP = 0.0f;
        float maxHP = 1.0f;
        if (av->health) {
            currentHP = player->GetActorValue(*av->health);
            float damageMod = player->GetModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, *av->health);
            float trueMax = currentHP - damageMod;
            maxHP = trueMax * (1.0f - needs.radsPercent);
            needs.healthPercent = (maxHP > 1.0f) ? (currentHP / maxHP) : 1.0f;
            needs.healthPercent = std::clamp(needs.healthPercent, 0.0f, 1.0f);
        }

        spdlog::debug("[SMARTGRAB] AssessPlayerNeeds: checkpoint 2 - health/rads OK (hp={:.0f} max={:.0f} rads={:.0f})",
                     currentHP, maxHP, needs.radsLevel);

        // Radiation area detection
        auto* magicTarget = player->GetMagicTarget();
        if (magicTarget) {
            needs.takingRadDamage = magicTarget->IsTakingRadDamageFromActiveEffect();
        }

        // Combat - use IsInActiveCombat to avoid false positives from stale/ended combat groups
        needs.inCombat = heisenberg::IsInActiveCombat(reinterpret_cast<RE::Actor*>(player));
        needs.isOverencumbered = false;

        spdlog::debug("[SMARTGRAB] AssessPlayerNeeds: checkpoint 4 - combat/rad area OK");

        // Scan active effects for survival needs (disease, hunger, thirst, fatigue, addiction)
        auto* effectList = magicTarget ? magicTarget->GetActiveEffectList() : nullptr;
        if (effectList) {
            for (auto& effectPtr : effectList->data) {
                if (!effectPtr) continue;
                auto* activeEffect = effectPtr.get();
                if (!activeEffect) continue;

                // Check if the effect is inactive/dispelled
                auto effectFlags = activeEffect->flags.get();
                if (static_cast<std::uint32_t>(effectFlags) &
                    (static_cast<std::uint32_t>(RE::ActiveEffect::Flags::kInactive) |
                     static_cast<std::uint32_t>(RE::ActiveEffect::Flags::kDispelled))) {
                    continue;
                }

                // Check the effect's spell/source for keyword hints
                RE::MagicItem* spell = activeEffect->spell;
                if (spell) {
                    const char* spellName = spell->GetFullName();
                    if (spellName) {
                        if (ContainsCI(spellName, "hunger") || ContainsCI(spellName, "HC_Hunger"))
                            needs.isHungry = true;
                        if (ContainsCI(spellName, "thirst") || ContainsCI(spellName, "HC_Thirst"))
                            needs.isThirsty = true;
                        if (ContainsCI(spellName, "disease") || ContainsCI(spellName, "HC_Disease"))
                            needs.hasDisease = true;
                        if (ContainsCI(spellName, "fatigue") || ContainsCI(spellName, "HC_Fatigue") ||
                            ContainsCI(spellName, "sleep"))
                            needs.isFatigued = true;
                        if (ContainsCI(spellName, "addiction") || ContainsCI(spellName, "Addicted"))
                            needs.hasAddiction = true;
                    }
                }

                // Also check the EffectSetting (MGEF) editor ID
                RE::EffectItem* effectItem = activeEffect->effect;
                if (effectItem && effectItem->effectSetting) {
                    const char* mgefEditorID = effectItem->effectSetting->GetFormEditorID();
                    if (mgefEditorID) {
                        if (ContainsCI(mgefEditorID, "hunger"))
                            needs.isHungry = true;
                        if (ContainsCI(mgefEditorID, "thirst"))
                            needs.isThirsty = true;
                        if (ContainsCI(mgefEditorID, "disease"))
                            needs.hasDisease = true;
                        if (ContainsCI(mgefEditorID, "fatigue") || ContainsCI(mgefEditorID, "sleep"))
                            needs.isFatigued = true;
                        if (ContainsCI(mgefEditorID, "addiction"))
                            needs.hasAddiction = true;
                    }
                }
            }
        }

        spdlog::debug("[SMARTGRAB] AssessPlayerNeeds: checkpoint 5 - active effects OK");

        spdlog::debug("[SMARTGRAB] Needs: hp={:.0f}/{:.0f} ({:.0f}%) rads={:.0f}% radArea={} combat={} disease={} "
                     "addiction={} hungry={} thirsty={} encumbered={} fatigued={} weapon={} ammo={:.0f}%",
                     currentHP, maxHP, needs.healthPercent * 100, needs.radsPercent * 100,
                     needs.takingRadDamage, needs.inCombat, needs.hasDisease,
                     needs.hasAddiction, needs.isHungry, needs.isThirsty,
                     needs.isOverencumbered, needs.isFatigued,
                     needs.hasWeaponDrawn, needs.ammoClipPercent * 100);

        return needs;
    }

    // =========================================================================
    // ITEM CATEGORIZATION
    // =========================================================================

    CategorizedItem SmartGrabHandler::CategorizeItem(RE::TESBoundObject* obj) const
    {
        CategorizedItem result;
        result.object = obj;
        result.formID = obj->GetFormID();

        // Categorize AMMO items
        if (obj->GetFormType() == RE::ENUM_FORM_ID::kAMMO) {
            result.categories = SmartGrabCategory::Ammo;
            return result;
        }

        // Only categorize AlchemyItems (food, drink, chems, medicine)
        if (obj->GetFormType() != RE::ENUM_FORM_ID::kALCH) {
            return result;
        }

        auto* alchItem = static_cast<RE::AlchemyItem*>(obj);
        auto* magicItem = static_cast<RE::MagicItem*>(alchItem);

        auto* av = RE::ActorValue::GetSingleton();
        if (!av) return result;

        // 1. Scan magic effects
        for (auto* effectItem : magicItem->listOfEffects) {
            if (!effectItem || !effectItem->effectSetting) continue;

            auto& effectData = effectItem->effectSetting->data;
            auto archetype = effectData.archetype.get();
            auto flags = effectData.flags.get();
            auto* primaryAV = effectData.primaryAV;
            float magnitude = effectItem->data.magnitude;

            // Stimpak archetype
            if (archetype == RE::EffectArchetypes::ArchetypeID::kStimpak) {
                result.categories = result.categories | SmartGrabCategory::Stimpack;
            }

            // Cure disease
            if (archetype == RE::EffectArchetypes::ArchetypeID::kCureDisease) {
                result.categories = result.categories | SmartGrabCategory::Antibiotic;
            }

            // Cure addiction
            if (archetype == RE::EffectArchetypes::ArchetypeID::kCureAddiction) {
                result.categories = result.categories | SmartGrabCategory::Addictol;
            }

            // Value modifier — check what AV it affects
            if (archetype == RE::EffectArchetypes::ArchetypeID::kValueModifier && primaryAV) {
                bool isRecover = (static_cast<std::uint32_t>(flags) &
                    static_cast<std::uint32_t>(RE::EffectSetting::EffectSettingData::Flag::kRecover)) != 0;

                // NOTE: Health-restoring ValueModifier is NOT tagged as Stimpack.
                // Many food/drink items restore HP and would incorrectly compete with
                // actual stimpaks (which have kStimpak archetype, keyword, or name).

                // Removes radiation
                if (primaryAV == av->rads && (isRecover || magnitude < 0.0f)) {
                    result.categories = result.categories | SmartGrabCategory::RadAway;
                }

                // Rad resistance
                if (primaryAV == av->radIngestionResistance || primaryAV == av->radExposureResistance) {
                    result.categories = result.categories | SmartGrabCategory::RadResistance;
                }

                // Carry weight boost
                if (primaryAV == av->carryWeight && magnitude > 0.0f) {
                    result.categories = result.categories | SmartGrabCategory::CarryWeightAid;
                }

                // Combat-useful effects (damage resist, strength)
                // NOTE: AP (actionPoints) excluded — many regular drinks restore AP
                // and shouldn't be tagged as combat chems. Actual combat chems (Jet, etc.)
                // are caught by name-based matching below.
                if ((primaryAV == av->damageResistance ||
                     primaryAV == av->strength) && magnitude > 0.0f) {
                    result.categories = result.categories | SmartGrabCategory::CombatChem;
                }
            }
        }

        // 2. Keyword checks
        auto* kwForm = static_cast<RE::BGSKeywordForm*>(magicItem);
        if (kwForm) {
            if (kwForm->ContainsKeywordString("Stimpak"sv))
                result.categories = result.categories | SmartGrabCategory::Stimpack;
            if (kwForm->ContainsKeywordString("Food"sv))
                result.categories = result.categories | SmartGrabCategory::Food;
            if (kwForm->ContainsKeywordString("Drink"sv) || kwForm->ContainsKeywordString("NukaCola"sv) ||
                kwForm->ContainsKeywordString("Alcohol"sv))
                result.categories = result.categories | SmartGrabCategory::Drink;
        }

        // 3. Virtual method checks
        if (magicItem->IsFood() && !HasCategory(result.categories, SmartGrabCategory::Stimpack)) {
            result.categories = result.categories | SmartGrabCategory::Food;
        }

        // 4. Name-based fallback
        std::string name = GetItemName(obj);
        const char* nameStr = name.c_str();

        if (result.categories == SmartGrabCategory::None || HasCategory(result.categories, SmartGrabCategory::Food)) {
            // Only do name-based if we haven't categorized yet, or to refine food/drink
            if (ContainsCI(nameStr, "stimpak"))
                result.categories = result.categories | SmartGrabCategory::Stimpack;
            if (ContainsCI(nameStr, "radaway") || ContainsCI(nameStr, "rad-away"))
                result.categories = result.categories | SmartGrabCategory::RadAway;
            if (ContainsCI(nameStr, "rad-x"))
                result.categories = result.categories | SmartGrabCategory::RadResistance;
            if (ContainsCI(nameStr, "antibiotic"))
                result.categories = result.categories | SmartGrabCategory::Antibiotic;
            if (ContainsCI(nameStr, "addictol"))
                result.categories = result.categories | SmartGrabCategory::Addictol;
            if (ContainsCI(nameStr, "psycho") || ContainsCI(nameStr, "jet") ||
                ContainsCI(nameStr, "buffout") || ContainsCI(nameStr, "med-x"))
                result.categories = result.categories | SmartGrabCategory::CombatChem;
            if (ContainsCI(nameStr, "water") || ContainsCI(nameStr, "nuka"))
                result.categories = result.categories | SmartGrabCategory::Drink;
        }

        // Compute sub-priority for stimpack, food and drink
        if (HasCategory(result.categories, SmartGrabCategory::Stimpack)) {
            result.subPriority = (std::max)(result.subPriority, GetStimpackSubPriority(obj));
        }
        if (HasCategory(result.categories, SmartGrabCategory::Food)) {
            result.subPriority = (std::max)(result.subPriority, GetFoodSubPriority(obj));
        }
        if (HasCategory(result.categories, SmartGrabCategory::Drink)) {
            result.subPriority = (std::max)(result.subPriority, GetDrinkSubPriority(obj));
        }

        return result;
    }

    int SmartGrabHandler::GetStimpackSubPriority(RE::TESBoundObject* obj) const
    {
        std::string name = GetItemName(obj);
        const char* n = name.c_str();

        // Actual stimpaks — highest priority
        if (ContainsCI(n, "stimpak"))
            return 3;

        // Blood packs — above other healing, below stimpaks
        if (ContainsCI(n, "blood pack"))
            return 2;

        // Other kStimpak-archetype healing items
        return 1;
    }

    int SmartGrabHandler::GetFoodSubPriority(RE::TESBoundObject* obj) const
    {
        std::string name = GetItemName(obj);
        const char* n = name.c_str();

        // Cooked/prepared food = highest priority
        if (ContainsCI(n, "grilled") || ContainsCI(n, "roasted") || ContainsCI(n, "cooked") ||
            ContainsCI(n, "steak") || ContainsCI(n, "soup") || ContainsCI(n, "stew") ||
            ContainsCI(n, "noodle") || ContainsCI(n, "pie") || ContainsCI(n, "baked") ||
            ContainsCI(n, "deathclaw") || ContainsCI(n, "mirelurk")) {
            return 3;
        }

        // Packaged/prepared food
        if (ContainsCI(n, "cram") || ContainsCI(n, "sugar bombs") || ContainsCI(n, "fancy lads") ||
            ContainsCI(n, "blamco") || ContainsCI(n, "instamash") || ContainsCI(n, "pork")) {
            return 2;
        }

        // Raw food (lowest)
        return 1;
    }

    int SmartGrabHandler::GetDrinkSubPriority(RE::TESBoundObject* obj) const
    {
        std::string name = GetItemName(obj);
        const char* n = name.c_str();

        if (ContainsCI(n, "purified water"))
            return 4;
        if (ContainsCI(n, "water") && !ContainsCI(n, "dirty"))
            return 3;
        if (ContainsCI(n, "nuka"))
            return 2;
        return 1;
    }

    // =========================================================================
    // ITEM SELECTION
    // =========================================================================

    CategorizedItem SmartGrabHandler::SelectBestItem(const PlayerNeeds& needs, bool isLeft)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) return {};

        int handIdx = isLeft ? 0 : 1;

        // Build priority list based on current needs
        // Priorities shift based on combat state:
        //   In combat:  ammo at #2, combat chem at #4
        //   Out combat: ammo at #12
        struct PriorityEntry {
            SmartGrabCategory category;
            bool condition;
        };

        // Stack-allocated priority list (no heap alloc)
        const PriorityEntry priorities[] = {
            { SmartGrabCategory::Stimpack,       needs.healthPercent < g_config.smartGrabHealthThreshold },
            { SmartGrabCategory::RadAway,         needs.radsLevel > 0.0f },
            { SmartGrabCategory::CombatChem,      needs.inCombat },
            { SmartGrabCategory::RadResistance,   needs.takingRadDamage },
            { SmartGrabCategory::Antibiotic,      needs.hasDisease },
            { SmartGrabCategory::Addictol,        needs.hasAddiction },
            { SmartGrabCategory::Food,            needs.isHungry },
            { SmartGrabCategory::Drink,           needs.isThirsty },
            { SmartGrabCategory::Stimpack,        needs.inCombat && needs.healthPercent < 0.75f },
            { SmartGrabCategory::CarryWeightAid,  needs.isOverencumbered },
            { SmartGrabCategory::Drink,           needs.isFatigued },
        };

        // Single-pass: categorize all ALCH items once, track best per category
        struct CategoryBest {
            CategorizedItem best;           // best excluding last-grabbed (for cycling)
            CategorizedItem bestAll;        // best including last-grabbed (fallback)
            bool skippedLast = false;
            int invIdx = -1;
            int invIdxAll = -1;
        };
        CategoryBest catBests[11] = {};
        for (auto& cb : catBests) { cb.best.subPriority = -1; cb.bestAll.subPriority = -1; }

        int invIdx = 0;
        for (auto& invItem : player->inventoryList->data) {
            if (!invItem.object || invItem.GetCount() <= 0) { invIdx++; continue; }
            if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kALCH) { invIdx++; continue; }

            CategorizedItem cat = CategorizeItem(invItem.object);
            if (cat.categories == SmartGrabCategory::None) { invIdx++; continue; }

            RE::TESFormID fid = invItem.object->GetFormID();

            for (int bit = 0; bit < 11; bit++) {
                auto catFlag = static_cast<SmartGrabCategory>(1u << bit);
                if (!HasCategory(cat.categories, catFlag)) continue;

                auto& cb = catBests[bit];

                // Track best including last (cycling fallback)
                if (cat.subPriority > cb.bestAll.subPriority) {
                    cb.bestAll = cat;
                    cb.invIdxAll = invIdx;
                }

                // Skip last grabbed for cycling
                if (fid == _lastSmartGrabFormID[handIdx] && _lastCategory[handIdx] == catFlag) {
                    cb.skippedLast = true;
                    continue;
                }

                if (cat.subPriority > cb.best.subPriority) {
                    cb.best = cat;
                    cb.invIdx = invIdx;
                }
            }
            invIdx++;
        }

        // Walk priorities and pick first match (no re-scanning)
        for (const auto& prio : priorities) {
            if (!prio.condition) continue;

            auto catVal = static_cast<std::uint32_t>(prio.category);
            int bit = 0;
            while (bit < 11 && (1u << bit) != catVal) bit++;
            if (bit >= 11) continue;

            auto& cb = catBests[bit];
            CategorizedItem* selected = nullptr;
            int selIdx = -1;

            if (cb.best.object) {
                selected = &cb.best;
                selIdx = cb.invIdx;
            } else if (cb.skippedLast && cb.bestAll.object) {
                selected = &cb.bestAll;
                selIdx = cb.invIdxAll;
            }

            if (selected) {
                if (selIdx >= 0 && selIdx < static_cast<int>(player->inventoryList->data.size())) {
                    selected->displayName = GetInventoryItemName(player->inventoryList->data[selIdx]);
                }
                _lastSmartGrabFormID[handIdx] = selected->formID;
                _lastCategory[handIdx] = prio.category;
                return *selected;
            }
        }

        // =====================================================================
        // FALLBACK: no priority condition was met
        //   1. Heaviest junk item (kMISC)
        //   2. Heaviest weapon with duplicates (count > 1)
        //   3. Heaviest weapon with lowest damage (least valuable)
        // =====================================================================
        spdlog::debug("[SMARTGRAB] No priority matched — trying fallback (junk/weapon)");

        CategorizedItem fallback;
        float bestWeight = -1.0f;
        bool skippedLastFallback = false;
        auto& holsters = GetHolsterContainers();

        // --- Pass 1: heaviest junk item (kMISC) ---
        // Only select junk if smartGrabIncludeHeavyJunk is enabled
        if (!g_config.smartGrabIncludeHeavyJunk) {
            spdlog::debug("[SMARTGRAB] Skipping junk fallback (smartGrabIncludeHeavyJunk=false)");
        } else
        for (auto& invItem : player->inventoryList->data) {
            if (!invItem.object || invItem.GetCount() <= 0) continue;
            if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kMISC) continue;

            RE::TESFormID fid = invItem.object->GetFormID();
            if (fid == _lastSmartGrabFormID[handIdx] && _lastCategory[handIdx] == SmartGrabCategory::None) {
                skippedLastFallback = true;
                continue;
            }

            float weight = static_cast<RE::TESObjectMISC*>(invItem.object)->GetFormWeight();
            if (weight > bestWeight) {
                bestWeight = weight;
                fallback.object = invItem.object;
                fallback.formID = fid;
                fallback.categories = SmartGrabCategory::None;
                fallback.displayName = GetInventoryItemName(invItem);
            }
        }

        // Cycling: if we skipped last and found nothing else, allow it
        if (!fallback.object && skippedLastFallback && g_config.smartGrabIncludeHeavyJunk) {
            for (auto& invItem : player->inventoryList->data) {
                if (!invItem.object || invItem.GetCount() <= 0) continue;
                if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kMISC) continue;

                float weight = static_cast<RE::TESObjectMISC*>(invItem.object)->GetFormWeight();
                if (weight > bestWeight) {
                    bestWeight = weight;
                    fallback.object = invItem.object;
                    fallback.formID = invItem.object->GetFormID();
                    fallback.categories = SmartGrabCategory::None;
                    fallback.displayName = GetInventoryItemName(invItem);
                }
            }
        }

        // --- Pass 2: heaviest weapon with duplicates (count > 1) ---
        if (!fallback.object && g_config.smartGrabIncludeHeavyJunk) {
            bestWeight = -1.0f;
            skippedLastFallback = false;

            for (auto& invItem : player->inventoryList->data) {
                if (!invItem.object || invItem.GetCount() <= 1) continue;  // must have duplicates
                if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;

                RE::TESFormID fid = invItem.object->GetFormID();
                if (fid == _lastSmartGrabFormID[handIdx] && _lastCategory[handIdx] == SmartGrabCategory::None) {
                    skippedLastFallback = true;
                    continue;
                }

                float weight = static_cast<RE::TESObjectWEAP*>(invItem.object)->weaponData.weight;
                if (weight > bestWeight) {
                    bestWeight = weight;
                    fallback.object = invItem.object;
                    fallback.formID = fid;
                    fallback.categories = SmartGrabCategory::None;
                    fallback.displayName = GetInventoryItemName(invItem);
                }
            }

            if (!fallback.object && skippedLastFallback) {
                for (auto& invItem : player->inventoryList->data) {
                    if (!invItem.object || invItem.GetCount() <= 1) continue;
                    if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;

                    float weight = static_cast<RE::TESObjectWEAP*>(invItem.object)->weaponData.weight;
                    if (weight > bestWeight) {
                        bestWeight = weight;
                        fallback.object = invItem.object;
                        fallback.formID = invItem.object->GetFormID();
                        fallback.categories = SmartGrabCategory::None;
                        fallback.displayName = GetInventoryItemName(invItem);
                    }
                }
            }
        }

        // --- Pass 3: heaviest weapon with lowest damage (least valuable) ---
        // Skip holstered weapons when player only has 1
        if (!fallback.object && g_config.smartGrabIncludeHeavyJunk) {
            float lowestDamage = FLT_MAX;
            bestWeight = -1.0f;
            skippedLastFallback = false;

            for (auto& invItem : player->inventoryList->data) {
                if (!invItem.object || invItem.GetCount() <= 0) continue;
                if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;

                RE::TESFormID fid = invItem.object->GetFormID();
                if (fid == _lastSmartGrabFormID[handIdx] && _lastCategory[handIdx] == SmartGrabCategory::None) {
                    skippedLastFallback = true;
                    continue;
                }

                std::string weapDisplayName = GetInventoryItemName(invItem);
                if (!holsters.empty() && IsWeaponHolstered(weapDisplayName, holsters) && invItem.GetCount() <= 1) {
                    spdlog::debug("[SMARTGRAB] Skipping holstered weapon '{}' (count={})", weapDisplayName, invItem.GetCount());
                    continue;
                }

                auto* weap = static_cast<RE::TESObjectWEAP*>(invItem.object);
                float damage = static_cast<float>(weap->weaponData.attackDamage);
                float weight = weap->weaponData.weight;

                // Prefer lowest damage first, then heaviest weight as tiebreaker
                if (damage < lowestDamage || (damage == lowestDamage && weight > bestWeight)) {
                    lowestDamage = damage;
                    bestWeight = weight;
                    fallback.object = invItem.object;
                    fallback.formID = fid;
                    fallback.categories = SmartGrabCategory::None;
                    fallback.displayName = weapDisplayName;
                }
            }

            if (!fallback.object && skippedLastFallback) {
                for (auto& invItem : player->inventoryList->data) {
                    if (!invItem.object || invItem.GetCount() <= 0) continue;
                    if (invItem.object->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;

                    std::string weapDisplayName = GetInventoryItemName(invItem);
                    if (!holsters.empty() && IsWeaponHolstered(weapDisplayName, holsters) && invItem.GetCount() <= 1) {
                        continue;
                    }

                    auto* weap = static_cast<RE::TESObjectWEAP*>(invItem.object);
                    float damage = static_cast<float>(weap->weaponData.attackDamage);
                    float weight = weap->weaponData.weight;

                    if (damage < lowestDamage || (damage == lowestDamage && weight > bestWeight)) {
                        lowestDamage = damage;
                        bestWeight = weight;
                        fallback.object = invItem.object;
                        fallback.formID = invItem.object->GetFormID();
                        fallback.categories = SmartGrabCategory::None;
                        fallback.displayName = weapDisplayName;
                    }
                }
            }
        }

        if (fallback.object) {
            _lastSmartGrabFormID[handIdx] = fallback.formID;
            _lastCategory[handIdx] = SmartGrabCategory::None;
            spdlog::debug("[SMARTGRAB] Fallback selected: '{}' ({:08X})",
                         fallback.displayName.empty() ? GetItemName(fallback.object) : fallback.displayName,
                         fallback.formID);
            return fallback;
        }

        return {};
    }

    // =========================================================================
    // SPAWN AND GRAB
    // =========================================================================

    bool SmartGrabHandler::SpawnAndGrab(RE::TESBoundObject* /*obj*/, RE::TESFormID formID, bool isLeft, int count)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        // Check the item is actually in inventory
        bool hasItem = false;
        if (player->inventoryList) {
            for (auto& invItem : player->inventoryList->data) {
                if (invItem.object && invItem.object->GetFormID() == formID && invItem.GetCount() >= static_cast<std::uint32_t>(count)) {
                    hasItem = true;
                    break;
                }
            }
        }
        if (!hasItem) {
            spdlog::warn("[SMARTGRAB] Item {:08X} not found in inventory (count={})", formID, count);
            return false;
        }

        // Use DropToHand queue for reliable async grab with 3D loading wait.
        // stickyGrab=true: item stays in hand without holding grip (hand is behind head)
        // markAsSmartGrab=true: prevents auto-storage from immediately re-storing the item
        auto& dropToHand = DropToHand::GetSingleton();
        dropToHand.QueueDropToHand(formID, isLeft, count, true, true);

        spdlog::debug("[SMARTGRAB] Queued {:08X} x{} for drop-to-hand ({} hand)",
                     formID, count, isLeft ? "left" : "right");
        return true;
    }

    // =========================================================================
    // TRY SMART GRAB
    // =========================================================================

    bool SmartGrabHandler::TrySmartGrab(bool isLeft)
    {
        spdlog::debug("[SMARTGRAB] TrySmartGrab called ({} hand) — enableSmartGrab={} initialized={}",
                     isLeft ? "left" : "right", g_config.enableSmartGrab, _initialized);

        if (!g_config.enableSmartGrab) {
            spdlog::debug("[SMARTGRAB] BLOCKED: enableSmartGrab is false");
            return false;
        }
        if (!_initialized) {
            spdlog::debug("[SMARTGRAB] BLOCKED: not initialized");
            return false;
        }

        // CRITICAL: Don't trigger smart retrieval if this hand has an active sticky grab
        // When storing an item from a sticky grab (grip press in storage zone), 
        // the grip is for releasing/storing that item, NOT for retrieving a new one.
        auto& grabMgr = GrabManager::GetSingleton();
        const auto& grabState = grabMgr.GetGrabState(isLeft);
        if (grabState.active && grabState.stickyGrab) {
            spdlog::debug("[SMARTGRAB] BLOCKED: {} hand has active sticky grab - grip is for storing, not retrieving",
                         isLeft ? "left" : "right");
            return false;
        }

        // CRITICAL: Don't trigger smart retrieval if hand is in a Virtual Holsters holster zone
        // When the hand is in a holster zone, the grip is for holstering/unholstering a weapon.
        // EXCEPTION: When near a cooking station, SmartGrab takes priority over VH holster zones
        // so the player can retrieve ingredients from their backpack.
        bool nearCooking = CookingHandler::GetSingleton().HasNearbyCookingStation();
        if (!nearCooking) {
            if (auto* vhApi = VirtualHolsters::RequestVirtualHolstersAPI()) {
                if (vhApi->IsHandInHolsterZone(isLeft)) {
                    spdlog::debug("[SMARTGRAB] BLOCKED: {} hand is in Virtual Holsters holster zone",
                                 isLeft ? "left" : "right");
                    return false;
                }
            }
        } else {
            spdlog::debug("[SMARTGRAB] Near cooking station — skipping VH holster zone check");
        }

        // Check hand is in storage zone (behind head)
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) {
            spdlog::debug("[SMARTGRAB] BLOCKED: playerNodes null");
            return false;
        }
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!wandNode) {
            spdlog::debug("[SMARTGRAB] BLOCKED: wandNode null");
            return false;
        }
        RE::NiPoint3 handPos = wandNode->world.translate;

        // Log hand position vs zone for diagnostics
        if (playerNodes->HmdNode) {
            const RE::NiPoint3& hmdPos = playerNodes->HmdNode->world.translate;
            spdlog::debug("[SMARTGRAB] Hand=({:.0f},{:.0f},{:.0f}) HMD=({:.0f},{:.0f},{:.0f}) zoneOffset=({:.1f},{:.1f},{:.1f}) radius={:.1f}",
                         handPos.x, handPos.y, handPos.z,
                         hmdPos.x, hmdPos.y, hmdPos.z,
                         g_config.storageZoneOffsetX, g_config.storageZoneOffsetY, g_config.storageZoneOffsetZ,
                         g_config.itemStorageZoneRadius);
        }

        auto zoneResult = CheckItemStorageZone(handPos);
        if (!zoneResult.isInZone) {
            spdlog::debug("[SMARTGRAB] BLOCKED: hand not in storage zone (enableItemStorageZones={})",
                         g_config.enableItemStorageZones);
            return false;
        }

        spdlog::debug("[SMARTGRAB] Hand IS in storage zone — assessing player needs...");

        // Assess player needs
        PlayerNeeds needs = AssessPlayerNeeds();

        // Select best item based on needs
        CategorizedItem selected = SelectBestItem(needs, isLeft);
        if (!selected.object) {
            spdlog::debug("[SMARTGRAB] No suitable item found — no priority matched and no fallback available");
            return false;
        }

        // Queue the item for async drop-to-hand (handles 3D loading)
        int spawnCount = 1;
        if (!SpawnAndGrab(selected.object, selected.formID, isLeft, spawnCount)) {
            return false;
        }

        // isFromSmartGrab is now set by DropToHand when the grab completes
        _smartGrabActive[isLeft ? 0 : 1] = true;

        // Show HUD message immediately (before async grab completes)
        std::string itemName = selected.displayName.empty() ? GetItemName(selected.object) : selected.displayName;
        std::string msg;
        if (spawnCount > 1) {
            msg = std::format("{} ({}) retrieved", itemName, spawnCount);
        } else {
            msg = std::format("{} retrieved", itemName);
        }
        Hooks::ShowHUDMessageDirect(msg.c_str());

        spdlog::debug("[SMARTGRAB] Queued '{}' ({:08X}) for {} hand (categories=0x{:X})",
                     itemName, selected.formID, isLeft ? "left" : "right",
                     static_cast<std::uint32_t>(selected.categories));

        return true;
    }

    // =========================================================================
    // TRY SMART RELEASE
    // =========================================================================

    bool SmartGrabHandler::TrySmartRelease(bool isLeft)
    {
        int handIdx = isLeft ? 0 : 1;
        if (!_smartGrabActive[handIdx]) return false;

        // Check hand is in storage zone
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) return false;
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!wandNode) return false;

        auto zoneResult = CheckItemStorageZone(wandNode->world.translate);
        if (!zoneResult.isInZone) {
            // Released outside zone — normal drop, clear smart grab
            _smartGrabActive[handIdx] = false;
            return false;
        }

        // Get the currently held object
        auto& grabMgr = GrabManager::GetSingleton();
        const auto& grabState = grabMgr.GetGrabState(isLeft);
        auto* heldRefr = grabState.GetRefr();
        if (!heldRefr) {
            _smartGrabActive[handIdx] = false;
            return false;
        }

        // Keep a reference before ending grab
        RE::NiPointer<RE::TESObjectREFR> heldRef(heldRefr);

        // End the grab (releases physics hold)
        grabMgr.EndGrab(isLeft, nullptr, true);

        // Store item back to inventory via ActivateRef
        if (heldRef) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                Hooks::SetSuppressHUDMessages(true);
                heldRef->ActivateRef(player, nullptr, 1, false, false, false);
                Hooks::SetSuppressHUDMessages(false);
            }
        }

        spdlog::debug("[SMARTGRAB] Stored item back to inventory from {} hand", isLeft ? "left" : "right");

        _smartGrabActive[handIdx] = false;
        return true;
    }
}
