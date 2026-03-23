#pragma once

#include <atomic>
#include <chrono>
#include <Version.h>
#include "Hand.h"
#include "FingerAnimator.h"

namespace heisenberg
{
    // Forward declarations
    class PhysicsManager;
    class SelectionManager;

    /**
     * Main Heisenberg mod class for Fallout 4 VR.
     * Handles physics-based object grabbing, throwing, and hand collision.
     *
     * Thread safety:
     *   - Main thread: OnFrameUpdate, OnInputUpdate, OnGrabUpdate, UpdateCachedWeaponState
     *   - OpenVR callback thread: reads _cb_* atomics, _cachedWeapon* atomics,
     *     _leftHandGripHeld, _rightHandGripHeld
     *   - _cb_* members: std::atomic, accessed with memory_order_relaxed
     *   - _cachedWeapon* members: std::atomic, written on main thread, read from OpenVR thread
     *   - _inputLayer*: written once during init, read from OpenVR thread (safe after init)
     *   - NOTE: STUF VR mod now handles preventing Unarmed equip on grip
     */
    class Heisenberg
    {
    public:
        static Heisenberg& GetSingleton()
        {
            static Heisenberg instance;
            return instance;
        }

        // Plugin lifecycle
        bool OnF4SEQuery(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info);
        bool OnF4SELoad(const F4SE::LoadInterface* a_f4se);

        // Game lifecycle
        void OnGameLoad();
        void OnFrameUpdate();      // Original single update (kept for compatibility)
        void OnInputUpdate();    // Runs in post-physics hook - handles input, hand state
        void OnGrabUpdate();     // Runs in post-physics hook - handles grab detection, zones

        // Accessors
        Hand* GetLeftHand() const { return _leftHand.get(); }
        Hand* GetRightHand() const { return _rightHand.get(); }
        bool IsInitialized() const { return _initialized; }
        bool IsModDisabled() const { return _modDisabled; }

        // Check if either hand is holding an object
        bool IsHoldingAnything() const;

        // Check if double-tap + hold is active (for native throwables)
        bool IsDoubleTapActive() const { return _doubleTapHoldActive; }

        // Check if in chest pocket zone (for native grenades)
        bool IsInChestPocketZone() const { return _isInChestPocketZone; }

        // Input suppression - prevents native F4VR from also responding to grip
        void UpdateInputSuppression();

        // Chest pocket zone - enables native grenades when hand is near chest
        void UpdateChestPocketZone();

        // Item storage zone configuration - allows configuring storage zones
        void UpdateStorageZoneConfig();

        // Notify that a grab has started (tracks if real weapon was drawn for right hand)
        void OnGrabStarted(bool isLeft);

        // Notify that a grab has ended (sheathe weapon only if real weapon was drawn at start)
        // Only force-sheathe for right hand grabs (isLeft=false)
        void OnGrabEnded(bool isLeft);

        // Deactivate unarmed fists at grab start so the hand opens for the grabbed item.
        // Suppresses kFighting briefly so the game doesn't immediately re-equip fists.
        void DeactivateUnarmedForGrab();

        // Set flag to keep weapon sheathed until trigger (called when force-sheathing for grab)
        void SetWeaponForceSheathed() { _weaponForceSheathed = true; }

        // Check if Virtual Holsters mod is detected (for compatibility mode)
        bool IsVirtualHolstersActive() const { return _virtualHolstersDetected; }

        // Sticky grab cooldown - prevents re-grabbing immediately after release
        void StartStickyGrabCooldown(bool isLeft);
        bool IsInStickyGrabCooldown(bool isLeft) const;
        void UpdateStickyGrabCooldowns(float deltaTime);

        // NOTE: Grip kFighting suppression removed - STUF VR mod now handles this

    private:
        Heisenberg() = default;
        Heisenberg(const Heisenberg&) = delete;
        Heisenberg& operator=(const Heisenberg&) = delete;

    public:
        // Destructor must be defined in cpp where Hand is complete
        ~Heisenberg();

        // Get/set finger curl value for saving (used by ItemPositionConfigMode)
        float GetFingerCurlValue(bool isLeft) const {
            return (isLeft ? _leftFingerAnimator : _rightFingerAnimator).GetAverageCurl();
        }
        void SetFingerCurlValue(bool isLeft, float value) {
            if (isLeft) _leftHandPoseValue = value;
            else _rightHandPoseValue = value;
        }

        // Access the per-joint finger animator for a hand
        FingerAnimator& GetFingerAnimator(bool isLeft) { return isLeft ? _leftFingerAnimator : _rightFingerAnimator; }

        /**
         * Clear all hand state on save/load to prevent dangling pointers.
         */
        void ClearHandStates()
        {
            if (_leftHand) _leftHand->ClearState();
            if (_rightHand) _rightHand->ClearState();
            spdlog::info("Cleared hand states on load");
        }

