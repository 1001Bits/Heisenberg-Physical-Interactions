#pragma once

#include <cstdint>

namespace rock::collision_layer_policy
{
    /*
     * Weapons of Fate turns firearm shots into real projectile bodies. ROCK's
     * weapon collision layer still needs to collide with clutter and world
     * geometry, but it must not broadly collide with projectile layers by
     * default or the player's bullets can hit the weapon's own generated hulls
     * while aiming down sights. This is kept as a layer-mask policy instead of
     * a projectile hook because owner-aware projectile filtering needs separate
     * verified collector layout work.
     */
    inline constexpr std::uint32_t FO4_LAYER_UNIDENTIFIED = 0;
    inline constexpr std::uint32_t FO4_LAYER_FILTER_MASK = 0x7F;

    [[nodiscard]] inline constexpr std::uint32_t withCollisionLayer(
        const std::uint32_t filterInfo,
        const std::uint32_t layer) noexcept
    {
        return (filterInfo & ~FO4_LAYER_FILTER_MASK) |
               (layer & FO4_LAYER_FILTER_MASK);
    }

    inline constexpr std::uint32_t FO4_LAYER_STATIC = 1;
    inline constexpr std::uint32_t FO4_LAYER_ANIMSTATIC = 2;
    inline constexpr std::uint32_t FO4_LAYER_TRANSPARENT = 3;
    inline constexpr std::uint32_t FO4_LAYER_CLUTTER = 4;
    inline constexpr std::uint32_t FO4_LAYER_WEAPON = 5;
    inline constexpr std::uint32_t FO4_LAYER_PROJECTILE = 6;
    inline constexpr std::uint32_t FO4_LAYER_SPELL = 7;
    inline constexpr std::uint32_t FO4_LAYER_BIPED = 8;
    inline constexpr std::uint32_t FO4_LAYER_TREES = 9;
    inline constexpr std::uint32_t FO4_LAYER_PROPS = 10;
    inline constexpr std::uint32_t FO4_LAYER_TERRAIN = 13;
    inline constexpr std::uint32_t FO4_LAYER_NONCOLLIDABLE = 15;
    inline constexpr std::uint32_t FO4_LAYER_GROUND = 17;
    inline constexpr std::uint32_t FO4_LAYER_DEBRIS_SMALL = 19;
    inline constexpr std::uint32_t FO4_LAYER_DEBRIS_LARGE = 20;
    inline constexpr std::uint32_t FO4_LAYER_SHELLCASING = 25;
    inline constexpr std::uint32_t FO4_LAYER_TRANSPARENT_SMALL = 26;
    inline constexpr std::uint32_t FO4_LAYER_INVISIBLE_WALL = 27;
    inline constexpr std::uint32_t FO4_LAYER_TRANSPARENT_SMALL_ANIM = 28;
    inline constexpr std::uint32_t FO4_LAYER_CLUTTER_LARGE = 29;
    inline constexpr std::uint32_t FO4_LAYER_CHARCONTROLLER = 30;
    inline constexpr std::uint32_t FO4_LAYER_STAIRHELPER = 31;
    inline constexpr std::uint32_t FO4_LAYER_DEADBIP = 32;
    inline constexpr std::uint32_t FO4_LAYER_BIPED_NO_CC = 33;
    inline constexpr std::uint32_t FO4_LAYER_AVOIDBOX = 34;
    inline constexpr std::uint32_t FO4_LAYER_COLLISIONBOX = 35;
    inline constexpr std::uint32_t FO4_LAYER_CAMERASPHERE = 36;
    inline constexpr std::uint32_t FO4_LAYER_CONEPROJECTILE = 38;
    inline constexpr std::uint32_t FO4_LAYER_ITEMPICK = 40;
    inline constexpr std::uint32_t FO4_LAYER_LINEOFSIGHT = 41;
    inline constexpr std::uint32_t FO4_LAYER_PATHPICK = 42;
    inline constexpr std::uint32_t ROCK_LAYER_HAND = 43;
    inline constexpr std::uint32_t ROCK_LAYER_WEAPON = 44;
    inline constexpr std::uint32_t FO4_LAYER_SPELLEXPLOSION = 45;
    inline constexpr std::uint32_t FO4_LAYER_DROPPINGPICK = 46;
    /*
     * FO4VR's vanilla BGSCollisionLayer/name table is configured only through
     * layer 46. The hknp filter matrix is still 64 rows wide, so ROCK owns 47 as
     * explicit extended matrix capacity for generated body colliders. Do not
     * treat this as a vanilla configured layer or depend on BGSCollisionLayer
     * records to initialize the Havok row/column.
     */
    inline constexpr std::uint32_t ROCK_LAYER_BODY = 47;
    /*
     * Reload mobile ammo bodies need to collide with equipped weapon socket
     * bodies. Layer 45 is native SPELLEXPLOSION, so the reviewed split is:
     * mobile reload ammo uses the ROCK hand/tool layer 43, weapon-owned sockets
     * and action bodies use the ROCK weapon/tool layer 44, and role/owner
     * metadata distinguishes reload contacts from real hands and weapon hulls.
     */
    inline constexpr std::uint32_t ROCK_LAYER_RELOAD = ROCK_LAYER_HAND;
    /*
     * Dynamic hand proxy bodies live on their own extended row so they collide
     * only with static world-surface layers and generated equipped-weapon
     * hulls: no clutter, actors, projectiles, other hands/body colliders, and
     * never each other. The proxy is a solver-side visual-stop driver, not
     * gameplay contact evidence.
     */
    inline constexpr std::uint32_t ROCK_LAYER_DYNAMIC_HAND_PROXY = 48;

