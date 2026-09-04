#include "ActivatorHandler.h"
#include "ActivatorHitboxShrinkPolicy.h"
#include "Config.h"
#include "Utils.h"
#include "f4vr/F4VRUtils.h"
#include "SharedUtils.h"

#include <SimpleIni.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace heisenberg
{
    // =========================================================================
    // EMBEDDED DEFAULT ACTIVATOR SETTINGS
    // These are used when no external HeisenbergActivators.ini exists
    // User can create the INI file to override these defaults
    // =========================================================================
    static const char* kDefaultActivatorConfig = R"(; ============================================================================
; HeisenbergActivators.ini - Per-activator settings for touch activation
; ============================================================================
; 
; [Whitelist] section: Add base form IDs to enable whitelist mode
;   Format: Description=0xHEXID
;
; Per-activator sections: [Description:HEXID]
;   fActivationRadius - Distance to trigger activation (game units)
;   fPointingRadius   - Distance to start pointing pose (game units)
;   fZOffset          - Vertical offset for activation point
;   sTargetNode       - Specific child node name to use for distance
;
; Discovery mode will add new activators here when touched
; ============================================================================

[Whitelist]
; Known working activators from Interactive Activators VR
Water Fountain=0x0020DE62
Vault Water Fountain=0x000B1CFE
Vault Sink=0x000C3676
Elevator Panel HiTech=0x000F7C7C
Elevator Call Button HiTech=0x000F7C7B
Big Red Button=0x0005815F
Big Red Button 2=0x0019C656
Power Lift Button Door=0x073037AC
Call Button=0x0013FECE
Circuit Breaker Lid=0x00108651
Elevator Panel=0x0013FECC
Gate=0x00111319
Radio=0x0014507B
Bell=0x000C4387
Call Elevator Button=0x00065F71
Elevator Button=0x00065F72
Port-A-Diner=0x0019F4C6

; === PER-ACTIVATOR SETTINGS ===

[Water Fountain:0020DE62]
; Water fountain - target the button bone instead of root
; fBoneButton is where the actual push button is located
sTargetNode=fBoneButton
fActivationRadius=10.0
fPointingRadius=30.0
fZOffset=0.0

[Vault Water Fountain:000B1CFE]
; Clean Vault 111 fountain uses the same visible push-button bone as the rads variant.
sTargetNode=fBoneButton
fActivationRadius=10.0
fPointingRadius=30.0
fZOffset=0.0

[Vault Sink:000C3676]
; Vault 111 sink activation is located on the tap, well away from the model root.
sTargetNode=fBoneTap
fActivationRadius=10.0
fPointingRadius=30.0
fZOffset=0.0

[Elevator Panel HiTech:000F7C7C]
; Elevator call button - uses default settings
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=Button02

[Elevator Call Button HiTech:000F7C7B]
; Elevator floor selection button - uses default settings  
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=Button01

[Big Red Button:0005815F]
; Red button box - target the actual button, not the base
sTargetNode=b_Button
fActivationRadius=10.0
fPointingRadius=30.0
fZOffset=0.0

[Big Red Button 2:0019C656]
; Another red button box variant - same structure
sTargetNode=b_Button
fActivationRadius=10.0
fPointingRadius=30.0
fZOffset=0.0

[Power Lift Button Door:073037AC]
; Door with button - target the actual button node
sTargetNode=Button01
fActivationRadius=10.0
fPointingRadius=30.0
fZOffset=0.0

[Call Button:0013FECE]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Circuit Breaker Lid:00108651]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Elevator Panel:0013FECC]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Gate:00111319]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Radio:0014507B]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Bell:000C4387]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=b_Bell

[Call Elevator Button:00065F71]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=button

[Elevator Button:00065F72]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Port-A-Diner:0019F4C6]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=
; === VANILLA TOUCH-TARGET SWEEP (auto-generated, adversarially verified) ===
; Every ACTI in Fallout4.esm whose mesh names its pressable part unambiguously.
; sTargetNode aims the touch point at that node - instance-independent, unlike
; a captured world offset. Radii intentionally unset: they follow the global
; fActivatorActivationRadius / fActivatorPointingRadius config values.
; NOT whitelist entries - whitelist membership is only the [Whitelist] section.

[Bell_Free:001244BA]
sTargetNode=Bell

[BoS304PrimeStartupButton:00162A53]
sTargetNode=b_Button

[CircuitBreaker:00108650]
sTargetNode=Lever01

[CircuitBreaker01OLD:0010798E]
sTargetNode=Lever01

[ConButtonStand01:0006D13B]
sTargetNode=button

[ConButtonStand01_NoActivate:00230F00]
sTargetNode=button

[ConButtonStand01Vault111:001A90CA]
sTargetNode=button

[CZLiftButton:0004651E]
sTargetNode=Button01

[DN001_BADTFLLootDoorTerminal:00043B42]
sTargetNode=T01Keyboard008:21

[DN009_SynthGateTerminal:0006D0C8]
sTargetNode=T01Keyboard008:21

[DN015_CleanRoomButton:0009834C]
sTargetNode=b_Button

[DN017_ToggleButtonDoors:0001D89D]
sTargetNode=Button01

[DN017_ToggleButtonRocket:0001D885]
sTargetNode=Button01

[DN029FireCannonActivator:000B810D]
sTargetNode=Handle

[DN041_PowerActivator:000E92B3]
sTargetNode=button

[DN041_StartButton:000E7FDA]
sTargetNode=b_Button

[DN045_TrainButton:0012D13C]
sTargetNode=b_Button

[DN047_CircuitBreaker:0002B0AC]
sTargetNode=Lever01

[DN047TempSensor:0003D70A]
sTargetNode=Handle

[DN049_GenAtomicsGiantHandyLiftButton:00157A8F]
sTargetNode=Button01

[DN050DoorTerminal:00099C20]
sTargetNode=T01Keyboard008:21

[DN070_IDCardReader:0014E09E]
sTargetNode=Keycard01

[DN070_JamaicaPlainButton:000B098A]
sTargetNode=Button01

[DN080_Siren01:001B3192]
sTargetNode=b_Switch

[DN084_InterlockReleaseButton:0014F673]
sTargetNode=b_Button

[DN107_TempButton:000E720B]
sTargetNode=b_Button

[DN130_HamRadioOFF:001E3340]
sTargetNode=Knob

[DN133CastleSpeakerControls:001642F0]
sTargetNode=b_Switch

[Dn136_ElevatorButton:001A8D03]
sTargetNode=Button01

[DN138ElevatorButton:0006AF2A]
sTargetNode=b_Button

[DN138ValveActivator:001157D4]
sTargetNode=Valve

[DN142_Vault114TempDoorTerminal:0005C99A]
sTargetNode=T01Keyboard008:21

[DN143AdminAccessCardReader:001A58AD]
sTargetNode=Keycard01

[DN143LabAccessCardReader:001A58A3]
sTargetNode=Keycard01

[DN143WallTerminal:000BCEEE]
sTargetNode=T01Keyboard008:21

[DN149_WattzUnlockTerminal:000A1CF6]
sTargetNode=T01Keyboard008:21

[DN151_CircuitBreaker:0019DFE4]
sTargetNode=Lever01

[DN151_ToggleButton:000E6AFB]
sTargetNode=Button01

[DN151_WaterDebris:001E7996]
sTargetNode=Lever01

[elevatorbutton01:000A78F5]
sTargetNode=Button01

[ElevatorCallButtonPublic:000E8D88]
sTargetNode=Button01

[ElevatorCallButtonUtility:000A2784]
sTargetNode=Button01

[ElevatorCallButtonVault:00183353]
sTargetNode=VltElevatorCallButton:0

