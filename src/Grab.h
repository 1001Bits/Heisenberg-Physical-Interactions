#pragma once

#include "Selection.h"
#include "ItemOffsets.h"
#include "HavokPhysicsKeyframe.h"
#include "RE/Fallout.h"
#include <array>
#include <unordered_map>
#include <chrono>

namespace heisenberg
{
    // True only for actual HOLOTAPES (playable on the Pip-Boy: voice logs, programs/games,
    // terminal tapes). Paper notes (kImage/kText) are kNOTE too but return false here, so they
    // can be treated as ordinary clutter — any hand, no deck scaling, normal grab placement.
    inline bool IsHolotapeNote(RE::TESForm* form)
    {
        if (!form || form->GetFormType() != RE::ENUM_FORM_ID::kNOTE) return false;
        auto* note = static_cast<RE::BGSNote*>(form);
        using T = RE::BGSNote::NOTE_TYPE;
        return note->type == T::kVoice || note->type == T::kProgram || note->type == T::kTerminal;
    }

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
     * GrabMode - Determines how grabbing works physically.
     * Two-scenario architecture: only two modes survive. Historical modes 1-8
     * (constraint/spring/mouse-spring/DynamicRock/forceGrab/RockGrabCore) were
     * removed; any other configured value degrades to Keyframed.
     */
    enum class GrabMode
    {
        Keyframed = 0,    // Simple: set motion to KEYFRAMED and use ApplyHardKeyframe
        FullDynamic = 9 // The EMBEDDED ROCK ENGINE owns grab + selection 1:1 (upstream Jul-6 code:
                          // its own input read, selection beam, force-grab, saved offsets, throw).
                          // Heisenberg's grab/selection stand DOWN for grip input (OnGrabPressed/
                          // OnGrabReleased become no-ops) and rock::HostSetGrabOwnership forces the
                          // embed's bGrabEnabled/bSelectionEnabled ON across ini reloads. Requires
                          // bUseRockEngineArchitecture=1 + HostLoad OK; else degrades to Keyframed.
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
        bool physicalTouchGrab = false; // Selection came from ROCK's exact native hand/body contact
        std::uint32_t physicalTouchBodyId = 0x7FFF'FFFFu;
        bool coHeldSecondary = false;  // two-handed grab: this hand is the secondary aim hand (no physics drive)
        // True two-point co-hold state lives on the primary holder. Both
        // anchors are captured on the visible mesh in object-local space; the
        // secondary rendered-hand frame is captured in the same space so the
        // hand stays on that exact spot while the object follows both tracked
        // palms.
        bool coHoldAnchorsValid = false;
        RE::NiPoint3 coHoldPrimaryAnchorObjectLocal;
        RE::NiPoint3 coHoldSecondaryAnchorObjectLocal;
        RE::NiTransform coHoldSecondaryHandObjectLocal;
        bool heldPlayerFilterApplied = false; // temporary BIPED_NO_CC held-body layer applied
        int  heldPlayerFilterFrames = 0;      // frame counter for periodic re-application
        std::uint32_t heldOriginalFilterInfo = 0; // exact body filter before the temporary layer (for restore)
        // Exact (hknpWorld*, bodyId) publication consumed by ROCK's native
        // character-controller hook. Unlike heldPlayerFilterApplied, this is
        // level-triggered and records the hand slot that currently owns the
        // registry snapshot so a two-hand promotion can publish the destination
        // before clearing the source.
        bool externalHeldBodiesPublished = false;
        bool externalHeldBodiesPublishedForLeft = false;

        // A Bethesda reference can contain several visible collision wrappers
        // (the Assault Rifle has seven) as well as mutually exclusive visual
        // variants sharing one physics system (10mmAmmo has AmmoSingle and
        // AmmoMultiple). Keep one record per BODY for byte-exact filter/cache
        // restore, but drive only one representative per shared MOTION.
        static constexpr std::size_t kMaxCapturedHeldBodies = 64;
        struct CapturedHeldBody
        {
            std::uint32_t bodyId = 0x7FFF'FFFFu;
            std::uint32_t motionId = 0xFFFF'FFFFu;
            std::uint32_t originalFilterInfo = 0;
            // Acquisition authority and rendered visibility are separate:
            // multipart objects can have several simultaneously visible wrappers,
            // while hidden variants can share a visible wrapper's physics system.
            bool isActiveWrapper = false;
            bool isVisibleWrapper = false;
            // hknp bodies can share one motion. Transform/velocity writes are
            // motion-level operations, so exactly one body in each motion group
            // is selected after the complete body set has been captured.
            bool isMotionRepresentative = false;
            // A bhkPhysicsSystem can contain bodies owned by different
            // bhkNPCollisionObject wrappers/nodes (10mmAmmo is the concrete
            // example: 10mmAmmo + AmmoMultiple). The body backpointer must be
            // validated against its own wrapper, not the wrapper that happened
            // to win grab acquisition.
            RE::NiCollisionObject* ownerCollisionObject = nullptr;
            RE::NiPointer<RE::NiAVObject> ownerNode;