    /*
     * Weapon CCD must see the same production hand hull that supplies the
     * working hand->weapon manifold.  Layer 48 is an optional solver twin; it
     * can be absent or one physics sample behind during hand transitions.
     * Layer 43 is therefore an authoritative fallback, with body-ID ownership
     * and directional arbitration performed by SoftContactRuntime.
     */
    [[nodiscard]] inline constexpr bool
    isWeaponSweepHandTargetLayer(const std::uint32_t layer) noexcept
    {
        return layer == ROCK_LAYER_HAND ||
               layer == ROCK_LAYER_DYNAMIC_HAND_PROXY;
    }

    inline constexpr std::uint32_t FO4_LAYER_VANILLA_CONFIGURED_COUNT = 47;
    inline constexpr std::uint32_t FO4_LAYER_LAST_VANILLA_CONFIGURED = FO4_LAYER_DROPPINGPICK;
    inline constexpr std::uint32_t FO4_LAYER_MATRIX_ADDRESSABLE_COUNT = 64;
    inline constexpr std::uint32_t ROCK_LAYER_EXTENDED_FIRST = ROCK_LAYER_BODY;
    inline constexpr std::uint32_t ROCK_LAYER_EXTENDED_LAST = FO4_LAYER_MATRIX_ADDRESSABLE_COUNT - 1;

    inline constexpr std::uint32_t FO4_LAYER_CONFIGURED_COUNT = FO4_LAYER_VANILLA_CONFIGURED_COUNT;
    inline constexpr std::uint32_t FO4_LAYER_LAST_CONFIGURED = FO4_LAYER_LAST_VANILLA_CONFIGURED;
    inline constexpr std::uint32_t FO4_LAYER_MAX_CONFIGURED = FO4_LAYER_LAST_CONFIGURED;

    inline constexpr bool isRockOwnedReusableLayer(std::uint32_t layer)
    {
        return layer == ROCK_LAYER_HAND || layer == ROCK_LAYER_WEAPON || layer == ROCK_LAYER_BODY;
    }

    inline constexpr bool isRockGeneratedCharacterControllerSuppressionLayer(std::uint32_t layer)
    {
        return isRockOwnedReusableLayer(layer) || layer == ROCK_LAYER_DYNAMIC_HAND_PROXY;
    }

    inline constexpr bool isDynamicPropInteractionLayer(std::uint32_t layer)
    {
        switch (layer) {
        case FO4_LAYER_CLUTTER:
        case FO4_LAYER_WEAPON:
        case FO4_LAYER_DEBRIS_SMALL:
        case FO4_LAYER_DEBRIS_LARGE:
        case FO4_LAYER_SHELLCASING:
        case FO4_LAYER_CLUTTER_LARGE:
            return true;
        default:
            return false;
        }
    }

    inline constexpr bool isActorOrBipedLayer(std::uint32_t layer)
    {
        return layer == FO4_LAYER_BIPED || layer == FO4_LAYER_DEADBIP || layer == FO4_LAYER_BIPED_NO_CC;
    }

    /*
     * These native layers can directly deliver damage to the player. They must
     * never be dropped by ROCK's character-controller contact filter: the
     * generated ROCK body is intentionally not a replacement damage hitbox.
     */
    inline constexpr bool isNativeDamageDeliveryLayer(std::uint32_t layer)
    {
        return layer == FO4_LAYER_PROJECTILE ||
               layer == FO4_LAYER_CONEPROJECTILE ||
               layer == FO4_LAYER_SPELL ||
               layer == FO4_LAYER_SPELLEXPLOSION;
    }

