#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace heisenberg::pipboy_node_snapshot
{
    /*
     * Fixed-capacity pointer/parent snapshot used to answer repeated subtree
     * membership questions without walking a live scene graph again. The
     * pointed-to objects are never dereferenced here; callers may therefore
     * safely query a pointer that has become dangling since it was cached.
     */
    template <std::size_t Capacity>
    class FixedSubtreeMembership
    {
    public:
        static_assert(Capacity > 0);
        static_assert(
            Capacity <=
            (std::numeric_limits<std::size_t>::max)() / 2);
        static_assert(
            Capacity <=
            static_cast<std::size_t>(
                (std::numeric_limits<int>::max)()));

        void Reset() noexcept
        {
            _size = 0;
            _complete = true;
            AdvanceLookupGeneration();
        }

        [[nodiscard]] int Append(
            const void* object,
            int parentIndex) noexcept
        {
            if (!object) {
                return -1;
            }
            if (_size >= Capacity) {
                _complete = false;
                return -1;
            }

            const int index = static_cast<int>(_size);
            _entries[_size++] = Entry{
                object,
                parentIndex
            };
            InsertFirstIndex(object, index);
            return index;
        }

        void MarkIncomplete() noexcept
        {
            _complete = false;
        }

        [[nodiscard]] bool Contains(
            const void* object) const noexcept
        {
            return Find(object) >= 0;
        }

        [[nodiscard]] bool ContainsWithin(
            const void* root,
            const void* target) const noexcept
        {
            if (!root || !target) {
                return false;
            }

            int index = Find(target);
            std::size_t remaining = _size;
            while (index >= 0 && remaining-- > 0) {
                const Entry& entry =
                    _entries[static_cast<std::size_t>(index)];
                if (entry.object == root) {
                    return true;
                }
                index = entry.parentIndex;
            }
            return false;
        }

        [[nodiscard]] std::size_t Size() const noexcept
        {
            return _size;
        }

        [[nodiscard]] bool Complete() const noexcept
        {
            return _complete;
        }

    private:
        // Keep the index at or below 50% occupancy. A power-of-two slot count
        // makes the bounded linear-probe wrap a mask instead of a division.
        static constexpr std::size_t LOOKUP_CAPACITY =
            std::bit_ceil(Capacity * 2);
        static constexpr std::size_t LOOKUP_MASK =
            LOOKUP_CAPACITY - 1;

        struct Entry
        {
            const void* object = nullptr;
            int parentIndex = -1;
        };

        struct LookupSlot
        {
            const void* object = nullptr;
            std::uint32_t generation = 0;
            int index = -1;
        };

        [[nodiscard]] static std::size_t PointerHash(
            const void* object) noexcept
        {
            std::uintptr_t value =
                reinterpret_cast<std::uintptr_t>(object);
            if constexpr (sizeof(std::uintptr_t) >= 8) {
                value ^= value >> 33;
                value *= static_cast<std::uintptr_t>(
                    UINT64_C(0xff51afd7ed558ccd));
                value ^= value >> 33;
                value *= static_cast<std::uintptr_t>(
                    UINT64_C(0xc4ceb9fe1a85ec53));
                value ^= value >> 33;
            } else {
                value ^= value >> 16;
                value *= static_cast<std::uintptr_t>(
                    UINT32_C(0x7feb352d));
                value ^= value >> 15;
                value *= static_cast<std::uintptr_t>(
                    UINT32_C(0x846ca68b));
                value ^= value >> 16;
            }
            return static_cast<std::size_t>(value);
        }

        void AdvanceLookupGeneration() noexcept
        {
            ++_lookupGeneration;
            if (_lookupGeneration != 0) {
                return;
            }

            // Generation zero means "never occupied". This exceptional sweep
            // is reached only after 2^32 resets; ordinary phase changes remain
            // O(1) and never clear the fixed table.
            for (auto& slot : _lookup) {
                slot.generation = 0;
            }
            _lookupGeneration = 1;
        }

        void InsertFirstIndex(
            const void* object,
            const int index) noexcept
        {
            std::size_t slotIndex =
                PointerHash(object) & LOOKUP_MASK;
            for (std::size_t probe = 0;
                 probe < LOOKUP_CAPACITY;
                 ++probe) {
                LookupSlot& slot = _lookup[slotIndex];
                if (slot.generation != _lookupGeneration) {
                    slot.object = object;
                    slot.index = index;
                    slot.generation = _lookupGeneration;
                    return;
                }
                if (slot.object == object) {
                    // The old linear Find returned the first duplicate. Keep
                    // that observable behavior while still storing every
                    // appended entry for parent-chain traversal.
                    return;
                }
                slotIndex = (slotIndex + 1) & LOOKUP_MASK;
            }
        }

        [[nodiscard]] int Find(
            const void* object) const noexcept
        {
            if (!object) {
                return -1;
            }

            std::size_t slotIndex =
                PointerHash(object) & LOOKUP_MASK;
            for (std::size_t probe = 0;
                 probe < LOOKUP_CAPACITY;
                 ++probe) {
                const LookupSlot& slot = _lookup[slotIndex];
                if (slot.generation != _lookupGeneration) {
                    return -1;
                }
                if (slot.object == object) {
                    return slot.index;
                }
                slotIndex = (slotIndex + 1) & LOOKUP_MASK;
            }
            return -1;
        }

        std::array<Entry, Capacity> _entries{};
        std::array<LookupSlot, LOOKUP_CAPACITY> _lookup{};
        std::size_t _size = 0;
        std::uint32_t _lookupGeneration = 1;
        bool _complete = true;
    };
}
