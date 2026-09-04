#include "physics-interaction/collision/CollisionLayerPolicy.h"
#include "physics-interaction/contact/SoftContactPolicy.h"
#include "physics-interaction/debug/SkeletonBoneDebugMath.h"
#include "physics-interaction/hand/DynamicHandCollisionAuthorityPolicy.h"
#include "physics-interaction/hand/DynamicHandCollisionFeedbackPolicy.h"
#include "physics-interaction/hand/DynamicHandCollisionStabilityPolicy.h"
#include "physics-interaction/hand/DynamicHandCollisionTelemetry.h"
#include "physics-interaction/hand/DynamicHandTwinTargets.h"
#include "physics-interaction/hand/HandColliderTypes.h"
#include "physics-interaction/visual/PreAuthorityHandFramePolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <string_view>

namespace
{
    struct TestPoint
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    bool Near(float lhs, float rhs, float epsilon = 1.0e-4f)
    {
        return std::abs(lhs - rhs) <= epsilon;
    }

    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    namespace semantics = rock::hand_collider_semantics;
    namespace authority = rock::dynamic_hand_collision_authority;
    namespace stability = rock::dynamic_hand_collision_stability;
    namespace telemetry = rock::dynamic_hand_collision_telemetry;
    namespace twins = rock::dynamic_hand_twin;
    namespace preauth = rock::pre_authority_hand_frame_policy;

    Require(
        preauth::isTrueCleanPass1(preauth::Provenance::CleanPass1) &&
            !preauth::isTrueCleanPass1(
                preauth::Provenance::SkinnedFallback) &&
            preauth::isAvailable(
                preauth::Provenance::SkinnedFallback),
        "only true FRIK pass-1 provenance may drive hand collision while the skinned fallback remains available to compatibility consumers");

    Require(
        semantics::kHandColliderBodyCountPerHand == 20u,
        "the production hand semantic table must retain all twenty roles");
    Require(
        telemetry::kFirstForearmSlot ==
            semantics::kHandColliderBodyCountPerHand,
        "the forearm may begin only after every production hand role");
    Require(
        telemetry::kBodiesPerHand ==
            semantics::kHandColliderBodyCountPerHand +
                twins::kForearmSegmentCountPerHand,
        "the fixed dynamic bank must contain twenty hand roles plus forearm");
    Require(
        telemetry::kBodiesPerHand <= 32u,
        "every fixed dynamic body must fit in the telemetry contact mask");

    const auto productionForearmCount = std::count_if(
        rock::skeleton_bone_debug_math::kStandardBodyColliderDescriptors.begin(),
        rock::skeleton_bone_debug_math::kStandardBodyColliderDescriptors.end(),
        [](const auto& descriptor) {
            return descriptor.role ==
                   rock::skeleton_bone_debug_math::BoneColliderRole::
                       ForearmSegment;
        });
    Require(
        productionForearmCount == 4,
        "retiring the hosted visual-stop forearm twin must leave both independent production forearm segments per arm intact");
    const auto productionBodyMask =
        rock::collision_layer_policy::buildRockBodyExpectedMask(true);
    Require(
        rock::collision_layer_policy::maskEnablesLayer(
            productionBodyMask,
            rock::collision_layer_policy::FO4_LAYER_STATIC) &&
            rock::collision_layer_policy::maskEnablesLayer(
                productionBodyMask,
                rock::collision_layer_policy::FO4_LAYER_ANIMSTATIC),
        "the independent production forearm/body colliders must retain static-world collision");
    const auto cleanFrameFailureFallback =
        rock::soft_contact_policy::resolveWorldContactChannels(
            true,
            true,
            false,
            true,
            false,
            false,
            true,
            true,
            true,
            false);
    Require(
        cleanFrameFailureFallback.rightHandPushback &&
            !cleanFrameFailureFallback.leftHandPushback,
        "a hosted clean-frame failure must re-enable legacy pushback only for the dynamic hand that failed closed");

