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
    // HAVOK CONSTRAINT MOTOR TYPES
    // From CommonLibSSE-NG and Havok 2010/2014 SDK
    // These are still used by hknp internally via hkpConstraintData
    // =========================================================================

    /**
     * hkpConstraintMotor - Base class for constraint motors
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
     * hkpLimitedForceConstraintMotor - Adds min/max force limits
     * Size: 0x20
     */
    struct hkpLimitedForceConstraintMotor : public hkpConstraintMotor
    {
        float minForce;  // 0x18
        float maxForce;  // 0x1C
    };
    static_assert(sizeof(hkpLimitedForceConstraintMotor) == 0x20);

    /**
     * hkpPositionConstraintMotor - Position-controlled motor with spring-like behavior
     * This is what HIGGS uses for grab constraints
     * Size: 0x30
     */
    struct hkpPositionConstraintMotor : public hkpLimitedForceConstraintMotor
    {
        float tau;                           // 0x20 - Softness (0=soft, 1=hard)
        float damping;                       // 0x24 - Damping factor
        float proportionalRecoveryVelocity;  // 0x28 - Speed of position recovery
        float constantRecoveryVelocity;      // 0x2C - Constant recovery rate
    };
    static_assert(sizeof(hkpPositionConstraintMotor) == 0x30);

    // =========================================================================
    // HAVOK CONSTRAINT ATOMS
    // These define how constraints behave
    // =========================================================================

    /**
     * hkpConstraintAtom - Base class for constraint atoms
     * Size: 0x02
     */
    struct hkpConstraintAtom
    {
        enum AtomType : std::uint16_t
        {
            TYPE_INVALID = 0,
            TYPE_BRIDGE = 1,
            TYPE_SET_LOCAL_TRANSFORMS = 2,
            TYPE_SET_LOCAL_TRANSLATIONS = 3,
            TYPE_SET_LOCAL_ROTATIONS = 4,
            TYPE_BALL_SOCKET = 5,
            TYPE_STIFF_SPRING = 6,
            TYPE_LIN = 7,
            TYPE_LIN_SOFT = 8,
            TYPE_LIN_LIMIT = 9,
            TYPE_LIN_FRICTION = 10,
            TYPE_LIN_MOTOR = 11,
            TYPE_2D_ANG = 12,
            TYPE_ANG = 13,
            TYPE_ANG_LIMIT = 14,
            TYPE_TWIST_LIMIT = 15,
            TYPE_CONE_LIMIT = 16,
            TYPE_ANG_FRICTION = 17,
            TYPE_ANG_MOTOR = 18,
            TYPE_RAGDOLL_MOTOR = 19,
            TYPE_PULLEY = 20,
            TYPE_RACK_AND_PINION = 21,
            TYPE_COG_WHEEL = 22,
            TYPE_SETUP_STABILIZATION = 23
        };
        
        std::uint16_t type;  // 0x00
    };
    static_assert(sizeof(hkpConstraintAtom) == 0x02);

    /**
     * hkpSetLocalTransformsConstraintAtom - Sets transform frames for both bodies
     * Size: 0x90
     * 
     * Layout:
     * - 0x00: type (uint16)
     * - 0x02: padding to 0x10
     * - 0x10: transformA (hkTransform = 64 bytes: 48 byte rotation + 16 byte translation)
     * - 0x50: transformB (hkTransform = 64 bytes)
     */
    struct hkpSetLocalTransformsConstraintAtom : public hkpConstraintAtom
    {
        std::uint8_t pad02[14];              // 0x02 - padding to 0x10
        std::uint8_t transformA[0x40];       // 0x10 - hkTransform (64 bytes)
        std::uint8_t transformB[0x40];       // 0x50 - hkTransform (64 bytes)
    };
    static_assert(sizeof(hkpSetLocalTransformsConstraintAtom) == 0x90);

    /**
     * hkpSetupStabilizationAtom - Stabilization settings
     * Size: 0x10
     */
    struct hkpSetupStabilizationAtom : public hkpConstraintAtom
    {
        bool enabled;               // 0x02
        std::uint8_t pad03;         // 0x03
        float maxLinearImpulse;     // 0x04
        float maxAngularImpulse;    // 0x08
        float maxAngle;             // 0x0C
    };
    static_assert(sizeof(hkpSetupStabilizationAtom) == 0x10);

    /**
     * hkpRagdollMotorConstraintAtom - 3-axis angular motor (for rotation control)
     * Size: 0x60
     * 
     * Layout from CommonLibSSE-NG:
     * - 0x00: type (uint16)
     * - 0x02: enabled (bool)
     * - 0x04: initializedOffset (int16)
     * - 0x06: previousTargetAnglesOffset (int16)
     * - 0x10: target_bRca (hkMatrix3 = 48 bytes)
     * - 0x40: motors[3] (3 pointers = 24 bytes)
     */
    struct hkpRagdollMotorConstraintAtom : public hkpConstraintAtom
    {
        bool enabled;                              // 0x02
        std::uint8_t pad03;                        // 0x03
        std::int16_t initializedOffset;            // 0x04
        std::int16_t previousTargetAnglesOffset;   // 0x06
        std::uint8_t pad08[8];                     // 0x08 - padding to 0x10
        float target_bRca[12];                     // 0x10 - 3x4 matrix (48 bytes) - hkMatrix3/NiMatrix3
        hkpConstraintMotor* motors[3];             // 0x40 - 3 motors (24 bytes)
        std::uint8_t pad58[8];                     // 0x58 - padding to 0x60
    };
    static_assert(sizeof(hkpRagdollMotorConstraintAtom) == 0x60);

    /**
     * hkpLinMotorConstraintAtom - Single-axis linear motor (for position control)
     * Size: 0x18
     */
    struct hkpLinMotorConstraintAtom : public hkpConstraintAtom
    {
        bool isEnabled;                         // 0x02
        std::uint8_t motorAxis;                 // 0x03
        std::int16_t initializedOffset;         // 0x04
        std::int16_t previousTargetPosOffset;   // 0x06
        float targetPosition;                   // 0x08
        std::uint8_t pad0C[4];                  // 0x0C
        hkpConstraintMotor* motor;              // 0x10
    };
    static_assert(sizeof(hkpLinMotorConstraintAtom) == 0x18);

    // =========================================================================
    // GRAB CONSTRAINT DATA
    // Custom constraint with 6 DOF motors (3 angular + 3 linear)
    // Ported from Skyrim HIGGS
    // =========================================================================

    /**
     * GrabConstraintData - Custom constraint for physics-based grabbing
     * 
     * Contains:
     * - Transform atoms for body-space pivot points
     * - Ragdoll motor atom for 3-axis rotation control
     * - 3x Linear motor atoms for 3-axis position control
     * 
     * Total: 6 degrees of freedom with motor control
     * 
     * Motor parameters (from Config):
     * - Angular: tau, damping, proportionalRecoveryVelocity, constantRecoveryVelocity, maxForce
     * - Linear: tau, damping, proportionalRecoveryVelocity, constantRecoveryVelocity, maxForce
     */
    class GrabConstraintData
    {
    public:
        // Havok 2014 motor atoms each use two solver results.
        enum
        {
            SOLVER_RESULT_MOTOR_ANG_0 = 0,
            SOLVER_RESULT_MOTOR_ANG_0_INTERNAL = 1,
            SOLVER_RESULT_MOTOR_ANG_1 = 2,
            SOLVER_RESULT_MOTOR_ANG_1_INTERNAL = 3,
            SOLVER_RESULT_MOTOR_ANG_2 = 4,
            SOLVER_RESULT_MOTOR_ANG_2_INTERNAL = 5,
            SOLVER_RESULT_MOTOR_LIN_0 = 6,
            SOLVER_RESULT_MOTOR_LIN_0_INTERNAL = 7,
            SOLVER_RESULT_MOTOR_LIN_1 = 8,
            SOLVER_RESULT_MOTOR_LIN_1_INTERNAL = 9,
            SOLVER_RESULT_MOTOR_LIN_2 = 10,
            SOLVER_RESULT_MOTOR_LIN_2_INTERNAL = 11,
            SOLVER_RESULT_MAX = 12
        };
        
        // hkpConstraintData vtable and header
        void* vtable;                    // 0x00 - Must point to valid vtable
        std::uint16_t memSizeAndFlags;   // 0x08
        std::uint16_t referenceCount;    // 0x0A
        std::uint32_t pad0C;             // 0x0C
        std::uint64_t userData;          // 0x10
        std::uint32_t constraintType;    // 0x18 - CONSTRAINT_TYPE_CUSTOM = 20
        std::uint8_t pad1C[4];           // 0x1C
        
        // Atoms structure
        // Total size: 0x90 + 0x10 + 0x60 + 0x18*3 = 0x148
        struct Atoms
        {
            hkpSetLocalTransformsConstraintAtom transforms;    // 0x00 - size 0x90
            hkpSetupStabilizationAtom setupStabilization;      // 0x90 - size 0x10
            hkpRagdollMotorConstraintAtom ragdollMotors;       // 0xA0 - size 0x60
            hkpLinMotorConstraintAtom linearMotor0;            // 0x100 - size 0x18
            hkpLinMotorConstraintAtom linearMotor1;            // 0x118 - size 0x18
            hkpLinMotorConstraintAtom linearMotor2;            // 0x130 - size 0x18
            
            const hkpConstraintAtom* getAtoms() const { return reinterpret_cast<const hkpConstraintAtom*>(&transforms); }
            int getSizeOfAllAtoms() const { return static_cast<int>(sizeof(Atoms)); }
        };
        static_assert(sizeof(Atoms) == 0x148);
        
        alignas(16) Atoms atoms;  // 0x20
        
        // Runtime data offsets for solver
        struct Runtime
        {
            // Solver results for the 6 motor atoms (2 results per motor in Havok 2014).
            std::uint8_t solverResults[SOLVER_RESULT_MAX * 8];  // 0x00
            
            // Angular motor runtime
            std::uint8_t initialized[3];         // 0x60
            std::uint8_t pad63;                  // 0x63
            float previousTargetAngles[3];       // 0x64
            
            // Linear motor runtime
            std::uint8_t initializedLinear[3];   // 0x70
            std::uint8_t pad73;                  // 0x73
            float previousTargetPositions[3];    // 0x74
            
            static int getSizeOfExternalRuntime() { return sizeof(Runtime); }
        };
        static_assert(offsetof(Runtime, initialized) == 0x60);
        static_assert(offsetof(Runtime, previousTargetAngles) == 0x64);
        static_assert(offsetof(Runtime, initializedLinear) == 0x70);
        static_assert(offsetof(Runtime, previousTargetPositions) == 0x74);
        static_assert(sizeof(Runtime) == 0x80);
        
        // =====================================================================
        // CONSTRUCTOR / DESTRUCTOR (like Skyrim HIGGS)
        // =====================================================================
        
        // Constructor - initializes atoms and creates motors with config values
        GrabConstraintData();
        
        // Destructor - releases motor references
        ~GrabConstraintData();
        
        // =====================================================================
        // MOTOR ACCESS
        // =====================================================================
        
        // Motor access helpers
        hkpPositionConstraintMotor* GetAngularMotor() 
        { 
            return reinterpret_cast<hkpPositionConstraintMotor*>(atoms.ragdollMotors.motors[0]); 
        }
        
        hkpPositionConstraintMotor* GetLinearMotor() 
        { 
            return reinterpret_cast<hkpPositionConstraintMotor*>(atoms.linearMotor0.motor); 
        }
        
        // Set motor at index (0-2 = angular, 3-5 = linear)
        // Like Skyrim HIGGS - handles reference counting
        void setMotor(int index, hkpConstraintMotor* newMotor);
        
        // Enable/disable all motors
        void SetMotorsActive(bool enabled)
        {
            atoms.ragdollMotors.enabled = enabled;
            atoms.linearMotor0.isEnabled = enabled;
            atoms.linearMotor1.isEnabled = enabled;
            atoms.linearMotor2.isEnabled = enabled;
        }
        
        // Set target rotation - multiplies with transformB like Skyrim HIGGS
        void setTarget(const float* target_cbRca);
        
        // Set target relative orientation of bodies
        void setTargetRelativeOrientationOfBodies(const float* bRa);
        
        // Set transforms in body space
        void setInBodySpace(const RE::NiTransform& transformA, const RE::NiTransform& transformB);
        
        // Set target rotation (copies 3x4 matrix to float[12]).
        // Clamps angular deviation to grabConstraintAngularMaxAngleDeg if soft limits enabled.
        void SetTarget(const RE::NiMatrix3& target);
        
        // Set target rotation from raw float array
        void SetTargetRaw(const float* target)
        {
            std::memcpy(atoms.ragdollMotors.target_bRca, target, sizeof(float) * 12);
        }
        
    private:
        // =====================================================================
        // OWNED MOTOR POINTERS
        // Since we allocate motors with C++ new (not Havok heap), we track them
        // separately for proper cleanup without using Havok's reference counting.
        // =====================================================================
        hkpPositionConstraintMotor* _angularMotorOwned = nullptr;
        hkpPositionConstraintMotor* _linearMotorOwned = nullptr;
    };
    // Note: Full size depends on alignment, approximately 0x170+

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
    // CONSTRAINT GRAB STATE
    // =========================================================================
    
    /**
     * ConstraintGrabState - State for one hand's constraint grab
     */
    struct ConstraintGrabState
    {
        bool active = false;
        bool useMotorConstraint = false;  // True if using 6-DOF motor constraint
        
        // The grabbed object
        RE::TESObjectREFR* refr = nullptr;
        RE::NiAVObject* node = nullptr;
        RE::bhkNPCollisionObject* collisionObject = nullptr;
        
        // Constraint handle
        hknpConstraintId constraintId;
        
        // Constraint data (heap allocated) - either ball-socket or motor
        void* constraintData = nullptr;
        
        // Non-owning pointers to the motors stored inside GrabConstraintData.
        hkpPositionConstraintMotor* angularMotor = nullptr;
        hkpPositionConstraintMotor* linearMotor = nullptr;
        
        // Transform offsets
        RE::NiPoint3 grabOffsetLocal;      // Offset from object origin to grab point
        RE::NiMatrix3 grabRotationLocal;   // Rotation offset
        RE::NiPoint3 initialHandPos;
        RE::NiMatrix3 initialHandRot;
        
        // Body IDs for the constraint
        std::uint32_t handBodyId = 0;
        std::uint32_t objectBodyId = 0;
        
        // World pointer for constraint management
        void* hknpWorld = nullptr;
        
        void Clear()
        {
            active = false;
            useMotorConstraint = false;
            refr = nullptr;
            node = nullptr;
            collisionObject = nullptr;
            constraintId.Invalidate();
            constraintData = nullptr;
            angularMotor = nullptr;
            linearMotor = nullptr;
            grabOffsetLocal = RE::NiPoint3();
            grabRotationLocal = RE::NiMatrix3();
            initialHandPos = RE::NiPoint3();
            initialHandRot = RE::NiMatrix3();
            handBodyId = 0;
            objectBodyId = 0;
            hknpWorld = nullptr;
        }
    };

    // =========================================================================
    // GRAB CONSTRAINT VTABLE
    // Custom vtable for GrabConstraintData (6-DOF motor constraint)
    // =========================================================================

    namespace GrabConstraintVtable
    {
        constexpr int CONSTRAINT_TYPE_CUSTOM = 20;
        constexpr std::int32_t HK_SUCCESS = 0;
        constexpr std::int32_t HK_FAILURE = 1;

        struct ConstraintInfo
        {
            int m_maxSizeOfSchema;
            int m_sizeOfSchemas;
            int m_numSolverResults;
            int m_numSolverElemTemps;
            hkpConstraintAtom* m_atoms;
            std::uint32_t m_sizeOfAllAtoms;
            std::uint32_t m_extraSchemaSize;

            void clear()
            {
                m_maxSizeOfSchema = 0;
                m_sizeOfSchemas = 0;
                m_numSolverResults = 0;
                m_numSolverElemTemps = 0;
                m_atoms = nullptr;
                m_sizeOfAllAtoms = 0;
                m_extraSchemaSize = 0;
            }

            void add(int schemaSize, int numSolverResults, int numSolverTempElems)
            {
                m_sizeOfSchemas += schemaSize;
                m_numSolverResults += numSolverResults;
                m_numSolverElemTemps += numSolverTempElems;
                if (schemaSize > m_maxSizeOfSchema) {
                    m_maxSizeOfSchema = schemaSize;
                }
            }
        };

        struct RuntimeInfo
        {
            int m_sizeOfExternalRuntime;
            int m_numSolverResults;
        };

        // Slot 0: ~dtor
        inline void __fastcall Destructor(GrabConstraintData* thisPtr) {}

        // Slot 1: __first_virtual_table_function__ (hkBaseObject)
        inline void __fastcall FirstVirtualTableFunction(GrabConstraintData* thisPtr) {}

        // Slot 2: getClassType (hkReferencedObject)
        inline void* __fastcall GetClassType(GrabConstraintData* thisPtr) { return nullptr; }

        // Slot 3: deleteThisReferencedObject (hkReferencedObject)
        inline void __fastcall DeleteThisReferencedObject(GrabConstraintData* thisPtr) {}

        // Slot 4: getType (hkpConstraintData - pure virtual)
        inline int __fastcall GetType(GrabConstraintData* thisPtr) { return CONSTRAINT_TYPE_CUSTOM; }

        // Slot 6: isValid (hkpConstraintData - pure virtual)
        inline bool __fastcall IsValid(GrabConstraintData* thisPtr) { return true; }

        inline void __fastcall GetConstraintInfo(GrabConstraintData* thisPtr, ConstraintInfo* infoOut)
        {
            // Schema sizes from Havok SDK hkpJacobianSchemaInfo:
            // SetLocalTransforms: schema=144, results=0, temps=0
            // SetupStabilization: schema=16,  results=0, temps=0
            // AngularMotor1D:     schema=64,  results=2, temps=2  (×3 axes)
            // LinearMotor1D:      schema=80,  results=2, temps=2  (×3 axes)
            infoOut->clear();

            infoOut->add(144, 0, 0);   // SetLocalTransforms
            infoOut->add(16, 0, 0);    // SetupStabilization
            infoOut->add(64 * 3, 2 * 3, 2 * 3);  // 3 angular motors
            infoOut->add(80 * 3, 2 * 3, 2 * 3);  // 3 linear motors
            // m_maxSizeOfSchema = largest single atom schema = SetLocalTransforms (144)
            // (add() already tracks the max internally)

            infoOut->m_atoms = const_cast<hkpConstraintAtom*>(thisPtr->atoms.getAtoms());
            infoOut->m_sizeOfAllAtoms = thisPtr->atoms.getSizeOfAllAtoms();
            infoOut->m_extraSchemaSize = 0;
        }

        inline void __fastcall SetMaximumLinearImpulse(GrabConstraintData* thisPtr, float value) {}
        inline void __fastcall SetMaximumAngularImpulse(GrabConstraintData* thisPtr, float value) {}
        inline void __fastcall SetBreachImpulse(GrabConstraintData* thisPtr, float value) {}
        inline float __fastcall GetMaximumLinearImpulse(GrabConstraintData* thisPtr) { return 3.402823466e+38f; }
        inline float __fastcall GetMaximumAngularImpulse(GrabConstraintData* thisPtr) { return 3.402823466e+38f; }
        inline float __fastcall GetBreachImpulse(GrabConstraintData* thisPtr) { return 3.402823466e+38f; }
        inline void __fastcall SetBodyToNotify(GrabConstraintData* thisPtr, int bodyIndex) {}
        inline std::uint8_t __fastcall GetNotifiedBodyIndex(GrabConstraintData* thisPtr) { return 0; }

        inline void __fastcall SetSolvingMethod(GrabConstraintData* thisPtr, int method)
        {
            thisPtr->atoms.setupStabilization.enabled = (method == 0);
        }

        inline std::int32_t __fastcall SetInertiaStabilizationFactor(GrabConstraintData* thisPtr, float value) { return HK_FAILURE; }
        inline std::int32_t __fastcall GetInertiaStabilizationFactor(GrabConstraintData* thisPtr, float* valueOut)
        {
            if (valueOut) {
                *valueOut = 0.0f;
            }
            return HK_FAILURE;
        }

        inline void __fastcall GetRuntimeInfo(GrabConstraintData* thisPtr, bool wantRuntime, RuntimeInfo* infoOut)
        {
            if (wantRuntime) {
                infoOut->m_sizeOfExternalRuntime = GrabConstraintData::Runtime::getSizeOfExternalRuntime();
                infoOut->m_numSolverResults = GrabConstraintData::SOLVER_RESULT_MAX;
            } else {
                infoOut->m_sizeOfExternalRuntime = 0;
                infoOut->m_numSolverResults = 0;
            }
        }

        inline void* __fastcall GetSolverResults(GrabConstraintData* thisPtr, void* runtime) { return runtime; }

        inline void __fastcall AddInstance(GrabConstraintData* thisPtr, void* runtime, int sizeOfRuntime)
        {
            if (runtime) std::memset(runtime, 0, sizeOfRuntime);
        }

        inline void __fastcall BuildJacobian(GrabConstraintData* thisPtr, void* queryIn, void* queryOut) {}
        inline bool __fastcall IsBuildJacobianCallbackRequired(GrabConstraintData* thisPtr) { return false; }
        inline void __fastcall BuildJacobianCallback(GrabConstraintData* thisPtr, void* queryIn, void* queryOut) {}

        // Vtable layout must match real hkpConstraintData (24 slots).
        // Verified against hkpRagdollConstraintData vtable at 0x142e18298 via Ghidra.
        inline void* g_GrabConstraintVtable[24] = {
            reinterpret_cast<void*>(&Destructor),                    // 0: ~dtor
            reinterpret_cast<void*>(&FirstVirtualTableFunction),     // 1: __first_virtual_table_function__
            reinterpret_cast<void*>(&GetClassType),                  // 2: getClassType
            reinterpret_cast<void*>(&DeleteThisReferencedObject),    // 3: deleteThisReferencedObject
            reinterpret_cast<void*>(&GetType),                       // 4: getType (pure)
            reinterpret_cast<void*>(&GetConstraintInfo),             // 5: getConstraintInfo (pure) ← CRITICAL
            reinterpret_cast<void*>(&IsValid),                       // 6: isValid (pure)
            reinterpret_cast<void*>(&SetMaximumLinearImpulse),       // 7
            reinterpret_cast<void*>(&SetMaximumAngularImpulse),      // 8
            reinterpret_cast<void*>(&SetBreachImpulse),              // 9
            reinterpret_cast<void*>(&GetMaximumLinearImpulse),       // 10
            reinterpret_cast<void*>(&GetMaximumAngularImpulse),      // 11
            reinterpret_cast<void*>(&GetBreachImpulse),              // 12
            reinterpret_cast<void*>(&SetBodyToNotify),               // 13
            reinterpret_cast<void*>(&GetNotifiedBodyIndex),          // 14
            reinterpret_cast<void*>(&SetSolvingMethod),              // 15
            reinterpret_cast<void*>(&SetInertiaStabilizationFactor), // 16
            reinterpret_cast<void*>(&GetInertiaStabilizationFactor), // 17
            reinterpret_cast<void*>(&GetRuntimeInfo),                // 18: getRuntimeInfo (pure) ← CRITICAL
            reinterpret_cast<void*>(&GetSolverResults),
            reinterpret_cast<void*>(&AddInstance),
            reinterpret_cast<void*>(&BuildJacobian),
            reinterpret_cast<void*>(&IsBuildJacobianCallbackRequired),
            reinterpret_cast<void*>(&BuildJacobianCallback),
        };

        inline void* GetVtable() { return &g_GrabConstraintVtable[0]; }

    }  // namespace GrabConstraintVtable

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

    // =========================================================================
    // CONSTRAINT GRAB MANAGER
    // =========================================================================
    
    /**
     * ConstraintGrabManager - Physics constraint-based grabbing
     * 
     * Uses ball-socket constraints to attach grabbed objects to a virtual
     * "hand body" that follows the VR controller position.
     */
    class ConstraintGrabManager
    {
    public:
        static ConstraintGrabManager& GetSingleton()
        {
            static ConstraintGrabManager instance;
            return instance;
        }

        /**
         * Initialize the constraint system
         * @return true if constraint functions are available
         */
        bool Initialize();

        /**
         * Check if constraint grabbing is available
         */
        bool IsAvailable() const { return _initialized; }

        /**
         * Start a constraint-based grab
         */
        bool StartConstraintGrab(const Selection& selection, const RE::NiPoint3& handPos,
                                  const RE::NiMatrix3& handRot, bool isLeft);

        /**
         * Update the constraint target position
         */
        void UpdateConstraintGrab(const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot,
                                   bool isLeft, float deltaTime);

        /**
         * End a constraint grab, optionally applying throw velocity
         */
        void EndConstraintGrab(bool isLeft, const RE::NiPoint3* throwVelocity);

        /**
         * Check if a hand is grabbing via constraint
         */
        bool IsGrabbing(bool isLeft) const;

        /**
         * Get the grab state for a hand
         */
        const ConstraintGrabState& GetState(bool isLeft) const;

    private:
        ConstraintGrabManager() = default;
        ~ConstraintGrabManager() = default;
        ConstraintGrabManager(const ConstraintGrabManager&) = delete;
        ConstraintGrabManager& operator=(const ConstraintGrabManager&) = delete;

        // Create the constraint between hand and object
        bool CreateGrabConstraint(ConstraintGrabState& state, const RE::NiPoint3& handPos);
        
        // Create a 6-DOF motor constraint (alternative to ball-socket)
        bool CreateMotorGrabConstraint(ConstraintGrabState& state, const RE::NiPoint3& handPos,
                                        const RE::NiMatrix3& handRot);
        
        // Destroy the active constraint
        void DestroyGrabConstraint(ConstraintGrabState& state);
        
        // Get the hknpWorld from a collision object
        void* GetHknpWorld(RE::bhkNPCollisionObject* collObj);
        
        // Get the body ID from a collision object
        std::uint32_t GetBodyId(RE::bhkNPCollisionObject* collObj);
        
        // =====================================================================
        // HAND BODY MANAGEMENT
        // =====================================================================
        
        /**
         * HandBody - A physics body that represents the VR controller
         * This is keyframed (position controlled) and used as an anchor for constraints
         */
        struct HandBody
        {
            std::uint32_t bodyId = 0x7FFFFFFF;  // hknpBodyId - invalid by default
            void* shape = nullptr;               // hknpConvexShape*
            void* hknpWorld = nullptr;           // The world this body lives in
            bool valid = false;
            
            bool IsValid() const { return valid && bodyId != 0x7FFFFFFF; }
            void Invalidate() 
            { 
                bodyId = 0x7FFFFFFF; 
                shape = nullptr; 
                hknpWorld = nullptr;
                valid = false; 
            }
        };
        
        // Create hand body in the physics world
        bool CreateHandBody(HandBody& handBody, void* hknpWorld, const RE::NiPoint3& position);
        
        // Destroy hand body
        void DestroyHandBody(HandBody& handBody);
        
        // Update hand body position (keyframed)
        void UpdateHandBodyPosition(HandBody& handBody, const RE::NiPoint3& position, 
                                     const RE::NiMatrix3& rotation, float deltaTime);

        bool _initialized = false;
        ConstraintGrabState _leftState;
        ConstraintGrabState _rightState;
        
        // Hand bodies - one per hand
        HandBody _leftHandBody;
        HandBody _rightHandBody;
    };

    // =========================================================================
    // MOTOR CONSTRAINT HELPERS
    // =========================================================================
    
    namespace MotorHelpers
    {
        /**
         * Create an hkpPositionConstraintMotor by manually setting up the structure
         * Since we don't have access to the constructor, we set the vtable manually
         * 
         * @param tau Softness (0-1, lower = softer spring)
         * @param damping Damping factor
         * @param maxForce Maximum force the motor can apply
         * @param proportionalVelocity Proportional recovery velocity
         * @param constantVelocity Constant recovery velocity
         * @return Pointer to allocated motor, or nullptr on failure
         */
        hkpPositionConstraintMotor* CreatePositionMotor(
            float tau = 0.8f,
            float damping = 1.0f,
            float maxForce = 1000.0f,
            float proportionalVelocity = 2.0f,
            float constantVelocity = 1.0f);
        
        /**
         * Destroy an hkpPositionConstraintMotor
         */
        void DestroyPositionMotor(hkpPositionConstraintMotor* motor);
        
        /**
         * Update motor parameters for a grab constraint
         */
        void UpdateMotorParameters(
            hkpPositionConstraintMotor* angularMotor,
            hkpPositionConstraintMotor* linearMotor,
            float mass,
            bool isColliding);
        
        /**
         * Create a fully initialized GrabConstraintData with 6-DOF motors.
         * The constraint data constructor owns and initializes its motors.
         * @param transformA Local transform in body A's space (hand body)
         * @param transformB Local transform in body B's space (grabbed object)
         * @return Allocated and initialized GrabConstraintData, or nullptr on failure
         */
        GrabConstraintData* CreateGrabConstraintData(
            const RE::NiTransform& transformA,
            const RE::NiTransform& transformB);
        
        /**
         * Destroy a GrabConstraintData and its owned motors.
         */
        void DestroyGrabConstraintData(GrabConstraintData* data);
        
        /**
         * Update the target transforms for a grab constraint
         * This should be called each frame to move the object toward the hand
         * @param data The GrabConstraintData to update
         * @param targetRotation Target rotation matrix (hand orientation relative to object)
         * @param targetPositions Target linear motor positions (x, y, z offsets)
         */
        void UpdateGrabConstraintTargets(
            GrabConstraintData* data,
            const RE::NiMatrix3& targetRotation,
            const RE::NiPoint3& targetPositions);
    }
}

