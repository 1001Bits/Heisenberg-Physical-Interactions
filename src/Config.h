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

        // Selection settings - 2 METER RANGE
        float maxGrabDistance = 200.0f;      // Game units - max distance for any selection (~2m)
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
        bool showHolsterMessages = true; // Show HUD message when holstering/equipping weapons
        bool showUnequipMessages = true; // Show HUD message when unequipping weapons (storage zone)
        
        // Finger pose settings
        // 0 = FRIK mode (controller tracking resumes after release)
        // 1 = Override mode (keep override until next grab)
        int fingerPoseMode = 0;
        float fingerAnimCloseSpeed = 5.0f;    // Speed of finger closing animation
        float fingerAnimOpenSpeed = 3.0f;     // Speed of finger opening animation
        
        float grabStartSpeed = 200.0f;       // Skyrim units/s
        float grabStartAngularSpeed = 360.0f; // Degrees/s
        float pullSpeedThreshold = 1.2f;      // m/s - speed needed to initiate pull

        // Grab mode - determines how physics grabbing works
        // 0 = Keyframe (direct position control via ApplyHardKeyframe - can crash)
        // 3 = VirtualSpring (SetTransform + zero velocity - RECOMMENDED)
        // 4 = MouseSpring (native game spring system - doesn't stay grabbed)
        int grabMode = 3;  // VirtualSpring - direct positioning with zeroed velocity
        
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
        bool enablePalmSnap = true;  // Snap objects to palm center on grab (for mid-range grabs)
        bool saveNaturalGrabAsOffset = false;  // Save natural grab positions as item offsets (default off)

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
        int dropToHandPreferredHand = 2;     // 0=Left, 1=Right, 2=Whichever is free
        bool favoritesToHand = false;       // Consumables from Favorites go to hand
        bool enableStealToHand = true;       // Items stolen from NPCs go to hand
        bool enableHarvestToHand = false;    // Harvested flora goes to hand instead of inventory
        
        // =====================================================================
        // NATIVE THROWABLES (Grenades, Mines)
        // Heisenberg can manage grenade input to prevent accidental throws
        // and provide zone-based activation. Set enableGrenadeHandling=false
        // to let the game handle grenades natively (no mod interference).
        // =====================================================================
        
        // Master toggle for grenade handling
        // When false: mod doesn't touch grenade input - game handles grenades natively
        // When true: mod manages grenade input (remap to A, zone restrictions, etc.)
        bool enableGrenadeHandling = true;   // Default: enabled
        
        // Throwable activation zone - single configurable zone relative to HMD
        // 0 = Disabled (A button works everywhere for grenades)
        // 1 = Enabled (A button only works in zone defined by XYZ offsets below)
        int throwableActivationZone = 0;     // Default: Disabled (A works everywhere)
        float throwableZoneRadius = 20.0f;   // Game units - radius of activation zone (~20cm)
        
        // Remap grenade button from Grip to A button
        // This allows Virtual Holsters (which uses Grip) to coexist with native grenades
        // When enabled: pressing A triggers grenade throw (Grip still works for VH)
        // If throwableActivationZone=0: A works everywhere
        // If throwableActivationZone=1: A only works in the configured zone
        bool remapGrenadeButtonToA = false;  // Default: disabled (grip works natively for grenades)
        
        // Throwable zone position (HMD local space: X=right/left, Y=forward/back, Z=up/down)
        float throwableZoneOffsetX = 0.0f;   // + = left, - = right (default: centered)
        float throwableZoneOffsetY = -5.0f;  // + = forward, - = behind (default: slightly behind)
        float throwableZoneOffsetZ = -20.0f; // + = up, - = down (default: chest height)
        
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
        
        // Hand collision settings
        bool enableHandCollision = false;    // DISABLED - causes collision with objects when walking near them
        bool usePhysicsHandBodies = false;   // DISABLED - hknpWorld::createBody crashes
        float handCollisionRadius = 8.0f;    // Game units - radius of hand collision sphere
        float handPushVelocityThreshold = 10.0f; // Min hand speed to push objects (lower = more sensitive)
        float handPushForceMultiplier = 1.0f;    // Push force multiplier (higher = stronger push)  
        
        // =====================================================================
        // GRAB CONSTRAINT MOTOR PARAMETERS
        // These control the "springiness" of physics grabbing
        // =====================================================================
        
        // Angular motor (rotation control)
        float grabConstraintAngularTau = 0.8f;                    // Softness (0=soft, 1=hard)
        float grabConstraintAngularDamping = 1.0f;                // Damping factor
        float grabConstraintAngularProportionalRecoveryVelocity = 2.0f;
        float grabConstraintAngularConstantRecoveryVelocity = 1.0f;
        float grabConstraintAngularMaxForce = 50.0f;              // Max angular force
        
        // Linear motor (position control)
        float grabConstraintLinearTau = 0.8f;                     // Softness
        float grabConstraintLinearDamping = 1.0f;                 // Damping factor
        float grabConstraintLinearProportionalRecoveryVelocity = 2.0f;
        float grabConstraintLinearConstantRecoveryVelocity = 1.0f;
        float grabConstraintLinearMaxForce = 500.0f;              // Max linear force
        
        // Force ratio between angular and linear motors
        float grabConstraintAngularToLinearForceRatio = 10.0f;  

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

        // Haptics for mouth consume
        bool consumableToHand = false;   // Redirect Pipboy consume to drop-to-hand
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
        int logLevel = 4; // 0=trace, 1=debug, 2=info, 3=warn, 4=error

        // =====================================================================
        // GRIP WEAPON DRAW
        // Always disable grip-based weapon draw — grip is for grabbing objects.
        // This is the same patch as STUF VR. Users don't need a separate mod.
        // =====================================================================
        bool disableGripWeaponDraw = true;  // Grip won't draw/sheathe weapons
        bool enableUnarmedAutoUnequip = true;  // Auto-unequip unarmed when grip pressed
        bool enableNaturalGrab = true;  // Natural grab for close objects (vs always palm snap)
        
        // =====================================================================
        // COMPANION/STORAGE SETTINGS
        // =====================================================================
        bool enableDropToCompanion = true;     // Drop item near companion to give it to them
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
        bool hideTerminalExitPrompt = true;  // Hide "(grip) Exit" button hint during wrist terminal
        float tapeDeckPushCloseRadius = 3.0f; // Distance at which hand starts pushing holotape deck closed
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
