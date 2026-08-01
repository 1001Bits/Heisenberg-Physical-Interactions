#pragma once

namespace heisenberg
{
    /**
     * Configuration settings for Heisenberg.
     * Loaded from INI file.
     */
    struct Config
    {
        static Config& GetSingleton()
        {
            static Config instance;
            return instance;
        }

        void Load();
        void Save();

        // Public API backing for IHeisenbergInterface001::Get/SetSettingDouble.
        // Names may be an INI key ("fPullSpeed") or a qualified
        // "Section.key". Values use the same units users see in the INI.
        // SetNumericSetting persists to disk and reloads the effective config;
        // both methods are main-game-thread only.
        bool GetNumericSetting(const char* name, double& out) const;
        bool SetNumericSetting(const char* name, double value);

        // Serialize every setting into a CSimpleIniA (passed as void* to keep SimpleIni.h out of
        // this widely-included header). Shared by Save() and the MCM-defaults seeding.
        void BuildIni(void* csimpleIniA) const;
        // Ensure the MCM settings file has an entry for every MCM-exposed control. Generated
        // entries carry provenance metadata so untouched seeds follow future default changes,
        // while values changed in MCM become permanently user-owned.
        void SeedMCMDefaultsIfMissing();

        // Converts the conversion-block fields (naturalGrabDistance, itemStorageZoneRadius,
        // mouthRadius, zone radii/offsets, etc.) from their ORIGINAL cm units to game units
        // in place. Load() calls this once after reading raw cm values from the INI;
        // SeedMCMDefaultsIfMissing() must call it too on a fresh default-constructed Config
        // before BuildIni() (which always multiplies by GAME_UNITS_TO_CM, i.e. expects a
        // post-conversion, game-units Config) - a default-constructed Config still holds the
        // raw cm defaults, so skipping this doubles the conversion (1.4x too large seeded).
        void ApplyCmToGameUnitsConversion();

        // Selection settings - 2 METER RANGE
        float maxGrabDistance = 500.0f;      // cm fallback (converted *CM_TO_GAME_UNITS on load) — 500cm = 5m
        float proximityRadius = 200.0f;      // Game units - radius for proximity selection (~2m)
        float nearCastRadius = 15.0f;        // Game units - radius for close selection (~15cm)
        float nearCastDistance = 200.0f;     // Game units - max distance for close selection (~2m)
        float requiredCastDotProduct = 0.5f; // cos(60 degrees) - how centered must target be
        
        // =====================================================================
        // SELECTION MODE - ViewCaster vs Extended Range
        // =====================================================================
        // ViewCaster = native game crosshair ("Press A/X to store" prompt)
        // Extended Range = custom raycast when ViewCaster has no target
        //
        // Selection order per hand:
        // 1. If ViewCaster has a target, ALWAYS use that (native game selection)
        // 2. If ViewCaster has no target AND extendedGrabRange is enabled, use raycast
        bool extendedGrabRange = false;      // DISABLED - testing ViewCaster only
        
        // Close grab threshold - objects within this distance use natural grab offset
        // Objects beyond this distance snap to palm center
        float closeGrabThreshold = 20.0f;    // Game units - ~20cm threshold for close vs far grab
        
        // Pull-to-hand settings
        bool enablePullToHand = true;        // Pull objects to hand when grabbing
        float pullSpeed = 500.0f;            // Game units/s - speed to pull objects to hand
        float snapDistance = 5.0f;           // Game units - distance at which pulled object snaps (~5cm)
        bool enableTelekinesis = false;      // DISABLED: Object follows hand from current distance (overrides pull-to-hand)
        
        // =====================================================================
        // SEATED MODE - Accessibility for seated VR players
        // =====================================================================
        // When physically seated (low HMD height), extends grab range and pulls objects to hand
        bool enableSeatedMode = false;             // DISABLED - Enable automatic seated detection
        float seatedModeHeightThreshold = 110.0f;  // Game units - HMD height below this = seated (~110cm)
        float seatedModeGrabDistance = 150.0f;     // Game units - grab distance when seated (~1.5m)
        float standingModeGrabDistance = 100.0f;   // Game units - grab distance when standing (~1m)

        // Grab settings
        bool enableGrabbing = true;          // Master toggle for grabbing feature
        bool allowGrabbingOwnedItems = true; // Allow grabbing items that would be stealing
        bool enableGrabActors = false;       // Hidden easter egg: pick up actors (ragdolls, NPCs)
        bool enableStickyGrab = false;       // Press grip once to grab, again to release
        
        // Weapon equip mode when grabbed:
        // 0 = Disabled - weapons held in hand like other items, no auto-equip
        // 1 = Zone-based - drop on weapon hand to equip
        // 2 = Auto equip on grab
        int weaponEquipMode = 1;
        bool enableVHHolstering = true;  // Drop weapon on VH holster zone to holster
        // Throwables (grenades/mines) are WEAP records, so they pass IsWeapon() and would
        // otherwise holster on VH zones along with guns. This gates them separately.
        bool enableThrowableHolstering = false; // Holster throwables on VH holster zones
        bool showHolsterMessages = true; // Show HUD message when holstering/equipping weapons
        bool showUnequipMessages = true; // Show HUD message when unequipping weapons (storage zone)
        
