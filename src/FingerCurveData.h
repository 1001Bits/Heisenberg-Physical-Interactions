#pragma once
// ============================================================================
// HIGGS Skyrim Finger Curve Lookup Tables - Ported to F4VR
// ============================================================================
// These are the calibrated 201-sample lookup tables from HIGGS Skyrim
// Each array is [6 fingers][201 samples] of SavedFingerData { curveVal, angle, fingerLength }
// 
// Finger indices: 0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky, 5=ThumbAlt
// curveVal: 0.0 (fully curled) to 1.0 (fully extended)
// angle: Rotation angle of finger at this curl value (radians)
// fingerLength: Effective finger length at this curl value
// 
// Usage:
// - g_fingerTipVals: For tip of finger contact detection
// - g_fingerOuterVals: For outer knuckle contact detection  
// - g_fingerInnerVals: For inner palm/base contact detection
// ============================================================================

#include "FingerCurves.h"

namespace heisenberg {

// Full 201-sample lookup tables - defined in FingerCurveData.cpp (auto-generated)
extern const SavedFingerData g_fingerTipVals[6][kNumFingerVals];
extern const SavedFingerData g_fingerOuterVals[6][kNumFingerVals];
extern const SavedFingerData g_fingerInnerVals[6][kNumFingerVals];

} // namespace heisenberg
