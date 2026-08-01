#pragma once

#include <cstddef>
#include <cstring>

namespace rock::character_controller_layout
{
    // Ghidra-verified FO4VR layout. In particular, the VR character-controller
    // pointer is eight bytes later than CommonLibF4VR's flat-FO4 declaration.
    inline constexpr std::size_t kActorCurrentProcessOffset = 0x300;
    inline constexpr std::size_t kProcessMiddleHighOffset = 0x08;
    inline constexpr std::size_t kVrCharacterControllerOffset = 0x3E8;
    inline constexpr std::size_t kFlatCharacterControllerOffset = 0x3E0;
    inline constexpr std::size_t kCharacterControllerLinearVelocityOffset = 0x250;

    // This policy is intentionally independent of CommonLib structs so the
    // FO4VR offsets can be regression-tested on the host. Runtime callers must
    // wrap it in SEH because native process pointers can disappear asynchronously.
    [[nodiscard]] inline void* readPointerUnchecked(
        const void* base,
        const std::size_t offset) noexcept
    {
        if (!base) {
            return nullptr;
        }

        void* value = nullptr;
        std::memcpy(
            &value,
            static_cast<const std::byte*>(base) + offset,
            sizeof(value));
        return value;
    }

    [[nodiscard]] inline void* resolveActorCharacterControllerUnchecked(
        const void* actor) noexcept
    {
        const void* const currentProcess =
            readPointerUnchecked(actor, kActorCurrentProcessOffset);
        const void* const middleHigh =
            readPointerUnchecked(currentProcess, kProcessMiddleHighOffset);
        return readPointerUnchecked(middleHigh, kVrCharacterControllerOffset);
    }
}
