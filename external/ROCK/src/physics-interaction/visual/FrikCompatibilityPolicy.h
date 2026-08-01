#pragma once

#include <cstdint>

namespace rock::frik_compatibility_policy
{
    // The public API shipped by the stock FRIK 0.77.12 release.
    inline constexpr std::uint32_t kStockFrik07712ApiVersion = 3;

    // The canonical hand-pose and native hand-world authority tail is only
    // present in the rebuilt API-v5 FRIK. Reading those members from the
    // shorter v3 table is an out-of-bounds access even if the bytes beyond the
    // table happen to look like null pointers.
    inline constexpr std::uint32_t kNativeVisualAuthorityApiVersion = 5;

    /*
     * Function-table slots are part of FRIK's binary ABI. Keep the stock-v3
     * prefix and the canonical v5 boundary expressible without including the
     * Windows/NetImmerse-heavy API header in pure policy tests.
     *
     * Stock FRIK 0.77.12 allocates exactly slots 0..17. Reading slot 18 merely
     * to test it for null is already out of bounds. Canonical API v5 extends
     * that same prefix through slot 27.
     */
    enum class ApiFunctionSlot : std::uint32_t
    {
        GetVersion = 0,
        GetModVersion = 1,
        IsSkeletonReady = 2,
        IsConfigOpen = 3,
        IsSelfieModeOn = 4,
        SetSelfieModeOn = 5,
        IsOffHandGrippingWeapon = 6,
        IsWristPipboyOpen = 7,
        GetIndexFingerTipPosition = 8,
        GetHandPoseSetTagState = 9,
        GetCurrentHandPose = 10,
        SetHandPose = 11,
        SetHandPoseCustomFingerPositions = 12,
        ClearHandPose = 13,
        SetHandPoseFingerPositions = 14,
        ClearHandPoseFingerPositions = 15,
        RegisterOpenModSettingButtonToMainConfig = 16,
        BlockOffHandWeaponGripping = 17,

        SetHandPoseCustom = 18,
        SetHandPoseWithPriority = 19,
        GetHandWorldTransform = 20,
        SetHandPoseCustomFingerPositionsWithPriority = 21,
        SetHandPoseCustomWithPriority = 22,
        ApplyExternalHandWorldTransform = 23,
        ClearExternalHandWorldTransform = 24,
        SetHandPoseCustomLocalTransformsWithPriority = 25,
        GetHandPoseLocalTransformsForPose = 26,
        BlockPrimaryHandWeaponPose = 27,
    };

    inline constexpr std::uint32_t kStockFrik07712ApiFunctionCount =
        static_cast<std::uint32_t>(ApiFunctionSlot::BlockOffHandWeaponGripping) + 1u;
    inline constexpr std::uint32_t kCanonicalFrikV5ApiFunctionCount =
        static_cast<std::uint32_t>(ApiFunctionSlot::BlockPrimaryHandWeaponPose) + 1u;

    static_assert(kStockFrik07712ApiFunctionCount == 18u);
    static_assert(
        static_cast<std::uint32_t>(ApiFunctionSlot::SetHandPoseCustom) ==
        kStockFrik07712ApiFunctionCount);
    static_assert(kCanonicalFrikV5ApiFunctionCount == 28u);

    [[nodiscard]] inline constexpr bool mayReadApiFunctionSlot(
        const bool apiInitializationSucceeded,
        const std::uint32_t apiVersion,
        const ApiFunctionSlot slot)
    {
        if (!apiInitializationSucceeded ||
            apiVersion < kStockFrik07712ApiVersion) {
            return false;
        }

        const auto index = static_cast<std::uint32_t>(slot);
        if (index < kStockFrik07712ApiFunctionCount) {
            return true;
        }
        return apiVersion >= kNativeVisualAuthorityApiVersion &&
            index < kCanonicalFrikV5ApiFunctionCount;
    }

    enum class FrikHand : std::uint8_t
    {
        Primary = 0,
        Offhand = 1,
        Right = 2,
        Left = 3,
    };

    enum class HostHand : std::uint8_t
    {
        Right = 0,
        Left = 1,
    };

    struct HostHandMapping
    {
        HostHand hand = HostHand::Right;
        bool valid = false;
    };

    // The embedded-host callback ABI uses 0=Right and 1=Left. FRIK's public
    // enum uses 2=Right and 3=Left, so a static_cast is not an ABI mapping.
    [[nodiscard]] inline constexpr HostHandMapping mapFrikHandToHost(
        const FrikHand hand,
        const bool primaryHandIsLeft = false)
    {
        switch (hand) {
        case FrikHand::Primary:
            return {
                .hand = primaryHandIsLeft ? HostHand::Left : HostHand::Right,
                .valid = true,
            };
        case FrikHand::Offhand:
            return {
                .hand = primaryHandIsLeft ? HostHand::Right : HostHand::Left,
                .valid = true,
            };
        case FrikHand::Right:
            return { .hand = HostHand::Right, .valid = true };
        case FrikHand::Left:
            return { .hand = HostHand::Left, .valid = true };
        default:
            return {};
        }
    }

