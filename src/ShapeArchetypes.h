#pragma once

#include "ItemOffsets.h"
#include <algorithm>
#include <functional>
#include <cmath>

namespace heisenberg {

// Auto-generated from 196 item offset JSONs
// Shape archetypes based on normalized dimension ratios

struct ShapeArchetype {
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
};

// Archetypes derived from 169 items across 23 shape clusters
static const ShapeArchetype SHAPE_ARCHETYPES[] = {
    // LONG_THIN: 22 items (e.g., [Aid] Rad-X, [Bottle] Empty Milk Bottle{{{Glass}}})
    {
        0.4172f, 0.3996f,  // ratio1, ratio2
        19.59f,                    // avgLargestDim
        RE::NiPoint3(4.0088f, -12.0017f, -2.5760f),  // position
        15.4750f,                // fingerDistance
        0.8131f, 0.8131f, 0.8131f,  // thumb, index, middle
        0.8131f, 0.8131f,   // ring, pinky
        "LONG_THIN"
    },
    // LONG_THIN: 20 items (e.g., Weed Whacker, [Chem] Overdrive)
    {
        0.3619f, 0.2234f,  // ratio1, ratio2
        27.05f,                    // avgLargestDim
        RE::NiPoint3(4.0725f, -0.1493f, -5.4691f),  // position
        9.3032f,                // fingerDistance
        0.8100f, 0.8100f, 0.8100f,  // thumb, index, middle
        0.8100f, 0.8100f,   // ring, pinky
        "LONG_THIN"
    },
    // GENERAL: 17 items (e.g., Traffic Cone, Wood Box)
    {
        0.6843f, 0.6740f,  // ratio1, ratio2
        20.94f,                    // avgLargestDim
        RE::NiPoint3(10.5168f, -5.7484f, -6.0626f),  // position
        16.0636f,                // fingerDistance
        0.8849f, 0.8849f, 0.8849f,  // thumb, index, middle
        0.8849f, 0.8849f,   // ring, pinky
        "GENERAL"
    },
    // GENERAL: 17 items (e.g., [Aid] RadAway, [Chem] Jet Fuel)
    {
        0.5616f, 0.2075f,  // ratio1, ratio2
        23.94f,                    // avgLargestDim
        RE::NiPoint3(3.1466f, 0.2890f, -8.6506f),  // position
        9.3050f,                // fingerDistance
        0.7301f, 0.7301f, 0.7301f,  // thumb, index, middle
        0.7301f, 0.7301f,   // ring, pinky
        "GENERAL"
    },
    // LONG_THIN: 15 items (e.g., [Aid] Mysterious Serum, [Beer] Gwinnett Brew)
    {
        0.2213f, 0.1968f,  // ratio1, ratio2
        22.53f,                    // avgLargestDim
        RE::NiPoint3(3.5978f, -7.0637f, -5.6661f),  // position
        10.6908f,                // fingerDistance
        0.7370f, 0.7370f, 0.7370f,  // thumb, index, middle
        0.7370f, 0.7370f,   // ring, pinky
        "LONG_THIN"
    },
    // GENERAL: 13 items (e.g., [Collectible] Nuka-Cola Lunchbox, [Collectible] Vault-Tec Lunchbox)
    {
        0.8862f, 0.4645f,  // ratio1, ratio2
        17.38f,                    // avgLargestDim
        RE::NiPoint3(4.8278f, -4.0413f, -9.2902f),  // position
        11.2661f,                // fingerDistance
        0.7231f, 0.7231f, 0.7231f,  // thumb, index, middle
        0.7231f, 0.7231f,   // ring, pinky
        "GENERAL"
    },
    // LONG_THIN: 10 items (e.g., [Chem] Med-X, [Scrap] Bloodbug Proboscis{{{Acid}}})
    {
        0.1147f, 0.0483f,  // ratio1, ratio2
        41.00f,                    // avgLargestDim
        RE::NiPoint3(0.4041f, -4.0105f, -2.3371f),  // position
        6.3034f,                // fingerDistance
        0.3900f, 0.3900f, 0.3900f,  // thumb, index, middle
        0.3900f, 0.3900f,   // ring, pinky
        "LONG_THIN"
    },
    // GENERAL: 10 items (e.g., [RadFood] Cram, [RadFood] Silt Bean)
    {
        0.6842f, 0.4529f,  // ratio1, ratio2
        21.00f,                    // avgLargestDim
        RE::NiPoint3(9.5905f, -4.6192f, -5.6453f),  // position
        12.1639f,                // fingerDistance
        0.8630f, 0.8630f, 0.8630f,  // thumb, index, middle
        0.8630f, 0.8630f,   // ring, pinky
        "GENERAL"
    },
    // FLAT_WIDE: 9 items (e.g., [RadFood] Potted Meat, [RadFood] Sugar Bombs)
    {
        0.7767f, 0.1836f,  // ratio1, ratio2
        17.11f,                    // avgLargestDim
        RE::NiPoint3(8.8167f, -2.5360f, -5.0238f),  // position
        7.1646f,                // fingerDistance
        0.7938f, 0.7938f, 0.7938f,  // thumb, index, middle
        0.7938f, 0.7938f,   // ring, pinky
        "FLAT_WIDE"
    },
    // THICK_BOX: 8 items (e.g., [Collectible] Mr. Handy Model, [RadFood] Bloodbug Meat)
    {
        0.9609f, 0.7711f,  // ratio1, ratio2
        18.00f,                    // avgLargestDim
        RE::NiPoint3(10.9145f, -1.6653f, -3.6530f),  // position
        10.5482f,                // fingerDistance
        0.9736f, 0.9736f, 0.9736f,  // thumb, index, middle
        0.9736f, 0.9736f,   // ring, pinky
        "THICK_BOX"
    },
    // FLAT_WIDE: 5 items (e.g., Tire, [Resource] Crystal)
    {
        0.9875f, 0.2833f,  // ratio1, ratio2
        19.60f,                    // avgLargestDim
        RE::NiPoint3(10.1798f, -2.2724f, -7.7458f),  // position
        8.4016f,                // fingerDistance
        0.8267f, 0.8267f, 0.8267f,  // thumb, index, middle
        0.8267f, 0.8267f,   // ring, pinky
        "FLAT_WIDE"
    },
    // THICK_BOX: 5 items (e.g., Tricycle, [RadFood] Melon)
    {
        0.8586f, 0.7994f,  // ratio1, ratio2
        26.20f,                    // avgLargestDim
        RE::NiPoint3(5.4869f, -9.0834f, -12.6120f),  // position
        17.2541f,                // fingerDistance
        0.6694f, 0.6694f, 0.6694f,  // thumb, index, middle
        0.6694f, 0.6694f,   // ring, pinky
        "THICK_BOX"
    },
    // CUBE: 4 items (e.g., [Scrap] Baseball{{{Cork, Leather}}}, [Scrap] Coffee Tin{{{Aluminum}}})
    {
        0.9833f, 0.9833f,  // ratio1, ratio2
        13.25f,                    // avgLargestDim
        RE::NiPoint3(9.2450f, -3.3007f, -2.1804f),  // position
        10.9159f,                // fingerDistance
        1.0639f, 1.0639f, 1.0639f,  // thumb, index, middle
        1.0639f, 1.0639f,   // ring, pinky
        "CUBE"
    },
    // LONG_THIN: 3 items (e.g., [Scrap] Cutting Board{{{Wood}}}, [Scrap] Paintbrush{{{Wood, Cloth}}})
    {
        0.4060f, 0.0831f,  // ratio1, ratio2
        28.33f,                    // avgLargestDim
        RE::NiPoint3(0.9824f, 4.5739f, 2.1557f),  // position
        9.6125f,                // fingerDistance
        0.5815f, 0.5815f, 0.5815f,  // thumb, index, middle
        0.5815f, 0.5815f,   // ring, pinky
        "LONG_THIN"
    },
    // GENERAL: 2 items (e.g., [Scrap] Battered Clipboard{{{Spring, Wood}}}, [Scrap] TV Dinner Tray{{{Aluminum}}})
    {
        0.6419f, 0.0364f,  // ratio1, ratio2
        28.50f,                    // avgLargestDim
        RE::NiPoint3(11.3066f, 3.8644f, -5.0382f),  // position
        9.1333f,                // fingerDistance
        0.9944f, 0.9944f, 0.9944f,  // thumb, index, middle
        0.9944f, 0.9944f,   // ring, pinky
        "GENERAL"
    },
    // FLAT_WIDE: 2 items (e.g., [Scrap] Hubcap{{{Screw, Aluminum}}}, [Scrap] Life Preserver{{{Plastic, Spring}}})
    {
        1.0000f, 0.1139f,  // ratio1, ratio2
        43.00f,                    // avgLargestDim
        RE::NiPoint3(14.8122f, 1.0876f, -16.6214f),  // position
        16.8523f,                // fingerDistance
        1.2056f, 1.2056f, 1.2056f,  // thumb, index, middle
        1.2056f, 1.2056f,   // ring, pinky
        "FLAT_WIDE"
    },
    // LONG_THIN: 1 items (e.g., [Scrap] Adjustable Wrench{{{Gear, Steel}}})
    {
        0.2162f, 0.0270f,  // ratio1, ratio2
        37.00f,                    // avgLargestDim
        RE::NiPoint3(1.7348f, -2.8908f, -1.0234f),  // position
        0.8549f,                // fingerDistance
        0.8778f, 0.8778f, 0.8778f,  // thumb, index, middle
        0.8778f, 0.8778f,   // ring, pinky
        "LONG_THIN"
    },
    // GENERAL: 1 items (e.g., [Scrap] Handcuffs{{{Screw, Spring, Steel}}})
    {
        0.5455f, 0.0455f,  // ratio1, ratio2
        22.00f,                    // avgLargestDim
        RE::NiPoint3(13.0231f, -8.2035f, -5.2880f),  // position
        11.3032f,                // fingerDistance
        1.1000f, 1.1000f, 1.1000f,  // thumb, index, middle
        1.1000f, 1.1000f,   // ring, pinky
        "GENERAL"
    },
    // THICK_BOX: 1 items (e.g., [Scrap] Hot Plate{{{Circuitry, Copper, Screw}}})
    {
        1.0000f, 0.5625f,  // ratio1, ratio2
        16.00f,                    // avgLargestDim
        RE::NiPoint3(7.7046f, -8.0739f, -8.4985f),  // position
        11.4740f,                // fingerDistance
        1.5000f, 1.5000f, 1.5000f,  // thumb, index, middle
        1.5000f, 1.5000f,   // ring, pinky
        "THICK_BOX"
    },
    // GENERAL: 1 items (e.g., [Scrap] Jangles the Moon Monkey{{{Cloth, Plastic, Fiberglass}}})
    {
        0.5556f, 0.4028f,  // ratio1, ratio2
        72.00f,                    // avgLargestDim
        RE::NiPoint3(-2.7849f, -30.3562f, 1.8900f),  // position
        33.2512f,                // fingerDistance
        1.5000f, 1.5000f, 1.5000f,  // thumb, index, middle
        1.5000f, 1.5000f,   // ring, pinky
        "GENERAL"
    },
    // THICK_BOX: 1 items (e.g., [Scrap] Kitchen Scale{{{Steel, Spring}}})
    {
        0.8333f, 0.6667f,  // ratio1, ratio2
        24.00f,                    // avgLargestDim
        RE::NiPoint3(13.7589f, -8.4954f, -9.4062f),  // position
        16.0507f,                // fingerDistance
        1.5000f, 1.5000f, 1.5000f,  // thumb, index, middle
        1.5000f, 1.5000f,   // ring, pinky
        "THICK_BOX"
    },
    // GENERAL: 1 items (e.g., [Scrap] Small Picture Frame{{{Wood}}})
    {
        0.8182f, 0.3182f,  // ratio1, ratio2
        22.00f,                    // avgLargestDim
        RE::NiPoint3(11.6336f, 0.8109f, -5.9264f),  // position
        8.3961f,                // fingerDistance
        1.1889f, 1.1889f, 1.1889f,  // thumb, index, middle
        1.1889f, 1.1889f,   // ring, pinky
        "GENERAL"
    },
    // GENERAL: 1 items (e.g., [Scrap] Souvenir Drinking Glass{{{Glass}}})
    {
        0.5455f, 0.5455f,  // ratio1, ratio2
        11.00f,                    // avgLargestDim
        RE::NiPoint3(3.6163f, -9.2723f, -3.0507f),  // position
        11.5504f,                // fingerDistance
        1.5000f, 1.5000f, 1.5000f,  // thumb, index, middle
        1.5000f, 1.5000f,   // ring, pinky
        "GENERAL"
    },
};

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
// Position is calculated directly from item size, finger curls from archetype
inline ItemOffset CalculateOffsetFromDimensions(float length, float width, float height) {
    ItemOffset offset;
    
    // Sort dimensions to understand shape
    float dims[3] = {length, width, height};
    std::sort(dims, dims + 3, std::greater<float>());
    float largest = dims[0];
    float middle = dims[1];
    float smallest = dims[2];
    
    // Calculate normalized ratios for shape classification
    float ratio1 = (largest > 0.01f) ? middle / largest : 1.0f;
    float ratio2 = (largest > 0.01f) ? smallest / largest : 1.0f;
    
    // Find best matching archetype (for finger curls)
    const auto& arch = FindBestArchetype(ratio1, ratio2);
    
    // CALCULATE POSITION DIRECTLY FROM ITEM SIZE
    // Based on analysis of 196 configured items:
    // - Y (depth): Items are held at Y ≈ -0.9 * largest dimension
    //   This places the center of the item forward in the palm
    // - X (side): Slight offset based on width, typically 3-7 units right
    // - Z (height): Slight down offset, scales with size
    //
    // Coordinate system: X=right, Y=negative toward fingers, Z=up
    
    // Y: Push item forward into palm. Larger items need more forward offset.
    // From data: Nuka bottles (largest=21) have Y≈-19, so Y ≈ -0.9 * largest
    offset.position.y = -0.9f * largest;
    
    // X: Slight right offset. Larger items offset more.
    // From data: Most items have X between 3-10, averaging around 5 + size factor
    offset.position.x = 4.0f + (middle * 0.15f);
    
    // Z: Slight downward offset. Scales with item thickness.
    // From data: Z is typically negative, around -2 to -6
    offset.position.z = -2.0f - (smallest * 0.15f);
    
    // Use archetype finger curl values
    offset.fingerDistance = arch.fingerDistance;
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
    
    spdlog::info("[Archetype] '{}' matched for dims ({:.1f}, {:.1f}, {:.1f}) ratios ({:.3f}, {:.3f}) -> offset ({:.2f}, {:.2f}, {:.2f}) sizeFactor={:.2f}",
                  arch.shapeName, length, width, height, ratio1, ratio2,
                  offset.position.x, offset.position.y, offset.position.z, sizeFactor);
    
    return offset;
}

} // namespace heisenberg
