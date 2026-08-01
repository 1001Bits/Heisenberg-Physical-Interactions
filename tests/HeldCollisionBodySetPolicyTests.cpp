#include "HeldCollisionBodySetPolicy.h"

#include <array>
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
    using namespace
        heisenberg::held_collision_body_set_policy;

    std::array<BodyCandidate, 5> multipart{ {
        { 101u, 10u, false, false },
        { 102u, 10u, false, true },
        { 103u, 10u, true, true },
        { 104u, 11u, false, true },
        { 105u, 12u, false, false },
    } };
    const auto selected =
        SelectMotionRepresentatives(
            multipart,
            multipart.size());
    Require(selected.valid, "a unique multipart body set must be accepted");
    Require(
        selected.uniqueMotionCount == 3,
        "five bodies sharing three motions must produce three writes");
    Require(
        selected.representatives[2],
        "the selected wrapper must represent its shared motion");
    Require(
        !selected.representatives[0] &&
            !selected.representatives[1],
        "hidden and merely visible peers must not duplicate the selected motion write");
    Require(
        selected.representatives[3] &&
            selected.representatives[4],
        "independent visible and hidden motions each need one representative");

    std::array<BodyCandidate, 2> visibleBeatsHidden{ {
        { 201u, 20u, false, false },
        { 202u, 20u, false, true },
    } };
    const auto visibleSelection =
        SelectMotionRepresentatives(
            visibleBeatsHidden,
            visibleBeatsHidden.size());
    Require(
        visibleSelection.valid &&
            !visibleSelection.representatives[0] &&
            visibleSelection.representatives[1],
        "a rendered wrapper must represent a motion ahead of its hidden alternate");
    Require(
        ShouldKeepNativeCollision(false, true),
        "every rendered wrapper must keep collision authority");
    Require(
        ShouldKeepNativeCollision(true, false),
        "physical-touch acquisition authority remains collidable defensively");
    Require(
        !ShouldKeepNativeCollision(false, false),
        "a genuinely hidden alternate must use the non-collidable hold layer");

    constexpr std::uint32_t nativeClutterFilter = 0x15C80004u;
    constexpr std::uint32_t heldFilter =
        HeldBodyFilter(nativeClutterFilter, true);
    Require(
        heldFilter == 0x15C80021u,
        "a rendered held body must move to BIPED_NO_CC without altering upper filter bits");
    Require(
        (heldFilter & ~kCollisionLayerMask) ==
            (nativeClutterFilter & ~kCollisionLayerMask),
        "held filtering must preserve all group, flag, and system metadata");
    Require(
        HeldBodyFilter(nativeClutterFilter, false) == 0x15C8000Fu,
        "a hidden alternate wrapper must move to the non-collidable layer");

    constexpr std::uint32_t alreadyHandGrouped =
        0x000BC084u;
    constexpr std::uint32_t alreadyHandGroupedHeld =
        HeldBodyFilter(alreadyHandGrouped, true);
    Require(
        alreadyHandGroupedHeld == 0x000BC0A1u,
        "layer replacement must preserve pre-existing group and suppression bits exactly");
    Require(
        (alreadyHandGroupedHeld & ~kCollisionLayerMask) ==
            (alreadyHandGrouped & ~kCollisionLayerMask),
        "BIPED_NO_CC must not OR or rewrite the existing collision group");

    std::array<BodyCandidate, 2> duplicateBody{ {
        { 301u, 30u, false, true },
        { 301u, 31u, true, true },
    } };
    Require(
        !SelectMotionRepresentatives(
             duplicateBody,
             duplicateBody.size())
             .valid,
        "duplicate body identity must invalidate the transactional set");

    Require(
        !SelectMotionRepresentatives(multipart, 0).valid,
        "an empty capture must not authorize recursive motion scope");
    Require(
        !SelectMotionRepresentatives(
             multipart,
             multipart.size() + 1)
             .valid,
        "a count beyond fixed capacity must fail closed");

    return 0;
}
