#pragma once

/*
 * Weapon support-grip behavior is grouped here so support authority, pose policy, and the two-handed solver evolve as one coherent subsystem.
 */


// ---- WeaponSupportAuthorityPolicy.h ----

/*
 * Equipped-weapon support authority is selected by distance from the firing
 * grip. A support grab close to that grip follows the weapon visually without
 * steering it, leaving one transform owner and allowing the support hand to be
 * promoted cleanly when it takes the firing grip. A grab farther out retains
 * full two-handed manipulation authority. Weapon class affects the steering
 * weight inside the full solver; it must never hard-demote a sidearm and erase
 * the authority floor. Explicit provider grab modes remain authoritative and
 * bypass this proximity contract.
 */

#include "physics-interaction/TransformMath.h"
#include "physics-interaction/weapon/TwoHandedWeaponPolicy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rock::weapon_support_authority_policy
{
    enum class WeaponSupportAuthorityMode
    {
        FullTwoHandedSolver = 0,
        VisualOnlySupport = 1,
    };

    enum class WeaponSupportWeaponClass
    {
        Unknown = 0,
        Sidearm = 1,
        LongGun = 2,
    };

    struct EquippedWeaponIdentity
    {
        std::uint32_t formID{ 0 };
        std::string_view displayName{};
        std::string_view nodeName{};
        bool hasPistolGripKeyword{ false };
        bool hasInstancePistolGripKeyword{ false };
        bool hasLongGunGripKeyword{ false };
        bool hasInstanceLongGunGripKeyword{ false };
    };

    inline constexpr bool hasPistolGripSemantic(const EquippedWeaponIdentity& identity)
    {
        return identity.hasPistolGripKeyword || identity.hasInstancePistolGripKeyword;
    }

    inline constexpr bool hasLongGunGripSemantic(const EquippedWeaponIdentity& identity)
    {
        return identity.hasLongGunGripKeyword || identity.hasInstanceLongGunGripKeyword;
    }

    /*
     * A sidearm remains in the full solver. The Jul-27 contract deliberately
     * separated aim authority from wrist presentation: short lever arms receive
     * a non-zero steering floor, the solved rotation is rate-limited, and the
     * firing-wrist follow is blended independently. Hard-demoting Sidearm here
     * reintroduces the exact regression that contract fixed: the support hand
     * can look attached but has no authority over the weapon.
     */
    inline constexpr WeaponSupportAuthorityMode resolveSupportAuthorityMode(
        WeaponSupportWeaponClass)
    {
        return WeaponSupportAuthorityMode::FullTwoHandedSolver;
    }

    static_assert(
        resolveSupportAuthorityMode(WeaponSupportWeaponClass::Sidearm) ==
            WeaponSupportAuthorityMode::FullTwoHandedSolver,
        "A sidearm must retain the configured non-zero support-authority floor");
    static_assert(
        resolveSupportAuthorityMode(WeaponSupportWeaponClass::LongGun) ==
            WeaponSupportAuthorityMode::FullTwoHandedSolver,
        "Long guns keep the rigid two-handed solver");

    /*
     * Class policy and proximity policy compose without letting the near-grip
     * rule erase a sidearm's steering floor. A pistol support hold is naturally
     * adjacent to the firing grip, so applying the long-gun handoff radius to a
     * sidearm would hard-demote virtually every valid pistol cup to visual-only.
     * Explicit provider AttachOnly/FullTwoHandAuthority modes bypass this local
     * combination before reaching this helper.
     */
    inline constexpr WeaponSupportAuthorityMode combineSupportAuthorityModes(
        WeaponSupportAuthorityMode classMode,
        WeaponSupportAuthorityMode proximityMode,
        WeaponSupportWeaponClass weaponClass =
            WeaponSupportWeaponClass::Unknown)
    {
        if (weaponClass == WeaponSupportWeaponClass::Sidearm &&
            classMode == WeaponSupportAuthorityMode::FullTwoHandedSolver) {
            return WeaponSupportAuthorityMode::FullTwoHandedSolver;
        }
        return (classMode == WeaponSupportAuthorityMode::VisualOnlySupport ||
                   proximityMode == WeaponSupportAuthorityMode::VisualOnlySupport) ?
                   WeaponSupportAuthorityMode::VisualOnlySupport :
                   WeaponSupportAuthorityMode::FullTwoHandedSolver;
    }

    static_assert(
        combineSupportAuthorityModes(
            WeaponSupportAuthorityMode::FullTwoHandedSolver,
            WeaponSupportAuthorityMode::VisualOnlySupport,
            WeaponSupportWeaponClass::Sidearm) ==
            WeaponSupportAuthorityMode::FullTwoHandedSolver,
        "Near-grip proximity must not hard-demote a sidearm support hold");

    inline constexpr char lowerAscii(char value)
    {
        return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
    }

    inline constexpr bool containsIgnoreCase(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty() || haystack.size() < needle.size()) {
            return false;
        }
        for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
            bool matched = true;
            for (std::size_t index = 0; index < needle.size(); ++index) {
                if (lowerAscii(haystack[start + index]) != lowerAscii(needle[index])) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                return true;
            }
        }
        return false;
    }

    inline bool identityNameMatchesAny(const EquippedWeaponIdentity& identity, const std::string_view* tokens, std::size_t tokenCount)
    {
        for (std::size_t index = 0; index < tokenCount; ++index) {
            if (containsIgnoreCase(identity.displayName, tokens[index]) || containsIgnoreCase(identity.nodeName, tokens[index])) {
                return true;
            }
        }
        return false;
    }

    inline WeaponSupportWeaponClass classifyEquippedWeaponForSupportGrip(const EquippedWeaponIdentity& identity)
    {
        if (hasLongGunGripSemantic(identity)) {
            return WeaponSupportWeaponClass::LongGun;
        }
        if (hasPistolGripSemantic(identity)) {
            return WeaponSupportWeaponClass::Sidearm;
        }

        static constexpr std::array<std::string_view, 8> kSidearmNameFallbackTokens{
            "pistol",
            "revolver",
            "deliverer",
            "alien blaster",
            "gamma gun",
            "flare gun",
            "the gainer",
            "western revolver",
        };
        if (identityNameMatchesAny(identity, kSidearmNameFallbackTokens.data(), kSidearmNameFallbackTokens.size())) {
            return WeaponSupportWeaponClass::Sidearm;
        }

        static constexpr std::array<std::string_view, 11> kLongGunTokens{
            "rifle",
            "shotgun",
            "musket",
            "launcher",
            "minigun",
            "fat man",
            "flamer",
            "gatling",
            "harpoon",
            "submachine",
            "machine gun",
        };
        if (identityNameMatchesAny(identity, kLongGunTokens.data(), kLongGunTokens.size())) {
            return WeaponSupportWeaponClass::LongGun;
        }

        return WeaponSupportWeaponClass::Unknown;
    }

    /*
     * The proximity contract applies uniformly to equipped weapons. Provider-
     * mandated grab modes are exempt: AttachOnly must never be upgraded and
     * FullTwoHandAuthority must never be downgraded by local proximity.
     * The decision is made once at grip capture; changing modes requires
     * releasing and re-grabbing.
     */
    inline constexpr bool canApplyFiringGripProximityAuthority(
        bool providerGrabModeOverride)
    {
        return !providerGrabModeOverride;
    }

    inline constexpr WeaponSupportAuthorityMode resolveFiringGripProximityAuthorityMode(
        float supportPalmToFiringGripDistance,
        float visualOnlyRadius)
    {
        return supportPalmToFiringGripDistance <= visualOnlyRadius ?
                   WeaponSupportAuthorityMode::VisualOnlySupport :
                   WeaponSupportAuthorityMode::FullTwoHandedSolver;
    }

    inline constexpr bool supportGripOwnsWeaponTransform(WeaponSupportAuthorityMode mode)
    {
        return mode == WeaponSupportAuthorityMode::FullTwoHandedSolver;
    }

    static_assert(
        two_handed_weapon_policy::usesRigidSupportGripHandAuthority(
            true,
            supportGripOwnsWeaponTransform(
                resolveSupportAuthorityMode(
                    WeaponSupportWeaponClass::Sidearm)),
            false),
        "A full-solver sidearm must publish a rigid support-hand target");
    static_assert(
        two_handed_weapon_policy::usesRigidSupportGripHandAuthority(
            true,
            supportGripOwnsWeaponTransform(
                resolveSupportAuthorityMode(
                    WeaponSupportWeaponClass::LongGun)),
            false),
        "A full-solver long gun must publish a rigid support-hand target");

    inline constexpr bool supportGripAppliesPrimaryHandAuthority(WeaponSupportAuthorityMode mode)
    {
        return mode == WeaponSupportAuthorityMode::FullTwoHandedSolver;
    }

    inline constexpr bool supportGripAppliesSupportHandAuthority(WeaponSupportAuthorityMode)
    {
        return true;
    }

    inline constexpr bool canPromoteSupportGripToFiringGrip(
        WeaponSupportAuthorityMode mode,
        bool authoredSupportGrip)
    {
        // A non-touch authored seat may be used for presentation under the
        // visual-only pistol/near-grip contract, but it must never turn that
        // acquisition into weapon authority. A true-touch dynamic visual grip
        // retains the established explicit handoff path.
        return mode != WeaponSupportAuthorityMode::VisualOnlySupport ||
               !authoredSupportGrip;
    }

    template <class Transform>
    inline Transform buildVisualOnlySupportHandWorld(const Transform& weaponWorld, const Transform& supportHandWeaponLocal)
    {
        return transform_math::composeTransforms(weaponWorld, supportHandWeaponLocal);
    }

}

