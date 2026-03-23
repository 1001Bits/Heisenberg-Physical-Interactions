#include "Config.h"
#include "Hooks.h"

#include <SimpleIni.h>
#include <chrono>

namespace heisenberg
{
    static const char* kConfigPath = "Data/F4SE/Plugins/Heisenberg_F4VR.ini";
    static const char* kMCMSettingsPath = "Data/MCM/Settings/Heisenberg.ini";
    
    // Conversion factor: 1 game unit = 1.4 cm, so 1 cm = 0.714 game units
    static constexpr float CM_TO_GAME_UNITS = 1.0f / 1.4f;  // ≈ 0.714
    static constexpr float GAME_UNITS_TO_CM = 1.4f;
    
    // =========================================================================
    // EMBEDDED DEFAULT CONFIG
    // This is used when no INI file exists - user can create INI to override
    // =========================================================================
    static const char* kDefaultConfig = R"(; ============================================================================
; Heisenberg F4VR Configuration
; ============================================================================
; This file allows customization of object pickup, activation, and storage behavior.
; Delete this file to reset all settings to defaults.
; All distances are in centimeters.
; ============================================================================

[ObjectPickup]
; Allow picking up owned items
bAllowGrabbingOwnedItems = true

; Sticky Pickup: Press grip once to pick up, press again to release
; When disabled (default), you must hold grip to keep holding objects
bEnableStickyGrab = false

; Weapon Equip Mode: How weapons are equipped when picked up
; 1 = Zone-based - drop weapon on hands to equip
iWeaponEquipMode = 1
bEnableVHHolstering = true

; Max telekinesis distance (in centimeters)
; Objects beyond this distance cannot be grabbed via telekinesis (3cm default)
fMaxGrabDistance = 3.0

; Natural pickup distance threshold (in centimeters)
; Objects within this distance use natural pickup (picked up at touch point)
fNaturalGrabDistance = 5.0

; Extended natural pickup distance for items without an exact offset match
; Items matched by fuzzy/partial matching (not FormID or exact name) use this
; Higher value prevents potentially bad offsets from being applied
fNaturalGrabDistanceNoMatch = 10.0

; Palm Snap: When enabled, mid-range objects snap to palm center
; When disabled, all objects use natural pickup regardless of distance
bEnablePalmSnap = true

; Throw velocity multiplier (1.0 = normal, 2.0 = double speed)
fThrowVelocityBoostFactor = 1.0

; Disable grip from drawing/sheathing weapons (grip is for grabbing)
bDisableGripWeaponDraw = true

; Auto-save natural pickup positions as item offsets
; When true, the first time you pick up an item naturally (close range),
; the position is saved so future pickups use the same offset
bSaveNaturalGrabAsOffset = false

; Neutral hand pose control (when not grabbing):
; 0 = FRIK (controller tracking resumes after release - trigger/thumbstick curl fingers)
; 1 = Heisenberg (hands stay open after release, no FRIK controller tracking)
iFingerPoseMode = 0

; Finger animation speeds (values per second, higher = faster)
fFingerAnimCloseSpeed = 4.0
fFingerAnimOpenSpeed = 3.0

[ItemPositioning]
; Enable Item Positioning Mode (allows adjusting how items are held)
bEnableItemPositioning = false

; Shortcut to enter Item Positioning Mode while holding an item
; 0 = Left Thumbstick Click (long press)
; 1 = Right Thumbstick Click (long press)
; 2 = Long Press A Button
; Press B to save position, use shortcut again to exit without saving
iItemPositioningShortcut = 0

[Throwables]
; Enable grenade handling by the mod
; When true: mod manages grenade input (remap, zones, blocking)
; When false: game handles grenades natively (grip = throw, no mod interference)
bEnableGrenadeHandling = true

; Remap grenade button from Grip to A button (only if bEnableGrenadeHandling=true)
; When enabled: A button readies throwables instead of grip
bRemapGrenadeButtonToA = false

; Enable throwable activation zone (only if bEnableGrenadeHandling=true)
; 0 = Disabled (grenades work everywhere)
; 1 = Enabled (grenades only work in the zone below)
iThrowableActivationZone = 0

; Radius of the throwable activation zone (in cm)
fThrowableZoneRadius = 21.0

; Throwable zone position relative to HMD
; X = left(+) / right(-), Y = forward(+) / behind(-), Z = up(+) / down(-)
; Default: chest level (below and slightly behind HMD)
fThrowableZoneOffsetX = 0.0
fThrowableZoneOffsetY = -5.0
fThrowableZoneOffsetZ = -20.0

[ItemStorage]
; Enable item storage zone system
bEnableItemStorageZones = true

; Radius of storage zone (in cm)
fItemStorageZoneRadius = 19.0

; Storage zone position relative to HMD
; X = left(+) / right(-), Y = forward(+) / behind(-), Z = up(+) / down(-)
; Default: behind and below head
; With radius 19 and Y=-20, front edge is at -1cm (just behind HMD)
fStorageZoneOffsetX = 0.0
fStorageZoneOffsetY = -20.0
fStorageZoneOffsetZ = -5.0

; Auto-storage delay - how long to hold item in storage zone before it's stored
; Item automatically stores after this many seconds in the zone (no grip release needed)
fStorageZoneHoldTime = 0.5

[Consumables]
; Activation zone for consumables (where to bring items to use them)
; 0 = Disabled (no quick-consume feature)
; 1 = Mouth/Face area
iConsumableActivationZone = 1

; Radius of the mouth zone for consuming items (in cm)
; Default: 11.0 (matches HIGGS Skyrim)
fMouthRadius = 11.0

; Mouth zone offset from HMD (in cm, HMD-relative)
; X = left(-)/right(+), Y = forward(-)/back(+), Z = up(+)/down(-)
; Defaults match HIGGS Skyrim exactly
fMouthOffsetX = 0.0
fMouthOffsetY = 10.0
fMouthOffsetZ = -9.0

; Max hand speed to consume items (m/s) - must be moving slower than this
; Prevents accidental consumes when quickly moving hand through zone
; Default: 2.0
fMouthVelocityThreshold = 2.0

; Block manual consumption/chem use while wearing Power Armor
; (helmet prevents bringing items to mouth/hand)
bBlockConsumptionInPA = true

; Consumable To Hand: Press trigger on food/drink/chems in Pipboy
; to drop them to your hand instead of consuming them
bConsumableToHand = false

[DropToHand]
; Drop to Hand: Items dropped from inventory spawn in your hand instead of falling
; If you drop another item while holding one, the held item drops first
bEnableDropToHand = true

; Loot to Hand Mode: How items looted from containers/quickloot are handled
; 0 = Off (native behavior - items go directly to inventory)
; 1 = Hybrid (items go to hand, if blocked go to inventory)
; 2 = Immersive (items go to hand, if blocked drop on floor)
; "Blocked" means: hand occupied or holding a weapon
iLootToHandMode = 1

; Preferred hand for dropped items
; 0 = Left hand
; 1 = Right hand
; 2 = Free hand (whichever is not holding something)
iDropToHandPreferredHand = 2

; Consumables selected from Favorites menu appear in hand instead of being consumed
bFavoritesToHand = false

; Items pickpocketed from NPCs appear in your hand (requires Loot To Hand Hybrid or Immersive)
bEnableStealToHand = true

; Harvested flora items appear in your hand instead of going to inventory
bEnableHarvestToHand = false

[Activators]
; Enable touch-based button/switch activation
bEnableInteractiveActivators = true

; Distance to start pointing pose (game units, ~25 = 25cm)
fActivatorPointingRadius = 25.0

; Distance to trigger activation (game units, ~8 = 8cm)  
fActivatorActivationRadius = 8.0

; Cooldown between activations (milliseconds)
fActivatorCooldownMs = 1000.0

[Cooking]
; Cook food by holding it near heat sources (campfires, stoves, etc.)
bEnableCooking = true

[Pipboy]
; Allow Pipboy Hacking - hack terminals on your wrist Pipboy
; When enabled, terminals display on your wrist Pipboy instead of the projected full-screen UI.
; When disabled, terminals use the default projected display.
; Requires wrist Pipboy mode (iniAlwaysUseProjectedPipboy = 0).
bForceTerminalOnWrist = true
bHideTerminalExitPrompt = true

; Experimental: Render terminal UI onto in-world terminal screen meshes
; Instead of displaying terminals on the Pipboy wrist, the terminal UI texture
; is applied directly to the physical terminal's screen mesh in the world.
; Requires bForceTerminalOnWrist = false (or overrides it when enabled).
bEnableTerminalOnWorldScreen = false

; Holotape deck push close radius
; Distance at which your hand starts pushing the holotape deck closed (game units)
; Higher values = easier to close the deck by hand
fTapeDeckPushCloseRadius = 3.0

; Hide action prompts ([A] Take, [B] Transfer) from wand rollover HUD
; Item names still show. Binary patches ShowActivateButton/ShowSecondaryButton.
bHideWandHUD = true

; Hide all wand HUD messages (actions AND item display) when pointing with wands
bHideAllWandHUD = false

[Debug]
; Log level: 0=trace, 1=debug, 2=info, 3=warn, 4=error
; Default: 3 (warn) for release builds
iLogLevel = 4
)";

    void Config::Load()
    {
        spdlog::info("Loading config...");

        CSimpleIniA ini;
        ini.SetUnicode();
        
        // STEP 1: Load embedded defaults first (always available)
        SI_Error rc = ini.LoadData(kDefaultConfig, strlen(kDefaultConfig));
        if (rc < 0) {
            spdlog::error("Failed to load embedded default config!");
            return;
        }
        spdlog::debug("Loaded embedded default configuration");
        
        // STEP 2: Try to load external INI file as override (optional)
        CSimpleIniA externalIni;
        externalIni.SetUnicode();
        rc = externalIni.LoadFile(kConfigPath);
        if (rc >= 0) {
            spdlog::info("Found external INI file: {} - applying overrides", kConfigPath);
            
            // Merge external settings into ini (external values override defaults)
            // SimpleIni doesn't have a merge function, so we iterate sections
            CSimpleIniA::TNamesDepend sections;
            externalIni.GetAllSections(sections);
            for (const auto& section : sections) {
                CSimpleIniA::TNamesDepend keys;
                externalIni.GetAllKeys(section.pItem, keys);
                for (const auto& key : keys) {
                    const char* value = externalIni.GetValue(section.pItem, key.pItem, nullptr);
                    if (value) {
                        ini.SetValue(section.pItem, key.pItem, value);
                    }
                }
            }
        } else {
            spdlog::info("No external INI file found at {} - using embedded defaults", kConfigPath);
        }

        // STEP 3: Try to load MCM settings file as highest-priority override
        // MCM VR writes user changes to Data/MCM/Settings/Heisenberg.ini
        // config.json IDs match our section/key names so MCM file merges directly
        CSimpleIniA mcmIni;
        mcmIni.SetUnicode();
        rc = mcmIni.LoadFile(kMCMSettingsPath);
        if (rc >= 0) {
            spdlog::info("Found MCM settings file: {} - applying overrides", kMCMSettingsPath);
            CSimpleIniA::TNamesDepend sections;
            mcmIni.GetAllSections(sections);
            for (const auto& section : sections) {
                CSimpleIniA::TNamesDepend keys;
                mcmIni.GetAllKeys(section.pItem, keys);
                for (const auto& key : keys) {
                    const char* value = mcmIni.GetValue(section.pItem, key.pItem, nullptr);
                    if (value) {
                        ini.SetValue(section.pItem, key.pItem, value);
                    }
                }
            }
        } else {
            spdlog::debug("No MCM settings file at {} - using current settings", kMCMSettingsPath);
        }

        // Selection - Close range only
        proximityRadius = static_cast<float>(ini.GetDoubleValue("Selection", "fProximityRadius", proximityRadius));
        extendedGrabRange = ini.GetBoolValue("Selection", "bExtendedGrabRange", extendedGrabRange);
        
        // Seated mode settings
        enableSeatedMode = ini.GetBoolValue("SeatedMode", "bEnableSeatedMode", enableSeatedMode);
        seatedModeHeightThreshold = static_cast<float>(ini.GetDoubleValue("SeatedMode", "fSeatedModeHeightThreshold", seatedModeHeightThreshold));
        seatedModeGrabDistance = static_cast<float>(ini.GetDoubleValue("SeatedMode", "fSeatedModeGrabDistance", seatedModeGrabDistance));
        standingModeGrabDistance = static_cast<float>(ini.GetDoubleValue("SeatedMode", "fStandingModeGrabDistance", standingModeGrabDistance));
        nearCastRadius = static_cast<float>(ini.GetDoubleValue("Selection", "fNearCastRadius", nearCastRadius));
        nearCastDistance = static_cast<float>(ini.GetDoubleValue("Selection", "fNearCastDistance", nearCastDistance));
        requiredCastDotProduct = static_cast<float>(ini.GetDoubleValue("Selection", "fRequiredCastDotProduct", requiredCastDotProduct));
        closeGrabThreshold = static_cast<float>(ini.GetDoubleValue("Selection", "fCloseGrabThreshold", closeGrabThreshold));
        
        // Pull-to-hand
        enablePullToHand = ini.GetBoolValue("Selection", "bEnablePullToHand", enablePullToHand);
        pullSpeed = static_cast<float>(ini.GetDoubleValue("Selection", "fPullSpeed", pullSpeed));
        snapDistance = static_cast<float>(ini.GetDoubleValue("Selection", "fSnapDistance", snapDistance));
        enableTelekinesis = ini.GetBoolValue("Selection", "bEnableTelekinesis", enableTelekinesis);

        // ObjectPickup
        enableGrabbing = ini.GetBoolValue("ObjectPickup", "bEnableGrabbing", enableGrabbing);
        allowGrabbingOwnedItems = ini.GetBoolValue("ObjectPickup", "bAllowGrabbingOwnedItems", allowGrabbingOwnedItems);
        enableGrabActors = ini.GetBoolValue("ObjectPickup", "bEnableGrabActors", enableGrabActors);
        enableStickyGrab = ini.GetBoolValue("ObjectPickup", "bEnableStickyGrab", enableStickyGrab);
        weaponEquipMode = static_cast<int>(ini.GetLongValue("ObjectPickup", "iWeaponEquipMode", weaponEquipMode));
        enableVHHolstering = ini.GetBoolValue("ObjectPickup", "bEnableVHHolstering", enableVHHolstering);
        showHolsterMessages = ini.GetBoolValue("ObjectPickup", "bShowHolsterMessages", showHolsterMessages);
        showUnequipMessages = ini.GetBoolValue("ObjectPickup", "bShowUnequipMessages", showUnequipMessages);
        enableUnarmedAutoUnequip = ini.GetBoolValue("ObjectPickup", "bEnableUnarmedAutoUnequip", enableUnarmedAutoUnequip);
        disableGripWeaponDraw = ini.GetBoolValue("ObjectPickup", "bDisableGripWeaponDraw", disableGripWeaponDraw);
        fingerPoseMode = static_cast<int>(ini.GetLongValue("ObjectPickup", "iFingerPoseMode", fingerPoseMode));
        fingerAnimCloseSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerAnimCloseSpeed", fingerAnimCloseSpeed));
        fingerAnimOpenSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerAnimOpenSpeed", fingerAnimOpenSpeed));
        grabMode = static_cast<int>(ini.GetLongValue("ObjectPickup", "iGrabMode", grabMode));
        grabStartSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabStartSpeed", grabStartSpeed));
        grabStartAngularSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabStartAngularSpeed", grabStartAngularSpeed));
        
        // Pickup distance settings
        maxGrabDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fMaxGrabDistance", maxGrabDistance));
        enableNaturalGrab = ini.GetBoolValue("ObjectPickup", "bEnableNaturalGrab", enableNaturalGrab);
        naturalGrabDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fNaturalGrabDistance", naturalGrabDistance));
        naturalGrabDistanceNoMatch = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fNaturalGrabDistanceNoMatch", naturalGrabDistanceNoMatch));
        
        // Palm snap settings
        enablePalmSnap = ini.GetBoolValue("ObjectPickup", "bEnablePalmSnap", enablePalmSnap);
        saveNaturalGrabAsOffset = ini.GetBoolValue("ObjectPickup", "bSaveNaturalGrabAsOffset", saveNaturalGrabAsOffset);
        palmOffsetX = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmOffsetX", palmOffsetX));
        palmOffsetY = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmOffsetY", palmOffsetY));
        palmOffsetZ = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmOffsetZ", palmOffsetZ));
        paGrabOffsetX = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPAGrabOffsetX", paGrabOffsetX));
        paGrabOffsetY = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPAGrabOffsetY", paGrabOffsetY));
        paGrabOffsetZ = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPAGrabOffsetZ", paGrabOffsetZ));

        // Item positioning mode
        enableItemPositioning = ini.GetBoolValue("ItemPositioning", "bEnableItemPositioning", enableItemPositioning);
        itemPositioningShortcut = static_cast<int>(ini.GetLongValue("ItemPositioning", "iItemPositioningShortcut", itemPositioningShortcut));
        
        // Drop to hand
        // Backwards compat: old bool bEnableDropToHand maps to 0/1, new int iDropToHandMode adds mode 2
        if (ini.GetValue("DropToHand", "iDropToHandMode")) {
            dropToHandMode = static_cast<int>(ini.GetLongValue("DropToHand", "iDropToHandMode", dropToHandMode));
        } else {
            dropToHandMode = ini.GetBoolValue("DropToHand", "bEnableDropToHand", true) ? 1 : 0;
        }
        lootToHandMode = static_cast<int>(ini.GetLongValue("DropToHand", "iLootToHandMode", lootToHandMode));
        dropToHandPreferredHand = static_cast<int>(ini.GetLongValue("DropToHand", "iDropToHandPreferredHand", dropToHandPreferredHand));
        favoritesToHand = ini.GetBoolValue("DropToHand", "bFavoritesToHand", favoritesToHand);
        enableStealToHand = ini.GetBoolValue("DropToHand", "bEnableStealToHand", enableStealToHand);
        enableHarvestToHand = ini.GetBoolValue("DropToHand", "bEnableHarvestToHand", enableHarvestToHand);

        // Native throwables (can be fully disabled for native game handling)
        enableGrenadeHandling = ini.GetBoolValue("Throwables", "bEnableGrenadeHandling", enableGrenadeHandling);
        throwableActivationZone = static_cast<int>(ini.GetLongValue("Throwables", "iThrowableActivationZone", throwableActivationZone));
        throwableZoneRadius = static_cast<float>(ini.GetDoubleValue("Throwables", "fThrowableZoneRadius", throwableZoneRadius));
        remapGrenadeButtonToA = ini.GetBoolValue("Throwables", "bRemapGrenadeButtonToA", remapGrenadeButtonToA);
        // Throwable zone position (single zone)
        throwableZoneOffsetX = static_cast<float>(ini.GetDoubleValue("Throwables", "fThrowableZoneOffsetX", throwableZoneOffsetX));
        throwableZoneOffsetY = static_cast<float>(ini.GetDoubleValue("Throwables", "fThrowableZoneOffsetY", throwableZoneOffsetY));
        throwableZoneOffsetZ = static_cast<float>(ini.GetDoubleValue("Throwables", "fThrowableZoneOffsetZ", throwableZoneOffsetZ));
        
        // Companion / hand transfer
        enableDropToCompanion = ini.GetBoolValue("ItemStorage", "bEnableDropToCompanion", enableDropToCompanion);
        companionTransferRadius = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fCompanionTransferRadius", companionTransferRadius));
        handTransferRadius = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fHandTransferRadius", handTransferRadius));
        enableAutoStorage = ini.GetBoolValue("ItemStorage", "bEnableAutoStorage", enableAutoStorage);

        // Item Storage Zone
        enableItemStorageZones = ini.GetBoolValue("ItemStorage", "bEnableItemStorageZones", enableItemStorageZones);
        enableStorageZoneWeaponEquip = ini.GetBoolValue("ItemStorage", "bEnableStorageZoneWeaponEquip", enableStorageZoneWeaponEquip);
        showStorageMessages = ini.GetBoolValue("ItemStorage", "bShowStorageMessages", showStorageMessages);
        enableStorageZoneConfigMode = ini.GetBoolValue("ItemStorage", "bEnableStorageZoneConfigMode", enableStorageZoneConfigMode);
        itemStorageZoneRadius = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fItemStorageZoneRadius", itemStorageZoneRadius));
        // Storage zone position (single zone)
        storageZoneOffsetX = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneOffsetX", storageZoneOffsetX));
        storageZoneOffsetY = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneOffsetY", storageZoneOffsetY));
        storageZoneOffsetZ = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneOffsetZ", storageZoneOffsetZ));
        requireHandBehindHead = ini.GetBoolValue("ItemStorage", "bRequireHandBehindHead", requireHandBehindHead);
        behindHeadTolerance = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fBehindHeadTolerance", behindHeadTolerance));
        storageZoneHoldTime = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneHoldTime", storageZoneHoldTime));
        
        // Hand collision
        enableHandCollision = ini.GetBoolValue("ObjectPickup", "bEnableHandCollision", enableHandCollision);
        usePhysicsHandBodies = ini.GetBoolValue("ObjectPickup", "bUsePhysicsHandBodies", usePhysicsHandBodies);
        handCollisionRadius = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandCollisionRadius", handCollisionRadius));
        handPushVelocityThreshold = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushVelocityThreshold", handPushVelocityThreshold));
        handPushForceMultiplier = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushForceMultiplier", handPushForceMultiplier));
        
        // Consumable zones (mouth)
        consumableActivationZone = static_cast<int>(ini.GetLongValue("Consumables", "iConsumableActivationZone", consumableActivationZone));
        mouthOffsetX = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthOffsetX", mouthOffsetX));
        mouthOffsetY = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthOffsetY", mouthOffsetY));
        mouthOffsetZ = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthOffsetZ", mouthOffsetZ));
        mouthRadius = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthRadius", mouthRadius));
        mouthVelocityThreshold = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthVelocityThreshold", mouthVelocityThreshold));
        mouthDropHapticStrength = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthHapticStrength", mouthDropHapticStrength));
        blockConsumptionInPA = ini.GetBoolValue("Consumables", "bBlockConsumptionInPA", blockConsumptionInPA);
        consumableToHand = ini.GetBoolValue("Consumables", "bConsumableToHand", consumableToHand);
        holotapeToHand = ini.GetBoolValue("Consumables", "bHolotapeToHand", holotapeToHand);
        showConsumeMessages = ini.GetBoolValue("Consumables", "bShowConsumeMessages", showConsumeMessages);

        // Hand injection zone
        enableHandInjection = ini.GetBoolValue("Consumables", "bEnableHandInjection", enableHandInjection);
        handInjectionOffsetX = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionOffsetX", handInjectionOffsetX));
        handInjectionOffsetY = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionOffsetY", handInjectionOffsetY));
        handInjectionOffsetZ = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionOffsetZ", handInjectionOffsetZ));
        handInjectionRadius = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionRadius", handInjectionRadius));

        // Armor equip zones
        armorEquipZone = static_cast<int>(ini.GetLongValue("Equipping", "iArmorEquipZone", armorEquipZone));
        headZoneRadius = static_cast<float>(ini.GetDoubleValue("Equipping", "fHeadZoneRadius", headZoneRadius));
        chestZoneRadius = static_cast<float>(ini.GetDoubleValue("Equipping", "fChestZoneRadius", chestZoneRadius));
        legZoneRadius = static_cast<float>(ini.GetDoubleValue("Equipping", "fLegZoneRadius", legZoneRadius));

        // Throw
        throwVelocityThreshold = static_cast<float>(ini.GetDoubleValue("Throw", "fThrowVelocityThreshold", throwVelocityThreshold));
        throwVelocityBoostFactor = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fThrowVelocityBoostFactor", throwVelocityBoostFactor));
        tangentialVelocityLimit = static_cast<float>(ini.GetDoubleValue("Throw", "fTangentialVelocityLimit", tangentialVelocityLimit));

        // Haptics
        grabBaseHapticStrength = static_cast<float>(ini.GetDoubleValue("Haptics", "fGrabBaseHapticStrength", grabBaseHapticStrength));
        grabProportionalHapticStrength = static_cast<float>(ini.GetDoubleValue("Haptics", "fGrabProportionalHapticStrength", grabProportionalHapticStrength));
        collisionBaseHapticStrength = static_cast<float>(ini.GetDoubleValue("Haptics", "fCollisionBaseHapticStrength", collisionBaseHapticStrength));

        // Timing
        selectionLeewayTime = static_cast<float>(ini.GetDoubleValue("Timing", "fSelectionLeewayTime", selectionLeewayTime));
        triggerPressedLeewayTime = static_cast<float>(ini.GetDoubleValue("Timing", "fTriggerPressedLeewayTime", triggerPressedLeewayTime));
        pullApplyVelocityTime = static_cast<float>(ini.GetDoubleValue("Timing", "fPullApplyVelocityTime", pullApplyVelocityTime));

        // Highlighting — hardcoded off (causes crashes)

        // Cooking
        enableCooking = ini.GetBoolValue("Cooking", "bEnableCooking", enableCooking);
        cookTime = static_cast<float>(ini.GetDoubleValue("Cooking", "fCookTime", cookTime));
        cookDetectionRadius = static_cast<float>(ini.GetDoubleValue("Cooking", "fCookDetectionRadius", cookDetectionRadius));
        cookingStationOnly = ini.GetBoolValue("Cooking", "bCookingStationOnly", cookingStationOnly);

        // Smart Grab
        enableSmartGrab = ini.GetBoolValue("SmartGrab", "bEnableSmartGrab", enableSmartGrab);
        smartGrabHealthThreshold = static_cast<float>(ini.GetDoubleValue("SmartGrab", "fHealthThreshold", smartGrabHealthThreshold));
        smartGrabRadsThreshold = static_cast<float>(ini.GetDoubleValue("SmartGrab", "fRadsThreshold", smartGrabRadsThreshold));
        smartGrabAmmoThreshold = static_cast<float>(ini.GetDoubleValue("SmartGrab", "fAmmoThreshold", smartGrabAmmoThreshold));
        smartGrabIncludeHealth = ini.GetBoolValue("SmartGrab", "bIncludeHealth", smartGrabIncludeHealth);
        smartGrabIncludeFood = ini.GetBoolValue("SmartGrab", "bIncludeFood", smartGrabIncludeFood);
        smartGrabIncludeCombatChems = ini.GetBoolValue("SmartGrab", "bIncludeCombatChems", smartGrabIncludeCombatChems);
        smartGrabIncludeAntibiotics = ini.GetBoolValue("SmartGrab", "bIncludeAntibiotics", smartGrabIncludeAntibiotics);
        smartGrabIncludeCarryWeight = ini.GetBoolValue("SmartGrab", "bIncludeCarryWeight", smartGrabIncludeCarryWeight);
        smartGrabIncludeHeavyJunk = ini.GetBoolValue("SmartGrab", "bIncludeHeavyJunk", smartGrabIncludeHeavyJunk);

        // Interactive Activators
        enableInteractiveActivators = ini.GetBoolValue("Activators", "bEnableInteractiveActivators", enableInteractiveActivators);
        activatorPointingRadius = static_cast<float>(ini.GetDoubleValue("Activators", "fActivatorPointingRadius", activatorPointingRadius));
        activatorActivationRadius = static_cast<float>(ini.GetDoubleValue("Activators", "fActivatorActivationRadius", activatorActivationRadius));
        activatorCooldownMs = static_cast<float>(ini.GetDoubleValue("Activators", "fActivatorCooldownMs", activatorCooldownMs));
        activatorDebugLogging = ini.GetBoolValue("Activators", "bActivatorDebugLogging", activatorDebugLogging);
        activatorUseWhitelist = ini.GetBoolValue("Activators", "bActivatorUseWhitelist", activatorUseWhitelist);
        enableActivatorDiscoveryMode = ini.GetBoolValue("Activators", "bEnableActivatorDiscoveryMode", enableActivatorDiscoveryMode);

        // Pipboy / Terminal
        forceTerminalOnWrist = ini.GetBoolValue("Pipboy", "bForceTerminalOnWrist", forceTerminalOnWrist);
        hideTerminalExitPrompt = ini.GetBoolValue("Pipboy", "bHideTerminalExitPrompt", hideTerminalExitPrompt);
        tapeDeckPushCloseRadius = static_cast<float>(ini.GetDoubleValue("Pipboy", "fTapeDeckPushCloseRadius", tapeDeckPushCloseRadius));
        hideWandHUD = ini.GetBoolValue("Pipboy", "bHideWandHUD", hideWandHUD);
        hideAllWandHUD = ini.GetBoolValue("Pipboy", "bHideAllWandHUD", hideAllWandHUD);
        spdlog::info("[Config] hideWandHUD = {}, hideAllWandHUD = {}", hideWandHUD, hideAllWandHUD);
        introHolotapeGiven = ini.GetBoolValue("Pipboy", "bIntroHolotapeGiven", introHolotapeGiven);
        introHolotapePlayed = ini.GetBoolValue("Pipboy", "bIntroHolotapePlayed", introHolotapePlayed);
        enableTerminalOnWorldScreen = ini.GetBoolValue("Pipboy", "bEnableTerminalOnWorldScreen", enableTerminalOnWorldScreen);
        // Water Interaction
        enableWaterInteraction = ini.GetBoolValue("Water", "bEnableWaterInteraction", enableWaterInteraction);
        waterSplashScale = static_cast<float>(ini.GetDoubleValue("Water", "fSplashScale", waterSplashScale));
        enableWakeRipples = ini.GetBoolValue("Water", "bEnableWakeRipples", enableWakeRipples);
        wakeRippleAmount = static_cast<float>(ini.GetDoubleValue("Water", "fWakeRippleAmount", wakeRippleAmount));
        wakeRippleIntervalMs = static_cast<int>(ini.GetLongValue("Water", "iWakeRippleIntervalMs", wakeRippleIntervalMs));
        wakeMinDistance = static_cast<float>(ini.GetDoubleValue("Water", "fWakeMinDistance", wakeMinDistance));
        wakeMaxMultiplier = static_cast<float>(ini.GetDoubleValue("Water", "fWakeMaxMultiplier", wakeMaxMultiplier));
        enableWaterSplashEffects = ini.GetBoolValue("Water", "bEnableWaterSplashEffects", enableWaterSplashEffects);
        splashEffectEntryMagnitude = static_cast<float>(ini.GetDoubleValue("Water", "fSplashEffectEntryMagnitude", splashEffectEntryMagnitude));
        splashEffectExitMagnitude = static_cast<float>(ini.GetDoubleValue("Water", "fSplashEffectExitMagnitude", splashEffectExitMagnitude));
        enableWaterSplashNif = ini.GetBoolValue("Water", "bEnableWaterSplashNif", enableWaterSplashNif);
        waterSplashNifScale = static_cast<float>(ini.GetDoubleValue("Water", "fWaterSplashNifScale", waterSplashNifScale));

        // Pickpocket / Stealing
        enablePickpocket = ini.GetBoolValue("Pickpocket", "bEnablePickpocket", enablePickpocket);

        // Debug
        debugDrawControllers = ini.GetBoolValue("Debug", "bDebugDrawControllers", debugDrawControllers);
        debugLogging = ini.GetBoolValue("Debug", "bDebugLogging", debugLogging);
        logLevel = static_cast<int>(ini.GetLongValue("Debug", "iLogLevel", logLevel));

        // Apply log level from config
        spdlog::level::level_enum level = spdlog::level::err;
        if (debugLogging) {
            level = spdlog::level::debug;
        } else {
            switch (logLevel) {
            case 0: level = spdlog::level::trace; break;
            case 1: level = spdlog::level::debug; break;
            case 2: level = spdlog::level::info; break;
            case 3: level = spdlog::level::warn; break;
            case 4: level = spdlog::level::err; break;
            default: level = spdlog::level::err; break;
            }
        }
        spdlog::set_level(level);

        // =====================================================================
        // CONFIG VALIDATION - clamp values to safe ranges, warn on out-of-bounds
        // =====================================================================
        auto clampFloat = [](float& val, float minVal, float maxVal, const char* name) {
            if (std::isnan(val) || std::isinf(val)) {
                spdlog::warn("[CONFIG] {} is NaN/Inf, resetting to {}", name, minVal);
                val = minVal;
            } else if (val < minVal) {
                spdlog::warn("[CONFIG] {} = {} below minimum {}, clamping", name, val, minVal);
                val = minVal;
            } else if (val > maxVal) {
                spdlog::warn("[CONFIG] {} = {} above maximum {}, clamping", name, val, maxVal);
                val = maxVal;
            }
        };
        auto clampInt = [](int& val, int minVal, int maxVal, const char* name) {
            if (val < minVal) {
                spdlog::warn("[CONFIG] {} = {} below minimum {}, clamping", name, val, minVal);
                val = minVal;
            } else if (val > maxVal) {
                spdlog::warn("[CONFIG] {} = {} above maximum {}, clamping", name, val, maxVal);
                val = maxVal;
            }
        };

        // Distance/radius values (game units or cm, must be non-negative)
        clampFloat(maxGrabDistance, 0.0f, 500.0f, "fMaxGrabDistance");
        clampFloat(proximityRadius, 0.0f, 500.0f, "fProximityRadius");
        clampFloat(nearCastRadius, 0.0f, 500.0f, "fNearCastRadius");
        clampFloat(nearCastDistance, 0.0f, 1000.0f, "fNearCastDistance");
        clampFloat(closeGrabThreshold, 0.0f, 500.0f, "fCloseGrabThreshold");
        clampFloat(pullSpeed, 0.0f, 10000.0f, "fPullSpeed");
        clampFloat(snapDistance, 0.0f, 500.0f, "fSnapDistance");
        clampFloat(handCollisionRadius, 0.0f, 100.0f, "fHandCollisionRadius");
        clampFloat(throwableZoneRadius, 0.0f, 100.0f, "fThrowableZoneRadius");
        clampFloat(itemStorageZoneRadius, 0.0f, 100.0f, "fItemStorageZoneRadius");
        clampFloat(mouthRadius, 0.0f, 100.0f, "fMouthRadius");
        clampFloat(handInjectionRadius, 0.0f, 100.0f, "fHandInjectionRadius");
        clampFloat(headZoneRadius, 0.0f, 100.0f, "fHeadZoneRadius");
        clampFloat(chestZoneRadius, 0.0f, 100.0f, "fChestZoneRadius");
        clampFloat(legZoneRadius, 0.0f, 100.0f, "fLegZoneRadius");
        clampFloat(naturalGrabDistance, 0.0f, 100.0f, "fNaturalGrabDistance");
        clampFloat(naturalGrabDistanceNoMatch, 0.0f, 100.0f, "fNaturalGrabDistanceNoMatch");

        // Multipliers/thresholds (must be non-negative)
        clampFloat(throwVelocityThreshold, 0.0f, 10000.0f, "fThrowVelocityThreshold");
        clampFloat(throwVelocityBoostFactor, 0.0f, 100.0f, "fThrowVelocityBoostFactor");
        clampFloat(grabStartSpeed, 0.0f, 10000.0f, "fGrabStartSpeed");
        clampFloat(grabStartAngularSpeed, 0.0f, 10000.0f, "fGrabStartAngularSpeed");
        clampFloat(activatorCooldownMs, 0.0f, 60000.0f, "fActivatorCooldownMs");
        clampFloat(storageZoneHoldTime, 0.0f, 30.0f, "fStorageZoneHoldTime");

        // Haptic values (0.0 to 1.0 range)
        clampFloat(grabBaseHapticStrength, 0.0f, 1.0f, "fGrabBaseHapticStrength");
        clampFloat(grabProportionalHapticStrength, 0.0f, 1.0f, "fGrabProportionalHapticStrength");
        clampFloat(collisionBaseHapticStrength, 0.0f, 1.0f, "fCollisionBaseHapticStrength");

        // Enum values
        clampInt(grabMode, 0, 4, "iGrabMode");
        clampInt(weaponEquipMode, 0, 2, "iWeaponEquipMode");
        clampInt(throwableActivationZone, 0, 1, "iThrowableActivationZone");
        clampInt(consumableActivationZone, 0, 1, "iConsumableActivationZone");
        clampInt(lootToHandMode, 0, 2, "iLootToHandMode");
        clampInt(dropToHandPreferredHand, 0, 2, "iDropToHandPreferredHand");
        clampInt(logLevel, 0, 4, "iLogLevel");

        // Convert cm values from INI to game units for internal use
        // NOTE: enableTelekinesis is independent of enableNaturalGrab
        // They were previously synced but are now separate settings

        // User-facing values in INI are in cm for intuitive configuration
        maxGrabDistance *= CM_TO_GAME_UNITS;
        naturalGrabDistance *= CM_TO_GAME_UNITS;
        naturalGrabDistanceNoMatch *= CM_TO_GAME_UNITS;
        throwableZoneRadius *= CM_TO_GAME_UNITS;
        mouthRadius *= CM_TO_GAME_UNITS;
        mouthOffsetX *= CM_TO_GAME_UNITS;
        mouthOffsetY *= CM_TO_GAME_UNITS;
        mouthOffsetZ *= CM_TO_GAME_UNITS;
        handInjectionRadius *= CM_TO_GAME_UNITS;
        handInjectionOffsetX *= CM_TO_GAME_UNITS;
        handInjectionOffsetY *= CM_TO_GAME_UNITS;
        handInjectionOffsetZ *= CM_TO_GAME_UNITS;

        // Armor zones - convert cm to game units
        headZoneOffsetX *= CM_TO_GAME_UNITS;
        headZoneOffsetY *= CM_TO_GAME_UNITS;
        headZoneOffsetZ *= CM_TO_GAME_UNITS;
        headZoneRadius *= CM_TO_GAME_UNITS;
        
        chestZoneOffsetX *= CM_TO_GAME_UNITS;
        chestZoneOffsetY *= CM_TO_GAME_UNITS;
        chestZoneOffsetZ *= CM_TO_GAME_UNITS;
        chestZoneRadius *= CM_TO_GAME_UNITS;
        
        legZoneOffsetX *= CM_TO_GAME_UNITS;
        legZoneOffsetY *= CM_TO_GAME_UNITS;
        legZoneOffsetZ *= CM_TO_GAME_UNITS;
        legZoneRadius *= CM_TO_GAME_UNITS;

        spdlog::info("Config loaded");
    }

    void Config::Save()
    {
        spdlog::info("Saving config to {}", kConfigPath);

        CSimpleIniA ini;
        ini.SetUnicode();

        // Selection
        ini.SetDoubleValue("Selection", "fProximityRadius", proximityRadius, "; Game units - radius for proximity detection");
        
        // Seated mode settings
        ini.SetBoolValue("SeatedMode", "bEnableSeatedMode", enableSeatedMode, "; Enable automatic seated player detection");
        ini.SetDoubleValue("SeatedMode", "fSeatedModeHeightThreshold", seatedModeHeightThreshold, "; HMD height below this = seated (game units, ~110 = 110cm)");
        ini.SetDoubleValue("SeatedMode", "fSeatedModeGrabDistance", seatedModeGrabDistance, "; Extended grab range when seated (~150 = 1.5m)");
        ini.SetDoubleValue("SeatedMode", "fStandingModeGrabDistance", standingModeGrabDistance, "; Close grab range when standing (~40 = 40cm)");
        ini.SetDoubleValue("Selection", "fNearCastRadius", nearCastRadius);
        ini.SetDoubleValue("Selection", "fNearCastDistance", nearCastDistance);
        ini.SetDoubleValue("Selection", "fRequiredCastDotProduct", requiredCastDotProduct);
        ini.SetDoubleValue("Selection", "fCloseGrabThreshold", closeGrabThreshold, "; Within this distance = natural grab, beyond = palm snap (~5 = 5cm)");
        
        // Pull-to-hand
        ini.SetBoolValue("Selection", "bEnablePullToHand", enablePullToHand, "; Pull objects to your hand when grabbing");
        ini.SetDoubleValue("Selection", "fPullSpeed", pullSpeed, "; Game units/s - speed to pull objects");
        ini.SetDoubleValue("Selection", "fSnapDistance", snapDistance, "; Game units - distance at which object snaps to hand");
        ini.SetBoolValue("Selection", "bEnableTelekinesis", enableTelekinesis, "; Object follows hand from current distance (overrides pull-to-hand)");

        // ObjectPickup
        ini.SetBoolValue("ObjectPickup", "bEnableGrabbing", enableGrabbing, "; Master toggle for object pickup feature");
        ini.SetBoolValue("ObjectPickup", "bAllowGrabbingOwnedItems", allowGrabbingOwnedItems, "; Allow picking up items that would be considered stealing");
        ini.SetBoolValue("ObjectPickup", "bEnableStickyGrab", enableStickyGrab, "; Press grip once to pick up, again to release");
        ini.SetLongValue("ObjectPickup", "iWeaponEquipMode", weaponEquipMode, "; 0=Disabled, 1=Drop on weapon hand, 2=Auto equip");
        ini.SetBoolValue("ObjectPickup", "bEnableVHHolstering", enableVHHolstering, "; Drop weapon on VH holster zone to holster");
        ini.SetBoolValue("ObjectPickup", "bEnableUnarmedAutoUnequip", enableUnarmedAutoUnequip, "; Auto-unequip Unarmed when grip pressed (disable if melee broken)");
        ini.SetBoolValue("ObjectPickup", "bDisableGripWeaponDraw", disableGripWeaponDraw, "; Prevent grip from drawing/sheathing weapons");
        ini.SetLongValue("ObjectPickup", "iGrabMode", grabMode, "; 0=Keyframe, 3=VirtualSpring (recommended), 4=MouseSpring");
        ini.SetDoubleValue("ObjectPickup", "fGrabStartSpeed", grabStartSpeed);
        ini.SetDoubleValue("ObjectPickup", "fGrabStartAngularSpeed", grabStartAngularSpeed);
        
        ini.SetBoolValue("ObjectPickup", "bEnableNaturalGrab", enableNaturalGrab, "; Use natural grab for close objects (vs always palm snap)");

        // Pickup distance settings (convert back to cm for INI)
        ini.SetDoubleValue("ObjectPickup", "fMaxGrabDistance", maxGrabDistance * GAME_UNITS_TO_CM, "; cm - max distance for telekinesis grab. Object Pull will not activate within this range.");
        ini.SetDoubleValue("ObjectPickup", "fNaturalGrabDistance", naturalGrabDistance * GAME_UNITS_TO_CM, "; cm - within this distance = natural pickup (object follows touch point)");
        ini.SetDoubleValue("ObjectPickup", "fNaturalGrabDistanceNoMatch", naturalGrabDistanceNoMatch * GAME_UNITS_TO_CM, "; cm - natural pickup distance for items without exact offset match");
        
        // Palm snap settings
        ini.SetBoolValue("ObjectPickup", "bEnablePalmSnap", enablePalmSnap, "; Snap objects to palm center for mid-range pickups");
        ini.SetBoolValue("ObjectPickup", "bSaveNaturalGrabAsOffset", saveNaturalGrabAsOffset, "; Auto-save natural pickup positions as item offsets (default off)");
        ini.SetDoubleValue("ObjectPickup", "fPalmOffsetX", palmOffsetX, "; Game units - left/right offset (0 = centered)");
        ini.SetDoubleValue("ObjectPickup", "fPalmOffsetY", palmOffsetY, "; Game units - forward offset (toward fingers, ~5 = 5cm)");
        ini.SetDoubleValue("ObjectPickup", "fPalmOffsetZ", palmOffsetZ, "; Game units - up/down offset (~3.5 = 5cm up from wand)");
        ini.SetDoubleValue("ObjectPickup", "fPAGrabOffsetX", paGrabOffsetX, "; Game units - extra X offset when in power armor (0 = no shift)");
        ini.SetDoubleValue("ObjectPickup", "fPAGrabOffsetY", paGrabOffsetY, "; Game units - extra Y offset in PA (forward, out of glove)");
        ini.SetDoubleValue("ObjectPickup", "fPAGrabOffsetZ", paGrabOffsetZ, "; Game units - extra Z offset in PA (up, above palm)");

        // Item Positioning Mode
        ini.SetBoolValue("ItemPositioning", "bEnableItemPositioning", enableItemPositioning, "; Enable item positioning mode (hold L3 to configure)");
        ini.SetLongValue("ItemPositioning", "iItemPositioningShortcut", itemPositioningShortcut, "; 0=Left Thumbstick Click, 1=Right Thumbstick Click, 2=Long Press A");
        
        // Drop to Hand
        ini.SetLongValue("DropToHand", "iDropToHandMode", dropToHandMode, "; 0=Off, 1=All Items, 2=Holotapes Only");
        ini.SetLongValue("DropToHand", "iLootToHandMode", lootToHandMode, "; 0=Off, 1=Hybrid (blocked->inventory), 2=Immersive (blocked->floor)");
        ini.SetLongValue("DropToHand", "iDropToHandPreferredHand", dropToHandPreferredHand, "; 0=Left, 1=Right, 2=Whichever is free");
        ini.SetBoolValue("DropToHand", "bFavoritesToHand", favoritesToHand, "; Consumables from Favorites go to hand");
        ini.SetBoolValue("DropToHand", "bEnableStealToHand", enableStealToHand, "; Items stolen from NPCs go to hand");
        ini.SetBoolValue("DropToHand", "bEnableHarvestToHand", enableHarvestToHand, "; Harvested flora goes to hand");

        // Native Throwables (Grenades, Mines) - can be fully disabled for native game handling
        ini.SetBoolValue("Throwables", "bEnableGrenadeHandling", enableGrenadeHandling, "; false = game handles grenades natively (no mod interference), true = mod manages grenade input");
        ini.SetBoolValue("Throwables", "bRemapGrenadeButtonToA", remapGrenadeButtonToA, "; Remap grenade throw button from Grip to A (for Virtual Holsters compatibility)");;
        ini.SetLongValue("Throwables", "iThrowableActivationZone", throwableActivationZone, "; 0=Disabled, 1=Enabled (use zone defined by XYZ offsets)");
        ini.SetDoubleValue("Throwables", "fThrowableZoneRadius", throwableZoneRadius * GAME_UNITS_TO_CM, "; cm - radius of throwable activation zone");
        // Throwable zone position (single zone)
        ini.SetDoubleValue("Throwables", "fThrowableZoneOffsetX", throwableZoneOffsetX, "; game units - X offset (+ = left, - = right)");
        ini.SetDoubleValue("Throwables", "fThrowableZoneOffsetY", throwableZoneOffsetY, "; game units - Y offset (+ = forward, - = behind)");
        ini.SetDoubleValue("Throwables", "fThrowableZoneOffsetZ", throwableZoneOffsetZ, "; game units - Z offset (+ = up, - = down)");
        
        // Item Storage Zone
        ini.SetBoolValue("ItemStorage", "bEnableItemStorageZones", enableItemStorageZones, "; Enable item storage zone system");
        ini.SetBoolValue("ItemStorage", "bEnableStorageZoneConfigMode", enableStorageZoneConfigMode, 
            "; Enable storage zone config mode (hold A to enter). Controls: R-Trigger=set position, L-Stick=radius, B=save, A=exit");
        ini.SetDoubleValue("ItemStorage", "fItemStorageZoneRadius", itemStorageZoneRadius * GAME_UNITS_TO_CM, "; cm - radius of storage zone");
        // Storage zone position (HMD local space)
        ini.SetDoubleValue("ItemStorage", "fStorageZoneOffsetX", storageZoneOffsetX, "; game units - X offset (+ = left, - = right)");
        ini.SetDoubleValue("ItemStorage", "fStorageZoneOffsetY", storageZoneOffsetY, "; game units - Y offset (+ = forward, - = behind)");
        ini.SetDoubleValue("ItemStorage", "fStorageZoneOffsetZ", storageZoneOffsetZ, "; game units - Z offset (+ = up, - = down)");
        ini.SetBoolValue("ItemStorage", "bRequireHandBehindHead", requireHandBehindHead, "; Require hand to be behind head for storage (prevents storing from front)");
        ini.SetDoubleValue("ItemStorage", "fBehindHeadTolerance", behindHeadTolerance, "; game units - allow hand to be this far forward and still count as behind head (~cm)");
        ini.SetDoubleValue("ItemStorage", "fStorageZoneHoldTime", storageZoneHoldTime, "; seconds - how long to hold in zone before auto-storing");
        ini.SetBoolValue("ItemStorage", "bEnableDropToCompanion", enableDropToCompanion, "; Drop item near companion to give it to them");
        ini.SetDoubleValue("ItemStorage", "fCompanionTransferRadius", companionTransferRadius, "; Game units - proximity to detect companion");
        ini.SetDoubleValue("ItemStorage", "fHandTransferRadius", handTransferRadius, "; CM - skip companion/storage when hands this close");
        ini.SetBoolValue("ItemStorage", "bEnableAutoStorage", enableAutoStorage, "; Auto-store after holding in zone for duration");
        
        // Consumables (Mouth zone) - convert back to cm for INI
        ini.SetLongValue("Consumables", "iConsumableActivationZone", consumableActivationZone, "; 0=Disabled, 1=Mouth/Face");
        ini.SetDoubleValue("Consumables", "fMouthOffsetX", mouthOffsetX * GAME_UNITS_TO_CM, "; cm - left/right (0 = centered)");
        ini.SetDoubleValue("Consumables", "fMouthOffsetY", mouthOffsetY * GAME_UNITS_TO_CM, "; cm - forward from HMD (toward face)");
        ini.SetDoubleValue("Consumables", "fMouthOffsetZ", mouthOffsetZ * GAME_UNITS_TO_CM, "; cm - down from eye level (mouth area)");
        ini.SetDoubleValue("Consumables", "fMouthRadius", mouthRadius * GAME_UNITS_TO_CM, "; cm - sphere radius for consume detection");
        ini.SetDoubleValue("Consumables", "fMouthVelocityThreshold", mouthVelocityThreshold, "; m/s - must be moving slower than this to consume");
        ini.SetDoubleValue("Consumables", "fMouthHapticStrength", mouthDropHapticStrength, "; Haptic strength when consuming at mouth (0.0-1.0)");
        ini.SetBoolValue("Consumables", "bBlockConsumptionInPA", blockConsumptionInPA, "; Block manual consumption/chem use while in Power Armor");
        ini.SetBoolValue("Consumables", "bConsumableToHand", consumableToHand, "; Redirect Pipboy consume to drop-to-hand");
        ini.SetBoolValue("Consumables", "bHolotapeToHand", holotapeToHand, "; Redirect Pipboy holotape play to drop-to-hand");

        // Hand injection zone - convert back to cm for INI
        ini.SetBoolValue("Consumables", "bEnableHandInjection", enableHandInjection, "; Enable injection consumption on opposite hand");
        ini.SetDoubleValue("Consumables", "fHandInjectionOffsetX", handInjectionOffsetX * GAME_UNITS_TO_CM, "; cm - left/right offset from wand");
        ini.SetDoubleValue("Consumables", "fHandInjectionOffsetY", handInjectionOffsetY * GAME_UNITS_TO_CM, "; cm - forward from wand");
        ini.SetDoubleValue("Consumables", "fHandInjectionOffsetZ", handInjectionOffsetZ * GAME_UNITS_TO_CM, "; cm - up/down from wand");
        ini.SetDoubleValue("Consumables", "fHandInjectionRadius", handInjectionRadius * GAME_UNITS_TO_CM, "; cm - sphere radius for injection detection");

        // Armor equip zones
        ini.SetLongValue("Equipping", "iArmorEquipZone", armorEquipZone, "; 0=Disabled, 1=Enabled (drop armor on body to equip)");
        ini.SetDoubleValue("Equipping", "fHeadZoneRadius", headZoneRadius * GAME_UNITS_TO_CM, "; cm - head zone radius for glasses/hats/helmets");
        ini.SetDoubleValue("Equipping", "fChestZoneRadius", chestZoneRadius * GAME_UNITS_TO_CM, "; cm - chest zone radius for shirts/armor");
        
        // Hand collision
        ini.SetBoolValue("ObjectPickup", "bEnableHandCollision", enableHandCollision, "; Enable hand collision with world");
        ini.SetBoolValue("ObjectPickup", "bUsePhysicsHandBodies", usePhysicsHandBodies, "; Use actual physics bodies for hands (experimental - may crash)");
        ini.SetDoubleValue("ObjectPickup", "fHandCollisionRadius", handCollisionRadius, "; Game units - radius of hand collision sphere");
        ini.SetDoubleValue("ObjectPickup", "fHandPushVelocityThreshold", handPushVelocityThreshold, "; Min hand speed to push objects (lower = more sensitive)");
        ini.SetDoubleValue("ObjectPickup", "fHandPushForceMultiplier", handPushForceMultiplier, "; Push force multiplier (higher = stronger)");

        // Throw
        ini.SetDoubleValue("Throw", "fThrowVelocityThreshold", throwVelocityThreshold);
        ini.SetDoubleValue("ObjectPickup", "fThrowVelocityBoostFactor", throwVelocityBoostFactor);
        ini.SetDoubleValue("Throw", "fTangentialVelocityLimit", tangentialVelocityLimit);

        // Haptics
        ini.SetDoubleValue("Haptics", "fGrabBaseHapticStrength", grabBaseHapticStrength);
        ini.SetDoubleValue("Haptics", "fGrabProportionalHapticStrength", grabProportionalHapticStrength);
        ini.SetDoubleValue("Haptics", "fCollisionBaseHapticStrength", collisionBaseHapticStrength);

        // Timing
        ini.SetDoubleValue("Timing", "fSelectionLeewayTime", selectionLeewayTime);
        ini.SetDoubleValue("Timing", "fTriggerPressedLeewayTime", triggerPressedLeewayTime);
        ini.SetDoubleValue("Timing", "fPullApplyVelocityTime", pullApplyVelocityTime);

        // Highlighting — hardcoded off (causes crashes)

        // Cooking
        ini.SetBoolValue("Cooking", "bEnableCooking", enableCooking, "; Enable cooking by holding raw food near fire/cooking station");
        ini.SetDoubleValue("Cooking", "fCookTime", cookTime, "; Seconds near fire to cook");
        ini.SetDoubleValue("Cooking", "fCookDetectionRadius", cookDetectionRadius, "; Game units (~1m) from cooking surface");
        ini.SetBoolValue("Cooking", "bCookingStationOnly", cookingStationOnly, "; Only cook at cooking stations (not near heat sources)");

        // Smart Grab
        ini.SetBoolValue("SmartGrab", "bEnableSmartGrab", enableSmartGrab, "; Enable context-aware item retrieval from inventory (grip behind head)");
        ini.SetDoubleValue("SmartGrab", "fHealthThreshold", smartGrabHealthThreshold, "; Pull stimpak when health below this fraction (0.5 = 50%)");
        ini.SetDoubleValue("SmartGrab", "fRadsThreshold", smartGrabRadsThreshold, "; Pull RadAway when rads above this fraction (0.25 = 25%)");
        ini.SetBoolValue("SmartGrab", "bIncludeHealth", smartGrabIncludeHealth, "; Include stimpaks, healing items");
        ini.SetBoolValue("SmartGrab", "bIncludeFood", smartGrabIncludeFood, "; Include food and drinks");
        ini.SetBoolValue("SmartGrab", "bIncludeCombatChems", smartGrabIncludeCombatChems, "; Include Psycho, Jet, Buffout, etc.");
        ini.SetBoolValue("SmartGrab", "bIncludeAntibiotics", smartGrabIncludeAntibiotics, "; Include antibiotics/disease cures");
        ini.SetBoolValue("SmartGrab", "bIncludeCarryWeight", smartGrabIncludeCarryWeight, "; Include carry weight boost items (Buffout, Grilled Radstag)");
        ini.SetBoolValue("SmartGrab", "bIncludeHeavyJunk", smartGrabIncludeHeavyJunk, "; Include heavy junk for companion drops");
        ini.SetDoubleValue("SmartGrab", "fAmmoThreshold", smartGrabAmmoThreshold, "; Pull ammo when magazine below this (0.3 = 30%)");

        // Interactive Activators
        ini.SetBoolValue("Activators", "bEnableInteractiveActivators", enableInteractiveActivators, "; Enable touch-based button/switch activation");
        ini.SetDoubleValue("Activators", "fActivatorPointingRadius", activatorPointingRadius, "; Game units - distance to start pointing pose (~25cm)");
        ini.SetDoubleValue("Activators", "fActivatorActivationRadius", activatorActivationRadius, "; Game units - distance to trigger activation (~8cm)");
        ini.SetDoubleValue("Activators", "fActivatorCooldownMs", activatorCooldownMs, "; Milliseconds between activations");
        ini.SetBoolValue("Activators", "bActivatorDebugLogging", activatorDebugLogging, "; Log activator proximity checks");
        ini.SetBoolValue("Activators", "bActivatorUseWhitelist", activatorUseWhitelist, "; Only track activators in optional HeisenbergActivators.ini");
        ini.SetBoolValue("Activators", "bEnableActivatorDiscoveryMode", enableActivatorDiscoveryMode, "; Hold right thumbstick near activator to set activation point");

        // Intro holotape persistence
        ini.SetBoolValue("Pipboy", "bIntroHolotapeGiven", introHolotapeGiven, "; Intro holotape ceremony done (set automatically)");
        ini.SetBoolValue("Pipboy", "bIntroHolotapePlayed", introHolotapePlayed, "; Intro audio fully played (set automatically)");
        ini.SetBoolValue("Pipboy", "bEnableTerminalOnWorldScreen", enableTerminalOnWorldScreen, "; Render terminal UI onto in-world terminal screen meshes instead of Pipboy wrist (experimental)");

        // Water Interaction
        ini.SetBoolValue("Water", "bEnableWaterInteraction", enableWaterInteraction, "; Enable water ripple/splash effects for VR hands");
        ini.SetDoubleValue("Water", "fSplashScale", waterSplashScale, "; Global splash/ripple scale multiplier (1.0 = default)");
        ini.SetBoolValue("Water", "bEnableWakeRipples", enableWakeRipples, "; Continuous ripples while hand moves underwater");
        ini.SetDoubleValue("Water", "fWakeRippleAmount", wakeRippleAmount, "; Base wake ripple radius (0.009 = Skyrim default)");
        ini.SetLongValue("Water", "iWakeRippleIntervalMs", wakeRippleIntervalMs, "; Min ms between wake ripples (0 = every frame, Skyrim default)");
        ini.SetDoubleValue("Water", "fWakeMinDistance", wakeMinDistance, "; Min distance between wake ripples in game units (prevents stretched shapes from fast swipes)");
        ini.SetDoubleValue("Water", "fWakeMaxMultiplier", wakeMaxMultiplier, "; Max speed scale applied to wake ripple radius (caps ring size at high hand speeds)");
        ini.SetBoolValue("Water", "bEnableWaterSplashEffects", enableWaterSplashEffects, "; Enable splash VFX/SFX for fast water entry/exit");
        ini.SetDoubleValue("Water", "fSplashEffectEntryMagnitude", splashEffectEntryMagnitude, "; Splash VFX magnitude for hand entry (300 = default)");
        ini.SetDoubleValue("Water", "fSplashEffectExitMagnitude", splashEffectExitMagnitude, "; Splash VFX magnitude for hand exit (150 = default)");
        ini.SetBoolValue("Water", "bEnableWaterSplashNif", enableWaterSplashNif, "; Enable waterSplash.NIF particle (false = sound only)");
        ini.SetDoubleValue("Water", "fWaterSplashNifScale", waterSplashNifScale, "; Scale multiplier for waterSplash.NIF particle (1.0 = engine default)");

        // Pickpocket / Stealing
        ini.SetBoolValue("Pickpocket", "bEnablePickpocket", enablePickpocket, "; Enable physical pickpocketing (touch NPC while sneaking + grip)");

        // Debug
        ini.SetBoolValue("Debug", "bDebugDrawControllers", debugDrawControllers);
        ini.SetBoolValue("Debug", "bDebugLogging", debugLogging, "; Enable verbose debug logging (PERFORMANCE IMPACT!)");
        ini.SetLongValue("Debug", "iLogLevel", logLevel, "; 0=trace, 1=debug, 2=info, 3=warn, 4=error");

        SI_Error rc = ini.SaveFile(kConfigPath);
        if (rc < 0) {
            spdlog::error("Failed to save config file");
        }
    }

    void Config::ReloadIfMCMChanged()
    {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point lastCheck = Clock::now();
        static FILETIME lastModTime{};
        static bool firstRun = true;

        // Only check every 2 seconds to avoid filesystem overhead
        auto now = Clock::now();
        if (!firstRun && std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck).count() < 2) {
            return;
        }
        lastCheck = now;

        WIN32_FILE_ATTRIBUTE_DATA fileInfo{};
        if (!GetFileAttributesExA(kMCMSettingsPath, GetFileExInfoStandard, &fileInfo)) {
            return;  // File doesn't exist or can't be accessed
        }

        if (firstRun) {
            lastModTime = fileInfo.ftLastWriteTime;
            firstRun = false;
            return;
        }

        if (CompareFileTime(&fileInfo.ftLastWriteTime, &lastModTime) != 0) {
            lastModTime = fileInfo.ftLastWriteTime;
            spdlog::info("[Config] MCM settings file changed - reloading config");
            Load();
            // Re-apply terminal patches based on updated config
            heisenberg::Hooks::ApplyTerminalPatches(forceTerminalOnWrist);
        }
    }
}