    // API-v5 canonical pose values are not numerically compatible with the
    // stock v3 enum after Open. Keep the translation explicit so Pointing,
    // Fist, HoldingGun, and HoldingMelee cannot silently become other poses.
    enum class CanonicalHandPose : std::uint8_t
    {
        Unset = 0,
        Custom = 1,
        Open = 2,
        Pointing = 3,
        HoldingWeapon = 4,
        OffhandGrip = 5,
        Attaboy = 6,
        ThumbsUp = 7,
        Fist = 8,
        HoldingGun = 9,
        HoldingMelee = 10,
    };

    enum class StockV3HandPose : std::uint8_t
    {
        Unset = 0,
        Custom = 1,
        Open = 2,
        Fist = 3,
        Pointing = 4,
        HoldingGun = 5,
        HoldingMelee = 6,
    };

    struct StockV3PoseMapping
    {
        StockV3HandPose pose = StockV3HandPose::Unset;
        bool supported = false;
    };

    [[nodiscard]] inline constexpr StockV3PoseMapping mapCanonicalPoseToStockV3(
        const CanonicalHandPose pose)
    {
        switch (pose) {
        case CanonicalHandPose::Unset:
            return { .pose = StockV3HandPose::Unset, .supported = true };
        case CanonicalHandPose::Custom:
            return { .pose = StockV3HandPose::Custom, .supported = true };
        case CanonicalHandPose::Open:
            return { .pose = StockV3HandPose::Open, .supported = true };
        case CanonicalHandPose::Pointing:
            return { .pose = StockV3HandPose::Pointing, .supported = true };
        case CanonicalHandPose::Fist:
            return { .pose = StockV3HandPose::Fist, .supported = true };
        case CanonicalHandPose::HoldingGun:
            return { .pose = StockV3HandPose::HoldingGun, .supported = true };
        case CanonicalHandPose::HoldingMelee:
            return { .pose = StockV3HandPose::HoldingMelee, .supported = true };
        case CanonicalHandPose::HoldingWeapon:
        case CanonicalHandPose::OffhandGrip:
        case CanonicalHandPose::Attaboy:
        case CanonicalHandPose::ThumbsUp:
        default:
            return {};
        }
    }

    enum class VisualAuthorityBackend : std::uint8_t
    {
        None = 0,
        NativeV5,
        EmbeddedHost,
    };

    /*
     * Finger-pose publication is separate from whole-hand world authority.
     * The stock-v3 scalar API remains a useful fail-open baseline, but cannot
     * represent independent joints, splay, palm motion, or local-transform
     * corrections. The embedded host backend supplies that missing fidelity
     * for FRIK 0.77.12 while native v5 remains preferred when available.
     */
    enum class FingerPoseBackend : std::uint8_t
    {
        None = 0,
        StockV3Scalar,
        EmbeddedHost,
        NativeV5,
    };

    struct FingerPoseBackendSelection
    {
        FingerPoseBackend backend = FingerPoseBackend::None;
        bool publishStockScalarBaseline = false;
        bool preservesCanonicalFullPose = false;
    };

    [[nodiscard]] inline constexpr FingerPoseBackendSelection
    selectFingerPoseBackend(
        const bool apiInitializationSucceeded,
        const std::uint32_t apiVersion,
        const bool nativeFullPoseOperationAvailable,
        const bool hostFullPoseOperationAvailable)
    {
        if (!apiInitializationSucceeded ||
            apiVersion < kStockFrik07712ApiVersion) {
            return {};
        }

        if (apiVersion >= kNativeVisualAuthorityApiVersion &&
            nativeFullPoseOperationAvailable) {
            return {
                .backend = FingerPoseBackend::NativeV5,
                .publishStockScalarBaseline = false,
                .preservesCanonicalFullPose = true,
            };
        }

        if (hostFullPoseOperationAvailable) {
            return {
                .backend = FingerPoseBackend::EmbeddedHost,
                // Keep the stock API engaged as a fail-open base pose. The
                // host applies the full-pose correction after FRIK's pass.
                .publishStockScalarBaseline = true,
                .preservesCanonicalFullPose = true,
            };
        }

        return {
            .backend = FingerPoseBackend::StockV3Scalar,
            .publishStockScalarBaseline = true,
            .preservesCanonicalFullPose = false,
        };
    }

