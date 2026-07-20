#pragma once

/**
 * HeisenbergInterface001.h - Public API for Heisenberg F4VR
 * 
 * This interface allows other F4SE plugins to interact with Heisenberg's
 * grabbing, selection, and physics systems.
 * 
 * Usage:
 *   1. Include this header in your plugin
 *   2. Call GetHeisenbergInterface001() after F4SE sends kMessage_PostPostLoad
 *   3. Use the returned interface pointer to call Heisenberg functions
 * 
 * Thread Safety:
 *   - Most functions should only be called from the main game thread
 *   - Callbacks are invoked from the main game thread
 *   - IsHoldingObject/GetGrabbedObject are safe to call from any thread
 */

#include "RE/Fallout.h"
#include "DualWieldAPI.h"

namespace HeisenbergPluginAPI {

    // Forward declaration
    struct IHeisenbergInterface001;

    // =========================================================================
    // MESSAGE STRUCTURE (for F4SE messaging system)
    // =========================================================================
    
    /**
     * Message structure for requesting the Heisenberg API.
     * 
     * This follows the same pattern as HIGGS Skyrim:
     * 1. Other plugins dispatch kMessage_Heisenberg_GetInterface to "Heisenberg_F4VR"
     * 2. Heisenberg fills in the GetApiFunction callback
     * 3. Other plugins call GetApiFunction(1) to get IHeisenbergInterface001*
     */
    struct HeisenbergMessage
    {
        // Unique message type ID (randomly generated)
        enum { kMessage_GetInterface = 0xF4D3B7A2 };

        // Callback that Heisenberg fills in
        // Call with revision=1 to get IHeisenbergInterface001*
        void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
    };

    /**
     * Fire-and-forget message: suppress item-to-hand routing for a window.
     *
     * When Heisenberg's loot-to-hand / drop-to-hand features are enabled, items
     * that enter the player's inventory while a container/quick-loot menu is
     * open get routed to the player's hand (or dropped on the floor if hands are
     * full). A plugin doing a scripted bulk transfer (e.g. a companion "give me
     * everything" command) usually wants those items to land in inventory
     * instead. The engine's TESContainerChangedEvent carries no transfer reason,
     * so the transferring plugin signals Heisenberg just before its transfer.
     *
     * Two equivalent ways to trigger it:
     *   1. Interface method: IHeisenbergInterface001::SuppressItemToHand(ms)
     *   2. This message (no interface handshake required):
     *        std::uint32_t ms = 1500;
     *        messaging->Dispatch(kMessage_SuppressItemToHand, &ms, sizeof(ms),
     *                            "Heisenberg_F4VR");
     *
     * Payload: optional uint32 window length in milliseconds. If absent/zero,
     * Heisenberg uses a sensible default (~1500ms). The window is time-based, so
     * it survives thread skew and only needs to cover the synchronous transfer.
     * No-op if Heisenberg isn't installed (Dispatch finds no listener).
     */
    enum : unsigned int { kMessage_SuppressItemToHand = 0x48535550 };  // 'HSUP'

    /**
     * Fire-and-forget message: a throwable was just pulled from a Virtual Holsters
     * holster — arm/ready it in hand immediately, skipping Heisenberg's normal
     * hold-to-arm delay (fThrowableHoldDuration). Dispatched by Virtual Holsters
     * on unholster:
     *   messaging->Dispatch(kMessage_ArmThrowable, nullptr, 0, "Heisenberg_F4VR");
     * No payload required.
     */
    enum : unsigned int { kMessage_ArmThrowable = 0x48544852 };  // 'HTHR'

    /**
     * Get the Heisenberg interface.
     * Call this after F4SE sends kGameLoaded or kPostLoadGame to your plugin.
     * 
     * @param pluginHandle Your plugin's handle from F4SE
     * @param messagingInterface The F4SE messaging interface
     * @return Pointer to the interface, or nullptr if Heisenberg is not loaded
     */
    IHeisenbergInterface001* GetHeisenbergInterface001(
        const F4SE::PluginHandle& pluginHandle,
        F4SE::MessagingInterface* messagingInterface);

