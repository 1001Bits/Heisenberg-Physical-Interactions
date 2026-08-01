#include "PipboyMeshContactPolicy.h"
#include "PipboyNodeSnapshotPolicy.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

using heisenberg::pipboy_mesh_contact::SweepSegmentCount;
using heisenberg::pipboy_mesh_contact::CanRejectEjectMeshQuery;
using heisenberg::pipboy_mesh_contact::GeneratedDeckContactRadius;
using heisenberg::pipboy_mesh_contact::RetainDeepestTapeDeckTravel;
using heisenberg::pipboy_mesh_contact::TapeDeckContactKeepsPushing;
using heisenberg::pipboy_mesh_contact::TapeDeckProgressAfterPenetration;
using heisenberg::pipboy_mesh_contact::TapeDeckReachedMechanicalLatch;
using heisenberg::pipboy_mesh_contact::TapeDeckProgressFromStroke;
using heisenberg::pipboy_mesh_contact::TapeDeckStrokeDistance;
using heisenberg::pipboy_mesh_contact::UpdateGripReleaseGate;
using heisenberg::pipboy_mesh_contact::UpdateLatch;

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    void RequireNear(
        const float actual,
        const float expected,
        const float tolerance,
        const char* message)
    {
        Require(
            std::isfinite(actual) &&
                std::fabs(actual - expected) <= tolerance,
            message);
    }
}

