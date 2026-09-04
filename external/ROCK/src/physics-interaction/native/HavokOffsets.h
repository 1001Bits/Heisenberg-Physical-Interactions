#pragma once

#include <cstddef>
#include <cstdint>

namespace rock::offsets
{

    constexpr std::uintptr_t kCollisionObject_OwnerNode = 0x10;

    constexpr std::uintptr_t kCollisionObject_PhysSystemPtr = 0x20;

    constexpr std::uintptr_t kBhkPhysicsSystem_Instance = 0x18;

    constexpr std::uintptr_t kHknpPhysicsSystemInstance_World = 0x18;

    constexpr std::uintptr_t kBhkWorld_HknpWorldPtr = 0x60;

    constexpr std::uintptr_t kNiAVObject_CollisionObject = 0x100;

    constexpr std::uintptr_t kHknpWorld_ModifierManager = 0x150;

    constexpr std::uintptr_t kModifierMgr_FilterPtr = 0x5E8;

    constexpr std::uintptr_t kFilter_CollisionMatrix = 0x1A0;

    constexpr std::uintptr_t kHknpWorld_MotionArrayPtr = 0xE0;

    constexpr std::uintptr_t kHknpWorld_MotionPropertiesLibraryPtr = 0x5D0;

    /*
     * FO4VR's world reader-writer lock (BSReadWriteLock: u32 owner thread id,
     * u32 state). Engine mutation wrappers write-lock it (raw-disasm verified in
     * bhkWorld::RemovePhysicsSystemInstance @0x1DFAD00: rbx=[bhkWorld+0x60],
     * lock at rbx+0x6D8); [bhkWorld+0x60] is the hknpWorld
     * (kBhkWorld_HknpWorldPtr), so the lock lives inside hknpWorld itself.
     * Threads inside the physics step publish a nonzero TLS byte at
     * kTlsSlot_InPhysicsStepFlag (exe TLS index at kGlobal_ExeTlsIndex) and the
     * engine skips locking there; ROCK must do the same.
     * See Docs/ROCK/lessons/2026-07-13-hknp-world-query-lock-discipline.md.
     */
    constexpr std::uintptr_t kHknpWorld_AccessLock = 0x6D8;

    constexpr std::uintptr_t kGlobal_ExeTlsIndex = 0x689CACC;

    constexpr std::uintptr_t kTlsSlot_InPhysicsStepFlag = 0x1529;

    constexpr std::uintptr_t kBody_CollisionFilterInfo = 0x44;

    constexpr std::uintptr_t kBody_CollisionObjectBackPointer = 0x88;

    constexpr std::uintptr_t kMotion_PropertiesId = 0x38;

    constexpr std::uintptr_t kMotion_MaxLinearVelocityPacked = 0x3A;

    constexpr std::uintptr_t kMotion_MaxAngularVelocityPacked = 0x3C;

    constexpr std::uintptr_t kMotionPropertiesLibrary_Entries = 0x28;

    constexpr std::uintptr_t kMotionPropertiesLibrary_Count = 0x30;

    constexpr std::size_t kMotionProperties_RecordSize = 0x40;

    constexpr std::uintptr_t kMotionProperties_LinearDamping = 0x18;

    constexpr std::uintptr_t kMotionProperties_AngularDamping = 0x1C;

    constexpr int kTransformA_Col0 = 0x30;
    constexpr int kTransformA_Col1 = 0x40;
    constexpr int kTransformA_Col2 = 0x50;
    constexpr int kTransformA_Pos = 0x60;

    constexpr int kTransformB_Col0 = 0x70;
    constexpr int kTransformB_Col1 = 0x80;
    constexpr int kTransformB_Col2 = 0x90;
    constexpr int kTransformB_Pos = 0xA0;

    constexpr std::uintptr_t kFunc_SetBodyCollisionFilterInfo = 0x1DF5B80;
    // hknpWorld::rebuildBodyCollisionCaches. Ghidra-confirmed: this is the function
    // hknpWorld::setBodyCollisionFilterInfo (0x153AF00) tail-calls when its
    // RebuildCachesMode argument is 0. Called directly after a raw transform write on a
    // freshly created body, where no filter change occurs and so no setter-driven rebuild
    // is triggered.
    constexpr std::uintptr_t kFunc_RebuildBodyCollisionCaches = 0x153C5A0;

    constexpr std::uintptr_t kFunc_SetBodyVelocity = 0x1539F30;

    constexpr std::uintptr_t kFunc_SetBodyTransformDeferred = 0x1DF55F0;

