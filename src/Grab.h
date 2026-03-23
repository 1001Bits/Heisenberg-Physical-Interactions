#pragma once

#include "Selection.h"
#include "ItemOffsets.h"
#include "HavokPhysicsKeyframe.h"
#include "RE/Fallout.h"
#include <unordered_map>
#include <chrono>

namespace heisenberg
{
    // Tick the deferred disable queue (call once per frame from main update)
    void TickDeferredDisables();

    // Safe disable: defers for items with behavior graphs (weapons, armor, holotapes)
    // to prevent hkbBehaviorGraph IO thread crash. Immediate for everything else.
    void SafeDisable(RE::TESObjectREFR* refr);

    // Storage zone detection result
    struct StorageZoneResult
    {
        bool isInZone = false;
        bool isLeftSide = false;  // Which side triggered (for shoulder zones)
    };

    // Check if hand position is within the item storage zone (behind head)
    StorageZoneResult CheckItemStorageZone(const RE::NiPoint3& handPos);

    // Check if a real weapon (gun, melee) is drawn - uses scene graph mesh detection
    // Returns true if should BLOCK grab, false if grab allowed
    bool HasRealWeaponEquipped();
    
    // Check if an object reference is storable (can be picked up to inventory)
    // Used by selection priority system to prefer storable items
    bool IsStorableItem(RE::TESObjectREFR* refr);
    
    /**
     * GrabMode - Determines how grabbing works physically
     */
    enum class GrabMode
    {
        Keyframed = 0,    // Simple: set motion to KEYFRAMED and use ApplyHardKeyframe
        BallSocket = 1,   // Physics: use ball-socket constraint (BROKEN - crashes)
        Motor = 2,        // 6-DOF motor constraint (like Skyrim HIGGS) (BROKEN - crashes)
        VirtualSpring = 3, // Apply forces to simulate spring (no HandBody needed)
        MouseSpring = 4   // Use native game mouse spring system - RECOMMENDED
    };

    /**
     * GrabState - Tracks the state of a grabbed object
     * 
     * Since F4VR uses Havok's New Physics (hknp*) which has a different constraint API
     * than Skyrim VR's hkp* API, we implement grabbing using velocity-based manipulation
     * initially, with the option to add proper constraint-based grabbing later.
     * 
     * THREAD SAFETY: Uses RE::ObjectRefHandle for safe reference lookup.
     * LIFETIME SAFETY: Uses RE::NiPointer<> for scene graph nodes.
     */
    struct GrabState
    {
        bool active = false;
        bool usingMouseSpring = false;  // True if using native mouse spring system
        bool isPulling = false;         // True if object is being pulled to hand
        bool pendingFingerCurls = false; // True if finger curls deferred until pull completes
        bool keyframedSetupComplete = false;  // True once KEYFRAMED physics is set up (first frame)
        bool isDynamicNodeGrab = false; // True if grabbed from DynamicNode - don't reparent, just update transform
        bool usedSnapMode = false;      // True if snap positioning was used (distance > 10cm)
        bool usingKeyframedMode = false;   // True if using KEYFRAMED physics mode (for interiors, DynamicNode, proxy objects)
        bool isProxyCollision = false;  // True if physics is on a child node (ragdolls, animated toys)

        // =========================================================================
        // SAFE OBJECT REFERENCES - Use handles and smart pointers
        // =========================================================================
        
        // Handle-based reference - thread-safe and validates object existence
        RE::ObjectRefHandle refrHandle;
        
        // Smart pointers for scene graph nodes - reference counted, prevents dangling
        RE::NiPointer<RE::NiAVObject> node;           // The grabbed visual root node
        RE::NiPointer<RE::NiAVObject> physicsNode;    // The physics node (same as node unless proxy collision)
        
        // Collision object is NOT ref-counted in Bethesda's system, but we validate before use
        RE::bhkNPCollisionObject* collisionObject = nullptr;
        
        // Original parent for restoring on release - smart pointer for safety
        RE::NiPointer<RE::NiNode> originalParent;
        // NOTE: wandNode removed - storing NiPointer to player skeleton nodes
        // caused crashes during flashlight toggle (engine's UpdateDownwardPass)
        // We now get fresh wand node via getPlayerNodes() each frame instead

