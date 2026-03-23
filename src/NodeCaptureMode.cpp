#include "NodeCaptureMode.h"

#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "ActivatorHandler.h"
#include "Config.h"
#include "FRIKInterface.h"
#include "Hooks.h"
#include "VRInput.h"

#include "f4vr/F4VRUtils.h"

// ViewCaster function for getting what the player is pointing at
// IMPORTANT: This uses OUTPUT PARAMETER pattern - returns handle via second param!
using QActivatePickRef_t = RE::ObjectRefHandle*(*)(void* viewCaster, RE::ObjectRefHandle* outHandle);
inline REL::Relocation<QActivatePickRef_t> QActivatePickRef{ REL::Offset(0x9D0CE0) };

// ViewCaster global instances - one per VR wand
// Primary = off-hand (LEFT in default mode)
// Secondary = weapon hand (RIGHT in default mode)
inline REL::Relocation<std::uintptr_t> g_primaryViewCaster{ REL::Offset(0x5B2A360) };
inline REL::Relocation<std::uintptr_t> g_secondaryViewCaster{ REL::Offset(0x5B2A280) };

namespace heisenberg
{
    void NodeCaptureMode::ClearState()
    {
        _modeActive = false;
        _thumbstickWasPressed = false;
        _isHoldingThumbstick = false;
        _lastResult = CaptureResult{};
        spdlog::info("[NodeCaptureMode] State cleared");
    }

    void NodeCaptureMode::ExitMode()
    {
        if (_modeActive) {
            _modeActive = false;
            _isHoldingThumbstick = false;
            
            // Reset right hand to fully open
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(false, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
            
            heisenberg::Hooks::ShowHUDMessageDirect("Node Capture Mode: EXITED");
            spdlog::info("[NodeCaptureMode] Mode exited");
        }
    }

    void NodeCaptureMode::EnterMode()
    {
        _modeActive = true;
        _isHoldingThumbstick = false;
        
        // Show entry message
        heisenberg::Hooks::ShowHUDMessageDirect("Node Capture Mode: ACTIVE");
        heisenberg::Hooks::ShowHUDMessageDirect("Position finger at activation point, hold thumbstick to save");
        
        // Haptic feedback on both hands
        g_vrInput.TriggerHaptic(true, 10000);   // Left hand (where thumbstick is)
        g_vrInput.TriggerHaptic(false, 5000);   // Right hand
        
        spdlog::info("[NodeCaptureMode] Mode entered");
    }

    void NodeCaptureMode::Update()
    {
        // Get current thumbstick state (RIGHT hand thumbstick)
        bool thumbstickPressed = g_vrInput.IsPressed(false, VRButton::ThumbstickPress);
        auto now = std::chrono::steady_clock::now();

        // =====================================================================
        // NOT IN MODE: Check for hold to enter
        // =====================================================================
        if (!_modeActive) {
            if (thumbstickPressed && !_thumbstickWasPressed) {
                // Just started pressing - begin hold timer
                _thumbstickHoldStartTime = now;
                _isHoldingThumbstick = true;
            } else if (thumbstickPressed && _isHoldingThumbstick) {
                // Still holding - check duration
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - _thumbstickHoldStartTime).count();
                
                if (elapsed >= THUMBSTICK_HOLD_TIME_MS) {
                    // Held long enough - enter mode
                    EnterMode();
                    _isHoldingThumbstick = false;
                }
            } else if (!thumbstickPressed) {
                // Released before threshold - cancel
                _isHoldingThumbstick = false;
            }
        }
        // =====================================================================
        // IN MODE: Force pointing pose and check for hold to capture
        // =====================================================================
        else {
            // Force RIGHT hand pointing pose: index extended, others curled
            auto& frik = FRIKInterface::GetSingleton();
            frik.SetHandPoseFingerPositions(false, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);  // thumb, index, middle, ring, pinky
            
            if (thumbstickPressed && !_thumbstickWasPressed) {
                // Just started pressing - begin hold timer for capture
                _thumbstickHoldStartTime = now;
                _isHoldingThumbstick = true;
            } else if (thumbstickPressed && _isHoldingThumbstick) {
                // Still holding - check duration
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - _thumbstickHoldStartTime).count();
                
                if (elapsed >= THUMBSTICK_HOLD_TIME_MS) {
                    // Held long enough - perform capture
                    PerformCapture();
                    _isHoldingThumbstick = false;
                    
                    // Exit mode after capture
                    ExitMode();
                }
            } else if (!thumbstickPressed) {
                // Released before threshold - cancel this hold attempt
                _isHoldingThumbstick = false;
            }
        }
        