        // Finger pose settings
        // 0 = FRIK mode (controller tracking resumes after release)
        // 1 = Override mode (keep override until next grab)
        int fingerPoseMode = 0;
        float fingerAnimCloseSpeed = 12.0f;   // Speed of finger closing animation (12 = prior hardcoded value; now live)
        float fingerAnimOpenSpeed = 3.0f;     // Speed of finger opening animation
        
        float grabStartSpeed = 200.0f;       // Skyrim units/s
        float grabStartAngularSpeed = 360.0f; // Degrees/s
        float pullSpeedThreshold = 1.2f;      // m/s - speed needed to initiate pull

        // Grab mode - determines how physics grabbing works (two-scenario architecture)
        // 0 = Keyframed (Heisenberg's own backend - direct position control)
        // 9 = Full Dynamic (embedded ROCK engine owns grab+selection natively; needs
        //     bUseRockEngineArchitecture=1, else degrades to Keyframed).
        // Any other value degrades to Keyframed (historical modes 1-8 removed).
        int grabMode = 0;
        // Two-handed single-object grab (HIGGS-style): grab one object with both hands; the
        // second hand acts as an aim/stabilize hand (object swings to point along the line
        // between the two hands). Default ON. When off, a second-hand grab transfers the
        // object hand-to-hand as before.
        bool enableTwoHandedGrab = true;
        // Limb grab: when grabbing an actor (requires bEnableGrabActors), constrain to the
        // specific ragdoll limb (bone body) nearest the hand instead of the actor root.
        // Default OFF. Grabbing NPC ragdoll bodies is CTD-prone — opt-in only.
        bool enableLimbGrab = false;

        // When the KEYFRAMED backend holds an object, keep it on a collidable layer
        // (instead of kNonCollidable) so it can hit/push other objects. Only affects
        // the keyframed hold (iGrabMode=0).
        bool heldObjectCollidable = true;
        bool heldObjectWallClamp = true;  // keyframed grab: stop held object at walls/NPCs
        // Block the A/activate button from looting/using a grabbable item just because it's
        // a grab SELECTION (you're pointing at it). Default OFF — it broke A-button looting
        // and secondary actions (Tune The Radios, Sentinel PA) at 0.7.9. The held-item block
        // already covers the Grip>A binding case, so this extra block is not needed.
        bool blockActivateOnGrabSelection = false;

        // Remap grab from Grip to A/X for users with custom controller bindings
        bool useAForRightGrab = false;  // Right hand uses A button instead of Grip
        bool useXForLeftGrab  = false;  // Left hand uses X button instead of Grip
        
        // Grab distance tiers:
        // - Within naturalGrabDistance: Natural grab (object moves from grabbed point)
        // - Between naturalGrabDistance and maxGrabDistance: Palm snap (object snaps to hand)
        // - Beyond maxGrabDistance: Cannot grab at all
        float naturalGrabDistance = 5.0f;   // Game units - max distance for natural grab (~5cm)
        float naturalGrabDistanceNoMatch = 10.0f;  // Extended natural grab distance for items without exact match (~10cm)
        // Note: maxGrabDistance (defined above) controls max grab distance (~200cm = 2m)
        
        // Palm snap settings - where grabbed objects snap to
        // Objects snap to palm center with this offset from the wand node
        // In wand-local space: X=right, Y=forward (toward fingers), Z=up
        float palmOffsetX = 0.0f;    // Game units - right/left offset (0 = centered)
        float palmOffsetY = 5.0f;    // Game units - forward offset (5 = ~5cm into palm)
        float palmOffsetZ = 3.5f;    // Game units - up/down offset (3.5 = ~5cm up from wand)

        // HIGGS-style palm vector/position (hand.cpp:2274 / include/hand.h:237).
        // palmVectorLocal is the unit normal of the palm surface in hand-local space
        // — the direction that points "out of the palm" toward objects being grabbed.
        // palmPositionLocal is the offset from the hand/wand node origin to the palm
        // center in hand-local space (game units). For the left hand, X is mirrored
        // at use time (matches HIGGS main.cpp:956-958). These replace the prior use
        // of `handNode->world.rotate.entry[*][1]` as the palm direction — the F4VR
        // wand node's axes don't line up with "out of palm" on column 1, which
        // caused the palm-ray cast to miss every grab target.
        // HIGGS Skyrim defaults (include/config.h:323-325) remapped Skyrim→F4VR
        // via (x,y,z)_sk → (x,z,y)_f4vr, since Skyrim wand has Y=up Z=forward
        // and F4VR wand has Y=forward Z=up (see Config.h:157).
        //   Skyrim palmVector   (-0.018, -0.965,  0.261) → F4VR (-0.018,  0.261, -0.965)
        //   Skyrim palmPosition (  0,    -2.4,    6   ) → F4VR (   0,     6,    -2.4  )
        // Palm normal faces -Z (down/out of palm); palm center is +Y (forward
        // toward fingertips) and -Z (below wand origin) from the wand node.
        float palmVectorX = -0.018f;
        float palmVectorY =  0.261f;
        float palmVectorZ = -0.965f;
        float palmPositionX = 0.0f;
        float palmPositionY = 6.0f;
        float palmPositionZ = -2.4f;
        bool enablePalmSnap = true;  // Snap objects to palm center on grab (for mid-range grabs)
        bool enableAutomaticHandPlacement = false;  // HIGGS-style geometry placement; off uses saved item offsets
        bool enableAutomaticFingerCurls = true;    // Geometry-based finger curls; fail-open when no trustworthy mesh stop exists
        bool saveNaturalGrabAsOffset = false;  // Save natural grab positions as item offsets (default off)