        // Transform information
        RE::NiPoint3 grabOffsetLocal;      // Offset from hand to grab point in object local space
        RE::NiMatrix3 grabRotationLocal;   // Rotation offset in object local space
        RE::NiPoint3 initialHandPos;       // Hand position when grab started
        RE::NiMatrix3 initialHandRot;      // Hand rotation when grab started
        RE::NiPoint3 initialObjectPos;     // Object position when grab started (for pulling)
        float pullProgress = 0.0f;         // 0.0 to 1.0 progress of pull animation

        // Item-specific offset from JSON config
        ItemOffset itemOffset;              // Per-item positioning offset
        bool hasItemOffset = false;         // True if custom offset was found for this item
        bool isFRIKOffset = false;          // True if using FRIK-style offset (needs Weapon node parent transform)

        // Sticky grab mode for config - keeps item grabbed even without grip held
        bool stickyGrab = false;            // True if in sticky grab mode (for repositioning)

        // Physics state saved when grabbed
        struct SavedPhysicsState
        {
            RE::hknpMotionPropertiesId::Preset motionType = RE::hknpMotionPropertiesId::Preset::DYNAMIC;
            float linearDamping = 0.0f;
            float angularDamping = 0.0f;
            bool wasDeactivated = false;
            std::uint32_t collisionLayer = 4;  // Default to kClutter, save original on grab
            std::uint16_t collisionObjectFlags = 0;  // bhkNPCollisionObjectBase flags - restore on release (like HIGGS)
            RE::bhkWorld* savedBhkWorld = nullptr;  // Store the world we removed physics from (for interior cells)
        } savedState;

        // Grab parameters
        float grabStrength = 10.0f;        // Velocity multiplier for position correction
        float rotationStrength = 5.0f;     // Angular velocity multiplier
        float maxVelocity = 10.0f;         // Maximum velocity to apply
        float maxAngularVelocity = 10.0f;  // Maximum angular velocity

        // Keyframed physics helper - manages direct hknp body manipulation
        KeyframedPhysicsHelper keyframedHelper;

        // Cached target for post-physics update
        RE::NiPoint3 lastTargetPos;
        RE::NiMatrix3 lastTargetRot;
        float lastDeltaTime = 0.016f;
        bool needsPostPhysicsUpdate = false;

        // Hand velocity tracking (for shoulder/mouth detection)
        RE::NiPoint3 lastHandPos;
        float handSpeed = 0.0f;  // Computed from position delta
        
        // Behind-ear storage timer (item stored when held behind head for 2+ seconds)
        float behindEarTimer = 0.0f;  // Accumulates time spent in storage zone
        float lastStoragePulseTime = 0.0f;  // Per-hand: last time haptic pulse fired in storage zone
        bool isInStorageZone = false; // True if hand is currently in a storage zone
        
        // Equip zone tracking (equips armor/weapons on grip release when in body zone)
        bool isInEquipZone = false; // True if held armor/weapon is in equip zone
        const char* currentZoneName = ""; // Zone name for HUD messages (HEAD, CHEST, LEGS)
        
        // VirtualHolsters zone tracking (holsters weapons on grip release in VH zone)
        bool isInVHZone = false;           // True if hand is in a VirtualHolsters holster zone
        std::uint32_t vhHolsterSlot = 0;   // Which VH slot (1-7) the hand is in
        bool vhHandModeSwitched = false;   // True if we called switchHandMode() for offhand grab
        
        // Mouth consume zone timer (consumes consumables when held near mouth for 0.5+ seconds)
        float mouthZoneTimer = 0.0f;   // Accumulates time spent in mouth zone
        bool isInMouthZone = false;    // True if wand is currently in mouth zone

        // Per-hand mouth zone visit tracking (prevents cross-hand state bleeding)
        bool consumeAttemptedThisVisit = false;  // Track if consume was attempted this visit
        bool wasInMouthZoneLocal = false;        // Track zone entry/exit per hand

        // Hand injection zone (opposite hand)
        bool isInHandInjectionZone = false;      // True if wand is in opposite hand's injection zone
        bool wasInHandInjectionZoneLocal = false; // Track zone entry/exit
        
