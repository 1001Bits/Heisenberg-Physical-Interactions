#pragma once

#include <cstring>  // For std::memcpy

#include "RE/Fallout.h"
#include "Selection.h"

namespace heisenberg
{
    // =========================================================================
    // HAVOK NEW PHYSICS CONSTRAINT TYPES
    // =========================================================================
    
    /**
     * hknpConstraintId - Handle to a constraint in the hknp world
     * Invalid value is 0x7FFFFFFF
     */
    struct hknpConstraintId
    {
        std::uint32_t m_value = 0x7FFFFFFF;
        
        bool IsValid() const { return m_value != 0x7FFFFFFF; }
        void Invalidate() { m_value = 0x7FFFFFFF; }
        
        bool operator==(const hknpConstraintId& other) const { return m_value == other.m_value; }
        bool operator!=(const hknpConstraintId& other) const { return m_value != other.m_value; }
    };
    static_assert(sizeof(hknpConstraintId) == 0x04);

    // =========================================================================
    // (Two-scenario cleanup: the HIGGS-ported 6-DOF motor constraint machinery —
    // hkp motor/atom struct definitions, GrabConstraintData, GrabConstraintVtable,
    // ConstraintGrabState/Manager and MotorHelpers — was removed together with
    // grab modes 1/2 and the HeldBodyGrab system. The hknp construction-info
    // structs and the ConstraintFunctions REL vocabulary below survive: they are
    // shared by Physics.cpp, HandCollision, PlayerCharacterProxyListener and the
    // keyframed grab path.)
    // =========================================================================

    /**
     * hkpConstraintMotor - Base class for constraint motors (kept: referenced by
     * ConstraintFunctions::RagdollSetMotor / LimitedHingeSetMotor signatures)
     * Size: 0x18
     */
    struct hkpConstraintMotor
    {
        void* vtable;                   // 0x00 - vtable pointer
        std::uint16_t memSizeAndFlags;  // 0x08 - hkReferencedObject
        std::uint16_t referenceCount;   // 0x0A
        std::uint32_t pad0C;            // 0x0C
        std::uint8_t type;              // 0x10 - MotorType enum
        std::uint8_t pad11[7];          // 0x11

        enum MotorType : std::uint8_t
        {
            TYPE_INVALID = 0,
            TYPE_POSITION = 1,
            TYPE_VELOCITY = 2,
            TYPE_SPRING_DAMPER = 3,
            TYPE_CALLBACK = 4
        };
    };
    static_assert(sizeof(hkpConstraintMotor) == 0x18);

    /**
     * hknpConstraintCinfo - Construction info for creating constraints
     * The hknp system accepts old hkpConstraintData types!
     */
    struct hknpConstraintCinfo
    {
        void* constraintData;       // 0x00 - Pointer to hkpConstraintData (ball socket, etc.)
        std::uint32_t bodyIdA;      // 0x08 - First body ID
        std::uint32_t bodyIdB;      // 0x0C - Second body ID
        std::uint8_t flags;         // 0x10 - Constraint flags
        std::uint8_t pad11[7];      // 0x11 - Padding
    };
    static_assert(sizeof(hknpConstraintCinfo) == 0x18);

    /**
     * hkpBallAndSocketConstraintData - Simple ball-socket constraint
     * This is the old hkp type but still used by hknp internally
     */
    struct hkpBallAndSocketConstraintData
    {
        // We don't need to define the full structure - we use function pointers
        // Just reserve enough space for the constraint data
        std::uint8_t data[0x80];
    };

    /**
     * hknpBodyCinfo - Construction info for creating physics bodies
     * Based on r52/CommonLibF4 and soulstruct-havok reverse engineering
     * 
     * This structure is used by hknpWorld::createBody to create new bodies.
     * The default constructor at 0x1561dd0 (VR) initializes it with defaults.
     * 
     * Size: 0x60 (96 bytes) - 16-byte aligned
     * From: https://github.com/r52/CommonLibF4/blob/master/CommonLibF4/include/RE/Havok/hknpBodyCinfo.h
     */
    struct alignas(16) hknpBodyCinfo
    {
        // Offset 0x00
        const void* shape;                     // hknpShape* - The collision shape
        
        // Offset 0x08
        std::uint32_t reservedBodyId;          // hknpBodyId - Reserved body ID (usually 0xFFFFFFFF)
        
        // Offset 0x0C  
        std::uint32_t motionId;                // hknpMotionId - Motion ID (usually 0xFFFFFFFF)
        
        // Offset 0x10
        std::uint8_t qualityId;                // hknpBodyQualityId
        std::uint8_t pad11;
        
        // Offset 0x12
        std::uint16_t materialId;              // hknpMaterialId
        
        // Offset 0x14
        std::uint32_t collisionFilterInfo;     // Collision layer/group info
        
        // Offset 0x18
        std::int32_t flags;                    // hknpCollisionFlags
        
        // Offset 0x1C
        float collisionLookAheadDistance;      // Look-ahead for CCD
        
        // Offset 0x20
        const char* name;                      // hkStringPtr - Optional name
        
        // Offset 0x28
        std::uint64_t userData;                // User data
        
        // Offset 0x30 (must be 16-byte aligned)
        RE::NiPoint4 position;                 // hkVector4f - World position
        
        // Offset 0x40 (must be 16-byte aligned)
        RE::NiPoint4 orientation;              // hkQuaternionf (x, y, z, w)
        
        // Offset 0x50
        std::uint8_t spuFlags;                 // SPU processing flags
        std::uint8_t pad51[7];                 // Padding
        
        // Offset 0x58
        void* localFrame;                      // hkRefPtr<hkLocalFrame>
        
