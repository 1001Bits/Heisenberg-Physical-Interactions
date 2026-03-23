#include "GrabConstraint.h"
#include "Config.h"
#include "FRIKInterface.h"
#include "Heisenberg.h"

namespace
{
    // Havok world scale - 1 game unit = 0.0142875 Havok units (1/70)
    constexpr float HAVOK_WORLD_SCALE = 0.0142875f;
    
    // bhkNPCollisionObject::SetLinearVelocity(hkVector4f&)
    // VR offset: 0x1e08050 - Status 2
    using SetLinearVelocity_t = void(*)(RE::bhkNPCollisionObject*, RE::NiPoint4&);
    REL::Relocation<SetLinearVelocity_t> SetLinearVelocity{ REL::Offset(0x1e08050) };
    
    // Helper to validate collision object before physics operations
    inline bool IsCollisionObjectValid(RE::bhkNPCollisionObject* obj)
    {
        if (!obj) return false;
        if (!obj->spSystem || reinterpret_cast<uintptr_t>(obj->spSystem.get()) == 0xFFFFFFFFFFFFFFFF) {
            return false;
        }
        return true;
    }
    
    // bhkNPCollisionObject::ApplyHardKeyframe(hkTransformf&, float invDeltaTime)
    // VR offset: 0x1e086e0 - Status 4 (Verified)
    using ApplyHardKeyframe_t = void(*)(RE::bhkNPCollisionObject*, RE::hkTransformf&, float);
    REL::Relocation<ApplyHardKeyframe_t> ApplyHardKeyframe{ REL::Offset(0x1e086e0) };
    
    // bhkNPCollisionObject::SetMotionType(hknpMotionPropertiesId::Preset)
    // VR offset: 0x1e07300 - Status 4 (from fo4_database.csv ID 200912)
    // NOTE: CommonLibF4VR's SetMotionType uses REL::ID which resolves to WRONG address in VR!
    using SetMotionType_t = void(*)(RE::bhkNPCollisionObject*, RE::hknpMotionPropertiesId::Preset);
    REL::Relocation<SetMotionType_t> SetMotionTypeVR{ REL::Offset(0x1e07300) };
    
    // Module base for vtable addresses
    std::uintptr_t GetModuleBase()
    {
        static std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("Fallout4VR.exe"));
        return base;
    }
    
    /**
     * Get the player's character controller body ID
     * This can be used as an anchor for constraints instead of creating a HandBody
     * @return The body ID, or 0x7FFFFFFF if not found
     */
    std::uint32_t GetPlayerBodyId()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player)
        {
            spdlog::debug("[CONSTRAINT] GetPlayerBodyId: No player singleton");
            return 0x7FFFFFFF;
        }
        
        // Method 1: Try character controller from currentProcess
        auto* process = player->currentProcess;
        if (process)
        {
            spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Got process at {:p}", (void*)process);
            
            auto* middleHigh = process->middleHigh;
            if (middleHigh)
            {
                spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Got middleHigh at {:p}", (void*)middleHigh);
                
                auto* charController = middleHigh->charController.get();
                if (charController)
                {
                    RE::hknpBodyId bodyId = charController->GetBodyIdImpl();
                    std::uint32_t id = bodyId.value;
                    spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Got player body ID from charController = 0x{:08X}", id);
                    return id;
                }
                else
                {
                    spdlog::debug("[CONSTRAINT] GetPlayerBodyId: middleHigh->charController is null");
                }
            }
            else
            {
                spdlog::debug("[CONSTRAINT] GetPlayerBodyId: process->middleHigh is null");
            }
        }
        else
        {
            spdlog::debug("[CONSTRAINT] GetPlayerBodyId: player->currentProcess is null");
        }
        
        // Method 2: Try to get collision object from player's 3D
        auto* root3d = player->Get3D();
        if (root3d)
        {
            spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Got player 3D at {:p}", (void*)root3d);
            
            // Try to find a collision object on the player using collisionObject member
            auto* collObj = root3d->collisionObject.get();
            if (collObj)
            {
                auto* rtti = collObj->GetRTTI();
                if (rtti && rtti->GetName())
                {
                    spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Found collision object on player 3D, type={}", rtti->GetName());
                    
                    // Check if it's a bhkNPCollisionObject
                    if (std::string_view(rtti->GetName()) == "bhkNPCollisionObject")
                    {
                        // Cast and try to get body ID
                        // The body ID is typically at a known offset in bhkNPCollisionObject
                        // For now, we'll use a fallback approach
                        spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Player has bhkNPCollisionObject but we need to extract body ID");
                    }
                }
            }
            else
            {
                spdlog::debug("[CONSTRAINT] GetPlayerBodyId: No collision object on player root 3D");
            }
        }
        else
        {
            spdlog::debug("[CONSTRAINT] GetPlayerBodyId: player->Get3D() is null");
        }
        
        spdlog::debug("[CONSTRAINT] GetPlayerBodyId: Could not get player body ID via any method");
        return 0x7FFFFFFF;
    }
}