// ---- WeaponSupportGripPolicy.h ----

#include "physics-interaction/weapon/WeaponTypes.h"

namespace rock::weapon_support_grip_policy
{
    /*
     * ROCK gates two-handing by whether the equipped item can accept support
     * authority and by the hand's live contact with the equipped weapon, not by
     * a narrow list of approved mesh part names. PAPER owns semantic
     * reload/action routing through the provider API; ROCK's support grip remains a generic equipped-weapon contact so
     * stocks, magazines, accessories, unknown NIF chunks, and melee blades can
     * all become the locked support point the player actually touched.
     */

    inline WeaponGripPoseId fallbackPoseForPart(WeaponPartKind partKind)
    {
        switch (partKind) {
        case WeaponPartKind::Foregrip:
            return WeaponGripPoseId::VerticalForegrip;
        case WeaponPartKind::Pump:
            return WeaponGripPoseId::PumpGrip;
        case WeaponPartKind::Handguard:
            return WeaponGripPoseId::HandguardClamp;
        case WeaponPartKind::Barrel:
        case WeaponPartKind::MuzzleDevice:
        case WeaponPartKind::Bipod:
            return WeaponGripPoseId::BarrelWrap;
        case WeaponPartKind::Magazine:
        case WeaponPartKind::Magwell:
            return WeaponGripPoseId::MagwellHold;
        case WeaponPartKind::Stock:
        case WeaponPartKind::Receiver:
        case WeaponPartKind::Grip:
        case WeaponPartKind::Bolt:
        case WeaponPartKind::Slide:
        case WeaponPartKind::ChargingHandle:
        case WeaponPartKind::BreakAction:
        case WeaponPartKind::Cylinder:
        case WeaponPartKind::Chamber:
        case WeaponPartKind::LaserCell:
        case WeaponPartKind::Lever:
        case WeaponPartKind::Sight:
        case WeaponPartKind::Accessory:
        case WeaponPartKind::LaserSight:
        case WeaponPartKind::Flashlight:
        case WeaponPartKind::LaserFlashlightCombo:
        case WeaponPartKind::Scope:
            return WeaponGripPoseId::ReceiverSupport;
        case WeaponPartKind::Shell:
        case WeaponPartKind::Round:
        case WeaponPartKind::CosmeticAmmo:
        case WeaponPartKind::Other:
        case WeaponPartKind::Count:
        default:
            return WeaponGripPoseId::BarrelWrap;
        }
    }

