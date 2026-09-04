#include "physics-interaction/visual/FrikCompatibilityPolicy.h"
#include "physics-interaction/RockLoggingPolicy.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>

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
    using namespace rock::frik_compatibility_policy;

    static_assert(
        rock::logging_policy::DefaultLogLevel ==
        static_cast<int>(rock::logging_policy::LogLevel::Warn));
    Require(
        rock::logging_policy::shouldEmit(
            rock::logging_policy::DefaultLogLevel,
            rock::logging_policy::LogLevel::Warn),
        "release logging must retain warnings at the configured warning level");
    Require(
        rock::logging_policy::shouldEmit(
            rock::logging_policy::DefaultLogLevel,
            rock::logging_policy::LogLevel::Error),
        "release logging must retain errors");

    static_assert(kStockFrik07712ApiVersion == 3);
    static_assert(kNativeVisualAuthorityApiVersion == 5);
    static_assert(kStockFrik07712ApiFunctionCount == 18);
    static_assert(kCanonicalFrikV5ApiFunctionCount == 28);
    static_assert(
        static_cast<std::uint32_t>(
            ApiFunctionSlot::BlockOffHandWeaponGripping) +
            1u ==
        kStockFrik07712ApiFunctionCount);
    static_assert(
        static_cast<std::uint32_t>(
            ApiFunctionSlot::SetHandPoseCustom) ==
        kStockFrik07712ApiFunctionCount);
    static_assert(
        static_cast<std::uint32_t>(
            ApiFunctionSlot::BlockPrimaryHandWeaponPose) +
            1u ==
        kCanonicalFrikV5ApiFunctionCount);
    static_assert(std::is_trivially_copyable_v<HostHandMapping>);
    static_assert(std::is_trivially_copyable_v<StockV3PoseMapping>);
    static_assert(std::is_trivially_copyable_v<FingerPoseBackendSelection>);

    Require(
        mayReadApiFunctionSlot(
            true,
            3,
            ApiFunctionSlot::BlockOffHandWeaponGripping),
        "stock v3 must expose its final prefix slot");
    Require(
        !mayReadApiFunctionSlot(
            true,
            3,
            ApiFunctionSlot::SetHandPoseCustom),
        "stock v3 must not read the first appended v5 slot");
    Require(
        !mayReadApiFunctionSlot(
            true,
            3,
            ApiFunctionSlot::ApplyExternalHandWorldTransform),
        "stock v3 must not probe a visual-authority tail slot");
    Require(
        !mayReadApiFunctionSlot(
            true,
            4,
            ApiFunctionSlot::SetHandPoseWithPriority),
        "API v4 must not be mistaken for ROCK's incompatible canonical-v5 tail");
    Require(
        mayReadApiFunctionSlot(
            true,
            5,
            ApiFunctionSlot::BlockPrimaryHandWeaponPose),
        "canonical v5 must expose its final required slot");
    Require(
        !mayReadApiFunctionSlot(
            false,
            5,
            ApiFunctionSlot::GetVersion),
        "a version value without an initialized table must expose no slot");

    // Stock 0.77.12 keeps its v3 core table while routing hand-world writes
    // through Heisenberg's host callbacks. It must never select/read the v5 tail.
    Require(shouldRetainCoreApiInstance(true), "successful stock API init must retain the core pointer");
    Require(hasStockV3ApiPrefix(true, 3), "stock FRIK 0.77.12 must expose the v3 prefix");
    Require(!mayReadNativeVisualAuthorityTail(true, 3), "stock v3 must not read the v5 table tail");
    Require(
        selectVisualAuthorityBackend(true, 3, true) == VisualAuthorityBackend::EmbeddedHost,
        "stock v3 must select embedded-host hand authority");
    Require(
        selectVisualAuthorityBackend(true, 3, false) == VisualAuthorityBackend::None,
        "stock v3 without a host operation must fail safely");

    // API v5 uses its native tail even when a host callback is also registered.
    Require(mayReadNativeVisualAuthorityTail(true, 5), "v5 may read native authority members");
    Require(
        selectVisualAuthorityBackend(true, 5, true) == VisualAuthorityBackend::NativeV5,
        "v5 must prefer native visual authority");
    Require(
        selectVisualAuthorityBackend(false, 5, true) == VisualAuthorityBackend::None,
        "a version number without a successfully initialized API is not a provider");

    Require(
        mayConsumeEmbeddedGoalAuthorityOnPass(true, true),
        "the offhand FRIK goal pass must remain available while a weapon is drawn");
    Require(
        !mayConsumeEmbeddedGoalAuthorityOnPass(false, true),
        "a drawn primary weapon must retain the weapon-subtree feedback guard");
    Require(
        mayConsumeEmbeddedGoalAuthorityOnPass(false, false),
        "an empty primary hand must use FRIK's coherent goal pass instead of late bone IK");

    // Full finger-pose fidelity follows the same compatibility rule, but keeps
    // the stock five-scalar call as a fail-open baseline for the host backend.
    auto fingerBackend = selectFingerPoseBackend(true, 3, false, true);
    Require(
        fingerBackend.backend == FingerPoseBackend::EmbeddedHost &&
            fingerBackend.publishStockScalarBaseline &&
            fingerBackend.preservesCanonicalFullPose,
        "stock FRIK 0.77.12 must use the host full-pose backend plus scalar baseline");

    fingerBackend = selectFingerPoseBackend(true, 3, false, false);
    Require(
        fingerBackend.backend == FingerPoseBackend::StockV3Scalar &&
            fingerBackend.publishStockScalarBaseline &&
            !fingerBackend.preservesCanonicalFullPose,
        "stock v3 without the host backend must degrade to its five-scalar pose");

    fingerBackend = selectFingerPoseBackend(true, 5, true, true);
    Require(
        fingerBackend.backend == FingerPoseBackend::NativeV5 &&
            !fingerBackend.publishStockScalarBaseline &&
            fingerBackend.preservesCanonicalFullPose,
        "native v5 must win over the embedded host backend");

    fingerBackend = selectFingerPoseBackend(true, 5, false, true);
    Require(
        fingerBackend.backend == FingerPoseBackend::EmbeddedHost &&
            fingerBackend.publishStockScalarBaseline &&
            fingerBackend.preservesCanonicalFullPose,
        "a v5 provider missing the required operation must fail over to the host");

    fingerBackend = selectFingerPoseBackend(false, 5, true, true);
    Require(
        fingerBackend.backend == FingerPoseBackend::None &&
            !fingerBackend.publishStockScalarBaseline &&
            !fingerBackend.preservesCanonicalFullPose,
        "an uninitialized API must not select any finger-pose backend");

    fingerBackend = selectFingerPoseBackend(true, 2, false, true);
    Require(
        fingerBackend.backend == FingerPoseBackend::None,
        "an API older than the known 0.77.12 prefix must not use the host backend");

    auto suppression = resolveOffhandGripSuppressionTransition(
        OffhandGripSuppressionState::Restored,
        OffhandGripSuppressionEvent::AcquisitionAttempt);
    Require(
        suppression.next == OffhandGripSuppressionState::Restored &&
            !suppression.writeRequired,
        "a failed/in-progress acquisition attempt must not flap FRIK suppression");
    suppression = resolveOffhandGripSuppressionTransition(
        OffhandGripSuppressionState::Restored,
        OffhandGripSuppressionEvent::AcquisitionSucceeded);
    Require(
        suppression.next == OffhandGripSuppressionState::Suppressed &&
            suppression.writeRequired,
        "a successful acquisition must suppress once");
    suppression = resolveOffhandGripSuppressionTransition(
        suppression.next,
        OffhandGripSuppressionEvent::AcquisitionSucceeded);
    Require(
        suppression.next == OffhandGripSuppressionState::Suppressed &&
            !suppression.writeRequired,
        "repeated calls within one grip episode must be idempotent");
    suppression = resolveOffhandGripSuppressionTransition(
        suppression.next,
        OffhandGripSuppressionEvent::GripEpisodeEnded);
    Require(
        suppression.next == OffhandGripSuppressionState::Restored &&
            suppression.writeRequired,
        "episode end must restore exactly once");
    suppression = resolveOffhandGripSuppressionTransition(
        suppression.next,
        OffhandGripSuppressionEvent::GripEpisodeEnded);
    Require(
        suppression.next == OffhandGripSuppressionState::Restored &&
            !suppression.writeRequired,
        "repeated cleanup must not issue duplicate restore writes");

    // FRIK's public Hand values (2/3) are not the host callback ABI values (0/1).
    auto hand = mapFrikHandToHost(FrikHand::Right);
    Require(hand.valid && hand.hand == HostHand::Right, "FRIK Right must map to host hand 0");
    hand = mapFrikHandToHost(FrikHand::Left);
    Require(hand.valid && hand.hand == HostHand::Left, "FRIK Left must map to host hand 1");
    hand = mapFrikHandToHost(FrikHand::Primary, false);
    Require(hand.valid && hand.hand == HostHand::Right, "right-handed Primary must map right");
    hand = mapFrikHandToHost(FrikHand::Offhand, false);
    Require(hand.valid && hand.hand == HostHand::Left, "right-handed Offhand must map left");
    hand = mapFrikHandToHost(FrikHand::Primary, true);
    Require(hand.valid && hand.hand == HostHand::Left, "left-handed Primary must map left");
    hand = mapFrikHandToHost(FrikHand::Offhand, true);
    Require(hand.valid && hand.hand == HostHand::Right, "left-handed Offhand must map right");

    // Exact semantic mappings protect against the incompatible v3/v5 enum ordinals.
    auto pose = mapCanonicalPoseToStockV3(CanonicalHandPose::Pointing);
    Require(
        pose.supported && pose.pose == StockV3HandPose::Pointing &&
            static_cast<unsigned>(pose.pose) == 4,
        "canonical Pointing must map to stock-v3 ordinal 4");
    pose = mapCanonicalPoseToStockV3(CanonicalHandPose::Fist);
    Require(
        pose.supported && pose.pose == StockV3HandPose::Fist &&
            static_cast<unsigned>(pose.pose) == 3,
        "canonical Fist must map to stock-v3 ordinal 3");
    pose = mapCanonicalPoseToStockV3(CanonicalHandPose::HoldingGun);
    Require(
        pose.supported && pose.pose == StockV3HandPose::HoldingGun &&
            static_cast<unsigned>(pose.pose) == 5,
        "canonical HoldingGun must map to stock-v3 ordinal 5");
    pose = mapCanonicalPoseToStockV3(CanonicalHandPose::HoldingMelee);
    Require(
        pose.supported && pose.pose == StockV3HandPose::HoldingMelee &&
            static_cast<unsigned>(pose.pose) == 6,
        "canonical HoldingMelee must map to stock-v3 ordinal 6");
    Require(
        !mapCanonicalPoseToStockV3(CanonicalHandPose::OffhandGrip).supported,
        "v5-only OffhandGrip must not be passed to the stock-v3 pose API");

    CollisionCreationRequestInput collision{
        .rockEnabled = true,
        .frikProviderAvailable = true,
        .localSkeletonReady = true,
        .interactionExists = false,
        .interactionInitialized = false,
    };
    Require(
        shouldRequestCollisionCreation(collision),
        "stock v3 must request collision creation without an existing interaction");

    collision.interactionExists = true;
    Require(
        shouldRequestCollisionCreation(collision),
        "an existing but uninitialized interaction must remain recoverable");

    collision.interactionInitialized = true;
    Require(
        !shouldRequestCollisionCreation(collision),
        "an initialized interaction must not be recreated");

    collision.frikProviderAvailable = false;
    Require(
        !shouldRequestCollisionCreation(collision),
        "a genuinely unavailable FRIK provider must block collision creation");

    return 0;
}
