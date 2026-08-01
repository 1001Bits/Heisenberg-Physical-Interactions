#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <type_traits>

#include "api/FRIKApi.h"
#include "physics-interaction/visual/FrikVisualAuthorityBridge.h"

namespace
{
    using Api = frik::api::FRIKApi;
    using HandPoseData = Api::HandPoseData;
    using HandPoseKind = Api::HandPoseKind;

    void Require(const bool condition, const char* const message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    constexpr std::size_t kFunctionSlotSize =
        sizeof(decltype(Api::getVersion));

#define REQUIRE_API_SLOT(member, slot)                                      \
    static_assert(                                                         \
        offsetof(Api, member) == (slot) * kFunctionSlotSize,               \
        "FRIK API member moved to a different binary function-table slot")

    static_assert(std::is_standard_layout_v<Api>);
    static_assert(sizeof(Api::FingerPoseData) == sizeof(float) * 4);
    static_assert(sizeof(HandPoseData) == sizeof(float) * 22);

    REQUIRE_API_SLOT(getVersion, 0);
    REQUIRE_API_SLOT(setHandPoseCustomFingerPositions, 12);
    REQUIRE_API_SLOT(blockOffHandWeaponGripping, 17);
    REQUIRE_API_SLOT(setHandPoseCustom, 18);
    REQUIRE_API_SLOT(setHandPoseWithPriority, 19);
    REQUIRE_API_SLOT(getHandWorldTransform, 20);
    REQUIRE_API_SLOT(setHandPoseCustomFingerPositionsWithPriority, 21);
    REQUIRE_API_SLOT(setHandPoseCustomWithPriority, 22);
    REQUIRE_API_SLOT(applyExternalHandWorldTransform, 23);
    REQUIRE_API_SLOT(clearExternalHandWorldTransform, 24);
    REQUIRE_API_SLOT(setHandPoseCustomLocalTransformsWithPriority, 25);
    REQUIRE_API_SLOT(getHandPoseLocalTransformsForPose, 26);
    REQUIRE_API_SLOT(blockPrimaryHandWeaponPose, 27);
    REQUIRE_API_SLOT(blockPrimaryWeaponNodeOwnership, 28);
    static_assert(sizeof(Api) == 29 * kFunctionSlotSize);

#undef REQUIRE_API_SLOT

    using SetStockScalarPoseFn = bool(FRIK_CALL*)(
        const char*,
        Api::Hand,
        float,
        float,
        float,
        float,
        float);
    using SetCanonicalPoseFn = bool(FRIK_CALL*)(
        const char*,
        Api::Hand,
        const HandPoseData&,
        int);
    using SetLocalTransformsFn = bool(FRIK_CALL*)(
        const char*,
        Api::Hand,
        const Api::FingerLocalTransformOverride*,
        int);
    using BuildLocalTransformsFn = bool(FRIK_CALL*)(
        Api::Hand,
        const HandPoseData&,
        Api::FingerLocalTransformOverride*);
    using BlockPrimaryPoseFn = bool(FRIK_CALL*)(const char*, bool);

    static_assert(std::same_as<
        decltype(Api::setHandPoseCustomFingerPositions),
        SetStockScalarPoseFn>);
    static_assert(std::same_as<
        decltype(Api::setHandPoseCustomWithPriority),
        SetCanonicalPoseFn>);
    static_assert(std::same_as<
        decltype(Api::setHandPoseCustomLocalTransformsWithPriority),
        SetLocalTransformsFn>);
    static_assert(std::same_as<
        decltype(Api::getHandPoseLocalTransformsForPose),
        BuildLocalTransformsFn>);
    static_assert(std::same_as<
        decltype(Api::blockPrimaryHandWeaponPose),
        BlockPrimaryPoseFn>);
    static_assert(std::same_as<
        decltype(Api::blockPrimaryWeaponNodeOwnership),
        BlockPrimaryPoseFn>);

    std::array<float, 15> JointValues(const HandPoseData& pose)
    {
        return {
            pose.thumb.prox,
            pose.thumb.mid,
            pose.thumb.dist,
            pose.index.prox,
            pose.index.mid,
            pose.index.dist,
            pose.middle.prox,
            pose.middle.mid,
            pose.middle.dist,
            pose.ring.prox,
            pose.ring.mid,
            pose.ring.dist,
            pose.pinky.prox,
            pose.pinky.mid,
            pose.pinky.dist,
        };
    }

    void RequireZeroSplayAndPalm(
        const HandPoseData& pose,
        const char* const message)
    {
        Require(
            pose.thumb.splay == 0.0f &&
                pose.index.splay == 0.0f &&
                pose.middle.splay == 0.0f &&
                pose.ring.splay == 0.0f &&
                pose.pinky.splay == 0.0f &&
                pose.palmPitch == 0.0f &&
                pose.palmYaw == 0.0f,
            message);
    }

    void RequireNamedPose(
        const HandPoseKind kind,
        const std::array<float, 15>& expected,
        const char* const valueMessage,
        const char* const auxiliaryMessage)
    {
        HandPoseData pose{};
        Require(
            rock::frik_visual_authority::makeStockV3NamedHandPoseData(
                kind,
                pose),
            "supported stock-v3 named pose was rejected");
        Require(JointValues(pose) == expected, valueMessage);
        RequireZeroSplayAndPalm(pose, auxiliaryMessage);
    }
}

int main()
{
    std::array<float, 15> open{};
    open.fill(1.0f);
    RequireNamedPose(
        HandPoseKind::Open,
        open,
        "Open must map all 15 joints to the stock-v3 straight value",
        "Open must not add splay or palm motion");

    RequireNamedPose(
        HandPoseKind::Fist,
        {},
        "Fist must map all 15 joints to the stock-v3 closed value",
        "Fist must not add splay or palm motion");

    RequireNamedPose(
        HandPoseKind::Pointing,
        {
            0.0f, 0.0f, 0.0f,
            1.0f, 1.0f, 1.0f,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f,
        },
        "Pointing must preserve the exact stock 0.77.12 authored joints",
        "Pointing must not add splay or palm motion");

    RequireNamedPose(
        HandPoseKind::HoldingGun,
        {
            0.7f, 0.4f, 0.5f,
            0.9f, 0.6f, 0.5f,
            0.3f, 0.5f, 0.5f,
            0.1f, 0.5f, 0.5f,
            0.0f, 0.5f, 0.7f,
        },
        "HoldingGun must preserve the exact stock 0.77.12 authored joints",
        "HoldingGun must not add splay or palm motion");

    RequireNamedPose(
        HandPoseKind::HoldingMelee,
        {
            0.7f, 0.5f, 0.8f,
            0.4f, 0.3f, 0.9f,
            0.1f, 0.5f, 0.9f,
            0.0f, 0.5f, 0.9f,
            0.0f, 0.4f, 0.9f,
        },
        "HoldingMelee must preserve the exact stock 0.77.12 authored joints",
        "HoldingMelee must not add splay or palm motion");

    constexpr std::array unsupported{
        HandPoseKind::Unset,
        HandPoseKind::Custom,
        HandPoseKind::HoldingWeapon,
        HandPoseKind::OffhandGrip,
        HandPoseKind::Attaboy,
        HandPoseKind::ThumbsUp,
    };
    for (const auto kind : unsupported) {
        HandPoseData pose{
            .thumb = { 1.0f, 1.0f, 1.0f, 1.0f },
            .palmPitch = 1.0f,
            .palmYaw = 1.0f,
        };
        Require(
            !rock::frik_visual_authority::makeStockV3NamedHandPoseData(
                kind,
                pose),
            "v5-only or non-authored named pose must be rejected on stock v3");
        Require(
            JointValues(pose) == std::array<float, 15>{},
            "rejected named pose must clear its joint output");
        RequireZeroSplayAndPalm(
            pose,
            "rejected named pose must clear its auxiliary output");
    }

    return 0;
}