    twins::TwinTargets live{};
    twins::TwinTargets canonical{};
    std::set<std::string_view> roleCodes;
    for (std::size_t index = 0;
         index < semantics::kHandColliderRoles.size();
         ++index) {
        const auto semanticRole = semantics::kHandColliderRoles[index];
        Require(
            static_cast<std::size_t>(semanticRole) == index,
            "the semantic role table must stay contiguous for fixed slots");
        Require(
            static_cast<std::size_t>(telemetry::roleForBodyIndex(index)) ==
                index,
            "telemetry role identity must match the production hand slot");
        Require(
            roleCodes
                .insert(telemetry::roleCode(
                    telemetry::roleForBodyIndex(index)))
                .second,
            "every full-hand dynamic role needs a unique diagnostic code");

        auto& liveSlot = live.forRole(semanticRole);
        liveSlot.valid = true;
        liveSlot.length = -1.0f;
        liveSlot.radius = -1.0f;
        liveSlot.convexRadius = -1.0f;

        auto& canonicalSlot = canonical.forRole(semanticRole);
        canonicalSlot.valid = true;
        canonicalSlot.length = 10.0f + static_cast<float>(index);
        canonicalSlot.radius = 1.0f + static_cast<float>(index) * 0.1f;
        canonicalSlot.convexRadius =
            0.1f + static_cast<float>(index) * 0.01f;
    }

    twins::applyCanonicalHandDimensions(live, canonical);
    for (std::size_t index = 0;
         index < semantics::kHandColliderRoles.size();
         ++index) {
        const auto role = semantics::kHandColliderRoles[index];
        const auto& liveSlot = live.forRole(role);
        const auto& canonicalSlot = canonical.forRole(role);
        Require(liveSlot.valid, "a valid canonical full-hand role must remain published");
        Require(
            liveSlot.length == canonicalSlot.length &&
                liveSlot.radius == canonicalSlot.radius &&
                liveSlot.convexRadius == canonicalSlot.convexRadius,
            "every live role must inherit its own generation-stable dimensions");
    }

    auto invalidCanonical = canonical;
    invalidCanonical.forRole(semantics::HandColliderRole::IndexMiddle).valid =
        false;
    twins::applyCanonicalHandDimensions(live, invalidCanonical);
    Require(
        !live.forRole(semantics::HandColliderRole::IndexMiddle).valid,
        "an unavailable canonical role must invalidate only that dynamic slot");
    Require(
        live.forRole(semantics::HandColliderRole::IndexBase).valid &&
            live.forRole(semantics::HandColliderRole::IndexTip).valid,
        "one missing phalanx must not erase its neighboring fixed roles");

    Require(
        telemetry::roleForBodyIndex(telemetry::kForearmSlot) ==
            telemetry::TwinRole::Forearm,
        "the final fixed slot must identify the merged forearm");
    Require(
        std::string_view(telemetry::roleCode(
            telemetry::roleForBodyIndex(telemetry::kForearmSlot))) ==
            "FARM",
        "forearm telemetry must retain a stable diagnostic code");

    Require(
        !authority::weaponOwnsHand({ .isLeft = false }),
        "an unrelated free hand must retain dynamic collision feedback");
    Require(
        authority::weaponOwnsHand({
            .isLeft = false,
            .dominantWeaponAuthority = true,
        }),
        "the ordinary firing hand must suppress dynamic self-feedback");
    Require(
        authority::weaponOwnsHand({
            .isLeft = false,
            .partGripActive = true,
        }),
        "a right-side part-carry grip must suppress dynamic self-feedback");
    Require(
        authority::weaponOwnsHand({
            .isLeft = true,
            .firingGripOccupied = true,
            .firingHandIsLeft = true,
        }),
        "a left firing-grip transition must suppress dynamic self-feedback");
    Require(
        !authority::weaponOwnsHand({
            .isLeft = false,
            .firingGripOccupied = true,
            .firingHandIsLeft = true,
        }),
        "left firing authority alone must not claim the free right hand");

    Require(
        authority::reciprocalWeaponStopOwnsHand({
            .stopActive = true,
            .targetIsDynamicHand = true,
            .targetHandIsLeft = true,
            .handIsLeft = true,
        }),
        "an active reciprocal gun stop must own the exact dynamic hand it contacted");
    Require(
        !authority::reciprocalWeaponStopOwnsHand({
            .stopActive = false,
            .targetIsDynamicHand = true,
            .targetHandIsLeft = true,
            .handIsLeft = true,
        }),
        "an inactive reciprocal gun stop must not suppress dynamic hand authority");
    Require(
        !authority::reciprocalWeaponStopOwnsHand({
            .stopActive = true,
            .targetIsDynamicHand = false,
            .targetHandIsLeft = true,
            .handIsLeft = true,
        }),
        "a weapon/world stop must not suppress dynamic hand authority");
    Require(
        !authority::reciprocalWeaponStopOwnsHand({
            .stopActive = true,
            .targetIsDynamicHand = true,
            .targetHandIsLeft = true,
            .handIsLeft = false,
        }),
        "a reciprocal gun stop must not suppress the opposite dynamic hand");

