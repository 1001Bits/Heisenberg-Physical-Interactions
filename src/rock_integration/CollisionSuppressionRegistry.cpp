#include "CollisionSuppressionRegistry.h"
#include "../Physics.h"

#include <spdlog/spdlog.h>

namespace heisenberg::rock_csr
{
    namespace
    {
        std::uint32_t leaseCount(std::uint32_t ownerMask)
        {
            std::uint32_t count = 0;
            while (ownerMask != 0) {
                count += ownerMask & 1u;
                ownerMask >>= 1u;
            }
            return count;
        }

        const char* ownerName(Owner o)
        {
            switch (o) {
            case Owner::Grab: return "Grab";
            case Owner::WeaponDominantHand: return "WeaponDominantHand";
            case Owner::WeaponSupportHand: return "WeaponSupportHand";
            }
            return "Unknown";
        }
    }

    // ---- PureRegistry ----------------------------------------------------

    PureRegistry::Entry* PureRegistry::find(std::uint32_t bodyId)
    {
        const auto it = std::find_if(_entries.begin(), _entries.end(),
            [&](const Entry& e) { return e.bodyId == bodyId; });
        return it != _entries.end() ? &*it : nullptr;
    }

    const PureRegistry::Entry* PureRegistry::find(std::uint32_t bodyId) const
    {
        const auto it = std::find_if(_entries.begin(), _entries.end(),
            [&](const Entry& e) { return e.bodyId == bodyId; });
        return it != _entries.end() ? &*it : nullptr;
    }