    inline WeaponGripPoseId resolveSupportGripPose(const WeaponInteractionContact& contact)
    {
        return contact.fallbackGripPose != WeaponGripPoseId::None ? contact.fallbackGripPose : fallbackPoseForPart(contact.partKind);
    }

    inline bool canUseContactForSupportGrip(const WeaponInteractionContact& contact, const WeaponInteractionRuntimeState& runtimeState)
    {
        return contact.valid && runtimeState.supportGripAllowed;
    }
}

// ---- EquippedWeaponManualOwnershipPolicy.h ----

namespace rock::equipped_weapon_manual_ownership_policy
{
    inline constexpr std::uint8_t kPrimaryReleaseConfirmFrames = 2;

    struct GripReleaseDebounceState
    {
        std::uint8_t consecutiveOpenFrames{ 0 };
    };

    struct GripReleaseDebounceDecision
    {
        bool retained{ false };
        bool releaseConfirmed{ false };
    };

    struct RuntimeState
    {
        bool active{ false };
        std::uint64_t ownershipKey{ 0 };
    };

    struct Input
    {
        bool weaponEquipped{ false };
        std::uint64_t ownershipKey{ 0 };
        bool startRequested{ false };
        bool primaryGripRetained{ false };
        bool supportGripRetained{ false };
    };

    struct Decision
    {
        bool active{ false };
        bool started{ false };
        bool cleared{ false };
        bool dropRequested{ false };
    };

    struct PendingPrimaryOnlyStartInput
    {
        bool pending{ false };
        bool gripHeld{ false };
        bool ownershipModeEnabled{ true };
        bool primaryPoseBlockerAvailable{ true };
        // Heisenberg-preserved: a VirtualHolsters holster zone owns the grip
        // button, so a pending primary-only start must not survive into it.
        bool virtualHolstersOwnsInput{ false };
    };

    struct FiringGripModeAvailability
    {
        bool primaryDetachEnabled{ false };
        bool ambidextrousHandoffAvailable{ false };
    };

    struct HeldWeaponEquipOwnershipInput
    {
        FiringGripModeAvailability modes{};
        bool handIsLeft{ false };
        bool gripHeld{ false };
    };

    [[nodiscard]] inline constexpr bool firingGripOwnershipEnabled(const FiringGripModeAvailability& modes) noexcept
    {
        return modes.primaryDetachEnabled || modes.ambidextrousHandoffAvailable;
    }

    [[nodiscard]] inline constexpr bool shouldStartHeldWeaponEquipOwnership(const HeldWeaponEquipOwnershipInput& input) noexcept
    {
        return input.gripHeld &&
               (input.modes.primaryDetachEnabled ||
                   (input.handIsLeft && input.modes.ambidextrousHandoffAvailable));
    }

    [[nodiscard]] inline constexpr bool canSettleEquipInGripZone(bool gripZoneEquipEnabled) noexcept
    {
        return gripZoneEquipEnabled;
    }

    [[nodiscard]] inline constexpr bool shouldRetainPrimaryOnlyOwnership(
        bool primaryDetachEnabled,
        bool primaryGripHeld) noexcept
    {
        return !primaryDetachEnabled || primaryGripHeld;
    }

    [[nodiscard]] inline constexpr bool featureAvailable(
        bool ownershipModeEnabled,
        bool primaryPoseBlockerAvailable,
        bool weaponNodeAvailable,
        std::uint64_t equippedWeaponOwnershipKey) noexcept
    {
        return ownershipModeEnabled &&
               primaryPoseBlockerAvailable &&
               weaponNodeAvailable &&
               equippedWeaponOwnershipKey != 0;
    }

    [[nodiscard]] inline constexpr bool shouldKeepPendingPrimaryOnlyStart(const PendingPrimaryOnlyStartInput& input) noexcept
    {
        // Keep trigger-equip's already-held grip alive across equipped weapon node/collision generation latency.
        return input.pending &&
               input.gripHeld &&
               input.ownershipModeEnabled &&
               input.primaryPoseBlockerAvailable &&
               !input.virtualHolstersOwnsInput;
    }

    [[nodiscard]] inline constexpr bool canPreserveManualOwnership(
        std::uint64_t activeEquippedOwnershipKey,
        std::uint64_t currentEquippedOwnershipKey,
        std::uint64_t currentCollisionGenerationKey,
        bool collisionGenerationRequired = true) noexcept
    {
        return activeEquippedOwnershipKey != 0 &&
               activeEquippedOwnershipKey == currentEquippedOwnershipKey &&
               (!collisionGenerationRequired || currentCollisionGenerationKey != 0);
    }

