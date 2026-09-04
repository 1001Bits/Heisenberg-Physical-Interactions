#pragma once

namespace rock::dynamic_hand_collision_authority
{
    struct Input
    {
        bool isLeft = false;
        bool dominantWeaponAuthority = false;
        bool partGripActive = false;
        bool firingGripOccupied = false;
        bool firingHandIsLeft = false;
    };

    /*
     * Dynamic proxies continue tracking while a weapon grip owns the hand, but
     * their lower-priority visual correction and collision-entry haptic must
     * stand down. Include part-carry and left-firing transitions explicitly;
     * neither is represented by the legacy "right weapon equipped" flag.
     */
    [[nodiscard]] constexpr bool weaponOwnsHand(const Input& input)
    {
        return input.dominantWeaponAuthority ||
               input.partGripActive ||
               (input.isLeft && input.firingGripOccupied &&
                   input.firingHandIsLeft);
    }

    struct ReciprocalWeaponStopInput
    {
        bool stopActive = false;
        bool targetIsDynamicHand = false;
        bool targetHandIsLeft = false;
        bool handIsLeft = false;
    };

    /*
     * A reciprocal gun stop is the stronger visual owner only for the exact
     * free hand that the generated weapon contacted. World stops and the
     * opposite hand must leave dynamic-hand presentation untouched.
     */
    [[nodiscard]] constexpr bool reciprocalWeaponStopOwnsHand(
        const ReciprocalWeaponStopInput& input)
    {
        return input.stopActive &&
               input.targetIsDynamicHand &&
               input.targetHandIsLeft == input.handIsLeft;
    }

    struct WorldStopReadinessInput
    {
        bool runtimeFrameReady = false;
        bool handDisabled = false;
        bool transitionCollisionSuppressed = false;
        bool visualAuthorityAvailable = false;
        bool allRequiredProductionBodiesCurrent = false;
        bool hasSolvedPhysicsSample = false;
    };

    /*
     * This is deliberately runtime health, not configuration intent.  The
     * legacy hand/world solver may stand down for one hand only after that
     * hand has a complete current-world proxy bank and at least one post-solve
     * sample proving the new bank has actually entered the physics loop.
     */
    [[nodiscard]] constexpr bool worldStopOperational(
        const WorldStopReadinessInput& input)
    {
        return input.runtimeFrameReady &&
               !input.handDisabled &&
               !input.transitionCollisionSuppressed &&
               input.visualAuthorityAvailable &&
               input.allRequiredProductionBodiesCurrent &&
               input.hasSolvedPhysicsSample;
    }
}
