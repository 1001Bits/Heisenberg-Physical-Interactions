#!/usr/bin/env python3
"""
Generate EmbeddedOffsets.h from JSON offset files.
"""

import json
import re
from pathlib import Path
from datetime import datetime

# Input directory with JSON offset files
OFFSETS_DIR = Path(r"C:\Development\higgs-master\Offsets")

# Output file
OUTPUT_FILE = Path(r"C:\Development\higgs-master\higgs_f4vr\src\EmbeddedOffsets.h")

def strip_category_and_components(name: str) -> str:
    """Strip [Category] prefix and {{{Components}}} suffix from item name."""
    result = name
    
    # Strip [Category] prefix (e.g., "[Scrap] Camera" -> "Camera")
    if result.startswith('['):
        close_bracket = result.find(']')
        if close_bracket != -1:
            result = result[close_bracket + 1:].lstrip()
    
    # Strip {{{Components}}} suffix (e.g., "Camera{{{Gear, Spring}}}" -> "Camera")
    brace_start = result.find('{{{')
    if brace_start != -1:
        result = result[:brace_start]
    
    return result.strip()

def load_all_offsets():
    """Load all JSON offset files."""
    items = {}  # Use dict to handle duplicates - later entries override earlier ones
    
    for json_file in sorted(OFFSETS_DIR.glob("*.json")):
        try:
            with open(json_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            for item_name, item_data in data.items():
                # Strip category prefix and component suffix to get clean name
                clean_name = strip_category_and_components(item_name)
                if clean_name:
                    items[clean_name] = item_data
        except Exception as e:
            print(f"Error loading {json_file}: {e}")
    
    # Return as sorted list of tuples
    return sorted(items.items())

def generate_cpp(items):
    """Generate C++ header content."""
    
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    lines = [
        '#pragma once',
        f'// Auto-generated from {len(items)} JSON offset files - DO NOT EDIT MANUALLY',
        f'// Generated: {timestamp}',
        '',
        '#include <array>',
        '#include <string_view>',
        '',
        'namespace EmbeddedOffsets {',
        '',
        'struct OffsetData {',
        '    std::string_view name;           // Item name (clean, no category prefix)',
        '    float posX, posY, posZ;          // Position offset',
        '    float rot[12];                   // 3x4 rotation matrix (row-major)',
        '    float scale;                     // Scale factor',
        '    float dimL, dimW, dimH;          // Dimensions (length, width, height)',
        '    float fingerDistance;            // Distance for finger positioning',
        '    float fingerCurls[5];            // thumb, index, middle, ring, pinky',
        '    std::string_view itemType;       // ALCH, MISC, WEAP, etc.',
        '    std::string_view formId;         // Hex form ID',
        '};',
        '',
        f'inline constexpr std::array<OffsetData, {len(items)}> kOffsets = {{{{',
    ]
    
    for item_name, data in items:
        pos = data.get("position", {})
        rot = data.get("rotation", [1,0,0,0, 0,1,0,0, 0,0,1,0])
        dims = data.get("dimensions", {})
        curls = data.get("fingerCurls", {})
        
        pos_x = pos.get("x", 0)
        pos_y = pos.get("y", 0)
        pos_z = pos.get("z", 0)
        
        scale = data.get("scale", 1.0)
        
        dim_l = dims.get("length", 0)
        dim_w = dims.get("width", 0)
        dim_h = dims.get("height", 0)
        
        finger_dist = data.get("fingerDistance", 0)
        
        thumb = curls.get("thumb", 0.5)
        index = curls.get("index", 0.5)
        middle = curls.get("middle", 0.5)
        ring = curls.get("ring", 0.5)
        pinky = curls.get("pinky", 0.5)
        
        item_type = data.get("itemType", "MISC")
        form_id = data.get("formId", "00000000")
        
        # Ensure rotation has 12 elements
        while len(rot) < 12:
            rot.append(0.0)
        
        lines.append('    {')
        lines.append(f'        "{item_name}",')
        lines.append(f'        {pos_x:.8f}f, {pos_y:.8f}f, {pos_z:.8f}f,')
        lines.append(f'        {{ {rot[0]:.8f}f, {rot[1]:.8f}f, {rot[2]:.8f}f, {rot[3]:.8f}f, {rot[4]:.8f}f, {rot[5]:.8f}f, {rot[6]:.8f}f, {rot[7]:.8f}f, {rot[8]:.8f}f, {rot[9]:.8f}f, {rot[10]:.8f}f, {rot[11]:.8f}f }},')
        lines.append(f'        {scale:.8f}f,')
        lines.append(f'        {dim_l:.8f}f, {dim_w:.8f}f, {dim_h:.8f}f,')
        lines.append(f'        {finger_dist:.8f}f,')
        lines.append(f'        {{ {thumb:.8f}f, {index:.8f}f, {middle:.8f}f, {ring:.8f}f, {pinky:.8f}f }},')
        lines.append(f'        "{item_type}",')
        lines.append(f'        "{form_id}"')
        lines.append('    },')
    
    lines.append('}};')
    lines.append('')
    lines.append(f'inline constexpr size_t kOffsetCount = {len(items)};')
    lines.append('')
    lines.append('} // namespace EmbeddedOffsets')
    lines.append('')
    
    return '\n'.join(lines)

def main():
    print(f"Loading offsets from: {OFFSETS_DIR}")
    items = load_all_offsets()
    print(f"Loaded {len(items)} items")
    
    cpp_content = generate_cpp(items)
    
    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write(cpp_content)
    
    print(f"Generated: {OUTPUT_FILE}")
    print(f"Total items: {len(items)}")

if __name__ == "__main__":
    main()
