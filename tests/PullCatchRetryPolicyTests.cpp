#include "physics-interaction/grab/PullCatchRetryPolicy.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    rock::pull_catch_retry_policy::PoseSample pose(
        float x = 0.0f,
        float y = 0.0f,
        float z = 0.0f,
        float rotationDegrees = 0.0f)
    {
        constexpr float degreesToRadians = 0.01745329251994329577f;
        const float angle = rotationDegrees * degreesToRadians;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        return {
            .position = { x, y, z },
            .rotation = {
                cosine, -sine, 0.0f,
                sine, cosine, 0.0f,
                0.0f, 0.0f, 1.0f,
            },
            .valid = true,
        };
    }
}

int main()
{
    using namespace rock::pull_catch_retry_policy;

    require(!evaluateCurrentMeshSnap(false, false, true, true, 9.0f, 32.0f).attempt,
        "ordinary grabs must never receive the pull-catch mesh-snap extension");
    require(!evaluateCurrentMeshSnap(true, true, true, true, 9.0f, 32.0f).attempt,
        "an existing mesh surface must remain authoritative without a recovery query");
    require(!evaluateCurrentMeshSnap(true, false, false, true, 9.0f, 32.0f).attempt,
        "pull catch recovery must fail closed without current mesh triangles");
    require(!evaluateCurrentMeshSnap(true, false, true, false, 9.0f, 32.0f).attempt,
        "pull catch recovery must fail closed without a valid palm pocket");

    const auto currentMeshSnap = evaluateCurrentMeshSnap(true, false, true, true, 9.0f, 32.0f);
    require(currentMeshSnap.attempt && std::abs(currentMeshSnap.maxDistanceGameUnits - 32.0f) < 0.001f,
        "a verified pull catch may extend its position-only query to the configured current-mesh radius");
    const auto boundedCurrentMeshSnap = evaluateCurrentMeshSnap(true, false, true, true, 9.0f, 120.0f);
    require(boundedCurrentMeshSnap.attempt && std::abs(boundedCurrentMeshSnap.maxDistanceGameUnits - 48.0f) < 0.001f,
        "the pull-catch-only current-mesh extension must remain bounded");
    require(!evaluateCurrentMeshSnap(true, false, true, true, 64.0f, 120.0f).attempt,
        "recovery must not repeat an ordinary mesh query when its envelope cannot be safely extended");

    require(allowsMissingMeshForExactProfilePullCatch(true, true, true, true, false),
        "a verified pull target with a usable exact profile may supply placement authority when current mesh recovery misses");
    require(!allowsMissingMeshForExactProfilePullCatch(false, true, true, true, false),
        "an ordinary grab must not inherit exact-profile pull admission");
    require(!allowsMissingMeshForExactProfilePullCatch(true, false, true, true, false),
        "a target excluded from offset placement must remain excluded");
    require(!allowsMissingMeshForExactProfilePullCatch(true, true, false, true, false),
        "a missing exact profile must fail closed");
    require(!allowsMissingMeshForExactProfilePullCatch(true, true, true, false, false),
        "a non-finite exact attach transform must fail closed");
    require(!allowsMissingMeshForExactProfilePullCatch(true, true, true, true, true),
        "real mesh contact remains authoritative when available");

    RuntimeState state{};
    require(advance(state, pose(), 0.0f, 0.65f).shouldAttempt(),
        "the first pull-catch commit must remain immediate");

    noteFailedAttempt(state, pose());
    require(advance(state, pose(), 0.01f, 0.65f).status == Status::Waiting,
        "an unchanged failed target must not retry on the next frame");
    require(advance(state, pose(), 0.17f, 0.65f).shouldAttempt(),
        "an unchanged target must receive a bounded transient timed retry");

    noteFailedAttempt(state, pose());
    require(advance(state, pose(2.49f), 0.01f, 0.65f).status == Status::Waiting,
        "sub-threshold hand drift must not retrigger expensive grab preparation");
    require(advance(state, pose(2.5f), 0.0f, 0.65f).shouldAttempt(),
        "meaningful hand translation must rearm the catch attempt immediately");

    noteFailedAttempt(state, pose());
    require(advance(state, pose(0.0f, 0.0f, 0.0f, 7.4f), 0.01f, 0.65f).status == Status::Waiting,
        "sub-threshold hand rotation must remain latched");
    require(advance(state, pose(0.0f, 0.0f, 0.0f, 7.6f), 0.0f, 0.65f).shouldAttempt(),
        "meaningful hand rotation must rearm the catch attempt immediately");

    noteFailedAttempt(state, pose());
    require(advance(state, pose(), 0.01f, 0.65f).status == Status::AttemptBudgetExhausted,
        "four failed commits must end the same-target retry storm");

    reset(state);
    require(advance(state, pose(), 0.0f, 0.65f).shouldAttempt(),
        "release or target-change reset must rearm an immediate first attempt");

    noteFailedAttempt(state, pose());
    require(advance(state, pose(), 0.66f, 0.65f).status == Status::RetryWindowExpired,
        "the configured pull-catch retry window must remain authoritative");

    std::cout << "PullCatchRetryPolicyTests passed\n";
    return 0;
}