    constexpr std::uintptr_t kFunc_SetBodyVelocityDeferred = 0x1DF56F0;

    constexpr std::uintptr_t kFunc_SetBodyKeyframed = 0x1DF5CB0;

    /*
     * Ghidra-verified (F4VR 1.2.72). This is hknpWorld's 7-parameter
     * "compute a hard keyframe" helper:
     *   void(hknpWorld*, hknpBodyId, float* targetPos, float* targetRot,
     *        float deltaTimeSeconds, float* outLinearVelocity, float* outAngularVelocity)
     * which is exactly the ComputeHardKeyFrame_t typedef its two consumers use
     * (native/GeneratedKeyframedBodyDrive.cpp, hand/HandGrab.cpp).
     *
     * THE 5TH ARGUMENT IS DELTA TIME, NOT ITS RECIPROCAL. Disassembly of 0x14153A6A0
     * (F4VR 1.2.72): the native broadcasts arg5, runs rcpps plus one Newton-Raphson refine
     * against the {2,2,2,2} constant to form 1/arg5, then multiplies the position delta by
     * it and stores the result into the out-linear-velocity buffer. It reciprocates its own
     * argument. Both call sites correctly pass dt
     * (GeneratedKeyframedBodyDrive.cpp driveDeltaSeconds, HandGrab.cpp deltaTime).
     * DO NOT "fix" them to 1/dt: the native would then compute 1/(1/dt) == dt and scale
     * every keyframe drive velocity by dt^2 (~1.2e-4 of correct at 90 Hz), so held objects
     * and hand colliders would stop tracking entirely with no error and no assertion - the
     * isFinite3 guards on the outputs still pass. The old parameter name here said
     * "invDeltaTime" and pointed a maintainer straight at that change.
     *
     * DO NOT put 0x1DF5930 back here. That address is the 5-parameter
     * bhkWorld-level applyHardKeyFrame, which has no out-velocity buffers at
     * all: called through the 7-param typedef it leaves both output arrays
     * untouched, so the drive reports success while writing ZERO velocity.
     * The arity mismatch is silent - the typedef is a raw function pointer -
     * which is why this was a live bug rather than a compile error.
     */
    constexpr std::uintptr_t kFunc_ComputeHardKeyFrame = 0x153a6a0;

    constexpr std::uintptr_t kFunc_RebuildMotionMassProperties = 0x1546570;

    constexpr std::uintptr_t kFunc_ConvexBuildConfig_Init = 0x16D4AB0;

    constexpr std::uintptr_t kFunc_ConvexShape_FromPoints = 0x16D4B30;

    constexpr std::uintptr_t kFunc_CompoundShapeCinfo_FromInstances = 0x16E1CF0;

    constexpr std::uintptr_t kFunc_StaticCompoundShape_Ctor = 0x1E9C950;

    constexpr std::uintptr_t kFunc_ShapeInstance_SetShape = 0x16E1780;

    constexpr std::uintptr_t kFunc_ShapeInstance_SetTransform = 0x16E1840;

    constexpr std::uintptr_t kFunc_ShapeInstance_SetScale = 0x16E1910;

    constexpr std::uintptr_t kFunc_CollisionObject_SetMotionType = 0x1E07300;

    constexpr std::uintptr_t kFunc_CollisionObject_Ctor = 0x1E07710;

    constexpr std::uintptr_t kFunc_CollisionObject_CreateInstance = 0x1E07AC0;

    constexpr std::uintptr_t kFunc_CollisionObject_AddToWorld = 0x1E07BE0;

    constexpr std::uintptr_t kFunc_PhysicsSystem_Ctor = 0x1E0C2B0;

    constexpr std::uintptr_t kFunc_PhysicsSystem_GetBodyId = 0x1E0C460;

    constexpr std::uintptr_t kFunc_PhysicsSystemData_Ctor = 0x5EAB0;

    constexpr std::uintptr_t kFunc_BodyCinfo_Ctor = 0x1561DD0;

    constexpr std::uintptr_t kFunc_MaterialCtor = 0x1536CB0;

    constexpr std::uintptr_t kFunc_CollisionObject_LinkObject = 0x2996CB0;

    constexpr std::uintptr_t kFunc_CollisionObject_DriveToKeyFrame = 0x1E086E0;

    constexpr std::uintptr_t kFunc_CollisionObject_SetTransform = 0x1E08A70;

    constexpr std::uintptr_t kFunc_CollisionObject_SetVelocity = 0x1E082A0;

    constexpr std::uintptr_t kFunc_CollisionObject_SetLinearVelocity = 0x1E08050;