        _thumbstickWasPressed = thumbstickPressed;
    }

    void NodeCaptureMode::PerformCapture()
    {
        spdlog::info("[NodeCaptureMode] Performing capture...");
        
        // Get right index finger tip position
        auto& frik = FRIKInterface::GetSingleton();
        RE::NiPoint3 fingerWorldPos;
        
        if (!frik.GetIndexFingerTipPosition(false, fingerWorldPos)) {
            heisenberg::Hooks::ShowHUDMessageDirect("CAPTURE FAILED: Could not get finger position");
            spdlog::error("[NodeCaptureMode] Failed to get finger position from FRIK");
            _lastResult = CaptureResult{};
            _lastResult.success = false;
            return;
        }
        
        spdlog::info("[NodeCaptureMode] Finger position: ({:.1f}, {:.1f}, {:.1f})",
            fingerWorldPos.x, fingerWorldPos.y, fingerWorldPos.z);
        
        // =====================================================================
        // STRATEGY: Try ViewCaster first (what player is pointing at),
        // then fall back to ActivatorHandler's tracked activators
        // =====================================================================
        RE::TESObjectREFR* targetRef = nullptr;
        std::uint32_t baseFormID = 0;
        bool fromViewCaster = false;
        
        // Try ViewCaster first - check both hands
        // IMPORTANT: These offsets (0x5B2A360, 0x5B2A280) are DIRECT ViewCaster objects!
        // NOT pointer holders - don't dereference!
        // In default mode: primary = LEFT (off-hand), secondary = RIGHT (weapon hand)
        // In left-handed mode: primary = RIGHT (off-hand), secondary = LEFT (weapon hand)
        void* rawPrimaryVC = reinterpret_cast<void*>(g_primaryViewCaster.address());
        void* rawSecondaryVC = reinterpret_cast<void*>(g_secondaryViewCaster.address());
        bool isLH = VRInput::GetSingleton().IsLeftHandedMode();
        void* offHandVC  = isLH ? rawSecondaryVC : rawPrimaryVC;
        void* weaponHandVC = isLH ? rawPrimaryVC : rawSecondaryVC;

        spdlog::debug("[NodeCaptureMode] ViewCaster pointers: offHand={:X}, weaponHand={:X} (LH={})",
            reinterpret_cast<uintptr_t>(offHandVC), reinterpret_cast<uintptr_t>(weaponHandVC), isLH);

        // Try off-hand first
        if (offHandVC) {
            RE::ObjectRefHandle handle;
            QActivatePickRef(offHandVC, &handle);
            spdlog::debug("[NodeCaptureMode] Off-hand ViewCaster handle valid: {}", handle ? true : false);
            if (handle) {
                RE::NiPointer<RE::TESObjectREFR> refPtr = handle.get();
                if (refPtr) {
                    targetRef = refPtr.get();
                    spdlog::info("[NodeCaptureMode] ViewCaster (off-hand) has target: {}",
                        targetRef->GetDisplayFullName() ? targetRef->GetDisplayFullName() : "Unknown");
                    fromViewCaster = true;
                }
            }
        }
        
        // If not found on off-hand, try weapon hand
        if (!targetRef && weaponHandVC) {
            RE::ObjectRefHandle handle;
            QActivatePickRef(weaponHandVC, &handle);
            spdlog::debug("[NodeCaptureMode] Weapon-hand ViewCaster handle valid: {}", handle ? true : false);
            if (handle) {
                RE::NiPointer<RE::TESObjectREFR> refPtr = handle.get();
                if (refPtr) {
                    targetRef = refPtr.get();
                    spdlog::info("[NodeCaptureMode] ViewCaster (weapon-hand) has target: {}",
                        targetRef->GetDisplayFullName() ? targetRef->GetDisplayFullName() : "Unknown");
                    fromViewCaster = true;
                }
            }
        }
        
        // If ViewCaster didn't find anything, fall back to ActivatorHandler
        if (!targetRef) {
            spdlog::info("[NodeCaptureMode] ViewCaster has no target, checking ActivatorHandler...");
            
            auto& actHandler = ActivatorHandler::GetSingleton();
            const auto& trackedActivators = actHandler.GetTrackedActivators();
            
            if (!trackedActivators.empty()) {
                // Find closest activator
                float closestDist = 100.0f;  // Max 100 units (~1 meter)
                const ActivatorHandler::TrackedActivator* closestActivator = nullptr;
                
                for (const auto& activator : trackedActivators) {
                    RE::TESObjectREFR* actRef = activator.GetRefr();
                    if (!actRef || !actRef->Get3D()) continue;
                    
                    RE::NiPoint3 actPos = actRef->Get3D()->world.translate;
                    float dx = fingerWorldPos.x - actPos.x;
                    float dy = fingerWorldPos.y - actPos.y;
                    float dz = fingerWorldPos.z - actPos.z;
                    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                    
                    if (dist < closestDist) {
                        closestDist = dist;
                        closestActivator = &activator;
                    }
                }
                
                if (closestActivator) {
                    targetRef = closestActivator->GetRefr();
                    baseFormID = closestActivator->baseFormID;
                    spdlog::info("[NodeCaptureMode] Using ActivatorHandler tracked activator (dist={:.1f})", closestDist);
                }
            }
        }
        
        // If still nothing found, fail
        if (!targetRef) {
            heisenberg::Hooks::ShowHUDMessageDirect("CAPTURE FAILED: No activator found");
            heisenberg::Hooks::ShowHUDMessageDirect("Point at an activator with your wand");
            spdlog::warn("[NodeCaptureMode] No target from ViewCaster or ActivatorHandler");
            _lastResult = CaptureResult{};
            _lastResult.success = false;
            return;
        }
        
        // Validate targetRef has 3D
        if (!targetRef->Get3D()) {
            heisenberg::Hooks::ShowHUDMessageDirect("CAPTURE FAILED: Target has no 3D model");
            _lastResult = CaptureResult{};
            _lastResult.success = false;
            return;
        }
        
        // Get activator info - if from ViewCaster, we need to get baseFormID
        if (baseFormID == 0) {
            // From ViewCaster - get base form ID
            if (const auto baseObj = targetRef->GetObjectReference()) {
                baseFormID = baseObj->GetFormID();
            }
        }
        
        std::string displayName = "Unknown";
        const char* editorID = "";
        
        if (const auto baseObj = targetRef->GetObjectReference()) {
            if (baseObj->GetFormEditorID()) {
                editorID = baseObj->GetFormEditorID();
            }
            const char* fullName = targetRef->GetDisplayFullName();
            if (fullName && fullName[0] != '\0') {
                displayName = fullName;
            } else if (editorID && editorID[0] != '\0') {
                displayName = editorID;
            }
        }
        
        // Get activator's 3D node info
        RE::NiAVObject* root3D = targetRef->Get3D();
        RE::NiPoint3 activatorWorldPos = root3D->world.translate;
        RE::NiMatrix3 activatorWorldRot = root3D->world.rotate;
        
        // Calculate finger position relative to activator (in activator's local space)
        // First get world-space offset
        RE::NiPoint3 worldOffset;
        worldOffset.x = fingerWorldPos.x - activatorWorldPos.x;
        worldOffset.y = fingerWorldPos.y - activatorWorldPos.y;
        worldOffset.z = fingerWorldPos.z - activatorWorldPos.z;
        
        // Transform to activator's local space by multiplying by inverse rotation
        // For orthonormal rotation matrix, inverse = transpose
        RE::NiMatrix3 invRot = activatorWorldRot.Transpose();
        RE::NiPoint3 relativePos;
        relativePos.x = invRot.entry[0][0] * worldOffset.x + invRot.entry[0][1] * worldOffset.y + invRot.entry[0][2] * worldOffset.z;
        relativePos.y = invRot.entry[1][0] * worldOffset.x + invRot.entry[1][1] * worldOffset.y + invRot.entry[1][2] * worldOffset.z;
        relativePos.z = invRot.entry[2][0] * worldOffset.x + invRot.entry[2][1] * worldOffset.y + invRot.entry[2][2] * worldOffset.z;
        
        spdlog::info("[NodeCaptureMode] Activator '{}' ({:08X}) at ({:.1f}, {:.1f}, {:.1f})",
            displayName, baseFormID, activatorWorldPos.x, activatorWorldPos.y, activatorWorldPos.z);
        spdlog::info("[NodeCaptureMode] Relative position (local space): ({:.2f}, {:.2f}, {:.2f})",
            relativePos.x, relativePos.y, relativePos.z);
        
        // Find closest child node for reference
        std::string closestNodeName = "root";
        // We'll just use root for now - the relative position IS the key data
        
        // Save to JSON
        if (SaveCaptureToJSON(displayName, baseFormID, closestNodeName, 
                              fingerWorldPos, activatorWorldPos, activatorWorldRot, relativePos)) {
            // Success!
            _lastResult.success = true;
            _lastResult.activatorName = displayName;
            _lastResult.nodeName = closestNodeName;
            _lastResult.baseFormID = baseFormID;
            // jsonPath is set in SaveCaptureToJSON
            
            // IMMEDIATELY register with ActivatorHandler so it works without restart
            ActivatorHandler::GetSingleton().RegisterCapturedOffset(
                baseFormID, displayName, relativePos.x, relativePos.y, relativePos.z);
            
            // Show success messages
            char msg1[128];
            snprintf(msg1, sizeof(msg1), "CAPTURED: '%s'", displayName.c_str());
            heisenberg::Hooks::ShowHUDMessageDirect(msg1);
            
            char msg2[256];
            snprintf(msg2, sizeof(msg2), "Saved: %s", _lastResult.jsonPath.c_str());
            heisenberg::Hooks::ShowHUDMessageDirect(msg2);
            
            // Strong haptic feedback
            g_vrInput.TriggerHaptic(true, 15000);
            g_vrInput.TriggerHaptic(false, 15000);
            
            spdlog::info("[NodeCaptureMode] SUCCESS: '{}' saved to {}", displayName, _lastResult.jsonPath);
        } else {
            heisenberg::Hooks::ShowHUDMessageDirect("CAPTURE FAILED: Could not save JSON");
            _lastResult = CaptureResult{};
            _lastResult.success = false;
        }
    }

    bool NodeCaptureMode::SaveCaptureToJSON(const std::string& activatorName,
                                            std::uint32_t baseFormID,
                                            const std::string& closestNodeName,
                                            const RE::NiPoint3& fingerWorldPos,
                                            const RE::NiPoint3& activatorWorldPos,
                                            const RE::NiMatrix3& activatorWorldRotation,
                                            const RE::NiPoint3& relativePos)
    {
        try {
            nlohmann::json j;
            
            // Activator info
            std::stringstream baseFormIDStr;
            baseFormIDStr << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << baseFormID;
            
            j["activator"]["name"] = activatorName;
            j["activator"]["baseFormID"] = baseFormIDStr.str();
            
            // Captured position (relative to activator, in activator's local space)
            j["capturedPosition"]["x"] = relativePos.x;
            j["capturedPosition"]["y"] = relativePos.y;
            j["capturedPosition"]["z"] = relativePos.z;
            
            // Activator rotation at capture time (for reference/debugging)
            j["activatorRotation"]["row0"] = { activatorWorldRotation.entry[0][0], activatorWorldRotation.entry[0][1], activatorWorldRotation.entry[0][2] };
            j["activatorRotation"]["row1"] = { activatorWorldRotation.entry[1][0], activatorWorldRotation.entry[1][1], activatorWorldRotation.entry[1][2] };
            j["activatorRotation"]["row2"] = { activatorWorldRotation.entry[2][0], activatorWorldRotation.entry[2][1], activatorWorldRotation.entry[2][2] };
            
            // Debug info
            j["debug"]["fingerWorldPos"] = { fingerWorldPos.x, fingerWorldPos.y, fingerWorldPos.z };
            j["debug"]["activatorWorldPos"] = { activatorWorldPos.x, activatorWorldPos.y, activatorWorldPos.z };
            j["debug"]["closestNode"] = closestNodeName;
            
            // Create sanitized filename
            std::string safeFileName = activatorName;
            for (char& c : safeFileName) {
                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || 
                    c == '"' || c == '<' || c == '>' || c == '|' || c == ' ') {
                    c = '_';
                }
            }
            
            // Save to F4SE/Plugins folder (where the DLL is located)
            std::string jsonPath = "Data/F4SE/Plugins/ActivatorCaptures/" + safeFileName + ".json";
            
            // Create directory if needed
            std::filesystem::create_directories("Data/F4SE/Plugins/ActivatorCaptures");
            
            // Write JSON file
            std::ofstream outFile(jsonPath, std::ios::out | std::ios::trunc);
            if (outFile.is_open()) {
                outFile << j.dump(2);  // Pretty print
                outFile.close();
                _lastResult.jsonPath = jsonPath;
                spdlog::info("[NodeCaptureMode] Saved JSON to: {}", jsonPath);
                return true;
            } else {
                spdlog::error("[NodeCaptureMode] Failed to open file for writing: {}", jsonPath);
                return false;
            }
        } catch (const std::exception& e) {
            spdlog::error("[NodeCaptureMode] JSON error: {}", e.what());
            return false;
        }
    }

} // namespace heisenberg