    /**
     * Main Heisenberg interface
     * 
     * Provides access to grabbing, selection, zone detection, and callbacks.
     */
    struct IHeisenbergInterface001
    {
        // =====================================================================
        // VERSION INFO
        // =====================================================================
        
        /**
         * Get the Heisenberg build number.
         * Use this to check for feature compatibility.
         */
        virtual unsigned int GetBuildNumber() = 0;

        // =====================================================================
        // GRAB STATE QUERIES
        // =====================================================================
        
        /**
         * Check if a hand is currently holding an object.
         * Thread-safe - can be called from any thread.
         * 
         * @param isLeft True for left hand, false for right hand
         * @return True if that hand is holding an object
         */
        virtual bool IsHoldingObject(bool isLeft) = 0;

        /**
         * Check if a hand is currently pulling an object toward it.
         * 
         * @param isLeft Which hand to check
         * @return True if object is being pulled but hasn't arrived yet
         */
        virtual bool IsPulling(bool isLeft) = 0;

        /**
         * Check if a hand can currently grab an object.
         * Returns false if hand is disabled, already holding, or in a menu.
         * 
         * @param isLeft Which hand to check
         * @return True if the hand is ready to grab
         */
        virtual bool CanGrabObject(bool isLeft) = 0;

        /**
         * Get the currently held object reference.
         * Thread-safe via handle system.
         * 
         * @param isLeft Which hand to check
         * @return Pointer to the held TESObjectREFR, or nullptr if not holding
         */
        virtual RE::TESObjectREFR* GetGrabbedObject(bool isLeft) = 0;

        /**
         * Get the name of the currently grabbed node.
         * 
         * @param isLeft Which hand to check
         * @return Node name, or empty string if not grabbing or no name
         */
        virtual const char* GetGrabbedNodeName(bool isLeft) = 0;

        // =====================================================================
        // VIEWCASTER / SELECTION QUERIES
        // =====================================================================

        /**
         * Get the object currently targeted by a hand's ViewCaster (activation target).
         * This is what the hand is pointing at and could activate/grab.
         * 
         * @param isLeft True for left/primary wand, false for right/secondary wand
         * @return Pointer to the targeted TESObjectREFR, or nullptr if nothing targeted
         */
        virtual RE::TESObjectREFR* GetViewCasterTarget(bool isLeft) = 0;

        /**
         * Get the primary wand's ViewCaster target (typically left hand / Pipboy hand).
         * Same as GetViewCasterTarget(true).
         * 
         * @return Pointer to the targeted TESObjectREFR, or nullptr
         */
        virtual RE::TESObjectREFR* GetPrimaryWandTarget() = 0;

        /**
         * Get the secondary wand's ViewCaster target (typically right hand / weapon hand).
         * Same as GetViewCasterTarget(false).
         * 
         * @return Pointer to the targeted TESObjectREFR, or nullptr
         */
        virtual RE::TESObjectREFR* GetSecondaryWandTarget() = 0;

        /**
         * Get the object selected for potential grabbing by Heisenberg's selection system.
         * This may differ from ViewCaster target due to Heisenberg's own physics raycasts.
         * 
         * @param isLeft Which hand's selection to query
         * @return Pointer to the selected TESObjectREFR, or nullptr
         */
        virtual RE::TESObjectREFR* GetSelectedObject(bool isLeft) = 0;

        // =====================================================================
        // GRAB CONTROL
        // =====================================================================

        /**
         * Programmatically grab an object with a hand.
         * The object must have collision and the hand must be in a grabbable state.
         * 
         * @param object The TESObjectREFR to grab
         * @param isLeft Which hand should grab it
         * @return True if grab was initiated successfully
         */
        virtual bool GrabObject(RE::TESObjectREFR* object, bool isLeft) = 0;

        /**
         * Drop/release the currently held object.
         * 
         * @param isLeft Which hand to release
         * @param throwVelocity Optional velocity to apply (for throwing). Can be nullptr.
         */
        virtual void DropObject(bool isLeft, const RE::NiPoint3* throwVelocity = nullptr) = 0;

        /**
         * Force end a grab without physics restoration.
         * Use this when the object is about to be deleted or consumed.
         * 
         * @param isLeft Which hand to release
         */
        virtual void ForceEndGrab(bool isLeft) = 0;