        // Grip inference (Task #3). When enabled, prefers a palm-ray cast into
        // the object mesh before falling back to closest-point-on-surface.
        // Ray-cast produces better grips on handles/levers where the palm
        // is not already near the surface.
        bool enablePalmRayCastPlacement = true;

        // Axial placement: put the object's near face (along palmDir) at the
        // palm with small clearance, and center it laterally on the palm axis.
        // OFF by default — F4VR vertex-buffer reads frequently produce far
        // outliers that poison the min/centroid. Use surface-snap until vertex
        // extraction is more reliable; this remains for experimental use.
        bool  enableAxialPlacement = true;

        // Forward clearance in cm applied to the surface-snap placement: lifts
        // the object a small distance off the palm along palmDir so the body
        // extends toward the fingertips with the near face just above palm.
        float axialPlacementClearance = 0.0f;

        // HIGGS-equivalent pre-shift ("shouldMoveHandBack" branch in
        // hand.cpp:1380-1382). Before running closest-point-on-geometry, shift
        // the object's transform (and the triangle set) by palmDir * distance,
        // then surface-snap against the shifted triangles. HIGGS default is
        // 0.15m / havokWorldScale ≈ 10.5 game units. 0 disables the shift.
        float pulledGrabHandAdjustDistance = 10.5f;

        // Squared distance (game units) from the palm's closest-mesh-point to
        // consider a triangle "nearby" for the finger intersection pass.
        // HIGGS config.h default = 100.f (i.e. 10-unit linear radius). Filters
        // out distant triangles that would spuriously trip a finger's curl
        // plane on the far side of the object. See HIGGS hand.cpp:1438-1444.
        float grabMaxTriangleDistance = 100.0f;

        // Weights for GetClosestPointOnGeometryToLine metric: distance =
        // directionalWeight * dirDist² + lateralWeight * latDist². HIGGS
        // defaults (config.h): directional=0.4, lateral=0.6 — biases selection
        // toward triangles close ALONG the palm ray over ones merely close
        // perpendicular to it, stabilizing which mesh point gets chosen when
        // the palm isn't perfectly aimed at the object.
        float grabDirectionalWeight = 0.4f;
        float grabLateralWeight     = 0.6f;

        // Palmar depth in cm: shift the derived palm anchor from the knuckle
        // centroid (dorsal side) toward the palm cup (palmar side). Without
        // this shift the grip point ends up on the BACK of the hand — objects
        // appear to float a few cm above the hand. ~2cm matches a typical
        // adult palm cup depth. Applied only when calibrated palm is used.
        float palmDepthOffset = 2.0f;

        // Per-frame finger curl re-evaluation during hold (Task #4).
        // Currently finger curl is computed once at grab time. Enabling this
        // re-probes the object geometry every N frames so fingers track the
        // object as it rotates in hand. fingerCurlSmoothingAlpha controls lerp
        // between old and new curl values to avoid jitter (0=no lerp/snap,
        // 1=full smoothing/static).
        bool  enableAutomaticFingerCurlsPerFrame = false;
        int   fingerCurlPerFrameInterval = 3;       // frames between re-evals (1=every frame)
        float fingerCurlSmoothingAlpha   = 0.25f;   // lerp factor 0..1 (lower=faster response)

        // Power armor grab offset — extra offset applied to ALL grabbed objects when in PA.
        // PA gloves are bulkier than bare hands, so small objects get hidden inside the model.
        // In hand-local space: X=right, Y=forward (toward fingers), Z=up (away from palm)
        float paGrabOffsetX = 0.0f;
        float paGrabOffsetY = 2.0f;    // Push forward ~2cm (out of glove)
        float paGrabOffsetZ = 1.0f;    // Push up ~1cm (above palm surface)
        
        // =====================================================================
        // ITEM POSITIONING MODE
        // Allows adjusting how items are held in hand
        // =====================================================================
        bool enableItemPositioning = false;   // Hold L3 (1s) while grabbing to enter reposition mode
        int itemPositioningShortcut = 0;     // 0=Left Thumbstick Click, 1=Right Thumbstick Click, 2=Long Press A
        
        // =====================================================================
        // DROP TO HAND
        // Items dropped from inventory spawn in hand instead of on ground
        // =====================================================================
        int dropToHandMode = 1;              // 0=Off, 1=All Items, 2=Holotapes Only
        // Loot-to-hand mode: 0=Off (native), 1=Hybrid (blocked→inventory), 2=Immersive (blocked→floor)
        int lootToHandMode = 1;              // Default: Hybrid (blocked→inventory)
        // Take-All bulk-loot guard: when this many or more auto-loots from one
        // container are pending at once (a "Take All"), keep only ONE for hand
        // delivery and leave the rest in inventory instead of dropping each to
        // the world (which piles non-grabbed items on the floor). 0 disables.
        int lootToHandTakeAllThreshold = 3;   // matches the documented default (BuildIni comment)
        int dropToHandPreferredHand = 2;     // 0=Left, 1=Right, 2=Whichever is free
        bool favoritesToHand = false;        // Consumables from Favorites go to hand
        bool enableStealToHand = true;       // Items stolen from NPCs go to hand
        bool enableHarvestToHand = false;     // Harvested flora goes to hand instead of inventory
        
