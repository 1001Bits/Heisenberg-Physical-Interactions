#pragma once

namespace heisenberg::storage_zone_weapon_policy
{
    [[nodiscard]] inline constexpr bool isPrimaryHand(
        const bool handIsLeft,
        const bool leftHandedMode) noexcept
    {
        return handIsLeft == leftHandedMode;
    }

    [[nodiscard]] inline constexpr bool offhandIsLeft(
        const bool leftHandedMode) noexcept
    {
        return !leftHandedMode;
    }

    struct Input
    {
        bool featureEnabled = false;
        bool primaryHand = false;
        bool insideStorageZone = false;
        bool offhandGrabbingWeapon = false;
        bool playerHasWeaponEquipped = false;
    };

    // StorageZone is not a general-purpose weapon holster. It may take the
    // primary-hand grip only for the weapon-swap gesture: a loose weapon is
    // already held in the offhand while another real weapon occupies the
    // player's weapon hand. Every ordinary shoulder-holster press stays
    // available to Virtual Holsters and the native input path.
    [[nodiscard]] inline constexpr bool shouldUnequipPrimaryWeapon(
        const Input& input) noexcept
    {
        return input.featureEnabled &&
               input.primaryHand &&
               input.insideStorageZone &&
               input.offhandGrabbingWeapon &&
               input.playerHasWeaponEquipped;
    }
}