#if 0  // DISABLED: Motor constraint code not yet complete - uses undefined ConstraintFunctions::
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
            const RE::NiTransform& transformB,
            hkpPositionConstraintMotor* angularMotor,
            hkpPositionConstraintMotor* linearMotor)
        {
            if (!angularMotor || !linearMotor)
            {
                spdlog::error("[MOTOR] CreateGrabConstraintData: Motors cannot be null");
                return nullptr;
            }
            
            // Allocate the constraint data
            auto* data = new GrabConstraintData();
            if (!data)
            {
                spdlog::error("[MOTOR] CreateGrabConstraintData: Failed to allocate");
                return nullptr;
            }
            
            std::memset(data, 0, sizeof(GrabConstraintData));
            
            // Initialize header - use generic constraint vtable 
            // Note: We might need to use a different vtable or create atoms directly
            // For now, set up the header fields
            data->vtable = nullptr;  // We'll need to find the right vtable for custom constraints
            data->memSizeAndFlags = sizeof(GrabConstraintData);
            data->referenceCount = 1;
            data->userData = 0;
            data->constraintType = 100;  // CONSTRAINT_TYPE_CUSTOM
            
            // =====================================================================
            // Initialize transform atoms
            // =====================================================================
            data->atoms.transforms.type = hkpConstraintAtom::TYPE_SET_LOCAL_TRANSFORMS;
            
            // Copy transformA (hand body local frame) to hkTransform format
            // hkTransform: rotation (3x4 matrix = 48 bytes) + translation (16 bytes)
            // The rotation part is stored as 3 column vectors (each 16 bytes = 4 floats)
            float* transA = reinterpret_cast<float*>(data->atoms.transforms.transformA);
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
            
            // Copy transformB (object local frame)
            float* transB = reinterpret_cast<float*>(data->atoms.transforms.transformB);
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
            
            // =====================================================================
            // Initialize stabilization atom
            // =====================================================================
            data->atoms.setupStabilization.type = hkpConstraintAtom::TYPE_SETUP_STABILIZATION;
            data->atoms.setupStabilization.enabled = true;
            data->atoms.setupStabilization.maxAngle = 0.1f;  // Small angle for stability
            
            // =====================================================================
            // Initialize ragdoll motor atom (3-axis angular control)
            // =====================================================================
            data->atoms.ragdollMotors.type = hkpConstraintAtom::TYPE_RAGDOLL_MOTOR;
            data->atoms.ragdollMotors.enabled = true;
            data->atoms.ragdollMotors.initializedOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, initialized));
            data->atoms.ragdollMotors.previousTargetAnglesOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, previousTargetAngles));
            
            // Set identity target rotation initially
            // target_bRca is a 3x4 matrix stored as 12 floats (3 rows, 4 columns)
            // Row-major format: [row0: x y z w] [row1: x y z w] [row2: x y z w]
            data->atoms.ragdollMotors.target_bRca[0] = 1.0f;  // Row 0
            data->atoms.ragdollMotors.target_bRca[1] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[2] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[3] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[4] = 0.0f;  // Row 1
            data->atoms.ragdollMotors.target_bRca[5] = 1.0f;
            data->atoms.ragdollMotors.target_bRca[6] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[7] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[8] = 0.0f;  // Row 2
            data->atoms.ragdollMotors.target_bRca[9] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[10] = 1.0f;
            data->atoms.ragdollMotors.target_bRca[11] = 0.0f;
            
            // Assign the same angular motor to all 3 axes
            data->atoms.ragdollMotors.motors[0] = angularMotor;
            data->atoms.ragdollMotors.motors[1] = angularMotor;
            data->atoms.ragdollMotors.motors[2] = angularMotor;
            
            // =====================================================================
            // Initialize linear motor atoms (3-axis position control)
            // =====================================================================
            
            // Linear motor 0 (X axis)
            data->atoms.linearMotor0.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
            data->atoms.linearMotor0.isEnabled = true;
            data->atoms.linearMotor0.motorAxis = 0;  // X axis
            data->atoms.linearMotor0.initializedOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, initializedLinear));
            data->atoms.linearMotor0.previousTargetPosOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, previousTargetPositions));
            data->atoms.linearMotor0.targetPosition = 0.0f;
            data->atoms.linearMotor0.motor = linearMotor;
            
            // Linear motor 1 (Y axis)
            data->atoms.linearMotor1.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
            data->atoms.linearMotor1.isEnabled = true;
            data->atoms.linearMotor1.motorAxis = 1;  // Y axis
            data->atoms.linearMotor1.initializedOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, initializedLinear) + 1);
            data->atoms.linearMotor1.previousTargetPosOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, previousTargetPositions) + 4);
            data->atoms.linearMotor1.targetPosition = 0.0f;
            data->atoms.linearMotor1.motor = linearMotor;
            
            // Linear motor 2 (Z axis)
            data->atoms.linearMotor2.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
            data->atoms.linearMotor2.isEnabled = true;
            data->atoms.linearMotor2.motorAxis = 2;  // Z axis
            data->atoms.linearMotor2.initializedOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, initializedLinear) + 2);
            data->atoms.linearMotor2.previousTargetPosOffset = 
                static_cast<std::int16_t>(offsetof(GrabConstraintData::Runtime, previousTargetPositions) + 8);
            data->atoms.linearMotor2.targetPosition = 0.0f;
            data->atoms.linearMotor2.motor = linearMotor;
            
            spdlog::debug("[MOTOR] Created GrabConstraintData with 6-DOF motors");
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
                // Note: Motors are NOT destroyed here - caller owns them
                delete data;
            }
        }
        
        void UpdateGrabConstraintTargets(
            GrabConstraintData* data,
            const RE::NiMatrix3& targetRotation,
            const RE::NiPoint3& targetPositions)
        {
            if (!data)
                return;
            
            // Update angular target (ragdoll motor target rotation)
            // The target is stored as a 3x4 matrix in row-major format
            data->atoms.ragdollMotors.target_bRca[0] = targetRotation.entry[0][0];  // Row 0
            data->atoms.ragdollMotors.target_bRca[1] = targetRotation.entry[0][1];
            data->atoms.ragdollMotors.target_bRca[2] = targetRotation.entry[0][2];
            data->atoms.ragdollMotors.target_bRca[3] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[4] = targetRotation.entry[1][0];  // Row 1
            data->atoms.ragdollMotors.target_bRca[5] = targetRotation.entry[1][1];
            data->atoms.ragdollMotors.target_bRca[6] = targetRotation.entry[1][2];
            data->atoms.ragdollMotors.target_bRca[7] = 0.0f;
            data->atoms.ragdollMotors.target_bRca[8] = targetRotation.entry[2][0];  // Row 2
            data->atoms.ragdollMotors.target_bRca[9] = targetRotation.entry[2][1];
            data->atoms.ragdollMotors.target_bRca[10] = targetRotation.entry[2][2];
            data->atoms.ragdollMotors.target_bRca[11] = 0.0f;
            
            // Update linear targets (position offsets in Havok units)
            data->atoms.linearMotor0.targetPosition = targetPositions.x * HAVOK_WORLD_SCALE;
            data->atoms.linearMotor1.targetPosition = targetPositions.y * HAVOK_WORLD_SCALE;
            data->atoms.linearMotor2.targetPosition = targetPositions.z * HAVOK_WORLD_SCALE;
        }
    }
    
    // =========================================================================
    // GRAB CONSTRAINT DATA MEMBER FUNCTIONS
    // Based on Skyrim HIGGS constraint.cpp
    // =========================================================================
    
    GrabConstraintData::GrabConstraintData()
    {
        // Initialize header
        vtable = nullptr;  // Will be set later if needed
        memSizeAndFlags = sizeof(GrabConstraintData);
        referenceCount = 1;
        pad0C = 0;
        userData = 0;
        constraintType = 100;  // CONSTRAINT_TYPE_CUSTOM
        
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
        atoms.setupStabilization.enabled = true;
        atoms.setupStabilization.maxAngle = 0.1f;
        
        // Initialize ragdoll motor atom
        atoms.ragdollMotors.type = hkpConstraintAtom::TYPE_RAGDOLL_MOTOR;
        atoms.ragdollMotors.enabled = false;  // Disabled until grab starts
        atoms.ragdollMotors.initializedOffset = static_cast<std::int16_t>(
            offsetof(Runtime, initialized));
        atoms.ragdollMotors.previousTargetAnglesOffset = static_cast<std::int16_t>(
            offsetof(Runtime, previousTargetAngles));
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
        atoms.linearMotor0.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        atoms.linearMotor0.isEnabled = false;
        atoms.linearMotor0.motorAxis = 0;
        atoms.linearMotor0.initializedOffset = static_cast<std::int16_t>(
            offsetof(Runtime, initializedLinear[0]));
        atoms.linearMotor0.previousTargetPosOffset = static_cast<std::int16_t>(
            offsetof(Runtime, previousTargetPositions[0]));
        atoms.linearMotor0.targetPosition = 0.0f;
        atoms.linearMotor0.motor = nullptr;
        
        atoms.linearMotor1.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        atoms.linearMotor1.isEnabled = false;
        atoms.linearMotor1.motorAxis = 1;
        atoms.linearMotor1.initializedOffset = static_cast<std::int16_t>(
            offsetof(Runtime, initializedLinear[1]));
        atoms.linearMotor1.previousTargetPosOffset = static_cast<std::int16_t>(
            offsetof(Runtime, previousTargetPositions[1]));
        atoms.linearMotor1.targetPosition = 0.0f;
        atoms.linearMotor1.motor = nullptr;
        
        atoms.linearMotor2.type = hkpConstraintAtom::TYPE_LIN_MOTOR;
        atoms.linearMotor2.isEnabled = false;
        atoms.linearMotor2.motorAxis = 2;
        atoms.linearMotor2.initializedOffset = static_cast<std::int16_t>(
            offsetof(Runtime, initializedLinear[2]));
        atoms.linearMotor2.previousTargetPosOffset = static_cast<std::int16_t>(
            offsetof(Runtime, previousTargetPositions[2]));
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
        
        if (!selection.refr || !selection.node)
        {
            spdlog::debug("[CONSTRAINT] StartConstraintGrab: Invalid selection");
            return false;
        }
        
        spdlog::debug("[CONSTRAINT] {} hand: Starting constraint grab on {:08X}",
                     isLeft ? "Left" : "Right", selection.refr->formID);
        
        // Store basic info
        state.refr = selection.refr;
        state.node = selection.node;
        state.initialHandPos = handPos;
        state.initialHandRot = handRot;
        
        // Get collision object
        auto* root = state.refr->Get3D();
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
        cinfo.flags = 0;
        
        spdlog::debug("[CONSTRAINT] cinfo: constraintData={:p}, bodyA=0x{:08X}, bodyB=0x{:08X}",
                      cinfo.constraintData, cinfo.bodyIdA, cinfo.bodyIdB);
        
        // 4. Create the constraint in the world
        spdlog::debug("[CONSTRAINT] Calling CreateConstraint at {:p}...",
                     (void*)ConstraintFunctions::CreateConstraint.address());
        state.constraintId = ConstraintFunctions::CreateConstraint(state.hknpWorld, cinfo);
        
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
        ConstraintFunctions::AddConstraintBodyMap(state.hknpWorld, state.constraintId, 
                                                   state.handBodyId, state.objectBodyId);
        
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
        // CREATE MOTORS
        // =====================================================================
        
        // Angular motor - controls rotation (softer for natural feel)
        state.angularMotor = MotorHelpers::CreatePositionMotor(
            0.6f,    // tau - softness (0=soft, 1=hard)
            1.5f,    // damping
            100.0f,  // maxForce - angular needs less force
            2.0f,    // proportionalRecoveryVelocity
            1.0f     // constantRecoveryVelocity
        );
        
        if (!state.angularMotor)
        {
            spdlog::error("[MOTOR_CONSTRAINT] Failed to create angular motor");
            return false;
        }
        
        // Linear motor - controls position (stronger for pulling objects)
        state.linearMotor = MotorHelpers::CreatePositionMotor(
            0.8f,     // tau - stiffer for position
            1.0f,     // damping
            1000.0f,  // maxForce
            3.0f,     // proportionalRecoveryVelocity
            1.0f      // constantRecoveryVelocity
        );
        
        if (!state.linearMotor)
        {
            spdlog::error("[MOTOR_CONSTRAINT] Failed to create linear motor");
            MotorHelpers::DestroyPositionMotor(state.angularMotor);
            state.angularMotor = nullptr;
            return false;
        }
        
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
        
        auto* grabData = MotorHelpers::CreateGrabConstraintData(
            handTransform, objectTransform, state.angularMotor, state.linearMotor);
        
        if (!grabData)
        {
            spdlog::error("[MOTOR_CONSTRAINT] Failed to create GrabConstraintData");
            MotorHelpers::DestroyPositionMotor(state.angularMotor);
            MotorHelpers::DestroyPositionMotor(state.linearMotor);
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
        cinfo.flags = 0;
        
        spdlog::info("[MOTOR_CONSTRAINT] Calling CreateConstraint...");
        state.constraintId = ConstraintFunctions::CreateConstraint(state.hknpWorld, cinfo);
        
        if (!state.constraintId.IsValid())
        {
            spdlog::error("[MOTOR_CONSTRAINT] CreateConstraint failed! id=0x{:08X}", 
                          state.constraintId.m_value);
            MotorHelpers::DestroyGrabConstraintData(grabData);
            MotorHelpers::DestroyPositionMotor(state.angularMotor);
            MotorHelpers::DestroyPositionMotor(state.linearMotor);
            state.constraintData = nullptr;
            state.angularMotor = nullptr;
            state.linearMotor = nullptr;
            state.useMotorConstraint = false;
            return false;
        }
        
        spdlog::info("[MOTOR_CONSTRAINT] Created motor constraint id=0x{:08X}", 
                     state.constraintId.m_value);
        
        // Register in body map
        ConstraintFunctions::AddConstraintBodyMap(state.hknpWorld, state.constraintId,
                                                   state.handBodyId, state.objectBodyId);
        
        spdlog::info("[MOTOR_CONSTRAINT] SUCCESS! 6-DOF motor constraint active.");
        return true;
    }

    void ConstraintGrabManager::DestroyGrabConstraint(ConstraintGrabState& state)
    {
        if (!state.constraintId.IsValid() || !state.hknpWorld)
            return;
        
        spdlog::debug("[CONSTRAINT] Destroying constraint id={} (motor={})", 
                      state.constraintId.m_value, state.useMotorConstraint);
        
        // Remove from body map first
        ConstraintFunctions::RemoveConstraintBodyMap(state.hknpWorld, state.constraintId);
        
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
        
        std::uint32_t bodyId = ConstraintFunctions::CreateBody(hknpWorld, &bodyCinfo, 1, 0);
        
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
#endif  // DISABLED: Motor constraint code

// =============================================================================
// STUB IMPLEMENTATIONS - ConstraintGrabManager (disabled for now)
// =============================================================================
namespace heisenberg
{
    bool ConstraintGrabManager::Initialize()
    {
        // Motor constraints not yet implemented - use keyframe mode
        _initialized = false;
        return false;
    }

    bool ConstraintGrabManager::StartConstraintGrab(const Selection& /*selection*/, const RE::NiPoint3& /*handPos*/,
                                                     const RE::NiMatrix3& /*handRot*/, bool /*isLeft*/)
    {
        return false;  // Not implemented
    }

    void ConstraintGrabManager::UpdateConstraintGrab(const RE::NiPoint3& /*handPos*/, const RE::NiMatrix3& /*handRot*/,
                                                      bool /*isLeft*/, float /*deltaTime*/)
    {
        // Not implemented
    }

    void ConstraintGrabManager::EndConstraintGrab(bool /*isLeft*/, const RE::NiPoint3* /*throwVelocity*/)
    {
        // Not implemented
    }

    bool ConstraintGrabManager::IsGrabbing(bool /*isLeft*/) const
    {
        return false;  // Never grabbing via constraint since not implemented
    }
}