    /*
     * A firing-grip release that confirms while the support grab is only a
     * few frames old is part of the SAME physical gesture (reach-over
     * takeover) or a grab-synchronized grip flicker - never an independent,
     * deliberate release. Acting on it immediately let a fresh offhand grab
     * steal the firing role one frame after capture (left-firing round-4
     * break, 2026-07-12). The window must exceed the release-confirm
     * debounce so the earliest confirm reachable after a grab is always
     * deferred; ~5 frames (about 110ms at 45Hz) also outlasts short grip
     * click flickers while staying imperceptible for deliberate takeovers.
     */
    inline constexpr std::uint32_t kFreshSupportGripPrimaryReleaseDeferFrames = 5;
    static_assert(kFreshSupportGripPrimaryReleaseDeferFrames > kPrimaryReleaseConfirmFrames,
        "defer window must outlast the release-confirm debounce or a grab-synchronized release acts on its first confirmable frame");

    [[nodiscard]] inline constexpr bool shouldDeferPrimaryReleaseActionForFreshSupportGrip(std::uint32_t supportGripAgeFrames) noexcept
    {
        return supportGripAgeFrames <= kFreshSupportGripPrimaryReleaseDeferFrames;
    }

    [[nodiscard]] inline constexpr GripReleaseDebounceDecision debouncePrimaryGripRelease(
        GripReleaseDebounceState& state,
        bool physicallyHeld,
        std::uint8_t confirmFrames = kPrimaryReleaseConfirmFrames) noexcept
    {
        if (physicallyHeld) {
            state = {};
            return GripReleaseDebounceDecision{ .retained = true };
        }

        if (confirmFrames == 0) {
            state = {};
            return GripReleaseDebounceDecision{ .releaseConfirmed = true };
        }

        if (state.consecutiveOpenFrames < confirmFrames) {
            ++state.consecutiveOpenFrames;
        }
        return GripReleaseDebounceDecision{
            .retained = state.consecutiveOpenFrames < confirmFrames,
            .releaseConfirmed = state.consecutiveOpenFrames == confirmFrames,
        };
    }

    inline constexpr Decision update(RuntimeState& state, const Input& input) noexcept
    {
        Decision decision{};

        if (!input.weaponEquipped || input.ownershipKey == 0) {
            decision.cleared = state.active;
            state = {};
            return decision;
        }

        if (state.active && state.ownershipKey != input.ownershipKey) {
            decision.cleared = true;
            state = {};
        }

        if (!state.active && input.startRequested) {
            state.active = true;
            state.ownershipKey = input.ownershipKey;
            decision.started = true;
        }

        if (state.active && !input.primaryGripRetained && !input.supportGripRetained) {
            decision.dropRequested = true;
            state = {};
            return decision;
        }

        decision.active = state.active;
        return decision;
    }
}

// ---- WeaponTwoHandedGripMath.h ----

namespace rock
{
    template <class Vector>
    inline Vector weaponSolverSub(const Vector& lhs, const Vector& rhs);

    template <class Vector>
    inline Vector weaponSolverAdd(const Vector& lhs, const Vector& rhs);
}

namespace rock::weapon_two_handed_grip_math
{
    enum class SupportReleaseManualAction
    {
        EndSupportOnly = 0,
        KeepPrimaryOwnership = 1,
        DropEquippedWeapon = 2,
    };

    struct SupportReleaseOwnershipInput
    {
        bool firingGripOwnershipEnabled{ false };
        bool primaryDetachEnabled{ false };
        bool primaryGripHeld{ false };
    };

    /*
     * Equipped weapon two-hand support has two independent ownership rules:
     * the support hand must be attached to the mesh point it actually touched,
     * and it must not also own a normal dynamic-object grab. Keeping these
     * rules as pure math/policy avoids mixing equipped-weapon authority with
     * ROCK's separate dynamic object grab path.
     */

    template <class Transform, class Vector>
    inline Transform alignHandFrameToGripPoint(const Transform& handWorldTransform, const Vector& currentGripPivotWorld, const Vector& targetGripPointWorld)
    {
        Transform result = handWorldTransform;
        const Vector correction = weaponSolverSub(targetGripPointWorld, currentGripPivotWorld);
        result.translate = weaponSolverAdd(result.translate, correction);
        return result;
    }

    /*
     * Finger solving keeps the live skeleton in its current authoritative hand
     * frame and moves the contacted mesh by the inverse of the pending hand-seat
     * translation. This is rigidly equivalent to moving the hand onto the
     * weapon, but it lets the regular frozen-target solver, pad probes, and
     * object-local surface capture all observe one coherent final relation.
     */
    template <class Transform, class Vector>
    inline Transform virtualizeMeshForTranslatedHandSeat(
        const Transform& meshWorldTransform,
        const Vector& currentGripPivotWorld,
        const Vector& targetGripPointWorld)
    {
        Transform result = meshWorldTransform;
        const Vector handSeatCorrection = weaponSolverSub(targetGripPointWorld, currentGripPivotWorld);
        result.translate = weaponSolverSub(result.translate, handSeatCorrection);
        return result;
    }

    template <class Vector>
    inline Vector virtualizeGripPointForTranslatedHandSeat(
        const Vector& currentGripPivotWorld,
        const Vector& targetGripPointWorld)
    {
        const Vector handSeatCorrection = weaponSolverSub(targetGripPointWorld, currentGripPivotWorld);
        return weaponSolverSub(targetGripPointWorld, handSeatCorrection);
    }

