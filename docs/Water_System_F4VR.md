# F4VR Water System - Research for Water Interactions Port

## Overview

This document contains research findings for porting Water Interactions VR (Skyrim VR mod) to Fallout 4 VR. The F4VR water system uses different class names and architecture than Skyrim.

## Key Functions for Ripples/Splashes

### Primary Ripple Function (MOST IMPORTANT)
```cpp
// TaskQueueInterface::QueueAddRipple - Queues a ripple effect at position
// VR Offset: 0x0dac660 (Status 2 - Possible)
// Non-VR: 0x0d5bca0
// Signature: void QueueAddRipple(float radius, NiPoint3& position)

using _QueueAddRipple = void(*)(float, RE::NiPoint3&);
inline REL::Relocation<_QueueAddRipple> QueueAddRipple(REL::Offset(0x0dac660));
```

### Splash Effects
```cpp
// TaskQueueInterface::QueueWaterSplashEffects - Queues splash VFX/SFX
// VR Offset: 0x0dac570 (Status 2)
// Non-VR: 0x0d5bbb0
// Signature: void QueueWaterSplashEffects(float magnitude, NiPoint3& position)

using _QueueWaterSplashEffects = void(*)(float, RE::NiPoint3&);
inline REL::Relocation<_QueueWaterSplashEffects> QueueWaterSplashEffects(REL::Offset(0x0dac570));
```

### BGSWaterCollisionManager Splash Functions
```cpp
// BGSWaterCollisionManager::PlaySplashEffects (position-based)
// VR Offset: 0x07c4cd0 (Status 4 - Verified!)
// Non-VR: 0x07d6760
// Signature: void PlaySplashEffects(float magnitude, NiPoint3& position)

// BGSWaterCollisionManager::PlaySplashEffects (body-based)
// VR Offset: 0x07c4f40 (Status 4 - Verified!)
// Non-VR: 0x07d69d0
// Signature: void PlaySplashEffects(hknpBodyId bodyId, hknpBSWorld* world, float magnitude)
```

## TESWaterSystem Class

### Constructor/Destructor
```cpp
// TESWaterSystem::TESWaterSystem
// VR Offset: 0x07cb220 (Status 4 - Verified)
// Non-VR: 0x07dcca0

// TESWaterSystem::~TESWaterSystem
// VR Offset: 0x07cb600 (Status 4 - Verified)
// Non-VR: 0x07dd080
```

### Key Methods
```cpp
// TESWaterSystem::EnableWaterSystem
// VR Offset: 0x07ce760 (Status 4)

// TESWaterSystem::DisableWaterSystem
// VR Offset: 0x07ce7e0 (Status 4)
// Signature: void DisableWaterSystem(bool flag1, bool flag2)

// TESWaterSystem::CreateDisplacementGeometry (for ripples!)
// VR Offset: 0x07cf0f0 (Status 2)
// Non-VR: 0x07e0b70
// Signature: void CreateDisplacementGeometry(NiPoint3& position)
```

## TESWaterDisplacement Class

```cpp
// TESWaterDisplacement::SetPosition - Set ripple position
// VR Offset: 0x07ca860 (Status 4 - Verified!)
// Non-VR: 0x07dc2e0
// Signature: void SetPosition(NiPoint3& position)

// TESWaterDisplacement RTTI
// VR: 0x43772e80
// Non-VR: 0x4374a9c0
```

## Water Height Detection

### TESWorldSpace Methods
```cpp
// TESWorldSpace::GetDefaultWaterHeight
// VR Offset: 0x047c540 (Status 4 - Verified!)
// Non-VR: 0x0493400
// Returns: float (default water height for the worldspace)
```

### TESObjectREFR Methods
```cpp
// TESObjectREFR::GetRelevantWaterHeight
// VR Offset: 0x03f8e90 (Status 4 - Verified!)
// Non-VR: 0x0410b80
// Returns: float (water height at reference location)
```

### TESObjectCELL Methods
```cpp
// TESObjectCELL::GetWaterType
// VR Offset: 0x039baa0 (Status 4 - Verified!)
// Non-VR: 0x03b53d0
// Returns: TESWaterForm*
```

## BGSWaterCollisionManager

Used for physics interaction with water.

```cpp
// BGSWaterCollisionManager::ShouldProcessBody
// VR Offset: 0x07c5250 (Status 4)
// Signature: bool ShouldProcessBody(hknpBodyId, hknpBSWorld*, float)

// BGSWaterCollisionManager::ProcessBody
// VR Offset: 0x07c5420 (Status 4)
// Signature: void ProcessBody(hknpBodyId, hknpBSWorld*, float, float)
```

## Global Pointers

```cpp
// g_DefaultWater - Pointer to default TESWaterForm*
// VR Offset: 0x45a3ad20 (Status 3 - Likely)
// Non-VR: 0x459d9690
// Type: TESWaterForm**
```

## Implementation Strategy for Water Interactions VR Port

