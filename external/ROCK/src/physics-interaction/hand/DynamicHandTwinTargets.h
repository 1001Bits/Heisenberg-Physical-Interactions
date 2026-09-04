#pragma once

#include "physics-interaction/hand/HandColliderTypes.h"

#include "RE/NetImmerse/NiTransform.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rock::dynamic_hand_twin
{
    inline constexpr std::size_t kForearmSegmentCountPerHand = 1;

    /*
     * Per-frame publication from HandBoneColliderSet for the complete dynamic
     * hand twin set: one fixed slot for every production hand collider role.
     * Each slot carries the EXACT frame and dimensions used by its keyframed
     * counterpart, so the solver-responsive proxy cannot leave uncovered
     * palm faces or base/middle phalanges while the contact-evidence set says
     * the hand touched the weapon. Main-thread only: written by
     * HandBoneColliderSet::update and consumed by DynamicHandCollisionRuntime
     * in the same frame loop.
     */
    struct TwinSlotFrame
    {
        bool valid = false;
        RE::NiTransform target{};
        float length = 0.0f;
        float radius = 0.0f;
        float convexRadius = 0.0f;
        // Palm/fingertip proxies map 1:1 to the hand target. The merged
        // forearm proxy publishes its pose-derived IK leverage correction.
        float handTargetResponseScale = 1.0f;
    };

    struct TwinTargets
    {
        std::array<
            TwinSlotFrame,
            hand_collider_semantics::kHandColliderBodyCountPerHand>
            roles{};
        std::uint64_t updateCounter = 0;
        // Changes only when the owning keyframed collider set is rebuilt for
        // a real source/tuning/lifetime reason; never changes for live pose.
        std::uint64_t geometryGeneration = 0;

        [[nodiscard]] TwinSlotFrame& forRole(
            hand_collider_semantics::HandColliderRole role)
        {
            return roles[static_cast<std::size_t>(role)];
        }

        [[nodiscard]] const TwinSlotFrame& forRole(
            hand_collider_semantics::HandColliderRole role) const
        {
            return roles[static_cast<std::size_t>(role)];
        }
    };

    static_assert(
        static_cast<std::size_t>(
            hand_collider_semantics::HandColliderRole::PinkyTip) +
                1u ==
            hand_collider_semantics::kHandColliderBodyCountPerHand,
        "dynamic hand role slots require the contiguous semantic role table");

    /*
     * Main-thread publication from BodyBoneColliderSet. These are the exact
     * merged ForeArm1->Hand frame derived from all three tuned production
     * keyframed segments (ForeArm1->2, ForeArm2->3, and ForeArm3->Hand).
     * DynamicHandCollisionRuntime consumes it after BodyBoneColliderSet::update
     * in the same frame, so the forearm twin never reads dynamic Havok bodies
     * back into intent.
     */
    struct ForearmTwinTargets
    {
        std::array<TwinSlotFrame, kForearmSegmentCountPerHand> right{};
        std::array<TwinSlotFrame, kForearmSegmentCountPerHand> left{};
        std::uint64_t updateCounter = 0;
        std::uint64_t geometryGeneration = 0;

        [[nodiscard]] const std::array<TwinSlotFrame, kForearmSegmentCountPerHand>& forHand(bool isLeft) const
        {
            return isLeft ? left : right;
        }
    };

    /*
     * hFRIK changes live forearm node spacing while solving animation/IK.
     * That spacing is pose, not collider topology. Preserve the current rigid
     * target and response scale, but always publish the dimensions captured by
     * the owning BodyBoneColliderSet generation.
     */
    inline void applyCanonicalSlotDimensions(
        TwinSlotFrame& live,
        const TwinSlotFrame& canonical)
    {
        if (!live.valid) {
            return;
        }
        if (!canonical.valid || canonical.length <= 0.0f || canonical.radius <= 0.0f || canonical.convexRadius < 0.0f) {
            live.valid = false;
            return;
        }
        live.length = canonical.length;
        live.radius = canonical.radius;
        live.convexRadius = canonical.convexRadius;
    }

    inline void applyCanonicalHandDimensions(
        TwinTargets& liveTargets,
        const TwinTargets& canonicalTargets)
    {
        for (std::size_t index = 0; index < liveTargets.roles.size(); ++index) {
            applyCanonicalSlotDimensions(
                liveTargets.roles[index],
                canonicalTargets.roles[index]);
        }
    }

    inline void applyCanonicalForearmDimensions(
        ForearmTwinTargets& liveTargets,
        const ForearmTwinTargets& canonicalTargets)
    {
        for (std::size_t index = 0; index < kForearmSegmentCountPerHand; ++index) {
            applyCanonicalSlotDimensions(liveTargets.right[index], canonicalTargets.right[index]);
            applyCanonicalSlotDimensions(liveTargets.left[index], canonicalTargets.left[index]);
        }
    }
}
