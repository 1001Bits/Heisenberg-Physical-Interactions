#include "ConsumableUsePolicy.h"
#include "NpcInjectionPolicy.h"
#include "PostConsumeActivationPolicy.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    void RequireNear(float actual, float expected, const char* message)
    {
        if (!std::isfinite(actual) || std::fabs(actual - expected) > 0.0001f) {
            std::cerr << "FAILED: " << message << " (actual=" << actual
                      << ", expected=" << expected << ")\n";
            std::exit(1);
        }
    }
}

int main()
{
    using heisenberg::consumable_use_policy::
        mayUseHeldConsumable;
    Require(
        !mayUseHeldConsumable(0.49f, false) &&
            mayUseHeldConsumable(0.5f, false),
        "ordinary grabs must retain the existing half-second use grace");
    Require(
        !mayUseHeldConsumable(0.5f, true) &&
            !mayUseHeldConsumable(0.99f, true) &&
            mayUseHeldConsumable(1.0f, true),
        "loot delivered into a seated hand must have a one-second self-use grace");
    Require(
        !mayUseHeldConsumable(10.0f, true, true) &&
            mayUseHeldConsumable(1.0f, true, false),
        "loot that materializes inside a consume zone must exit once before use is armed");
    Require(
        !mayUseHeldConsumable(
            std::numeric_limits<float>::quiet_NaN(),
            true) &&
            !mayUseHeldConsumable(-1.0f, true),
        "invalid placement timing must fail closed");

    using heisenberg::post_consume_activation_policy::
        shouldBlockPlayerActivation;
    using heisenberg::post_consume_activation_policy::
        shouldKeepSuppressed;
    Require(
        shouldKeepSuppressed(false, false) &&
            shouldKeepSuppressed(true, true) &&
            !shouldKeepSuppressed(true, false),
        "post-consume activation must outlive both its minimum tail and the consuming grip");
    Require(
        shouldBlockPlayerActivation(true, false, true, false) &&
            !shouldBlockPlayerActivation(true, true, true, false) &&
            !shouldBlockPlayerActivation(true, false, false, false) &&
            !shouldBlockPlayerActivation(true, false, true, true),
        "the consume tail must block only external player activation");

    using namespace heisenberg::npc_injection_policy;

    constexpr std::uintptr_t player = 0x1000u;
    constexpr std::uintptr_t settler = 0x2000u;

    // Every independent runtime source is sufficient evidence that the actor
    // is the player's current companion; only the all-false row is rejected.
    for (std::uint32_t mask = 0u; mask < 16u; ++mask) {
        const CompanionEvidence evidence{
            .commandedFlag = (mask & 0x1u) != 0u,
            .commandedByPlayer = (mask & 0x2u) != 0u,
            .inPlayerCommandList = (mask & 0x4u) != 0u,
            .inCurrentCompanionFaction = (mask & 0x8u) != 0u,
        };
        Require(
            IsCurrentPlayerCompanion(evidence) == (mask != 0u),
            "current-companion evidence must be an OR across all four runtime sources");
    }

    struct WoundedStateCase
    {
        std::uint32_t lifeState;
        bool inBleedoutAnimation;
        bool expected;
    };
    constexpr std::array woundedStateMatrix{
        WoundedStateCase{ kLifeStateAlive, false, false },
        WoundedStateCase{ kLifeStateAlive, true, true },
        WoundedStateCase{ kLifeStateDying, false, false },
        WoundedStateCase{ kLifeStateDying, true, false },
        WoundedStateCase{ kLifeStateDead, false, false },
        WoundedStateCase{ kLifeStateDead, true, false },
        WoundedStateCase{ kLifeStateRecycle, false, false },
        WoundedStateCase{ kLifeStateRecycle, true, false },
        WoundedStateCase{ kLifeStateEssentialDown, false, true },
        WoundedStateCase{ kLifeStateEssentialDown, true, true },
        WoundedStateCase{ kLifeStateBleedout, false, true },
        WoundedStateCase{ kLifeStateBleedout, true, true },
        WoundedStateCase{ 3u, false, false },  // Unconscious/scripted state
        WoundedStateCase{ 3u, true, false },
        WoundedStateCase{ 4u, false, false },  // Reanimate
        WoundedStateCase{ 4u, true, false },
        WoundedStateCase{ 6u, false, false },  // Restrained/scripted state
        WoundedStateCase{ 6u, true, false },
        WoundedStateCase{ 99u, false, false }, // Unknown future state
        WoundedStateCase{ 99u, true, false },
    };
    for (const auto& testCase : woundedStateMatrix) {
        Require(
            IsWoundedCompanionState(
                testCase.lifeState,
                testCase.inBleedoutAnimation) == testCase.expected,
            "companion wounded/downed state matrix must fail closed outside verified states");
    }

    constexpr CompanionEvidence currentCompanion{
        .inCurrentCompanionFaction = true,
    };
    constexpr CompanionEvidence stranger{};
    Require(
        AllowsCompanionStimpakInjection(
            true,
            true,
            currentCompanion,
            kLifeStateEssentialDown,
            false),
        "an enabled Stimpak gesture must allow a verified downed current companion");
    Require(
        !AllowsCompanionStimpakInjection(
            false,
            true,
            currentCompanion,
            kLifeStateEssentialDown,
            false) &&
            !AllowsCompanionStimpakInjection(
                true,
                false,
                currentCompanion,
                kLifeStateEssentialDown,
                false) &&
            !AllowsCompanionStimpakInjection(
                true,
                true,
                stranger,
                kLifeStateEssentialDown,
                false) &&
            !AllowsCompanionStimpakInjection(
                true,
                true,
                currentCompanion,
                kLifeStateDead,
                true),
        "the top-level companion Stimpak gate must require every independent safety condition");

    Require(
        !IsEligibleNpcTarget(0u, player, 0u, false),
        "a null actor must never be an injection target");
    Require(
        !IsEligibleNpcTarget(player, player, 0xFF000123u, false),
        "pointer-identical PlayerRef must be rejected");
    Require(
        !IsEligibleNpcTarget(settler, player, kPlayerRefFormID, false),
        "PlayerRef FormID must be rejected even if the pointer identity differs");
    Require(
        !IsEligibleNpcTarget(settler, player, 0x03001234u, true),
        "dead actors must be rejected");
    Require(
        IsEligibleNpcTarget(settler, player, 0x03001234u, false),
        "a living non-player actor remains eligible; SS2, not the gesture layer, owns treatment-state rules");

    Require(
        IsCanonicalDiseaseCureIdentity(
            true,
            "SS2_C2_DiseaseCureAllKnown"),
        "the canonical SS2 Disease Cure must be recognized without Papyrus");
    Require(
        !IsCanonicalDiseaseCureIdentity(
            false,
            "SS2_C2_DiseaseCureAllKnown"),
        "an editor-ID collision outside SS2 must fail closed");
    Require(
        !IsCanonicalDiseaseCureIdentity(true, "SS2_C2_DiseaseCure_Typhus"),
        "SS2's internal per-disease potions are not the held generic cure");
    Require(
        IsCanonicalDiseaseCureIdentity(
            true,
            {},
            kCanonicalDiseaseCureLocalFormID),
        "the verified SS2 local FormID must reserve the cure even when runtime EditorID data is unavailable");
    Require(
        !IsCanonicalDiseaseCureIdentity(
            false,
            {},
            kCanonicalDiseaseCureLocalFormID),
        "the cure local FormID must never match a record outside SS2.esm");

    int consumeCalls = 0;
    Require(
        !TryPlayerConsumption(true, [&]() {
            ++consumeCalls;
            return true;
        }),
        "the Disease Cure must be rejected from every player-consume route");
    Require(
        consumeCalls == 0,
        "rejecting a Disease Cure must not invoke the consume side effect");
    Require(
        TryPlayerConsumption(false, [&]() {
            ++consumeCalls;
            return true;
        }),
        "ordinary consumables must retain player use");
    Require(
        consumeCalls == 1,
        "an ordinary consumable should invoke its consume side effect once");

    const CapsuleSegment torsoSegment{
        .start = { 0.0f, 0.0f, 0.0f },
        .end = { 10.0f, 0.0f, 0.0f },
        .radius = 1.0f,
    };
    const ContactProbeSet midpointProbe = MakeContactProbeSet(
        { 5.0f, 2.0f, 0.0f },
        false,
        {},
        0.0f);
    RequireNear(
        MinimumProbeDistanceToCapsule(midpointProbe, torsoSegment),
        1.0f,
        "a segment midpoint touch must not be missed just because both bones are farther away");

    const CapsuleSegment degenerateSegment{
        .start = { 3.0f, 0.0f, 0.0f },
        .end = { 3.0f, 0.0f, 0.0f },
        .radius = 0.0f,
    };
    RequireNear(
        PointToSegmentDistance(
            { 0.0f, 0.0f, 0.0f },
            degenerateSegment.start,
            degenerateSegment.end),
        3.0f,
        "a degenerate bone segment must safely fall back to point distance");

    const ContactProbeSet heldSphereProbe = MakeContactProbeSet(
        { 0.0f, 0.0f, 0.0f },
        true,
        { 10.0f, 0.0f, 0.0f },
        2.0f);
    const CapsuleSegment heldSphereTarget{
        .start = { 13.0f, 0.0f, 0.0f },
        .end = { 13.0f, 0.0f, 0.0f },
        .radius = 1.0f,
    };
    Require(
        heldSphereProbe.count == 2,
        "a finite nearby held-object bound must augment the permanent wand probe");
    RequireNear(
        MinimumProbeDistanceToCapsule(
            heldSphereProbe,
            heldSphereTarget),
        0.0f,
        "the held-object sphere must detect contact when the wand itself is short of the actor");

    const ContactProbeSet staleHeldBoundWithWandHit = MakeContactProbeSet(
        { 5.0f, 0.0f, 0.0f },
        true,
        { 1000.0f, 0.0f, 0.0f },
        2.0f);
    Require(
        staleHeldBoundWithWandHit.count == 1,
        "a stale far-away held bound must be ignored without discarding the wand probe");
    RequireNear(
        MinimumProbeDistanceToCapsule(
            staleHeldBoundWithWandHit,
            torsoSegment),
        0.0f,
        "the permanent wand probe must still hit when the optional held bound is stale");
    Require(
        MakeContactProbeSet(
            {},
            true,
            {},
            -1.0f).count == 1,
        "a negative held bound radius must be ignored");
    Require(
        MakeContactProbeSet(
            {},
            true,
            {},
            kMaximumHeldProbeRadiusGameUnits + 1.0f).count == 1,
        "an oversized held bound must be ignored instead of clamped into a remote cure probe");
    Require(
        MakeContactProbeSet(
            {},
            true,
            { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f },
            1.0f).count == 1,
        "a non-finite held bound must be ignored");

    HostedDispatchGuard hostedGuard{};
    Require(
        AllowsHostedDispatch(hostedGuard, 41u),
        "a fresh ROCK grab session must be eligible for host dispatch");
    // A preflight failure deliberately does not mutate the guard, so a later
    // frame may retry after SS2's VM/manager finishes loading.
    Require(
        AllowsHostedDispatch(hostedGuard, 41u),
        "a preflight-only attempt must not consume the session's one dispatch");
    MarkHostedDispatchCommitted(hostedGuard, 41u);
    Require(
        !AllowsHostedDispatch(hostedGuard, 41u),
        "an accepted or refunded commit must be terminal for that exact ROCK grab session");
    Require(
        AllowsHostedDispatch(hostedGuard, 42u),
        "re-grabbing the cure under a new trace must permit another target attempt");

    std::cout << "NpcInjectionPolicyTests passed\n";
    return 0;
}
