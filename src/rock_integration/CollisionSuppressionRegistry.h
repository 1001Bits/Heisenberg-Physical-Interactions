#pragma once

// Heisenberg port of ROCK's collision_suppression_registry (May 2026).
// Source: ROCK-main/src/physics-interaction/collision/CollisionSuppressionRegistry.{h,cpp}
// Logic preserved verbatim. Only changes: namespace renamed `heisenberg::rock_csr`,
// Physics:: wrappers used for filter-info R/W (HavokRuntime equivalents), spdlog used
// for diagnostics.
//
// Hand/weapon collision suppression is a shared ownership problem, not a local toggle.
// Grab, dominant-weapon handling, and support grip can all suppress the same hand body
// in the same frame. This registry models suppression as **leases**: the first owner
// captures the original filter, later owners join the lease, and the filter state is
// restored only after the last owner releases. Suppression is filter-only by design
// because FO4VR's native body deactivation table is not guaranteed for ROCK-generated
// bodies.
//
// Gated by Config::rockCollisionSuppressionRegistry (bRockCollisionSuppressionRegistry
// in [RockIntegration] section).

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace heisenberg::rock_csr
{
    inline constexpr std::uint32_t kInvalidBodyId = 0x7FFF'FFFFu;
    inline constexpr std::uint32_t kSuppressionNoCollideBit = 1u << 14;  // ROCK match

    enum class Owner : std::uint8_t
    {
        Grab = 0,
        WeaponDominantHand = 1,
        WeaponSupportHand = 2,
    };

    inline constexpr std::uint32_t ownerBit(Owner o) { return 1u << static_cast<std::uint32_t>(o); }

    struct LeaseResult
    {
        bool valid = false;
        bool firstLeaseForBody = false;
        bool bodyFullyReleased = false;
        bool filterChanged = false;
        bool ownerAlreadyHeld = false;
        bool wasNoCollideBeforeSuppression = false;
        std::uint32_t bodyId = kInvalidBodyId;
        std::uint32_t filterBefore = 0;
        std::uint32_t filterAfter = 0;
        std::uint32_t activeLeaseCount = 0;
    };

    struct RuntimeResult : LeaseResult
    {
        bool readFailed = false;
    };

    // Pure (testable, no engine calls) — same algorithm ROCK uses for unit tests.
    class PureRegistry
    {
    public:
        LeaseResult acquire(std::uint32_t bodyId, Owner owner, std::uint32_t currentFilter);
        LeaseResult release(std::uint32_t bodyId, Owner owner, std::uint32_t currentFilter);
        std::uint32_t activeLeaseCount(std::uint32_t bodyId) const;
        bool hasBody(std::uint32_t bodyId) const { return find(bodyId) != nullptr; }
        void clear() { _entries.clear(); }

    private:
        struct Entry
        {
            std::uint32_t bodyId = kInvalidBodyId;
            std::uint32_t originalFilter = 0;
            std::uint32_t ownerMask = 0;
            bool wasNoCollideBeforeSuppression = false;
        };

        Entry* find(std::uint32_t bodyId);
        const Entry* find(std::uint32_t bodyId) const;
        void erase(std::uint32_t bodyId);

        std::vector<Entry> _entries;
    };

    // Runtime — wraps PureRegistry + reads/writes the body's filterInfo via Heisenberg's
    // Physics:: helpers (which are SEH-protected and use the same engine functions ROCK
    // uses via havok_runtime::setFilterInfo internally).
    class Registry
    {
    public:
        RuntimeResult acquire(void* hknpWorld, std::uint32_t bodyId, Owner owner, const char* context);
        RuntimeResult release(void* hknpWorld, std::uint32_t bodyId, Owner owner, const char* context);
        void releaseOwner(void* hknpWorld, Owner owner, const char* context);
        bool hasLease(std::uint32_t bodyId, Owner owner) const;
        void clear();

    private:
        struct Entry
        {
            std::uint32_t bodyId = kInvalidBodyId;
            std::uint32_t originalFilter = 0;
            std::uint32_t ownerMask = 0;
            bool wasNoCollideBeforeSuppression = false;
        };
        Entry* find(std::uint32_t bodyId);
        const Entry* find(std::uint32_t bodyId) const;
        void erase(std::uint32_t bodyId);

        std::vector<Entry> _entries;
        // globalRegistry() can be touched by Grab (game thread) and, in future, by
        // weapon owners from other contexts. push_back invalidates the &_entries.back()
        // pointers used internally, so all public mutators serialize on this mutex.
        mutable std::mutex _mutex;
    };

    Registry& globalRegistry();
}