        // =====================================================================
        // NATIVE THROWABLES (Grenades, Mines)
        // Heisenberg can manage grenade input to prevent accidental throws
        // and provide zone-based activation. Set enableGrenadeHandling=false
        // to let the game handle grenades natively (no mod interference).
        // =====================================================================
        
        // Master toggle for grenade handling — always on; the mod owns throwable input.
        bool enableGrenadeHandling = true;

        // Throwable activation zone - single configurable zone relative to HMD
        // 0 = Disabled (A button works everywhere for grenades)
        // 1 = Enabled (A button only works in zone defined by XYZ offsets below)
        int throwableActivationZone = 0;     // Default: Disabled (A works everywhere)
        float throwableZoneRadius = 20.0f;   // Game units - radius of activation zone (~20cm)

        // Throwables uses A Button: when true, A on the weapon hand readies a throwable
        // (mod injects grip from A and strips physical grip from triggering grenades).
        // When false, throwables are disabled entirely (no native grip-throw either) so
        // SteamVR A↔Grip remaps can't accidentally fire grenades.
        bool remapGrenadeButtonToA = false;  // Default: disabled

        // How long A must be held before a throwable readies (seconds, default 0.3)
        float throwableHoldDuration = 0.3f;
        
        // Throwable zone position (HMD local space: X=right/left, Y=forward/back, Z=up/down)
        float throwableZoneOffsetX = 0.0f;   // + = left, - = right (default: centered)
        float throwableZoneOffsetY = -5.0f;  // + = forward, - = behind (default: slightly behind)
        float throwableZoneOffsetZ = -20.0f; // + = up, - = down (default: chest height)

        // =====================================================================
        // IMPACT EFFECTS (GrabAndThrow port)
        // Thrown objects can damage NPCs, break destructibles, alert enemies,
        // and fire Papyrus OnHit events.
        // =====================================================================
        bool impactDamageEnabled = true;       // Thrown objects damage actors via ProcessHurtfulBody
        float impactDamageMult = 1.0f;         // Multiplier on game-computed impact damage
        bool impactDestroyEnabled = true;      // Thrown objects damage destructibles (bottles, glass, etc.)
        float impactMinDestroySpeed = 250.0f;  // Min impact speed (game units/sec) to deal destructible damage
        bool impactDetectionEvent = true;      // Throws fire AI detection events (NPCs hear them)
        bool impactHitEvent = true;            // Throws fire Papyrus OnHit/SendHitEvent on target
        float impactMinDetectionSpeed = 100.0f; // Min impact speed for AI detection event (lower than destroy)
        float impactActorCooldown = 0.5f;       // Seconds between damage hits on same actor (mirrors PLANCK)
        bool impactMassScaledSound = true;      // Bin AI detection level by thrown mass (HIGGS style)
        bool impactMassGate = true;             // Skip damaging hit destructible if thrown weight < target weight

        // =====================================================================
        // ITEM STORAGE ZONES
        // Configurable body zones where items can be stored/retrieved
        // Uses spheres positioned relative to HMD in local space
        // =====================================================================
        bool enableItemStorageZones = true;  // Enable item storage zone system
        bool enableStorageZoneWeaponEquip = true;  // Grip in storage zone unequips/re-equips weapons (auto-off with VH)
        bool showStorageMessages = true;     // Show HUD message when storing items ("X stored")
        
        // Item storage zone configuration mode - similar to throwable zone config
        // When enabled: hold LEFT thumbstick click for 1 second to enter storage zone config mode
        bool enableStorageZoneConfigMode = false;  // Enable storage zone configuration mode (default OFF)
        
        // Item storage activation zone - where player must place hand to store items
        // Single configurable zone relative to HMD
        float itemStorageZoneRadius = 15.0f; // Game units - radius of storage zone (~15cm)
        
        // Storage zone position (HMD local space: X=right/left, Y=forward/back, Z=up/down)
        float storageZoneOffsetX = 0.0f;    // + = left, - = right
        float storageZoneOffsetY = -15.0f;  // + = forward, - = behind
        float storageZoneOffsetZ = -5.0f;   // + = up, - = down
        
        // Require hand to be behind head for storage (prevents accidental storage when reaching forward)
        // DISABLED by default (v0.5.1 behavior) - zone position is sufficient
        bool requireHandBehindHead = false;  // If true, storage only triggers when hand Y < tolerance
        float behindHeadTolerance = 20.0f;   // Allow hand to be this far forward and still count as "behind" (game units ~cm)
        
        // Auto-storage delay - how long to hold in storage zone before item is stored
        float storageZoneHoldTime = 0.5f;    // Seconds to hold in storage zone before auto-storing
        
        // =====================================================================
        // SMART GRAB
        // Context-aware item retrieval from inventory in storage zone
        // =====================================================================
        bool enableSmartGrab = true;              // Enable smart grab feature
        float smartGrabHealthThreshold = 0.50f;   // Health fraction (0-1) below which stimpaks are prioritized
        float smartGrabRadsThreshold = 0.25f;     // Rads fraction (0-1) above which RadAway is prioritized
        
