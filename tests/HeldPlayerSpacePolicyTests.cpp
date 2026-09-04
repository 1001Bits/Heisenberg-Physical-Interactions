#include "physics-interaction/grab/GrabHeldObject.h"
#include "physics-interaction/grab/HeldLocomotionTransportPolicy.h"
#include "physics-interaction/grab/HeldPlayerSpaceRegistry.h"
#include "physics-interaction/grab/HeldPlayerSpaceWarpPolicy.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Matrix3
    {
        float entry[3][3]{};
    };

    struct Transform
    {
        Matrix3 rotate{};
        Vec3 translate{};
        float scale = 1.0f;
    };

    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(float left, float right, float epsilon = 0.0001f)
    {
        return std::fabs(left - right) <= epsilon;
    }

    Transform IdentityTransform(Vec3 translation = {})
    {
        Transform result{};
        result.rotate.entry[0][0] = 1.0f;
        result.rotate.entry[1][1] = 1.0f;
        result.rotate.entry[2][2] = 1.0f;
        result.translate = translation;
        return result;
    }

    Transform Yaw90Transform(Vec3 translation = {})
    {
        Transform result{};
        result.rotate.entry[0][1] = 1.0f;
        result.rotate.entry[1][0] = -1.0f;
        result.rotate.entry[2][2] = 1.0f;
        result.translate = translation;
        return result;
    }
}

