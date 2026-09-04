#pragma once

/*
 * Internal dynamic-hand collision telemetry contract. This is deliberately
 * separate from ROCKProviderApi.h: ROCK remains on provider API V1 and a later
 * API addition can copy the stable, engine-agnostic subset without exposing
 * runtime objects, pointers, or thread-owned state.
 *
 * The runtime publishes one fixed-capacity snapshot per main frame. All vector
 * names state their coordinate space and all distances/speeds state their
 * units. Proxy body IDs are transient diagnostics, never ownership handles.
 */

#include "physics-interaction/hand/DynamicHandTwinTargets.h"
#include "physics-interaction/hand/HandColliderTypes.h"

#include "RE/NetImmerse/NiPoint.h"
#include "RE/NetImmerse/NiTransform.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rock::dynamic_hand_collision_telemetry
{
    inline constexpr std::size_t kPalmSlot = 0;
    inline constexpr std::size_t kFirstForearmSlot =
        hand_collider_semantics::kHandColliderBodyCountPerHand;
    inline constexpr std::size_t kForearmSlot = kFirstForearmSlot;
    inline constexpr std::size_t kBodiesPerHand = kFirstForearmSlot + dynamic_hand_twin::kForearmSegmentCountPerHand;
    inline constexpr std::uint32_t kInvalidBodyId = 0x7FFF'FFFF;

    enum class TwinRole : std::uint8_t
    {
        PalmAnchor = 0,
        PalmFace,
        PalmBack,
        PalmHeel,
        ThumbPad,
        ThumbBase,
        ThumbMiddle,
        ThumbTip,
        IndexBase,
        IndexMiddle,
        IndexTip,
        MiddleBase,
        MiddleMiddle,
        MiddleTip,
        RingBase,
        RingMiddle,
        RingTip,
        PinkyBase,
        PinkyMiddle,
        PinkyTip,
        Forearm,
    };

    [[nodiscard]] constexpr TwinRole roleForBodyIndex(std::size_t bodyIndex) noexcept
    {
        return bodyIndex < kBodiesPerHand ?
            static_cast<TwinRole>(bodyIndex) :
            TwinRole::PalmAnchor;
    }

    [[nodiscard]] constexpr const char* roleCode(TwinRole role) noexcept
    {
        switch (role) {
        case TwinRole::PalmAnchor:
            return "PANC";
        case TwinRole::PalmFace:
            return "PFAC";
        case TwinRole::PalmBack:
            return "PBAK";
        case TwinRole::PalmHeel:
            return "PHEL";
        case TwinRole::ThumbPad:
            return "TPAD";
        case TwinRole::ThumbBase:
            return "TBAS";
        case TwinRole::ThumbMiddle:
            return "TMID";
        case TwinRole::ThumbTip:
            return "TTIP";
        case TwinRole::IndexBase:
            return "IBAS";
        case TwinRole::IndexMiddle:
            return "IMID";
        case TwinRole::IndexTip:
            return "ITIP";
        case TwinRole::MiddleBase:
            return "MBAS";
        case TwinRole::MiddleMiddle:
            return "MMID";
        case TwinRole::MiddleTip:
            return "MTIP";
        case TwinRole::RingBase:
            return "RBAS";
        case TwinRole::RingMiddle:
            return "RMID";
        case TwinRole::RingTip:
            return "RTIP";
        case TwinRole::PinkyBase:
            return "PBAS";
        case TwinRole::PinkyMiddle:
            return "PMID";
        case TwinRole::PinkyTip:
            return "PTIP";
        case TwinRole::Forearm:
            return "FARM";
        }
        return "UNKN";
    }

    static_assert(static_cast<std::size_t>(TwinRole::Forearm) + 1 == kBodiesPerHand);
    static_assert(kBodiesPerHand <= 32u, "dynamic hand telemetry contact masks are 32-bit fixed capacity");
    static_assert(
        static_cast<std::size_t>(TwinRole::PinkyTip) ==
            static_cast<std::size_t>(
                hand_collider_semantics::HandColliderRole::PinkyTip),
        "dynamic twin role order must match hand collider semantic order");

    struct TwinSample
    {
        TwinRole role{ TwinRole::PalmAnchor };
        std::uint32_t bodyId{ kInvalidBodyId };
        std::uint64_t physicsSampleSequence = 0;

        RE::NiTransform publishedTargetWorld{};
        RE::NiPoint3 requestedTargetWorldGame{};
        RE::NiPoint3 commandedTargetWorldGame{};
        RE::NiPoint3 liveBodyWorldGame{};
        RE::NiPoint3 solverResidualWorldGame{};
        RE::NiPoint3 requestedGapWorldGame{};
        RE::NiPoint3 contactDeviationWorldGame{};
        RE::NiPoint3 handTargetCorrectionWorldGame{};
        RE::NiPoint3 targetVelocityWorldGameUnitsPerSecond{};

        float lengthGameUnits = 0.0f;
        float radiusGameUnits = 0.0f;
        float convexRadiusGameUnits = 0.0f;
        float solverResidualGameUnits = 0.0f;
        float requestedGapGameUnits = 0.0f;
        float contactDeviationGameUnits = 0.0f;
        float handTargetCorrectionGameUnits = 0.0f;
        float handTargetResponseScale = 1.0f;
        float approachSpeedGameUnitsPerSecond = 0.0f;
        float physicsDeltaSeconds = 0.0f;

        bool bodyCreated = false;
        bool publishedTargetValid = false;
        bool physicsSampleValid = false;
        bool targetVelocityValid = false;
        bool contactActive = false;
        bool recoveryTeleport = false;
    };

    struct HandSample
    {
        std::array<TwinSample, kBodiesPerHand> twins{};
        RE::NiPoint3 combinedContactDeviationWorldGame{};
        RE::NiPoint3 appliedVisualDeviationWorldGame{};

        std::uint64_t contactEntrySequence = 0;
        std::uint32_t contactMask = 0;
        std::uint32_t contactCount = 0;
        std::uint32_t entryContactMask = 0;
        float combinedContactDeviationGameUnits = 0.0f;
        float appliedVisualDeviationGameUnits = 0.0f;
        float contactEntryApproachSpeedGameUnitsPerSecond = 0.0f;
        float teleportRecoverySecondsRemaining = 0.0f;

        bool isLeft = false;
        bool handDisabled = false;
        bool ownedByStrongerSystem = false;
        bool visualAuthorityAvailable = false;
        bool worldStopOperational = false;
        bool visualActive = false;
        bool anyContact = false;
    };

    struct Snapshot
    {
        std::array<HandSample, 2> hands{};
        std::uint64_t updateSequence = 0;
        bool runtimeEnabled = false;
        bool worldReady = false;
        bool menuBlocked = false;
        bool physicsWritesAllowed = false;
        bool transitionCollisionSuppressed = false;
    };

    struct HapticPulse
    {
        bool fire = false;
        bool isLeft = false;
        float intensity = 0.0f;
        float approachSpeedGameUnitsPerSecond = 0.0f;
        std::uint64_t contactEntrySequence = 0;
        std::uint32_t contactMask = 0;
    };

    struct HapticEvents
    {
        std::array<HapticPulse, 2> hands{};
    };
}
