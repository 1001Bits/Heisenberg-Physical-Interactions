#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rock::external_held_body_registry
{
    /*
     * Exact host-held body identity shared from the game thread to the Havok
     * character-controller callback. Body IDs are scoped to one hknpWorld, so
     * both values participate in every lookup.
     *
     * Each hand uses two atomic payload slots. The single game-thread writer
     * fills the inactive slot completely and only then publishes its index.
     * A physics-thread reader therefore always has a last-complete snapshot
     * available even if the writer is pre-empted halfway through the next one.
     * Per-slot sequences protect a reader that still holds the formerly active
     * slot when a later publication starts reusing it.
     */
    template <std::size_t Capacity>
    class Registry
    {
    public:
        static constexpr std::size_t kHandCount = 2;

        void publish(
            const bool isLeft,
            const void* const world,
            const std::uint32_t* const bodyIds,
            const std::size_t count) noexcept
        {
            auto& hand = _hands[isLeft ? 1u : 0u];
            const std::uint32_t active =
                hand.activeSlot.load(
                    std::memory_order_acquire) &
                1u;
            const std::uint32_t publishSlot = active ^ 1u;
            auto& slot = hand.slots[publishSlot];
            slot.sequence.fetch_add(
                1u,
                std::memory_order_acq_rel);

            std::size_t storedCount = 0u;
            if (world && bodyIds) {
                for (std::size_t i = 0;
                     i < count && storedCount < Capacity;
                     ++i) {
                    const std::uint32_t bodyId = bodyIds[i];
                    if (bodyId == 0x7FFFFFFFu ||
                        bodyId == 0xFFFFFFFFu) {
                        continue;
                    }
                    bool duplicate = false;
                    for (std::size_t prior = 0;
                         prior < storedCount;
                         ++prior) {
                        if (slot.bodyIds[prior].load(
                                std::memory_order_relaxed) ==
                            bodyId) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        slot.bodyIds[storedCount++].store(
                            bodyId,
                            std::memory_order_relaxed);
                    }
                }
            }
            slot.world.store(
                storedCount > 0u ? world : nullptr,
                std::memory_order_relaxed);
            slot.count.store(
                static_cast<std::uint32_t>(storedCount),
                std::memory_order_relaxed);

            slot.sequence.fetch_add(
                1u,
                std::memory_order_release);
            hand.activeSlot.store(
                publishSlot,
                std::memory_order_release);
        }

        void clear(const bool isLeft) noexcept
        {
            publish(isLeft, nullptr, nullptr, 0u);
        }

        [[nodiscard]] bool contains(
            const void* const world,
            const std::uint32_t bodyId) const noexcept
        {
            return ownerMask(world, bodyId) != 0u;
        }

        // Bit 0 = right hand, bit 1 = left hand. During an atomic handover both
        // bits may be present briefly; callers must treat either bit as exact
        // held-object identity rather than reclassifying layer 33 as an actor.
        [[nodiscard]] std::uint8_t ownerMask(
            const void* const world,
            const std::uint32_t bodyId) const noexcept
        {
            if (!world) {
                return 0u;
            }
            std::uint8_t result = 0u;
            if (handContains(_hands[0], world, bodyId)) {
                result |= 1u;
            }
            if (handContains(_hands[1], world, bodyId)) {
                result |= 2u;
            }
            return result;
        }

        [[nodiscard]] bool any() const noexcept
        {
            return handHasEntries(_hands[0]) ||
                   handHasEntries(_hands[1]);
        }

    private:
        struct HandSnapshot
        {
            struct Slot
            {
                std::atomic<std::uint64_t> sequence{ 0u };
                std::atomic<const void*> world{ nullptr };
                std::atomic<std::uint32_t> count{ 0u };
                std::array<
                    std::atomic<std::uint32_t>,
                    Capacity>
                    bodyIds{};
            };

            std::array<Slot, 2> slots{};
            std::atomic<std::uint32_t> activeSlot{ 0u };
        };

        [[nodiscard]] static bool handContains(
            const HandSnapshot& hand,
            const void* const world,
            const std::uint32_t bodyId) noexcept
        {
            for (;;) {
                const std::uint32_t active =
                    hand.activeSlot.load(
                        std::memory_order_acquire) &
                    1u;
                const auto& slot = hand.slots[active];
                const std::uint64_t sequenceBefore =
                    slot.sequence.load(std::memory_order_acquire);
                if ((sequenceBefore & 1u) != 0u) {
                    continue;
                }

                const void* const publishedWorld =
                    slot.world.load(std::memory_order_relaxed);
                const std::uint32_t count =
                    slot.count.load(std::memory_order_relaxed);
                bool found =
                    publishedWorld == world &&
                    count <= Capacity;
                if (found) {
                    found = false;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        if (slot.bodyIds[i].load(
                                std::memory_order_relaxed) ==
                            bodyId) {
                            found = true;
                            break;
                        }
                    }
                }

                const std::uint64_t sequenceAfter =
                    slot.sequence.load(std::memory_order_acquire);
                if (sequenceBefore == sequenceAfter &&
                    (sequenceAfter & 1u) == 0u) {
                    return found;
                }
            }
        }

        [[nodiscard]] static bool handHasEntries(
            const HandSnapshot& hand) noexcept
        {
            for (;;) {
                const std::uint32_t active =
                    hand.activeSlot.load(
                        std::memory_order_acquire) &
                    1u;
                const auto& slot = hand.slots[active];
                const std::uint64_t sequenceBefore =
                    slot.sequence.load(std::memory_order_acquire);
                if ((sequenceBefore & 1u) != 0u) {
                    continue;
                }
                const bool populated =
                    slot.world.load(std::memory_order_relaxed) !=
                        nullptr &&
                    slot.count.load(std::memory_order_relaxed) >
                        0u;
                const std::uint64_t sequenceAfter =
                    slot.sequence.load(std::memory_order_acquire);
                if (sequenceBefore == sequenceAfter &&
                    (sequenceAfter & 1u) == 0u) {
                    return populated;
                }
            }
        }

        std::array<HandSnapshot, kHandCount> _hands{};
    };
}