        /**
         * Reset grenade zone tracking and callback state after save load.
         */
        void ReapplyThrowDelay();

    private:

        void InitHands();
        void UpdateHands();

        // F4SE interfaces
        const F4SE::MessagingInterface* _messaging = nullptr;

        // Hand instances
        std::unique_ptr<Hand> _leftHand;
        std::unique_ptr<Hand> _rightHand;

        // State
        bool _initialized = false;
        bool _modDisabled = false;  // True if mod is disabled due to incompatible FRIK
        bool _inputSuppressed = false;
        bool _fightingSuppressed = false;  // Suppressing kFighting to prevent native throwable ready
        bool _zKeySuppressed = false;      // Permanently suppress native Z-key spring mode

        // Chest zone + grip detection for native throwables
        // When right hand is in chest zone AND grip pressed = native throwable ready
        bool _lastRightGripPressed = false;
        bool _doubleTapHoldActive = false;    // True when in chest zone + grip pressed

        // Per-joint finger animation controllers
        FingerAnimator _leftFingerAnimator;
        FingerAnimator _rightFingerAnimator;

        // Hand pose debug control (used by thumbstick reposition mode)
        float _leftHandPoseValue = 0.5f;   // 0.0 = closed, 1.0 = open
        float _rightHandPoseValue = 0.5f;  // 0.0 = closed, 1.0 = open
        bool _handPoseOverrideActive = false;

        // Node capture mode for activators
        // Long-press left thumbstick to capture closest node to right fingertip
        float _nodeCaptureHoldTimer = 0.0f;
        bool _lastLeftThumbstickForCapture = false;
        bool _nodeCaptureTriggered = false;  // Prevent repeating while held
        static constexpr float NODE_CAPTURE_HOLD_TIME = 2.0f;  // 2 seconds hold

        // Throwable activation zone - enables native grenades when hand is in zone
        bool _wasInChestPocketZone = false;  // Previous frame state
        bool _isInChestPocketZone = false;   // Current frame state

        // Grenade cooldown after grab release — prevents grip hold from
        // immediately readying a grenade after consuming/dropping an item
        bool _wasHoldingForGrenadeCooldown = false;
        std::chrono::steady_clock::time_point _lastGrabReleaseTime{};

        // Individual zone tracking for debug mode
        bool _wasInChest = false;  // Used for throwable zone haptic feedback

        // Storage zone configuration mode (Item Storage - L3)
        bool _storageConfigModeActive = false;
        bool _storageConfigJustEntered = false;  // Skip first exit check after entering
        int _selectedStorageZone = 1;  // 1=LeftShoulder, 2=RightShoulder, 3=LeftHip, 4=RightHip, 5=Back
        float _storageConfigModeHoldTimer = 0.0f;
        bool _lastLeftThumbstickClick = false;
        float _storageConfigCooldown = 0.0f;  // 2 second cooldown after activation/deactivation

        // Storage zone tracking for haptic feedback
        bool _wasInStorageLeftShoulder = false;
        bool _wasInStorageRightShoulder = false;
        bool _wasInStorageLeftHip = false;
        bool _wasInStorageRightHip = false;
        bool _wasInStorageBack = false;

        // Smart retrieval: track empty hand entering storage zone for haptic pulse
        bool _emptyHandInStorageZone[2] = { false, false };  // [0]=left, [1]=right

        // Post-grab kFighting suppression - prevents Unarmed from auto-equipping on grip release
        float _postGrabSuppressTimer = 0.0f;       // Time remaining for post-grab suppression
        bool _postGrabFightingSuppressed = false;  // Currently suppressing due to post-grab
        static constexpr float POST_GRAB_SUPPRESS_DURATION = 0.5f;  // How long to suppress after grab

        // Force-sheathed tracking - when we sheathe a weapon for grabbing, keep it sheathed until trigger
        bool _weaponForceSheathed = false;  // True when we've force-sheathed, cleared on trigger press

        // NOTE: Weapon state tracking removed - STUF VR mod now handles Unarmed prevention

        // Left hand grip-hold state (legacy, always false now)
        bool _leftHandGripHeld = false;
        bool _rightHandGripHeld = false;

        // NOTE: Hand pose override tracking removed - FingerAnimator is sole authority
        // FingerAnimator handles Closing/Holding/Opening/Idle states and clears FRIK override

        // NOTE: Grip kFighting suppression removed - STUF VR mod now handles this

        // InputEnableLayer for kFighting control (like FRIK/VH use)
        RE::BSTSmartPointer<RE::BSInputEnableLayer> _inputLayer;  // Our layer for disabling fighting
        std::uint32_t _inputLayerID = 0;  // Layer ID for EnableUserEvent calls
        bool _inputLayerInitialized = false;

