#include "api/ROCKProviderApi.h"
#include "api/WeaponPartDriveLeasePolicy.h"

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

    return 0;
}