            // Preserve both links in the rendered hierarchy:
            //   root -> collision-owner node -> hknp body.
            // Driving every body through these two frames keeps collision on
            // its corresponding visible mesh even when a multipart NIF has
            // independently positioned child bodies.
            RE::NiTransform ownerRootLocal{};
            RE::NiTransform bodyOwnerLocal{};
            bool valid = false;
            bool filterChanged = false;
        };
        std::array<CapturedHeldBody, kMaxCapturedHeldBodies> capturedHeldBodies{};
        std::uint32_t capturedHeldBodyCount = 0;
        // False means the body records are retained only as a byte-exact filter
        // restoration ledger. Transform/velocity authority then falls back to
        // the selected wrapper without losing obligations for bodies that may
        // become readable again before release.
        bool capturedHeldBodySetValid = false;
        // hknp body IDs are meaningful only inside one exact world/generation.
        // A held object can survive a cell transition while its old body IDs do
        // not, so every captured set carries the world that produced it.
        void* capturedHeldBodiesWorld = nullptr;
        // Records the motion scope that was actually changed to KEYFRAMED.
        // This deliberately survives capture/filter invalidation so release
        // always restores the same recursive-vs-selected scope to DYNAMIC.
        bool heldMotionScopeIsReferenceSubtree = false;
        bool heldScalarFilterApplied = false;
        // Instant DropToHand placement captures body frames before moving the
        // scene graph. Setup must reuse that set (or fail to selected-wrapper
        // scope) rather than recapturing the artificial visual/body gap.
        bool instantPreTeleportBodyCaptureAttempted = false;
        bool capturedHeldBodiesExcludeAlternateWrappers = false;
        bool capturedHeldBodiesIncludeAlternateWrappers = false;

        // A child collision variant can already be visibly separated from the
        // reference root when acquisition begins. The visible child is the
        // authority, but the root rebase must wait until that selected body is
        // KEYFRAMED; otherwise Havok immediately writes the stale dynamic pose
        // back over the root. Initial placement calculations use this staged
        // transform so the object never appears to jump away and pull back.
        bool pendingVisibleRootRebase = false;
        RE::NiTransform pendingVisibleRootWorld{};

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

        // Jul 19 (held-object-survives-a-cell-load fix): bhkWorld the collisionObject above was
        // last synced against. A held object's NiNode keeps tracking correctly across a cell/
        // worldspace transition (script-space, engine-driven), but its Havok collision presence
        // does not self-heal the same way — PostPhysicsGrabUpdate compares this every frame
        // against the refr's CURRENT bhkWorld and re-syncs on mismatch. See GetBhkWorldFromRefr.
        RE::bhkWorld* lastSyncedBhkWorld = nullptr;
        
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

        // Live held-object placement. When automatic hand placement is enabled,
        // these override the saved item offset position/rotation for the active grab.
        bool hasRuntimeHandPlacement = false;
        RE::NiPoint3 runtimeHandPlacementPosition;
        RE::NiMatrix3 runtimeHandPlacementRotation;
        // True when the placement was computed in the SKINNED hand frame
        // (COM-tree LArm_Hand/RArm_Hand) rather than the wand frame. When set,
        // apply sites must resolve the parent transform from that same skinned
        // hand so the object tracks the same node the fingers cast from —
        // otherwise FRIK IK offset between wand and bone reintroduces drift.
        bool runtimePlacementSkinnedHand = false;

