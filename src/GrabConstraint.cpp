#include "GrabConstraint.h"
#include "Config.h"
#include "FRIKInterface.h"
#include "Heisenberg.h"
#include "Physics.h"

namespace
{
    // Havok world scale - 1 game unit = 0.0142875 Havok units (1/70)
    constexpr float HAVOK_WORLD_SCALE = 0.0142875f;

    inline std::uintptr_t GetModuleBase()
    {
        return REL::Module::get().base();
    }
    
    // bhkNPCollisionObject::SetLinearVelocity(hkVector4f&)
    // VR offset: 0x1e08050 - Status 2
    using SetLinearVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
    REL::Relocation<SetLinearVelocity_t> SetLinearVelocity{ REL::Offset(0x1e08050) };

    // bhkNPCollisionObject::ApplyHardKeyframe(hkTransformf&, float invDeltaTime)
    // VR offset: 0x1e086e0 - Status 4 (Verified)
    using ApplyHardKeyframe_t = void(*)(RE::bhkNPCollisionObject*, RE::hkTransformf&, float);
    REL::Relocation<ApplyHardKeyframe_t> ApplyHardKeyframeReloc{ REL::Offset(0x1e086e0) };
    
    // Helper to validate collision object before physics operations
    inline bool IsCollisionObjectValid(RE::bhkNPCollisionObject* obj)
    {
        if (!obj) return false;
        if (!obj->spSystem || reinterpret_cast<uintptr_t>(obj->spSystem.get()) == 0xFFFFFFFFFFFFFFFF) {
            return false;
        }
        return true;
    }

    inline std::uint32_t GetPlayerBodyId()
    {
        return heisenberg::Physics::GetPlayerBodyId();
    }

    inline void SetMotionTypeVR(RE::bhkNPCollisionObject* obj, RE::hknpMotionPropertiesId::Preset motion)
    {
        if (!IsCollisionObjectValid(obj)) {
            return;
        }

        heisenberg::ConstraintFunctions::BhkNPCollisionObjectSetMotionType(
            obj, static_cast<int>(motion));
    }

    inline void ApplyHardKeyframe(RE::bhkNPCollisionObject* obj, RE::hkTransformf& transform, float invDeltaTime)
    {
        if (!IsCollisionObjectValid(obj)) {
            return;
        }

        ApplyHardKeyframeReloc(obj, transform, invDeltaTime);
    }
}

// 6-DOF motor constraint implementation — signature fixes applied, now enabled
namespace heisenberg
{
    // =========================================================================
    // MOTOR CONSTRAINT HELPERS IMPLEMENTATION
    // =========================================================================
    
    namespace MotorHelpers
    {
        // Track whether motors were allocated on Havok heap or C++ heap
        // This is needed for proper cleanup
        static bool g_useHavokHeap = false;
        static bool g_havokHeapChecked = false;
        
        /**
         * Check if Havok heap allocation is available
         * This should only be called after game initialization
         */
        bool IsHavokHeapAvailable()
        {
            if (!g_havokHeapChecked)
            {
                void* router = ConstraintFunctions::GetHkMemoryRouter();
                if (router)
                {
                    void* heap = ConstraintFunctions::GetHkHeapAllocator(router);
                    g_useHavokHeap = (heap != nullptr);
                }
                else
                {
                    g_useHavokHeap = false;
                }
                g_havokHeapChecked = true;
                spdlog::debug("[MOTOR] Havok heap allocation: {}", g_useHavokHeap ? "AVAILABLE" : "NOT AVAILABLE (using C++ heap)");
            }
            return g_useHavokHeap;
        }
        
        hkpPositionConstraintMotor* CreatePositionMotor(
            float tau,
            float damping,
            float maxForce,
            float proportionalVelocity,
            float constantVelocity)
        {
            hkpPositionConstraintMotor* motor = nullptr;
            bool usedHavokHeap = false;
            
            // Try Havok heap first (preferred - allows proper reference counting)
            if (IsHavokHeapAvailable())
            {
                motor = ConstraintFunctions::HkAllocReferencedObject<hkpPositionConstraintMotor>();
                if (motor)
                {
                    usedHavokHeap = true;
                    spdlog::debug("[MOTOR] Allocated motor on Havok heap");
                }
            }
            
            // Fall back to C++ heap if Havok heap not available
            if (!motor)
            {
                motor = new hkpPositionConstraintMotor();
                if (!motor)
                {
                    spdlog::error("[MOTOR] Failed to allocate hkpPositionConstraintMotor");
                    return nullptr;
                }
                spdlog::debug("[MOTOR] Allocated motor on C++ heap");
            }
            
            // Set the vtable - this makes it a valid Havok object
            // The vtable address is relative to module base
            std::uintptr_t vtableAddr = GetModuleBase() + ConstraintFunctions::PositionConstraintMotorVtable;
            motor->vtable = reinterpret_cast<void*>(vtableAddr);
            
            // Initialize hkReferencedObject header
            motor->memSizeAndFlags = sizeof(hkpPositionConstraintMotor);
            motor->referenceCount = 1;
            motor->pad0C = 0;
            
            // Set motor type
            motor->type = hkpConstraintMotor::TYPE_POSITION;
            
            // Set force limits
            motor->minForce = -maxForce;
            motor->maxForce = maxForce;
            
            // Set position motor parameters
            motor->tau = tau;
            motor->damping = damping;
            motor->proportionalRecoveryVelocity = proportionalVelocity;
            motor->constantRecoveryVelocity = constantVelocity;
            
            spdlog::debug("[MOTOR] Created PositionMotor: vtable={:p}, tau={:.2f}, damping={:.2f}, maxForce={:.1f}, heap={}",
                          motor->vtable, tau, damping, maxForce, usedHavokHeap ? "Havok" : "C++");
            
            return motor;
        }
        
        void DestroyPositionMotor(hkpPositionConstraintMotor* motor)
        {
            if (!motor)
                return;
            
            // If we're using Havok heap, use removeReference which handles deallocation
            // If using C++ heap, we need to delete directly
            if (IsHavokHeapAvailable())
            {
                // Use Havok's removeReference - it will free via Havok heap
                ConstraintFunctions::hkReferencedObject_removeReference(motor);
            }
            else
            {
                // C++ heap - delete directly
                delete motor;
            }
        }
        
        void UpdateMotorParameters(
            hkpPositionConstraintMotor* angularMotor,
            hkpPositionConstraintMotor* linearMotor,
            float mass,
            bool isColliding)
        {
            if (!angularMotor || !linearMotor)
                return;
            
            // Adjust motor parameters based on mass and collision state
            // Higher mass = more force needed
            float massMultiplier = (std::max)(1.0f, mass / 10.0f);
            
            // Default parameters (can be made configurable later)
            float baseTau = isColliding ? 0.3f : 0.8f;
            float baseDamping = isColliding ? 2.0f : 1.0f;
            float baseMaxForce = 1000.0f * massMultiplier;
            
            // Apply to angular motor
            angularMotor->tau = baseTau;
            angularMotor->damping = baseDamping;
            angularMotor->maxForce = baseMaxForce / 10.0f;  // Angular needs less force
            angularMotor->minForce = -angularMotor->maxForce;
            
            // Apply to linear motor
            linearMotor->tau = baseTau;
            linearMotor->damping = baseDamping;
            linearMotor->maxForce = baseMaxForce;
            linearMotor->minForce = -linearMotor->maxForce;
        }
        
