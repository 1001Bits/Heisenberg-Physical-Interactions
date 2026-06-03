#include "WeaponCollision.h"
#include "Config.h"
#include "GrabConstraint.h"
#include "Utils.h"
#include "VRInput.h"
#include "WandNodeHelper.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"

#include <cmath>
#include <spdlog/spdlog.h>

// =========================================================================
// SEH HELPERS (Must be separate functions without C++ objects)
// =========================================================================

static volatile bool g_weaponSehException = false;

static std::uint32_t SafeCreateWeaponBody(void* funcPtr, void* hknpWorld, void* bodyCinfo)
{
    using Func_t = std::uint32_t(__fastcall*)(void*, void*, int, unsigned char);
    g_weaponSehException = false;
    __try {
        // AdditionMode = 0 (ADD_ACTIVE), AdditionFlags = 0
        return ((Func_t)funcPtr)(hknpWorld, bodyCinfo, 0, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_weaponSehException = true;
        return 0x7FFFFFFE;
    }
}

static bool SafeApplyHardKeyFrame(void* funcPtr, void* world, std::uint32_t bodyId,
                                   const void* position, const void* orientation, float invDt)
{
    using Func_t = void(__fastcall*)(void*, std::uint32_t, const void*, const void*, float);
    __try {
        ((Func_t)funcPtr)(world, bodyId, position, orientation, invDt);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* SafeReadVtable(void* ptr)
{
    __try {
        return *reinterpret_cast<void**>(ptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

namespace heisenberg
{
    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    bool WeaponCollision::Initialize()
    {
        if (_initialized) return true;

        _weaponBody.Invalidate();
        _lastWeaponDrawn = false;
        _lastWorld = nullptr;
        _initialized = true;

        spdlog::info("[WEAPON_COLLISION] Weapon collision system initialized");
        return true;
    }

    void WeaponCollision::Shutdown()
    {
        std::scoped_lock lock(_mutex);
        DestroyWeaponBody();
        _initialized = false;
        spdlog::info("[WEAPON_COLLISION] Weapon collision system shut down");
    }

    void WeaponCollision::ClearState()
    {
        std::scoped_lock lock(_mutex);
        DestroyWeaponBody();
        _lastWeaponDrawn = false;
        _lastWorld = nullptr;
    }

    bool WeaponCollision::HasWeaponBody() const
    {
        return _weaponBody.IsValid();
    }

    std::uint32_t WeaponCollision::GetBodyId() const
    {
        return _weaponBody.bodyId;
    }

    // =========================================================================
    // MAIN UPDATE
    // =========================================================================

    void WeaponCollision::Update(float deltaTime)
    {
        if (!_initialized || !g_config.enableWeaponCollision) return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;

        std::scoped_lock lock(_mutex);

        void* world = GetCurrentHknpWorld();

        // World changed - destroy existing body
        if (_weaponBody.IsValid() && world != _weaponBody.hknpWorld) {
            spdlog::info("[WEAPON_COLLISION] Physics world changed, destroying weapon body");
            DestroyWeaponBody();
        }

        bool weaponDrawn = IsWeaponDrawnAndMelee();

        // Get weapon hand node (weapon is in primary hand)
        // In normal mode primary = right, in LH mode primary = left
        bool weaponIsLeft = f4vr::isLeftHandedMode();
        auto* f4vrPlayer = f4vr::getPlayer();
        if (!f4vrPlayer) return;

        auto* nodes = reinterpret_cast<f4cf::f4vr::PlayerNodes*>(
            reinterpret_cast<uintptr_t>(f4vrPlayer) + 0x6E0);

        RE::NiNode* wandNode = GetWandNode(nodes, weaponIsLeft);
        if (!wandNode) return;

        RE::NiPoint3 handPos = wandNode->world.translate;
        RE::NiMatrix3 handRot = wandNode->world.rotate;

        if (weaponDrawn && world && _externalEnabled) {
            // Weapon is drawn - create body if needed
            if (!_weaponBody.IsValid()) {
                RE::NiPoint3 weaponCenter = ComputeWeaponCenter(handPos, handRot);
                if (CreateWeaponBody(world, weaponCenter, handRot)) {
                    spdlog::info("[WEAPON_COLLISION] Created weapon collision body");
                }
            }

            if (_weaponBody.IsValid()) {
                // Move weapon body each frame
                RE::NiPoint3 weaponCenter = ComputeWeaponCenter(handPos, handRot);
                float invDt = (deltaTime > 0.001f) ? (1.0f / deltaTime) : 90.0f;
                MoveWeaponBody(weaponCenter, handRot, invDt);

                // Update collision state
                double now = Utils::GetTime();
                double elapsed = now - _weaponBody.createdTime;
                bool shouldEnable = _externalEnabled && (elapsed > ENABLE_DELAY);
                UpdateCollisionState(shouldEnable);
            }
        } else {
            // No weapon drawn or externally disabled - remove body
            if (_weaponBody.IsValid()) {
                spdlog::info("[WEAPON_COLLISION] Weapon sheathed/disabled, destroying body");
                DestroyWeaponBody();
            }
        }

        _lastWeaponDrawn = weaponDrawn;
        _lastWorld = world;
    }

    // =========================================================================
    // BODY CREATION
    // =========================================================================

    bool WeaponCollision::CreateWeaponBody(void* hknpWorld,
                                            const RE::NiPoint3& position,
                                            const RE::NiMatrix3& rotation)
    {
        if (!hknpWorld) return false;

        // Validate world pointer
        if (!SafeReadVtable(hknpWorld)) {
            spdlog::error("[WEAPON_COLLISION] Invalid hknpWorld pointer");
            return false;
        }

        // =====================================================================
        // 1. CREATE SHAPE - Elongated box approximating weapon geometry
        // =====================================================================
        // Weapon extends forward (Y axis) from hand
        // Half extents: X = width/2, Y = length/2, Z = thickness/2

        float halfLength = g_config.weaponCollisionLength * 0.5f * HAVOK_SCALE;
        float halfWidth = g_config.weaponCollisionWidth * 0.5f * HAVOK_SCALE;
        float halfHeight = g_config.weaponCollisionHeight * 0.5f * HAVOK_SCALE;

        RE::NiPoint4 halfExtents(halfWidth, halfLength, halfHeight, 0.0f);
        constexpr float convexRadius = 0.05f;

        ConstraintFunctions::hknpConvexShapeBuildConfig buildConfig;
        ConstraintFunctions::BuildConfigCtor(&buildConfig);

        void* shape = ConstraintFunctions::CreateConvexShapeFromHalfExtents(
            halfExtents, convexRadius, &buildConfig);

        if (!shape) {
            spdlog::error("[WEAPON_COLLISION] Failed to create weapon shape");
            return false;
        }

        // =====================================================================
        // 2. CREATE BODY CINFO
        // =====================================================================

        hknpBodyCinfo bodyCinfo;
        ConstraintFunctions::BodyCinfoCtor(&bodyCinfo);

        bodyCinfo.shape = shape;

        // Position in Havok units
        bodyCinfo.position = RE::NiPoint4(
            position.x * HAVOK_SCALE,
            position.y * HAVOK_SCALE,
            position.z * HAVOK_SCALE,
            0.0f);

        // Orientation from rotation matrix
        RE::NiPoint4 quat = MatrixToQuaternion(rotation);
        bodyCinfo.orientation = quat;

        // KEYFRAMED motion (we control position manually)
        bodyCinfo.qualityId = static_cast<std::uint8_t>(hknpMotionPropertiesId::KEYFRAMED);

        // =====================================================================
        // 3. COLLISION FILTER (same pattern as Skyrim HIGGS)
        // =====================================================================
        // Layer 56 (custom), player group in bits 16-31
        // Bit 15 = collide with same group that has bit 15
        // Bit 14 = collision disabled initially (enabled after delay)
        // Ragdoll bits 8-12: LeftHand=1, RightHand=2

        bool weaponIsLeft = f4vr::isLeftHandedMode();
        std::uint32_t filterInfo = 0x38;  // Layer 56
        filterInfo |= (1 << 15);          // Bit 15: same-group collision

        std::uint8_t ragdollBits = weaponIsLeft ? 1 : 2;
        filterInfo |= (ragdollBits << 8);

        filterInfo |= (1 << 14);          // Bit 14: collision OFF initially

        bodyCinfo.collisionFilterInfo = filterInfo;

        spdlog::info("[WEAPON_COLLISION] Creating body: halfExtents=({:.3f},{:.3f},{:.3f}), "
                     "filter=0x{:08X}, shape={:p}",
                     halfWidth, halfLength, halfHeight, filterInfo, shape);

        // =====================================================================
        // 4. CREATE BODY
        // =====================================================================

        using hknpWorld_createBody_t = std::uint32_t(__fastcall*)(
            void*, hknpBodyCinfo*, int, unsigned char);
        static REL::Relocation<hknpWorld_createBody_t> hknpWorld_createBody{
            REL::Offset(0x1543ff0) };

        std::uint32_t bodyId = SafeCreateWeaponBody(
            (void*)hknpWorld_createBody.address(), hknpWorld, &bodyCinfo);

        if (g_weaponSehException) {
            spdlog::error("[WEAPON_COLLISION] createBody CRASHED (SEH)");
            return false;
        }

        if (bodyId == INVALID_BODY_ID) {
            spdlog::error("[WEAPON_COLLISION] createBody returned invalid ID");
            return false;
        }

        spdlog::info("[WEAPON_COLLISION] Created body ID=0x{:08X}", bodyId);

        // =====================================================================
        // 5. STORE RESULTS
        // =====================================================================

        _weaponBody.bodyId = bodyId;
        _weaponBody.shape = shape;
        _weaponBody.hknpWorld = hknpWorld;
        _weaponBody.valid = true;
        _weaponBody.collisionEnabled = false;  // Disabled initially (bit 14 set)
        _weaponBody.collisionFilterInfo = filterInfo;
        _weaponBody.createdTime = Utils::GetTime();

        return true;
    }

    void WeaponCollision::DestroyWeaponBody()
    {
        if (!_weaponBody.IsValid()) return;

        spdlog::info("[WEAPON_COLLISION] Destroying weapon body ID=0x{:08X}", _weaponBody.bodyId);

        if (_weaponBody.hknpWorld) {
            ConstraintFunctions::DestroyBodies(
                _weaponBody.hknpWorld, &_weaponBody.bodyId, 1, 0);
        }

        _weaponBody.Invalidate();
    }

    // =========================================================================
    // BODY MOVEMENT (via ApplyHardKeyFrame for proper velocity-based keyframing)
    // =========================================================================

    void WeaponCollision::MoveWeaponBody(const RE::NiPoint3& position,
                                          const RE::NiMatrix3& rotation,
                                          float invDeltaTime)
    {
        if (!_weaponBody.IsValid() || !_weaponBody.hknpWorld) return;

        // Position in Havok units
        RE::NiPoint4 hkPosition(
            position.x * HAVOK_SCALE,
            position.y * HAVOK_SCALE,
            position.z * HAVOK_SCALE,
            0.0f);

        // Orientation as quaternion
        RE::NiPoint4 hkOrientation = MatrixToQuaternion(rotation);

        // ApplyHardKeyFrame - sets velocity so body moves to target position
        // This is what Skyrim HIGGS uses (ApplyHardKeyframeVelocityClamped)
        // Much better than setBodyPosition which teleports without velocity
        bool ok = SafeApplyHardKeyFrame(
            (void*)ConstraintFunctions::ApplyHardKeyFrameBodyId.address(),
            _weaponBody.hknpWorld,
            _weaponBody.bodyId,
            &hkPosition,
            &hkOrientation,
            invDeltaTime);

        if (!ok) {
            spdlog::error("[WEAPON_COLLISION] ApplyHardKeyFrame CRASHED (SEH)");
            // Body may be corrupted - destroy it
            _weaponBody.Invalidate();
        }
    }

    // =========================================================================
    // COLLISION STATE (bit 14 toggle)
    // =========================================================================

    void WeaponCollision::UpdateCollisionState(bool shouldEnable)
    {
        if (!_weaponBody.IsValid()) return;

        bool wasEnabled = _weaponBody.collisionEnabled;

        if (shouldEnable && !wasEnabled) {
            // Clear bit 14 to enable collision
            std::uint32_t newFilter = _weaponBody.collisionFilterInfo & ~(1u << 14);
            _weaponBody.collisionFilterInfo = newFilter;
            _weaponBody.collisionEnabled = true;

            // Update filter in hknpWorld
            // hknpWorld::setBodyCollisionFilterInfo(bodyId, filterInfo)
            // VR offset: 0x153af00 - Status 4 (Verified)
            using SetFilterFn = void(__fastcall*)(void*, std::uint32_t, std::uint32_t);
            static REL::Relocation<SetFilterFn> setFilter{ REL::Offset(0x153af00) };

            try {
                setFilter(_weaponBody.hknpWorld, _weaponBody.bodyId, newFilter);
            } catch (...) {
                spdlog::error("[WEAPON_COLLISION] Exception in setBodyCollisionFilterInfo");
            }

            spdlog::info("[WEAPON_COLLISION] Collision ENABLED (filter=0x{:08X})", newFilter);
        } else if (!shouldEnable && wasEnabled) {
            // Set bit 14 to disable collision
            std::uint32_t newFilter = _weaponBody.collisionFilterInfo | (1u << 14);
            _weaponBody.collisionFilterInfo = newFilter;
            _weaponBody.collisionEnabled = false;

            using SetFilterFn = void(__fastcall*)(void*, std::uint32_t, std::uint32_t);
            static REL::Relocation<SetFilterFn> setFilter{ REL::Offset(0x153af00) };

            try {
                setFilter(_weaponBody.hknpWorld, _weaponBody.bodyId, newFilter);
            } catch (...) {
                spdlog::error("[WEAPON_COLLISION] Exception in setBodyCollisionFilterInfo");
            }

            spdlog::debug("[WEAPON_COLLISION] Collision DISABLED");
        }
    }

    // =========================================================================
    // HELPERS
    // =========================================================================

    void* WeaponCollision::GetCurrentHknpWorld()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return nullptr;

        using GetbhkWorld_t = void* (*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };

        void* bhkWorld = GetbhkWorld(player->parentCell);
        if (!bhkWorld) return nullptr;

        // hknpBSWorld* at bhkWorld + 0x60
        return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkWorld) + 0x60);
    }

    bool WeaponCollision::IsWeaponDrawnAndMelee() const
    {
        // Must use f4vr::getPlayer() for actorState/middleProcess access
        // RE::PlayerCharacter doesn't expose these in CommonLibF4
        auto* player = f4vr::getPlayer();
        if (!player) return false;

        if (!player->actorState.IsWeaponDrawn()) return false;

        // Check if equipped weapon exists and is melee
        // Access via middleProcess chain (same as Grab.cpp HasRealWeaponEquipped)
        if (!player->middleProcess) return false;
        auto* unk08 = player->middleProcess->unk08;
        if (!unk08) return false;
        auto* equipData = unk08->equipData;
        if (!equipData) return false;
        auto* item = equipData->item;
        if (!item) return false;

        // Check if it has a name (real weapon, not Unarmed)
        const char* name = item->GetFullName();
        if (!name || name[0] == '\0') return false;

        // IsWeaponDrawn() guarantees this is a weapon, name check filters Unarmed.
        // Cast to RE::TESObjectWEAP and check if melee type.
        auto* weapon = reinterpret_cast<RE::TESObjectWEAP*>(item);
        return weapon->IsMeleeWeapon();
    }

    RE::NiPoint3 WeaponCollision::ComputeWeaponCenter(const RE::NiPoint3& handPos,
                                                       const RE::NiMatrix3& handRot) const
    {
        // Weapon extends FORWARD from the hand along the Y axis (Bethesda convention)
        // The collision body center should be offset forward by half the weapon length
        // plus a small handle offset (the hand grips the handle end)

        float halfLength = g_config.weaponCollisionLength * 0.5f;
        float handleOffset = g_config.weaponCollisionHandleOffset;

        // Forward direction = Y column of rotation matrix
        RE::NiPoint3 forward;
        forward.x = handRot.entry[0][1];
        forward.y = handRot.entry[1][1];
        forward.z = handRot.entry[2][1];

        // Center = handPos + forward * (handleOffset + halfLength)
        float totalOffset = handleOffset + halfLength;
        RE::NiPoint3 center;
        center.x = handPos.x + forward.x * totalOffset;
        center.y = handPos.y + forward.y * totalOffset;
        center.z = handPos.z + forward.z * totalOffset;

        return center;
    }

    RE::NiPoint4 WeaponCollision::MatrixToQuaternion(const RE::NiMatrix3& m)
    {
        // Shepperd method - numerically stable matrix to quaternion conversion
        // NiMatrix3.entry[row][col] is row-major
        // Returns (x, y, z, w) matching Havok's hkQuaternionf layout

        float m00 = m.entry[0][0];
        float m11 = m.entry[1][1];
        float m22 = m.entry[2][2];
        float trace = m00 + m11 + m22;

        float x, y, z, w;

        if (trace > 0.0f) {
            float s = std::sqrt(trace + 1.0f) * 2.0f;  // s = 4*w
            w = 0.25f * s;
            x = (m.entry[2][1] - m.entry[1][2]) / s;
            y = (m.entry[0][2] - m.entry[2][0]) / s;
            z = (m.entry[1][0] - m.entry[0][1]) / s;
        } else if (m00 > m11 && m00 > m22) {
            float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;  // s = 4*x
            w = (m.entry[2][1] - m.entry[1][2]) / s;
            x = 0.25f * s;
            y = (m.entry[0][1] + m.entry[1][0]) / s;
            z = (m.entry[0][2] + m.entry[2][0]) / s;
        } else if (m11 > m22) {
            float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;  // s = 4*y
            w = (m.entry[0][2] - m.entry[2][0]) / s;
            x = (m.entry[0][1] + m.entry[1][0]) / s;
            y = 0.25f * s;
            z = (m.entry[1][2] + m.entry[2][1]) / s;
        } else {
            float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;  // s = 4*z
            w = (m.entry[1][0] - m.entry[0][1]) / s;
            x = (m.entry[0][2] + m.entry[2][0]) / s;
            y = (m.entry[1][2] + m.entry[2][1]) / s;
            z = 0.25f * s;
        }

        return RE::NiPoint4(x, y, z, w);
    }
}