        // =====================================================================
        // HAND STATE CONTROL
        // =====================================================================

        /**
         * Disable grabbing/selection for a hand.
         * The hand will not select or grab objects until re-enabled.
         * 
         * @param isLeft Which hand to disable
         */
        virtual void DisableHand(bool isLeft) = 0;

        /**
         * Re-enable grabbing/selection for a hand.
         * 
         * @param isLeft Which hand to enable
         */
        virtual void EnableHand(bool isLeft) = 0;

        /**
         * Check if a hand is disabled.
         * 
         * @param isLeft Which hand to check
         * @return True if the hand is disabled
         */
        virtual bool IsHandDisabled(bool isLeft) = 0;

        // =====================================================================
        // FINGER TRACKING
        // =====================================================================

        /**
         * Get the current finger curl values for a hand.
         * Values range from 0.0 (fully open) to 1.0 (fully curled).
         * Order: [thumb, index, middle, ring, pinky]
         * 
         * @param isLeft Which hand
         * @param values Output array of 5 floats
         */
        virtual void GetFingerCurls(bool isLeft, float values[5]) = 0;

        /**
         * Set finger curl values for a hand.
         * This overrides the automatic finger positioning based on held object.
         * Values range from 0.0 (fully open) to 1.0 (fully curled).
         * 
         * @param isLeft Which hand
         * @param values Array of 5 floats [thumb, index, middle, ring, pinky]
         */
        virtual void SetFingerCurls(bool isLeft, const float values[5]) = 0;

        // =====================================================================
        // ZONE DETECTION
        // =====================================================================

        /**
         * Check if a hand is currently in the storage zone (behind head).
         * Items held in this zone can be stashed to inventory.
         * 
         * @param isLeft Which hand to check
         * @return True if the hand is in the storage zone
         */
        virtual bool IsInStorageZone(bool isLeft) = 0;

        /**
         * Check if a hand is currently in an equip zone (body areas).
         * Armor/weapons released here will be equipped.
         * 
         * @param isLeft Which hand to check
         * @return True if the hand is in an equip zone
         */
        virtual bool IsInEquipZone(bool isLeft) = 0;

        /**
         * Check if a hand is currently in the mouth/consume zone.
         * Consumables held here will be used.
         * 
         * @param isLeft Which hand to check
         * @return True if the hand is in the mouth zone
         */
        virtual bool IsInMouthZone(bool isLeft) = 0;

        /**
         * Check if a hand is currently in a VirtualHolsters zone.
         * Weapons released here will be holstered via VH API.
         * 
         * @param isLeft Which hand to check
         * @return True if the hand is in a VH holster zone
         */
        virtual bool IsInVHZone(bool isLeft) = 0;

        /**
         * Get the name of the current zone the hand is in.
         * Returns zone names like "HEAD", "CHEST", "LEFT_HIP", etc.
         * 
         * @param isLeft Which hand to check
         * @return Zone name string, or empty string if not in any zone
         */
        virtual const char* GetCurrentZoneName(bool isLeft) = 0;

        /**
         * Get the VirtualHolsters zone index for the current hand position.
         * 0=None, 1=LeftShoulder, 2=RightShoulder, 3=LeftHip, 4=RightHip,
         * 5=LowerBack, 6=LeftChest, 7=RightChest
         * 
         * @param isLeft Which hand to check
         * @return VH zone index (0 if not in a zone or VH not available)
         */
        virtual int GetVHZoneIndex(bool isLeft) = 0;

        // =====================================================================
        // INVENTORY INTEGRATION
        // =====================================================================

        /**
         * Spawn an item from player inventory to a hand.
         * The item will appear and be grabbed by the specified hand.
         * 
         * @param form The base form of the item to spawn
         * @param isLeft Which hand should receive the item
         * @return True if the item was successfully spawned and grabbed
         */
        virtual bool DropToHand(RE::TESForm* form, bool isLeft) = 0;

        /**
         * Check if a hand is near an interactive activator (button, switch, door, terminal, etc.).
         * Uses Heisenberg's proximity detection system (finger tip distance to activator).
         *
         * @param isLeft Which hand to check
         * @return True if the hand is within pointing range of an activator
         */
        virtual bool IsInActivationZone(bool isLeft) = 0;