    void PureRegistry::erase(std::uint32_t bodyId)
    {
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
            [&](const Entry& e) { return e.bodyId == bodyId; }), _entries.end());
    }

    std::uint32_t PureRegistry::activeLeaseCount(std::uint32_t bodyId) const
    {
        const auto* e = find(bodyId);
        if (!e) return 0;
        return leaseCount(e->ownerMask);
    }

    LeaseResult PureRegistry::acquire(std::uint32_t bodyId, Owner owner, std::uint32_t currentFilter)
    {
        LeaseResult result{};
        result.bodyId = bodyId;
        result.filterBefore = currentFilter;
        result.filterAfter = currentFilter | kSuppressionNoCollideBit;
        result.filterChanged = result.filterAfter != currentFilter;
        if (bodyId == kInvalidBodyId) return result;

        const std::uint32_t bit = ownerBit(owner);
        auto* entry = find(bodyId);
        if (!entry) {
            Entry ne{};
            ne.bodyId = bodyId;
            ne.originalFilter = currentFilter;
            ne.wasNoCollideBeforeSuppression = (currentFilter & kSuppressionNoCollideBit) != 0;
            ne.ownerMask = bit;
            _entries.push_back(ne);
            entry = &_entries.back();
            result.firstLeaseForBody = true;
        } else if ((entry->ownerMask & bit) != 0) {
            result.valid = true;
            result.ownerAlreadyHeld = true;
            result.filterAfter = currentFilter;
            result.filterChanged = false;
            result.wasNoCollideBeforeSuppression = entry->wasNoCollideBeforeSuppression;
            result.activeLeaseCount = activeLeaseCount(bodyId);
            return result;
        } else {
            entry->ownerMask |= bit;
        }

        result.valid = true;
        result.wasNoCollideBeforeSuppression = entry->wasNoCollideBeforeSuppression;
        result.activeLeaseCount = activeLeaseCount(bodyId);
        return result;
    }

    LeaseResult PureRegistry::release(std::uint32_t bodyId, Owner owner, std::uint32_t currentFilter)
    {
        LeaseResult result{};
        result.bodyId = bodyId;
        result.filterBefore = currentFilter;
        result.filterAfter = currentFilter;
        auto* entry = find(bodyId);
        if (!entry) return result;

        entry->ownerMask &= ~ownerBit(owner);
        result.valid = true;
        result.wasNoCollideBeforeSuppression = entry->wasNoCollideBeforeSuppression;
        result.activeLeaseCount = activeLeaseCount(bodyId);
        if (entry->ownerMask != 0) {
            result.filterAfter = currentFilter | kSuppressionNoCollideBit;
            result.filterChanged = result.filterAfter != currentFilter;
            return result;
        }

        result.bodyFullyReleased = true;
        // Restore the captured original filter (see Registry::release for rationale).
        result.filterAfter = entry->originalFilter;
        result.filterChanged = result.filterAfter != currentFilter;
        erase(bodyId);
        return result;
    }

    // ---- Registry (runtime wrapper that does the engine R/W) -------------

    Registry::Entry* Registry::find(std::uint32_t bodyId)
    {
        const auto it = std::find_if(_entries.begin(), _entries.end(),
            [&](const Entry& e) { return e.bodyId == bodyId; });
        return it != _entries.end() ? &*it : nullptr;
    }

    const Registry::Entry* Registry::find(std::uint32_t bodyId) const
    {
        const auto it = std::find_if(_entries.begin(), _entries.end(),
            [&](const Entry& e) { return e.bodyId == bodyId; });
        return it != _entries.end() ? &*it : nullptr;
    }

    void Registry::erase(std::uint32_t bodyId)
    {
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
            [&](const Entry& e) { return e.bodyId == bodyId; }), _entries.end());
    }

    RuntimeResult Registry::acquire(void* hknpWorld, std::uint32_t bodyId, Owner owner, const char* context)
    {
        std::scoped_lock lock(_mutex);
        RuntimeResult result{};
        result.bodyId = bodyId;
        if (!hknpWorld || bodyId == kInvalidBodyId) {
            result.readFailed = true;
            return result;
        }

        const std::uint32_t ownerMask = ownerBit(owner);
        auto* entry = find(bodyId);
        if (entry && (entry->ownerMask & ownerMask) != 0) {
            result.valid = true;
            result.ownerAlreadyHeld = true;
            result.wasNoCollideBeforeSuppression = entry->wasNoCollideBeforeSuppression;
            result.activeLeaseCount = leaseCount(entry->ownerMask);
            result.filterBefore = entry->originalFilter;
            result.filterAfter = entry->originalFilter;
            return result;
        }

        std::uint32_t currentFilter = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, bodyId, currentFilter)) {
            result.readFailed = true;
            spdlog::warn("[CSR] acquire failed: owner={} bodyId=0x{:08X} ctx={} (cannot read filter)",
                         ownerName(owner), bodyId, context ? context : "");
            return result;
        }

        if (!entry) {
            Entry ne{};
            ne.bodyId = bodyId;
            ne.originalFilter = currentFilter;
            ne.wasNoCollideBeforeSuppression = (currentFilter & kSuppressionNoCollideBit) != 0;
            ne.ownerMask = ownerMask;
            _entries.push_back(ne);
            entry = &_entries.back();
            result.firstLeaseForBody = true;
        } else {
            entry->ownerMask |= ownerMask;
        }

        const std::uint32_t disabledFilter = currentFilter | kSuppressionNoCollideBit;
        if (disabledFilter != currentFilter) {
            heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, bodyId, disabledFilter);
        }

        result.valid = true;
        result.filterBefore = currentFilter;
        result.filterAfter = disabledFilter;
        result.filterChanged = disabledFilter != currentFilter;
        result.wasNoCollideBeforeSuppression = entry->wasNoCollideBeforeSuppression;
        result.activeLeaseCount = leaseCount(entry->ownerMask);

        if (result.firstLeaseForBody || result.filterChanged) {
            spdlog::debug("[CSR] acquired: owner={} bodyId=0x{:08X} ctx={} filter=0x{:08X}->0x{:08X} preDisabled={} leases={}",
                          ownerName(owner), bodyId, context ? context : "",
                          currentFilter, disabledFilter,
                          entry->wasNoCollideBeforeSuppression ? "yes" : "no",
                          result.activeLeaseCount);
        }
        return result;
    }

    RuntimeResult Registry::release(void* hknpWorld, std::uint32_t bodyId, Owner owner, const char* context)
    {
        std::scoped_lock lock(_mutex);
        RuntimeResult result{};
        result.bodyId = bodyId;
        auto* entry = find(bodyId);
        if (!entry) return result;
        if (!hknpWorld) {
            result.readFailed = true;
            return result;
        }

        std::uint32_t currentFilter = 0;
        if (!heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, bodyId, currentFilter)) {
            result.readFailed = true;
            spdlog::warn("[CSR] release deferred: owner={} bodyId=0x{:08X} ctx={} (cannot read filter); lease preserved",
                         ownerName(owner), bodyId, context ? context : "");
            return result;
        }

        entry->ownerMask &= ~ownerBit(owner);
        result.valid = true;
        result.filterBefore = currentFilter;
        result.wasNoCollideBeforeSuppression = entry->wasNoCollideBeforeSuppression;
        result.activeLeaseCount = leaseCount(entry->ownerMask);

        if (entry->ownerMask != 0) {
            const std::uint32_t keptFilter = currentFilter | kSuppressionNoCollideBit;
            if (keptFilter != currentFilter) {
                heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, bodyId, keptFilter);
            }
            result.filterAfter = keptFilter;
            result.filterChanged = keptFilter != currentFilter;
            spdlog::debug("[CSR] owner released: owner={} bodyId=0x{:08X} ctx={} leasesRemaining={} filter=0x{:08X}->0x{:08X}",
                          ownerName(owner), bodyId, context ? context : "",
                          result.activeLeaseCount, currentFilter, keptFilter);
            return result;
        }

        // Restore the body's TRUE pre-suppression filter captured at first acquire,
        // not `currentFilter & ~bit`. The contract (header) is "restore the original
        // filter on full release"; toggling only the suppression bit off the live value
        // would leave behind any other modification (layer locks, group bits) made while
        // leased, corrupting the body. originalFilter already encodes whether it was
        // no-collide beforehand, so wasNoCollideBeforeSuppression is logging-only now.
        const std::uint32_t restoredFilter = entry->originalFilter;
        if (restoredFilter != currentFilter) {
            heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, bodyId, restoredFilter);
        }
        result.bodyFullyReleased = true;
        result.filterAfter = restoredFilter;
        result.filterChanged = restoredFilter != currentFilter;

        spdlog::debug("[CSR] fully released: owner={} bodyId=0x{:08X} ctx={} filter=0x{:08X}->0x{:08X} (restored original) wasNoCollide={}",
                      ownerName(owner), bodyId, context ? context : "",
                      currentFilter, restoredFilter,
                      entry->wasNoCollideBeforeSuppression ? "yes" : "no");

        erase(bodyId);
        return result;
    }

    void Registry::releaseOwner(void* hknpWorld, Owner owner, const char* context)
    {
        // Snapshot the matching body IDs under the lock, then release each one outside
        // the lock — release() locks individually, so holding the lock across the loop
        // would self-deadlock on the non-recursive mutex.
        std::vector<std::uint32_t> bodyIds;
        {
            std::scoped_lock lock(_mutex);
            bodyIds.reserve(_entries.size());
            for (const auto& e : _entries) {
                if ((e.ownerMask & ownerBit(owner)) != 0) {
                    bodyIds.push_back(e.bodyId);
                }
            }
        }
        for (const auto bodyId : bodyIds) {
            release(hknpWorld, bodyId, owner, context);
        }
    }

    bool Registry::hasLease(std::uint32_t bodyId, Owner owner) const
    {
        std::scoped_lock lock(_mutex);
        const auto* e = find(bodyId);
        return e && (e->ownerMask & ownerBit(owner)) != 0;
    }

    void Registry::clear()
    {
        std::scoped_lock lock(_mutex);
        _entries.clear();
    }

    Registry& globalRegistry()
    {
        static Registry r;
        return r;
    }
}