    inline constexpr bool isWorldSurfaceLayer(std::uint32_t layer)
    {
        switch (layer) {
        case FO4_LAYER_STATIC:
        case FO4_LAYER_ANIMSTATIC:
        case FO4_LAYER_TRANSPARENT:
        case FO4_LAYER_TREES:
        case FO4_LAYER_TERRAIN:
        case FO4_LAYER_GROUND:
        case FO4_LAYER_TRANSPARENT_SMALL:
        case FO4_LAYER_INVISIBLE_WALL:
        case FO4_LAYER_TRANSPARENT_SMALL_ANIM:
        case FO4_LAYER_STAIRHELPER:
        case FO4_LAYER_AVOIDBOX:
        case FO4_LAYER_COLLISIONBOX:
            return true;
        default:
            return false;
        }
    }

    /*
     * Fallout VR's native player character controller remains authoritative for
     * every readable vanilla body. The narrow filter exists only to remove ROCK's
     * generated hand/weapon/body/proxy colliders and unreadable transient body
     * references before the native solver sees them.
     */
    struct PlayerCharacterControllerContactPolicyInput
    {
        bool filterEnabled = false;
        bool playerController = false;
        bool targetLayerKnown = false;
        std::uint32_t targetLayer = FO4_LAYER_UNIDENTIFIED;
    };

    struct PlayerCharacterControllerContactPolicyDecision
    {
        bool suppress = false;
        const char* reason = "native";
    };

    inline constexpr bool isPlayerCharacterControllerSupportLayer(std::uint32_t layer)
    {
        return isWorldSurfaceLayer(layer);
    }

    /*
     * Native BIPED bodies remain Fallout's projectile/spell/melee damage
     * authority. A global per-body no-collide bit would suppress every one of
     * those systems at once, so ROCK never owns such a rewrite. Object-contact
     * noise is handled only by the narrow CHARCONTROLLER policy below.
     */
    inline constexpr bool kAllowGlobalNativePlayerBodySuppression = false;