        // Default constructor - zeroes everything and sets identity orientation
        hknpBodyCinfo()
        {
            std::memset(this, 0, sizeof(*this));
            reservedBodyId = 0xFFFFFFFF;       // Invalid body ID
            motionId = 0xFFFFFFFF;             // Invalid motion ID  
            orientation.w = 1.0f;              // Identity quaternion (x=0, y=0, z=0, w=1)
        }
    };
    static_assert(sizeof(hknpBodyCinfo) == 0x60, "hknpBodyCinfo size mismatch - should be 96 bytes");

    // Motion property IDs - controls physics behavior
    namespace hknpMotionPropertiesId
    {
        enum Preset : std::uint8_t
        {
            STATIC = 0,
            DYNAMIC = 1,
            KEYFRAMED = 2,
            // Higher values for game-specific presets
        };
    }

    // =========================================================================
    // hknpPhysicsSystemData - Contains body/motion/constraint construction info
    // Based on reverse engineering from working Havok 2014 code
    // =========================================================================
    
    /**
     * hkArray<T> - Havok's dynamic array template
     * Size: 0x10 (16 bytes)
     */
    template<typename T>
    struct hkArray
    {
        T* data;                    // 0x00 - Pointer to data
        std::int32_t size;          // 0x08 - Number of elements
        std::int32_t capacityAndFlags; // 0x0C - Capacity with flags in high bits
        
        T& operator[](int index) { return data[index]; }
        const T& operator[](int index) const { return data[index]; }
    };
    static_assert(sizeof(hkArray<int>) == 0x10, "hkArray size mismatch");

    /**
     * hknpMotionCinfo - Construction info for creating physics motions
     * Size: estimate 0x80 bytes based on typical Havok patterns
     */
    struct alignas(16) hknpMotionCinfo
    {
        std::uint8_t data[0x80];  // Opaque - exact layout unknown
        
        hknpMotionCinfo() { std::memset(this, 0, sizeof(*this)); }
    };

    // Note: hknpConstraintCinfo is defined earlier in this file (around line 357)

    /**
     * hknpMaterialDescriptor - Material properties
     * Size: estimate 0x20 bytes
     */
    struct hknpMaterialDescriptor
    {
        std::uint8_t data[0x20];  // Opaque - exact layout unknown
        
        hknpMaterialDescriptor() { std::memset(this, 0, sizeof(*this)); }
    };

    /**
     * hknpMotionPropertiesCinfo - Motion properties construction info
     * Size: estimate 0x40 bytes
     */
    struct hknpMotionPropertiesCinfo
    {
        std::uint8_t data[0x40];  // Opaque - exact layout unknown
        
        hknpMotionPropertiesCinfo() { std::memset(this, 0, sizeof(*this)); }
    };

    /**
     * hknpPhysicsSystemData - Contains all data needed to create a physics system
     * 
     * Based on reverse engineering from Havok 2014 SDK sample code:
     *   pSystemData->m_bodyCinfos[0].m_collisionFilterInfo
     *   pSystemData->m_bodyCinfos[0].m_flags  
     *   pSystemData->m_bodyCinfos[0].m_motionType
     * 
     * This structure inherits from hkReferencedObject (size 0x10)
     * 
     * Estimated total size: ~0x90 bytes
     */
    struct hknpPhysicsSystemData
    {
        // hkReferencedObject base: 0x00 - 0x0F (vtable + refcount + memsize)
        void* vtable;                           // 0x00
        std::int16_t memSizeAndRefCount;        // 0x08
        std::int16_t pad0A;
        std::int32_t pad0C;
        
        // Arrays of construction info
        hkArray<hknpMaterialDescriptor> materials;       // 0x10
        hkArray<hknpMotionPropertiesCinfo> motionProperties; // 0x20
        hkArray<hknpMotionCinfo> motionCinfos;           // 0x30
        hkArray<hknpBodyCinfo> bodyCinfos;               // 0x40 - CONFIRMED by sample code
        hkArray<hknpConstraintCinfo> constraintCinfos;   // 0x50
        
        // Padding/additional data - exact layout unknown
        std::uint8_t additionalData[0x30];              // 0x60
        
        // Default constructor - just zero everything
        hknpPhysicsSystemData()
        {
            std::memset(this, 0, sizeof(*this));
        }
    };
    static_assert(sizeof(hknpPhysicsSystemData) == 0x90, "hknpPhysicsSystemData size estimate");

    // =========================================================================
    // CONSTRAINT FUNCTION POINTERS
    // =========================================================================

    namespace ConstraintFunctions
    {
        // hkpBallAndSocketConstraintData constructor
        // VR offset: 0x19af690 - Status 4 (Verified)
        using BallSocketCtor_t = void(*)(hkpBallAndSocketConstraintData*);
        inline REL::Relocation<BallSocketCtor_t> BallSocketCtor{ REL::Offset(0x19af690) };

        // hkpBallAndSocketConstraintData::setInBodySpace
        // VR offset: 0x19af6e0 - Status 4 (Verified)
        using BallSocketSetInBodySpace_t = void(*)(hkpBallAndSocketConstraintData*,
                                                    const RE::NiPoint4& pivotA,
                                                    const RE::NiPoint4& pivotB);
        inline REL::Relocation<BallSocketSetInBodySpace_t> BallSocketSetInBodySpace{ REL::Offset(0x19af6e0) };

        // hknpWorld::createConstraint
        // VR offset: 0x15469b0 - Status 4 (Verified)
        // Uses output parameter pattern (like createBody)
        using CreateConstraint_t = hknpConstraintId*(*)(void* hknpWorld, hknpConstraintId* outId,
                                                         const hknpConstraintCinfo* cinfo);
        inline REL::Relocation<CreateConstraint_t> CreateConstraint{ REL::Offset(0x15469b0) };

        // hknpWorld::destroyConstraints
        // VR offset: 0x1546b40 - Status 4 (Verified)
        using DestroyConstraints_t = void(*)(void* hknpWorld, const hknpConstraintId* ids, int count);
        inline REL::Relocation<DestroyConstraints_t> DestroyConstraints{ REL::Offset(0x1546b40) };

