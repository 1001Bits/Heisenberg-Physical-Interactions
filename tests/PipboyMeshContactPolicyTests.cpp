#include "PipboyMeshContactPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using heisenberg::pipboy_mesh_contact::SweepSegmentCount;
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

    // Contact made during a cooldown is consumed, not delayed until the cooldown expires.
    auto blockedEntry = UpdateLatch(false, 0.5f, 0.5f, 1.5f, 3.0f, false);
    Require(!blockedEntry.triggered, "contact during cooldown should not trigger");
    Require(blockedEntry.latched, "contact during cooldown should still latch");
    auto stillResting = UpdateLatch(blockedEntry.latched, 0.5f, 0.5f, 1.5f, 3.0f, true);
    Require(!stillResting.triggered, "remaining latched and resting should not re-trigger");

    // Leaving the release band only re-arms. The swept path includes last frame's contact,
    // but that old endpoint must never become a second activation on release.
    auto released = UpdateLatch(stillResting.latched, 4.0f, 0.5f, 1.5f, 3.0f, true);
    Require(!released.latched, "leaving the release band should unlatch");
    Require(!released.triggered, "unlatching on release should not itself trigger");
    auto retouched = UpdateLatch(released.latched, 1.0f, 1.0f, 1.5f, 3.0f, true);
    Require(retouched.triggered, "a fresh touch after re-arming should trigger");

    Require(SweepSegmentCount(0.0f, 1.5f) == 1, "zero distance should require a single segment");
    Require(SweepSegmentCount(9.0f, 1.5f) == 12, "sweep segment count should scale with distance");
    Require(SweepSegmentCount(1000.0f, 1.5f) == 32, "sweep segment count should be capped");
    Require(SweepSegmentCount(std::numeric_limits<float>::infinity(), 1.5f) == 1,
        "an infinite distance must not produce an invalid segment count");

    return 0;
}
