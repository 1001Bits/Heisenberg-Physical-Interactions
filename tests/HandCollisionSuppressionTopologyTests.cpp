#include "physics-interaction/collision/CollisionSuppressionRegistry.h"
#include "physics-interaction/hand/HandLifecycle.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    namespace suppression =
        rock::hand_collision_suppression_math;
    namespace registry =
        rock::collision_suppression_registry;

    {
        // Runtime reserves two complete banks: one can remain in the retry
        // ledger while the replacement bank is suppressed immediately.
        suppression::SuppressionSet<8> set{};
        for (const auto bodyId : { 10u, 11u, 12u, 13u }) {
            Require(
                suppression::beginSuppression(set, bodyId, 0x1200u)
                    .stored,
                "the initial full hand topology must fit its fixed suppression bank");
        }

        const std::array<std::uint32_t, 4> replacement{
            20u, 21u, 22u, 23u
        };
        std::vector<std::uint32_t> releaseAttempts;
        const auto firstReconcile =
            suppression::reconcileSuppressionTopology(
                set,
                [&](const std::uint32_t bodyId) {
                    for (const auto currentBodyId : replacement) {
                        if (bodyId == currentBodyId) {
                            return true;
                        }
                    }
                    return false;
                },
                [&](const std::uint32_t bodyId) {
                    releaseAttempts.push_back(bodyId);
                    return bodyId != 11u;
                });
        Require(
            firstReconcile.staleCount == 4u &&
                firstReconcile.releasedCount == 3u &&
                firstReconcile.deferredCount == 1u,
            "full-bank replacement must vacate every releasable stale slot and retain a failed restore for retry");
        Require(
            suppression::activeCount(set) == 1u &&
                suppression::findSuppressionState(set, 11u) != nullptr,
            "only the deferred stale body may remain after topology reconciliation");

        for (const auto bodyId : replacement) {
            Require(
                suppression::beginSuppression(set, bodyId, 0x2200u)
                    .stored,
                "a complete replacement topology must remain suppressible while one retired restore is deferred");
        }
        Require(
            suppression::activeCount(set) == replacement.size() + 1u,
            "the retry ledger and current replacement bank must coexist without capacity loss");

        const auto retry =
            suppression::reconcileSuppressionTopology(
                set,
                [&](const std::uint32_t bodyId) {
                    for (const auto currentBodyId : replacement) {
                        if (bodyId == currentBodyId) {
                            return true;
                        }
                    }
                    return false;
                },
                [](std::uint32_t) { return true; });
        Require(
            retry.staleCount == 1u && retry.releasedCount == 1u &&
                retry.deferredCount == 0u &&
                suppression::activeCount(set) == replacement.size(),
            "a later frame must retry and clear the deferred stale lease");
        std::size_t stableReleaseCalls = 0;
        const auto stable =
            suppression::reconcileSuppressionTopology(
                set,
                [&](const std::uint32_t bodyId) {
                    for (const auto currentBodyId : replacement) {
                        if (bodyId == currentBodyId) {
                            return true;
                        }
                    }
                    return false;
                },
                [&](std::uint32_t) {
                    ++stableReleaseCalls;
                    return true;
                });
        Require(
            stable.staleCount == 0u && stableReleaseCalls == 0u &&
                suppression::activeCount(set) == replacement.size(),
            "an unchanged topology must retain every lease without a release/reacquire gap");
    }

    {
        enum class Operation
        {
            AcquireDominant,
            AcquireSupport,
            ReleaseDominant,
            ReleaseSupport,
        };

        registry::PureCollisionSuppressionRegistry leases{};
        constexpr std::uint32_t bodyId = 77u;
        constexpr std::uint32_t originalFilter = 0x1101u;
        std::uint32_t liveFilter = leases.acquire(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponDominantHand,
            originalFilter)
                                       .filterAfter;
        std::vector<Operation> operations;

        const auto acquireDominant = [&] {
            operations.push_back(Operation::AcquireDominant);
            const auto result = leases.acquire(
                bodyId,
                registry::CollisionSuppressionOwner::WeaponDominantHand,
                liveFilter);
            Require(result.valid,
                "dominant suppression acquisition must remain valid during an owner transfer");
            liveFilter = result.filterAfter;
        };
        const auto acquireSupport = [&] {
            operations.push_back(Operation::AcquireSupport);
            const auto result = leases.acquire(
                bodyId,
                registry::CollisionSuppressionOwner::WeaponSupportHand,
                liveFilter);
            Require(result.valid,
                "support suppression acquisition must remain valid during an owner transfer");
            liveFilter = result.filterAfter;
        };
        const auto releaseDominant = [&] {
            operations.push_back(Operation::ReleaseDominant);
            const auto result = leases.release(
                bodyId,
                registry::CollisionSuppressionOwner::WeaponDominantHand,
                liveFilter);
            Require(result.valid,
                "dominant suppression release must remain valid during an owner transfer");
            liveFilter = result.filterAfter;
        };
        const auto releaseSupport = [&] {
            operations.push_back(Operation::ReleaseSupport);
            const auto result = leases.release(
                bodyId,
                registry::CollisionSuppressionOwner::WeaponSupportHand,
                liveFilter);
            Require(result.valid,
                "support suppression release must remain valid during an owner transfer");
            liveFilter = result.filterAfter;
        };

        suppression::reconcileOverlappingSuppressionOwners(
            false,
            true,
            acquireDominant,
            acquireSupport,
            releaseDominant,
            releaseSupport);
        Require(
            operations == std::vector<Operation>{
                              Operation::AcquireSupport,
                              Operation::ReleaseDominant,
                          },
            "dominant-to-part-grip transfer must acquire support before releasing dominant");
        Require(
            (liveFilter & registry::kSuppressionNoCollideBit) != 0,
            "dominant-to-part-grip transfer must never clear the no-collide bit");

        operations.clear();
        suppression::reconcileOverlappingSuppressionOwners(
            true,
            false,
            acquireDominant,
            acquireSupport,
            releaseDominant,
            releaseSupport);
        Require(
            operations == std::vector<Operation>{
                              Operation::AcquireDominant,
                              Operation::ReleaseSupport,
                          },
            "part-grip-to-dominant transfer must acquire dominant before releasing support");
        Require(
            (liveFilter & registry::kSuppressionNoCollideBit) != 0,
            "part-grip-to-dominant transfer must never clear the no-collide bit");

        (void)leases.release(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponDominantHand,
            liveFilter);
    }

    {
        registry::PureCollisionSuppressionRegistry leases{};
        constexpr std::uint32_t bodyId = 88u;
        constexpr std::uint32_t originalFilter = 0x1201u;

        const auto dominant = leases.acquire(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponDominantHand,
            originalFilter);
        Require(
            dominant.valid && dominant.firstLeaseForBody &&
                dominant.filterAfter ==
                    (originalFilter |
                        registry::kSuppressionNoCollideBit),
            "the first attached-hand owner must capture and disable the body filter");

        const auto dominantRefresh = leases.acquire(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponDominantHand,
            originalFilter);
        Require(
            dominantRefresh.valid &&
                dominantRefresh.ownerAlreadyHeld &&
                dominantRefresh.filterChanged &&
                (dominantRefresh.filterAfter &
                    registry::kSuppressionNoCollideBit) != 0,
            "a repeated owner acquire must repair collision-filter drift instead of trusting stale local ownership");

        const auto support = leases.acquire(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponSupportHand,
            dominantRefresh.filterAfter);
        Require(
            support.valid && !support.firstLeaseForBody &&
                support.activeLeaseCount == 2u,
            "dominant and support ownership must coexist on one generated body");

        const auto releaseDominant = leases.release(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponDominantHand,
            support.filterAfter);
        Require(
            releaseDominant.valid &&
                !releaseDominant.bodyFullyReleased &&
                (releaseDominant.filterAfter &
                    registry::kSuppressionNoCollideBit) != 0,
            "releasing either attached-hand owner must preserve suppression for the other owner");

        constexpr std::uint32_t unrelatedRuntimeBit = 1u << 29;
        const auto releaseSupport = leases.release(
            bodyId,
            registry::CollisionSuppressionOwner::WeaponSupportHand,
            releaseDominant.filterAfter | unrelatedRuntimeBit);
        Require(
            releaseSupport.valid && releaseSupport.bodyFullyReleased &&
                (releaseSupport.filterAfter &
                    registry::kSuppressionNoCollideBit) == 0 &&
                (releaseSupport.filterAfter & unrelatedRuntimeBit) != 0,
            "the final release must restore the captured no-collide state without overwriting unrelated live filter changes");

        constexpr std::uint32_t preDisabledBodyId = 89u;
        constexpr std::uint32_t preDisabledFilter =
            0x3300u | registry::kSuppressionNoCollideBit;
        const auto preDisabled = leases.acquire(
            preDisabledBodyId,
            registry::CollisionSuppressionOwner::WeaponSupportHand,
            preDisabledFilter);
        const auto restoredPreDisabled = leases.release(
            preDisabledBodyId,
            registry::CollisionSuppressionOwner::WeaponSupportHand,
            preDisabled.filterAfter);
        Require(
            restoredPreDisabled.bodyFullyReleased &&
                (restoredPreDisabled.filterAfter &
                    registry::kSuppressionNoCollideBit) != 0,
            "a body disabled before the weapon lease must remain disabled after the final release");
    }

    std::cout << "HandCollisionSuppressionTopologyTests passed\n";
    return 0;
}
