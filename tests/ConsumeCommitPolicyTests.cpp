#include "physics-interaction/consume/ConsumeCommitPolicy.h"

#include <cstdlib>
#include <iostream>

namespace
{
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
    using namespace rock::consume_commit_policy;

    const auto fastReleaseWithoutDwell = decide(Input{
        .eligible = true,
        .spatiallyInsideCoreZone = true,
        .detectorConfirmed = false,
        .gripReleased = true,
        .autoConsumeWhileHeld = false,
    });
    Require(
        fastReleaseWithoutDwell.trigger == Trigger::GripRelease,
        "release in the core zone must commit without dwell/speed confirmation");

    Require(
        !decide(Input{
            .eligible = true,
            .spatiallyInsideCoreZone = false,
            .detectorConfirmed = true,
            .gripReleased = true,
            .autoConsumeWhileHeld = true,
        }).shouldCommit(),
        "neither release nor auto may commit outside the current core zone");

    Require(
        !decide(Input{
            .eligible = true,
            .spatiallyInsideCoreZone = true,
            .detectorConfirmed = true,
            .gripReleased = false,
            .autoConsumeWhileHeld = false,
        }).shouldCommit(),
        "default-off auto mode must leave a confirmed held item alone");

    Require(
        decide(Input{
            .eligible = true,
            .spatiallyInsideCoreZone = true,
            .detectorConfirmed = true,
            .gripReleased = false,
            .autoConsumeWhileHeld = true,
        }).trigger == Trigger::AutoWhileHeld,
        "enabled auto mode must commit a confirmed held candidate");

    Require(
        !decide(Input{
            .eligible = true,
            .spatiallyInsideCoreZone = true,
            .detectorConfirmed = false,
            .gripReleased = false,
            .autoConsumeWhileHeld = true,
        }).shouldCommit(),
        "auto mode must retain detector dwell/speed safety");

    Require(
        !decide(Input{
            .eligible = false,
            .spatiallyInsideCoreZone = true,
            .detectorConfirmed = true,
            .gripReleased = true,
            .autoConsumeWhileHeld = true,
        }).shouldCommit(),
        "route or item eligibility must remain authoritative");

    const auto sameFrameReleaseAndAuto = decide(Input{
        .eligible = true,
        .spatiallyInsideCoreZone = true,
        .detectorConfirmed = true,
        .gripReleased = true,
        .autoConsumeWhileHeld = true,
    });
    Require(
        sameFrameReleaseAndAuto.trigger == Trigger::GripRelease,
        "release must deterministically win over auto on the same frame");

    std::cout << "Consume commit policy tests passed\n";
    return 0;
}
