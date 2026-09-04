# Heisenberg F4VR Plugin API

This document describes how to interface with Heisenberg from another F4SE plugin.

## Overview

Heisenberg provides a plugin API that allows other mods to:
- Query grab state (what's being held, is an object being pulled, etc.)
- Register callbacks for grab/drop/stash/consume events
- Control grabbing programmatically
- Access finger curl values for hand poses
- Query zone detection (storage, mouth, VH holster zones)
- Access VR ViewCaster targets (what each wand is pointing at)

## Getting the Interface

### Step 1: Include the Header

Copy `HeisenbergInterface001.h` to your project's include directory.

```cpp
#include "HeisenbergInterface001.h"
```

### Step 2: Request the Interface

After F4SE sends `kGameLoaded` to your plugin, request the Heisenberg interface:

```cpp
HeisenbergPluginAPI::IHeisenbergInterface001* g_heisenbergInterface = nullptr;

void OnF4SEMessage(F4SE::MessagingInterface::Message* msg)
{
    if (msg->type == F4SE::MessagingInterface::kGameLoaded)
    {
        auto* messaging = F4SE::GetMessagingInterface();
        g_heisenbergInterface = HeisenbergPluginAPI::GetHeisenbergInterface001(
            g_pluginHandle, 
            messaging
        );
        
        if (g_heisenbergInterface)
        {
            spdlog::info("Heisenberg API acquired, build {}",
                        g_heisenbergInterface->GetBuildNumber());
        }
    }
}
```

### Step 3: Use the Interface

```cpp
// Check if right hand is holding something
if (g_heisenbergInterface && g_heisenbergInterface->IsHoldingObject(false))
{
    auto* held = g_heisenbergInterface->GetGrabbedObject(false);
    if (held)
    {
        spdlog::info("Right hand holding: {:08X}", held->formID);
    }
}
```

## Callback Registration

Register callbacks to be notified when grab events occur:

```cpp
void OnGrabbed(bool isLeft, RE::TESObjectREFR* refr)
{
    spdlog::info("{} hand grabbed {:08X}", 
                isLeft ? "Left" : "Right", 
                refr->formID);
}

void OnDropped(bool isLeft, RE::TESObjectREFR* refr)
{
    spdlog::info("{} hand dropped {:08X}", 
                isLeft ? "Left" : "Right", 
                refr->formID);
}

void OnStashed(bool isLeft, RE::TESForm* form)
{
    // Note: form is the BASE form, not a reference
    // The object no longer exists in the world
    spdlog::info("{} hand stashed {}", 
                isLeft ? "Left" : "Right", 
                form->GetFullName());
}

void OnConsumed(bool isLeft, RE::TESForm* form)
{
    // Note: form is the BASE form (AlchemyItem)
    spdlog::info("{} hand consumed {}", 
                isLeft ? "Left" : "Right", 
                form->GetFullName());
}

// Register callbacks
g_heisenbergInterface->AddGrabbedCallback(&OnGrabbed);
g_heisenbergInterface->AddDroppedCallback(&OnDropped);
g_heisenbergInterface->AddStashedCallback(&OnStashed);
g_heisenbergInterface->AddConsumedCallback(&OnConsumed);
```

## Available Callbacks

| Callback | Signature | When Triggered |
|----------|-----------|----------------|
| `GrabbedCallback` | `void(bool isLeft, TESObjectREFR* refr)` | Object grabbed (after pull completes) |
| `DroppedCallback` | `void(bool isLeft, TESObjectREFR* refr)` | Object released normally (dropped/thrown) |
| `StashedCallback` | `void(bool isLeft, TESForm* form)` | Object stored to inventory via storage zone |
| `ConsumedCallback` | `void(bool isLeft, TESForm* form)` | Food/drink consumed via mouth zone |
| `PulledCallback` | `void(bool isLeft, TESObjectREFR* refr)` | Object pull animation completed |
| `CollisionCallback` | `void(bool isLeft, float mass, float velocity)` | Hand/held object collided with something |
| `PrePhysicsCallback` | `void(void* bhkWorld)` | Before physics step (for physics mods) |
| `PostPhysicsCallback` | `void(void* bhkWorld)` | After physics step |
| `ViewCasterTargetChangedCallback` | `void(bool isPrimaryWand, TESObjectREFR* newTarget)` | ViewCaster target changed |

## API Reference

### Version Info

```cpp
unsigned int GetBuildNumber();
```
Returns the Heisenberg build number. Use this to check for feature compatibility.

### Grab State Queries

```cpp
bool IsHoldingObject(bool isLeft);
```
Returns true if the specified hand is holding an object.

```cpp
bool IsPulling(bool isLeft);
```
Returns true if an object is being pulled toward the hand but hasn't arrived yet.

```cpp
bool CanGrabObject(bool isLeft);
```
Returns true if the hand can currently grab (not disabled, not holding, not in menu).

```cpp
RE::TESObjectREFR* GetGrabbedObject(bool isLeft);
```
Returns the currently held object, or nullptr if not holding anything.

```cpp
const char* GetGrabbedNodeName(bool isLeft);
```
Returns the name of the grabbed node within the object's skeleton.

### ViewCaster Queries

```cpp
RE::TESObjectREFR* GetViewCasterTarget(bool isLeft);
```
Returns what the specified VR wand is pointing at (for activation/pickup).

```cpp
RE::TESObjectREFR* GetPrimaryWandTarget();
```
Returns what the primary wand (left hand, Pipboy hand) is pointing at.

```cpp
RE::TESObjectREFR* GetSecondaryWandTarget();
```
Returns what the secondary wand (right hand, weapon hand) is pointing at.

```cpp
RE::TESObjectREFR* GetSelectedObject(bool isLeft);
```
Returns Heisenberg's internal selection (may differ from ViewCaster in some cases).

### Grab Control

```cpp
bool GrabObject(RE::TESObjectREFR* object, bool isLeft);
```
Programmatically grab an object. Returns true if successful.

```cpp
void DropObject(bool isLeft);
```
Force-drop the currently held object.

```cpp
void ForceEndGrab(bool isLeft);
```
Immediately end grab with no physics (object disappears).

### Hand Enable/Disable

```cpp
void DisableHand(bool isLeft);
```
Disable grabbing for a hand (for cutscenes, other mods taking control, etc.)

```cpp
void EnableHand(bool isLeft);
```
Re-enable grabbing for a hand.

```cpp
bool IsHandDisabled(bool isLeft);
```
Check if a hand is currently disabled.

### Finger Tracking

```cpp
void GetFingerCurls(bool isLeft, float outCurls[5]);
```
Get current finger curl values (0=curled, 1=extended). Order: thumb, index, middle, ring, pinky.

```cpp
void SetFingerCurls(bool isLeft, const float curls[5]);
```
Override finger curl values (for custom hand poses).

### Zone Detection

```cpp
bool IsInStorageZone(bool isLeft);
```
Check if hand is in the behind-ear storage zone.

```cpp
bool IsInEquipZone(bool isLeft);
```
Check if hand is in an armor/weapon equip zone.

```cpp
bool IsInMouthZone(bool isLeft);
```
Check if hand is in the mouth consume zone.

```cpp
bool IsInVHZone(bool isLeft);
```
Check if hand is in a VirtualHolsters holster zone.

```cpp
const char* GetCurrentZoneName(bool isLeft);
```
Get the name of the current zone ("STORAGE", "MOUTH", "VH_HOLSTER", etc.)

```cpp
int GetVHZoneIndex(bool isLeft);
```
Get the VirtualHolsters holster slot index (1-7), or 0 if not in a holster zone.

### Inventory Integration

```cpp
bool DropToHand(RE::TESForm* form, bool isLeft);
```
Spawn an item from inventory into the specified hand. Only the base form is
known, so the engine removes the first stack of that form in the inventory
list. For weapons and armor with mods, legendary effects or custom names use
`DropInstanceToHand` (build 4) so the exact instance leaves the inventory.

```cpp
bool DropInstanceToHand(RE::TESForm* form, RE::TBO_InstanceData* instanceData,
                        unsigned short uniqueID, bool isLeft, int count);
```
Build 4. Spawn a specific inventory stack into the hand. See the Build 4 tail
below for the matching rules.

```cpp
bool SmartGrab(bool isLeft);
```
Trigger "smart grab" - automatically picks the best item from inventory based on context.

### Transform Control

```cpp
RE::NiTransform GetGrabTransform(bool isLeft);
```
Get the current grabbed object's transform relative to the hand.

```cpp
void SetGrabTransform(bool isLeft, const RE::NiTransform& transform);
```
Override the grabbed object's transform (for positioning mods).

### Settings Access

```cpp
bool GetSettingDouble(const char* name, double& outValue);
```
Read a serialized numeric Heisenberg INI setting. Use either a unique key such
as `fPullSpeed` or a qualified name such as `ObjectPickup.fPullSpeed`. Boolean
values are returned as `0.0`/`1.0`; distances use the same units shown in the
INI.

```cpp
bool SetSettingDouble(const char* name, double value);
```
Persist and reload a Heisenberg INI/MCM setting. Boolean settings accept only
`0.0` or `1.0`, integer settings require an integral value, and non-finite
values are rejected. Call on the main game thread.

### Hand Collision

```cpp
bool IsHandCollisionEnabled();
```
Check if hand collision physics is enabled.

```cpp
void* GetHandRigidBody(bool isLeft);
```
Legacy ABI slot. It returns `nullptr`: hand rigid bodies are owned by ROCK and
are not exposed as stable borrowed pointers.

```cpp
bool IsHandInContact(bool isLeft);
```
Check if the hand is currently touching an object.

```cpp
RE::TESObjectREFR* GetHandContactObject(bool isLeft);
```
Get the object the hand is touching.

### Build 1 Tail

```cpp
void SuppressItemToHand(unsigned int durationMs);
```

Temporarily keep scripted inventory transfers out of loot-to-hand routing.

### Build 2 Tail

After checking `GetBuildNumber() >= 2`, consumers may control the weapon
collision latch, offhand-grip block, query two-hand support, and disable or
enable each hand's collision:

```cpp
void EnableWeaponCollision(bool enable);
bool IsWeaponCollisionDisabled();
void BlockOffHandWeaponGripping(const char* tag, bool block);
bool IsOffHandWeaponGrippingBlocked();
bool IsOffHandGrippingWeapon();
void DisableHandCollision(bool isLeft);
void EnableHandCollision(bool isLeft);
bool IsHandCollisionDisabled(bool isLeft);
```

### Build 3 Tail

After checking `GetBuildNumber() >= 3`, dual-wield integrations can register
one state provider, query physical-hand input, and register one weapon-contact
callback. Registration never replaces another owner. Unregister waits for
other in-flight calls; self-unregistration is safe and completes when the
current invocation returns. See `DualWieldAPI.h` for the versioned structures.

### Build 4 Tail

After checking `GetBuildNumber() >= 4`, consumers can drop a specific
inventory item instance into a hand:

```cpp
bool DropInstanceToHand(RE::TESForm* form, RE::TBO_InstanceData* instanceData,
                        unsigned short uniqueID, bool isLeft, int count);
```

`DropToHand(form, isLeft)` only knows the base form, so the engine removes the
first stack of that form in the inventory list. A looted legendary or modded
weapon can therefore be swapped for the player's own plain copy of the same
base form. `DropInstanceToHand` identifies the exact stack and drops it through
`Actor::DropObject` with a `BGSObjectInstance`, so mods, legendary effects and
custom names are preserved. (Proposed by CylonSurfer.)

The stack is matched by either key; pass what you have:

| Parameter | Source | Notes |
|-----------|--------|-------|
| `instanceData` | `BGSInventoryItem::Stack::extra` → `ExtraInstanceData::data` | Compared by pointer only, never dereferenced. `nullptr` = unknown. |
| `uniqueID` | `ExtraUniqueID::uniqueID`, also `TESContainerChangedEvent::uniqueID` | `0` = unknown. |

When neither key matches a stack, the engine's default stack is dropped, which
is the same behaviour as `DropToHand`. The drop is queued and processed on the
next main-thread update; call it from the main game thread. `count` is the
number of items moved out of that stack (minimum 1).

Example, dropping the stack an inventory menu just selected:

```cpp
void DropSelectedStack(RE::TESForm* baseForm, RE::BGSInventoryItem::Stack* stack, bool isLeft)
{
    if (!g_heisenberg || g_heisenberg->GetBuildNumber() < 4) return;

    RE::TBO_InstanceData* instance = nullptr;
    unsigned short uniqueID = 0;
    if (auto* extra = stack ? stack->extra.get() : nullptr) {
        if (auto* inst = extra->GetByType<RE::ExtraInstanceData>()) instance = inst->data.get();
        if (auto* uid = extra->GetByType<RE::ExtraUniqueID>()) uniqueID = uid->uniqueID;
    }
    g_heisenberg->DropInstanceToHand(baseForm, instance, uniqueID, isLeft, 1);
}
```

## Thread Safety

- `GetBuildNumber()` is immutable.
- Interface state queries and mutations are main-game-thread only unless a
  method explicitly says otherwise. Returned game pointers are borrowed.
- Callback registration is synchronized. Ordinary interaction callbacks run
  on the main thread; physics/contact callbacks can run on the Havok thread.
- Generic callbacks cannot be unregistered in revision 1, so a registering
  module must remain loaded.

## Version History

| Build | Changes |
|-------|---------|
| 1 | Initial API plus item-to-hand suppression tail |
| 2 | Weapon collision, offhand grip, and per-hand collision controls |
| 3 | Versioned dual-wield state/input/contact bridge |
| 4 | `DropInstanceToHand`: instance-exact inventory drop via `Actor::DropObject` |

## Example: Integration with Another VR Mod

```cpp
#include "HeisenbergInterface001.h"

HeisenbergPluginAPI::IHeisenbergInterface001* g_heisenberg = nullptr;

void OnGameLoaded()
{
    g_heisenberg = HeisenbergPluginAPI::GetHeisenbergInterface001(
        g_pluginHandle, 
        F4SE::GetMessagingInterface()
    );
    
    if (g_heisenberg)
    {
        // Register for grab events
        g_heisenberg->AddGrabbedCallback([](bool isLeft, RE::TESObjectREFR* refr) {
            // Do something when object is grabbed
            MyMod::OnObjectGrabbed(isLeft, refr);
        });
        
        g_heisenberg->AddDroppedCallback([](bool isLeft, RE::TESObjectREFR* refr) {
            // Do something when object is dropped
            MyMod::OnObjectDropped(isLeft, refr);
        });
    }
}

void MyMod::Update()
{
    if (!g_heisenberg) return;
    
    // Check what player is pointing at with right hand
    auto* target = g_heisenberg->GetSecondaryWandTarget();
    if (target && target->GetFormType() == RE::ENUM_FORM_ID::kACTI)
    {
        // Right hand pointing at an activator
        ShowActivationPrompt(target);
    }
    
    // Check if player is holding something
    for (bool isLeft : {true, false})
    {
        if (g_heisenberg->IsHoldingObject(isLeft))
        {
            auto* held = g_heisenberg->GetGrabbedObject(isLeft);
            ProcessHeldObject(isLeft, held);
        }
    }
}
```

## Troubleshooting

### Interface is nullptr

- Ensure Heisenberg is installed and enabled
- Request the interface after `kGameLoaded` message, not before
- Check your F4SE log for error messages

### Callbacks not firing

- Callbacks are only invoked from the main thread
- Ensure you registered callbacks after getting the interface
- Check that the grab/drop actually completed (not interrupted)

### Build number mismatch

- If `GetBuildNumber()` returns a lower number than expected, some features may be unavailable
- Check for Heisenberg updates