[ElevatorLiftButton01:000B11EA]
sTargetNode=Button02

[ElevatorMaintenanceDoorSwitch:000A285B]
sTargetNode=Handle

[ElevatorPanelPublic:000E8D8C]
sTargetNode=Button02

[ElevatorPanelUtility:000A2785]
sTargetNode=Button02

[ElevatorPanelVault:00183354]
sTargetNode=VltElevatorCarCallButton

[Fusebox01:0001D306]
sTargetNode=Handle

[HamRadio:0018E8FC]
sTargetNode=Knob

[HamRadioOFF:001BA3A0]
sTargetNode=Knob

[IDCardReader01:000998F4]
sTargetNode=Keycard01

[Inst305ReactorButton:000AB2D5]
sTargetNode=Handle

[InstituteControlPanelButton01:00062994]
sTargetNode=Button01

[InstituteRelayButton:0016D062]
sTargetNode=b_Button

[InstM03ThermalRegulator:000BA07F]
sTargetNode=Handle

[LoadElevatorCallButtonHiTech:000F7C5C]
sTargetNode=Button01

[LoadElevatorCallButtonPublic:000E8DC9]
sTargetNode=Button01

[LoadElevatorCallButtonUtility:0009DC99]
sTargetNode=Button01

[LoadElevatorPanelHiTech:000F7C5E]
sTargetNode=Button02

[LoadElevatorPanelPublic:000E8DC8]
sTargetNode=Button02

[LoadElevatorPanelUtility:0009C020]
sTargetNode=Button02

[MS02InstallCoupler:00024B03]
sTargetNode=Handle

[MS02SaugusPowerCore:00026F09]
sTargetNode=Handle

[MS02SensorConsole:000250FB]
sTargetNode=Handle

[MS04_KentPrimaryOverride:000328D4]
sTargetNode=Handle

[MS04_KentSearch:000471B8]
sTargetNode=Handle

[MS04_KentSecondaryOverride:000328D6]
sTargetNode=Handle

[MS04TestActivities:0003FECC]
sTargetNode=Handle

[MS09LorenzoCellDoorButton:0004D939]
sTargetNode=b_Button

[MS09LorenzoCellGenerator:0007CB5C]
sTargetNode=Lever01

[MS10_Intercom:0006AAD7]
sTargetNode=Handle

[MS11_FinalCircuitBreaker:0003ADD2]
sTargetNode=Handle

[MS11_TestCannons:0003ADD0]
sTargetNode=Handle

[MS11_TestComponents:0003ADCC]
sTargetNode=Handle

[MS17_HiddenHQIntercom:00039FDA]
sTargetNode=Handle

[MS17_KidnapClue:00039FD7]
sTargetNode=Handle

[MS19_Intercom:00079217]
sTargetNode=Handle

[NatDoorSmSwitch01:001B5A3B]
sTargetNode=Wheel01

[nauticalswitch01:000BB7F7]
sTargetNode=SwitchHelper01

[PaintMixer01:001F3F1F]
sTargetNode=Handle01

[PfbBarredDoorChains01:0006D133]
sTargetNode=Chains

[PfbBarredDoorChainsDbl01:00022210]
sTargetNode=Chains

[PlayerHouse_KitchenSink01Activator:000F5D8D]
sTargetNode=fBoneTap

[PlayerHouse_Ruin_KitchenSink01Activator:001A4AA4]
sTargetNode=fBoneTap

[PowerLiftButton01:000EEFBD]
sTargetNode=Button01

[RelayTowerButton:0003C626]
sTargetNode=b_Button

[RelayTowerReceiver01:00191EB7]
sTargetNode=Knob

[RelayTowerReceiver02:00191EB8]
sTargetNode=Knob

[RelayTowerReceiver03:00191EB9]
sTargetNode=Knob

[RelayTowerReceiver04:001E5FAB]
sTargetNode=Knob

[RelayTowerReceiver05:001E5FAC]
sTargetNode=Knob

[RelayTowerReceiver06:001E5FAD]
sTargetNode=Knob

[RelayTowerReceiver08:001E5FAE]
sTargetNode=Knob

[RelayTowerReceiver09:001E5FAA]
sTargetNode=Knob

[RelayTowerReceiver10:001E5FAF]
sTargetNode=Knob

[RelayTowerReceiver12:001E5FB0]
sTargetNode=Knob

[RelayTowerReceiver13:001E5FB1]
sTargetNode=Knob

[RelayTowerReceiver14:001E5FA9]
sTargetNode=Knob

[RelayTowerReceiver15:001E5FB2]
sTargetNode=Knob

[RelayTowerReceiver16:001E5FB3]
sTargetNode=Knob

[RelayTowerReceiver17:001E5FB4]
sTargetNode=Knob

[RelayTowerReceiver18:001E5FB5]
sTargetNode=Knob

[RelayTowerReceiver19:001E5FB6]
sTargetNode=Knob

[RR101CenterRing:00108091]
sTargetNode=Button

[RRFreedomTrailMarkerActivator01:0010C3D4]
sTargetNode=Button

[TellMeMoreDisplayAnimated:00173B16]
sTargetNode=Button01

[ToggleButton:00194FB3]
sTargetNode=Button01

[tpSmelterButton:00090BAC]
sTargetNode=b_Button

[V111GearDoorConsole01:00091A48]
sTargetNode=Button

[Valve:001157D8]
sTargetNode=Valve

[VaultControlPanel:0001ED64]
sTargetNode=Handle

[VaultDoorConsole01:00145D93]
sTargetNode=Button

[VaultDoorConsole01NoActivate:0019C899]
sTargetNode=Button

[VltElevatorCallButtonCar:0013C380]
sTargetNode=VltElevatorCarCallButton

[VltElevatorCallButtonExt:0013C37F]
sTargetNode=VltElevatorCallButton:0

[vltoutsideentranceexterior01valve:00092015]
sTargetNode=Valve:5

[WorkshopBell01:000C01D0]
sTargetNode=b_Bell

[WorkshopCircuitBreaker_SI:002404B9]
sTargetNode=Lever01

[WorkshopPowerInteriorFuseboxNoEdit:000CFF77]
sTargetNode=b_Switch

[WorkshopPowerPylonSwitch01:0014583D]
sTargetNode=b_Lever

[WorkshopPowerPylonSwitch02:00138BF8]
sTargetNode=b_Switch

[WorkshopPowerSource_SI:002404BB]
sTargetNode=Lever01

[WorkshopPowerSwitchbox01:00138BF4]
sTargetNode=b_Switch

[WorkshopRadioBeacon:0002A193]
sTargetNode=b_Switch