        // hknpBSWorld::addConstraintBodyMap
        // VR offset: 0x1df66d0 - Status 4 (Verified)
        using AddConstraintBodyMap_t = void(*)(void* hknpBSWorld, hknpConstraintId id,
                                                const hknpConstraintCinfo* cinfo);
        inline REL::Relocation<AddConstraintBodyMap_t> AddConstraintBodyMap{ REL::Offset(0x1df66d0) };

        // hknpBSWorld::removeConstraintBodyMap
        // VR offset: 0x1df6720 - Status 4 (Verified)
        using RemoveConstraintBodyMap_t = void(*)(void* hknpBSWorld, std::uint64_t unused,
                                                   std::uint32_t constraintId);
        inline REL::Relocation<RemoveConstraintBodyMap_t> RemoveConstraintBodyMap{ REL::Offset(0x1df6720) };

        // =====================================================================
        // BODY CREATION FUNCTIONS
        // =====================================================================
        
        /**
         * hknpConvexShape::BuildConfig - Configuration for creating convex shapes
         * 
         * Based on Havok SDK patterns - this is initialized by the game's constructor
         * at offset 0x1416d4ab0 (VR). The exact structure size is uncertain but
         * we allocate enough space and let the constructor initialize it.
         * 
         * Size estimate: 0x30 (48 bytes) based on typical Havok config structs
         */
        struct hknpConvexShapeBuildConfig
        {
            std::uint8_t data[0x30];  // Opaque data - initialized by constructor
            
            // Default constructor - should be called before use
            hknpConvexShapeBuildConfig() { std::memset(data, 0, sizeof(data)); }
        };
        
        // hknpConvexShape::BuildConfig::BuildConfig(void) - Default constructor
        // VR offset: 0x16d4ab0 - Status 4 (Verified)
        // This initializes the BuildConfig with default values
        using BuildConfigCtor_t = void(*)(hknpConvexShapeBuildConfig*);
        inline REL::Relocation<BuildConfigCtor_t> BuildConfigCtor{ REL::Offset(0x16d4ab0) };
        
        // hknpConvexShape::createFromHalfExtents(hkVector4f& halfExtents, float radius, BuildConfig&)
        // VR offset: 0x16d57c0 - Status 4 (Verified)
        // Returns hknpConvexShape*
        using CreateConvexShapeFromHalfExtents_t = void*(*)(const RE::NiPoint4& halfExtents, float radius, hknpConvexShapeBuildConfig* buildConfig);
        inline REL::Relocation<CreateConvexShapeFromHalfExtents_t> CreateConvexShapeFromHalfExtents{ REL::Offset(0x16d57c0) };

        // hkAabb structure - axis-aligned bounding box
        struct hkAabb {
            RE::NiPoint4 min;
            RE::NiPoint4 max;
        };

        // hknpConvexShape::createFromAabb(hkAabb& aabb, float radius, BuildConfig& config)
        // VR offset: 0x16d5800 - Status 4 (Verified)
        using CreateConvexShapeFromAabb_t = void*(*)(const hkAabb& aabb, float radius, hknpConvexShapeBuildConfig* buildConfig);
        inline REL::Relocation<CreateConvexShapeFromAabb_t> CreateConvexShapeFromAabb{ REL::Offset(0x16d5800) };

        // hkStridedVertices — Havok strided vertex-array descriptor.
        // Ghidra-verified layout (createFromVertices reads numVertices at +0x8, builds an internal
        // 16-byte/hkVector4 buffer via SHL 4): { ptr(+0), int numVertices(+8), int striding(+0xC) }.
        // striding = BYTES between consecutive source vertices (16 for hkVector4-packed x,y,z,w).
        struct hkStridedVertices {
            const float* vertices = nullptr;
            std::int32_t numVertices = 0;
            std::int32_t striding = 16;
        };

        // hknpConvexShape::createFromVertices(hkStridedVertices&, float radius, BuildConfig&)
        // VR offset: 0x16d4b30 - Ghidra-verified May 31 2026 (sibling of createFromHalfExtents/Aabb;
        // RCX=verts&, XMM1=radius, R8=BuildConfig&, returns hknpConvexShape*). Builds the convex
        // hull internally (hkgpConvexHull) from the supplied point cloud.
        using CreateConvexShapeFromVertices_t = void*(*)(const hkStridedVertices& verts, float radius, hknpConvexShapeBuildConfig* buildConfig);
        inline REL::Relocation<CreateConvexShapeFromVertices_t> CreateConvexShapeFromVertices{ REL::Offset(0x16d4b30) };
        
        // hknpWorld::createBody(hknpBodyCinfo&, AdditionMode, AdditionFlags)
        // VR offset: 0x1543ff0 - Status 4 (Verified)
        // From PDB: hknpWorld::createBody(hknpBodyCinfo&,hknpWorld::AdditionMode,hkFlags<hknpWorld::AdditionFlagsEnum,uchar>)
        // Returns pointer to output param (outBodyId)
        // CRITICAL: Uses OUTPUT PARAMETER for body ID, NOT return value!
        // NOTE: This is a MEMBER function - first param is 'this' (hknpWorld*)
        using CreateBody_t = std::uint32_t*(__fastcall*)(void* thisWorld, std::uint32_t* outBodyId,
                                                          const hknpBodyCinfo* bodyCinfo, int additionMode, int additionFlags);
        inline REL::Relocation<CreateBody_t> CreateBody{ REL::Offset(0x1543ff0) };

        // hknpBodyCinfo::hknpBodyCinfo(void) - Default constructor
        // VR offset: 0x1561dd0 - Status 4 (Verified)
        // This initializes the structure with default values
        using BodyCinfoCtor_t = void(__fastcall*)(hknpBodyCinfo* thisCinfo);
        inline REL::Relocation<BodyCinfoCtor_t> BodyCinfoCtor{ REL::Offset(0x1561dd0) };
        