int main()
{
    using namespace rock;

    using WarpAction = held_player_space_warp_policy::Action;
    using WarpInput = held_player_space_warp_policy::Input;
    const auto completeWarp = held_player_space_warp_policy::evaluate(
        WarpInput{
            .requiredTransformWrites = 4,
            .queuedTransformWrites = 4,
            .requiredVelocityWrites = 3,
            .queuedVelocityWrites = 3,
            .preflightComplete = true,
        });
    Require(completeWarp.action == WarpAction::Commit &&
                completeWarp.rebaseHands &&
                completeWarp.advancePlayerSpaceBaseline &&
                !completeWarp.freezeHeldUpdate &&
                !completeWarp.invalidateGrab,
        "only a complete held-body, motion, and proxy warp may commit/rebase");

    const auto preflightFailure = held_player_space_warp_policy::evaluate(
        WarpInput{
            .requiredTransformWrites = 4,
            .requiredVelocityWrites = 3,
            .consecutiveRetryCount = 1,
            .maxSafeRetries = 3,
            .preflightComplete = false,
        });
    Require(preflightFailure.action == WarpAction::Retry &&
                !preflightFailure.rebaseHands &&
                !preflightFailure.advancePlayerSpaceBaseline &&
                preflightFailure.freezeHeldUpdate &&
                !preflightFailure.invalidateGrab,
        "preflight failure before native writes must preserve the old basis for retry");

    const auto rolledBackPartial = held_player_space_warp_policy::evaluate(
        WarpInput{
            .requiredTransformWrites = 4,
            .queuedTransformWrites = 4,
            .requiredVelocityWrites = 3,
            .queuedVelocityWrites = 2,
            .rollbackTransformWritesQueued = 4,
            .rollbackVelocityWritesQueued = 2,
            .consecutiveRetryCount = 1,
            .maxSafeRetries = 3,
            .preflightComplete = true,
        });
    Require(rolledBackPartial.action == WarpAction::Retry &&
                !rolledBackPartial.rebaseHands &&
                !rolledBackPartial.advancePlayerSpaceBaseline &&
                rolledBackPartial.freezeHeldUpdate &&
                !rolledBackPartial.invalidateGrab,
        "a fully rolled-back partial queue must retry without mixing world bases");

    const auto unsafePartial = held_player_space_warp_policy::evaluate(
        WarpInput{
            .requiredTransformWrites = 4,
            .queuedTransformWrites = 4,
            .requiredVelocityWrites = 3,
            .queuedVelocityWrites = 2,
            .rollbackTransformWritesQueued = 3,
            .rollbackVelocityWritesQueued = 2,
            .consecutiveRetryCount = 1,
            .maxSafeRetries = 3,
            .preflightComplete = true,
        });
    Require(unsafePartial.action == WarpAction::FailClosed &&
                !unsafePartial.rebaseHands &&
                unsafePartial.advancePlayerSpaceBaseline &&
                unsafePartial.freezeHeldUpdate &&
                unsafePartial.invalidateGrab,
        "incomplete rollback must fail the grab closed instead of retrying a mixed basis");

    const auto exhaustedRetry = held_player_space_warp_policy::evaluate(
        WarpInput{
            .requiredTransformWrites = 4,
            .requiredVelocityWrites = 3,
            .consecutiveRetryCount = 3,
            .maxSafeRetries = 3,
            .preflightComplete = false,
        });
    Require(exhaustedRetry.action == WarpAction::FailClosed &&
                exhaustedRetry.invalidateGrab &&
                exhaustedRetry.advancePlayerSpaceBaseline,
        "a stale required body or proxy must fail closed after bounded retries");

    const Transform previous = IdentityTransform(Vec3{ 10.0f, 20.0f, 30.0f });
    const Transform current = Yaw90Transform(Vec3{ 500.0f, -200.0f, 90.0f });
    const Vec3 reoriented =
        held_player_space_math::reorientWorldVectorThroughPlayerSpace(
            previous,
            current,
            Vec3{ 1.0f, 0.0f, 0.0f });
    Require(Near(reoriented.x, 0.0f) && Near(reoriented.y, 1.0f) &&
                Near(reoriented.z, 0.0f),
        "snap-turn velocity must preserve its room-local direction in the new world basis");

    const Vec3 translatedOnly =
        held_player_space_math::reorientWorldVectorThroughPlayerSpace(
            IdentityTransform(Vec3{ -999.0f, 400.0f, 70.0f }),
            IdentityTransform(Vec3{ 1234.0f, -25.0f, 8.0f }),
            Vec3{ 3.0f, -4.0f, 5.0f });
    Require(Near(translatedOnly.x, 3.0f) && Near(translatedOnly.y, -4.0f) &&
                Near(translatedOnly.z, 5.0f),
        "teleport translation must not alter linear or angular vectors");

    held_player_space_registry::RuntimeHeldPlayerSpaceResult warpResult{};
    warpResult.warpedMotionIndices = { 41, 77 };
    Require(warpResult.motionWasWarped(41) &&
                warpResult.motionWasWarped(77) &&
                !warpResult.motionWasWarped(42) &&
                !warpResult.motionWasWarped(0),
        "the successful discontinuity signal must remain exact per Havok motion");

    using TransportState =
        held_locomotion_transport_policy::RuntimeState<Vec3>;
    using TransportInput =
        held_locomotion_transport_policy::FrameInput<Vec3>;
    TransportState transport{};
    transport.appliedRoomVelocityGame = Vec3{ 400.0f, 0.0f, 0.0f };
    transport.lastProcessedGameFrameIndex = 88;
    transport.active = true;
    transport.hasProcessedGameFrame = true;
    held_locomotion_transport_policy::rebaseForPlayerSpaceWarp(
        transport,
        Vec3{ 0.0f, 400.0f, 0.0f });
    Require(transport.active && !transport.hasProcessedGameFrame &&
                Near(transport.appliedRoomVelocityGame.x, 0.0f) &&
                Near(transport.appliedRoomVelocityGame.y, 400.0f),
        "warp rebase must rotate the carried contribution and reopen same-frame reconciliation");
    const auto acceleratedAfterTurn =
        held_locomotion_transport_policy::evaluate(
            transport,
            TransportInput{
                88,
                Vec3{ 0.0f, 475.0f, 0.0f },
                true,
                true,
            });
    Require(acceleratedAfterTurn.applyVelocityDelta &&
                Near(acceleratedAfterTurn.velocityDeltaGame.x, 0.0f) &&
                Near(acceleratedAfterTurn.velocityDeltaGame.y, 75.0f),
        "post-turn transport must apply only the new room-velocity delta, not the old axis twice");

    const Vec3 oldLocalSample =
        held_locomotion_transport_policy::removeAppliedRoomVelocity(
            Vec3{ 450.0f, 0.0f, 0.0f },
            Vec3{ 400.0f, 0.0f, 0.0f });
    const Vec3 stoppedLocalSample =
        held_locomotion_transport_policy::removeAppliedRoomVelocity(
            Vec3{ 75.0f, 0.0f, 0.0f },
            Vec3{});
    const std::array<Vec3, 2> localHistory{
        oldLocalSample,
        stoppedLocalSample,
    };
    const Vec3 releaseLocal =
        held_object_physics_math::maxMagnitudeVelocity(
            localHistory,
            localHistory.size());
    Require(Near(oldLocalSample.x, 50.0f) && Near(releaseLocal.x, 75.0f),
        "each release sample must remove its own room contribution before history selection");
    Require(
        !held_locomotion_transport_policy::
            shouldRecordHeldReleaseMotionSample(true, false) &&
            !held_locomotion_transport_policy::
                shouldRecordHeldReleaseMotionSample(false, true) &&
            held_locomotion_transport_policy::
                shouldRecordHeldReleaseMotionSample(false, false),
        "a native warp must suppress both same-frame and deferred pre-warp release samples");

    TransportState inactive{};
    inactive.appliedRoomVelocityGame = Vec3{ 999.0f, 1.0f, 0.0f };
    inactive.hasProcessedGameFrame = true;
    held_locomotion_transport_policy::rebaseForPlayerSpaceWarp(
        inactive,
        Vec3{ 1.0f, 999.0f, 0.0f });
    Require(!inactive.active &&
                Near(inactive.appliedRoomVelocityGame.x, 0.0f) &&
                !inactive.hasProcessedGameFrame,
        "inactive transport state must not resurrect a stale contribution after a warp");

    std::cout << "Held player-space policy tests passed\n";
    return 0;
}