        GrabConstraintData* CreateGrabConstraintData(
            const RE::NiTransform& transformA,
            const RE::NiTransform& transformB)
        {
            auto* data = new GrabConstraintData();
            if (!data)
            {
                spdlog::error("[MOTOR] CreateGrabConstraintData: Failed to allocate");
                return nullptr;
            }
            data->setInBodySpace(transformA, transformB);

            auto* angularMotor = data->GetAngularMotor();
            auto* linearMotor = data->GetLinearMotor();
            if (!angularMotor || !linearMotor)
            {
                spdlog::error("[MOTOR] CreateGrabConstraintData: Constructor did not initialize motors");
                delete data;
                return nullptr;
            }

            spdlog::debug("[MOTOR] Created GrabConstraintData with constructor-owned 6-DOF motors");
            spdlog::debug("[MOTOR]   Angular motor: vtable={:p}, tau={:.2f}, maxForce={:.1f}",
                          angularMotor->vtable, angularMotor->tau, angularMotor->maxForce);
            spdlog::debug("[MOTOR]   Linear motor: vtable={:p}, tau={:.2f}, maxForce={:.1f}",
                          linearMotor->vtable, linearMotor->tau, linearMotor->maxForce);
            
            return data;
        }
        
        void DestroyGrabConstraintData(GrabConstraintData* data)
        {
            if (data)
            {
                delete data;
            }
        }
        
        // Clamp a 3x3 rotation so its angular deviation from identity is within maxAngleRad.
        // Operates on the axis-angle representation extracted from the matrix.
        static RE::NiMatrix3 ClampRotationToMaxAngle(const RE::NiMatrix3& r, float maxAngleRad)
        {
            // Angle of rotation = acos((trace - 1) / 2), clamped to [0, pi]
            float trace = r.entry[0][0] + r.entry[1][1] + r.entry[2][2];
            float cosA = std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f);
            float angle = std::acos(cosA);
            if (angle <= maxAngleRad || angle < 1e-4f) {
                return r;
            }

            // Extract rotation axis (unnormalized) from skew part
            float x = r.entry[2][1] - r.entry[1][2];
            float y = r.entry[0][2] - r.entry[2][0];
            float z = r.entry[1][0] - r.entry[0][1];
            float norm = std::sqrt(x*x + y*y + z*z);
            if (norm < 1e-5f) {
                return r;  // Degenerate (180° flip handling would go here)
            }
            x /= norm; y /= norm; z /= norm;

            // Rebuild matrix at clamped angle using Rodrigues' formula
            float c = std::cos(maxAngleRad), s = std::sin(maxAngleRad), oc = 1.0f - c;
            RE::NiMatrix3 clamped;
            clamped.entry[0][0] = c + x*x*oc;      clamped.entry[0][1] = x*y*oc - z*s;  clamped.entry[0][2] = x*z*oc + y*s;
            clamped.entry[1][0] = y*x*oc + z*s;    clamped.entry[1][1] = c + y*y*oc;    clamped.entry[1][2] = y*z*oc - x*s;
            clamped.entry[2][0] = z*x*oc - y*s;    clamped.entry[2][1] = z*y*oc + x*s;  clamped.entry[2][2] = c + z*z*oc;
            return clamped;
        }

