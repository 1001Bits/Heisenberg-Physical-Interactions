#pragma once

/**
 * RockFingerPose.h - Faithful port of ROCK's grab_finger_pose_math solver.
 *
 * This is a verbatim copy of ROCK's pose math (ROCK-main/src/physics-interaction/
 * grab/GrabFinger.h, namespace rock::grab_finger_pose_math), placed here as a
 * self-contained, templated geometry library. It replaces Heisenberg's
 * curve-table solver (GetFingerIntersections) with ROCK's parametric curl-disk
 * solve:
 *   - solveFingerCurveCurlValue: intersect the object's triangle slice with the
 *     finger curl disk and convert hit angle into an open/closed curve value.
 *   - HitKind classification rejects back-face and behind-curl-plane hits so the
 *     finger doesn't wrap through the mesh.
 *   - solveThumbAwareFingerCurveCurlValue: a second thumb curve for cross-palm
 *     opposition grips.
 *   - solveFingerCurlValue: straight-ray + capsule-probe fallback.
 *
 * The driver lives in FingerCurves.cpp (CalculateFingerCurlFromGeometryRock),
 * which reuses Heisenberg's existing skeleton calibration, triangle extraction
 * and palmToPoint seating, then calls these solvers instead of the curve tables.
 * Value convention matches Heisenberg's FingerCurlResult: 1.0 = open, 0.0 = closed.
 */

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace heisenberg::rock_finger_math
{
    template <class Vector>
    struct Triangle
    {
        Vector v0{};
        Vector v1{};
        Vector v2{};
    };

    struct FingerCurlValue
    {
        float value = 0.0f;
        float rawCurveValue = 0.0f;
        bool hit = false;
        float distance = 0.0f;
        float hitPointX = 0.0f;
        float hitPointY = 0.0f;
        float hitPointZ = 0.0f;
        float hitNormalX = 0.0f;
        float hitNormalY = 0.0f;
        float hitNormalZ = 0.0f;
        bool hasHitPoint = false;
        bool hasHitNormal = false;
        bool openedByBehindContact = false;
        enum class HitKind : std::uint8_t
        {
            Miss,
            FrontValid,
            BehindCurlPlane,
            BackSurface,
            Rejected
        } hitKind = HitKind::Miss;
    };

    template <class Vector>
    struct ThumbAwareFingerCurveCurlValue
    {
        FingerCurlValue value{};
        FingerCurlValue primary{};
        FingerCurlValue alternateThumb{};
        bool usedAlternateThumbCurve = false;
    };

    inline bool shouldRunFallbackRayAfterCurveSolve(std::size_t fingerIndex, bool curveHit, bool usedAlternateThumbCurve)
    {
        if (curveHit) {
            return false;
        }
        return !(fingerIndex == 0 && usedAlternateThumbCurve);
    }

    template <class Vector>
    inline Vector sub(const Vector& a, const Vector& b)
    {
        return Vector{ a.x - b.x, a.y - b.y, a.z - b.z };
    }

    template <class Vector>
    inline Vector add(const Vector& a, const Vector& b)
    {
        return Vector{ a.x + b.x, a.y + b.y, a.z + b.z };
    }

    template <class Vector>
    inline Vector scale(const Vector& value, float scalar)
    {
        return Vector{ value.x * scalar, value.y * scalar, value.z * scalar };
    }

    template <class Vector>
    inline Vector cross(const Vector& a, const Vector& b)
    {
        return Vector{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    template <class Vector>
    inline float dot(const Vector& a, const Vector& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    template <class Vector>
    inline float length(const Vector& value)
    {
        return std::sqrt(dot(value, value));
    }

    template <class Vector>
    inline float lengthSquared(const Vector& value)
    {
        return dot(value, value);
    }

    template <class Vector>
    inline Vector normalize(const Vector& value)
    {
        const float len = length(value);
        if (len <= 0.000001f) {
            return Vector{ 1.0f, 0.0f, 0.0f };
        }
        const float inv = 1.0f / len;
        return Vector{ value.x * inv, value.y * inv, value.z * inv };
    }

    template <class Vector>
    inline bool hasUsableDirection(const Vector& value)
    {
        return lengthSquared(value) > 0.000001f;
    }

    template <class Vector>
    inline Vector triangleNormal(const Triangle<Vector>& triangle)
    {
        return normalize(cross(sub(triangle.v1, triangle.v0), sub(triangle.v2, triangle.v0)));
    }

    template <class Vector>
    inline std::vector<Triangle<Vector>> filterTrianglesNearPoint(const std::vector<Triangle<Vector>>& triangles, const Vector& point, float maxDistanceSquared)
    {
        std::vector<Triangle<Vector>> result;
        if (!std::isfinite(maxDistanceSquared) || maxDistanceSquared <= 0.0f) {
            return result;
        }

        result.reserve((std::min)(triangles.size(), static_cast<std::size_t>(256)));
        for (const auto& triangle : triangles) {
            const Vector centroid = scale(add(add(triangle.v0, triangle.v1), triangle.v2), 1.0f / 3.0f);
            const float bestVertexOrCentroidDistance = (std::min)({
                lengthSquared(sub(centroid, point)),
                lengthSquared(sub(triangle.v0, point)),
                lengthSquared(sub(triangle.v1, point)),
                lengthSquared(sub(triangle.v2, point)),
            });
            if (bestVertexOrCentroidDistance <= maxDistanceSquared) {
                result.push_back(triangle);
            }
        }
        return result;
    }

    template <class Vector>
    inline float clampedUnitDot(const Vector& a, const Vector& b)
    {
        return std::clamp(dot(normalize(a), normalize(b)), -1.0f, 1.0f);
    }

    template <class Vector>
    inline float signedAngleAroundNormal(const Vector& vector, const Vector& zeroAngleVector, const Vector& normal)
    {
        const Vector v = normalize(vector);
        const Vector zero = normalize(zeroAngleVector);
        float angle = std::acos(clampedUnitDot(v, zero));
        if (dot(normalize(normal), cross(zero, v)) < 0.0f) {
            angle *= -1.0f;
        }
        return angle;
    }

    template <class Vector>
    inline bool planeIntersectsSegment(const Vector& planePoint, const Vector& planeNormal, const Vector& a, const Vector& b, Vector& outPoint)
    {
        const Vector normal = normalize(planeNormal);
        const float da = dot(normal, sub(a, planePoint));
        const float db = dot(normal, sub(b, planePoint));
        if ((da > 0.0f && db > 0.0f) || (da < 0.0f && db < 0.0f)) {
            return false;
        }

        const float denom = da - db;
        if (std::abs(denom) <= 0.000001f) {
            outPoint = a;
            return true;
        }

        const float t = std::clamp(da / denom, 0.0f, 1.0f);
        outPoint = add(a, scale(sub(b, a), t));
        return true;
    }

    template <class Vector>
    inline bool rayTriangleIntersection(const Vector& origin, const Vector& direction, const Triangle<Vector>& triangle, float maxDistance, float& outT)
    {
        constexpr float kRayEpsilon = 0.000001f;
        const Vector edge1 = sub(triangle.v1, triangle.v0);
        const Vector edge2 = sub(triangle.v2, triangle.v0);
        const Vector pvec = cross(direction, edge2);
        const float det = dot(edge1, pvec);
        if (std::abs(det) < kRayEpsilon) {
            return false;
        }

        const float invDet = 1.0f / det;
        const Vector tvec = sub(origin, triangle.v0);
        const float u = dot(tvec, pvec) * invDet;
        if (u < 0.0f || u > 1.0f) {
            return false;
        }

        const Vector qvec = cross(tvec, edge1);
        const float v = dot(direction, qvec) * invDet;
        if (v < 0.0f || u + v > 1.0f) {
            return false;
        }

        const float t = dot(edge2, qvec) * invDet;
        if (t < 0.0f || t > maxDistance) {
            return false;
        }

        outT = t;
        return true;
    }

    template <class Vector>
    inline Vector closestPointOnTriangle(const Vector& point, const Triangle<Vector>& triangle)
    {
        const Vector ab = sub(triangle.v1, triangle.v0);
        const Vector ac = sub(triangle.v2, triangle.v0);
        const Vector ap = sub(point, triangle.v0);
        const float d1 = dot(ab, ap);
        const float d2 = dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) {
            return triangle.v0;
        }

        const Vector bp = sub(point, triangle.v1);
        const float d3 = dot(ab, bp);
        const float d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) {
            return triangle.v1;
        }

        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
            const float v = d1 / (d1 - d3);
            return add(triangle.v0, scale(ab, v));
        }

        const Vector cp = sub(point, triangle.v2);
        const float d5 = dot(ab, cp);
        const float d6 = dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) {
            return triangle.v2;
        }

        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
            const float w = d2 / (d2 - d6);
            return add(triangle.v0, scale(ac, w));
        }

        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
            const Vector bc = sub(triangle.v2, triangle.v1);
            const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return add(triangle.v1, scale(bc, w));
        }

        const float denom = 1.0f / (va + vb + vc);
        const float v = vb * denom;
        const float w = vc * denom;
        return add(triangle.v0, add(scale(ab, v), scale(ac, w)));
    }

    template <class Vector>
    inline float segmentSegmentDistanceSquared(
        const Vector& p1, const Vector& q1, const Vector& p2, const Vector& q2, float& outFirstSegmentT, float* outSecondSegmentT = nullptr)
    {
        constexpr float kSegmentEpsilon = 0.000001f;
        const Vector d1 = sub(q1, p1);
        const Vector d2 = sub(q2, p2);
        const Vector r = sub(p1, p2);
        const float a = dot(d1, d1);
        const float e = dot(d2, d2);
        const float f = dot(d2, r);

        float s = 0.0f;
        float t = 0.0f;
        if (a <= kSegmentEpsilon && e <= kSegmentEpsilon) {
            outFirstSegmentT = 0.0f;
            return lengthSquared(sub(p1, p2));
        }

        if (a <= kSegmentEpsilon) {
            s = 0.0f;
            t = std::clamp(f / e, 0.0f, 1.0f);
        } else {
            const float c = dot(d1, r);
            if (e <= kSegmentEpsilon) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else {
                const float b = dot(d1, d2);
                const float denom = a * e - b * b;
                if (denom != 0.0f) {
                    s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
                }
                t = (b * s + f) / e;
                if (t < 0.0f) {
                    t = 0.0f;
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                } else if (t > 1.0f) {
                    t = 1.0f;
                    s = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }

        outFirstSegmentT = s;
        if (outSecondSegmentT) {
            *outSecondSegmentT = t;
        }
        const Vector c1 = add(p1, scale(d1, s));
        const Vector c2 = add(p2, scale(d2, t));
        return lengthSquared(sub(c1, c2));
    }

    template <class Vector>
    inline bool probeCapsuleTriangleIntersection(
        const Vector& origin, const Vector& direction, const Triangle<Vector>& triangle, float maxDistance, float probeRadius, float& outT, Vector* outClosestPoint = nullptr)
    {
        if (probeRadius <= 0.0f) {
            return false;
        }

        const Vector segmentEnd = add(origin, scale(direction, maxDistance));
        const float radiusSquared = probeRadius * probeRadius;
        float bestDistanceSquared = (std::numeric_limits<float>::max)();
        float bestT = maxDistance;
        Vector bestClosestPoint{};
        bool bestClosestPointValid = false;

        auto consider = [&](float distanceSquared, float segmentRatio, const Vector& closestPoint) {
            if (distanceSquared < bestDistanceSquared) {
                bestDistanceSquared = distanceSquared;
                bestT = std::clamp(segmentRatio, 0.0f, 1.0f) * maxDistance;
                bestClosestPoint = closestPoint;
                bestClosestPointValid = true;
            }
        };

        {
            const Vector closest = closestPointOnTriangle(origin, triangle);
            consider(lengthSquared(sub(origin, closest)), 0.0f, closest);
        }
        {
            const Vector closest = closestPointOnTriangle(segmentEnd, triangle);
            consider(lengthSquared(sub(segmentEnd, closest)), 1.0f, closest);
        }

        auto considerEdge = [&](const Vector& a, const Vector& b) {
            float segmentRatio = 0.0f;
            float edgeRatio = 0.0f;
            const float distanceSquared = segmentSegmentDistanceSquared(origin, segmentEnd, a, b, segmentRatio, &edgeRatio);
            consider(distanceSquared, segmentRatio, add(a, scale(sub(b, a), edgeRatio)));
        };

        considerEdge(triangle.v0, triangle.v1);
        considerEdge(triangle.v1, triangle.v2);
        considerEdge(triangle.v2, triangle.v0);

        if (bestDistanceSquared <= radiusSquared) {
            outT = bestT;
            if (outClosestPoint && bestClosestPointValid) {
                *outClosestPoint = bestClosestPoint;
            }
            return true;
        }
        return false;
    }

    template <class Vector>
    inline FingerCurlValue solveFingerCurlValue(const std::vector<Triangle<Vector>>& triangles,
        const Vector& origin,
        const Vector& direction,
        float maxDistance,
        float minValue,
        float probeRadius,
        const Vector& curlNormal = Vector{ 0.0f, 0.0f, 1.0f },
        const Vector& zeroAngleVector = Vector{ 1.0f, 0.0f, 0.0f },
        float maxCurlAngleRadians = 3.14159265358979323846f,
        const Vector& surfacePoint = Vector{},
        const Vector& surfaceNormal = Vector{},
        bool rejectBacksideHits = false,
        float surfacePlaneToleranceGameUnits = 0.0f,
        Vector* outHitPoint = nullptr)
    {
        FingerCurlValue result{};
        result.value = std::clamp(minValue, 0.0f, 1.0f);

        if (triangles.empty() || maxDistance <= 0.0001f) {
            return result;
        }

        const Vector dir = normalize(direction);
        float bestT = maxDistance;
        Vector bestHitPoint{};
        Vector bestHitNormal{};
        bool bestHitPointValid = false;
        bool bestHitNormalValid = false;
        bool sawBehindCurlPlane = false;
        bool sawBackSurface = false;
        const Vector normalizedCurlNormal = normalize(curlNormal);
        const Vector normalizedZeroAngle = normalize(zeroAngleVector);
        const bool hasSurfaceGate = rejectBacksideHits && hasUsableDirection(surfaceNormal);
        const Vector normalizedSurfaceNormal = hasSurfaceGate ? normalize(surfaceNormal) : Vector{};
        const float planeTolerance = (std::max)(0.0f, std::isfinite(surfacePlaneToleranceGameUnits) ? surfacePlaneToleranceGameUnits : 0.0f);
        const float maxCurlAngle = std::isfinite(maxCurlAngleRadians) && maxCurlAngleRadians > 0.0f ? maxCurlAngleRadians : 3.14159265358979323846f;
        for (const auto& triangle : triangles) {
            float t = 0.0f;
            Vector capsuleHitPoint{};
            bool hitPointFromCapsule = false;
            if (!rayTriangleIntersection(origin, dir, triangle, maxDistance, t)) {
                if (!probeCapsuleTriangleIntersection(origin, dir, triangle, maxDistance, probeRadius, t, &capsuleHitPoint)) {
                    continue;
                }
                hitPointFromCapsule = true;
            }

            const Vector hitPoint = hitPointFromCapsule ? capsuleHitPoint : add(origin, scale(dir, t));
            const Vector hitNormal = triangleNormal(triangle);
            if (rejectBacksideHits) {
                const Vector toHit = sub(hitPoint, origin);
                if (hasUsableDirection(toHit)) {
                    const float angle = signedAngleAroundNormal(toHit, normalizedZeroAngle, normalizedCurlNormal);
                    if (angle < -0.0001f && std::abs(angle) <= maxCurlAngle + 0.0001f) {
                        sawBehindCurlPlane = true;
                        continue;
                    }
                }

                if (hasSurfaceGate) {
                    const float planeDistance = dot(normalizedSurfaceNormal, sub(hitPoint, surfacePoint));
                    const float normalDot = dot(hitNormal, normalizedSurfaceNormal);
                    if (planeDistance < -planeTolerance || normalDot < -0.25f) {
                        sawBackSurface = true;
                        continue;
                    }
                }
            }

            if (t < bestT) {
                bestT = t;
                result.hit = true;
                result.hitKind = FingerCurlValue::HitKind::FrontValid;
                bestHitPoint = hitPoint;
                bestHitNormal = hitNormal;
                bestHitPointValid = true;
                bestHitNormalValid = hasUsableDirection(hitNormal);
            }
        }

        if (result.hit) {
            result.distance = bestT;
            result.rawCurveValue = bestT / maxDistance;
            result.value = std::clamp(result.rawCurveValue, std::clamp(minValue, 0.0f, 1.0f), 1.0f);
            if (outHitPoint && bestHitPointValid) {
                *outHitPoint = bestHitPoint;
            }
            if (bestHitPointValid) {
                result.hitPointX = bestHitPoint.x;
                result.hitPointY = bestHitPoint.y;
                result.hitPointZ = bestHitPoint.z;
                result.hasHitPoint = true;
            }
            if (bestHitNormalValid) {
                result.hitNormalX = bestHitNormal.x;
                result.hitNormalY = bestHitNormal.y;
                result.hitNormalZ = bestHitNormal.z;
                result.hasHitNormal = true;
            }
        } else if (sawBehindCurlPlane) {
            result.hit = true;
            result.value = 1.0f;
            result.rawCurveValue = -1.0f;
            result.openedByBehindContact = true;
            result.hitKind = FingerCurlValue::HitKind::BehindCurlPlane;
        } else if (sawBackSurface) {
            result.hitKind = FingerCurlValue::HitKind::BackSurface;
        }

        return result;
    }

    template <class Vector>
    inline FingerCurlValue solveFingerCurveCurlValue(const std::vector<Triangle<Vector>>& triangles, const Vector& center, const Vector& normal,
        const Vector& zeroAngleVector, float maxCurlAngleRadians, float fingerLength, float minValue, const Vector& surfacePoint = Vector{},
        const Vector& surfaceNormal = Vector{}, bool rejectBacksideHits = false, float surfacePlaneToleranceGameUnits = 0.0f)
    {
        FingerCurlValue result{};
        result.value = std::clamp(minValue, 0.0f, 1.0f);

        if (triangles.empty() || !std::isfinite(maxCurlAngleRadians) || maxCurlAngleRadians <= 0.0001f ||
            !std::isfinite(fingerLength) || fingerLength <= 0.0001f) {
            return result;
        }

        const float clampedMin = std::clamp(minValue, 0.0f, 1.0f);
        const Vector planeNormal = normalize(normal);
        const Vector zero = normalize(zeroAngleVector);
        const float maxAngle = (std::max)(0.0001f, maxCurlAngleRadians);
        constexpr float kCurveThickness = 0.35f;

        float bestPositiveAngle = (std::numeric_limits<float>::max)();
        Vector bestPositivePoint{};
        Vector bestPositiveNormal{};
        bool bestPositivePointValid = false;
        bool bestPositiveNormalValid = false;
        bool foundBehindContact = false;
        bool sawBackSurface = false;
        const bool hasSurfaceGate = rejectBacksideHits && hasUsableDirection(surfaceNormal);
        const Vector normalizedSurfaceNormal = hasSurfaceGate ? normalize(surfaceNormal) : Vector{};
        const float planeTolerance = (std::max)(0.0f, std::isfinite(surfacePlaneToleranceGameUnits) ? surfacePlaneToleranceGameUnits : 0.0f);

        auto considerPoint = [&](const Vector& point, const Vector& candidateSurfaceNormal) {
            const Vector fromCenter = sub(point, center);
            const float radius = length(fromCenter);
            if (radius <= 0.0001f || radius > fingerLength + kCurveThickness) {
                return;
            }

            const float angle = signedAngleAroundNormal(fromCenter, zero, planeNormal);
            if (angle < 0.0f && std::abs(angle) <= maxAngle) {
                foundBehindContact = true;
                return;
            }
            if (hasSurfaceGate) {
                const float planeDistance = dot(normalizedSurfaceNormal, sub(point, surfacePoint));
                const float normalDot = dot(candidateSurfaceNormal, normalizedSurfaceNormal);
                if (planeDistance < -planeTolerance || normalDot < -0.25f) {
                    sawBackSurface = true;
                    return;
                }
            }
            if (angle >= 0.0f && angle <= maxAngle && angle < bestPositiveAngle) {
                bestPositiveAngle = angle;
                bestPositivePoint = point;
                bestPositiveNormal = candidateSurfaceNormal;
                bestPositivePointValid = true;
                bestPositiveNormalValid = hasUsableDirection(candidateSurfaceNormal);
            }
        };

        for (const auto& triangle : triangles) {
            std::array<Vector, 3> intersections{};
            std::size_t intersectionCount = 0;
            auto addIntersection = [&](const Vector& a, const Vector& b) {
                if (intersectionCount >= intersections.size()) {
                    return;
                }
                Vector intersection{};
                if (planeIntersectsSegment(center, planeNormal, a, b, intersection)) {
                    intersections[intersectionCount++] = intersection;
                }
            };

            addIntersection(triangle.v0, triangle.v1);
            addIntersection(triangle.v1, triangle.v2);
            addIntersection(triangle.v2, triangle.v0);

            if (intersectionCount == 0) {
                continue;
            }

            const Vector candidateSurfaceNormal = triangleNormal(triangle);
            for (std::size_t i = 0; i < intersectionCount; ++i) {
                considerPoint(intersections[i], candidateSurfaceNormal);
            }
            if (intersectionCount >= 2) {
                considerPoint(scale(add(intersections[0], intersections[1]), 0.5f), candidateSurfaceNormal);
            }
        }

        if (bestPositiveAngle != (std::numeric_limits<float>::max)()) {
            result.hit = true;
            result.distance = bestPositiveAngle;
            result.rawCurveValue = 1.0f - (bestPositiveAngle / maxAngle);
            result.value = std::clamp(result.rawCurveValue, clampedMin, 1.0f);
            result.hitKind = FingerCurlValue::HitKind::FrontValid;
            if (bestPositivePointValid) {
                result.hitPointX = bestPositivePoint.x;
                result.hitPointY = bestPositivePoint.y;
                result.hitPointZ = bestPositivePoint.z;
                result.hasHitPoint = true;
            }
            if (bestPositiveNormalValid) {
                result.hitNormalX = bestPositiveNormal.x;
                result.hitNormalY = bestPositiveNormal.y;
                result.hitNormalZ = bestPositiveNormal.z;
                result.hasHitNormal = true;
            }
            return result;
        }

        if (foundBehindContact) {
            result.hit = true;
            result.value = 1.0f;
            result.rawCurveValue = -1.0f;
            result.openedByBehindContact = true;
            result.hitKind = FingerCurlValue::HitKind::BehindCurlPlane;
            return result;
        }

        if (sawBackSurface) {
            result.hitKind = FingerCurlValue::HitKind::BackSurface;
        }
        return result;
    }

    template <class Vector>
    inline ThumbAwareFingerCurveCurlValue<Vector> solveThumbAwareFingerCurveCurlValue(const std::vector<Triangle<Vector>>& triangles,
        const Vector& center,
        const Vector& primaryNormal,
        const Vector& alternateThumbNormal,
        const Vector& zeroAngleVector,
        float maxCurlAngleRadians,
        float fingerLength,
        float minValue,
        bool allowAlternateThumbCurve,
        const Vector& surfacePoint = Vector{},
        const Vector& surfaceNormal = Vector{},
        bool rejectBacksideHits = false,
        float surfacePlaneToleranceGameUnits = 0.0f)
    {
        ThumbAwareFingerCurveCurlValue<Vector> result{};
        result.primary = solveFingerCurveCurlValue(triangles,
            center,
            primaryNormal,
            zeroAngleVector,
            maxCurlAngleRadians,
            fingerLength,
            minValue,
            surfacePoint,
            surfaceNormal,
            rejectBacksideHits,
            surfacePlaneToleranceGameUnits);
        result.value = result.primary;

        if (!allowAlternateThumbCurve) {
            return result;
        }

        result.alternateThumb = solveFingerCurveCurlValue(triangles,
            center,
            alternateThumbNormal,
            zeroAngleVector,
            maxCurlAngleRadians,
            fingerLength,
            minValue,
            surfacePoint,
            surfaceNormal,
            rejectBacksideHits,
            surfacePlaneToleranceGameUnits);
        constexpr float kClosedEpsilon = 0.0001f;
        const bool primaryClosedOrMissed = !result.primary.hit || result.primary.rawCurveValue <= kClosedEpsilon;
        const bool primaryNeedsAlternate = primaryClosedOrMissed || result.primary.openedByBehindContact;
        const bool alternatePositive =
            result.alternateThumb.hit && !result.alternateThumb.openedByBehindContact && result.alternateThumb.rawCurveValue > kClosedEpsilon;
        const bool alternateClosedOrMissed =
            !result.alternateThumb.hit || (!result.alternateThumb.openedByBehindContact && result.alternateThumb.rawCurveValue <= kClosedEpsilon);
        const bool bothCurvesClosedOrMissed = primaryClosedOrMissed && !result.primary.openedByBehindContact && alternateClosedOrMissed;

        if (primaryNeedsAlternate && (alternatePositive || bothCurvesClosedOrMissed)) {
            result.value = result.alternateThumb;
            result.usedAlternateThumbCurve = true;
        }

        return result;
    }

    inline std::array<float, 15> expandFingerCurlsToJointValues(const std::array<float, 5>& values)
    {
        std::array<float, 15> joints{};
        for (std::size_t finger = 0; finger < values.size(); ++finger) {
            const float value = std::clamp(values[finger], 0.0f, 1.0f);
            const float closed = 1.0f - value;
            const float proximalOpenBias = (finger == 0) ? 0.15f : 0.25f;
            const float distalCloseBias = (finger == 0) ? 0.10f : 0.15f;
            const std::size_t base = finger * 3;
            joints[base + 0] = std::clamp(value + closed * proximalOpenBias, 0.0f, 1.0f);
            joints[base + 1] = value;
            joints[base + 2] = std::clamp(value - closed * distalCloseBias, 0.0f, 1.0f);
        }
        return joints;
    }
}
