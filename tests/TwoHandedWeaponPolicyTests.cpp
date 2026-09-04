#include "physics-interaction/weapon/TwoHandedWeaponPolicy.h"
#include "physics-interaction/weapon/EquippedWeaponDropPolicy.h"
#include "physics-interaction/weapon/NativeScopeReentryPolicy.h"
#include "physics-interaction/weapon/WeaponInteractionProbeFramePolicy.h"
#include "physics-interaction/contact/SoftContactPolicy.h"
#include "physics-interaction/core/PostHostGeneratedDriveFinalizePolicy.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Matrix3
    {
        float entry[3][3]{};
    };

    struct Transform
    {
        Matrix3 rotate{};
        Vec3 translate{};
        float scale = 1.0f;
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(const float lhs, const float rhs, const float epsilon = 0.001f)
    {
        return std::abs(lhs - rhs) <= epsilon;
    }

    Matrix3 Yaw90()
    {
        Matrix3 result{};
        result.entry[0][1] = 1.0f;
        result.entry[1][0] = -1.0f;
        result.entry[2][2] = 1.0f;
        return result;
    }
}

int main()
{
    using rock::equipped_weapon_drop_policy::SourceHand;
    using rock::equipped_weapon_drop_policy::
        resolveEquippedWeaponStashCarryHand;
    Require(
        resolveEquippedWeaponStashCarryHand(
            false, true, true, false, false) == SourceHand::Left,
        "a sole left offhand carry may stash a right-fired equipped weapon");
    Require(
        resolveEquippedWeaponStashCarryHand(
            false, true, false, true, true) == SourceHand::Right,
        "a sole right offhand carry may stash a left-fired equipped weapon");
    Require(
        resolveEquippedWeaponStashCarryHand(
            true, false, false, false, false) == SourceHand::None &&
            resolveEquippedWeaponStashCarryHand(
                true, false, false, false, true) == SourceHand::None,
        "primary-only carry must leave day-to-day shoulder holstering to Virtual Holsters");
    Require(
        resolveEquippedWeaponStashCarryHand(
            false, true, false, true, false) == SourceHand::None &&
            resolveEquippedWeaponStashCarryHand(
                false, true, true, false, true) == SourceHand::None,
        "a sole firing-hand part carry must not claim StorageZone");
    Require(
        resolveEquippedWeaponStashCarryHand(
            false, true, true, true, false) == SourceHand::None &&
            resolveEquippedWeaponStashCarryHand(
                false, true, false, false, false) == SourceHand::None,
        "two carry grips or no carry grip must not arm an equipped-weapon stash");

    using namespace rock::two_handed_weapon_policy;

    {
        using rock::weapon_interaction_probe_frame_policy::Source;
        using rock::weapon_interaction_probe_frame_policy::select;

        Require(
            select(false, false) == Source::RootFlattened,
            "ordinary weapon acquisition must preserve the root/proxy hand "
            "path when no scope-safe frame exists");
        Require(
            select(false, true) == Source::RootFlattened,
            "a cached scope-safe frame must not replace ordinary non-scope "
            "weapon acquisition");
        Require(
            select(true, true) == Source::ScopeSafe,
            "ScopeMenu weapon acquisition must use the prepared "
            "driver-reconstructed hand frame");
        Require(
            select(true, false) == Source::Unavailable,
            "ScopeMenu weapon acquisition must defer instead of probing from "
            "hFRIK's collapsed root");
    }

    {
        using rock::native_scope_reentry_policy::filter;
        using rock::native_scope_reentry_policy::
            shouldExitForSupportGripTransition;

        Require(
            !shouldExitForSupportGripTransition(false, true),
            "acquiring an offhand support grip while scoped must preserve "
            "ScopeMenu");
        Require(
            !shouldExitForSupportGripTransition(true, true),
            "holding an established support grip must not close ScopeMenu");
        Require(
            shouldExitForSupportGripTransition(true, false),
            "releasing an established offhand support grip must close "
            "ScopeMenu");
        Require(
            !shouldExitForSupportGripTransition(false, false),
            "an idle support hand must not issue repeated scope exits");

        const auto unblockedInside = filter(false, true);
        Require(
            unblockedInside.nativeScopeRequested &&
                unblockedInside.bypassNativeApproachFade &&
                !unblockedInside.blockRemainsArmed &&
                !unblockedInside.rearmedThisFrame,
            "normal native scope entry must pass through while unblocked");

        const auto blockedInside = filter(true, true);
        Require(
            !blockedInside.nativeScopeRequested &&
                blockedInside.bypassNativeApproachFade &&
                blockedInside.blockRemainsArmed &&
                !blockedInside.rearmedThisFrame,
            "an armed scope close must consume repeated inside-cone entry "
            "without retaining the native approach blackout");

        const auto firstOutside = filter(true, false);
        Require(
            !firstOutside.nativeScopeRequested &&
                !firstOutside.bypassNativeApproachFade &&
                !firstOutside.blockRemainsArmed &&
                firstOutside.rearmedThisFrame,
            "the first outside-cone sample must rearm without activating");

        const auto laterReentry = filter(
            firstOutside.blockRemainsArmed,
            true);
        Require(
            laterReentry.nativeScopeRequested &&
                laterReentry.bypassNativeApproachFade,
            "scope activation must resume only after leave then re-enter");
    }

    {
        using rock::soft_contact_policy::
            shouldRejectImplausibleFirstWorldWitness;
        using rock::soft_contact_policy::
            shouldDeferNativeWeaponFirstWitnessToSweep;
        using rock::soft_contact_policy::worldContactCorrectionLimit;
        Require(
            Near(
                worldContactCorrectionLimit(
                    false,
                    false,
                    37.0f,
                    18.0f),
                18.0f),
            "fresh wall evidence must retain the acquisition safety cap");
        Require(
            Near(
                worldContactCorrectionLimit(
                    false,
                    false,
                    37.0f,
                    18.0f),
                18.0f),
            "fresh native weapon evidence must retain the acquisition safety cap");
        Require(
            Near(
                worldContactCorrectionLimit(
                    false,
                    true,
                    37.0f,
                    18.0f),
                37.0f),
            "a validated weapon sweep must clear a deep thin-wall crossing in its acquisition frame");
        Require(
            Near(
                worldContactCorrectionLimit(
                    true,
                    true,
                    37.0f,
                    18.0f),
                37.0f),
            "a validated weapon stop plane must provide the full required correction");
        Require(
            Near(
                worldContactCorrectionLimit(
                    true,
                    false,
                    37.0f,
                    18.0f),
                37.0f),
            "a validated hand stop plane must not let locomotion ratchet through at 18 gu");
        Require(
            Near(
                worldContactCorrectionLimit(
                    true,
                    false,
                    9.0f,
                    18.0f),
                18.0f),
            "ordinary cached hand contact must preserve the configured acquisition envelope");

        Require(
            !shouldRejectImplausibleFirstWorldWitness(
                false,
                true,
                37.0f,
                18.0f),
            "a validated fast weapon sweep must survive the 18-gu first-witness guard");
        Require(
            shouldRejectImplausibleFirstWorldWitness(
                false,
                false,
                37.0f,
                18.0f),
            "an equally deep native-only first witness must still be rejected as implausible");
        Require(
            shouldRejectImplausibleFirstWorldWitness(
                false,
                true,
                std::numeric_limits<float>::quiet_NaN(),
                18.0f),
            "validated sweep provenance must never admit a non-finite correction");
        Require(
            shouldDeferNativeWeaponFirstWitnessToSweep(
                true,
                true,
                true,
                false,
                37.0f,
                18.0f),
            "a deep native weapon first witness must not suppress its validated hull sweep");
        Require(
            !shouldDeferNativeWeaponFirstWitnessToSweep(
                true,
                true,
                true,
                true,
                37.0f,
                18.0f) &&
                !shouldDeferNativeWeaponFirstWitnessToSweep(
                    true,
                    true,
                    true,
                    false,
                    9.0f,
                    18.0f),
            "an established episode or shallow native witness must retain native contact authority");
        Require(
            !shouldDeferNativeWeaponFirstWitnessToSweep(
                false,
                true,
                true,
                false,
                37.0f,
                18.0f),
            "without a weapon sweep fallback, the native candidate must remain available to the caller's fail-closed guard");
    }

    {
        using namespace
            rock::post_host_generated_drive_finalize_policy;

        const Identity queued{
            .gameFrameIndex = 90,
            .weaponGenerationKey = 0xABCDu,
            .worldGeneration = 2,
            .skeletonGeneration = 3,
            .providerGeneration = 4,
            .collisionGeneration = 5,
            .bhkWorld = 0x1000u,
            .hknpWorld = 0x2000u,
            .weaponNode = 0x3000u,
        };
        Require(
            isCurrent(true, true, queued, queued),
            "the same post-host frame/world/weapon/lifecycle record must be admitted");
        Require(
            !isCurrent(false, true, queued, queued) &&
                !isCurrent(true, false, queued, queued),
            "a consumed token or shut-down runtime must reject finalization");

        const auto RequireMismatchRejected =
            [&](Identity current, const char* message) {
                Require(!isCurrent(true, true, queued, current), message);
            };
        auto current = queued;
        ++current.gameFrameIndex;
        RequireMismatchRejected(current, "a stale game frame must reject the collider queue");
        current = queued;
        ++current.worldGeneration;
        RequireMismatchRejected(current, "a changed world generation must reject the collider queue");
        current = queued;
        ++current.skeletonGeneration;
        RequireMismatchRejected(current, "a changed skeleton generation must reject the collider queue");
        current = queued;
        ++current.providerGeneration;
        RequireMismatchRejected(current, "a changed provider generation must reject the collider queue");
        current = queued;
        ++current.collisionGeneration;
        RequireMismatchRejected(current, "a changed collision generation must reject the collider queue");
        current = queued;
        current.bhkWorld = 0x1001u;
        RequireMismatchRejected(current, "a changed bhk world must reject the collider queue");
        current = queued;
        current.hknpWorld = 0x2001u;
        RequireMismatchRejected(current, "a changed hknp world must reject the collider queue");
        current = queued;
        ++current.weaponGenerationKey;
        RequireMismatchRejected(current, "a changed weapon generation must reject the collider queue");
        current = queued;
        current.weaponNode = 0x3001u;
        RequireMismatchRejected(current, "a changed weapon node must reject the collider queue");
        current = queued;
        current.bhkWorld = 0;
        RequireMismatchRejected(current, "a missing live world must reject the collider queue");
    }

    {
        using namespace rock::soft_contact_policy;

        Require(
            kExactWeaponWorldProbeStateIndex ==
                kSampledWeaponWorldProbeCapacity,
            "the primary exact weapon anchor must use a state slot outside the sampled hull bank");
        Require(
            kSecondaryExactWeaponWorldProbeStateIndex ==
                kSampledWeaponWorldProbeCapacity + 1,
            "the secondary exact weapon anchor must have its own non-aliasing state slot");
        Require(
            kWeaponWorldProbeStateCapacity ==
                kSampledWeaponWorldProbeCapacity + 2,
            "weapon probe state must remain bounded to samples plus two exact corner anchors");
        Require(
            !isSampledWeaponWorldProbeId(
                kExactWeaponWorldProbeId) &&
                !isSampledWeaponWorldProbeId(
                    kSecondaryExactWeaponWorldProbeId) &&
                isExactWeaponWorldProbeId(
                    kExactWeaponWorldProbeId) &&
                isExactWeaponWorldProbeId(
                    kSecondaryExactWeaponWorldProbeId),
            "both exact native anchor IDs must remain distinct from sampled hull IDs");
        Require(
            kCriticalWeaponWorldProbeCapacity == 6,
            "weapon CCD must reserve the six axis extrema, including the asset-axis-independent muzzle/breech pair");
        CriticalWeaponExtremaSelection<std::size_t>
            extremaSelection{};
        observeCriticalWeaponExtrema(
            extremaSelection,
            Vec3{ 0.0f, 0.0f, 0.0f },
            std::size_t{ 0 });
        observeCriticalWeaponExtrema(
            extremaSelection,
            Vec3{ -5.0f, 1.0f, 2.0f },
            std::size_t{ 1 });
        observeCriticalWeaponExtrema(
            extremaSelection,
            Vec3{ 6.0f, -4.0f, 3.0f },
            std::size_t{ 2 });
        observeCriticalWeaponExtrema(
            extremaSelection,
            Vec3{ 2.0f, 7.0f, -8.0f },
            std::size_t{ 3 });
        observeCriticalWeaponExtrema(
            extremaSelection,
            Vec3{
                std::numeric_limits<float>::quiet_NaN(),
                -100.0f,
                100.0f },
            std::size_t{ 4 });
        Require(
            extremaSelection.valid[0] &&
                extremaSelection.valid[1] &&
                extremaSelection.valid[2] &&
                extremaSelection.valid[3] &&
                extremaSelection.valid[4] &&
                extremaSelection.valid[5] &&
                extremaSelection.references[0] == 1u &&
                extremaSelection.references[1] == 2u &&
                extremaSelection.references[2] == 2u &&
                extremaSelection.references[3] == 3u &&
                extremaSelection.references[4] == 3u &&
                extremaSelection.references[5] == 2u,
            "production extrema selection must mark the stable min/max point reference on all three weapon-root axes and ignore invalid samples");
        Require(
            shouldAdmitWeaponProbeSweep(
                true,
                true,
                false),
            "a critical weapon extremum must sweep even while cached/native evidence owns the frame");
        Require(
            !shouldAdmitWeaponProbeSweep(
                false,
                true,
                true),
            "cached/native authority must suppress only the rotating noncritical remainder");
        Require(
            shouldAdmitWeaponProbeSweep(
                false,
                false,
                true) &&
                !shouldAdmitWeaponProbeSweep(
                    false,
                    false,
                    false),
            "a noncritical weapon sample must follow bounded rotating admission during acquisition");

        const auto crossReturn =
            modelCrossReturnSweepCoverage(
                4.0f,
                -3.0f,
                5.0f);
        Require(
            !crossReturn.sparseEndpointSweepDetects &&
                crossReturn.perFrameCriticalSweepDetects,
            "a per-frame critical sweep must detect a thin-plane cross-and-return that sparse endpoints erase");
        const auto noCross =
            modelCrossReturnSweepCoverage(
                4.0f,
                2.0f,
                5.0f);
        Require(
            !noCross.sparseEndpointSweepDetects &&
                !noCross.perFrameCriticalSweepDetects,
            "critical coverage must not invent a crossing when every observation stays on one side");

        WeaponCorrectionManifold<Vec3> cornerManifold{};
        Require(
            admitWeaponCorrectionPlane(
                cornerManifold,
                Vec3{ 1.0f, 0.0f, 0.0f },
                3.0f) &&
                admitWeaponCorrectionPlane(
                    cornerManifold,
                    Vec3{ 0.0f, 1.0f, 0.0f },
                    4.0f) &&
                cornerManifold.count == 2,
            "two independent wall planes must enter the bounded weapon manifold");
        const auto cornerCorrection =
            solveWeaponCorrectionManifold(cornerManifold);
        Require(
            Near(cornerCorrection.x, 3.0f) &&
                Near(cornerCorrection.y, 4.0f) &&
                Near(cornerCorrection.z, 0.0f),
            "an orthogonal corner must preserve both plane corrections instead of selecting one wall");

        WeaponCorrectionManifold<Vec3> obliqueManifold{};
        (void)admitWeaponCorrectionPlane(
            obliqueManifold,
            Vec3{ 1.0f, 0.0f, 0.0f },
            2.0f);
        (void)admitWeaponCorrectionPlane(
            obliqueManifold,
            Vec3{ 0.70710678f, 0.70710678f, 0.0f },
            3.0f);
        const auto obliqueCorrection =
            solveWeaponCorrectionManifold(obliqueManifold);
        Require(
            obliqueCorrection.x >= 1.999f &&
                (obliqueCorrection.x + obliqueCorrection.y) *
                        0.70710678f >=
                    2.999f,
            "sequential projection must satisfy both constraints of an oblique two-plane corner");

        WeaponCorrectionManifold<Vec3> parallelManifold{};
        (void)admitWeaponCorrectionPlane(
            parallelManifold,
            Vec3{ 1.0f, 0.0f, 0.0f },
            2.0f);
        (void)admitWeaponCorrectionPlane(
            parallelManifold,
            Vec3{ 1.0f, 0.0f, 0.0f },
            5.0f);
        (void)admitWeaponCorrectionPlane(
            parallelManifold,
            Vec3{ -1.0f, 0.0f, 0.0f },
            4.0f);
        const auto parallelCorrection =
            solveWeaponCorrectionManifold(parallelManifold);
        Require(
            parallelManifold.count == 1 &&
                Near(parallelCorrection.x, 5.0f) &&
                Near(parallelCorrection.y, 0.0f),
            "parallel or opposed-collinear wall witnesses must dedupe to the stronger constraint instead of adding twice");

        Require(
            weaponCorrectionNormalsAreIndependent(
                Vec3{ 1.0f, 0.0f, 0.0f },
                Vec3{ 0.0f, 1.0f, 0.0f }) &&
                !weaponCorrectionNormalsAreIndependent(
                    Vec3{ 1.0f, 0.0f, 0.0f },
                    Vec3{ -1.0f, 0.0f, 0.0f }),
            "only genuinely independent wall normals may occupy both persistent corner slots");
        Require(
            secondaryWeaponPlaneMayUseProbe(
                sampledWeaponWorldProbeId(3),
                false) &&
                secondaryWeaponPlaneMayUseProbe(
                    kExactWeaponWorldProbeId,
                    true) &&
                secondaryWeaponPlaneMayUseProbe(
                    kSecondaryExactWeaponWorldProbeId,
                    true),
            "either transient exact hit may enter the secondary cache because the destination slot normalizes its ID");
        Require(
            normalizeWeaponCachedPlaneProbeId(
                WeaponCachedPlaneSlot::Primary,
                kSecondaryExactWeaponWorldProbeId,
                true) == kExactWeaponWorldProbeId &&
                normalizeWeaponCachedPlaneProbeId(
                    WeaponCachedPlaneSlot::Secondary,
                    kExactWeaponWorldProbeId,
                    true) == kSecondaryExactWeaponWorldProbeId,
            "cache destination slots must own exact probe IDs even when hit #1 is the stronger primary plane");
        Require(
            normalizeWeaponCachedPlaneProbeId(
                WeaponCachedPlaneSlot::Secondary,
                sampledWeaponWorldProbeId(3),
                false) == sampledWeaponWorldProbeId(3),
            "sampled probe IDs must remain stable when admitted to the secondary cache");
        Require(
            selectWeaponCachedPlaneSlot(
                false,
                false,
                false,
                false) == WeaponCachedPlaneSlot::Primary,
            "the first valid weapon plane must populate the primary cache even when it is an exact native anchor");
        Require(
            selectWeaponCachedPlaneSlot(
                true,
                false,
                true,
                true) == WeaponCachedPlaneSlot::Secondary,
            "an independent sampled plane must persist in the bounded secondary corner cache");
        Require(
            selectWeaponCachedPlaneSlot(
                true,
                false,
                false,
                true) == WeaponCachedPlaneSlot::None &&
                selectWeaponCachedPlaneSlot(
                    true,
                    false,
                    true,
                    false) == WeaponCachedPlaneSlot::None &&
                selectWeaponCachedPlaneSlot(
                    true,
                    true,
                    true,
                    true) == WeaponCachedPlaneSlot::None,
            "parallel evidence, exact secondary anchors, and a full two-plane cache must not allocate another slot");
        Require(
            shouldRetainCachedWeaponPlane(
                true,
                true,
                true,
                true) &&
                !shouldRetainCachedWeaponPlane(
                    false,
                    true,
                    true,
                    true) &&
                !shouldRetainCachedWeaponPlane(
                    true,
                    false,
                    true,
                    true) &&
                !shouldRetainCachedWeaponPlane(
                    true,
                    true,
                    false,
                    true) &&
                !shouldRetainCachedWeaponPlane(
                    true,
                    true,
                    true,
                    false),
            "either cached corner plane must clear on generation/layout/probe invalidation or geometric separation");
        for (std::size_t ordinal = 0;
             ordinal < kSampledWeaponWorldProbeCapacity;
             ++ordinal) {
            const auto probeId =
                sampledWeaponWorldProbeId(ordinal);
            Require(
                isSampledWeaponWorldProbeId(probeId),
                "every sampled hull ordinal must map to the sampled ID range");
            Require(
                probeId != kExactWeaponWorldProbeId,
                "no sampled hull ordinal may reuse the exact native anchor ID");
            Require(
                probeId == sampledWeaponWorldProbeId(ordinal),
                "a sampled hull ordinal must retain a stable ID");
        }

        std::array<bool, kSampledWeaponWorldProbeCapacity>
            visited{};
        std::size_t cursor = 0;
        constexpr std::size_t kCastBudget = 4;
        const std::size_t windowsNeeded =
            (kSampledWeaponWorldProbeCapacity +
             kCastBudget - 1) /
            kCastBudget;
        for (std::size_t frame = 0;
             frame < windowsNeeded;
             ++frame) {
            const auto window =
                resolveBoundedRotatingProbeWindow(
                    cursor,
                    kSampledWeaponWorldProbeCapacity,
                    kCastBudget);
            Require(
                window.count <= kCastBudget,
                "weapon probe admission must not exceed the per-frame cast budget");
            for (std::size_t offset = 0;
                 offset < window.count;
                 ++offset) {
                const std::size_t index =
                    boundedRotatingProbeIndex(
                        window,
                        offset,
                        kSampledWeaponWorldProbeCapacity);
                Require(
                    index < visited.size(),
                    "rotating weapon probe admission must stay inside bounded state");
                visited[index] = true;
            }
            cursor = window.nextCursor;
        }
        for (const bool wasVisited : visited) {
            Require(
                wasVisited,
                "bounded rotation must admit every sampled weapon hull probe without starvation");
        }

        SparseProbeQueryHistory<Vec3> sparseHistory{};
        constexpr std::uint64_t sampleIdentity = 0x1234u;
        (void)observeSparseProbe(
            sparseHistory,
            Vec3{ 0.0f, 0.0f, 0.0f },
            sampleIdentity);
        (void)observeSparseProbe(
            sparseHistory,
            Vec3{ 4.0f, 0.0f, 0.0f },
            sampleIdentity);
        (void)observeSparseProbe(
            sparseHistory,
            Vec3{ 9.0f, 0.0f, 0.0f },
            sampleIdentity);
        Require(
            Near(sparseHistory.lastQueryPosition.x, 0.0f),
            "an unadmitted sparse probe must retain the last position covered by a cast");
        Require(
            Near(sparseHistory.previousObserved.x, 9.0f),
            "per-frame observation must remain independent of sparse query history");
        commitSparseProbeQuery(
            sparseHistory,
            Vec3{ 9.0f, 0.0f, 0.0f },
            sampleIdentity);
        Require(
            Near(sparseHistory.lastQueryPosition.x, 9.0f),
            "an admitted cast must advance sparse query history");
        Require(
            observeSparseProbe(
                sparseHistory,
                Vec3{ 30.0f, 0.0f, 0.0f },
                0x5678u) &&
                Near(sparseHistory.lastQueryPosition.x, 30.0f),
            "a changed physical sample identity must rebase instead of bridging unrelated points");

        SparseProbeQueryHistory<Vec3> discontinuityHistory{};
        (void)observeSparseProbe(
            discontinuityHistory,
            Vec3{},
            sampleIdentity);
        Require(
            !sparseProbeObservationIsDiscontinuous(
                discontinuityHistory,
                Vec3{ 20.0f, 0.0f, 0.0f },
                sampleIdentity,
                1.0f / 90.0f),
            "a plausible fast weapon sample must retain continuous sweep history");
        Require(
            sparseProbeObservationIsDiscontinuous(
                discontinuityHistory,
                Vec3{ 40.0f, 0.0f, 0.0f },
                sampleIdentity,
                1.0f / 90.0f),
            "a one-frame pose teleport must rebase before it can become a deferred sparse sweep");
        Require(
            !sparseProbeObservationIsDiscontinuous(
                discontinuityHistory,
                Vec3{ 1000.0f, 0.0f, 0.0f },
                0x9999u,
                1.0f / 90.0f),
            "a changed sample identity is handled by identity rebasing rather than discontinuity bridging");

        const auto sampleA = makeWeaponSampleIdentity(0, 2, 0x1000u, true);
        const auto sampleB = makeWeaponSampleIdentity(1, 5, 0x2000u, false);
        auto layoutAB = appendWeaponSampleLayoutIdentity(0, sampleA);
        layoutAB = appendWeaponSampleLayoutIdentity(layoutAB, sampleB);
        auto layoutBA = appendWeaponSampleLayoutIdentity(0, sampleB);
        layoutBA = appendWeaponSampleLayoutIdentity(layoutBA, sampleA);
        Require(
            sampleA == makeWeaponSampleIdentity(0, 2, 0x1000u, true),
            "a physical weapon sample identity must be stable for one body/point/basis");
        Require(
            sampleA != makeWeaponSampleIdentity(0, 3, 0x1000u, true) &&
                sampleA != makeWeaponSampleIdentity(0, 2, 0x3000u, true),
            "point or transform-basis changes must invalidate sample history");
        Require(
            makeExactWeaponWorldProbeIdentity(0x1234u) !=
                makeExactWeaponWorldProbeIdentity(0x5678u),
            "native exact-probe history must be generation-local to its source weapon body");
        Require(
            makeExactWeaponWorldProbeIdentity(
                0x1234u,
                kExactWeaponWorldProbeId) !=
                makeExactWeaponWorldProbeIdentity(
                    0x1234u,
                    kSecondaryExactWeaponWorldProbeId),
            "the two exact corner anchors must never alias history for the same source body");
        Require(
            layoutAB != layoutBA,
            "reordering the bounded sample layout must invalidate ordinal probe state");
    }

    {
        using rock::soft_contact_policy::
            shouldAdmitNativeVisualStopTarget;
        using rock::soft_contact_policy::
            shouldCacheNativeVisualStopPlane;
        using rock::soft_contact_policy::
            shouldAcquireWeaponHandStop;
        using rock::soft_contact_policy::
            isHandPhysicallyFreeForWeaponStop;
        using rock::soft_contact_policy::
            hasHeldObjectAuthorityForWeaponStop;
        using rock::soft_contact_policy::
            resolveWeaponHandEntryFraction;
        using rock::soft_contact_policy::
            resolveWeaponHandDirectionalMotion;
        const auto cleanWeaponHandEntry =
            resolveWeaponHandEntryFraction(10.0f, 2.0f, 4.0f);
        Require(
            cleanWeaponHandEntry.valid &&
                Near(cleanWeaponHandEntry.fraction, 0.75f),
            "a clear-to-contact weapon-hand crossing must resolve an interior entry fraction");
        const auto lateWeaponHandEntry =
            resolveWeaponHandEntryFraction(3.0f, 1.0f, 4.0f);
        Require(
            lateWeaponHandEntry.valid &&
                Near(lateWeaponHandEntry.fraction, 0.0f),
            "a one-solve-late first manifold must conservatively retain the preceding weapon pose");
        const auto delayedAcquisitionEntry =
            resolveWeaponHandEntryFraction(3.8f, 3.4f, 4.0f);
        Require(
            delayedAcquisitionEntry.valid &&
                Near(delayedAcquisitionEntry.fraction, 0.0f),
            "a persistent already-overlapping manifold must never capture the penetrated segment endpoint");
        Require(
            !resolveWeaponHandEntryFraction(10.0f, 5.0f, 4.0f).valid,
            "a weapon segment that remains clear of the hand must not report an entry fraction");
        Require(
            !resolveWeaponHandEntryFraction(5.0f, 5.0f, 4.0f).valid &&
                !resolveWeaponHandEntryFraction(
                    std::numeric_limits<float>::quiet_NaN(),
                    2.0f,
                    4.0f)
                    .valid,
            "stationary or non-finite weapon-hand samples must reject entry reconstruction");
        const auto thresholdWeaponHandEntry =
            resolveWeaponHandEntryFraction(6.0f, 4.0f, 4.0f);
        Require(
            thresholdWeaponHandEntry.valid &&
                Near(thresholdWeaponHandEntry.fraction, 1.0f) &&
                resolveWeaponHandEntryFraction(4.0f, 3.0f, 4.0f).valid,
            "touching the clearance at the segment end is valid and an already-contacting prior sample retains fraction zero");
        Require(
            shouldAdmitNativeVisualStopTarget(
                false, true, false, true, false),
            "a generated weapon manifold must stop a free legacy hand when its dynamic proxy bank is unavailable");
        Require(
            !shouldAdmitNativeVisualStopTarget(
                false, true, false, false, true),
            "the weapon/world probe channel must not consume a hand/weapon manifold as wall evidence");
        Require(
            !shouldAdmitNativeVisualStopTarget(
                false, false, true, false, true),
            "generic native world admission must not reinterpret generated hands as static walls");
        Require(
            shouldAcquireWeaponHandStop(
                true, true, true, true, true, true,
                6.0f, 0.25f, 6.25f),
            "a continuously sampled weapon that closes on a free hand must stop at its previous clear pose");
        Require(
            !shouldAcquireWeaponHandStop(
                true, true, true, true, true, true,
                0.0f, 6.0f, 6.0f) &&
                !shouldAcquireWeaponHandStop(
                    true, true, true, true, true, true,
                    2.0f, 3.0f, 5.0f),
            "a stationary/slower weapon must not move when the hand is the side entering contact");
        Require(
            !shouldAcquireWeaponHandStop(
                false, true, true, true, true, true,
                6.0f, 0.0f, 6.0f) &&
                !shouldAcquireWeaponHandStop(
                    true, false, true, true, true, true,
                    6.0f, 0.0f, 6.0f) &&
                !shouldAcquireWeaponHandStop(
                    true, true, false, true, true, true,
                    6.0f, 0.0f, 6.0f),
            "missing continuous history, stale weapon identity, or an occupied hand must reject reciprocal weapon stops");
        Require(
            !shouldAcquireWeaponHandStop(
                true, true, true, true, false, true,
                6.0f, 0.0f, 6.0f) &&
                !shouldAcquireWeaponHandStop(
                    true, true, true, true, true, false,
                    6.0f, 0.0f, 6.0f),
            "stale contact frames and unknown endpoint velocities must not acquire a weapon stop");
        Require(
            !shouldAcquireWeaponHandStop(
                true, true, true, true, true, true,
                8.0f, 0.0f, 0.0f) &&
                shouldAcquireWeaponHandStop(
                    true, true, true, true, true, true,
                    8.0f, 0.0f, 4.0f),
            "common-mode motion must reject while a gun catching a co-moving hand may stop");
        Require(
            !shouldAcquireWeaponHandStop(
                true, true, true, true, true, true,
                0.5f, 0.0f, 0.5f) &&
                shouldAcquireWeaponHandStop(
                    true, true, true, true, true, true,
                    0.6f, 0.0f, 0.6f),
            "weapon-hand acquisition must reject tracking noise at and below the half-unit-per-second floor");
        Require(
            shouldAcquireWeaponHandStop(
                true, true, true, false, true, true,
                6.0f, 0.0f, 6.0f),
            "a persistent weapon-dominant manifold may acquire conservatively when first-frame metadata was unavailable");
        Require(
            isHandPhysicallyFreeForWeaponStop(
                false, true, false, false),
            "a presentation-only visual return must not disable reciprocal gun-to-hand collision");
        Require(
            !isHandPhysicallyFreeForWeaponStop(
                true, false, false, false) &&
                !isHandPhysicallyFreeForWeaponStop(
                    false, false, true, false) &&
                !isHandPhysicallyFreeForWeaponStop(
                    false, false, false, true),
            "real weapon ownership, a disabled hand, and held-object ownership must still block reciprocal gun-to-hand collision");
        Require(
            !hasHeldObjectAuthorityForWeaponStop(false, false, false) &&
                hasHeldObjectAuthorityForWeaponStop(true, false, false) &&
                hasHeldObjectAuthorityForWeaponStop(false, true, false) &&
                hasHeldObjectAuthorityForWeaponStop(false, false, true),
            "ROCK-held, host-held, and host-suppressed hands must each preserve their stronger object/collision authority");

        const auto towardNoise =
            resolveWeaponHandDirectionalMotion(-0.6f, 0.49f);
        const auto awayNoise =
            resolveWeaponHandDirectionalMotion(-0.6f, -0.49f);
        Require(
            towardNoise.valid && awayNoise.valid &&
                Near(towardNoise.handApproachSpeedGameUnits, 0.0f) &&
                Near(awayNoise.handApproachSpeedGameUnits, 0.0f) &&
                Near(towardNoise.relativeClosingSpeedGameUnits, 0.6f) &&
                Near(awayNoise.relativeClosingSpeedGameUnits, 0.6f),
            "stationary offhand proxy noise must not reverse slow gun-to-hand arbitration");
        Require(
            rock::soft_contact_policy::
                    isAuthoritativeWeaponHandEvidenceLayer(48u, 48u) &&
                !rock::soft_contact_policy::
                    isAuthoritativeWeaponHandEvidenceLayer(43u, 48u),
            "a legacy callback must not consume the dynamic proxy bank's first-contact episode");

        using rock::soft_contact_policy::
            observeWeaponHandContactEpisode;
        rock::soft_contact_policy::WeaponHandContactEpisodeState
            episode{};
        auto episodeStep = observeWeaponHandContactEpisode(
            episode, true, 0x1234u, 10u);
        Require(episodeStep.freshEntry && episodeStep.next.active,
            "the first canonical hand/weapon witness must open one stop episode");
        episode = episodeStep.next;
        episode = observeWeaponHandContactEpisode(
                      episode, false, 0x1234u, 11u)
                      .next;
        episode = observeWeaponHandContactEpisode(
                      episode, false, 0x1234u, 12u)
                      .next;
        episodeStep = observeWeaponHandContactEpisode(
            episode, true, 0x1234u, 12u);
        Require(!episodeStep.freshEntry,
            "body-pair churn and a bounded callback miss must not rearm a persistent overlap");
        episode = episodeStep.next;
        episode = observeWeaponHandContactEpisode(
                      episode, false, 0x1234u, 13u)
                      .next;
        episode = observeWeaponHandContactEpisode(
                      episode, false, 0x1234u, 14u)
                      .next;
        episode = observeWeaponHandContactEpisode(
                      episode, false, 0x1234u, 15u)
                      .next;
        Require(!episode.active,
            "three clear frames must close the prior hand/weapon episode");
        episodeStep = observeWeaponHandContactEpisode(
            episode, true, 0x1234u, 16u);
        Require(episodeStep.freshEntry,
            "a genuinely separated pair may acquire a new snap-free stop");
        const auto generationStep =
            observeWeaponHandContactEpisode(
                episodeStep.next, true, 0x5678u, 17u);
        Require(generationStep.freshEntry,
            "a weapon generation change must start an independent episode");
        const auto wrapStep =
            observeWeaponHandContactEpisode(
                { true, 0x5678u, 0xFFFF'FFFFu },
                true,
                0x5678u,
                0u);
        Require(!wrapStep.freshEntry,
            "unsigned frame wrap must preserve a continuous contact episode");
        Require(
            !shouldCacheNativeVisualStopPlane(true) &&
                shouldCacheNativeVisualStopPlane(false),
            "moving generated-weapon planes must remain fresh while stable world planes retain normal caching");

        using rock::soft_contact_policy::
            resolveWorldContactChannels;
        const auto releaseChannels =
            resolveWorldContactChannels(
                true,
                false,
                false,
                false,
                false,
                false,
                true,
                true,
                true,
                false);
        Require(
            !releaseChannels.handPushback,
            "the release gate must disable free-hand wall pushback");
        Require(
            releaseChannels.weaponWallCollision,
            "disabling hand pushback must preserve equipped-weapon wall collision");

        const auto weaponDisabled =
            resolveWorldContactChannels(
                true,
                false,
                false,
                false,
                false,
                false,
                true,
                false,
                true,
                false);
        Require(
            !weaponDisabled.anyEnabled(),
            "weapon wall collision must honor its static-world setting when hand pushback is off");

        const auto menuBlocked =
            resolveWorldContactChannels(
                true,
                true,
                false,
                false,
                false,
                false,
                true,
                true,
                true,
                true);
        Require(
            !menuBlocked.anyEnabled(),
            "a blocking menu must suppress both world-contact channels");

        const auto dynamicHandOwner =
            resolveWorldContactChannels(
                true,
                true,
                true,
                true,
                false,
                false,
                true,
                true,
                true,
                false);
        Require(
            !dynamicHandOwner.handPushback,
            "the dynamic hand solver must supersede legacy hand soft contact");
        Require(
            dynamicHandOwner.weaponWallCollision,
            "selecting dynamic hand collision must preserve the independent weapon wall channel");

        const auto oneHandFallback =
            resolveWorldContactChannels(
                true,
                true,
                true,
                false,
                false,
                false,
                true,
                true,
                true,
                false);
        Require(
            !oneHandFallback.rightHandPushback &&
                oneHandFallback.leftHandPushback,
            "a current-frame proxy failure must restore legacy wall stop only for the affected hand");

        const auto oneAttachedHandFallback =
            resolveWorldContactChannels(
                true,
                true,
                false,
                false,
                true,
                false,
                true,
                true,
                true,
                false);
        Require(
            !oneAttachedHandFallback.rightHandPushback &&
                oneAttachedHandFallback.leftHandPushback,
            "an attached hand must not start a legacy fallback authority while the unrelated free hand retains it");
        Require(
            oneAttachedHandFallback.weaponWallCollision,
            "attached-hand fallback suppression must retain the rigid weapon wall channel");
    }

    {
        Require(
            !isHandAttachedForWeaponWallTransport(
                true, true, false, false),
            "PartCarry must not drag its detached former firing hand during visual return");
        Require(
            isHandAttachedForWeaponWallTransport(
                true, false, true, false),
            "a PartCarry pivot or attached part grip must travel with the weapon");
        Require(
            isHandAttachedForWeaponWallTransport(
                true, true, false, true),
            "an occupied two-hand primary grip must travel with the weapon");
        Require(
            isHandAttachedForWeaponWallTransport(
                false, true, false, false),
            "an unowned native weapon may be transported by its firing hand");
        Require(
            !isHandAttachedForWeaponWallTransport(
                false, false, false, false),
            "an unrelated free hand must never receive weapon wall transport");

        Require(
            hasCompleteAttachedHandWorldSet(false, false),
            "a genuinely hand-free weapon pose may use the finite wall-correction fallback");
        Require(
            hasCompleteAttachedHandWorldSet(true, true),
            "a complete attached-hand target set may transport rigidly with the weapon");
        Require(
            !hasCompleteAttachedHandWorldSet(true, false),
            "a missing attached-hand target must fail closed instead of splitting the weapon from that hand");

        Require(
            hasCompleteAttachedHandPublicationSet(
                true, true, true, true),
            "a two-hand wall transport may commit only after both attached hand writers accept it");
        Require(
            !hasCompleteAttachedHandPublicationSet(
                true, true, false, true) &&
                !hasCompleteAttachedHandPublicationSet(
                    true, true, true, false),
            "either attached-hand publication failure must abort the complete rigid wall transaction");
        Require(
            hasCompleteAttachedHandPublicationSet(
                true, false, true, false),
            "an unattached free hand must not be required to publish a weapon wall correction");

        Require(
            shouldReuseWeaponWorldContactRigidPin(
                true, true, true, true, true, false),
            "an immutable wall episode must preserve its captured grip relation while the bounded release target advances");
        Require(
            shouldReuseWeaponWorldContactRigidPin(
                true, true, true, false, false, true) &&
                !shouldReuseWeaponWorldContactRigidPin(
                    true, true, true, false, false, false),
            "reciprocal hand stops must retain their prior exact blocked-pose identity rule");
        Require(
            !shouldReuseWeaponWorldContactRigidPin(
                true, false, true, true, true, true) &&
                !shouldReuseWeaponWorldContactRigidPin(
                    true, true, false, true, true, true) &&
                !shouldReuseWeaponWorldContactRigidPin(
                    true, true, true, true, false, true),
            "generation, ownership, or stop-kind changes must retire an immutable grip pin");

        Require(
            Near(safeWeaponWallCorrectionFallback(18.0f), 18.0f) &&
                Near(
                    safeWeaponWallCorrectionFallback(
                        std::numeric_limits<float>::quiet_NaN()),
                    kWeaponWallCorrectionFallbackGameUnits),
            "missing reach data must retain a finite configured/default correction envelope");
        Require(
            Near(
                safeWeaponWallCorrectionFallback(1000000.0f),
                kWeaponWallCorrectionHardSafetyGameUnits),
            "corrupt correction configuration must remain below the hard safety ceiling");
        Require(
            weaponWallPoseDoesNotWorsenReach(55.0f, 54.0f, 40.0f),
            "a wall stop that improves an already overextended raw pose must remain admissible");
        Require(
            weaponWallPoseDoesNotWorsenReach(55.0f, 55.2f, 40.0f),
            "a sub-tolerance stop step must remain admissible outside nominal reach");
        Require(
            !weaponWallPoseDoesNotWorsenReach(55.0f, 57.0f, 40.0f),
            "a wall response must not worsen an already overextended hand pose");

        const auto roomyReach = directionalArmReachCorrectionLimit(
            Vec3{},
            Vec3{ 37.0f, 0.0f, 0.0f },
            Vec3{},
            50.0f);
        Require(
            roomyReach.valid &&
                !roomyReach.constrained &&
                Near(roomyReach.maxCorrection, 37.0f),
            "a validated roomy reach sphere must preserve full cached wall depth");

        const auto obliqueReach = directionalArmReachCorrectionLimit(
            Vec3{ 0.0f, 4.0f, 0.0f },
            Vec3{ 4.0f, 0.0f, 0.0f },
            Vec3{},
            5.15f);
        Require(
            obliqueReach.valid &&
                obliqueReach.constrained &&
                Near(obliqueReach.maxCorrection, 3.0f),
            "reach limiting must shorten only the wall-direction scalar without adding tangent motion");

        const auto tighterPeerReach = directionalArmReachCorrectionLimit(
            Vec3{ 0.0f, 4.8f, 0.0f },
            Vec3{ 4.0f, 0.0f, 0.0f },
            Vec3{},
            5.15f);
        Require(
            tighterPeerReach.valid &&
                tighterPeerReach.maxCorrection <
                    obliqueReach.maxCorrection,
            "a shared two-hand correction must use the tighter attached arm's scalar limit");
    }

    {
        Require(
            !canUseHandOnlyWeaponWorldContactTransport(
                false,
                true,
                false),
            "FRIK API v3 must translate the weapon explicitly because the host shim only moves the skinned arm");
        Require(
            canUseHandOnlyWeaponWorldContactTransport(
                false,
                true,
                true),
            "native FRIK visual authority may carry an unowned child weapon with the firing hand");
        Require(
            !canUseHandOnlyWeaponWorldContactTransport(
                true,
                true,
                true),
            "a ROCK-owned weapon always requires explicit weapon transport");
        Require(
            !canUseHandOnlyWeaponWorldContactTransport(
                false,
                false,
                true),
            "hand-only transport requires a valid firing-hand world transform");
    }

    {
        Require(
            shouldPublishPrimaryHandAuthority(false, false),
            "legacy FRIK must pin the right firing hand to the solved trigger grip");
        Require(
            shouldPublishPrimaryHandAuthority(false, true),
            "legacy FRIK must preserve left-firing hand authority");
        Require(
            shouldPublishPrimaryHandAuthority(true, false),
            "native FRIK visual authority must preserve right-firing hand publication");
        Require(
            shouldPublishPrimaryHandAuthority(true, true),
            "native FRIK visual authority must preserve left-firing hand publication");
        Require(
            isPrimaryGripHandAuthorityTag(
                kPrimaryGripHandAuthorityTag),
            "the legacy host must recognize the canonical primary-grip writer");
        Require(
            !isPrimaryGripHandAuthorityTag(
                kSupportGripHandAuthorityTag),
            "the primary same-frame pin must not capture support-grip writers");
    }

    {
        Require(
            usesRigidSupportGripHandAuthority(true, true, false),
            "a Gripping support hand whose solver owns the weapon must use rigid authority");
        Require(
            std::string_view(selectSupportGripHandAuthorityTag(
                true,
                true,
                false)) == kRigidSupportGripHandAuthorityTag,
            "the full two-hand solver must publish the legacy-FRIK rigid tag");

        Require(
            !usesRigidSupportGripHandAuthority(true, false, false),
            "visual-only support must retain free-hand safety");
        Require(
            !usesRigidSupportGripHandAuthority(true, true, true),
            "AttachOnly support must retain free-hand safety");
        Require(
            !usesRigidSupportGripHandAuthority(false, true, false),
            "part-carry and non-Gripping states must retain free-hand safety");
        Require(
            std::string_view(selectSupportGripHandAuthorityTag(
                false,
                true,
                false)) == kSupportGripHandAuthorityTag,
            "part-carry must publish the ordinary support tag");

        // REACHABILITY INVARIANT (Jul 31, replaces the inverted assertion this
        // slot used to hold). The old rule — "a full-authority sidearm must
        // retain its contacted support-hand weld" — became unsatisfiable once
        // sidearms started entering the full two-handed solver: no weapon class
        // could pass both terms, so the reseat silently became dead code and a
        // pistol's off-hand stayed welded to whatever mesh triangle it first
        // touched (measured 19.1gu out), forcing the rendered arm ~40% past
        // anatomy — the reported "offhand stretches unnaturally long".
        //
        // The reseat only rewrites the RENDERED hand (handWeaponLocal) and
        // clears the source frames; the solver keeps steering from the captured
        // contacted point (gripLocal, which the reseat never touches — verified
        // via resolvePartGripWorld -> resolveCurrentSupportAttachmentRoot).
        // Presentation and steering are therefore genuinely separable, and the
        // reseat MUST stay reachable in the full-solver case.
        Require(
            shouldApplySidearmPresentationReseat(
                true,
                true,
                true,
                false,
                true),
            "the sidearm presentation reseat must stay reachable while the support grip owns the weapon");
        Require(
            shouldApplySidearmPresentationReseat(
                true,
                true,
                false,
                false,
                true),
            "a visual-only sidearm may use the presentation cup/reseat");
        Require(
            !shouldApplySidearmPresentationReseat(
                true,
                false,
                false,
                false,
                true),
            "a long gun must never use the sidearm presentation reseat");
        Require(
            !shouldApplySidearmPresentationReseat(
                true,
                true,
                false,
                true,
                true),
            "AttachOnly support must retain its provider-authored frame");
    }

    {
        Transform weapon{};
        weapon.rotate = Yaw90();
        weapon.translate = Vec3{ 14000.0f, -87000.0f, 250.0f };
        const Vec3 primaryLocal{ 2.0f, -3.0f, 1.0f };
        const Vec3 primaryTarget{ 14020.0f, -86970.0f, 260.0f };

        // Simulate any post-solve translation (the removed whole-gun reach clamp),
        // then prove the invariant restores the captured primary pivot exactly even
        // at large world coordinates.
        weapon.translate.x += 17.0f;
        weapon.translate.y -= 9.0f;
        const Transform anchored = reanchorAtPrimaryGrip(weapon, primaryLocal, primaryTarget);
        const Vec3 primaryWorld = rock::transform_math::localPointToWorld(anchored, primaryLocal);
        Require(Near(primaryWorld.x, primaryTarget.x), "primary grip x must remain exact");
        Require(Near(primaryWorld.y, primaryTarget.y), "primary grip y must remain exact");
        Require(Near(primaryWorld.z, primaryTarget.z), "primary grip z must remain exact");
    }

    {
        // The rigid firing-hand anchor is the captured hand-frame origin, not
        // a palm point offset from it.  Even after a large offhand-induced
        // weapon rotation, recomposing the captured hand frame must leave the
        // wrist at the controller position exactly.
        Transform weapon{};
        weapon.rotate = Yaw90();
        weapon.translate = Vec3{ -1200.0f, 8400.0f, 95.0f };
        Transform handWeaponLocal{};
        handWeaponLocal.translate = Vec3{ 1.5f, -4.0f, 2.25f };
        const Vec3 liveWristTarget{ -1190.0f, 8412.0f, 101.0f };
        const Transform anchored = reanchorAtPrimaryGrip(
            weapon,
            handWeaponLocal.translate,
            liveWristTarget);
        const Transform recomposedHand =
            rock::transform_math::composeTransforms(
                anchored,
                handWeaponLocal);
        Require(
            Near(recomposedHand.translate.x, liveWristTarget.x) &&
                Near(recomposedHand.translate.y, liveWristTarget.y) &&
                Near(recomposedHand.translate.z, liveWristTarget.z),
            "the firing wrist must not orbit or translate when the offhand "
            "rotates the weapon");
    }

    {
        Require(Near(armLengthScale(false), 1.0f), "ordinary and primary hands must keep native reach");
        Require(Near(armLengthScale(true), 1.40f), "support grip should expose only the bounded maximum reach allowance");
    }

    {
        constexpr float nativeReach = 38.7f;
        const float localScale = adaptiveArmLengthScale(40.5f, nativeReach, armLengthScale(true));
        const float testerScale = adaptiveArmLengthScale(44.8f, nativeReach, armLengthScale(true));
        Require(localScale > 1.0f && localScale < testerScale,
            "larger tracked distances should request proportionally more visual reach");
        Require(testerScale < armLengthScale(true),
            "the tester's 44.8-unit target should remain exactly reachable below the cap");
        Require(Near(
                    projectedArmTargetDistance(44.8f, nativeReach, testerScale),
                    44.8f),
            "adaptive reach should keep the support hand seated at the grip");
    }

    {
        // Model the same physical extension under several common fVrScale values.
        // The policy consumes the resulting world-space ratio, so no hard-coded
        // reference VR scale is required.
        constexpr float nativeReach = 38.7f;
        constexpr float physicalDistanceAtScaleOne = 0.60f;
        for (const float vrScale : { 64.0f, 70.0f, 74.0f }) {
            const float requested = physicalDistanceAtScaleOne * vrScale;
            const float scale = adaptiveArmLengthScale(
                requested, nativeReach, armLengthScale(true));
            Require(Near(
                        projectedArmTargetDistance(requested, nativeReach, scale),
                        requested),
                "representative VR scales must retain an exact support-hand seat");
        }
    }

    {
        // The fVrScale=74 tester log reached 51.8 game units while the live
        // native chain was 38.7.  The old 1.20 cap stopped at 46.4 and left a
        // 5.5-unit visible wrist-to-grip gap.
        constexpr float nativeReach = 38.7f;
        constexpr float observedTesterDistance = 51.8f;
        const float scale = adaptiveArmLengthScale(
            observedTesterDistance,
            nativeReach,
            armLengthScale(true));
        Require(scale > 1.20f && scale < armLengthScale(true),
            "the observed tester reach must use the added scale headroom without saturating");
        Require(Near(
                    projectedArmTargetDistance(
                        observedTesterDistance,
                        nativeReach,
                        scale),
                    observedTesterDistance),
            "the observed fVrScale=74 support hand must stay exactly seated");
    }

    {
        // Beyond the visual stretch cap, projection must saturate monotonically.
        // The old easing curve increased and then decreased in this interval,
        // which made walk sway visibly pull the hand back and forth.
        constexpr float nativeReach = 38.7f;
        const float maxScale = armLengthScale(true);
        float previous = -1.0f;
        for (float requested = 35.0f; requested <= 60.0f; requested += 0.05f) {
            const float scale = adaptiveArmLengthScale(requested, nativeReach, maxScale);
            const float projected =
                projectedArmTargetDistance(requested, nativeReach, scale);
            Require(projected + 0.0001f >= previous,
                "reach projection must never move backward as the controller extends");
            Require(projected <= requested + 0.0001f,
                "reach projection must never move ahead of the requested target");
            previous = projected;
        }
        Require(Near(
                    previous,
                    nativeReach * maxScale - kSupportArmReachSafetyMargin),
            "far targets should settle at the bounded visual reach cap");
    }

    {
        // Lever-arm-conditioned steering authority (pistol grip fix). The live
        // measurements the thresholds came from: 10mm pistol cup 13.3gu, shortest
        // long gun 22.4gu, defaults min=15 full=22.
        Require(Near(supportSteeringAuthorityWeight(13.3f, 15.0f, 22.0f), 0.0f),
            "a pistol-length lever arm must fully demote support steering");
        Require(Near(supportSteeringAuthorityWeight(15.0f, 15.0f, 22.0f), 0.0f),
            "the minimum lever arm itself must still be demoted");
        Require(Near(supportSteeringAuthorityWeight(22.0f, 15.0f, 22.0f), 1.0f),
            "the full lever arm must grant bit-exact legacy authority");
        Require(Near(supportSteeringAuthorityWeight(22.4f, 15.0f, 22.0f), 1.0f),
            "every measured long gun must stay on the legacy path");
        Require(Near(supportSteeringAuthorityWeight(18.5f, 15.0f, 22.0f), 0.5f),
            "the smoothstep must cross one-half exactly mid-band");
        {
            // C1 smoothstep: monotonic, no overshoot, flat-tangent entry/exit.
            float previous = -1.0f;
            for (float lever = 14.0f; lever <= 23.0f; lever += 0.05f) {
                const float weight = supportSteeringAuthorityWeight(lever, 15.0f, 22.0f);
                Require(weight >= previous - 0.0001f,
                    "steering authority must never decrease as the lever arm grows");
                Require(weight >= 0.0f && weight <= 1.0f,
                    "steering authority must stay inside [0, 1]");
                previous = weight;
            }
        }
        // Fail-open contract: bad inputs and degenerate windows keep full authority.
        Require(Near(supportSteeringAuthorityWeight(
                        std::numeric_limits<float>::quiet_NaN(), 15.0f, 22.0f),
                    1.0f),
            "a non-finite lever arm must fail open to full authority");
        Require(Near(supportSteeringAuthorityWeight(18.0f, 22.0f, 15.0f), 1.0f),
            "an inverted window must fail open to full authority");
        Require(Near(supportSteeringAuthorityWeight(18.0f, 18.0f, 18.0f), 1.0f),
            "a zero-width window must fail open to full authority");

        Require(Near(
                    effectiveSupportSteeringWeight(
                        13.3f,
                        15.0f,
                        22.0f,
                        0.35f),
                    0.35f),
            "a pistol-length lever must retain the configured authority floor");
        Require(Near(
                    effectiveSupportSteeringWeight(
                        18.5f,
                        15.0f,
                        22.0f,
                        0.35f),
                    0.5f),
            "the authority floor must not reduce a larger smoothstep weight");
        Require(Near(
                    effectiveSupportSteeringWeight(
                        13.3f,
                        15.0f,
                        22.0f,
                        0.0f),
                    0.0f),
            "an explicit zero floor must preserve the opt-out contract");
        Require(Near(
                    effectiveSupportSteeringWeight(
                        13.3f,
                        15.0f,
                        22.0f,
                        1.0f),
                    1.0f),
            "the MCM maximum must give a short weapon full off-hand steering authority");
    }

    {
        Transform weld{};
        weld.rotate = Yaw90();
        weld.translate = Vec3{ 12.0f, 34.0f, 56.0f };
        Transform controller{};
        controller.rotate.entry[0][0] = 1.0f;
        controller.rotate.entry[1][1] = 1.0f;
        controller.rotate.entry[2][2] = 1.0f;
        controller.translate = Vec3{ -2.0f, -4.0f, -6.0f };

        const Transform controllerWrist =
            blendFiringWristTowardController(weld, controller, 0.0f);
        Require(
            Near(controllerWrist.rotate.entry[0][0], 1.0f) &&
                Near(controllerWrist.rotate.entry[0][1], 0.0f) &&
                Near(controllerWrist.translate.x, weld.translate.x),
            "zero wrist follow must keep controller orientation but preserve weld translation");

        const Transform rigidWrist =
            blendFiringWristTowardController(weld, controller, 1.0f);
        Require(
            Near(rigidWrist.rotate.entry[0][1], weld.rotate.entry[0][1]) &&
                Near(rigidWrist.translate.y, weld.translate.y),
            "full wrist follow must preserve the rigid weld exactly");

        Require(
            shouldBlendFiringWristFromLiveHand(true, true),
            "an exact full-authority acquisition must still honor firing-wrist follow");
        Require(
            !shouldBlendFiringWristFromLiveHand(false, false),
            "firing-wrist follow requires a live primary-hand frame");
    }

    {
        // Jul 30 regression guard: the firing hand must keep the rigid weld on
        // long guns even when the configured follow factor is 0.
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 22.0f, 15.0f, 22.0f), 1.0f),
            "a long-gun grip separation at the full lever arm must restore the rigid wrist weld");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 38.6f, 15.0f, 22.0f), 1.0f),
            "a wide long-gun grip must keep the full wrist weld");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 13.3f, 15.0f, 22.0f), 0.0f),
            "a sidearm cup under the minimum lever arm must keep the controller wrist (Jul 27)");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 15.0f, 15.0f, 22.0f), 0.0f),
            "the minimum lever arm itself must still protect the sidearm wrist");
        Require(Near(firingWristFollowFactorForLeverArm(0.0f, 18.5f, 15.0f, 22.0f), 0.5f),
            "the transition band must blend the weld smoothly");
        Require(Near(firingWristFollowFactorForLeverArm(0.8f, 13.3f, 15.0f, 22.0f), 0.8f),
            "an explicit configured follow factor must act as a global minimum");
        Require(Near(firingWristFollowFactorForLeverArm(
                    std::numeric_limits<float>::quiet_NaN(), 30.0f, 15.0f, 22.0f),
                    1.0f),
            "a non-finite configured factor must defer to the lever gate");
        Require(Near(firingWristFollowFactorForLeverArm(
                    0.0f, std::numeric_limits<float>::quiet_NaN(), 15.0f, 22.0f),
                    1.0f),
            "a non-finite grip separation must fail open to the weld (safe for long guns)");
    }

    {
        Require(Near(meshBorderSafeFingerOpenValue(0.42f, true, false), 0.42f),
            "a valid first-surface curl should be preserved");
        Require(Near(meshBorderSafeFingerOpenValue(0.30f, false, false), 1.0f),
            "a mesh miss must not use the generic closed fallback");
        Require(Near(meshBorderSafeFingerOpenValue(0.55f, true, true), 1.0f),
            "a pad detected inside the mesh must be opened fully");
        Require(Near(meshBorderSafeFingerOpenValue(1.15f, false, false), 1.15f),
            "mesh safety must never close an already over-open thumb");
    }

    return 0;
}
