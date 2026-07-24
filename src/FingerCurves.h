#pragma once

/**
 * FingerCurves.h - HIGGS Skyrim-style geometry-based finger curl system for Fallout 4 VR
 * 
 * This implements the EXACT sophisticated finger curl calculation from HIGGS Skyrim:
 * 1. Full 201-sample pre-computed finger curve tables (tip/outer/inner × 6 fingers)
 * 2. Real triangle mesh extraction from grabbed objects using F4VR BSTriShape API
 * 3. Proper finger curve intersection with mesh triangles
 * 4. Per-finger independent calculation
 * 
 * Based on HIGGS Skyrim's finger_curves.cpp and math_utils.cpp
 */

#include <RE/Fallout.h>
#include <vector>
#include <array>
#include <tuple>
#include <cmath>

namespace heisenberg
{
    // =========================================================================
    // DATA STRUCTURES
    // =========================================================================

    /**
     * Raw triangle indices (3 × uint16)
     */
    struct Triangle
    {
        std::uint16_t vertexIndices[3];
    };

    /**
     * Triangle data with world-space vertices for mesh intersection tests
     */
    struct TriangleData
    {
        RE::NiPoint3 v0;
        RE::NiPoint3 v1;
        RE::NiPoint3 v2;
        
        TriangleData() : v0(), v1(), v2() {}
        TriangleData(const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c)
            : v0(a), v1(b), v2(c) {}
        
        // Construct from raw triangle indices + vertex buffer.
        // When fullPrec=false (the common case), positions are stored as 3
        // half-floats (6 bytes) at posOffset. When fullPrec=true the
        // VF_FULLPREC flag was set on vertexDesc and positions are 3 full
        // floats (12 bytes). See BSGraphics::Utility::UnpackVertexData in
        // F4VR 1.2.72 @ 0x141db2650 for the authoritative format.
        TriangleData(const Triangle& tri, std::uintptr_t vertices, std::uint32_t posOffset, std::uint8_t vertexSize, bool fullPrec);
        
        void ApplyTransform(const RE::NiTransform& transform);
    };

    /**
     * 2D point for finger curve calculations (polar coordinate projection)
     */
    struct Point2
    {
        float x, y;
        Point2() : x(0), y(0) {}
        Point2(float x_, float y_) : x(x_), y(y_) {}
        Point2 operator*(float s) const { return Point2(x * s, y * s); }
        Point2 operator+(const Point2& other) const { return Point2(x + other.x, y + other.y); }
        Point2 operator-(const Point2& other) const { return Point2(x - other.x, y - other.y); }
    };

    /**
     * Saved finger curve data point
     * Each finger has 201 samples from fully open (curveVal=1) to fully closed (curveVal=0)
     */
    struct SavedFingerData
    {
        float curveVal;     // 1.0 = fully open, 0.0 = fully closed
        float angle;        // Angle in radians at this position
        float fingerLength; // Length of finger curve at this position
    };

    /**
     * Intersection result
     */
    struct Intersection
    {
        float angle;            // Angle of fingertip at intersection
        std::int32_t triangleIndex; // Index in triangle array
    };

    /**
     * Result of finger curl calculation for all 5 fingers
     */
    struct FingerCurlResult
    {
        float thumb = 0.9f;
        float index = 0.9f;
        float middle = 0.9f;
        float ring = 0.9f;
        float pinky = 0.9f;
        bool useAlternateThumbCurve = false;
        bool success = false;
    };

    // =========================================================================
    // FINGER CURVE DATA (from HIGGS Skyrim)
    // These are calibrated for VRIK/FRIK hand models
    // 6 entries: 5 fingers + 1 alternate thumb curve
    // =========================================================================

    // Number of samples per finger curve
    constexpr int kNumFingerVals = 201;

    // Finger indices
    constexpr int FINGER_THUMB = 0;
    constexpr int FINGER_INDEX = 1;
    constexpr int FINGER_MIDDLE = 2;
    constexpr int FINGER_RING = 3;
    constexpr int FINGER_PINKY = 4;
    constexpr int FINGER_THUMB_ALT = 5;  // Alternate thumb curve for different grip angles

    // Finger calibration data (defined in FingerCurves.cpp)
    extern RE::NiPoint3 g_fingerZeroAngleVecs[6];   // Direction of fully open finger (in hand space)
    extern RE::NiPoint3 g_fingerNormals[6];          // Normal of finger curl plane (in hand space)
    extern RE::NiPoint3 g_fingerStartPositions[6];   // Start position of each finger (in hand space)
    