### Approach 1: QueueAddRipple (Simplest)
Use `TaskQueueInterface::QueueAddRipple()` to queue ripple effects:

```cpp
void AddWaterRipple(const RE::NiPoint3& position, float radius = 1.0f)
{
    using _QueueAddRipple = void(*)(float, RE::NiPoint3&);
    static REL::Relocation<_QueueAddRipple> QueueAddRipple(REL::Offset(0x0dac660));
    
    RE::NiPoint3 pos = position;  // Non-const copy
    QueueAddRipple(radius, pos);
}
```

### Approach 2: CreateDisplacementGeometry
Use `TESWaterSystem::CreateDisplacementGeometry()`:

```cpp
void CreateRipple(const RE::NiPoint3& position)
{
    using _CreateDisplacementGeometry = void(*)(RE::TESWaterSystem*, RE::NiPoint3&);
    static REL::Relocation<_CreateDisplacementGeometry> CreateDisplacementGeometry(REL::Offset(0x07cf0f0));
    
    // Need TESWaterSystem singleton pointer - not yet found
    // CreateDisplacementGeometry(waterSystem, position);
}
```

### Approach 3: PlaySplashEffects (For audible splashes)
Use `BGSWaterCollisionManager::PlaySplashEffects()`:

```cpp
void PlaySplash(const RE::NiPoint3& position, float magnitude = 1.0f)
{
    // Need BGSWaterCollisionManager singleton - not yet found
    // PlaySplashEffects(magnitude, position);
}
```

## Water Detection for Hands

To check if a hand is in water:

```cpp
bool IsHandInWater(const RE::NiPoint3& handPos)
{
    // Method 1: Check against worldspace water height
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;
    
    auto* worldspace = player->GetWorldspace();
    if (!worldspace) return false;
    
    float waterHeight = worldspace->GetDefaultWaterHeight();
    return handPos.z < waterHeight;
    
    // Method 2: Use cell water type (more accurate for interiors)
    // auto* cell = player->GetParentCell();
    // auto* waterForm = cell->GetWaterType();
    // ... check cell-specific water height
}
```

## Required Singletons (Need to Find)

1. **TESWaterSystem singleton** - For CreateDisplacementGeometry
   - Search pattern: References to TESWaterSystem::TESWaterSystem() result
   
2. **BGSWaterCollisionManager singleton** - For PlaySplashEffects
   - May be accessed via bhkWorld or similar
   
3. **TaskQueueInterface** - May be a global or accessed via Main singleton

## Comparison: Skyrim VR vs F4VR

| Feature | Skyrim VR (HIGGS) | F4VR |
|---------|-------------------|------|
| Ripple function | `TESWaterSystem::AddRipple(pos, strength)` | `TaskQueueInterface::QueueAddRipple(radius, pos)` |
| Splash function | `BGSWaterCollisionManager::PlaySplash(...)` | `BGSWaterCollisionManager::PlaySplashEffects(...)` |
| Water height | `TESWorldSpace::waterHeight` | `TESWorldSpace::GetDefaultWaterHeight()` |
| Point-in-water | `TESWaterSystem::IsPointInWater(pos)` | `Actor::IsPointDeepUnderWater(pos, cell, handle)` |

## Actor Water Detection Function

```cpp
// Actor::IsPointDeepUnderWater
// VR Offset: 0x0dd79c0 (Status 2)
// Non-VR: 0x0d86f30
// Signature: bool IsPointDeepUnderWater(NiPoint3& point, TESObjectCELL* cell, BSPointerHandle<TESObjectREFR>* outWaterRef)
```

## Files Changed in Original Water Interactions VR

Reference: `C:\Development\higgs-master\Water-interactions-VR_SouceCode\`

- `main.cpp` - Plugin entry, event sink registration
- `RE/offsets.h` - Skyrim function addresses
- Uses `TESWaterSystem::AddRipple()` - need to map to F4VR equivalent

## Testing Plan

1. **Test QueueAddRipple first** - Easiest approach
   - Hook into hand tracking update
   - When hand crosses water height, call `QueueAddRipple(1.0f, handPos)`
   - Look for visual ripple effect

2. **Test PlaySplashEffects** - For audio feedback
   - Need to find BGSWaterCollisionManager access pattern
   
3. **Implement cooldown** - Prevent ripple spam
   - Track last ripple time per hand
   - Only emit new ripple every ~0.1-0.2 seconds

## Implementation Checklist

- [ ] Find TESWaterSystem singleton access
- [ ] Find BGSWaterCollisionManager singleton access  
- [ ] Test QueueAddRipple at hardcoded position
- [ ] Integrate with hand tracking (VRControllersManager)
- [ ] Add water height detection for each hand
- [ ] Add ripple emission on water entry
- [ ] Add splash sound on water entry
- [ ] Add continuous ripples while hand is in water
- [ ] Add config options (enable/disable, ripple interval)