        // hknpWorld::destroyBodies
        // VR offset: 0x1544e80 - Status 4 (Verified)
        // NOTE: This is a MEMBER function - first param is 'this' (hknpWorld*)
        using DestroyBodies_t = void(__fastcall*)(void* thisWorld, const std::uint32_t* bodyIds, int count, int activationMode);
        inline REL::Relocation<DestroyBodies_t> DestroyBodies{ REL::Offset(0x1544e80) };

        // hknpWorld::commitAddBodies(void)
        // VR offset: 0x1544a50 - Status 4 (Verified)
        using CommitAddBodies_t = void(__fastcall*)(void* thisWorld);
        inline REL::Relocation<CommitAddBodies_t> hknpWorld_commitAddBodies{ REL::Offset(0x1544a50) };

        // hknpWorld::activateBody(hknpBodyId)
        // VR offset: 0x1546ef0 - Status 4 (Verified)
        using ActivateBody_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId);
        inline REL::Relocation<ActivateBody_t> hknpWorld_activateBody{ REL::Offset(0x1546ef0) };

        // NOTE: bhkPhysicsSystem_CreateInstance is a duplicate of BhkPhysicsSystemCreateInstance (same offset 0x1e0c320)
        // NOTE: bhkPhysicsSystem_Ctor is a duplicate of BhkPhysicsSystemCtor (same offset 0x1e0c2b0)
        // NOTE: hknpPhysicsSystem_Ctor is a duplicate of PhysicsSystemCtor (same offset 0x1564de0)
        // NOTE: hknpPhysicsSystem_AddToWorld is a duplicate of PhysicsSystemAddToWorld (same offset 0x1565770)
        // =====================================================================
        // BETHESDA WRAPPER APPROACH - More reliable than direct hknpWorld calls
        // =====================================================================

        // bhkPhysicsSystem::CreateInstance(bhkWorld&, hkTransformf&)
        // VR offset: 0x1e0c320 - Status 4 (Verified)
        // DUPLICATE of BhkPhysicsSystemCreateInstance below
        using bhkPhysicsSystem_CreateInstance_t = void*(__fastcall*)(void* thisBhkPhysicsSystem, void* bhkWorld, const void* transform);
        inline REL::Relocation<bhkPhysicsSystem_CreateInstance_t> bhkPhysicsSystem_CreateInstance{ REL::Offset(0x1e0c320) };

        // bhkPhysicsSystem::bhkPhysicsSystem(hknpPhysicsSystemData&)
        // VR offset: 0x1e0c2b0 - Status 4 (Verified)
        // DUPLICATE of BhkPhysicsSystemCtor below
        using bhkPhysicsSystem_Ctor_t = void(__fastcall*)(void* thisBhkPhysicsSystem, const void* physicsSystemData);
        inline REL::Relocation<bhkPhysicsSystem_Ctor_t> bhkPhysicsSystem_Ctor{ REL::Offset(0x1e0c2b0) };

        // hknpPhysicsSystem::hknpPhysicsSystem(hknpPhysicsSystemData*, hknpWorld*, hkTransformf&, AdditionMode, AdditionFlags, Flags)
        // VR offset: 0x1564de0 - Status 4 (Verified)
        // DUPLICATE of PhysicsSystemCtor below
        using hknpPhysicsSystem_Ctor_t = void(__fastcall*)(void* thisSystem, const void* systemData, void* world, const void* transform, int additionMode, int additionFlags, int flags);
        inline REL::Relocation<hknpPhysicsSystem_Ctor_t> hknpPhysicsSystem_Ctor{ REL::Offset(0x1564de0) };

        // hknpPhysicsSystem::addToWorld(AdditionMode, AdditionFlags, ActivationMode)
        // VR offset: 0x1565770 - Status 4 (Verified)
        // DUPLICATE of PhysicsSystemAddToWorld below
        using hknpPhysicsSystem_AddToWorld_t = void(__fastcall*)(void* thisSystem, int additionMode, int additionFlags, int activationMode);
        inline REL::Relocation<hknpPhysicsSystem_AddToWorld_t> hknpPhysicsSystem_AddToWorld{ REL::Offset(0x1565770) };

        // =====================================================================
        // BODY MOVEMENT FUNCTIONS
        // =====================================================================

        // hknpBSWorld::applyHardKeyFrame(hknpBodyId, hkVector4f& position, hkQuaternionf& orientation, float invDeltaTime)
        // VR offset: 0x1df5930 - Status 4 (Verified)
        // This moves a keyframed body to a specific position/orientation with proper velocity
        using ApplyHardKeyFrameBodyId_t = void(*)(void* hknpBSWorld, std::uint32_t bodyId,
                                                   const RE::NiPoint4& position,
                                                   const RE::NiPoint4& orientation,
                                                   float invDeltaTime);
        inline REL::Relocation<ApplyHardKeyFrameBodyId_t> ApplyHardKeyFrameBodyId{ REL::Offset(0x1df5930) };

        // hknpBSWorld::setBodyTransform(hknpBodyId, hkTransformf&, hknpActivationBehavior::Enum)
        // VR offset: 0x1df55f0 - Status 4 (Verified)
        // Alternative: directly sets transform without velocity calculation
        using SetBodyTransform_t = void(*)(void* hknpBSWorld, std::uint32_t bodyId,
                                            const RE::NiTransform& transform, int activationBehavior);
        inline REL::Relocation<SetBodyTransform_t> SetBodyTransform{ REL::Offset(0x1df55f0) };

        // =====================================================================
        // MOTOR CONSTRAINT FUNCTIONS
        // For implementing 6-DOF motor-based grab constraints like Skyrim HIGGS
        // =====================================================================