    inline bool canStartSupportGrip(bool touchingSupportPart, bool gripPressed, bool supportHandHoldingObject)
    {
        return touchingSupportPart && gripPressed && !supportHandHoldingObject;
    }

    inline bool shouldContinueSupportGrip(bool gripPressed, bool supportHandHoldingObject)
    {
        return gripPressed && !supportHandHoldingObject;
    }

    inline constexpr SupportReleaseManualAction resolveSupportReleaseManualAction(const SupportReleaseOwnershipInput& input)
    {
        if (!input.firingGripOwnershipEnabled) {
            return SupportReleaseManualAction::EndSupportOnly;
        }

        if (input.primaryGripHeld || !input.primaryDetachEnabled) {
            return SupportReleaseManualAction::KeepPrimaryOwnership;
        }

        return SupportReleaseManualAction::DropEquippedWeapon;
    }

    /*
     * Normal (world) grab gating per hand ROLE, not per physical hand. The
     * firing hand is blocked while an equipped weapon exists unless it is
     * detached and free in part-carry; the support hand is blocked only while
     * its own weapon part grip is active. Roles follow the runtime firing
     * hand (TwoHandedGrip::isFiringHandLeft()).
     */
    inline bool canProcessNormalGrabInput(bool handIsFiringHand, bool weaponEquipped, bool handPartGripActive, bool handDetachedFree)
    {
        if (!handIsFiringHand) {
            return !handPartGripActive;
        }

        return !weaponEquipped || handDetachedFree;
    }

    struct FiringGripReattachInput
    {
        bool partCarryActive{ false };
        bool menuInputActive{ false };
        bool handHoldingObject{ false };
    };

    inline constexpr bool canAttemptFiringGripReattach(const FiringGripReattachInput& input)
    {
        return input.partCarryActive && !input.menuInputActive && !input.handHoldingObject;
    }

    /*
     * Firing-grip reattach contract: the grab button is the hand. A held grab
     * with the free firing palm inside the reattach radius re-takes the grip;
     * nothing ever attaches to an open hand. The gesture cannot re-capture a
     * fresh detach because the detach itself requires the grab to be open,
     * and the same squeeze outside the radius stays available for weapon part
     * grips and world grabs.
     */
    inline constexpr bool shouldReattachFiringGripOnGrab(bool gripHeld, float palmToGripDistance, float reattachRadius)
    {
        return gripHeld && palmToGripDistance <= reattachRadius;
    }

    /*
     * Hover twin of the reattach gate: an OPEN firing palm inside the radius
     * means a squeeze right now would re-take the firing grip, so the runtime
     * owner drives continuous haptic feedback while this holds. A held grab
     * is never a hover -- it is the reattach itself.
     */
    inline constexpr bool isFiringGripReattachHoverCandidate(bool gripHeld, float palmToGripDistance, float reattachRadius)
    {
        return !gripHeld && palmToGripDistance <= reattachRadius;
    }

    inline constexpr bool canStartFreeHandPartGrip(
        bool routedSupportGrip,
        bool gripPressed,
        bool handHoldingObject,
        bool handAlreadyGripping)
    {
        return routedSupportGrip && gripPressed && !handHoldingObject && !handAlreadyGripping;
    }
}

// ---- WeaponTwoHandedSolver.h ----

#include "physics-interaction/TransformMath.h"

#include <algorithm>
#include <cmath>

namespace rock
{
    /*
     * FRIK's suppressed offhand grip works because it solves the weapon
     * transform from two points: the fixed primary grip and the offhand support
     * point. ROCK uses generated mesh collision, so this solver returns one
     * coherent weapon-root transform that can be applied to both the visible
     * weapon node and every generated collision body.
     */

    template <class Transform, class Vector>
    struct WeaponTwoHandedSolverInput
    {
        Transform weaponWorldTransform{};
        Vector primaryGripLocal{};
        Vector supportGripLocal{};
        Vector primaryTargetWorld{};
        Vector supportTargetWorld{};
        Vector supportNormalLocal{};
        Vector supportNormalTargetWorld{};
        float minimumSeparation{ 0.001f };
        float supportNormalTwistFactor{ 0.0f };
        bool useSupportNormalTwist{ false };
        // When active, roll about the aim axis follows the live firing
        // controller and pivots at the primary grip. This takes precedence over
        // support-normal twist so the firing hand, not the support palm, owns
        // weapon roll.
        Vector primaryRollLocal{};
        Vector primaryRollTargetWorld{};
        float primaryRollTwistFactor{ 1.0f };
        bool usePrimaryRollTwist{ false };
    };

    template <class Transform>
    struct WeaponTwoHandedSolverResult
    {
        Transform weaponWorldTransform{};
        decltype(Transform{}.rotate) rotationDelta{};
        bool solved{ false };
        float primaryError{ 0.0f };
        float supportError{ 0.0f };
        float appliedTwistRadians{ 0.0f };
    };