        // Loot drop protection - prevent accidental equip immediately after looting
        float grabStartTime = 0.0f;   // Time (in seconds) when grab started
        bool isFromLootDrop = false;  // True if this grab was initiated by DropToHand (looting)
        bool isFromSmartGrab = false; // True if this grab was initiated by SmartGrab (behind head retrieval)
        bool skipStorageZone = false; // True to skip storage zone (e.g. holotape from deck near Pipboy)
        bool isNaturalGrab = false;   // True if this was a natural grab (close to object)
        bool isTelekinesis = false;   // True if this is a telekinesis grab (object follows hand from distance)

        // Simple mode room tracking (per-grab state, not global)
        RE::NiPoint3 lastRoomPos;       // Last frame's room node position
        RE::NiPoint3 smoothedRoomDelta; // Smoothed room movement delta
        bool roomTrackingInitialized = false;  // First frame flag
        
        // =========================================================================
        // VELOCITY TRACKING FOR DEBUGGING/TUNING
        // =========================================================================
        RE::NiPoint3 prevWandPos;          // Previous frame wand world position
        RE::NiPoint3 prevObjectPos;        // Previous frame object world position
        RE::NiPoint3 wandVelocity;         // Wand velocity (units/frame)
        RE::NiPoint3 objectVelocity;       // Object velocity (units/frame)
        RE::NiPoint3 velocityError;        // Difference between wand and object velocity
        bool velocityTrackingInit = false; // First frame flag for velocity tracking

        // =========================================================================
        // SAFE REFERENCE ACCESS
        // =========================================================================
        
        /**
         * Get the TESObjectREFR if it's still valid.
         * Returns nullptr if the object has been deleted.
         * Thread-safe via handle system.
         */
        RE::TESObjectREFR* GetRefr() const
        {
            if (!refrHandle) return nullptr;
            RE::NiPointer<RE::TESObjectREFR> refPtr = refrHandle.get();
            return refPtr.get();
        }

        /**
         * Set the reference from a raw pointer.
         * Creates a handle for safe lookup later.
         */
        void SetRefr(RE::TESObjectREFR* refr)
        {
            if (refr) {
                refrHandle = RE::ObjectRefHandle(refr);
            } else {
                refrHandle.reset();
            }
        }

        /**
         * Check if grab state has a valid reference.
         */
        bool HasValidRefr() const { return GetRefr() != nullptr; }

        void Clear()
        {
            active = false;
            usingMouseSpring = false;
            isPulling = false;
            pendingFingerCurls = false;
            keyframedSetupComplete = false;
            isDynamicNodeGrab = false;
            usingKeyframedMode = false;
            isProxyCollision = false;
            lastRoomPos = RE::NiPoint3();
            smoothedRoomDelta = RE::NiPoint3();
            roomTrackingInitialized = false;
            refrHandle.reset();
            node.reset();
            physicsNode.reset();
            collisionObject = nullptr;
            originalParent.reset();
            // wandNode removed - was causing crashes by holding ref to player skeleton
            grabOffsetLocal = RE::NiPoint3();
            grabRotationLocal = RE::NiMatrix3();
            initialHandPos = RE::NiPoint3();
            initialHandRot = RE::NiMatrix3();
            initialObjectPos = RE::NiPoint3();
            pullProgress = 0.0f;
            itemOffset = ItemOffset();
            hasItemOffset = false;
            isFRIKOffset = false;
            stickyGrab = false;
            savedState = SavedPhysicsState();
            keyframedHelper = KeyframedPhysicsHelper();  // Reset helper
            lastTargetPos = RE::NiPoint3();
            lastTargetRot = RE::NiMatrix3();
            lastDeltaTime = 0.016f;
            needsPostPhysicsUpdate = false;
            lastHandPos = RE::NiPoint3();
            handSpeed = 0.0f;
            behindEarTimer = 0.0f;
            lastStoragePulseTime = 0.0f;
            isInStorageZone = false;
            isInEquipZone = false;
            isInVHZone = false;
            vhHolsterSlot = 0;
            vhHandModeSwitched = false;
            consumeAttemptedThisVisit = false;
            wasInMouthZoneLocal = false;
            currentZoneName = "";
            grabStartTime = 0.0f;
            isFromLootDrop = false;
            isFromSmartGrab = false;
            skipStorageZone = false;
            isNaturalGrab = false;
            isTelekinesis = false;
            // Velocity tracking
            prevWandPos = RE::NiPoint3();
            prevObjectPos = RE::NiPoint3();
            wandVelocity = RE::NiPoint3();
            objectVelocity = RE::NiPoint3();
            velocityError = RE::NiPoint3();
            velocityTrackingInit = false;
        }
    };