    {
        namespace feedback =
            rock::dynamic_hand_collision_feedback;
        feedback::ContactPulseState state{};
        feedback::ContactPulseConfig config{};
        config.minApproachSpeedGameUnitsPerSecond = 0.0f;
        config.cooldownSeconds = 0.0f;
        const auto suppressedEntry = feedback::updateContactPulse(
            state,
            1u,
            50.0f,
            1.0f / 90.0f,
            false,
            config);
        Require(
            !suppressedEntry.fire && state.observedEntrySequence == 1u,
            "a weapon-owned dynamic contact entry must be consumed without firing haptics");
        Require(
            !feedback::updateContactPulse(
                 state,
                 1u,
                 50.0f,
                 1.0f / 90.0f,
                 true,
                 config)
                 .fire,
            "releasing weapon ownership must not replay an attached-hand contact as a delayed pulse");
        Require(
            feedback::updateContactPulse(
                state,
                2u,
                50.0f,
                1.0f / 90.0f,
                true,
                config)
                .fire,
            "a genuinely new free-hand contact entry must retain dynamic haptic feedback");
    }

    const authority::WorldStopReadinessInput readyHand{
        .runtimeFrameReady = true,
        .handDisabled = false,
        .transitionCollisionSuppressed = false,
        .visualAuthorityAvailable = true,
        .allRequiredProductionBodiesCurrent = true,
        .hasSolvedPhysicsSample = true,
    };
    Require(
        authority::worldStopOperational(readyHand),
        "a complete current-world proxy bank with solved evidence may own the hand wall stop");
    auto creationFailed = readyHand;
    creationFailed.allRequiredProductionBodiesCurrent = false;
    Require(
        !authority::worldStopOperational(creationFailed),
        "a partial proxy creation failure must keep legacy hand wall fallback available");
    auto freshlyRebuilt = readyHand;
    freshlyRebuilt.hasSolvedPhysicsSample = false;
    Require(
        !authority::worldStopOperational(freshlyRebuilt),
        "a fresh or rebuilt proxy bank must not suppress fallback before its first physics sample");
    auto transitionSuppressed = readyHand;
    transitionSuppressed.transitionCollisionSuppressed = true;
    Require(
        !authority::worldStopOperational(transitionSuppressed),
        "transition suppression must immediately return hand wall ownership to the legacy fallback");

    {
        Require(
            stability::hostedCleanFrameAccepted(
                false,
                false,
                false,
                false),
            "standalone dynamic collision may use its uncontaminated native hand frame");
        Require(
            !stability::hostedCleanFrameAccepted(
                true,
                false,
                false,
                false) &&
                !stability::hostedCleanFrameAccepted(
                    true,
                    true,
                    false,
                    false) &&
                !stability::hostedCleanFrameAccepted(
                    true,
                    true,
                    true,
                    false),
            "hosted dynamic collision must fail closed for missing, non-finite, or unusable clean truth");
        Require(
            stability::hostedCleanFrameAccepted(
                true,
                true,
                true,
                true),
            "a fresh finite hosted clean frame with a valid rebase must be admitted");

        Require(
            stability::roleTargetAccepted(true, false, false) &&
                !stability::roleTargetAccepted(true, true, false) &&
                stability::roleTargetAccepted(true, true, true) &&
                stability::roleTargetAccepted(false, true, false),
            "hosted hand descendants may use the hand rebase, but an IK-ancestor forearm needs its own clean frame");

        /*
         * Hosted FRIK's flattened hand is last frame's stopped render pose at
         * x=10, while pass-1 controller truth is still pressing to x=12. The
         * same +2 translation must restore every proxy target to clean truth;
         * otherwise the proxy sees no gap, contact releases, and the hand
         * visibly alternates between x=10 and x=12.
         */
        const auto cleanRebase =
            stability::resolveCleanTranslationRebase(
                TestPoint{ 10.0f, 2.0f, 3.0f },
                TestPoint{ 12.0f, 2.0f, 3.0f });
        Require(cleanRebase.valid, "a finite hosted pre-authority hand must provide a clean dynamic-drive rebase");
        const auto cleanFinger = stability::applyTranslationRebase(
            TestPoint{ 11.0f, 4.0f, 3.0f },
            cleanRebase);
        Require(
            Near(cleanFinger.x, 13.0f) && Near(cleanFinger.y, 4.0f) && Near(cleanFinger.z, 3.0f),
            "every flattened proxy role must receive the same clean-hand translation delta");

        const auto deepPressRebase =
            stability::resolveCleanTranslationRebase(
                TestPoint{ -100.0f, 0.0f, 0.0f },
                TestPoint{ 20.0f, 0.0f, 0.0f });
        Require(
            deepPressRebase.valid &&
                Near(deepPressRebase.translationOffset.x, 120.0f),
            "fresh pre-authority truth must remain authoritative through a press deeper than 64 game units");
        const auto deeplyRebasedFinger =
            stability::applyTranslationRebase(
                TestPoint{ -95.0f, 1.0f, 0.0f },
                deepPressRebase);
        Require(
            Near(deeplyRebasedFinger.x, 25.0f) &&
                Near(deeplyRebasedFinger.y, 1.0f),
            "a deep clean-frame rebase must translate every role without a distance fallback");

        const auto nonfiniteRebase =
            stability::resolveCleanTranslationRebase(
                TestPoint{},
                TestPoint{
                    std::numeric_limits<float>::quiet_NaN(),
                    0.0f,
                    0.0f });
        Require(
            !nonfiniteRebase.valid,
            "a non-finite pre-authority hand must still fail closed");
    }

