#include "Hand.h"

#include "ActivatorHandler.h"
#include "Config.h"
#include "F4VROffsets.h"
#include "FRIKInterface.h"
#include "Grab.h"
#include "Heisenberg.h"
#include "Highlight.h"
#include "Hooks.h"
#include "MenuChecker.h"
#include "Physics.h"
#include "SmartGrabHandler.h"
#include "Utils.h"
#include "VRInput.h"
#include "WandNodeHelper.h"

#include <f4vr/PlayerNodes.h>
#include <RE/Bethesda/UI.h>

#include <RE/Bethesda/BSStringT.h>
#include <RE/Bethesda/TESBoundObjects.h>

#include <cmath>

namespace
{
    // Returns true if any blocking menu is open (pause, pipboy, inventory, etc.)
    // Uses event-based MenuChecker for thread safety instead of direct UI calls
    bool IsBlockingMenuOpen()
    {
        return heisenberg::MenuChecker::GetSingleton().IsGameStopped();
    }

    // Get the full display name of an equipped weapon (includes mod names like "Laser Pistol").
    // Searches the player's inventory for the form and reads ExtraTextDisplayData.
    // Falls back to GetFullName() if no ETTD found.
    std::string GetWeaponDisplayName(RE::TESForm* weaponForm)
    {
        if (!weaponForm) return "Weapon";

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->inventoryList) {
            for (auto& invItem : player->inventoryList->data) {
                if (invItem.object != weaponForm) continue;

                auto* stack = invItem.stackData.get();
                while (stack) {
                    if (stack->extra) {
                        auto* ettd = stack->extra->GetByType<RE::ExtraTextDisplayData>();
                        if (ettd && !ettd->displayName.empty()) {
                            return std::string(ettd->displayName.c_str());
                        }
                    }
                    stack = stack->nextStack.get();
                }
                break;  // Found the form, no ETTD — fall through to base name
            }
        }

        auto nameView = RE::TESFullName::GetFullName(*weaponForm, false);
        return nameView.empty() ? "Weapon" : std::string(nameView);
    }
}

namespace heisenberg
{
    Hand::Hand(bool isLeft) :
        _isLeft(isLeft),
        _handNodeName(isLeft ? "LArm_Hand" : "RArm_Hand"),
        _velocityHistory(5, RE::NiPoint3())
    {
        spdlog::debug("Created {} hand", isLeft ? "left" : "right");
    }

    bool Hand::IsPrimaryHand() const
    {
        return VRInput::GetSingleton().IsLeftHandedMode() ? _isLeft : !_isLeft;
    }

    bool Hand::IsOffHand() const
    {
        return !IsPrimaryHand();
    }

    void Hand::Update()
    {
        // =========================================================================
        // WORKSHOP MODE CHECK - Disable all Heisenberg processing in workshop
        // =========================================================================
        // In workshop mode, grip is used for power wire connections and other
        // workshop functions. We must completely disable Heisenberg to allow the
        // native grip functionality to work.
        if (IsBlockingMenuOpen()) {
            // Only update tracking (needed for other mods like FRIK)
            UpdateTracking();

            // Clear any selection and transition to idle
            if (_selection.IsValid()) {
                RE::TESObjectREFR* selRefr = _selection.GetRefr();
                if (selRefr && g_config.enableHighlighting) {
                    Highlighter::GetSingleton().StopHighlight(selRefr);
                }
                _selection.Clear();
            }
            if (_state != State::Idle && _state != State::Held && _state != State::Pulling) {
                _state = State::Idle;
            }

            // IMPORTANT: Still update held objects during menus!
            // DropToHand can grab items while Pipboy is open, and we need
            // UpdateGrab to run so KEYFRAMED physics gets set up.
            auto& grabMgr = GrabManager::GetSingleton();
            const auto& grabState = grabMgr.GetGrabState(_isLeft);

            // While Pipboy (or other blocking menu) is open, do NOT react to grip
            // input — the player may press grip to close the Pipboy, and that should
            // not drop held items. Passively track grip state so there is no stale
            // transition when the menu closes.
            if (grabState.active) {
                _grabPressed = g_vrInput.IsPressed(_isLeft, VRButton::Grip);
            } else {
                _grabPressed = false;
            }

            if (grabState.active) {
                if (_state != State::Held && _state != State::Pulling) {
                    _state = grabState.isPulling ? State::Pulling : State::Held;
                }
                UpdateHeldObject(_lastDeltaTime);
            }

            return;
        }
        
        UpdateTracking();
        UpdateSelection();  // Find nearby objects first
        UpdateState();      // Transition states based on selection
        UpdateInput();      // Check VR controller input (now state is correct)

        // Sync Hand state with GrabManager - handles grabs started by DropToHand
        // which bypass the normal Hand state machine
        auto& grabMgr = GrabManager::GetSingleton();
        const auto& grabState = grabMgr.GetGrabState(_isLeft);
        if (grabState.active && _state != State::Held && _state != State::Pulling) {
            // GrabManager has an active grab but Hand doesn't know about it
            // This happens when DropToHand grabs directly
            spdlog::debug("[SYNC] {} hand: Syncing state to Held (GrabManager has active grab)",
                         _isLeft ? "Left" : "Right");
            _state = grabState.isPulling ? State::Pulling : State::Held;
        }

        // Update held object if we're holding something
        if (IsHolding()) {
            UpdateHeldObject(_lastDeltaTime);
            
            // Check if pull animation is complete - transition to Held
            if (_state == State::Pulling && !grabMgr.IsPulling(_isLeft)) {
                spdlog::debug("[STATE] {} hand: Pulling -> Held (pull complete)",
                             _isLeft ? "Left" : "Right");
                TransitionToHeld();
            }
        }
        
        // Check for nearby activators (buttons, switches) and handle touch activation
        UpdateActivatorProximity();
        
        // Always try to override native grab position (for A/X grabbed objects)
        grabMgr.OverrideNativeGrabPosition(_position);
    }