    inline constexpr PlayerCharacterControllerContactPolicyDecision evaluatePlayerCharacterControllerContact(
        const PlayerCharacterControllerContactPolicyInput& input)
    {
        if (!input.filterEnabled) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = false, .reason = "filterDisabled" };
        }
        if (!input.playerController) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = false, .reason = "nonPlayerController" };
        }
        if (!input.targetLayerKnown) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = true, .reason = "unknownTargetLayerSuppressed" };
        }
        if (isRockGeneratedCharacterControllerSuppressionLayer(input.targetLayer)) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = true, .reason = "rockGeneratedBody" };
        }
        if (input.targetLayer == FO4_LAYER_BIPED_NO_CC) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = true, .reason = "heldNoCharacterController" };
        }
        if (isNativeDamageDeliveryLayer(input.targetLayer)) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = false, .reason = "nativeDamageAuthority" };
        }
        if (isPlayerCharacterControllerSupportLayer(input.targetLayer)) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = false, .reason = "supportLayer" };
        }
        if (input.targetLayer < FO4_LAYER_VANILLA_CONFIGURED_COUNT) {
            return PlayerCharacterControllerContactPolicyDecision{ .suppress = false, .reason = "nativeBody" };
        }
        return PlayerCharacterControllerContactPolicyDecision{ .suppress = true, .reason = "nonNativeLayer" };
    }

    inline constexpr bool isQueryOnlyLayer(std::uint32_t layer)
    {
        return layer == FO4_LAYER_ITEMPICK || layer == FO4_LAYER_LINEOFSIGHT || layer == FO4_LAYER_PATHPICK || layer == FO4_LAYER_CAMERASPHERE;
    }

    inline constexpr bool isPassivePushInteractionLayer(std::uint32_t layer)
    {
        return isDynamicPropInteractionLayer(layer) || isActorOrBipedLayer(layer);
    }

    inline constexpr bool isVanillaConfiguredLayer(std::uint32_t layer)
    {
        return layer < FO4_LAYER_VANILLA_CONFIGURED_COUNT;
    }

    inline constexpr bool isMatrixAddressableLayer(std::uint32_t layer)
    {
        return layer < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT;
    }

    inline constexpr bool isRockExtendedLayer(std::uint32_t layer)
    {
        return layer >= ROCK_LAYER_EXTENDED_FIRST && layer <= ROCK_LAYER_EXTENDED_LAST;
    }

    inline constexpr bool isConfiguredLayer(std::uint32_t layer)
    {
        return isVanillaConfiguredLayer(layer);
    }

    inline constexpr std::uint64_t layerBitOrZero(std::uint32_t layer)
    {
        return isMatrixAddressableLayer(layer) ? (1ULL << layer) : 0ULL;
    }

    inline void setPair(std::uint64_t* matrix, std::uint32_t layerA, std::uint32_t layerB, bool enabled)
    {
        if (!matrix || !isMatrixAddressableLayer(layerA) || !isMatrixAddressableLayer(layerB)) {
            return;
        }

        const std::uint64_t bitA = layerBitOrZero(layerA);
        const std::uint64_t bitB = layerBitOrZero(layerB);
        if (bitA == 0 || bitB == 0) {
            return;
        }

        if (enabled) {
            matrix[layerA] |= bitB;
            matrix[layerB] |= bitA;
        } else {
            matrix[layerA] &= ~bitB;
            matrix[layerB] &= ~bitA;
        }
    }

    inline void applyWeaponProjectileBlockingPolicy(std::uint64_t* matrix, std::uint32_t weaponLayer, bool blocksProjectiles, bool blocksSpells)
    {
        setPair(matrix, weaponLayer, FO4_LAYER_PROJECTILE, blocksProjectiles);
        setPair(matrix, weaponLayer, FO4_LAYER_CONEPROJECTILE, blocksProjectiles);
        setPair(matrix, weaponLayer, FO4_LAYER_SPELL, blocksSpells);
    }

    inline constexpr std::uint64_t allVanillaConfiguredLayerBits()
    {
        return (FO4_LAYER_VANILLA_CONFIGURED_COUNT >= 64) ? ~0ULL : ((1ULL << FO4_LAYER_VANILLA_CONFIGURED_COUNT) - 1ULL);
    }

    inline constexpr std::uint64_t allConfiguredLayerBits() { return allVanillaConfiguredLayerBits(); }

    inline constexpr std::uint64_t allMatrixAddressableLayerBits()
    {
        return ~0ULL;
    }

    inline constexpr std::uint64_t configuredMask(std::uint64_t mask) { return mask & allConfiguredLayerBits(); }

    inline constexpr std::uint64_t matrixAddressableMask(std::uint64_t mask) { return mask & allMatrixAddressableLayerBits(); }

    inline constexpr bool configuredLayerMaskMatches(std::uint64_t currentMask, std::uint64_t expectedMask)
    {
        return configuredMask(currentMask) == configuredMask(expectedMask);
    }

    inline constexpr bool matrixLayerMaskMatches(std::uint64_t currentMask, std::uint64_t expectedMask)
    {
        return matrixAddressableMask(currentMask) == matrixAddressableMask(expectedMask);
    }

    inline constexpr std::uint64_t rockBodyManagedLayerBits()
    {
        return allMatrixAddressableLayerBits() &
               ~layerBitOrZero(FO4_LAYER_BIPED) &
               ~layerBitOrZero(FO4_LAYER_DEADBIP) &
               ~layerBitOrZero(FO4_LAYER_BIPED_NO_CC);
    }

    inline constexpr bool bodyManagedLayerMaskMatches(std::uint64_t currentMask, std::uint64_t expectedMask)
    {
        const auto managedBits = rockBodyManagedLayerBits();
        return (currentMask & managedBits) == (expectedMask & managedBits);
    }

    inline constexpr std::uint64_t withoutLayer(std::uint64_t mask, std::uint32_t layer) { return mask & ~layerBitOrZero(layer); }

    inline constexpr std::uint64_t withLayer(std::uint64_t mask, std::uint32_t layer) { return mask | layerBitOrZero(layer); }

    inline constexpr bool maskEnablesLayer(std::uint64_t mask, std::uint32_t layer)
    {
        const auto bit = layerBitOrZero(layer);
        return bit != 0 && (mask & bit) != 0;
    }

    /*
     * LARGE-OBJECT SPLIT (car fix, #219/#220).
     *
     * MANAGED != SUPPRESSED. The managed mask is the set of CHARCONTROLLER matrix
     * columns this policy owns, so it stays constant for the drift watchdog.
     *
     * A layer row cannot express object ownership or safely distinguish a generated
     * collider from native clutter. Clearing a global CLUTTER/WEAPON pair also prevents
     * Havok from generating the small-prop/car contact the native solver needs, so those
     * pairs are restored to their captured native value. BIPED_NO_CC is different: a
     * body explicitly opts into that native no-controller layer while held, so its one
     * controller pair is disabled. The installed ProcessConstraints pre-filter remains
     * the exact-body path for generated ROCK bodies.
     */
    inline constexpr std::uint64_t nativeCharacterControllerManagedLayerMask()
    {
        return layerBitOrZero(FO4_LAYER_CLUTTER) |
               layerBitOrZero(FO4_LAYER_WEAPON) |
               layerBitOrZero(FO4_LAYER_DEBRIS_SMALL) |
               layerBitOrZero(FO4_LAYER_DEBRIS_LARGE) |
               layerBitOrZero(FO4_LAYER_SHELLCASING) |
               layerBitOrZero(FO4_LAYER_CLUTTER_LARGE) |
               /*
                * BIPED_NO_CC is the native per-body escape hatch used while an
                * object is held.  Unlike CLUTTER/WEAPON, suppressing this pair
                * cannot make an unleased world prop or car pass through the
                * player: only bodies explicitly moved to layer 33 opt in.
                *
                * This matrix pair is also required for the constraint-only
                * character-controller callback layout.  Those rows carry no
                * target body ID, so the exact held-body pre-filter cannot
                * identify and compact them safely.
                */
               layerBitOrZero(FO4_LAYER_BIPED_NO_CC);
    }

    inline constexpr std::uint64_t nativeCharacterControllerSuppressedLayerMask(bool blockLargeObjects)
    {
        (void)blockLargeObjects;
        return layerBitOrZero(FO4_LAYER_BIPED_NO_CC);
    }

    inline constexpr bool isNativeCharacterControllerManagedLayer(std::uint32_t layer)
    {
        return maskEnablesLayer(nativeCharacterControllerManagedLayerMask(), layer);
    }

    inline constexpr bool isNativeCharacterControllerObjectSuppressionLayer(std::uint32_t layer, bool blockLargeObjects = false)
    {
        return maskEnablesLayer(nativeCharacterControllerSuppressedLayerMask(blockLargeObjects), layer);
    }

    inline constexpr std::uint64_t nativeCharacterControllerExpectedMask(
        std::uint64_t originalMask,
        bool suppressDynamicObjects,
        bool blockLargeObjects = false)
    {
        (void)suppressDynamicObjects;
        /*
         * Layer 33 means "BIPED_NO_CC"; honour that contract independently of
         * the optional generated-object contact filter setting.  Damage pairs
         * (projectile/spell/melee versus BIPED_NO_CC) are untouched.
         */
        return originalMask &
               ~nativeCharacterControllerSuppressedLayerMask(
                   blockLargeObjects);
    }

    inline constexpr std::uint64_t buildRockHandExpectedMask(bool includeWeaponLayer, bool includeStaticWorld = true)
    {
        /*
         * ROCK's generated hand and weapon tool bodies use real keyframed Havok
         * collision as contact evidence while higher-level ownership gates
         * decide visual hand authority. Hand bodies collide broadly enough to
         * produce body/world contacts, but hand-hand self collision and query-only
         * layers stay excluded so the generated palm and finger set does not
         * become its own solver noise source.
         */
        std::uint64_t mask = allConfiguredLayerBits();
        mask = withoutLayer(mask, FO4_LAYER_UNIDENTIFIED);
        if (!includeStaticWorld) {
            mask = withoutLayer(mask, FO4_LAYER_STATIC);
            mask = withoutLayer(mask, FO4_LAYER_ANIMSTATIC);
        }
        mask = withoutLayer(mask, FO4_LAYER_PROJECTILE);
        mask = withoutLayer(mask, FO4_LAYER_SPELL);
        mask = withoutLayer(mask, FO4_LAYER_CONEPROJECTILE);
        mask = withoutLayer(mask, FO4_LAYER_NONCOLLIDABLE);
        mask = withoutLayer(mask, ROCK_LAYER_HAND);
        mask = withoutLayer(mask, ROCK_LAYER_BODY);
        mask = withoutLayer(mask, FO4_LAYER_CAMERASPHERE);
        mask = withoutLayer(mask, FO4_LAYER_ITEMPICK);
        mask = withoutLayer(mask, FO4_LAYER_LINEOFSIGHT);
        mask = withoutLayer(mask, FO4_LAYER_PATHPICK);
        mask = withoutLayer(mask, FO4_LAYER_CHARCONTROLLER);
        if (includeWeaponLayer) {
            mask = withLayer(mask, ROCK_LAYER_WEAPON);
        }
        return mask;
    }

    inline constexpr std::uint64_t buildRockWeaponExpectedMask(bool blocksProjectiles, bool blocksSpells, bool includeStaticWorld = true, bool includeBodyLayer = false)
    {
        std::uint64_t mask = allConfiguredLayerBits();
        mask = withoutLayer(mask, FO4_LAYER_UNIDENTIFIED);
        if (!includeStaticWorld) {
            mask = withoutLayer(mask, FO4_LAYER_STATIC);
            mask = withoutLayer(mask, FO4_LAYER_ANIMSTATIC);
        }
        mask = withoutLayer(mask, FO4_LAYER_NONCOLLIDABLE);
        mask = withoutLayer(mask, FO4_LAYER_CHARCONTROLLER);
        mask = withoutLayer(mask, ROCK_LAYER_WEAPON);
        mask = withoutLayer(mask, FO4_LAYER_CAMERASPHERE);
        mask = withoutLayer(mask, FO4_LAYER_ITEMPICK);
        mask = withoutLayer(mask, FO4_LAYER_LINEOFSIGHT);
        mask = withoutLayer(mask, FO4_LAYER_PATHPICK);
        if (!blocksProjectiles) {
            mask = withoutLayer(mask, FO4_LAYER_PROJECTILE);
            mask = withoutLayer(mask, FO4_LAYER_CONEPROJECTILE);
        }
        if (!blocksSpells) {
            mask = withoutLayer(mask, FO4_LAYER_SPELL);
        }
        if (includeBodyLayer) {
            mask = withLayer(mask, ROCK_LAYER_BODY);
        }
        /*
         * The dynamic hand proxy is an extended (non-vanilla) layer, so it is
         * absent from allConfiguredLayerBits(). Add it explicitly to keep the
         * weapon row symmetric with buildRockDynamicHandProxyExpectedMask();
         * otherwise the matrix watchdog observes bit 48 as perpetual drift.
         */
        mask = withLayer(mask, ROCK_LAYER_DYNAMIC_HAND_PROXY);
        return mask;
    }

    inline constexpr std::uint64_t buildRockBodyExpectedMask(bool includeStaticWorld = true)
    {
        std::uint64_t mask = 0;
        if (includeStaticWorld) {
            mask = withLayer(mask, FO4_LAYER_STATIC);
            mask = withLayer(mask, FO4_LAYER_ANIMSTATIC);
        }
        mask = withLayer(mask, FO4_LAYER_CLUTTER);
        mask = withLayer(mask, FO4_LAYER_WEAPON);
        mask = withLayer(mask, FO4_LAYER_DEBRIS_SMALL);
        mask = withLayer(mask, FO4_LAYER_DEBRIS_LARGE);
        mask = withLayer(mask, FO4_LAYER_SHELLCASING);
        mask = withLayer(mask, FO4_LAYER_CLUTTER_LARGE);
        mask = withLayer(mask, ROCK_LAYER_WEAPON);
        return mask;
    }

    inline constexpr std::uint64_t buildRockReloadExpectedMask(bool blocksProjectiles, bool blocksSpells, bool includeStaticWorld = true)
    {
        if constexpr (ROCK_LAYER_RELOAD == ROCK_LAYER_HAND) {
            return buildRockHandExpectedMask(true, includeStaticWorld);
        } else {
            return buildRockWeaponExpectedMask(blocksProjectiles, blocksSpells, includeStaticWorld);
        }
    }

    inline void applyLayerExpectedMask(std::uint64_t* matrix, std::uint32_t layer, std::uint64_t expectedMask)
    {
        if (!matrix || !isMatrixAddressableLayer(layer)) {
            return;
        }

        for (std::uint32_t other = 0; other < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT; ++other) {
            setPair(matrix, layer, other, maskEnablesLayer(expectedMask, other));
        }
    }

    inline constexpr bool layerPairEnabledFromRow(const std::uint64_t* matrix, std::uint32_t rowLayer, std::uint32_t otherLayer)
    {
        const auto bit = layerBitOrZero(otherLayer);
        return matrix && isMatrixAddressableLayer(rowLayer) && bit != 0 && ((matrix[rowLayer] & bit) != 0);
    }

    inline constexpr bool layerPairSymmetricMatches(const std::uint64_t* matrix, std::uint32_t layerA, std::uint32_t layerB, bool expectedEnabled)
    {
        return layerPairEnabledFromRow(matrix, layerA, layerB) == expectedEnabled &&
               layerPairEnabledFromRow(matrix, layerB, layerA) == expectedEnabled;
    }

    inline constexpr bool nativeCharacterControllerObjectPairsMatch(const std::uint64_t* matrix, std::uint64_t expectedCharacterControllerMask)
    {
        if (!matrix) {
            return false;
        }

        const auto managedMask = nativeCharacterControllerManagedLayerMask();
        for (std::uint32_t other = 0; other < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT; ++other) {
            if (!maskEnablesLayer(managedMask, other)) {
                continue;
            }
            if (!layerPairSymmetricMatches(matrix, FO4_LAYER_CHARCONTROLLER, other, maskEnablesLayer(expectedCharacterControllerMask, other))) {
                return false;
            }
        }
        return true;
    }

    inline constexpr bool rockToolActorPairsMatch(
        const std::uint64_t* matrix,
        std::uint64_t expectedHandMask,
        std::uint64_t expectedWeaponMask)
    {
        if (!matrix) {
            return false;
        }

        constexpr std::uint32_t actorLayers[] = { FO4_LAYER_BIPED, FO4_LAYER_DEADBIP, FO4_LAYER_BIPED_NO_CC };
        for (const auto actorLayer : actorLayers) {
            if (!layerPairSymmetricMatches(matrix, ROCK_LAYER_HAND, actorLayer, maskEnablesLayer(expectedHandMask, actorLayer))) {
                return false;
            }
            if (!layerPairSymmetricMatches(matrix, ROCK_LAYER_WEAPON, actorLayer, maskEnablesLayer(expectedWeaponMask, actorLayer))) {
                return false;
            }
            if constexpr (ROCK_LAYER_RELOAD != ROCK_LAYER_HAND && ROCK_LAYER_RELOAD != ROCK_LAYER_WEAPON) {
                if (!layerPairSymmetricMatches(matrix, ROCK_LAYER_RELOAD, actorLayer, maskEnablesLayer(buildRockReloadExpectedMask(true, true), actorLayer))) {
                    return false;
                }
            }
        }
        return true;
    }

    inline constexpr bool rockBodyPairsMatch(const std::uint64_t* matrix, std::uint64_t expectedBodyMask)
    {
        if (!matrix) {
            return false;
        }

        for (std::uint32_t other = 0; other < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT; ++other) {
            if (!layerPairSymmetricMatches(matrix, ROCK_LAYER_BODY, other, maskEnablesLayer(expectedBodyMask, other))) {
                return false;
            }
        }
        return true;
    }

    inline constexpr bool rockBodyManagedPairsMatch(const std::uint64_t* matrix, std::uint64_t expectedBodyMask)
    {
        if (!matrix) {
            return false;
        }

        /*
         * ROCK owns the generated body layer's matrix row. FO4VR can restore
         * native actor rows such as BIPED/BIPED_NO_CC onto the extended body
         * layer after registration; those actor bits stay excluded from ROCK's
         * authored body mask, but they should not make the watchdog churn the
         * whole collision matrix every frame. All non-actor pairs remain guarded.
         */
        for (std::uint32_t other = 0; other < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT; ++other) {
            if (isActorOrBipedLayer(other)) {
                continue;
            }
            if (!layerPairSymmetricMatches(matrix, ROCK_LAYER_BODY, other, maskEnablesLayer(expectedBodyMask, other))) {
                return false;
            }
        }
        return true;
    }

    inline constexpr bool rockDynamicHandProxyPairsMatch(
        const std::uint64_t* matrix,
        std::uint64_t expectedProxyMask)
    {
        if (!matrix) {
            return false;
        }

        /*
         * Layer 48 is an extended ROCK-owned row. Checking only its row misses
         * one-sided native/mod matrix rewrites in a world or weapon column;
         * those leave Havok pair filtering asymmetric and can silently remove
         * free-hand/weapon contact. Guard every row/column pair we author.
         */
        for (std::uint32_t other = 0;
             other < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT;
             ++other) {
            if (!layerPairSymmetricMatches(
                    matrix,
                    ROCK_LAYER_DYNAMIC_HAND_PROXY,
                    other,
                    maskEnablesLayer(expectedProxyMask, other))) {
                return false;
            }
        }
        return true;
    }

    inline void applyRockHandLayerPolicy(std::uint64_t* matrix, bool includeWeaponLayer, bool includeStaticWorld = true)
    {
        applyLayerExpectedMask(matrix, ROCK_LAYER_HAND, buildRockHandExpectedMask(includeWeaponLayer, includeStaticWorld));
    }

    inline void applyRockWeaponLayerPolicy(std::uint64_t* matrix, bool blocksProjectiles, bool blocksSpells, bool includeStaticWorld = true)
    {
        applyLayerExpectedMask(matrix, ROCK_LAYER_WEAPON, buildRockWeaponExpectedMask(blocksProjectiles, blocksSpells, includeStaticWorld));
    }

    inline void applyRockReloadLayerPolicy(std::uint64_t* matrix, bool blocksProjectiles, bool blocksSpells, bool includeStaticWorld = true)
    {
        applyLayerExpectedMask(matrix, ROCK_LAYER_RELOAD, buildRockReloadExpectedMask(blocksProjectiles, blocksSpells, includeStaticWorld));
    }

    inline void applyRockBodyLayerPolicy(std::uint64_t* matrix, bool includeStaticWorld = true)
    {
        applyLayerExpectedMask(matrix, ROCK_LAYER_BODY, buildRockBodyExpectedMask(includeStaticWorld));
    }

    inline constexpr std::uint64_t buildRockDynamicHandProxyExpectedMask(
        bool includeStaticWorld = true)
    {
        std::uint64_t mask = 0;
        if (includeStaticWorld) {
            for (std::uint32_t layer = 0; layer < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT; ++layer) {
                if (isWorldSurfaceLayer(layer)) {
                    mask = withLayer(mask, layer);
                }
            }
        }
        // One-way visual stopping against the generated equipped-weapon hull.
        // The weapon remains keyframed authority; owned-hand visual output is
        // gated by DynamicHandCollisionRuntime rather than fed back to it.
        mask = withLayer(mask, ROCK_LAYER_WEAPON);
        return mask;
    }

    inline void applyRockDynamicHandProxyLayerPolicy(
        std::uint64_t* matrix,
        bool includeStaticWorld = true)
    {
        applyLayerExpectedMask(
            matrix,
            ROCK_LAYER_DYNAMIC_HAND_PROXY,
            buildRockDynamicHandProxyExpectedMask(includeStaticWorld));
    }

    inline void applyNativeCharacterControllerObjectSuppressionPolicy(
        std::uint64_t* matrix,
        bool suppressDynamicObjects,
        std::uint64_t originalCharacterControllerMask,
        bool blockLargeObjects = false)
    {
        if (!matrix) {
            return;
        }

        /*
         * The loop walks the MANAGED mask. Native CLUTTER/WEAPON/car pairs are
         * restored to their captured values; only the explicit BIPED_NO_CC opt-in
         * pair is disabled. Keeping the parameters preserves configuration and
         * ABI compatibility; generated-body suppression remains per contact.
         */
        const auto managedMask = nativeCharacterControllerManagedLayerMask();
        const auto expectedMask = nativeCharacterControllerExpectedMask(originalCharacterControllerMask, suppressDynamicObjects, blockLargeObjects);
        for (std::uint32_t other = 0; other < FO4_LAYER_MATRIX_ADDRESSABLE_COUNT; ++other) {
            if (maskEnablesLayer(managedMask, other)) {
                setPair(matrix, FO4_LAYER_CHARCONTROLLER, other, maskEnablesLayer(expectedMask, other));
            }
        }
    }

    inline void applyRockGeneratedLayerPolicies(
        std::uint64_t* matrix,
        bool handStaticWorld,
        bool weaponStaticWorld,
        bool bodyStaticWorld,
        bool weaponBlocksProjectiles,
        bool weaponBlocksSpells)
    {
        /*
         * Runtime registration uses one aggregate helper because layer 47 is an
         * extended ROCK-owned matrix row: body<->weapon requires both the weapon
         * row and body row to be normalized together instead of depending on a
         * later standalone body-policy call to repair symmetry.
         */
        applyRockHandLayerPolicy(matrix, true, handStaticWorld);
        applyLayerExpectedMask(matrix,
            ROCK_LAYER_WEAPON,
            buildRockWeaponExpectedMask(weaponBlocksProjectiles, weaponBlocksSpells, weaponStaticWorld, true));
        applyRockReloadLayerPolicy(matrix, weaponBlocksProjectiles, weaponBlocksSpells, handStaticWorld);
        applyRockBodyLayerPolicy(matrix, bodyStaticWorld);
        applyRockDynamicHandProxyLayerPolicy(matrix, handStaticWorld);
    }
}