        // =====================================================================
        // COOKING
        // Cook food items by holding them near heat sources
        // =====================================================================
        bool enableCooking = true;                // Enable cooking feature
        float cookDetectionRadius = 100.0f;       // Game units - proximity to heat source for cooking
        float cookTime = 3.0f;                    // Seconds to hold near heat source to cook
        bool cookingStationOnly = false;          // Only cook at cooking stations (not near heat sources)
        
        // ROCK integration (brunocatani/ROCK as physics/collision/grab provider).
        // When ROCK is present and this is enabled, Heisenberg delegates hand
        // collision + grab to ROCK and consumes its API. When ROCK is absent (or
        // this is off), Heisenberg uses its own built-in systems (fallback).
        // -1 = auto (on iff ROCK.dll loaded), 0 = force off (always fallback),
        // 1 = force on (delegate to ROCK if present). Default auto.
        int  useRockPhysics = -1;

        // When true AND ROCK is active, Heisenberg stops initiating WORLD-OBJECT grabs
        // (grip->select->grab, pull, hand-to-hand) so ROCK owns the grab and holds the
        // object fully dynamically. Heisenberg's programmatic in-hand placement
        // (loot-to-hand, drop-to-hand chems, holotape removal via StartGrabOnRef) is
        // unaffected, as is release/drop of those items. Default off (Heisenberg owns grab).
        bool delegateWorldGrabToRock = false;

        // Hand collision settings (two-scenario cleanup: gates the body-less proximity
        // push only — Heisenberg no longer creates its own physics hand bodies)
        bool enableHandCollision = true;       // Re-enabled Apr 2026 after removing deadlock loop

        // ============================================================================
        // ROCK Integration Toggles (May 2026)
        // Heisenberg-direct ports of ROCK subsystems, each gated by an INI flag.
        // See rock_integration/ for each module's implementation.
        // ============================================================================

        // Internal ROCK integration tuning. Release builds deliberately lock these
        // values: exposing them as player settings changes collision geometry and
        // weapon-solver behavior, so they are diagnostics rather than supported
        // gameplay controls. _DEBUG builds may still override them through the INI/API.
        static constexpr float kHandColliderRadiusPaddingDefault = 0.2f;
        static constexpr float kOffHandSteeringAuthorityDefault = 0.35f;
        float handColliderRadiusPadding = kHandColliderRadiusPaddingDefault;
        float offHandSteeringAuthority = kOffHandSteeringAuthorityDefault;

        // HandBumpGuard — suppress player char-proxy bumps generated by physics hand
        // bodies (the CTD guard ported from ROCK's HandleBumpedCharacter hook). Enable
        // only with physics hand bodies on; the hook is installed regardless and is a
        // no-op when this is off.
        bool rockHandBumpGuard = false;
        float handCollisionRadius = 8.0f;    // Game units - radius of hand collision sphere (broadphase reach)
        float handContactSlop = 1.5f;        // Game units - bone-to-mesh distance that counts as TRUE contact (push trigger). ~finger flesh radius; smaller = must touch closer
        float handPushVelocityThreshold = 10.0f; // Min hand speed to push objects (lower = more sensitive)
        float handPushForceMultiplier = 1.0f;    // Push force multiplier (higher = stronger push)

        // Collision-driven haptics (proximity push → TriggerCollisionHaptics)
        bool  enableHandCollisionHaptics = true;   // Pulse controller on hand contact
        float handCollisionHapticScale   = 500.0f; // Microseconds per unit (mass·speed); 500 = prior hardcoded value, now live

