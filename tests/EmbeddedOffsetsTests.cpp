#include "EmbeddedOffsets.h"

#include <cassert>
#include <cmath>
#include <string_view>

namespace
{
    bool nearlyEqual(float lhs, float rhs)
    {
        return std::fabs(lhs - rhs) < 0.0001f;
    }
}

int main()
{
    const EmbeddedOffsets::OffsetData* base = nullptr;
    const EmbeddedOffsets::OffsetData* left = nullptr;
    const EmbeddedOffsets::OffsetData* powerArmor = nullptr;
    std::size_t cryolatorEntries = 0;

    for (const auto& offset : EmbeddedOffsets::kOffsets) {
        if (offset.name != std::string_view("Cryolator")) {
            continue;
        }
        ++cryolatorEntries;
        assert(offset.isRightHandSpace);
        assert(offset.isFRIKOffset);
        assert(!offset.isThrowable);

        if (!offset.isLeftHanded && !offset.isPowerArmor) {
            assert(base == nullptr);
            base = &offset;
        } else if (offset.isLeftHanded && !offset.isPowerArmor) {
            assert(left == nullptr);
            left = &offset;
        } else if (!offset.isLeftHanded && offset.isPowerArmor) {
            assert(powerArmor == nullptr);
            powerArmor = &offset;
        } else {
            assert(false && "unexpected Cryolator variant identity");
        }
    }

    assert(cryolatorEntries == 3);
    assert(base != nullptr);
    assert(left != nullptr);
    assert(powerArmor != nullptr);
    assert(base->formId == std::string_view("00171B2B"));
    assert(nearlyEqual(base->posX, 6.60221052f));
    assert(nearlyEqual(left->posZ, 4.19195080f));
    assert(nearlyEqual(powerArmor->posX, 7.28200722f));
    return 0;
}
