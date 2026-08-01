#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <iostream>

#include "physics-interaction/weapon/WeaponPartMotionConstraintPolicy.h"

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    bool Near(float lhs, float rhs, float epsilon = 0.0001f)
    {
        return std::abs(lhs - rhs) <= epsilon;
    }
}

int main()
{
    using namespace rock::weapon_part_motion_constraint_policy;

    constexpr float reloadSlideY = 4.75f;
    constexpr float maxBoltY = 2.9463147f;
    constexpr float totalTravel = reloadSlideY - maxBoltY;

    Axis axis{};
    axis.direction = RE::NiPoint3{ 0.0f, -1.0f, 0.0f };
    axis.minValue = 0.0f;
    axis.maxValue = totalTravel;

    GrabReference reference{};
    reference.partLocalAtGrab.translate = RE::NiPoint3{ 0.0f, reloadSlideY, 0.0f };
    reference.handPositionAtGrab = RE::NiPoint3{ 10.0f, 20.0f, 30.0f };

    // Capturing an AttachOnly grip without moving the tracked controller must
    // leave the bolt exactly at its dry-fire position.
    const RE::NiTransform atCapture = projectHandOntoConstraint(
        ConstraintKind::Linear,
        axis,
        reference,
        reference.handPositionAtGrab);
    Require(Near(atCapture.translate.y, reloadSlideY),
        "grip capture must produce zero bolt travel");

    // Pulling the clean tracked hand along parent-local -Y moves the bolt by
    // the same signed rail distance.
    const RE::NiTransform partlyPulled = projectHandOntoConstraint(
        ConstraintKind::Linear,
        axis,
        reference,
        RE::NiPoint3{ 10.0f, 19.75f, 30.0f });
    Require(Near(partlyPulled.translate.y, reloadSlideY - 0.25f),
        "tracked pull must move the bolt backwards");

    // Moving the controller forward again is evaluated from the immutable
    // at-grab reference, so the bolt reverses instead of remaining at max.
    const RE::NiTransform pushedForward = projectHandOntoConstraint(
        ConstraintKind::Linear,
        axis,
        reference,
        RE::NiPoint3{ 10.0f, 19.90f, 30.0f });
    Require(Near(pushedForward.translate.y, reloadSlideY - 0.10f),
        "tracked forward motion must return the bolt toward dry fire");

    const RE::NiTransform fullyReturned = projectHandOntoConstraint(
        ConstraintKind::Linear,
        axis,
        reference,
        reference.handPositionAtGrab);
    Require(Near(fullyReturned.translate.y, reloadSlideY),
        "returning the tracked hand to capture must return the bolt to zero");

    const RE::NiTransform beyondMaximum = projectHandOntoConstraint(
        ConstraintKind::Linear,
        axis,
        reference,
        RE::NiPoint3{ 10.0f, 10.0f, 30.0f });
    Require(Near(beyondMaximum.translate.y, maxBoltY),
        "bolt travel must clamp at the authored maximum position");

    // F4VR/NiTransform uses row vectors (world = local * parent). Sweeping the
    // hand +90 degrees from +X to +Y around +Z must therefore rotate the
    // part's pivot offset to +Y and post-compose the sweep onto its authored
    // local rotation. A non-identity authored rotation makes the multiplication
    // order observable instead of allowing either order to pass.
    Axis rotationalAxis{};
    rotationalAxis.direction = RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
    rotationalAxis.minValue = -180.0f;
    rotationalAxis.maxValue = 180.0f;

    GrabReference rotationalReference{};
    rotationalReference.handPositionAtGrab = RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
    rotationalReference.partLocalAtGrab.translate = RE::NiPoint3{ 2.0f, 0.0f, 0.0f };
    // Row-vector +90 degrees around +X.
    rotationalReference.partLocalAtGrab.rotate.entry[0][0] = 1.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[0][1] = 0.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[0][2] = 0.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[1][0] = 0.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[1][1] = 0.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[1][2] = 1.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[2][0] = 0.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[2][1] = -1.0f;
    rotationalReference.partLocalAtGrab.rotate.entry[2][2] = 0.0f;

    const RE::NiTransform sweptPositiveNinety = projectHandOntoConstraint(
        ConstraintKind::Rotational,
        rotationalAxis,
        rotationalReference,
        RE::NiPoint3{ 0.0f, 1.0f, 0.0f });
    Require(Near(sweptPositiveNinety.translate.x, 0.0f) &&
            Near(sweptPositiveNinety.translate.y, 2.0f) &&
            Near(sweptPositiveNinety.translate.z, 0.0f),
        "+90 hand sweep must move the part through the +Z pivot arc from +X to +Y");
    Require(Near(sweptPositiveNinety.rotate.entry[0][0], 0.0f) &&
            Near(sweptPositiveNinety.rotate.entry[0][1], 1.0f) &&
            Near(sweptPositiveNinety.rotate.entry[0][2], 0.0f) &&
            Near(sweptPositiveNinety.rotate.entry[1][0], 0.0f) &&
            Near(sweptPositiveNinety.rotate.entry[1][1], 0.0f) &&
            Near(sweptPositiveNinety.rotate.entry[1][2], 1.0f) &&
            Near(sweptPositiveNinety.rotate.entry[2][0], 1.0f) &&
            Near(sweptPositiveNinety.rotate.entry[2][1], 0.0f) &&
            Near(sweptPositiveNinety.rotate.entry[2][2], 0.0f),
        "+90 hand sweep must use row-vector sign and authoredLocal*sweep composition");

    return 0;
}
