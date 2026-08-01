#include "Config.h"
#include "Hooks.h"

#include <SimpleIni.h>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
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

    namespace
    {
        struct NumericSettingLocation
        {
            std::string section;
            std::string key;
        };

        constexpr std::array<std::string_view, 23> kHeisenbergIniSections{
            "Selection", "SeatedMode", "ObjectPickup", "ItemPositioning",
            "DropToHand", "Impact", "ItemStorage", "Consumables", "Equipping",
            "ROCK", "Throw", "Haptics", "Timing", "Cooking", "SmartGrab",
            "Activators", "Pipboy", "Water", "Pickpocket", "Debug",
            "RockIntegration", "RockEngine", "VRInput"
        };

        std::mutex g_numericSettingMutex;

        bool ResolveNumericSetting(
            const CSimpleIniA& ini,
            const char* rawName,
            NumericSettingLocation& result)
        {
            if (!rawName || !*rawName) {
                return false;
            }

            const std::string_view name(rawName);
            const auto separator = name.find('.');
            if (separator != std::string_view::npos) {
                if (separator == 0 || separator + 1 >= name.size()) {
                    return false;
                }

                NumericSettingLocation qualified{
                    std::string(name.substr(0, separator)),
                    std::string(name.substr(separator + 1))
                };
                if (!ini.GetValue(qualified.section.c_str(), qualified.key.c_str(), nullptr)) {
                    return false;
                }
                result = std::move(qualified);
                return true;
            }

            bool found = false;
            for (const auto section : kHeisenbergIniSections) {
                const std::string sectionName(section);
                if (!ini.GetValue(sectionName.c_str(), rawName, nullptr)) {
                    continue;
                }
                // An unqualified duplicate is ambiguous by definition. Require
                // callers to use Section.key instead of silently editing one.
                if (found) {
                    return false;
                }
                result = { sectionName, rawName };
                found = true;
            }
            return found;
        }

        bool DecodeNumericValue(const char* textValue, double& value)
        {
            if (!textValue) {
                return false;
            }
            if (_stricmp(textValue, "true") == 0) {
                value = 1.0;
                return true;
            }
            if (_stricmp(textValue, "false") == 0) {
                value = 0.0;
                return true;
            }

            errno = 0;
            char* end = nullptr;
            const double parsed = std::strtod(textValue, &end);
            while (end && *end == ' ') {
                ++end;
            }
            if (errno == ERANGE || end == textValue || (end && *end != '\0') ||
                !std::isfinite(parsed)) {
                return false;
            }
            value = parsed;
            return true;
        }

        bool AssignNumericValue(
            CSimpleIniA& ini,
            const NumericSettingLocation& setting,
            double value)
        {
            if (!std::isfinite(value) || setting.key.empty()) {
                return false;
            }

            const char kind = setting.key.front();
            SI_Error result = SI_FAIL;
            switch (kind) {
            case 'b':
                if (value != 0.0 && value != 1.0) {
                    return false;
                }
                result = ini.SetBoolValue(
                    setting.section.c_str(), setting.key.c_str(), value != 0.0);
                break;
            case 'i': {
                const double integral = std::trunc(value);
                if (integral != value ||
                    integral < static_cast<double>((std::numeric_limits<long>::min)()) ||
                    integral > static_cast<double>((std::numeric_limits<long>::max)())) {
                    return false;
                }
                result = ini.SetLongValue(
                    setting.section.c_str(),
                    setting.key.c_str(),
                    static_cast<long>(integral));
                break;
            }
            case 'f':
                result = ini.SetDoubleValue(
                    setting.section.c_str(), setting.key.c_str(), value);
                break;
            default:
                return false;
            }
            return result >= 0;
        }

        bool SaveIniAtomically(CSimpleIniA& ini, const char* path)
        {
            const std::string temporaryPath = std::string(path) + ".tmp";
            if (ini.SaveFile(temporaryPath.c_str()) < 0) {
                spdlog::error("[Config] Failed writing temporary settings file {}", temporaryPath);
                return false;
            }
            if (!MoveFileExA(
                    temporaryPath.c_str(),
                    path,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const auto error = GetLastError();
                DeleteFileA(temporaryPath.c_str());
                spdlog::error("[Config] Failed replacing {} (Win32 error {})", path, error);
                return false;
            }
            return true;
        }
    }
    
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
fFingerAnimCloseSpeed = 12.0
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

; Holotape To Hand: Off = holotape plays immediately when selected from
; Pipboy inventory. On = it appears in your right hand instead of playing.
bHolotapeToHand = true

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
fTapeDeckPushCloseRadius = 1.2

; Hide action prompts ([A] Take, [B] Transfer) from wand rollover HUD
; Item names still show. Binary patches ShowActivateButton/ShowSecondaryButton.
bHideWandHUD = true

; Hide all wand HUD messages (actions AND item display) when pointing with wands
bHideAllWandHUD = false

[Debug]
; Temporarily enable debug-level startup/config/hook diagnostics before iLogLevel
; is applied. This does not change the steady-state log level below.
bVerboseLaunch = false

; Log level: 0=trace, 1=debug, 2=info, 3=warn, 4=error
; Default: 4 (error) - release default. Use 2 (info) or 1 (debug) to diagnose.
iLogLevel = 4
)";

    void Config::Load()
    {
        spdlog::info("Loading config...");

        // Fresh-default snapshot in the ORIGINAL units (cm for the conversion-block fields).
        // Used as the GetDoubleValue default and as the reset source for never-loaded convertible
        // fields, so the cm->game conversion below is idempotent across repeated Load() calls
        // (OnGameLoad re-runs Load on every save load). Previously the live (already-converted)
        // field was used as the default, so any conversion-block field absent from the INI was
        // re-multiplied by CM_TO_GAME_UNITS every reload and shrank ~0.7x each time.
        static const Config kDefaults;

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
        enableThrowableHolstering = ini.GetBoolValue("ObjectPickup", "bEnableThrowableHolstering", enableThrowableHolstering);
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
        enableTwoHandedGrab = ini.GetBoolValue("ObjectPickup", "bEnableTwoHandedGrab", enableTwoHandedGrab);
        enableLimbGrab = ini.GetBoolValue("ObjectPickup", "bEnableLimbGrab", enableLimbGrab);
        heldObjectCollidable = ini.GetBoolValue("ObjectPickup", "bHeldObjectCollidable", heldObjectCollidable);
        heldObjectWallClamp = ini.GetBoolValue("ObjectPickup", "bHeldObjectWallClamp", heldObjectWallClamp);
        blockActivateOnGrabSelection = ini.GetBoolValue("ObjectPickup", "bBlockActivateOnGrabSelection", blockActivateOnGrabSelection);
        useXForLeftGrab  = ini.GetBoolValue("ObjectPickup", "bUseXForLeftGrab",  useXForLeftGrab);
        useAForRightGrab = ini.GetBoolValue("ObjectPickup", "bUseAForRightGrab", useAForRightGrab);
        if (useXForLeftGrab && !enableStickyGrab) {
            // X is a digital alternate grab button, so hold-to-grab would release
            // immediately after the press. Make the dependency explicit and persist
            // it in MCM's highest-priority settings file so both runtime and menu
            // agree. Turning the alternate control back off intentionally leaves
            // Sticky Grab enabled; the user can then disable it independently.
            enableStickyGrab = true;
            mcmIni.SetValue(
                "ObjectPickup",
                "bEnableStickyGrab",
                "1");
            const SI_Error saveRc =
                mcmIni.SaveFile(kMCMSettingsPath);
            if (saveRc >= 0) {
                spdlog::info(
                    "[Config] Left Grab Alternate Control enabled — "
                    "automatically enabled and persisted Sticky Grab");
            } else {
                spdlog::warn(
                    "[Config] Left Grab Alternate Control requires Sticky Grab; "
                    "runtime enabled it but MCM persistence failed (code {})",
                    static_cast<int>(saveRc));
            }
        }
        grabStartSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabStartSpeed", grabStartSpeed));
        grabStartAngularSpeed = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabStartAngularSpeed", grabStartAngularSpeed));
        
        // Pickup distance settings
        maxGrabDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fMaxGrabDistance", kDefaults.maxGrabDistance));
        enableNaturalGrab = ini.GetBoolValue("ObjectPickup", "bEnableNaturalGrab", enableNaturalGrab);
        naturalGrabDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fNaturalGrabDistance", kDefaults.naturalGrabDistance));
        naturalGrabDistanceNoMatch = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fNaturalGrabDistanceNoMatch", kDefaults.naturalGrabDistanceNoMatch));
        
        // Palm snap settings
        enablePalmSnap = ini.GetBoolValue("ObjectPickup", "bEnablePalmSnap", enablePalmSnap);
        enableAutomaticHandPlacement = ini.GetBoolValue("ObjectPickup", "bEnableAutomaticHandPlacement", enableAutomaticHandPlacement);
        enableAutomaticFingerCurls = ini.GetBoolValue("ObjectPickup", "bEnableAutomaticFingerCurls", enableAutomaticFingerCurls);
        saveNaturalGrabAsOffset = ini.GetBoolValue("ObjectPickup", "bSaveNaturalGrabAsOffset", saveNaturalGrabAsOffset);
        enablePalmRayCastPlacement = ini.GetBoolValue("ObjectPickup", "bEnablePalmRayCastPlacement", enablePalmRayCastPlacement);
        enableAxialPlacement = ini.GetBoolValue("ObjectPickup", "bEnableAxialPlacement", enableAxialPlacement);
        axialPlacementClearance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fAxialPlacementClearance", axialPlacementClearance));
        grabMaxTriangleDistance = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fGrabMaxTriangleDistance", grabMaxTriangleDistance));
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
        // Backwards compat: old bool bEnableDropToHand maps to 0/1, new int iDropToHandMode adds mode 2.
        // BUG (Jul 20, Ginga's VR Essentials user report + matches an unverified Jul-19 audit
        // finding): third-party MCM frameworks (e.g. f4mcm) that scrape/write a mod's .ini
        // directly, bypassing our own Config::Save(), only know the legacy bEnableDropToHand
        // toggle. iDropToHandMode is ALWAYS present after the first-ever Save() (Save()
        // unconditionally writes it, never bEnableDropToHand), so under the old precedence a
        // user toggling "off" via such an MCM UI wrote bEnableDropToHand=false to disk and it
        // was silently ignored forever — iDropToHandMode's mere presence took over regardless
        // of its own value. Confirmed via a real session log: dropMode=1 logged for the entire
        // session despite the user believing drop-to-hand was disabled. An explicit
        // bEnableDropToHand=false is now treated as an unconditional override to off — the
        // clearest, most unambiguous "user wants this off" signal an external tool can produce,
        // and it must never be silently defeated by a stale sibling key. Self-heals: the next
        // Config::Save() persists iDropToHandMode=0, so the two keys agree from then on.
        //
        // SYMMETRY FIX: the override used to be one-directional, which stranded the ON case.
        // Toggling the MCM switch OFF forced mode 0, the "self-heal" then persisted
        // iDropToHandMode=0 to Heisenberg_F4VR.ini, and toggling the switch back ON only
        // cleared the override - control fell through to the iDropToHandMode branch, which
        // read back the persisted 0. The menu showed the feature ON while dropped items kept
        // falling to the floor, and there was no way out in-game because iDropToHandMode is
        // not an MCM control (config.json exposes only bEnableDropToHand:DropToHand) and
        // BuildIni deliberately never writes bEnableDropToHand. An explicit true is now just
        // as authoritative as an explicit false: it promotes a stranded 0 back to mode 1
        // while still honouring any non-zero mode the user picked by hand.
        // Presence must come from a USER-authored layer. The merged `ini` always contains
        // kDefaultConfig's bEnableDropToHand=true, so probing it makes an explicitly authored
        // iDropToHandMode=0 look as if the user also authored legacy=true and promotes 0 back
        // to 1. MCM is the highest-priority user layer, matching the merge order above.
        const CSimpleIniA* legacyDropToHandSource = nullptr;
        if (mcmIni.GetValue("DropToHand", "bEnableDropToHand", nullptr)) {
            legacyDropToHandSource = &mcmIni;
        } else if (externalIni.GetValue("DropToHand", "bEnableDropToHand", nullptr)) {
            legacyDropToHandSource = &externalIni;
        }
        const bool legacyDropToHandPresent = legacyDropToHandSource != nullptr;
        const bool legacyDropToHandValue = legacyDropToHandSource
            ? legacyDropToHandSource->GetBoolValue("DropToHand", "bEnableDropToHand", true)
            : true;
        const bool legacyDropToHandExplicitlyOff = legacyDropToHandPresent && !legacyDropToHandValue;
        const bool legacyDropToHandExplicitlyOn = legacyDropToHandPresent && legacyDropToHandValue;
        if (legacyDropToHandExplicitlyOff) {
            dropToHandMode = 0;
        } else if (ini.GetValue("DropToHand", "iDropToHandMode")) {
            dropToHandMode = static_cast<int>(ini.GetLongValue("DropToHand", "iDropToHandMode", dropToHandMode));
            if (legacyDropToHandExplicitlyOn && dropToHandMode == 0) {
                dropToHandMode = 1;
            }
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
        enableDropToContainer = ini.GetBoolValue("ItemStorage", "bEnableDropToContainer", enableDropToContainer);
        companionTransferRadius = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fCompanionTransferRadius", companionTransferRadius));
        handTransferRadius = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fHandTransferRadius", handTransferRadius));
        enableAutoStorage = ini.GetBoolValue("ItemStorage", "bEnableAutoStorage", enableAutoStorage);

        // Item Storage Zone
        enableItemStorageZones = ini.GetBoolValue("ItemStorage", "bEnableItemStorageZones", enableItemStorageZones);
        enableStorageZoneWeaponEquip = ini.GetBoolValue("ItemStorage", "bEnableStorageZoneWeaponEquip", enableStorageZoneWeaponEquip);
        showStorageMessages = ini.GetBoolValue("ItemStorage", "bShowStorageMessages", showStorageMessages);
        enableStorageZoneConfigMode = ini.GetBoolValue("ItemStorage", "bEnableStorageZoneConfigMode", enableStorageZoneConfigMode);
        itemStorageZoneRadius = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fItemStorageZoneRadius", kDefaults.itemStorageZoneRadius));
        // Storage zone position (single zone)
        storageZoneOffsetX = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneOffsetX", storageZoneOffsetX));
        storageZoneOffsetY = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneOffsetY", storageZoneOffsetY));
        storageZoneOffsetZ = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneOffsetZ", storageZoneOffsetZ));
        requireHandBehindHead = ini.GetBoolValue("ItemStorage", "bRequireHandBehindHead", requireHandBehindHead);
        behindHeadTolerance = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fBehindHeadTolerance", behindHeadTolerance));
        storageZoneHoldTime = static_cast<float>(ini.GetDoubleValue("ItemStorage", "fStorageZoneHoldTime", storageZoneHoldTime));
        
        // ROCK integration
        useRockPhysics = static_cast<int>(ini.GetLongValue("ROCK", "iUseRockPhysics", useRockPhysics));
        delegateWorldGrabToRock = ini.GetBoolValue("ROCK", "bDelegateWorldGrabToRock", delegateWorldGrabToRock);

        // Hand collision
        enableHandCollision = ini.GetBoolValue("ObjectPickup", "bEnableHandCollision", enableHandCollision);

        // These two engine-tuning values are diagnostic controls, not supported
        // release settings. Keep them configurable in _DEBUG builds while making a
        // Release immune to stale external/MCM overrides from older packages.
#if defined(_DEBUG)
        handColliderRadiusPadding        = static_cast<float>(ini.GetDoubleValue("RockIntegration", "fHandColliderRadiusPadding", handColliderRadiusPadding));
        offHandSteeringAuthority         = static_cast<float>(ini.GetDoubleValue("RockIntegration", "fOffHandSteeringAuthority", offHandSteeringAuthority));
#else
        handColliderRadiusPadding = Config::kHandColliderRadiusPaddingDefault;
        offHandSteeringAuthority = Config::kOffHandSteeringAuthorityDefault;
#endif
        rockHandBumpGuard                = ini.GetBoolValue("RockIntegration", "bHandBumpGuard",                rockHandBumpGuard);
        spdlog::info("[Config] RockIntegration: BumpGuard={} colliderPadding={:.2f} offHandAuthority={:.2f}",
                     rockHandBumpGuard, handColliderRadiusPadding, offHandSteeringAuthority);
        handCollisionRadius = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandCollisionRadius", handCollisionRadius));
        handContactSlop = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandContactSlop", handContactSlop));
        handPushVelocityThreshold = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushVelocityThreshold", handPushVelocityThreshold));
        handPushForceMultiplier = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandPushForceMultiplier", handPushForceMultiplier));
        enableHandCollisionHaptics = ini.GetBoolValue("ObjectPickup", "bEnableHandCollisionHaptics", enableHandCollisionHaptics);
        handCollisionHapticScale = static_cast<float>(ini.GetDoubleValue("ObjectPickup", "fHandCollisionHapticScale", handCollisionHapticScale));

        handAuthorityApplyMode  = static_cast<int>(ini.GetLongValue("ObjectPickup", "iHandAuthorityApplyMode", handAuthorityApplyMode));
        handAuthorityGoalHookLog = ini.GetBoolValue("ObjectPickup", "bHandAuthorityGoalHookLog", handAuthorityGoalHookLog);
        suppressThumbstickTouch = ini.GetBoolValue("VRInput", "bSuppressThumbstickTouch", suppressThumbstickTouch);
        postLoadInputRecovery = ini.GetBoolValue("VRInput", "bPostLoadInputRecovery", postLoadInputRecovery);
        pipboyBootOpensContent = ini.GetBoolValue("Pipboy", "bPipboyBootOpensContent", pipboyBootOpensContent);
        pipboyBootScreenDropGameUnits = static_cast<float>(ini.GetDoubleValue("Pipboy", "fPipboyBootScreenDrop", pipboyBootScreenDropGameUnits));
        twoHandedFingerPoseMode = static_cast<int>(ini.GetLongValue("ObjectPickup", "iTwoHandedFingerPoseMode", twoHandedFingerPoseMode));
        havokTimingFix = ini.GetBoolValue("RockIntegration", "bHavokTimingFix", havokTimingFix);
        havokTimingMinFrameRate = static_cast<float>(ini.GetDoubleValue("RockIntegration", "fHavokTimingMinFrameRate", havokTimingMinFrameRate));
        havokTimingMaxSubsteps = static_cast<int>(ini.GetLongValue("RockIntegration", "iHavokTimingMaxSubsteps", havokTimingMaxSubsteps));
        useRockEngineArchitecture = ini.GetBoolValue("RockEngine", "bUseRockEngineArchitecture", useRockEngineArchitecture);

        // Hand collision enabled — pair filter + proxy listener handle player capsule

        // Consumable zones (mouth)
        consumableActivationZone = static_cast<int>(ini.GetLongValue("Consumables", "iConsumableActivationZone", consumableActivationZone));
        mouthOffsetX = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthOffsetX", kDefaults.mouthOffsetX));
        mouthOffsetY = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthOffsetY", kDefaults.mouthOffsetY));
        mouthOffsetZ = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthOffsetZ", kDefaults.mouthOffsetZ));
        mouthRadius = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthRadius", kDefaults.mouthRadius));
        mouthVelocityThreshold = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthVelocityThreshold", mouthVelocityThreshold));
        mouthDropHapticStrength = static_cast<float>(ini.GetDoubleValue("Consumables", "fMouthHapticStrength", mouthDropHapticStrength));
        blockConsumptionInPA = ini.GetBoolValue("Consumables", "bBlockConsumptionInPA", blockConsumptionInPA);
        allowWeaponGrabInPowerArmor = ini.GetBoolValue("Grab", "bAllowWeaponGrabInPowerArmor", allowWeaponGrabInPowerArmor);
        consumableToHand = ini.GetBoolValue("Consumables", "bConsumableToHand", consumableToHand);
        holotapeToHand = ini.GetBoolValue("Consumables", "bHolotapeToHand", holotapeToHand);
        showConsumeMessages = ini.GetBoolValue("Consumables", "bShowConsumeMessages", showConsumeMessages);

        // Hand injection zone
        enableHandInjection = ini.GetBoolValue("Consumables", "bEnableHandInjection", enableHandInjection);
        handInjectionOffsetX = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionOffsetX", kDefaults.handInjectionOffsetX));
        handInjectionOffsetY = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionOffsetY", kDefaults.handInjectionOffsetY));
        handInjectionOffsetZ = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionOffsetZ", kDefaults.handInjectionOffsetZ));
        handInjectionRadius = static_cast<float>(ini.GetDoubleValue("Consumables", "fHandInjectionRadius", kDefaults.handInjectionRadius));

        // Armor equip zones
        armorEquipZone = static_cast<int>(ini.GetLongValue("Equipping", "iArmorEquipZone", armorEquipZone));
        headZoneRadius = static_cast<float>(ini.GetDoubleValue("Equipping", "fHeadZoneRadius", kDefaults.headZoneRadius));
        chestZoneRadius = static_cast<float>(ini.GetDoubleValue("Equipping", "fChestZoneRadius", kDefaults.chestZoneRadius));
        legZoneRadius = static_cast<float>(ini.GetDoubleValue("Equipping", "fLegZoneRadius", kDefaults.legZoneRadius));
        // Armor zone offsets — now loadable (cm in INI; converted in the block below)
        headZoneOffsetX = static_cast<float>(ini.GetDoubleValue("Equipping", "fHeadZoneOffsetX", kDefaults.headZoneOffsetX));
        headZoneOffsetY = static_cast<float>(ini.GetDoubleValue("Equipping", "fHeadZoneOffsetY", kDefaults.headZoneOffsetY));
        headZoneOffsetZ = static_cast<float>(ini.GetDoubleValue("Equipping", "fHeadZoneOffsetZ", kDefaults.headZoneOffsetZ));
        chestZoneOffsetX = static_cast<float>(ini.GetDoubleValue("Equipping", "fChestZoneOffsetX", kDefaults.chestZoneOffsetX));
        chestZoneOffsetY = static_cast<float>(ini.GetDoubleValue("Equipping", "fChestZoneOffsetY", kDefaults.chestZoneOffsetY));
        chestZoneOffsetZ = static_cast<float>(ini.GetDoubleValue("Equipping", "fChestZoneOffsetZ", kDefaults.chestZoneOffsetZ));
        legZoneOffsetX = static_cast<float>(ini.GetDoubleValue("Equipping", "fLegZoneOffsetX", kDefaults.legZoneOffsetX));
        legZoneOffsetY = static_cast<float>(ini.GetDoubleValue("Equipping", "fLegZoneOffsetY", kDefaults.legZoneOffsetY));
        legZoneOffsetZ = static_cast<float>(ini.GetDoubleValue("Equipping", "fLegZoneOffsetZ", kDefaults.legZoneOffsetZ));

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
        holotapeWristOverrideInProjected = ini.GetBoolValue("Pipboy", "bHolotapeWristOverrideInProjected", holotapeWristOverrideInProjected);
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
        // Ceiling raised 1.0 -> 3.0 (Jul 27). Measured finger-vs-token intersections reached
        // 1.26gu of overlap, i.e. DEEPER than the largest padding the old clamp would accept — so
        // the "fingers clip into the token" report was untunable by construction. The overlap is a
        // render-vs-collision size mismatch (the diagnostic measures the token's RENDER mesh
        // against the finger's physics capsule, and FO4 clutter hulls are inset), which padding is
        // exactly the right compensator for.
        clampFloat(handColliderRadiusPadding, 0.0f, 3.0f, "fHandColliderRadiusPadding");
        clampFloat(offHandSteeringAuthority, 0.35f, 1.0f, "fOffHandSteeringAuthority");
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
        clampInt(grabMode, 0, 9, "iGrabMode");  // only 0 (Keyframed) and 9 (Full Dynamic) are real backends; every other value degrades to 0 via GetEffectiveGrabMode()
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

        // Never loaded from INI — reset to cm default before converting so they don't
        // compound the cm->game conversion across reloads (they previously shrank every Load).
        throwableZoneRadius = kDefaults.throwableZoneRadius;

        // User-facing values in INI are in cm for intuitive configuration
        ApplyCmToGameUnitsConversion();

        spdlog::info("Config loaded");
    }

    void Config::ApplyCmToGameUnitsConversion()
    {
        maxGrabDistance *= CM_TO_GAME_UNITS;
        naturalGrabDistance *= CM_TO_GAME_UNITS;
        naturalGrabDistanceNoMatch *= CM_TO_GAME_UNITS;
        throwableZoneRadius *= CM_TO_GAME_UNITS;
        itemStorageZoneRadius *= CM_TO_GAME_UNITS;  // Load now matches Save (*GAME_UNITS_TO_CM); was asymmetric → inflated ~1.4x per Save/Load cycle
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
    }

    void Config::Save()
    {
        spdlog::info("Saving config to {}", kConfigPath);

        CSimpleIniA ini;
        ini.SetUnicode();

        // Embedded ROCK uses this same file for [PhysicsInteraction],
        // [RealisticWeapons], and its [Debug] values. Load the existing file
        // before replacing Heisenberg-owned keys so Save() preserves every
        // ROCK setting and any future third-party/unknown sections.
        const SI_Error loadRc = ini.LoadFile(kConfigPath);
        if (loadRc < 0) {
            spdlog::debug("No existing config to merge while saving (code {})", static_cast<int>(loadRc));
        }

        BuildIni(&ini);

        SI_Error rc = ini.SaveFile(kConfigPath);
        if (rc < 0) {
            spdlog::error("Failed to save config file");
        }
    }

    bool Config::GetNumericSetting(const char* name, double& out) const
    {
        std::lock_guard lock(g_numericSettingMutex);

        CSimpleIniA effective;
        effective.SetUnicode();
        BuildIni(&effective);

        NumericSettingLocation setting;
        if (!ResolveNumericSetting(effective, name, setting)) {
            return false;
        }
        return DecodeNumericValue(
            effective.GetValue(setting.section.c_str(), setting.key.c_str(), nullptr),
            out);
    }

    bool Config::SetNumericSetting(const char* name, double value)
    {
        std::lock_guard lock(g_numericSettingMutex);

        // Resolve against a serialization of the live effective state. This
        // both constrains edits to Heisenberg-owned numeric keys and reports
        // values in the same units the INI exposes.
        CSimpleIniA effective;
        effective.SetUnicode();
        BuildIni(&effective);

        NumericSettingLocation setting;
        if (!ResolveNumericSetting(effective, name, setting)) {
            return false;
        }

        // MCM settings overlay the plugin INI during Load(). If the key exists
        // there, edit that authoritative value; otherwise edit the plugin INI.
        CSimpleIniA mcm;
        mcm.SetUnicode();
        const bool hasMcmOverlay =
            mcm.LoadFile(kMCMSettingsPath) >= 0 &&
            mcm.GetValue(setting.section.c_str(), setting.key.c_str(), nullptr);

        if (hasMcmOverlay) {
            if (setting.key.front() == 'b') {
                if ((value != 0.0 && value != 1.0) ||
                    mcm.SetValue(
                        setting.section.c_str(),
                        setting.key.c_str(),
                        value != 0.0 ? "1" : "0") < 0) {
                    return false;
                }
            } else if (!AssignNumericValue(mcm, setting, value)) {
                return false;
            }
            const std::string marker = setting.section + "." + setting.key;
            mcm.Delete("__HeisenbergSeededDefaults", marker.c_str());
            if (!SaveIniAtomically(mcm, kMCMSettingsPath)) {
                return false;
            }
        } else {
            CSimpleIniA primary;
            primary.SetUnicode();
            if (primary.LoadFile(kConfigPath) < 0) {
                // Preserve all current Heisenberg defaults when creating a
                // previously absent file.
                BuildIni(&primary);
            }
            if (!AssignNumericValue(primary, setting, value) ||
                !SaveIniAtomically(primary, kConfigPath)) {
                return false;
            }
        }

        Load();
        return true;
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
        ini.SetBoolValue("ObjectPickup", "bEnableThrowableHolstering", enableThrowableHolstering, "; Holster throwables (grenades/mines) on VH holster zones");
        ini.SetBoolValue("ObjectPickup", "bEnableUnarmedAutoUnequip", enableUnarmedAutoUnequip, "; Auto-unequip Unarmed when grip pressed (disable if melee broken)");
        ini.SetBoolValue("ObjectPickup", "bDisableGripWeaponDraw", disableGripWeaponDraw, "; Prevent grip from drawing/sheathing weapons");
        ini.SetBoolValue("ObjectPickup", "bPassGripToActivatorSecondary", passGripToActivatorSecondary, "; When grip-weapon-draw is off, still pass primary grip to the game when aiming at an activator/furniture (mod secondary actions)");
        ini.SetBoolValue("ObjectPickup", "bUseGrenadeReadyHook", useGrenadeReadyHook, "; Control grenade readying at the game level (hook MeleeThrowHandler) instead of stripping grip input (fixes VirtualHolsters/reload/secondary)");
        ini.SetBoolValue("ObjectPickup", "bShowHolsterMessages", showHolsterMessages, "; Show HUD message when holstering weapons");
        ini.SetBoolValue("ObjectPickup", "bShowUnequipMessages", showUnequipMessages, "; Show HUD message when unequipping weapons");
        ini.SetBoolValue("ObjectPickup", "bEnableTwoHandedGrab", enableTwoHandedGrab, "; Two-handed single-object grab: second hand aims/stabilizes the held object");
        ini.SetBoolValue("ObjectPickup", "bEnableLimbGrab", enableLimbGrab, "; Limb grab: grab the nearest ragdoll limb of an actor (needs bEnableGrabActors); CTD-prone, opt-in");
        ini.SetLongValue("ObjectPickup", "iGrabMode", grabMode, "; 0=Keyframed (Heisenberg keyframed backend), 9=Full Dynamic (embedded ROCK engine owns grab+selection; needs bUseRockEngineArchitecture=1). Other values degrade to 0.");
        ini.SetBoolValue("ObjectPickup", "bHeldObjectCollidable", heldObjectCollidable, "; Keyframed hold keeps object collidable so it can hit other objects (iGrabMode=0 only)");
        ini.SetBoolValue("ObjectPickup", "bHeldObjectWallClamp", heldObjectWallClamp, "; Keyframed hold: sweep-cast the held object so it STOPS at walls and NPCs instead of clipping through (iGrabMode=0)");
        ini.SetBoolValue("ObjectPickup", "bBlockActivateOnGrabSelection", blockActivateOnGrabSelection, "; Block A/activate looting an item just because you're aiming at it. OFF by default (ON broke A-looting + mod secondary actions). Only enable for a Grip>A SteamVR binding.");
        ini.SetBoolValue("ObjectPickup", "bUseXForLeftGrab",  useXForLeftGrab,  "; Use X instead of Grip for left-hand grabbing; enabling automatically enables Sticky Grab");
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
        ini.SetBoolValue("ItemStorage", "bEnableDropToContainer", enableDropToContainer, "; Release while pointing at a chest/desk deposits the item into it");
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
        ini.SetBoolValue("Grab", "bAllowWeaponGrabInPowerArmor", allowWeaponGrabInPowerArmor, "; Allow grabbing weapons while in Power Armor (false restores the legacy block)");
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
        // fLegZoneRadius is loaded (Load() reads+clamps+converts it) but was never written
        // here - every BuildIni() (Save(), the intro-ceremony auto-save, item-positioning
        // config-mode saves) silently dropped the user's value back to the embedded default
        // on the next launch. The leg OFFSET fields below were already written; only the
        // radius itself was missing.
        ini.SetDoubleValue("Equipping", "fLegZoneRadius", legZoneRadius * GAME_UNITS_TO_CM, "; cm - leg zone radius for pants/boots");
        ini.SetDoubleValue("Equipping", "fHeadZoneOffsetX", headZoneOffsetX * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fHeadZoneOffsetY", headZoneOffsetY * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fHeadZoneOffsetZ", headZoneOffsetZ * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fChestZoneOffsetX", chestZoneOffsetX * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fChestZoneOffsetY", chestZoneOffsetY * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fChestZoneOffsetZ", chestZoneOffsetZ * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fLegZoneOffsetX", legZoneOffsetX * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fLegZoneOffsetY", legZoneOffsetY * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        ini.SetDoubleValue("Equipping", "fLegZoneOffsetZ", legZoneOffsetZ * GAME_UNITS_TO_CM, "; cm - armor zone offset");
        
        // ROCK integration
        ini.SetLongValue("ROCK", "iUseRockPhysics", useRockPhysics, "; -1=auto (on if ROCK.dll present), 0=force off (built-in fallback), 1=force on");
        ini.SetBoolValue("ROCK", "bDelegateWorldGrabToRock", delegateWorldGrabToRock, "; true = ROCK owns world-object grab (fully dynamic held object); Heisenberg keeps loot/drop/holotape in-hand placement");

        // Hand collision
        ini.SetBoolValue("ObjectPickup", "bEnableHandCollision", enableHandCollision, "; Enable hand collision with world");
        ini.SetDoubleValue("ObjectPickup", "fHandCollisionRadius", handCollisionRadius, "; Game units - radius of hand collision sphere (broadphase reach)");
        ini.SetDoubleValue("ObjectPickup", "fHandContactSlop", handContactSlop, "; Game units - bone-to-mesh distance counted as TRUE contact (push trigger); ~finger flesh radius, smaller = must touch closer");
        ini.SetDoubleValue("ObjectPickup", "fHandPushVelocityThreshold", handPushVelocityThreshold, "; Min hand speed to push objects (lower = more sensitive)");
        ini.SetDoubleValue("ObjectPickup", "fHandPushForceMultiplier", handPushForceMultiplier, "; Push force multiplier (higher = stronger)");
        ini.SetBoolValue("ObjectPickup", "bEnableHandCollisionHaptics", enableHandCollisionHaptics, "; Controller buzz on hand-object contact");
        ini.SetDoubleValue("ObjectPickup", "fHandCollisionHapticScale", handCollisionHapticScale, "; Microseconds per mass-speed unit for haptic pulse");

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

        // Diagnostic-only ROCK tuning remains round-trippable in _DEBUG builds.
        // Release neither exposes nor serializes it, matching the fixed runtime values.
#if defined(_DEBUG)
        ini.SetDoubleValue("RockIntegration", "fHandColliderRadiusPadding",    handColliderRadiusPadding,        "; Padding (game units) added to every hand/arm collider radius — reduces resting-object clip-through, 0=unchanged");
        ini.SetDoubleValue("RockIntegration", "fOffHandSteeringAuthority",     offHandSteeringAuthority,         "; Minimum support-hand steering authority: 0.35=current balanced behavior, 1.0=full authority (safety rate limit still applies)");
#endif
        ini.SetBoolValue  ("RockIntegration", "bHandBumpGuard",                rockHandBumpGuard,                "; Char-proxy bump CTD guard (auto-armed when the embedded engine is hosted)");
        ini.SetBoolValue  ("RockIntegration", "bHavokTimingFix",               havokTimingFix,                   "; Override physics substeps to hold a min physics rate on long frames");
        ini.SetDoubleValue("RockIntegration", "fHavokTimingMinFrameRate",      havokTimingMinFrameRate,          "; Min physics frame rate, clamped [30,240] (default 70)");
        ini.SetLongValue  ("RockIntegration", "iHavokTimingMaxSubsteps",       havokTimingMaxSubsteps,           "; Max substeps, clamped [1,6] (default 3)");
        ini.SetBoolValue  ("RockEngine",      "bUseRockEngineArchitecture",    useRockEngineArchitecture,        "; SECOND ARCHITECTURE: host the full embedded ROCK engine in-process. ROCK settings share this Heisenberg_F4VR.ini under [PhysicsInteraction].");

        // [ObjectPickup] hand-authority / input keys (read in Load(); serialize too).
        ini.SetLongValue  ("ObjectPickup", "iHandAuthorityApplyMode",      handAuthorityApplyMode,         "; hand authority: 1=FRIK-goal seam (FRIK's own arm solver carries the hand, v5-equivalent; auto-degrades), 0=legacy post-FRIK bone IK");
        ini.SetBoolValue  ("ObjectPickup", "bHandAuthorityGoalHookLog",    handAuthorityGoalHookLog,       "; per-apply [FRIK-GOAL] debug logging");
        ini.SetBoolValue  ("VRInput",      "bSuppressThumbstickTouch",      suppressThumbstickTouch,        "; strip thumbstick capacitive-touch bit (stops FRIK thumb twitch on stick edge-touch)");
        ini.SetBoolValue  ("VRInput",      "bPostLoadInputRecovery",        postLoadInputRecovery,          "; after a save load, log the engine input-enable layers ([INPUT-DIAG]) and re-assert movement (fixes 'cannot move or jump but can turn' after loading some saves)");
        ini.SetBoolValue  ("Pipboy",       "bPipboyBootOpensContent",       pipboyBootOpensContent,         "; after the Vault 111 boot sequence, auto-open the Pip-Boy on STATS like vanilla flat. OFF by default: that forced-open Pip-Boy cannot be closed with grip (black screen, stuck open)");
        ini.SetLongValue  ("ObjectPickup", "iTwoHandedFingerPoseMode",      twoHandedFingerPoseMode,        "; support-hand fingers during two-handed grip: 0=off, 1=ROCK grip pose, 2=geometry solve vs weapon mesh");

        // Round-trip serializers — these keys were LOADED in Config::Load but never written here,
        // so Save()/MCM rewrites silently dropped the user's value (it reverted to default next
        // load). Adding them keeps every loaded setting round-tripping. (Skipped intentionally:
        // bEnableDropToHand is a legacy bool alias shadowed by iDropToHandMode;
        // fLegZoneRadius needs the game->cm conversion and is handled elsewhere.)
        ini.SetBoolValue  ("ItemStorage",  "bEnableStorageZoneWeaponEquip",    enableStorageZoneWeaponEquip,    "; Equip weapon dropped on a storage zone");
        ini.SetBoolValue  ("ObjectPickup", "bEnableAutomaticFingerCurls",      enableAutomaticFingerCurls,      "; Geometry-based finger curl solving");
        ini.SetBoolValue  ("ObjectPickup", "bEnableAutomaticHandPlacement",    enableAutomaticHandPlacement,    "; Geometry-based hand placement on grab");
        ini.SetBoolValue  ("ObjectPickup", "bEnableGrabActors",                enableGrabActors,                "; Allow grabbing actors/ragdolls");
        ini.SetDoubleValue("ObjectPickup", "fFingerAnimCloseSpeed",            fingerAnimCloseSpeed,            "; Finger closing animation speed");
        ini.SetDoubleValue("ObjectPickup", "fFingerAnimOpenSpeed",             fingerAnimOpenSpeed,             "; Finger opening animation speed");
        ini.SetLongValue  ("ObjectPickup", "iFingerPoseMode",                  fingerPoseMode,                  "; 0=FRIK resumes after release, 1=keep override");
        ini.SetBoolValue  ("Pipboy",       "bForceTerminalOnWrist",            forceTerminalOnWrist,            "; Force terminals onto the wrist Pip-Boy");
        ini.SetBoolValue  ("Pipboy",       "bHideAllWandHUD",                  hideAllWandHUD,                  "; Hide all wand HUD elements");
        ini.SetBoolValue  ("Pipboy",       "bHideTerminalExitPrompt",          hideTerminalExitPrompt,          "; Hide the terminal exit prompt");
        ini.SetBoolValue  ("Pipboy",       "bHideWandHUD",                     hideWandHUD,                     "; Hide the wand HUD");
        ini.SetBoolValue  ("Pipboy",       "bHolotapeWristOverrideInProjected", holotapeWristOverrideInProjected, "; Wrist override for holotapes in projected mode");
        ini.SetDoubleValue("Pipboy",       "fTapeDeckPushCloseRadius",         tapeDeckPushCloseRadius,         "; Push-to-close radius for the tape deck");
        ini.SetBoolValue  ("Selection",    "bExtendedGrabRange",               extendedGrabRange,               "; Extended-range raycast selection (currently unused by grab logic)");
    }

    // Seed the MCM settings file with field defaults for any MCM-exposed control it lacks, so the
    // menu and engine agree. Machine-seeded values are tracked separately from user choices:
    // when a later build changes a compiled default, an untouched seed migrates with it, while a
    // value changed by the player loses its seed marker and remains authoritative.
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

        // Field defaults from a freshly-constructed Config. A default-constructed Config
        // holds the conversion-block fields in their ORIGINAL cm units (same as
        // Load's kDefaults), but BuildIni always multiplies by GAME_UNITS_TO_CM expecting
        // a post-Load, already-in-game-units Config - so convert cm->game units first,
        // the same step Load() applies, or the seeded defaults come out 1.4x too large.
        Config def;
        def.ApplyCmToGameUnitsConversion();
        CSimpleIniA defIni;
        defIni.SetUnicode();
        def.BuildIni(&defIni);

        CSimpleIniA mcm;
        mcm.SetUnicode();
        mcm.LoadFile(kMCMSettingsPath);  // ok if absent

        constexpr const char* kSeedMetadataSection = "__HeisenbergSeededDefaults";
        int seeded = 0;
        int migrated = 0;
        int released = 0;
        bool changed = false;

        // One-time migration for the old seeder's unmistakable paired fingerprint:
        // natural grab OFF plus 14 cm (the historical 10 cm default multiplied by
        // GAME_UNITS_TO_CM a second time). Requiring both values, and no provenance
        // marker, avoids treating an arbitrary single user choice as generated state.
        // Once migrated, the normal metadata path below owns future default changes.
        const std::string naturalGrabMarker =
            "ObjectPickup.bEnableNaturalGrab";
        const std::string naturalDistanceMarker =
            "ObjectPickup.fNaturalGrabDistance";
        const char* legacyNaturalGrab =
            mcm.GetValue("ObjectPickup", "bEnableNaturalGrab", nullptr);
        const char* legacyNaturalDistance =
            mcm.GetValue("ObjectPickup", "fNaturalGrabDistance", nullptr);
        const bool legacyFingerprintUnmarked =
            legacyNaturalGrab &&
            legacyNaturalDistance &&
            !mcm.GetBoolValue("ObjectPickup", "bEnableNaturalGrab", true) &&
            std::abs(
                mcm.GetDoubleValue(
                    "ObjectPickup",
                    "fNaturalGrabDistance",
                    0.0) -
                14.0) < 0.0001 &&
            !mcm.GetValue(
                kSeedMetadataSection,
                naturalGrabMarker.c_str(),
                nullptr) &&
            !mcm.GetValue(
                kSeedMetadataSection,
                naturalDistanceMarker.c_str(),
                nullptr);
        if (legacyFingerprintUnmarked) {
            std::string naturalGrabDefault =
                defIni.GetValue(
                    "ObjectPickup",
                    "bEnableNaturalGrab",
                    "true");
            if (naturalGrabDefault == "true") naturalGrabDefault = "1";
            else if (naturalGrabDefault == "false") naturalGrabDefault = "0";
            const std::string naturalDistanceDefault =
                defIni.GetValue(
                    "ObjectPickup",
                    "fNaturalGrabDistance",
                    "5.000000");

            mcm.SetValue(
                "ObjectPickup",
                "bEnableNaturalGrab",
                naturalGrabDefault.c_str());
            mcm.SetValue(
                "ObjectPickup",
                "fNaturalGrabDistance",
                naturalDistanceDefault.c_str());
            mcm.SetValue(
                kSeedMetadataSection,
                naturalGrabMarker.c_str(),
                naturalGrabDefault.c_str());
            mcm.SetValue(
                kSeedMetadataSection,
                naturalDistanceMarker.c_str(),
                naturalDistanceDefault.c_str());
            migrated += 2;
            changed = true;
            spdlog::info(
                "[Config] Migrated legacy generated natural-grab fingerprint "
                "(enabled=0, distance=14) to this build's defaults");
        }

        for (const auto& [section, key] : mcmKeys) {
            const char* defVal = defIni.GetValue(section.c_str(), key.c_str(), nullptr);
            if (!defVal) continue;  // control id with no matching serialized setting

            // MCM stores bools as 1/0, not true/false (SetBoolValue's output) — convert so the
            // menu (and F4SE's GetModSettingBool) reads the seeded default correctly.
            std::string v = defVal;
            if (v == "true") v = "1";
            else if (v == "false") v = "0";

            const std::string markerKey = section + "." + key;
            const char* currentValue =
                mcm.GetValue(section.c_str(), key.c_str(), nullptr);
            const char* priorSeed =
                mcm.GetValue(kSeedMetadataSection, markerKey.c_str(), nullptr);

            if (!currentValue) {
                mcm.SetValue(section.c_str(), key.c_str(), v.c_str());
                mcm.SetValue(kSeedMetadataSection, markerKey.c_str(), v.c_str());
                ++seeded;
                changed = true;
                continue;
            }

            if (!priorSeed) {
                continue;  // pre-existing/unmarked value belongs to the player
            }

            if (std::string_view(currentValue) != std::string_view(priorSeed)) {
                // MCM changed the value since we seeded it. Stop tracking it so future
                // compiled-default changes never overwrite the player's choice.
                mcm.Delete(kSeedMetadataSection, markerKey.c_str());
                ++released;
                changed = true;
                continue;
            }

            if (std::string_view(currentValue) != std::string_view(v)) {
                mcm.SetValue(section.c_str(), key.c_str(), v.c_str());
                mcm.SetValue(kSeedMetadataSection, markerKey.c_str(), v.c_str());
                ++migrated;
                changed = true;
            }
        }

        if (changed) {
            mcm.SaveFile(kMCMSettingsPath);
            spdlog::info(
                "[Config] MCM defaults reconciled at {}: seeded={}, migrated={}, userOwned={}",
                kMCMSettingsPath,
                seeded,
                migrated,
                released);
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
