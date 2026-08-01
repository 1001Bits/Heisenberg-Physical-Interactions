#include "api/ROCKProviderApi.h"
#include "api/WeaponPartDriveLeasePolicy.h"
#include "physics-interaction/collision/ContactActivityTracker.h"
#include "physics-interaction/weapon/WeaponPartContactAcquisitionPolicy.h"
#include "physics-interaction/weapon/WeaponSemantics.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>

namespace
{
    struct LegacyPoint3
    {
        float x;
        float y;
        float z;
    };

    struct LegacyMatrix3
    {
        float data[3][3];
    };

    struct LegacyNiTransform
    {
        LegacyMatrix3 rot;
        LegacyPoint3 pos;
        float scale;
    };

    using namespace rock::provider;

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
    using namespace rock::provider;
    using namespace rock::provider::weapon_part_drive_lease_policy;

    static_assert(std::is_standard_layout_v<RockProviderTransform>);
    static_assert(std::is_trivially_copyable_v<RockProviderTransform>);
    static_assert(sizeof(RockProviderTransform) == 52);
    static_assert(ROCK_PROVIDER_MAX_WEAPON_PART_DRIVE_LEASE_FRAMES_V1 + 1u ==
                  ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1);
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartDrivePersistentLease) ==
                  (1u << 22));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartExclusiveExactContact) ==
                  (1u << 23));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartControlledRoot) ==
                  (1u << 24));
    static_assert(sizeof(RockProviderWeaponPartDriveTargetV1) == 192);
    static_assert(offsetof(RockProviderWeaponPartDriveTargetV1, controlledRoot) == 168);
    static_assert(sizeof(RockProviderWeaponPartMotionConstraintV1) == 192);
    static_assert(offsetof(RockProviderWeaponPartMotionConstraintV1, controlledRoot) == 168);
    static_assert(sizeof(RockProviderWeaponPartMotionConstraintResolutionV1) == 64);
    static_assert(
        RockProviderWeaponPartDriveSpaceV1::ControlledRootParentLocal ==
        RockProviderWeaponPartDriveSpaceV1::SourceParentLocal);
    LegacyNiTransform legacy{
        .rot = {
            .data = {
                { 1.0f, 2.0f, 3.0f },
                { 4.0f, 5.0f, 6.0f },
                { 7.0f, 8.0f, 9.0f },
            },
        },
        .pos = { 10.0f, 11.0f, 12.0f },
        .scale = 1.25f,
    };

    RockProviderTransform converted{};
    converted = legacy;
    for (std::size_t index = 0; index < 9; ++index) {
        Require(
            converted.rotate[index] == static_cast<float>(index + 1),
            "legacy rotation elements must be copied in order");
    }
    Require(converted.translate[0] == 10.0f, "legacy translation x must be copied");
    Require(converted.translate[1] == 11.0f, "legacy translation y must be copied");
    Require(converted.translate[2] == 12.0f, "legacy translation z must be copied");
    Require(converted.scale == 1.25f, "legacy scale must be copied");

    RockProviderTransform copied{};
    copied = converted;
    Require(copied.rotate[8] == 9.0f, "provider transform rotation must remain copyable");
    Require(copied.translate[1] == 11.0f, "provider transform translation must remain copyable");
    Require(copied.scale == 1.25f, "provider transform scale must remain copyable");

    constexpr std::uint64_t submittedAt = 1'000;
    static_assert(!isPersistent(1));
    static_assert(expiresAfterFrame(submittedAt, 1) == 1'001);
    static_assert(!isExpired(1'001, 1'001));
    static_assert(isExpired(1'001, 1'002));

    static_assert(isPersistent(ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1));
    static_assert(isPersistent(0xFFFF'FFFFu));
    static_assert(
        expiresAfterFrame(submittedAt, ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1) ==
        NEVER_EXPIRES_FRAME);
    static_assert(!isExpired(NEVER_EXPIRES_FRAME, (std::numeric_limits<std::uint64_t>::max)()));

    /*
     * Regression: an exclusive bolt target must not be satisfied by the
     * palm-ranked result merely because the bolt is near a physically touched
     * barrel. Only the exact physical bolt contact may pass.
     */
    constexpr std::uint32_t barrelBodyId = 100;
    constexpr std::uint32_t boltBodyId = 200;
    const bool barrelMatched = barrelBodyId == boltBodyId;
    const bool boltMatched = boltBodyId == boltBodyId;

    using rock::weapon_part_contact_acquisition_policy::AcquisitionMode;
    const auto exactMode = rock::weapon_part_contact_acquisition_policy::selectMode(true);
    Require(
        exactMode == AcquisitionMode::ExactPhysicalContact,
        "an active whitelist must select exact physical contact mode");
    Require(
        !rock::weapon_part_contact_acquisition_policy::mayUseRankedPalmProbe(exactMode),
        "exact mode must reject the ranked palm probe");
    Require(
        !rock::weapon_part_contact_acquisition_policy::mayAcceptExactPhysicalContact(
            exactMode,
            barrelMatched),
        "a touched neighboring barrel must not satisfy the exact bolt target");
    Require(
        rock::weapon_part_contact_acquisition_policy::mayAcceptExactPhysicalContact(
            exactMode,
            boltMatched),
        "a physical contact on the exact bolt target must be accepted");
    Require(
        !rock::weapon_part_contact_acquisition_policy::mayRecoverExactTargetMeshContact(
            exactMode,
            barrelMatched,
            true),
        "mesh overlap must not recover a non-target barrel");
    Require(
        !rock::weapon_part_contact_acquisition_policy::mayRecoverExactTargetMeshContact(
            exactMode,
            boltMatched,
            false),
        "an exact target without mesh overlap must not be recovered");
    Require(
        rock::weapon_part_contact_acquisition_policy::mayRecoverExactTargetMeshContact(
            exactMode,
            boltMatched,
            true),
        "an overlapping exact target mesh must be recoverable");

    const auto normalMode = rock::weapon_part_contact_acquisition_policy::selectMode(false);
    Require(
        normalMode == AcquisitionMode::RankedPalmProbe,
        "normal interaction must select ranked palm probe mode");
    Require(
        rock::weapon_part_contact_acquisition_policy::mayUseRankedPalmProbe(normalMode),
        "normal interaction must allow the ranked palm probe");

    /*
     * A single hand collider can report barrel and bolt contacts in the same
     * physics frame. The multi-pair tracker must retain both instead of
     * reducing them to the callback's last body.
     */
    rock::contact_activity_tracker::ContactActivityTracker contactActivity{};
    constexpr std::uint32_t handBodyId = 50;
    const auto barrelRegistration =
        contactActivity.registerHandContact(
            true,
            handBodyId,
            barrelBodyId);
    const auto boltRegistration =
        contactActivity.registerHandContact(
            true,
            handBodyId,
            boltBodyId);
    Require(
        barrelRegistration.tracked,
        "the first left-hand weapon contact must be tracked");
    Require(
        boltRegistration.tracked,
        "a second same-frame left-hand weapon contact must also be tracked");
    Require(
        contactActivity.hasFreshHandContactWithTarget(
            true,
            barrelBodyId,
            0),
        "the same-frame barrel pair must remain queryable");
    Require(
        contactActivity.hasFreshHandContactWithTarget(
            true,
            boltBodyId,
            0),
        "the same-frame bolt pair must not be hidden by another contact");
    Require(
        !contactActivity.hasFreshHandContactWithTarget(
            false,
            boltBodyId,
            0),
        "contact evidence must remain isolated by hand");

    contactActivity.advanceFrame();
    Require(
        !contactActivity.hasFreshHandContactWithTarget(
            true,
            boltBodyId,
            0),
        "zero-age evidence must expire after the frame advances");
    Require(
        contactActivity.hasFreshHandContactWithTarget(
            true,
            boltBodyId,
            1),
        "one-frame exact-contact grace must retain the bolt pair");

    /*
     * Stock 10 mm evidence must not expose its release button as a second
     * Bolt. MatchPartKind::Bolt then identifies the moving bolt mesh only.
     */
    Require(
        rock::classifyWeaponPartName("Pistol10mmBolt:0").partKind ==
            rock::WeaponPartKind::Bolt,
        "the stock 10 mm moving bolt must classify as Bolt");
    Require(
        rock::classifyWeaponPartName("Pistol10mmBoltRelease:0").partKind !=
            rock::WeaponPartKind::Bolt,
        "the stock 10 mm bolt release must not classify as Bolt");
    Require(
        rock::classifyWeaponPartName("SlideRelease").partKind !=
            rock::WeaponPartKind::Slide,
        "a slide release must not classify as the moving Slide");

    return 0;
}
