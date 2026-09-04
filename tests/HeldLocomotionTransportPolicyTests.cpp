#include "physics-interaction/grab/HeldLocomotionTransportPolicy.h"

#include <cmath>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    struct Point3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(float left, float right)
    {
        return std::fabs(left - right) < 0.0001f;
    }
}

int main()
{
    using namespace rock::held_locomotion_transport_policy;
    using State = RuntimeState<Point3>;
    using Input = FrameInput<Point3>;

    auto shared = std::make_shared<State>();
    auto first = evaluate(*shared, Input{ 100, Point3{ 400.0f, 0.0f, 0.0f }, true, true });
    Require(first.applyVelocityDelta && Near(first.velocityDeltaGame.x, 400.0f),
        "the first hand must add the room velocity once");
    *shared = first.nextState;

    auto joinedSameFrame = evaluate(*shared, Input{ 100, Point3{ 400.0f, 0.0f, 0.0f }, true, true });
    Require(!joinedSameFrame.applyVelocityDelta && joinedSameFrame.reason == DecisionReason::AlreadyProcessed,
        "a peer hand sharing the object runtime must not add velocity twice in one frame");

    auto steadyNextFrame = evaluate(*shared, Input{ 101, Point3{ 400.0f, 0.0f, 0.0f }, true, true });
    Require(!steadyNextFrame.applyVelocityDelta && steadyNextFrame.reason == DecisionReason::VelocityUnchanged,
        "a late peer join while already moving must not add the standing contribution again");
    *shared = steadyNextFrame.nextState;

    auto promotedOwner = shared;
    shared.reset();
    auto accelerated = evaluate(*promotedOwner, Input{ 102, Point3{ 475.0f, 0.0f, 0.0f }, true, true });
    Require(accelerated.applyVelocityDelta && Near(accelerated.velocityDeltaGame.x, 75.0f),
        "the surviving hand must inherit object-level transport bookkeeping");
    *promotedOwner = accelerated.nextState;

    auto stopped = evaluate(*promotedOwner, Input{ 103, Point3{}, true, true });
    Require(stopped.applyVelocityDelta && Near(stopped.velocityDeltaGame.x, -475.0f),
        "stopping must remove the standing contribution exactly once");
    *promotedOwner = stopped.nextState;
    auto stoppedPeer = evaluate(*promotedOwner, Input{ 103, Point3{}, true, true });
    Require(!stoppedPeer.applyVelocityDelta && stoppedPeer.reason == DecisionReason::AlreadyProcessed,
        "a peer must not remove the standing contribution twice");

    State disabledState{};
    auto engageBeforeDisable = evaluate(disabledState, Input{ 200, Point3{ 300.0f, 0.0f, 0.0f }, true, true });
    disabledState = engageBeforeDisable.nextState;
    auto disabled = evaluate(disabledState, Input{ 201, Point3{ 300.0f, 0.0f, 0.0f }, false, true });
    Require(disabled.applyVelocityDelta && Near(disabled.velocityDeltaGame.x, -300.0f),
        "disabling transport mid-hold must remove its contribution");
    disabledState = disabled.nextState;
    auto disabledPeer = evaluate(disabledState, Input{ 201, Point3{ 300.0f, 0.0f, 0.0f }, false, true });
    Require(!disabledPeer.applyVelocityDelta && disabledPeer.reason == DecisionReason::AlreadyProcessed,
        "mid-hold disable cleanup must be object-level and once per frame");

    State failureState{};
    failureState = evaluate(failureState, Input{ 300, Point3{ 250.0f, 0.0f, 0.0f }, true, true }).nextState;
    for (std::uint32_t failure = 1; failure < kReadFailureTripCount; ++failure) {
        auto grace = evaluate(failureState, Input{ 300 + failure, Point3{}, true, false });
        Require(!grace.applyVelocityDelta && grace.reason == DecisionReason::UnreadableGrace,
            "unreadable velocity must retain the contribution during the grace window");
        failureState = grace.nextState;
    }
    auto trip = evaluate(failureState, Input{ 300 + kReadFailureTripCount, Point3{}, true, false });
    Require(trip.applyVelocityDelta && Near(trip.velocityDeltaGame.x, -250.0f) &&
                trip.reason == DecisionReason::UnreadableRemoveContribution,
        "the read-failure trip must remove the contribution");
    failureState = trip.nextState;
    auto tripPeer = evaluate(failureState, Input{ 300 + kReadFailureTripCount, Point3{}, true, false });
    Require(!tripPeer.applyVelocityDelta && tripPeer.reason == DecisionReason::AlreadyProcessed,
        "the read-failure trip must not fire twice through the peer hand");

    State objectA{};
    State objectB{};
    const auto objectAFirst = evaluate(objectA, Input{ 500, Point3{ 100.0f, 0.0f, 0.0f }, true, true });
    const auto objectBFirst = evaluate(objectB, Input{ 500, Point3{ 100.0f, 0.0f, 0.0f }, true, true });
    Require(objectAFirst.applyVelocityDelta && objectBFirst.applyVelocityDelta,
        "two distinct held objects must each receive one room-velocity delta");

    State retryState{};
    const auto failedWriter = evaluate(retryState, Input{ 600, Point3{ 125.0f, 0.0f, 0.0f }, true, true });
    Require(failedWriter.applyVelocityDelta, "the first writer must request a delta");
    const auto peerRetry = evaluate(retryState, Input{ 600, Point3{ 125.0f, 0.0f, 0.0f }, true, true });
    Require(peerRetry.applyVelocityDelta && Near(peerRetry.velocityDeltaGame.x, 125.0f),
        "an uncommitted failed body write must remain retryable by the peer");

    State standingState{};
    const auto noise = evaluate(standingState, Input{ 700, Point3{ 0.5f, 0.0f, 0.0f }, true, true });
    Require(!noise.applyVelocityDelta && noise.reason == DecisionReason::StandingIdle,
        "sub-threshold controller noise must not engage transport");

    std::array<MotionRuntimeSlot<Point3>, 2> motionSlots{};
    Require(findOrCreateMotionSlot(motionSlots, 0) == nullptr,
        "static/invalid motion zero must fail closed");
    auto* motionA = findOrCreateMotionSlot(motionSlots, 41);
    auto* motionB = findOrCreateMotionSlot(motionSlots, 42);
    Require(motionA && motionB && motionA != motionB,
        "independent Havok motions must receive independent state slots");
    Require(findOrCreateMotionSlot(motionSlots, 41) == motionA,
        "the same motion must reuse its existing state slot");

    const Input partialWriteFrame{
        800,
        Point3{ 180.0f, 0.0f, 0.0f },
        true,
        true,
    };
    const auto motionAWrite = evaluate(motionA->state, partialWriteFrame);
    const auto motionBFailedWrite = evaluate(motionB->state, partialWriteFrame);
    Require(motionAWrite.applyVelocityDelta &&
                motionBFailedWrite.applyVelocityDelta,
        "both independent motions must request the room contribution");
    motionA->state = motionAWrite.nextState;
    // Deliberately do not commit B: this models a failed deferred write.
    const auto motionAPeer = evaluate(motionA->state, partialWriteFrame);
    const auto motionBPeerRetry = evaluate(motionB->state, partialWriteFrame);
    Require(!motionAPeer.applyVelocityDelta &&
                motionAPeer.reason == DecisionReason::AlreadyProcessed,
        "a successfully committed motion must deduplicate the peer hand");
    Require(motionBPeerRetry.applyVelocityDelta &&
                Near(motionBPeerRetry.velocityDeltaGame.x, 180.0f),
        "a failed motion write must remain independently retryable in the same frame");

    Require(findOrCreateMotionSlot(motionSlots, 43) == nullptr,
        "motion-state capacity exhaustion must fail closed without aliasing state");

    std::cout << "Held locomotion transport policy tests passed\n";
    return 0;
}
