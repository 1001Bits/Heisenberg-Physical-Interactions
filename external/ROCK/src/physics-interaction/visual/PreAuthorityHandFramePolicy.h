#pragma once

#include <cstdint>

namespace rock::pre_authority_hand_frame_policy
{
    /*
     * The host publishes two kinds of same-frame hand snapshots through the
     * pre-authority seam. CleanPass1 is captured inside FRIK before any
     * external hand authority is consumed. SkinnedFallback is still useful to
     * weapon compatibility consumers, but it may already contain a pass-2 or
     * late bone-IK result and must never become hand-collision intent.
     */
    enum class Provenance : std::uint8_t
    {
        Unavailable = 0,
        SkinnedFallback,
        CleanPass1,
    };

    [[nodiscard]] inline constexpr bool isAvailable(
        const Provenance provenance) noexcept
    {
        return provenance != Provenance::Unavailable;
    }

    [[nodiscard]] inline constexpr bool isTrueCleanPass1(
        const Provenance provenance) noexcept
    {
        return provenance == Provenance::CleanPass1;
    }
}