        // =====================================================================
        // TRANSFORM CONTROL
        // =====================================================================

        /**
         * Get the current grab transform (hand-to-object offset).
         * This is how the object is positioned relative to the hand.
         * 
         * @param isLeft Which hand
         * @return The current grab transform
         */
        virtual RE::NiTransform GetGrabTransform(bool isLeft) = 0;

        /**
         * Set the grab transform (hand-to-object offset).
         * Changes how the held object is positioned relative to the hand.
         * 
         * @param isLeft Which hand
         * @param transform The new grab transform
         */
        virtual void SetGrabTransform(bool isLeft, const RE::NiTransform& transform) = 0;

        // =====================================================================
        // CALLBACKS
        // =====================================================================

        /**
         * Callback when an object starts being grabbed.
         * @param isLeft Which hand grabbed it
         * @param grabbedRefr The object that was grabbed
         */
        typedef void(*GrabbedCallback)(bool isLeft, RE::TESObjectREFR* grabbedRefr);

        /**
         * Callback when an object is dropped/released.
         * @param isLeft Which hand dropped it
         * @param droppedRefr The object that was dropped
         */
        typedef void(*DroppedCallback)(bool isLeft, RE::TESObjectREFR* droppedRefr);

        /**
         * Callback when an object is stashed to inventory (behind head).
         * The object no longer exists in the world after this.
         * @param isLeft Which hand stashed it
         * @param stashedForm The base form of the stashed item
         */
        typedef void(*StashedCallback)(bool isLeft, RE::TESForm* stashedForm);

        /**
         * Callback when a consumable is consumed (mouth zone).
         * The item has been used/consumed.
         * @param isLeft Which hand consumed it
         * @param consumedForm The base form of the consumed item
         */
        typedef void(*ConsumedCallback)(bool isLeft, RE::TESForm* consumedForm);

        /**
         * Callback when an object starts being pulled toward the hand.
         * @param isLeft Which hand is pulling
         * @param pulledRefr The object being pulled
         */
        typedef void(*PulledCallback)(bool isLeft, RE::TESObjectREFR* pulledRefr);

        /**
         * Callback when the hand or held object collides with something.
         * Only called if hand collision is enabled.
         * @param isLeft Which hand collided
         * @param mass Mass of the collided object
         * @param separatingVelocity Collision velocity (higher = harder hit)
         */
        typedef void(*CollisionCallback)(bool isLeft, float mass, float separatingVelocity);

        /**
         * Callback before physics simulation runs.
         *
         * NOTE: Currently NOT fired. The pre-physics hook (0xd83ec4) is disabled
         * due to crashes. Registering this callback will succeed but the callback
         * will never be invoked until the hook is restored.
         * Use AddPostPhysicsCallback for post-physics timing.
         *
         * @param bhkWorld Pointer to the bhkWorld (nullptr when dormant)
         */
        typedef void(*PrePhysicsCallback)(void* bhkWorld);

        /**
         * Callback after physics simulation runs.
         * Fired once per frame from the post-physics hook (0xd8405e),
         * after all Heisenberg grab/input processing completes.
         * @param bhkWorld Currently nullptr (reserved for future use)
         */
        typedef void(*PostPhysicsCallback)(void* bhkWorld);

        /**
         * Callback when ViewCaster target changes.
         * @param isLeft Which wand's target changed
         * @param newTarget The new target (can be nullptr)
         * @param oldTarget The previous target (can be nullptr)
         */
        typedef void(*ViewCasterTargetChangedCallback)(bool isLeft, RE::TESObjectREFR* newTarget, RE::TESObjectREFR* oldTarget);

        virtual void AddGrabbedCallback(GrabbedCallback callback) = 0;
        virtual void AddDroppedCallback(DroppedCallback callback) = 0;
        virtual void AddStashedCallback(StashedCallback callback) = 0;
        virtual void AddConsumedCallback(ConsumedCallback callback) = 0;
        virtual void AddPulledCallback(PulledCallback callback) = 0;
        virtual void AddCollisionCallback(CollisionCallback callback) = 0;
        virtual void AddPrePhysicsCallback(PrePhysicsCallback callback) = 0;
        virtual void AddPostPhysicsCallback(PostPhysicsCallback callback) = 0;
        virtual void AddViewCasterTargetChangedCallback(ViewCasterTargetChangedCallback callback) = 0;

