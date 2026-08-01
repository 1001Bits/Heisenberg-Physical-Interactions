#pragma once

#include <cstdint>

namespace rock::weapon_interaction_probe_frame_policy
{
    enum class Source : std::uint8_t
    {
        RootFlattened,
        ScopeSafe,
        Unavailable,
    };

    /*
     * ScopeMenu deliberately collapses hFRIK's root hand bones. A cached
     * root/proxy point is therefore never a valid fallback for scoped weapon
     * acquisition: either use the prepared driver-reconstructed frame shared
     * with TwoHandedGrip::capturePartGrip, or defer capture. Outside ScopeMenu
     * the existing root/proxy path remains authoritative.
     */
    [[nodiscard]] inline constexpr Source select(
        const bool scopeMenuOpen,
        const bool scopeSafeFrameAvailable) noexcept
    {
        if (!scopeMenuOpen) {
            return Source::RootFlattened;
        }
        return scopeSafeFrameAvailable ?
                   Source::ScopeSafe :
                   Source::Unavailable;
    }
}