        // Once the final grab placement is known, re-express it in the
        // rendered COM-tree hand frame. FRIK can visually lag the raw wand
        // during body turns; keeping the object on the wand while fingers are
        // on the rendered hand makes the item lead the hand. This independent
        // placement layer preserves the authored/runtime target at acquisition
        // but then makes the object-to-visible-hand transform invariant.
        bool hasRigidRenderedHandPlacement = false;
        RE::NiPoint3 rigidRenderedHandPlacementPosition;
        RE::NiMatrix3 rigidRenderedHandPlacementRotation;

        // Live held-object finger curls. Keep these separate from saved item
        // offset curls so automatic grab poses are never persisted to JSON.
        bool hasRuntimeFingerCurls = false;
        float runtimeThumbCurl = 0.6f;
        float runtimeIndexCurl = 0.6f;
        float runtimeMiddleCurl = 0.6f;
        float runtimeRingCurl = 0.6f;
        float runtimePinkyCurl = 0.6f;
        // The mesh solver resolves the actual 15 driven joints. Keeping that
        // result intact avoids re-expanding five scalar curls with a distal
        // bias that can push the rendered fingertips through thin objects.
        bool hasRuntimeJointCurls = false;
        std::array<float, 15> runtimeJointCurls{
            0.6f, 0.6f, 0.6f,
            0.6f, 0.6f, 0.6f,
            0.6f, 0.6f, 0.6f,
            0.6f, 0.6f, 0.6f,
            0.6f, 0.6f, 0.6f
        };
        // ROCK's rich pose includes splay, pad-probe corrections, thumb-lane
        // selection, and local joint transforms that cannot be represented by
        // the legacy 15-curl array. Once published through the 0.77.12 host
        // bridge, scalar re-application must stand down until release.
        bool rockRichFingerPosePublished = false;

        // Sticky grab mode for config - keeps item grabbed even without grip held
        bool stickyGrab = false;            // True if in sticky grab mode (for repositioning)

        // Physics state saved when grabbed
        struct SavedPhysicsState
        {
            RE::hknpMotionPropertiesId::Preset motionType = RE::hknpMotionPropertiesId::Preset::DYNAMIC;
            float linearDamping = 0.0f;
            float angularDamping = 0.0f;
            bool wasDeactivated = false;
            std::uint32_t collisionLayer = 4;      // Real pre-grab layer, captured when (and only when) the grab changes it
            bool collisionLayerChanged = false;    // True only if the grab actually re-layered the body (kNonCollidable=15 paths); release skips SetLayer otherwise
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
        bool needsPostPhysicsUpdate = false;  // Legacy field — kept for state layout compatibility

        // Hand velocity tracking (for shoulder/mouth detection)
        RE::NiPoint3 lastHandPos;
        float handSpeed = 0.0f;  // Computed from position delta
        
        // Behind-ear storage timer (item stored when held behind head for 2+ seconds)
        float behindEarTimer = 0.0f;  // Accumulates time spent in storage zone
        float faceReadTimer = 0.0f;   // Dwell while a note/magazine mesh is in front of the HMD
        float lastStoragePulseTime = 0.0f;  // Per-hand: last time haptic pulse fired in storage zone
        bool isInStorageZone = false; // True if hand is currently in a storage zone
        bool bookStoreBlockedHinted = false; // One-shot: hinted that a magazine can't be stashed behind head (read at face)
        
        // Equip zone tracking (equips armor/weapons on grip release when in body zone)
        bool isInEquipZone = false; // True if held armor/weapon is in equip zone
        const char* currentZoneName = ""; // Zone name for HUD messages (HEAD, CHEST, LEGS)
        // Fingertip weapon-equip must be EDGE-triggered: a drop-to-hand catch
        // materializes the weapon at the Pip-Boy, centimeters from the weapon-hand
        // fingertip, so a level-triggered check latches "equip on release" on the
        // very first frame. The zone stays disarmed until the held weapon has been
        // measured OUTSIDE the equip radius once (with the Pip-Boy closed).
        bool weaponEquipZoneArmed = false;
        
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
        bool naturalFingerPosing = false;  // close natural grab — wrap fingers around mesh + keep reapplying them

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

        void SetRuntimeFingerCurls(float thumb, float index, float middle, float ring, float pinky)
        {
            hasRuntimeFingerCurls = true;
            hasRuntimeJointCurls = false;
            runtimeThumbCurl = thumb;
            runtimeIndexCurl = index;
            runtimeMiddleCurl = middle;
            runtimeRingCurl = ring;
            runtimePinkyCurl = pinky;
        }