        // =====================================================================
        // SETTINGS
        // =====================================================================

        /**
         * Get a Heisenberg INI setting value.
         * 
         * @param name The setting name (e.g., "fGrabStrength", "bEnablePullToHand")
         * @param out Output parameter for the value
         * @return True if the setting exists and was retrieved
         */
        virtual bool GetSettingDouble(const char* name, double& out) = 0;

        /**
         * Set a Heisenberg INI setting value.
         * Note: Not all settings take effect immediately; some require reload.
         * 
         * @param name The setting name
         * @param val The new value
         * @return True if the setting exists and was set
         */
        virtual bool SetSettingDouble(const char* name, double val) = 0;

        // =====================================================================
        // HAND COLLISION (if enabled)
        // =====================================================================

        /**
         * Check if hand collision is enabled.
         * When enabled, hands have physics bodies that can push objects.
         * 
         * @return True if hand collision is enabled
         */
        virtual bool IsHandCollisionEnabled() = 0;

        /**
         * Get the physics body for a hand.
         * Only valid if hand collision is enabled.
         * 
         * @param isLeft Which hand
         * @return Pointer to bhkNPCollisionObject, or nullptr if not available
         */
        virtual void* GetHandRigidBody(bool isLeft) = 0;

        /**
         * Check if a hand is currently in contact with an object.
         * Only valid if hand collision is enabled.
         * 
         * @param isLeft Which hand
         * @return True if the hand is touching something
         */
        virtual bool IsHandInContact(bool isLeft) = 0;

        /**
         * Get the object the hand is currently touching.
         * Only valid if hand collision is enabled and IsHandInContact returns true.
         *
         * @param isLeft Which hand
         * @return Pointer to the contacted TESObjectREFR, or nullptr
         */
        virtual RE::TESObjectREFR* GetHandContactObject(bool isLeft) = 0;

        // =====================================================================
        // ITEM-TO-HAND SUPPRESSION
        // =====================================================================

        /**
         * Suppress loot-to-hand / drop-to-hand routing for a short window so a
         * scripted bulk inventory transfer (e.g. a companion "give me everything"
         * command) lands directly in the player's inventory instead of being
         * routed to the hand or dropped on the floor.
         *
         * Call this immediately before performing the transfer (RemoveItem with
         * a_otherContainer = player, AddItem, etc.). The window is time-based and
         * covers the synchronous transfer plus any thread skew.
         *
         * Equivalent to dispatching kMessage_SuppressItemToHand. Safe to call
         * from the main game thread.
         *
         * @param durationMs Window length in milliseconds (0 = default ~1500ms)
         */
        virtual void SuppressItemToHand(unsigned int durationMs) = 0;

        // =====================================================================
        // WEAPON COLLISION + TWO-HANDING CONTROL (Virtual Reloads API, Jul 19)
        // ABI: appended at the END of the vtable — never reorder or insert above.
        // AVAILABILITY: every function below this line exists only when
        // GetBuildNumber() >= 2 — guard with that check before calling, or a vtable
        // slot that does not exist on an older Heisenberg DLL will be invoked.
        // =====================================================================

        /**
         * Enable/disable the equipped-weapon collision body (embedded ROCK engine).
         * Latch semantics: disabled stays disabled until EnableWeaponCollision(true).
         * Re-enable takes effect the same frame.
         */
        virtual void EnableWeaponCollision(bool enable) = 0;

        /** True while weapon collision is disabled via this API (lease active). */
        virtual bool IsWeaponCollisionDisabled() = 0;

        /**
         * Block/unblock off-hand weapon gripping (two-handed support grip).
         * Blocking releases an engaged support grip and refuses new captures.
         * Latch semantics: blocked until unblocked. tag is informational
         * (logged), mirroring FRIK's blockOffHandWeaponGripping signature.
         */
        virtual void BlockOffHandWeaponGripping(const char* tag, bool block) = 0;

