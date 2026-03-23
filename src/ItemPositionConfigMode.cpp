#include "ItemPositionConfigMode.h"
#include <cstring>
#include "Config.h"
#include "F4VROffsets.h"
#include "FRIKInterface.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "Utils.h"
#include "VRInput.h"
#include "WandNodeHelper.h"
#include "common/MatrixUtils.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include "RE/Bethesda/BSInputEnableManager.h"
#include "RE/Bethesda/UserEvents.h"

using namespace f4cf::common;

namespace heisenberg
{
    // Helper to adjust values with deadzone and speed control
    static float correctAdjustmentValue(float value, float speed)
    {
        constexpr float deadzone = 0.15f;
        if (std::abs(value) < deadzone) {
            return 0.0f;
        }
        // Normalize past deadzone and apply speed
        float sign = value > 0 ? 1.0f : -1.0f;
        float normalized = (std::abs(value) - deadzone) / (1.0f - deadzone);
        return sign * normalized / speed;
    }

    // Helper to disable/enable player movement and button controls
    static void SetPlayerControlsEnabled(bool enabled)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            f4cf::f4vr::SetActorRestrained(player, !enabled);
        }
        
        // Disable/enable native game controls (ABXY, VATS, Menu, etc.)
        auto* inputManager = RE::BSInputEnableManager::GetSingleton();
        if (inputManager) {
            // User event flags: MainFour (ABXY), VATS, Activate, Menu
            // NOTE: kFighting intentionally excluded — always suppressed to prevent unarmed
            using UEFlag = RE::UserEvents::USER_EVENT_FLAG;
            UEFlag flagsToControl = UEFlag::kMainFour | UEFlag::kVATS | UEFlag::kActivate | UEFlag::kMenu;
            inputManager->ForceUserEventEnabled(flagsToControl, enabled);
            
            // Other event flags: VATS, Activation, Favorites (needed for VR)
            using OEFlag = RE::OtherInputEvents::OTHER_EVENT_FLAG;
            OEFlag otherFlags = OEFlag::kVATS | OEFlag::kActivation | OEFlag::kFavorites;
            inputManager->ForceOtherEventEnabledVR(otherFlags, enabled);
        }
    }

    ItemPositionConfigMode::~ItemPositionConfigMode()
    {
        // Make sure controls are re-enabled if we're destroyed while active
        if (_repositionModeActive) {
            SetPlayerControlsEnabled(true);
        }
    }

    void ItemPositionConfigMode::Initialize()
    {
        if (_initialized) {
            return;
        }

        _initialized = true;
        
        // Check if item positioning is enabled
        if (!g_config.enableItemPositioning) {
            spdlog::info("Item position config mode disabled (enable with bEnableItemPositioning=true in INI)");
            return;
        }
        
        // Log the configured shortcut at debug level only
        const char* shortcutName = "Left Thumbstick Click";
        switch (g_config.itemPositioningShortcut) {
            case 0: shortcutName = "Left Thumbstick Click"; break;
            case 1: shortcutName = "Right Thumbstick Click"; break;
            case 2: shortcutName = "Long Press A"; break;
        }
        spdlog::debug("[ItemPositionConfig] Controls: While holding object, {} (1s) to toggle reposition mode", shortcutName);
    }

    void ItemPositionConfigMode::OnFrameUpdate(float deltaTime)
    {
        // Check if item positioning is enabled in config
        if (!g_config.enableItemPositioning) {
            if (_repositionModeActive) {
                // Force exit if disabled while active
                _repositionModeActive = false;
                SetPlayerControlsEnabled(true);  // Re-enable player controls
            }
            return;
        }
        
        auto& vrInput = VRInput::GetSingleton();
        auto& grabMgr = GrabManager::GetSingleton();

        // Check for reposition mode toggle based on configured shortcut
        // 0 = Left Thumbstick Click, 1 = Right Thumbstick Click, 2 = Long Press A
        // Edge detection uses member variables (reset by ClearAllState on save/load)
        
        // Check if holding an object in EITHER hand (query GrabManager directly)
        bool holdingObject = grabMgr.IsGrabbing(true) || grabMgr.IsGrabbing(false);
        
        // Check the configured shortcut button
        bool shortcutPressed = false;
        bool hapticOnLeft = true;  // Which hand gets haptic feedback
        
        switch (g_config.itemPositioningShortcut) {
            case 0:  // Left Thumbstick Click
                shortcutPressed = vrInput.IsPressed(true, VRButton::ThumbstickPress);
                hapticOnLeft = true;
                break;
            case 1:  // Right Thumbstick Click
                shortcutPressed = vrInput.IsPressed(false, VRButton::ThumbstickPress);
                hapticOnLeft = false;
                break;
            case 2:  // Long Press A
                shortcutPressed = vrInput.IsPressed(false, VRButton::A);
                hapticOnLeft = false;
                break;
            default:
                shortcutPressed = vrInput.IsPressed(true, VRButton::ThumbstickPress);
                hapticOnLeft = true;
                break;
        }
        
        if (holdingObject && shortcutPressed) {
            _stickHoldTime += deltaTime;

            // Trigger once at 1 second hold
            if (_stickHoldTime >= 1.0f && !_stickTriggered) {
                _stickTriggered = true;  // Mark as triggered, won't trigger again until released
                ToggleRepositionMode();
                vrInput.TriggerHaptic(hapticOnLeft, 5000);
            }
        } else {
            _stickHoldTime = 0.0f;
            _stickTriggered = false;  // Reset when button released or not holding object
        }

        // If reposition mode is not active, nothing more to do
        if (!_repositionModeActive) {
            return;
        }
        
        // Re-apply input suppression periodically (every ~0.5 seconds) while in reposition mode
        // The game may reset the flags, but we don't need to do it every frame
        _suppressionTimer += deltaTime;
        if (_suppressionTimer >= 0.5f) {
            _suppressionTimer = 0.0f;
            SetPlayerControlsEnabled(false);
        }

        // If we have an active grab, handle repositioning
        if (_currentGrabState && _currentGrabState->active) {
            HandleRepositionInput();
            
            // Update the grab state's item offset
            _currentGrabState->itemOffset = _currentOffset;
            
            // Re-apply suppression immediately before checking buttons that might conflict
            // This ensures VATS/native controls don't trigger when we detect B/Y
            SetPlayerControlsEnabled(false);
            
            // Check for save (B button on right hand)
            bool bPressed = vrInput.IsPressed(false, VRButton::B);
            if (bPressed && !_bWasPressed) {
                SaveCurrentOffset();
            }
            _bWasPressed = bPressed;

            // Check for reset (Y button on left hand - button index 1)
            // Note: Y on Oculus is button 1 on left controller
            bool yPressed = vrInput.IsPressed(true, VRButton::Y);
            if (yPressed && !_yWasPressed) {
                ResetCurrentOffset();
                vrInput.TriggerHaptic(true, 3000);  // Short haptic for reset
            }
            _yWasPressed = yPressed;
        }
    }

    void ItemPositionConfigMode::ToggleConfigUI()
    {
        // No UI in button-based mode - just toggle reposition mode
        ToggleRepositionMode();
    }

    void ItemPositionConfigMode::ToggleRepositionMode()
    {
        _repositionModeActive = !_repositionModeActive;
        spdlog::info("[ItemPositionConfig] Reposition mode: {}", _repositionModeActive ? "ON" : "OFF");
        
        auto& frik = FRIKInterface::GetSingleton();
        auto& grabMgr = GrabManager::GetSingleton();
        
        if (_repositionModeActive) {
            // Reset saved flag for new session
            _offsetSavedThisSession = false;
            
            // Get the current grab state from GrabManager
            // Check which hand is grabbing and use that state
            bool leftGrabbing = grabMgr.IsGrabbing(true);
            bool rightGrabbing = grabMgr.IsGrabbing(false);
            
            if (leftGrabbing || rightGrabbing) {
                _isLeftHand = leftGrabbing;  // Prefer left if both (unlikely)
                GrabState& grabState = grabMgr.GetGrabState(_isLeftHand);
                RE::TESObjectREFR* grabRefr = grabState.GetRefr();
                
                if (grabState.active && grabRefr) {
                    _currentGrabState = &grabState;
                    grabState.stickyGrab = true;  // Enable sticky grab
                    _currentItemName = ItemOffsetManager::GetItemName(grabRefr);
                    
                    // Get current offset or default
                    auto& offsetMgr = ItemOffsetManager::GetSingleton();
                    auto existingOffset = offsetMgr.GetOffset(_currentItemName);
                    if (existingOffset.has_value()) {
                        _currentOffset = existingOffset.value();
                        spdlog::info("[ItemPositionConfig] Loaded existing offset for: {}", _currentItemName);
                    } else {
                        _currentOffset = offsetMgr.GetDefaultOffset();
                        spdlog::info("[ItemPositionConfig] Using default offset for: {}", _currentItemName);
                    }
                    
                    // Copy dimensions from grab state
                    _currentOffset.length = grabState.itemOffset.length;
                    _currentOffset.width = grabState.itemOffset.width;
                    _currentOffset.height = grabState.itemOffset.height;
                    _currentOffset.itemType = ItemOffsetManager::GetItemType(grabRefr);
                    _currentOffset.formId = ItemOffsetManager::GetItemFormId(grabRefr);
                    
                    // Set frozen position from current object world position
                    // NOTE: Fetch fresh wandNode from playerNodes instead of using cached
                    // grabState.wandNode to avoid any stale reference issues
                    auto* freshPlayerNodes = f4cf::f4vr::getPlayerNodes();
                    RE::NiNode* freshWandNode = freshPlayerNodes ?
                        heisenberg::GetWandNode(freshPlayerNodes, _isLeftHand) : nullptr;
                    if (grabState.node && freshWandNode) {
                        SetFrozenWorldTransform(
                            grabState.node->world.translate,
                            grabState.node->world.rotate,
                            freshWandNode->world.translate,
                            freshWandNode->world.rotate
                        );
                        spdlog::info("[ItemPositionConfig] Set frozen position from current grab");
                    }
                    
                    spdlog::info("[ItemPositionConfig] Item: {} Dims: {:.1f}x{:.1f}x{:.1f}",
                                 _currentItemName, _currentOffset.length, _currentOffset.width, _currentOffset.height);
                }
            }
            
            // Disable player movement so thumbsticks don't move player
            SetPlayerControlsEnabled(false);
            spdlog::info("[ItemPositionConfig] Player controls disabled for repositioning");
            spdlog::info("[ItemPositionConfig] Grab an item - it will stay grabbed for adjustment");
            spdlog::info("[ItemPositionConfig] Thumbsticks = position, Grip+Thumbsticks = rotation");
            spdlog::info("[ItemPositionConfig] B = Save, Y = Reset, Long-press stick = Exit");
            
            // Show HUD message to user with controls
            char msg[192];
            snprintf(msg, sizeof(msg), "ITEM REPOSITION MODE | %s | Sticks=Move, R-Grip+Sticks=Rotate | B=Save, Y=Reset",
                     _currentItemName.empty() ? "No item" : _currentItemName.c_str());
            heisenberg::ShowHUDMessage_VR(msg, nullptr, false, false);
            
            // Set fingers fully extended on both hands for better visibility
            if (frik.IsAvailable()) {
                frik.SetHandPoseFingerPositions(true, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);   // Left hand
                frik.SetHandPoseFingerPositions(false, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);  // Right hand
                spdlog::info("[ItemPositionConfig] Set hands to open pose for repositioning");
            }
        } else {
            // Re-enable player movement
            SetPlayerControlsEnabled(true);
            spdlog::info("[ItemPositionConfig] Player controls re-enabled");
            
            // Show exit message
            heisenberg::ShowHUDMessage_VR("Item Reposition Mode EXITED", nullptr, false, false);
            VRInput::GetSingleton().TriggerHaptic(false, 30000);
            
            // Restore hand poses
            if (frik.IsAvailable()) {
                frik.ClearHandPoseFingerPositions(true);   // Left hand
                frik.ClearHandPoseFingerPositions(false);  // Right hand
                spdlog::info("[ItemPositionConfig] Restored hand poses to default");
            }
            
            // Reset frozen position
            _hasFrozenPosition = false;
            
            // If offset was saved, keep the object in hand; otherwise drop it
            if (_currentGrabState && _currentGrabState->active) {
                _currentGrabState->stickyGrab = false;
                
                if (_offsetSavedThisSession) {
                    // Keep the object - just exit sticky mode
                    spdlog::info("[ItemPositionConfig] Offset saved - keeping item in hand");
                    heisenberg::ShowHUDMessage_VR("Offset saved - item kept in hand", nullptr, false, false);
                } else {
                    // No offset saved - drop the object
                    auto& grabMgr = GrabManager::GetSingleton();
                    grabMgr.EndGrab(_isLeftHand, nullptr);  // Drop without throwing
                    spdlog::info("[ItemPositionConfig] No offset saved - released item");
                }
            }
            _currentGrabState = nullptr;
            _currentItemName.clear();
        }
    }

    void ItemPositionConfigMode::OnGrabStarted(GrabState* grabState, bool isLeft)
    {
        RE::TESObjectREFR* grabRefr = grabState ? grabState->GetRefr() : nullptr;
        if (!grabState || !grabRefr) {
            return;
        }

        // If reposition mode is active, enable sticky grab on this item
        if (_repositionModeActive) {
            grabState->stickyGrab = true;
            _currentGrabState = grabState;
            _isLeftHand = isLeft;
            _currentItemName = ItemOffsetManager::GetItemName(grabRefr);
            
            // Get current offset or default
            auto& offsetMgr = ItemOffsetManager::GetSingleton();
            auto existingOffset = offsetMgr.GetOffset(_currentItemName);
            if (existingOffset.has_value()) {
                _currentOffset = existingOffset.value();
                spdlog::info("[ItemPositionConfig] Loaded existing offset for: {}", _currentItemName);
            } else {
                _currentOffset = offsetMgr.GetDefaultOffset();
                spdlog::info("[ItemPositionConfig] Using default offset for: {}", _currentItemName);
            }
            
            // Copy dimensions from grab state (populated by GetItemDimensions in StartGrab)
            _currentOffset.length = grabState->itemOffset.length;
            _currentOffset.width = grabState->itemOffset.width;
            _currentOffset.height = grabState->itemOffset.height;
            
            // Get item type and form ID for metadata
            _currentOffset.itemType = ItemOffsetManager::GetItemType(grabRefr);
            _currentOffset.formId = ItemOffsetManager::GetItemFormId(grabRefr);
            
            spdlog::info("[ItemPositionConfig] Item: {} [{}] FormID: {} Dims: {:.1f}x{:.1f}x{:.1f}",
                         _currentItemName, _currentOffset.itemType, _currentOffset.formId,
                         _currentOffset.length, _currentOffset.width, _currentOffset.height);
            
            spdlog::info("[ItemPositionConfig] Sticky grab active - adjust with thumbsticks");
        }
    }

    void ItemPositionConfigMode::OnGrabEnded(bool isLeft)
    {
        if (_currentGrabState && _isLeftHand == isLeft) {
            _currentGrabState = nullptr;
            _currentItemName.clear();
            spdlog::info("[ItemPositionConfig] Grab ended");
        }
    }
    
    void ItemPositionConfigMode::ClearAllState()
    {
        spdlog::info("[ItemPositionConfig] ClearAllState - resetting all state");
        
        // Exit reposition mode if active
        if (_repositionModeActive) {
            spdlog::info("[ItemPositionConfig] Disabling reposition mode");
            _repositionModeActive = false;
            
            // Restore player controls if they were disabled
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                auto* controls = RE::PlayerControls::GetSingleton();
                if (controls) {
                    controls->blockPlayerInput = false;
                }
            }
        }
        
        // Clear all state
        _currentGrabState = nullptr;
        _currentItemName.clear();
        _hasFrozenPosition = false;

        // Reset input edge detection state (prevents stale triggers after load)
        _stickHoldTime = 0.0f;
        _stickTriggered = false;
        _suppressionTimer = 0.0f;
        _bWasPressed = false;
        _yWasPressed = false;
    }

    void ItemPositionConfigMode::CreateConfigUI()
    {
        // No UI in button-based mode
        spdlog::info("[ItemPositionConfig] UI not available - using button controls");
    }

    void ItemPositionConfigMode::CloseConfigUI()
    {
        // No UI to close
    }

    void ItemPositionConfigMode::HandleRepositionInput()
    {
        if (!_hasFrozenPosition) {
            return;  // Wait for frozen position to be set
        }
        
        auto& vrInput = VRInput::GetSingleton();
        
        // FRIK-style control scheme:
        // Adjustments are in WORLD space so item stays visually stable
        // Primary (right) stick Y = forward/backward (world Y)
        // Primary (right) stick X = left/right (world X)  
        // Offhand (left) stick Y = up/down (world Z)
        // Offhand grip + sticks = rotation
        
        float primAxisX = vrInput.GetThumbstickX(false);  // Right controller
        float primAxisY = vrInput.GetThumbstickY(false);
        float secAxisX = vrInput.GetThumbstickX(true);    // Left controller (offhand)
        float secAxisY = vrInput.GetThumbstickY(true);

        if (primAxisX == 0.0f && primAxisY == 0.0f && secAxisX == 0.0f && secAxisY == 0.0f) {
            return;
        }

        // Check if primary (right) grip is held for rotation mode
        bool primaryGripHeld = vrInput.IsPressed(false, VRButton::Grip);

        if (primaryGripHeld) {
            // Rotation mode - adjust frozen world rotation
            auto rot = MatrixUtils::getMatrixFromEulerAngles(
                -MatrixUtils::degreesToRads(correctAdjustmentValue(primAxisY, 5)),   // Pitch
                -MatrixUtils::degreesToRads(correctAdjustmentValue(secAxisX, 3)),    // Roll
                MatrixUtils::degreesToRads(correctAdjustmentValue(primAxisX, 5)));   // Yaw
            AdjustFrozenRotation(rot);
        } else {
            // Position mode - adjust frozen world position
            // Use world axes so movement is intuitive regardless of hand orientation
            RE::NiPoint3 delta;
            delta.x = correctAdjustmentValue(primAxisX, 12);   // World left/right
            delta.y = correctAdjustmentValue(primAxisY, 12);   // World forward/back
            delta.z = -correctAdjustmentValue(secAxisY, 12);   // World up/down
            AdjustFrozenPosition(delta);
        }
    }

    void ItemPositionConfigMode::SaveCurrentOffset()
    {
        if (_currentItemName.empty() || !_hasFrozenPosition) {
            spdlog::info("[ItemPositionConfig] Nothing to save - no item grabbed or no position set");
            return;
        }

        // Get CURRENT hand position/rotation at save time
        // This is more intuitive: "wherever my hand is now, that's the reference point"
        RE::NiPoint3 handPos;
        RE::NiMatrix3 handRot;
        
        // Get current hand transform from the wand node
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (playerNodes) {
            RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, _isLeftHand);
            if (wandNode) {
                handPos = wandNode->world.translate;
                handRot = wandNode->world.rotate;
            } else {
                // Fallback to frozen hand pos if wand node not available
                handPos = _frozenHandPos;
                handRot = _frozenHandRot;
            }
        } else {
            // Fallback to frozen hand pos
            handPos = _frozenHandPos;
            handRot = _frozenHandRot;
        }
        
        // Calculate RELATIVE offset from CURRENT hand position to frozen world item position
        // FRIK forward formula: world.translate = parent.translate + parent.rotate.T * local.translate
        // Inverse: local.translate = parent.rotate * (world.translate - parent.translate)
        RE::NiPoint3 worldOffset = _frozenWorldPos - handPos;
        RE::NiPoint3 localOffset = handRot * worldOffset;
        
        // Calculate relative rotation
        // FRIK forward formula: world.rotate = local.rotate * parent.rotate
        // Inverse: local.rotate = world.rotate * parent.rotate.T
        RE::NiMatrix3 localRot = _frozenWorldRot * handRot.Transpose();
        
        // Store in _currentOffset
        _currentOffset.position = localOffset;
        _currentOffset.rotation = localRot;
        
        // Calculate finger distance from FRIK finger tip to item position
        // This allows automatic finger curl adjustment when grabbing
        auto& frik = FRIKInterface::GetSingleton();
        if (frik.IsAvailable()) {
            RE::NiPoint3 fingerTipPos;
            if (frik.GetIndexFingerTipPosition(_isLeftHand, fingerTipPos)) {
                // Distance from finger tip to frozen item world position
                float fingerDist = (_frozenWorldPos - fingerTipPos).Length();
                _currentOffset.fingerDistance = fingerDist;
                spdlog::info("[ItemPositionConfig]   Finger distance: {:.2f}", fingerDist);
            }
        }
        
        // Save per-joint finger curl values from the animator
        auto& animator = Heisenberg::GetSingleton().GetFingerAnimator(_isLeftHand);
        const float* joints = animator.GetCurrentValues();
        std::memcpy(_currentOffset.jointCurls, joints, sizeof(_currentOffset.jointCurls));
        _currentOffset.hasJointCurls = true;
        // Also save as 5-value for backward compat (proximal joint of each finger)
        _currentOffset.thumbCurl = joints[0];
        _currentOffset.indexCurl = joints[3];
        _currentOffset.middleCurl = joints[6];
        _currentOffset.ringCurl = joints[9];
        _currentOffset.pinkyCurl = joints[12];
        _currentOffset.hasFingerCurls = true;
        spdlog::info("[ItemPositionConfig]   Finger curl (avg): {:.2f}", animator.GetAverageCurl());
        
        // Also update the grab state so it takes effect immediately
        if (_currentGrabState) {
            _currentGrabState->itemOffset = _currentOffset;
        }

        auto& offsetMgr = ItemOffsetManager::GetSingleton();
        offsetMgr.SaveOffset(_currentItemName, _currentOffset, _isLeftHand);
        _offsetSavedThisSession = true;  // Mark that we saved an offset this session
        
        spdlog::info("[ItemPositionConfig] Saved RELATIVE offset for '{}' ({} hand{})", 
                     _currentItemName, 
                     _isLeftHand ? "LEFT" : "RIGHT",
                     Utils::IsPlayerInPowerArmor() ? ", Power Armor" : "");
        spdlog::info("[ItemPositionConfig]   Frozen item pos: ({:.2f}, {:.2f}, {:.2f})",
                     _frozenWorldPos.x, _frozenWorldPos.y, _frozenWorldPos.z);
        spdlog::info("[ItemPositionConfig]   Current hand pos at save: ({:.2f}, {:.2f}, {:.2f})",
                     handPos.x, handPos.y, handPos.z);
        spdlog::info("[ItemPositionConfig]   Local offset: ({:.2f}, {:.2f}, {:.2f})",
                     localOffset.x, localOffset.y, localOffset.z);
        
        // Show save confirmation HUD message
        char msg[128];
        snprintf(msg, sizeof(msg), "SAVED! Offset for: %s | Hold stick to exit", _currentItemName.c_str());
        heisenberg::ShowHUDMessage_VR(msg, nullptr, false, false);
        
        // Strong haptic feedback for save
        VRInput::GetSingleton().TriggerHaptic(false, 50000);  // Right hand - strong like zone config
        VRInput::GetSingleton().TriggerHaptic(true, 50000);   // Left hand too
    }

    void ItemPositionConfigMode::ResetCurrentOffset()
    {
        auto& offsetMgr = ItemOffsetManager::GetSingleton();
        _currentOffset = offsetMgr.GetDefaultOffset();
        
        if (_currentGrabState) {
            _currentGrabState->itemOffset = _currentOffset;
        }
        
        // Also reset frozen position to recalculate from default offset
        _hasFrozenPosition = false;

        spdlog::info("[ItemPositionConfig] Reset offset for '{}'", _currentItemName);
    }

    void ItemPositionConfigMode::SetFrozenWorldTransform(const RE::NiPoint3& itemPos, const RE::NiMatrix3& itemRot,
                                                          const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot)
    {
        _frozenWorldPos = itemPos;
        _frozenWorldRot = itemRot;
        _frozenHandPos = handPos;
        _frozenHandRot = handRot;
        _hasFrozenPosition = true;
        spdlog::info("[ItemPositionConfig] Frozen item world position: ({:.2f}, {:.2f}, {:.2f})",
                     itemPos.x, itemPos.y, itemPos.z);
        spdlog::info("[ItemPositionConfig] Frozen hand position: ({:.2f}, {:.2f}, {:.2f})",
                     handPos.x, handPos.y, handPos.z);
    }

    void ItemPositionConfigMode::AdjustFrozenPosition(const RE::NiPoint3& delta)
    {
        _frozenWorldPos += delta;
    }

    void ItemPositionConfigMode::AdjustFrozenRotation(const RE::NiMatrix3& rot)
    {
        _frozenWorldRot = rot * _frozenWorldRot;
    }
}