        void SetRuntimeJointCurls(const std::array<float, 15>& joints)
        {
            hasRuntimeFingerCurls = true;
            hasRuntimeJointCurls = true;
            runtimeJointCurls = joints;

            // Preserve the public five-finger view and the Pip-Boy's temporary
            // pointing-pose integration. The mesh solver currently emits a
            // uniform safe value for each finger's three joints, but averaging
            // keeps this correct if that representation is refined later.
            auto averageFinger = [&](std::size_t finger) {
                const std::size_t base = finger * 3;
                return (runtimeJointCurls[base] +
                        runtimeJointCurls[base + 1] +
                        runtimeJointCurls[base + 2]) / 3.0f;
            };
            runtimeThumbCurl = averageFinger(0);
            runtimeIndexCurl = averageFinger(1);
            runtimeMiddleCurl = averageFinger(2);
            runtimeRingCurl = averageFinger(3);
            runtimePinkyCurl = averageFinger(4);
        }

        void ClearRuntimeFingerCurls()
        {
            hasRuntimeFingerCurls = false;
            hasRuntimeJointCurls = false;
            runtimeThumbCurl = 0.6f;
            runtimeIndexCurl = 0.6f;
            runtimeMiddleCurl = 0.6f;
            runtimeRingCurl = 0.6f;
            runtimePinkyCurl = 0.6f;
            runtimeJointCurls.fill(0.6f);
        }

        void SetRuntimeHandPlacement(const RE::NiPoint3& position, const RE::NiMatrix3& rotation, bool skinnedHandFrame = false)
        {
            hasRuntimeHandPlacement = true;
            runtimeHandPlacementPosition = position;
            runtimeHandPlacementRotation = rotation;
            runtimePlacementSkinnedHand = skinnedHandFrame;
        }

        void ClearRuntimeHandPlacement()
        {
            hasRuntimeHandPlacement = false;
            runtimeHandPlacementPosition = RE::NiPoint3();
            runtimeHandPlacementRotation = RE::NiMatrix3();
            runtimeHandPlacementRotation.MakeIdentity();
            runtimePlacementSkinnedHand = false;
        }

        void SetRigidRenderedHandPlacement(
            const RE::NiPoint3& position,
            const RE::NiMatrix3& rotation)
        {
            hasRigidRenderedHandPlacement = true;
            rigidRenderedHandPlacementPosition = position;
            rigidRenderedHandPlacementRotation = rotation;
        }