    {
        stability::ContactHoldState<TestPoint> laggedEntry{};
        laggedEntry.lastCleanHandTranslation =
            TestPoint{ 9.0f, 0.0f, 0.0f };
        laggedEntry.lastCleanHandTranslationValid = true;
        const auto wallEntry = stability::advanceContactHold(
            laggedEntry,
            TestPoint{ 12.0f, 0.0f, 0.0f },
            TestPoint{ -2.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            Near(wallEntry.state.blockedWorldTarget.x, 10.0f),
            "contact must stop at the coherent solver boundary instead of jumping to the previous controller frame");

        const auto staleProxyEntry = stability::advanceContactHold(
            laggedEntry,
            TestPoint{ 12.0f, 0.0f, 0.0f },
            TestPoint{ -4.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            Near(staleProxyEntry.state.blockedWorldTarget.x, 9.0f),
            "contact entry must never sweep a hand backward past its previous free pose while it moves into a wall");

        stability::ContactHoldState<TestPoint> movingWeaponEntry{};
        movingWeaponEntry.lastCleanHandTranslation =
            TestPoint{ 10.0f, 0.0f, 0.0f };
        movingWeaponEntry.lastCleanHandTranslationValid = true;
        const auto pushedByWeapon = stability::advanceContactHold(
            movingWeaponEntry,
            TestPoint{ 10.0f, 0.0f, 0.0f },
            TestPoint{ 8.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            Near(pushedByWeapon.state.blockedWorldTarget.x, 13.0f),
            "a moving obstacle fallback may push the hand only by the bounded per-frame entry speed");
        const auto continuedWeaponPush =
            stability::advanceContactHold(
                pushedByWeapon.state,
                TestPoint{ 10.0f, 0.0f, 0.0f },
                TestPoint{ 6.0f, 0.0f, 0.0f },
                true,
                1.0f / 90.0f,
                45.0f,
                0.05f);
        Require(
            Near(continuedWeaponPush.state.blockedWorldTarget.x, 13.0f),
            "continued moving-obstacle pressure must not move the latched hand pose");

        stability::ContactHoldState<TestPoint> hold{};
        auto step = stability::advanceContactHold(
            hold,
            TestPoint{ 12.0f, 0.0f, 0.0f },
            TestPoint{ -2.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            step.holdsWorldTarget && Near(step.state.blockedWorldTarget.x, 12.0f) && Near(step.correction.x, 0.0f),
            "contact entry without history must latch the current pose without a correction jump");

        hold = step.state;
        step = stability::advanceContactHold(
            hold,
            TestPoint{ 14.0f, 0.0f, 0.0f },
            TestPoint{ -4.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            Near(step.state.blockedWorldTarget.x, 12.0f) && Near(14.0f + step.correction.x, 12.0f),
            "continued controller pressure must grow correction while the rendered hand remains stationary at the wall");

        hold = step.state;
        step = stability::advanceContactHold(
            hold,
            TestPoint{ 14.2f, 0.0f, 0.0f },
            TestPoint{ -4.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            Near(step.state.blockedWorldTarget.x, 12.0f),
            "a lagging physics sample must never creep an established world stop into the wall");

        hold = step.state;
        step = stability::advanceContactHold(
            hold,
            TestPoint{ 14.0f, 0.0f, 0.0f },
            TestPoint{},
            false,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            step.holdsWorldTarget && Near(14.0f + step.correction.x, step.state.blockedWorldTarget.x),
            "one missing after-solve witness must retain the stationary wall target while the controller still presses inward");

        hold = step.state;
        step = stability::advanceContactHold(
            hold,
            TestPoint{ 9.0f, 0.0f, 0.0f },
            TestPoint{},
            false,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            !step.holdsWorldTarget && !step.state.active &&
                step.releasedByRetreat,
            "moving the clean controller back outside the wall must release the contact hold immediately");

        hold = stability::advanceContactHold(
            {},
            TestPoint{ 12.0f, 0.0f, 0.0f },
            TestPoint{ -2.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f)
                   .state;
        for (int frame = 0; frame < 6; ++frame) {
            step = stability::advanceContactHold(
                hold,
                TestPoint{ 12.0f, 0.0f, 0.0f },
                TestPoint{},
                false,
                1.0f / 90.0f,
                45.0f,
                0.05f);
            hold = step.state;
        }
        Require(
            !step.holdsWorldTarget && !hold.active,
            "a genuinely lost surface must release after the bounded contact-miss grace window");

        auto fastRetreat = stability::advanceContactHold(
            stability::ContactHoldState<TestPoint>{},
            TestPoint{ 14.0f, 0.0f, 0.0f },
            TestPoint{ -4.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        fastRetreat = stability::advanceContactHold(
            fastRetreat.state,
            TestPoint{ 11.0f, 0.0f, 0.0f },
            // Deliberately stale: the previous physics substep still reports
            // the four-unit blocked depth from clean x=14.
            TestPoint{ -4.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            fastRetreat.releasedByRetreat &&
                !fastRetreat.holdsWorldTarget,
            "retreat through the latched contact pose must release immediately even with lagged contact evidence");

        const auto crossedDuringStaleContact =
            stability::advanceContactHold(
                stability::ContactHoldState<TestPoint>{},
                TestPoint{ 9.0f, 0.0f, 0.0f },
                TestPoint{ -4.0f, 0.0f, 0.0f },
                true,
                1.0f / 90.0f,
                45.0f,
                0.05f);
        Require(
            crossedDuringStaleContact.holdsWorldTarget,
            "a fresh contact episode without history must latch its current pose");
    }

    {
        auto step = stability::advanceContactHold(
            stability::ContactHoldState<TestPoint>{},
            TestPoint{ 12.0f, 0.0f, 0.0f },
            TestPoint{ -2.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);

        step = stability::advanceContactHold(
            step.state,
            TestPoint{ 12.0f, 5.0f, 0.0f },
            TestPoint{ -2.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            Near(step.state.blockedWorldTarget.x, 12.0f) &&
                Near(step.state.blockedWorldTarget.y, 0.0f),
            "a contact episode must keep the complete hand pose stationary");

        const auto oppositeSurface = stability::advanceContactHold(
            step.state,
            TestPoint{ 8.0f, 5.0f, 0.0f },
            TestPoint{ 2.0f, 0.0f, 0.0f },
            true,
            1.0f / 90.0f,
            45.0f,
            0.05f);
        Require(
            oppositeSurface.releasedByRetreat &&
                !oppositeSurface.holdsWorldTarget,
            "retreat across the latched plane must release instead of switching contact anchors");

        const auto newPerpendicularSurface =
            stability::advanceContactHold(
                step.state,
                TestPoint{ 14.0f, 12.0f, 0.0f },
                TestPoint{ 0.0f, -2.0f, 0.0f },
                true,
                1.0f / 90.0f,
                45.0f,
                0.05f);
        Require(
            !newPerpendicularSurface.surfaceReset &&
                Near(newPerpendicularSurface.state.blockedWorldTarget.x, 12.0f) &&
                Near(newPerpendicularSurface.state.blockedWorldTarget.y, 0.0f),
            "a changing contact normal must not move the established stop pose");

        const auto implausibleTargetJump =
            stability::advanceContactHold(
                newPerpendicularSurface.state,
                TestPoint{ 50.0f, 42.0f, 0.0f },
                TestPoint{ 0.0f, -2.0f, 0.0f },
                true,
                1.0f / 90.0f,
                45.0f,
                0.05f);
        Require(
            implausibleTargetJump.holdsWorldTarget &&
                Near(implausibleTargetJump.state.blockedWorldTarget.x, 12.0f) &&
                Near(implausibleTargetJump.state.blockedWorldTarget.y, 0.0f),
            "an implausible contact-target jump must retain the last safe pose");
    }

    std::cout << "DynamicHandCollisionTopologyTests passed\n";
    return 0;
}