    enum class OffhandGripSuppressionState : std::uint8_t
    {
        Restored = 0,
        Suppressed,
    };

    enum class OffhandGripSuppressionEvent : std::uint8_t
    {
        AcquisitionAttempt = 0,
        AcquisitionSucceeded,
        GripEpisodeEnded,
    };

    struct OffhandGripSuppressionTransition
    {
        OffhandGripSuppressionState next =
            OffhandGripSuppressionState::Restored;
        bool writeRequired = false;
    };

    /*
     * Failed acquisition attempts are deliberately inert. Suppress only after
     * the support grip has actually been captured, keep that state for the
     * entire grip episode, and restore once when the episode ends.
     */
    [[nodiscard]] inline constexpr OffhandGripSuppressionTransition
    resolveOffhandGripSuppressionTransition(
        const OffhandGripSuppressionState current,
        const OffhandGripSuppressionEvent event)
    {
        switch (event) {
        case OffhandGripSuppressionEvent::AcquisitionSucceeded:
            return current == OffhandGripSuppressionState::Restored ?
                OffhandGripSuppressionTransition{
                    .next = OffhandGripSuppressionState::Suppressed,
                    .writeRequired = true,
                } :
                OffhandGripSuppressionTransition{ .next = current };
        case OffhandGripSuppressionEvent::GripEpisodeEnded:
            return current == OffhandGripSuppressionState::Suppressed ?
                OffhandGripSuppressionTransition{
                    .next = OffhandGripSuppressionState::Restored,
                    .writeRequired = true,
                } :
                OffhandGripSuppressionTransition{ .next = current };
        case OffhandGripSuppressionEvent::AcquisitionAttempt:
        default:
            return { .next = current };
        }
    }

    [[nodiscard]] inline constexpr bool shouldRetainCoreApiInstance(
        const bool apiInitializationSucceeded)
    {
        return apiInitializationSucceeded;
    }

    [[nodiscard]] inline constexpr bool hasStockV3ApiPrefix(
        const bool apiInitializationSucceeded,
        const std::uint32_t apiVersion)
    {
        return apiInitializationSucceeded && apiVersion >= kStockFrik07712ApiVersion;
    }

    [[nodiscard]] inline constexpr bool mayReadNativeVisualAuthorityTail(
        const bool apiInitializationSucceeded,
        const std::uint32_t apiVersion)
    {
        return apiInitializationSucceeded && apiVersion >= kNativeVisualAuthorityApiVersion;
    }

    // Select the backend before reading any v5 function pointer. On stock v3,
    // native authority is impossible and the host callback is the only valid
    // hand-world path. Core API availability remains independent of this
    // optional visual-authority choice.
    [[nodiscard]] inline constexpr VisualAuthorityBackend selectVisualAuthorityBackend(
        const bool apiInitializationSucceeded,
        const std::uint32_t apiVersion,
        const bool hostOperationAvailable)
    {
        if (!apiInitializationSucceeded) {
            return VisualAuthorityBackend::None;
        }
        if (mayReadNativeVisualAuthorityTail(true, apiVersion)) {
            return VisualAuthorityBackend::NativeV5;
        }
        return hostOperationAvailable ?
            VisualAuthorityBackend::EmbeddedHost :
            VisualAuthorityBackend::None;
    }

    /*
     * The stock-0.77.12 host seam obtains coherent whole-arm authority by
     * repeating FRIK's own first-person arm pass with a substituted goal.
     * Repeating the offhand pass is always safe. Repeating the primary pass
     * while a weapon is drawn would also move its child weapon subtree and
     * feed that output into ROCK's next weapon solve. With the primary hand
     * empty/holstered there is no such weapon owner, so refusing that pass
     * needlessly falls back to late bone IK and can deform the gun-side arm
     * during sustained wall contact.
     */
    [[nodiscard]] inline constexpr bool mayConsumeEmbeddedGoalAuthorityOnPass(
        const bool offhandPass,
        const bool playerWeaponDrawn)
    {
        return offhandPass || !playerWeaponDrawn;
    }

    struct CollisionCreationRequestInput
    {
        bool rockEnabled = false;
        bool frikProviderAvailable = false;
        bool localSkeletonReady = false;
        bool interactionExists = false;
        bool interactionInitialized = false;
    };

    // Collision construction depends on the FRIK provider/lifecycle and the
    // locally sampled skeleton, not on the optional API-v5 visual-authority
    // tail. An allocated-but-uninitialized interaction must remain recoverable.
    [[nodiscard]] inline constexpr bool shouldRequestCollisionCreation(
        const CollisionCreationRequestInput& input)
    {
        return input.rockEnabled &&
            input.frikProviderAvailable &&
            input.localSkeletonReady &&
            (!input.interactionExists || !input.interactionInitialized);
    }
}