        // Virtual Holsters compatibility - detected at startup
        bool _virtualHolstersDetected = false;  // True if VirtualHolsters.dll is loaded

        // Sticky grab cooldown - 1 second cooldown after releasing a sticky grab
        // During cooldown, grip is completely blocked to prevent accidental re-grab
        float _leftStickyGrabCooldown = 0.0f;
        float _rightStickyGrabCooldown = 0.0f;
        static constexpr float STICKY_GRAB_COOLDOWN_DURATION = 1.0f;

        // THREAD SAFETY: Cached weapon state for OpenVR callback
        // These are updated on main thread in OnInputUpdate() and read from OpenVR thread
        // This avoids accessing game data structures from OpenVR callback thread
        std::atomic<bool> _cachedWeaponDrawn{false};       // player->actorState.IsWeaponDrawn()
        std::atomic<bool> _cachedHasRealWeapon{false};     // HasRealWeaponEquipped() result

        // =====================================================================
        // THREAD-SAFE OPENVR CALLBACK STATE
        // These replace static locals in the OpenVR controller state callback.
        // All times stored as double (seconds since start via Utils::GetTime()).
        // All accessed with std::memory_order_relaxed (no ordering requirements).
        // =====================================================================

        // Right-hand kFighting toggle state
        std::atomic<bool>   _cb_lastGripRight{false};
        std::atomic<double> _cb_gripPressTime{0.0};
        std::atomic<double> _cb_lastToggleTime{0.0};
        std::atomic<bool>   _cb_waitingToToggle{false};
        std::atomic<bool>   _cb_needsRetry{false};

        // DEFERRED kFighting toggle - set by OpenVR callback, processed on main thread
        // This prevents deadlock from calling EnableUserEvent on OpenVR thread
        std::atomic<bool>   _cb_requestKFightingToggle{false};

        // Debug log counters
        std::atomic<int>    _cb_cooldownBlockCounter{0};

        // A-button grenade remap state
        std::atomic<double> _cb_aButtonPressTime{0.0};
        std::atomic<bool>   _cb_aButtonHeldLongEnough{false};
        std::atomic<bool>   _cb_aButtonWasPressed{false};

        // THREAD SAFETY: Physical grip state from OpenVR callback
        // Written by OpenVR thread, read by main thread for finger-closing decision
        std::atomic<bool> _physicalGripPressedLeft{false};
        std::atomic<bool> _physicalGripPressedRight{false};

        // Deferred weapon unequip - set by Hand::OnGrabPressed, processed next frame
        // UnEquipItem crashes when called during HookPostPhysics, so defer by one frame
        RE::TESForm* _pendingUnequipForm = nullptr;
        std::string _pendingUnequipName;

        // Deferred weapon re-equip - set by Hand::OnGrabPressed, processed next frame
        RE::TESForm* _pendingReequipForm = nullptr;
        std::string _pendingReequipName;

        // Last weapon unequipped via storage zone - for re-equip on next grip
        RE::TESForm* _lastUnequippedWeapon = nullptr;
        std::string _lastUnequippedWeaponName;

    public:
        // Thread-safe accessors for OpenVR callback
        bool GetCachedWeaponDrawn() const { return _cachedWeaponDrawn.load(std::memory_order_relaxed); }
        bool GetCachedHasRealWeapon() const { return _cachedHasRealWeapon.load(std::memory_order_relaxed); }


        // Called from main thread to update cached weapon state
        void UpdateCachedWeaponState();

        // Queue a weapon unequip (processed in HookEndUpdate, safe context)
        void QueueWeaponUnequip(RE::TESForm* form, const char* name)
        {
            _pendingUnequipForm = form;
            _pendingUnequipName = name ? name : "Weapon";
        }

        // Queue a weapon re-equip (processed in HookEndUpdate, safe context)
        void QueueWeaponReequip(RE::TESForm* form, const char* name)
        {
            _pendingReequipForm = form;
            _pendingReequipName = name ? name : "Weapon";
        }

        // Get/clear last weapon unequipped via storage zone (for re-equip)
        RE::TESForm* GetLastUnequippedWeapon() const { return _lastUnequippedWeapon; }
        const std::string& GetLastUnequippedWeaponName() const { return _lastUnequippedWeaponName; }
        void ClearLastUnequippedWeapon() { _lastUnequippedWeapon = nullptr; _lastUnequippedWeaponName.clear(); }

        // Process pending weapon unequip - called from HookEndUpdate (after physics)
        void ProcessPendingWeaponUnequip();

        // Process pending weapon re-equip - called from HookEndUpdate (after physics)
        void ProcessPendingWeaponReequip();

        // Called from main thread to update finger close state based on grip + selection
        void UpdateFingerCloseState();
    };

    // Global access
    inline Heisenberg& g_heisenberg = Heisenberg::GetSingleton();
}
