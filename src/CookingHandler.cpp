#include "CookingHandler.h"
#include "Config.h"
#include "DropToHand.h"
#include "Grab.h"  // For CheckItemStorageZone, GrabManager
#include "Hooks.h"
#include "F4VROffsets.h"
#include "WandNodeHelper.h"
#include "f4vr/PlayerNodes.h"
#include "SharedUtils.h"

// Activation ViewCasters — these return activation targets (furniture, buttons, cooking stations)
// Different from the storage ViewCasters in F4VROffsets.h which return pickup/store targets
using QActivatePickRef_t = RE::ObjectRefHandle*(*)(void* viewCaster, RE::ObjectRefHandle* outHandle);
inline REL::Relocation<QActivatePickRef_t> CookingQActivatePickRef{ REL::Offset(0x9D0CE0) };
inline REL::Relocation<std::uintptr_t> g_cookingPrimaryViewCaster{ REL::Offset(0x5B2A360) };
inline REL::Relocation<std::uintptr_t> g_cookingSecondaryViewCaster{ REL::Offset(0x5B2A280) };

namespace heisenberg
{

    // Check if a keyword editor ID suggests a fire/heat source
    static bool IsFireHeatKeyword(const char* editorID)
    {
        if (!editorID) return false;
        return ContainsCI(editorID, "fire")
            || ContainsCI(editorID, "flame")
            || ContainsCI(editorID, "lava")
            || ContainsCI(editorID, "forge")
            || ContainsCI(editorID, "heat");
    }

    static std::string GetFormDisplayName(RE::TESFormID formID)
    {
        auto* form = RE::TESForm::GetFormByID(formID);
        if (!form) return "?";
        auto name = RE::TESFullName::GetFullName(*form, false);
        return name.empty() ? "?" : std::string(name);
    }

    void CookingHandler::Initialize()
    {
        if (_initialized) return;

        BuildRecipeMap();
        _initialized = true;

        spdlog::info("[COOKING] Initialized — {} recipes, {} ingredient mappings, {} cooking keyword(s), {} station base forms",
            _allRecipes.size(), _ingredientToRecipes.size(), _cookingKeywordIDs.size(), _cookingStationBaseIDs.size());
    }

