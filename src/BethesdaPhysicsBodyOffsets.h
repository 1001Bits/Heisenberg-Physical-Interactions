#pragma once

// Offsets and field layouts for porting ROCK's BethesdaPhysicsBody pipeline into Heisenberg.
// Extracted via Ghidra disassembly of ROCK.dll (May 2026) — see project_rock_bethesdaphysicsbody_port.md.
//
// PURPOSE: replace HandCollision::CreateHandBody's _aligned_malloc-based path (which crashes
// in TBB scalable_aligned_free during the engine's destruction phase) with ROCK's engine-
// allocator-based pipeline.

#include <cstddef>
#include <cstdint>

namespace heisenberg::bethesda_physics_body
{
    // ──────────────────────────────────────────────────────────────────────────
    // FALLOUT4VR.EXE OFFSETS (call order in BethesdaPhysicsBody::create)
    // ──────────────────────────────────────────────────────────────────────────
    //
    // Heisenberg already has many of these as REL::Relocation in:
    //   GrabConstraint.h (namespace ConstraintFunctions)
    // Only the (NEW) ones below need new declarations.

    namespace offsets
    {
        // === 12 create() callees, in CALL order ===
        constexpr std::uintptr_t kFunc_PhysicsSystemDataCtor    = 0x5eab0;    // ✓ already in GrabConstraint.h
        constexpr std::uintptr_t kFunc_MotionCinfoCtor          = 0x17a2fc0;  // NEW (only for non-static motion)
        constexpr std::uintptr_t kFunc_BodyCinfoCtor            = 0x1561dd0;  // ✓ already in GrabConstraint.h
        constexpr std::uintptr_t kFunc_MaterialCtor             = 0x1536cb0;  // NEW
        constexpr std::uintptr_t kFunc_PhysicsSystemCtor        = 0x1e0c2b0;  // ✓ bhkPhysicsSystem ctor
        constexpr std::uintptr_t kFunc_CollisionObjectCtor      = 0x1e07710;  // ✓ bhkNPCollisionObject ctor
        constexpr std::uintptr_t kFunc_CollisionObjectAddToWorld = 0x1e07be0; // ✓
        constexpr std::uintptr_t kFunc_PhysicsSystemGetBodyId   = 0x1e0c460;  // ✓
        constexpr std::uintptr_t kFunc_SetBodyMaterial          = 0x153afc0;  // NEW (hknpWorld::setBodyMaterial)
        constexpr std::uintptr_t kFunc_SetBodyKeyframed         = 0x1df5cb0;  // NEW (hknpWorld::setBodyKeyframed)
        constexpr std::uintptr_t kFunc_CollisionObjectSetMotionType = 0x1e07300; // ✓
        constexpr std::uintptr_t kFunc_EnableBodyFlags          = 0x153c090;  // ✓ already used by ThrownObjectTracker

        // === destroy() callees ===
        constexpr std::uintptr_t kFunc_RemovePhysicsSystemInstance = 0x1dfad00;  // NEW (bhkWorld member)

        // === ROCK-INTERNAL HELPERS (relative to ROCK.dll base 0x180000000 — only useful
        // when ROCK.dll is loaded; for Heisenberg's INDEPENDENT port we re-implement these)
        constexpr std::uintptr_t kRockInternal_AllocateHavok    = 0x1ad3c0;   // for 0x78 systemData
        constexpr std::uintptr_t kRockInternal_BethesdaAlloc    = 0x1a0de0;   // for 0x28/0x30
        constexpr std::uintptr_t kRockInternal_HkArrayReserveMore = 0x1ae1f0; // grow systemData arrays

        // === Bethesda allocator (confirmed via ROCK's bethesdaAlloc + ensureInit disasm) ===
        // The pool struct lives at kData_BethesdaAllocatorPool and its first u32 is the init
        // state flag (so kData_BethesdaAllocatorState == kData_BethesdaAllocatorPool + 0).
        // Signature inferred from ROCK source + call site:
        //   void* BethesdaAllocatorInit(void* pool, std::uint32_t* state)
        //   void* BethesdaAlloc(void* poolContext, std::size_t size, std::uint32_t, char)
        constexpr std::uintptr_t kFunc_BethesdaAllocatorInit = 0x1b91dd0;
        constexpr std::uintptr_t kData_BethesdaAllocatorPool = 0x392e880;
        constexpr std::uintptr_t kFunc_BethesdaAlloc         = 0x1b91950;

        // === Havok hkArray grow (confirmed via ROCK 0x1801ae1f0 disasm) ===
        // Signature: bool hkArrayReserveMore(void* allocatorInfo, void* arrayBase, int stride)
        constexpr std::uintptr_t kFunc_HkArrayReserveMore  = 0x155d820;
        constexpr std::uintptr_t kData_HavokAllocatorInfo  = 0x3866310;

        // === createNiNode F4VR offsets (confirmed via ROCK 0x1801a29c0) ===
        // (Already declared above. Skipped in our minimal port — keyframed bodies work
        // without a NiNode owner; ROCK uses it for owner-node bookkeeping only.)

        // === Struct field offsets in bhk* wrappers (confirmed via destroy disasm) ===
        constexpr std::ptrdiff_t kBhkPhysicsSystem_Instance = 0x18;  // native hknpPhysicsSystemInstance*