    constexpr std::uintptr_t kFunc_CollisionObject_SetAngularVelocity = 0x1E08170;

    constexpr std::uintptr_t kFunc_CollisionObject_ApplyLinearImpulse = 0x1E08520;

    constexpr std::uintptr_t kFunc_CollisionObject_ApplyPointImpulse = 0x1E08640;

    constexpr std::uintptr_t kFunc_CollisionObject_SetMass = 0x1E08C00;

    constexpr std::uintptr_t kFunc_CollisionObject_GetFilterInfo = 0x1E08D60;

    constexpr std::uintptr_t kFunc_CollisionObject_GetCOMWorld = 0x1E08EF0;

    constexpr std::uintptr_t kFunc_CollisionObject_GetShape = 0x1E07F30;

    // Phase-1 verdict: KEEP HEISENBERG BASELINE. 0x1E090E0 is
    // bhkNPCollisionObject::IsBodyConstrained (checks BOTH constraint endpoints at
    // +8 and +0xC against systemBodyIdx, excludes constraint type 0xB). Upstream's
    // 0x1E09170 is the adjacent bhkNPCollisionObject::IsBodyConstrainedChild, which
    // only checks the +8 (child/A) side - it contradicts this constant's own name.
    // Same signature either way (no crash risk); ours is the broader/safer predicate
    // for its only consumer, palmAnchorBody.isConstrained() rebuild-deferral.
    constexpr std::uintptr_t kFunc_IsBodyConstrained = 0x1E090E0;

    constexpr std::uintptr_t kFunc_World_AddPhysicsSystem = 0x1DFAC30;

    constexpr std::uintptr_t kFunc_BhkWorld_RemovePhysicsSystemInstance = 0x1DFAD00;

    constexpr std::uintptr_t kFunc_HknpWorld_SetBodyMotion = 0x153BAE0;

    constexpr std::uintptr_t kFunc_HknpWorld_SetBodyMaterial = 0x153AFC0;

    constexpr std::uintptr_t kFunc_BhkWorld_SetMotionRecursive = 0x1DF95B0;

    constexpr std::uintptr_t kFunc_World_EnableCollision = 0x1DF9940;

    constexpr std::uintptr_t kFunc_World_PickObject = 0x1DF8D60;

    constexpr std::uintptr_t kFunc_World_AddStepListener = 0x1DFA7B0;

    constexpr std::uintptr_t kData_BhkWorldRawDeltaSeconds = 0x65A3D70;

    constexpr std::uintptr_t kData_BhkWorldSubstepDeltaSeconds = 0x65A3D74;

    constexpr std::uintptr_t kData_BhkWorldRemainderDeltaSeconds = 0x65A3D7C;

    constexpr std::uintptr_t kData_BhkWorldAccumulatedDeltaSeconds = 0x65A3D84;

    constexpr std::uintptr_t kData_BhkWorldSubstepCount = 0x65A3D8C;

    /*
     * Ghidra-verified (F4VR 1.2.72). The MemoryManager POOL object itself, i.e.
     * the `this` that BethesdaPhysicsBody::bethesdaAllocatorPool() hands to the
     * allocator entry points.
     *
     * The old value 0x392E880 is NOT the pool - it is the allocator STATE flag
     * (kData_BethesdaAllocatorState, just below; the two differ by exactly
     * +0x480). Passing the state word as MemoryManager* meant every allocation
     * ran against a `this` pointing four bytes of init-state, not a pool.
     * Keep both constants: the state word is read as an int
     * (BethesdaPhysicsBody.cpp:53, `*state != 2`), the pool is taken by
     * address (BethesdaPhysicsBody.cpp:45).
     */
    constexpr std::uintptr_t kData_BethesdaAllocatorPool = 0x392E400;

    // Allocator init-state flag; pool + 0x480. Read as a value, never passed as
    // a MemoryManager* - see the note on kData_BethesdaAllocatorPool above.
    constexpr std::uintptr_t kData_BethesdaAllocatorState = 0x392E880;

    constexpr std::uintptr_t kData_BethesdaTlsIndex = 0x689CACC;

    constexpr std::uintptr_t kBethesdaTlsAllocatorContext = 0x9C0;

    constexpr std::uintptr_t kFunc_BethesdaAlloc = 0x1B91950;

    constexpr std::uintptr_t kFunc_BethesdaAllocatorInit = 0x1B91DD0;

    constexpr std::uintptr_t kData_HavokTlsAllocKey = 0x5B63B20;