    template <class Vector>
    inline Vector weaponSolverSub(const Vector& lhs, const Vector& rhs)
    {
        return Vector{ lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
    }

    template <class Vector>
    inline Vector weaponSolverAdd(const Vector& lhs, const Vector& rhs)
    {
        return Vector{ lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
    }

    template <class Vector>
    inline Vector weaponSolverScale(const Vector& vector, float scale)
    {
        return Vector{ vector.x * scale, vector.y * scale, vector.z * scale };
    }

    template <class Vector>
    inline float weaponSolverDot(const Vector& lhs, const Vector& rhs)
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    template <class Vector>
    inline Vector weaponSolverCross(const Vector& lhs, const Vector& rhs)
    {
        return Vector{ lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x };
    }

    template <class Vector>
    inline float weaponSolverLength(const Vector& vector)
    {
        return std::sqrt(weaponSolverDot(vector, vector));
    }

    template <class Vector>
    inline Vector weaponSolverNormalize(const Vector& vector)
    {
        const float length = weaponSolverLength(vector);
        if (length <= 0.000001f) {
            return Vector{};
        }
        return weaponSolverScale(vector, 1.0f / length);
    }

    template <class Matrix, class Vector>
    inline Matrix weaponSolverStoredRotationFromConventionalRows(const Vector rows[3])
    {
        Matrix result{};
        for (int row = 0; row < 3; ++row) {
            result.entry[row][0] = rows[0].x;
            result.entry[row][1] = rows[1].x;
            result.entry[row][2] = rows[2].x;
        }
        result.entry[0][0] = rows[0].x;
        result.entry[0][1] = rows[1].x;
        result.entry[0][2] = rows[2].x;
        result.entry[1][0] = rows[0].y;
        result.entry[1][1] = rows[1].y;
        result.entry[1][2] = rows[2].y;
        result.entry[2][0] = rows[0].z;
        result.entry[2][1] = rows[1].z;
        result.entry[2][2] = rows[2].z;
        return result;
    }

    template <class Vector>
    inline Vector weaponSolverOrthogonalAxis(const Vector& value)
    {
        Vector axis = std::abs(value.x) < 0.7f ? Vector{ 1.0f, 0.0f, 0.0f } : Vector{ 0.0f, 1.0f, 0.0f };
        return weaponSolverNormalize(weaponSolverCross(value, axis));
    }

    template <class Matrix, class Vector>
    inline Matrix weaponSolverRotationBetweenStored(const Vector& fromRaw, const Vector& toRaw)
    {
        const Vector from = weaponSolverNormalize(fromRaw);
        const Vector to = weaponSolverNormalize(toRaw);
        float cosTheta = (std::max)(-1.0f, (std::min)(1.0f, weaponSolverDot(from, to)));

        /*
         * The parallel guard exists only to keep the cross-product axis
         * numerically stable; it must stay far below perception. At 0.9999
         * (0.81deg) it becomes a visible deadzone for any solve whose base
         * transform is its own previous output: part-carry accumulated slow
         * hand motion inside the deadzone and released it as ~1deg snaps at
         * ~10Hz. 0.999999 (~0.08deg) still leaves sin(theta) ~1.4e-3, orders
         * of magnitude above float32 cross-product noise.
         */
        if (cosTheta > 0.999999f) {
            return transform_math::makeIdentityRotation<Matrix>();
        }

        Vector axis{};
        float sinTheta = 0.0f;
        if (cosTheta < -0.9999f) {
            axis = weaponSolverOrthogonalAxis(from);
            sinTheta = 0.0f;
            cosTheta = -1.0f;
        } else {
            axis = weaponSolverNormalize(weaponSolverCross(from, to));
            sinTheta = std::sqrt((std::max)(0.0f, 1.0f - cosTheta * cosTheta));
        }

        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        const float oneMinusCos = 1.0f - cosTheta;

        Vector conventionalRows[3]{
            Vector{ cosTheta + x * x * oneMinusCos, x * y * oneMinusCos - z * sinTheta, x * z * oneMinusCos + y * sinTheta },
            Vector{ y * x * oneMinusCos + z * sinTheta, cosTheta + y * y * oneMinusCos, y * z * oneMinusCos - x * sinTheta },
            Vector{ z * x * oneMinusCos - y * sinTheta, z * y * oneMinusCos + x * sinTheta, cosTheta + z * z * oneMinusCos },
        };

        return weaponSolverStoredRotationFromConventionalRows<Matrix, Vector>(conventionalRows);
    }

    template <class Matrix, class Vector>
    inline Matrix weaponSolverAxisAngleStored(const Vector& axisRaw, float angle)
    {
        const Vector axis = weaponSolverNormalize(axisRaw);
        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        const float cosTheta = std::cos(angle);
        const float sinTheta = std::sin(angle);
        const float oneMinusCos = 1.0f - cosTheta;

        Vector conventionalRows[3]{
            Vector{ cosTheta + x * x * oneMinusCos, x * y * oneMinusCos - z * sinTheta, x * z * oneMinusCos + y * sinTheta },
            Vector{ y * x * oneMinusCos + z * sinTheta, cosTheta + y * y * oneMinusCos, y * z * oneMinusCos - x * sinTheta },
            Vector{ z * x * oneMinusCos - y * sinTheta, z * y * oneMinusCos + x * sinTheta, cosTheta + z * z * oneMinusCos },
        };

        return weaponSolverStoredRotationFromConventionalRows<Matrix, Vector>(conventionalRows);
    }

    template <class Matrix, class Vector>
    inline Matrix weaponSolverApplyWorldRotationToStoredBasis(const Matrix& worldRotationStored, const Matrix& baseRotation)
    {
        Matrix result{};
        const Vector basis[3]{
            Vector{ baseRotation.entry[0][0], baseRotation.entry[0][1], baseRotation.entry[0][2] },
            Vector{ baseRotation.entry[1][0], baseRotation.entry[1][1], baseRotation.entry[1][2] },
            Vector{ baseRotation.entry[2][0], baseRotation.entry[2][1], baseRotation.entry[2][2] },
        };

        for (int axis = 0; axis < 3; ++axis) {
            const Vector rotated = transform_math::rotateLocalVectorToWorld(worldRotationStored, basis[axis]);
            result.entry[axis][0] = rotated.x;
            result.entry[axis][1] = rotated.y;
            result.entry[axis][2] = rotated.z;
        }
        return result;
    }

    template <class Matrix, class Vector>
    inline Vector weaponSolverApplyStoredWorldRotationToVector(const Matrix& worldRotationStored, const Vector& vector)
    {
        return transform_math::rotateLocalVectorToWorld(worldRotationStored, vector);
    }

    template <class Vector>
    inline Vector weaponSolverProjectOntoPlane(const Vector& vector, const Vector& planeNormal)
    {
        const float normalDot = weaponSolverDot(vector, planeNormal);
        return weaponSolverSub(vector, weaponSolverScale(planeNormal, normalDot));
    }

    template <class Vector>
    inline Vector makeLockedSupportGripTarget(
        const Vector& primaryTargetWorld,
        const Vector& supportControllerWorld,
        const Vector& fallbackSupportTargetWorld,
        float lockedGripDistance,
        float minimumSeparation)
    {
        /*
         * ROCK keeps two-handed weapon grabs as captured hand-to-weapon
         * relationships and uses controller motion to rotate the held weapon,
         * not to slide the visual contact point along the model. The solver
         * preserves the captured primary-support distance while still using the
         * support controller direction for aim.
         */
        if (lockedGripDistance <= minimumSeparation) {
            return fallbackSupportTargetWorld;
        }

        Vector targetAxis = weaponSolverSub(supportControllerWorld, primaryTargetWorld);
        if (weaponSolverLength(targetAxis) <= minimumSeparation) {
            targetAxis = weaponSolverSub(fallbackSupportTargetWorld, primaryTargetWorld);
        }
        if (weaponSolverLength(targetAxis) <= minimumSeparation) {
            return fallbackSupportTargetWorld;
        }

        const Vector direction = weaponSolverNormalize(targetAxis);
        return weaponSolverAdd(primaryTargetWorld, weaponSolverScale(direction, lockedGripDistance));
    }

    /*
     * Low-pass the support direction without changing the locked grip
     * separation. The state is owned by a single grip episode and reset at
     * acquisition/release, so a new hold can never inherit stale aim.
     */
    template <class Vector>
    inline Vector makeSmoothedLockedSupportGripTarget(
        const Vector& primaryTargetWorld,
        const Vector& supportControllerWorld,
        const Vector& fallbackSupportTargetWorld,
        float lockedGripDistance,
        float minimumSeparation,
        Vector& smoothedDirectionWorld,
        bool& hasSmoothedDirection,
        float dt,
        float smoothingTimeConstantSeconds = 0.03f)
    {
        if (lockedGripDistance <= minimumSeparation) {
            hasSmoothedDirection = false;
            return fallbackSupportTargetWorld;
        }

        Vector targetAxis = weaponSolverSub(supportControllerWorld, primaryTargetWorld);
        if (weaponSolverLength(targetAxis) <= minimumSeparation) {
            targetAxis = weaponSolverSub(fallbackSupportTargetWorld, primaryTargetWorld);
        }
        if (weaponSolverLength(targetAxis) <= minimumSeparation) {
            hasSmoothedDirection = false;
            return fallbackSupportTargetWorld;
        }

        const Vector rawDirection = weaponSolverNormalize(targetAxis);
        Vector direction = rawDirection;
        if (hasSmoothedDirection && smoothingTimeConstantSeconds > 0.0f && dt > 0.0f) {
            const float alpha = 1.0f - std::exp(-dt / smoothingTimeConstantSeconds);
            const Vector blended = weaponSolverAdd(
                weaponSolverScale(smoothedDirectionWorld, 1.0f - alpha),
                weaponSolverScale(rawDirection, alpha));
            const Vector normalizedBlended = weaponSolverNormalize(blended);
            if (weaponSolverLength(normalizedBlended) > 0.0001f) {
                direction = normalizedBlended;
            }
        }

        smoothedDirectionWorld = direction;
        hasSmoothedDirection = true;
        return weaponSolverAdd(primaryTargetWorld, weaponSolverScale(direction, lockedGripDistance));
    }

    template <class Transform, class Vector>
    inline WeaponTwoHandedSolverResult<Transform> solveTwoHandedWeaponTransform(const WeaponTwoHandedSolverInput<Transform, Vector>& input)
    {
        WeaponTwoHandedSolverResult<Transform> result{};
        result.weaponWorldTransform = input.weaponWorldTransform;
        result.rotationDelta = transform_math::makeIdentityRotation<decltype(input.weaponWorldTransform.rotate)>();

        const Vector localAxis = weaponSolverSub(input.supportGripLocal, input.primaryGripLocal);
        const Vector currentAxisWorld = transform_math::localVectorToWorld(input.weaponWorldTransform, localAxis);
        const Vector desiredAxisWorld = weaponSolverSub(input.supportTargetWorld, input.primaryTargetWorld);

        if (weaponSolverLength(currentAxisWorld) <= input.minimumSeparation || weaponSolverLength(desiredAxisWorld) <= input.minimumSeparation) {
            return result;
        }

        const auto rotationDelta = weaponSolverRotationBetweenStored<decltype(input.weaponWorldTransform.rotate), Vector>(currentAxisWorld, desiredAxisWorld);
        result.rotationDelta = rotationDelta;
        result.weaponWorldTransform.rotate =
            weaponSolverApplyWorldRotationToStoredBasis<decltype(input.weaponWorldTransform.rotate), Vector>(rotationDelta, input.weaponWorldTransform.rotate);

        const Vector primaryAfterRotation = transform_math::localPointToWorld(result.weaponWorldTransform, input.primaryGripLocal);
        const Vector primaryCorrection = weaponSolverSub(input.primaryTargetWorld, primaryAfterRotation);
        result.weaponWorldTransform.translate = weaponSolverAdd(result.weaponWorldTransform.translate, primaryCorrection);

        const Vector primaryWorld = transform_math::localPointToWorld(result.weaponWorldTransform, input.primaryGripLocal);
        const Vector supportWorld = transform_math::localPointToWorld(result.weaponWorldTransform, input.supportGripLocal);
        result.primaryError = weaponSolverLength(weaponSolverSub(primaryWorld, input.primaryTargetWorld));
        result.supportError = weaponSolverLength(weaponSolverSub(supportWorld, input.supportTargetWorld));

        const bool primaryRollActive =
            input.usePrimaryRollTwist && input.primaryRollTwistFactor > 0.0f;
        if (primaryRollActive ||
            (input.useSupportNormalTwist && input.supportNormalTwistFactor > 0.0f)) {
            const Vector twistAxis = weaponSolverNormalize(weaponSolverSub(input.supportTargetWorld, input.primaryTargetWorld));
            const Vector currentNormalWorld = transform_math::localVectorToWorld(
                result.weaponWorldTransform,
                primaryRollActive ? input.primaryRollLocal : input.supportNormalLocal);
            const Vector desiredNormalWorld =
                primaryRollActive ? input.primaryRollTargetWorld : input.supportNormalTargetWorld;
            const float twistFactor =
                primaryRollActive ? input.primaryRollTwistFactor : input.supportNormalTwistFactor;
            const Vector currentProjected = weaponSolverNormalize(weaponSolverProjectOntoPlane(currentNormalWorld, twistAxis));
            const Vector desiredProjected = weaponSolverNormalize(weaponSolverProjectOntoPlane(desiredNormalWorld, twistAxis));

            if (weaponSolverLength(currentProjected) > input.minimumSeparation && weaponSolverLength(desiredProjected) > input.minimumSeparation) {
                const float dotValue = (std::max)(-1.0f, (std::min)(1.0f, weaponSolverDot(currentProjected, desiredProjected)));
                const Vector crossValue = weaponSolverCross(currentProjected, desiredProjected);
                const float signedAngle =
                    std::atan2(weaponSolverDot(twistAxis, crossValue), dotValue) * twistFactor;
                result.appliedTwistRadians = signedAngle;
                const auto twistRotation = weaponSolverAxisAngleStored<decltype(input.weaponWorldTransform.rotate), Vector>(twistAxis, signedAngle);
                result.rotationDelta =
                    weaponSolverApplyWorldRotationToStoredBasis<decltype(input.weaponWorldTransform.rotate), Vector>(twistRotation, result.rotationDelta);

                const Vector twistPivot =
                    primaryRollActive ? input.primaryTargetWorld : input.supportTargetWorld;
                const Vector pivotToWeapon =
                    weaponSolverSub(result.weaponWorldTransform.translate, twistPivot);
                const Vector rotatedPivotToWeapon = weaponSolverApplyStoredWorldRotationToVector<decltype(input.weaponWorldTransform.rotate), Vector>(twistRotation, pivotToWeapon);
                result.weaponWorldTransform.translate =
                    weaponSolverAdd(twistPivot, rotatedPivotToWeapon);
                result.weaponWorldTransform.rotate =
                    weaponSolverApplyWorldRotationToStoredBasis<decltype(input.weaponWorldTransform.rotate), Vector>(twistRotation, result.weaponWorldTransform.rotate);

                const Vector primaryAfterTwist = transform_math::localPointToWorld(result.weaponWorldTransform, input.primaryGripLocal);
                const Vector primaryTwistCorrection = weaponSolverSub(input.primaryTargetWorld, primaryAfterTwist);
                result.weaponWorldTransform.translate = weaponSolverAdd(result.weaponWorldTransform.translate, primaryTwistCorrection);

                const Vector primaryFinal = transform_math::localPointToWorld(result.weaponWorldTransform, input.primaryGripLocal);
                const Vector supportFinal = transform_math::localPointToWorld(result.weaponWorldTransform, input.supportGripLocal);
                result.primaryError = weaponSolverLength(weaponSolverSub(primaryFinal, input.primaryTargetWorld));
                result.supportError = weaponSolverLength(weaponSolverSub(supportFinal, input.supportTargetWorld));
            }
        }

        result.solved = true;
        return result;
    }

    template <class Transform, class Vector>
    inline WeaponTwoHandedSolverResult<Transform> solveTwoHandedWeaponTransformFrikPivot(const WeaponTwoHandedSolverInput<Transform, Vector>& input)
    {
        /*
         * This named entry point is intentionally kept beside the generic
         * solver. FRIK's suppressed two-handed grip already proved the right
         * firearm behavior: aim from the right-hand primary grip toward the
         * offhand support point, then retranslate the weapon so the primary
         * grip pivot does not drift. ROCK layers semantic mesh anchors and full
         * weapon/collision authority around that same math instead of inventing
         * a second aiming convention.
         */
        return solveTwoHandedWeaponTransform(input);
    }
}