    void Hand::UpdateInput()
    {
        // Check grip button state from VR controllers
        bool gripPressed = g_vrInput.IsPressed(_isLeft, VRButton::Grip);
        float gripValue = g_vrInput.GetGripValue(_isLeft);

        // DEBUG: Log grip state periodically
        static int frameCounter = 0;
        if (++frameCounter % 90 == 0) {  // Every ~1 second at 90Hz
            spdlog::debug("[INPUT DEBUG] {} hand: gripValue={:.2f} gripPressed={} _grabPressed={} state={}",
                        _isLeft ? "Left" : "Right", gripValue, gripPressed, _grabPressed, static_cast<int>(_state));
        }

        // =========================================================================
        // FRIK TWO-HANDED MODE DETECTION
        // =========================================================================
        // When FRIK is in two-handed weapon mode, the offhand grip is used to grip
        // the weapon. We should NOT intercept this grip for Heisenberg object grabbing.
        // Only skip for the OFF-hand - primary hand grip should still work for dropping objects.
        // Off-hand = left normally, right in left-handed mode
        auto& frik = FRIKInterface::GetSingleton();
        bool frikTwoHandedActive = frik.IsOffHandGrippingWeapon();
        
        // If this is the offhand and FRIK is in two-handed mode, don't process Heisenberg grab
        if (IsOffHand() && frikTwoHandedActive)
        {
            // If we're holding something and FRIK just took over, release it
            if (_state == State::Held || _state == State::Pulling)
            {
                spdlog::debug("[FRIK] Offhand entering two-handed mode - releasing grab");
                auto& grabMgr = GrabManager::GetSingleton();
                grabMgr.EndGrab(_isLeft, nullptr);  // Drop without throw
                TransitionToIdle();
            }
            
            // Don't process grip input for Heisenberg while in two-handed mode
            _grabPressed = gripPressed;  // Track state but don't act on it
            return;
        }

        // Detect press/release transitions
        if (gripPressed && !_grabPressed) {
            RE::TESObjectREFR* selRefr = _selection.GetRefr();
            spdlog::debug("[INPUT] {} grip PRESSED (state={}, selection={})", 
                         _isLeft ? "Left" : "Right", 
                         static_cast<int>(_state),
                         selRefr ? selRefr->formID : 0);
            OnGrabPressed();
        } else if (!gripPressed && _grabPressed) {
            spdlog::debug("[INPUT] {} grip RELEASED (state={})", 
                         _isLeft ? "Left" : "Right",
                         static_cast<int>(_state));
            OnGrabReleased();
        }
    }

    void Hand::UpdateTracking()
    {
        // Get position and rotation from VR controller wand nodes
        // These are the most reliable source - directly from OpenVR
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) {
            return;
        }
        
