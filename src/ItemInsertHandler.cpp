#include "ItemInsertHandler.h"
#include "Config.h"
#include "Utils.h"
#include "Grab.h"
#include "F4VROffsets.h"
#include "WandNodeHelper.h"
#include "f4vr/PlayerNodes.h"
#include "SharedUtils.h"

#include <SimpleIni.h>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace heisenberg
{
    // INI file path for insert zones
    static constexpr const char* INSERT_ZONES_INI_PATH = "Data/F4SE/Plugins/HeisenbergInsertZones.ini";

    void ItemInsertHandler::Initialize()
    {
        if (_initialized) {
            return;
        }
        
        spdlog::info("[ItemInsertHandler] Initializing...");
        
        // Load insert zone definitions from INI
        LoadZonesFromINI();
        
        // If no zones loaded from INI, register default Nuka Fridge zone
        if (_insertZones.empty()) {
            RegisterDefaultZones();
        }
        
        _initialized = true;
        spdlog::info("[ItemInsertHandler] Initialized with {} insert zones", _insertZones.size());
    }

    void ItemInsertHandler::RegisterDefaultZones()
    {
        // ═══════════════════════════════════════════════════════════════════════════
        // NUKA-COLA MACHINE / FRIDGE
        // ═══════════════════════════════════════════════════════════════════════════
        // The Nuka fridge accepts bottle caps in the coin slot
        // Multiple base form IDs observed for Nuka-Cola machines:
        //   0x000302DC - Nuka-Cola Machine (common)
        //   0x001A6E50 - Another variant
        //   0x0020DE62 - Another variant (from earlier tests)
        
        // Common settings for all Nuka machines
        // Coin slot is offset from the "Door" node: X=-57 (left), Y=0 (center), Z=8 (up)
        auto createNukaMachineZone = [](std::uint32_t formID, const char* desc) {
            InsertZone zone;
            zone.activatorBaseFormID = formID;
            zone.description = desc;
            zone.targetNodeName = "Door";  // Coin slot is relative to the Door node
            zone.activationRadius = 10.0f;  // Tight radius for coin slot
            zone.positionOffset = RE::NiPoint3(-57.0f, 0.0f, 8.0f);  // Coin slot offset from Door node
            zone.consumeItem = true;  // Consume the bottle cap
            zone.playAnimation = true;
            
            // Accepted items: bottle caps, pre-war money, and Nuka-Cade tokens
            zone.acceptedItemBaseFormIDs.insert(0x0000000F);  // Bottle Cap (Caps001)
            zone.acceptedItemBaseFormIDs.insert(0x00059B02);  // Pre-War Money
            zone.acceptedItemBaseFormIDs.insert(0x0001998A);  // Nuka-Cade Token (Nuka-World DLC)
            return zone;
        };
        
        // Register each known Nuka-Cola machine form ID
        _insertZones[0x000302DC] = createNukaMachineZone(0x000302DC, "Nuka-Cola Machine (0302DC)");
        _insertZones[0x001A6E50] = createNukaMachineZone(0x001A6E50, "Nuka-Cola Machine (1A6E50)");
        _insertZones[0x0020DE62] = createNukaMachineZone(0x0020DE62, "Nuka-Cola Machine (20DE62)");
        
        // Also keep a wildcard zone (form ID 0) for editor ID and 3D node name matching
        InsertZone wildcardZone;
        wildcardZone.activatorBaseFormID = 0;
        wildcardZone.description = "Nuka-Cola Machine (wildcard)";
        wildcardZone.targetNodeName = "Door";
        wildcardZone.activationRadius = 10.0f;
        wildcardZone.positionOffset = RE::NiPoint3(-57.0f, 0.0f, 8.0f);
        wildcardZone.consumeItem = true;
        wildcardZone.playAnimation = true;
        wildcardZone.acceptedItemBaseFormIDs.insert(0x0000000F);  // Bottle Cap
        wildcardZone.acceptedItemBaseFormIDs.insert(0x00059B02);  // Pre-War Money
        wildcardZone.acceptedItemBaseFormIDs.insert(0x0001998A);  // Nuka-Cade Token
        wildcardZone.editorIDPattern = "NukaCola";  // Match by editor ID if available
        wildcardZone.nodeNamePattern = "NukaCola";  // Also match by 3D node name!
        _insertZones[0] = wildcardZone;
        
        spdlog::info("[ItemInsertHandler] Registered {} default Nuka-Cola Machine zones", _insertZones.size());
    }

    void ItemInsertHandler::LoadZonesFromINI()
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        
        SI_Error rc = ini.LoadFile(INSERT_ZONES_INI_PATH);
        if (rc < 0) {
            spdlog::info("[ItemInsertHandler] No insert zones INI found at '{}', using defaults", INSERT_ZONES_INI_PATH);
            return;
        }
        
        spdlog::info("[ItemInsertHandler] Loading insert zones from '{}'", INSERT_ZONES_INI_PATH);
        
        // Load General section settings first
        _discoveryMode = ini.GetBoolValue("General", "bDiscoveryMode", false);
        spdlog::info("[ItemInsertHandler] Discovery mode: {}", _discoveryMode ? "ENABLED" : "disabled");
        
        // Load zones from sections with format [Description:HEXID]
        CSimpleIniA::TNamesDepend sections;
        ini.GetAllSections(sections);
        
        for (const auto& section : sections) {
            std::string sectionName = section.pItem;
            
            // Skip non-zone sections
            if (sectionName == "General") {
                continue;
            }
            
            // Check if section contains a colon (format: [Name:HEXID])
            size_t colonPos = sectionName.rfind(':');
            if (colonPos != std::string::npos && colonPos < sectionName.length() - 1) {
                std::string formIDStr = sectionName.substr(colonPos + 1);
                std::uint32_t formID = ParseHexFormID(formIDStr);
                
                InsertZone zone;
                zone.activatorBaseFormID = formID;
                zone.description = sectionName.substr(0, colonPos);
                
                // Load zone settings
                zone.targetNodeName = ini.GetValue(section.pItem, "sTargetNode", "");
                zone.activationRadius = static_cast<float>(ini.GetDoubleValue(section.pItem, "fActivationRadius", 15.0));
                zone.consumeItem = ini.GetBoolValue(section.pItem, "bConsumeItem", false);
                zone.playAnimation = ini.GetBoolValue(section.pItem, "bPlayAnimation", true);
                
                // Position offset
                zone.positionOffset.x = static_cast<float>(ini.GetDoubleValue(section.pItem, "fOffsetX", 0.0));
                zone.positionOffset.y = static_cast<float>(ini.GetDoubleValue(section.pItem, "fOffsetY", 0.0));
                zone.positionOffset.z = static_cast<float>(ini.GetDoubleValue(section.pItem, "fOffsetZ", 0.0));
                
                // Cylindrical zone mode (rotation-independent)
                zone.useCylindricalZone = ini.GetBoolValue(section.pItem, "bUseCylindricalZone", false);
                zone.cylindricalMinRadius = static_cast<float>(ini.GetDoubleValue(section.pItem, "fCylindricalMinRadius", 0.0));
                zone.cylindricalMaxRadius = static_cast<float>(ini.GetDoubleValue(section.pItem, "fCylindricalMaxRadius", 50.0));
                zone.cylindricalZOffset = static_cast<float>(ini.GetDoubleValue(section.pItem, "fCylindricalZOffset", 0.0));
                zone.cylindricalZTolerance = static_cast<float>(ini.GetDoubleValue(section.pItem, "fCylindricalZTolerance", 10.0));
                
                // Accepted items - comma-separated hex IDs
                const char* acceptedItems = ini.GetValue(section.pItem, "sAcceptedItems", "");
                if (acceptedItems && *acceptedItems) {
                    std::stringstream ss(acceptedItems);
                    std::string itemID;
                    while (std::getline(ss, itemID, ',')) {
                        // Trim whitespace
                        itemID.erase(0, itemID.find_first_not_of(" \t"));
                        itemID.erase(itemID.find_last_not_of(" \t") + 1);
                        
                        std::uint32_t id = ParseHexFormID(itemID);
                        if (id != 0) {
                            zone.acceptedItemBaseFormIDs.insert(id);
                        }
                    }
                }
                
                // Optional editor ID pattern for matching activators
                zone.editorIDPattern = ini.GetValue(section.pItem, "sEditorIDPattern", "");
                
                if (formID != 0 || !zone.editorIDPattern.empty()) {
                    _insertZones[formID] = zone;
                    if (zone.useCylindricalZone) {
                        spdlog::info("[ItemInsertHandler] [{}] {:08X}: CYLINDRICAL zone, Z={:.1f}±{:.1f}, R={:.1f}-{:.1f}, node='{}', items={}",
                            zone.description, formID, zone.cylindricalZOffset, zone.cylindricalZTolerance,
                            zone.cylindricalMinRadius, zone.cylindricalMaxRadius,
                            zone.targetNodeName, zone.acceptedItemBaseFormIDs.size());
                    } else {
                        spdlog::info("[ItemInsertHandler] [{}] {:08X}: radius={:.1f}, node='{}', items={}",
                            zone.description, formID, zone.activationRadius, 
                            zone.targetNodeName, zone.acceptedItemBaseFormIDs.size());
                    }
                }
            }
        }
        
        spdlog::info("[ItemInsertHandler] Loaded {} insert zones from INI", _insertZones.size());
    }

    void ItemInsertHandler::Update()
    {
        if (!_initialized) {
            return;
        }
        
        // Check for cell change
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        
        auto* currentCell = player->GetParentCell();
        if (currentCell != _currentCell) {
            spdlog::info("[ItemInsertHandler] Cell changed - rescanning for insert zones...");
            _currentCell = currentCell;
            ScanCellForInsertZones();
        }
        
        // Check held items for each hand
        auto& grabMgr = GrabManager::GetSingleton();
        
        // Debug: Log every 5 seconds if discovery mode is on
        static auto lastDebugLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (_discoveryMode && std::chrono::duration_cast<std::chrono::seconds>(now - lastDebugLog).count() >= 5) {
            lastDebugLog = now;
            bool leftGrabbing = grabMgr.IsGrabbing(true);
            bool rightGrabbing = grabMgr.IsGrabbing(false);
            spdlog::info("[ItemInsertHandler] UPDATE tick - Left grabbing: {}, Right grabbing: {}, tracked zones: {}", 
                leftGrabbing, rightGrabbing, _trackedZones.size());
        }
        
        // Discovery mode: Log left hand position when crosshair target changes (user activating with right hand)
        if (_discoveryMode) {
            // Simpler approach: Log left hand position relative to nearby Nuka-Cola machines
            static auto lastHandLog = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHandLog).count() >= 500) {
                lastHandLog = now;
                
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                RE::NiNode* leftWandNode = heisenberg::GetWandNode(playerNodes, true);
                if (playerNodes && leftWandNode && _currentCell) {
                    RE::NiPoint3 leftHandPos = leftWandNode->world.translate;
                    
                    // Check for nearby Nuka-Cola machines (base form 000302DC)
                    for (const auto& ref : _currentCell->references) {
                        if (!ref || ref->IsDisabled() || ref->IsDeleted()) continue;
                        
                        const auto baseObj = ref->GetObjectReference();
                        if (!baseObj) continue;
                        
                        std::uint32_t baseFormID = baseObj->GetFormID();
                        if (baseFormID != 0x000302DC) continue;  // Only Nuka-Cola machines
                        
                        auto* node3D = ref->Get3D();
                        if (!node3D) continue;
                        
                        RE::NiPoint3 rootPos = node3D->world.translate;
                        RE::NiMatrix3 rootRot = node3D->world.rotate;
                        
                        // Calculate distance
                        float dx = leftHandPos.x - rootPos.x;
                        float dy = leftHandPos.y - rootPos.y;
                        float dz = leftHandPos.z - rootPos.z;
                        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        
                        if (dist < 150.0f) {
                            // Transform to local space (relative to root)
                            RE::NiPoint3 worldOffset(dx, dy, dz);
                            RE::NiPoint3 localOffset;
                            localOffset.x = rootRot.entry[0][0] * worldOffset.x + rootRot.entry[1][0] * worldOffset.y + rootRot.entry[2][0] * worldOffset.z;
                            localOffset.y = rootRot.entry[0][1] * worldOffset.x + rootRot.entry[1][1] * worldOffset.y + rootRot.entry[2][1] * worldOffset.z;
                            localOffset.z = rootRot.entry[0][2] * worldOffset.x + rootRot.entry[1][2] * worldOffset.y + rootRot.entry[2][2] * worldOffset.z;
                            
                            float horizontalDist = std::sqrt(dx * dx + dy * dy);
                            
                            spdlog::info("[ItemInsertHandler] LEFT HAND @ NukaCola ROOT: dist={:.1f} localOffset=({:.1f}, {:.1f}, {:.1f}) horiz={:.1f}",
                                dist, localOffset.x, localOffset.y, localOffset.z, horizontalDist);
                            
                            // Also log relative to "Door" node if it exists
                            RE::NiAVObject* doorNode = heisenberg::Utils::FindNode(node3D, "Door", 10);
                            if (doorNode) {
                                RE::NiPoint3 doorPos = doorNode->world.translate;
                                RE::NiMatrix3 doorRot = doorNode->world.rotate;
                                
                                float ddx = leftHandPos.x - doorPos.x;
                                float ddy = leftHandPos.y - doorPos.y;
                                float ddz = leftHandPos.z - doorPos.z;
                                float doorDist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                                
                                RE::NiPoint3 doorWorldOffset(ddx, ddy, ddz);
                                RE::NiPoint3 doorLocalOffset;
                                doorLocalOffset.x = doorRot.entry[0][0] * doorWorldOffset.x + doorRot.entry[1][0] * doorWorldOffset.y + doorRot.entry[2][0] * doorWorldOffset.z;
                                doorLocalOffset.y = doorRot.entry[0][1] * doorWorldOffset.x + doorRot.entry[1][1] * doorWorldOffset.y + doorRot.entry[2][1] * doorWorldOffset.z;
                                doorLocalOffset.z = doorRot.entry[0][2] * doorWorldOffset.x + doorRot.entry[1][2] * doorWorldOffset.y + doorRot.entry[2][2] * doorWorldOffset.z;
                                
                                spdlog::info("[ItemInsertHandler] LEFT HAND @ NukaCola DOOR: dist={:.1f} localOffset=({:.1f}, {:.1f}, {:.1f})",
                                    doorDist, doorLocalOffset.x, doorLocalOffset.y, doorLocalOffset.z);
                            }
                        }
                    }
                }
            }
        }
        
        // Lock mutex for _trackedZones access during proximity checks
        std::scoped_lock lock(_trackedZonesMutex);

        for (bool isLeft : {true, false}) {
            if (!grabMgr.IsGrabbing(isLeft)) {
                continue;
            }
            
            const auto& grabState = grabMgr.GetGrabState(isLeft);
            RE::TESObjectREFR* grabRefr = grabState.GetRefr();  // Use handle-safe accessor
            if (!grabRefr || !grabState.node) {
                continue;
            }
            
            // Get held item position and base form ID
            RE::NiPoint3 heldItemPos = grabState.node->world.translate;
            
            std::uint32_t heldItemBaseFormID = 0;
            if (auto* baseObj = grabRefr->GetObjectReference()) {
                heldItemBaseFormID = baseObj->GetFormID();
            }
            
            // Discovery mode: Log proximity to ALL nearby containers/activators
            if (_discoveryMode) {
                LogNearbyActivators(heldItemPos, heldItemBaseFormID);
            }
            
            // Check proximity to insert zones
            auto result = CheckHeldItemProximity(heldItemPos, heldItemBaseFormID, grabRefr);
            
            if (result.inRange && result.zone) {
                // Log for discovery
                if (_discoveryMode) {
                    spdlog::info("[ItemInsertHandler] Held item {:08X} near insert zone '{}' (dist={:.1f})",
                        heldItemBaseFormID, result.zone->zoneConfig->description, result.distance);
                }
                
                // Trigger activation
                TryActivateInsertZone(heldItemPos, heldItemBaseFormID, grabRefr, isLeft);
            }
        }
    }
    
    void ItemInsertHandler::LogNearbyActivators(const RE::NiPoint3& heldItemPos, std::uint32_t heldItemBaseFormID)
    {
        // Throttle logging - only log every 2 seconds
        static auto lastLogTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLogTime).count() < 2000) {
            return;
        }
        lastLogTime = now;
        
        if (!_currentCell) return;
        
        float searchRadius = 100.0f;  // Log activators within 100 units
        
        for (const auto& ref : _currentCell->references) {
            if (!ref || ref->IsDisabled() || ref->IsDeleted()) continue;
            
            const auto baseObj = ref->GetObjectReference();
            if (!baseObj) continue;
            
            auto formType = baseObj->GetFormType();
            if (formType != RE::ENUM_FORM_ID::kACTI && 
                formType != RE::ENUM_FORM_ID::kCONT && 
                formType != RE::ENUM_FORM_ID::kFURN) continue;
            
            auto* node3D = ref->Get3D();
            if (!node3D) continue;
            
            // Get root node position and rotation
            RE::NiPoint3 rootPos = node3D->world.translate;
            RE::NiMatrix3 rootRot = node3D->world.rotate;
            
            // Calculate distance from root
            float dx = heldItemPos.x - rootPos.x;
            float dy = heldItemPos.y - rootPos.y;
            float dz = heldItemPos.z - rootPos.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            
            if (dist < searchRadius) {
                std::uint32_t baseFormID = baseObj->GetFormID();
                const char* editorID = baseObj->GetFormEditorID();
                
                // Calculate world-space offset from root to held item
                RE::NiPoint3 worldOffset(dx, dy, dz);
                
                // Transform world offset to local space (inverse rotation = transpose)
                RE::NiPoint3 localOffsetRoot;
                localOffsetRoot.x = rootRot.entry[0][0] * worldOffset.x + rootRot.entry[1][0] * worldOffset.y + rootRot.entry[2][0] * worldOffset.z;
                localOffsetRoot.y = rootRot.entry[0][1] * worldOffset.x + rootRot.entry[1][1] * worldOffset.y + rootRot.entry[2][1] * worldOffset.z;
                localOffsetRoot.z = rootRot.entry[0][2] * worldOffset.x + rootRot.entry[1][2] * worldOffset.y + rootRot.entry[2][2] * worldOffset.z;
                
                spdlog::info("[ItemInsertHandler] NEARBY: base={:08X} '{}' dist={:.1f}", baseFormID, editorID ? editorID : "", dist);
                spdlog::info("[ItemInsertHandler]   WorldOffset=({:.1f}, {:.1f}, {:.1f})", dx, dy, dz);
                spdlog::info("[ItemInsertHandler]   RootRot row0=({:.2f}, {:.2f}, {:.2f})", rootRot.entry[0][0], rootRot.entry[0][1], rootRot.entry[0][2]);
                spdlog::info("[ItemInsertHandler]   RootRot row1=({:.2f}, {:.2f}, {:.2f})", rootRot.entry[1][0], rootRot.entry[1][1], rootRot.entry[1][2]);
                spdlog::info("[ItemInsertHandler]   RootRot row2=({:.2f}, {:.2f}, {:.2f})", rootRot.entry[2][0], rootRot.entry[2][1], rootRot.entry[2][2]);
                spdlog::info("[ItemInsertHandler]   ROOT localOffset=({:.1f}, {:.1f}, {:.1f})", 
                    localOffsetRoot.x, localOffsetRoot.y, localOffsetRoot.z);
                
                // Also try to find Door node and calculate offset from there
                RE::NiAVObject* doorNode = FindNodeRecursive(node3D, "Door");
                if (doorNode) {
                    RE::NiPoint3 doorPos = doorNode->world.translate;
                    RE::NiMatrix3 doorRot = doorNode->world.rotate;
                    
                    float ddx = heldItemPos.x - doorPos.x;
                    float ddy = heldItemPos.y - doorPos.y;
                    float ddz = heldItemPos.z - doorPos.z;
                    float doorDist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                    
                    // Local offset using Door's rotation
                    RE::NiPoint3 doorWorldOffset(ddx, ddy, ddz);
                    RE::NiPoint3 localOffsetDoor;
                    localOffsetDoor.x = doorRot.entry[0][0] * doorWorldOffset.x + doorRot.entry[1][0] * doorWorldOffset.y + doorRot.entry[2][0] * doorWorldOffset.z;
                    localOffsetDoor.y = doorRot.entry[0][1] * doorWorldOffset.x + doorRot.entry[1][1] * doorWorldOffset.y + doorRot.entry[2][1] * doorWorldOffset.z;
                    localOffsetDoor.z = doorRot.entry[0][2] * doorWorldOffset.x + doorRot.entry[1][2] * doorWorldOffset.y + doorRot.entry[2][2] * doorWorldOffset.z;
                    
                    // Also try using ROOT rotation instead (Door might have different local rotations)
                    RE::NiPoint3 localOffsetDoorRootRot;
                    localOffsetDoorRootRot.x = rootRot.entry[0][0] * doorWorldOffset.x + rootRot.entry[1][0] * doorWorldOffset.y + rootRot.entry[2][0] * doorWorldOffset.z;
                    localOffsetDoorRootRot.y = rootRot.entry[0][1] * doorWorldOffset.x + rootRot.entry[1][1] * doorWorldOffset.y + rootRot.entry[2][1] * doorWorldOffset.z;
                    localOffsetDoorRootRot.z = rootRot.entry[0][2] * doorWorldOffset.x + rootRot.entry[1][2] * doorWorldOffset.y + rootRot.entry[2][2] * doorWorldOffset.z;
                    
                    spdlog::info("[ItemInsertHandler]   DOOR localOffset=({:.1f}, {:.1f}, {:.1f}) dist={:.1f}",
                        localOffsetDoor.x, localOffsetDoor.y, localOffsetDoor.z, doorDist);
                    spdlog::info("[ItemInsertHandler]   DOOR+RootRot=({:.1f}, {:.1f}, {:.1f})",
                        localOffsetDoorRootRot.x, localOffsetDoorRootRot.y, localOffsetDoorRootRot.z);
                }
            }
        }
    }

    void ItemInsertHandler::ScanCellForInsertZones()
    {
        std::scoped_lock lock(_trackedZonesMutex);
        _trackedZones.clear();
        
        if (!_currentCell) {
            spdlog::debug("[ItemInsertHandler] No current cell");
            return;
        }
        
        spdlog::info("[ItemInsertHandler] Scanning cell for insert zones...");
        
        int zoneCount = 0;
        int activatorCount = 0;
        
        for (const auto& ref : _currentCell->references) {
            if (!ref || ref->IsDisabled() || ref->IsDeleted()) {
                continue;
            }
            
            const auto baseObj = ref->GetObjectReference();
            if (!baseObj) {
                continue;
            }
            
            // Check activators, containers, AND furniture (Nuka machines are furniture!)
            auto formType = baseObj->GetFormType();
            if (formType != RE::ENUM_FORM_ID::kACTI && 
                formType != RE::ENUM_FORM_ID::kCONT &&
                formType != RE::ENUM_FORM_ID::kFURN) {
                continue;
            }
            
            std::uint32_t baseFormID = baseObj->GetFormID();
            const char* editorID = baseObj->GetFormEditorID();
            
            // Log all activators in discovery mode
            if (_discoveryMode) {
                activatorCount++;
                spdlog::info("[ItemInsertHandler] ACTIVATOR #{}: ref={:08X} base={:08X} editorID='{}'",
                    activatorCount, ref->GetFormID(), baseFormID, editorID ? editorID : "(null)");
            }
            
            // Check if this activator has an insert zone defined
            const InsertZone* zoneConfig = nullptr;
            
            // First, check by exact form ID
            auto it = _insertZones.find(baseFormID);
            if (it != _insertZones.end()) {
                zoneConfig = &it->second;
                spdlog::info("[ItemInsertHandler] Matched zone by form ID: {:08X} -> '{}'",
                    baseFormID, zoneConfig->description);
            }
            
            // If not found, check wildcards (form ID 0) by editor ID or 3D node name
            if (!zoneConfig) {
                auto wildcardIt = _insertZones.find(0);
                if (wildcardIt != _insertZones.end()) {
                    // Try editor ID pattern
                    if (editorID && !wildcardIt->second.editorIDPattern.empty()) {
                        std::string_view editorIDView(editorID);
                        if (editorIDView.find(wildcardIt->second.editorIDPattern) != std::string_view::npos) {
                            zoneConfig = &wildcardIt->second;
                            spdlog::info("[ItemInsertHandler] Matched zone by editorID pattern: '{}' contains '{}'",
                                editorID, wildcardIt->second.editorIDPattern);
                        }
                    }
                    
                    // Try 3D node name pattern
                    if (!zoneConfig && !wildcardIt->second.nodeNamePattern.empty()) {
                        if (auto* node3D = ref->Get3D()) {
                            const char* nodeName = node3D->name.c_str();
                            if (nodeName) {
                                std::string_view nodeNameView(nodeName);
                                if (nodeNameView.find(wildcardIt->second.nodeNamePattern) != std::string_view::npos) {
                                    zoneConfig = &wildcardIt->second;
                                    spdlog::info("[ItemInsertHandler] Matched zone by 3D node name: '{}' contains '{}'",
                                        nodeName, wildcardIt->second.nodeNamePattern);
                                }
                            }
                        }
                    }
                }
            }
            
            if (!zoneConfig) {
                continue;  // No insert zone for this activator
            }
            
            // Create tracked zone
            TrackedInsertZone tracked;
            tracked.activatorRefHandle = RE::ObjectRefHandle(ref.get());
            tracked.activatorFormID = ref->GetFormID();
            tracked.zoneConfig = zoneConfig;
            
            // Cache target node
            if (!zoneConfig->targetNodeName.empty() && ref->Get3D()) {
                tracked.cachedTargetNode.reset(FindNodeRecursive(ref->Get3D(), zoneConfig->targetNodeName));
                if (tracked.cachedTargetNode) {
                    spdlog::debug("[ItemInsertHandler] Found target node '{}' for {:08X}",
                        zoneConfig->targetNodeName, baseFormID);
                } else {
                    spdlog::warn("[ItemInsertHandler] Target node '{}' NOT FOUND for {:08X}",
                        zoneConfig->targetNodeName, baseFormID);
                }
            }
            
            _trackedZones.push_back(tracked);
            zoneCount++;
            
            spdlog::info("[ItemInsertHandler] [{}] ref={:08X} base={:08X} '{}'",
                zoneCount, tracked.activatorFormID, baseFormID, editorID ? editorID : "");
            
            // Dump node tree for discovery
            if (_discoveryMode && ref->Get3D()) {
                LogActivatorNodes(ref.get());
            }
        }
        
        spdlog::info("[ItemInsertHandler] Tracking {} insert zones in current cell", zoneCount);
    }

    RE::NiAVObject* ItemInsertHandler::FindNodeRecursive(RE::NiAVObject* root, const std::string& nodeName) const
    {
        return FindNodeByName(root, nodeName);
    }

    void ItemInsertHandler::CollectNodeNames(RE::NiAVObject* node, std::vector<std::string>& outNames, int depth) const
    {
        CollectNodeNamesRecursive(node, outNames, depth, /*includeWorldPos=*/true);
    }

    void ItemInsertHandler::LogActivatorNodes(RE::TESObjectREFR* ref)
    {
        LogRefNodeTree(ref, "[ItemInsertHandler]", /*includeWorldPos=*/true);
    }

    float ItemInsertHandler::GetDistanceToZone(const RE::NiPoint3& itemPos, const TrackedInsertZone& zone) const
    {
        auto* activatorRefr = zone.GetRefr();
        if (!activatorRefr) {
            return (std::numeric_limits<float>::max)();
        }

        RE::NiPoint3 zonePos;
        RE::NiMatrix3 objectRot;

        if (zone.cachedTargetNode) {
            zonePos = zone.cachedTargetNode->world.translate;
            objectRot = zone.cachedTargetNode->world.rotate;
        } else if (auto* node3D = activatorRefr->Get3D()) {
            zonePos = node3D->world.translate;
            objectRot = node3D->world.rotate;
        } else {
            zonePos = activatorRefr->GetPosition();
            // No rotation available, use identity
            objectRot = RE::NiMatrix3();
        }
        
        // Check if using cylindrical zone mode
        if (zone.zoneConfig && zone.zoneConfig->useCylindricalZone) {
            // Cylindrical mode: check if item is within a ring at the correct Z height
            // This works regardless of object rotation!
            
            float zOffset = zone.zoneConfig->cylindricalZOffset;
            float zTolerance = zone.zoneConfig->cylindricalZTolerance;
            float minRadius = zone.zoneConfig->cylindricalMinRadius;
            float maxRadius = zone.zoneConfig->cylindricalMaxRadius;
            
            // Check Z height (this is vertical and rotation-independent)
            float itemZ = itemPos.z;
            float targetZ = zonePos.z + zOffset;
            float zDiff = std::abs(itemZ - targetZ);
            
            if (zDiff > zTolerance) {
                // Too high or too low, return large distance
                return zDiff;  // Return the actual Z difference for debugging
            }
            
            // Check horizontal distance (XY plane only)
            float dx = itemPos.x - zonePos.x;
            float dy = itemPos.y - zonePos.y;
            float horizontalDist = std::sqrt(dx * dx + dy * dy);
            
            // Check if within the ring
            if (horizontalDist < minRadius) {
                // Inside the inner radius (too close)
                return minRadius - horizontalDist;  // Return how far inside we are
            }
            
            if (horizontalDist > maxRadius) {
                // Outside the outer radius (too far)
                return horizontalDist - maxRadius;  // Return how far outside we are
            }
            
            // We're within the ring and at correct Z - return 0 (in zone)
            return 0.0f;
        }
        
        // Standard spherical mode with local offset
        if (zone.zoneConfig) {
            const RE::NiPoint3& localOffset = zone.zoneConfig->positionOffset;
            // Transform local offset to world space using object's rotation
            // Transpose because NiMatrix3 is row-major
            RE::NiPoint3 worldOffset;
            worldOffset.x = objectRot.entry[0][0] * localOffset.x + objectRot.entry[0][1] * localOffset.y + objectRot.entry[0][2] * localOffset.z;
            worldOffset.y = objectRot.entry[1][0] * localOffset.x + objectRot.entry[1][1] * localOffset.y + objectRot.entry[1][2] * localOffset.z;
            worldOffset.z = objectRot.entry[2][0] * localOffset.x + objectRot.entry[2][1] * localOffset.y + objectRot.entry[2][2] * localOffset.z;
            zonePos += worldOffset;
        }
        
        float dx = itemPos.x - zonePos.x;
        float dy = itemPos.y - zonePos.y;
        float dz = itemPos.z - zonePos.z;
        
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    bool ItemInsertHandler::ItemMatchesZone(std::uint32_t itemBaseFormID, RE::TESObjectREFR* itemRef, const InsertZone& zone) const
    {
        // Check by form ID
        if (zone.acceptedItemBaseFormIDs.count(itemBaseFormID) > 0) {
            return true;
        }
        
        // TODO: Check by keyword if specified
        // if (!zone.acceptedKeyword.empty() && itemRef) { ... }
        
        return false;
    }

    bool ItemInsertHandler::CanActivate(const TrackedInsertZone& zone) const
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - zone.lastActivationTime).count();
        
        if (elapsed < _activationCooldownMs) {
            return false;
        }
        
        auto* activatorRefr = zone.GetRefr();
        if (!activatorRefr || activatorRefr->IsDisabled() || activatorRefr->IsDeleted()) {
            return false;
        }
        
        return true;
    }

    ItemInsertHandler::ProximityResult ItemInsertHandler::CheckHeldItemProximity(
        const RE::NiPoint3& heldItemPos,
        std::uint32_t heldItemBaseFormID,
        RE::TESObjectREFR* heldItemRef)
    {
        ProximityResult result;
        
        if (_trackedZones.empty()) {
            return result;
        }
        
        float closestDist = (std::numeric_limits<float>::max)();
        TrackedInsertZone* closestZone = nullptr;
        
        for (auto& zone : _trackedZones) {
            if (!zone.GetRefr() || !zone.zoneConfig) {
                continue;
            }
            
            // Check if item matches this zone's requirements
            if (!ItemMatchesZone(heldItemBaseFormID, heldItemRef, *zone.zoneConfig)) {
                continue;
            }
            
            float dist = GetDistanceToZone(heldItemPos, zone);
            
            if (dist < zone.zoneConfig->activationRadius && dist < closestDist) {
                closestDist = dist;
                closestZone = &zone;
            }
        }
        
        if (closestZone) {
            result.inRange = true;
            result.zone = closestZone;
            result.distance = closestDist;
        }
        
        return result;
    }

    bool ItemInsertHandler::TryActivateInsertZone(
        const RE::NiPoint3& heldItemPos,
        std::uint32_t heldItemBaseFormID,
        RE::TESObjectREFR* heldItemRef,
        bool isLeftHand)
    {
        auto result = CheckHeldItemProximity(heldItemPos, heldItemBaseFormID, heldItemRef);
        
        if (result.inRange && result.zone && CanActivate(*result.zone)) {
            ActivateZone(*result.zone, heldItemRef, isLeftHand);
            result.zone->lastActivationTime = std::chrono::steady_clock::now();
            return true;
        }
        
        return false;
    }

    void ItemInsertHandler::ActivateZone(TrackedInsertZone& zone, RE::TESObjectREFR* itemRef, bool isLeftHand)
    {
        auto* activatorRefr = zone.GetRefr();
        if (!activatorRefr || !zone.zoneConfig) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        std::uint32_t activatorBaseFormID = 0;
        const char* editorID = "";
        if (const auto baseObj = activatorRefr->GetObjectReference()) {
            activatorBaseFormID = baseObj->GetFormID();
            if (baseObj->GetFormEditorID()) {
                editorID = baseObj->GetFormEditorID();
            }
        }

        spdlog::info("[ItemInsertHandler] *** ACTIVATING '{}' (ref={:08X} base={:08X} '{}') ***",
            zone.zoneConfig->description, zone.activatorFormID, activatorBaseFormID, editorID);

        // Discovery mode: log node tree
        if (_discoveryMode) {
            LogActivatorNodes(activatorRefr);
        }

        // Activate the object
        if (zone.zoneConfig->playAnimation) {
            activatorRefr->ActivateRef(player, nullptr, 1, false, false, false);
        }
        
        // Consume item if configured
        if (zone.zoneConfig->consumeItem && itemRef) {
            // Get the base object before we release the grab
            RE::TESBoundObject* baseObj = itemRef->GetObjectReference();
            
            // Release the grab first (forStorage=true to skip physics restoration)
            auto& grabMgr = GrabManager::GetSingleton();
            grabMgr.EndGrab(isLeftHand, nullptr, true);
            
            // Play the same pickup sound that plays when storing items
            // PlayPickUpSound(boundObj, pickUp=true, use=false) - same as storage
            if (baseObj) {
                player->PlayPickUpSound(baseObj, true, false);
                spdlog::info("[ItemInsertHandler] Played pickup sound for {}", 
                    baseObj->GetFormEditorID() ? baseObj->GetFormEditorID() : "item");
            }
            
            // Actually consume the item - disable the world reference.
            // Do NOT call SetDelete — Inventory3DManager may still hold a handle,
            // causing crash in FinishItemLoadTask → GetOnLocalMap.
            if (itemRef->Get3D()) {
                heisenberg::SafeDisable(itemRef);
                spdlog::info("[ItemInsertHandler] Consumed item {:08X} (disabled from world)", itemRef->formID);
            }
        }
    }

    void ItemInsertHandler::RegisterInsertZone(const InsertZone& zone)
    {
        _insertZones[zone.activatorBaseFormID] = zone;
        spdlog::info("[ItemInsertHandler] Registered insert zone: {:08X} '{}'",
            zone.activatorBaseFormID, zone.description);
    }

} // namespace heisenberg