    /**
     * GrabManager - Manages physics-based grabbing of objects
     *
     * This uses a velocity-based approach where we calculate the desired
     * position of the grabbed object and apply velocities to move it there.
     * This is simpler than constraints but works well for basic grabbing.
     *
     * Thread safety:
     *   - Main thread: StartGrab, UpdateGrab, EndGrab, PrePhysicsUpdate, PostPhysicsUpdate, PreRenderUpdate
     *   - OpenVR callback thread: IsGrabbing (read-only on _leftGrab.active / _rightGrab.active)
     *   - _releaseCooldowns: protected by _cooldownMutex (accessed from main thread + possible OpenVR reads)
     *   - _leftGrab/_rightGrab: main-thread only (except .active read from OpenVR thread)
     */
    class GrabManager
    {
    public:
        static GrabManager& GetSingleton()
        {
            static GrabManager instance;
            return instance;
        }

        /**
         * Start grabbing an object at the selection point
         * @param selection The selection to grab
         * @param handPos Current hand position in world space
         * @param handRot Current hand rotation
         * @param isLeft True if this is the left hand
         * @param skipWeaponEquip If true, don't auto-equip picked up weapons (for DropToHand)
         * @return true if grab was successful
         */
        bool StartGrab(const Selection& selection, const RE::NiPoint3& handPos, 
                       const RE::NiMatrix3& handRot, bool isLeft, bool skipWeaponEquip = false);

        /**
         * Update the grabbed object's position/velocity
         * Called each frame while holding
         * @param handPos Current hand position
         * @param handRot Current hand rotation
         * @param isLeft Which hand
         * @param deltaTime Frame delta time
         */
        void UpdateGrab(const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                        bool isLeft, float deltaTime);

        /**
         * Release the grabbed object
         * @param isLeft Which hand
         * @param throwVelocity Optional velocity to apply when releasing
         * @param forStorage If true, skip node restoration (object is about to be deleted)
         */
        void EndGrab(bool isLeft, const RE::NiPoint3* throwVelocity = nullptr, bool forStorage = false);

        /**
         * Check if a hand is currently grabbing
         * @param isLeft Which hand to check
         * @return true if that hand is holding an object
         */
        bool IsGrabbing(bool isLeft) const;

        /**
         * Post-physics update - runs AFTER engine physics step
         * This is where we apply our position/velocity changes so they
         * won't be overwritten by the physics simulation.
         */
        void PostPhysicsUpdate();

        /**
         * Pre-physics update - runs BEFORE engine physics step
         * THE KEY TO ELIMINATING LATENCY!
         * Updates grabbed object positions BEFORE physics runs, so changes
         * take effect THIS frame instead of next frame.
         */
        void PrePhysicsUpdate();
        
        /**
         * Pre-render update - runs just before rendering
         * Updates grabbed object VISUAL positions with final wand position
         * after player movement has been applied. No prediction needed.
         */
        void PreRenderUpdate();

        /**
         * Override the native grab system's target position
         * Call this each frame to make the natively-grabbed object follow the hand
         * @param handPos The hand position in world space
         */
        void OverrideNativeGrabPosition(const RE::NiPoint3& handPos);

        /**
         * Get the current grab state for a hand
         * @param isLeft Which hand
         * @return Reference to the grab state
         */
        const GrabState& GetGrabState(bool isLeft) const;
        GrabState& GetGrabState(bool isLeft);

        /**
         * Check if the grabbed object is still being pulled to hand
         * @param isLeft Which hand
         * @return true if object is still being pulled, false if in hand or not grabbing
         */
        bool IsPulling(bool isLeft) const;
        
