#pragma once

#include "RE/NetImmerse/NiPoint.h"
#include "RE/NetImmerse/NiTransform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

/*
 * Pure math: projects a grabbing hand's motion onto a consumer-configured
 * axis (RockProviderWeaponPartMotionConstraintV1, see api/ROCKProviderApi.h)
 * instead of letting a whitelisted part follow the hand freely in 3D.
 *
 * No NiAVObject/scene-graph/Havok knowledge lives here - every position and
 * vector is in whatever single local space the caller has already resolved
 * everything into (weapon-root-local or source-parent-local, matching
 * RockProviderWeaponPartDriveSpaceV1). Operators on RE::NiPoint3/NiMatrix3
 * are not assumed to exist (TwoHandedGrip.cpp's own weaponSolverDot/Cross/
 * Normalize helpers are file-local for the same reason) - every vector op
 * here is written out explicitly against .x/.y/.z and .entry[row][col].
 */
namespace rock::weapon_part_motion_constraint_policy
{
    enum class ConstraintKind : std::uint32_t
    {
        None = 0,
        Linear = 1,
        Rotational = 2,
    };

    struct Axis
    {
        RE::NiPoint3 origin{ 0.0f, 0.0f, 0.0f };
        // Must already be unit length - the provider API rejects near-zero
        // vectors at setWeaponPartMotionConstraintsV1 time (see
        // isFiniteWeaponPartMotionConstraintAxes in ROCKProviderApi.cpp) but
        // does not itself normalize, so a caller building an Axis from raw
        // consumer data should normalize once before repeated per-frame use.
        RE::NiPoint3 direction{ 0.0f, 0.0f, 1.0f };
        // Linear: game units of travel from origin. Rotational: degrees.
        float minValue{ 0.0f };
        float maxValue{ 0.0f };
    };

    /*
     * Captured once, at the moment a hand grabs a constrained part: the
     * part's own local transform at that instant, and the hand's local-space
     * position at that instant. Every subsequent frame re-projects the
     * hand's CURRENT position against this fixed reference, so the result
     * only ever depends on total hand displacement since grab - never on
     * frame-to-frame deltas, so a single dropped or late frame can't cause
     * drift or a sudden jump.
     */
    struct GrabReference
    {
        RE::NiTransform partLocalAtGrab{};
        RE::NiPoint3 handPositionAtGrab{ 0.0f, 0.0f, 0.0f };
    };

    [[nodiscard]] inline RE::NiPoint3 addPoints(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return RE::NiPoint3{ a.x + b.x, a.y + b.y, a.z + b.z };
    }