    constexpr std::uintptr_t kFunc_HkArray_ReserveMore = 0x155D820;

    constexpr std::uintptr_t kData_HkArrayAllocatorGlobal = 0x3866310;

    constexpr std::uintptr_t kFunc_MotionCinfo_Ctor = 0x17A2FC0;

    constexpr std::uintptr_t kData_HavokGameToHavokScale = 0x5A38628;

    constexpr std::uintptr_t kData_HavokToGameScale = 0x3718110;

    constexpr std::uintptr_t kData_VRScalePrimary = 0x5B29178;

    constexpr std::uintptr_t kData_RaycastResultScale = 0x37CEA5C;

    constexpr std::uintptr_t kFunc_EnableBodyFlags = 0x153C090;

    constexpr std::uintptr_t kFunc_DisableBodyFlags = 0x153C150;

    constexpr std::uintptr_t kFunc_ActivateBody = 0x1546EF0;

    constexpr std::uintptr_t kFunc_NiNode_Ctor = 0x1C17D30;

    constexpr std::uintptr_t kFunc_NiNode_Dtor = 0x1C17DD0;

    constexpr std::uintptr_t kFunc_NiNode_SetName = 0x1C16C30;

    constexpr std::uintptr_t kFunc_BSFixedString_Create = 0x1BC1650;

    constexpr std::uintptr_t kData_NiNode_Vtable = 0x2E57A68;

    constexpr std::size_t kNiNodeSize = 0x180;

    constexpr int kNiNodeAlignment = 0x10;

    constexpr int kSysData_Materials = 0x10;
    constexpr int kSysData_Array1 = 0x20;
    constexpr int kSysData_Array2 = 0x30;
    constexpr int kSysData_MotionCinfos = kSysData_Array2;
    constexpr int kSysData_BodyCinfos = 0x40;
    constexpr int kSysData_ConstraintInfos = 0x50;
    constexpr int kSysData_Shapes = 0x60;

    constexpr std::uintptr_t kFunc_World_CastRay = 0x15A6B10;

    constexpr std::uintptr_t kFunc_World_GetClosestPoints = 0x15A6DF0;

    constexpr std::uintptr_t kFunc_World_QueryAabb = 0x15A64B0;

    constexpr std::uintptr_t kFunc_World_QueryAabbBroadphaseOnly = 0x15A6330;

    constexpr std::uintptr_t kFunc_CreateSphereShape = 0x15FF4E0;

    constexpr std::uintptr_t kFunc_NativeVRGrabDrop = 0xF1AB90;

    /*
     * Equipped-weapon 3D attach task submission. Blind raw-disassembly
     * verification against Fallout4VR.exe 1.2.72 on 2026-07-22 confirmed
     * 0x140DAB8F0 submits task type 0x12 and retains both the actor and the
     * BGSObjectInstance payload. The task re-resolves and exact-compares the
     * actor's current form/instance before calling Actor::AttachWeapon, so a
     * stale recovery request fails closed after a later weapon switch.
     */
    constexpr std::uintptr_t kFunc_QueueEquippedWeaponAttach = 0xDAB8F0;
    constexpr std::uintptr_t kData_EquippedWeaponAttachManager = 0x5B279E0;

    constexpr std::uintptr_t kFunc_SetBodyMotionProperties = 0x153B2F0;

    constexpr std::uintptr_t kFunc_MotionPropertiesLibrary_AddEntry = 0x1767A70;

    constexpr std::uintptr_t kData_ConvexPolytopeVtable = 0x2C9A108;

    constexpr std::uintptr_t kFunc_MaterialLibrary_AddMaterial = 0x1537840;

    constexpr std::uintptr_t kFunc_GetConstraintInfoUtil = 0x1A4AD20;

    /*
     * Stock FO4VR constraint-data constructors and world-frame setters.
     * Blind Ghidra verification on 2026-07-23 established the exact object
     * sizes and setter ABIs used by TouchGrabRuntime:
     *   Ball-and-socket: 0x70
     *   Limited hinge:   0x130
     *   Prismatic:       0x120
     */
    constexpr std::uintptr_t kFunc_BallAndSocketConstraintData_Ctor = 0x19AF690;
    constexpr std::uintptr_t kFunc_BallAndSocketConstraintData_SetPivots = 0x19AF6E0;
    constexpr std::uintptr_t kFunc_LimitedHingeConstraintData_Ctor = 0x19ACA30;
    constexpr std::uintptr_t kFunc_LimitedHingeConstraintData_SetInWorldSpace = 0x19ACD90;
    constexpr std::uintptr_t kFunc_PrismaticConstraintData_Ctor = 0x19B1350;
    constexpr std::uintptr_t kFunc_PrismaticConstraintData_SetInWorldSpace = 0x19B1520;

