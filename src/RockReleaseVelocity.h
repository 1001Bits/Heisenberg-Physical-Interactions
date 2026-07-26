#pragma once

/*
 * FAITHFULNESS (2026-07-05 audit rank 3): ROCK's release/throw velocity composition,
 * mirrored VERBATIM from the embedded engine's pure-math header
 * external/ROCK/src/physics-interaction/grab/GrabHeldObject.h (namespace
 * rock::grab_held_response), specialized for RE::NiPoint3. Mirrored rather than
 * included because the main target deliberately keeps ROCK's include tree out of
 * Heisenberg TUs (GrabHeldObject.h pulls "physics-interaction/TransformMath.h").
 * If the vendored engine header changes, re-mirror.
 *
 * Composition (standalone ROCK, HandGrab.cpp release path):
 *   local  = handLocal + tangentialScale*tangential + clamp01(objectBlend)*objectLocal
 *   final  = clampMag( playerVelocity + throwMultiplier * clampMag(local, maxV), maxV )
 *   angular = clampMag( handAngular * angularScale, maxAngular )
 * All linear quantities in HAVOK meters/second; angular in radians/second.
 * Live tuning ([PhysicsInteraction] in Heisenberg_F4VR.ini == RockConfig defaults): multiplier 1.5, object
 * blend 0.35, tangential scale 1.0, maxVelocity 12.0 Havok m/s, angular scale 1.0,
 * maxAngular 18.0 rad/s, controller-derived enabled.
 */

// RE types come from the project PCH (like sibling headers — see Hand.h).
#include <cmath>