        // FRIK-GOAL seam (FrikArmGoalHook): 1 = swap FRIK's arm-solve goal via the U1PA
        // slot (v5-equivalent, default), 0 = legacy post-FRIK analytic bone IK only.
        int   handAuthorityApplyMode = 1;  // 1 = U1PA double-solve seam (v2: FP arm + Pip-Boy +
                                           //     skinned arm all land on the grip coherently),
                                           //     0 = legacy post-FRIK skinned-arm solver only
        bool  handAuthorityGoalHookLog = false;  // per-apply [FRIK-GOAL] debug lines
        // Strip the thumbstick capacitive-TOUCH bit from controller state. FRIK
        // bends a finger on any stick edge-touch, which was added (Jul 19) to
        // stop that twitch during grabs; presses and axis values always pass
        // through either way. Turned OFF by user request (Jul 30): losing FRIK's
        // natural finger response to the stick is worse than the twitch it
        // avoided. Set true to restore the strip.
        bool  suppressThumbstickTouch = false;
        float pipboyBootScreenDropGameUnits = 0.18f;  // shift the temporary boot screen DOWN by this
                                                 // many game units so the top of the RobCo startup
                                                 // animation is not clipped by the wrist aperture.
                                                 // 0.35 uncovered the top but sat too low overall
                                                 // (user, Jul 27) — halved. Raise to move the screen
                                                 // DOWN, lower it (toward 0) to move the screen UP.
        bool  pipboyBootOpensContent = false;    // after the Vault 111 RobCo boot sequence, auto-open
                                                 // the Pip-Boy on STATS like vanilla flat does.
                                                 // OFF: that forced-open Pip-Boy is opened through a
                                                 // native path FRIK does not own, so grip could not
                                                 // close it (black screen, stuck open). Closed hand-off
                                                 // to FRIK is reliable; only the auto-open is lost.
        bool  postLoadInputRecovery = true;      // after a save load, dump the engine's input-enable
                                                 // layers ([INPUT-DIAG]) and re-assert un-restrained
                                                 // + force-enabled movement at several checkpoints.
                                                 // Fixes saves that carry a stale control-disable
                                                 // layer (symptom: cannot move or jump, CAN turn).
                                                 // Never heals while a control-owning menu is open.
        int   twoHandedFingerPoseMode = 1;       // support-hand fingers during ROCK two-handed
                                                 // grip: 0=off (FRIK grip fist), 1=ROCK's
                                                 // rich mesh-solved pose (native v5 or
                                                 // Heisenberg's stock-0.77.12 renderer),
                                                 // 2=HIGGS geometry solve vs weapon mesh
        // Havok timing fix (ROCK HAVOK_TIMING_FIX): override the engine's physics substep
        // delta/count so physics steps at >= the min rate even on long render frames — keeps
        // held-object / hand physics stable at low FPS.
        bool  havokTimingFix = false;
        float havokTimingMinFrameRate = 70.0f;  // clamped [30,240]
        int   havokTimingMaxSubsteps  = 3;       // clamped [1,6] (engine max)
        // ─── SECOND ARCHITECTURE: embedded full ROCK engine (rock_engine static lib) ───
        // When ON, Heisenberg hosts ROCK's *complete* vendored engine in-process via
        // rock::HostLoad() — ROCK's own PhysicsInteraction/grab/hand/weapon pipeline runs
        // from [PhysicsInteraction] in Heisenberg_F4VR.ini. Missing ROCK keys use compiled
        // defaults, so there is no standalone ROCK.ini or ROCK_Config folder. Default OFF
        // — when off the engine is linked but completely dormant (no hook installed, no
        // listener registered), so behaviour is identical to today.
        // Read EARLY (during F4SEPlugin_Load) so ROCK registers its lifecycle listener
        // before kGameLoaded fires — see Heisenberg::OnF4SELoad.
        bool  useRockEngineArchitecture = false;

        // Throw settings  
        float throwVelocityThreshold = 3.0f;  // m/s - minimum speed for throw (increased to reduce accidental throws)
        float throwVelocityBoostFactor = 1.0f; // Multiplier on throw velocity
        float tangentialVelocityLimit = 5.0f; // m/s - max tangential component

        // Haptics
        float grabBaseHapticStrength = 0.25f;
        float grabProportionalHapticStrength = 0.06f;
        float collisionBaseHapticStrength = 0.1f;

        // =====================================================================
        // MOUTH CONSUME
        // Uses sphere-based detection with HMD-relative offsets
        // =====================================================================
        
        // Consumable activation zone - where player must place item to consume
        // 0 = Disabled, 1 = Mouth/Face
        int consumableActivationZone = 1;     // Default: Mouth
        bool showConsumeMessages = true;       // Show HUD message when consuming items ("X was consumed")
        
        // Mouth consume - sphere position relative to HMD
        float mouthOffsetX = 0.0f;            // Centered
        float mouthOffsetY = 10.0f;           // Forward of HMD (toward face)
        float mouthOffsetZ = -9.0f;           // Below eye level (mouth area)
        float mouthRadius = 11.0f;            // Sphere radius for detection (Game units - ~11cm, matches HIGGS)
        float mouthVelocityThreshold = 2.0f;  // m/s - must be moving slower than this to consume
        
        // Hand injection zone - sphere on opposite wand for syringe-style consumption
        // Offsets relative to the opposite wand node
        // Defaults match Virtual Chems (which uses game units): converted to cm (×1.4)
        bool enableHandInjection = true;          // Enable injection consumption on opposite hand
        float handInjectionOffsetX = 0.0f;         // Right of wand (cm)
        float handInjectionOffsetY = 5.0f;        // Forward from wand (cm)
        float handInjectionOffsetZ = 0.0f;        // Below wand (cm)
        float handInjectionRadius = 21.0f;        // Sphere radius (cm)

        // Power Armor
        bool blockConsumptionInPA = true;  // Block manual consumption/chem use while in Power Armor
        // Grabbing WEAPONS while in Power Armor. Blocked unconditionally since
        // v0.7.0 with the rationale "mechanics interfere with native game
        // behavior, causing infinite loading screens" — a guard written for the
        // original grab path, long before keyframed mode, the embedded ROCK
        // engine, held-body leases and the release filter restore. Default ON
        // (weapons ARE grabbable in PA); set false to restore the old block if
        // native PA weapon pickup ever misbehaves again.
        bool allowWeaponGrabInPowerArmor = true;

        // Haptics for mouth consume
        bool consumableToHand = false;    // Redirect Pipboy consume to drop-to-hand
        bool holotapeToHand = true;      // Redirect Pipboy holotape play to drop-to-hand
        float mouthConstantHapticStrength = 0.3f;
        float mouthDropHapticStrength = 0.5f;

        // Armor equipping zones - drop armor on appropriate body part to equip
        // 0 = Disabled, 1 = Enabled
        int armorEquipZone = 1;               // Master toggle for armor equipping
        