    /*
     * CONFIRMED 2026-07-28 (Ghidra, Fallout4VR.exe 1.2.72). getFunctionAt(0x141E24980)
     * is bhkCharacterController::HandleBumpedCharacter - the exact entry, not merely a
     * containing function - and its first 15 bytes are
     *   48 89 5C 24 08 | 48 89 74 24 18 | 57 | 48 83 EC 70
     * which is byte-identical to the prefix PhysicsHooks::installBumpHook validates.
     * The constant is correct; do not "fix" it in response to a validation failure.
     *
     * Scope note, also confirmed by decompilation: this function is pure
     * character-controller-vs-character-controller shove (contact normal, relative
     * approach speed, push accumulation, bump counter, SetVelocityModifier, bump
     * listener vf +0x30). Its only callers are four CC solve callbacks. It contains no
     * health, damage, hit-data or projectile code, so suppressing it CANNOT make the
     * player immune to bullets - worst case is that NPCs and the player stop shoving
     * each other.
     */
    constexpr std::uintptr_t kFunc_HandleBumpedCharacter = 0x1E24980;

    // CONFIRMED 2026-08-31 (Ghidra, Fallout4VR.exe 1.2.72):
    // bhkCharacterController::SetVelocityModifier. The point argument is a
    // signed displacement in game units and the scalar is its duration in
    // seconds; the native routine performs the Havok conversion and velocity
    // derivation before publishing the bounded controller modifier.
    constexpr std::uintptr_t kFunc_CharacterController_SetVelocityModifier =
        0x1E22FC0;

    /*
     * CONFIRMED (decrypted Fallout4VR.exe 1.2.72): VR CreateMouseSpring entry.
     * installNativeGrabHook() validates the complete native prefix before its
     * atomic xor-eax/ret write and also accepts that exact suppressed prefix
     * when the host-side caller has already installed it.
     */
    constexpr std::uintptr_t kFunc_VRGrabInitiate = 0xF19250;

    constexpr std::uintptr_t kFunc_ProcessConstraintsCallback = 0x1E4B7E0;

    // CONFIRMED (Ghidra, F4VR 1.2.72): 0x140FEF820 = WeaponSwingHandler::vf001,
    // and the reverse-xref list for it contains the vtable slot 0x142D8CA00 below.
    constexpr std::uintptr_t kFunc_WeaponSwingHandler_Handle = 0x0FEF820;

    // CONFIRMED (Ghidra, F4VR 1.2.72): 0x140FEFFB0 = HitFrameHandler::vf001,
    // paired with vtable slot 0x142D8CB98 below.
    constexpr std::uintptr_t kFunc_HitFrameHandler_Handle = 0x0FEFFB0;

    /*
     * Ghidra-verified (F4VR 1.2.72). AttackBlockHandler::ShouldHandleEvent -
     * the implementation body. Paired with
     * kVtableEntry_AttackBlockHandler_ShouldHandleEvent below; PhysicsHooks
     * validates that the vtable slot currently points at THIS address before
     * swapping it (core/PhysicsHooks.cpp:1015), so the two must be updated
     * together or the hook refuses to install.
     */
    constexpr std::uintptr_t kFunc_AttackBlockHandler_ShouldHandleEvent = 0x0FCD770;

    // CONFIRMED (Ghidra, F4VR 1.2.72): 0x140F23E00 = PlayerCharacter::WeaponSwingCallBack,
    // held by vtable slot 0x142D817A8 below.
    constexpr std::uintptr_t kFunc_PlayerCharacter_WeaponSwingCallBack = 0x0F23E00;

    /*
     * CONFIRMED 2026-07-28 (Ghidra, F4VR 1.2.72) by three independent checks:
     *  1. Bytes at 0x140EFF000 are 48 8B C4 4C 89 40 18 48 89 50 10 55 53 56 - an exact
     *     14/14 match with kVrMeleeImpactExpectedPrefix in PhysicsHooks.cpp.
     *  2. Instruction boundaries sum to exactly 14 (mov rax,rsp / mov [rax+18],r8 /
     *     mov [rax+10],rdx / push rbp / push rbx / push rsi), none RIP-relative, so a
     *     14-byte steal is legal.
     *  3. The address is registered by FUN_140F38630 and FUN_140F38810 alongside the
     *     literal string "VRMeleeImpact" at 0x142D858E8 - the game's own name for it.
     *
     * HISTORY: shipped 0.8.4 had this as TODO_RE (== 0). REL::Offset(0) is the module
     * base, so the validator memcmp'd against the PE/DOS header ("MZ\x90\0\x03..."):
     * readable, so no crash; never equal, so it failed on EVERY machine and looked
     * exactly like a mod conflict. That single zero was the whole reason melee
     * suppression reported "install deferred".
     */
    constexpr std::uintptr_t kFunc_VRMeleeImpactCallback = 0x0EFF000;

