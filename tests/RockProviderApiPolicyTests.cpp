#include "api/ROCKProviderApi.h"
#include "api/WeaponPartDriveLeasePolicy.h"
#include "physics-interaction/collision/ContactActivityTracker.h"
#include "physics-interaction/weapon/WeaponPartContactAcquisitionPolicy.h"
#include "physics-interaction/weapon/WeaponSemantics.h"

#include <cassert>
#include <cstdint>
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
        assert(converted.rotate[index] == static_cast<float>(index + 1));
    }
    assert(converted.translate[0] == 10.0f);
    assert(converted.translate[1] == 11.0f);
    assert(converted.translate[2] == 12.0f);
    assert(converted.scale == 1.25f);

    RockProviderTransform copied{};
    copied = converted;
    assert(copied.rotate[8] == 9.0f);
    assert(copied.translate[1] == 11.0f);
    assert(copied.scale == 1.25f);

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
    assert(exactMode == AcquisitionMode::ExactPhysicalContact);
    assert(!rock::weapon_part_contact_acquisition_policy::mayUseRankedPalmProbe(exactMode));
    assert(!rock::weapon_part_contact_acquisition_policy::mayAcceptExactPhysicalContact(exactMode, barrelMatched));
    assert(rock::weapon_part_contact_acquisition_policy::mayAcceptExactPhysicalContact(exactMode, boltMatched));
    assert(!rock::weapon_part_contact_acquisition_policy::mayRecoverExactTargetMeshContact(
        exactMode,
        barrelMatched,
        true));
    assert(!rock::weapon_part_contact_acquisition_policy::mayRecoverExactTargetMeshContact(
        exactMode,
        boltMatched,
        false));
    assert(rock::weapon_part_contact_acquisition_policy::mayRecoverExactTargetMeshContact(
        exactMode,
        boltMatched,
        true));

    const auto normalMode = rock::weapon_part_contact_acquisition_policy::selectMode(false);
    assert(normalMode == AcquisitionMode::RankedPalmProbe);
    assert(rock::weapon_part_contact_acquisition_policy::mayUseRankedPalmProbe(normalMode));

    /*
     * Stock 10 mm evidence must not expose its release button as a second
     * Bolt. MatchPartKind::Bolt then identifies the moving bolt mesh only.
     */
    assert(rock::classifyWeaponPartName("Pistol10mmBolt:0").partKind == rock::WeaponPartKind::Bolt);
    assert(rock::classifyWeaponPartName("Pistol10mmBoltRelease:0").partKind != rock::WeaponPartKind::Bolt);
    assert(rock::classifyWeaponPartName("SlideRelease").partKind != rock::WeaponPartKind::Slide);

    /*
     * A single hand collider can report barrel and bolt contacts in the same
     * physics frame. The multi-pair tracker must retain both instead of
     * reducing them to the callback's last body.
     */
    rock::contact_activity_tracker::ContactActivityTracker contactActivity{};
    constexpr std::uint32_t handBodyId = 50;
    assert(contactActivity.registerHandContact(true, handBodyId, barrelBodyId).tracked);
    assert(contactActivity.registerHandContact(true, handBodyId, boltBodyId).tracked);
    assert(contactActivity.hasFreshHandContactWithTarget(true, barrelBodyId, 0));
    assert(contactActivity.hasFreshHandContactWithTarget(true, boltBodyId, 0));
    assert(!contactActivity.hasFreshHandContactWithTarget(false, boltBodyId, 0));

    contactActivity.advanceFrame();
    assert(!contactActivity.hasFreshHandContactWithTarget(true, boltBodyId, 0));
    assert(contactActivity.hasFreshHandContactWithTarget(true, boltBodyId, 1));

    return 0;
}