        // Head zone (glasses, hats, helmets) - covers face AND top of head
        float headZoneOffsetX = 0.0f;         // Centered
        float headZoneOffsetY = 5.0f;         // Slightly forward of HMD
        float headZoneOffsetZ = 5.0f;         // Slightly above eye level (better for hats on TOP of head)
        float headZoneRadius = 25.0f;         // Sphere radius (~35cm) - covers glasses at face AND hats on top
        
        // Chest zone (shirts, jackets, chest armor)
        float chestZoneOffsetX = 0.0f;        // Centered
        float chestZoneOffsetY = 15.0f;       // Forward from HMD (hand is in front of body)
        float chestZoneOffsetZ = -30.0f;      // Below eye level (chest area ~30cm down)
        float chestZoneRadius = 15.0f;        // Sphere radius (smaller to prevent accidental equips)
        
        // Leg zone (pants, leg armor, boots)
        float legZoneOffsetX = 0.0f;          // Centered
        float legZoneOffsetY = 15.0f;         // Forward from HMD (hand is in front of body)
        float legZoneOffsetZ = -80.0f;        // Much lower (waist/hip area ~80cm down)
        float legZoneRadius = 30.0f;          // Sphere radius (larger)
        
        float armorEquipVelocityThreshold = 2.0f;  // m/s - must be moving slower than this

        // =====================================================================
        // INTERACTIVE ACTIVATORS
        // Touch buttons, switches, doors with finger proximity
        // =====================================================================
        bool enableInteractiveActivators = true;  // Master toggle for touch-based activation
        float activatorPointingRadius = 25.0f;    // Game units - distance to start pointing pose (~25cm)
        float activatorActivationRadius = 8.0f;   // Game units - distance to trigger activation (~8cm)
        float activatorCooldownMs = 1000.0f;       // Milliseconds between activations
        bool activatorDebugLogging = false;       // Log activator proximity checks
        bool activatorUseWhitelist = false;       // Only track activators listed in optional HeisenbergActivators.ini

        // Timing
        float selectionLeewayTime = 0.25f;    // Seconds to keep selection after losing sight
        float triggerPressedLeewayTime = 0.3f; // Seconds to consider trigger "just pressed"
        float pullApplyVelocityTime = 0.2f;   // Seconds to apply velocity during pull

        // =====================================================================
        // OBJECT HIGHLIGHTING
        // Visual highlight on objects that can be grabbed
        // =====================================================================
        bool enableHighlighting = false;     // Enable object highlighting when pointing at grabbable objects
        
        // Debug
        bool debugDrawControllers = false;
        bool debugLogging = false;  // Enable verbose per-frame logging (impacts performance!)
        // RELEASE (v0.8.4): default is ERROR-ONLY. Info/debug chatter costs frame time in VR
        // and fills testers' logs; fatal/startup errors and warnings-escalated-to-error still
        // print. [Debug] bVerboseLaunch is the opt-in pre-config diagnostic escape hatch.
        // NOTE: this NSDMI IS the effective default - Config::Load() passes the live member as
        // the GetLongValue fallback, and the embedded kDefaultConfig block (Config.cpp) must
        // agree with it or the embedded copy wins.
        int logLevel = 4; // 0=trace, 1=debug, 2=info, 3=warn, 4=error

        // =====================================================================
        // GRIP WEAPON DRAW
        // Always disable grip-based weapon draw — grip is for grabbing objects.
        // This is the same patch as STUF VR. Users don't need a separate mod.
        // =====================================================================
        bool disableGripWeaponDraw = true;  // Grip won't draw/sheathe weapons
        // When grip-weapon-draw is disabled, still let the primary-hand WandGrip reach the
        // game when the wand is aimed at an activator/furniture and you're not grabbing — so
        // mod secondary actions (Tune The Radios, Sentinel PA) that trigger on grip still fire.
        bool passGripToActivatorSecondary = true;
        // Game-level grenade-ready control: hook MeleeThrowHandler (0xFC8AE0) to block/allow
        // the game from readying a held grenade per our rules, instead of stripping grip in the
        // OpenVR input stream (which broke VirtualHolsters / reload / secondary actions).
        bool useGrenadeReadyHook = true;
        bool enableUnarmedAutoUnequip = true;  // Auto-unequip unarmed when grip pressed
        // Was left at false ("Disabled for testing") from an earlier debugging session and
        // never reverted — with this off, EVERY grab falls through to palm-snap/offset mode
        // regardless of how close the hand is to the object (confirmed via live log: distance
        // check correctly resolves isNatural=true at point-blank range, but the mode decision
        // below still forces palm-snap because this flag gates it). Reverted to its original
        // intended default; still user-tunable via the MCM "Telekinesis" switcher.
        bool enableNaturalGrab = true;
        
        // =====================================================================
        // COMPANION/STORAGE SETTINGS
        // =====================================================================
        bool enableDropToCompanion = true;     // Drop item near companion to give it to them
        bool enableDropToContainer = true;     // Release while pointing at a chest/desk deposits into it (MCM "Drop To Container" — was a dead switcher before this existed)
        float companionTransferRadius = 150.0f; // Game units - proximity to detect companion
        float handTransferRadius = 30.0f;       // CM - skip companion/storage when hands this close
        bool enableAutoStorage = true;          // Auto-store after holding in zone for duration
        