    // CONFIRMED (Ghidra, F4VR 1.2.72): slot 0x142D8CA00 holds 0x140FEF820.
    constexpr std::uintptr_t kVtableEntry_WeaponSwingHandler_Handle = 0x2D8CA00;

    // CONFIRMED (Ghidra, F4VR 1.2.72): slot 0x142D8CB98 holds 0x140FEFFB0.
    constexpr std::uintptr_t kVtableEntry_HitFrameHandler_Handle = 0x2D8CB98;

    /*
     * Ghidra-verified (F4VR 1.2.72). The vtable SLOT that holds
     * kFunc_AttackBlockHandler_ShouldHandleEvent, i.e. the address
     * installNativeMeleeVtableHook() overwrites (core/PhysicsHooks.cpp:1651)
     * and restoreNativeMeleeVtableHook() puts back (:1578).
     *
     * CONFIRMED 2026-07-28: vtable_AttackBlockHandler is a named symbol at 0x142D8A348
     * (2 data xrefs, both from its constructor FUN_140FC5F30). slot[0] @0x142D8A348 is
     * the dtor (AttackBlockHandler::vf000, 99 bytes); slot[1] @0x142D8A350 holds
     * 0x140FCD770, a 964-byte body that does a virtual QUserEvent on the event,
     * compares three control-ID strings and checks six UI::GetMenuOpen states - i.e.
     * PlayerInputHandler::ShouldHandleEvent. The reverse-xref list for 0x140FCD770
     * contains 0x142D8A350.
     *
     * This was TODO_RE = 0 in the reconstructed baseline. A zero slot makes
     * validateNativeMeleeVtableTarget() read the module base and report a phantom
     * "external target 0x300905A4D" (the first qword of the PE header), so the hook
     * NEVER installed and native melee suppression was silently dead. With the real
     * slot in place that suppression goes live for the first time - expect a behaviour
     * change here, not just a constant change.
     *
     * Note also what shipped 0.8.4 had as the paired function constant:
     * 0x1401ED740, which is AttackBlockHandler::vf004 - a 3-byte `ret 0` stub at
     * slot[4]. Wrong virtual entirely; it only never mattered because the slot
     * constant beside it was 0.
     */
    constexpr std::uintptr_t kVtableEntry_AttackBlockHandler_ShouldHandleEvent = 0x2D8A350;

    // CONFIRMED (Ghidra, F4VR 1.2.72): slot 0x142D817A8 holds 0x140F23E00
    // (PlayerCharacter::WeaponSwingCallBack). Also TODO_RE = 0 in shipped 0.8.4.
    constexpr std::uintptr_t kVtableEntry_PlayerCharacter_WeaponSwingCallBack = 0x2D817A8;

    constexpr std::uintptr_t kData_PlayerActorSingleton = 0x5A38518;

    constexpr std::uintptr_t kHookSite_MainLoop = 0xD8405E;

    /*
     * FO4VR's recurring first-person node-chain alignment helper. Blind raw
     * disassembly verification on 2026-07-17 confirmed the entry at
     * 0x140EF6280 and the paired calls at 0x140EF6108/0x140EF614B. In
     * right-handed mode the first call uses PlayerNodes +0x718 while the
     * second uses +0x790; module+0xEF610D is therefore the exact return site
     * for Bethesda's primary-hand pass and module+0xEF6150 is the exact
     * return site for the paired secondary/offhand pass. Its player-update caller at
     * module+0xD83F0F precedes the framework/FRIK main-loop injection at
     * module+0xD8405E in the same native routine, proving this snapshot is
     * taken before hFRIK's arm/weapon pass. ROCK validates the complete
     * position-independent 14-byte prologue before intercepting the helper.
     */
    constexpr std::uintptr_t kFunc_UpdateFirstPersonArm = 0xEF6280;
    constexpr std::uintptr_t kCallsite_UpdateFirstPersonArmPrimaryReturn = 0xEF610D;
    constexpr std::uintptr_t kCallsite_UpdateFirstPersonArmSecondaryReturn = 0xEF6150;
    constexpr std::uintptr_t kFunc_PlayerPostUpdateAnimationGraphManager = 0xF2F0A0;