namespace heisenberg::rock_release_velocity
{
    inline float lengthOf(const RE::NiPoint3& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    inline float lengthSquaredOf(const RE::NiPoint3& v)
    {
        return v.x * v.x + v.y * v.y + v.z * v.z;
    }

    inline RE::NiPoint3 clampMagnitude(const RE::NiPoint3& value, float maxMagnitude)
    {
        if (!std::isfinite(maxMagnitude) || maxMagnitude <= 0.0f) {
            return value;
        }
        const float magnitude = lengthOf(value);
        if (!std::isfinite(magnitude) || magnitude <= maxMagnitude || magnitude <= 0.000001f) {
            return value;
        }
        return value * (maxMagnitude / magnitude);
    }

    inline float finiteOr(float value, float fallback)
    {
        return std::isfinite(value) ? value : fallback;
    }

    inline float clamp01(float value)
    {
        if (!std::isfinite(value)) return 0.0f;
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    inline float safePositive(float value, float fallback)
    {
        return (std::isfinite(value) && value > 0.0f) ? value : fallback;
    }

    struct ReleaseVelocityInput
    {
        bool controllerDerivedEnabled = true;
        bool hasHandLocalVelocity = false;
        bool hasObjectLocalVelocity = false;
        bool hasTangentialVelocity = false;
        RE::NiPoint3 handLocalVelocityHavok{};
        RE::NiPoint3 objectLocalVelocityHavok{};
        RE::NiPoint3 playerVelocityHavok{};
        RE::NiPoint3 tangentialVelocityHavok{};
        float objectVelocityBlend = 0.35f;
        float tangentialVelocityScale = 1.0f;
        float throwMultiplier = 1.5f;
        float maxVelocityHavok = 12.0f;
    };

    struct ReleaseAngularVelocityInput
    {
        bool controllerDerivedEnabled = true;
        bool hasHandAngularVelocity = false;
        RE::NiPoint3 handAngularVelocityRadiansPerSecond{};
        float angularVelocityScale = 1.0f;
        float maxAngularVelocityRadiansPerSecond = 18.0f;
    };

    inline RE::NiPoint3 composeControllerReleaseVelocity(const ReleaseVelocityInput& input)
    {
        RE::NiPoint3 localVelocity{};
        if (input.controllerDerivedEnabled && input.hasHandLocalVelocity) {
            localVelocity = input.handLocalVelocityHavok;
            if (input.hasTangentialVelocity) {
                localVelocity = localVelocity + input.tangentialVelocityHavok * finiteOr(input.tangentialVelocityScale, 1.0f);
            }
            if (input.hasObjectLocalVelocity) {
                localVelocity = localVelocity + input.objectLocalVelocityHavok * clamp01(input.objectVelocityBlend);
            }
        } else if (input.hasObjectLocalVelocity) {
            localVelocity = input.objectLocalVelocityHavok;
        }

        localVelocity = clampMagnitude(localVelocity, input.maxVelocityHavok);
        const float multiplier = safePositive(input.throwMultiplier, 1.0f);
        return clampMagnitude(input.playerVelocityHavok + localVelocity * multiplier, input.maxVelocityHavok);
    }

    inline RE::NiPoint3 composeControllerReleaseAngularVelocity(const ReleaseAngularVelocityInput& input)
    {
        if (!input.controllerDerivedEnabled || !input.hasHandAngularVelocity) {
            return RE::NiPoint3{};
        }
        const float scaleFactor = (std::max)(0.0f, finiteOr(input.angularVelocityScale, 1.0f));          // (std::max) — dodge windows.h max macro
        const float maxAngularVelocity = (std::max)(0.0f, finiteOr(input.maxAngularVelocityRadiansPerSecond, 0.0f));
        if (scaleFactor <= 0.0f || maxAngularVelocity <= 0.0f) {
            return RE::NiPoint3{};
        }
        return clampMagnitude(input.handAngularVelocityRadiansPerSecond * scaleFactor, maxAngularVelocity);
    }

    inline RE::NiPoint3 normalizeOrZero(const RE::NiPoint3& v)
    {
        const float len = lengthOf(v);
        if (!std::isfinite(len) || len <= 0.000001f) {
            return RE::NiPoint3{};
        }
        return v * (1.0f / len);
    }

    inline RE::NiPoint3 crossOf(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return RE::NiPoint3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    inline float dotOf(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    // Lever/swing term: the linear velocity the hand's rotation imparts at the object's
    // center of mass (throwing by swinging, not just translating the hand).
    inline RE::NiPoint3 computeTangentialVelocityFromAngularSwing(
        const RE::NiPoint3& angularVelocityRadiansPerSecond,
        const RE::NiPoint3& handPositionHavok,
        const RE::NiPoint3& centerOfMassHavok)
    {
        const float angularSpeed = lengthOf(angularVelocityRadiansPerSecond);
        if (!std::isfinite(angularSpeed) || angularSpeed <= 0.000001f) {
            return RE::NiPoint3{};
        }
        const RE::NiPoint3 axis = normalizeOrZero(angularVelocityRadiansPerSecond);
        const RE::NiPoint3 handToCenter = centerOfMassHavok - handPositionHavok;
        const RE::NiPoint3 radial = handToCenter - axis * dotOf(handToCenter, axis);
        if (lengthSquaredOf(radial) <= 0.000001f) {
            return RE::NiPoint3{};
        }
        return crossOf(axis, radial) * angularSpeed;
    }

    // ROCK samples release velocity from the recent history ring via held_object_physics_math::
    // maxMagnitudeVelocity (GrabHeldObject.h:490) — ported VERBATIM: find the peak-magnitude
    // entry among the first `validCount` samples; if it's an INTERIOR sample (not the first or
    // last of the valid window and validCount>=3), return the 3-sample average centered on the
    // peak (smooths a single-frame tracking spike), else the raw peak. `history[i]` for
    // i in [0,validCount) must be the real samples (our deques push_front, so indices
    // 0..validCount-1 are the newest real frames; the pre-seeded zeros sit past validCount).
    template <class Container>
    inline RE::NiPoint3 maxMagnitudeVelocity(const Container& history, std::size_t validCount)
    {
        const std::size_t n = history.size();
        if (validCount > n) validCount = n;
        if (validCount == 0) return RE::NiPoint3{};

        std::size_t largest = 0;
        float largestMag = lengthSquaredOf(history[0]);
        for (std::size_t i = 1; i < validCount; ++i) {
            const float m = lengthSquaredOf(history[i]);
            if (m > largestMag) { largestMag = m; largest = i; }
        }
        if (validCount < 3 || largest == 0 || largest == validCount - 1) {
            return history[largest];
        }
        return RE::NiPoint3{
            (history[largest - 1].x + history[largest].x + history[largest + 1].x) / 3.0f,
            (history[largest - 1].y + history[largest].y + history[largest + 1].y) / 3.0f,
            (history[largest - 1].z + history[largest].z + history[largest + 1].z) / 3.0f };
    }
}