    void CookingHandler::BuildRecipeMap()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            spdlog::warn("[COOKING] TESDataHandler not available");
            return;
        }

        auto& cobjArray = dataHandler->GetFormArray<RE::BGSConstructibleObject>();
        spdlog::info("[COOKING] Scanning {} COBJ records for cooking recipes...", cobjArray.size());

        // First pass: discover cooking-related workbench keywords
        for (auto* cobj : cobjArray) {
            if (!cobj || !cobj->benchKeyword || !cobj->createdItem) continue;

            const char* kwEditorID = cobj->benchKeyword->GetFormEditorID();
            if (kwEditorID && ContainsCI(kwEditorID, "cook")) {
                _cookingKeywordIDs.insert(cobj->benchKeyword->GetFormID());
            }
        }

        if (_cookingKeywordIDs.empty()) {
            spdlog::warn("[COOKING] No cooking workbench keywords found in COBJ data");
            return;
        }

        for (auto kwID : _cookingKeywordIDs) {
            auto* kwForm = RE::TESForm::GetFormByID(kwID);
            const char* name = kwForm ? kwForm->GetFormEditorID() : "unknown";
            spdlog::info("[COOKING] Found cooking keyword: {:08X} '{}'", kwID, name ? name : "");
        }

        // Second pass: build recipe list with all ingredients
        for (auto* cobj : cobjArray) {
            if (!cobj || !cobj->benchKeyword || !cobj->createdItem) continue;

            if (_cookingKeywordIDs.find(cobj->benchKeyword->GetFormID()) == _cookingKeywordIDs.end())
                continue;

            if (!cobj->requiredItems || cobj->requiredItems->empty())
                continue;

            CookingRecipe recipe;
            recipe.cookedFormID = cobj->createdItem->GetFormID();
            recipe.cookedCount = cobj->data.numConstructed;
            if (recipe.cookedCount == 0) recipe.cookedCount = 1;

            for (auto& [reqItem, reqVal] : *cobj->requiredItems) {
                if (!reqItem) continue;
                CookingIngredient ing;
                ing.formID = reqItem->GetFormID();
                ing.count = 1;  // Default to 1, most cooking recipes use 1 of each
                recipe.ingredients.push_back(ing);
            }

            if (recipe.ingredients.empty()) continue;

            size_t idx = _allRecipes.size();
            _allRecipes.push_back(recipe);

            // Map each ingredient to this recipe
            for (auto& ing : recipe.ingredients) {
                _ingredientToRecipes[ing.formID].push_back(idx);
            }
        }

        // Log all recipes
        spdlog::info("[COOKING] Built {} cooking recipes:", _allRecipes.size());
        for (size_t i = 0; i < _allRecipes.size(); ++i) {
            auto& r = _allRecipes[i];
            std::string cookedName = GetFormDisplayName(r.cookedFormID);
            std::string ingList;
            for (auto& ing : r.ingredients) {
                if (!ingList.empty()) ingList += " + ";
                ingList += GetFormDisplayName(ing.formID);
                if (ing.count > 1) ingList += " x" + std::to_string(ing.count);
            }
            spdlog::info("[COOKING]   [{}] {} -> '{}' x{}", i, ingList, cookedName, r.cookedCount);
        }

        // Third pass: find all FURN and ACTI base forms that have a cooking or fire/heat keyword
        constexpr size_t kKeywordFormOffset_ACTI = 0xD0;

        auto& furnArray = dataHandler->GetFormArray<RE::TESFurniture>();
        for (auto* furn : furnArray) {
            if (!furn) continue;

            auto* kwForm = reinterpret_cast<RE::BGSKeywordForm*>(
                reinterpret_cast<std::uint8_t*>(furn) + kKeywordFormOffset_ACTI);

            if (kwForm && kwForm->keywords && kwForm->numKeywords > 0) {
                for (std::uint32_t i = 0; i < kwForm->numKeywords; ++i) {
                    RE::BGSKeyword* kw = kwForm->keywords[i];
                    if (kw && (_cookingKeywordIDs.count(kw->GetFormID()) || IsFireHeatKeyword(kw->GetFormEditorID()))) {
                        _cookingStationBaseIDs.insert(furn->GetFormID());
                        break;
                    }
                }
            }
        }

        auto& actiArray = dataHandler->GetFormArray<RE::TESObjectACTI>();
        for (auto* acti : actiArray) {
            if (!acti) continue;

            auto* kwForm = reinterpret_cast<RE::BGSKeywordForm*>(
                reinterpret_cast<std::uint8_t*>(acti) + kKeywordFormOffset_ACTI);

            if (kwForm && kwForm->keywords && kwForm->numKeywords > 0) {
                for (std::uint32_t i = 0; i < kwForm->numKeywords; ++i) {
                    RE::BGSKeyword* kw = kwForm->keywords[i];
                    if (kw && (_cookingKeywordIDs.count(kw->GetFormID()) || IsFireHeatKeyword(kw->GetFormEditorID()))) {
                        _cookingStationBaseIDs.insert(acti->GetFormID());
                        break;
                    }
                }
            }
        }

        spdlog::info("[COOKING] Found {} cooking/fire station base forms", _cookingStationBaseIDs.size());
    }

    bool CookingHandler::IsCookingStation(RE::TESObjectREFR* refr) const
    {
        if (!refr) return false;

        const auto baseObj = refr->GetObjectReference();
        if (!baseObj) return false;

        if (_cookingStationBaseIDs.count(baseObj->GetFormID()) > 0) {
            return true;
        }

        auto nameView = RE::TESFullName::GetFullName(*baseObj, false);
        if (!nameView.empty()) {
            const char* name = nameView.data();
            if (ContainsCI(name, "cooking")
                || ContainsCI(name, "fire")
                || ContainsCI(name, "lava")
                || ContainsCI(name, "forge")
                || ContainsCI(name, "stove")
                || ContainsCI(name, "oven")
                || ContainsCI(name, "grill")
                || ContainsCI(name, "brazier")
                || ContainsCI(name, "flame"))
            {
                return true;
            }
        }

        return false;
    }

    bool CookingHandler::HasIngredients(const CookingRecipe& recipe, RE::TESFormID heldFormID) const
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) return false;

        for (auto& ing : recipe.ingredients) {
            if (ing.formID == heldFormID) continue;  // The held item counts

            // Check player inventory for this ingredient
            bool found = false;
            for (auto& invItem : player->inventoryList->data) {
                if (invItem.object && invItem.object->GetFormID() == ing.formID) {
                    if (invItem.GetCount() >= ing.count) {
                        found = true;
                    }
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }

    int CookingHandler::FindBestRecipe(RE::TESFormID heldFormID, std::string& outLackingMsg) const
    {
        auto it = _ingredientToRecipes.find(heldFormID);
        if (it == _ingredientToRecipes.end()) return -1;

        const auto& recipeIndices = it->second;

        // Sort candidates: prefer recipes with more ingredients (complex recipes first)
        std::vector<size_t> sorted(recipeIndices.begin(), recipeIndices.end());
        std::sort(sorted.begin(), sorted.end(), [this](size_t a, size_t b) {
            return _allRecipes[a].ingredients.size() > _allRecipes[b].ingredients.size();
        });

        // Find first satisfiable recipe (most complex first)
        for (size_t idx : sorted) {
            if (HasIngredients(_allRecipes[idx], heldFormID)) {
                return static_cast<int>(idx);
            }
        }

        // No satisfiable recipe — build lacking message from the first (most complex) recipe
        const auto& bestCandidate = _allRecipes[sorted[0]];
        auto* player = RE::PlayerCharacter::GetSingleton();

        std::string lacking;
        for (auto& ing : bestCandidate.ingredients) {
            if (ing.formID == heldFormID) continue;

            bool found = false;
            if (player && player->inventoryList) {
                for (auto& invItem : player->inventoryList->data) {
                    if (invItem.object && invItem.object->GetFormID() == ing.formID) {
                        if (invItem.GetCount() >= ing.count) {
                            found = true;
                        }
                        break;
                    }
                }
            }

            if (!found) {
                if (!lacking.empty()) lacking += ", ";
                lacking += GetFormDisplayName(ing.formID);
            }
        }

        std::string cookedName = GetFormDisplayName(bestCandidate.cookedFormID);
        outLackingMsg = std::format("Lacking {} to make {}", lacking, cookedName);
        return -1;
    }

    void CookingHandler::Update(float deltaTime)
    {
        if (!g_config.enableCooking || !_initialized) return;

        if (_heatSourceMsgCooldown > 0.0f) _heatSourceMsgCooldown -= deltaTime;

        // Delayed cook result message (so "Cooking..." has time to display)
        if (_pendingCookMessageTimer > 0.0f) {
            _pendingCookMessageTimer -= deltaTime;
            if (_pendingCookMessageTimer <= 0.0f) {
                Hooks::ShowHUDMessageDirect(_pendingCookMessage.c_str());
                _pendingCookMessage.clear();
            }
        }

        UpdateViewCasterDetection();
        UpdateHand(true, deltaTime);
        UpdateHand(false, deltaTime);
    }

    void CookingHandler::UpdateViewCasterDetection()
    {
        _viewCasterPointingAtStation = false;

        void* viewCasters[2] = {
            reinterpret_cast<void*>(g_cookingPrimaryViewCaster.address()),
            reinterpret_cast<void*>(g_cookingSecondaryViewCaster.address())
        };

        for (int i = 0; i < 2; ++i) {
            if (!viewCasters[i]) continue;

            RE::ObjectRefHandle handle;
            CookingQActivatePickRef(viewCasters[i], &handle);
            if (!handle) continue;

            RE::NiPointer<RE::TESObjectREFR> refrPtr = handle.get();
            RE::TESObjectREFR* refr = refrPtr.get();
            if (!refr) continue;

            if (IsCookingStation(refr)) {
                _viewCasterPointingAtStation = true;

                if (g_config.cookingStationOnly) {
                    // Station-only mode: just set the flag, don't cache or show message
                    return;
                }

                if (!_activeCookingStation) {
                    auto* baseObj = refr->GetObjectReference();
                    auto nameView = baseObj ? RE::TESFullName::GetFullName(*baseObj, false) : std::string_view{};
                    auto pos = refr->GetPosition();
                    spdlog::info("[COOKING] Detected cooking station: {:08X} '{}' at ({:.0f}, {:.0f}, {:.0f})",
                        baseObj ? baseObj->GetFormID() : 0, nameView.empty() ? "?" : std::string(nameView),
                        pos.x, pos.y, pos.z);
                    if (_heatSourceMsgCooldown <= 0.0f) {
                        Hooks::ShowHUDMessageDirect("Heat source detected");
                        _heatSourceMsgCooldown = 5.0f;  // 5-second cooldown
                    }
                }
                _activeCookingStation = handle;
                _activeStationPos = refr->GetPosition();
                _activeStationPos.z += 80.0f;
                return;
            }
        }

        if (_activeCookingStation) {
            RE::NiPointer<RE::TESObjectREFR> refrPtr = _activeCookingStation.get();
            if (refrPtr) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (player) {
                    RE::NiPoint3 playerPos = player->GetPosition();
                    float dx = playerPos.x - _activeStationPos.x;
                    float dy = playerPos.y - _activeStationPos.y;
                    float dz = playerPos.z - _activeStationPos.z;
                    float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq < 200.0f * 200.0f) {
                        return;
                    }
                }
            }
            _activeCookingStation = RE::ObjectRefHandle();
        }
    }

    void CookingHandler::ClearState()
    {
        _activeCookingStation = RE::ObjectRefHandle();
        _leftState.Reset();
        _rightState.Reset();
    }

    void CookingHandler::UpdateHand(bool isLeft, float deltaTime)
    {
        CookingState& state = isLeft ? _leftState : _rightState;

        // Post-cook cooldown — prevents the cooked item from immediately starting a new cook cycle
        if (state.cooldownTimer > 0.0f) {
            state.cooldownTimer -= deltaTime;
            return;
        }

        // Periodic debug logging (every ~2 seconds at 90Hz)
        state.debugCounter++;
        bool logThisFrame = (state.debugCounter % 180 == 0);

        // Check active cooking context
        if (g_config.cookingStationOnly) {
            // Station-only mode: only cook when ViewCaster actively points at station
            if (!_viewCasterPointingAtStation) {
                if (state.isCooking) state.Reset();
                if (logThisFrame) spdlog::debug("[COOKING] {} UpdateHand: station-only mode, viewcaster not on station", isLeft ? "L" : "R");
                return;
            }
        } else {
            // Default mode: cook if viewcaster active OR station cached within proximity
            if (!_activeCookingStation && !_viewCasterPointingAtStation) {
                if (state.isCooking) state.Reset();
                if (logThisFrame) spdlog::debug("[COOKING] {} UpdateHand: no active station", isLeft ? "L" : "R");
                return;
            }
        }

        auto& grabMgr = GrabManager::GetSingleton();
        if (!grabMgr.IsGrabbing(isLeft)) {
            if (state.isCooking) state.Reset();
            return;
        }

        const auto& grabState = grabMgr.GetGrabState(isLeft);
        // Hold NiPointer immediately to prevent ref from being freed mid-update
        RE::NiPointer<RE::TESObjectREFR> heldItemRef(grabState.GetRefr());
        if (!heldItemRef) {
            if (state.isCooking) state.Reset();
            if (logThisFrame) spdlog::debug("[COOKING] {} UpdateHand: grab active but no refr", isLeft ? "L" : "R");
            return;
        }
        auto* heldItem = heldItemRef.get();

        auto* baseForm = heldItem->GetObjectReference();
        if (!baseForm) {
            if (state.isCooking) state.Reset();
            return;
        }

        RE::TESFormID heldFormID = baseForm->GetFormID();

        // Check if this ingredient is used in any recipe
        if (_ingredientToRecipes.find(heldFormID) == _ingredientToRecipes.end()) {
            if (state.isCooking) state.Reset();
            if (logThisFrame) spdlog::debug("[COOKING] {} UpdateHand: '{}' ({:08X}) not a recipe ingredient",
                isLeft ? "L" : "R", RE::TESFullName::GetFullName(*baseForm, false), heldFormID);
            return;
        }

        // Get held item position
        RE::NiPoint3 itemPos;
        if (grabState.node) {
            itemPos = grabState.node->world.translate;
        } else {
            if (state.isCooking) state.Reset();
            return;
        }

        // Check proximity to the cached cooking station
        bool inRange = _viewCasterPointingAtStation;  // Always in range if actively pointing

        if (!inRange) {
            // Fall back to proximity check (only in default mode, since station-only already returned)
            float dx = itemPos.x - _activeStationPos.x;
            float dy = itemPos.y - _activeStationPos.y;
            float dz = itemPos.z - _activeStationPos.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            float radiusSq = g_config.cookDetectionRadius * g_config.cookDetectionRadius;

            if (logThisFrame) {
                float dist = std::sqrt(distSq);
                spdlog::debug("[COOKING] {} hand: ingredient {:08X} dist={:.0f} (radius={:.0f}) item=({:.0f},{:.0f},{:.0f}) station=({:.0f},{:.0f},{:.0f})",
                    isLeft ? "L" : "R", heldFormID, dist, g_config.cookDetectionRadius,
                    itemPos.x, itemPos.y, itemPos.z,
                    _activeStationPos.x, _activeStationPos.y, _activeStationPos.z);
            }

            inRange = (distSq < radiusSq);
        }

        if (inRange) {
            if (!state.notifiedStart) {
                state.notifiedStart = true;
                state.isCooking = true;
                Hooks::ShowHUDMessageDirect("Cooking...");
                spdlog::info("[COOKING] Started cooking with '{}'", GetFormDisplayName(heldFormID));
            }

            state.cookTimer += deltaTime;

            if (state.cookTimer >= g_config.cookTime) {
                CookItem(isLeft, heldItem, heldFormID);
                state.Reset();
                state.cooldownTimer = 3.0f;  // 3 second cooldown before cooking can start again
            }
        } else {
            // Only reset timer, keep notifiedStart to prevent repeated "Cooking..." messages
            // from brief range fluctuations
            if (state.isCooking) {
                state.cookTimer = 0.0f;
                state.isCooking = false;
            }
        }
    }

    void CookingHandler::CookItem(bool isLeft, RE::TESObjectREFR* heldItem, RE::TESFormID heldFormID)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        // Find the best recipe for this ingredient
        std::string lackingMsg;
        int recipeIdx = FindBestRecipe(heldFormID, lackingMsg);

        if (recipeIdx < 0) {
            // No recipe satisfiable — show what's lacking
            Hooks::ShowHUDMessageDirect(lackingMsg.c_str());
            spdlog::info("[COOKING] {}", lackingMsg);
            return;
        }

        const CookingRecipe& recipe = _allRecipes[recipeIdx];

        auto* cookedForm = RE::TESForm::GetFormByID(recipe.cookedFormID);
        if (!cookedForm) {
            spdlog::error("[COOKING] Could not find cooked item form {:08X}", recipe.cookedFormID);
            return;
        }

        auto* cookedBoundObj = cookedForm->As<RE::TESBoundObject>();
        if (!cookedBoundObj) {
            spdlog::error("[COOKING] Cooked item {:08X} is not a TESBoundObject", recipe.cookedFormID);
            return;
        }

        std::string cookedName = GetFormDisplayName(recipe.cookedFormID);
        std::string heldName = GetFormDisplayName(heldFormID);

        // Keep heldItem alive
        RE::NiPointer<RE::TESObjectREFR> heldRef(heldItem);

        spdlog::info("[COOKING] Cooking '{}' -> '{}'", heldName, cookedName);

        // Clear smart grab state (cooking ends the selection cycle)
        ClearSmartGrab(isLeft);

        // Step 1: Release the held raw item
        auto& grabMgr = GrabManager::GetSingleton();
        grabMgr.EndGrab(isLeft, nullptr, true);

        // Step 2: Store held item back to inventory via ActivateRef
        if (heldRef) {
            Hooks::SetSuppressHUDMessages(true);
            heldRef->ActivateRef(player, nullptr, 1, false, false, false);
            Hooks::SetSuppressHUDMessages(false);
        }

        // Step 3: Remove all required ingredients from inventory
        Hooks::SetSuppressHUDMessages(true);
        for (auto& ing : recipe.ingredients) {
            auto* ingForm = RE::TESForm::GetFormByID(ing.formID);
            if (!ingForm) continue;
            auto* ingBound = ingForm->As<RE::TESBoundObject>();
            if (!ingBound) continue;

            RE::TESObjectREFR::RemoveItemData consumeData(ingBound, ing.count);
            consumeData.reason = RE::ITEM_REMOVE_REASON::kNone;
            player->RemoveItem(consumeData);

            spdlog::info("[COOKING] Consumed {}x '{}'", ing.count, GetFormDisplayName(ing.formID));
        }
        // Step 4: Add cooked item to inventory
        player->AddObjectToContainer(cookedBoundObj, {}, recipe.cookedCount, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        Hooks::SetSuppressHUDMessages(false);
        spdlog::info("[COOKING] Added {}x '{}' to inventory", recipe.cookedCount, cookedName);

        // Step 5: Queue cooked item for drop-to-hand (async, handles 3D loading)
        auto& dropToHand = DropToHand::GetSingleton();
        dropToHand.QueueDropToHand(recipe.cookedFormID, isLeft, 1, false, false);
        spdlog::info("[COOKING] Queued cooked item for drop-to-hand");

        player->PlayPickUpSound(cookedBoundObj, true, false);

        _pendingCookMessage = std::format("You cooked {}", cookedName);
        _pendingCookMessageTimer = 1.5f;  // Delay so "Cooking..." has time to display

        spdlog::info("[COOKING] Successfully cooked '{}' -> '{}'", heldName, cookedName);
    }

    // =========================================================================
    // SMART GRAB — pull cookable food from inventory when gripping in storage zone
    // =========================================================================

    bool CookingHandler::TrySmartGrab(bool isLeft)
    {
        if (!g_config.enableCooking || !_initialized) return false;
        if (!_activeCookingStation) return false;

        // Check hand is in storage zone
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) return false;
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!wandNode) return false;
        RE::NiPoint3 handPos = wandNode->world.translate;

        auto zoneResult = CheckItemStorageZone(handPos);
        if (!zoneResult.isInZone) return false;

        // Scan player inventory for a cookable item
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) return false;

        RE::TESBoundObject* candidateObj = nullptr;
        RE::TESFormID candidateFormID = 0;
        bool skippedLast = false;

        // First pass: skip _lastSmartGrabFormID to cycle
        for (auto& invItem : player->inventoryList->data) {
            if (!invItem.object) continue;
            RE::TESFormID fid = invItem.object->GetFormID();
            if (_ingredientToRecipes.find(fid) == _ingredientToRecipes.end()) continue;
            if (fid == _lastSmartGrabFormID) { skippedLast = true; continue; }

            std::string msg;
            if (FindBestRecipe(fid, msg) >= 0) {
                candidateObj = invItem.object;
                candidateFormID = fid;
                break;
            }
        }

        // Second pass: if we skipped the last one and found nothing else, allow it
        if (!candidateObj && skippedLast) {
            for (auto& invItem : player->inventoryList->data) {
                if (!invItem.object) continue;
                RE::TESFormID fid = invItem.object->GetFormID();
                if (_ingredientToRecipes.find(fid) == _ingredientToRecipes.end()) continue;

                std::string msg;
                if (FindBestRecipe(fid, msg) >= 0) {
                    candidateObj = invItem.object;
                    candidateFormID = fid;
                    break;
                }
            }
        }

        if (!candidateObj) {
            Hooks::ShowHUDMessageDirect("No cookable food");
            return false;
        }

        // Queue the item for async drop-to-hand (handles 3D loading reliably)
        auto& dropToHand = DropToHand::GetSingleton();
        dropToHand.QueueDropToHand(candidateFormID, isLeft, 1, false, false);  // non-sticky, not smart grab

        _lastSmartGrabFormID = candidateFormID;
        _smartGrabActive[isLeft ? 0 : 1] = true;

        std::string itemName = GetFormDisplayName(candidateFormID);
        std::string msg = std::format("{} selected", itemName);
        Hooks::ShowHUDMessageDirect(msg.c_str());

        spdlog::info("[COOKING] Smart grab: '{}' ({:08X}) in {} hand", itemName, candidateFormID, isLeft ? "left" : "right");
        return true;
    }

    bool CookingHandler::TrySmartRelease(bool isLeft)
    {
        int idx = isLeft ? 0 : 1;
        if (!_smartGrabActive[idx]) return false;

        // Check hand is still in storage zone
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) return false;
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!wandNode) return false;

        auto zoneResult = CheckItemStorageZone(wandNode->world.translate);
        if (!zoneResult.isInZone) {
            // Released outside zone — normal drop, clear smart grab
            _smartGrabActive[idx] = false;
            return false;
        }

        // Store item back to inventory
        auto& grabMgr = GrabManager::GetSingleton();
        const auto& grabState = grabMgr.GetGrabState(isLeft);
        auto* heldRefr = grabState.GetRefr();
        if (!heldRefr) {
            _smartGrabActive[idx] = false;
            return false;
        }

        RE::NiPointer<RE::TESObjectREFR> heldRef(heldRefr);

        grabMgr.EndGrab(isLeft, nullptr, true);

        if (heldRef) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                Hooks::SetSuppressHUDMessages(true);
                heldRef->ActivateRef(player, nullptr, 1, false, false, false);
                Hooks::SetSuppressHUDMessages(false);
            }
        }

        _smartGrabActive[idx] = false;
        spdlog::info("[COOKING] Smart release: stored item back to inventory");
        return true;
    }

    bool CookingHandler::IsSmartGrabActive(bool isLeft) const
    {
        return _smartGrabActive[isLeft ? 0 : 1];
    }

    void CookingHandler::ClearSmartGrab(bool isLeft)
    {
        _smartGrabActive[isLeft ? 0 : 1] = false;
    }
}