    /*
     * FO4VR native-scope geometry boundary. Raw-disassembly verified
     * 2026-07-15: CALL at 0x140EF851F targets 0x140EFAA60 with ABI
     * (PlayerCharacter*, bool). The surrounding routine has already applied
     * the native menu/weapon/VATS gates; only its final cone decision is
     * replaced. The renderer accessor reads request byte +0x3 from the state
     * object at 0x146239340.
     */
    constexpr std::uintptr_t kHookSite_NativeScopeGeometryDecision = 0xEF851F;

    // PipboyInventoryMenu::DoSelectItem call to UseItem. Raw-disassembly
    // verified against Fallout4VR.exe 1.2.72; the runtime hook validates both
    // the E8 opcode and decoded destination before patching.
    constexpr std::uintptr_t kHookSite_PipboyInventoryUseItem = 0xB9CFBA;

    constexpr std::uintptr_t kFunc_PipboyInventoryUseItem = 0xB9B890;

    constexpr std::uintptr_t kFunc_PipboyInventoryUpdateData = 0xB99C30;
    constexpr std::uintptr_t kPatchSite_NativeScopePostDecisionTest = 0xEF8528;
    constexpr std::uintptr_t kFunc_NativeScopeStateTransition = 0xEFAA60;
    constexpr std::uintptr_t kFunc_NativeScopeRequestStateGet = 0x1D947B0;
    constexpr std::uintptr_t kData_NativeScopeRendererState = 0x6239340;

    /*
     * Native WSScope presentation setup skipped by the equip path when an
     * otherwise valid magnified optic omits WEAPON_FLAGS::kHasScope. The
     * configure entry consumes the ZOOM record's overlay index. ROCK validates
     * its prologue, the singleton pointer, and the singleton's primary vtable
     * before invoking it for an unflagged manual-scope generation.
     */
    constexpr std::uintptr_t kFunc_NativeWorldScopeConfigure = 0xC8DC60;
    constexpr std::uintptr_t kData_NativeWorldScopeSingleton = 0x5ACBF58;
    constexpr std::uintptr_t kData_NativeWorldScopePrimaryVtable = 0x2D68718;

    /*
     * BGSModelMaterialSwap application used by TryAttach3DRecurse immediately
     * after cloning an OMOD model. Manual physical-housing enrichment calls the
     * same entry so recovered geometry keeps the equipped instance's skins.
     */
    constexpr std::uintptr_t kFunc_ApplyOmodModelCustomization = 0x53CD0;

    /*
     * PlayerCharacter flag storage is independently witnessed in the scope
     * update at 0x140EF84AF and the state transition at 0x140EFAAF7. Bit 0x08
     * is the native force-true branch immediately before the cone decision and
     * must retain priority over ROCK's replacement geometry.
     */
    constexpr std::ptrdiff_t kPlayerCharacter_NativeScopeFlags = 0x12A1;
    constexpr std::uint8_t kPlayerCharacter_NativeScopeForceDecisionMask = 0x08;

    /* Engine Setting objects; the live float value is the first four bytes. */
    constexpr std::uintptr_t kSetting_HmdScopeOffsetX = 0x37CF468;
    constexpr std::uintptr_t kSetting_HmdScopeOffsetY = 0x37CF480;
    constexpr std::uintptr_t kSetting_HmdScopeOffsetZ = 0x37CF498;
    constexpr std::uintptr_t kSetting_HmdScopeAngleEnterDegrees = 0x37CF4F8;
    constexpr std::uintptr_t kSetting_HmdScopeAngleExitDegrees = 0x37CF528;
    constexpr std::uintptr_t kSetting_WeaponScopeAngleEnterDegrees = 0x37CF540;
    constexpr std::uintptr_t kSetting_WeaponScopeAngleExitDegrees = 0x37CF570;
    constexpr std::uintptr_t kSetting_WeaponScopeDistanceEnter = 0x37CF5A0;
    constexpr std::uintptr_t kSetting_WeaponScopeDistanceExit = 0x37CF5B8;
    constexpr std::uintptr_t kSetting_ScopeWeaponAngleWideningFactor = 0x37CF5E8;
    constexpr std::uintptr_t kSetting_ScopeWeaponAngleExponent = 0x37CF600;

    constexpr std::uintptr_t kData_CollisionFilterSingleton = 0x59429B8;

