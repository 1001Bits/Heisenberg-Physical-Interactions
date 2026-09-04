#pragma once

#include <cstdint>
#include <string_view>

namespace rock::viewcaster_selection_policy
{
    enum class CandidateSource : std::uint8_t
    {
        None,
        RockNear,
        HostViewCaster,
        RockFar,
    };

    // In embedded mode Fallout's ViewCaster is the user's explicit target.
    // Once ROCK validates that exact reference it must beat incidental ROCK
    // proximity candidates; ordinary ROCK casts remain fallbacks only.
    [[nodiscard]] inline constexpr CandidateSource chooseCandidateSource(
        const bool rockNearValid,
        const bool hostViewCasterValid,
        const bool rockFarValid,
        const bool rockNearMatchesHost = false) noexcept
    {
        // When both systems identify the same reference, retain ROCK's close
        // collision hit and palm-seat geometry. Treating that object as a far
        // ray target loses the local grip point and seats it incorrectly.
        if (rockNearValid && hostViewCasterValid &&
            rockNearMatchesHost) {
            return CandidateSource::RockNear;
        }
        if (hostViewCasterValid) {
            return CandidateSource::HostViewCaster;
        }
        if (rockNearValid) {
            return CandidateSource::RockNear;
        }
        if (rockFarValid) {
            return CandidateSource::RockFar;
        }
        return CandidateSource::None;
    }

    [[nodiscard]] inline constexpr bool shouldRunTargetedFallback(
        const bool hostTargetPublished,
        const bool rockNearValid,
        const bool normalFarMatchesHostTarget,
        const bool targetCanBeQueried) noexcept
    {
        return hostTargetPublished &&
               !normalFarMatchesHostTarget && targetCanBeQueried;
    }

    // A target-directed sphere cast can encounter several bodies. Only the
    // exact published reference may be imported; every candidate must first
    // survive ROCK's normal collision, classification, ownership, cone,
    // blacklist, range, and occlusion checks.
    [[nodiscard]] inline constexpr bool acceptsImportedCandidate(
        const bool hostTargetPublished,
        const bool candidateValid,
        const bool exactReferenceMatch) noexcept
    {
        return hostTargetPublished && candidateValid && exactReferenceMatch;
    }

    [[nodiscard]] inline constexpr bool isSupportedLoosePickupFormType(
        const std::string_view formType) noexcept
    {
        return formType == "MISC" || formType == "WEAP" ||
               formType == "AMMO" || formType == "ALCH" ||
               formType == "BOOK" || formType == "KEYM" ||
               formType == "NOTE" || formType == "ARMO" ||
               formType == "FLOR" || formType == "ACTI" ||
               formType == "INGR" || formType == "CMPO" ||
               formType == "LIGH";
    }
}
