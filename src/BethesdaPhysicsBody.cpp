#include "BethesdaPhysicsBody.h"
#include "BethesdaAlloc.h"
#include "BethesdaPhysicsBodyOffsets.h"

#include "F4SE/F4SE.h"
#include "GrabConstraint.h"
#include <intrin.h>
#include <spdlog/spdlog.h>
#include <windows.h>

namespace heisenberg::bethesda_physics_body
{
    using namespace offsets;
    namespace SD = sysdata_layout;
    namespace BC = bodycinfo_layout;

    // ──────────────────────────────────────────────────────────────────────────
    // SEH-wrapped helpers (no C++ objects in scope — required by __try)
    // ──────────────────────────────────────────────────────────────────────────

    static bool CallSystemDataCtor(void* sysData)
    {
        __try {
            heisenberg::ConstraintFunctions::PhysicsSystemDataCtor(sysData);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallMotionCinfoCtor(void* mcinfo)
    {
        __try {
            heisenberg::ConstraintFunctions::MotionCinfoCtor(mcinfo);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallBodyCinfoCtor(void* bcinfo)
    {
        __try {
            heisenberg::ConstraintFunctions::BodyCinfoCtor(
                reinterpret_cast<heisenberg::hknpBodyCinfo*>(bcinfo));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // File-scope REL::Relocations for SEH-wrapped helpers (static-in-function
    // causes unwind frames that conflict with __try).
    namespace {
        using EnableBodyFlagsFn = void (*)(void*, std::uint32_t, std::uint32_t, int);
        REL::Relocation<EnableBodyFlagsFn> g_enableBodyFlagsFn{ REL::Offset(0x153c090) };
        using SetXformFn = void (*)(void*, const float*);
        REL::Relocation<SetXformFn> g_setXformFn{ REL::Offset(0x1e08a70) };
        using SetVelFn = void (*)(void*, const float*, const float*);
        REL::Relocation<SetVelFn> g_setVelFn{ REL::Offset(0x1e082a0) };
    }

    static bool CallMaterialCtor(void* mat)
    {
        __try {
            heisenberg::ConstraintFunctions::MaterialCtor(mat);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallBhkPhysicsSystemCtor(void* physSys, void* sysData)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkPhysicsSystemCtor(physSys, sysData);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallBhkNPCollisionObjectCtor(void* collObj, void* physSys)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkNPCollisionObjectCtor(collObj, 0u, physSys);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallAddToWorld(void* collObj, void* bhkWorld)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkNPCollisionObjectAddToWorld(collObj, bhkWorld);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallGetBodyId(void* physSys, std::uint32_t* outBodyId)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(physSys, outBodyId, 0u);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallSetBodyKeyframed(void* hknpWorld, std::uint32_t bodyId)
    {
        __try {
            heisenberg::ConstraintFunctions::HknpWorld_SetBodyKeyframed(hknpWorld, bodyId);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallSetMotionType(void* collObj, int mt)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkNPCollisionObjectSetMotionType(collObj, mt);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallEnableFlags(void* hknpWorld, std::uint32_t bodyId, std::uint32_t flags)
    {
        __try {
            g_enableBodyFlagsFn(hknpWorld, bodyId, flags, 0);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallHkArrayReserveMore(void* allocatorInfo, void* arrayBase, std::int32_t stride)
    {
        __try {
            return heisenberg::ConstraintFunctions::HkArrayReserveMore(allocatorInfo, arrayBase, stride);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallRemovePhysicsSystem(void* bhkWorld, void* instance)
    {
        __try {
            heisenberg::ConstraintFunctions::BhkWorld_RemovePhysicsSystemInstance(bhkWorld, instance);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void* ReadNativeInstance(void* physSys)
    {
        if (!physSys) return nullptr;
        __try {
            return *reinterpret_cast<void**>(
                reinterpret_cast<char*>(physSys) + kBhkPhysicsSystem_Instance);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static bool CallSetTransform(void* collObj, const void* hkXform64)
    {
        __try {
            g_setXformFn(collObj, reinterpret_cast<const float*>(hkXform64));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CallSetVelocity(void* collObj, const void* linVel4, const void* angVel4)
    {
        __try {
            g_setVelFn(collObj,
               reinterpret_cast<const float*>(linVel4),
               reinterpret_cast<const float*>(angVel4));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // ROCK's CAS-decrement-then-vtable[0] refcount-release (ref @ object+0x08).
    // No C++ objects in this fn → __try OK.
    static void ReleaseRefCounted(void* obj)
    {
        if (!obj) return;
        auto* refCountDw = reinterpret_cast<volatile long*>(reinterpret_cast<char*>(obj) + 0x08);
        for (;;) {
            long oldVal = *refCountDw;
            std::uint16_t rc = static_cast<std::uint16_t>(oldVal & 0xFFFF);
            if (rc == 0 || rc == 0xFFFF) return;
            long newVal = (oldVal & static_cast<long>(0xFFFF0000u)) |
                          static_cast<long>(static_cast<std::uint16_t>(rc - 1));
            if (_InterlockedCompareExchange(refCountDw, newVal, oldVal) == oldVal) {
                if (rc - 1 == 0) {
                    __try {
                        auto** vtable = *reinterpret_cast<void***>(obj);
                        auto destructor = reinterpret_cast<void (*)(void*, int)>(vtable[0]);
                        destructor(obj, 1);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        // Can't spdlog here — has C++ format objects. Swallow silently.
                    }
                }
                return;
            }
        }
    }

    // ──────────────────────────────────────────────────────────────────────────
    // hkArray append-one (mirrors ROCK's lambda). Returns slot ptr or nullptr.
    // No C++ objects → callable from anywhere.
    // ──────────────────────────────────────────────────────────────────────────
    static char* HkArrayAppendOne(char* arrayBase, std::int32_t stride)
    {
        auto*& dataPtr = *reinterpret_cast<char**>(arrayBase);
        auto&  size    = *reinterpret_cast<std::int32_t*>(arrayBase + 0x08);
        auto&  capFlg  = *reinterpret_cast<std::int32_t*>(arrayBase + 0x0C);
        std::int32_t cap = capFlg & 0x3FFFFFFF;

        if (size >= cap) {
            void* allocatorInfo = reinterpret_cast<void*>(
                heisenberg::ConstraintFunctions::HavokAllocatorInfo.address());
            if (!CallHkArrayReserveMore(allocatorInfo, arrayBase, stride)) {
                return nullptr;
            }
        }
        if (!dataPtr) return nullptr;
        char* newEntry = dataPtr + (std::ptrdiff_t)size * stride;
        size++;
        return newEntry;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Create
    // ──────────────────────────────────────────────────────────────────────────
    bool BethesdaPhysicsBody::Create(void* bhkWorld, void* hknpWorld, void* hknpShape,
                                     std::uint32_t collisionFilterInfo, MotionType motionType,
                                     const char* debugName)
    {
        if (_created) {
            spdlog::warn("[BethesdaBody] Create called on already-created body");
            return false;
        }
        if (!bhkWorld || !hknpWorld || !hknpShape) {
            spdlog::error("[BethesdaBody] Create: null param (bhkWorld={:p} hknpWorld={:p} shape={:p})",
                          bhkWorld, hknpWorld, hknpShape);
            return false;
        }

        // 1. Allocate + construct hknpPhysicsSystemData.
        _systemData = heisenberg::bethesda_alloc::Allocate(0x78);
        if (!_systemData) {
            spdlog::error("[BethesdaBody] Failed to allocate systemData (0x78)");
            return false;
        }
        std::memset(_systemData, 0, 0x78);
        if (!CallSystemDataCtor(_systemData)) {
            spdlog::error("[BethesdaBody] AV in PhysicsSystemDataCtor");
            _systemData = nullptr;
            return false;
        }

        // 2. (Non-static) Append a MotionCinfo.
        std::uint32_t localMotionIdx = 0x7FFFFFFFu;
        if (motionType != MotionType::Static) {
            char* motionArrBase = reinterpret_cast<char*>(_systemData) + SD::kMotionCinfos;
            std::int32_t sizeBefore = *reinterpret_cast<std::int32_t*>(motionArrBase + 0x08);
            char* mcinfo = HkArrayAppendOne(motionArrBase, 0x70);
            if (!mcinfo || !CallMotionCinfoCtor(mcinfo)) {
                spdlog::error("[BethesdaBody] MotionCinfo append/ctor failed");
                _systemData = nullptr;
                return false;
            }
            localMotionIdx = static_cast<std::uint32_t>(sizeBefore);
        }

        // 3. Append + populate BodyCinfo.
        {
            char* bodyArrBase = reinterpret_cast<char*>(_systemData) + SD::kBodyCinfos;
            char* bcinfo = HkArrayAppendOne(bodyArrBase, 0x60);
            if (!bcinfo || !CallBodyCinfoCtor(bcinfo)) {
                spdlog::error("[BethesdaBody] BodyCinfo append/ctor failed");
                _systemData = nullptr;
                return false;
            }
            // Jul 6: keyframed quality REVERTED 2 -> 0xFF (mirrors the embed). On the
            // hknpPhysicsSystemData->AddToWorld path 0xFF auto-assigns the game's keyframed
            // quality (speculative contacts + no-deactivation); 2 starved hand-object contacts
            // (tunneling + no clutter wake). Matches proven standalone ROCK.
            std::uint16_t qualityId = (motionType == MotionType::Static)    ? 0 :
                                      (motionType == MotionType::Dynamic)   ? 1 : 0xFF;
            *reinterpret_cast<void**>        (bcinfo + BC::kShape)         = hknpShape;
            *reinterpret_cast<std::uint32_t*>(bcinfo + BC::kBodyIndex)     = 0x7FFFFFFFu;
            *reinterpret_cast<std::uint32_t*>(bcinfo + BC::kMotionIndex)   = localMotionIdx;
            *reinterpret_cast<std::uint16_t*>(bcinfo + BC::kQualityId)     = qualityId;
            *reinterpret_cast<std::uint16_t*>(bcinfo + BC::kMaterialIndex) = 0;
            *reinterpret_cast<std::uint32_t*>(bcinfo + BC::kFilterInfo)    = collisionFilterInfo;
            *reinterpret_cast<const char**>  (bcinfo + BC::kName)          = debugName;
            *reinterpret_cast<std::uintptr_t*>(bcinfo + BC::kReserved)     = 0;
        }

        // 4. Append a Material.
        {
            char* matArrBase = reinterpret_cast<char*>(_systemData) + SD::kMaterials;
            char* mat = HkArrayAppendOne(matArrBase, 0x50);
            if (!mat || !CallMaterialCtor(mat)) {
                spdlog::error("[BethesdaBody] Material append/ctor failed");
                _systemData = nullptr;
                return false;
            }
        }

        // 5. Shape ref slot.
        {
            char* shpArrBase = reinterpret_cast<char*>(_systemData) + SD::kShapes;
            char* shpSlot = HkArrayAppendOne(shpArrBase, 8);
            if (shpSlot) {
                *reinterpret_cast<void**>(shpSlot) = hknpShape;
            }
        }

        // 6. Alloc + ctor bhkPhysicsSystem.
        _physicsSystem = heisenberg::bethesda_alloc::Allocate(0x28);
        if (!_physicsSystem) {
            spdlog::error("[BethesdaBody] Failed to allocate bhkPhysicsSystem (0x28)");
            _systemData = nullptr;
            return false;
        }
        std::memset(_physicsSystem, 0, 0x28);
        if (!CallBhkPhysicsSystemCtor(_physicsSystem, _systemData)) {
            spdlog::error("[BethesdaBody] AV in BhkPhysicsSystemCtor");
            _physicsSystem = nullptr;
            _systemData = nullptr;
            return false;
        }
        _systemData = nullptr;  // wrapper owns it now

        // 7. Alloc + ctor bhkNPCollisionObject.
        _collisionObject = heisenberg::bethesda_alloc::Allocate(0x30);
        if (!_collisionObject) {
            spdlog::error("[BethesdaBody] Failed to allocate bhkNPCollisionObject (0x30)");
            ReleaseRefCounted(_physicsSystem);
            _physicsSystem = nullptr;
            return false;
        }
        std::memset(_collisionObject, 0, 0x30);
        if (!CallBhkNPCollisionObjectCtor(_collisionObject, _physicsSystem)) {
            spdlog::error("[BethesdaBody] AV in BhkNPCollisionObjectCtor");
            ReleaseRefCounted(_collisionObject);
            _collisionObject = nullptr;
            ReleaseRefCounted(_physicsSystem);
            _physicsSystem = nullptr;
            return false;
        }

        // 8. AddToWorld — engine populates physSys+0x18 with the native instance.
        if (!CallAddToWorld(_collisionObject, bhkWorld)) {
            spdlog::error("[BethesdaBody] AV in AddToWorld");
            // Release the refcounted wrappers we already built — the caller only does
            // `delete bb`, and ~BethesdaPhysicsBody is defaulted (does NOT call Destroy),
            // so without this they leak. Destroy is safe here (_collisionObject is set).
            Destroy(bhkWorld);
            return false;
        }

        // 9. GetBodyId.
        std::uint32_t bodyId = 0x7FFFFFFFu;
        if (!CallGetBodyId(_physicsSystem, &bodyId) || bodyId == 0x7FFFFFFFu) {
            spdlog::error("[BethesdaBody] GetBodyId failed (id={:08X})", bodyId);
            Destroy(bhkWorld);
            return false;
        }
        _bodyId = bodyId;

        // 10. Motion-type promotion.
        if (motionType == MotionType::Keyframed) {
            CallSetBodyKeyframed(hknpWorld, _bodyId);
        } else {
            CallSetMotionType(_collisionObject, static_cast<int>(motionType));
        }

        // 11. Body flags (contact modifier + keep-awake).
        CallEnableFlags(hknpWorld, _bodyId, 0x08020000u);

        _created = true;
        spdlog::info("[BethesdaBody] Created '{}': bodyId=0x{:04X} collObj={:p} physSys={:p}",
                     debugName ? debugName : "?", _bodyId, _collisionObject, _physicsSystem);
        return true;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Destroy
    // ──────────────────────────────────────────────────────────────────────────
    void BethesdaPhysicsBody::Destroy(void* bhkWorld)
    {
        if (!_created && !_collisionObject) return;
        const std::uint32_t savedId = _bodyId;

        void* nativeInst = ReadNativeInstance(_physicsSystem);
        if (bhkWorld && nativeInst) {
            CallRemovePhysicsSystem(bhkWorld, nativeInst);
        }
        if (_collisionObject) {
            ReleaseRefCounted(_collisionObject);
            _collisionObject = nullptr;
        }
        _physicsSystem = nullptr;
        _systemData = nullptr;
        _bodyId = 0x7FFFFFFFu;
        _created = false;
        spdlog::debug("[BethesdaBody] Destroyed body 0x{:04X}", savedId);
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Drive
    // ──────────────────────────────────────────────────────────────────────────
    bool BethesdaPhysicsBody::SetTransform(const void* hkTransformf64)
    {
        if (!IsValid()) return false;
        return CallSetTransform(_collisionObject, hkTransformf64);
    }

    bool BethesdaPhysicsBody::SetVelocity(const void* linVel4, const void* angVel4)
    {
        if (!IsValid()) return false;
        return CallSetVelocity(_collisionObject, linVel4, angVel4);
    }
}
