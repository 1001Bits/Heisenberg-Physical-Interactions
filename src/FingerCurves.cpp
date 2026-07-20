/**
 * FingerCurves.cpp - HIGGS Skyrim-style geometry-based finger curl system for Fallout 4 VR
 * 
 * This implements the EXACT finger curl calculation from HIGGS Skyrim:
 * 1. Full 201-sample pre-computed finger curve tables (from FingerCurveData.cpp)
 * 2. Real triangle mesh extraction from grabbed objects using F4VR BSTriShape API
 * 3. Proper finger curve intersection with mesh triangles
 * 4. Per-finger independent calculation
 * 
 * Based on HIGGS Skyrim's finger_curves.cpp and math_utils.cpp
 */

#include "FingerCurves.h"
#include "FingerCurveData.h"  // Contains the full 201-sample lookup tables
#include "RockFingerPose.h"   // Ported ROCK grab_finger_pose_math solver
#include "FRIKInterface.h"
#include "Config.h"
#include "Utils.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

// Disable Windows min/max macros that conflict with std::numeric_limits
#undef min
#undef max

namespace heisenberg
{
    // =========================================================================
    // CONSTANTS
    // =========================================================================

    static constexpr float g_minAllowedFingerAngle = 0.0f;  // From HIGGS: 5 * 0.0174533 = 5 degrees

    // =========================================================================
    // FINGER CALIBRATION DATA (from HIGGS Skyrim finger_curves.cpp)
    // These are calibrated for VRIK/FRIK hand models at 0.85 hand size
    // 6 entries: 5 fingers + 1 alternate thumb curve
    // =========================================================================

    // Direction of fully open finger (in hand space) - when curled = 0, finger points this way
    RE::NiPoint3 g_fingerZeroAngleVecs[6] = {
        { 0.645101f, -0.2895f, 0.707132f },     // Thumb
        { 0.0860232f, -0.367079f, 0.926203f },  // Index
        { 0.0679187f, -0.440377f, 0.89524f },   // Middle
        { 0.00764529f, -0.484981f, 0.874491f }, // Ring
        { 0.00850428f, -0.456607f, 0.889628f }, // Pinky
        { 0.423404f, -0.681123f, 0.597328f },   // Thumb alternate
    };

    // Normal of finger curl plane (in hand space) - finger curls around this axis
    RE::NiPoint3 g_fingerNormals[6] = {
        { 0.323886f, -0.734591f, -0.596216f },  // Thumb
        { 0.991006f, 0.127169f, -0.0416413f },  // Index
        { 0.983405f, 0.180855f, 0.0143569f },   // Middle
        { 0.943637f, 0.292879f, 0.154177f },    // Ring
        { 0.893703f, 0.402568f, 0.198077f },    // Pinky
        { -0.499817f, -0.725545f, -0.473041f }, // Thumb alternate
    };

    // Start position of each finger (in hand space, relative to hand node)
    RE::NiPoint3 g_fingerStartPositions[6] = {
        { 3.50024f, -0.920044f, 2.56506f },   // Thumb
        { 2.29413f, -0.0739136f, 9.21887f },  // Index  
        { -0.00622559f, -0.173218f, 9.20593f }, // Middle
        { -1.90326f, -0.712585f, 8.77051f },  // Ring
        { -3.40552f, -1.56665f, 7.83484f },   // Pinky
        { 3.50049f, -0.920029f, 2.56506f },   // Thumb alternate
    };

    // Forward decl — defined further down with the other math helpers.
    static RE::NiPoint3 TransformPoint(const RE::NiTransform& transform, const RE::NiPoint3& point);

    // =========================================================================
    // FINGER CALIBRATION
    //
    // HIGGS Skyrim ships purely hardcoded VRIK-calibrated constants for
    // g_fingerZeroAngleVecs / g_fingerNormals / g_fingerStartPositions.
    // Those values assume VRIK's "NPC L/R Hand [LHnd/RHnd]" hand-local frame.
    // FRIK's LArm_Hand / RArm_Hand bones use a different axis convention, so
    // the VRIK values point every finger arc in the wrong world direction on
    // F4VR — the finger plane ends up tangent to the mesh instead of cutting
    // through it, and 0/5 fingers intersect.
    //
    // We fix this by deriving the three per-finger vectors from the live FRIK
    // skeleton the first time a hand is used:
    //   startPos       = finger knuckle position, in hand-local space
    //   zeroAngleVec   = direction from knuckle to distal/tip bone
    //   fingerNormal   = curl plane normal (bone chain cross product)
    //
    // Calibration happens ONCE per hand per session (lazy, guarded by
    // g_fingerCalibrated[]). Only the hand subtree is walked so the scan
    // can't pick up 3rd-person-skeleton bones (the old attempt did, landing
    // world coords ~5000 into the arrays).
    // =========================================================================

    static bool g_fingerCalibrated[2] = { false, false };  // [0]=right, [1]=left

    // Palm derived from calibrated finger geometry, in hand-local (COM-tree
    // LArm_Hand / RArm_Hand) space. Written once per hand during calibration.
    // Used by placement + finger-curl so both run in the SAME frame (the
    // skinned hand), eliminating the wand↔bone offset that was leaving
    // grabbed objects floating outside the visible palm.
    static RE::NiPoint3 g_calibratedPalmPos[2];     // [0]=right, [1]=left
    static RE::NiPoint3 g_calibratedPalmDir[2];     // [0]=right, [1]=left
    // Palmar direction (from knuckle centroid toward palm cup) in hand-local
    // space. Derived from cross(fingerNormal, zeroAngle) — the initial curl
    // direction of the fingertips as curl angle opens from 0.
    static RE::NiPoint3 g_calibratedPalmarDir[2];   // [0]=right, [1]=left

    // Per-hand finger data used by runtime curl placement. The public g_finger*
    // arrays remain the unmirrored HIGGS defaults; each hand gets its own copy
    // so calibration on one side cannot overwrite the other side.
    static RE::NiPoint3 g_handFingerZeroAngleVecs[2][6];   // [0]=right, [1]=left
    static RE::NiPoint3 g_handFingerNormals[2][6];
    static RE::NiPoint3 g_handFingerStartPositions[2][6];
    static bool g_handFingerDataInitialized = false;

    static void InitializeHandFingerDataDefaults(bool force = false)
    {
        if (g_handFingerDataInitialized && !force) {
            return;
        }

        for (int handIdx = 0; handIdx < 2; ++handIdx) {
            const bool isLeft = handIdx == 1;
            for (int i = 0; i < 6; ++i) {
                g_handFingerZeroAngleVecs[handIdx][i] = g_fingerZeroAngleVecs[i];
                g_handFingerNormals[handIdx][i] = g_fingerNormals[i];
                g_handFingerStartPositions[handIdx][i] = g_fingerStartPositions[i];
                if (isLeft) {
                    g_handFingerZeroAngleVecs[handIdx][i].x *= -1.0f;
                    g_handFingerStartPositions[handIdx][i].x *= -1.0f;
                    g_handFingerNormals[handIdx][i].y *= -1.0f;
                    g_handFingerNormals[handIdx][i].z *= -1.0f;
                }
            }
        }

        g_handFingerDataInitialized = true;
    }

    // Utils::FindNode uses node->GetRuntimeData().children which, for reasons
    // yet unclear on F4VR, returned nullptr for LArm_Finger11 even though our
    // direct children dump found that node. Use direct `children` iteration
    // here (same pattern as the diagnostic that confirmed the bones exist).
    static RE::NiAVObject* FindNodeDirect(RE::NiAVObject* root, const char* name, int maxDepth)
    {
        if (!root || maxDepth <= 0) return nullptr;
        if (root->name.c_str() && std::strcmp(root->name.c_str(), name) == 0) {
            return root;
        }
        RE::NiNode* asNode = root->IsNode();
        if (!asNode) return nullptr;
        for (std::uint32_t i = 0; i < asNode->children.size(); ++i) {
            auto& ch = asNode->children[i];
            if (!ch) continue;
            if (auto found = FindNodeDirect(ch.get(), name, maxDepth - 1)) {
                return found;
            }
        }
        return nullptr;
    }