        // hkpRagdollConstraintData::setMotor(MotorIndex, hkpConstraintMotor*)
        // VR offset: 0x1419b2520 - Status 4 (Verified)
        // Sets motor for a specific axis of the ragdoll constraint
        using RagdollSetMotor_t = void(*)(void* ragdollConstraintData, int motorIndex, hkpConstraintMotor* motor);
        inline REL::Relocation<RagdollSetMotor_t> RagdollSetMotor{ REL::Offset(0x19b2520) };

        // hkpLimitedHingeConstraintData::setMotor(hkpConstraintMotor*)
        // VR offset: 0x1419ad060 - Status 4 (Verified)
        using LimitedHingeSetMotor_t = void(*)(void* hingeConstraintData, hkpConstraintMotor* motor);
        inline REL::Relocation<LimitedHingeSetMotor_t> LimitedHingeSetMotor{ REL::Offset(0x19ad060) };

        // hknpRagdollMotorController::updateMotors(void)
        // VR offset: 0x141af7260 - Status 4 (Verified)
        using RagdollMotorControllerUpdate_t = void(*)(void* controller);
        inline REL::Relocation<RagdollMotorControllerUpdate_t> RagdollMotorControllerUpdate{ REL::Offset(0x1af7260) };
        
        // hkpPositionConstraintMotor vtable - for manually creating motors
        // VR offset: 0x2e95fe8 - Status 3
        // Use this to set the vtable when allocating hkpPositionConstraintMotor manually
        inline constexpr std::uintptr_t PositionConstraintMotorVtable = 0x2e95fe8;  // Relative to module base
        
        // hkpRagdollConstraintData vtable - for creating ragdoll constraints
        // VR offset: 0x2e18298 (VERIFIED via Ghidra - constructor assigns this to vtable)
        inline constexpr std::uintptr_t RagdollConstraintDataVtable = 0x2e18298;  // Relative to module base

        // hkpRagdollConstraintData::hkpRagdollConstraintData() - Default constructor
        // VR offset: 0x19b1d50 - Creates ragdoll constraint with default settings
        using RagdollConstraintDataCtor_t = void*(*)(void* constraintData);
        inline REL::Relocation<RagdollConstraintDataCtor_t> RagdollConstraintData_ctor{ REL::Offset(0x19b1d50) };

        // hkpRagdollConstraintData::setInBodySpace(pivotA, pivotB, planeA, planeB, twistA, twistB)
        // VR offset: 0x19b21d0
        using RagdollSetInBodySpace_t = void(*)(void* constraintData,
                                                 const RE::NiPoint4* pivotA, const RE::NiPoint4* pivotB,
                                                 const RE::NiPoint4* planeA, const RE::NiPoint4* planeB,
                                                 const RE::NiPoint4* twistA, const RE::NiPoint4* twistB);
        inline REL::Relocation<RagdollSetInBodySpace_t> RagdollSetInBodySpace{ REL::Offset(0x19b21d0) };
        
        // hkpGenericConstraintData vtable - for creating generic 6-DOF constraints
        // VR offset: 0x2e8fb38 (from vr_address_tools database) - Status 3
        // The generic constraint can have any combination of linear/angular motors
        inline constexpr std::uintptr_t GenericConstraintDataVtable = 0x2e8fb38;  // Relative to module base
        
        // =====================================================================
        // HAVOK REFERENCE COUNTING
        // =====================================================================

        // hkReferencedObject::addReference(void) - Increments reference count
        // VR offset: 0x5a400 - Status 4 (Verified), ID 866015
        using HkAddReference_t = void(*)(void* hkReferencedObject);
        inline REL::Relocation<HkAddReference_t> hkReferencedObject_addReference{ REL::Offset(0x5a400) };

        // hkReferencedObject::removeReference(void) - Decrements reference count, may delete
        // VR offset: 0x27f50 - Status 4 (Verified), ID 1379897
        using HkRemoveReference_t = void(*)(void* hkReferencedObject);
        inline REL::Relocation<HkRemoveReference_t> hkReferencedObject_removeReference{ REL::Offset(0x27f50) };

        // =====================================================================
        // HAVOK MATRIX OPERATIONS
        // =====================================================================

        // hkMatrix3f::setMul(hkMatrix3f&, hkMatrix3f&) - Matrix multiplication
        // VR offset: 0x17cf420 - Status 2, ID 1296037
        using HkMatrix3fSetMul_t = void(*)(void* result, const void* a, const void* b);
        inline REL::Relocation<HkMatrix3fSetMul_t> hkMatrix3f_setMul{ REL::Offset(0x17cf420) };

        // =====================================================================
        // HAVOK MEMORY ALLOCATION
        // For proper motor allocation like Skyrim HIGGS
        // =====================================================================

        // hkContainerHeapAllocator::Allocator::blockAlloc(int) - Allocates from Havok heap
        // VR offset: 0x158bd90 - Status 2, ID 99620
        using HkBlockAlloc_t = void*(*)(void* allocator, int numBytes);
        inline REL::Relocation<HkBlockAlloc_t> hkContainerHeapAllocator_blockAlloc{ REL::Offset(0x158bd90) };
        
        // =====================================================================
        // HAVOK MEMORY ROUTER ACCESS
        // The TLS index for hkMemoryRouter is stored at a global address.
        // From CommonLibF4VR: REL::RelocationID(878080, 2787927) for VR = 0x5b63b20
        // =====================================================================
        
        /**
         * Get the Havok memory router for the current thread
         * Uses TLS (Thread Local Storage) to get the thread-specific router
         * @return Pointer to hkMemoryRouter, or nullptr if not initialized
         */
        inline void* GetHkMemoryRouter()
        {
            // The TLS index is stored at offset 0x5b63b20 (VR address)
            static REL::Relocation<std::uint32_t*> tlsSlot{ REL::Offset(0x5b63b20) };
            std::uint32_t tlsIndex = *tlsSlot;
            if (tlsIndex == 0 || tlsIndex == 0xFFFFFFFF)
            {
                return nullptr;
            }
            return TlsGetValue(tlsIndex);
        }
        