int main()
{
    {
        using Snapshot =
            heisenberg::pipboy_node_snapshot::
                FixedSubtreeMembership<4>;
        Snapshot snapshot;
        int rootStorage = 0;
        int childStorage = 0;
        int grandchildStorage = 0;
        int siblingStorage = 0;
        int staleStorage = 0;

        const int root =
            snapshot.Append(&rootStorage, -1);
        const int child =
            snapshot.Append(&childStorage, root);
        (void)snapshot.Append(&grandchildStorage, child);
        (void)snapshot.Append(&siblingStorage, root);

        Require(
            snapshot.Complete() && snapshot.Size() == 4,
            "a within-capacity subtree snapshot should be complete");
        Require(
            snapshot.Contains(&grandchildStorage),
            "a captured descendant should be found by pointer only");
        Require(
            snapshot.ContainsWithin(
                &rootStorage,
                &grandchildStorage),
            "a captured grandchild should belong to its root");
        Require(
            snapshot.ContainsWithin(
                &childStorage,
                &grandchildStorage),
            "a captured grandchild should belong to its direct subtree");
        Require(
            !snapshot.ContainsWithin(
                &siblingStorage,
                &grandchildStorage),
            "a sibling must not satisfy nested subtree membership");
        Require(
            !snapshot.Contains(&staleStorage),
            "an uncaptured stale address must fail liveness without dereference");

        Require(
            snapshot.Append(&staleStorage, root) < 0,
            "a full fixed snapshot must reject overflow");
        Require(
            !snapshot.Complete() && snapshot.Size() == 4,
            "overflow must be reported without corrupting captured entries");
        Require(
            snapshot.ContainsWithin(
                &rootStorage,
                &grandchildStorage),
            "overflow must preserve earlier membership answers");

        snapshot.Reset();
        Require(
            snapshot.Complete() &&
                snapshot.Size() == 0 &&
                !snapshot.Contains(&rootStorage),
            "reset must start a fresh independent validation phase");
    }

    {
        // These address-only sentinels collide in the snapshot's eight-slot
        // pointer index on the supported 64-bit build (and the alternate pair
        // does so for the header's 32-bit hash). They are never dereferenced.
        using Snapshot =
            heisenberg::pipboy_node_snapshot::
                FixedSubtreeMembership<4>;
        Snapshot snapshot;
        const auto firstAddress =
            sizeof(std::uintptr_t) >= 8 ?
                std::uintptr_t{ 0x1008 } :
                std::uintptr_t{ 0x1004 };
        const auto secondAddress =
            sizeof(std::uintptr_t) >= 8 ?
                std::uintptr_t{ 0x1018 } :
                std::uintptr_t{ 0x1014 };
        const void* first =
            reinterpret_cast<const void*>(firstAddress);
        const void* second =
            reinterpret_cast<const void*>(secondAddress);

        const int firstIndex = snapshot.Append(first, -1);
        (void)snapshot.Append(second, firstIndex);
        Require(
            snapshot.Contains(first) && snapshot.Contains(second),
            "distinct colliding pointers must both remain findable");
        Require(
            snapshot.ContainsWithin(first, second),
            "a colliding pointer lookup must preserve its parent chain");
    }

    {
        using Snapshot =
            heisenberg::pipboy_node_snapshot::
                FixedSubtreeMembership<8>;
        Snapshot snapshot;
        int firstRootStorage = 0;
        int secondRootStorage = 0;
        int duplicateStorage = 0;

        const int firstRoot =
            snapshot.Append(&firstRootStorage, -1);
        const int secondRoot =
            snapshot.Append(&secondRootStorage, -1);
        const int firstDuplicate =
            snapshot.Append(&duplicateStorage, firstRoot);
        const int secondDuplicate =
            snapshot.Append(&duplicateStorage, secondRoot);

        Require(
            firstDuplicate >= 0 &&
                secondDuplicate > firstDuplicate &&
                snapshot.Size() == 4,
            "duplicate pointers must retain the old append and size semantics");
        Require(
            snapshot.ContainsWithin(
                &firstRootStorage,
                &duplicateStorage),
            "duplicate lookup must retain the first appended parent chain");
        Require(
            !snapshot.ContainsWithin(
                &secondRootStorage,
                &duplicateStorage),
            "a later duplicate must not replace the first lookup index");

        snapshot.Reset();
        const int freshRoot =
            snapshot.Append(&secondRootStorage, -1);
        (void)snapshot.Append(&duplicateStorage, freshRoot);
        Require(
            snapshot.Size() == 2 &&
                snapshot.ContainsWithin(
                    &secondRootStorage,
                    &duplicateStorage) &&
                !snapshot.ContainsWithin(
                    &firstRootStorage,
                    &duplicateStorage),
            "generation reset must hide stale slots and accept reused pointers");
    }

    // A fast pass can end outside the release band and still trigger from its swept path.
    auto fastPass = UpdateLatch(false, 10.0f, 0.5f, 1.5f, 3.0f, true);
    Require(fastPass.triggered, "fast pass should trigger from its swept path");
    Require(fastPass.latched, "fast pass should latch");

    // A contact made during cooldown remains eligible while it is physically held down.
    auto blockedEntry = UpdateLatch(false, 0.5f, 0.5f, 1.5f, 3.0f, false);
    Require(!blockedEntry.triggered, "contact during cooldown should not trigger");
    Require(!blockedEntry.latched, "cooldown must not consume and latch the press");
    auto stillResting = UpdateLatch(blockedEntry.latched, 0.5f, 0.5f, 1.5f, 3.0f, true);
    Require(stillResting.triggered, "a still-depressed button should trigger when cooldown expires");
    Require(stillResting.latched, "a successful delayed press should latch");

    // Leaving the release band only re-arms. The swept path includes last frame's contact,
    // but that old endpoint must never become a second activation on release.
    auto released = UpdateLatch(stillResting.latched, 4.0f, 0.5f, 1.5f, 3.0f, true);
    Require(!released.latched, "leaving the release band should unlatch");
    Require(!released.triggered, "unlatching on release should not itself trigger");
    auto retouched = UpdateLatch(released.latched, 1.0f, 1.0f, 1.5f, 3.0f, true);
    Require(retouched.triggered, "a fresh touch after re-arming should trigger");

    // Inserting a held tape consumes that Grip until the player actually lets go.
    // This is state-based rather than time-based, so holding Grip for several
    // seconds can never pull the just-inserted tape straight back out.
    auto insertionGripHeld = UpdateGripReleaseGate(true, true);
    Require(insertionGripHeld.requiresRelease, "held insertion grip should remain gated");
    Require(insertionGripHeld.blocksAction, "held insertion grip should block removal");
    auto insertionGripReleased =
        UpdateGripReleaseGate(insertionGripHeld.requiresRelease, false);
    Require(!insertionGripReleased.requiresRelease, "grip release should re-arm removal");
    Require(!insertionGripReleased.blocksAction, "released grip should stop blocking removal");
    auto deliberateRemovalPress =
        UpdateGripReleaseGate(insertionGripReleased.requiresRelease, true);
    Require(!deliberateRemovalPress.blocksAction, "a later deliberate grip press should be allowed");

    Require(SweepSegmentCount(0.0f, 1.5f) == 1, "zero distance should require a single segment");
    Require(SweepSegmentCount(9.0f, 1.5f) == 12, "sweep segment count should scale with distance");
    Require(SweepSegmentCount(1000.0f, 1.5f) == 32, "sweep segment count should be capped");
    Require(SweepSegmentCount(std::numeric_limits<float>::infinity(), 1.5f) == 1,
        "an infinite distance must not produce an invalid segment count");

    Require(
        CanRejectEjectMeshQuery(20.0f, 2.0f, 1.0f, 6.0f),
        "a fingertip path clearly outside the mesh, pivot, release band, and guard may skip extraction");
    Require(
        !CanRejectEjectMeshQuery(10.0f, 2.0f, 1.0f, 6.0f),
        "the broadphase boundary must remain on the exact query path");
    Require(
        !CanRejectEjectMeshQuery(0.2f, 2.0f, 1.0f, 6.0f),
        "a fast swept path near the button bound must remain exact");
    Require(
        !CanRejectEjectMeshQuery(
            20.0f,
            std::numeric_limits<float>::quiet_NaN(),
            1.0f,
            6.0f),
        "an invalid eject world bound must fail open to exact mesh extraction");

    RequireNear(
        GeneratedDeckContactRadius(0.10f, 0.50f),
        0.60f,
        1.0e-5f,
        "generated hull skin should admit the measured 0.59-unit visible touch");
    RequireNear(
        GeneratedDeckContactRadius(
            std::numeric_limits<float>::quiet_NaN(),
            0.50f),
        0.50f,
        1.0e-5f,
        "an invalid exported radius must not poison deck contact");
    RequireNear(
        GeneratedDeckContactRadius(0.10f, -2.0f),
        0.10f,
        1.0e-5f,
        "a negative contact skin must clamp to zero");

    Require(
        TapeDeckContactKeepsPushing(0.01f, false, false),
        "positive overlap should enter a deck push");
    Require(
        TapeDeckContactKeepsPushing(-100.0f, true, false),
        "a validated continuous crossing should enter a deck push");
    Require(
        !TapeDeckContactKeepsPushing(-0.20f, false, false, 0.30f),
        "release hysteresis must not create a new contact through empty space");
    Require(
        TapeDeckContactKeepsPushing(-0.20f, false, true, 0.30f),
        "an existing push should survive a small rotating-surface separation");
    Require(
        !TapeDeckContactKeepsPushing(-0.31f, false, true, 0.30f),
        "an existing push should release outside its hysteresis band");
    Require(
        !TapeDeckContactKeepsPushing(
            std::numeric_limits<float>::quiet_NaN(),
            false,
            true,
            0.30f),
        "invalid penetration must release instead of latching the mechanism");

    constexpr float openAngleRadians = 16.0f * 3.14159265358979323846f / 180.0f;
    RequireNear(
        TapeDeckProgressAfterPenetration(
            1.0f,
            10.0f,
            10.0f,
            0.25f,
            false),
        0.80f,
        1.0e-5f,
        "slow overlap should retain the per-frame progress cap");
    RequireNear(
        TapeDeckProgressAfterPenetration(
            1.0f,
            2.5f,
            10.0f,
            0.25f,
            true),
        0.0f,
        1.0e-5f,
        "a validated full-depth sweep should clear in the crossing frame");
    RequireNear(
        TapeDeckProgressAfterPenetration(
            0.65f,
            -0.20f,
            10.0f,
            0.25f,
            false),
        0.65f,
        1.0e-5f,
        "hysteresis separation should hold rather than advance progress");
    RequireNear(
        TapeDeckProgressAfterPenetration(
            0.65f,
            2.0f,
            1.0f,
            0.25f,
            true),
        0.0f,
        1.0e-5f,
        "a deep validated near-hinge crossing should use the minimum physical stroke");
    RequireNear(
        TapeDeckProgressAfterPenetration(
            0.65f,
            0.10f,
            0.0f,
            0.25f,
            false),
        0.61f,
        1.0e-5f,
        "ordinary contact at the hinge should advance gradually instead of stalling");

    const float deckStroke = TapeDeckStrokeDistance(12.0f, 1.0f, openAngleRadians);
    Require(deckStroke > 3.3f && deckStroke < 3.4f,
        "deck stroke should follow the contact point's hinge arc");
    Require(TapeDeckStrokeDistance(0.1f, 1.0f, openAngleRadians) == 2.5f,
        "a near-hinge touch should retain a deliberate minimum stroke");
    Require(TapeDeckStrokeDistance(100.0f, 1.0f, openAngleRadians) == 6.0f,
        "a far-edge touch should retain a reachable maximum stroke");

    Require(TapeDeckProgressFromStroke(1.0f, 0.0f, 4.0f) == 1.0f,
        "touching without pressing must not move the deck");
    Require(TapeDeckProgressFromStroke(1.0f, 1.0f, 4.0f) == 0.75f,
        "one quarter of the physical stroke should close one quarter");
    Require(TapeDeckProgressFromStroke(1.0f, 2.0f, 4.0f) == 0.5f,
        "half of the physical stroke should half-close the deck");
    Require(TapeDeckProgressFromStroke(1.0f, 4.0f, 4.0f) == 0.0f,
        "the full physical stroke should close the deck");
    Require(TapeDeckProgressFromStroke(0.8f, 1.0f, 4.0f) == 0.6f,
        "a push that starts during opening should preserve proportional travel");
    Require(TapeDeckProgressFromStroke(1.0f, -2.0f, 4.0f) == 1.0f,
        "moving away from first touch must never close the deck");
    Require(RetainDeepestTapeDeckTravel(2.2f, 1.4f) == 2.2f,
        "contact jitter must not undo the deepest continuous push");
    Require(RetainDeepestTapeDeckTravel(2.2f, 2.8f) == 2.8f,
        "a deeper push must advance the retained stroke");
    Require(RetainDeepestTapeDeckTravel(
                2.2f,
                std::numeric_limits<float>::quiet_NaN()) == 2.2f,
        "an invalid sample must not corrupt retained stroke travel");
    Require(!TapeDeckReachedMechanicalLatch(0.16f),
        "the deck should remain hand-driven above the latch band");
    Require(TapeDeckReachedMechanicalLatch(0.15f),
        "the final fifteen percent should engage the mechanical latch");

    return 0;
}
