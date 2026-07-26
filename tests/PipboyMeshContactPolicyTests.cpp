#include "PipboyMeshContactPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using heisenberg::pipboy_mesh_contact::SweepSegmentCount;
using heisenberg::pipboy_mesh_contact::RetainDeepestTapeDeckTravel;
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
}

int main()
{
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

    constexpr float openAngleRadians = 16.0f * 3.14159265358979323846f / 180.0f;
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