        /**
         * Clear all grab state - call on save load to prevent stuck grabs
         */
        void ClearAllState();

        /**
         * Properly release all grabbed objects (restores physics, drops objects).
         * Call on kPreSaveGame/kPreLoadGame so objects are saved at their natural position
         * with DYNAMIC physics instead of floating at the hand position with KEYFRAMED physics.
         */
        void ForceReleaseAll();
        
        /**
         * Start grabbing a specific TESObjectREFR directly (used by DropToHand)
         * Creates a Selection internally with the object at the hand position
         * @param refr The reference to grab
         * @param isLeft Which hand to grab with
         * @param stickyGrab If true, item stays grabbed without holding grip (default: true)
         * @param instantGrab If true, skip pull animation and appear in hand instantly (default: false)
         * @return true if grab was successful
         */
        bool StartGrabOnRef(RE::TESObjectREFR* refr, bool isLeft, bool stickyGrab = true, bool instantGrab = false, bool skipWeaponEquip = false, bool forceOffset = false);

        /**
         * Check if an item is on cooldown (recently dropped)
         * Used by Hand.cpp to skip selecting recently dropped items
         */
        bool IsOnCooldown(std::uint32_t formID) const;
        
        /**
         * Add an item to the cooldown list
         */
        void AddCooldown(std::uint32_t formID);

        GrabState& GetLeftGrab() { return _leftGrab; }
        GrabState& GetRightGrab() { return _rightGrab; }
        GrabState& GetGrab(bool isLeft) { return isLeft ? _leftGrab : _rightGrab; }

        /**
         * Process any pending holster request queued by PickupWeaponForHolster.
         * Called from OnGrabUpdate (next frame after EndGrab) so VH's heavy NIF
         * cloning (displayWeapon) runs outside our grab release flow, avoiding deadlock.
         */
        void ProcessPendingHolster();

        /**
         * Queue a holster request to be processed on the next frame.
         * Stores all parameters needed for AddHolster + ActivateRef.
         */
        void QueueHolsterRequest(RE::ObjectRefHandle refrHandle, std::uint32_t holsterIndex,
                                 const std::string& weaponName);

        /**
         * Check if there is a pending holster request
         */
        bool HasPendingHolster() const { return _pendingHolster.pending; }

    private:
        GrabManager() = default;
        ~GrabManager() = default;
        GrabManager(const GrabManager&) = delete;
        GrabManager& operator=(const GrabManager&) = delete;

        // Helper methods
        bool SetupGrabPhysics(GrabState& state);
        void RestorePhysics(GrabState& state);
        void ApplyGrabVelocity(GrabState& state, const RE::NiPoint3& targetPos,
                               const RE::NiMatrix3& targetRot, float deltaTime);
        RE::bhkNPCollisionObject* GetCollisionObject(RE::TESObjectREFR* refr);
        
        // Virtual spring mode - applies forces to make object follow hand
        void UpdateVirtualSpring(GrabState& state, const RE::NiPoint3& targetPos,
                                 const RE::NiMatrix3& targetRot, float deltaTime);

        // State for each hand
        GrabState _leftGrab;
        GrabState _rightGrab;
        
        // Release cooldown - prevents re-selecting same item too quickly
        static constexpr float kReleaseCooldown = 1.0f;  // Seconds before dropped item can be selected again
        std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> _releaseCooldowns;
        mutable std::mutex _cooldownMutex;  // Thread safety: protects _releaseCooldowns

        void CleanupCooldowns();  // Remove expired entries

        // Deferred holster request — queued during EndGrab, processed next frame
        // This avoids deadlock: VH's displayWeapon does heavy NIF cloning that
        // conflicts with locks held during our EndGrab flow.
        struct PendingHolsterRequest {
            bool pending = false;
            int framesRemaining = 0;              // Countdown: process when reaches 0
            RE::ObjectRefHandle refrHandle;       // Safe handle to weapon ref (still in world)
            std::uint32_t holsterIndex = 0;
            std::string weaponName;               // For logging and IsWeaponAlreadyHolstered check
        };
        PendingHolsterRequest _pendingHolster;
    };
}
