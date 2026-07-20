#include "DropToHand.h"
#include "Config.h"
#include "F4VROffsets.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "PipboyInteraction.h"
#include "Hooks.h"
#include "ItemOffsets.h"
#include "MenuChecker.h"
#include "Utils.h"
#include "VRInput.h"
#include "WandNodeHelper.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include <algorithm>
#include <map>
#include <utility>

namespace heisenberg
{
    // Check if a MISC item is a scripted "placement" item from a known mod.
    // These items have scripts that react to OnContainerChanged — when loot-to-hand
    // removes them from inventory (DropObject), the mod's script interprets it as
    // "place this item in the world" and triggers placement instead of physics drop.
    // Known mods: Sim Settlements 2 (building plans), Campsite (camping gear).
    // True if any of the form's source plugins matches a_pluginName (case-insensitive).
    // VR-safe: walks sourceFiles directly — TESForm::GetFile() uses a REL::ID that is wrong
    // on F4VR. The defining/override plugins all live in sourceFiles.
    static bool IsFormFromPlugin(RE::TESForm* baseForm, const char* a_pluginName)
    {
        if (!baseForm) return false;
        const auto* arr = baseForm->sourceFiles.array;
        if (!arr) return false;
        const std::uint32_t n = arr->size();
        for (std::uint32_t i = 0; i < n; ++i) {
            const RE::TESFile* f = (*arr)[i];
            if (!f) continue;
            const std::string_view fn = f->GetFilename();
            if (!fn.empty() && _stricmp(std::string(fn).c_str(), a_pluginName) == 0) {
                return true;
            }
        }
        return false;
    }

    static bool IsPlacementModItem(RE::TESForm* baseForm)
    {
        if (!baseForm) return false;

        // Placement / config mods (Place in Red VR, etc.): their control/config items are
        // activated from the Pipboy and re-added on use. DropToHand must NEVER touch anything
        // from these plugins — intercepting yanks the item to the hand (where it renders as
        // nothing) and the mod re-adds it, producing the inventory loop the user sees. Match
        // by source plugin, for ANY form type (the config item may be a holotape/aid/note).
        static const char* const kBypassPlugins[] = { "PlaceInRedVR.esp" };
        for (const char* plugin : kBypassPlugins) {
            if (IsFormFromPlugin(baseForm, plugin)) {
                spdlog::info("[DropToHand] Bypassing item {:08X} from placement plugin '{}'",
                             baseForm->GetFormID(), plugin);
                return true;
            }
        }

        // Only MISC items are affected by the editorID/name heuristics below
        if (baseForm->GetFormType() != RE::ENUM_FORM_ID::kMISC) {
            return false;
        }

        // Campsite mod: inventory items have editor IDs starting with "CampingKit_Inv"
        const char* editorID = baseForm->GetFormEditorID();
        if (editorID && std::strstr(editorID, "CampingKit_Inv")) {
            spdlog::debug("[LootToHand] Detected Campsite placement item: '{}'", editorID);
            return true;
        }

        // Sim Settlements 2: plans have "Plan:" or "SS2" in the display name
        auto* boundObj = baseForm->As<RE::TESBoundObject>();
        if (boundObj) {
            std::string itemName(RE::TESFullName::GetFullName(*boundObj, false));
            if (itemName.find("Plan:") != std::string::npos ||
                itemName.find("Plan -") != std::string::npos ||
                itemName.find("SS2") != std::string::npos) {
                spdlog::debug("[LootToHand] Detected SS2 plan: '{}'", itemName);
                return true;
            }
        }

        return false;
    }

    // True when a crafting/cooking/vendor/workbench menu is open. Item-to-hand
    // must NOT fire in these contexts: cooking outputs, crafted weapon mods,
    // vendor purchases, etc. fire container-changed events that look like loot
    // but should land in inventory (otherwise items spawn in the hand, pile up,
    // and explode when the menu closes).
    static bool IsCraftingOrVendorMenuOpen()
    {
        auto& mc = MenuChecker::GetSingleton();
        if (mc.IsCookingOpen() || mc.IsExamineOpen() || mc.IsWorkshopOpen()) {
            return true;
        }
        auto* ui = RE::UI::GetSingleton();
        // Workbench crafting (chem/weapon/armor/PA) and vendor barter aren't
        // tracked by MenuChecker — query the UI directly.
        return ui && (ui->GetMenuOpen("CraftingMenu") || ui->GetMenuOpen("BarterMenu"));
    }

