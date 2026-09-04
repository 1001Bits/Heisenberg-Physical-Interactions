#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rock::soft_contact_policy
{
    inline constexpr std::uint32_t kExactWeaponWorldProbeId =
        0x7000u;
    inline constexpr std::uint32_t kSecondaryExactWeaponWorldProbeId =
        0x7001u;
    inline constexpr std::uint32_t kSampledWeaponWorldProbeIdBase =
        0x7100u;
    inline constexpr std::size_t kSampledWeaponWorldProbeCapacity =
        22;
    inline constexpr std::size_t kExactWeaponWorldProbeStateIndex =
        kSampledWeaponWorldProbeCapacity;
    inline constexpr std::size_t
        kSecondaryExactWeaponWorldProbeStateIndex =
            kSampledWeaponWorldProbeCapacity + 1;
    inline constexpr std::size_t kWeaponWorldProbeStateCapacity =
        kSampledWeaponWorldProbeCapacity + 2;
    // The producer emits the six weapon-root AABB extrema first.  Those
    // extrema include the longitudinal muzzle/breech pair regardless of which
    // local axis an asset uses, and are the bounded CCD set swept every frame.
    inline constexpr std::size_t kCriticalWeaponWorldProbeCapacity = 6;
    inline constexpr std::size_t kRotatingWeaponWorldProbeBudget = 4;
    inline constexpr std::uint64_t kWeaponSampleLayoutHashSeed =
        1469598103934665603ull;
    inline constexpr std::uint64_t kWeaponSampleLayoutHashPrime =
        1099511628211ull;

    template <class Reference>
    struct CriticalWeaponExtremaSelection
    {
        std::array<Reference,
            kCriticalWeaponWorldProbeCapacity>
            references{};
        std::array<bool,
            kCriticalWeaponWorldProbeCapacity>
            valid{};
        std::array<float,
            kCriticalWeaponWorldProbeCapacity>
            values{
                (std::numeric_limits<float>::max)(),
                (std::numeric_limits<float>::lowest)(),
                (std::numeric_limits<float>::max)(),
                (std::numeric_limits<float>::lowest)(),
                (std::numeric_limits<float>::max)(),
                (std::numeric_limits<float>::lowest)(),
            };
    };

    static_assert(
        kCriticalWeaponWorldProbeCapacity == 6,
        "weapon extrema selection expects min/max on three axes");

    /*
     * Incremental and allocation-free so production can rank points across all
     * generated bodies while pure tests exercise the exact selector. Slots are
     * minX/maxX/minY/maxY/minZ/maxZ; strict comparison makes ties retain the
     * first stable generation-local body/point reference.
     */
    template <class Point, class Reference>
    inline void observeCriticalWeaponExtrema(
        CriticalWeaponExtremaSelection<Reference>& selection,
        const Point& point,
        const Reference& reference) noexcept
    {
        if (!std::isfinite(point.x) ||
            !std::isfinite(point.y) ||
            !std::isfinite(point.z)) {
            return;
        }
        const std::array<float,
            kCriticalWeaponWorldProbeCapacity>
            coordinates{
                point.x,
                point.x,
                point.y,
                point.y,
                point.z,
                point.z,
            };
        for (std::size_t extremum = 0;
             extremum < selection.references.size();
             ++extremum) {
            const bool isMinimum = (extremum & 1u) == 0;
            const bool improves = isMinimum
                ? coordinates[extremum] < selection.values[extremum]
                : coordinates[extremum] > selection.values[extremum];
            if (!improves) {
                continue;
            }
            selection.values[extremum] = coordinates[extremum];
            selection.references[extremum] = reference;
            selection.valid[extremum] = true;
        }
    }

    [[nodiscard]] inline constexpr std::uint32_t
    sampledWeaponWorldProbeId(const std::size_t ordinal) noexcept
    {
        return kSampledWeaponWorldProbeIdBase +
               static_cast<std::uint32_t>(ordinal);
    }

    [[nodiscard]] inline constexpr bool isSampledWeaponWorldProbeId(
        const std::uint32_t probeId) noexcept
    {
        return probeId >= kSampledWeaponWorldProbeIdBase &&
               probeId < sampledWeaponWorldProbeId(
                             kSampledWeaponWorldProbeCapacity);
    }

    static_assert(
        !isSampledWeaponWorldProbeId(kExactWeaponWorldProbeId),
        "the native exact weapon anchor must not alias a sampled hull probe");
    static_assert(
        !isSampledWeaponWorldProbeId(
            kSecondaryExactWeaponWorldProbeId),
        "the second native exact weapon anchor must not alias a sampled hull probe");

    [[nodiscard]] inline constexpr bool isExactWeaponWorldProbeId(
        const std::uint32_t probeId) noexcept
    {
        return probeId == kExactWeaponWorldProbeId ||
               probeId == kSecondaryExactWeaponWorldProbeId;
    }

    [[nodiscard]] inline constexpr std::size_t
    exactWeaponWorldProbeStateIndex(
        const std::uint32_t probeId) noexcept
    {
        return probeId == kSecondaryExactWeaponWorldProbeId
                   ? kSecondaryExactWeaponWorldProbeStateIndex
                   : kExactWeaponWorldProbeStateIndex;
    }

    [[nodiscard]] inline constexpr std::uint64_t
    makeExactWeaponWorldProbeIdentity(
        const std::uint32_t sourceBodyId,
        const std::uint32_t probeId =
            kExactWeaponWorldProbeId) noexcept
    {
        const std::uint64_t identity =
            (static_cast<std::uint64_t>(sourceBodyId) << 32u) ^
            probeId;
        return identity != 0 ? identity : 1u;
    }

    /*
     * A sampled point's output ordinal is only a bounded storage slot. Bodies
     * can temporarily disappear or switch between weapon-root and source-local
     * bases without changing the collision generation, so using that ordinal as
     * physical identity can sweep one point from another point's old position.
     * The producer supplies this stable, generation-local identity alongside
     * each sample and the runtime resets sparse query history when the ordered
     * layout changes.
     */
    [[nodiscard]] inline constexpr std::uint64_t makeWeaponSampleIdentity(
        const std::uint32_t publicationIndex,
        const std::size_t pointIndex,
        const std::uint64_t basisIdentity,
        const bool sourceLocal) noexcept
    {
        std::uint64_t hash = kWeaponSampleLayoutHashSeed;
        const auto mix = [&hash](const std::uint64_t value) constexpr {
            hash ^= value;
            hash *= kWeaponSampleLayoutHashPrime;
        };
        mix(publicationIndex);
        mix(static_cast<std::uint64_t>(pointIndex));
        mix(basisIdentity);
        mix(sourceLocal ? 1u : 0u);
        return hash != 0 ? hash : 1u;
    }

    [[nodiscard]] inline constexpr std::uint64_t appendWeaponSampleLayoutIdentity(
        std::uint64_t layoutHash,
        const std::uint64_t sampleIdentity) noexcept
    {
        if (layoutHash == 0) {
            layoutHash = kWeaponSampleLayoutHashSeed;
        }
        layoutHash ^= sampleIdentity;
        layoutHash *= kWeaponSampleLayoutHashPrime;
        return layoutHash != 0 ? layoutHash : 1u;
    }

    template <class Point>
    struct SparseProbeQueryHistory
    {
        bool observedValid = false;
        Point previousObserved{};
        bool queryValid = false;
        Point lastQueryPosition{};
        std::uint64_t identity = 0;
    };

    /*
     * Observing a sparse probe is deliberately distinct from querying it.
     * Unadmitted probes keep the last position actually covered by a cast, so
     * their later turn sweeps the complete accumulated path. A changed physical
     * identity rebases both histories and cannot bridge unrelated hull points.
     */
    template <class Point>
    [[nodiscard]] inline bool observeSparseProbe(
        SparseProbeQueryHistory<Point>& history,
        const Point& position,
        const std::uint64_t identity) noexcept
    {
        const bool identityChanged =
            history.identity != 0 && history.identity != identity;
        if (!history.observedValid || identityChanged) {
            history = {};
            history.queryValid = true;
            history.lastQueryPosition = position;
        }
        history.observedValid = true;
        history.previousObserved = position;
        history.identity = identity;
        return identityChanged;
    }

    template <class Point>
    inline void commitSparseProbeQuery(
        SparseProbeQueryHistory<Point>& history,
        const Point& position,
        const std::uint64_t identity) noexcept
    {
        if (history.identity != 0 && history.identity != identity) {
            history = {};
        }
        history.queryValid = true;
        history.lastQueryPosition = position;
        history.identity = identity;
    }

    inline constexpr float
        kSparseProbeDiscontinuityMinDistanceGameUnits = 25.0f;
    inline constexpr float
        kSparseProbeMaxPlausibleSpeedGameUnitsPerSecond = 2500.0f;

    [[nodiscard]] inline float sparseProbeDiscontinuityDistance(
        const float deltaSeconds) noexcept
    {
        const float safeDelta =
            std::isfinite(deltaSeconds) && deltaSeconds > 0.0f
                ? deltaSeconds
                : (1.0f / 90.0f);
        return (std::max)(
            kSparseProbeDiscontinuityMinDistanceGameUnits,
            kSparseProbeMaxPlausibleSpeedGameUnitsPerSecond *
                safeDelta);
    }

    template <class Point>
    [[nodiscard]] inline bool sparseProbeObservationIsDiscontinuous(
        const SparseProbeQueryHistory<Point>& history,
        const Point& position,
        const std::uint64_t identity,
        const float deltaSeconds) noexcept
    {
        if (!history.observedValid ||
            history.identity == 0 ||
            history.identity != identity) {
            return false;
        }

        const float dx = position.x - history.previousObserved.x;
        const float dy = position.y - history.previousObserved.y;
        const float dz = position.z - history.previousObserved.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distanceSquared) ||
            distanceSquared < 0.0f) {
            return true;
        }
        const float limit =
            sparseProbeDiscontinuityDistance(deltaSeconds);
        return distanceSquared > limit * limit;
    }

    struct BoundedRotatingProbeWindow
    {
        std::size_t start = 0;
        std::size_t count = 0;
        std::size_t nextCursor = 0;
    };

    [[nodiscard]] inline constexpr BoundedRotatingProbeWindow
    resolveBoundedRotatingProbeWindow(
        const std::size_t cursor,
        const std::size_t availableProbeCount,
        const std::size_t probeBudget) noexcept
    {
        if (availableProbeCount == 0 || probeBudget == 0) {
            return {};
        }

        const std::size_t start = cursor % availableProbeCount;
        const std::size_t count =
            (std::min)(availableProbeCount, probeBudget);
        return {
            .start = start,
            .count = count,
            .nextCursor = (start + count) % availableProbeCount,
        };
    }

    [[nodiscard]] inline constexpr std::size_t
    boundedRotatingProbeIndex(
        const BoundedRotatingProbeWindow& window,
        const std::size_t offset,
        const std::size_t availableProbeCount) noexcept
    {
        if (availableProbeCount == 0 || offset >= window.count) {
            return availableProbeCount;
        }

        return (window.start + offset) % availableProbeCount;
    }

    /*
     * Cached/native evidence may own the current contact episode, but it only
     * describes one plane.  Critical extrema still have to sweep so sliding
     * along wall A can discover wall B.  Non-critical probes retain the
     * rotating acquisition budget only while there is no authoritative plane.
     */
    [[nodiscard]] inline constexpr bool shouldAdmitWeaponProbeSweep(
        const bool critical,
        const bool authoritativePlaneOwnsFrame,
        const bool admittedByRotation) noexcept
    {
        return critical ||
               (!authoritativePlaneOwnsFrame && admittedByRotation);
    }

    [[nodiscard]] inline bool signedSegmentCrossesPlane(
        const float startSignedDistance,
        const float endSignedDistance,
        const float planeSignedDistance = 0.0f) noexcept
    {
        if (!std::isfinite(startSignedDistance) ||
            !std::isfinite(endSignedDistance) ||
            !std::isfinite(planeSignedDistance)) {
            return false;
        }
        const float start =
            startSignedDistance - planeSignedDistance;
        const float end =
            endSignedDistance - planeSignedDistance;
        return (start > 0.0f && end <= 0.0f) ||
               (start < 0.0f && end >= 0.0f);
    }

    struct CrossReturnSweepCoverage
    {
        bool sparseEndpointSweepDetects = false;
        bool perFrameCriticalSweepDetects = false;
    };

    /*
     * A rotating sparse probe can cross a thin plane and return to the same
     * side before its next admitted cast.  Its long endpoint segment then
     * contains no crossing.  A critical per-frame sample observes the two
     * constituent segments and cannot lose that cross-and-return motion.
     */
    [[nodiscard]] inline CrossReturnSweepCoverage
    modelCrossReturnSweepCoverage(
        const float lastQuerySignedDistance,
        const float intermediateSignedDistance,
        const float returnedSignedDistance,
        const float planeSignedDistance = 0.0f) noexcept
    {
        return {
            .sparseEndpointSweepDetects =
                signedSegmentCrossesPlane(
                    lastQuerySignedDistance,
                    returnedSignedDistance,
                    planeSignedDistance),
            .perFrameCriticalSweepDetects =
                signedSegmentCrossesPlane(
                    lastQuerySignedDistance,
                    intermediateSignedDistance,
                    planeSignedDistance) ||
                signedSegmentCrossesPlane(
                    intermediateSignedDistance,
                    returnedSignedDistance,
                    planeSignedDistance),
        };
    }

    template <class Point>
    struct WeaponCorrectionPlane
    {
        Point normal{};
        float correctionDistance = 0.0f;
    };

    template <class Point>
    struct WeaponCorrectionManifold
    {
        static constexpr std::size_t kCapacity = 2;
        std::array<WeaponCorrectionPlane<Point>, kCapacity> planes{};
        std::size_t count = 0;
    };

    template <class Point>
    [[nodiscard]] inline float weaponCorrectionPointDot(
        const Point& lhs,
        const Point& rhs) noexcept
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    template <class Point>
    [[nodiscard]] inline bool weaponCorrectionPointIsFinite(
        const Point& point) noexcept
    {
        return std::isfinite(point.x) &&
               std::isfinite(point.y) &&
               std::isfinite(point.z);
    }

    template <class Point>
    [[nodiscard]] inline bool weaponCorrectionNormalsAreIndependent(
        const Point& lhs,
        const Point& rhs,
        const float parallelCosine = 0.985f) noexcept
    {
        if (!weaponCorrectionPointIsFinite(lhs) ||
            !weaponCorrectionPointIsFinite(rhs)) {
            return false;
        }
        const float lhsLengthSquared =
            weaponCorrectionPointDot(lhs, lhs);
        const float rhsLengthSquared =
            weaponCorrectionPointDot(rhs, rhs);
        if (!std::isfinite(lhsLengthSquared) ||
            !std::isfinite(rhsLengthSquared) ||
            lhsLengthSquared <= 1.0e-8f ||
            rhsLengthSquared <= 1.0e-8f) {
            return false;
        }
        const float alignment =
            weaponCorrectionPointDot(lhs, rhs) /
            std::sqrt(lhsLengthSquared * rhsLengthSquared);
        const float safeParallelCosine =
            std::clamp(
                std::isfinite(parallelCosine) ? parallelCosine : 0.985f,
                0.0f,
                1.0f);
        return std::isfinite(alignment) &&
               std::abs(alignment) < safeParallelCosine;
    }

    enum class WeaponCachedPlaneSlot : std::uint8_t
    {
        None = 0,
        Primary,
        Secondary,
    };

    [[nodiscard]] inline constexpr bool
    secondaryWeaponPlaneMayUseProbe(
        const std::uint32_t probeId,
        const bool exactAnchorValid) noexcept
    {
        return (!exactAnchorValid &&
                isSampledWeaponWorldProbeId(probeId)) ||
               (exactAnchorValid &&
                isExactWeaponWorldProbeId(probeId));
    }

    /*
     * Exact probe IDs belong to the persistent cache SLOT, not to transient
     * sweep-hit ordering.  The stronger of two first-frame corner hits may be
     * hit #1; normalizing on cache admission prevents both cached planes from
     * aliasing the secondary probe state on the following frame.
     */
    [[nodiscard]] inline constexpr std::uint32_t
    normalizeWeaponCachedPlaneProbeId(
        const WeaponCachedPlaneSlot slot,
        const std::uint32_t candidateProbeId,
        const bool exactAnchorValid) noexcept
    {
        if (!exactAnchorValid ||
            !isExactWeaponWorldProbeId(candidateProbeId)) {
            return candidateProbeId;
        }
        if (slot == WeaponCachedPlaneSlot::Primary) {
            return kExactWeaponWorldProbeId;
        }
        if (slot == WeaponCachedPlaneSlot::Secondary) {
            return kSecondaryExactWeaponWorldProbeId;
        }
        return candidateProbeId;
    }

    [[nodiscard]] inline constexpr WeaponCachedPlaneSlot
    selectWeaponCachedPlaneSlot(
        const bool primaryActive,
        const bool secondaryActive,
        const bool independentFromPrimary,
        const bool secondaryProbeEligible) noexcept
    {
        if (!primaryActive) {
            return WeaponCachedPlaneSlot::Primary;
        }
        if (!secondaryActive &&
            independentFromPrimary &&
            secondaryProbeEligible) {
            return WeaponCachedPlaneSlot::Secondary;
        }
        return WeaponCachedPlaneSlot::None;
    }

    [[nodiscard]] inline constexpr bool shouldRetainCachedWeaponPlane(
        const bool generationCurrent,
        const bool sampleLayoutCurrent,
        const bool probeResolved,
        const bool stillInContact) noexcept
    {
        return generationCurrent &&
               sampleLayoutCurrent &&
               probeResolved &&
               stillInContact;
    }

    /*
     * Admit at most two independent response planes. Near-parallel planes are
     * one constraint, so retain only the stronger distance. If a third plane
     * appears, replace the weakest bounded slot only when the new constraint is
     * stronger; this keeps cost deterministic and avoids additive duplicates.
     */
    template <class Point>
    [[nodiscard]] inline bool admitWeaponCorrectionPlane(
        WeaponCorrectionManifold<Point>& manifold,
        const Point& rawNormal,
        const float correctionDistance,
        const float parallelCosine = 0.985f) noexcept
    {
        if (!weaponCorrectionPointIsFinite(rawNormal) ||
            !std::isfinite(correctionDistance) ||
            correctionDistance <= 0.0f) {
            return false;
        }
        const float lengthSquared =
            weaponCorrectionPointDot(rawNormal, rawNormal);
        if (!std::isfinite(lengthSquared) ||
            lengthSquared <= 1.0e-8f) {
            return false;
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        const Point normal{
            rawNormal.x * inverseLength,
            rawNormal.y * inverseLength,
            rawNormal.z * inverseLength,
        };
        const float safeParallelCosine =
            std::clamp(
                std::isfinite(parallelCosine) ? parallelCosine : 0.985f,
                0.0f,
                1.0f);

        for (std::size_t index = 0;
             index < manifold.count && index < manifold.planes.size();
             ++index) {
            auto& existing = manifold.planes[index];
            const float alignment =
                weaponCorrectionPointDot(existing.normal, normal);
            if (std::abs(alignment) < safeParallelCosine) {
                continue;
            }
            // Opposed collinear constraints cannot form a useful corner from
            // correction vectors alone (their plane offsets are not retained).
            // Treat them as one bounded channel and keep the stronger witness.
            if (correctionDistance > existing.correctionDistance) {
                existing = {
                    .normal = normal,
                    .correctionDistance = correctionDistance,
                };
            }
            return true;
        }

        if (manifold.count < manifold.planes.size()) {
            manifold.planes[manifold.count++] = {
                .normal = normal,
                .correctionDistance = correctionDistance,
            };
            return true;
        }

        std::size_t weakest = 0;
        for (std::size_t index = 1; index < manifold.planes.size(); ++index) {
            if (manifold.planes[index].correctionDistance <
                manifold.planes[weakest].correctionDistance) {
                weakest = index;
            }
        }
        if (correctionDistance <=
            manifold.planes[weakest].correctionDistance) {
            return false;
        }
        manifold.planes[weakest] = {
            .normal = normal,
            .correctionDistance = correctionDistance,
        };
        return true;
    }

    /*
     * Project a shared translation into every admitted plane half-space. Two
     * deterministic passes are sufficient for orthogonal/ordinary concave
     * corners; four bounded passes also settle strongly oblique pairs without
     * creating an unbounded iterative solver in the frame hook.
     */
    template <class Point>
    [[nodiscard]] inline Point solveWeaponCorrectionManifold(
        const WeaponCorrectionManifold<Point>& manifold) noexcept
    {
        Point correction{};
        constexpr std::size_t kProjectionPasses = 4;
        for (std::size_t pass = 0;
             pass < kProjectionPasses;
             ++pass) {
            bool changed = false;
            for (std::size_t index = 0;
                 index < manifold.count && index < manifold.planes.size();
                 ++index) {
                const auto& plane = manifold.planes[index];
                const float deficit =
                    plane.correctionDistance -
                    weaponCorrectionPointDot(correction, plane.normal);
                if (!std::isfinite(deficit) || deficit <= 0.0001f) {
                    continue;
                }
                correction.x += plane.normal.x * deficit;
                correction.y += plane.normal.y * deficit;
                correction.z += plane.normal.z * deficit;
                changed = true;
            }
            if (!changed) {
                break;
            }
        }
        return weaponCorrectionPointIsFinite(correction) ?
                   correction :
                   Point{};
    }

    struct WorldContactChannels
    {
        bool handPushback = false;
        bool rightHandPushback = false;
        bool leftHandPushback = false;
        bool weaponWallCollision = false;

        [[nodiscard]] constexpr bool anyEnabled() const noexcept
        {
            return handPushback || weaponWallCollision;
        }
    };

    /*
     * Legacy layer-43 hands and layer-44 weapon hulls are both keyframed, so
     * their native manifold is the only one-way visual-stop witness available
     * when the dynamic proxy bank is not operational. Admit it only for the
     * free-hand probe channel. Reciprocal weapon-to-free-hand response is a
     * separate, direction-gated consumer of the original hand->weapon record;
     * it must not make arbitrary generated-hand records look like static world.
     */
    [[nodiscard]] inline constexpr bool
    shouldAdmitNativeVisualStopTarget(
        bool targetIsWorldSurface,
        bool targetIsGeneratedWeapon,
        bool /*targetIsGeneratedHand*/,
        bool includeHandProbes,
        bool includeWeaponProbes) noexcept
    {
        return targetIsWorldSurface ||
               (targetIsGeneratedWeapon && includeHandProbes &&
                   !includeWeaponProbes);
    }

    struct WeaponHandEntryFraction
    {
        bool valid = false;
        float fraction = 1.0f;
    };

    struct WeaponHandDirectionalMotion
    {
        bool valid = false;
        float weaponApproachSpeedGameUnits = 0.0f;
        float handApproachSpeedGameUnits = 0.0f;
        float relativeClosingSpeedGameUnits = 0.0f;
    };

    /*
     * A solver-driven hand proxy can report a tiny normal velocity while the
     * controller is visually stationary.  Letting that sub-half-unit noise
     * participate in dominance makes a slowly moving gun alternate between
     * hand and weapon authority.  Dead-band the signed hand velocity before
     * deriving both dominance and relative closing so the two calculations
     * cannot disagree about whether the hand moved.
     */
    [[nodiscard]] inline WeaponHandDirectionalMotion
    resolveWeaponHandDirectionalMotion(
        const float weaponNormalSpeedGameUnits,
        const float handNormalSpeedGameUnits,
        const float stationaryHandDeadbandGameUnits = 0.5f) noexcept
    {
        WeaponHandDirectionalMotion result{};
        if (!std::isfinite(weaponNormalSpeedGameUnits) ||
            !std::isfinite(handNormalSpeedGameUnits) ||
            !std::isfinite(stationaryHandDeadbandGameUnits)) {
            return result;
        }

        const float deadband =
            (std::max)(stationaryHandDeadbandGameUnits, 0.0f);
        const float stableHandNormalSpeed =
            std::abs(handNormalSpeedGameUnits) <= deadband
                ? 0.0f
                : handNormalSpeedGameUnits;
        result.valid = true;
        result.weaponApproachSpeedGameUnits =
            (std::max)(0.0f, -weaponNormalSpeedGameUnits);
        result.handApproachSpeedGameUnits =
            (std::max)(0.0f, stableHandNormalSpeed);
        result.relativeClosingSpeedGameUnits =
            -weaponNormalSpeedGameUnits + stableHandNormalSpeed;
        return result;
    }

    /*
     * Resolve the time at which a weapon sample moving toward a hand first
     * reaches the required center-to-surface clearance. If the first physics
     * manifold arrives one solve late and the preceding sample is already
     * contacting, retain that preceding pose (fraction zero). This is
     * conservative but prevents a low-frame-rate gun from tunnelling through
     * the hand merely because the exact clear-side sample was not reported.
     */
    [[nodiscard]] inline WeaponHandEntryFraction
    resolveWeaponHandEntryFraction(
        const float previousSignedDistance,
        const float currentSignedDistance,
        const float requiredClearance) noexcept
    {
        WeaponHandEntryFraction result{};
        if (!std::isfinite(previousSignedDistance) ||
            !std::isfinite(currentSignedDistance) ||
            !std::isfinite(requiredClearance) ||
            requiredClearance < 0.0f) {
            return result;
        }

        const float previousSeparation =
            previousSignedDistance - requiredClearance;
        const float currentSeparation =
            currentSignedDistance - requiredClearance;
        const float closingDistance =
            previousSignedDistance - currentSignedDistance;
        if (!(currentSeparation <= 0.0f) ||
            !(closingDistance > 0.0f) ||
            !std::isfinite(closingDistance)) {
            return result;
        }

        if (previousSeparation <= 0.0f) {
            result.valid = true;
            result.fraction = 0.0f;
            return result;
        }

        const float fraction = previousSeparation / closingDistance;
        if (!std::isfinite(fraction)) {
            return result;
        }
        result.valid = true;
        result.fraction = std::clamp(fraction, 0.0f, 1.0f);
        return result;
    }

    /*
     * A keyframed weapon may stop on a free hand only when continuous weapon
     * history provides a snap-free previous pose and the weapon is the side
     * actually closing the contact. This preserves the existing behavior when
     * the hand moves into a stationary gun: the hand stops and the gun stays
     * put, instead of both authority channels fighting one another.
     */
    [[nodiscard]] inline bool shouldAcquireWeaponHandStop(
        const bool previousClearPoseValid,
        const bool sourceWeaponCurrent,
        const bool targetHandFree,
        const bool /*firstContactEpisodeFrame*/,
        const bool evidenceCurrentFrame,
        const bool velocityEvidenceValid,
        const float weaponApproachSpeedGameUnits,
        const float handApproachSpeedGameUnits,
        const float relativeClosingSpeedGameUnits,
        const float minimumWeaponApproachSpeedGameUnits = 0.5f,
        const float dominanceEpsilonGameUnits = 0.15f) noexcept
    {
        if (!previousClearPoseValid || !sourceWeaponCurrent ||
            !targetHandFree ||
            !evidenceCurrentFrame ||
            !velocityEvidenceValid ||
            !std::isfinite(weaponApproachSpeedGameUnits) ||
            !std::isfinite(handApproachSpeedGameUnits) ||
            !std::isfinite(relativeClosingSpeedGameUnits)) {
            return false;
        }
        const float minimumSpeed =
            (std::max)(
                std::isfinite(minimumWeaponApproachSpeedGameUnits)
                    ? minimumWeaponApproachSpeedGameUnits
                    : 0.5f,
                0.0f);
        const float dominance =
            (std::max)(
                std::isfinite(dominanceEpsilonGameUnits)
                    ? dominanceEpsilonGameUnits
                    : 0.15f,
                0.0f);
        const float weaponApproach =
            (std::max)(weaponApproachSpeedGameUnits, 0.0f);
        const float handApproach =
            (std::max)(handApproachSpeedGameUnits, 0.0f);
        return relativeClosingSpeedGameUnits > minimumSpeed &&
               weaponApproach > minimumSpeed &&
               weaponApproach > handApproach + dominance;
    }

    [[nodiscard]] inline constexpr bool
    hasHeldObjectAuthorityForWeaponStop(
        const bool rockHandHoldingObject,
        const bool hostHandHoldingObject,
        const bool hostHandCollisionSuppressed) noexcept
    {
        // Host ownership is a stronger signal than ROCK's local hand state:
        // an external grab owner may intentionally omit native hand bodies.
        // Preserve the older collision-suppression veto as a conservative
        // compatibility signal for hosts that do not publish holding state.
        return rockHandHoldingObject || hostHandHoldingObject ||
               hostHandCollisionSuppressed;
    }

    [[nodiscard]] inline constexpr bool
    isHandPhysicallyFreeForWeaponStop(
        const bool weaponOwned,
        const bool visualReturnActive,
        const bool handDisabled,
        const bool heldObjectOwned) noexcept
    {
        // A visual return is presentation-only: both generated collider banks
        // remain live while the hand eases back from a released support/grab
        // pose.  Treating that blend as physical ownership opens a reciprocal
        // CCD hole exactly when the gun can be moved back into the offhand.
        // The winning weapon stop cancels the return writer before publishing
        // its blocked pose, so it is safe (and required) to ignore it here.
        // Likewise, a selection/pull transition may temporarily suppress the
        // hand's native contact producer without owning the physical hand.
        // Reciprocal CCD moves the weapon, not that hand, so only an actual
        // held-object owner blocks this path.  The caller folds ROCK and host
        // ownership (plus explicit host collision suppression) into that input,
        // separately from transient native-contact-production suppression.
        (void)visualReturnActive;
        return !weaponOwned && !handDisabled &&
               !heldObjectOwned;
    }

    struct WeaponHandContactEpisodeState
    {
        bool active = false;
        std::uint64_t generationKey = 0;
        std::uint32_t lastSeenFrame = 0;
    };

    [[nodiscard]] inline constexpr bool
    isAuthoritativeWeaponHandEvidenceLayer(
        const std::uint32_t observedLayer,
        const std::uint32_t authoritativeLayer) noexcept
    {
        return observedLayer == authoritativeLayer;
    }

    struct WeaponHandContactEpisodeObservation
    {
        WeaponHandContactEpisodeState next{};
        bool freshEntry = false;
    };

    [[nodiscard]] inline constexpr
        WeaponHandContactEpisodeObservation
        observeWeaponHandContactEpisode(
            const WeaponHandContactEpisodeState prior,
            const bool seenCurrentFrame,
            const std::uint64_t generationKey,
            const std::uint32_t currentFrame,
            const std::uint32_t separationGapFrames = 2u) noexcept
    {
        const bool sameEpisode =
            prior.active && generationKey != 0 &&
            prior.generationKey == generationKey &&
            static_cast<std::uint32_t>(
                currentFrame - prior.lastSeenFrame) <=
                separationGapFrames;
        if (seenCurrentFrame) {
            return {
                { true, generationKey, currentFrame },
                !sameEpisode,
            };
        }
        return { sameEpisode ? prior
                             : WeaponHandContactEpisodeState{},
                 false };
    }

    // A generated weapon can move every frame. Its fresh manifold may stop a
    // free hand, but its contact point must never become a static cached plane.
    [[nodiscard]] inline constexpr bool
    shouldCacheNativeVisualStopPlane(
        bool targetIsGeneratedWeapon) noexcept
    {
        return !targetIsGeneratedWeapon;
    }

    // Free-hand presentation and equipped-weapon collision share this runtime
    // but are separate authority channels. The solver-backed dynamic hand
    // proxies supersede only the legacy hand-presentation channel; they must
    // never turn off the independent equipped-weapon/world channel.
    [[nodiscard]] inline constexpr WorldContactChannels
    resolveWorldContactChannels(
        bool softContactWorldEnabled,
        bool handWorldPushbackEnabled,
        bool rightDynamicHandWorldStopOperational,
        bool leftDynamicHandWorldStopOperational,
        bool rightHandWeaponOwned,
        bool leftHandWeaponOwned,
        bool weaponCollisionEnabled,
        bool weaponStaticWorldEnabled,
        bool worldReady,
        bool menuBlocked) noexcept
    {
        if (!worldReady || menuBlocked) {
            return {};
        }

        const bool baseHandPushback =
            softContactWorldEnabled &&
            handWorldPushbackEnabled;
        const bool rightHandPushback =
            baseHandPushback &&
            !rightDynamicHandWorldStopOperational &&
            !rightHandWeaponOwned;
        const bool leftHandPushback =
            baseHandPushback &&
            !leftDynamicHandWorldStopOperational &&
            !leftHandWeaponOwned;
        return {
            .handPushback =
                rightHandPushback || leftHandPushback,
            .rightHandPushback = rightHandPushback,
            .leftHandPushback = leftHandPushback,
            .weaponWallCollision =
                weaponCollisionEnabled &&
                weaponStaticWorldEnabled,
        };
    }

    /*
     * fWorldMaxCorrection is an acquisition/discontinuity guard. It is large
     * enough to reject bogus first witnesses, but it cannot also be the travel
     * limit of an already validated hand stop plane: locomotion would carry
     * the controller beyond that fixed distance and the hand would then pass
     * through the wall.
     *
     * A cached plane may retain the full required normal response for either
     * authority channel. A weapon sphere cast is also a validated entry-plane
     * witness, so it must clear its complete measured crossing in that frame;
     * otherwise the rendered gun can finish on the far side of a thin wall and
     * lose the episode after sparse query history advances. Other fresh evidence
     * remains capped. The caller subsequently applies the shared directional arm
     * reach limit and the independent 256-gu absolute safety ceiling.
     */
    [[nodiscard]] inline float worldContactCorrectionLimit(
        bool cachedPlane,
        bool validatedWeaponSweep,
        float requiredCorrection,
        float acquisitionLimit)
    {
        const float safeAcquisitionLimit =
            std::isfinite(acquisitionLimit) && acquisitionLimit > 0.0f ?
                acquisitionLimit :
                0.0f;
        if ((!cachedPlane && !validatedWeaponSweep) ||
            !std::isfinite(requiredCorrection) ||
            requiredCorrection <= safeAcquisitionLimit) {
            return safeAcquisitionLimit;
        }

        return requiredCorrection;
    }

    [[nodiscard]] inline bool shouldRejectImplausibleFirstWorldWitness(
        const bool previouslyInContact,
        const bool validatedWeaponSweep,
        const float penetration,
        const float acquisitionLimit) noexcept
    {
        if (previouslyInContact) {
            return false;
        }
        if (!std::isfinite(penetration)) {
            return true;
        }
        const float safeAcquisitionLimit =
            std::isfinite(acquisitionLimit) && acquisitionLimit > 0.0f
                ? acquisitionLimit
                : 0.0f;
        return !validatedWeaponSweep &&
               penetration > safeAcquisitionLimit;
    }

    /*
     * Native manifold evidence can arrive in the same frame as a fast weapon
     * hull crossing. A deep first native witness is intentionally not trusted,
     * but it must be discarded before it suppresses the validated sweep and
     * rebases that sweep's sparse history.
     */
    [[nodiscard]] inline bool shouldDeferNativeWeaponFirstWitnessToSweep(
        const bool weaponSweepsAvailable,
        const bool candidateValid,
        const bool candidateSourceIsWeapon,
        const bool previouslyInContact,
        const float penetration,
        const float acquisitionLimit) noexcept
    {
        return weaponSweepsAvailable &&
               candidateValid &&
               candidateSourceIsWeapon &&
               shouldRejectImplausibleFirstWorldWitness(
                   previouslyInContact,
                   false,
                   penetration,
                   acquisitionLimit);
    }
}
