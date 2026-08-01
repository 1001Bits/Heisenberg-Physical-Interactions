#!/usr/bin/env python3
"""
Analyze 196 item offset JSON files and generate C++ archetype lookup table.
Groups items by normalized dimension ratios and computes average offsets.
"""

import json
import os
from pathlib import Path
from collections import defaultdict
import math

OFFSETS_DIR = Path(r"C:\Development\higgs-master\Offsets")

def load_all_offsets():
    """Load all JSON offset files and extract dimension/offset data."""
    items = []
    
    for json_file in OFFSETS_DIR.glob("*.json"):
        try:
            with open(json_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
                
            for item_name, item_data in data.items():
                dims = item_data.get("dimensions", {})
                pos = item_data.get("position", {})
                
                length = dims.get("length", 0)
                width = dims.get("width", 0)
                height = dims.get("height", 0)
                
                if length <= 0 or width <= 0 or height <= 0:
                    continue
                
                # Sort dimensions to normalize (largest, middle, smallest)
                sorted_dims = sorted([length, width, height], reverse=True)
                largest, middle, smallest = sorted_dims
                
                # Calculate normalized ratios (largest = 1.0)
                ratio1 = middle / largest if largest > 0.01 else 1.0
                ratio2 = smallest / largest if largest > 0.01 else 1.0
                
                items.append({
                    "name": item_name,
                    "dims": (length, width, height),
                    "sorted_dims": sorted_dims,
                    "largest": largest,
                    "ratio1": ratio1,  # middle/largest
                    "ratio2": ratio2,  # smallest/largest
                    "position": (pos.get("x", 0), pos.get("y", 0), pos.get("z", 0)),
                    "finger_distance": item_data.get("fingerDistance", 0),
                    "finger_curls": item_data.get("fingerCurls", {}),
                })
        except Exception as e:
            print(f"Error loading {json_file}: {e}")
    
    return items

def cluster_by_shape(items, ratio_threshold=0.15):
    """Group items by similar shape ratios."""
    clusters = []
    
    for item in items:
        # Find existing cluster with similar ratios
        found_cluster = None
        for cluster in clusters:
            avg_ratio1 = sum(i["ratio1"] for i in cluster) / len(cluster)
            avg_ratio2 = sum(i["ratio2"] for i in cluster) / len(cluster)
            
            if (abs(item["ratio1"] - avg_ratio1) < ratio_threshold and
                abs(item["ratio2"] - avg_ratio2) < ratio_threshold):
                found_cluster = cluster
                break
        
        if found_cluster:
            found_cluster.append(item)
        else:
            clusters.append([item])
    
    return clusters

def compute_archetype(cluster):
    """Compute average values for a cluster of similar items."""
    n = len(cluster)
    
    # Average ratios
    avg_ratio1 = sum(i["ratio1"] for i in cluster) / n
    avg_ratio2 = sum(i["ratio2"] for i in cluster) / n
    avg_largest = sum(i["largest"] for i in cluster) / n
    
    # Average position (normalized by largest dimension)
    avg_pos_x = sum(i["position"][0] for i in cluster) / n
    avg_pos_y = sum(i["position"][1] for i in cluster) / n
    avg_pos_z = sum(i["position"][2] for i in cluster) / n
    
    # Average finger distance
    avg_finger_dist = sum(i["finger_distance"] for i in cluster) / n
    
    # Average finger curls
    avg_curls = {"thumb": 0.5, "index": 0.5, "middle": 0.5, "ring": 0.5, "pinky": 0.5}
    curl_count = 0
    for item in cluster:
        curls = item.get("finger_curls", {})
        if curls:
            curl_count += 1
            for finger in avg_curls:
                avg_curls[finger] += curls.get(finger, 0.5)
    
    if curl_count > 0:
        for finger in avg_curls:
            avg_curls[finger] /= curl_count
    
    # Determine shape category name
    if avg_ratio1 > 0.9 and avg_ratio2 > 0.9:
        shape = "CUBE"
    elif avg_ratio1 > 0.7 and avg_ratio2 < 0.3:
        shape = "FLAT_WIDE"  # Like a book or clipboard
    elif avg_ratio1 < 0.5 and avg_ratio2 < 0.5:
        shape = "LONG_THIN"  # Like a bottle or stick
    elif avg_ratio1 > 0.8 and avg_ratio2 > 0.5:
        shape = "THICK_BOX"
    elif avg_ratio1 < 0.6 and avg_ratio2 > 0.6:
        shape = "CYLINDER"  # middle ~ smallest, both small relative to largest
    else:
        shape = "GENERAL"
    
    example_names = [i["name"] for i in cluster[:3]]
    
    return {
        "shape": shape,
        "ratio1": avg_ratio1,
        "ratio2": avg_ratio2,
        "avg_largest": avg_largest,
        "position": (avg_pos_x, avg_pos_y, avg_pos_z),
        "finger_distance": avg_finger_dist,
        "finger_curls": avg_curls,
        "count": n,
        "examples": example_names,
    }

def generate_cpp_code(archetypes):
    """Generate C++ code for the archetype lookup table."""
    
    total_items = sum(a["count"] for a in archetypes)
    num_archetypes = len(archetypes)
    
    code = f'''// Auto-generated from 196 item offset JSONs
// Shape archetypes based on normalized dimension ratios

struct ShapeArchetype {{
    float ratio1;           // middle/largest dimension ratio
    float ratio2;           // smallest/largest dimension ratio
    float avgLargestDim;    // Average largest dimension for scaling
    RE::NiPoint3 position;  // Average position offset
    float fingerDistance;   // Average finger distance
    float thumbCurl;
    float indexCurl;
    float middleCurl;
    float ringCurl;
    float pinkyCurl;
    const char* shapeName;  // For debugging
}};

// Archetypes derived from {total_items} items across {num_archetypes} shape clusters
static const ShapeArchetype SHAPE_ARCHETYPES[] = {{
'''
    
    for i, arch in enumerate(archetypes):
        pos = arch["position"]
        curls = arch["finger_curls"]
        code += f'''    // {arch["shape"]}: {arch["count"]} items (e.g., {", ".join(arch["examples"][:2])})
    {{
        {arch["ratio1"]:.4f}f, {arch["ratio2"]:.4f}f,  // ratio1, ratio2
        {arch["avg_largest"]:.2f}f,                    // avgLargestDim
        RE::NiPoint3({pos[0]:.4f}f, {pos[1]:.4f}f, {pos[2]:.4f}f),  // position
        {arch["finger_distance"]:.4f}f,                // fingerDistance
        {curls["thumb"]:.4f}f, {curls["index"]:.4f}f, {curls["middle"]:.4f}f,  // thumb, index, middle
        {curls["ring"]:.4f}f, {curls["pinky"]:.4f}f,   // ring, pinky
        "{arch["shape"]}"
    }},
'''
    
    code += '''};

static constexpr size_t NUM_ARCHETYPES = sizeof(SHAPE_ARCHETYPES) / sizeof(SHAPE_ARCHETYPES[0]);

// Find the best matching archetype for given dimension ratios
inline const ShapeArchetype& FindBestArchetype(float ratio1, float ratio2) {
    float bestScore = 999999.0f;
    size_t bestIndex = 0;
    
    for (size_t i = 0; i < NUM_ARCHETYPES; ++i) {
        const auto& arch = SHAPE_ARCHETYPES[i];
        float score = std::abs(ratio1 - arch.ratio1) + std::abs(ratio2 - arch.ratio2);
        if (score < bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }
    
    return SHAPE_ARCHETYPES[bestIndex];
}

// Calculate offset from dimensions using archetype matching
inline ItemOffset CalculateOffsetFromDimensions(float length, float width, float height) {
    ItemOffset offset;
    
    // Sort dimensions
    float dims[3] = {length, width, height};
    std::sort(dims, dims + 3, std::greater<float>());
    float largest = dims[0];
    float middle = dims[1];
    float smallest = dims[2];
    
    // Calculate normalized ratios
    float ratio1 = (largest > 0.01f) ? middle / largest : 1.0f;
    float ratio2 = (largest > 0.01f) ? smallest / largest : 1.0f;
    
    // Find best matching archetype
    const auto& arch = FindBestArchetype(ratio1, ratio2);
    
    // Scale position based on size difference from archetype average
    float sizeScale = (arch.avgLargestDim > 0.01f) ? largest / arch.avgLargestDim : 1.0f;
    
    // Apply archetype position with size scaling
    // Y (forward) and Z (up) scale with size, X (left/right) usually stays centered
    offset.position.x = arch.position.x;
    offset.position.y = arch.position.y * sizeScale;
    offset.position.z = arch.position.z * sizeScale;
    
    // Use archetype finger values
    offset.fingerDistance = arch.fingerDistance * sizeScale;
    offset.thumbCurl = arch.thumbCurl;
    offset.indexCurl = arch.indexCurl;
    offset.middleCurl = arch.middleCurl;
    offset.ringCurl = arch.ringCurl;
    offset.pinkyCurl = arch.pinkyCurl;
    offset.hasFingerCurls = true;
    
    // Store dimensions
    offset.length = length;
    offset.width = width;
    offset.height = height;
    
    // Identity rotation (will be set based on grab orientation)
    offset.rotation.MakeIdentity();
    offset.scale = 1.0f;
    
    return offset;
}
'''
    
    return code

def main():
    print("Loading offset JSONs...")
    items = load_all_offsets()
    print(f"Loaded {len(items)} items with valid dimensions")
    
    print("\nClustering by shape ratios...")
    clusters = cluster_by_shape(items, ratio_threshold=0.12)
    print(f"Found {len(clusters)} shape clusters")
    
    # Compute archetypes and sort by cluster size
    archetypes = [compute_archetype(c) for c in clusters]
    archetypes.sort(key=lambda x: x["count"], reverse=True)
    
    print("\nShape Archetypes:")
    print("-" * 80)
    for arch in archetypes:
        print(f"{arch['shape']:12} ratio=({arch['ratio1']:.2f}, {arch['ratio2']:.2f}) "
              f"pos=({arch['position'][0]:6.2f}, {arch['position'][1]:6.2f}, {arch['position'][2]:6.2f}) "
              f"count={arch['count']:3} ex: {arch['examples'][0][:30]}")
    
    print("\nGenerating C++ code...")
    cpp_code = generate_cpp_code(archetypes)
    
    output_file = Path(__file__).parent.parent / "src" / "ShapeArchetypes.h"
    with open(output_file, 'w') as f:
        f.write("#pragma once\n\n")
        f.write('#include "ItemOffsets.h"\n')
        f.write("#include <algorithm>\n")
        f.write("#include <functional>\n")
        f.write("#include <cmath>\n\n")
        f.write("namespace heisenberg {\n\n")
        f.write(cpp_code)
        f.write("\n} // namespace heisenberg\n")
    
    print(f"\nGenerated: {output_file}")

if __name__ == "__main__":
    main()
