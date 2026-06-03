#include "Config.h"
#include "Hooks.h"

#include <SimpleIni.h>
#include <chrono>
#include <fstream>
#include <vector>
#include <string>
#include <utility>

namespace heisenberg
{
    static const char* kConfigPath = "Data/F4SE/Plugins/Heisenberg_F4VR.ini";
    static const char* kMCMSettingsPath = "Data/MCM/Settings/Heisenberg.ini";
    static const char* kMCMConfigPath = "Data/MCM/Config/Heisenberg/config.json";
    
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
; Objects beyond this distance cannot be grabbed via telekinesis (500cm = 5m default)
fMaxGrabDistance = 500.0

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

; Automatic hand placement: uses object geometry to place grabbed items HIGGS-style
; When false, the existing saved item offset system is used
bEnableAutomaticHandPlacement = true

; Automatic finger curls: uses object geometry to calculate grab pose
; When false, saved finger curl offsets are used instead
bEnableAutomaticFingerCurls = true

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
bConsumableToHand = true

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
bEnableHarvestToHand = true

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

[HeldBody]
; Enable HIGGS-style HeldBody grabbing (object stays DYNAMIC)
; Note: iGrabMode=0 still forces the original keyframed backend for testing.
bUseHeldBodyGrab = true

; HeldBody runtime mode:
; 0 = dynamic spring held mode
; 1 = custom 6-DOF constraint
; 2 = native ragdoll constraint
iHeldBodyMode = 0

; Legacy fallback if iHeldBodyMode is absent
bUseNativeRagdollConstraint = false

; Use Bethesda CreateInstance path for HeldBody hand bodies
bUseSimpleHandBodyCreation = true

[Debug]
; Log level: 0=trace, 1=debug, 2=info, 3=warn, 4=error
; Default: 2 (info)
iLogLevel = 2
)";

    void Config::Load()
    {
        spdlog::info("Loading config...");

        // Make sure the MCM settings file lists every control at this build's default BEFORE we
        // merge it below, so the menu display and the merged value agree (and the menu's OFF
        // actually wins over a stale external-INI ON). Runs once per session.
        SeedMCMDefaultsIfMissing();

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
        passGripToActivatorSecondary = ini.GetBoolValue("ObjectPickup", "bPassGripToActivatorSecondary", passGripToActivatorSecondary);
        useGrenadeReadyHook = ini.GetBoolValue("ObjectPickup", "bUseGrenadeReadyHook", useGrenadeReadyHook);
        fingerPoseMode = static_cast<int>(ini.GetLongValue("ObjectPickup", "iFingerPoseMode", fingerPoseMode));
        fingerAnimCloseSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerAnimCloseSpeed", fingerAnimCloseSpeed));
        fingerAnimOpenSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerAnimOpenSpeed", fingerAnimOpenSpeed));
        grabMode = static_cast<int>(ini.GetLongValue("ObjectPickup", "iGrabMode", grabMode));
        heldObjectCollidable = ini.GetBoolValue("ObjectPickup", "bHeldObjectCollidable", heldObjectCollidable);
        heldObjectWallClamp = ini.GetBoolValue("ObjectPickup", "bHeldObjectWallClamp", heldObjectWallClamp);
        blockActivateOnGrabSelection = ini.GetBoolValue("ObjectPickup", "bBlockActivateOnGrabSelection", blockActivateOnGrabSelection);
        rockDynamicHandoff = ini.GetBoolValue("ObjectPickup", "bRockDynamicHandoff", rockDynamicHandoff);
        useXForLeftGrab  = ini.GetBoolValue("ObjectPickup", "bUseXForLeftGrab",  useXForLeftGrab);
        useAForRightGrab = ini.GetBoolValue("ObjectPickup", "bUseAForRightGrab", useAForRightGrab);
        grabStartSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabStartSpeed", grabStartSpeed));
        grabStartAngularSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabStartAngularSpeed", grabStartAngularSpeed));
        
        // Pickup distance settings
        maxGrabDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fMaxGrabDistance", maxGrabDistance));
        enableNaturalGrab = ini.GetBoolValue("ObjectPickup", "bEnableNaturalGrab", enableNaturalGrab);
        naturalGrabDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fNaturalGrabDistance", naturalGrabDistance));
        naturalGrabDistanceNoMatch = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fNaturalGrabDistanceNoMatch", naturalGrabDistanceNoMatch));
        
        // Palm snap settings
        enablePalmSnap = ini.GetBoolValue("ObjectPickup", "bEnablePalmSnap", enablePalmSnap);
        enableAutomaticHandPlacement = ini.GetBoolValue("ObjectPickup", "bEnableAutomaticHandPlacement", enableAutomaticHandPlacement);
        enableAutomaticFingerCurls = ini.GetBoolValue("ObjectPickup", "bEnableAutomaticFingerCurls", enableAutomaticFingerCurls);
        saveNaturalGrabAsOffset = ini.GetBoolValue("ObjectPickup", "bSaveNaturalGrabAsOffset", saveNaturalGrabAsOffset);
        enablePalmRayCastPlacement = ini.GetBoolValue("ObjectPickup", "bEnablePalmRayCastPlacement", enablePalmRayCastPlacement);
        enableAxialPlacement = ini.GetBoolValue("ObjectPickup", "bEnableAxialPlacement", enableAxialPlacement);
        axialPlacementClearance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fAxialPlacementClearance", axialPlacementClearance));
        grabMaxTriangleDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabMaxTriangleDistance", grabMaxTriangleDistance));
        useRockFingerPose = ini.GetBoolValue("ObjectPickup", "bUseRockFingerPose", useRockFingerPose);
        rockFingerRejectBackside = ini.GetBoolValue("ObjectPickup", "bRockFingerRejectBackside", rockFingerRejectBackside);
        rockFingerSurfaceTolerance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fRockFingerSurfaceTolerance", rockFingerSurfaceTolerance));
        grabDirectionalWeight = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabDirectionalWeight", grabDirectionalWeight));
        grabLateralWeight = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabLateralWeight", grabLateralWeight));
        palmDepthOffset = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmDepthOffset", palmDepthOffset));
        enableAutomaticFingerCurlsPerFrame = ini.GetBoolValue("ObjectPickup", "bEnableAutomaticFingerCurlsPerFrame", enableAutomaticFingerCurlsPerFrame);
        fingerCurlPerFrameInterval = static_cast<int>(ini.GetLongValue("ObjectPickup", "iFingerCurlPerFrameInterval", fingerCurlPerFrameInterval));
        fingerCurlSmoothingAlpha = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerCurlSmoothingAlpha", fingerCurlSmoothingAlpha));
        palmOffsetX = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmOffsetX", palmOffsetX));
        palmOffsetY = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmOffsetY", palmOffsetY));
        palmOffsetZ = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmOffsetZ", palmOffsetZ));
        palmVectorX = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmVectorX", palmVectorX));
        palmVectorY = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmVectorY", palmVectorY));
        palmVectorZ = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmVectorZ", palmVectorZ));
        palmPositionX = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmPositionX", palmPositionX));
        palmPositionY = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmPositionY", palmPositionY));
        palmPositionZ = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fPalmPositionZ", palmPositionZ));
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
        lootToHandTakeAllThreshold = static_cast<int>(ini.GetLongValue("DropToHand", "iLootToHandTakeAllThreshold", lootToHandTakeAllThreshold));
        dropToHandPreferredHand = static_cast<int>(ini.GetLongValue("DropToHand", "iDropToHandPreferredHand", dropToHandPreferredHand));
        favoritesToHand = ini.GetBoolValue("DropToHand", "bFavoritesToHand", favoritesToHand);
        enableStealToHand = ini.GetBoolValue("DropToHand", "bEnableStealToHand", enableStealToHand);
        enableHarvestToHand = ini.GetBoolValue("DropToHand", "bEnableHarvestToHand", enableHarvestToHand);

        // Throwables: no longer user-configurable — locked to vanilla game behaviour.
        // The [Throwables] INI section and its MCM page were removed; the fields keep
        // their vanilla C++ defaults (grenade handling on for grip-vs-grab gating,
        // zone disabled, remap off, hold 0.3s). Do NOT read them from the INI.

        // Impact effects (GrabAndThrow port)
        impactDamageEnabled = ini.GetBoolValue("Impact", "bImpactDamageEnabled", impactDamageEnabled);
        impactDamageMult = static_cast<float>(ini.GetDoubleValue("Impact", "fImpactDamageMult", impactDamageMult));
        impactDestroyEnabled = ini.GetBoolValue("Impact", "bImpactDestroyEnabled", impactDestroyEnabled);
        impactMinDestroySpeed = static_cast<float>(ini.GetDoubleValue("Impact", "fImpactMinDestroySpeed", impactMinDestroySpeed));
        impactDetectionEvent = ini.GetBoolValue("Impact", "bImpactDetectionEvent", impactDetectionEvent);
        impactHitEvent = ini.GetBoolValue("Impact", "bImpactHitEvent", impactHitEvent);
        impactMinDetectionSpeed = static_cast<float>(ini.GetDoubleValue("Impact", "fImpactMinDetectionSpeed", impactMinDetectionSpeed));
        impactActorCooldown = static_cast<float>(ini.GetDoubleValue("Impact", "fImpactActorCooldown", impactActorCooldown));
        impactMassScaledSound = ini.GetBoolValue("Impact", "bImpactMassScaledSound", impactMassScaledSound);
        impactMassGate = ini.GetBoolValue("Impact", "bImpactMassGate", impactMassGate);

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
        
        // ROCK integration
        useRockPhysics = static_cast<int>(ini.GetLongValue("ROCK", "iUseRockPhysics", useRockPhysics));

        // Hand collision
        enableHandCollision = ini.GetBoolValue("ObjectPickup", "bEnableHandCollision", enableHandCollision);
        usePhysicsHandBodies = ini.GetBoolValue("ObjectPickup", "bUsePhysicsHandBodies", usePhysicsHandBodies);
        useBethesdaPhysicsBody = ini.GetBoolValue("ObjectPickup", "bUseBethesdaPhysicsBody", useBethesdaPhysicsBody);

        // ROCK integration toggles — section [RockIntegration]
        rockCollisionSuppressionRegistry = ini.GetBoolValue("RockIntegration", "bCollisionSuppressionRegistry", rockCollisionSuppressionRegistry);
        rockHandBoneColliderSet          = ini.GetBoolValue("RockIntegration", "bHandBoneColliderSet",          rockHandBoneColliderSet);
        rockBodyBoneColliderSet          = ini.GetBoolValue("RockIntegration", "bBodyBoneColliderSet",          rockBodyBoneColliderSet);
        rockWeaponCollision              = ini.GetBoolValue("RockIntegration", "bWeaponCollision",              rockWeaponCollision);
        rockTwoHandedGrip                = ini.GetBoolValue("RockIntegration", "bTwoHandedGrip",                rockTwoHandedGrip);
        spdlog::info("[Config] RockIntegration: CSR={} HBCS={} BBCS={} WC={} THG={}",
                     rockCollisionSuppressionRegistry, rockHandBoneColliderSet, rockBodyBoneColliderSet,
                     rockWeaponCollision, rockTwoHandedGrip);
        handCollisionRadius = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandCollisionRadius", handCollisionRadius));
        handContactSlop = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandContactSlop", handContactSlop));
        handPushVelocityThreshold = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushVelocityThreshold", handPushVelocityThreshold));
        handPushForceMultiplier = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushForceMultiplier", handPushForceMultiplier));
        enableHandCollisionHaptics = ini.GetBoolValue("ObjectPickup", "bEnableHandCollisionHaptics", enableHandCollisionHaptics);
        handCollisionHapticScale = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandCollisionHapticScale", handCollisionHapticScale));
        enableFingerSegmentColliders = ini.GetBoolValue("ObjectPickup", "bEnableFingerSegmentColliders", enableFingerSegmentColliders);
        fingerSegmentHalfExtentX = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerSegmentHalfExtentX", fingerSegmentHalfExtentX));
        fingerSegmentHalfExtentY = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerSegmentHalfExtentY", fingerSegmentHalfExtentY));
        fingerSegmentHalfExtentZ = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fFingerSegmentHalfExtentZ", fingerSegmentHalfExtentZ));
        useCollisionOverlapForGrabCandidates = ini.GetBoolValue("ObjectPickup", "bUseCollisionOverlapForGrabCandidates", useCollisionOverlapForGrabCandidates);

        // Hand wall-pushback
        handWallPushbackMode    = static_cast<int>(ini.GetLongValue("ObjectPickup", "iHandWallPushbackMode", handWallPushbackMode));
        handPushbackProbeRadius = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushbackProbeRadius", handPushbackProbeRadius));
        handPushbackMaxPush     = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushbackMaxPush", handPushbackMaxPush));
        handPushbackSmoothing   = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushbackSmoothing", handPushbackSmoothing));
        handPushbackHardStop    = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushbackHardStop", handPushbackHardStop));
        handPushbackScale       = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushbackScale", handPushbackScale));
        spdlog::info("[Config] HandWallPushback mode={} (0=off,1=linear,2=rock) probeR={:.1f} maxPush={:.1f}",
                     handWallPushbackMode, handPushbackProbeRadius, handPushbackMaxPush);
        enableGrabApproachSubstates = ini.GetBoolValue("HeldBody", "bEnableGrabApproachSubstates", enableGrabApproachSubstates);
        grabApproachRampSeconds = static_cast<float>(ini.GetDoubleValue("HeldBody", "fGrabApproachRampSeconds", grabApproachRampSeconds));

        // Hand collision enabled — pair filter + proxy listener handle player capsule

        // HeldBody constraint grab
        if (ini.GetValue("HeldBody", "bUseHeldBodyGrab", nullptr)) {
            useHeldBodyGrab = ini.GetBoolValue("HeldBody", "bUseHeldBodyGrab", useHeldBodyGrab);
        } else {
            useHeldBodyGrab = ini.GetBoolValue("ObjectPickup", "bUseHeldBodyGrab", useHeldBodyGrab);
        }
        if (ini.GetValue("HeldBody", "iHeldBodyMode", nullptr)) {
            heldBodyMode = static_cast<int>(ini.GetLongValue("HeldBody", "iHeldBodyMode", heldBodyMode));
        } else if (ini.GetValue("HeldBody", "bUseNativeRagdollConstraint", nullptr)) {
            heldBodyMode = ini.GetBoolValue("HeldBody", "bUseNativeRagdollConstraint", useNativeRagdollConstraint)
                ? static_cast<int>(HeldBodyMode::NativeConstraint)
                : static_cast<int>(HeldBodyMode::Custom6DOF);
        } else if (ini.GetValue("HeldBody", "bUse6DOFGrabConstraint", nullptr)) {
            heldBodyMode = ini.GetBoolValue("HeldBody", "bUse6DOFGrabConstraint", false)
                ? static_cast<int>(HeldBodyMode::Custom6DOF)
                : static_cast<int>(HeldBodyMode::NativeConstraint);
        }
        heldBodyMode = static_cast<int>(GetHeldBodyMode());
        useNativeRagdollConstraint = UseHeldBodyNativeConstraint();
        if (ini.GetValue("HeldBody", "bUseSimpleHandBodyCreation", nullptr)) {
            useSimpleHandBodyCreation = ini.GetBoolValue("HeldBody", "bUseSimpleHandBodyCreation", useSimpleHandBodyCreation);
        } else {
            useSimpleHandBodyCreation = ini.GetBoolValue("ObjectPickup", "bUseSimpleHandBodyCreation", useSimpleHandBodyCreation);
        }
        heldBodyTauLerpTime = static_cast<float>(ini.GetDoubleValue("HeldBody", "fTauLerpTime", heldBodyTauLerpTime));
        grabConstraintAngularTauBodyStart = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularTauBodyStart", grabConstraintAngularTauBodyStart));
        grabConstraintAngularTauBody = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularTauBody", grabConstraintAngularTauBody));
        grabConstraintLinearTauBodyStart = static_cast<float>(ini.GetDoubleValue("HeldBody", "fLinearTauBodyStart", grabConstraintLinearTauBodyStart));
        grabConstraintLinearTauBody = static_cast<float>(ini.GetDoubleValue("HeldBody", "fLinearTauBody", grabConstraintLinearTauBody));
        grabConstraintAngularTau = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularTau", grabConstraintAngularTau));
        grabConstraintAngularDamping = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularDamping", grabConstraintAngularDamping));
        grabConstraintAngularMaxForce = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularMaxForce", grabConstraintAngularMaxForce));
        grabConstraintLinearTau = static_cast<float>(ini.GetDoubleValue("HeldBody", "fLinearTau", grabConstraintLinearTau));
        grabConstraintLinearDamping = static_cast<float>(ini.GetDoubleValue("HeldBody", "fLinearDamping", grabConstraintLinearDamping));
        grabConstraintLinearMaxForce = static_cast<float>(ini.GetDoubleValue("HeldBody", "fLinearMaxForce", grabConstraintLinearMaxForce));
        grabConstraintAngularToLinearForceRatio = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularToLinearForceRatio", grabConstraintAngularToLinearForceRatio));
        grabConstraintEnableSoftLimits = ini.GetBoolValue("HeldBody", "bEnableSoftLimits", grabConstraintEnableSoftLimits);
        grabConstraintLinearMaxStretch = static_cast<float>(ini.GetDoubleValue("HeldBody", "fLinearMaxStretch", grabConstraintLinearMaxStretch));
        grabConstraintAngularMaxAngleDeg = static_cast<float>(ini.GetDoubleValue("HeldBody", "fAngularMaxAngleDeg", grabConstraintAngularMaxAngleDeg));
        grabConstraintForceDynamicHeldBody = ini.GetBoolValue("HeldBody", "bForceDynamicHeldBody", grabConstraintForceDynamicHeldBody);

        if (UseHeldBodyManagedGrab() && grabMode == 0) {
            spdlog::info("[Config] iGrabMode=0 keeps the original keyframed backend even while bUseHeldBodyGrab=true");
        }
        if (UseHeldBodyManagedGrab() && (grabMode == 1 || grabMode == 2)) {
            spdlog::info("[Config] iGrabMode={} is a legacy constraint mode; HeldBody will override it while bUseHeldBodyGrab=true", grabMode);
        }
        if (UseHeldBodyManagedGrab()) {
            spdlog::info("[Config] HeldBody mode: {}", GetHeldBodyModeName());
            if (UseHeldBodySpringMode() && (grabMode == 1 || grabMode == 2)) {
                spdlog::info("[Config] HeldBody spring mode selected; iGrabMode={} will be overridden by the HeldBody dynamic spring backend", grabMode);
            }
        }
        if (enableHandCollision && !usePhysicsHandBodies) {
            spdlog::warn("[Config] Hand collision is enabled but bUsePhysicsHandBodies=false; only proximity fallback collision is active");
        }

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
        clampFloat(handContactSlop, 0.0f, 20.0f, "fHandContactSlop");
        clampFloat(throwableZoneRadius, 0.0f, 100.0f, "fThrowableZoneRadius");
        clampFloat(throwableHoldDuration, 0.05f, 2.0f, "fThrowableHoldDuration");
        clampFloat(impactDamageMult, 0.0f, 10.0f, "fImpactDamageMult");
        clampFloat(impactMinDestroySpeed, 0.0f, 10000.0f, "fImpactMinDestroySpeed");
        clampFloat(impactMinDetectionSpeed, 0.0f, 10000.0f, "fImpactMinDetectionSpeed");
        clampFloat(impactActorCooldown, 0.0f, 10.0f, "fImpactActorCooldown");
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
        clampInt(grabMode, 0, 5, "iGrabMode");
        clampInt(weaponEquipMode, 0, 2, "iWeaponEquipMode");
        clampInt(throwableActivationZone, 0, 1, "iThrowableActivationZone");
        clampInt(consumableActivationZone, 0, 1, "iConsumableActivationZone");
        clampInt(lootToHandMode, 0, 2, "iLootToHandMode");
        clampInt(lootToHandTakeAllThreshold, 0, 99, "iLootToHandTakeAllThreshold");
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
        BuildIni(&ini);

        SI_Error rc = ini.SaveFile(kConfigPath);
        if (rc < 0) {
            spdlog::error("Failed to save config file");
        }
    }

    void Config::BuildIni(void* iniPtr) const
    {
        CSimpleIniA& ini = *static_cast<CSimpleIniA*>(iniPtr);

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
        ini.SetBoolValue("ObjectPickup", "bPassGripToActivatorSecondary", passGripToActivatorSecondary, "; When grip-weapon-draw is off, still pass primary grip to the game when aiming at an activator/furniture (mod secondary actions)");
        ini.SetBoolValue("ObjectPickup", "bUseGrenadeReadyHook", useGrenadeReadyHook, "; Control grenade readying at the game level (hook MeleeThrowHandler) instead of stripping grip input (fixes VirtualHolsters/reload/secondary)");
        ini.SetBoolValue("ObjectPickup", "bShowHolsterMessages", showHolsterMessages, "; Show HUD message when holstering weapons");
        ini.SetBoolValue("ObjectPickup", "bShowUnequipMessages", showUnequipMessages, "; Show HUD message when unequipping weapons");
        ini.SetLongValue("ObjectPickup", "iGrabMode", grabMode, "; 0=Keyframe (always original backend), 1/2=Legacy constraint fallback, 3=Post-physics/HeldBody wrapper (recommended), 4=MouseSpring, 5=DynamicRock (self-contained ROCK-style dynamic hold: places at offset then holds dynamically so it collides with walls/NPCs; no ROCK.dll)");
        ini.SetBoolValue("ObjectPickup", "bHeldObjectCollidable", heldObjectCollidable, "; Keyframed hold keeps object collidable so it can hit other objects (iGrabMode=0 only)");
        ini.SetBoolValue("ObjectPickup", "bHeldObjectWallClamp", heldObjectWallClamp, "; Keyframed hold: sweep-cast the held object so it STOPS at walls and NPCs instead of clipping through (iGrabMode=0)");
        ini.SetBoolValue("ObjectPickup", "bBlockActivateOnGrabSelection", blockActivateOnGrabSelection, "; Block A/activate looting an item just because you're aiming at it. OFF by default (ON broke A-looting + mod secondary actions). Only enable for a Grip>A SteamVR binding.");
        ini.SetBoolValue("ObjectPickup", "bUseXForLeftGrab",  useXForLeftGrab,  "; Use X button instead of Grip for left hand grabbing");
        ini.SetBoolValue("ObjectPickup", "bUseAForRightGrab", useAForRightGrab, "; Use A button instead of Grip for right hand grabbing");
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
        ini.SetBoolValue("ObjectPickup", "bEnablePalmRayCastPlacement", enablePalmRayCastPlacement, "; Prefer palm-ray hit over closest-point for grip placement (handles/levers)");
        ini.SetBoolValue("ObjectPickup", "bEnableAxialPlacement", enableAxialPlacement, "; Place object near-face at palm with small clearance, extending forward along palmDir");
        ini.SetDoubleValue("ObjectPickup", "fAxialPlacementClearance", axialPlacementClearance, "; cm from palm to object near face when axial placement is enabled");
        ini.SetDoubleValue("ObjectPickup", "fGrabMaxTriangleDistance", grabMaxTriangleDistance, "; Squared distance (game units) from closest-mesh-point to consider a triangle nearby for finger intersection (HIGGS default 100)");
        ini.SetBoolValue("ObjectPickup", "bUseRockFingerPose", useRockFingerPose, "; Use ROCK's ported curl-disk finger solver instead of the HIGGS curve tables (A/B toggle, default false)");
        ini.SetBoolValue("ObjectPickup", "bRockFingerRejectBackside", rockFingerRejectBackside, "; ROCK solver: reject finger hits behind the seated mesh surface plane (default false)");
        ini.SetDoubleValue("ObjectPickup", "fRockFingerSurfaceTolerance", rockFingerSurfaceTolerance, "; ROCK solver back-face plane tolerance in game units (default 1.5)");
        ini.SetDoubleValue("ObjectPickup", "fGrabDirectionalWeight", grabDirectionalWeight, "; Weight on directional distance squared when scoring closest-point-on-geometry (HIGGS default 0.4)");
        ini.SetDoubleValue("ObjectPickup", "fGrabLateralWeight", grabLateralWeight, "; Weight on lateral distance squared when scoring closest-point-on-geometry (HIGGS default 0.6)");
        ini.SetDoubleValue("ObjectPickup", "fPalmDepthOffset", palmDepthOffset, "; cm to shift palm anchor from knuckle centroid into palm cup (palmar side)");
        ini.SetBoolValue("ObjectPickup", "bEnableAutomaticFingerCurlsPerFrame", enableAutomaticFingerCurlsPerFrame, "; Recompute geometry finger curl every N frames during hold");
        ini.SetLongValue("ObjectPickup", "iFingerCurlPerFrameInterval", fingerCurlPerFrameInterval, "; Frames between per-frame finger curl re-evaluations (1=every frame)");
        ini.SetDoubleValue("ObjectPickup", "fFingerCurlSmoothingAlpha", fingerCurlSmoothingAlpha, "; Lerp factor 0..1 for per-frame finger curl smoothing (lower=snappier)");
        ini.SetDoubleValue("ObjectPickup", "fPalmOffsetX", palmOffsetX, "; Game units - left/right offset (0 = centered)");
        ini.SetDoubleValue("ObjectPickup", "fPalmOffsetY", palmOffsetY, "; Game units - forward offset (toward fingers, ~5 = 5cm)");
        ini.SetDoubleValue("ObjectPickup", "fPalmOffsetZ", palmOffsetZ, "; Game units - up/down offset (~3.5 = 5cm up from wand)");
        ini.SetDoubleValue("ObjectPickup", "fPalmVectorX", palmVectorX, "; Hand-local palm normal X (mirrored for left hand)");
        ini.SetDoubleValue("ObjectPickup", "fPalmVectorY", palmVectorY, "; Hand-local palm normal Y (points out of palm toward grabbed object)");
        ini.SetDoubleValue("ObjectPickup", "fPalmVectorZ", palmVectorZ, "; Hand-local palm normal Z");
        ini.SetDoubleValue("ObjectPickup", "fPalmPositionX", palmPositionX, "; Hand-local palm center X (cm, mirrored for left hand)");
        ini.SetDoubleValue("ObjectPickup", "fPalmPositionY", palmPositionY, "; Hand-local palm center Y (cm)");
        ini.SetDoubleValue("ObjectPickup", "fPalmPositionZ", palmPositionZ, "; Hand-local palm center Z (cm)");
        ini.SetDoubleValue("ObjectPickup", "fPAGrabOffsetX", paGrabOffsetX, "; Game units - extra X offset when in power armor (0 = no shift)");
        ini.SetDoubleValue("ObjectPickup", "fPAGrabOffsetY", paGrabOffsetY, "; Game units - extra Y offset in PA (forward, out of glove)");
        ini.SetDoubleValue("ObjectPickup", "fPAGrabOffsetZ", paGrabOffsetZ, "; Game units - extra Z offset in PA (up, above palm)");

        // Item Positioning Mode
        ini.SetBoolValue("ItemPositioning", "bEnableItemPositioning", enableItemPositioning, "; Enable item positioning mode (hold L3 to configure)");
        ini.SetLongValue("ItemPositioning", "iItemPositioningShortcut", itemPositioningShortcut, "; 0=Left Thumbstick Click, 1=Right Thumbstick Click, 2=Long Press A");
        
        // Drop to Hand
        ini.SetLongValue("DropToHand", "iDropToHandMode", dropToHandMode, "; 0=Off, 1=All Items, 2=Holotapes Only");
        ini.SetLongValue("DropToHand", "iLootToHandMode", lootToHandMode, "; 0=Off, 1=Hybrid (blocked->inventory), 2=Immersive (blocked->floor)");
        ini.SetLongValue("DropToHand", "iLootToHandTakeAllThreshold", lootToHandTakeAllThreshold, "; Take-All guard: this many+ items from one container in one burst stay in inventory instead of dropping to the floor (0=off, default 3)");
        ini.SetLongValue("DropToHand", "iDropToHandPreferredHand", dropToHandPreferredHand, "; 0=Left, 1=Right, 2=Whichever is free");
        ini.SetBoolValue("DropToHand", "bFavoritesToHand", favoritesToHand, "; Consumables from Favorites go to hand");
        ini.SetBoolValue("DropToHand", "bEnableStealToHand", enableStealToHand, "; Items stolen from NPCs go to hand");
        ini.SetBoolValue("DropToHand", "bEnableHarvestToHand", enableHarvestToHand, "; Harvested flora goes to hand");

        // Throwables: intentionally NOT written — locked to vanilla behaviour, no user tuning.
        // (The [Throwables] INI section and its MCM page were removed.)

        // Impact effects (GrabAndThrow port)
        ini.SetBoolValue("Impact", "bImpactDamageEnabled", impactDamageEnabled, "; Thrown objects damage actors via native ProcessHurtfulBody");
        ini.SetDoubleValue("Impact", "fImpactDamageMult", impactDamageMult, "; Multiplier on game-computed impact damage (1.0 = native)");
        ini.SetBoolValue("Impact", "bImpactDestroyEnabled", impactDestroyEnabled, "; Thrown objects damage destructibles (bottles, glass, etc.)");
        ini.SetDoubleValue("Impact", "fImpactMinDestroySpeed", impactMinDestroySpeed, "; Min impact speed (game units/sec) for destructible damage");
        ini.SetBoolValue("Impact", "bImpactDetectionEvent", impactDetectionEvent, "; Throws fire AI detection events (NPCs hear them)");
        ini.SetBoolValue("Impact", "bImpactHitEvent", impactHitEvent, "; Throws fire Papyrus OnHit/SendHitEvent on target");
        ini.SetDoubleValue("Impact", "fImpactMinDetectionSpeed", impactMinDetectionSpeed, "; Min impact speed for AI detection event (lower than destroy)");
        ini.SetDoubleValue("Impact", "fImpactActorCooldown", impactActorCooldown, "; Seconds between damage hits on the same actor");
        ini.SetBoolValue("Impact", "bImpactMassScaledSound", impactMassScaledSound, "; Bin detection sound level by thrown mass (silent/normal/loud/very-loud)");
        ini.SetBoolValue("Impact", "bImpactMassGate", impactMassGate, "; Skip damaging hit destructible if thrown weight < target weight");

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
        ini.SetBoolValue("ItemStorage", "bShowStorageMessages", showStorageMessages, "; Show HUD message when storing items");
        
        // Consumables (Mouth zone) - convert back to cm for INI
        ini.SetLongValue("Consumables", "iConsumableActivationZone", consumableActivationZone, "; 0=Disabled, 1=Mouth/Face");
        ini.SetDoubleValue("Consumables", "fMouthOffsetX", mouthOffsetX * GAME_UNITS_TO_CM, "; cm - left/right (0 = centered)");
        ini.SetDoubleValue("Consumables", "fMouthOffsetY", mouthOffsetY * GAME_UNITS_TO_CM, "; cm - forward from HMD (toward face)");
        ini.SetDoubleValue("Consumables", "fMouthOffsetZ", mouthOffsetZ * GAME_UNITS_TO_CM, "; cm - down from eye level (mouth area)");
        ini.SetDoubleValue("Consumables", "fMouthRadius", mouthRadius * GAME_UNITS_TO_CM, "; cm - sphere radius for consume detection");
        ini.SetDoubleValue("Consumables", "fMouthVelocityThreshold", mouthVelocityThreshold, "; m/s - must be moving slower than this to consume");
        ini.SetDoubleValue("Consumables", "fMouthHapticStrength", mouthDropHapticStrength, "; Haptic strength when consuming at mouth (0.0-1.0)");
        ini.SetBoolValue("Consumables", "bBlockConsumptionInPA", blockConsumptionInPA, "; Block manual consumption/chem use while in Power Armor");
        ini.SetBoolValue("Consumables", "bShowConsumeMessages", showConsumeMessages, "; Show HUD message when consuming items");
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
        
        // ROCK integration
        ini.SetLongValue("ROCK", "iUseRockPhysics", useRockPhysics, "; -1=auto (on if ROCK.dll present), 0=force off (built-in fallback), 1=force on");

        // Hand collision
        ini.SetBoolValue("ObjectPickup", "bEnableHandCollision", enableHandCollision, "; Enable hand collision with world");
        ini.SetBoolValue("ObjectPickup", "bUsePhysicsHandBodies", usePhysicsHandBodies, "; Use actual physics bodies for hands (required for full physical hand collision)");
        ini.SetDoubleValue("ObjectPickup", "fHandCollisionRadius", handCollisionRadius, "; Game units - radius of hand collision sphere (broadphase reach)");
        ini.SetDoubleValue("ObjectPickup", "fHandContactSlop", handContactSlop, "; Game units - bone-to-mesh distance counted as TRUE contact (push trigger); ~finger flesh radius, smaller = must touch closer");
        ini.SetDoubleValue("ObjectPickup", "fHandPushVelocityThreshold", handPushVelocityThreshold, "; Min hand speed to push objects (lower = more sensitive)");
        ini.SetDoubleValue("ObjectPickup", "fHandPushForceMultiplier", handPushForceMultiplier, "; Push force multiplier (higher = stronger)");
        ini.SetBoolValue("ObjectPickup", "bEnableHandCollisionHaptics", enableHandCollisionHaptics, "; Controller buzz on hand-object contact");
        ini.SetDoubleValue("ObjectPickup", "fHandCollisionHapticScale", handCollisionHapticScale, "; Microseconds per mass-speed unit for haptic pulse");
        ini.SetBoolValue("ObjectPickup", "bEnableFingerSegmentColliders", enableFingerSegmentColliders, "; Per-finger KEYFRAMED bodies (experimental)");
        ini.SetDoubleValue("ObjectPickup", "fFingerSegmentHalfExtentX", fingerSegmentHalfExtentX, "; Finger collider half-extent along finger X axis");
        ini.SetDoubleValue("ObjectPickup", "fFingerSegmentHalfExtentY", fingerSegmentHalfExtentY, "; Finger collider half-extent along finger Y axis (length)");
        ini.SetDoubleValue("ObjectPickup", "fFingerSegmentHalfExtentZ", fingerSegmentHalfExtentZ, "; Finger collider half-extent along finger Z axis");
        ini.SetBoolValue("ObjectPickup", "bUseCollisionOverlapForGrabCandidates", useCollisionOverlapForGrabCandidates, "; HIGGS-style collision-driven grab candidate pool");
        ini.SetBoolValue("HeldBody", "bEnableGrabApproachSubstates", enableGrabApproachSubstates, "; Approach/Contact/Grip FSM smoothing");
        ini.SetDoubleValue("HeldBody", "fGrabApproachRampSeconds", grabApproachRampSeconds, "; Seconds to ramp motor tau on grab onset");

        // HeldBody constraint grab
        ini.SetBoolValue("HeldBody", "bUseHeldBodyGrab", useHeldBodyGrab, "; HIGGS-style HeldBody grab (object stays DYNAMIC)");
        ini.SetLongValue("HeldBody", "iHeldBodyMode", static_cast<long>(static_cast<int>(GetHeldBodyMode())), "; 0=Dynamic spring, 1=Custom 6-DOF, 2=Native ragdoll constraint");
        ini.SetBoolValue("HeldBody", "bUseNativeRagdollConstraint", UseHeldBodyNativeConstraint(), "; Legacy fallback if iHeldBodyMode is absent");
        ini.SetBoolValue("HeldBody", "bUseSimpleHandBodyCreation", useSimpleHandBodyCreation, "; Use Bethesda CreateInstance path for HeldBody hand bodies");
        ini.SetDoubleValue("HeldBody", "fTauLerpTime", heldBodyTauLerpTime, "; Seconds to lerp spring strength from start to target");
        ini.SetDoubleValue("HeldBody", "fAngularTau", grabConstraintAngularTau, "; Angular resting tau (0=soft, 1=hard)");
        ini.SetDoubleValue("HeldBody", "fAngularTauBody", grabConstraintAngularTauBody, "; Angular steady-state tau for held bodies");
        ini.SetDoubleValue("HeldBody", "fAngularTauBodyStart", grabConstraintAngularTauBodyStart, "; Angular tau at grab start");
        ini.SetDoubleValue("HeldBody", "fAngularDamping", grabConstraintAngularDamping, "; Angular damping factor");
        ini.SetDoubleValue("HeldBody", "fAngularMaxForce", grabConstraintAngularMaxForce, "; Max angular force");
        ini.SetDoubleValue("HeldBody", "fLinearTau", grabConstraintLinearTau, "; Linear resting tau");
        ini.SetDoubleValue("HeldBody", "fLinearTauBody", grabConstraintLinearTauBody, "; Linear steady-state tau for held bodies");
        ini.SetDoubleValue("HeldBody", "fLinearTauBodyStart", grabConstraintLinearTauBodyStart, "; Linear tau at grab start");
        ini.SetDoubleValue("HeldBody", "fLinearDamping", grabConstraintLinearDamping, "; Linear damping factor");
        ini.SetDoubleValue("HeldBody", "fLinearMaxForce", grabConstraintLinearMaxForce, "; Max linear force");
        ini.SetDoubleValue("HeldBody", "fAngularToLinearForceRatio", grabConstraintAngularToLinearForceRatio, "; Force ratio between angular and linear");
        ini.SetBoolValue("HeldBody",   "bEnableSoftLimits", grabConstraintEnableSoftLimits, "; Clamp motor targets to prevent runaway stretch/twist (soft 6DOF limits)");
        ini.SetDoubleValue("HeldBody", "fLinearMaxStretch", grabConstraintLinearMaxStretch, "; Max distance (game units) grabbed object can drift from hand when soft limits on");
        ini.SetDoubleValue("HeldBody", "fAngularMaxAngleDeg", grabConstraintAngularMaxAngleDeg, "; Max angular deviation (degrees) from initial grab orientation when soft limits on");
        ini.SetBoolValue("HeldBody",   "bForceDynamicHeldBody", grabConstraintForceDynamicHeldBody, "; Guard: never KEYFRAME the grabbed body while motor constraint is active");

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
    }

    // Seed the MCM settings file with field defaults for any MCM-exposed control it lacks, so the
    // menu and engine agree. Runs once per session. Without it, a control absent from the MCM file
    // falls through to the (possibly stale) external INI — so a toggle can read OFF in the menu yet
    // behave ON until the user toggles it ON then OFF to force an explicit MCM entry.
    void Config::SeedMCMDefaultsIfMissing()
    {
        static bool s_seeded = false;
        if (s_seeded) return;
        s_seeded = true;

        // Read config.json and collect every control id ("key:section").
        std::ifstream f(kMCMConfigPath);
        if (!f) {
            spdlog::debug("[Config] No MCM config.json at {} - skipping default seeding", kMCMConfigPath);
            return;
        }
        std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        std::vector<std::pair<std::string, std::string>> mcmKeys;  // (section, key)
        size_t pos = 0;
        while ((pos = json.find("\"id\"", pos)) != std::string::npos) {
            size_t q1 = json.find('"', json.find(':', pos + 4));
            if (q1 == std::string::npos) break;
            size_t q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string id = json.substr(q1 + 1, q2 - q1 - 1);  // "key:section"
            pos = q2 + 1;
            size_t sep = id.find(':');
            if (sep == std::string::npos) continue;
            mcmKeys.emplace_back(id.substr(sep + 1), id.substr(0, sep));
        }
        if (mcmKeys.empty()) return;

        // Field defaults from a freshly-constructed Config.
        Config def;
        CSimpleIniA defIni;
        defIni.SetUnicode();
        def.BuildIni(&defIni);

        CSimpleIniA mcm;
        mcm.SetUnicode();
        mcm.LoadFile(kMCMSettingsPath);  // ok if absent

        int wrote = 0;
        for (const auto& [section, key] : mcmKeys) {
            if (mcm.GetValue(section.c_str(), key.c_str(), nullptr) != nullptr) continue;  // user already set it
            const char* defVal = defIni.GetValue(section.c_str(), key.c_str(), nullptr);
            if (!defVal) continue;  // control id with no matching serialized setting
            // MCM stores bools as 1/0, not true/false (SetBoolValue's output) — convert so the
            // menu (and F4SE's GetModSettingBool) reads the seeded default correctly.
            std::string v = defVal;
            if (v == "true") v = "1";
            else if (v == "false") v = "0";
            mcm.SetValue(section.c_str(), key.c_str(), v.c_str());
            ++wrote;
        }
        if (wrote > 0) {
            mcm.SaveFile(kMCMSettingsPath);
            spdlog::info("[Config] Seeded {} missing MCM default(s) into {}", wrote, kMCMSettingsPath);
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