        /**
         * Get the heap allocator from the memory router
         * The heap member is at offset 0x58 in hkMemoryRouter (from CommonLibF4VR)
         * @param router The memory router from GetHkMemoryRouter()
         * @return Pointer to hkMemoryAllocator for heap allocations
         */
        inline void* GetHkHeapAllocator(void* router)
        {
            if (!router) return nullptr;
            // hkMemoryRouter::heap is at offset 0x58
            return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(router) + 0x58);
        }
        
        /**
         * Allocate memory from the Havok heap
         * @param numBytes Number of bytes to allocate
         * @return Allocated memory, or nullptr on failure
         */
        inline void* HkHeapAlloc(int numBytes)
        {
            void* router = GetHkMemoryRouter();
            if (!router) return nullptr;
            
            void* heap = GetHkHeapAllocator(router);
            if (!heap) return nullptr;
            
            // Call virtual function BlockAlloc (vtable index 1)
            // vtable layout: [0] = destructor, [1] = BlockAlloc, [2] = BlockFree, ...
            using BlockAllocFn = void*(*)(void* thisPtr, std::int32_t numBytes);
            void** vtable = *reinterpret_cast<void***>(heap);
            auto blockAlloc = reinterpret_cast<BlockAllocFn>(vtable[1]);
            return blockAlloc(heap, numBytes);
        }
        
        /**
         * Free memory to the Havok heap
         * @param ptr Pointer to free
         * @param numBytes Size of allocation (must match allocation size)
         */
        inline void HkHeapFree(void* ptr, int numBytes)
        {
            if (!ptr) return;
            
            void* router = GetHkMemoryRouter();
            if (!router) return;
            
            void* heap = GetHkHeapAllocator(router);
            if (!heap) return;
            
            // Call virtual function BlockFree (vtable index 2)
            using BlockFreeFn = void(*)(void* thisPtr, void* ptr, std::int32_t numBytes);
            void** vtable = *reinterpret_cast<void***>(heap);
            auto blockFree = reinterpret_cast<BlockFreeFn>(vtable[2]);
            blockFree(heap, ptr, numBytes);
        }
        
        /**
         * Allocate a Havok referenced object on the Havok heap
         * Sets up memSizeAndFlags for proper deallocation via removeReference
         * @tparam T Type deriving from hkReferencedObject
         * @return Allocated object (uninitialized except header), or nullptr on failure
         */
        template<typename T>
        inline T* HkAllocReferencedObject()
        {
            constexpr int size = sizeof(T);
            T* obj = static_cast<T*>(HkHeapAlloc(size));
            if (obj)
            {
                // Set memSizeAndFlags so removeReference knows how much to free
                // memSizeAndFlags is at offset 0x08 for hkReferencedObject
                *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uintptr_t>(obj) + 0x08) = 
                    static_cast<std::uint16_t>(size);
            }
            return obj;
        }
        
        inline bool AreConstraintFunctionsAvailable()
        {
            // All offsets are Status 4 (Verified), so they should work
            return true;
        }
        
        // NOTE: PhysicsSystemCtor is a duplicate of hknpPhysicsSystem_Ctor (same offset 0x1564de0)
        // NOTE: PhysicsSystemAddToWorld is a duplicate of hknpPhysicsSystem_AddToWorld (same offset 0x1565770)
        // NOTE: BhkPhysicsSystemCtor is a duplicate of bhkPhysicsSystem_Ctor (same offset 0x1e0c2b0)
        // NOTE: BhkPhysicsSystemCreateInstance is a duplicate of bhkPhysicsSystem_CreateInstance (same offset 0x1e0c320)
        // =====================================================================
        // hknpPhysicsSystem / hknpPhysicsSystemData FUNCTIONS
        // For creating bodies via the proper Havok pattern (like in the SDK)
        // =====================================================================

        // hknpPhysicsSystemData::hknpPhysicsSystemData(void) - Default constructor
        // VR offset: 0x5eab0 - Status 4 (Verified)
        using PhysicsSystemDataCtor_t = void(__fastcall*)(void* thisSystemData);
        inline REL::Relocation<PhysicsSystemDataCtor_t> PhysicsSystemDataCtor{ REL::Offset(0x5eab0) };

        // hknpPhysicsSystemData::~hknpPhysicsSystemData(void) - Destructor
        // VR offset: 0x15622c0 - Status 4 (Verified)
        using PhysicsSystemDataDtor_t = void(__fastcall*)(void* thisSystemData);
        inline REL::Relocation<PhysicsSystemDataDtor_t> PhysicsSystemDataDtor{ REL::Offset(0x15622c0) };

        // hknpPhysicsSystem::hknpPhysicsSystem(hknpPhysicsSystemData*, hknpWorld*, hkTransformf&, AdditionMode, AdditionFlags, Flags)
        // VR offset: 0x1564de0 - Status 4 (Verified)
        // This creates bodies from the system data
        // DUPLICATE of hknpPhysicsSystem_Ctor above
        using PhysicsSystemCtor_t = void(__fastcall*)(void* thisSystem, void* systemData, void* world,
                                                       const void* transform, int additionMode,
                                                       std::uint8_t additionFlags, int flags);
        inline REL::Relocation<PhysicsSystemCtor_t> PhysicsSystemCtor{ REL::Offset(0x1564de0) };

        // hknpPhysicsSystem::~hknpPhysicsSystem(void) - Destructor
        // VR offset: 0x15653f0 - Status 4 (Verified)
        using PhysicsSystemDtor_t = void(__fastcall*)(void* thisSystem);
        inline REL::Relocation<PhysicsSystemDtor_t> PhysicsSystemDtor{ REL::Offset(0x15653f0) };

        // hknpPhysicsSystem::addToWorld(AdditionMode, AdditionFlags, ActivationMode)
        // VR offset: 0x1565770 - Status 4 (Verified)
        // DUPLICATE of hknpPhysicsSystem_AddToWorld above
        using PhysicsSystemAddToWorld_t = void(__fastcall*)(void* thisSystem, int additionMode,
                                                             std::uint8_t additionFlags, int activationMode);
        inline REL::Relocation<PhysicsSystemAddToWorld_t> PhysicsSystemAddToWorld{ REL::Offset(0x1565770) };

        // hknpPhysicsSystem::removeFromWorld(void)
        // VR offset: 0x1565a00 - Status 4 (Verified)
        using PhysicsSystemRemoveFromWorld_t = void(__fastcall*)(void* thisSystem);
        inline REL::Relocation<PhysicsSystemRemoveFromWorld_t> PhysicsSystemRemoveFromWorld{ REL::Offset(0x1565a00) };

        // bhkPhysicsSystem::bhkPhysicsSystem(hknpPhysicsSystemData&) - Bethesda wrapper constructor
        // VR offset: 0x1e0c2b0 - Status 4 (Verified)
        // DUPLICATE of bhkPhysicsSystem_Ctor above
        using BhkPhysicsSystemCtor_t = void(__fastcall*)(void* thisSystem, void* systemData);
        inline REL::Relocation<BhkPhysicsSystemCtor_t> BhkPhysicsSystemCtor{ REL::Offset(0x1e0c2b0) };

        // bhkPhysicsSystem::CreateInstance(bhkWorld&, hkTransformf&)
        // VR offset: 0x1e0c320 - Status 4 (Verified)
        // DUPLICATE of bhkPhysicsSystem_CreateInstance above
        using BhkPhysicsSystemCreateInstance_t = void(__fastcall*)(void* thisSystem, void* bhkWorld, const void* transform);
        inline REL::Relocation<BhkPhysicsSystemCreateInstance_t> BhkPhysicsSystemCreateInstance{ REL::Offset(0x1e0c320) };

        // bhkPhysicsSystem::AddToWorld(void)
        // VR offset: 0x1e0c580 - Status 4 (Verified)
        using BhkPhysicsSystemAddToWorld_t = void(__fastcall*)(void* thisSystem);
        inline REL::Relocation<BhkPhysicsSystemAddToWorld_t> BhkPhysicsSystemAddToWorld{ REL::Offset(0x1e0c580) };

        // =====================================================================
        // bhkNPCollisionObject FUNCTIONS
        // =====================================================================

        // bhkNPCollisionObject::bhkNPCollisionObject(uint bodyIndex, bhkPhysicsSystem&)
        // VR offset: 0x1e07710 - Status 4 (Verified)
        using BhkNPCollisionObjectCtor_t = void*(__fastcall*)(void* thisCollObj, std::uint32_t bodyIndex, void* bhkPhysicsSystem);
        inline REL::Relocation<BhkNPCollisionObjectCtor_t> BhkNPCollisionObjectCtor{ REL::Offset(0x1e07710) };

        // bhkNPCollisionObject::AddToWorld(bhkWorld*)
        // VR offset: 0x1e07be0 - Status 4 (Verified)
        using BhkNPCollisionObjectAddToWorld_t = void(__fastcall*)(void* thisCollObj, void* bhkWorld);
        inline REL::Relocation<BhkNPCollisionObjectAddToWorld_t> BhkNPCollisionObjectAddToWorld{ REL::Offset(0x1e07be0) };

        // bhkNPCollisionObject::SetMotionType(int motionType)
        // VR offset: 0x1e07300 - Status 4 (Verified)
        using BhkNPCollisionObjectSetMotionType_t = void(__fastcall*)(void* thisCollObj, int motionType);
        inline REL::Relocation<BhkNPCollisionObjectSetMotionType_t> BhkNPCollisionObjectSetMotionType{ REL::Offset(0x1e07300) };

        // bhkPhysicsSystem::GetBodyId(uint* outBodyId, uint index) - Get body ID at index
        // VR offset: 0x1e0c460 - Status 4 (Verified)
        // NOTE: This writes the result to the outBodyId pointer!
        using BhkPhysicsSystemGetBodyId_t = void(__fastcall*)(void* thisSystem, std::uint32_t* outBodyId, std::uint32_t index);
        inline REL::Relocation<BhkPhysicsSystemGetBodyId_t> BhkPhysicsSystemGetBodyId{ REL::Offset(0x1e0c460) };
        
        // bhkPhysicsSystem::GetNumBodies(void) - Get number of bodies in system
        // VR offset: 0x1e0c780 - Status 4 (Verified)
        using BhkPhysicsSystemGetNumBodies_t = std::uint32_t(__fastcall*)(void* thisSystem);
        inline REL::Relocation<BhkPhysicsSystemGetNumBodies_t> BhkPhysicsSystemGetNumBodies{ REL::Offset(0x1e0c780) };

        // ============================================================================
        // ROCK BethesdaPhysicsBody port — additional REL::Relocations (May 2026)
        // Extracted from ROCK.dll via Ghidra (see BethesdaPhysicsBodyOffsets.h).
        // These five offsets, combined with the existing ones above, give us the full
        // F4VR vocabulary needed to mirror ROCK's BethesdaPhysicsBody::create pipeline.
        // ============================================================================

        // hknpMotionCinfo constructor — called when growing a systemData's MotionCinfos
        // array for non-static bodies. (Static bodies skip this; their motion index is
        // 0x7FFFFFFF.)
        using MotionCinfoCtor_t = void*(__fastcall*)(void* thisMotionCinfo);
        inline REL::Relocation<MotionCinfoCtor_t> MotionCinfoCtor{ REL::Offset(0x17a2fc0) };

        // hknpMaterial constructor — called for each material slot in systemData's
        // Materials array. ROCK uses local material index 0 (single material per body)
        // and overrides the global material id via setBodyMaterial after add-to-world.
        using MaterialCtor_t = void*(__fastcall*)(void* thisMaterial);
        inline REL::Relocation<MaterialCtor_t> MaterialCtor{ REL::Offset(0x1536cb0) };

        // hknpWorld::SetBodyMaterial(world, bodyId, materialId, mode=0) — assigns a
        // world-level material id to a body after add-to-world. Used to swap from
        // local material index 0 to a real world material.
        using HknpWorld_SetBodyMaterial_t = void(__fastcall*)(void* hknpWorld, std::uint32_t bodyId, std::uint16_t materialId, std::int32_t mode);
        inline REL::Relocation<HknpWorld_SetBodyMaterial_t> HknpWorld_SetBodyMaterial{ REL::Offset(0x153afc0) };

        // hknpWorld::SetBodyKeyframed(world, bodyId) — the keyframed motion-type
        // promoter for ROCK-generated bodies. Different from
        // BhkNPCollisionObjectSetMotionType: keyframed driver bodies must be promoted
        // on the hknp body itself, not via the bhk wrapper.
        using HknpWorld_SetBodyKeyframed_t = void(__fastcall*)(void* hknpWorld, std::uint32_t bodyId);
        inline REL::Relocation<HknpWorld_SetBodyKeyframed_t> HknpWorld_SetBodyKeyframed{ REL::Offset(0x1df5cb0) };

        // bhkWorld::RemovePhysicsSystemInstance(world, physicsSystemInstance) — used
        // by destroy() to detach the native hknpPhysicsSystemInstance from the world
        // before letting the bhkPhysicsSystem wrapper refcount-zero free itself.
        // Pass the NATIVE instance pointer, not the bhk wrapper.
        using BhkWorld_RemovePhysicsSystemInstance_t = void(__fastcall*)(void* bhkWorld, void* physicsSystemInstance);
        inline REL::Relocation<BhkWorld_RemovePhysicsSystemInstance_t> BhkWorld_RemovePhysicsSystemInstance{ REL::Offset(0x1dfad00) };

        // hkArrayReserveMore(allocatorInfo, arrayBase, stride) — grows a Havok hkArray's
        // underlying buffer when size >= capacity. Returns true on success. Required for
        // appending bodyCinfo/materialCinfo/etc. entries into a fresh hknpPhysicsSystemData.
        using HkArrayReserveMore_t = bool(__fastcall*)(void* allocatorInfo, void* arrayBase, std::int32_t stride);
        inline REL::Relocation<HkArrayReserveMore_t> HkArrayReserveMore{ REL::Offset(0x155d820) };

        // The Havok allocator info struct used as 1st arg to HkArrayReserveMore.
        // Read as a global pointer (it's stored at the F4VR address).
        inline REL::Relocation<std::uintptr_t> HavokAllocatorInfo{ REL::Offset(0x3866310) };
        
        // =====================================================================
        // WORLD BODY MANIPULATION FUNCTIONS  
        // Safer alternatives that work on existing bodies
        // =====================================================================
        
        // hknpWorld::setBodyPosition(hknpBodyId, hkVector4f&, hknpActivationBehavior::Enum)
        // VR offset: 0x15391c0 - Status 4 (Verified)
        using SetBodyPosition_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId, 
                                                     const RE::NiPoint4& position, int activationBehavior);
        inline REL::Relocation<SetBodyPosition_t> hknpWorld_setBodyPosition{ REL::Offset(0x15391c0) };
        
        // hknpWorld::setBodyTransform(hknpBodyId, hkTransformf&, hknpActivationBehavior::Enum)
        // VR offset: 0x15395e0 - Status 4 (Verified)
        using SetBodyTransformWorld_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId,
                                                           const void* transform, int activationBehavior);
        inline REL::Relocation<SetBodyTransformWorld_t> hknpWorld_setBodyTransform{ REL::Offset(0x15395e0) };

        // hknpWorld::setBodyLinearVelocity(hknpBodyId, hkVector4f&)
        // VR offset: 0x1539c10 - Status 4 (Verified)
        using SetBodyLinearVelocity_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId,
                                                          const RE::NiPoint4& velocity);
        inline REL::Relocation<SetBodyLinearVelocity_t> hknpWorld_setBodyLinearVelocity{ REL::Offset(0x1539c10) };

        // hknpWorld::setBodyVelocity(hknpBodyId, hkVector4f& linear, hkVector4f& angular)
        // VR offset: 0x1539f30 - Status 4 (Verified)
        using SetBodyVelocity_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId,
                                                     const RE::NiPoint4& linear, const RE::NiPoint4& angular);
        inline REL::Relocation<SetBodyVelocity_t> hknpWorld_setBodyVelocity{ REL::Offset(0x1539f30) };

        // hknpWorld::applyBodyLinearImpulse(hknpBodyId, hkVector4f&)
        // VR offset: 0x153a250 - Status 4 (Verified)
        using ApplyBodyLinearImpulse_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId,
                                                           const RE::NiPoint4& impulse);
        inline REL::Relocation<ApplyBodyLinearImpulse_t> hknpWorld_applyBodyLinearImpulse{ REL::Offset(0x153a250) };

        // hknpWorld::setBodyMotionType - we need to find the offset
        // From working code: m_pWorld->setBodyMotionType(bodyId, hknpMotionType::STATIC)

        // hknpWorld::setBodyQuality(hknpBodyId, hknpBodyQualityId, RebuildCachesMode)
        // VR offset: 0x153b070 - Status 4 (Verified)
        using SetBodyQuality_t = void(__fastcall*)(void* thisWorld, std::uint32_t bodyId,
                                                    std::uint8_t qualityId, int rebuildCachesMode);
        inline REL::Relocation<SetBodyQuality_t> hknpWorld_setBodyQuality{ REL::Offset(0x153b070) };
        
    }

}

