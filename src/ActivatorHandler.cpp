#include "ActivatorHandler.h"
#include "Config.h"
#include "Utils.h"
#include "Physics.h"
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
Elevator Call Button=0x000F7C7C
Elevator Floor Button=0x000F7C7B
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

[Elevator Call Button:000F7C7C]
; Elevator call button - uses default settings
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

[Elevator Floor Button:000F7C7B]
; Elevator floor selection button - uses default settings  
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

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
sTargetNode=

[Call Elevator Button:00065F71]
fActivationRadius=8.0
fPointingRadius=25.0
fZOffset=0.0
sTargetNode=

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
                    // Use configured settings
                    if (actSettings->activationRadius >= 0) {
                        tracked.activationRadius = actSettings->activationRadius;
                    }
                    if (actSettings->pointingRadius >= 0) {
                        tracked.pointingRadius = actSettings->pointingRadius;
                    }
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
                    
                    // Cache target node pointer (avoid recursive search every frame)
                    if (!tracked.targetNodeName.empty()) {
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

    float ActivatorHandler::GetDistanceToActivator(const RE::NiPoint3& fingerPos, const TrackedActivator& activator) const
    {
        RE::TESObjectREFR* actRef = activator.GetRefr();
        if (!actRef) {
            return (std::numeric_limits<float>::max)();
        }

        // Get activator position and rotation for distance calculation
        RE::NiPoint3 actPos;
        RE::NiMatrix3 actRot;
        bool hasRotation = false;

        if (activator.hasCapturedOffset) {
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
        } else if (activator.cachedTargetNode) {
            // No captured offset — use the cached target node's world position and rotation
            actPos = activator.cachedTargetNode->world.translate;
            actRot = activator.cachedTargetNode->world.rotate;
            hasRotation = true;
        } else if (auto* node3D = actRef->Get3D()) {
            actPos = node3D->world.translate;
            actRot = node3D->world.rotate;
            hasRotation = true;
        } else {
            actPos = actRef->GetPosition();
        }

        // Apply Z offset (for buttons that are above/below the node center)
        actPos.z += activator.zOffset;

        // If we have a captured offset, transform it from local to world space and apply
        if (activator.hasCapturedOffset && hasRotation) {
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
            return result;
        }

        // Throttle: only run full proximity scan every 3 frames per hand
        {
            int& frameCount = isLeftHand ? _leftProximityFrame : _rightProximityFrame;
            ProximityResult& cached = isLeftHand ? _cachedLeftResult : _cachedRightResult;
            frameCount++;
            if (frameCount % 3 != 0 && cached.activator != nullptr) {
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
            RE::TESObjectREFR* actRef = activator.GetRefr();
            if (!actRef || !actRef->Get3D()) {
                continue;
            }
            
            float dist = GetDistanceToActivator(fingerTipPos, activator);
            
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
        // Enable hitbox shrink if EITHER hand is in pointing range
        bool shouldShrink = _leftHandInPointingRange || _rightHandInPointingRange;
        
        // Log state changes
        static bool lastState = false;
        if (shouldShrink != lastState) {
            spdlog::debug("[ActivatorHandler] UpdateHitboxShrink: left={} right={} -> shouldShrink={}",
                        _leftHandInPointingRange, _rightHandInPointingRange, shouldShrink);
            lastState = shouldShrink;
        }
        
        SetHitboxShrinkEnabled(shouldShrink);
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
        ProximityResult result = CheckProximity(fingerTipPos, isLeftHand);
        
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
        if (!_useWhitelist || _activatorSettings.empty()) {
            return true;  // No whitelist = everything allowed
        }
        return _activatorSettings.find(baseFormID) != _activatorSettings.end();
    }

    void ActivatorHandler::AddToWhitelist(std::uint32_t baseFormID, const std::string& description)
    {
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
        
        // Disable player collision to let the player body get closer to activatable objects.
        // This is needed because:
        // 1. Player collision keeps the body far from walls/objects
        // 2. When body is far, hand has to reach "too far" from shoulder
        // 3. FRIK's arm IK has a 2.25x arm length limit and stops updating
        // 4. Result: visual arm freezes before reaching the button
        // 
        // By disabling collision, the body can get closer, arm doesn't stretch as far,
        // and FRIK arm IK works normally.
        
        if (enabled) {
            // Disable player collision to let body get closer to activators
            if (Physics::SetPlayerCollisionEnabled(false)) {
                _hitboxShrinkActive = true;
                spdlog::debug("[ActivatorHandler] Player collision DISABLED for activator reach");
            }
        } else {
            // Re-enable player collision
            if (Physics::SetPlayerCollisionEnabled(true)) {
                _hitboxShrinkActive = false;
                spdlog::debug("[ActivatorHandler] Player collision ENABLED (normal)");
            }
        }
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