        // === createNiNode helpers (in BethesdaPhysicsBody::createNiNode @ ROCK 0x1801a29c0) ===
        constexpr std::uintptr_t kFunc_NiNodeCtor             = 0x1c17d30;  // NEW
        constexpr std::uintptr_t kFunc_NiNode_SetName         = 0x1bc1650;  // NEW
        constexpr std::uintptr_t kFunc_CollisionObject_LinkObject = 0x1c16c30; // NEW
        constexpr std::uintptr_t kFunc_BSFixedString_Create   = 0x2996cb0;  // NEW

        constexpr std::size_t    kNiNodeSize                  = 0x180;      // confirmed via bethesdaAlloc call
    }

    // ──────────────────────────────────────────────────────────────────────────
    // STRUCT FIELD LAYOUTS — derived from the LEA RCX, [base+offset] sites in
    // create()'s hkArrayAppendOne calls (each array gets a typed stride):
    //   stride 0x70 = motion cinfo  (MotionCinfoCtor)
    //   stride 0x60 = body   cinfo  (BodyCinfoCtor)
    //   stride 0x50 = material      (MaterialCtor)
    //   stride 0x08 = shape ref     (raw pointer)
    // ──────────────────────────────────────────────────────────────────────────

    namespace sysdata_layout
    {
        constexpr std::ptrdiff_t kMaterials   = 0x10;  // hkArray<hknpMaterial,         stride 0x50>
        constexpr std::ptrdiff_t kMotionCinfos = 0x30; // hkArray<hknpMotionCinfo,      stride 0x70>
        constexpr std::ptrdiff_t kBodyCinfos  = 0x40;  // hkArray<hknpBodyCinfo,        stride 0x60>
        constexpr std::ptrdiff_t kShapes      = 0x60;  // hkArray<hknpShape*,           stride 0x08>
    }

    // bodyCinfo field offsets (from ROCK source — same in F4VR):
    namespace bodycinfo_layout
    {
        constexpr std::ptrdiff_t kShape         = 0x00;  // RE::hknpShape*
        constexpr std::ptrdiff_t kBodyIndex     = 0x08;  // u32, set to 0x7FFFFFFF
        constexpr std::ptrdiff_t kMotionIndex   = 0x0C;  // u32, generatedLocalMotionIndex
        constexpr std::ptrdiff_t kQualityId     = 0x10;  // u16, 0xFF for keyframed
        constexpr std::ptrdiff_t kMaterialIndex = 0x12;  // u16, 0 (local)
        constexpr std::ptrdiff_t kFilterInfo    = 0x14;  // u32, collision layer
        constexpr std::ptrdiff_t kName          = 0x20;  // const char*
        constexpr std::ptrdiff_t kReserved      = 0x28;  // uintptr_t, 0
    }

    // ──────────────────────────────────────────────────────────────────────────
    // NEW REL::Relocations to add to GrabConstraint.h's ConstraintFunctions
    // namespace once we begin Phase 2 (writing the wrapper class):
    //
    // using PhysicsSystem_SetBodyMaterial_t = void (*)(void*, u32, u16, i32);
    // inline REL::Relocation<...> PhysicsSystem_SetBodyMaterial { REL::Offset(0x153afc0) };
    //
    // using PhysicsSystem_SetBodyKeyframed_t = void (*)(void*, u32);
    // inline REL::Relocation<...> PhysicsSystem_SetBodyKeyframed { REL::Offset(0x1df5cb0) };
    //
    // using PhysicsSystemData_MotionCinfoCtor_t = void* (*)(void*);
    // inline REL::Relocation<...> MotionCinfoCtor { REL::Offset(0x17a2fc0) };
    //
    // using PhysicsSystemData_MaterialCtor_t = void* (*)(void*);
    // inline REL::Relocation<...> MaterialCtor { REL::Offset(0x1536cb0) };
    //
    // using BhkWorld_RemovePhysicsSystemInstance_t = void (*)(void*, void*);
    // inline REL::Relocation<...> BhkWorld_RemovePhysicsSystemInstance { REL::Offset(0x1dfad00) };
    // ──────────────────────────────────────────────────────────────────────────

    // ──────────────────────────────────────────────────────────────────────────
    // STILL MISSING — extract next session
    // ──────────────────────────────────────────────────────────────────────────
    //   - hknpWorld_setFilterInfo (havok_runtime helper — Heisenberg has TryWriteBodyFilterInfo
    //     in Physics.cpp which does the equivalent direct write; may not need an F4VR call)
    //   - releaseRefCounted: the CAS-dec-vtable[0] pattern — inline in BethesdaPhysicsBody.cpp
    //     source. Heisenberg can re-implement this directly (no F4VR offset needed).
    //   - Struct offsets for: kBhkPhysicsSystem_Instance, kHknpPhysicsSystemInstance_World,
    //     kCollisionObject_OwnerNode, kNiAVObject_CollisionObject, kBethesdaTlsAllocatorContext
    //   - Identify which of kFunc_Bethesda_A/B/C is alloc-vs-pool-vs-state/init (decompile
    //     ROCK's `ensureBethesdaAllocatorInitialized` at 0x1801a0??? to see the access pattern)
    //   - TLS context guard value 0x41 (per ROCK source) — known constant, no offset needed
}
