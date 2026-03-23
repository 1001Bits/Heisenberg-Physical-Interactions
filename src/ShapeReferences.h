#pragma once

#include "ItemOffsets.h"
#include <algorithm>
#include <cmath>

namespace heisenberg {

// Auto-generated reference offset table
// Each entry represents a shape class with one reference item
// At runtime: find best ratio match, scale position by (targetLargest / refLargest)

struct ShapeReference {
    float ratio1;        // middle/largest
    float ratio2;        // smallest/largest
    float refLargest;    // Reference item's largest dimension
    float posX, posY, posZ;  // Reference position
    float fingerDistance;
    float thumbCurl, indexCurl, middleCurl, ringCurl, pinkyCurl;
};

// 56 shape references from 195 items
static const ShapeReference SHAPE_REFERENCES[] = {
    // .303 Round (3 items)
    {0.0000f, 0.0000f, 5.00f, 1.2118f, -2.0978f, -1.5664f, 3.3385f, 0.6889f, 0.6889f, 0.6889f, 0.6889f, 0.6889f},
    // [Scrap] Silver Table Knife{{{Silver}}} (5 items)
    {0.0952f, 0.0000f, 21.00f, 0.9229f, -2.2182f, -1.0454f, 1.7870f, 0.1556f, 0.1556f, 0.1556f, 0.1556f, 0.1556f},
    // [Scrap] Bloodbug Proboscis{{{Acid}}} (4 items)
    {0.1333f, 0.0667f, 30.00f, -0.7864f, -9.6220f, -2.6779f, 6.6971f, 0.4000f, 0.4000f, 0.4000f, 0.4000f, 0.4000f},
    // [Scrap] Adjustable Wrench{{{Gear, Steel}}} (4 items)
    {0.2162f, 0.0270f, 37.00f, 1.7348f, -2.8908f, -1.0234f, 0.8549f, 0.3778f, 0.3778f, 0.3778f, 0.3778f, 0.3778f},
    // [Chem] Daddy-O (2 items)
    {0.2000f, 0.1000f, 20.00f, 1.7562f, -1.2033f, -3.7787f, 2.3195f, 0.7778f, 0.7778f, 0.7778f, 0.7778f, 0.7778f},
    // [Beer] Gwinnett Brew (11 items)
    {0.2000f, 0.2000f, 20.00f, 0.4070f, -19.0717f, -9.0524f, 17.5729f, 0.4889f, 0.4889f, 0.4889f, 0.4889f, 0.4889f},
    // [Scrap] Scissors{{{Plastic, Steel}}} (1 items)
    {0.3333f, 0.0000f, 15.00f, 0.6728f, -4.3531f, -2.4677f, 1.5894f, 0.2667f, 0.2667f, 0.2667f, 0.2667f, 0.2667f},
    // [Scrap] Connecting Rod{{{Steel}}} (1 items)
    {0.3077f, 0.1154f, 26.00f, -0.2808f, 5.0073f, 0.1736f, 8.7106f, 0.6889f, 0.6889f, 0.6889f, 0.6889f, 0.6889f},
    // [Scrap] Extinguisher{{{Rubber, Steel, Asbestos}}} (1 items)
    {0.3400f, 0.2400f, 50.00f, -3.2823f, -22.1537f, -19.7452f, 26.6040f, 0.3556f, 0.3556f, 0.3556f, 0.3556f, 0.3556f},
    // [Drink] Vim Quartz (8 items)
    {0.3000f, 0.3000f, 20.00f, 2.7391f, -9.7547f, -2.1693f, 11.6885f, 0.9556f, 0.9556f, 0.9556f, 0.9556f, 0.9556f},
    // [Scrap] Paintbrush{{{Wood, Cloth}}} (4 items)
    {0.3913f, 0.0870f, 23.00f, 0.4385f, 3.3590f, 0.1559f, 7.3206f, 0.3111f, 0.3111f, 0.3111f, 0.3111f, 0.3111f},
    // [Scrap] Pack of Cigarettes{{{Plastic, Cloth, Asbestos}}} (6 items)
    {0.4000f, 0.2000f, 10.00f, 3.0614f, -4.8899f, -3.9700f, 5.5088f, 0.8667f, 0.8667f, 0.8667f, 0.8667f, 0.8667f},
    // [Scrap] Pepper Mill{{{Screw, Plastic}}} (3 items)
    {0.4286f, 0.2857f, 14.00f, 0.3800f, -9.9299f, -4.3306f, 10.0405f, 0.7889f, 0.7889f, 0.7889f, 0.7889f, 0.7889f},
    // [Drink] Ice Cold Nuka-Cola Victory (15 items)
    {0.4000f, 0.4000f, 20.00f, 3.2411f, -18.2733f, -5.2584f, 19.3026f, 0.9333f, 0.9333f, 0.9333f, 0.9333f, 0.9333f},
    // [Scrap] Handcuffs{{{Screw, Spring, Steel}}} (1 items)
    {0.5455f, 0.0455f, 22.00f, 13.0231f, -8.2035f, -5.2880f, 11.3032f, 0.6000f, 0.6000f, 0.6000f, 0.6000f, 0.6000f},
    // [Stimpak] Stimpak (1 items)
    {0.4583f, 0.0833f, 24.00f, -0.2185f, -2.0121f, -1.7998f, 3.0417f, 0.4444f, 0.4444f, 0.4444f, 0.4444f, 0.4444f},
    // [Chem] Mentats (8 items)
    {0.5000f, 0.1875f, 16.00f, 5.8566f, -3.0641f, -8.4661f, 6.4613f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Scrap] Small Sauce Pan{{{Steel}}} (1 items)
    {0.5000f, 0.3000f, 20.00f, 1.2264f, 6.8720f, -0.7795f, 10.5007f, 0.1667f, 0.1667f, 0.1667f, 0.1667f, 0.1667f},
    // [Scrap] Synth Component{{{Plastic}}} (2 items)
    {0.5000f, 0.3750f, 8.00f, 3.7704f, -3.7614f, -0.8513f, 5.1703f, 0.8000f, 0.8000f, 0.8000f, 0.8000f, 0.8000f},
    // [Aid] Rad-X (5 items)
    {0.5000f, 0.5000f, 16.00f, 5.8277f, -7.3621f, -3.5431f, 10.2775f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Scrap] Charge Card{{{Plastic}}} (2 items)
    {0.6000f, 0.0000f, 10.00f, 8.0542f, -2.2038f, -2.0462f, 4.3710f, 0.6222f, 0.6222f, 0.6222f, 0.6222f, 0.6222f},
    // [Scrap] Military-Grade Circuit Board{{{Circuitry}}} (7 items)
    {0.5714f, 0.2000f, 35.00f, 10.0759f, 7.2711f, -4.6294f, 10.5657f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Collectible] Protectron Model (2 items)
    {0.5625f, 0.3125f, 16.00f, 1.1230f, -8.7245f, -2.6977f, 9.2229f, 0.8222f, 0.8222f, 0.8222f, 0.8222f, 0.8222f},
    // [Scrap] Soap{{{Oil}}} (3 items)
    {0.6000f, 0.4000f, 10.00f, 3.9058f, -2.0041f, -3.9272f, 3.3468f, 0.8889f, 0.8889f, 0.8889f, 0.8889f, 0.8889f},
    // [Scrap] Broken Light Bulb{{{Copper}}} (4 items)
    {0.6000f, 0.6000f, 10.00f, 6.9723f, 0.8300f, -3.2960f, 3.8656f, 0.8000f, 0.8000f, 0.8000f, 0.8000f, 0.8000f},
    // [PerkMag] Grognak the Barbarian (8 items)
    {0.7143f, 0.0000f, 21.00f, 8.2463f, 0.0385f, -6.8129f, 5.4145f, 0.7111f, 0.7111f, 0.7111f, 0.7111f, 0.7111f},
    // [Scrap] Oven Mitt{{{Asbestos, Cloth}}} (1 items)
    {0.6667f, 0.1429f, 21.00f, 3.1506f, -2.6712f, -7.1277f, 3.3121f, 0.8667f, 0.8667f, 0.8667f, 0.8667f, 0.8667f},
    // [RadFood] Sugar Bombs (1 items)
    {0.7273f, 0.1818f, 22.00f, 9.0336f, -6.8586f, -8.0602f, 10.0763f, 0.9111f, 0.9111f, 0.9111f, 0.9111f, 0.9111f},
    // [Scrap] Turpentine{{{Steel, Antiseptic}}} (4 items)
    {0.7059f, 0.3529f, 17.00f, 6.8885f, -8.3655f, -5.2629f, 11.2639f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Scrap] Bread Box{{{Plastic}}} (3 items)
    {0.6842f, 0.4737f, 38.00f, 20.9698f, -4.0568f, -2.5419f, 20.6231f, 0.5000f, 0.5000f, 0.5000f, 0.5000f, 0.5000f},
    // Wood Box (11 items)
    {0.6944f, 0.6667f, 36.00f, 24.4635f, -3.4065f, -16.1916f, 24.2131f, 0.6889f, 0.6889f, 0.6889f, 0.6889f, 0.6889f},
    // [PerkMag] Guns and Bullets (6 items)
    {0.7895f, 0.0000f, 19.00f, 7.5758f, 2.2208f, -6.2002f, 5.5889f, 0.7111f, 0.7111f, 0.7111f, 0.7111f, 0.7111f},
    // [Scrap] Silver Locket{{{Silver}}} (4 items)
    {0.8000f, 0.1000f, 10.00f, 7.5872f, -0.9380f, -3.7904f, 4.3795f, 0.6000f, 0.6000f, 0.6000f, 0.6000f, 0.6000f},
    // [Resource] Glass (2 items)
    {0.8077f, 0.1923f, 26.00f, 8.6320f, -0.4547f, -3.0563f, 5.0051f, 0.7556f, 0.7556f, 0.7556f, 0.7556f, 0.7556f},
    // [Resource] Ceramic (4 items)
    {0.8000f, 0.3333f, 15.00f, 10.9295f, -1.3859f, -3.5504f, 7.3806f, 0.8000f, 0.8000f, 0.8000f, 0.8000f, 0.8000f},
    // [Scrap] Cutting Fluid{{{Oil, Steel}}} (3 items)
    {0.8333f, 0.4444f, 18.00f, -0.9247f, -6.0547f, -13.3733f, 12.6770f, 0.3778f, 0.3778f, 0.3778f, 0.3778f, 0.3778f},
    // [Scrap] Toaster{{{Spring, Steel}}} (3 items)
    {0.8000f, 0.5000f, 20.00f, 14.6250f, -7.7299f, 1.4164f, 16.9018f, 0.9333f, 0.9333f, 0.9333f, 0.9333f, 0.9333f},
    // [Scrap] ProSnap Camera{{{Gear, Spring, Crystal}}} (2 items)
    {0.7826f, 0.6957f, 23.00f, 14.9788f, -0.3087f, -4.2986f, 11.9211f, 0.5778f, 0.5778f, 0.5778f, 0.5778f, 0.5778f},
    // [Scrap] Tea Kettle{{{Steel}}} (3 items)
    {0.8000f, 0.8000f, 20.00f, 7.3550f, -1.3252f, -16.7218f, 17.1372f, 0.3111f, 0.3111f, 0.3111f, 0.3111f, 0.3111f},
    // [Scrap] Boston Bugle{{{Cloth}}} (2 items)
    {0.8571f, 0.0000f, 28.00f, 7.2125f, 8.3362f, -11.1961f, 10.5729f, 0.8667f, 0.8667f, 0.8667f, 0.8667f, 0.8667f},
    // [Resource] Crystal (1 items)
    {0.9375f, 0.2500f, 16.00f, 10.7171f, -0.5384f, -5.5415f, 6.9895f, 0.7556f, 0.7556f, 0.7556f, 0.7556f, 0.7556f},
    // [Collectible] Nuka-Cola Lunchbox (2 items)
    {0.9286f, 0.4286f, 14.00f, 0.2563f, -0.3814f, -17.5822f, 16.6043f, 0.4444f, 0.4444f, 0.4444f, 0.4444f, 0.4444f},
    // [Scrap] Teddy Bear{{{Leather, Cloth}}} (1 items)
    {0.8846f, 0.4615f, 26.00f, 3.7144f, -1.0308f, -6.3782f, 3.3280f, 0.5000f, 0.5000f, 0.5000f, 0.5000f, 0.5000f},
    // [Scrap] Toy Alien{{{Plastic, Rubber}}} (1 items)
    {0.9000f, 0.5500f, 20.00f, 6.0729f, -12.5688f, -2.9701f, 15.2474f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [RadFood] Wild Mutfruit (1 items)
    {0.9375f, 0.6875f, 16.00f, 4.7829f, -4.2620f, -4.3408f, 7.0772f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [RadFood] Melon (3 items)
    {0.8611f, 0.8333f, 36.00f, 8.1066f, -24.0868f, -30.2552f, 36.3324f, 0.2667f, 0.2667f, 0.2667f, 0.2667f, 0.2667f},
    // [Collectible] Mr. Handy Model (2 items)
    {0.9333f, 0.8667f, 15.00f, 6.1775f, -1.9692f, -2.7438f, 2.7798f, 0.7333f, 0.7333f, 0.7333f, 0.7333f, 0.7333f},
    // [Currency] Book Return Token (4 items)
    {1.0000f, 0.0000f, 4.00f, 4.4179f, -0.9194f, -2.7252f, 1.2286f, 0.6667f, 0.6667f, 0.6667f, 0.6667f, 0.6667f},
    // [Scrap] Hubcap{{{Screw, Aluminum}}} (2 items)
    {1.0000f, 0.1071f, 28.00f, 10.9183f, 1.6010f, -8.3748f, 8.6655f, 0.9111f, 0.9111f, 0.9111f, 0.9111f, 0.9111f},
    // [Scrap] Sauce Pan Lid{{{Steel}}} (1 items)
    {1.0000f, 0.2143f, 14.00f, 5.7853f, -1.2637f, -2.3435f, 2.4011f, 0.6111f, 0.6111f, 0.6111f, 0.6111f, 0.6111f},
    // Tire (3 items)
    {1.0000f, 0.2857f, 56.00f, 23.6568f, -3.5923f, -19.7546f, 26.1894f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Scrap] Duct Tape{{{Adhesive, Cloth}}} (3 items)
    {1.0000f, 0.5000f, 8.00f, 5.1061f, -1.3076f, -5.9124f, 4.5698f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Scrap] Hot Plate{{{Circuitry, Copper, Screw}}} (1 items)
    {1.0000f, 0.5625f, 16.00f, 7.7046f, -8.0739f, -8.4985f, 11.4740f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Resource] Concrete (1 items)
    {0.9565f, 0.6957f, 23.00f, 12.5599f, 7.5241f, -1.1578f, 13.7486f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [RadFood] Mirelurk Meat (5 items)
    {1.0000f, 0.7857f, 14.00f, 9.3227f, -6.5606f, -5.4639f, 11.2892f, 1.0000f, 1.0000f, 1.0000f, 1.0000f, 1.0000f},
    // [Scrap] Baseball{{{Cork, Leather}}} (3 items)
    {1.0000f, 1.0000f, 6.00f, 1.1986f, -3.9701f, -3.0693f, 5.1656f, 0.8444f, 0.8444f, 0.8444f, 0.8444f, 0.8444f},
};

static constexpr size_t NUM_SHAPE_REFS = sizeof(SHAPE_REFERENCES) / sizeof(SHAPE_REFERENCES[0]);

// Find best matching reference and scale offset by size ratio
inline ItemOffset CalculateOffsetFromDimensions(float length, float width, float height) {
    ItemOffset offset;
    
    // Sort dimensions
    float dims[3] = {length, width, height};
    std::sort(dims, dims + 3, std::greater<float>());
    float largest = dims[0];
    float middle = dims[1];
    float smallest = dims[2];
    
    // Calculate ratios
    float ratio1 = (largest > 0.01f) ? middle / largest : 1.0f;
    float ratio2 = (largest > 0.01f) ? smallest / largest : 1.0f;
    
    // Find best matching reference
    float bestScore = 999999.0f;
    size_t bestIdx = 0;
    for (size_t i = 0; i < NUM_SHAPE_REFS; ++i) {
        float score = std::abs(ratio1 - SHAPE_REFERENCES[i].ratio1) + 
                      std::abs(ratio2 - SHAPE_REFERENCES[i].ratio2);
        if (score < bestScore) {
            bestScore = score;
            bestIdx = i;
        }
    }
    
    const auto& ref = SHAPE_REFERENCES[bestIdx];
    
    // DON'T scale position - same shape = same grip = same offset
    // Only finger distance needs to adjust for size
    float scale = largest / ref.refLargest;
    scale = std::clamp(scale, 0.3f, 3.0f);  // Clamp to reasonable range
    
    // Use reference position directly - no scaling!
    offset.position.x = ref.posX;
    offset.position.y = ref.posY;
    offset.position.z = ref.posZ;
    
    // Only scale finger distance for size - bigger objects need wider grip
    offset.fingerDistance = ref.fingerDistance * scale;
    offset.thumbCurl = ref.thumbCurl;
    offset.indexCurl = ref.indexCurl;
    offset.middleCurl = ref.middleCurl;
    offset.ringCurl = ref.ringCurl;
    offset.pinkyCurl = ref.pinkyCurl;
    offset.hasFingerCurls = true;
    
    // Store dimensions
    offset.length = length;
    offset.width = width;
    offset.height = height;
    
    offset.rotation.MakeIdentity();
    offset.scale = 1.0f;
    
    spdlog::info("[ShapeRef] ratio ({:.3f}, {:.3f}) -> ref #{} scale={:.2f} -> pos ({:.2f}, {:.2f}, {:.2f})",
                 ratio1, ratio2, bestIdx, scale, offset.position.x, offset.position.y, offset.position.z);
    
    return offset;
}

} // namespace heisenberg