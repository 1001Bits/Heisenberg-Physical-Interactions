#include "physics-interaction/weapon/TwoHandedWeaponPolicy.h"

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

    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(const float lhs, const float rhs, const float epsilon = 0.001f)
    {
        return std::abs(lhs - rhs) <= epsilon;
    }

    Matrix3 Yaw90()
    {
        Matrix3 result{};
        result.entry[0][1] = 1.0f;
        result.entry[1][0] = -1.0f;
        result.entry[2][2] = 1.0f;
        return result;
    }
}

int main()
{
    using namespace rock::two_handed_weapon_policy;

    {
        Transform weapon{};
        weapon.rotate = Yaw90();
        weapon.translate = Vec3{ 14000.0f, -87000.0f, 250.0f };
        const Vec3 primaryLocal{ 2.0f, -3.0f, 1.0f };
        const Vec3 primaryTarget{ 14020.0f, -86970.0f, 260.0f };

        // Simulate any post-solve translation (the removed whole-gun reach clamp),
        // then prove the invariant restores the captured primary pivot exactly even
        // at large world coordinates.
        weapon.translate.x += 17.0f;
        weapon.translate.y -= 9.0f;
        const Transform anchored = reanchorAtPrimaryGrip(weapon, primaryLocal, primaryTarget);
        const Vec3 primaryWorld = rock::transform_math::localPointToWorld(anchored, primaryLocal);
        Require(Near(primaryWorld.x, primaryTarget.x), "primary grip x must remain exact");
        Require(Near(primaryWorld.y, primaryTarget.y), "primary grip y must remain exact");
        Require(Near(primaryWorld.z, primaryTarget.z), "primary grip z must remain exact");
    }

    {
        Require(Near(armLengthScale(false), 1.0f), "ordinary and primary hands must keep native reach");
        Require(Near(armLengthScale(true), 1.08f), "support grip should receive only the bounded reach allowance");
    }

    {
        Require(Near(meshBorderSafeFingerOpenValue(0.42f, true, false), 0.42f),
            "a valid first-surface curl should be preserved");
        Require(Near(meshBorderSafeFingerOpenValue(0.30f, false, false), 1.0f),
            "a mesh miss must not use the generic closed fallback");
        Require(Near(meshBorderSafeFingerOpenValue(0.55f, true, true), 1.0f),
            "a pad detected inside the mesh must be opened fully");
        Require(Near(meshBorderSafeFingerOpenValue(1.15f, false, false), 1.15f),
            "mesh safety must never close an already over-open thumb");
    }

    return 0;
}