        // In Fallout 4 VR: primaryWandNode and SecondaryWandNode SWAP in left-handed mode.
        // Normal:  primaryWandNode = RIGHT physical,  SecondaryWandNode = LEFT physical
        // LH mode: primaryWandNode = LEFT physical,   SecondaryWandNode = RIGHT physical
        // GetWandNode() accounts for this swap so callers just pass isLeft (physical).
        RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, _isLeft);
        if (!wandNode) {
            return;
        }
        
        // Get BOTH position and rotation from wand node
        _prevPosition = _position;
        _position = wandNode->world.translate;
        _rotation = wandNode->world.rotate;
        
        // Calculate velocity
        float dt = 1.0f / 90.0f; // Assume 90Hz for now - TODO: get actual frame time
        _lastDeltaTime = dt;
        if (dt > 0.0f) {
            RE::NiPoint3 newVelocity = (_position - _prevPosition) / dt;
            
            // Add to history and compute average
            _velocityHistory.pop_back();
            _velocityHistory.push_front(newVelocity);
            
            _velocity = RE::NiPoint3();
            for (const auto& v : _velocityHistory) {
                _velocity += v;
            }
            _velocity /= static_cast<float>(_velocityHistory.size());
        }
        
        // PERF: Removed periodic tracking log - was causing unnecessary log IO
        // Enable debugLogging in config if you need to verify hand tracking
    }

    void Hand::UpdateSelection()
    {
        if (_state == State::Held || _state == State::Pulling) {
            // Don't update selection while holding or pulling
            return;
        }

        // =====================================================================
        // SIMPLE VIEWCASTER-ONLY SELECTION
        // Check what the game's native wand laser is pointing at.
        // This is the authoritative source for "Press A/X to store" targets.
        // =====================================================================
        
        RE::ObjectRefHandle targetHandle = GetVRWandTargetHandle(_isLeft);
        
        // Check if target changed from last frame
        bool targetChanged = (targetHandle != _lastViewCasterHandle);
        _lastViewCasterHandle = targetHandle;
        
        if (!targetHandle) {
            // No target - clear selection if we had one
            if (_selection.IsValid()) {
                _selection.Clear();
            }
            return;
        }
        
        // Only process if target changed (avoid redundant work)
        if (!targetChanged && _selection.IsValid()) {
            return;  // Same target, already selected
        }
        
        // Get reference from handle
        RE::NiPointer<RE::TESObjectREFR> refrPtr = targetHandle.get();
        RE::TESObjectREFR* refr = refrPtr.get();
        
        if (!refr) {
            _selection.Clear();
            return;
        }
        
        // Validate it's a grabbable object type
        if (!Physics::IsGrabbable(refr)) {
            _selection.Clear();
            return;
        }

        // Verify the game would show an activation prompt ("Press A/X to store")
        // Prevents phantom grabs on objects the UI doesn't highlight
        RE::TESBoundObject* baseObj = refr->GetObjectReference();
        if (baseObj) {
            RE::BSStringT<char> activateText;
            if (!baseObj->GetActivateText(refr, activateText)) {
                _selection.Clear();
                return;
            }
        }

        // Get the 3D node for distance calculation
        RE::NiAVObject* node = refr->Get3D();
        if (!node) {
            _selection.Clear();
            return;
        }
        
        // Calculate distance from hand
        RE::NiPoint3 objPos = node->world.translate;
        float dx = objPos.x - _position.x;
        float dy = objPos.y - _position.y;
        float dz = objPos.z - _position.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        
        // Set the selection
        Selection newSelection;
        newSelection.SetRefr(refr);
        newSelection.hitPoint = objPos;
        newSelection.hitNormal = RE::NiPoint3(0, 0, 1);
        newSelection.distance = distance;
        newSelection.isClose = (distance <= Config::GetSingleton().closeGrabThreshold);
        _selection = newSelection;
        _lastSelectionTime = Utils::GetTime();
        
        // Log only when target changes (formID is different)
        if (refr->formID != _lastLoggedFormID) {
            _lastLoggedFormID = refr->formID;
            spdlog::debug("[SELECT-VC] {} hand: ViewCaster selected {:08X} at {:.1f} units",
                         _isLeft ? "Left" : "Right", refr->formID, distance);
        }
    }

    void Hand::UpdateState()
    {
        double currentTime = Utils::GetTime();

        switch (_state) {
        case State::Idle:
            // Check for nearby objects
            if (_selection.IsValid()) {
                RE::TESObjectREFR* selRefr = _selection.GetRefr();
                spdlog::debug("[STATE] {} hand: Idle -> SelectedClose (ref {:08X} at {:.1f} units)",
                             _isLeft ? "Left" : "Right",
                             selRefr ? selRefr->formID : 0,
                             _selection.distance);
                TransitionToSelectedClose(_selection);
            }
            break;

        case State::SelectedClose:
            // Check if selection is still valid
            if (!_selection.IsValid()) {
                if (currentTime - _lastSelectionTime > kSelectionLeewayTime) {
                    spdlog::debug("[STATE] {} hand: SelectedClose -> Idle (selection lost)",
                                 _isLeft ? "Left" : "Right");
                    TransitionToIdle();
                }
            }
            // Grab is handled via OnGrabPressed()
            break;

        case State::Pulling:
            // Object is being pulled toward hand - handled in UpdateHeldObject
            break;

        case State::Held:
            // Object is in hand - release handled via OnGrabReleased()
            // Safety: if GrabManager says we're NOT grabbing (e.g. EndGrab-for-storage
            // cleared GrabState but didn't reset Hand._state), auto-recover to Idle.
            {
                auto& grabMgr = GrabManager::GetSingleton();
                if (!grabMgr.IsGrabbing(_isLeft)) {
                    spdlog::debug("[STATE] {} hand: Held -> Idle (GrabManager not active — auto-recover)",
                                 _isLeft ? "Left" : "Right");
                    TransitionToIdle();
                }
            }
            break;
        }
    }

    void Hand::UpdateActivatorProximity()
    {
        // Skip if activators are disabled
        if (!g_config.enableInteractiveActivators) {
            _isPointingAtActivator = false;
            _isInActivatorRange = false;
            _nearbyActivatorHandle.reset();
            return;
        }

        // Don't check activators while holding or pulling objects
        if (_state == State::Held || _state == State::Pulling) {
            _isPointingAtActivator = false;
            _isInActivatorRange = false;
            _nearbyActivatorHandle.reset();
            return;
        }

        // Get finger tip position from FRIK
        auto& frik = FRIKInterface::GetSingleton();
        RE::NiPoint3 fingerTipPos;
        
        if (!frik.GetIndexFingerTipPosition(_isLeft, fingerTipPos)) {
            // Fallback: use wand position with forward offset (approximate finger tip)
            // Index finger is roughly 10cm forward from wand
            RE::NiPoint3 forward = { _rotation.entry[0][1], _rotation.entry[1][1], _rotation.entry[2][1] };
            fingerTipPos = _position + forward * 10.0f;  // 10 units ~= 10cm
        }

        // Check proximity to activators
        // Pass hand speed so pointing range can extend for fast-moving hands
        float handSpeed = std::sqrt(_velocity.x * _velocity.x + _velocity.y * _velocity.y + _velocity.z * _velocity.z);
        auto& activatorHandler = ActivatorHandler::GetSingleton();
        auto result = activatorHandler.CheckProximity(fingerTipPos, _isLeft, handSpeed);

        _isPointingAtActivator = result.inPointingRange;
        _isInActivatorRange = result.inActivationRange;
        if (result.activator && result.activator->HasValidRefr()) {
            _nearbyActivatorHandle = RE::ObjectRefHandle(result.activator->GetRefr());
        } else {
            _nearbyActivatorHandle.reset();
        }
        _activatorDistance = result.distance;

        // Handle activation when finger enters activation range
        if (result.inActivationRange && result.activator) {
            // Try to activate (will handle cooldowns internally)
            if (activatorHandler.TryActivate(fingerTipPos, _isLeft)) {
                spdlog::debug("[ACTIVATOR] {} hand activated {:08X}",
                            _isLeft ? "Left" : "Right",
                            result.activator->formID);
            }
        }

        // Apply pointing finger pose when near activator, reset when moving away
        // CRITICAL: Don't clear finger pose if we're holding an object — that would wipe
        // the grab finger curls that were just set in StartGrab!
        if (_isPointingAtActivator) {
            // Set finger pose: index extended (1.0), others curled (0.0)
            // Values: thumb, index, middle, ring, pinky (1.0 = extended, 0.0 = curled)
            frik.SetHandPoseFingerPositions(_isLeft, 0.3f, 1.0f, 0.0f, 0.0f, 0.0f);
        } else if (_wasPointingAtActivator && _state != State::Held && _state != State::Pulling) {
            // Clear the pointing pose when transitioning away from pointing (return to open hand)
            // But NOT if we just grabbed something — that would clear the grab's finger curls!
            frik.ClearHandPoseFingerPositions(_isLeft);
        } else if (_wasPointingAtActivator) {
            spdlog::debug("[ACTIVATOR] Suppressed ClearHandPoseFingerPositions for {} hand (state=Held/Pulling)",
                        _isLeft ? "Left" : "Right");
        }
        
        // Track state for next frame
        _wasPointingAtActivator = _isPointingAtActivator;
    }

    void Hand::TransitionToIdle()
    {
        spdlog::trace("{} hand -> Idle", _isLeft ? "Left" : "Right");
        
        // Stop highlighting the previously selected object
        RE::TESObjectREFR* selRefr = _selection.GetRefr();
        if (selRefr && g_config.enableHighlighting) {
            Highlighter::GetSingleton().StopHighlight(selRefr);
        }
        
        _state = State::Idle;
        _selection.Clear();
        
        _stateEnterTime = Utils::GetTime();
    }

    void Hand::TransitionToSelectedClose(const Selection& selection)
    {
        RE::TESObjectREFR* newRefr = selection.GetRefr();
        RE::TESObjectREFR* oldRefr = _selection.GetRefr();
        
        spdlog::debug("{} hand -> SelectedClose, refr FormID {:08X}", _isLeft ? "Left" : "Right", newRefr ? newRefr->formID : 0);
        
        // Stop highlighting previous selection if different
        if (oldRefr && oldRefr != newRefr && g_config.enableHighlighting) {
            Highlighter::GetSingleton().StopHighlight(oldRefr);
        }
        
        // Start highlighting new selection
        if (newRefr && g_config.enableHighlighting) {
            spdlog::debug("[HIGHLIGHT] Calling StartHighlight for FormID {:08X}", newRefr->formID);
            Highlighter::GetSingleton().StartHighlight(newRefr);
        }
        
        _state = State::SelectedClose;
        _selection = selection;
        _stateEnterTime = Utils::GetTime();
    }

    void Hand::TransitionToPulling()
    {
        spdlog::trace("{} hand -> Pulling", _isLeft ? "Left" : "Right");
        
        // Stop highlighting while pulling - object is being grabbed
        RE::TESObjectREFR* selRefr = _selection.GetRefr();
        if (selRefr && g_config.enableHighlighting) {
            Highlighter::GetSingleton().StopHighlight(selRefr);
        }
        
        _state = State::Pulling;
        _stateEnterTime = Utils::GetTime();
    }

    void Hand::TransitionToHeld()
    {
        spdlog::trace("{} hand -> Held", _isLeft ? "Left" : "Right");
        
        // Stop highlighting while held - no need for visual cue
        RE::TESObjectREFR* selRefr = _selection.GetRefr();
        if (selRefr && g_config.enableHighlighting) {
            Highlighter::GetSingleton().StopHighlight(selRefr);
        }
        
        _state = State::Held;
        _stateEnterTime = Utils::GetTime();
    }

    void Hand::OnGrabPressed()
    {
        _grabPressed = true;

        // Check if we have an active grab — both sticky and non-sticky desync cases.
        // Must be checked BEFORE menu cooldown: player should be able to release
        // a grab immediately after closing Pipboy (not blocked by 1s cooldown).
        auto& grabMgr = GrabManager::GetSingleton();
        const auto& grabState = grabMgr.GetGrabState(_isLeft);
        if (grabState.active && !IsBlockingMenuOpen()) {
            if (grabState.stickyGrab) {
                // Sticky grab: grip PRESS releases it
                spdlog::debug("[GRAB] {} hand: Grip pressed on sticky grab - releasing",
                            _isLeft ? "Left" : "Right");
                Release(true);
                g_heisenberg.StartStickyGrabCooldown(_isLeft);
                TransitionToIdle();
                return;
            }
            if (_state == State::Idle) {
                // Non-sticky grab active but Hand is in Idle (desync from DropToHand timing).
                // The grip release that should have ended this grab was missed because
                // DropToHand hadn't processed yet. Release it now on press as recovery.
                spdlog::debug("[GRAB] {} hand: Grip pressed on desynced non-sticky grab - releasing (recovery)",
                            _isLeft ? "Left" : "Right");
                Release(true);
                TransitionToIdle();
                return;
            }
        }

        // Don't allow grabbing when blocking menus are open
        if (IsBlockingMenuOpen()) {
            return;
        }

        // Don't allow grabbing for 1 second after closing a menu
        // This prevents accidental grabs when exiting inventory/pipboy
        if (MenuChecker::GetSingleton().IsInMenuCloseCooldown()) {
            spdlog::debug("[GRAB] {} hand: Grip pressed during menu close cooldown - ignoring",
                         _isLeft ? "Left" : "Right");
            return;
        }
        
        switch (_state) {
        case State::SelectedClose:
            // Close object - try to grab immediately
            // If object is beyond snap distance, start pull-to-hand animation
            spdlog::debug("[GRAB] {} hand: In SelectedClose, trying grab (dist={:.1f}, snapDist={:.1f})",
                        _isLeft ? "Left" : "Right", _selection.distance, g_config.snapDistance);
            if (_selection.distance > g_config.snapDistance) {
                if (TryStartGrab()) {
                    spdlog::debug("[GRAB] {} hand: Starting PULL for object at {:.1f} units",
                                 _isLeft ? "Left" : "Right", _selection.distance);
                    TransitionToPulling();
                }
            } else {
                // Object is close enough - grab directly
                if (TryStartGrab()) {
                    TransitionToHeld();
                }
            }
            break;

        default:
            // No valid target - check weapon unequip or SmartGrab (grip in storage zone)
            {
                auto storageCheck = CheckItemStorageZone(_position);

                // WEAPON UNEQUIP: If weapon hand is in storage zone and weapon is drawn, unequip it.
                // Must check IsWeaponDrawn() because equipData->item persists even after unequip,
                // which would cause repeated unequip triggers on subsequent grip presses.
                if (g_config.enableStorageZoneWeaponEquip && IsPrimaryHand() && storageCheck.isInZone) {
                    auto* player = f4vr::getPlayer();
                    if (player && player->actorState.IsWeaponDrawn() &&
                        player->middleProcess && player->middleProcess->unk08 &&
                        player->middleProcess->unk08->equipData && player->middleProcess->unk08->equipData->item) {
                        auto* item = player->middleProcess->unk08->equipData->item;
                        auto* reForm = reinterpret_cast<RE::TESForm*>(item);
                        // Only unequip real weapons (guns, melee) - skip throwables and non-weapons
                        bool isRealWeapon = false;
                        if (reForm->IsWeapon()) {
                            auto* weapon = static_cast<RE::TESObjectWEAP*>(reForm);
                            auto weaponType = weapon->weaponData.type.get();
                            if (weaponType != RE::WEAPON_TYPE::kGrenade && weaponType != RE::WEAPON_TYPE::kMine) {
                                isRealWeapon = true;
                            }
                        }
                        if (isRealWeapon) {
                            // Get full display name with mods (e.g., "Laser Pistol" not just "Laser")
                            std::string displayName = GetWeaponDisplayName(reForm);
                            // Defer unequip to next frame - calling UnEquipItem during
                            // HookPostPhysics causes an access violation exception
                            g_heisenberg.QueueWeaponUnequip(reForm, displayName.c_str());
                            spdlog::debug("[GRAB] Queued weapon '{}' for unequip via storage zone",
                                        displayName);
                            break;
                        }
                    }
                    // RE-EQUIP: Weapon not drawn, but we have a previously unequipped weapon
                    // Grip in storage zone with weapon hand → re-equip the last unequipped weapon
                    else if (player && !player->actorState.IsWeaponDrawn()) {
                        auto* lastWeapon = g_heisenberg.GetLastUnequippedWeapon();
                        if (lastWeapon) {
                            auto weaponName = g_heisenberg.GetLastUnequippedWeaponName();
                            g_heisenberg.QueueWeaponReequip(lastWeapon, weaponName.c_str());
                            spdlog::debug("[GRAB] Queued weapon '{}' for re-equip via storage zone",
                                        weaponName);
                            break;
                        }
                    }
                }

                // SmartGrab: grip in storage zone with no weapon - pull item from inventory
                if (storageCheck.isInZone && g_config.enableSmartGrab) {
                    spdlog::debug("[SMARTGRAB] {} hand: Grip pressed in storage zone - trying smart grab",
                                _isLeft ? "Left" : "Right");
                    if (SmartGrabHandler::GetSingleton().TrySmartGrab(_isLeft)) {
                        spdlog::debug("[SMARTGRAB] {} hand: Smart grab succeeded!",
                                    _isLeft ? "Left" : "Right");
                        // Don't transition state here - SmartGrabHandler spawns item and starts grab
                    }
                } else {
                    spdlog::debug("[GRAB] {} hand: Grip pressed but state={} (not SelectedClose), selection.valid={} - no target",
                                _isLeft ? "Left" : "Right", static_cast<int>(_state), _selection.IsValid());
                }
            }
            break;
        }
    }

    void Hand::OnGrabReleased()
    {
        _grabPressed = false;
        
        // Get the grab state from GrabManager - this is the authoritative source
        // because grabs can be started by DropToHand without going through Hand state machine
        auto& grabMgr = GrabManager::GetSingleton();
        const auto& grabState = grabMgr.GetGrabState(_isLeft);

        // If GrabManager has an active grab for this hand (regardless of Hand's state)
        if (grabState.active) {
            // Check for sticky grab mode - if active, don't release the object
            if (grabState.stickyGrab) {
                spdlog::debug("{} hand: Grip released but sticky grab active - keeping hold",
                              _isLeft ? "Left" : "Right");
                return;  // Don't release
            }
            
            // Don't release while blocking menus are open - this protects
            // objects grabbed by DropToHand while in Pipboy/inventory/etc.
            if (IsBlockingMenuOpen()) {
                spdlog::debug("{} hand: Grip released but blocking menu open - keeping hold",
                              _isLeft ? "Left" : "Right");
                return;
            }
            
            // Release with throwing
            Release(true);
            
            TransitionToIdle();
            return;
        }

        // Fallback: handle based on Hand's own state machine
        switch (_state) {
        case State::Held:
        case State::Pulling:
            {
                // This path is for grabs started via the normal Hand state machine
                // Release with throwing
                Release(true);
                TransitionToIdle();
            }
            break;

        default:
            break;
        }
    }

    bool Hand::TryStartGrab()
    {
        // Block grabbing when PRIMARY hand is in chest pocket zone (throwable mode)
        // Primary = right hand normally, left hand in left-handed mode
        if (IsPrimaryHand() && Heisenberg::GetSingleton().IsInChestPocketZone()) {
            spdlog::debug("TryStartGrab: Blocked - primary hand in throwable zone");
            return false;
        }
        
        // NOTE: We used to block right hand grabbing when a weapon was holstered here,
        // but that caused issues because getEquippedWeaponName() returns stale data.
        // The holstered weapon protection is now handled entirely at the OpenVR level
        // via ShouldBlockGripForHolsteredWeapon() which strips grip from native input.
        // Heisenberg uses unfiltered input so it can still grab.
        
        if (!_selection.IsValid()) {
            spdlog::debug("TryStartGrab: No valid selection");
            return false;
        }

        auto& grabMgr = GrabManager::GetSingleton();
        
        // Create a copy of selection with the 3D node populated
        Selection grabSelection = _selection;
        RE::TESObjectREFR* grabRefr = grabSelection.GetRefr();
        if (grabRefr && !grabSelection.node) {
            grabSelection.node.reset(grabRefr->Get3D());
        }

        bool success = grabMgr.StartGrab(grabSelection, _position, _rotation, _isLeft);
        RE::TESObjectREFR* selRefr = _selection.GetRefr();
        if (success) {
            spdlog::debug("[GRAB] {} hand: TryStartGrab SUCCESS for {:08X}", 
                         _isLeft ? "Left" : "Right",
                         selRefr ? selRefr->formID : 0);
            
            // Notify Heisenberg that grab started (tracks if real weapon was drawn)
            Heisenberg::GetSingleton().OnGrabStarted(_isLeft);
        } else {
            spdlog::debug("[GRAB] {} hand: TryStartGrab FAILED for {:08X} (StartGrab returned false)", 
                         _isLeft ? "Left" : "Right",
                         selRefr ? selRefr->formID : 0);
        }
        return success;
    }

    void Hand::Release(bool throw_object)
    {
        auto& grabMgr = GrabManager::GetSingleton();
        
        if (throw_object) {
            // Calculate throw velocity from hand velocity, clamped to max
            RE::NiPoint3 throwVel = _velocity * Config::GetSingleton().throwVelocityBoostFactor;
            // Clamp each axis to max velocity
            throwVel.x = std::clamp(throwVel.x, -kMaxThrowVelocity, kMaxThrowVelocity);
            throwVel.y = std::clamp(throwVel.y, -kMaxThrowVelocity, kMaxThrowVelocity);
            throwVel.z = std::clamp(throwVel.z, -kMaxThrowVelocity, kMaxThrowVelocity);
            
            // Check if velocity is above threshold - if not, just drop without velocity
            float speed = std::sqrt(throwVel.x * throwVel.x + throwVel.y * throwVel.y + throwVel.z * throwVel.z);
            if (speed >= Config::GetSingleton().throwVelocityThreshold) {
                grabMgr.EndGrab(_isLeft, &throwVel);
                spdlog::debug("{} hand threw object with velocity ({:.2f}, {:.2f}, {:.2f}) speed={:.2f}",
                              _isLeft ? "Left" : "Right",
                              throwVel.x, throwVel.y, throwVel.z, speed);
            } else {
                grabMgr.EndGrab(_isLeft, nullptr);
                spdlog::debug("{} hand dropped object (speed {:.2f} below threshold {:.2f})",
                              _isLeft ? "Left" : "Right", speed, Config::GetSingleton().throwVelocityThreshold);
            }
        } else {
            grabMgr.EndGrab(_isLeft, nullptr);
            spdlog::debug("{} hand dropped object", _isLeft ? "Left" : "Right");
        }
    }

    void Hand::UpdateHeldObject(float deltaTime)
    {
        auto& grabMgr = GrabManager::GetSingleton();
        grabMgr.UpdateGrab(_position, _rotation, _isLeft, deltaTime);
    }

    // =========================================================================
    // SEATED MODE DETECTION
    // =========================================================================
    // Returns true if the physical VR player appears to be seated (low HMD height)
    // This enables extended grab range for accessibility
    // Note: This is a CUSTOM F4VR feature - Skyrim HIGGS does not have seated mode detection
    
    static bool s_lastSeatedState = false;      // Track last state for change detection
    static bool s_seatedStateInitialized = false;  // Have we logged initial state?
    static int s_seatedCheckCounter = 0;        // Throttle logging
    
    static bool IsPlayerPhysicallySeated()
    {
        if (!g_config.enableSeatedMode)
            return false;
            
        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes || !playerNodes->HmdNode || !playerNodes->roomnode)
            return false;
        
        // Calculate HMD height above floor (roomnode is at floor level)
        float hmdHeight = playerNodes->HmdNode->world.translate.z - 
                          playerNodes->roomnode->world.translate.z;
        
        // If HMD is below threshold, player is likely seated
        bool isSeated = hmdHeight < g_config.seatedModeHeightThreshold;
        
        // Log initial state once
        if (!s_seatedStateInitialized) {
            s_seatedStateInitialized = true;
            s_lastSeatedState = isSeated;
            spdlog::debug("[POSTURE] Initial mode: {} (HMD height: {:.1f}, threshold: {:.1f})",
                         isSeated ? "SEATED" : "STANDING",
                         hmdHeight, g_config.seatedModeHeightThreshold);
            spdlog::debug("[POSTURE] Grab distance: {:.1f} (seated={:.1f}, standing={:.1f})",
                         isSeated ? g_config.seatedModeGrabDistance : g_config.standingModeGrabDistance,
                         g_config.seatedModeGrabDistance, g_config.standingModeGrabDistance);
        }
        
        // Detect and log mode changes
        if (isSeated != s_lastSeatedState) {
            s_lastSeatedState = isSeated;
            spdlog::debug("[POSTURE] Mode changed to: {} (HMD height: {:.1f})",
                         isSeated ? "SEATED" : "STANDING", hmdHeight);
            spdlog::debug("[POSTURE] New grab distance: {:.1f}",
                         isSeated ? g_config.seatedModeGrabDistance : g_config.standingModeGrabDistance);
        }
        
        // Periodic debug logging (every ~300 frames, ~5 seconds)
        if (++s_seatedCheckCounter >= 300) {
            s_seatedCheckCounter = 0;
            spdlog::debug("[POSTURE] HMD height: {:.1f} | threshold: {:.1f} | mode: {}",
                          hmdHeight, g_config.seatedModeHeightThreshold,
                          isSeated ? "SEATED" : "STANDING");
        }
        
        return isSeated;
    }
    
    // Get the effective grab distance based on player posture
    // NOTE: This function is only used by FindSelectionRaycast and FindSelectionProximity
    // which are currently DEAD CODE. It is preserved for future use.
    [[maybe_unused]]
    static float GetEffectiveGrabDistance()
    {
        if (g_config.enableSeatedMode)
        {
            bool seated = IsPlayerPhysicallySeated();
            return seated ? g_config.seatedModeGrabDistance : g_config.standingModeGrabDistance;
        }
        return g_config.maxGrabDistance;
    }

    // Helper to get object name for debug logging
    static std::string GetRefrName(RE::TESObjectREFR* refr)
    {
        if (!refr) return "null";
        auto* baseForm = refr->GetObjectReference();
        if (baseForm) {
            auto fullName = RE::TESFullName::GetFullName(*baseForm, false);
            if (!fullName.empty()) {
                return std::string(fullName);
            }
        }
        // Fallback to formID
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", static_cast<uint32_t>(refr->formID));
        return std::string(buf);
    }

    bool Hand::FindSelectionViewCaster()
    {
        // =====================================================================
        // VIEWCASTER-BASED SELECTION (Native game crosshair)
        // =====================================================================
        // The ViewCaster is the game's authoritative source for "Press A/X to store".
        // If it has a selection, we ALWAYS use it (highest priority).
        // This ensures we grab exactly what the player expects.
        // =====================================================================
        
        // Get ViewCaster target for this hand (direct call to F4VROffsets)
        // Safety: skip if player isn't loaded or in a cell
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return false;
        }
        
        // FIXED: Use handle-based lookup for thread safety!
        // This prevents crashes when objects are deleted on worker threads.
        RE::ObjectRefHandle targetHandle = GetVRWandTargetHandle(_isLeft);
        
        if (!targetHandle) {
            return false;
        }
        
        // Get reference-counted smart pointer - keeps object alive during our checks
        RE::NiPointer<RE::TESObjectREFR> refrPtr = targetHandle.get();
        RE::TESObjectREFR* refr = refrPtr.get();
        
        if (!refr) {
            return false;
        }
        
        // Validate it's a grabbable object type
        if (!Physics::IsGrabbable(refr)) {
            return false;
        }
        
        // Get the 3D node
        RE::NiAVObject* node = refr->Get3D();
        if (!node) {
            return false;
        }
        
        // Calculate distance from hand
        RE::NiPoint3 objPos = node->world.translate;
        float dx = objPos.x - _position.x;
        float dy = objPos.y - _position.y;
        float dz = objPos.z - _position.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        
        // ViewCaster selections are ALWAYS valid (game has already validated)
        // but we still check max grab distance for sanity
        if (distance > Config::GetSingleton().maxGrabDistance) {
            return false;
        }
        
        // Set the selection - stores the HANDLE for thread-safe access later
        Selection newSelection;
        newSelection.SetRefr(refr);  // This creates a new ObjectRefHandle
        newSelection.hitPoint = objPos;
        newSelection.hitNormal = RE::NiPoint3(0, 0, 1);  // Default up
        newSelection.distance = distance;
        newSelection.isClose = (distance <= Config::GetSingleton().closeGrabThreshold);
        _selection = newSelection;
        
        static int logCounter = 0;
        if (++logCounter >= 60) {  // Log every ~1 second
            logCounter = 0;
            spdlog::debug("[SELECT-VC] {} hand: ViewCaster selected '{}' at {:.1f} units",
                         _isLeft ? "Left" : "Right", GetRefrName(refr), distance);
        }
        
        return true;
    }

    bool Hand::FindSelectionRaycast()
    {
        // =====================================================================
        // DEAD CODE - NOT CURRENTLY CALLED
        // This function is preserved for future use if extended range selection
        // beyond ViewCaster is needed. Currently UpdateSelection() uses only
        // ViewCaster (FindSelectionViewCaster) for simple, reliable selection.
        // =====================================================================
        // HIGGS-STYLE SELECTION (Ported from Skyrim HIGGS)
        // =====================================================================
        // HIGGS Skyrim uses a sphere linear cast along the pointing direction.
        // In F4VR, we don't have direct hkpWorld_LinearCast access, so we:
        // 1. Get all nearby physics objects
        // 2. For each object, check if it's within a cylinder along the pointing direction
        // 3. Score by perpendicular distance from the ray axis (closest wins)
        // 
        // This mirrors HIGGS's FindCloseObject() and FindFarObject() logic.
        
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return false;
        }

        // =====================================================================
        // DISABLED: Native crosshair system doesn't work in F4VR - can't read target
        // =====================================================================
        // Native crosshair code removed - use only custom raycast/sphere cast
        
        // =====================================================================
        // Custom raycast/sphere cast for object selection
        // =====================================================================

        // DEBUG: Log every 10 frames (per-hand counter) for faster feedback
        bool shouldLogDebug = (++_debugFrameCounter >= 10);
        if (shouldLogDebug) {
            _debugFrameCounter = 0;
        }

        // Get wand forward direction from rotation matrix
        // Extract all three axes for diagnosis
        RE::NiPoint3 xAxis(_rotation.entry[0][0], _rotation.entry[1][0], _rotation.entry[2][0]);  // Right
        RE::NiPoint3 yAxis(_rotation.entry[0][1], _rotation.entry[1][1], _rotation.entry[2][1]);  // Forward?
        RE::NiPoint3 zAxis(_rotation.entry[0][2], _rotation.entry[1][2], _rotation.entry[2][2]);  // Up?
        
        // F4VR wand: Try NEGATIVE Z axis as pointing direction
        // The controller's laser/pointer typically comes out along -Z in many VR setups
        RE::NiPoint3 pointingDir;
        pointingDir.x = -zAxis.x;
        pointingDir.y = -zAxis.y;
        pointingDir.z = -zAxis.z;
        
        // Normalize
        float len = std::sqrt(pointingDir.x * pointingDir.x + pointingDir.y * pointingDir.y + pointingDir.z * pointingDir.z);
        if (len > 0.0001f) {
            pointingDir.x /= len;
            pointingDir.y /= len;
            pointingDir.z /= len;
        }
        
        // Log all axes for diagnosis
        if (shouldLogDebug) {
            spdlog::debug("[SELECT] {} hand AXES: X=({:.2f},{:.2f},{:.2f}) Y=({:.2f},{:.2f},{:.2f}) Z=({:.2f},{:.2f},{:.2f})",
                         _isLeft ? "Left" : "Right",
                         xAxis.x, xAxis.y, xAxis.z,
                         yAxis.x, yAxis.y, yAxis.z,
                         zAxis.x, zAxis.y, zAxis.z);
        }

        // HIGGS-style cast parameters (converted from Skyrim units to Fallout units)
        // Skyrim HIGGS: nearCastRadius = 0.05m (5cm), nearCastDistance = 0.1m (10cm)
        //               farCastRadius = 0.3m (30cm), farCastDistance = 5.0m (500cm)
        // Fallout uses same scale (1 unit = 1cm approximately)
        // 
        // Using WIDE cast radius because we use bounding sphere tests, not physics mesh raycasts.
        // The visibility check + scoring will filter to the correct object.
        constexpr float NEAR_CAST_RADIUS = 30.0f;   // 30cm sphere radius for close grab
        constexpr float NEAR_CAST_DISTANCE = 30.0f; // 30cm cast distance for close grab
        constexpr float FAR_CAST_RADIUS = 100.0f;   // 100cm sphere radius for far grab (1 meter)
        constexpr float FAR_CAST_DISTANCE = 200.0f; // 200cm cast distance for far grab
        
        float maxDistance = GetEffectiveGrabDistance();
        float castRadius = (maxDistance <= 50.0f) ? NEAR_CAST_RADIUS : FAR_CAST_RADIUS;
        
        if (shouldLogDebug) {
            spdlog::debug("[SELECT] {} hand: pos=({:.1f},{:.1f},{:.1f}) dir=({:.2f},{:.2f},{:.2f}) maxDist={:.0f} radius={:.0f}",
                         _isLeft ? "Left" : "Right",
                         _position.x, _position.y, _position.z,
                         pointingDir.x, pointingDir.y, pointingDir.z,
                         maxDistance, castRadius);
        }

        // =====================================================================
        // GET HMD FORWARD FOR VISIBILITY CHECK
        // =====================================================================
        // Only select objects the player can actually see (in front of head)
        RE::NiPoint3 hmdPos;
        RE::NiPoint3 hmdForward;
        bool hasHmdData = false;
        
        if (auto* playerNodes = f4vr::getPlayerNodes()) {
            if (playerNodes->HmdNode) {
                hmdPos = playerNodes->HmdNode->world.translate;
                // HMD forward is -Z axis (same as wand)
                hmdForward.x = -playerNodes->HmdNode->world.rotate.entry[0][2];
                hmdForward.y = -playerNodes->HmdNode->world.rotate.entry[1][2];
                hmdForward.z = -playerNodes->HmdNode->world.rotate.entry[2][2];
                hasHmdData = true;
            }
        }
        
        // Minimum dot product for visibility (cos of half FOV)
        // HIGGS Skyrim: Does NOT have visibility check for CLOSE objects, only for FAR grabs
        // We follow the same approach: For close/near selection, we DISABLE visibility check
        // because you should be able to grab objects behind you if your hand is near them.
        // 
        // For FAR grabs (if implemented), HIGGS uses cos(50°) ≈ 0.64
        // For now: -0.9 = only reject objects DIRECTLY behind you (165° off center)
        constexpr float MIN_VISIBILITY_DOT = -0.9f;  // Very permissive - almost disabled

        // =====================================================================
        // HIGGS-STYLE SPHERE CAST: Find all objects in cylinder along pointing direction
        // =====================================================================
        auto nearbyObjects = Physics::GetObjectsInRadius(_position, maxDistance + castRadius, player);
        
        if (shouldLogDebug) {
            spdlog::debug("[SELECT] Found {} nearby objects", nearbyObjects.size());
        }
        
        RE::TESObjectREFR* closestRefr = nullptr;
        float closestPerpendicularDist = (std::numeric_limits<float>::max)();
        float closestAlongDist = maxDistance;
        RE::NiPoint3 closestHitPoint;
        
        int objectIndex = 0;
        for (auto* refr : nearbyObjects) {
            if (!refr) continue;
            
            // Skip player
            if (refr == player) continue;
            
            // Skip objects on cooldown
            if (GrabManager::GetSingleton().IsOnCooldown(refr->formID)) continue;
            
            // Log each object we're considering
            if (shouldLogDebug && objectIndex < 5) {
                std::string name = GetRefrName(refr);
                spdlog::debug("[SELECT] Object #{}: '{}' {:08X}", objectIndex, name.c_str(), refr->formID);
            }
            objectIndex++;
            
            // Get object position (use world bound center if available for accuracy)
            RE::NiPoint3 refrPos = refr->GetPosition();
            float boundRadius = 10.0f;  // Default bounding radius
            
            if (auto* node3D = refr->Get3D()) {
                if (node3D->IsNode()) {
                    auto* asNode = static_cast<RE::NiNode*>(node3D);
                    refrPos = asNode->worldBound.center;
                    boundRadius = asNode->worldBound.fRadius;
                    if (boundRadius < 5.0f) boundRadius = 5.0f;
                }
            }
            
            // =====================================================================
            // VISIBILITY CHECK: Skip objects the player cannot see
            // =====================================================================
            if (hasHmdData) {
                // Calculate direction from HMD to object
                RE::NiPoint3 hmdToObject;
                hmdToObject.x = refrPos.x - hmdPos.x;
                hmdToObject.y = refrPos.y - hmdPos.y;
                hmdToObject.z = refrPos.z - hmdPos.z;
                
                // Normalize
                float hmdDist = std::sqrt(hmdToObject.x * hmdToObject.x + 
                                          hmdToObject.y * hmdToObject.y + 
                                          hmdToObject.z * hmdToObject.z);
                if (hmdDist > 0.01f) {
                    hmdToObject.x /= hmdDist;
                    hmdToObject.y /= hmdDist;
                    hmdToObject.z /= hmdDist;
                    
                    // Dot product with HMD forward - positive = in front, negative = behind
                    float visibilityDot = hmdToObject.x * hmdForward.x + 
                                          hmdToObject.y * hmdForward.y + 
                                          hmdToObject.z * hmdForward.z;
                    
                    if (visibilityDot < MIN_VISIBILITY_DOT) {
                        if (shouldLogDebug) {
                            std::string name = GetRefrName(refr);
                            spdlog::debug("[SELECT]   '{}': NOT VISIBLE (dot={:.2f} < {:.2f})",
                                         name.c_str(), visibilityDot, MIN_VISIBILITY_DOT);
                        }
                        continue;  // Object is outside player's field of view
                    }
                }
            }
            
            // Calculate vector from hand to object
            RE::NiPoint3 toObject;
            toObject.x = refrPos.x - _position.x;
            toObject.y = refrPos.y - _position.y;
            toObject.z = refrPos.z - _position.z;
            
            // Distance along the ray (dot product with pointing direction)
            float distAlongRay = toObject.x * pointingDir.x + 
                                 toObject.y * pointingDir.y + 
                                 toObject.z * pointingDir.z;
            
            // Object must be in front of hand and within range
            if (distAlongRay < -boundRadius || distAlongRay > maxDistance + boundRadius) {
                continue;
            }
            
            // Calculate perpendicular distance from ray axis
            // Project toObject onto pointing direction, then subtract to get perpendicular component
            RE::NiPoint3 projOnRay;
            projOnRay.x = pointingDir.x * distAlongRay;
            projOnRay.y = pointingDir.y * distAlongRay;
            projOnRay.z = pointingDir.z * distAlongRay;
            
            RE::NiPoint3 perpendicular;
            perpendicular.x = toObject.x - projOnRay.x;
            perpendicular.y = toObject.y - projOnRay.y;
            perpendicular.z = toObject.z - projOnRay.z;
            
            float perpDist = std::sqrt(perpendicular.x * perpendicular.x + 
                                       perpendicular.y * perpendicular.y + 
                                       perpendicular.z * perpendicular.z);
            
            // For INCLUSION check: use effective distance (subtract bounding radius)
            // This allows the ray to "hit" the object's bounding sphere
            float effectivePerpDistForInclusion = perpDist - boundRadius;
            if (effectivePerpDistForInclusion < 0) effectivePerpDistForInclusion = 0;
            
            // Object must be within the cast cylinder (perpendicular distance <= cast radius)
            if (effectivePerpDistForInclusion > castRadius) {
                if (shouldLogDebug) {
                    std::string name = GetRefrName(refr);
                    spdlog::debug("[SELECT]   '{}': perpDist={:.1f} (raw={:.1f}) > castRadius={:.1f}, OUTSIDE CYLINDER",
                                 name.c_str(), effectivePerpDistForInclusion, perpDist, castRadius);
                }
                continue;
            }
            
            // Check if object is grabbable
            if (!Physics::IsGrabbable(refr)) {
                if (shouldLogDebug) {
                    std::string name = GetRefrName(refr);
                    spdlog::debug("[SELECT]   '{}': NOT GRABBABLE", name.c_str());
                }
                continue;
            }
            
            // For SCORING: Combined score that prefers closer objects when pointing is similar
            // - Primary: perpendicular distance (how close to ray axis)
            // - Secondary: distance along ray (closer objects win when perp dist is similar)
            // 
            // Formula: score = perpDist + (alongDist * distanceWeight)
            // A low distanceWeight (0.1-0.3) means pointing accuracy matters most,
            // but closer objects get a slight advantage
            constexpr float DISTANCE_WEIGHT = 0.25f;  // How much to weight distance vs pointing
            float scoreDistance = perpDist + (distAlongRay * DISTANCE_WEIGHT);
            
            // Give storable items 2x priority (lower score = higher priority)
            // This makes it easier to select items you can pick up over static objects
            if (heisenberg::IsStorableItem(refr)) {
                scoreDistance *= 0.5f;  // Halve score = 2x priority
            }
            
            if (shouldLogDebug) {
                std::string name = GetRefrName(refr);
                bool isStorable = heisenberg::IsStorableItem(refr);
                spdlog::debug("[SELECT]   '{}': score={:.1f} (perp={:.1f} + along={:.1f}*{:.2f}) boundR={:.1f} storable={} {}",
                             name.c_str(), scoreDistance, perpDist, distAlongRay, DISTANCE_WEIGHT, boundRadius, isStorable,
                             (scoreDistance < closestPerpendicularDist) ? "<-- BEST SO FAR" : "");
            }
            
            // Combined scoring: lower score wins
            if (scoreDistance < closestPerpendicularDist) {
                closestPerpendicularDist = scoreDistance;
                closestAlongDist = distAlongRay;
                closestRefr = refr;
                closestHitPoint = refrPos;
            }
        }
        
        if (closestRefr) {
            std::string baseName = GetRefrName(closestRefr);
            
            Selection newSelection;
            newSelection.SetRefr(closestRefr);
            newSelection.hitPoint = closestHitPoint;
            newSelection.hitNormal = RE::NiPoint3(0, 0, 1);
            newSelection.distance = closestAlongDist;
            newSelection.isClose = (closestAlongDist <= g_config.closeGrabThreshold);
            
            _selection = newSelection;
            
            if (closestRefr->formID != _lastLoggedFormID) {
                spdlog::debug("[SELECT] {} hand: '{}' ({:08X}) score={:.1f} alongDist={:.1f}",
                              _isLeft ? "Left" : "Right",
                              baseName.c_str(), closestRefr->formID,
                              closestPerpendicularDist, closestAlongDist);
                _lastLoggedFormID = closestRefr->formID;
            }
            return true;
        }

        return false;
    }

    bool Hand::FindSelectionProximity()
    {
        // =====================================================================
        // DEAD CODE - NOT CURRENTLY CALLED
        // This function is preserved for future use if proximity-based selection
        // is needed. Currently UpdateSelection() uses only ViewCaster.
        // =====================================================================
        
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return false;
        }

        // Get effective grab distance based on posture (seated vs standing)
        float proximityRadius = GetEffectiveGrabDistance();
        bool isSeated = IsPlayerPhysicallySeated();
        
        auto nearbyObjects = Physics::GetObjectsInRadius(_position, proximityRadius, player);
        
        // Find the closest grabbable object
        RE::TESObjectREFR* closestRefr = nullptr;
        float closestDistSq = proximityRadius * proximityRadius;
        
        for (auto* refr : nearbyObjects) {
            if (!refr || !Physics::IsGrabbable(refr)) {
                continue;
            }
            
            RE::NiPoint3 refrPos = refr->GetPosition();
            float dx = refrPos.x - _position.x;
            float dy = refrPos.y - _position.y;
            float dz = refrPos.z - _position.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            
            if (distSq < closestDistSq) {
                closestDistSq = distSq;
                closestRefr = refr;
            }
        }
        
        if (closestRefr) {
            float distance = std::sqrt(closestDistSq);
            
            Selection newSelection;
            newSelection.SetRefr(closestRefr);
            newSelection.hitPoint = closestRefr->GetPosition();
            newSelection.hitNormal = RE::NiPoint3(0, 0, 1);  // Default up
            newSelection.distance = distance;
            newSelection.isClose = (distance <= g_config.snapDistance);
            
            _selection = newSelection;
            
            // PERF: Only log when selection changes, and use debug level
            if (closestRefr->formID != _lastLoggedFormID) {
                spdlog::debug("[PROXIMITY] {} hand: Selected ref {:08X} dist={:.1f} units ({}mode, range={:.0f})",
                              _isLeft ? "Left" : "Right",
                              closestRefr->formID,
                              distance,
                              isSeated ? "SEATED " : "standing ",
                              proximityRadius);
                _lastLoggedFormID = closestRefr->formID;
            }
            return true;
        }

        return false;
    }
}