        void ClearRigidRenderedHandPlacement()
        {
            hasRigidRenderedHandPlacement = false;
            rigidRenderedHandPlacementPosition = RE::NiPoint3();
            rigidRenderedHandPlacementRotation = RE::NiMatrix3();
            rigidRenderedHandPlacementRotation.MakeIdentity();
        }

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
            physicalTouchGrab = false;
            physicalTouchBodyId = 0x7FFF'FFFFu;
            coHeldSecondary = false;
            coHoldAnchorsValid = false;
            coHoldPrimaryAnchorObjectLocal = RE::NiPoint3();
            coHoldSecondaryAnchorObjectLocal = RE::NiPoint3();
            coHoldSecondaryHandObjectLocal = RE::NiTransform();
            coHoldSecondaryHandObjectLocal.rotate.MakeIdentity();
            coHoldSecondaryHandObjectLocal.scale = 1.0f;
            heldPlayerFilterApplied = false;
            heldPlayerFilterFrames = 0;
            heldOriginalFilterInfo = 0;
            externalHeldBodiesPublished = false;
            externalHeldBodiesPublishedForLeft = false;
            capturedHeldBodies = {};
            capturedHeldBodyCount = 0;
            capturedHeldBodySetValid = false;
            capturedHeldBodiesWorld = nullptr;
            heldMotionScopeIsReferenceSubtree = false;
            heldScalarFilterApplied = false;
            instantPreTeleportBodyCaptureAttempted = false;
            capturedHeldBodiesExcludeAlternateWrappers = false;
            capturedHeldBodiesIncludeAlternateWrappers = false;
            pendingVisibleRootRebase = false;
            pendingVisibleRootWorld = {};
            lastRoomPos = RE::NiPoint3();
            smoothedRoomDelta = RE::NiPoint3();
            roomTrackingInitialized = false;
            refrHandle.reset();
            node.reset();
            physicsNode.reset();
            collisionObject = nullptr;
            lastSyncedBhkWorld = nullptr;
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
            ClearRuntimeHandPlacement();
            ClearRigidRenderedHandPlacement();
            ClearRuntimeFingerCurls();
            rockRichFingerPosePublished = false;
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
            faceReadTimer = 0.0f;
            lastStoragePulseTime = 0.0f;
            isInStorageZone = false;
            bookStoreBlockedHinted = false;
            isInEquipZone = false;
            weaponEquipZoneArmed = false;
            isInVHZone = false;
            vhHolsterSlot = 0;
            vhHandModeSwitched = false;
            consumeAttemptedThisVisit = false;
            wasInMouthZoneLocal = false;
            // Every other per-grab zone/pose flag in this function is reset except these:
            // isInMouthZone/mouthZoneTimer are only ever re-written by CheckMouthConsume,
            // which the update loop gates to consumables only - a Nuka-Cola grabbed at the
            // mouth then released without a fast-enough consume left isInMouthZone=true
            // leaking through the public IsInMouthZone()/GetCurrentZoneName() plugin API
            // for the rest of the session (and every later non-consumable grab, which
            // never runs CheckMouthConsume to clear it). isInHandInjectionZone/
            // wasInHandInjectionZoneLocal have the same latch problem for the injection-
            // zone entry haptic. usedSnapMode is a per-grab flag that should
            // start fresh too.
            isInMouthZone = false;
            mouthZoneTimer = 0.0f;
            isInHandInjectionZone = false;
            wasInHandInjectionZoneLocal = false;
            usedSnapMode = false;
            currentZoneName = "";
            grabStartTime = 0.0f;
            isFromLootDrop = false;
            isFromSmartGrab = false;
            skipStorageZone = false;
            isNaturalGrab = false;
            isTelekinesis = false;
            naturalFingerPosing = false;
            // Velocity tracking
            prevWandPos = RE::NiPoint3();
            prevObjectPos = RE::NiPoint3();
            wandVelocity = RE::NiPoint3();
            objectVelocity = RE::NiPoint3();
            velocityError = RE::NiPoint3();
            velocityTrackingInit = false;
        }
    };

    bool IsAutomaticHandPlacementEnabled();
    void GetEffectiveGrabPlacement(const GrabState& state, RE::NiPoint3& outLocalOffset, RE::NiMatrix3& outLocalRotation);

    // Per-hand opportunistic finger calibration — call every frame while idle.
    // No-ops once the hand is calibrated. Uses a straightness gate so only
    // open-hand frames capture the zero-angle/normal triplets.
    void TryCalibrateFingerDataIfIdle(bool isLeft);

    /**
     * GrabManager - Manages physics-based grabbing of objects
     *
     * This uses a velocity-based approach where we calculate the desired
     * position of the grabbed object and apply velocities to move it there.
     * This is simpler than constraints but works well for basic grabbing.
     *
     * Thread safety:
     *   - Main thread: StartGrab, UpdateGrab, EndGrab, PostPhysicsGrabUpdate, PreRenderUpdate
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
        void EndGrab(bool isLeft, const RE::NiPoint3* throwVelocity = nullptr, bool forStorage = false,
                     const RE::NiPoint3* throwAngularVelocity = nullptr);  // rad/s world frame (ROCK release composition, audit rank 3)

        /**
         * Check if a hand is currently grabbing
         * @param isLeft Which hand to check
         * @return true if that hand is holding an object
         */
        bool IsGrabbing(bool isLeft) const;

        /**
         * Post-physics grab update - runs from the post-physics hook (0xd8405e).
         * Updates grabbed object visual positions and hand body keyframes.
         * Called AFTER the engine physics step completes.
         */
        void PostPhysicsGrabUpdate();

        /**
         * Pre-render visual update. Currently DORMANT — the pre-render hook
         * (0x1C21156) is disabled due to scene graph crashes.
         * Kept for future restoration.
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

    // Two-handed support-hand finger pose driver (Jul 19): mode 1 is owned by
    // ROCK's rich native-v5/stock-v3 bridge; mode 2 publishes the separate
    // HIGGS whole-weapon solve. Called from HookPostPhysics.
    void UpdateTwoHandedSupportFingerPose();
}
