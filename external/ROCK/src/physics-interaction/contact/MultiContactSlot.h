#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace rock::contact_pipeline_policy
{
    /*
     * Captures up to 4 distinct (sourceBodyId, targetBodyId) contact pairs recorded
     * within a single frame, instead of a single last-write-wins slot.
     *
     * handleContactEvent can fire many times per frame - once per raw Havok contact,
     * across multiple sub-colliders (fingers, weapon sub-bodies) - from Havok's own
     * callback thread, potentially concurrently (see s_inFlightCallbacks). A single
     * atomic slot silently loses every contact but the last one written that frame:
     * a genuine, deliberate push can lose that race against an unrelated simultaneous
     * contact (another finger, another weapon sub-body touching something) and never
     * get evaluated at all, with no log line to show it happened. This round-robins
     * across 4 independently-atomic slots so up to 4 simultaneous contacts per source
     * all survive to be drained and evaluated by resolveContacts once per frame, not
     * just whichever happened to be the final write.
     *
     * Each slot packs both IDs into one uint64_t so a slot is always read as a
     * self-consistent (source, target) pair - the two-separate-atomics design this
     * replaces could race and be read back as a mismatched pair assembled from two
     * different contacts (source from one, target from another).
     */
    class MultiContactSlot4
    {
    public:
        void record(std::uint32_t sourceBodyId, std::uint32_t targetBodyId) noexcept
        {
            const auto index = _nextIndex.fetch_add(1, std::memory_order_relaxed) % kCapacity;
            _slots[index].store(pack(sourceBodyId, targetBodyId), std::memory_order_release);
            _hasPending.store(true, std::memory_order_release);
        }

        // Discards every pending contact without evaluating it - for lifecycle
        // resets (shutdown/reinit), not the normal per-frame path (drainEach).
        void clear() noexcept
        {
            for (auto& slot : _slots) {
                slot.store(kSentinel, std::memory_order_release);
            }
            _hasPending.store(false, std::memory_order_release);
        }

        // Drains every slot holding a real contact, invoking fn(sourceBodyId,
        // targetBodyId) for each. Call once per frame from the consuming thread;
        // safe to call while writers are concurrently recording into other slots.
        template <class Fn>
        void drainEach(Fn&& fn)
        {
            if (!_hasPending.load(std::memory_order_acquire)) {
                return;
            }
            // Clearing the hint before draining makes a concurrent writer
            // publish true after its slot write. The current drain either
            // consumes that contact or the next frame observes the hint.
            _hasPending.exchange(false, std::memory_order_acq_rel);
            for (auto& slot : _slots) {
                const auto packed = slot.exchange(kSentinel, std::memory_order_acq_rel);
                if (packed == kSentinel) {
                    continue;
                }
                fn(static_cast<std::uint32_t>(packed >> 32), static_cast<std::uint32_t>(packed & 0xFFFF'FFFFull));
            }
        }

    private:
        static constexpr std::size_t kCapacity = 4;
        static constexpr std::uint64_t kSentinel = 0xFFFF'FFFF'FFFF'FFFFull;

        static constexpr std::uint64_t pack(std::uint32_t sourceBodyId, std::uint32_t targetBodyId) noexcept
        {
            return (static_cast<std::uint64_t>(sourceBodyId) << 32) | static_cast<std::uint64_t>(targetBodyId);
        }

        std::array<std::atomic<std::uint64_t>, kCapacity> _slots{ kSentinel, kSentinel, kSentinel, kSentinel };
        std::atomic<std::uint32_t> _nextIndex{ 0 };
        std::atomic<bool> _hasPending{ false };
    };
}
