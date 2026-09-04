#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace heisenberg::npc_injection_policy
{
    inline constexpr std::uint32_t kPlayerRefFormID = 0x00000014u;
    inline constexpr std::string_view kCanonicalDiseaseCureEditorID =
        "SS2_C2_DiseaseCureAllKnown";
    inline constexpr std::uint32_t kCanonicalDiseaseCureLocalFormID =
        0x0004B384u;
    inline constexpr float kMaximumHeldProbeRadiusGameUnits = 20.0f;
    inline constexpr float kMaximumHeldProbeOffsetGameUnits = 45.0f;

    // RE::LIFE_STATE values are kept as integers at this pure-policy seam so
    // the policy remains independent of CommonLibF4 headers.
    inline constexpr std::uint32_t kLifeStateAlive = 0u;
    inline constexpr std::uint32_t kLifeStateDying = 1u;
    inline constexpr std::uint32_t kLifeStateDead = 2u;
    inline constexpr std::uint32_t kLifeStateRecycle = 5u;
    inline constexpr std::uint32_t kLifeStateEssentialDown = 7u;
    inline constexpr std::uint32_t kLifeStateBleedout = 8u;

    struct CompanionEvidence
    {
        bool commandedFlag = false;
        bool commandedByPlayer = false;
        bool inPlayerCommandList = false;
        bool inCurrentCompanionFaction = false;
    };

    [[nodiscard]] constexpr bool IsCurrentPlayerCompanion(
        const CompanionEvidence& evidence) noexcept
    {
        return evidence.commandedFlag ||
               evidence.commandedByPlayer ||
               evidence.inPlayerCommandList ||
               evidence.inCurrentCompanionFaction;
    }

    [[nodiscard]] constexpr bool IsWoundedCompanionState(
        std::uint32_t lifeState,
        bool inBleedoutAnimation) noexcept
    {
        // Terminal states always win over contradictory animation evidence.
        if (lifeState == kLifeStateDying ||
            lifeState == kLifeStateDead ||
            lifeState == kLifeStateRecycle) {
            return false;
        }

        // Essential companions use dedicated down/bleedout life states. Some
        // runtime frames still report Alive while the bleedout animation graph
        // is authoritative, so accept that narrowly as a second source.
        return lifeState == kLifeStateEssentialDown ||
               lifeState == kLifeStateBleedout ||
               (lifeState == kLifeStateAlive && inBleedoutAnimation);
    }

    [[nodiscard]] constexpr bool AllowsCompanionStimpakInjection(
        bool featureEnabled,
        bool isStimpak,
        const CompanionEvidence& evidence,
        std::uint32_t lifeState,
        bool inBleedoutAnimation) noexcept
    {
        return featureEnabled &&
               isStimpak &&
               IsCurrentPlayerCompanion(evidence) &&
               IsWoundedCompanionState(
                   lifeState,
                   inBleedoutAnimation);
    }

    struct Point3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct ProbeSphere
    {
        Point3 center{};
        float radius = 0.0f;
    };

    struct CapsuleSegment
    {
        Point3 start{};
        Point3 end{};
        float radius = 0.0f;
    };

    struct ContactProbeSet
    {
        // Probe zero is always the current wand tip. Probe one is the optional
        // visible held-object bound, but only while that bound remains small,
        // finite, and plausibly attached to the hand.
        std::array<ProbeSphere, 2> probes{};
        std::size_t count = 1;
    };

    [[nodiscard]] inline bool IsFinite(Point3 point) noexcept
    {
        return std::isfinite(point.x) &&
               std::isfinite(point.y) &&
               std::isfinite(point.z);
    }

    [[nodiscard]] inline float SquaredDistance(
        Point3 lhs,
        Point3 rhs) noexcept
    {
        const float dx = lhs.x - rhs.x;
        const float dy = lhs.y - rhs.y;
        const float dz = lhs.z - rhs.z;
        return dx * dx + dy * dy + dz * dz;
    }

    [[nodiscard]] inline ContactProbeSet MakeContactProbeSet(
        Point3 wandTip,
        bool hasHeldBound,
        Point3 heldBoundCenter,
        float heldBoundRadius) noexcept
    {
        ContactProbeSet result{};
        result.probes[0] = ProbeSphere{ .center = wandTip, .radius = 0.0f };

        const float maximumOffsetSquared =
            kMaximumHeldProbeOffsetGameUnits *
            kMaximumHeldProbeOffsetGameUnits;
        const bool validHeldBound =
            hasHeldBound &&
            IsFinite(wandTip) &&
            IsFinite(heldBoundCenter) &&
            std::isfinite(heldBoundRadius) &&
            heldBoundRadius >= 0.0f &&
            heldBoundRadius <= kMaximumHeldProbeRadiusGameUnits &&
            SquaredDistance(wandTip, heldBoundCenter) <=
                maximumOffsetSquared;
        if (validHeldBound) {
            result.probes[1] = ProbeSphere{
                .center = heldBoundCenter,
                .radius = heldBoundRadius,
            };
            result.count = 2;
        }
        return result;
    }

    [[nodiscard]] inline float PointToSegmentDistance(
        Point3 point,
        Point3 start,
        Point3 end) noexcept
    {
        if (!IsFinite(point) || !IsFinite(start) || !IsFinite(end)) {
            return std::numeric_limits<float>::infinity();
        }

        const Point3 segment{
            end.x - start.x,
            end.y - start.y,
            end.z - start.z,
        };
        const float lengthSquared =
            segment.x * segment.x +
            segment.y * segment.y +
            segment.z * segment.z;
        float t = 0.0f;
        if (std::isfinite(lengthSquared) && lengthSquared > 0.000001f) {
            const Point3 fromStart{
                point.x - start.x,
                point.y - start.y,
                point.z - start.z,
            };
            t = std::clamp(
                (fromStart.x * segment.x +
                    fromStart.y * segment.y +
                    fromStart.z * segment.z) /
                    lengthSquared,
                0.0f,
                1.0f);
        }

        const Point3 nearest{
            start.x + segment.x * t,
            start.y + segment.y * t,
            start.z + segment.z * t,
        };
        const float distanceSquared = SquaredDistance(point, nearest);
        return std::isfinite(distanceSquared) && distanceSquared >= 0.0f ?
                   std::sqrt(distanceSquared) :
                   std::numeric_limits<float>::infinity();
    }

    [[nodiscard]] inline float ProbeToCapsuleDistance(
        const ProbeSphere& probe,
        const CapsuleSegment& capsule) noexcept
    {
        const float centerlineDistance = PointToSegmentDistance(
            probe.center,
            capsule.start,
            capsule.end);
        if (!std::isfinite(centerlineDistance) ||
            !std::isfinite(probe.radius) ||
            !std::isfinite(capsule.radius) ||
            probe.radius < 0.0f || capsule.radius < 0.0f) {
            return std::numeric_limits<float>::infinity();
        }
        return (std::max)(
            0.0f,
            centerlineDistance - probe.radius - capsule.radius);
    }

    [[nodiscard]] inline float MinimumProbeDistanceToCapsule(
        const ContactProbeSet& probes,
        const CapsuleSegment& capsule) noexcept
    {
        float nearest = std::numeric_limits<float>::infinity();
        const std::size_t count =
            (std::min)(probes.count, probes.probes.size());
        for (std::size_t i = 0; i < count; ++i) {
            nearest = (std::min)(
                nearest,
                ProbeToCapsuleDistance(probes.probes[i], capsule));
        }
        return nearest;
    }

    struct HostedDispatchGuard
    {
        std::uint64_t committedGrabTraceId = 0;
    };

    [[nodiscard]] constexpr bool AllowsHostedDispatch(
        const HostedDispatchGuard& guard,
        std::uint64_t grabTraceId) noexcept
    {
        return grabTraceId != 0 &&
               guard.committedGrabTraceId != grabTraceId;
    }

    constexpr void MarkHostedDispatchCommitted(
        HostedDispatchGuard& guard,
        std::uint64_t grabTraceId) noexcept
    {
        if (grabTraceId != 0) {
            guard.committedGrabTraceId = grabTraceId;
        }
    }

    [[nodiscard]] constexpr bool IsEligibleNpcTarget(
        std::uintptr_t candidateIdentity,
        std::uintptr_t playerIdentity,
        std::uint32_t candidateFormID,
        bool isDead) noexcept
    {
        return candidateIdentity != 0u &&
               candidateIdentity != playerIdentity &&
               candidateFormID != kPlayerRefFormID &&
               !isDead;
    }

    [[nodiscard]] constexpr bool IsCanonicalDiseaseCureIdentity(
        bool hasSs2SourceFile,
        std::string_view editorID,
        std::uint32_t localFormID = 0u) noexcept
    {
        return hasSs2SourceFile &&
               (editorID == kCanonicalDiseaseCureEditorID ||
                localFormID == kCanonicalDiseaseCureLocalFormID);
    }

    [[nodiscard]] constexpr bool AllowsPlayerConsumption(
        bool isDiseaseCure) noexcept
    {
        return !isDiseaseCure;
    }

    template <class Consume>
    bool TryPlayerConsumption(bool isDiseaseCure, Consume&& consume)
    {
        if (!AllowsPlayerConsumption(isDiseaseCure)) {
            return false;
        }
        return static_cast<bool>(std::forward<Consume>(consume)());
    }
}