        void UpdateGrabConstraintTargets(
            GrabConstraintData* data,
            const RE::NiMatrix3& targetRotation,
            const RE::NiPoint3& targetPositions)
        {
            if (!data)
                return;

            // ===========================================================
            // Task #2 — Soft 6DOF limits.
            // Clamp target position magnitude and target rotation angle so
            // the motor never tries to drive beyond the configured limits.
            // This is functionally equivalent to a Havok limit atom but
            // safer on hknp 2014 (no solver-atom layout changes required).
            // ===========================================================
            RE::NiMatrix3 rotToUse = targetRotation;
            RE::NiPoint3 posToUse = targetPositions;

            const auto& cfg = Config::GetSingleton();
            if (cfg.grabConstraintEnableSoftLimits) {
                // Linear: clamp target-offset magnitude (game units)
                float lenSq = posToUse.x * posToUse.x + posToUse.y * posToUse.y + posToUse.z * posToUse.z;
                float maxLen = (std::max)(0.01f, cfg.grabConstraintLinearMaxStretch);
                if (lenSq > maxLen * maxLen) {
                    float invLen = maxLen / std::sqrt(lenSq);
                    posToUse.x *= invLen;
                    posToUse.y *= invLen;
                    posToUse.z *= invLen;
                    spdlog::debug("[CONSTRAINT-LIMIT] Linear target clamped to {:.1f} units", maxLen);
                }

                // Angular: clamp rotation angle-from-identity to configured max
                constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
                float maxAngle = (std::max)(0.01f, cfg.grabConstraintAngularMaxAngleDeg) * kDegToRad;
                rotToUse = ClampRotationToMaxAngle(rotToUse, maxAngle);
            }

            // Update angular target (ragdoll motor target rotation)
            // The target is stored as a 3x4 matrix in row-major format
            data->atoms.ragdollMotors.target_bRca[0] = rotToUse.entry[0][0];  // Row 0
            data->atoms.ragdollMotors.target_bRca[1] = rotToUse.entry[0][1];
            data->atoms.ragdollMotors.target_bRca[2] = rotToUse.entry[0][2];
            data->atoms.ragdollMotors.target_bRca[3] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[4] = rotToUse.entry[1][0];  // Row 1
            data->atoms.ragdollMotors.target_bRca[5] = rotToUse.entry[1][1];
            data->atoms.ragdollMotors.target_bRca[6] = rotToUse.entry[1][2];
            data->atoms.ragdollMotors.target_bRca[7] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[8] = rotToUse.entry[2][0];  // Row 2
            data->atoms.ragdollMotors.target_bRca[9] = rotToUse.entry[2][1];
            data->atoms.ragdollMotors.target_bRca[10] = rotToUse.entry[2][2];
            data->atoms.ragdollMotors.target_bRca[11] = 0.0f;

            // Update linear targets (position offsets in Havok units)
            data->atoms.linearMotor0.targetPosition = posToUse.x * HAVOK_WORLD_SCALE;
            data->atoms.linearMotor1.targetPosition = posToUse.y * HAVOK_WORLD_SCALE;
            data->atoms.linearMotor2.targetPosition = posToUse.z * HAVOK_WORLD_SCALE;
        }
    }
    
    // =========================================================================
    // GRAB CONSTRAINT DATA MEMBER FUNCTIONS
    // Based on Skyrim HIGGS constraint.cpp
    // =========================================================================
    
    GrabConstraintData::GrabConstraintData()
    {
        // Initialize header
        vtable = GrabConstraintVtable::GetVtable();
        memSizeAndFlags = sizeof(GrabConstraintData);
        referenceCount = 1;
        pad0C = 0;
        userData = 0;
        constraintType = 20;  // CONSTRAINT_TYPE_CUSTOM (100+ are chain types)

        std::memset(pad1C, 0, sizeof(pad1C));
        std::memset(&atoms, 0, sizeof(atoms));
        
        auto& config = Config::GetSingleton();
        
        // Initialize transform atoms with identity
        atoms.transforms.type = hkpConstraintAtom::TYPE_SET_LOCAL_TRANSFORMS;
        std::memset(atoms.transforms.pad02, 0, sizeof(atoms.transforms.pad02));
        // Set identity transforms
        float* transA = reinterpret_cast<float*>(atoms.transforms.transformA);
        float* transB = reinterpret_cast<float*>(atoms.transforms.transformB);
        for (int i = 0; i < 16; i++) {
            transA[i] = (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f;  // Identity matrix
            transB[i] = (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f;
        }
        
        // Initialize stabilization atom
        atoms.setupStabilization.type = hkpConstraintAtom::TYPE_SETUP_STABILIZATION;
        atoms.setupStabilization.enabled = false;
        atoms.setupStabilization.maxLinearImpulse = 3.402823466e+38f;
        atoms.setupStabilization.maxAngularImpulse = 3.402823466e+38f;
        atoms.setupStabilization.maxAngle = 3.402823466e+38f;
        
        // Initialize ragdoll motor atom
        atoms.ragdollMotors.type = hkpConstraintAtom::TYPE_RAGDOLL_MOTOR;
        atoms.ragdollMotors.enabled = false;  // Disabled until grab starts
        // Runtime offsets are RELATIVE to the runtime cursor (param_4[1]) at the time
        // the ragdoll motor atom is processed by hknpSolverBuildJacobianFromAtomsNotContact.
        // The runtime buffer layout is:
        //   [0x00-0x5F]: Solver results (12 results * 8 bytes = 0x60)
        //   [0x60-0x7F]: External runtime (initialized bytes, target angles/positions)
        // When the ragdoll motor starts, the cursor is at offset 0x00.
        // initialized[3] is at external offset 0x60, previousTargetAngles[3] at 0x64.
        atoms.ragdollMotors.initializedOffset = offsetof(Runtime, initialized);          // 0x60
        atoms.ragdollMotors.previousTargetAnglesOffset = offsetof(Runtime, previousTargetAngles);  // 0x64
        // Identity target rotation
        for (int i = 0; i < 12; i++) {
            atoms.ragdollMotors.target_bRca[i] = (i == 0 || i == 5 || i == 10) ? 1.0f : 0.0f;
        }
        atoms.ragdollMotors.motors[0] = nullptr;
        atoms.ragdollMotors.motors[1] = nullptr;
        atoms.ragdollMotors.motors[2] = nullptr;
        
        // Create angular motor (like Skyrim HIGGS)
        hkpPositionConstraintMotor* angularMotor = MotorHelpers::CreatePositionMotor(
            config.grabConstraintAngularTau,
            config.grabConstraintAngularDamping,
            config.grabConstraintLinearMaxForce / config.grabConstraintAngularToLinearForceRatio,
            config.grabConstraintAngularProportionalRecoveryVelocity,
            config.grabConstraintAngularConstantRecoveryVelocity
        );
        
        // Store motor pointer for cleanup (we own it since we used C++ new)
        _angularMotorOwned = angularMotor;
        
        // Set angular motor on all 3 axes
        // NOTE: We DON'T use setMotor() here because that calls addReference/removeReference
        // which would try to use Havok's heap. Instead we just assign directly.
        atoms.ragdollMotors.motors[0] = reinterpret_cast<hkpConstraintMotor*>(angularMotor);
        atoms.ragdollMotors.motors[1] = reinterpret_cast<hkpConstraintMotor*>(angularMotor);
        atoms.ragdollMotors.motors[2] = reinterpret_cast<hkpConstraintMotor*>(angularMotor);
        
        // Initialize linear motor atoms
        // Linear motor runtime offsets: each linear motor is processed after the ragdoll
        // motor (cursor at 0x30) and after preceding linear motors (+0x10 each).
        // Linear motor 0: cursor at 0x30, initialized[0] at 0x70, target[0] at 0x74
        atoms.linearMotor0.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        atoms.linearMotor0.isEnabled = false;
        atoms.linearMotor0.motorAxis = 0;
        atoms.linearMotor0.initializedOffset = offsetof(Runtime, initializedLinear) - 0x30;   // 0x70-0x30=0x40
        atoms.linearMotor0.previousTargetPosOffset = offsetof(Runtime, previousTargetPositions) - 0x30; // 0x74-0x30=0x44
        atoms.linearMotor0.targetPosition = 0.0f;
        atoms.linearMotor0.motor = nullptr;

        // Linear motor 1: cursor at 0x40, initialized[1] at 0x71, target[1] at 0x78
        atoms.linearMotor1.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        atoms.linearMotor1.isEnabled = false;
        atoms.linearMotor1.motorAxis = 1;
        atoms.linearMotor1.initializedOffset = static_cast<std::int16_t>(offsetof(Runtime, initializedLinear) + 1 - 0x40);  // 0x71-0x40=0x31
        atoms.linearMotor1.previousTargetPosOffset = static_cast<std::int16_t>(offsetof(Runtime, previousTargetPositions) + 4 - 0x40); // 0x78-0x40=0x38
        atoms.linearMotor1.targetPosition = 0.0f;
        atoms.linearMotor1.motor = nullptr;

        // Linear motor 2: cursor at 0x50, initialized[2] at 0x72, target[2] at 0x7C
        atoms.linearMotor2.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        atoms.linearMotor2.isEnabled = false;
        atoms.linearMotor2.motorAxis = 2;
        atoms.linearMotor2.initializedOffset = static_cast<std::int16_t>(offsetof(Runtime, initializedLinear) + 2 - 0x50);  // 0x72-0x50=0x22
        atoms.linearMotor2.previousTargetPosOffset = static_cast<std::int16_t>(offsetof(Runtime, previousTargetPositions) + 8 - 0x50); // 0x7C-0x50=0x2C
        atoms.linearMotor2.targetPosition = 0.0f;
        atoms.linearMotor2.motor = nullptr;
        
        // Create linear motor
        hkpPositionConstraintMotor* linearMotor = MotorHelpers::CreatePositionMotor(
            config.grabConstraintLinearTau,
            config.grabConstraintLinearDamping,
            config.grabConstraintLinearMaxForce,
            config.grabConstraintLinearProportionalRecoveryVelocity,
            config.grabConstraintLinearConstantRecoveryVelocity
        );
        
        // Store motor pointer for cleanup
        _linearMotorOwned = linearMotor;
        
        // Set linear motor on all 3 axes (direct assignment, no ref counting)
        atoms.linearMotor0.motor = reinterpret_cast<hkpConstraintMotor*>(linearMotor);
        atoms.linearMotor1.motor = reinterpret_cast<hkpConstraintMotor*>(linearMotor);
        atoms.linearMotor2.motor = reinterpret_cast<hkpConstraintMotor*>(linearMotor);
        
        spdlog::debug("[CONSTRAINT] GrabConstraintData created with 6-DOF motors (heap={}))",
                     MotorHelpers::IsHavokHeapAvailable() ? "Havok" : "C++");
        spdlog::debug("[CONSTRAINT]   Angular: tau={:.2f}, damping={:.2f}, maxForce={:.1f}",
                      config.grabConstraintAngularTau,
                      config.grabConstraintAngularDamping,
                      config.grabConstraintLinearMaxForce / config.grabConstraintAngularToLinearForceRatio);
        spdlog::debug("[CONSTRAINT]   Linear: tau={:.2f}, damping={:.2f}, maxForce={:.1f}",
                      config.grabConstraintLinearTau,
                      config.grabConstraintLinearDamping,
                      config.grabConstraintLinearMaxForce);
    }
    
    GrabConstraintData::~GrabConstraintData()
    {
        // Clear motor pointers in atoms (don't use setMotor - it does ref counting)
        atoms.ragdollMotors.motors[0] = nullptr;
        atoms.ragdollMotors.motors[1] = nullptr;
        atoms.ragdollMotors.motors[2] = nullptr;
        atoms.linearMotor0.motor = nullptr;
        atoms.linearMotor1.motor = nullptr;
        atoms.linearMotor2.motor = nullptr;
        
        // Delete the motors using the appropriate method based on allocation
        if (_angularMotorOwned)
        {
            MotorHelpers::DestroyPositionMotor(_angularMotorOwned);
            _angularMotorOwned = nullptr;
        }
        if (_linearMotorOwned)
        {
            MotorHelpers::DestroyPositionMotor(_linearMotorOwned);
            _linearMotorOwned = nullptr;
        }
        
        spdlog::debug("[CONSTRAINT] GrabConstraintData destroyed");
    }
    
    void GrabConstraintData::setMotor(int index, hkpConstraintMotor* newMotor)
    {
        // NOTE: This function uses Havok reference counting
        // Only use with Havok-heap-allocated motors!
        // For C++ allocated motors, assign to atoms directly.
        
        // Add reference to new motor if provided
        if (newMotor)
        {
            ConstraintFunctions::hkReferencedObject_addReference(newMotor);
        }
        
        // Get reference to the motor slot based on index
        hkpConstraintMotor*& motor = index < 3 
            ? atoms.ragdollMotors.motors[index] 
            : (index == 3 ? atoms.linearMotor0.motor 
               : (index == 4 ? atoms.linearMotor1.motor 
                  : atoms.linearMotor2.motor));
        
        // Remove reference from old motor if it exists
        if (motor)
        {
            ConstraintFunctions::hkReferencedObject_removeReference(motor);
        }
        
        motor = newMotor;
    }
    
    void GrabConstraintData::setInBodySpace(const RE::NiTransform& transformA, const RE::NiTransform& transformB)
    {
        // Copy transformA to hkTransform format (column-major)
        float* transA = reinterpret_cast<float*>(atoms.transforms.transformA);
        // Column 0 (X axis)
        transA[0] = transformA.rotate.entry[0][0];
        transA[1] = transformA.rotate.entry[1][0];
        transA[2] = transformA.rotate.entry[2][0];
        transA[3] = 0.0f;
        // Column 1 (Y axis)
        transA[4] = transformA.rotate.entry[0][1];
        transA[5] = transformA.rotate.entry[1][1];
        transA[6] = transformA.rotate.entry[2][1];
        transA[7] = 0.0f;
        // Column 2 (Z axis)
        transA[8] = transformA.rotate.entry[0][2];
        transA[9] = transformA.rotate.entry[1][2];
        transA[10] = transformA.rotate.entry[2][2];
        transA[11] = 0.0f;
        // Translation
        transA[12] = transformA.translate.x * HAVOK_WORLD_SCALE;
        transA[13] = transformA.translate.y * HAVOK_WORLD_SCALE;
        transA[14] = transformA.translate.z * HAVOK_WORLD_SCALE;
        transA[15] = 0.0f;
        
        // Copy transformB
        float* transB = reinterpret_cast<float*>(atoms.transforms.transformB);
        // Column 0 (X axis)
        transB[0] = transformB.rotate.entry[0][0];
        transB[1] = transformB.rotate.entry[1][0];
        transB[2] = transformB.rotate.entry[2][0];
        transB[3] = 0.0f;
        // Column 1 (Y axis)
        transB[4] = transformB.rotate.entry[0][1];
        transB[5] = transformB.rotate.entry[1][1];
        transB[6] = transformB.rotate.entry[2][1];
        transB[7] = 0.0f;
        // Column 2 (Z axis)
        transB[8] = transformB.rotate.entry[0][2];
        transB[9] = transformB.rotate.entry[1][2];
        transB[10] = transformB.rotate.entry[2][2];
        transB[11] = 0.0f;
        // Translation
        transB[12] = transformB.translate.x * HAVOK_WORLD_SCALE;
        transB[13] = transformB.translate.y * HAVOK_WORLD_SCALE;
        transB[14] = transformB.translate.z * HAVOK_WORLD_SCALE;
        transB[15] = 0.0f;
    }
    
    void GrabConstraintData::setTarget(const float* target_cbRca)
    {
        // Like Skyrim HIGGS: target_bRca = transformB.rotation * target_cbRca
        // For now, just copy directly - we can add matrix multiply later
        float* transB = reinterpret_cast<float*>(atoms.transforms.transformB);
        
        // Use game's matrix multiply function
        ConstraintFunctions::hkMatrix3f_setMul(
            atoms.ragdollMotors.target_bRca,  // result
            transB,                            // transformB rotation
            target_cbRca                       // target
        );
    }
    
    void GrabConstraintData::setTargetRelativeOrientationOfBodies(const float* bRa)
    {
        // target_bRca = bRa * transformA.rotation
        float* transA = reinterpret_cast<float*>(atoms.transforms.transformA);

        ConstraintFunctions::hkMatrix3f_setMul(
            atoms.ragdollMotors.target_bRca,  // result
            bRa,                               // bRa rotation
            transA                             // transformA rotation
        );
    }

    // SetTarget now clamps rotation angle to soft-limit when enabled (Task #2).
    void GrabConstraintData::SetTarget(const RE::NiMatrix3& target)
    {
        RE::NiMatrix3 clamped = target;
        const auto& cfg = Config::GetSingleton();
        if (cfg.grabConstraintEnableSoftLimits) {
            constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
            float maxAngle = (std::max)(0.01f, cfg.grabConstraintAngularMaxAngleDeg) * kDegToRad;
            clamped = MotorHelpers::ClampRotationToMaxAngle(target, maxAngle);
        }
        std::memcpy(atoms.ragdollMotors.target_bRca, &clamped, sizeof(float) * 12);
    }
    
    bool ConstraintGrabManager::Initialize()
    {
        if (_initialized)
            return true;
            
        if (!ConstraintFunctions::AreConstraintFunctionsAvailable())
        {
            spdlog::error("[CONSTRAINT] Constraint functions not available");
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] ConstraintGrabManager initialized - constraint grabbing available");
        _initialized = true;
        return true;
    }

    bool ConstraintGrabManager::StartConstraintGrab(const Selection& selection, 
                                                     const RE::NiPoint3& handPos,
                                                     const RE::NiMatrix3& handRot, 
                                                     bool isLeft)
    {
        if (!_initialized)
        {
            spdlog::debug("[CONSTRAINT] StartConstraintGrab: Not initialized");
            return false;
        }
        
        ConstraintGrabState& state = isLeft ? _leftState : _rightState;
        
        // End any existing grab first
        if (state.active)
        {
            EndConstraintGrab(isLeft, nullptr);
        }
        
        RE::TESObjectREFR* selRefr = selection.GetRefr();
        if (!selRefr || !selection.node)
        {
            spdlog::debug("[CONSTRAINT] StartConstraintGrab: Invalid selection");
            return false;
        }

        spdlog::debug("[CONSTRAINT] {} hand: Starting constraint grab on {:08X}",
                     isLeft ? "Left" : "Right", selRefr->formID);

        // Store basic info
        state.refr = selRefr;
        state.node = selection.node.get();
        state.initialHandPos = handPos;
        state.initialHandRot = handRot;

        // Get collision object
        auto* root = selRefr->Get3D();
        if (!root)
        {
            spdlog::debug("[CONSTRAINT] StartConstraintGrab: No 3D on ref");
            return false;
        }
        
        // Initialize havok if needed
        f4cf::f4vr::TESObjectREFR_InitHavokForCollisionObject(state.refr);
        
        // Find collision object
        state.collisionObject = nullptr;
        
        // Check root node
        if (auto* coll = root->collisionObject.get())
        {
            auto* rtti = coll->GetRTTI();
            if (rtti && rtti->GetName() && 
                std::strcmp(rtti->GetName(), "bhkNPCollisionObject") == 0)
            {
                state.collisionObject = reinterpret_cast<RE::bhkNPCollisionObject*>(coll);
            }
        }
        
        // Search children if not found
        if (!state.collisionObject)
        {
            std::function<RE::bhkNPCollisionObject*(RE::NiAVObject*)> search = 
                [&](RE::NiAVObject* node) -> RE::bhkNPCollisionObject* {
                if (!node) return nullptr;
                
                if (auto* coll = node->collisionObject.get())
                {
                    auto* rtti = coll->GetRTTI();
                    if (rtti && rtti->GetName() && 
                        std::strcmp(rtti->GetName(), "bhkNPCollisionObject") == 0)
                    {
                        return reinterpret_cast<RE::bhkNPCollisionObject*>(coll);
                    }
                }
                
                if (auto* asNode = node->IsNode())
                {
                    for (auto& child : asNode->children)
                    {
                        if (child)
                        {
                            if (auto* found = search(child.get()))
                                return found;
                        }
                    }
                }
                return nullptr;
            };
            
            state.collisionObject = search(root);
        }
        
        if (!state.collisionObject)
        {
            spdlog::debug("[CONSTRAINT] StartConstraintGrab: No collision object found");
            return false;
        }
        
        // Calculate grab offsets
        RE::NiPoint3 hitWorld = selection.hitPoint;
        RE::NiTransform worldTransform = selection.node->world;
        
        RE::NiPoint3 diff = hitWorld - worldTransform.translate;
        RE::NiMatrix3 rotInv = worldTransform.rotate.Transpose();
        state.grabOffsetLocal = rotInv * diff;
        state.grabRotationLocal = handRot.Transpose() * worldTransform.rotate;
        
        // Get body ID and world pointer
        state.objectBodyId = GetBodyId(state.collisionObject);
        state.hknpWorld = GetHknpWorld(state.collisionObject);
        
        if (!state.hknpWorld)
        {
            spdlog::debug("[CONSTRAINT] StartConstraintGrab: Could not get hknpWorld");
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] StartConstraintGrab: Got hknpWorld={:p}, bodyId={}",
                     state.hknpWorld, state.objectBodyId);
        
        auto& config = Config::GetSingleton();
        bool constraintCreated = false;
        
        // Check grab mode from config
        if (config.grabMode == 2)
        {
            // Mode 2: 6-DOF motor constraint (like Skyrim HIGGS)
            spdlog::debug("[CONSTRAINT] Using MOTOR CONSTRAINT mode (grabMode=2)");
            constraintCreated = CreateMotorGrabConstraint(state, handPos, handRot);
        }
        else
        {
            // Mode 1: Ball-socket constraint
            spdlog::debug("[CONSTRAINT] Using BALL-SOCKET CONSTRAINT mode (grabMode=1)");
            constraintCreated = CreateGrabConstraint(state, handPos);
        }
        
        if (constraintCreated && state.constraintId.IsValid())
        {
            // Constraint mode: The object will be pulled by the constraint
            // We don't need to keyframe the object, just update the hand body position
            spdlog::debug("[CONSTRAINT] {} hand: Constraint grab started ({} MODE)",
                         isLeft ? "Left" : "Right",
                         state.useMotorConstraint ? "6-DOF MOTOR" : "BALL-SOCKET");
        }
        else
        {
            // Fallback to keyframe mode: directly control the object's position
            // Set object to keyframed motion so we can control it
            // NOTE: Use VR-safe SetMotionTypeVR - CommonLibF4's version uses wrong address!
            SetMotionTypeVR(state.collisionObject, RE::hknpMotionPropertiesId::Preset::KEYFRAMED);
            spdlog::debug("[CONSTRAINT] {} hand: Constraint grab started (KEYFRAME FALLBACK MODE)",
                         isLeft ? "Left" : "Right");
        }
        
        state.active = true;
        
        return true;
    }

    void ConstraintGrabManager::UpdateConstraintGrab(const RE::NiPoint3& handPos,
                                                      const RE::NiMatrix3& handRot,
                                                      bool isLeft,
                                                      float deltaTime)
    {
        ConstraintGrabState& state = isLeft ? _leftState : _rightState;
        HandBody& handBody = isLeft ? _leftHandBody : _rightHandBody;
        
        if (!state.active || !state.collisionObject)
            return;
        
        // Check if we're in constraint mode or keyframe fallback mode
        bool constraintMode = state.constraintId.IsValid() && handBody.IsValid();
        
        if (constraintMode)
        {
            // CONSTRAINT MODE: Update hand body position, physics pulls the object
            UpdateHandBodyPosition(handBody, handPos, handRot, deltaTime);
            
            // If using motor constraint, also update the motor targets
            if (state.useMotorConstraint && state.constraintData)
            {
                auto* grabData = reinterpret_cast<GrabConstraintData*>(state.constraintData);
                
                // Calculate desired relative rotation from initial grab
                // target = inverse(initial hand rot) * current hand rot
                RE::NiMatrix3 initialInv = state.initialHandRot.Transpose();
                RE::NiMatrix3 relativeDelta = handRot * initialInv;
                
                // Set target rotation for the motors
                grabData->SetTarget(relativeDelta);
                
                // Linear motor targets stay at 0 since we're using the hand body position
                // The motors will work to keep the object at the constraint pivot point
                
                // Ensure motors are enabled
                grabData->SetMotorsActive(true);
            }
            
            // Debug logging
            static std::atomic<int> logCountC{0};
            if (++logCountC >= 60)
            {
                spdlog::debug("[CONSTRAINT] UpdateConstraintGrab ({}): hand=({:.1f}, {:.1f}, {:.1f})",
                             state.useMotorConstraint ? "MOTOR" : "BALL-SOCKET",
                             handPos.x, handPos.y, handPos.z);
                logCountC = 0;
            }
        }
        else
        {
            // KEYFRAME FALLBACK MODE: Directly control the object
            // Calculate target position
            RE::NiPoint3 rotatedOffset = handRot * state.grabOffsetLocal;
            RE::NiPoint3 targetPos = handPos - rotatedOffset;
            
            // Calculate target rotation
            RE::NiMatrix3 targetRot = handRot * state.grabRotationLocal;
            
            // Build transform
            RE::hkTransformf targetTransform;
            targetTransform.rotation = targetRot;
            targetTransform.translation = RE::NiPoint4(
                targetPos.x * HAVOK_WORLD_SCALE,
                targetPos.y * HAVOK_WORLD_SCALE,
                targetPos.z * HAVOK_WORLD_SCALE,
                0.0f
            );
            
            // Apply keyframe
            float invDeltaTime = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 60.0f;
            ApplyHardKeyframe(state.collisionObject, targetTransform, invDeltaTime);
            
            // Debug logging
            static int logCountK = 0;
            if (++logCountK >= 60)
            {
                spdlog::debug("[CONSTRAINT] UpdateConstraintGrab (KEYFRAME): target=({:.1f}, {:.1f}, {:.1f})",
                             targetPos.x, targetPos.y, targetPos.z);
                logCountK = 0;
            }
        }
    }

    void ConstraintGrabManager::EndConstraintGrab(bool isLeft, const RE::NiPoint3* throwVelocity)
    {
        ConstraintGrabState& state = isLeft ? _leftState : _rightState;
        
        if (!state.active)
            return;
        
        spdlog::debug("[CONSTRAINT] {} hand: Ending constraint grab on {:08X}",
                     isLeft ? "Left" : "Right",
                     state.refr ? state.refr->formID : 0);
        
        // Restore physics
        if (state.collisionObject)
        {
            // Set back to dynamic
            // NOTE: Use VR-safe SetMotionTypeVR - CommonLibF4's version uses wrong address!
            SetMotionTypeVR(state.collisionObject, RE::hknpMotionPropertiesId::Preset::DYNAMIC);
            
            // Apply throw velocity if provided
            if (throwVelocity && (throwVelocity->x != 0 || throwVelocity->y != 0 || throwVelocity->z != 0))
            {
                if (IsCollisionObjectValid(state.collisionObject))
                {
                    RE::NiPoint4 hkVelocity(
                        throwVelocity->x * HAVOK_WORLD_SCALE,
                        throwVelocity->y * HAVOK_WORLD_SCALE,
                        throwVelocity->z * HAVOK_WORLD_SCALE,
                        0.0f
                    );
                    SetLinearVelocity(state.collisionObject, hkVelocity);
                    spdlog::debug("[CONSTRAINT] Applied throw velocity ({:.2f}, {:.2f}, {:.2f})",
                                 throwVelocity->x, throwVelocity->y, throwVelocity->z);
                }
            }
        }
        
        // Destroy constraint if we have one
        if (state.constraintId.IsValid() && state.hknpWorld)
        {
            DestroyGrabConstraint(state);
        }
        
        // Restore fingers to open position
        Heisenberg::GetSingleton().GetFingerAnimator(isLeft).ForceReset(isLeft);
        
        // Notify Heisenberg that grab ended (starts post-grab kFighting suppression)
        // This prevents Unarmed from auto-equipping when grip is released
        Heisenberg::GetSingleton().OnGrabEnded(isLeft);
        
        state.Clear();
    }

    bool ConstraintGrabManager::IsGrabbing(bool isLeft) const
    {
        return isLeft ? _leftState.active : _rightState.active;
    }

    const ConstraintGrabState& ConstraintGrabManager::GetState(bool isLeft) const
    {
        return isLeft ? _leftState : _rightState;
    }

    bool ConstraintGrabManager::CreateGrabConstraint(ConstraintGrabState& state, 
                                                      const RE::NiPoint3& handPos)
    {
        // =====================================================================
        // USE PLAYER BODY AS ANCHOR (instead of creating a HandBody)
        // This avoids the crash in hknpWorld::createBody
        // =====================================================================
        
        // Get the player's character controller body ID
        std::uint32_t playerBodyId = GetPlayerBodyId();
        if (playerBodyId == 0x7FFFFFFF || playerBodyId == 0)
        {
            spdlog::error("[CONSTRAINT] CreateGrabConstraint: Failed to get player body ID");
            return false;
        }
        
        // Store player body ID as the "hand body" - we'll use it as the constraint anchor
        state.handBodyId = playerBodyId;
        
        // Get object body ID
        state.objectBodyId = GetBodyId(state.collisionObject);
        if (state.objectBodyId == 0 || state.objectBodyId == 0x7FFFFFFF)
        {
            spdlog::error("[CONSTRAINT] CreateGrabConstraint: Failed to get object body ID");
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] Creating constraint: hand body=0x{:08X}, object body=0x{:08X}",
                     state.handBodyId, state.objectBodyId);
        
        // =====================================================================
        // CREATE BALL-SOCKET CONSTRAINT
        // =====================================================================
        
        // 1. Allocate and initialize ball-socket constraint data
        spdlog::debug("[CONSTRAINT] Allocating hkpBallAndSocketConstraintData (size=0x{:X})...", 
                      sizeof(hkpBallAndSocketConstraintData));
        auto* ballSocketData = new hkpBallAndSocketConstraintData();
        std::memset(ballSocketData, 0, sizeof(hkpBallAndSocketConstraintData));
        state.constraintData = ballSocketData;
        
        // Call the game's constructor
        spdlog::debug("[CONSTRAINT] Calling BallSocketCtor at {:p}...", 
                      (void*)ConstraintFunctions::BallSocketCtor.address());
        ConstraintFunctions::BallSocketCtor(ballSocketData);
        spdlog::debug("[CONSTRAINT] BallSocketCtor returned successfully");
        
        // 2. Set pivot points in body space
        // For hand body: pivot at center (0, 0, 0)
        RE::NiPoint4 pivotA(0.0f, 0.0f, 0.0f, 0.0f);
        
        // For object: pivot at the grab offset (local to object, in Havok units)
        RE::NiPoint4 pivotB(
            state.grabOffsetLocal.x * HAVOK_WORLD_SCALE,
            state.grabOffsetLocal.y * HAVOK_WORLD_SCALE,
            state.grabOffsetLocal.z * HAVOK_WORLD_SCALE,
            0.0f
        );
        
        spdlog::debug("[CONSTRAINT] Calling BallSocketSetInBodySpace at {:p}...", 
                      (void*)ConstraintFunctions::BallSocketSetInBodySpace.address());
        ConstraintFunctions::BallSocketSetInBodySpace(ballSocketData, pivotA, pivotB);
        
        spdlog::debug("[CONSTRAINT] Pivot points set: A=(0,0,0), B=({:.3f},{:.3f},{:.3f})",
                      pivotB.x, pivotB.y, pivotB.z);
        
        // 3. Create constraint cinfo
        spdlog::debug("[CONSTRAINT] Building hknpConstraintCinfo (size=0x{:X})...", sizeof(hknpConstraintCinfo));
        hknpConstraintCinfo cinfo;
        std::memset(&cinfo, 0, sizeof(cinfo));
        cinfo.constraintData = state.constraintData;
        cinfo.bodyIdA = state.handBodyId;
        cinfo.bodyIdB = state.objectBodyId;
        cinfo.flags = 1;  // Bit 0 = wantRuntime: forces hknp to allocate constraint runtime buffer

        spdlog::debug("[CONSTRAINT] cinfo: constraintData={:p}, bodyA=0x{:08X}, bodyB=0x{:08X}",
                      cinfo.constraintData, cinfo.bodyIdA, cinfo.bodyIdB);
        
        // 4. Create the constraint in the world (output parameter pattern)
        spdlog::debug("[CONSTRAINT] Calling CreateConstraint at {:p}...",
                     (void*)ConstraintFunctions::CreateConstraint.address());
        state.constraintId.m_value = 0x7FFFFFFF;
        ConstraintFunctions::CreateConstraint(state.hknpWorld, &state.constraintId, &cinfo);

        if (!state.constraintId.IsValid())
        {
            spdlog::error("[CONSTRAINT] CreateConstraint failed! Returned id=0x{:08X}", state.constraintId.m_value);
            delete state.constraintData;
            state.constraintData = nullptr;
            return false;
        }

        spdlog::debug("[CONSTRAINT] Created constraint id=0x{:08X}", state.constraintId.m_value);

        // 5. Register in body map for proper tracking
        spdlog::debug("[CONSTRAINT] Calling AddConstraintBodyMap at {:p}...",
                      (void*)ConstraintFunctions::AddConstraintBodyMap.address());
        ConstraintFunctions::AddConstraintBodyMap(state.hknpWorld, state.constraintId, &cinfo);
        
        spdlog::debug("[CONSTRAINT] CreateGrabConstraint: SUCCESS! Constraint active.");
        
        return true;
    }

    bool ConstraintGrabManager::CreateMotorGrabConstraint(ConstraintGrabState& state, 
                                                           const RE::NiPoint3& handPos,
                                                           const RE::NiMatrix3& handRot)
    {
        // Determine which hand body to use
        bool isLeft = (!_leftState.active && state.refr != _rightState.refr);
        HandBody& handBody = isLeft ? _leftHandBody : _rightHandBody;
        
        if (!handBody.IsValid())
        {
            // Create hand body in the same world as the object
            if (!CreateHandBody(handBody, state.hknpWorld, handPos))
            {
                spdlog::error("[MOTOR_CONSTRAINT] CreateMotorGrabConstraint: Failed to create hand body");
                return false;
            }
        }
        else
        {
            // Update existing hand body position
            UpdateHandBodyPosition(handBody, handPos, handRot, 1.0f / 60.0f);
        }
        
        // Store hand body ID in state
        state.handBodyId = handBody.bodyId;
        
        // Get object body ID
        state.objectBodyId = GetBodyId(state.collisionObject);
        if (state.objectBodyId == 0 || state.objectBodyId == 0x7FFFFFFF)
        {
            spdlog::error("[MOTOR_CONSTRAINT] CreateMotorGrabConstraint: Failed to get object body ID");
            return false;
        }
        
        spdlog::info("[MOTOR_CONSTRAINT] Creating 6-DOF motor constraint: hand=0x{:08X}, object=0x{:08X}",
                     state.handBodyId, state.objectBodyId);
        
        // =====================================================================
        // CREATE GRAB CONSTRAINT DATA
        // =====================================================================
        
        // Build transforms for the constraint
        RE::NiTransform handTransform;
        handTransform.rotate = handRot;
        handTransform.translate = RE::NiPoint3(0.0f, 0.0f, 0.0f);  // Pivot at hand center
        handTransform.scale = 1.0f;
        
        RE::NiTransform objectTransform;
        objectTransform.rotate = state.grabRotationLocal;
        objectTransform.translate = state.grabOffsetLocal;
        objectTransform.scale = 1.0f;
        
        state.angularMotor = nullptr;
        state.linearMotor = nullptr;

        auto* grabData = MotorHelpers::CreateGrabConstraintData(handTransform, objectTransform);
        
        if (!grabData)
        {
            spdlog::error("[MOTOR_CONSTRAINT] Failed to create GrabConstraintData");
            return false;
        }

        state.angularMotor = grabData->GetAngularMotor();
        state.linearMotor = grabData->GetLinearMotor();
        if (!state.angularMotor || !state.linearMotor)
        {
            spdlog::error("[MOTOR_CONSTRAINT] GrabConstraintData returned null motor pointers");
            delete grabData;
            state.angularMotor = nullptr;
            state.linearMotor = nullptr;
            return false;
        }
        
        state.constraintData = grabData;
        state.useMotorConstraint = true;
        
        // =====================================================================
        // CREATE CONSTRAINT IN WORLD
        // =====================================================================
        
        spdlog::debug("[MOTOR_CONSTRAINT] Building hknpConstraintCinfo...");
        hknpConstraintCinfo cinfo;
        std::memset(&cinfo, 0, sizeof(cinfo));
        cinfo.constraintData = grabData;
        cinfo.bodyIdA = state.handBodyId;
        cinfo.bodyIdB = state.objectBodyId;
        cinfo.flags = 1;  // Bit 0 = wantRuntime: forces hknp to allocate constraint runtime buffer

        spdlog::info("[MOTOR_CONSTRAINT] Calling CreateConstraint...");
        state.constraintId.m_value = 0x7FFFFFFF;
        ConstraintFunctions::CreateConstraint(state.hknpWorld, &state.constraintId, &cinfo);

        if (!state.constraintId.IsValid())
        {
            spdlog::error("[MOTOR_CONSTRAINT] CreateConstraint failed! id=0x{:08X}",
                          state.constraintId.m_value);
            MotorHelpers::DestroyGrabConstraintData(grabData);
            state.constraintData = nullptr;
            state.angularMotor = nullptr;
            state.linearMotor = nullptr;
            state.useMotorConstraint = false;
            return false;
        }

        spdlog::info("[MOTOR_CONSTRAINT] Created motor constraint id=0x{:08X}",
                     state.constraintId.m_value);

        // Register in body map
        ConstraintFunctions::AddConstraintBodyMap(state.hknpWorld, state.constraintId, &cinfo);
        
        spdlog::info("[MOTOR_CONSTRAINT] SUCCESS! 6-DOF motor constraint active.");
        return true;
    }

    void ConstraintGrabManager::DestroyGrabConstraint(ConstraintGrabState& state)
    {
        if (!state.constraintId.IsValid() || !state.hknpWorld)
            return;
        
        spdlog::debug("[CONSTRAINT] Destroying constraint id={} (motor={})", 
                      state.constraintId.m_value, state.useMotorConstraint);
        
        // Remove from body map first (unused param must be 0)
        ConstraintFunctions::RemoveConstraintBodyMap(state.hknpWorld, 0, state.constraintId.m_value);
        
        // Destroy the constraint
        ConstraintFunctions::DestroyConstraints(state.hknpWorld, &state.constraintId, 1);
        
        // Free constraint data and motors if we allocated them
        if (state.useMotorConstraint)
        {
            // Motor constraint mode - use GrabConstraintData destructor
            if (state.constraintData)
            {
                // GrabConstraintData destructor handles motor cleanup via setMotor(nullptr)
                auto* grabData = reinterpret_cast<GrabConstraintData*>(state.constraintData);
                delete grabData;
                state.constraintData = nullptr;
            }
            // Motors are cleaned up by GrabConstraintData destructor, but clean pointers
            state.angularMotor = nullptr;
            state.linearMotor = nullptr;
        }
        else
        {
            // Ball-socket mode
            if (state.constraintData)
            {
                delete state.constraintData;
                state.constraintData = nullptr;
            }
        }
        
        state.constraintId.Invalidate();
        state.useMotorConstraint = false;
    }

    void* ConstraintGrabManager::GetHknpWorld(RE::bhkNPCollisionObject* collObj)
    {
        if (!collObj)
            return nullptr;
        
        // The hknpWorld is accessible through bhkWorld
        // We need to get the cell's physics world
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
            return nullptr;
        
        auto* cell = player->GetParentCell();
        if (!cell)
            return nullptr;
        
        // TESObjectCell::GetbhkWorld
        // VR offset: 0x39b070
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        void* bhkWorld = GetbhkWorld(cell);
        if (!bhkWorld)
            return nullptr;
        
        // bhkWorld contains pointer to hknpBSWorld at offset 0x60
        // hknpBSWorld inherits from hknpWorld
        void* hknpWorld = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkWorld) + 0x60);
        
        return hknpWorld;
    }

    std::uint32_t ConstraintGrabManager::GetBodyId(RE::bhkNPCollisionObject* collObj)
    {
        if (!collObj)
            return 0;
        
        // The body ID is stored in the collision object
        // bhkNPCollisionObject has hknpBodyId at offset 0x28
        std::uint32_t bodyId = *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<uintptr_t>(collObj) + 0x28);
        
        return bodyId;
    }

    // =========================================================================
    // HAND BODY MANAGEMENT
    // =========================================================================

    bool ConstraintGrabManager::CreateHandBody(HandBody& handBody, void* hknpWorld, 
                                                const RE::NiPoint3& position)
    {
        if (!hknpWorld)
        {
            spdlog::error("[CONSTRAINT] CreateHandBody: No hknpWorld provided");
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] CreateHandBody: Creating hand body at ({:.1f}, {:.1f}, {:.1f})",
                     position.x, position.y, position.z);
        
        // Create a small box shape for the hand
        // Half extents in Havok units (game units * HAVOK_WORLD_SCALE)
        constexpr float handSizeGameUnits = 5.0f;  // ~5 game units = small box
        RE::NiPoint4 halfExtents(
            handSizeGameUnits * HAVOK_WORLD_SCALE,
            handSizeGameUnits * HAVOK_WORLD_SCALE,
            handSizeGameUnits * HAVOK_WORLD_SCALE,
            0.0f
        );
        
        constexpr float convexRadius = 0.05f;  // Small convex radius
        
        // Create the shape using proper BuildConfig
        // Initialize BuildConfig with the game's constructor to get default values
        ConstraintFunctions::hknpConvexShapeBuildConfig buildConfig;
        spdlog::debug("[CONSTRAINT] CreateHandBody: Calling BuildConfigCtor at {:p}...",
                      (void*)ConstraintFunctions::BuildConfigCtor.address());
        ConstraintFunctions::BuildConfigCtor(&buildConfig);
        spdlog::debug("[CONSTRAINT] CreateHandBody: BuildConfigCtor returned successfully");
        
        // Create the convex shape from half extents
        void* shape = ConstraintFunctions::CreateConvexShapeFromHalfExtents(halfExtents, convexRadius, &buildConfig);
        
        if (!shape)
        {
            spdlog::error("[CONSTRAINT] CreateHandBody: Failed to create convex shape");
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] CreateHandBody: Created shape at {:p}", shape);
        
        // Initialize hknpBodyCinfo using the game's constructor
        hknpBodyCinfo bodyCinfo;
        
        // Call the game's default constructor to properly initialize all fields
        spdlog::debug("[CONSTRAINT] CreateHandBody: Calling BodyCinfoCtor at {:p}...", 
                      (void*)ConstraintFunctions::BodyCinfoCtor.address());
        ConstraintFunctions::BodyCinfoCtor(&bodyCinfo);
        spdlog::debug("[CONSTRAINT] CreateHandBody: BodyCinfoCtor returned successfully");
        
        // Log the structure contents after game initialization
        spdlog::debug("[CONSTRAINT] CreateHandBody: After ctor - shape={:p}, reservedBodyId=0x{:08X}, motionId=0x{:08X}",
                      bodyCinfo.shape, bodyCinfo.reservedBodyId, bodyCinfo.motionId);
        spdlog::debug("[CONSTRAINT] CreateHandBody: After ctor - qualityId={}, materialId={}, collisionFilterInfo=0x{:08X}",
                      bodyCinfo.qualityId, bodyCinfo.materialId, bodyCinfo.collisionFilterInfo);
        
        // Now set up our specific values
        bodyCinfo.shape = shape;
        
        // Position in Havok units
        bodyCinfo.position = RE::NiPoint4(
            position.x * HAVOK_WORLD_SCALE,
            position.y * HAVOK_WORLD_SCALE,
            position.z * HAVOK_WORLD_SCALE,
            0.0f  // w = 0 for position vectors
        );
        
        // Identity orientation (quaternion: x=0, y=0, z=0, w=1)
        bodyCinfo.orientation = RE::NiPoint4(0.0f, 0.0f, 0.0f, 1.0f);
        
        // Set quality ID for keyframed body (quality affects how the physics processes it)
        // Quality ID 2 is typically KEYFRAMED
        bodyCinfo.qualityId = 2;  // KEYFRAMED quality
        
        // No collision with most things - we just want it as a constraint anchor
        // Use a collision filter that won't interact with the world
        bodyCinfo.collisionFilterInfo = 0;  // No collisions
        
        // Log final structure before creation
        spdlog::debug("[CONSTRAINT] CreateHandBody: Final bodyCinfo - shape={:p}, pos=({:.3f},{:.3f},{:.3f})",
                     bodyCinfo.shape, bodyCinfo.position.x, bodyCinfo.position.y, bodyCinfo.position.z);
        spdlog::debug("[CONSTRAINT] CreateHandBody: Final bodyCinfo - qualityId={}, sizeof={}", 
                     bodyCinfo.qualityId, sizeof(hknpBodyCinfo));
        
        // Create the body in the physics world
        // AdditionMode: 1 = ADD_BODY_NOW
        // AdditionFlags: 0 = default
        spdlog::debug("[CONSTRAINT] CreateHandBody: Calling CreateBody at {:p}...",
                     (void*)ConstraintFunctions::CreateBody.address());
        
        std::uint32_t bodyId = 0x7FFFFFFF;
        ConstraintFunctions::CreateBody(hknpWorld, &bodyId, &bodyCinfo, 1, 0);

        if (bodyId == 0x7FFFFFFF || bodyId == 0)
        {
            spdlog::error("[CONSTRAINT] CreateHandBody: Failed to create body (id=0x{:08X}). "
                         "Shape at {:p} leaked (no safe hknpShape release API available).", bodyId, shape);
            // KNOWN LEAK: The convex shape allocated at CreateConvexShapeFromHalfExtents is leaked here.
            // Havok hknpShape uses internal reference counting (hkReferencedObject) but we don't have
            // a reliable way to call removeReference() without the correct vtable offset.
            // This path is rare (only on physics world full or invalid params) so the leak is acceptable.
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] CreateHandBody: SUCCESS! Created body id=0x{:08X}", bodyId);
        
        // Store the results
        handBody.shape = shape;
        handBody.hknpWorld = hknpWorld;
        handBody.bodyId = bodyId;
        handBody.valid = true;
        
        return true;  // Success!
    }

    void ConstraintGrabManager::DestroyHandBody(HandBody& handBody)
    {
        if (!handBody.IsValid())
            return;
        
        spdlog::debug("[CONSTRAINT] DestroyHandBody: Destroying body id=0x{:08X}", handBody.bodyId);
        
        // Call hknpWorld::destroyBodies to remove the body
        if (handBody.hknpWorld)
        {
            // ActivationMode: 0 = default
            ConstraintFunctions::DestroyBodies(handBody.hknpWorld, &handBody.bodyId, 1, 0);
        }
        
        // The shape is ref-counted and should clean itself up when the body is destroyed
        handBody.Invalidate();
    }

    void ConstraintGrabManager::UpdateHandBodyPosition(HandBody& handBody, 
                                                        const RE::NiPoint3& position,
                                                        const RE::NiMatrix3& rotation, 
                                                        float deltaTime)
    {
        if (!handBody.IsValid() || !handBody.hknpWorld)
            return;
        
        // Convert position to Havok units
        RE::NiPoint4 hkPosition(
            position.x * HAVOK_WORLD_SCALE,
            position.y * HAVOK_WORLD_SCALE,
            position.z * HAVOK_WORLD_SCALE,
            0.0f
        );
        
        // Convert rotation matrix to quaternion
        // For now, use identity quaternion - we mainly care about position for constraint anchoring
        RE::NiPoint4 hkOrientation(0.0f, 0.0f, 0.0f, 1.0f);  // Identity quaternion
        
        // TODO: Convert NiMatrix3 to quaternion properly
        // The rotation affects the constraint attachment point orientation
        (void)rotation;
        
        // Calculate inverse delta time for velocity-based keyframing
        float invDeltaTime = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 60.0f;
        
        // Apply hard keyframe to move the hand body
        ConstraintFunctions::ApplyHardKeyFrameBodyId(
            handBody.hknpWorld,
            handBody.bodyId,
            hkPosition,
            hkOrientation,
            invDeltaTime
        );
    }
}
// End of 6-DOF motor constraint implementation

// Stub implementations removed — real implementations now active above.
