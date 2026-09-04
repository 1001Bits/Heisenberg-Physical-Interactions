#pragma once

#include <cstdint>

namespace rock::post_host_generated_drive_finalize_policy
{
    struct Identity
    {
        std::uint64_t gameFrameIndex = 0;
        std::uint64_t weaponGenerationKey = 0;
        std::uint32_t worldGeneration = 0;
        std::uint32_t skeletonGeneration = 0;
        std::uint32_t providerGeneration = 0;
        std::uint32_t collisionGeneration = 0;
        std::uintptr_t bhkWorld = 0;
        std::uintptr_t hknpWorld = 0;
        std::uintptr_t weaponNode = 0;
    };

    [[nodiscard]] inline constexpr bool isCurrent(
        const bool pending,
        const bool initialized,
        const Identity& queued,
        const Identity& current) noexcept
    {
        return pending &&
               initialized &&
               current.bhkWorld != 0 &&
               current.hknpWorld != 0 &&
               queued.gameFrameIndex == current.gameFrameIndex &&
               queued.weaponGenerationKey ==
                   current.weaponGenerationKey &&
               queued.worldGeneration == current.worldGeneration &&
               queued.skeletonGeneration ==
                   current.skeletonGeneration &&
               queued.providerGeneration ==
                   current.providerGeneration &&
               queued.collisionGeneration ==
                   current.collisionGeneration &&
               queued.bhkWorld == current.bhkWorld &&
               queued.hknpWorld == current.hknpWorld &&
               queued.weaponNode == current.weaponNode;
    }
}
