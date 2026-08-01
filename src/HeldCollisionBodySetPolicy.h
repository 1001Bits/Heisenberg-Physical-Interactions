#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace heisenberg::held_collision_body_set_policy
{
    inline constexpr std::uint32_t kInvalidBodyId = 0x7FFF'FFFFu;
    inline constexpr std::uint32_t kInvalidMotionId = 0xFFFF'FFFFu;
    inline constexpr std::uint32_t kCollisionLayerMask = 0x7Fu;
    inline constexpr std::uint32_t kNonCollidableLayer = 15u;
    inline constexpr std::uint32_t kHeldNoCharacterControllerLayer = 33u;

    struct BodyCandidate
    {
        std::uint32_t bodyId = kInvalidBodyId;
        std::uint32_t motionId = kInvalidMotionId;
        bool selectedWrapper = false;
        bool visibleWrapper = false;
    };

    template <std::size_t Capacity>
    struct MotionRepresentativeSelection
    {
        std::array<bool, Capacity> representatives{};
        std::size_t uniqueMotionCount = 0;
        bool valid = false;
    };

    [[nodiscard]] constexpr int RepresentativePriority(
        const BodyCandidate& candidate)
    {
        if (candidate.selectedWrapper) {
            return 2;
        }
        return candidate.visibleWrapper ? 1 : 0;
    }

    [[nodiscard]] constexpr bool ShouldKeepNativeCollision(
        bool selectedWrapper,
        bool visibleWrapper)
    {
        return selectedWrapper || visibleWrapper;
    }

    [[nodiscard]] constexpr std::uint32_t WithCollisionLayer(
        std::uint32_t filterInfo,
        std::uint32_t layer)
    {
        return (filterInfo & ~kCollisionLayerMask) |
               (layer & kCollisionLayerMask);
    }

    // Fallout's vendored Havok wrapper documents BIPED_NO_CC as the temporary
    // layer for grabbed objects that must not bump the character controller.
    // Hidden alternate wrappers remain fully non-collidable so invisible
    // multipart bodies cannot knock over nearby props.
    [[nodiscard]] constexpr std::uint32_t HeldBodyFilter(
        std::uint32_t currentFilterInfo,
        bool renderedCollisionAuthority)
    {
        return WithCollisionLayer(
            currentFilterInfo,
            renderedCollisionAuthority
                ? kHeldNoCharacterControllerLayer
                : kNonCollidableLayer);
    }

    // BODY records remain unique for filter/cache restoration. Transform and
    // velocity operations choose one representative for each shared MOTION,
    // preferring acquisition authority, then another rendered wrapper, then a
    // hidden shared-system alternate.
    template <std::size_t Capacity>
    [[nodiscard]] constexpr MotionRepresentativeSelection<Capacity>
    SelectMotionRepresentatives(
        const std::array<BodyCandidate, Capacity>& candidates,
        std::size_t count)
    {
        MotionRepresentativeSelection<Capacity> result{};
        if (count == 0 || count > Capacity) {
            return result;
        }

        for (std::size_t i = 0; i < count; ++i) {
            if (candidates[i].bodyId == kInvalidBodyId ||
                candidates[i].motionId == kInvalidMotionId) {
                return result;
            }
            for (std::size_t previous = 0;
                 previous < i;
                 ++previous) {
                if (candidates[previous].bodyId ==
                    candidates[i].bodyId) {
                    return result;
                }
            }
        }

        for (std::size_t i = 0; i < count; ++i) {
            bool motionAlreadyAssigned = false;
            for (std::size_t previous = 0;
                 previous < i;
                 ++previous) {
                if (candidates[previous].motionId ==
                    candidates[i].motionId) {
                    motionAlreadyAssigned = true;
                    break;
                }
            }
            if (motionAlreadyAssigned) {
                continue;
            }

            std::size_t representative = i;
            for (std::size_t candidate = i + 1;
                 candidate < count;
                 ++candidate) {
                if (candidates[candidate].motionId ==
                        candidates[i].motionId &&
                    RepresentativePriority(candidates[candidate]) >
                        RepresentativePriority(
                            candidates[representative])) {
                    representative = candidate;
                }
            }
            result.representatives[representative] = true;
            ++result.uniqueMotionCount;
        }

        result.valid = result.uniqueMotionCount > 0;
        return result;
    }
}