    // Pre-computed finger curve data (defined in FingerCurveData.cpp - auto-generated)
    // Include FingerCurveData.h for access to these arrays:
    // - g_fingerTipVals[6][201]: Tip curve data
    // - g_fingerOuterVals[6][201]: Outer edge curve data
    // - g_fingerInnerVals[6][201]: Inner edge curve data

    // =========================================================================
    // MATH UTILITIES
    // =========================================================================

    inline float VectorLengthSquared(const RE::NiPoint3& vec)
    {
        return vec.x * vec.x + vec.y * vec.y + vec.z * vec.z;
    }

    inline float VectorLength(const RE::NiPoint3& vec)
    {
        return std::sqrt(VectorLengthSquared(vec));
    }

    inline RE::NiPoint3 VectorNormalized(const RE::NiPoint3& vec)
    {
        float length = VectorLength(vec);
        if (length > 0.0001f) {
            return RE::NiPoint3(vec.x / length, vec.y / length, vec.z / length);
        }
        return RE::NiPoint3();
    }

    inline float DotProduct(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline RE::NiPoint3 CrossProduct(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return RE::NiPoint3(
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        );
    }

    inline float Point2LengthSquared(const Point2& p)
    {
        return p.x * p.x + p.y * p.y;
    }

    inline float Point2Length(const Point2& p)
    {
        return std::sqrt(Point2LengthSquared(p));
    }

    /**
     * Rotate a vector around an axis by an angle (radians)
     */
    RE::NiPoint3 RotateVectorByAxisAngle(const RE::NiPoint3& vec, const RE::NiPoint3& axis, float angle);

    /**
     * Find distance between two line segments
     */
    float LineSegmentLineSegmentDistance(const Point2& p1, const Point2& p2, const Point2& p3, const Point2& p4);

    /**
     * Check if two 2D line segments intersect
     */
    bool LineSegmentIntersectsLineSegment(const Point2& p1, const Point2& p2, const Point2& p3, const Point2& p4, Point2* outIntersection = nullptr);

    /**
     * Find the furthest point on a line segment from a given point
     */
    RE::NiPoint3 GetFurthestPointOnLineSegment(const RE::NiPoint3& segStart, const RE::NiPoint3& segEnd, const RE::NiPoint3& point);

    /**
     * Check if a circle intersects a triangle
     * Returns number of intersection points (0, 1, or 2)
     */
    int CircleIntersectsTriangle(
        const RE::NiPoint3& circleCenter,
        const RE::NiPoint3& circleNormal,
        float circleRadius,
        const TriangleData& triangle,
        RE::NiPoint3& outIntersectionPoint1,
        RE::NiPoint3& outIntersectionPoint2);

    // =========================================================================
    // PUBLIC API
    // =========================================================================

    /**
     * Calculate finger curl values for grabbing an object
     * 
     * @param objectNode The root node of the grabbed object (for mesh extraction)
     * @param handNode The VR hand node (for hand transform)
     * @param palmPos Palm position in world space
     * @param palmDirection Palm forward direction in world space
     * @param isLeft True if left hand
     * @param handSize Scale factor for hand (default 1.0)
     * @return FingerCurlResult with curl values 0.0-1.0 for each finger
     */
    void CalibrateFingerDataFromSkeleton(RE::NiAVObject* handNode, bool isLeft);
    void ResetFingerCalibration();

    /**
     * Get the calibrated palm anchor in hand-local space (COM-tree LArm_Hand
     * / RArm_Hand frame). Only valid after the hand is calibrated. Returns
     * false if calibration hasn't run yet.
     *
     * Derived from the average of the 5 finger knuckle positions (centroid
     * of the metacarpals) and the averaged zero-angle directions — i.e.,
     * the anatomical palm center and its forward axis. Placement code built
     * on these values lands grabbed objects exactly where the finger cast
     * meets the mesh, eliminating the wand↔bone frame mismatch.
     */
    bool GetCalibratedPalmLocal(bool isLeft, RE::NiPoint3& outPos, RE::NiPoint3& outDir);

    /**
     * Fetch the hand-local palmar direction (knuckle centroid → palm cup)
     * established at calibration. Used to shift the palm anchor off the
     * dorsal (back of hand) knuckle centroid into the palm cup so grabbed
     * objects rest on the fingers instead of hovering above the hand.
     */
    bool GetCalibratedPalmarLocal(bool isLeft, RE::NiPoint3& outDir);

    FingerCurlResult CalculateFingerCurlFromGeometry(
        RE::NiAVObject* objectNode,
        RE::NiAVObject* handNode,
        const RE::NiPoint3& palmPos,
        const RE::NiPoint3& palmDirection,
        bool isLeft,
        float handSize = 1.0f);

    /**
     * Extract triangles from an object's mesh hierarchy
     * 
     * @param root Root node to extract from
     * @param outTriangles Output vector to append triangles to
     */
    void GetTriangles(RE::NiAVObject* root, std::vector<TriangleData>& outTriangles,
                      std::size_t maxTriangles = SIZE_MAX);

    /**
     * Find the closest point on object geometry to a line (palm position + direction)
     * 
     * @param triangles Pre-extracted triangles
     * @param lineStart Start of line (palm position)
     * @param lineDir Direction of line (palm direction)
     * @param outPoint Closest point on geometry
     * @param outNormal Normal at closest point
     * @param outTriIndex Index of triangle containing closest point
     * @param outDist Distance to closest point
     * @return True if a point was found
     */
    bool GetClosestPointOnGeometryToLine(
        const std::vector<TriangleData>& triangles,
        const RE::NiPoint3& lineStart,
        const RE::NiPoint3& lineDir,
        RE::NiPoint3& outPoint,
        RE::NiPoint3& outNormal,
        int& outTriIndex,
        float& outDist);

    /**
     * Find closest point on a mesh surface to an arbitrary point.
     *
     * Unlike GetClosestPointOnGeometryToLine, this ignores ray direction and
     * just minimizes Euclidean distance from `point` to any triangle surface.
     * Use for distance diagnostics where "how far is the mesh from the palm?"
     * is the actual question.
     *
     * @return True if any triangle was valid; outDist is set to the distance.
     */
    bool GetClosestMeshPointToPoint(
        const std::vector<TriangleData>& triangles,
        const RE::NiPoint3& point,
        RE::NiPoint3& outPoint,
        float& outDist);

    /**
     * Lookup curve value from angle using pre-computed finger data
     * 
     * @param fingerVals Finger curve data array (e.g., g_fingerTipVals[fingerIndex])
     * @param desiredAngle Angle to lookup (radians)
     * @param outData Output data at this angle
     * @return Index in array, or -1 if not found
     */
    int LookupFingerByAngle(const SavedFingerData fingerVals[], float desiredAngle, SavedFingerData* outData);

    /**
     * Check if a plane intersects a line segment
     * 
     * @param planePoint Point on the plane
     * @param planeNormal Normal of the plane
     * @param lineStart Start of line segment
     * @param lineEnd End of line segment
     * @param outIntersection Intersection point (if found)
     * @return True if intersection exists
     */
    bool PlaneIntersectsLineSegment(
        const RE::NiPoint3& planePoint,
        const RE::NiPoint3& planeNormal,
        const RE::NiPoint3& lineStart,
        const RE::NiPoint3& lineEnd,
        RE::NiPoint3& outIntersection);

    /**
     * Find finger intersections with triangles
     * 
     * @param triangles Triangles to test against
     * @param fingerIndex Which finger (0-5)
     * @param handScale Hand size multiplier
     * @param fingerCenter Center of finger curl (finger start position + offset to grab point)
     * @param fingerNormal Normal of finger curl plane (worldspace)
     * @param zeroAngleVector Direction of fully open finger (worldspace)
     * @param outIntersection Best intersection result
     * @return True if any intersection found
     */
    bool GetFingerIntersections(
        const std::vector<TriangleData>& triangles,
        int fingerIndex,
        float handScale,
        const RE::NiPoint3& fingerCenter,
        const RE::NiPoint3& fingerNormal,
        const RE::NiPoint3& zeroAngleVector,
        Intersection& outIntersection);

    /**
     * Check if finger intersects a specific triangle
     * Returns which curves (tip/outer/inner) intersected
     */
    std::tuple<bool, bool, bool> FingerIntersectsTriangle(
        int fingerIndex,
        const RE::NiPoint3& fingerCenter,
        const RE::NiPoint3& fingerNormal,
        const RE::NiPoint3& zeroAngleVector,
        float handScale,
        float nodeScale,
        const TriangleData& triangle,
        float& outTipAngle,
        float& outOuterAngle,
        float& outInnerAngle);

} // namespace heisenberg