    static inline RE::NiTransform InverseRigidTransform(const RE::NiTransform& t)
    {
        RE::NiTransform inv;
        // Rotation inverse = transpose (assumes orthonormal, which NiNode
        // world rotations are in practice — uniform scale lives in .scale).
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                inv.rotate.entry[r][c] = t.rotate.entry[c][r];
            }
        }
        inv.scale = t.scale > 0.0001f ? 1.0f / t.scale : 1.0f;
        // translate = -R^T * t.translate * invScale
        RE::NiPoint3 nt;
        nt.x = -(inv.rotate.entry[0][0]*t.translate.x + inv.rotate.entry[0][1]*t.translate.y + inv.rotate.entry[0][2]*t.translate.z);
        nt.y = -(inv.rotate.entry[1][0]*t.translate.x + inv.rotate.entry[1][1]*t.translate.y + inv.rotate.entry[1][2]*t.translate.z);
        nt.z = -(inv.rotate.entry[2][0]*t.translate.x + inv.rotate.entry[2][1]*t.translate.y + inv.rotate.entry[2][2]*t.translate.z);
        inv.translate.x = nt.x * inv.scale;
        inv.translate.y = nt.y * inv.scale;
        inv.translate.z = nt.z * inv.scale;
        return inv;
    }

    void CalibrateFingerDataFromSkeleton(RE::NiAVObject* handNode, bool isLeft)
    {
        if (!handNode) return;
        InitializeHandFingerDataDefaults();
        int handIdx = isLeft ? 1 : 0;
        if (g_fingerCalibrated[handIdx]) return;

        const RE::NiTransform handInv = InverseRigidTransform(handNode->world);
        const char* prefix = isLeft ? "LArm_Finger" : "RArm_Finger";

        // One-shot diagnostic: list direct children of the hand node so we
        // can tell whether finger bones are actually in this subtree or if
        // we were handed a non-skinned wrapper node.
        if (RE::NiNode* handAsNode = handNode->IsNode()) {
            std::string childNames;
            for (std::uint32_t i = 0; i < handAsNode->children.size() && i < 32; ++i) {
                auto& ch = handAsNode->children[i];
                if (ch) {
                    childNames += ch->name.c_str();
                    childNames += ", ";
                }
            }
            spdlog::info("[FINGER-CAL] {} handNode='{}' direct children: [{}]",
                         isLeft ? "L" : "R", handNode->name.c_str(), childNames);
        }

        int foundCount = 0;
        for (int fi = 0; fi < 5; ++fi) {
            const int fingerDigit = fi + 1;  // 1=thumb..5=pinky in FRIK naming
            char n1[16], n2[16], n3[16];
            std::snprintf(n1, sizeof(n1), "%s%d1", prefix, fingerDigit);
            std::snprintf(n2, sizeof(n2), "%s%d2", prefix, fingerDigit);
            std::snprintf(n3, sizeof(n3), "%s%d3", prefix, fingerDigit);

            RE::NiAVObject* b1 = FindNodeDirect(handNode, n1, 6);
            RE::NiAVObject* b2 = FindNodeDirect(handNode, n2, 6);
            RE::NiAVObject* b3 = FindNodeDirect(handNode, n3, 6);
            if (!b1 || !b2 || !b3) {
                spdlog::warn("[FINGER-CAL] {} hand: missing bones for finger {} ({}/{}/{})",
                             isLeft ? "LEFT" : "RIGHT", fi, n1, n2, n3);
                continue;
            }

            const RE::NiPoint3 p1 = TransformPoint(handInv, b1->world.translate);
            const RE::NiPoint3 p2 = TransformPoint(handInv, b2->world.translate);
            const RE::NiPoint3 p3 = TransformPoint(handInv, b3->world.translate);

            // zeroAngle = direction of the PROXIMAL segment (p1→p2), not the
            // full knuckle→tip vector. FRIK's idle pose has the medial/distal
            // bones pre-curled ~40° (observed dot(d12,d23)≈0.65-0.75), so a
            // p1→p3 zeroAngle points tangent to the curl, not along the
            // finger. The proximal segment alone rotates only when the finger
            // *itself* flexes at the knuckle, which is rare in FRIK's rest
            // pose — it's the stable "finger-forward" axis we need for the
            // curl lookup to work. (The curve tables are still queried by
            // angle so the medial/distal curl is captured by the mesh-hit
            // angle, not by zeroAngle's orientation.)
            const RE::NiPoint3 v12 = p2 - p1;
            const RE::NiPoint3 v23 = p3 - p2;
            const float proxLen = VectorLength(v12);
            if (proxLen < 0.1f) continue;
            const RE::NiPoint3 zeroAngle = v12 * (1.0f / proxLen);
            const float fingerLen = VectorLength(p3 - p1);

            // Curl plane normal from the bone triplet. Valid for any pose
            // where p1/p2/p3 aren't colinear — FRIK's pre-curled rest pose
            // guarantees a real bend, so this is always well-defined.
            RE::NiPoint3 normal = CrossProduct(v12, v23);
            float nLen = VectorLength(normal);
            if (nLen < 0.01f) {
                // Degenerate straight chain — fall back to a stable normal
                // perpendicular to the finger, using hand-local +X (thumb-side
                // for right hand, opposite for left).
                RE::NiPoint3 ref(isLeft ? -1.0f : 1.0f, 0.0f, 0.0f);
                normal = CrossProduct(ref, zeroAngle);
                nLen = VectorLength(normal);
                if (nLen < 0.01f) continue;  // can't derive a stable normal
            }
            normal = normal * (1.0f / nLen);

            g_handFingerStartPositions[handIdx][fi] = p1;
            g_handFingerZeroAngleVecs[handIdx][fi] = zeroAngle;
            g_handFingerNormals[handIdx][fi] = normal;
            foundCount++;

            spdlog::info("[FINGER-CAL] {} f{}: start=({:.2f},{:.2f},{:.2f}) zero=({:.3f},{:.3f},{:.3f}) normal=({:.3f},{:.3f},{:.3f}) len={:.2f}",
                         isLeft ? "L" : "R", fi,
                         p1.x, p1.y, p1.z,
                         zeroAngle.x, zeroAngle.y, zeroAngle.z,
                         normal.x, normal.y, normal.z,
                         fingerLen);
        }

        if (foundCount == 5) {
            // Alt-thumb shares the thumb vectors — its alternate curl curve
            // is what matters, not the geometry. HIGGS uses slightly different
            // constants here but FRIK bone data doesn't expose that; thumb
            // parity is sufficient for our grabs.
            g_handFingerStartPositions[handIdx][FINGER_THUMB_ALT] = g_handFingerStartPositions[handIdx][FINGER_THUMB];
            g_handFingerZeroAngleVecs[handIdx][FINGER_THUMB_ALT] = g_handFingerZeroAngleVecs[handIdx][FINGER_THUMB];
            g_handFingerNormals[handIdx][FINGER_THUMB_ALT] = g_handFingerNormals[handIdx][FINGER_THUMB];

            // Derive palm anchor from calibrated fingers. Average the 5
            // knuckle positions and zero-angle directions — centroid of the
            // metacarpals is the anatomical palm center, and the averaged
            // forward direction is the palm-forward axis. Both are in the
            // same hand-local frame as the finger cast data, so placement
            // built from these lands exactly where the fingers will meet.
            RE::NiPoint3 palmPos(0, 0, 0);
            RE::NiPoint3 palmDir(0, 0, 0);
            for (int i = 0; i < 5; ++i) {
                palmPos = palmPos + g_handFingerStartPositions[handIdx][i];
                palmDir = palmDir + g_handFingerZeroAngleVecs[handIdx][i];
            }
            palmPos = palmPos * 0.2f;
            palmDir = VectorNormalized(palmDir);

            // Palmar direction: average of cross(fingerNormal, zeroAngle) across
            // fingers 1..4 (skip thumb — its opposable geometry makes its normal
            // point off-axis and pollutes the mean). This is the direction each
            // fingertip moves as the curl angle opens from zero — from the
            // extended position toward the palm cup. Used to shift the palm
            // anchor off the knuckle centroid (dorsal side) into the palm.
            RE::NiPoint3 palmarDir(0, 0, 0);
            for (int i = 1; i < 5; ++i) {
                palmarDir = palmarDir + CrossProduct(g_handFingerNormals[handIdx][i], g_handFingerZeroAngleVecs[handIdx][i]);
            }
            // Orthogonalize against palmDir. The raw sum often has a component
            // along the finger-forward axis (slight curl/normal asymmetry
            // across fingers) which would pull the shifted palm anchor toward
            // the wrist or past the fingertips instead of into the palm cup.
            const float fwdProj = DotProduct(palmarDir, palmDir);
            palmarDir = palmarDir - palmDir * fwdProj;
            palmarDir = VectorNormalized(palmarDir);

            g_calibratedPalmPos[handIdx] = palmPos;
            g_calibratedPalmDir[handIdx] = palmDir;
            g_calibratedPalmarDir[handIdx] = palmarDir;

            g_fingerCalibrated[handIdx] = true;
            spdlog::info("[FINGER-CAL] {} hand calibration complete ({}/5 fingers) palm=({:.2f},{:.2f},{:.2f}) dir=({:.3f},{:.3f},{:.3f}) palmar=({:.3f},{:.3f},{:.3f})",
                         isLeft ? "LEFT" : "RIGHT", foundCount,
                         palmPos.x, palmPos.y, palmPos.z,
                         palmDir.x, palmDir.y, palmDir.z,
                         palmarDir.x, palmarDir.y, palmarDir.z);
        } else {
            spdlog::warn("[FINGER-CAL] {} hand: only {}/5 fingers resolved — retaining HIGGS defaults",
                         isLeft ? "LEFT" : "RIGHT", foundCount);
        }
    }

    void ResetFingerCalibration()
    {
        InitializeHandFingerDataDefaults(true);
        g_fingerCalibrated[0] = false;
        g_fingerCalibrated[1] = false;
        g_calibratedPalmPos[0] = RE::NiPoint3();
        g_calibratedPalmPos[1] = RE::NiPoint3();
        g_calibratedPalmDir[0] = RE::NiPoint3();
        g_calibratedPalmDir[1] = RE::NiPoint3();
        g_calibratedPalmarDir[0] = RE::NiPoint3();
        g_calibratedPalmarDir[1] = RE::NiPoint3();
    }

    bool GetCalibratedPalmLocal(bool isLeft, RE::NiPoint3& outPos, RE::NiPoint3& outDir)
    {
        int handIdx = isLeft ? 1 : 0;
        if (!g_fingerCalibrated[handIdx]) return false;
        outPos = g_calibratedPalmPos[handIdx];
        outDir = g_calibratedPalmDir[handIdx];
        return true;
    }

    bool GetCalibratedPalmarLocal(bool isLeft, RE::NiPoint3& outDir)
    {
        int handIdx = isLeft ? 1 : 0;
        if (!g_fingerCalibrated[handIdx]) return false;
        outDir = g_calibratedPalmarDir[handIdx];
        return true;
    }

    // =========================================================================
    // TRIANGLE DATA METHODS
    // =========================================================================

    // IEEE 754 binary16 → binary32. Handles zeros, normals, denormals, and
    // inf/NaN. Mesh data stays in normal range in practice, but we handle the
    // rest so corrupted meshes don't produce infinities that poison the math.
    static inline float HalfToFloat(std::uint16_t h)
    {
        std::uint32_t s = (h >> 15) & 0x1;
        std::uint32_t e = (h >> 10) & 0x1F;
        std::uint32_t m = h & 0x3FF;
        std::uint32_t bits;
        if (e == 0) {
            if (m == 0) {
                bits = s << 31;
            } else {
                while (!(m & 0x400)) { m <<= 1; e = static_cast<std::uint32_t>(e - 1); }
                e = static_cast<std::uint32_t>(e + 1);
                m &= ~0x400u;
                bits = (s << 31) | ((e + (127 - 15)) << 23) | (m << 13);
            }
        } else if (e == 31) {
            bits = (s << 31) | 0x7F800000u | (m << 13);
        } else {
            bits = (s << 31) | ((e + (127 - 15)) << 23) | (m << 13);
        }
        float f;
        std::memcpy(&f, &bits, sizeof(float));
        return f;
    }

    static inline RE::NiPoint3 ReadVertexPosition(std::uintptr_t vert, std::uint32_t posOffset, bool fullPrec)
    {
        if (fullPrec) {
            return *reinterpret_cast<RE::NiPoint3*>(vert + posOffset);
        }
        // F4VR default: position is 3 half-floats (6 bytes) at posOffset.
        // Fourth half-float at +6 is bitangentX (ignored here).
        const std::uint16_t* h = reinterpret_cast<const std::uint16_t*>(vert + posOffset);
        return RE::NiPoint3(HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]));
    }

    TriangleData::TriangleData(const Triangle& tri, std::uintptr_t vertices, std::uint32_t posOffset, std::uint8_t vertexSize, bool fullPrec)
    {
        std::uintptr_t vert = vertices + tri.vertexIndices[0] * vertexSize;
        v0 = ReadVertexPosition(vert, posOffset, fullPrec);

        vert = vertices + tri.vertexIndices[1] * vertexSize;
        v1 = ReadVertexPosition(vert, posOffset, fullPrec);

        vert = vertices + tri.vertexIndices[2] * vertexSize;
        v2 = ReadVertexPosition(vert, posOffset, fullPrec);
    }

    void TriangleData::ApplyTransform(const RE::NiTransform& transform)
    {
        // Transform each vertex using NiTransform: translate + rotate * point * scale
        auto TransformPointLocal = [&](RE::NiPoint3& p) {
            RE::NiPoint3 rotated;
            rotated.x = transform.rotate.entry[0][0] * p.x + transform.rotate.entry[0][1] * p.y + transform.rotate.entry[0][2] * p.z;
            rotated.y = transform.rotate.entry[1][0] * p.x + transform.rotate.entry[1][1] * p.y + transform.rotate.entry[1][2] * p.z;
            rotated.z = transform.rotate.entry[2][0] * p.x + transform.rotate.entry[2][1] * p.y + transform.rotate.entry[2][2] * p.z;
            p.x = rotated.x * transform.scale + transform.translate.x;
            p.y = rotated.y * transform.scale + transform.translate.y;
            p.z = rotated.z * transform.scale + transform.translate.z;
        };
        
        TransformPointLocal(v0);
        TransformPointLocal(v1);
        TransformPointLocal(v2);
    }

    // =========================================================================
    // MATH UTILITIES
    // =========================================================================

    // Transform a point by NiTransform
    static RE::NiPoint3 TransformPoint(const RE::NiTransform& transform, const RE::NiPoint3& point)
    {
        RE::NiPoint3 rotated;
        rotated.x = transform.rotate.entry[0][0] * point.x + transform.rotate.entry[0][1] * point.y + transform.rotate.entry[0][2] * point.z;
        rotated.y = transform.rotate.entry[1][0] * point.x + transform.rotate.entry[1][1] * point.y + transform.rotate.entry[1][2] * point.z;
        rotated.z = transform.rotate.entry[2][0] * point.x + transform.rotate.entry[2][1] * point.y + transform.rotate.entry[2][2] * point.z;
        return RE::NiPoint3(
            rotated.x * transform.scale + transform.translate.x,
            rotated.y * transform.scale + transform.translate.y,
            rotated.z * transform.scale + transform.translate.z
        );
    }

    // Transform a vector (direction only, no translate) by NiMatrix3
    static RE::NiPoint3 TransformVector(const RE::NiMatrix3& rotate, const RE::NiPoint3& vec)
    {
        return RE::NiPoint3(
            rotate.entry[0][0] * vec.x + rotate.entry[0][1] * vec.y + rotate.entry[0][2] * vec.z,
            rotate.entry[1][0] * vec.x + rotate.entry[1][1] * vec.y + rotate.entry[1][2] * vec.z,
            rotate.entry[2][0] * vec.x + rotate.entry[2][1] * vec.y + rotate.entry[2][2] * vec.z
        );
    }

    RE::NiPoint3 RotateVectorByAxisAngle(const RE::NiPoint3& vec, const RE::NiPoint3& axis, float angle)
    {
        // Rodrigues' rotation formula
        float cosA = std::cos(angle);
        float sinA = std::sin(angle);
        RE::NiPoint3 cross = CrossProduct(axis, vec);
        float dot = DotProduct(axis, vec);
        
        return RE::NiPoint3(
            vec.x * cosA + cross.x * sinA + axis.x * dot * (1.0f - cosA),
            vec.y * cosA + cross.y * sinA + axis.y * dot * (1.0f - cosA),
            vec.z * cosA + cross.z * sinA + axis.z * dot * (1.0f - cosA)
        );
    }

    float LineSegmentLineSegmentDistance(const Point2& p1, const Point2& p2, const Point2& p3, const Point2& p4)
    {
        // Distance between two 2D line segments
        // Using parametric form and finding closest approach
        Point2 d1 = p2 - p1;
        Point2 d2 = p4 - p3;
        Point2 r = p1 - p3;
        
        float a = d1.x * d1.x + d1.y * d1.y;
        float b = d1.x * d2.x + d1.y * d2.y;
        float c = d2.x * d2.x + d2.y * d2.y;
        float d = d1.x * r.x + d1.y * r.y;
        float e = d2.x * r.x + d2.y * r.y;
        
        float denom = a * c - b * b;
        
        float s, t;
        if (std::abs(denom) < 0.0001f) {
            s = 0.0f;
            t = (b > c ? d / b : e / c);
        } else {
            s = (b * e - c * d) / denom;
            t = (a * e - b * d) / denom;
        }
        
        s = std::clamp(s, 0.0f, 1.0f);
        t = std::clamp(t, 0.0f, 1.0f);
        
        Point2 closest1(p1.x + s * d1.x, p1.y + s * d1.y);
        Point2 closest2(p3.x + t * d2.x, p3.y + t * d2.y);
        
        return Point2Length(closest1 - closest2);
    }

    bool LineSegmentIntersectsLineSegment(const Point2& p1, const Point2& p2, const Point2& p3, const Point2& p4, Point2* outIntersection)
    {
        float d1x = p2.x - p1.x;
        float d1y = p2.y - p1.y;
        float d2x = p4.x - p3.x;
        float d2y = p4.y - p3.y;
        
        float cross = d1x * d2y - d1y * d2x;
        if (std::abs(cross) < 0.0001f) return false;
        
        float dx = p3.x - p1.x;
        float dy = p3.y - p1.y;
        
        float t1 = (dx * d2y - dy * d2x) / cross;
        float t2 = (dx * d1y - dy * d1x) / cross;
        
        if (t1 >= 0.0f && t1 <= 1.0f && t2 >= 0.0f && t2 <= 1.0f) {
            if (outIntersection) {
                outIntersection->x = p1.x + t1 * d1x;
                outIntersection->y = p1.y + t1 * d1y;
            }
            return true;
        }
        return false;
    }

    RE::NiPoint3 GetFurthestPointOnLineSegment(const RE::NiPoint3& segStart, const RE::NiPoint3& segEnd, const RE::NiPoint3& point)
    {
        float distToStart = VectorLength(segStart - point);
        float distToEnd = VectorLength(segEnd - point);
        return distToStart > distToEnd ? segStart : segEnd;
    }

    bool PlaneIntersectsLineSegment(
        const RE::NiPoint3& planePoint,
        const RE::NiPoint3& planeNormal,
        const RE::NiPoint3& lineStart,
        const RE::NiPoint3& lineEnd,
        RE::NiPoint3& outIntersection)
    {
        RE::NiPoint3 lineDir = lineEnd - lineStart;
        float denom = DotProduct(planeNormal, lineDir);

        if (std::abs(denom) < 0.0001f) {
            return false;  // Line is parallel to plane
        }

        float t = DotProduct(planeNormal, planePoint - lineStart) / denom;

        if (t < 0.0f || t > 1.0f) {
            return false;  // Intersection outside line segment
        }

        outIntersection = RE::NiPoint3(
            lineStart.x + lineDir.x * t,
            lineStart.y + lineDir.y * t,
            lineStart.z + lineDir.z * t
        );
        return true;
    }

    int CircleIntersectsTriangle(
        const RE::NiPoint3& circleCenter,
        const RE::NiPoint3& circleNormal,
        float circleRadius,
        const TriangleData& triangle,
        RE::NiPoint3& outIntersectionPoint1,
        RE::NiPoint3& outIntersectionPoint2)
    {
        // Check each triangle edge for intersection with the finger curl circle (plane)
        RE::NiPoint3 edge1Intersection, edge2Intersection, edge3Intersection;
        bool edge1Intersects = PlaneIntersectsLineSegment(circleCenter, circleNormal, triangle.v0, triangle.v1, edge1Intersection);
        bool edge2Intersects = PlaneIntersectsLineSegment(circleCenter, circleNormal, triangle.v0, triangle.v2, edge2Intersection);
        bool edge3Intersects = PlaneIntersectsLineSegment(circleCenter, circleNormal, triangle.v1, triangle.v2, edge3Intersection);
        
        int numIntersections = edge1Intersects + edge2Intersects + edge3Intersects;
        if (numIntersections < 2) return 0;
        
        // Get the two intersection points
        RE::NiPoint3 p, d;
        float edgeLength;
        if (edge1Intersects && edge2Intersects) {
            RE::NiPoint3 edge = edge2Intersection - edge1Intersection;
            edgeLength = VectorLength(edge);
            d = edgeLength > 0.0001f ? edge * (1.0f / edgeLength) : RE::NiPoint3();
            p = edge1Intersection - circleCenter;
        } else if (edge1Intersects && edge3Intersects) {
            RE::NiPoint3 edge = edge3Intersection - edge1Intersection;
            edgeLength = VectorLength(edge);
            d = edgeLength > 0.0001f ? edge * (1.0f / edgeLength) : RE::NiPoint3();
            p = edge1Intersection - circleCenter;
        } else {
            RE::NiPoint3 edge = edge3Intersection - edge2Intersection;
            edgeLength = VectorLength(edge);
            d = edgeLength > 0.0001f ? edge * (1.0f / edgeLength) : RE::NiPoint3();
            p = edge2Intersection - circleCenter;
        }
        
        // Find intersection with circle of radius circleRadius
        float a = DotProduct(d, d);
        float b = 2.0f * DotProduct(p, d);
        float c = DotProduct(p, p) - circleRadius * circleRadius;
        float disc = b * b - 4 * a * c;
        
        if (disc < 0) return 0;
        
        float sqrtDisc = std::sqrt(disc);
        float t1 = (-b - sqrtDisc) / (2 * a);
        float t2 = (-b + sqrtDisc) / (2 * a);
        
        bool t1Valid = t1 >= 0 && t1 <= edgeLength;
        bool t2Valid = t2 >= 0 && t2 <= edgeLength;
        
        if (t1Valid && t2Valid) {
            outIntersectionPoint1 = circleCenter + p + d * t1;
            outIntersectionPoint2 = circleCenter + p + d * t2;
            return 2;
        } else if (t1Valid) {
            outIntersectionPoint1 = circleCenter + p + d * t1;
            return 1;
        } else if (t2Valid) {
            outIntersectionPoint1 = circleCenter + p + d * t2;
            return 1;
        }
        
        return 0;
    }

    // =========================================================================
    // LOOKUP FUNCTIONS (Binary search in 201-sample finger curve data)
    // =========================================================================

    int LookupFingerByAngle(const SavedFingerData fingerVals[], float desiredAngle, SavedFingerData* outData)
    {
        // Binary search for the angle in the sorted finger curve data
        // Finger data is sorted by increasing angle (from g_fingerTipVals[finger][0].angle to [200].angle)
        int lo = 0;
        int hi = kNumFingerVals - 1;
        
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (fingerVals[mid].angle < desiredAngle) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        
        // Clamp to valid range
        if (lo >= kNumFingerVals) lo = kNumFingerVals - 1;
        if (lo < 0) lo = 0;

        // HIGGS floor semantics (finger_curves.cpp:487-499, Jul 19 finger audit): the
        // binary search lands on the first sample >= desiredAngle (ceil). HIGGS returns
        // the sample BELOW so the curve-walk seed covers the segment containing the
        // query angle — ceil skipped that segment, missing graze-contact crossings
        // (over-curl, or full miss -> 0.2 near-fist stab through the mesh).
        if (lo > 0 && fingerVals[lo].angle > desiredAngle) {
            lo -= 1;
        }

        if (outData) {
            *outData = fingerVals[lo];
        }
        return lo;
    }

    // =========================================================================
    // TRIANGLE EXTRACTION (F4VR BSTriShape API)
    // =========================================================================

    static void UpdateTriangles(RE::BSTriShape* geom, std::vector<TriangleData>& triangles)
    {
        if (!geom) return;
        
        // Skip culled geometry
        if (geom->flags.flags & 0x01) return;
        
        // Get geometry data
        // In F4VR, BSTriShape inherits from BSGeometry which has geometryData
        // 
        // CRITICAL: F4VR offsets are DIFFERENT from F4SE (non-VR)!
        // Verified via Ghidra decompilation of BSGeometry::GetVertexDesc (0x141c31de0)
        // 
        // F4VR BSGeometry layout (VR offsets = non-VR + 0x40):
        // 0x188: BSGeometryData* geometryData   (non-VR: 0x148)
        // 0x190: vertexDesc                      (non-VR: 0x150)
        //
        // F4VR BSTriShape layout:
        // 0x1A0: numTriangles (uint32)           (non-VR: 0x160)
        // 0x1A4: numVertices (uint16)            (non-VR: 0x164)
        
        // Access via offsets since CommonLibF4 may not have all members
        auto* baseGeom = static_cast<RE::BSGeometry*>(geom);
        
        // F4VR BSGeometryData layout (from f4sevr/f4se/BSGeometry.h) - UNCHANGED:
        // 0x00: vertexDesc (uint64)
        // 0x08: VertexData* vertexData
        // 0x10: TriangleData* triangleData
        // 0x18: refCount
        //
        // VertexData layout:
        // 0x00: d3d11Buffer
        // 0x08: vertexBlock (actual vertex data)
        //
        // TriangleData layout:
        // 0x00: d3d11Buffer
        // 0x08: triangles (actual triangle indices)
        
        struct VertexData_F4VR {
            void* d3d11Buffer;        // 0x00
            std::uint8_t* vertexBlock; // 0x08
            // More fields follow but we don't need them
        };
        
        struct TriangleDataStruct_F4VR {
            void* d3d11Buffer;          // 0x00
            std::uint16_t* triangles;   // 0x08
            // More fields follow but we don't need them
        };
        
        struct BSGeometryData_F4VR {
            std::uint64_t vertexDesc;            // 0x00
            VertexData_F4VR* vertexData;         // 0x08
            TriangleDataStruct_F4VR* triangleData; // 0x10
            volatile std::int32_t refCount;       // 0x18
        };
        
        // VR-specific offsets (verified via Ghidra)
        constexpr std::ptrdiff_t VR_GEOMETRY_DATA_OFFSET = 0x188;  // BSGeometry::geometryData
        constexpr std::ptrdiff_t VR_NUM_TRIANGLES_OFFSET = 0x1A0;  // BSTriShape::numTriangles
        constexpr std::ptrdiff_t VR_NUM_VERTICES_OFFSET = 0x1A4;   // BSTriShape::numVertices
        
        // Read geometryData pointer (VR offset 0x188)
        std::uint8_t* geomBytes = reinterpret_cast<std::uint8_t*>(baseGeom);
        BSGeometryData_F4VR* geomData = *reinterpret_cast<BSGeometryData_F4VR**>(geomBytes + VR_GEOMETRY_DATA_OFFSET);
        
        // Debug: Log what we're finding
        static int debugLogCounter = 0;
        bool shouldLog = (++debugLogCounter % 180 == 0);  // Log every ~3 seconds at 60fps
        
        if (!geomData) {
            if (shouldLog) spdlog::debug("[FINGER] No geometry data at VR offset 0x188 for '{}'", 
                geom->name.c_str() ? geom->name.c_str() : "unnamed");
            return;
        }
        
        // Get vertex and triangle counts (VR offsets)
        std::uint32_t numTris = *reinterpret_cast<std::uint32_t*>(geomBytes + VR_NUM_TRIANGLES_OFFSET);
        std::uint16_t numVerts = *reinterpret_cast<std::uint16_t*>(geomBytes + VR_NUM_VERTICES_OFFSET);
        
        // Debug: Also check non-VR offsets and other potential locations to understand the data layout
        std::uint32_t numTrisNonVR = *reinterpret_cast<std::uint32_t*>(geomBytes + 0x160);
        std::uint16_t numVertsNonVR = *reinterpret_cast<std::uint16_t*>(geomBytes + 0x164);
        
        // Check BSGeometry type byte at 0x198 (VR offset, non-VR would be 0x158)
        std::uint8_t geomType = *reinterpret_cast<std::uint8_t*>(geomBytes + 0x198);
        
        if (shouldLog || (numTris == 0 && numVerts > 0)) {
            spdlog::debug("[FINGER] BSTriShape '{}': VR(0x1A0)={} tris, VR(0x1A4)={} verts | NonVR(0x160)={} tris | type=0x{:02X}",
                geom->name.c_str() ? geom->name.c_str() : "unnamed",
                numTris, numVerts, numTrisNonVR, geomType);
        }
        
        if (numTris == 0 || numVerts == 0) {
            if (shouldLog) {
                spdlog::debug("[FINGER] Skipping '{}' with invalid geometry counts (tris={}, verts={})",
                    geom->name.c_str() ? geom->name.c_str() : "unnamed", numTris, numVerts);
            }
            return;
        }
        
        // Get vertex data
        if (!geomData->vertexData || !geomData->vertexData->vertexBlock) {
            if (shouldLog) spdlog::debug("[FINGER] No vertex data for '{}' (vertexData={}, vertexBlock={})",
                geom->name.c_str() ? geom->name.c_str() : "unnamed",
                (void*)geomData->vertexData,
                geomData->vertexData ? (void*)geomData->vertexData->vertexBlock : nullptr);
            return;
        }
        
        if (!geomData->triangleData || !geomData->triangleData->triangles) {
            if (shouldLog) spdlog::debug("[FINGER] No triangle data for '{}' (triangleData={}, triangles={})",
                geom->name.c_str() ? geom->name.c_str() : "unnamed",
                (void*)geomData->triangleData,
                geomData->triangleData ? (void*)geomData->triangleData->triangles : nullptr);
            return;
        }
        
        std::uintptr_t verts = reinterpret_cast<std::uintptr_t>(geomData->vertexData->vertexBlock);
        Triangle* tris = reinterpret_cast<Triangle*>(geomData->triangleData->triangles);
        
        // Calculate vertex size from vertexDesc
        // VR offset 0x190 for vertexDesc (if geometryData is NULL - otherwise use geomData->vertexDesc)
        // vertexSize = (vertexDesc & 0xF) * 4  OR  vertexSize = ((vertexDesc >> 2) & 0x3C)
        // The BSGeometryData has vertexDesc at offset 0x00
        constexpr std::ptrdiff_t VR_VERTEX_DESC_OFFSET = 0x190;
        
        // Prefer geometryData's vertexDesc (offset 0x00), fallback to BSGeometry's vertexDesc
        std::uint64_t vertexDescRaw = geomData->vertexDesc;
        if (vertexDescRaw == 0) {
            vertexDescRaw = *reinterpret_cast<std::uint64_t*>(geomBytes + VR_VERTEX_DESC_OFFSET);
        }
        
        // F4VR vertex size calculation: (vertexDesc & 0xF) * 4
        // Confirmed via Ghidra decomp of BSGraphics::Utility::UnpackVertexData @ 0x141db2650
        // which computes offset = index * ((desc*4) & 0x3C) ≡ index * ((desc&0xF)*4).
        std::uint8_t vertexSize = static_cast<std::uint8_t>((vertexDescRaw & 0xF) * 4);
        std::uint32_t posOffset = 0;  // Position is always at offset 0 in F4VR vertex format

        // VF_FULLPREC flag: (desc >> 44) & 0x400 → bit 54 of vertexDesc.
        // When set, position is 3 float32s (12 bytes). When clear (the usual
        // case for game meshes), position is 3 half-floats (6 bytes) followed
        // by a bitangentX half. Reading 12 bytes as NiPoint3 when it's really
        // 4 half-floats is the "70% garbage vertex" bug — bytes [2..6) and
        // [6..10) get reinterpreted as float32 and decode to huge values.
        const bool fullPrec = (vertexDescRaw & (1ULL << 54)) != 0;
        
        if (vertexSize == 0) {
            if (shouldLog) spdlog::debug("[FINGER] Invalid vertex size 0 for '{}', vertexDesc raw={:#x}",
                geom->name.c_str() ? geom->name.c_str() : "unnamed",
                vertexDescRaw);
            return;
        }
        
        // Log success on first triangle extraction
        static bool loggedFirstSuccess = false;
        if (!loggedFirstSuccess) {
            spdlog::info("[FINGER] SUCCESS extracting triangles: {} tris, {} verts, vertSize={} fullPrec={} from '{}'",
                numTris, numVerts, vertexSize, fullPrec ? 1 : 0,
                geom->name.c_str() ? geom->name.c_str() : "unnamed");
            loggedFirstSuccess = true;
        }
        
        // Extract triangles and transform to world space
        RE::NiTransform& nodeTransform = geom->world;
        
        for (std::uint32_t i = 0; i < numTris; i++) {
            Triangle tri = tris[i];
            
            // Validate indices
            if (tri.vertexIndices[0] >= numVerts || 
                tri.vertexIndices[1] >= numVerts || 
                tri.vertexIndices[2] >= numVerts) {
                continue;
            }
            
            TriangleData triData(tri, verts, posOffset, vertexSize, fullPrec);
            triData.ApplyTransform(nodeTransform);
            triangles.push_back(triData);
        }
        
        spdlog::trace("[FINGER] Extracted {} triangles from {}", 
                     numTris, geom->name.c_str() ? geom->name.c_str() : "unnamed");
    }

    void GetTriangles(RE::NiAVObject* root, std::vector<TriangleData>& outTriangles, std::size_t maxTriangles)
    {
        if (!root) return;

        // PERF CAP: stop once we've collected enough triangles. A high-poly object (e.g. an ornate
        // vase, tens of thousands of tris) extracted in full every frame — and then distance-tested
        // against the whole hand point cloud — collapses the frame rate to a freeze. Callers that
        // need the FULL mesh (finger solver) pass the default (unlimited); the per-frame hand push
        // passes a budget so it stays cheap on any object.
        if (outTriangles.size() >= maxTriangles) return;

        // Skip culled nodes (APP_CULLED = 0x01)
        if (root->flags.flags & 0x01) return;

        // Debug: Log node names on first extraction attempt
        static int nodeVisitCounter = 0;
        static bool loggedFirstAttempt = false;
        nodeVisitCounter++;
        bool shouldLog = (nodeVisitCounter <= 20);  // Log first 20 nodes
        
        // Get RTTI type name if available
        const char* typeName = "unknown";
        void* vtablePtr = *reinterpret_cast<void**>(root);
        
        // Check if this is a BSTriShape - try various methods
        RE::BSTriShape* triShape = root->IsTriShape();
        
        // Also check BSGeometry (BSTriShape inherits from BSGeometry)
        RE::BSGeometry* geom = root->IsGeometry();
        
        if (shouldLog && !loggedFirstAttempt) {
            spdlog::debug("[FINGER] Visiting node '{}': triShape={}, geom={}, vtable=0x{:016X}",
                root->name.c_str() ? root->name.c_str() : "(null)",
                triShape != nullptr,
                geom != nullptr,
                reinterpret_cast<uintptr_t>(vtablePtr));
        }
        
        // F4VR: BSTriShape may not be recognized by IsTriShape()
        // Try BSGeometry instead since it also has geometry data
        if (geom) {
            // Cast to BSTriShape even if IsTriShape() failed
            RE::BSTriShape* asTriShape = static_cast<RE::BSTriShape*>(geom);
            
            // Verify it has triangle/vertex counts
            std::uint32_t numTris = 0;
            std::uint16_t numVerts = 0;
            
            // Read numTriangles/numVertices from BSTriShape VR offsets (0x1A0/0x1A4)
            // CRITICAL: VR offsets are non-VR + 0x40!
            // Verified via Ghidra decompilation of BSTriShape::LoadBinary (0x141c368e0)
            constexpr std::ptrdiff_t VR_NUM_TRIANGLES_OFFSET = 0x1A0;  // Non-VR: 0x160
            constexpr std::ptrdiff_t VR_NUM_VERTICES_OFFSET = 0x1A4;   // Non-VR: 0x164
            
            std::uint8_t* geomBytes = reinterpret_cast<std::uint8_t*>(geom);
            numTris = *reinterpret_cast<std::uint32_t*>(geomBytes + VR_NUM_TRIANGLES_OFFSET);
            numVerts = *reinterpret_cast<std::uint16_t*>(geomBytes + VR_NUM_VERTICES_OFFSET);
            
            if (shouldLog && !loggedFirstAttempt) {
                spdlog::debug("[FINGER]   BSGeometry '{}': numTris={}, numVerts={}",
                    root->name.c_str() ? root->name.c_str() : "(null)",
                    numTris, numVerts);
            }
            
            // Always try to extract - don't filter by numTris since geometryData may have different counts
            // The UpdateTriangles function will validate internally
            UpdateTriangles(asTriShape, outTriangles);
            return;
        }

        // If it's a node, recurse into children
        RE::NiNode* node = root->IsNode();
        if (node) {
            if (shouldLog && !loggedFirstAttempt) {
                spdlog::debug("[FINGER]   Node '{}' has {} children",
                    root->name.c_str() ? root->name.c_str() : "(null)",
                    node->children.size());
            }
            if (nodeVisitCounter == 20) {
                loggedFirstAttempt = true;  // Stop after first batch
            }
            for (auto& child : node->children) {
                if (child) {
                    if (outTriangles.size() >= maxTriangles) break;
                    GetTriangles(child.get(), outTriangles, maxTriangles);
                }
            }
        }
    }

    // =========================================================================
    // FINGER INTERSECTION (HIGGS algorithm)
    // =========================================================================

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
        float& outInnerAngle)
    {
        // Check each triangle edge for intersection with finger plane
        RE::NiPoint3 edge1Intersection, edge2Intersection, edge3Intersection;
        bool edge1Intersects = PlaneIntersectsLineSegment(fingerCenter, fingerNormal, triangle.v0, triangle.v1, edge1Intersection);
        bool edge2Intersects = PlaneIntersectsLineSegment(fingerCenter, fingerNormal, triangle.v0, triangle.v2, edge2Intersection);
        
        if (!edge1Intersects && !edge2Intersects) {
            return { false, false, false };
        }
        
        bool edge3Intersects = PlaneIntersectsLineSegment(fingerCenter, fingerNormal, triangle.v1, triangle.v2, edge3Intersection);
        
        int numIntersections = edge1Intersects + edge2Intersects + edge3Intersects;
        if (numIntersections < 2) {
            return { false, false, false };
        }
        
        // Get intersection line segment
        RE::NiPoint3 d, p;
        float edgeLength;
        if (edge1Intersects && edge2Intersects) {
            RE::NiPoint3 edge = edge2Intersection - edge1Intersection;
            edgeLength = VectorLength(edge);
            d = edgeLength > 0.0001f ? edge * (1.0f / edgeLength) : RE::NiPoint3();
            p = edge1Intersection - fingerCenter;
        } else if (edge1Intersects && edge3Intersects) {
            RE::NiPoint3 edge = edge3Intersection - edge1Intersection;
            edgeLength = VectorLength(edge);
            d = edgeLength > 0.0001f ? edge * (1.0f / edgeLength) : RE::NiPoint3();
            p = edge1Intersection - fingerCenter;
        } else {
            RE::NiPoint3 edge = edge3Intersection - edge2Intersection;
            edgeLength = VectorLength(edge);
            d = edgeLength > 0.0001f ? edge * (1.0f / edgeLength) : RE::NiPoint3();
            p = edge2Intersection - fingerCenter;
        }
        
        RE::NiPoint3 edgeStart = p;
        RE::NiPoint3 edgeEnd = p + d * edgeLength;
        
        // Calculate angles for the edge endpoints
        float startLen = VectorLength(edgeStart);
        float endLen = VectorLength(edgeEnd);
        
        RE::NiPoint3 startDir = startLen > 0.0001f ? edgeStart * (1.0f / startLen) : RE::NiPoint3();
        RE::NiPoint3 endDir = endLen > 0.0001f ? edgeEnd * (1.0f / endLen) : RE::NiPoint3();
        
        float startAngle = std::acos(std::clamp(DotProduct(startDir, zeroAngleVector), -1.0f, 1.0f));
        if (DotProduct(fingerNormal, CrossProduct(zeroAngleVector, edgeStart)) < 0) {
            startAngle *= -1;
        }
        
        float endAngle = std::acos(std::clamp(DotProduct(endDir, zeroAngleVector), -1.0f, 1.0f));
        if (DotProduct(fingerNormal, CrossProduct(zeroAngleVector, edgeEnd)) < 0) {
            endAngle *= -1;
        }
        
        float scale = handScale / nodeScale;
        
        // Lambda to check if finger curve intersects the triangle edge
        auto CurveCheck = [&](const SavedFingerData fingerVals[], float startAngle, float endAngle, float& outAngle) -> bool
        {
            float smallestAngle = std::min(startAngle, endAngle);
            float minAllowedAngle = fingerVals[0].angle + g_minAllowedFingerAngle;

            // HIGGS CircleIntersection (math_utils.cpp:1338-1407, faithful port Jul 19
            // finger audit): the old port inspected ONE arbitrary hit with no face gate,
            // dropping HIGGS's dual-hit ordering + front-face gates and the -largerAngle
            // guaranteed-open fallback — on large faces pressed against the palm that
            // lost the open signal and let the finger close through the object.
            auto CircleIntersection = [&]() -> bool {
                RE::NiPoint3 circleInt1, circleInt2;
                int numCircleHits = CircleIntersectsTriangle(fingerCenter, fingerNormal,
                    fingerVals[0].fingerLength * scale, triangle, circleInt1, circleInt2);
                if (numCircleHits <= 0) {
                    return false;
                }
                const RE::NiPoint3 triNormal = VectorNormalized(CrossProduct(
                    triangle.v1 - triangle.v0, triangle.v2 - triangle.v1));
                if (numCircleHits > 1) {
                    const RE::NiPoint3 cti1 = circleInt1 - fingerCenter;
                    const RE::NiPoint3 cti2 = circleInt2 - fingerCenter;
                    const float angle1 = std::acos(std::clamp(DotProduct(VectorNormalized(cti1), zeroAngleVector), -1.0f, 1.0f));
                    const float angle2 = std::acos(std::clamp(DotProduct(VectorNormalized(cti2), zeroAngleVector), -1.0f, 1.0f));
                    const RE::NiPoint3 tangent1 = CrossProduct(fingerNormal, cti1);
                    const RE::NiPoint3 tangent2 = CrossProduct(fingerNormal, cti2);
                    const bool angle1Smaller = angle1 <= angle2;
                    const float smallerAngle = angle1Smaller ? angle1 : angle2;
                    const float largerAngle = angle1Smaller ? angle2 : angle1;
                    const RE::NiPoint3& smallerTangent = angle1Smaller ? tangent1 : tangent2;
                    const RE::NiPoint3& largerTangent = angle1Smaller ? tangent2 : tangent1;
                    const RE::NiPoint3& smallerCTI = angle1Smaller ? cti1 : cti2;
                    if (DotProduct(triNormal, smallerTangent) <= 0) {
                        float angle = smallerAngle;
                        if (DotProduct(fingerNormal, CrossProduct(zeroAngleVector, smallerCTI)) < 0) {
                            angle *= -1;
                        }
                        if (angle > 0) {
                            // hit at a positive angle while the finger curve missed — the
                            // circle pass only exists to catch behind-finger points (HIGGS)
                            return false;
                        }
                        outAngle = angle;
                        return true;
                    }
                    if (DotProduct(triNormal, largerTangent) <= 0) {
                        outAngle = -largerAngle;  // guaranteed-open signal
                        return true;
                    }
                    return false;
                }
                const RE::NiPoint3 cti = circleInt1 - fingerCenter;
                const RE::NiPoint3 tangent = CrossProduct(fingerNormal, cti);
                if (DotProduct(triNormal, tangent) <= 0) {
                    float angle = std::acos(std::clamp(DotProduct(VectorNormalized(cti), zeroAngleVector), -1.0f, 1.0f));
                    if (DotProduct(fingerNormal, CrossProduct(zeroAngleVector, cti)) < 0) {
                        angle *= -1;
                    }
                    if (angle > 0) {
                        return false;
                    }
                    outAngle = angle;
                    return true;
                }
                return false;
            };

            // Both ends behind finger - check circle intersection (derotate)
            if (std::max(startAngle, endAngle) < minAllowedAngle) {
                return CircleIntersection();
            }
            
            bool crossesBehind = smallestAngle < minAllowedAngle;
            
            bool startSmaller = startAngle < endAngle;
            float smallerAngle = startSmaller ? startAngle : endAngle;
            float largerAngle = startSmaller ? endAngle : startAngle;
            
            float lengthAtSmallerAngle = startSmaller ? VectorLength(edgeStart) : VectorLength(edgeEnd);
            float lengthAtLargerAngle = startSmaller ? VectorLength(edgeEnd) : VectorLength(edgeStart);
            
            Point2 smallerAnglePt = Point2(std::cos(smallerAngle), std::sin(smallerAngle)) * lengthAtSmallerAngle;
            Point2 largerAnglePt = Point2(std::cos(largerAngle), std::sin(largerAngle)) * lengthAtLargerAngle;
            
            SavedFingerData fingerData;
            int smallerIndex = LookupFingerByAngle(fingerVals, smallerAngle, &fingerData);
            float radiusAtSmallerAngle = fingerData.fingerLength * scale;
            
            int largerIndex = LookupFingerByAngle(fingerVals, largerAngle, &fingerData);
            float radiusAtLargerAngle = fingerData.fingerLength * scale;
            
            // Check if edge is outside the curve
            if (lengthAtSmallerAngle > radiusAtSmallerAngle || lengthAtLargerAngle > radiusAtLargerAngle) {
                // Check for intersection with the finger curve
                int end = std::min(largerIndex, kNumFingerVals - 2);
                for (int i = smallerIndex; i <= end; i++) {
                    float angle = fingerVals[i].angle;
                    float length = fingerVals[i].fingerLength * scale;
                    
                    float nextAngle = fingerVals[i + 1].angle;
                    float nextLength = fingerVals[i + 1].fingerLength * scale;
                    
                    Point2 currentPt = Point2(std::cos(angle), std::sin(angle)) * length;
                    Point2 nextPt = Point2(std::cos(nextAngle), std::sin(nextAngle)) * nextLength;
                    
                    Point2 intersection;
                    if (LineSegmentIntersectsLineSegment(smallerAnglePt, largerAnglePt, currentPt, nextPt, &intersection) ||
                        LineSegmentLineSegmentDistance(smallerAnglePt, largerAnglePt, currentPt, nextPt) <= 0.015f) {
                        
                        RE::NiPoint3 triNormal = VectorNormalized(CrossProduct(
                            triangle.v1 - triangle.v0, triangle.v2 - triangle.v1));
                        
                        RE::NiPoint3 currentPtDiskspace = VectorNormalized(RotateVectorByAxisAngle(zeroAngleVector, fingerNormal, angle)) * length;
                        RE::NiPoint3 nextPtDiskspace = VectorNormalized(RotateVectorByAxisAngle(zeroAngleVector, fingerNormal, nextAngle)) * nextLength;
                        RE::NiPoint3 tangent = nextPtDiskspace - currentPtDiskspace;
                        
                        if (DotProduct(triNormal, tangent) <= 0) {
                            outAngle = angle;
                            return true;
                        }
                    }
                }
            }
            
            // Check circle intersection if edge crosses behind finger
            if (crossesBehind) {
                return CircleIntersection();
            }

            return false;
        };
        
        float tipAngle = 0, outerAngle = 0, innerAngle = 0;
        bool tipIntersects = CurveCheck(g_fingerTipVals[fingerIndex], startAngle, endAngle, tipAngle);
        bool outerIntersects = CurveCheck(g_fingerOuterVals[fingerIndex], startAngle, endAngle, outerAngle);
        bool innerIntersects = CurveCheck(g_fingerInnerVals[fingerIndex], startAngle, endAngle, innerAngle);
        
        outTipAngle = tipAngle;
        outOuterAngle = outerAngle;
        outInnerAngle = innerAngle;
        
        return { tipIntersects, outerIntersects, innerIntersects };
    }

    // HIGGS three-list structure: collect inner/outer/tip intersections
    // independently, look each up in its own table, then take max(inner,
    // outer, tip) — the loosest curl that still wraps the mesh. Port from
    // backups\0.6.250\FingerCollision.cpp:430-500 (matches HIGGS Skyrim
    // math_utils.cpp:1989-2083).
    //
    // NOTE: outIntersection.angle carries the final CURL VALUE (0..1) on
    // success, not an angle. Field reused to avoid signature churn.
    bool GetFingerIntersections(
        const std::vector<TriangleData>& triangles,
        int fingerIndex,
        float handScale,
        const RE::NiPoint3& fingerCenter,
        const RE::NiPoint3& fingerNormal,
        const RE::NiPoint3& zeroAngleVector,
        Intersection& outIntersection)
    {
        std::vector<Intersection> tipIntersections, outerIntersections, innerIntersections;

        // TEMP diagnostic: count how many triangles pass each stage so we can
        // see whether the plane intersects any geometry at all, and if so how
        // many triangles survive the curve-check.
        int planeHits = 0;
        float minDistToPlane = 1e30f;
        float maxDistToPlane = -1e30f;
        for (size_t i = 0; i < triangles.size(); ++i) {
            const auto& t = triangles[i];
            auto sdist = [&](const RE::NiPoint3& v) {
                return (v.x - fingerCenter.x) * fingerNormal.x
                     + (v.y - fingerCenter.y) * fingerNormal.y
                     + (v.z - fingerCenter.z) * fingerNormal.z;
            };
            float d0 = sdist(t.v0), d1 = sdist(t.v1), d2 = sdist(t.v2);
            float mn = std::min({d0, d1, d2});
            float mx = std::max({d0, d1, d2});
            if (mn < minDistToPlane) minDistToPlane = mn;
            if (mx > maxDistToPlane) maxDistToPlane = mx;
            if (mn <= 0 && mx >= 0) ++planeHits;

            float tipAngle, outerAngle, innerAngle;
            auto [tipHit, outerHit, innerHit] = FingerIntersectsTriangle(
                fingerIndex, fingerCenter, fingerNormal, zeroAngleVector,
                handScale, 1.0f, triangles[i], tipAngle, outerAngle, innerAngle);

            if (tipHit)   tipIntersections.push_back({ tipAngle,   static_cast<std::int32_t>(i) });
            if (outerHit) outerIntersections.push_back({ outerAngle, static_cast<std::int32_t>(i) });
            if (innerHit) innerIntersections.push_back({ innerAngle, static_cast<std::int32_t>(i) });
        }
        spdlog::warn("[FINGER-DIAG] f{} center=({:.1f},{:.1f},{:.1f}) normal=({:.3f},{:.3f},{:.3f}) | plane-hit tris={}/{} signedDist=[{:.1f},{:.1f}] | curves tip={} outer={} inner={}",
                     fingerIndex, fingerCenter.x, fingerCenter.y, fingerCenter.z,
                     fingerNormal.x, fingerNormal.y, fingerNormal.z,
                     planeHits, (int)triangles.size(), minDistToPlane, maxDistToPlane,
                     (int)tipIntersections.size(), (int)outerIntersections.size(), (int)innerIntersections.size());

        constexpr float HUGE_ANGLE = 1.0e30f;

        auto findBest = [HUGE_ANGLE](const std::vector<Intersection>& list) -> std::pair<float, std::int32_t> {
            float bestAngle = HUGE_ANGLE;
            std::int32_t bestIndex = -1;
            for (const auto& inter : list) {
                if (inter.angle >= 0 && inter.angle < bestAngle) {
                    bestAngle = inter.angle;
                    bestIndex = inter.triangleIndex;
                }
            }
            return { bestAngle, bestIndex };
        };

        // HIGGS CheckNegativeWithinAngle (math_utils.cpp:2009-2020). Return
        // the triangle index of a negative-angle intersection whose magnitude
        // is smaller than the best positive angle — signals that a backface
        // sits nearer than the frontface, i.e. the finger plane crossed the
        // "inside" of the mesh first. Caller interprets this as "hand should
        // stay open" (open-grasp like a flat palm or far-side grip).
        auto checkNegativeWithinAngle = [](const std::vector<Intersection>& list, float angle) -> std::int32_t {
            for (const auto& inter : list) {
                if (inter.angle < 0.0f && std::abs(inter.angle) < angle) {
                    return inter.triangleIndex;
                }
            }
            return -1;
        };

        // HIGGS order: inner → outer → tip. First list whose best positive
        // is beaten by a negative wins — short-circuit to open hand.
        auto [innerAngle, innerIdx] = findBest(innerIntersections);
        if (std::int32_t negIdx = checkNegativeWithinAngle(innerIntersections, innerAngle); negIdx >= 0) {
            outIntersection.angle = -1.0f;
            outIntersection.triangleIndex = negIdx;
            return true;
        }

        auto [outerAngle, outerIdx] = findBest(outerIntersections);
        if (std::int32_t negIdx = checkNegativeWithinAngle(outerIntersections, outerAngle); negIdx >= 0) {
            outIntersection.angle = -1.0f;
            outIntersection.triangleIndex = negIdx;
            return true;
        }

        auto [tipAngle, tipIdx] = findBest(tipIntersections);
        if (std::int32_t negIdx = checkNegativeWithinAngle(tipIntersections, tipAngle); negIdx >= 0) {
            outIntersection.angle = -1.0f;
            outIntersection.triangleIndex = negIdx;
            return true;
        }

        float tipVal = -1.0f, outerVal = -1.0f, innerVal = -1.0f;
        SavedFingerData data;

        if (tipAngle < HUGE_ANGLE) {
            LookupFingerByAngle(g_fingerTipVals[fingerIndex], tipAngle, &data);
            tipVal = data.curveVal;
        }
        if (outerAngle < HUGE_ANGLE) {
            LookupFingerByAngle(g_fingerOuterVals[fingerIndex], outerAngle, &data);
            outerVal = data.curveVal;
        }
        if (innerAngle < HUGE_ANGLE) {
            LookupFingerByAngle(g_fingerInnerVals[fingerIndex], innerAngle, &data);
            innerVal = data.curveVal;
        }

        // Loosest fit that still wraps the mesh — max across the three curves.
        float curveVal = tipVal;
        if (outerVal > curveVal) curveVal = outerVal;
        if (innerVal > curveVal) curveVal = innerVal;
        if (curveVal < 0.0f) {
            return false;
        }

        std::int32_t triIdx = (curveVal == tipVal)   ? tipIdx
                            : (curveVal == outerVal) ? outerIdx
                            :                          innerIdx;
        outIntersection.angle = curveVal;   // curl value, not an angle (field reused)
        outIntersection.triangleIndex = triIdx;
        return true;
    }

    // =========================================================================
    // MAIN FINGER CURL CALCULATION
    // =========================================================================

    static RE::NiPoint3 ClosestPointOnTriangleToPoint(const RE::NiPoint3& point, const TriangleData& triangle);

    FingerCurlResult CalculateFingerCurlFromGeometry(
        RE::NiAVObject* objectNode,
        RE::NiAVObject* handNode,
        const RE::NiPoint3& palmPos,
        const RE::NiPoint3& /* palmDirection */,
        bool isLeft,
        float handSize)
    {
        FingerCurlResult result;
        result.success = false;

        if (!objectNode || !handNode) {
            spdlog::warn("[FINGER] Invalid nodes for geometry-based curl (obj={:p}, hand={:p})",
                        (void*)objectNode, (void*)handNode);
            return result;
        }

        spdlog::warn("[FINGER] Entry: handNode='{}' objectNode='{}' isLeft={}",
                    handNode->name.c_str(), objectNode->name.c_str(),
                    isLeft ? 1 : 0);

        // Lazily calibrate the finger constants from the FRIK skeleton on
        // first use per hand. VRIK's hardcoded constants don't match FRIK's
        // LArm_Hand / RArm_Hand frame — without this each finger plane lands
        // tangent to the mesh.
        CalibrateFingerDataFromSkeleton(handNode, isLeft);

        // Extract triangles from object mesh
        std::vector<TriangleData> triangles;
        triangles.reserve(256);  // Pre-allocate reasonable size
        GetTriangles(objectNode, triangles);

        if (triangles.empty()) {
            spdlog::warn("[FINGER] No triangles extracted from object '{}', using fallback 0.9",
                        objectNode->name.c_str());
            // Fallback for objects without accessible geometry: 0.9 (HIGGS parity, Jul 19
            // finger audit) — 0.6 buried the fingertips inside fat objects; near-open only
            // ever hovers, which reads far better than penetration.
            result.thumb = 0.9f;
            result.index = 0.9f;
            result.middle = 0.9f;
            result.ring = 0.9f;
            result.pinky = 0.9f;
            result.success = true;
            return result;
        }
        
        spdlog::debug("[FINGER] Extracted {} triangles from object", triangles.size());

        // HIGGS palmToPoint shift (hand.cpp:1448, 1482): find the closest point
        // on the mesh to palmPos, then shift each finger's worldspace start by
        // (closestPoint - palmPos). This casts the fingers as if the object
        // were already snapped to the palm, which is what the placement step
        // will do a moment later.
        RE::NiPoint3 closestMeshPoint = palmPos;
        {
            float bestDistSq = std::numeric_limits<float>::max();
            for (const auto& tri : triangles) {
                RE::NiPoint3 cp = ClosestPointOnTriangleToPoint(palmPos, tri);
                RE::NiPoint3 d = cp - palmPos;
                float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    closestMeshPoint = cp;
                }
            }
        }
        const RE::NiPoint3 palmToPoint = closestMeshPoint - palmPos;

        // HIGGS nearby-triangle filter (hand.cpp:1436-1444): only pass
        // triangles that sit within grabMaxTriangleDistance² (sqrDist) of the
        // closest mesh point to the finger intersection pass. Without this,
        // finger curl planes can trip on triangles on the far side of the
        // object and report spurious hits.
        const float maxTriSqrDist = g_config.grabMaxTriangleDistance;
        std::vector<TriangleData> nearbyTriangles;
        nearbyTriangles.reserve(triangles.size());
        for (const auto& tri : triangles) {
            RE::NiPoint3 cp = ClosestPointOnTriangleToPoint(closestMeshPoint, tri);
            RE::NiPoint3 d = cp - closestMeshPoint;
            float sqrDist = d.x * d.x + d.y * d.y + d.z * d.z;
            if (sqrDist < maxTriSqrDist) {
                nearbyTriangles.push_back(tri);
            }
        }
        spdlog::debug("[FINGER] {}/{} triangles within grabMaxTriangleDistance",
                     nearbyTriangles.size(), triangles.size());

        // Get hand transform
        RE::NiTransform handTransform = handNode->world;
        float handScale = handSize / 0.85f;  // 0.85 is VRIK/FRIK default hand size
        InitializeHandFingerDataDefaults();
        const int handIdx = isLeft ? 1 : 0;

        // Calculate curl for each finger — HIGGS default 0.0 (closed). On
        // intersection miss the finger is "just closed completely"
        // (hand.cpp:1494-1495). After the loop we clamp to min 0.2 so the
        // finger doesn't over-curl.
        float fingerCurls[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        bool foundAnyIntersection = false;
        bool useAltThumb = false;

        for (int i = 0; i < 5; i++) {
            // Get finger vectors in hand space
            RE::NiPoint3 zeroAngleVec = g_handFingerZeroAngleVecs[handIdx][i];
            RE::NiPoint3 fingerNormal = g_handFingerNormals[handIdx][i];
            RE::NiPoint3 startPos = g_handFingerStartPositions[handIdx][i];

            // Transform to world space
            RE::NiPoint3 zeroAngleWorld = VectorNormalized(TransformVector(handTransform.rotate, zeroAngleVec));
            RE::NiPoint3 fingerNormalWorld = VectorNormalized(TransformVector(handTransform.rotate, fingerNormal));
            RE::NiPoint3 fingerCenterWorld = TransformPoint(handTransform, startPos) + palmToPoint;

            // Find intersection with geometry — use nearby-filtered set.
            Intersection intersection;
            bool found = GetFingerIntersections(nearbyTriangles, i, handScale,
                fingerCenterWorld, fingerNormalWorld, zeroAngleWorld, intersection);

            if (found) {
                foundAnyIntersection = true;
                // intersection.angle carries the max-across-curves curl value.
                // HIGGS only rejects strictly negative angles (→ open hand);
                // positive values pass through unclamped. We match that below
                // in the min-0.2 clamp pass.
                fingerCurls[i] = intersection.angle;
            }
            // On miss: leave fingerCurls[i] at 0.0 (closed). HIGGS hand.cpp:1495
            // "No finger intersection, so just close it completely" (return 0).

            // HIGGS thumb alternate curve (hand.cpp:1508-1518): if the standard
            // sideways thumb missed (curveVal <= 0), try the alternate curve.
            // Prefer the alt result only if it intersects at a non-negative
            // angle, or if both missed — otherwise keep the (negative) standard
            // result which the post-loop pass converts to "open hand".
            if (i == 0 && fingerCurls[i] <= 0.0f) {
                RE::NiPoint3 altZeroAngle = g_handFingerZeroAngleVecs[handIdx][FINGER_THUMB_ALT];
                RE::NiPoint3 altNormal = g_handFingerNormals[handIdx][FINGER_THUMB_ALT];
                RE::NiPoint3 altStart = g_handFingerStartPositions[handIdx][FINGER_THUMB_ALT];

                RE::NiPoint3 altZeroAngleWorld = VectorNormalized(TransformVector(handTransform.rotate, altZeroAngle));
                RE::NiPoint3 altNormalWorld = VectorNormalized(TransformVector(handTransform.rotate, altNormal));
                RE::NiPoint3 altCenterWorld = TransformPoint(handTransform, altStart) + palmToPoint;

                Intersection altIntersection;
                float altCurl = 0.0f;  // miss → closed (HIGGS FingerCheck returns 0)
                bool altFound = GetFingerIntersections(nearbyTriangles, FINGER_THUMB_ALT, handScale,
                    altCenterWorld, altNormalWorld, altZeroAngleWorld, altIntersection);
                if (altFound) altCurl = altIntersection.angle;

                // HIGGS rule: take alt if it's non-negative, or if both missed.
                if (altCurl > 0.0f || (altCurl == 0.0f && fingerCurls[0] == 0.0f)) {
                    fingerCurls[0] = altCurl;
                    useAltThumb = true;
                    if (altFound) foundAnyIntersection = true;
                }
            }
        }

        // HIGGS post-pass (hand.cpp:1521-1532):
        //   - negative angle → hand stays open (1.0)
        //   - positive angle → use curve value
        //   - clamp min 0.2 so the finger doesn't over-curl
        for (int i = 0; i < 5; i++) {
            if (fingerCurls[i] < 0.0f) {
                fingerCurls[i] = 1.0f;  // open
            }
            fingerCurls[i] = (std::max)(0.2f, fingerCurls[i]);
        }

        if (!foundAnyIntersection) {
            spdlog::warn("[FINGER] {} tris extracted from '{}' but no finger intersected (palm pos=({:.1f},{:.1f},{:.1f}))",
                        triangles.size(), objectNode->name.c_str(),
                        palmPos.x, palmPos.y, palmPos.z);
            return result;
        }

        result.thumb = fingerCurls[0];
        result.index = fingerCurls[1];
        result.middle = fingerCurls[2];
        result.ring = fingerCurls[3];
        result.pinky = fingerCurls[4];
        result.useAlternateThumbCurve = useAltThumb;
        result.success = true;

        spdlog::info("[FINGER] Geometry-based curl: thumb={:.2f} index={:.2f} middle={:.2f} ring={:.2f} pinky={:.2f}{}",
                     result.thumb, result.index, result.middle, result.ring, result.pinky,
                     useAltThumb ? " (alt thumb)" : "");

        return result;
    }

    // =========================================================================
    // ROCK-BASED FINGER CURL (ported grab_finger_pose_math solver)
    // =========================================================================

    // Per-finger curl-disk radius in game units at FRIK/VRIK default hand size
    // (0.85). Sourced from the open-end (curveVal=1) fingerLength of
    // g_fingerTipVals: thumb/index/middle/ring/pinky. The 6th (thumb-alt) reuses
    // the thumb length. Scaled by handScale at runtime.
    static constexpr float kRockFingerLengths[6] = { 7.518f, 6.587f, 7.219f, 6.203f, 5.184f, 7.518f };

    // ROCK fingerMaxAnglesRadians (GrabFinger.h:804). Max curl sweep per finger.
    static constexpr float kRockFingerMaxAngles[6] = { 1.225f, 1.45f, 1.50f, 1.48f, 1.42f, 1.225f };

    /**
     * Drop-in alternative to CalculateFingerCurlFromGeometry that uses ROCK's
     * parametric curl-disk solver (RockFingerPose.h) instead of Heisenberg's
     * curve tables. Reuses the same calibration, triangle extraction, palmToPoint
     * seating and nearby-triangle filter so the only difference is the solver
     * core. Output convention is identical (1.0 = open, 0.0 = closed) so the
     * caller's application path is unchanged.
     */
    FingerCurlResult CalculateFingerCurlFromGeometryRock(
        RE::NiAVObject* objectNode,
        RE::NiAVObject* handNode,
        const RE::NiPoint3& palmPos,
        const RE::NiPoint3& /* palmDirection */,
        bool isLeft,
        float handSize)
    {
        namespace rfm = heisenberg::rock_finger_math;
        using RockTri = rfm::Triangle<RE::NiPoint3>;

        FingerCurlResult result;
        result.success = false;

        if (!objectNode || !handNode) {
            spdlog::warn("[FINGER-ROCK] Invalid nodes (obj={:p}, hand={:p})",
                        (void*)objectNode, (void*)handNode);
            return result;
        }

        // Same lazy per-hand calibration the table solver uses.
        CalibrateFingerDataFromSkeleton(handNode, isLeft);

        std::vector<TriangleData> triangles;
        triangles.reserve(256);
        GetTriangles(objectNode, triangles);

        if (triangles.empty()) {
            spdlog::warn("[FINGER-ROCK] No triangles from '{}', fallback 0.6", objectNode->name.c_str());
            result.thumb = result.index = result.middle = result.ring = result.pinky = 0.6f;
            result.success = true;
            return result;
        }

        // palmToPoint seat: find closest mesh point to palm, shift finger bases
        // as if the object were already snapped to the palm (matches the table
        // solver and the placement step that follows).
        RE::NiPoint3 closestMeshPoint = palmPos;
        {
            float bestDistSq = std::numeric_limits<float>::max();
            for (const auto& tri : triangles) {
                RE::NiPoint3 cp = ClosestPointOnTriangleToPoint(palmPos, tri);
                RE::NiPoint3 d = cp - palmPos;
                float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    closestMeshPoint = cp;
                }
            }
        }
        const RE::NiPoint3 palmToPoint = closestMeshPoint - palmPos;

        // Nearby-triangle filter (same grabMaxTriangleDistance² gate) + convert
        // to ROCK triangle type once.
        const float maxTriSqrDist = g_config.grabMaxTriangleDistance;
        std::vector<RockTri> rockTris;
        rockTris.reserve(triangles.size());
        for (const auto& tri : triangles) {
            RE::NiPoint3 cp = ClosestPointOnTriangleToPoint(closestMeshPoint, tri);
            RE::NiPoint3 d = cp - closestMeshPoint;
            float sqrDist = d.x * d.x + d.y * d.y + d.z * d.z;
            if (sqrDist < maxTriSqrDist) {
                rockTris.push_back(RockTri{ tri.v0, tri.v1, tri.v2 });
            }
        }
        if (rockTris.empty()) {
            spdlog::warn("[FINGER-ROCK] No triangles within grabMaxTriangleDistance for '{}'", objectNode->name.c_str());
            return result;
        }

        RE::NiTransform handTransform = handNode->world;
        const float handScale = handSize / 0.85f;
        InitializeHandFingerDataDefaults();
        const int handIdx = isLeft ? 1 : 0;

        // rejectBacksideHits uses the seated mesh point + its triangle normal as
        // the surface plane. Off by default (config-tunable) to keep the first
        // pass robust; the behind-curl-plane open detection is always active.
        const bool rejectBackside = g_config.rockFingerRejectBackside;
        const float surfaceTol = g_config.rockFingerSurfaceTolerance;
        RE::NiPoint3 surfaceNormal{};
        if (rejectBackside) {
            // Use the normal of the triangle owning the closest mesh point.
            float bestDistSq = std::numeric_limits<float>::max();
            for (const auto& t : rockTris) {
                RE::NiPoint3 cp = rfm::closestPointOnTriangle(closestMeshPoint, t);
                RE::NiPoint3 d = cp - closestMeshPoint;
                float dsq = d.x * d.x + d.y * d.y + d.z * d.z;
                if (dsq < bestDistSq) {
                    bestDistSq = dsq;
                    surfaceNormal = rfm::triangleNormal(t);
                }
            }
        }

        float fingerCurls[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        bool foundAny = false;
        bool useAltThumb = false;

        for (int i = 0; i < 5; i++) {
            RE::NiPoint3 zeroAngleVec = g_handFingerZeroAngleVecs[handIdx][i];
            RE::NiPoint3 fingerNormal = g_handFingerNormals[handIdx][i];
            RE::NiPoint3 startPos = g_handFingerStartPositions[handIdx][i];

            RE::NiPoint3 zeroAngleWorld = VectorNormalized(TransformVector(handTransform.rotate, zeroAngleVec));
            RE::NiPoint3 fingerNormalWorld = VectorNormalized(TransformVector(handTransform.rotate, fingerNormal));
            RE::NiPoint3 centerWorld = TransformPoint(handTransform, startPos) + palmToPoint;

            const float fingerLength = kRockFingerLengths[i] * handScale;
            const float maxAngle = kRockFingerMaxAngles[i];

            rfm::FingerCurlValue solved{};

            if (i == FINGER_THUMB) {
                // Thumb-aware: also try the alternate cross-palm curl normal.
                RE::NiPoint3 altNormal = g_handFingerNormals[handIdx][FINGER_THUMB_ALT];
                RE::NiPoint3 altNormalWorld = VectorNormalized(TransformVector(handTransform.rotate, altNormal));

                auto thumb = rfm::solveThumbAwareFingerCurveCurlValue(rockTris,
                    centerWorld, fingerNormalWorld, altNormalWorld, zeroAngleWorld,
                    maxAngle, fingerLength, 0.0f, /*allowAlternate*/ true,
                    closestMeshPoint, surfaceNormal, rejectBackside, surfaceTol);
                solved = thumb.value;
                useAltThumb = thumb.usedAlternateThumbCurve;
            } else {
                solved = rfm::solveFingerCurveCurlValue(rockTris,
                    centerWorld, fingerNormalWorld, zeroAngleWorld,
                    maxAngle, fingerLength, 0.0f,
                    closestMeshPoint, surfaceNormal, rejectBackside, surfaceTol);
            }

            // Ray fallback toward the seated mesh point when the disk missed.
            // Use ROCK's exact gate: skip the fallback for a thumb that intentionally used the
            // alternate curve and missed (that miss is the closed-thumb result, not an error).
            if (rfm::shouldRunFallbackRayAfterCurveSolve(static_cast<std::size_t>(i), solved.hit, useAltThumb)) {
                RE::NiPoint3 toContact = closestMeshPoint - centerWorld;
                RE::NiPoint3 probeDir = VectorNormalized(toContact);
                if (VectorLengthSquared(probeDir) > 0.5f) {
                    const float probeDist = std::clamp(VectorLength(toContact) + 6.0f, 6.0f, 26.0f);
                    auto ray = rfm::solveFingerCurlValue(rockTris,
                        centerWorld, probeDir, probeDist, 0.0f, /*probeRadius*/ 1.25f,
                        fingerNormalWorld, zeroAngleWorld, maxAngle,
                        closestMeshPoint, surfaceNormal, rejectBackside, surfaceTol);
                    if (ray.hit) {
                        solved = ray;
                    }
                }
            }

            if (solved.hit) {
                foundAny = true;
            }
            // ROCK value: [0,1], 1=open, 0=closed; openedByBehindContact → 1.0.
            // Miss leaves value at clampedMin (0) = closed, matching the table
            // solver's "close completely" on miss.
            fingerCurls[i] = solved.value;
        }

        // Same post-pass clamp as the table solver: min 0.2 so fingers don't
        // over-curl.
        for (int i = 0; i < 5; i++) {
            fingerCurls[i] = (std::max)(0.2f, fingerCurls[i]);
        }

        if (!foundAny) {
            spdlog::warn("[FINGER-ROCK] {} tris from '{}' but no finger intersected (palm=({:.1f},{:.1f},{:.1f}))",
                        triangles.size(), objectNode->name.c_str(), palmPos.x, palmPos.y, palmPos.z);
            return result;
        }

        result.thumb = fingerCurls[0];
        result.index = fingerCurls[1];
        result.middle = fingerCurls[2];
        result.ring = fingerCurls[3];
        result.pinky = fingerCurls[4];
        result.useAlternateThumbCurve = useAltThumb;
        result.success = true;

        spdlog::info("[FINGER-ROCK] curl: thumb={:.2f} index={:.2f} middle={:.2f} ring={:.2f} pinky={:.2f}{}",
                     result.thumb, result.index, result.middle, result.ring, result.pinky,
                     useAltThumb ? " (alt thumb)" : "");

        return result;
    }

    // =========================================================================
    // ADDITIONAL GEOMETRY UTILITIES
    // =========================================================================

    static RE::NiPoint3 ClosestPointOnTriangleToPoint(const RE::NiPoint3& point, const TriangleData& triangle)
    {
        const RE::NiPoint3& a = triangle.v0;
        const RE::NiPoint3& b = triangle.v1;
        const RE::NiPoint3& c = triangle.v2;

        RE::NiPoint3 ab = b - a;
        RE::NiPoint3 ac = c - a;
        RE::NiPoint3 ap = point - a;

        float d1 = DotProduct(ab, ap);
        float d2 = DotProduct(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) {
            return a;
        }

        RE::NiPoint3 bp = point - b;
        float d3 = DotProduct(ab, bp);
        float d4 = DotProduct(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) {
            return b;
        }

        float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
            float v = d1 / (d1 - d3);
            return a + ab * v;
        }

        RE::NiPoint3 cp = point - c;
        float d5 = DotProduct(ab, cp);
        float d6 = DotProduct(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) {
            return c;
        }

        float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
            float w = d2 / (d2 - d6);
            return a + ac * w;
        }

        float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
            RE::NiPoint3 bc = c - b;
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return b + bc * w;
        }

        float denom = 1.0f / (va + vb + vc);
        float v = vb * denom;
        float w = vc * denom;
        return a + ab * v + ac * w;
    }

    // Closest point on a mesh to a palm ray, tolerant to imprecise aim.
    //
    // HIGGS's math_utils.cpp:2176 uses ray-plane intersection clamped to the
    // triangle, then a weighted (directional/lateral) metric. That assumes the
    // palm already points at the target — fine in Skyrim VR where grab target
    // is confirmed adjacent before this runs, but F4VR activations fire with
    // the palm aimed anywhere (floor items, side-held objects). The clamped
    // plane-intersect then lands on far triangle edges and placement snaps to
    // the wrong side of the object ("fork ends up further along the approach
    // axis instead of in the palm").
    //
    // Robust alternative (used here): for each triangle, compute the closest
    // point on the triangle to the PALM POSITION, then score by squared
    // distance to the ray (primary) with squared distance to palm as tiebreak.
    // Always returns the nearest mesh surface to the palm regardless of aim.
    // No front-face filter — with a closest-to-palm metric, back-faces of a
    // thin object (like a fork tine) are always further than the front face
    // so the filter is redundant and dangerous on meshes with inverted winding.
    bool GetClosestPointOnGeometryToLine(
        const std::vector<TriangleData>& triangles,
        const RE::NiPoint3& lineStart,
        const RE::NiPoint3& lineDir,
        RE::NiPoint3& outPoint,
        RE::NiPoint3& outNormal,
        int& outTriIndex,
        float& outDist)
    {
        RE::NiPoint3 direction = VectorNormalized(lineDir);
        if (VectorLengthSquared(direction) < 1e-6f) {
            return false;
        }

        const bool usePalmRay = g_config.enablePalmRayCastPlacement;
        const float directionalWeight = (std::max)(0.0f, g_config.grabDirectionalWeight);
        const float lateralWeight = (std::max)(0.0f, g_config.grabLateralWeight);

        float minScore         = std::numeric_limits<float>::max();
        float minDistSqToRay   = std::numeric_limits<float>::max();
        float minDistSqToPalm  = std::numeric_limits<float>::max();
        bool  found            = false;

        for (size_t i = 0; i < triangles.size(); i++) {
            const TriangleData& tri = triangles[i];

            RE::NiPoint3 edge1 = tri.v1 - tri.v0;
            RE::NiPoint3 edge2 = tri.v2 - tri.v0;
            RE::NiPoint3 rawNormal = CrossProduct(edge1, edge2);
            RE::NiPoint3 triNormal = VectorNormalized(rawNormal);
            if (VectorLengthSquared(triNormal) < 1e-6f) {
                continue; // degenerate triangle
            }

            // Closest point on THIS triangle to the infinite palm LINE, not
            // to the palm position. Using to-line makes per-triangle candidate
            // points independent of small palm jitter along the ray, which
            // fixes grab-to-grab placement drift on thin triangles (e.g. fan
            // blades) where closest-to-palm slid along the triangle with tiny
            // palm shifts. Method: ray-plane intersect, then clamp to triangle
            // (HIGGS math_utils.cpp:906). If line is ~parallel to the plane,
            // fall back to projecting the centroid onto the line.
            RE::NiPoint3 candidatePoint;
            if (usePalmRay) {
                const float denom = DotProduct(rawNormal, direction);
                if (std::abs(denom) > 1e-6f) {
                    float t = DotProduct(tri.v0 - lineStart, rawNormal) / denom;
                    RE::NiPoint3 planeHit = lineStart + direction * t;
                    candidatePoint = ClosestPointOnTriangleToPoint(planeHit, tri);
                } else {
                    RE::NiPoint3 centroid = (tri.v0 + tri.v1 + tri.v2) * (1.0f / 3.0f);
                    float t = DotProduct(centroid - lineStart, direction);
                    RE::NiPoint3 onLine = lineStart + direction * t;
                    candidatePoint = ClosestPointOnTriangleToPoint(onLine, tri);
                }
            } else {
                candidatePoint = ClosestPointOnTriangleToPoint(lineStart, tri);
            }
            RE::NiPoint3 toCandidate    = candidatePoint - lineStart;
            float        alongRay       = DotProduct(toCandidate, direction);
            RE::NiPoint3 closestOnRay   = (alongRay > 0.0f)
                ? lineStart + direction * alongRay
                : lineStart; // clamp at ray origin for points behind the palm

            float distSqToRay  = VectorLengthSquared(candidatePoint - closestOnRay);
            float distSqToPalm = VectorLengthSquared(toCandidate);
            float directionalDist = (std::max)(0.0f, alongRay);
            float score = usePalmRay
                ? directionalWeight * directionalDist * directionalDist + lateralWeight * distSqToRay
                : distSqToPalm;

            bool isBetter = score < minScore - 1e-4f;
            if (!isBetter && std::abs(score - minScore) <= 1e-4f) {
                isBetter = distSqToPalm < minDistSqToPalm;
            }
            if (isBetter) {
                minScore        = score;
                minDistSqToRay  = distSqToRay;
                minDistSqToPalm = distSqToPalm;
                outPoint        = candidatePoint;
                // Orient normal toward the palm so placement offset push
                // direction is consistent regardless of triangle winding.
                outNormal = (DotProduct(triNormal, direction) > 0.0f)
                    ? triNormal * -1.0f
                    : triNormal;
                outTriIndex = static_cast<int>(i);
                found = true;
            }
        }

        outDist = found ? std::sqrt(minDistSqToRay) : std::numeric_limits<float>::max();
        return found;
    }

    bool GetClosestMeshPointToPoint(
        const std::vector<TriangleData>& triangles,
        const RE::NiPoint3& point,
        RE::NiPoint3& outPoint,
        float& outDist)
    {
        float minDistSq = std::numeric_limits<float>::max();
        bool found = false;
        for (const TriangleData& tri : triangles) {
            RE::NiPoint3 cand = ClosestPointOnTriangleToPoint(point, tri);
            float d2 = VectorLengthSquared(cand - point);
            if (d2 < minDistSq) {
                minDistSq = d2;
                outPoint = cand;
                found = true;
            }
        }
        outDist = found ? std::sqrt(minDistSq) : std::numeric_limits<float>::max();
        return found;
    }

} // namespace heisenberg