    [[nodiscard]] inline RE::NiPoint3 subPoints(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return RE::NiPoint3{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    [[nodiscard]] inline RE::NiPoint3 scalePoint(const RE::NiPoint3& a, float scale)
    {
        return RE::NiPoint3{ a.x * scale, a.y * scale, a.z * scale };
    }

    [[nodiscard]] inline float dot(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    [[nodiscard]] inline RE::NiPoint3 cross(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return RE::NiPoint3{
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    }

    // Rodrigues' rotation formula: rotates `point` by `angleRadians` around
    // the unit axis `axisUnit` (both in the same local space).
    [[nodiscard]] inline RE::NiPoint3 rotateAroundAxis(const RE::NiPoint3& point, const RE::NiPoint3& axisUnit, float angleRadians)
    {
        const float cosA = std::cos(angleRadians);
        const float sinA = std::sin(angleRadians);
        const RE::NiPoint3 axisCrossPoint = cross(axisUnit, point);
        const float axisDotPoint = dot(axisUnit, point);

        return RE::NiPoint3{
            point.x * cosA + axisCrossPoint.x * sinA + axisUnit.x * axisDotPoint * (1.0f - cosA),
            point.y * cosA + axisCrossPoint.y * sinA + axisUnit.y * axisDotPoint * (1.0f - cosA),
            point.z * cosA + axisCrossPoint.z * sinA + axisUnit.z * axisDotPoint * (1.0f - cosA),
        };
    }

    // Builds the F4VR/NiMatrix3 ROW-VECTOR rotation matrix for
    // `angleRadians` around unit axis `axisUnit`. Each rotated basis vector is
    // therefore a ROW, not a column. Keeping that convention explicit matters:
    // transposing these assignments reverses the visible sweep even though all
    // values remain finite and orthonormal.
    [[nodiscard]] inline RE::NiMatrix3 rotationMatrixAroundAxis(const RE::NiPoint3& axisUnit, float angleRadians)
    {
        const RE::NiPoint3 rotatedX = rotateAroundAxis(RE::NiPoint3{ 1.0f, 0.0f, 0.0f }, axisUnit, angleRadians);
        const RE::NiPoint3 rotatedY = rotateAroundAxis(RE::NiPoint3{ 0.0f, 1.0f, 0.0f }, axisUnit, angleRadians);
        const RE::NiPoint3 rotatedZ = rotateAroundAxis(RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, axisUnit, angleRadians);

        RE::NiMatrix3 result{};
        result.entry[0][0] = rotatedX.x;
        result.entry[0][1] = rotatedX.y;
        result.entry[0][2] = rotatedX.z;
        result.entry[1][0] = rotatedY.x;
        result.entry[1][1] = rotatedY.y;
        result.entry[1][2] = rotatedY.z;
        result.entry[2][0] = rotatedZ.x;
        result.entry[2][1] = rotatedZ.y;
        result.entry[2][2] = rotatedZ.z;
        return result;
    }

    [[nodiscard]] inline RE::NiMatrix3 multiplyMatrices(const RE::NiMatrix3& lhs, const RE::NiMatrix3& rhs)
    {
        RE::NiMatrix3 result{};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    sum += lhs.entry[row][k] * rhs.entry[k][col];
                }
                result.entry[row][col] = sum;
            }
        }
        return result;
    }

    /*
     * Projects the hand's current position (same local space as `axis` and
     * `reference`) onto the constraint and returns the part's resulting
     * local transform.
     *
     * Linear: signed distance of (handCurrent - handAtGrab) onto
     * axis.direction, clamped to [minValue, maxValue] game units, applied as
     * a translate offset from partLocalAtGrab along axis.direction. The
     * part's rotation never changes - a slide/detach path is a pure
     * translation.
     *
     * Rotational: signed angle swept by the hand's position around
     * axis.direction through axis.origin, measured from the hand's
     * at-grab position, clamped to [minValue, maxValue] degrees, applied as
     * a rotation composed onto partLocalAtGrab's own rotation. The part's
     * translation is rotated through the same arc around axis.origin, so a
     * part whose origin isn't exactly at the pivot still swings through a
     * real arc instead of spinning in place.
     */
    [[nodiscard]] inline RE::NiTransform projectHandOntoConstraint(
        ConstraintKind kind,
        const Axis& axis,
        const GrabReference& reference,
        const RE::NiPoint3& handPositionCurrent)
    {
        RE::NiTransform result = reference.partLocalAtGrab;

        if (kind == ConstraintKind::Linear) {
            const RE::NiPoint3 handDelta = subPoints(handPositionCurrent, reference.handPositionAtGrab);
            const float travel = std::clamp(dot(handDelta, axis.direction), axis.minValue, axis.maxValue);
            result.translate = addPoints(reference.partLocalAtGrab.translate, scalePoint(axis.direction, travel));
            return result;
        }

        if (kind == ConstraintKind::Rotational) {
            // Project both the at-grab and current hand offsets from axis.origin
            // into the plane perpendicular to axis.direction, so radial hand
            // jitter toward/away from the pivot never registers as rotation -
            // only the sweep around it does.
            const auto projectToPlane = [&](const RE::NiPoint3& point) {
                const RE::NiPoint3 fromOrigin = subPoints(point, axis.origin);
                const float alongAxis = dot(fromOrigin, axis.direction);
                return subPoints(fromOrigin, scalePoint(axis.direction, alongAxis));
            };
            const RE::NiPoint3 atGrabPlanar = projectToPlane(reference.handPositionAtGrab);
            const RE::NiPoint3 currentPlanar = projectToPlane(handPositionCurrent);

            const float atGrabLenSq = dot(atGrabPlanar, atGrabPlanar);
            const float currentLenSq = dot(currentPlanar, currentPlanar);
            // Hand too close to the pivot axis for a stable angle - hold at the
            // last valid transform rather than let a near-zero-length
            // projection produce a noisy or undefined angle.
            if (atGrabLenSq < 0.0001f || currentLenSq < 0.0001f) {
                return result;
            }

            const RE::NiPoint3 atGrabUnit = scalePoint(atGrabPlanar, 1.0f / std::sqrt(atGrabLenSq));
            const RE::NiPoint3 currentUnit = scalePoint(currentPlanar, 1.0f / std::sqrt(currentLenSq));
            const float cosAngle = std::clamp(dot(atGrabUnit, currentUnit), -1.0f, 1.0f);
            float angleRadians = std::acos(cosAngle);

            // Sign via the cross product's alignment with axis.direction
            // (right-hand rule around the constraint axis).
            const RE::NiPoint3 sweepCross = cross(atGrabUnit, currentUnit);
            if (dot(sweepCross, axis.direction) < 0.0f) {
                angleRadians = -angleRadians;
            }

            constexpr float kRadiansToDegrees = 180.0f / 3.14159265358979323846f;
            constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
            const float angleDegrees = std::clamp(angleRadians * kRadiansToDegrees, axis.minValue, axis.maxValue);
            const float clampedRadians = angleDegrees * kDegreesToRadians;

            const RE::NiMatrix3 sweep = rotationMatrixAroundAxis(axis.direction, clampedRadians);
            // F4VR composes row-vector transforms as world = local * parent.
            // The authored part-local orientation is applied first and the
            // constraint-space sweep second.
            result.rotate = multiplyMatrices(reference.partLocalAtGrab.rotate, sweep);

            const RE::NiPoint3 offsetFromPivot = subPoints(reference.partLocalAtGrab.translate, axis.origin);
            result.translate = addPoints(axis.origin, rotateAroundAxis(offsetFromPivot, axis.direction, clampedRadians));
            return result;
        }

        return result;
    }
}
