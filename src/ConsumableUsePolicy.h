#pragma once

namespace heisenberg::consumable_use_policy
{
    inline constexpr float kOrdinaryGrabGraceSeconds = 0.5f;
    inline constexpr float kDeliveredLootGraceSeconds = 1.0f;

    [[nodiscard]] inline constexpr float requiredGraceSeconds(
        const bool deliveredToHand) noexcept
    {
        return deliveredToHand ?
                   kDeliveredLootGraceSeconds :
                   kOrdinaryGrabGraceSeconds;
    }

    // A NaN or negative elapsed time compares false and therefore fails safe.
    [[nodiscard]] inline constexpr bool mayUseHeldConsumable(
        const float elapsedSinceGrabSeconds,
        const bool deliveredToHand,
        const bool consumeZoneExitRequired = false) noexcept
    {
        return !consumeZoneExitRequired &&
               elapsedSinceGrabSeconds >=
               requiredGraceSeconds(deliveredToHand);
    }
}
