"""
Extract HIGGS Skyrim finger curve lookup tables and generate C++ data file.

This script reads the original HIGGS Skyrim finger_curves.cpp and extracts
the g_fingerTipVals, g_fingerOuterVals, and g_fingerInnerVals arrays,
then generates a FingerCurveData.cpp file for F4VR.

Usage: python generate_finger_data.py
"""

import re
import os

HIGGS_SOURCE = r"c:\Development\higgs-master\src\finger_curves.cpp"
OUTPUT_FILE = r"c:\Development\higgs-master\higgs_f4vr\src\FingerCurveData.cpp"

def parse_array_data(content, array_name):
    """Parse a 2D SavedFingerData array from C++ source"""
    
    # Find the start of the array
    pattern = rf'{array_name}\s*\[\s*6\s*\]\s*\[\s*201\s*\]\s*=\s*\n\{{'
    match = re.search(pattern, content)
    if not match:
        print(f"WARNING: Could not find {array_name}")
        return None
    
    start_pos = match.end()
    
    # Find matching closing braces by counting
    brace_count = 1
    pos = start_pos
    while brace_count > 0 and pos < len(content):
        if content[pos] == '{':
            brace_count += 1
        elif content[pos] == '}':
            brace_count -= 1
        pos += 1
    
    array_content = content[start_pos:pos-1]  # -1 to not include the final }
    
    # Parse each of the 6 finger arrays
    finger_arrays = []
    
    # Split by major array sections (each finger starts with '{')
    finger_pattern = r'\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}'
    finger_matches = re.findall(finger_pattern, array_content)
    
    for finger_data in finger_matches:
        entries = []
        # Parse individual entries: { curveVal, angle, fingerLength }
        entry_pattern = r'\{\s*([0-9.eE+-]+)\s*,\s*([0-9.eE+-]+)\s*,\s*([0-9.eE+-]+)\s*\}'
        for entry_match in re.finditer(entry_pattern, finger_data):
            entries.append({
                'curveVal': entry_match.group(1),
                'angle': entry_match.group(2),
                'fingerLength': entry_match.group(3)
            })
        if len(entries) > 0:
            finger_arrays.append(entries)
    
    return finger_arrays


def generate_cpp_array(array_name, data, is_const=True):
    """Generate C++ array declaration from parsed data"""
    
    lines = []
    const_prefix = "const " if is_const else ""
    lines.append(f"{const_prefix}SavedFingerData {array_name}[6][kNumFingerVals] =")
    lines.append("{")
    
    for finger_idx, finger_data in enumerate(data):
        lines.append("{")
        for entry_idx, entry in enumerate(finger_data):
            comma = "," if entry_idx < len(finger_data) - 1 else ""
            lines.append(f"{{ {entry['curveVal']}, {entry['angle']}, {entry['fingerLength']} }}{comma}")
        comma = "," if finger_idx < len(data) - 1 else ""
        lines.append("}" + comma)
    
    lines.append("};")
    return "\n".join(lines)


def main():
    print(f"Reading HIGGS source from: {HIGGS_SOURCE}")
    
    with open(HIGGS_SOURCE, 'r') as f:
        content = f.read()
    
    print("Parsing g_fingerTipVals...")
    tip_data = parse_array_data(content, "g_fingerTipVals")
    if tip_data:
        print(f"  Found {len(tip_data)} finger arrays, {len(tip_data[0]) if tip_data else 0} entries each")
    
    print("Parsing g_fingerOuterVals...")
    outer_data = parse_array_data(content, "g_fingerOuterVals")
    if outer_data:
        print(f"  Found {len(outer_data)} finger arrays, {len(outer_data[0]) if outer_data else 0} entries each")
    
    print("Parsing g_fingerInnerVals...")
    inner_data = parse_array_data(content, "g_fingerInnerVals")
    if inner_data:
        print(f"  Found {len(inner_data)} finger arrays, {len(inner_data[0]) if inner_data else 0} entries each")
    
    if not tip_data or not outer_data or not inner_data:
        print("ERROR: Failed to parse one or more arrays")
        return 1
    
    # Generate output file
    print(f"Generating: {OUTPUT_FILE}")
    
    output = []
    output.append("""/**
 * FingerCurveData.cpp - HIGGS Skyrim finger curve lookup tables for F4VR
 * 
 * AUTO-GENERATED from HIGGS Skyrim finger_curves.cpp
 * DO NOT EDIT MANUALLY - Regenerate with generate_finger_data.py
 * 
 * These are calibrated 201-sample lookup tables for geometry-based finger curl.
 * Each array maps [finger_index][sample_index] to SavedFingerData {curveVal, angle, fingerLength}
 * 
 * Finger indices: 0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky, 5=ThumbAlt
 */

#include "FingerCurves.h"

namespace heisenberg {

""")
    
    output.append("// ============================================================================")
    output.append("// FINGER TIP CURVE DATA (201 samples per finger)")
    output.append("// Maps finger curl position to the tip contact point")
    output.append("// ============================================================================")
    output.append("")
    output.append(generate_cpp_array("g_fingerTipVals", tip_data))
    output.append("")
    output.append("")
    
    output.append("// ============================================================================")
    output.append("// FINGER OUTER CURVE DATA (201 samples per finger)")
    output.append("// Maps finger curl position to the outer knuckle contact point")
    output.append("// ============================================================================")
    output.append("")
    output.append(generate_cpp_array("g_fingerOuterVals", outer_data))
    output.append("")
    output.append("")
    
    output.append("// ============================================================================")
    output.append("// FINGER INNER CURVE DATA (201 samples per finger)")
    output.append("// Maps finger curl position to the inner palm contact point")
    output.append("// ============================================================================")
    output.append("")
    output.append(generate_cpp_array("g_fingerInnerVals", inner_data))
    output.append("")
    
    output.append("} // namespace heisenberg")
    output.append("")
    
    with open(OUTPUT_FILE, 'w') as f:
        f.write("\n".join(output))
    
    # Count lines
    total_entries = sum(len(f) for f in tip_data) + sum(len(f) for f in outer_data) + sum(len(f) for f in inner_data)
    print(f"Generated {len(output)} lines with {total_entries} total data entries")
    print("Done!")
    return 0


if __name__ == "__main__":
    exit(main())