        /** True while off-hand weapon gripping is blocked via this API. */
        virtual bool IsOffHandWeaponGrippingBlocked() = 0;

        /** True while either hand holds a two-handed support grip on the weapon. */
        virtual bool IsOffHandGrippingWeapon() = 0;

        /**
         * Disable this hand's PHYSICS collision (embedded ROCK hand+arm colliders) —
         * separate from DisableHand, which only affects grab/selection. Latch
         * semantics: disabled until EnableHandCollision, which restores
         * IMMEDIATELY (same frame — pair caches are rebuilt, no settle delay).
         */
        virtual void DisableHandCollision(bool isLeft) = 0;
        virtual void EnableHandCollision(bool isLeft) = 0;

        /** True while this hand's physics collision is disabled via this API. */
        virtual bool IsHandCollisionDisabled(bool isLeft) = 0;

        // =====================================================================
        // DUAL WIELD BRIDGE
        // ABI: build 3, appended after every build-2 slot above. Never reorder.
        // All methods below require GetBuildNumber() >= 3.
        // Registration owns one callback slot and never replaces another owner.
        // Unregister waits for in-flight calls. A callback/provider must therefore
        // never unregister itself from inside its own invocation.
        // =====================================================================
        virtual bool RegisterDualWieldStateProvider(DualWieldStateProvider provider) = 0;
        virtual bool UnregisterDualWieldStateProvider(DualWieldStateProvider provider) = 0;

        // Main-game-thread query. The caller initializes abiVersion, structSize,
        // and physicalHand (MakeInputState does this).
        virtual bool GetPhysicalHandInputState(
            PhysicalHand hand,
            PhysicalHandInputState* state) = 0;

        virtual bool RegisterDualWieldWeaponContactCallback(DualWieldWeaponContactCallback callback) = 0;
        virtual bool UnregisterDualWieldWeaponContactCallback(DualWieldWeaponContactCallback callback) = 0;
    };

    // =========================================================================
    // INTERNAL FUNCTIONS (used by Heisenberg)
    // =========================================================================
    
    /**
     * Handle an interface request message.
     * Called internally by Heisenberg when receiving kMessage_GetInterface.
     *
     * @param message The HeisenbergMessage to fill in
     */
    void HandleInterfaceRequest(HeisenbergMessage* message);

    /**
     * Get the API interface pointer for a given revision.
     * Used by DLL export and messaging system.
     *
     * @param revisionNumber 1 for IHeisenbergInterface001
     * @return Interface pointer, or nullptr if revision not supported
     */
    void* GetApi(unsigned int revisionNumber);

    // =========================================================================
    // CALLBACK INVOCATION (called from Heisenberg internals)
    // =========================================================================
    
    void InvokeGrabbedCallbacks(bool isLeft, RE::TESObjectREFR* refr);
    void InvokeDroppedCallbacks(bool isLeft, RE::TESObjectREFR* refr);
    void InvokeStashedCallbacks(bool isLeft, RE::TESForm* form);
    void InvokeConsumedCallbacks(bool isLeft, RE::TESForm* form);
    void InvokePulledCallbacks(bool isLeft, RE::TESObjectREFR* refr);
    void InvokeCollisionCallbacks(bool isLeft, float mass, float velocity);
    void InvokePrePhysicsCallbacks(void* bhkWorld);
    void InvokePostPhysicsCallbacks(void* bhkWorld);
    void InvokeViewCasterTargetChangedCallbacks(bool isLeft, RE::TESObjectREFR* newTarget, RE::TESObjectREFR* oldTarget);

    /** Check if a hand is disabled via the API (for internal use by Hand.cpp, Grab.cpp). */
    bool IsHandDisabledByAPI(bool isLeft);

    /** Lease-evaluated external control state (for Hooks.cpp per-frame push into ROCK). */
    bool IsWeaponCollisionDisabledByAPI();
    bool IsOffHandGripBlockedByAPI();
    bool IsHandCollisionDisabledByAPI(bool isLeft);

} // namespace HeisenbergPluginAPI

// Global interface pointer - set after calling GetHeisenbergInterface001
extern HeisenbergPluginAPI::IHeisenbergInterface001* g_heisenbergInterface;