        // =====================================================================
        // SMART GRAB FILTERS
        // =====================================================================
        bool smartGrabIncludeHealth = true;        // Include stimpaks, healing items
        bool smartGrabIncludeFood = true;          // Include food and drinks
        bool smartGrabIncludeCombatChems = false;  // Include Psycho, Jet, Buffout, etc.
        bool smartGrabIncludeAntibiotics = true;   // Include antibiotics/disease cures
        bool smartGrabIncludeCarryWeight = false;  // Include carry weight boost items
        bool smartGrabIncludeHeavyJunk = false;    // Include heavy junk for companion drops
        float smartGrabAmmoThreshold = 0.3f;       // Pull ammo when magazine below this (0.3 = 30%)
        
        // =====================================================================
        // PIPBOY / TERMINAL
        // =====================================================================
        bool forceTerminalOnWrist = true;    // Force terminal holotapes to display on wrist Pipboy
        // When a holotape/terminal tape is inserted while the Pip-Boy is in PROJECTED mode,
        // temporarily flip bAlwaysUseProjectedPipboy so it plays on the wrist. This WORKS with
        // FRIK's NORMAL Pip-Boy (default ON). It DEADLOCKS / spawns a duplicate Pip-Boy when
        // FRIK is in HOLO Pip-Boy mode (the flip makes FRIK rebuild the holo Pip-Boy mid-
        // transition). Until we can auto-detect holo mode, FRIK holo-Pip-Boy users set this
        // false. TODO: detect FRIK holo mode and skip the override automatically only there.
        bool holotapeWristOverrideInProjected = true;
        bool hideTerminalExitPrompt = true;  // Hide "(grip) Exit" button hint during wrist terminal
        // Contact envelope for the proportional push-close (tray follows the finger inside
        // this distance of its mesh). 0.6 (bare skin radius) proved unreachable in practice —
        // log-confirmed: the fingertip's tracked point never came within 20gu of the tray mesh
        // during real pushes, so the follow never engaged and fingers clipped through. 1.2
        // still reads as "touching" visually while actually being enterable.
        float tapeDeckPushCloseRadius = 1.2f; // Fingertip-to-tray-mesh distance that counts as touching (push-close envelope)
        bool hideWandHUD = true;             // Hide action prompts ("[A] Take") from wand HUD (item name still shows)
        bool hideAllWandHUD = false;         // Hide all wand HUD messages (actions + item display) when pointing with wands
        bool introHolotapeGiven = false;     // Persistent flag: intro holotape ceremony done
        bool introHolotapePlayed = false;    // Persistent flag: intro audio fully played
        bool enableTerminalOnWorldScreen = false;  // Render terminal UI onto in-world terminal screen meshes (experimental)

        // =====================================================================
        // ACTIVATOR SETTINGS
        // =====================================================================
        bool enableActivatorDiscoveryMode = false;  // Hold right thumbstick near activator to set activation point
        
        // =====================================================================
        // WATER INTERACTION
        // Ripples and splashes when VR hands touch water
        // Defaults aligned with WaterConfig internal tuning values.
        // =====================================================================
        bool enableWaterInteraction = true;         // Master toggle for water effects
        float waterSplashScale = 0.1f;              // Scale of splash effects
        bool enableWakeRipples = true;              // Enable wake ripples when moving through water
        float wakeRippleAmount = 0.008f;            // Wake ripple radius. Ring world radius ≈ value * displacement texture radius (~512 units).
                                                    // 0.008 → ~4 world unit rings. Old 0.03 → ~31 unit rings that merged into linear bands.
        int wakeRippleIntervalMs = 0;               // ms between wake ripples (0 = every frame for visible ripples)
        float wakeMinDistance = 150.0f;             // Game units - min distance between wake ripples. At 150 with ~4-6 unit rings = clean separate circles.
                                                    // Old 20 caused rings to overlap (radius > spacing) creating a linear band along hand movement path.
        float wakeMaxMultiplier = 1.5f;             // Max speed scale applied to wakeRippleAmount (caps ring size at high hand speeds)
        bool enableWaterSplashEffects = true;       // Enable splash particle effects (VFX + SFX)
        float splashEffectEntryMagnitude = 300.0f;  // Splash magnitude when entering water (engine small=5, med=20, large=35)
        float splashEffectExitMagnitude = 150.0f;   // Splash magnitude when exiting water
        bool enableWaterSplashNif = true;           // Enable waterSplash.NIF particle visual
        float waterSplashNifScale = 1.0f;           // Scale multiplier for waterSplash.NIF
        
        // =====================================================================
        // PICKPOCKET / STEALING
        // Touch NPC while sneaking + grip to steal items from their inventory.
        // Calls the native AiFormulas::ComputePickpocketSuccess which uses:
        //   - All fPickPocket* game settings (ActorSkillMult, TargetSkillMult, etc.)
        //   - Perk entry point 0x38 (kModPickpocketChance)
        //   - Combat multiplier
        //   - Item value and weight
        // And Actor::PickpocketAlarm for the full crime system on failure.
        // No custom modifiers — matches the base game exactly.
        // =====================================================================
        bool enablePickpocket = true;              // Master toggle for pickpocketing

        // MCM support - reload settings if MCM changed them
        void ReloadIfMCMChanged();

    private:
        Config() = default;
    };

    inline Config& g_config = Config::GetSingleton();
}