[WorkshopSiren01:000C4444]
sTargetNode=b_Switch
)";

    // Embedded captured activation offsets — shipped with the mod so no JSON files needed
    // Water Fountain excluded (uses target node fBoneButton instead)
    struct EmbeddedCapture {
        std::uint32_t baseFormID;
        float offsetX, offsetY, offsetZ;
        const char* description;
    };

    static constexpr EmbeddedCapture kEmbeddedCaptures[] = {
        { 0x0013FECE,  89.67404175f, -74.89656830f,  98.69071960f, "Call Button" },
        { 0x00108651,  18.09272575f,   3.49605846f,   3.72173786f, "Circuit Breaker Lid" },
        { 0x0013FECC, -83.33186340f,  75.31886292f,  96.89434814f, "Elevator Panel" },
        { 0x00111319, -60.09521866f,  48.64046097f,  81.41055298f, "Gate" },
        { 0x0014507B, -11.31639290f,  11.13917351f,   6.64483643f, "Radio" },
        { 0x000C4387, -28.44641113f,   4.88256836f, -16.46571350f, "Bell" },
        { 0x00065F71,  17.03657150f,   1.40475821f,  72.77709961f, "Call Elevator Button" },
        { 0x00065F72,  32.38513184f,  77.00559235f,  94.58129883f, "Elevator Button" },
        { 0x0019F4C6,  59.29364014f, -23.27640343f,  56.07546234f, "Port-A-Diner" },
    };

    void ActivatorHandler::Initialize()
    {
        if (_initialized) {
            return;
        }

        spdlog::debug("[ActivatorHandler] Initializing...");

        // Load settings from main config
        _useWhitelist = g_config.activatorUseWhitelist;
        _activationCooldownMs = g_config.activatorCooldownMs;

        // Load per-activator settings from dedicated INI
        LoadSettingsFromINI();

        // Apply embedded captured offsets (shipped with mod)
        for (const auto& cap : kEmbeddedCaptures) {
            ActivatorSettings& settings = _activatorSettings[cap.baseFormID];
            if (!settings.hasCapturedOffset) {  // Don't overwrite user-captured offsets
                settings.baseFormID = cap.baseFormID;
                settings.hasCapturedOffset = true;
                settings.capturedOffsetX = cap.offsetX;
                settings.capturedOffsetY = cap.offsetY;
                settings.capturedOffsetZ = cap.offsetZ;
                if (settings.description.empty()) {
                    settings.description = cap.description;
                }
                if (settings.activationRadius < 0) settings.activationRadius = 8.0f;
                if (settings.pointingRadius < 0) settings.pointingRadius = 25.0f;
            }
        }

        // Load user-captured activation points from JSON files (override embedded)
        LoadCapturedActivatorOffsets();
        
        _initialized = true;
        spdlog::debug("[ActivatorHandler] Initialized (whitelist={}, {} activator settings)",
            _useWhitelist, _activatorSettings.size());
    }

    void ActivatorHandler::LoadSettingsFromINI()
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        
        // STEP 1: Load embedded defaults first (always available)
        SI_Error rc = ini.LoadData(kDefaultActivatorConfig, strlen(kDefaultActivatorConfig));
        if (rc < 0) {
            spdlog::error("[ActivatorHandler] Failed to load embedded default activator config!");
            return;
        }
        spdlog::debug("[ActivatorHandler] Loaded embedded default activator settings");
        
        // STEP 2: Try to load external INI file as override (optional)
        CSimpleIniA externalIni;
        externalIni.SetUnicode();
        rc = externalIni.LoadFile(ACTIVATOR_INI_PATH);
        if (rc >= 0) {
            spdlog::debug("[ActivatorHandler] Found external INI: {} - applying overrides", ACTIVATOR_INI_PATH);
            
            // Merge external settings into ini (external values override defaults)
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
            spdlog::debug("[ActivatorHandler] No external INI file - using embedded defaults");
        }
        
        // Load whitelist from [Whitelist] section
        CSimpleIniA::TNamesDepend keys;
        if (ini.GetAllKeys("Whitelist", keys)) {
            for (const auto& key : keys) {
                const char* value = ini.GetValue("Whitelist", key.pItem, "");
                if (value && *value) {
                    std::uint32_t formID = ParseHexFormID(value);
                    if (formID != 0) {
                        // Add with default settings
                        ActivatorSettings settings;
                        settings.baseFormID = formID;
                        settings.description = key.pItem;
                        _activatorSettings[formID] = settings;
                        _whitelist.insert(formID);
                        spdlog::debug("[ActivatorHandler] Whitelist: {:08X} '{}'", formID, key.pItem);
                    }
                }
            }
        }
        
        // Load per-activator settings from sections with format [Description:HEXID]
        // Examples: [Elevator Button:0020DE62], [Water Fountain:000F7C7C]
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        
        for (const auto& section : sections) {
            std::string sectionName = section.pItem;
            
            // Skip non-activator sections
            if (sectionName == "Whitelist" || sectionName == "General") {
                continue;
            }
            
            // Check if section contains a colon (format: [Name:HEXID])
            size_t colonPos = sectionName.rfind(':');
            if (colonPos != std::string::npos && colonPos < sectionName.length() - 1) {
                std::string formIDStr = sectionName.substr(colonPos + 1);
                std::uint32_t formID = ParseHexFormID(formIDStr);
                
                if (formID != 0) {
                    // Get or create settings for this activator
                    ActivatorSettings& settings = _activatorSettings[formID];
                    settings.baseFormID = formID;
                    settings.description = sectionName.substr(0, colonPos);
                    
                    // Load per-activator overrides
                    double actRadius = ini.GetDoubleValue(section.pItem, "fActivationRadius", -1.0);
                    if (actRadius >= 0) {
                        settings.activationRadius = static_cast<float>(actRadius);
                    }
                    
                    double pntRadius = ini.GetDoubleValue(section.pItem, "fPointingRadius", -1.0);
                    if (pntRadius >= 0) {
                        settings.pointingRadius = static_cast<float>(pntRadius);
                    }
                    
                    double zOff = ini.GetDoubleValue(section.pItem, "fZOffset", 0.0);
                    settings.zOffset = static_cast<float>(zOff);
                    
                    const char* targetNode = ini.GetValue(section.pItem, "sTargetNode", "");
                    if (targetNode && *targetNode) {
                        settings.targetNode = targetNode;
                    }
                    
                    spdlog::debug("[ActivatorHandler] [{}] {:08X}: actRadius={:.1f}, pntRadius={:.1f}, zOff={:.1f}, targetNode='{}'",
                        settings.description, formID, settings.activationRadius, settings.pointingRadius, 
                        settings.zOffset, settings.targetNode);
                }
            }
        }
        
        spdlog::debug("[ActivatorHandler] Loaded {} custom activator settings from INI", _activatorSettings.size());
    }

    void ActivatorHandler::LoadCapturedActivatorOffsets()
    {
        // Load captured activation points from JSON files in Data/F4SE/Plugins/ActivatorCaptures/
        const std::string captureDir = "Data/F4SE/Plugins/ActivatorCaptures";
        
        if (!std::filesystem::exists(captureDir)) {
            spdlog::debug("[ActivatorHandler] No ActivatorCaptures directory found");
            return;
        }
        
        int loadedCount = 0;
        
        for (const auto& entry : std::filesystem::directory_iterator(captureDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            
            try {
                std::ifstream file(entry.path());
                if (!file.is_open()) continue;
                
                nlohmann::json j;
                file >> j;
                file.close();
                
                // Parse the JSON
                if (!j.contains("activator") || !j.contains("capturedPosition")) {
                    spdlog::warn("[ActivatorHandler] Invalid JSON format in {}", entry.path().string());
                    continue;
                }
                
                // Get base form ID
                std::string baseFormIDStr = j["activator"]["baseFormID"].get<std::string>();
                std::uint32_t baseFormID = ParseHexFormID(baseFormIDStr);
                if (baseFormID == 0) {
                    spdlog::warn("[ActivatorHandler] Invalid baseFormID in {}", entry.path().string());
                    continue;
                }
                
                // Get captured position
                float offsetX = j["capturedPosition"]["x"].get<float>();
                float offsetY = j["capturedPosition"]["y"].get<float>();
                float offsetZ = j["capturedPosition"]["z"].get<float>();
                
                // Get or create settings for this activator
                ActivatorSettings& settings = _activatorSettings[baseFormID];
                settings.baseFormID = baseFormID;
                settings.hasCapturedOffset = true;
                settings.capturedOffsetX = offsetX;
                settings.capturedOffsetY = offsetY;
                settings.capturedOffsetZ = offsetZ;
                
                // Set description from JSON if not already set
                if (settings.description.empty() && j["activator"].contains("name")) {
                    settings.description = j["activator"]["name"].get<std::string>();
                }
                
                // Use smaller activation radius for captured points (more precise)
                if (settings.activationRadius < 0) {
                    settings.activationRadius = 8.0f;  // 8 units = ~8cm precision
                }
                if (settings.pointingRadius < 0) {
                    settings.pointingRadius = 25.0f;
                }
                
                loadedCount++;
                spdlog::debug("[ActivatorHandler] Loaded capture for '{:08X}' '{}': offset=({:.2f}, {:.2f}, {:.2f})",
                    baseFormID, settings.description, offsetX, offsetY, offsetZ);
                
            } catch (const std::exception& e) {
                spdlog::error("[ActivatorHandler] Error loading {}: {}", entry.path().string(), e.what());
            }
        }
        
        if (loadedCount > 0) {
            spdlog::debug("[ActivatorHandler] Loaded {} captured activator offsets from JSON files", loadedCount);
        }
    }

    void ActivatorHandler::SaveSettingsToINI()
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        
        // Try to load existing file first (to preserve manual edits)
        ini.LoadFile(ACTIVATOR_INI_PATH);
        
        // Add header comment
        ini.SetValue("General", nullptr, nullptr, 
            "; HeisenbergActivators.ini - Per-activator settings for touch activation\n"
            "; \n"
            "; [Whitelist] section: Add base form IDs to enable whitelist mode\n"
            ";   Format: Description=0xHEXID\n"
            ";\n"
            "; Per-activator sections: [Description:HEXID]\n"
            ";   fActivationRadius - Distance to trigger activation (game units)\n"
            ";   fPointingRadius   - Distance to start pointing pose (game units)\n"
            ";   fZOffset          - Vertical offset for activation point\n"
            ";   sTargetNode       - Specific child node name to use for distance\n"
            ";\n"
            "; Discovery mode will add new activators here when touched\n");
        
        // Write discovered activators
        for (const auto& [formID, settings] : _discoveredActivators) {
            // Skip if already in main settings
            if (_activatorSettings.find(formID) != _activatorSettings.end()) {
                continue;
            }
            
            // Add to whitelist section
            std::stringstream ssKey;
            ssKey << (settings.description.empty() ? "Unknown" : settings.description);
            
            std::stringstream ssValue;
            ssValue << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << formID;
            
            ini.SetValue("Whitelist", ssKey.str().c_str(), ssValue.str().c_str());
            
            // Create a section for this activator with default values
            std::stringstream sectionName;
            sectionName << settings.description << ":" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << formID;
            
            ini.SetDoubleValue(sectionName.str().c_str(), "fActivationRadius", 8.0, 
                "; Distance to trigger activation (game units)");
            ini.SetDoubleValue(sectionName.str().c_str(), "fPointingRadius", 25.0,
                "; Distance to start pointing pose");
            ini.SetValue(sectionName.str().c_str(), "sTargetNode", "",
                "; Specific child node name (see log for available nodes)");
        }
        
        SI_Error rc = ini.SaveFile(ACTIVATOR_INI_PATH);
        if (rc < 0) {
            spdlog::error("[ActivatorHandler] Failed to save INI to '{}'", ACTIVATOR_INI_PATH);
        } else {
            spdlog::debug("[ActivatorHandler] Saved {} discovered activators to '{}'", 
                _discoveredActivators.size(), ACTIVATOR_INI_PATH);
        }
    }

    void ActivatorHandler::Update()
    {
        if (!_initialized) {
            // Auto-initialize if setting was toggled on mid-game via MCM
            Initialize();
            if (!_initialized) return;
            spdlog::debug("[ActivatorHandler] Late-initialized (setting toggled on mid-game)");
        }
        
        // Check for cell change
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        
        auto* currentCell = player->GetParentCell();
        if (currentCell != _currentCell) {
            _currentCell = currentCell;
            ScanCellForActivators();
        }
    }

    void ActivatorHandler::ScanCellForActivators()
    {
        _trackedActivators.clear();
        _trackedTerminals.clear();

        // _cachedLeftResult/_cachedRightResult hold a raw TrackedActivator* into the
        // vector just cleared above; the throttled path in CheckProximity returns that
        // cache on 2 of every 3 calls without checking it against the current vector,
        // so a stale pointer here is a dangling-pointer read/write on the very next
        // throttled frame after a cell change. Reset both, and reset the per-hand
        // pointing-range latches + re-run UpdateHitboxShrink so a cell with zero
        // activators doesn't leave player collision permanently disabled from a
        // hitbox-shrink that was active in the PREVIOUS cell (CheckProximity's own
        // empty-vector early return never reaches that flag update).
        _cachedLeftResult = ProximityResult{};
        _cachedRightResult = ProximityResult{};
        _leftProximityFrame = 2;
        _rightProximityFrame = 2;
        _leftHandInPointingRange = false;
        _rightHandInPointingRange = false;
        UpdateHitboxShrink();

        if (!_currentCell) {
            spdlog::debug("[ActivatorHandler] No current cell");
            return;
        }
        
        spdlog::debug("[ActivatorHandler] Scanning cell for activators...");
        
        int activatorCount = 0;
        int totalRefs = 0;
        
        for (const auto& ref : _currentCell->references) {
            totalRefs++;
            if (!ref) continue;
            
            if (IsValidActivator(ref.get())) {
                TrackedActivator tracked;
                tracked.SetRefr(ref.get());
                // formID is now set by SetRefr
                
                // Get base object info
                const char* editorID = "";
                if (const auto baseObj = ref->GetObjectReference()) {
                    tracked.baseFormID = baseObj->GetFormID();
                    if (baseObj->GetFormEditorID()) {
                        editorID = baseObj->GetFormEditorID();
                    }
                }
                
                tracked.type = DetermineActivatorType(ref.get());
                
                // Doors are always skipped - they have native VR interaction
                if (tracked.type == ActivatorType::Door) {
                    continue;
                }

                // Terminals: track separately for world-screen redirect, skip from main activator list
                if (tracked.type == ActivatorType::Terminal) {
                    _trackedTerminals.push_back(tracked);
                    continue;
                }
                
                // Apply whitelist filter if enabled
                // Activators with captured offsets (profiles) always pass — they should
                // be active regardless of whitelist mode
                const ActivatorSettings* preCheck = GetActivatorSettings(tracked.baseFormID);
                bool hasProfile = preCheck && preCheck->hasCapturedOffset;
                if (!hasProfile && _useWhitelist && !IsWhitelisted(tracked.baseFormID)) {
                    continue;
                }
                
                // Get per-activator settings if configured
                const ActivatorSettings* actSettings = GetActivatorSettings(tracked.baseFormID);
                if (actSettings) {
                    // Use configured settings. Unset radii (-1) fall back to the GLOBAL config
                    // values, not TrackedActivator's hard-coded 8.0/25.0: the embedded sweep
                    // data ships ~120 node-only sections with no radii, and a user tuning
                    // fActivatorActivationRadius in the INI/MCM would otherwise find their
                    // setting applied to unlisted activators but silently ignored for every
                    // listed one. (The numbers coincide today; the ROUTE is what matters.)
                    tracked.activationRadius = actSettings->activationRadius >= 0
                        ? actSettings->activationRadius
                        : g_config.activatorActivationRadius;
                    tracked.pointingRadius = actSettings->pointingRadius >= 0
                        ? actSettings->pointingRadius
                        : g_config.activatorPointingRadius;
                    tracked.zOffset = actSettings->zOffset;
                    tracked.targetNodeName = actSettings->targetNode;
                    
                    // Copy captured offset if available
                    if (actSettings->hasCapturedOffset) {
                        tracked.hasCapturedOffset = true;
                        tracked.capturedOffsetX = actSettings->capturedOffsetX;
                        tracked.capturedOffsetY = actSettings->capturedOffsetY;
                        tracked.capturedOffsetZ = actSettings->capturedOffsetZ;
                        spdlog::debug("[ActivatorHandler] Using captured offset ({:.2f}, {:.2f}, {:.2f}) for {:08X}",
                            tracked.capturedOffsetX, tracked.capturedOffsetY, tracked.capturedOffsetZ, tracked.baseFormID);
                    }
                    
                    // Cache target node pointer (avoid recursive search every frame).
                    // Also remember WHICH 3D root it was resolved against - see
                    // RefreshTargetNodeIfStale for why the pointer alone is not enough.
                    if (!tracked.targetNodeName.empty()) {
                        tracked.cachedTargetRoot = ref->Get3D();
                        tracked.cachedTargetNode.reset(FindNodeRecursive(ref->Get3D(), tracked.targetNodeName));
                        if (tracked.cachedTargetNode) {
                            spdlog::debug("[ActivatorHandler] Found target node '{}' for {:08X}",
                                tracked.targetNodeName, tracked.baseFormID);
                        } else {
                            spdlog::warn("[ActivatorHandler] Target node '{}' NOT FOUND for {:08X}",
                                tracked.targetNodeName, tracked.baseFormID);
                        }
                    }
                } else {
                    // Default settings based on type
                    switch (tracked.type) {
                        case ActivatorType::TwoState:
                            tracked.activationRadius = g_config.activatorActivationRadius;
                            tracked.pointingRadius = g_config.activatorPointingRadius;
                            break;
                        case ActivatorType::Door:
                            tracked.activationRadius = 15.0f;
                            tracked.pointingRadius = 40.0f;
                            break;
                        case ActivatorType::Terminal:
                            tracked.activationRadius = 12.0f;
                            tracked.pointingRadius = 30.0f;
                            break;
                        default:
                            tracked.activationRadius = g_config.activatorActivationRadius;
                            tracked.pointingRadius = g_config.activatorPointingRadius;
                            break;
                    }
                }
                
                _trackedActivators.push_back(tracked);
                activatorCount++;
                
                // Log found activators
                if (g_config.activatorDebugLogging) {
                    const auto pos = ref->GetPosition();
                    spdlog::debug("[ActivatorHandler] [{}] ref={:08X} base={:08X} '{}' at ({:.0f},{:.0f},{:.0f})",
                        activatorCount, tracked.formID, tracked.baseFormID, editorID,
                        pos.x, pos.y, pos.z);
                }
            }
        }
        
        spdlog::debug("[ActivatorHandler] Tracking {} activators in current cell (scanned {} refs)",
            activatorCount, totalRefs);
    }

    bool ActivatorHandler::IsValidActivator(RE::TESObjectREFR* ref) const
    {
        if (!ref || ref->IsDisabled() || ref->IsDeleted()) {
            return false;
        }
        
        const auto baseObj = ref->GetObjectReference();
        if (!baseObj) {
            return false;
        }
        
        // Check if it's an activator type (NOT doors - they have native VR interaction)
        const auto formType = baseObj->GetFormType();
        if (formType == RE::ENUM_FORM_ID::kACTI) {
            return true;
        }
        // Doors excluded - native VR handles them
        // if (formType == RE::ENUM_FORM_ID::kDOOR) { return true; }
        
        return false;
    }

    ActivatorHandler::ActivatorType ActivatorHandler::DetermineActivatorType(RE::TESObjectREFR* ref) const
    {
        if (!ref) {
            return ActivatorType::Generic;
        }
        
        const auto baseObj = ref->GetObjectReference();
        if (!baseObj) {
            return ActivatorType::Generic;
        }
        
        const auto formType = baseObj->GetFormType();
        
        // Doors are easy to identify
        if (formType == RE::ENUM_FORM_ID::kDOOR) {
            return ActivatorType::Door;
        }
        
        // For activators, try to determine subtype
        if (formType == RE::ENUM_FORM_ID::kACTI) {
            // Check editor ID for hints
            const char* editorID = baseObj->GetFormEditorID();
            if (editorID) {
                std::string_view name(editorID);
                
                // Common patterns for buttons/switches
                if (name.find("Button") != std::string_view::npos ||
                    name.find("Switch") != std::string_view::npos ||
                    name.find("Elevator") != std::string_view::npos ||
                    name.find("2State") != std::string_view::npos) {
                    return ActivatorType::TwoState;
                }
                
                // Terminals
                if (name.find("Terminal") != std::string_view::npos ||
                    name.find("Computer") != std::string_view::npos) {
                    return ActivatorType::Terminal;
                }
            }
            
            // Default: treat as two-state (most common interactive activator)
            return ActivatorType::TwoState;
        }
        
        return ActivatorType::Generic;
    }

    RE::NiAVObject* ActivatorHandler::FindNodeRecursive(RE::NiAVObject* root, const std::string& nodeName) const
    {
        return FindNodeByName(root, nodeName);
    }

    void ActivatorHandler::CollectNodeNames(RE::NiAVObject* node, std::vector<std::string>& outNames, int depth) const
    {
        CollectNodeNamesRecursive(node, outNames, depth, /*includeWorldPos=*/false);
    }

    void ActivatorHandler::LogActivatorNodes(RE::TESObjectREFR* ref)
    {
        LogRefNodeTree(ref, "[ActivatorHandler]", /*includeWorldPos=*/false);
    }

    float ActivatorHandler::GetDistanceToActivator(
        const RE::NiPoint3& fingerPos,
        const TrackedActivator& activator,
        RE::TESObjectREFR* resolvedRefr) const
    {
        RE::TESObjectREFR* actRef =
            resolvedRefr ? resolvedRefr : activator.GetRefr();
        if (!actRef) {
            return (std::numeric_limits<float>::max)();
        }

        // Re-resolve a stale target-node cache. It is resolved once per cell scan, which
        // breaks silently in two directions: 3D that streams in AFTER the scan leaves the
        // node null for the whole cell visit (targeting degrades to root-origin), and 3D
        // rebuilt mid-visit (workshop build/scrap, Reset3D) leaves the NiPointer holding a
        // DETACHED node whose world transform never updates again - distances against frozen
        // geometry. Comparing the current root against the one we resolved under catches
        // both; the compare is two pointer reads, the FindNode walk only runs on change.
        if (!activator.targetNodeName.empty()) {
            RE::NiAVObject* curRoot = actRef->Get3D();
            const bool rootChanged = curRoot != activator.cachedTargetRoot;
            const bool unresolved = !activator.cachedTargetNode && curRoot;
            if (rootChanged || unresolved) {
                activator.cachedTargetRoot = curRoot;
                activator.cachedTargetNode.reset(
                    curRoot ? FindNodeByName(curRoot, activator.targetNodeName) : nullptr);
            }
        }
        // Get activator position and rotation for distance calculation
        RE::NiPoint3 actPos;
        RE::NiMatrix3 actRot;
        bool hasRotation = false;

        // PRECEDENCE: a RESOLVED target node beats a captured offset. The node is the mesh
        // author's own name for the pressable part, correct for every placement of that mesh;
        // a capture is a hand-measured world offset authored against ONE instance, and it can
        // be wrong for another (live case: ConButtonStand01ElevatorCaller 00065F71 carries an
        // embedded capture at Z+72.8 that misses the owner's elevator entirely, while the mesh
        // names its pressable node 'button' outright). The capture remains the authority when
        // no target node is configured or the node fails to resolve on this instance - the
        // node pointer being null is exactly the "capture knows better" case.
        bool usedTargetNode = false;
        if (activator.cachedTargetNode) {
            actPos = activator.cachedTargetNode->world.translate;
            actRot = activator.cachedTargetNode->world.rotate;
            hasRotation = true;
            usedTargetNode = true;
        } else if (activator.hasCapturedOffset) {
            // Captured offsets were computed relative to the ROOT node during capture
            // (NodeCaptureMode::PerformCapture uses root3D->world.rotate for inverse transform)
            // MUST use root node here too, not a target child node — child nodes have their
            // own local rotation which would corrupt the offset for different placements
            if (auto* node3D = actRef->Get3D()) {
                actPos = node3D->world.translate;
                actRot = node3D->world.rotate;
                hasRotation = true;
            } else {
                actPos = actRef->GetPosition();
            }
        } else if (auto* node3D = actRef->Get3D()) {
            actPos = node3D->world.translate;
            actRot = node3D->world.rotate;
            hasRotation = true;
        } else {
            actPos = actRef->GetPosition();
        }

        // Apply Z offset (for buttons that are above/below the node center)
        actPos.z += activator.zOffset;

        // If we have a captured offset, transform it from local to world space and apply.
        // Never on top of a target node - the offset is root-relative and the node position
        // already IS the activation point; adding both lands somewhere meaningless.
        if (activator.hasCapturedOffset && hasRotation && !usedTargetNode) {
            // Transform local offset to world space using root node's rotation
            RE::NiPoint3 localOffset(activator.capturedOffsetX, activator.capturedOffsetY, activator.capturedOffsetZ);
            RE::NiPoint3 worldOffset = actRot * localOffset;

            actPos.x += worldOffset.x;
            actPos.y += worldOffset.y;
            actPos.z += worldOffset.z;
        }
        
        // Calculate distance
        float dx = fingerPos.x - actPos.x;
        float dy = fingerPos.y - actPos.y;
        float dz = fingerPos.z - actPos.z;
        
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    ActivatorHandler::ProximityResult ActivatorHandler::CheckProximity(const RE::NiPoint3& fingerTipPos, bool isLeftHand, float handSpeed)
    {
        ProximityResult result;

        if (_trackedActivators.empty()) {
            // Still update the pointing-range latch + hitbox shrink for this hand even
            // with nothing tracked, so a hand that WAS in range before the list emptied
            // (cell change, whitelist filtering) doesn't leave collision disabled forever.
            if (isLeftHand) {
                _leftHandInPointingRange = false;
            } else {
                _rightHandInPointingRange = false;
            }
            UpdateHitboxShrink();
            (isLeftHand ? _cachedLeftResult : _cachedRightResult) = result;
            return result;
        }

        // Throttle: only run the full cell-wide proximity scan every 3 frames
        // per hand. The old condition returned the cache only when it already
        // named an activator, so the common "nothing nearby" case defeated the
        // throttle and scanned every tracked activator for both hands on every
        // frame.
        {
            int& frameCount = isLeftHand ? _leftProximityFrame : _rightProximityFrame;
            ProximityResult& cached = isLeftHand ? _cachedLeftResult : _cachedRightResult;
            frameCount++;
            if (frameCount % 3 != 0) {
                return cached;
            }
        }
        
        // Scale pointing radius based on hand speed
        // At 0 speed: use base radius. At high speed: extend up to 2x base radius
        // This gives fast-moving hands more time to transition to pointing pose
        // handSpeed is in game units/sec, threshold around 50-100 is "fast" movement
        constexpr float kSpeedThreshold = 50.0f;   // Speed at which we start extending
        constexpr float kMaxSpeedBonus = 100.0f;   // Speed at which we reach max extension
        constexpr float kMaxRadiusMultiplier = 2.0f; // Maximum multiplier (2x base radius)
        
        float speedFactor = 1.0f;
        if (handSpeed > kSpeedThreshold) {
            float speedOverThreshold = handSpeed - kSpeedThreshold;
            float normalizedSpeed = (std::min)(speedOverThreshold / (kMaxSpeedBonus - kSpeedThreshold), 1.0f);
            speedFactor = 1.0f + normalizedSpeed * (kMaxRadiusMultiplier - 1.0f);
        }
        
        float closestDist = (std::numeric_limits<float>::max)();
        TrackedActivator* closestActivator = nullptr;
        
        for (auto& activator : _trackedActivators) {
            // Resolve the handle once and retain its NiPointer for the full
            // distance calculation. The previous path resolved it here and a
            // second time inside GetDistanceToActivator for every tracked ACTI.
            RE::NiPointer<RE::TESObjectREFR> actRefPtr =
                activator.refrHandle.get();
            RE::TESObjectREFR* actRef = actRefPtr.get();
            if (!actRef || !actRef->Get3D()) {
                // Clear the in-range flags before skipping. Leaving them alone lets a TRUE
                // survive from the last frame the activator was measurable - so an activator
                // the player stood next to, then walked away from until its 3D unloaded,
                // keeps reporting "hand in range" indefinitely. Anything reading these flags
                // (prompt text, pose decisions) then acts on proximity to an object that is
                // not even loaded.
                if (isLeftHand) {
                    activator.isLeftHandInRange = false;
                } else {
                    activator.isRightHandInRange = false;
                }
                continue;
            }
            
            float dist =
                GetDistanceToActivator(
                    fingerTipPos,
                    activator,
                    actRef);
            
            // Apply speed-scaled pointing radius for fast-moving hands
            float scaledPointingRadius = activator.pointingRadius * speedFactor;
            
            // Update hand-in-range state
            if (isLeftHand) {
                activator.isLeftHandInRange = (dist < scaledPointingRadius);
            } else {
                activator.isRightHandInRange = (dist < scaledPointingRadius);
            }
            
            // Track closest activator
            if (dist < closestDist) {
                closestDist = dist;
                closestActivator = &activator;
            }
        }
        
        if (closestActivator) {
            result.activator = closestActivator;
            result.distance = closestDist;
            // Use speed-scaled pointing radius for fast-moving hands
            float scaledPointingRadius = closestActivator->pointingRadius * speedFactor;
            result.inPointingRange = (closestDist < scaledPointingRadius);
            result.inActivationRange = (closestDist < closestActivator->activationRadius);
        }
        
        // Track per-hand pointing state for hitbox shrink management
        if (isLeftHand) {
            _leftHandInPointingRange = result.inPointingRange;
        } else {
            _rightHandInPointingRange = result.inPointingRange;
        }
        
        // Automatically update hitbox shrink based on either hand being in range
        UpdateHitboxShrink();

        // Cache result for throttled frames
        (isLeftHand ? _cachedLeftResult : _cachedRightResult) = result;

        return result;
    }
    
    void ActivatorHandler::UpdateHitboxShrink()
    {
        // Track activator-reach mode if EITHER hand is in pointing range. This
        // state still drives the existing pointing/IK flow, but must never alter
        // the collision layer of the player's whole Havok body.
        const bool shouldShrink =
            activator_hitbox_shrink_policy::shouldShrink(
                activator_hitbox_shrink_policy::PointingState{
                    _leftHandInPointingRange,
                    _rightHandInPointingRange,
                });
        
        // Log state changes
        static bool lastState = false;
        if (shouldShrink != lastState) {
            spdlog::debug("[ActivatorHandler] UpdateHitboxShrink: left={} right={} -> shouldShrink={}",
                        _leftHandInPointingRange, _rightHandInPointingRange, shouldShrink);
            lastState = shouldShrink;
        }
        
        SetHitboxShrinkEnabled(shouldShrink);
    }

    void ActivatorHandler::ClearHandPointingState(const bool isLeftHand)
    {
        const auto cleared =
            activator_hitbox_shrink_policy::clearHand(
                activator_hitbox_shrink_policy::PointingState{
                    _leftHandInPointingRange,
                    _rightHandInPointingRange,
                },
                isLeftHand);
        _leftHandInPointingRange = cleared.left;
        _rightHandInPointingRange = cleared.right;

        // Always reconcile, even if this latch was already false. This repairs
        // split-state paths where both latches were clear while the cached reach
        // mode remained set.
        UpdateHitboxShrink();
    }

    bool ActivatorHandler::CanActivate(const TrackedActivator& activator) const
    {
        // Check cooldown
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - activator.lastActivationTime).count();
        
        if (elapsed < _activationCooldownMs) {
            return false;
        }
        
        // Check if ref is still valid
        RE::TESObjectREFR* actRef = activator.GetRefr();
        if (!actRef || actRef->IsDisabled() || actRef->IsDeleted()) {
            return false;
        }
        
        return true;
    }

    void ActivatorHandler::ActivateObject(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return;
        }
        
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        
        std::uint32_t baseFormID = 0;
        const char* editorID = "";
        if (const auto baseObj = ref->GetObjectReference()) {
            baseFormID = baseObj->GetFormID();
            if (baseObj->GetFormEditorID()) {
                editorID = baseObj->GetFormEditorID();
            }
        }
        
        spdlog::debug("[ActivatorHandler] Activating ref={:08X} base={:08X} '{}'", 
            ref->GetFormID(), baseFormID, editorID);
        
        // Perform the activation
        ref->ActivateRef(player, nullptr, 1, false, false, false);
    }

    bool ActivatorHandler::TryActivate(const RE::NiPoint3& fingerTipPos, bool isLeftHand)
    {
        return TryActivate(CheckProximity(fingerTipPos, isLeftHand));
    }

    bool ActivatorHandler::TryActivate(const ProximityResult& result)
    {
        if (result.inActivationRange && result.activator && CanActivate(*result.activator)) {
            ActivateObject(result.activator->GetRefr());
            result.activator->lastActivationTime = std::chrono::steady_clock::now();
            return true;
        }
        
        return false;
    }

    const ActivatorSettings* ActivatorHandler::GetActivatorSettings(std::uint32_t baseFormID) const
    {
        auto it = _activatorSettings.find(baseFormID);
        if (it != _activatorSettings.end()) {
            return &it->second;
        }
        return nullptr;
    }

    bool ActivatorHandler::IsWhitelisted(std::uint32_t baseFormID) const
    {
        // Membership means the [Whitelist] SECTION, not mere presence in _activatorSettings.
        // The settings map now carries ~120 embedded sTargetNode entries whose only job is to
        // aim the touch point better on activators that are tracked anyway - treating those as
        // whitelist rows silently widened opt-in whitelist mode from ~17 curated forms to all
        // of them, making the mode meaningless the moment the sweep data landed.
        if (!_useWhitelist || _whitelist.empty()) {
            return true;  // No whitelist = everything allowed
        }
        return _whitelist.find(baseFormID) != _whitelist.end();
    }

    void ActivatorHandler::AddToWhitelist(std::uint32_t baseFormID, const std::string& description)
    {
        _whitelist.insert(baseFormID);
        ActivatorSettings settings;
        settings.baseFormID = baseFormID;
        settings.description = description;
        _activatorSettings[baseFormID] = settings;
    }

    void ActivatorHandler::RemoveFromWhitelist(std::uint32_t baseFormID)
    {
        _activatorSettings.erase(baseFormID);
    }

    void ActivatorHandler::RegisterCapturedOffset(std::uint32_t baseFormID, const std::string& description,
                                                   float offsetX, float offsetY, float offsetZ)
    {
        // Add or update settings
        ActivatorSettings& settings = _activatorSettings[baseFormID];
        settings.baseFormID = baseFormID;
        settings.description = description;
        settings.hasCapturedOffset = true;
        settings.capturedOffsetX = offsetX;
        settings.capturedOffsetY = offsetY;
        settings.capturedOffsetZ = offsetZ;
        
        // Set reasonable defaults if not already set
        if (settings.activationRadius < 0) {
            settings.activationRadius = 8.0f;
        }
        if (settings.pointingRadius < 0) {
            settings.pointingRadius = 25.0f;
        }
        
        spdlog::debug("[ActivatorHandler] Registered capture for {:08X} '{}': offset=({:.2f}, {:.2f}, {:.2f})",
            baseFormID, description, offsetX, offsetY, offsetZ);
        
        // Update any existing tracked activators with this base form ID
        for (auto& tracked : _trackedActivators) {
            if (tracked.baseFormID == baseFormID) {
                tracked.hasCapturedOffset = true;
                tracked.capturedOffsetX = offsetX;
                tracked.capturedOffsetY = offsetY;
                tracked.capturedOffsetZ = offsetZ;
                tracked.activationRadius = settings.activationRadius;
                tracked.pointingRadius = settings.pointingRadius;
                spdlog::debug("[ActivatorHandler] Updated existing tracked activator {:08X} with new offset", baseFormID);
            }
        }
    }

    // =========================================================================
    // NODE CAPTURE MODE
    // Find the closest child node to the finger tip position and save it
    // =========================================================================
    
    // Helper: recursively find the closest node to a position
    static void FindClosestNodeRecursive(
        RE::NiAVObject* node, 
        const RE::NiPoint3& targetPos,
        RE::NiAVObject*& closestNode,
        float& closestDist,
        int depth = 0)
    {
        if (!node || depth > 15) return;  // Limit depth
        
        // Check this node's distance
        RE::NiPoint3 nodePos = node->world.translate;
        float dx = targetPos.x - nodePos.x;
        float dy = targetPos.y - nodePos.y;
        float dz = targetPos.z - nodePos.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        
        // Only consider named nodes (skip unnamed ones)
        if (node->name.c_str() && node->name.length() > 0) {
            if (dist < closestDist) {
                closestDist = dist;
                closestNode = node;
            }
        }
        
        // Recurse into children
        if (auto* niNode = node->IsNode()) {
            for (auto& child : niNode->children) {
                if (child) {
                    FindClosestNodeRecursive(child.get(), targetPos, closestNode, closestDist, depth + 1);
                }
            }
        }
    }

    // Helper: collect all nodes with their positions and distances
    struct NodeInfo {
        std::string name;
        float worldX, worldY, worldZ;
        float distanceToFinger;
        int depth;
    };
    
    static void CollectAllNodesRecursive(
        RE::NiAVObject* node,
        const RE::NiPoint3& fingerPos,
        std::vector<NodeInfo>& nodes,
        int depth = 0)
    {
        if (!node || depth > 15) return;
        
        // Add this node if it has a name
        if (node->name.c_str() && node->name.length() > 0) {
            NodeInfo info;
            info.name = node->name.c_str();
            info.worldX = node->world.translate.x;
            info.worldY = node->world.translate.y;
            info.worldZ = node->world.translate.z;
            
            float dx = fingerPos.x - info.worldX;
            float dy = fingerPos.y - info.worldY;
            float dz = fingerPos.z - info.worldZ;
            info.distanceToFinger = std::sqrt(dx * dx + dy * dy + dz * dz);
            info.depth = depth;
            
            nodes.push_back(info);
        }
        
        // Recurse into children
        if (auto* niNode = node->IsNode()) {
            for (auto& child : niNode->children) {
                if (child) {
                    CollectAllNodesRecursive(child.get(), fingerPos, nodes, depth + 1);
                }
            }
        }
    }

    ActivatorHandler::NodeCaptureResult ActivatorHandler::CaptureTargetNode(const RE::NiPoint3& fingerTipPos)
    {
        NodeCaptureResult result;
        
        if (_trackedActivators.empty()) {
            spdlog::warn("[ActivatorHandler] No tracked activators - cannot capture node");
            return result;
        }
        
        // Find the closest activator to the finger
        TrackedActivator* closestActivator = nullptr;
        float closestActivatorDist = 100.0f;  // Max search distance (100 units = ~1m)
        
        for (auto& activator : _trackedActivators) {
            float dist = GetDistanceToActivator(fingerTipPos, activator);
            if (dist < closestActivatorDist) {
                closestActivatorDist = dist;
                closestActivator = &activator;
            }
        }
        
        if (!closestActivator) {
            spdlog::warn("[ActivatorHandler] No activator within range for node capture");
            return result;
        }
        
        RE::TESObjectREFR* actRef = closestActivator->GetRefr();
        if (!actRef) {
            spdlog::warn("[ActivatorHandler] Activator reference is null");
            return result;
        }
        
        RE::NiAVObject* root3D = actRef->Get3D();
        if (!root3D) {
            spdlog::warn("[ActivatorHandler] Activator has no 3D");
            return result;
        }
        
        // Get activator info
        std::uint32_t baseFormID = closestActivator->baseFormID;
        std::uint32_t refFormID = closestActivator->formID;
        const char* editorID = "";
        std::string displayName = "Unknown";
        
        if (const auto baseObj = actRef->GetObjectReference()) {
            if (baseObj->GetFormEditorID()) {
                editorID = baseObj->GetFormEditorID();
            }
            // Get the human-readable display name (e.g., "Bell", "Elevator Button")
            const char* fullName = actRef->GetDisplayFullName();
            if (fullName && fullName[0] != '\0') {
                displayName = fullName;
            } else if (editorID && editorID[0] != '\0') {
                displayName = editorID;  // Fallback to editor ID
            }
        }
        
        // Collect ALL nodes with distances
        std::vector<NodeInfo> allNodes;
        CollectAllNodesRecursive(root3D, fingerTipPos, allNodes);
        
        if (allNodes.empty()) {
            spdlog::warn("[ActivatorHandler] No nodes found in activator");
            return result;
        }
        
        // Find the closest node
        NodeInfo* closestNodeInfo = &allNodes[0];
        for (auto& node : allNodes) {
            if (node.distanceToFinger < closestNodeInfo->distanceToFinger) {
                closestNodeInfo = &node;
            }
        }
        
        std::string closestNodeName = closestNodeInfo->name;
        float closestNodeDist = closestNodeInfo->distanceToFinger;
        
        spdlog::debug("[ActivatorHandler] CAPTURED NODE: '{}' for activator {:08X} '{}' (dist={:.1f})",
            closestNodeName, baseFormID, editorID, closestNodeDist);
        
        // =====================================================================
        // Save to JSON file
        // =====================================================================
        try {
            nlohmann::json j;
            
            // Activator info
            std::stringstream baseFormIDStr, refFormIDStr;
            baseFormIDStr << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << baseFormID;
            refFormIDStr << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << refFormID;
            
            j["activator"]["baseFormID"] = baseFormIDStr.str();
            j["activator"]["refFormID"] = refFormIDStr.str();
            j["activator"]["editorID"] = editorID ? editorID : "";
            j["activator"]["displayName"] = displayName;
            j["activator"]["distanceFromFinger"] = closestActivatorDist;
            
            // Finger position
            j["fingerPosition"]["x"] = fingerTipPos.x;
            j["fingerPosition"]["y"] = fingerTipPos.y;
            j["fingerPosition"]["z"] = fingerTipPos.z;
            
            // Selected (closest) node
            j["selectedNode"]["name"] = closestNodeName;
            j["selectedNode"]["distanceToFinger"] = closestNodeDist;
            j["selectedNode"]["worldPosition"]["x"] = closestNodeInfo->worldX;
            j["selectedNode"]["worldPosition"]["y"] = closestNodeInfo->worldY;
            j["selectedNode"]["worldPosition"]["z"] = closestNodeInfo->worldZ;
            
            // All nodes
            j["allNodes"] = nlohmann::json::array();
            for (const auto& node : allNodes) {
                nlohmann::json nodeJ;
                nodeJ["name"] = node.name;
                nodeJ["distanceToFinger"] = node.distanceToFinger;
                nodeJ["depth"] = node.depth;
                nodeJ["worldPosition"]["x"] = node.worldX;
                nodeJ["worldPosition"]["y"] = node.worldY;
                nodeJ["worldPosition"]["z"] = node.worldZ;
                j["allNodes"].push_back(nodeJ);
            }
            
            // Sort allNodes by distance
            std::sort(j["allNodes"].begin(), j["allNodes"].end(),
                [](const nlohmann::json& a, const nlohmann::json& b) {
                    return a["distanceToFinger"].get<float>() < b["distanceToFinger"].get<float>();
                });
            
            // Create sanitized filename from display name
            // Replace invalid filename characters with underscores
            std::string safeFileName = displayName;
            for (char& c : safeFileName) {
                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || 
                    c == '"' || c == '<' || c == '>' || c == '|' || c == ' ') {
                    c = '_';
                }
            }
            
            // Write JSON file with activator name (in NodeCaptures subfolder)
            std::string captureDir = "Data/F4SE/Plugins/NodeCaptures";
            std::string jsonPath = captureDir + "/" + safeFileName + ".json";
            
            // Create directory if it doesn't exist
            std::filesystem::create_directories(captureDir);
            
            std::ofstream outFile(jsonPath, std::ios::out | std::ios::trunc);  // Overwrite existing
            if (outFile.is_open()) {
                outFile << j.dump(2);  // Pretty print with 2-space indent
                outFile.close();
                spdlog::debug("[ActivatorHandler] Saved capture to: {}", jsonPath);
                result.jsonPath = jsonPath;  // Store path in result for user notification
            } else {
                spdlog::error("[ActivatorHandler] Failed to write JSON file: {}", jsonPath);
            }
        } catch (const std::exception& e) {
            spdlog::error("[ActivatorHandler] JSON error: {}", e.what());
        }
        
        spdlog::debug("[ActivatorHandler] JSON save complete");
        
        // Set result and return - JSON file is the output, no need for INI
        result.success = true;
        result.baseFormID = baseFormID;
        result.nodeName = closestNodeName;
        result.activatorName = displayName;
        result.distance = closestNodeDist;
        
        return result;
    }

    void ActivatorHandler::SetHitboxShrinkEnabled(bool enabled)
    {
        if (enabled == _hitboxShrinkActive) {
            return;  // Already in desired state
        }

        // This is deliberately only a logical reach/pointing latch. The old
        // implementation put the complete player collision body on layer 15
        // whenever either fingertip approached an activator. That made bullets,
        // melee and world collision miss the player globally. Pointing pose,
        // activation proximity and arm-goal handling already consume their own
        // per-hand state and do not require changing the player collision layer.
        _hitboxShrinkActive = enabled;
        spdlog::debug(
            "[ActivatorHandler] Activator reach mode {} (player collision unchanged)",
            enabled ? "ENABLED" : "DISABLED");
    }

    ActivatorHandler::TrackedActivator* ActivatorHandler::GetNearestTerminal(const RE::NiPoint3& pos, float maxRange)
    {
        TrackedActivator* nearest = nullptr;
        float nearestDist = maxRange;

        for (auto& terminal : _trackedTerminals) {
            auto* refr = terminal.GetRefr();
            if (!refr) continue;

            auto* node3D = refr->Get3D();
            if (!node3D) continue;

            RE::NiPoint3 diff = pos - node3D->world.translate;
            float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

            if (dist < nearestDist) {
                nearestDist = dist;
                nearest = &terminal;
            }
        }

        return nearest;
    }

} // namespace heisenberg
