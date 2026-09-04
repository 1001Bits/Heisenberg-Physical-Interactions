#pragma once

#include <bit>
#include <cstdint>

namespace rock::physics_shape_cast_cache_policy
{
    struct QuantizedBoxKey
    {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 0;

        bool operator==(const QuantizedBoxKey&) const = default;
    };

    [[nodiscard]] inline constexpr QuantizedBoxKey logarithmicOverflowKey(
        const QuantizedBoxKey& requested) noexcept
    {
        const auto roundAxis = [](const std::uint32_t value) {
            return value == 0 ? std::uint32_t{ 0 } : std::bit_ceil(value);
        };
        return QuantizedBoxKey{
            roundAxis(requested.x),
            roundAxis(requested.y),
            roundAxis(requested.z),
        };
    }
}