    /*
     * DO NOT "correct" this address. It is one leg of an ATOMIC TRIPLE:
     *   (1) this offset,
     *   (2) the subscribe_ext_t typedef at its call site
     *       (core/PhysicsInteractionContacts.inl:377), and
     *   (3) the arity of the contact callback that typedef hands over.
     * Changing any one of the three in isolation silently mismatches the
     * calling convention and corrupts the contact stream. Verified working at
     * 0x3B9E50 against F4VR 1.2.72; leave it alone.
     */
    constexpr std::uintptr_t kFunc_SubscribeContactEvent = 0x3B9E50;

    constexpr std::uintptr_t kFunc_UnsubscribeSignalCallback = 0x1725B70;

    constexpr std::uintptr_t kFunc_ExtractContactSignalPoints = 0x175C650;

    /*
     * BSReadWriteLock read-side pair used with kHknpWorld_AccessLock. Raw-disasm
     * verified 2026-07-13: LockForRead CAS-increments the reader count while no
     * writer bit (0x80000000) is set and is recursive for the writer-owning
     * thread; UnlockForRead decrements. 40+ engine callers share the pair.
     */
    constexpr std::uintptr_t kFunc_BSReadWriteLock_LockForRead = 0x1B932B0;

    constexpr std::uintptr_t kFunc_BSReadWriteLock_UnlockForRead = 0x1B93570;
    /*
     * ---- Heisenberg-preserved offsets (phase-2 wholesale sync) --------------
     * Symbols present in Heisenberg's reconstructed native layer but absent from
     * upstream's published HavokOffsets.h. All names are disjoint from upstream's
     * set, so this block is purely additive. Sources: Ghidra RE of FO4VR 1.2.72
     * (hknpMaterial / hknpMotionProperties field layouts, material-library and
     * motion-property-library mutators, bhkWorld::SetDeltaTime timing-fix hook).
     */
    inline constexpr std::uintptr_t TODO_RE = 0;

    constexpr std::uintptr_t kData_HavokMemoryRouterTlsIndex = 0x5B63B20;
    /*
     * CONFIRMED 2026-07-28 (Ghidra, F4VR 1.2.72):
     *   0x140D84BD0  E8 4B 25 07 01  CALL 0x141DF7120   (rel32 = 0x0107254B)
     *   ...inside Main::OnIdle_UpdateTimer, entry 0x140D84B40; next instruction is
     *   CMP byte ptr [RBX+0x1D0],0x0.
     *   0x141DF7120 is the entry of bhkWorld::SetDeltaTime (48 89 5C 24 10 57 48 83 EC 40)
     *   and has exactly 5 xrefs, one of which is 0x140D84BD0.
     * These duplicate the private copies in core/PhysicsHooks.cpp; keep both in sync.
     */
    constexpr std::uintptr_t kFunc_BhkWorldSetDeltaTime = 0x1DF7120;
    constexpr std::uintptr_t kHookSite_BhkWorldSetDeltaTimeMainCall = 0x0D84BD0;
    constexpr std::uintptr_t kFunc_MaterialLibrary_AddEntry = 0x1537840;
    constexpr std::uintptr_t kFunc_MaterialLibrary_FindEntryByName = 0x1537BC0;
    constexpr std::uintptr_t kFunc_MaterialLibrary_UpdateEntry = 0x1537A70;
    constexpr std::uintptr_t kFunc_MotionPropertiesLibrary_UpdateEntry = 0x1767BF0;
    constexpr std::uintptr_t kFunc_MotionProperties_SetPreset = 0x1841D30;
    constexpr std::uintptr_t kFunc_World_SetBodyMaterial = 0x153AFC0;
    constexpr std::uintptr_t kHknpWorld_MaterialLibraryPtr = 0x5C8;

    // hknpMaterial field layout (see memory: hknp material/motionproperties layouts)
    constexpr std::uintptr_t kMaterial_DynamicFriction = 0x12;
    constexpr std::uintptr_t kMaterial_StaticFriction = 0x14;
    constexpr std::uintptr_t kMaterial_Restitution = 0x16;
    constexpr std::uintptr_t kMaterial_FrictionCombinePolicy = 0x18;
    constexpr std::uintptr_t kMaterial_RestitutionCombinePolicy = 0x19;

    // hknpMotionProperties field layout
    constexpr std::uintptr_t kMotionProperties_GravityFactor = 0x08;
    constexpr std::uintptr_t kMotionProperties_MaxLinearSpeed = 0x10;
    constexpr std::uintptr_t kMotionProperties_MaxAngularSpeed = 0x14;
}
