#pragma once

#include "ROCKProviderApi.h"

#include <cstdint>
#include <limits>

namespace rock::provider::weapon_part_drive_lease_policy
{
    inline constexpr std::uint64_t NEVER_EXPIRES_FRAME = (std::numeric_limits<std::uint64_t>::max)();

    [[nodiscard]] constexpr bool isPersistent(std::uint32_t leaseFrames) noexcept
    {
        return leaseFrames >= ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1;
    }

    [[nodiscard]] constexpr std::uint32_t normalizeFiniteFrames(std::uint32_t leaseFrames) noexcept
    {
        return leaseFrames > ROCK_PROVIDER_MAX_WEAPON_PART_DRIVE_LEASE_FRAMES_V1 ?
                   ROCK_PROVIDER_MAX_WEAPON_PART_DRIVE_LEASE_FRAMES_V1 :
                   leaseFrames;
    }

    [[nodiscard]] constexpr std::uint64_t expiresAfterFrame(
        std::uint64_t currentFrame,
        std::uint32_t leaseFrames) noexcept
    {
        if (isPersistent(leaseFrames)) {
            return NEVER_EXPIRES_FRAME;
        }

        const auto finiteFrames = static_cast<std::uint64_t>(normalizeFiniteFrames(leaseFrames));
        if (currentFrame > NEVER_EXPIRES_FRAME - finiteFrames) {
            return NEVER_EXPIRES_FRAME;
        }
        return currentFrame + finiteFrames;
    }

    [[nodiscard]] constexpr bool isExpired(
        std::uint64_t expiresAfter,
        std::uint64_t currentFrame) noexcept
    {
        return expiresAfter != NEVER_EXPIRES_FRAME && expiresAfter < currentFrame;
    }
}