    void DropToHand::MarkAsRecentlyStored(std::uint32_t baseFormID)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _recentlyStored.push_back({ baseFormID, 0.0f });
    }

    bool DropToHand::WasRecentlyStored(std::uint32_t baseFormID)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& entry : _recentlyStored) {
            if (entry.baseFormID == baseFormID) {
                return true;
            }
        }
        return false;
    }

    void DropToHand::MarkAsRecentlyLootedFrom(std::uint32_t containerFormID)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        // Update existing entry or add new one
        for (auto& entry : _recentlyLootedFrom) {
            if (entry.containerFormID == containerFormID) {
                entry.timeLooted = 0.0f;  // Reset timer
                return;
            }
        }
        _recentlyLootedFrom.push_back({ containerFormID, 0.0f });
    }

    bool DropToHand::WasRecentlyLootedFrom(std::uint32_t containerFormID)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& entry : _recentlyLootedFrom) {
            if (entry.containerFormID == containerFormID) {
                return true;
            }
        }
        return false;
    }

    void DropToHand::CancelPendingDropByRefID(std::uint32_t refID)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = std::remove_if(_pendingDrops.begin(), _pendingDrops.end(),
            [refID](const PendingDrop& d) { return d.referenceFormID == refID; });
        if (it != _pendingDrops.end()) {
            _pendingDrops.erase(it, _pendingDrops.end());
            spdlog::debug("[DropToHand] Cancelled pending drop for RefID {:08X}", refID);
        }
    }

    void DropToHand::Initialize()
    {
        if (_initialized) {
            return;
        }

        // Always register for container changed events.
        // Even if enableDropToHand is off, QueueStoreAndGrab (weapon pickup) and
        // QueueDropToHand (cooking/smart grab) need the update loop to run.
        // Individual features gate themselves in ProcessEvent.
        auto* eventSource = RE::TESContainerChangedEvent::GetEventSource();
        if (eventSource) {
            eventSource->RegisterSink(this);
            spdlog::debug("[DropToHand] Registered for TESContainerChangedEvent");
            _initialized = true;
        } else {
            spdlog::error("[DropToHand] Failed to get TESContainerChangedEvent source");
        }
    }
    
    RE::BSEventNotifyControl DropToHand::ProcessEvent(
        const RE::TESContainerChangedEvent& a_event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*)
    {
        // Get player form ID
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return RE::BSEventNotifyControl::kContinue;
        }
        
        std::uint32_t playerFormID = player->GetFormID();

        // SESSION-READY GATE: Block all "to hand" interception during load.
        // During game load, container-changed events fire for engine-internal
        // transfers (survival status effects, perk items, NPC inventory fills).
        // These must not be intercepted by any to-hand system. The gate opens
        // a few seconds after LoadingMenu closes (set in MenuChecker).
        if (!IsSessionReady()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // ===== DIAGNOSTIC: Log ALL container change events =====
        {
            std::string formName;
            const char* editorID = "";
            auto* baseForm = RE::TESForm::GetFormByID(a_event.baseObjectFormID);
            if (baseForm) {
                auto nameView = RE::TESFullName::GetFullName(*baseForm, false);
                formName = std::string(nameView);
                editorID = baseForm->GetFormEditorID();
                if (!editorID) editorID = "";
            }
            spdlog::info("[ContainerEvent] base={:08X} '{}' edID='{}' old={:08X} new={:08X} ref={:08X} count={}"
                         " | dropMode={} lootMode={} harvestTH={} pipOpen={} containerOpen={}",
                a_event.baseObjectFormID, formName, editorID,
                a_event.oldContainerFormID, a_event.newContainerFormID,
                a_event.referenceFormID, a_event.itemCount,
                g_config.dropToHandMode, g_config.lootToHandMode, g_config.enableHarvestToHand,
                MenuChecker::GetSingleton().IsPipboyOpen(),
                MenuChecker::GetSingleton().IsContainerOpen());
        }

        // EARLY, ALL-PATH bypass for placement/config mod items (Place in Red VR's
        // 'PIRVR_Holotape', etc.). Their control items get used and re-added in a tight
        // loop; intercepting on EITHER the drop OR the loot path perpetuates it. The old
        // check only covered the drop path, so the re-add (loot path) kept the loop alive.
        // Bypass here, before any drop/loot logic, on every path.
        if (auto* earlyForm = RE::TESForm::GetFormByID(a_event.baseObjectFormID)) {
            if (IsPlacementModItem(earlyForm)) {
                return RE::BSEventNotifyControl::kContinue;
            }
        }

        // =====================================================================
        // DROP TO HAND: Player dropping item to world
        // =====================================================================
        // Only active when the Pipboy inventory is open. This prevents the
        // "consume-and-replace loop" caused by EquipObject calls from the favorites
        // wheel (e.g. stimpak): the consume fires a container-changed event with
        // refID=0 that looks identical to a drop, and the RefID=0 fallback below
        // would loot-to-hand another copy from inventory.
        if (g_config.dropToHandMode > 0 && MenuChecker::GetSingleton().IsPipboyOpen()) {
            // Check if this is the player dropping an item to the world
            // oldContainerFormID = player, newContainerFormID = 0 (world)
            if (a_event.oldContainerFormID == playerFormID && a_event.newContainerFormID == 0) {
                // Skip throwable weapons (grenades/mines) — these are throws, not drops.
                // The game removes them from inventory when readying/throwing, which fires
                // this event. DropToHand must NOT intercept grenade throws.
                // Skip ammo — firing consumes ammo from inventory, which fires this same event
                // (oldContainer=player, newContainer=0, refID=0), causing a bullet to spawn in hand.
                auto* baseForm = RE::TESForm::GetFormByID(a_event.baseObjectFormID);
                if (baseForm) {
                    // A consumable that was just USED (not dropped) is marked by the EquipObject hook
                    // right before the game consumes it. Using a chem/food/drink fires the same
                    // old=player->new=0 event as a drop, so without this an activated item (e.g. Jet)
                    // gets yanked to the hand even with consume-to-hand OFF. A real DROP never goes
                    // through EquipObject, so it's not marked and still grabs.
                    if (WasRecentlyStored(a_event.baseObjectFormID)) {
                        spdlog::debug("[DropToHand] Skipping {:08X} — consumed/handled via equip, not a drop",
                            a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    if (baseForm->GetFormType() == RE::ENUM_FORM_ID::kAMMO) {
                        spdlog::debug("[DropToHand] Skipping ammo {:08X} - consumed by weapon fire, not a drop",
                            a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    if (baseForm->IsWeapon()) {
                        auto* weapon = static_cast<RE::TESObjectWEAP*>(baseForm);
                        auto weaponType = weapon->weaponData.type.get();
                        if (weaponType == RE::WEAPON_TYPE::kGrenade || weaponType == RE::WEAPON_TYPE::kMine) {
                            spdlog::debug("[DropToHand] Skipping throwable weapon {:08X} (type={}) - grenade/mine throw, not a drop",
                                a_event.baseObjectFormID, static_cast<int>(weaponType));
                            return RE::BSEventNotifyControl::kContinue;
                        }
                    }
                    // Skip Power Armor frames and pieces — PA exit drops the chassis
                    // to the world; intercepting it breaks PA and spawns phantom items
                    if (Utils::IsPowerArmorPiece(baseForm) || Utils::IsPowerArmorFrame(baseForm)) {
                        spdlog::debug("[DropToHand] Skipping PA item {:08X} - PA entry/exit transfer",
                            a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    // Skip scripted placement items (Campsite tents, SS2 plans, etc.)
                    // These must completely bypass all our systems. Let the game
                    // drop the item normally so the mod's Papyrus script handles it.
                    if (IsPlacementModItem(baseForm)) {
                        spdlog::info("[DropToHand] Skipping placement mod item {:08X} - full bypass",
                            a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    // Skip non-playable engine items — Survival status effects
                    // (Peckish/Hungry/Parched/Dehydrated/Weary etc.) live as hidden
                    // ALCH items in player inventory. When the effect changes tier,
                    // the old one is removed (old=player, new=0, refID=real) and
                    // that looks identical to a player drop, flooding _pendingDrops
                    // and looping the pickup sound.
                    if (!baseForm->GetPlayable(nullptr)) {
                        spdlog::info("[DropToHand] Skipping non-playable item {:08X} '{}' (engine transfer)",
                            a_event.baseObjectFormID,
                            RE::TESFullName::GetFullName(*baseForm, false));
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    // Fallback: GetPlayable() is unreliable for HC_ status effects
                    // and omod items — catch them by editor ID prefix.
                    {
                        const char* editorID = baseForm->GetFormEditorID();
                        if (editorID && (std::strncmp(editorID, "HC_", 3) == 0 ||
                                         std::strncmp(editorID, "omod", 4) == 0)) {
                            spdlog::info("[DropToHand] Skipping engine item {:08X} '{}' edID='{}' (HC_/omod prefix)",
                                a_event.baseObjectFormID,
                                RE::TESFullName::GetFullName(*baseForm, false), editorID);
                            return RE::BSEventNotifyControl::kContinue;
                        }
                    }
                    // Survival HC status tokens (Parched/Thirsty/Dehydrated/...) are PLAYABLE ALCH
                    // with an empty editor ID, slipping past both guards above. They're invisible
                    // (no model) → a drop-to-hand waits forever for a 3D, times out, re-adds the
                    // token (re-applying its effect) and re-queues = the "status effects loop" bug.
                    // Real chems have a model and stay grabbable. Skip only model-less ALCH.
                    if (baseForm->GetFormType() == RE::ENUM_FORM_ID::kALCH) {
                        const char* modelPath = nullptr;
                        if (auto* asModel = baseForm->As<RE::TESModel>()) modelPath = asModel->GetModel();
                        if (!modelPath || modelPath[0] == '\0') {
                            spdlog::info("[DropToHand] Skipping model-less ALCH {:08X} (invisible survival/status token)",
                                a_event.baseObjectFormID);
                            return RE::BSEventNotifyControl::kContinue;
                        }
                    }
                }

                // Mode 2 (Holotapes Only): skip everything that isn't an actual holotape
                // (paper notes are excluded too — they're ordinary clutter).
                if (g_config.dropToHandMode == 2) {
                    if (!IsHolotapeNote(baseForm)) {
                        spdlog::debug("[DropToHand] Skipping non-holotape {:08X} (mode=HolotapesOnly)",
                            a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }

                spdlog::debug("[DropToHand] EVENT: Player dropped item BaseID: {:08X}, RefID: {:08X}",
                    a_event.baseObjectFormID, a_event.referenceFormID);

                // Item dropped from player inventory to world!
                if (a_event.referenceFormID != 0) {
                    std::lock_guard<std::mutex> lock(_mutex);
                    
                    // Check for duplicates - don't add if already queued
                    bool alreadyQueued = false;
                    for (const auto& existing : _pendingDrops) {
                        if (existing.referenceFormID == a_event.referenceFormID) {
                            alreadyQueued = true;
                            spdlog::debug("[DropToHand] RefID {:08X} already queued, skipping duplicate", a_event.referenceFormID);
                            break;
                        }
                    }
                    
                    if (!alreadyQueued) {
                        // Queue this drop for grabbing on next frame
                        PendingDrop drop;
                        drop.referenceFormID = a_event.referenceFormID;
                        drop.expectedBaseFormID = a_event.baseObjectFormID;
                        drop.timeQueued = 0.0f;  // Will be set properly on frame update

                        // Capture ref ID for Pipboy drop hook (so it can call SetPosition after DropItem)
                        if (_pipboyDropCapture) {
                            _pipboyDropRefID = a_event.referenceFormID;
                        }

                        _pendingDrops.push_back(drop);

                        spdlog::debug("[DropToHand] Queued dropped item RefID: {:08X}, BaseID: {:08X}",
                            a_event.referenceFormID, a_event.baseObjectFormID);
                    }
                } else {
                    // RefID=0: some items (e.g. quest holotapes) fire the drop event with no world
                    // reference assigned yet — or the item remains in inventory (scripted handling).
                    // Fall back to loot-to-hand: if the item is in inventory it will be delivered
                    // to hand; if it's actually on the ground the queue will time out cleanly.
                    spdlog::debug("[DropToHand] RefID is 0 for {:08X} — queuing loot-to-hand fallback",
                        a_event.baseObjectFormID);
                    bool isLeft = true;
                    GetTargetHand(isLeft);
                    std::lock_guard<std::mutex> lock(_mutex);
                    bool alreadyPending = false;
                    for (const auto& l : _pendingLoots) {
                        if (l.baseFormID == a_event.baseObjectFormID && l.forceHand) {
                            alreadyPending = true;
                            break;
                        }
                    }
                    if (!alreadyPending) {
                        PendingLoot loot;
                        loot.baseFormID = a_event.baseObjectFormID;
                        loot.itemCount = 1;
                        loot.timeQueued = 0.0f;
                        loot.forceHand = true;
                        loot.forcedIsLeft = isLeft;
                        loot.stickyGrab = false;
                        loot.markAsSmartGrab = false;
                        _pendingLoots.push_back(loot);
                    }
                }
            }
        }
        
        // =====================================================================
        // LOOT COOLDOWN: Track containers we recently looted from
        // Prevents accidentally storing items back to the same NPC
        // =====================================================================
        if (a_event.oldContainerFormID != 0 &&
            a_event.oldContainerFormID != playerFormID &&
            a_event.newContainerFormID == playerFormID) {
            MarkAsRecentlyLootedFrom(a_event.oldContainerFormID);
        }

        // =====================================================================
        // LOOT TO HAND: Item looted from container to player
        // =====================================================================
        if (g_config.lootToHandMode > 0) {  // Mode 1 (Hybrid) or 2 (Immersive)
            // Note: Loot to hand works in Power Armor, but PA pieces are filtered below

            // Check if this is an item coming FROM a container TO the player
            // oldContainerFormID != 0 (not world), != player, newContainerFormID = player
            if (a_event.oldContainerFormID != 0 &&
                a_event.oldContainerFormID != playerFormID &&
                a_event.newContainerFormID == playerFormID) {

                // A plugin doing a scripted bulk transfer (e.g. the companion
                // voice mod's "give me everything") signals us to keep these
                // items out of the hand/floor pipeline — they must land directly
                // in inventory. The engine event carries no transfer reason, so
                // the transferring plugin sets this window explicitly via F4SE
                // messaging just before its RemoveItem loop.
                if (IsItemToHandSuppressed()) {
                    spdlog::debug("[LootToHand] Suppressed (plugin API) — {:08X} from {:08X} stays in inventory",
                        a_event.baseObjectFormID, a_event.oldContainerFormID);
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Vendor purchases (BarterMenu) and workbench crafting transfers
                // must go to inventory, not the hand.
                if (IsCraftingOrVendorMenuOpen()) {
                    spdlog::debug("[LootToHand] Crafting/vendor menu open — {:08X} stays in inventory",
                        a_event.baseObjectFormID);
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Loot-to-hand fires when the player is actively looting via
                // ContainerMenu or a quick-loot-style menu (LootMenu / WSLootMenu).
                // Any other menu (workbench, cooking, pipboy, dialogue, workshop)
                // fires container-changed events for unrelated reasons (crafting
                // component transfers, vendor purchases, quest rewards) and must
                // not be routed to hand.
                {
                    auto& mc = MenuChecker::GetSingleton();
                    auto* ui = RE::UI::GetSingleton();
                    bool inAllowedLootContext =
                        mc.IsContainerOpen() ||
                        (ui && (ui->GetMenuOpen("LootMenu") || ui->GetMenuOpen("WSLootMenu")));
                    if (!inAllowedLootContext) {
                        spdlog::debug("[LootToHand] Skipping - no container/quickloot menu open");
                        return RE::BSEventNotifyControl::kContinue;
                    }
                }

                // Item looted from container to player!
                if (a_event.baseObjectFormID != 0) {
                    // Skip items we just stored via StoreGrabbedItem (anti-loop)
                    if (WasRecentlyStored(a_event.baseObjectFormID)) {
                        spdlog::debug("[LootToHand] Skipping recently stored item {:08X}", a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }

                    // Skip Power Armor pieces and frames - PA entry transfers these
                    // to player inventory; intercepting them breaks PA entry animation/sound
                    auto* baseForm = RE::TESForm::GetFormByID(a_event.baseObjectFormID);
                    if (baseForm && Utils::IsPowerArmorPiece(baseForm)) {
                        spdlog::debug("[LootToHand] Skipping PA piece: {:08X}", a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    if (baseForm && Utils::IsPowerArmorFrame(baseForm)) {
                        spdlog::debug("[LootToHand] Skipping PA frame: {:08X}", a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    
                    // Skip scripted placement items (SS2 plans, Campsite gear, etc.)
                    // These have scripts that trigger building/campsite placement when
                    // removed from inventory — loot-to-hand's DropObject would fire them.
                    if (IsPlacementModItem(baseForm)) {
                        spdlog::debug("[LootToHand] Skipping placement mod item: {:08X}", a_event.baseObjectFormID);
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    
                    std::lock_guard<std::mutex> lock(_mutex);

                    // Queue this item to be dropped from inventory on next frame
                    PendingLoot loot;
                    loot.baseFormID = a_event.baseObjectFormID;
                    loot.itemCount = a_event.itemCount;
                    loot.timeQueued = 0.0f;
                    loot.oldContainerFormID = a_event.oldContainerFormID;  // for Take-All bulk detection
                    loot.burstId = _lootBurstId;                            // same id for all events this frame

                    // Deliver to the hand that looted this container: whichever wand's
                    // viewcaster is pointing at the container being looted. Falls back
                    // to GetTargetHand() if neither wand resolves to it (wand moved).
                    for (int h = 0; h < 2; ++h) {
                        bool handIsLeft = (h == 0);
                        auto tgt = GetVRWandTargetHandle(handIsLeft).get();
                        if (tgt && tgt->formID == a_event.oldContainerFormID) {
                            // If the looting hand is the primary hand and a real weapon
                            // is drawn, that hand can't receive the item — use the off-hand.
                            const bool primaryIsLeft = VRInput::GetSingleton().IsLeftHandedMode();
                            if (handIsLeft == primaryIsLeft && g_heisenberg.GetCachedHasRealWeapon()) {
                                handIsLeft = !primaryIsLeft;
                            }
                            loot.forceHand = true;
                            loot.forcedIsLeft = handIsLeft;
                            spdlog::debug("[LootToHand] Looting hand = {} (wand targeting container {:08X})",
                                handIsLeft ? "left" : "right", a_event.oldContainerFormID);
                            break;
                        }
                    }

                    _pendingLoots.push_back(loot);

                    // Suppress "X added" HUD message — it displays next frame
                    heisenberg::Hooks::SetSuppressHUDMessages(true);
                    heisenberg::Hooks::ScheduleDeferredHUDUnsuppress(2);

                    spdlog::debug("[LootToHand] Queued looted item BaseID: {:08X} x{} from container {:08X}",
                        a_event.baseObjectFormID, a_event.itemCount, a_event.oldContainerFormID);
                }
            }
        }

        // =====================================================================
        // HARVEST TO HAND: Flora harvested → item appears from world (oldContainer=0)
        // =====================================================================
        if (g_config.enableHarvestToHand && g_config.lootToHandMode > 0) {
            // Harvest-to-hand is allowed when:
            //   - No menu is open (world harvest / flora pickup)
            //   - ContainerMenu is open (player is actively looting)
            //   - A quick-loot-style menu is open (LootMenu / WSLootMenu)
            // Blocked for everything else (workbench, cooking, pipboy, dialogue,
            // workshop, etc.) — those fire similar events for unrelated reasons.
            {
                // Harvest-to-hand is for the player physically harvesting flora.
                // The harvest window is stamped only when the player activates a
                // FLOR/TREE ref (see HookActivateRef). Requiring it keeps script
                // AddItem calls from unrelated mods — Fill'em Up bottles at a pump,
                // cooking/crafting outputs, etc., which fire the same old=0->player
                // event shape — from being yanked into the hand and piling up.
                if (!IsHarvestWindowOpen()) {
                    spdlog::debug("[HarvestToHand] No harvest window — {:08X} stays in inventory (not a flora harvest)",
                        a_event.baseObjectFormID);
                    return RE::BSEventNotifyControl::kContinue;
                }
                // And never while a crafting/cooking/vendor menu is open.
                if (IsCraftingOrVendorMenuOpen()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
            }
            // Harvest events: item comes from world (0) into player
            if (a_event.oldContainerFormID == 0 && a_event.newContainerFormID == playerFormID) {
                // Skip items we recently stored (anti-loop for grab→store→event cycle)
                if (!WasRecentlyStored(a_event.baseObjectFormID)) {
                    auto* baseForm = RE::TESForm::GetFormByID(a_event.baseObjectFormID);
                    if (baseForm) {
                        // Skip non-playable items — Survival mode status effects
                        // (Hungry, Parched, Weary, etc.) are non-playable ALCH items that
                        // the engine adds to the player with oldContainer=0, the same shape
                        // as a harvest event. Without this filter they get queued and the
                        // HUD suppress/unsuppress cycle spams on every tick.
                        if (!baseForm->GetPlayable(nullptr)) {
                            spdlog::debug("[HarvestToHand] Skipping non-playable item {:08X} '{}' (likely survival status effect)",
                                a_event.baseObjectFormID,
                                RE::TESFullName::GetFullName(*baseForm, false));
                            return RE::BSEventNotifyControl::kContinue;
                        }

                        // Skip scripted placement items (SS2 plans, Campsite gear, etc.)
                        if (IsPlacementModItem(baseForm)) {
                            spdlog::debug("[HarvestToHand] Skipping placement mod item: {:08X}", a_event.baseObjectFormID);
                            return RE::BSEventNotifyControl::kContinue;
                        }

                        auto formType = baseForm->GetFormType();
                        // Only intercept harvest-like items: ingredients, alchemy, misc
                        if (formType == RE::ENUM_FORM_ID::kINGR ||
                            formType == RE::ENUM_FORM_ID::kALCH ||
                            formType == RE::ENUM_FORM_ID::kMISC) {

                            std::lock_guard<std::mutex> lock(_mutex);
                            PendingLoot loot;
                            loot.baseFormID = a_event.baseObjectFormID;
                            loot.itemCount = a_event.itemCount;
                            loot.timeQueued = 0.0f;
                            _pendingLoots.push_back(loot);

                            // Suppress "X added" HUD message — it displays next frame
                            heisenberg::Hooks::SetSuppressHUDMessages(true);
                            heisenberg::Hooks::ScheduleDeferredHUDUnsuppress(2);

                            spdlog::debug("[HarvestToHand] Queued harvested item BaseID: {:08X} x{} (type={})",
                                a_event.baseObjectFormID, a_event.itemCount, static_cast<int>(formType));
                        }
                    }
                }
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    bool DropToHand::GetTargetHand(bool& outIsLeft)
    {
        auto& grabMgr = GrabManager::GetSingleton();
        bool leftHolding = grabMgr.IsGrabbing(true);
        bool rightHolding = grabMgr.IsGrabbing(false);
        
        // Check if player has a real weapon equipped (blocks PRIMARY hand grabs).
        // Use cached value updated once/frame to avoid per-call WEAPON CHECK spam.
        bool hasWeaponEquipped = g_heisenberg.GetCachedHasRealWeapon();
        
        // Determine which hand is the primary (weapon) hand
        // Primary = right hand normally, left hand in left-handed mode
        bool isLeftHandedMode = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
        bool primaryHandIsLeft = isLeftHandedMode;
        
        // Helper: Check if a given hand is blocked by weapon
        auto isBlockedByWeapon = [&](bool isLeft) -> bool {
            bool isPrimaryHand = (isLeft == primaryHandIsLeft);
            return isPrimaryHand && hasWeaponEquipped;
        };
        
        switch (g_config.dropToHandPreferredHand) {
            case 0:  // Left hand preferred
                // If left hand is blocked by weapon (left-handed mode), fall back to right
                if (isBlockedByWeapon(true)) {
                    spdlog::debug("[DropToHand] Left hand preferred but weapon equipped - using right hand");
                    outIsLeft = false;
                } else {
                    outIsLeft = true;
                }
                break;
            case 1:  // Right hand preferred
                // If right hand is blocked by weapon (normal mode), fall back to left
                if (isBlockedByWeapon(false)) {
                    spdlog::debug("[DropToHand] Right hand preferred but weapon equipped - using left hand");
                    outIsLeft = true;
                } else {
                    outIsLeft = false;
                }
                break;
            case 2:  // Whichever is free
            default:
                // Check off-hand first (it's never blocked by weapons)
                bool offHandIsLeft = !primaryHandIsLeft;
                bool offHandHolding = offHandIsLeft ? leftHolding : rightHolding;
                bool primaryHandHolding = primaryHandIsLeft ? leftHolding : rightHolding;

                if (!offHandHolding) {
                    // Off-hand is free - use it
                    outIsLeft = offHandIsLeft;
                } else if (!primaryHandHolding && !hasWeaponEquipped) {
                    // Primary hand is free AND no weapon equipped
                    outIsLeft = primaryHandIsLeft;
                } else {
                    // Both occupied or primary has weapon, use off-hand (will drop its item)
                    outIsLeft = offHandIsLeft;
                }
                break;
        }

        spdlog::debug("[DropToHand] GetTargetHand: LHmode={} primary={} weapon={} leftHold={} rightHold={} pref={} -> {}",
            isLeftHandedMode, primaryHandIsLeft ? "L" : "R", hasWeaponEquipped,
            leftHolding, rightHolding, g_config.dropToHandPreferredHand,
            outIsLeft ? "LEFT" : "RIGHT");
        return true;
    }
    
    bool DropToHand::TryGrabPendingDrop(const PendingDrop& drop)
    {
        spdlog::debug("[DropToHand] TryGrabPendingDrop: Processing RefID {:08X}", drop.referenceFormID);

        // Look up the reference by form ID
        auto* form = RE::TESForm::GetFormByID(drop.referenceFormID);
        if (!form) {
            spdlog::debug("[DropToHand] Could not find form {:08X}", drop.referenceFormID);
            return false;
        }
        
        auto* refrRaw = form->As<RE::TESObjectREFR>();
        if (!refrRaw) {
            spdlog::debug("[DropToHand] Form {:08X} is not a TESObjectREFR", drop.referenceFormID);
            return false;
        }
        // Hold NiPointer immediately to prevent ref from being freed during processing
        RE::NiPointer<RE::TESObjectREFR> refrHolder(refrRaw);
        auto* refr = refrHolder.get();

        // REFID RECYCLING CHECK: Verify the ref still holds the same base object we queued.
        // The engine can recycle temp RefIDs (FF-prefix) for completely different objects
        // (e.g. a dropped holotape RefID gets reused for a spawning ghoul).
        RE::TESBoundObject* baseObj = refr->GetObjectReference();
        if (drop.expectedBaseFormID != 0 && baseObj) {
            RE::TESFormID actualBase = baseObj->GetFormID();
            if (actualBase != drop.expectedBaseFormID) {
                spdlog::warn("[DropToHand] RefID {:08X} was recycled! Expected base {:08X}, got {:08X} ('{}') — retrying via loot path",
                    drop.referenceFormID, drop.expectedBaseFormID, actualBase,
                    RE::TESFullName::GetFullName(*baseObj, false));
                // Recover: re-add to inventory and re-queue through loot-to-hand path
                auto* expectedForm = RE::TESForm::GetFormByID(drop.expectedBaseFormID);
                auto* expectedBound = expectedForm ? expectedForm->As<RE::TESBoundObject>() : nullptr;
                if (expectedBound) {
                    // Safety: never AddObjectToContainer a creature/NPC form.
                    // Dynamic FF-prefixed form IDs can be recycled — if expectedBaseFormID
                    // was a holotape at drop time but is now a ghoul, skip to avoid spawning actors.
                    auto ft = expectedBound->GetFormType();
                    bool isSafeItemType = (ft == RE::ENUM_FORM_ID::kWEAP || ft == RE::ENUM_FORM_ID::kARMO ||
                                           ft == RE::ENUM_FORM_ID::kMISC || ft == RE::ENUM_FORM_ID::kALCH ||
                                           ft == RE::ENUM_FORM_ID::kNOTE || ft == RE::ENUM_FORM_ID::kAMMO ||
                                           ft == RE::ENUM_FORM_ID::kKEYM || ft == RE::ENUM_FORM_ID::kINGR ||
                                           ft == RE::ENUM_FORM_ID::kBOOK || ft == RE::ENUM_FORM_ID::kFLOR ||
                                           ft == RE::ENUM_FORM_ID::kSCRL);
                    if (!isSafeItemType) {
                        spdlog::warn("[DropToHand] Expected form {:08X} is now type {} — dynamic form was recycled, skipping retry to prevent actor spawn",
                            drop.expectedBaseFormID, static_cast<int>(ft));
                    } else {
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (player) {
                            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                            heisenberg::AddObjectToContainer(player, expectedBound, &nullExtra, 1, nullptr, 0);
                            bool isLeft = drop.forceHand ? drop.forcedIsLeft : true;
                            if (!drop.forceHand) GetTargetHand(isLeft);
                            QueueDropToHand(drop.expectedBaseFormID, isLeft, 1, drop.stickyGrab, drop.markAsSmartGrab);
                            spdlog::debug("[DropToHand] Re-queued {:08X} via loot-to-hand path", drop.expectedBaseFormID);
                        }
                    }
                }
                return true;  // Remove from drop queue
            }
        }

        // WEAPON EXCLUSION IN POWER ARMOR ONLY: Skip weapons when in PA - PA has special weapon mounts
        if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kWEAP && Utils::IsPlayerInPowerArmor()) {
            spdlog::debug("[DropToHand] Skipping weapon {:08X} in Power Armor - using native behavior", drop.referenceFormID);
            return true;  // Remove from queue, let native system handle it
        }

        // RAGDOLL EXCLUSION: Skip items with complex ragdoll physics that stretch when grabbed
        // Jangles the Moon Monkey (BaseID 0x00147B03) has multi-part ragdoll that doesn't handle KEYFRAMED mode
        constexpr uint32_t kJanglesBaseID = 0x00147B03;
        if (baseObj && baseObj->GetFormID() == kJanglesBaseID) {
            spdlog::debug("[DropToHand] Skipping Jangles {:08X} - ragdoll doesn't work with grab", drop.referenceFormID);
            return true;  // Remove from queue, let it drop naturally in front of player
        }

        // Check if the reference has a 3D model loaded
        auto* node = refr->Get3D();
        if (!node) {
            // Early recycling check: verify base form hasn't changed while we wait
            if (drop.expectedBaseFormID != 0 && baseObj && baseObj->GetFormID() != drop.expectedBaseFormID) {
                spdlog::warn("[DropToHand] RefID {:08X} recycled during 3D wait (expected {:08X}, now {:08X}) — retrying via loot path",
                    drop.referenceFormID, drop.expectedBaseFormID, baseObj->GetFormID());
                auto* expectedForm = RE::TESForm::GetFormByID(drop.expectedBaseFormID);
                auto* expectedBound = expectedForm ? expectedForm->As<RE::TESBoundObject>() : nullptr;
                if (expectedBound) {
                    auto ft = expectedBound->GetFormType();
                    bool isSafeItemType = (ft == RE::ENUM_FORM_ID::kWEAP || ft == RE::ENUM_FORM_ID::kARMO ||
                                           ft == RE::ENUM_FORM_ID::kMISC || ft == RE::ENUM_FORM_ID::kALCH ||
                                           ft == RE::ENUM_FORM_ID::kNOTE || ft == RE::ENUM_FORM_ID::kAMMO ||
                                           ft == RE::ENUM_FORM_ID::kKEYM || ft == RE::ENUM_FORM_ID::kINGR ||
                                           ft == RE::ENUM_FORM_ID::kBOOK || ft == RE::ENUM_FORM_ID::kFLOR ||
                                           ft == RE::ENUM_FORM_ID::kSCRL);
                    if (!isSafeItemType) {
                        spdlog::warn("[DropToHand] Expected form {:08X} is now type {} — dynamic form recycled during 3D wait, skipping retry",
                            drop.expectedBaseFormID, static_cast<int>(ft));
                    } else {
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (player) {
                            RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                            heisenberg::AddObjectToContainer(player, expectedBound, &nullExtra, 1, nullptr, 0);
                            bool isLeft = drop.forceHand ? drop.forcedIsLeft : true;
                            if (!drop.forceHand) GetTargetHand(isLeft);
                            QueueDropToHand(drop.expectedBaseFormID, isLeft, 1, drop.stickyGrab, drop.markAsSmartGrab);
                            spdlog::debug("[DropToHand] Re-queued {:08X} via loot-to-hand path", drop.expectedBaseFormID);
                        }
                    }
                }
                return true;  // Remove from drop queue
            }
            spdlog::debug("[DropToHand] RefID {:08X} has no 3D yet, waiting... (waited {:.2f}s)",
                drop.referenceFormID, drop.timeQueued);
            return false;  // Not ready yet, keep in queue
        }

        // CRITICAL: Some items have "__DummyRootNode" as the root. This can mean two things:
        // 1. A temporary placeholder (real mesh not loaded yet) - has no children
        // 2. The actual root node name (like Toy Truck) - has children with the real mesh
        // Check for children to distinguish between these cases.
        const char* nodeName = node->name.c_str();
        if (nodeName && (strcmp(nodeName, "__DummyRootNode") == 0 || nodeName[0] == '\0')) {
            RE::NiNode* asNode = node->IsNode();
            bool hasChildren = asNode && asNode->children.size() > 0;

            if (!hasChildren) {
                spdlog::debug("[DropToHand] RefID {:08X} has placeholder 3D '{}' with no children, waiting for real mesh... (waited {:.2f}s)",
                    drop.referenceFormID, nodeName, drop.timeQueued);
                return false;  // True placeholder with no children, keep in queue
            }
            // Has children - this IS the real root (like Toy Truck), proceed with grab
            spdlog::debug("[DropToHand] RefID {:08X} has root '{}' with {} children - real mesh loaded, proceeding",
                drop.referenceFormID, nodeName, asNode->children.size());
        }

        spdlog::debug("[DropToHand] RefID {:08X} has 3D node '{}', parent='{}'",
            drop.referenceFormID,
            node->name.c_str(),
            node->parent ? node->parent->name.c_str() : "NULL");

        // Log parent status (no longer wait - grab immediately for responsiveness)
        if (node->parent) {
            spdlog::debug("[DropToHand] Node '{}' has parent='{}' - grabbing",
                        node->name.c_str(), node->parent->name.c_str());
        } else {
            spdlog::debug("[DropToHand] Node '{}' has NULL parent - grabbing anyway (grab system takes over)",
                        node->name.c_str());
        }
        
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) {
            spdlog::warn("[DropToHand] No player nodes, cannot grab");
            return true;  // Remove from queue
        }
        
        // Determine target hand.
        // HOLOTAPES always go to the right hand (for Pip-Boy deck insertion) regardless of
        // forceHand. Paper notes are ordinary clutter — choose a hand normally.
        bool isLeft = true;
        if (baseObj && IsHolotapeNote(baseObj)) {
            isLeft = false;
            spdlog::debug("[DropToHand] Holotape -> right hand (always)");
        } else if (drop.forceHand) {
            isLeft = drop.forcedIsLeft;
            spdlog::debug("[DropToHand] Using forced hand: {} (from QueueStoreAndGrab)", isLeft ? "left" : "right");
        } else {
            GetTargetHand(isLeft);
        }
        
        auto& grabMgr = GrabManager::GetSingleton();
        
        // Check if a grab is already in progress for this item (race condition prevention)
        // This catches cases where StartGrabOnRef is called but hasn't set active=true yet
        if (_grabsInProgress.find(drop.referenceFormID) != _grabsInProgress.end()) {
            spdlog::debug("[DropToHand] Grab already in progress for {:08X}, skipping", drop.referenceFormID);
            return true;  // Already being grabbed, remove from queue
        }
        
        // Check if THIS item is already grabbed by either hand
        const auto& leftGrab = grabMgr.GetGrabState(true);
        const auto& rightGrab = grabMgr.GetGrabState(false);
        RE::TESObjectREFR* leftRefr = leftGrab.GetRefr();
        RE::TESObjectREFR* rightRefr = rightGrab.GetRefr();
        if ((leftGrab.active && leftRefr == refr) || 
            (rightGrab.active && rightRefr == refr)) {
            spdlog::debug("[DropToHand] Item {:08X} already grabbed, skipping", drop.referenceFormID);
            return true;  // Already grabbed, remove from queue
        }
        
        // If target hand is already holding something, skip this drop
        // Don't interrupt existing grabs
        if (grabMgr.IsGrabbing(isLeft)) {
            spdlog::debug("[DropToHand] Target {} hand occupied, skipping drop {:08X}",
                isLeft ? "left" : "right", drop.referenceFormID);
            return true;  // Remove from queue, let item fall naturally
        }

        // HOLSTER-FIRST: when this drop targets the WEAPON hand and a weapon is drawn, sheathe
        // it and WAIT (re-queue) until it's holstered before placing the item — otherwise the
        // item appears in hand while the weapon is still drawn (the holotape, always the
        // right/weapon hand, is the visible case). Must run BEFORE _grabsInProgress.insert below,
        // or the re-queued drop would be dropped by the "grab already in progress" guard above.
        // Frame cap is a failsafe so the queue can never stick.
        {
            auto* pc = f4vr::getPlayer();  // F4SEVR::PlayerCharacter — has actorState
            const bool targetIsWeaponHand = (isLeft == VRInput::GetSingleton().IsLeftHandedMode());
            if (pc && targetIsWeaponHand && pc->GetWeaponMagicDrawn()) {
                int& waited = _weaponSheatheWait[drop.referenceFormID];
                if (waited == 0) {
                    if (auto* rePc = RE::PlayerCharacter::GetSingleton())
                        rePc->DrawWeaponMagicHands(false);  // sheathe the drawn weapon (or fists)
                    spdlog::debug("[DropToHand] Weapon drawn on target hand — sheathing before placing {:08X}", drop.referenceFormID);
                }
                if (++waited < 90) {  // ~1.5s failsafe at 60fps
                    return false;     // keep in queue; retry next frame until holstered
                }
                spdlog::warn("[DropToHand] Sheathe wait timed out for {:08X} — placing anyway", drop.referenceFormID);
            }
            _weaponSheatheWait.erase(drop.referenceFormID);
        }
        
        // Log match quality for debugging (no filtering - all items go to hand)
        if (!drop.skipWeaponFilter && baseObj) {
            auto& offsetMgr = ItemOffsetManager::GetSingleton();
            auto offset = offsetMgr.GetOffsetWithFallback(refr, isLeft);
            if (offset.has_value()) {
                std::string itemName = ItemOffsetManager::GetItemName(refr);
                const char* qualityStr =
                    offset->matchQuality == OffsetMatchQuality::Exact ? "Exact" :
                    offset->matchQuality == OffsetMatchQuality::Dimensions ? "Dimensions" :
                    offset->matchQuality == OffsetMatchQuality::Partial ? "Partial" :
                    offset->matchQuality == OffsetMatchQuality::Fuzzy ? "Fuzzy" : "None";
                spdlog::debug("[DropToHand] Item '{}' has {} match quality - grabbing", itemName, qualityStr);
            }
        }
        
        // Mark this item as having a grab in progress BEFORE calling StartGrabOnRef
        // This prevents race conditions where the same item is processed twice
        _grabsInProgress.insert(drop.referenceFormID);

        // IMMEDIATE TELEPORT: Move item to hand position BEFORE starting grab
        // This prevents the item from being visible at its spawn location (falling from sky)
        // for even a single frame. StartGrabOnRef's instantGrab will finalize positioning.
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (wandNode && node) {
            RE::NiPoint3 handPos = wandNode->world.translate;
            node->local.translate = handPos;
            node->world.translate = handPos;
            refr->data.location.x = handPos.x;
            refr->data.location.y = handPos.y;
            refr->data.location.z = handPos.z;
            spdlog::debug("[DropToHand] Pre-teleported {:08X} to hand ({:.1f},{:.1f},{:.1f})",
                drop.referenceFormID, handPos.x, handPos.y, handPos.z);
        }

        // Log item info before grab
        spdlog::debug("[DropToHand] ========== GRABBING DROPPED ITEM ==========");
        spdlog::debug("[DropToHand] RefID: {:08X}, Node: '{}', Parent: '{}'",
            refr->formID,
            node->name.c_str(),
            node->parent ? node->parent->name.c_str() : "NULL");

        // Start a grab on this object using the SAME path as normal world grabs
        // stickyGrab: from drop struct - false for world weapon pickups, true for inventory drops
        // instantGrab=true: skip pull animation, teleport to hand immediately
        // skipWeaponEquip=true: don't auto-equip if it's a weapon
        // forceOffset=true: load item offset (including finger curls) even when item is close to hand
        bool grabStarted = grabMgr.StartGrabOnRef(refr, isLeft, drop.stickyGrab, true, true, true);  // instantGrab=true, skipWeaponEquip=true, forceOffset=true
        
        if (grabStarted) {
            spdlog::debug("[DropToHand] SUCCESS - Grabbed {:08X} in {} hand (stickyGrab={}, smartGrab={})",
                refr->formID, isLeft ? "left" : "right", drop.stickyGrab, drop.markAsSmartGrab);

            auto& state = grabMgr.GetGrab(isLeft);

            // Scale HOLOTAPES for hand size (Pip-Boy deck proportions). Paper notes keep their
            // native scale. Both get the brief storage-zone grace — a note/holotape can start
            // near the Pip-Boy (= storage zone) and shouldn't auto-store the instant it spawns.
            if (baseObj && baseObj->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                if (IsHolotapeNote(baseObj) && state.node) {
                    const float pipScale = PipboyInteraction::GetSingleton().GetFrikPipboyScale();
                    constexpr float BASE_HAND_SCALE = 0.7f;
                    float finalScale = BASE_HAND_SCALE * pipScale;

                    std::string_view nodeName(state.node->name.c_str());
                    if (nodeName.find("Hologram") != std::string_view::npos) {
                        constexpr float HOLOGRAM_COMPENSATION = 1.9f;
                        finalScale *= HOLOGRAM_COMPENSATION;
                        spdlog::debug("[DropToHand] Hologram variant '{}' scale={:.2f} (compensation={}x)",
                                    nodeName, finalScale, HOLOGRAM_COMPENSATION);
                    }

                    state.node->local.scale = finalScale;
                }
                state.skipStorageZone = true;
            }

            // Mark as smart grab if requested (prevents auto-storage)
            if (drop.markAsSmartGrab) {
                state.isFromSmartGrab = true;
            }

        } else {
            spdlog::warn("[DropToHand] FAILED to start grab on dropped item {:08X}", refr->formID);
            // Clear the in-progress flag on failure so it can be retried
            _grabsInProgress.erase(drop.referenceFormID);
        }
        spdlog::debug("[DropToHand] =========================================");
        
        // Clear the in-progress flag after successful grab - item is now in grabbed state
        // The actual grab state (leftGrab.active) will be true now
        _grabsInProgress.erase(drop.referenceFormID);
        
        return true;  // Remove from queue even if grab failed
    }
    
    bool DropToHand::TryDropPendingLoot(PendingLoot& loot)
    {
        spdlog::debug("[LootToHand] TryDropPendingLoot called for BaseID: {:08X}, waited {:.2f}s, forceHand={}", 
            loot.baseFormID, loot.timeQueued, loot.forceHand);
        
        // Look up the base form
        auto* baseForm = RE::TESForm::GetFormByID(loot.baseFormID);
        if (!baseForm) {
            spdlog::debug("[LootToHand] Could not find base form {:08X}", loot.baseFormID);
            return true;  // Remove from queue
        }
        
        auto* boundObj = baseForm->As<RE::TESBoundObject>();
        if (!boundObj) {
            spdlog::debug("[LootToHand] Form {:08X} is not a TESBoundObject", loot.baseFormID);
            return true;  // Remove from queue
        }
        
        // Skip weapons in Power Armor - PA has special weapon handling
        if (baseForm->GetFormType() == RE::ENUM_FORM_ID::kWEAP && Utils::IsPlayerInPowerArmor()) {
            spdlog::debug("[LootToHand] Skipping weapon {:08X} in Power Armor - using native behavior", loot.baseFormID);
            return true;  // Remove from queue
        }
        
        // Track if this is a weapon/armor - we'll check for exact match after spawning
        // when we have the refr and can check dimensions
        auto formType = baseForm->GetFormType();
        bool isWeaponOrArmor = (formType == RE::ENUM_FORM_ID::kWEAP || formType == RE::ENUM_FORM_ID::kARMO);
        
        // =====================================================================
        // ALL items attempt loot-to-hand - match quality is checked in drop path
        // The drop path will reject items with Partial/Fuzzy/None match quality
        // (Priority 7 or worse) and let them fall naturally
        // EXCEPTION: Items from QueueStoreAndGrab (forceHand=true) bypass quality check
        // =====================================================================
        std::string itemName(RE::TESFullName::GetFullName(*boundObj, false));
        spdlog::debug("[LootToHand] Item '{}' - attempting loot-to-hand (match quality checked after spawn)", itemName);
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;  // Keep in queue
        }
        
        // Check if player actually has this item in inventory
        auto* inventory = player->inventoryList;
        bool hasItem = false;
        if (inventory) {
            for (auto& item : inventory->data) {
                if (item.object && item.object->GetFormID() == loot.baseFormID) {
                    hasItem = true;
                    spdlog::debug("[LootToHand] Confirmed item {:08X} is in inventory", loot.baseFormID);
                    break;
                }
            }
        }
        
        if (!hasItem) {
            spdlog::debug("[LootToHand] Item {:08X} NOT in inventory yet, waiting...", loot.baseFormID);
            return false;  // Keep in queue, try again
        }
        
        // Determine which hand to use
        // HOLOTAPES always go to the right hand (for Pip-Boy deck insertion). Paper notes are
        // ordinary clutter — choose a hand normally.
        bool isLeft = true;
        if (IsHolotapeNote(boundObj)) {
            isLeft = false;
            spdlog::debug("[LootToHand] Holotape -> right hand (always)");
        } else if (loot.forceHand) {
            isLeft = loot.forcedIsLeft;
            spdlog::debug("[LootToHand] Using forced hand: {}", isLeft ? "left" : "right");
        } else {
            GetTargetHand(isLeft);
        }
        auto& grabMgr = GrabManager::GetSingleton();
        if (grabMgr.IsGrabbing(isLeft)) {
            // Hand is occupied - behavior depends on lootToHandMode
            // Mode 1 (Hybrid): let item go to inventory
            // Mode 2 (Immersive): drop item on floor at hand position
            if (g_config.lootToHandMode == 2) {
                spdlog::debug("[LootToHand] Target {} hand occupied - IMMERSIVE MODE: dropping on floor", 
                    isLeft ? "left" : "right");
                // Don't return - continue to drop the item, it will just fall to floor
                // since we won't queue it for grab
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                if (playerNodes) {
                    RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
                    if (wandNode) {
                        RE::NiPoint3 dropPos = wandNode->world.translate;
                        RE::NiPoint3 dropRot(0.0f, 0.0f, 0.0f);
                        RE::TESObjectREFR::RemoveItemData removeData(boundObj, loot.itemCount);
                        removeData.reason = RE::ITEM_REMOVE_REASON::KDropping;
                        removeData.dropLoc = &dropPos;
                        removeData.rotate = &dropRot;
                        heisenberg::Hooks::SetSuppressHUDMessages(true);
                        player->RemoveItem(removeData);
                        heisenberg::Hooks::SetSuppressHUDMessages(false);
                        spdlog::debug("[LootToHand] Dropped {:08X} x{} on floor (hands full)", 
                            loot.baseFormID, loot.itemCount);
                    }
                }
                return true;  // Remove from queue
            } else {
                // Mode 1 (Hybrid) or fallback: let item go to inventory
                spdlog::debug("[LootToHand] Target {} hand occupied, letting item go to inventory", 
                    isLeft ? "left" : "right");
                return true;  // Remove from queue, don't drop
            }
        }
        
        spdlog::debug("[LootToHand] Hand {} is free, proceeding to drop", isLeft ? "left" : "right");
        
        // Get hand position for drop location
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) {
            return false;  // Keep in queue
        }
        
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
        if (!wandNode) {
            return false;  // Keep in queue
        }
        
        // Drop position: at the hand
        RE::NiPoint3 dropPos = wandNode->world.translate;
        RE::NiPoint3 dropRot(0.0f, 0.0f, 0.0f);
        
        // For stacks, only drop 1 item to hand - the rest stay in inventory
        // EXCEPTION: Ammo drops the full stack (more intuitive for ammo pickups)
        // Example: Loot 10 buffout → 1 goes to hand, 9 stay in inventory
        // Example: Loot 10 .308 ammo → all 10 go to hand
        bool isAmmo = (formType == RE::ENUM_FORM_ID::kAMMO);
        std::int32_t dropCount = isAmmo ? loot.itemCount : 1;
        
        spdlog::debug("[LootToHand] Dropping {:08X} x{} (of {} total) to hand{}", 
            loot.baseFormID, dropCount, loot.itemCount, isAmmo ? " (ammo - full stack)" : "");
        RE::TESObjectREFR::RemoveItemData removeData(boundObj, dropCount);
        removeData.reason = RE::ITEM_REMOVE_REASON::KDropping;
        removeData.dropLoc = &dropPos;
        removeData.rotate = &dropRot;
        
        // Suppress "X was removed" HUD notification during our RemoveItem call
        heisenberg::Hooks::SetSuppressHUDMessages(true);
        RE::ObjectRefHandle droppedHandle = player->RemoveItem(removeData);
        heisenberg::Hooks::SetSuppressHUDMessages(false);
        
        if (droppedHandle) {
            // Get the actual reference from the handle
            auto* droppedRefr = droppedHandle.get().get();
            if (droppedRefr) {
                spdlog::debug("[LootToHand] SUCCESS: Dropped item {:08X}, RefID: {:08X}", 
                    loot.baseFormID, droppedRefr->formID);
                
                // For weapons/armor, now that we have a refr, check for exact dimensions match
                // The early name check already filtered obvious non-matches, but dimensions
                // can provide additional matches for items with the same model
                // SKIP this check for forced hand grabs (QueueStoreAndGrab) - user explicitly grabbed from world
                // UPDATE: Always grab weapons/armor even without exact match - use generic fallback offset
                // The player explicitly looted this item, so they want it in their hand!
                if (isWeaponOrArmor && !loot.forceHand) {
                    auto& offsetMgr = ItemOffsetManager::GetSingleton();
                    std::string itemName = ItemOffsetManager::GetItemName(droppedRefr);
                    bool hasNameMatch = offsetMgr.HasOffset(itemName) || 
                                       offsetMgr.HasOffset(itemName + "_L") || 
                                       offsetMgr.HasOffset(itemName + "_R");
                    bool hasDimensionsMatch = offsetMgr.HasExactDimensionsMatch(droppedRefr);
                    
                    if (!hasNameMatch && !hasDimensionsMatch) {
                        // No exact match, but still grab it with generic fallback offset
                        spdlog::debug("[LootToHand] Weapon/armor '{}' has no exact match - using generic fallback offset", itemName);
                    } else {
                        spdlog::debug("[LootToHand] Weapon/armor '{}' has exact match, queueing for grab", itemName);
                    }
                }
                
                // Queue this reference for grabbing OR update existing entry
                // The event handler may have already added this RefID during RemoveItem callback
                // If so, we need to UPDATE that entry with our forceHand info, not add a duplicate
                // NOTE: Must hold _mutex here — ProcessEvent on another thread could be
                // modifying _pendingDrops concurrently.
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    bool foundExisting = false;
                    for (auto& existingDrop : _pendingDrops) {
                        if (existingDrop.referenceFormID == droppedRefr->formID) {
                            // Found existing entry from event handler - update it with forceHand info
                            existingDrop.skipWeaponFilter = loot.forceHand;
                            existingDrop.stickyGrab = loot.stickyGrab;
                            existingDrop.forceHand = loot.forceHand;
                            existingDrop.forcedIsLeft = loot.forcedIsLeft;
                            existingDrop.markAsSmartGrab = loot.markAsSmartGrab;
                            foundExisting = true;
                            spdlog::debug("[LootToHand] Updated existing entry for RefID {:08X} with forceHand={}, forcedIsLeft={}",
                                droppedRefr->formID, loot.forceHand, loot.forcedIsLeft);
                            break;
                        }
                    }

                    if (!foundExisting) {
                        // No existing entry - add new one
                        PendingDrop drop;
                        drop.referenceFormID = droppedRefr->formID;
                        drop.expectedBaseFormID = loot.baseFormID;
                        drop.timeQueued = 0.0f;
                        drop.skipWeaponFilter = loot.forceHand;  // Skip weapon filter for QueueStoreAndGrab items
                        drop.stickyGrab = loot.stickyGrab;       // Pass through sticky grab setting
                        drop.forceHand = loot.forceHand;         // Pass through forced hand preference
                        drop.forcedIsLeft = loot.forcedIsLeft;   // Pass through which hand was forced
                        drop.markAsSmartGrab = loot.markAsSmartGrab;  // Pass through smart grab flag
                        _pendingDrops.push_back(drop);

                        spdlog::debug("[LootToHand] Added new entry for RefID {:08X} to _pendingDrops, forceHand={}, forcedIsLeft={}",
                            droppedRefr->formID, drop.forceHand, drop.forcedIsLeft);
                    }
                }
                
                // Show HUD message for items that stayed in inventory (stacks > 1)
                // Skip for ammo - ammo drops full stack so nothing stays in inventory
                // Example: Loot 3 stimpaks → 1 to hand, show "Stimpak (2) was stored"
                // Example: Loot 10 .308 ammo → all 10 to hand, no message
                int storedCount = loot.itemCount - dropCount;  // How many stayed in inventory
                if (storedCount > 0 && boundObj && g_config.showStorageMessages) {
                    // Must convert to std::string - GetFullName returns string_view that may reference temporary data
                    std::string itemName(RE::TESFullName::GetFullName(*boundObj, false));
                    if (!itemName.empty()) {
                        std::string msg;
                        if (storedCount > 1) {
                            msg = std::format("{} ({}) was stored", itemName, storedCount);
                        } else {
                            msg = std::format("{} was stored", itemName);
                        }
                        heisenberg::Hooks::ShowHUDMessageDirect(msg.c_str());
                        spdlog::debug("[LootToHand] Showed storage message: '{}'", msg);
                    }
                }
            } else {
                spdlog::debug("[LootToHand] Handle valid but get() returned null for {:08X}", loot.baseFormID);
            }
        } else {
            spdlog::debug("[LootToHand] FAILED: DropObject returned null for {:08X}", loot.baseFormID);
        }
        
        return true;  // Remove from queue
    }
    
    void DropToHand::OnFrameUpdate(float deltaTime)
    {
        if (!_initialized) {
            return;
        }

        // Advance the loot burst id once per frame. All container-changed events
        // the game fires between two OnFrameUpdate calls (e.g. a single "Take All")
        // share the previous id, so a bulk transfer is detectable by grouping
        // pending loots by burstId below.
        _lootBurstId++;

        // Cleanup expired RecentlyStored entries
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _recentlyStored.begin();
            while (it != _recentlyStored.end()) {
                it->timeStored += deltaTime;
                if (it->timeStored > RECENTLY_STORED_TIMEOUT) {
                    it = _recentlyStored.erase(it);
                } else {
                    ++it;
                }
            }
            // Cleanup expired RecentlyLootedFrom entries
            auto lootIt = _recentlyLootedFrom.begin();
            while (lootIt != _recentlyLootedFrom.end()) {
                lootIt->timeLooted += deltaTime;
                if (lootIt->timeLooted > RECENTLY_LOOTED_FROM_TIMEOUT) {
                    lootIt = _recentlyLootedFrom.erase(lootIt);
                } else {
                    ++lootIt;
                }
            }
        }

        // =====================================================================
        // Process pending loots (LootToHand) - drop items from inventory
        // IMPORTANT: We copy the list and release the mutex before calling
        // RemoveItem, because RemoveItem triggers TESContainerChangedEvent
        // which would deadlock if we still held the mutex.
        // Always process forceHand items (from QueueStoreAndGrab/QueueDropToHand)
        // regardless of lootToHandMode — they are explicit requests, not auto-loot.
        // =====================================================================
        {
            std::vector<PendingLoot> lootsToProcess;

            // Lock to update times and get items ready to process
            {
                std::lock_guard<std::mutex> lock(_mutex);

                // TAKE-ALL GUARD: a "Take All" queues many items from one container
                // at once. They all target the SAME hand (whichever wand pointed at
                // the container), so loot-to-hand can place at most ONE; DropObject'ing
                // the rest piles every ungrabbed item on the FLOOR. Desired behavior:
                // one item to the hand, everything else stays in the player's INVENTORY
                // (never the floor). So when >= threshold auto-loots from one container
                // are pending simultaneously, keep just one for hand delivery (only if a
                // hand is actually free) and cancel the rest. Grouped by container only
                // (NOT frame-burst) so it triggers even when Take-All spreads its events
                // across several frames. forceHand loots (explicit QueueStoreAndGrab /
                // QueueDropToHand) are deliberate single requests and are exempt.
                const int takeAllThreshold = g_config.lootToHandTakeAllThreshold;
                if (takeAllThreshold > 0) {
                    std::map<std::uint32_t, int> containerCounts;
                    for (const auto& l : _pendingLoots) {
                        if (l.forceHand) continue;
                        containerCounts[l.oldContainerFormID]++;
                    }
                    auto& gmXfer = GrabManager::GetSingleton();
                    const bool anyHandFree = !gmXfer.IsGrabbing(true) || !gmXfer.IsGrabbing(false);
                    const int keepToHand = anyHandFree ? 1 : 0;  // at most one item can land in a hand
                    for (const auto& [container, count] : containerCounts) {
                        if (count < takeAllThreshold) continue;
                        int kept = 0;
                        const std::size_t before = _pendingLoots.size();
                        _pendingLoots.erase(
                            std::remove_if(_pendingLoots.begin(), _pendingLoots.end(),
                                [&](const PendingLoot& l) {
                                    if (l.forceHand || l.oldContainerFormID != container) return false;
                                    if (kept < keepToHand) { ++kept; return false; }  // keep for hand
                                    return true;  // cancel -> stays in inventory
                                }),
                            _pendingLoots.end());
                        spdlog::info("[LootToHand] Take-All from container {:08X}: {} to hand, {} kept in inventory (threshold {})",
                            container, kept, before - _pendingLoots.size(), takeAllThreshold);
                    }
                }

                auto lootIt = _pendingLoots.begin();
                while (lootIt != _pendingLoots.end()) {
                    // Skip event-driven loots when lootToHandMode is off
                    if (!lootIt->forceHand && g_config.lootToHandMode == 0) {
                        lootIt = _pendingLoots.erase(lootIt);
                        continue;
                    }

                    lootIt->timeQueued += deltaTime;

                    // Timeout after 1 second
                    if (lootIt->timeQueued > 1.0f) {
                        spdlog::debug("[LootToHand] Loot {:08X} timed out", lootIt->baseFormID);
                        lootIt = _pendingLoots.erase(lootIt);
                        continue;
                    }

                    // Ready to process? (after 0.1s delay)
                    if (lootIt->timeQueued >= 0.1f) {
                        lootsToProcess.push_back(*lootIt);
                        lootIt = _pendingLoots.erase(lootIt);
                    } else {
                        ++lootIt;
                    }
                }
            }

            // Process outside the lock
            for (auto& loot : lootsToProcess) {
                if (!TryDropPendingLoot(loot)) {
                    // Re-add to queue if failed
                    std::lock_guard<std::mutex> lock(_mutex);
                    _pendingLoots.push_back(loot);
                }
            }
        }

        // =====================================================================
        // Process pending drops (DropToHand) - grab items dropped to world
        // Always process: drops come from event-driven path AND from forced
        // paths (QueueStoreAndGrab, QueueDropToHand). Feature-specific gating
        // is in ProcessEvent.
        //
        // Process pending drops even while Pipboy is open - SetPosition already
        // moved item to hand, grab system will lock it in place.
        // =====================================================================
        // IMPORTANT: We copy the list and release the mutex before calling
        // TryGrabPendingDrop, because grab processing can trigger
        // TESContainerChangedEvent which would deadlock if we still held the mutex.
        // (Same pattern as loot section above.)
        {
            std::vector<PendingDrop> dropsToProcess;
            std::vector<PendingDrop> dropsToKeep;
            std::vector<PendingDrop> timedOutDrops;  // Recover outside the lock to avoid deadlock

            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto& drop : _pendingDrops) {
                    // Skip event-driven drops when dropToHandMode is off
                    if (!drop.forceHand && g_config.dropToHandMode == 0) {
                        continue;  // discard
                    }

                    drop.timeQueued += deltaTime;

                    // Timeout after 2 seconds (item should be loaded by then)
                    if (drop.timeQueued > 2.0f) {
                        spdlog::warn("[DropToHand] Drop {:08X} timed out waiting for 3D (base {:08X}) — will retry via loot path",
                            drop.referenceFormID, drop.expectedBaseFormID);
                        if (drop.expectedBaseFormID != 0) {
                            timedOutDrops.push_back(drop);
                        }
                        continue;  // discard from drop queue
                    }

                    dropsToProcess.push_back(drop);
                }
                _pendingDrops.clear();
            }

            // Recover timed-out items OUTSIDE the lock (QueueDropToHand takes _mutex)
            for (auto& drop : timedOutDrops) {
                auto* expectedForm = RE::TESForm::GetFormByID(drop.expectedBaseFormID);
                auto* expectedBound = expectedForm ? expectedForm->As<RE::TESBoundObject>() : nullptr;
                if (expectedBound) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (player) {
                        RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                        heisenberg::AddObjectToContainer(player, expectedBound, &nullExtra, 1, nullptr, 0);
                        bool isLeft = drop.forceHand ? drop.forcedIsLeft : true;
                        if (!drop.forceHand) GetTargetHand(isLeft);
                        QueueDropToHand(drop.expectedBaseFormID, isLeft, 1, drop.stickyGrab, drop.markAsSmartGrab);
                        spdlog::debug("[DropToHand] Re-queued {:08X} via loot-to-hand path after timeout", drop.expectedBaseFormID);
                    }
                }
            }

            // Process outside the lock
            for (auto& drop : dropsToProcess) {
                if (!TryGrabPendingDrop(drop)) {
                    dropsToKeep.push_back(drop);
                }
            }

            // Re-add failed drops back to queue
            if (!dropsToKeep.empty()) {
                std::lock_guard<std::mutex> lock(_mutex);
                for (auto& drop : dropsToKeep) {
                    _pendingDrops.push_back(drop);
                }
            }
        }
    }
    
    bool DropToHand::QueueStoreAndGrab(RE::TESObjectREFR* refr, bool isLeft)
    {
        if (!refr) {
            spdlog::warn("[StoreAndGrab] Null reference");
            return false;
        }
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            spdlog::warn("[StoreAndGrab] No player");
            return false;
        }
        
        // Get the base object BEFORE we store (the reference will be deleted)
        auto* baseObj = refr->data.objectReference;
        if (!baseObj) {
            spdlog::warn("[StoreAndGrab] No base object for {:08X}", refr->formID);
            return false;
        }
        
        std::uint32_t baseFormID = baseObj->GetFormID();
        spdlog::debug("[StoreAndGrab] Storing world object {:08X} (base {:08X}) to inventory, then dropping to {} hand",
            refr->formID, baseFormID, isLeft ? "left" : "right");
        
        // Suppress HUD message - we'll show our own when item goes to hand
        heisenberg::Hooks::SetSuppressHUDMessages(true);
        
        // Store the world object to inventory via ActivateRef
        bool result = refr->ActivateRef(player, nullptr, 1, false, false, false);
        
        heisenberg::Hooks::SetSuppressHUDMessages(false);
        
        if (!result) {
            spdlog::warn("[StoreAndGrab] ActivateRef failed for {:08X}", refr->formID);
            return false;
        }
        
        spdlog::debug("[StoreAndGrab] ActivateRef succeeded, queueing for loot-to-hand");
        
        // Queue this item for loot-to-hand with forced hand
        // stickyGrab=false: World weapon pickups should NOT be sticky - release on grip release
        {
            std::lock_guard<std::mutex> lock(_mutex);
            PendingLoot loot;
            loot.baseFormID = baseFormID;
            loot.itemCount = 1;
            loot.timeQueued = 0.0f;
            loot.forceHand = true;
            loot.forcedIsLeft = isLeft;
            loot.stickyGrab = false;  // World weapon pickup - not sticky
            _pendingLoots.push_back(loot);
        }
        
        spdlog::debug("[StoreAndGrab] Queued base {:08X} for drop to {} hand (non-sticky)", baseFormID, isLeft ? "left" : "right");
        return true;
    }

    void DropToHand::QueueDropToHand(RE::TESFormID baseFormID, bool isLeft, int itemCount, bool stickyGrab, bool markAsSmartGrab)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        PendingLoot loot;
        loot.baseFormID = baseFormID;
        loot.itemCount = itemCount;
        loot.timeQueued = 0.0f;
        loot.forceHand = true;
        loot.forcedIsLeft = isLeft;
        loot.stickyGrab = stickyGrab;
        loot.markAsSmartGrab = markAsSmartGrab;
        _pendingLoots.push_back(loot);

        spdlog::debug("[DropToHand] Queued {:08X} x{} for drop to {} hand (sticky={}, smartGrab={})",
            baseFormID, itemCount, isLeft ? "left" : "right", stickyGrab, markAsSmartGrab);
    }
}
