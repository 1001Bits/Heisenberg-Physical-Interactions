#include "HandCollision.h"
#include "Config.h"
#include "ContactImpulseListener.h"
#include "MenuChecker.h"
#include "Physics.h"
#include "PlayerCharacterProxyListener.h"
#include "Grab.h"
#include "GrabConstraint.h"
#include "FingerCurves.h"   // GetTriangles / GetClosestMeshPointToPoint for touch-only mesh check
#include "ThrownObjectTracker.h"
#include "BethesdaPhysicsBody.h"
#include "rock_integration/HandBoneColliderSet.h"
#include "rock_integration/BodyBoneColliderSet.h"
#include "rock_integration/WeaponCollision.h"
#include "rock_integration/CollisionLayerPolicy.h"
#include "rock_integration/TwoHandedGrip.h"
#include "Utils.h"
#include "VRInput.h"
#include "f4vr/PlayerNodes.h"
#include <f4vr/F4VRUtils.h>
#include "WandNodeHelper.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// Temporarily disable the HandCollision implementation without deleting it.
#if 0

// =====================================================================
// bhkNPCollisionProxyObject - Proxy collision object structure
// Inherits from bhkNPCollisionObjectBase (NOT bhkNPCollisionObject!)
// =====================================================================
// Helper to get the target collision object from a proxy
inline RE::bhkNPCollisionObject* GetProxyTarget(RE::NiCollisionObject* proxyObj)
{
    if (!proxyObj) return nullptr;
    
    uintptr_t proxyAddr = reinterpret_cast<uintptr_t>(proxyObj);
    
    // Try offset 0x20 first
    RE::bhkNPCollisionObject** targetPtrAt20 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x20);
    if (*targetPtrAt20) return *targetPtrAt20;
    
    // Try offset 0x28
    RE::bhkNPCollisionObject** targetPtrAt28 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x28);
    if (*targetPtrAt28) return *targetPtrAt28;
    
    // Try offset 0x30 
    RE::bhkNPCollisionObject** targetPtrAt30 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x30);
    if (*targetPtrAt30) return *targetPtrAt30;
    
    return nullptr;
}

// =========================================================================
// SEH HELPER FUNCTIONS (Must be in separate functions with no C++ objects)
// These use raw function pointers to avoid any C++ object creation
// =========================================================================

// Flag to track if SEH caught an exception (can't use C++ objects in SEH functions)
static volatile bool g_sehExceptionCaught = false;

// Helper to safely call hknpWorld::createBody
// CORRECT signature: int* hknpWorld_createBody(void* world, int* outBodyId, hknpBodyCinfo* cinfo, int additionMode, char flags)
// param_2 is an OUTPUT parameter — the bodyId is written there
// Returns bodyId on success, 0x7FFFFFFE on SEH exception
static std::uint32_t SafeCallCreateBody(void* funcPtr, void* hknpWorld, void* bodyCinfo)
{
    using Func_t = std::int32_t*(__fastcall*)(void*, std::int32_t*, void*, int, unsigned char);
    g_sehExceptionCaught = false;
    __try {
        std::int32_t outBodyId = 0x7FFFFFFF;
        ((Func_t)funcPtr)(hknpWorld, &outBodyId, bodyCinfo, 0, 0);
        return static_cast<std::uint32_t>(outBodyId);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_sehExceptionCaught = true;
        return 0x7FFFFFFE;
    }
}

// Helper to safely read vtable from a pointer
static void* SafeReadVtable(void* ptr)
{
    __try {
        return *reinterpret_cast<void**>(ptr);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Helper to safely call hknpBSWorld::applyHardKeyFrame for moving bodies
static bool SafeCallApplyHardKeyFrame(void* funcPtr, void* world, std::uint32_t bodyId, void* position, void* orientation, float deltaTime)
{
    using Func_t = void(__fastcall*)(void*, std::uint32_t, void*, void*, float);
    __try {
        ((Func_t)funcPtr)(world, bodyId, position, orientation, deltaTime);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper to safely call hknpWorld::destroyBodies
static bool SafeCallDestroyBodies(void* funcPtr, void* hknpWorld, std::uint32_t* bodyIds, int count)
{
    using Func_t = void(__fastcall*)(void*, const std::uint32_t*, int, int);
    __try {
        ((Func_t)funcPtr)(hknpWorld, bodyIds, count, 0);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper to safely call bhkNPCollisionObject::AddToWorld
static bool SafeCallAddToWorld(void* funcPtr, void* collisionObject, void* bhkWorld)
{
    using Func_t = void(__fastcall*)(void*, void*);
    __try {
        ((Func_t)funcPtr)(collisionObject, bhkWorld);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper to safely call bhkNPCollisionObject::SetMotionType
static bool SafeCallSetMotionType(void* funcPtr, void* collisionObject, int motionType)
{
    using Func_t = void(__fastcall*)(void*, int);
    __try {
        ((Func_t)funcPtr)(collisionObject, motionType);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeCallSetBodyCollisionFilterInfo(void* funcPtr, void* hknpWorld, std::uint32_t bodyId, std::uint32_t filterInfo)
{
    using Func_t = void(__fastcall*)(void*, std::uint32_t, std::uint32_t);
    __try {
        ((Func_t)funcPtr)(hknpWorld, bodyId, filterInfo);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace heisenberg
{
    namespace
    {
        constexpr double kHandBodyCreationSettleDelaySeconds = 0.75;
        constexpr double kPlayerBodyRetryCooldownSeconds = 3.0;
        constexpr double kHandBodyPostCreateMoveDelaySeconds = 0.10;
        constexpr double kHandBodyCollisionEnableDelaySeconds = 0.30;
        // Use kClutter (layer 4) with proper F4VR filter format (0x02EF prefix)
        // This collides with world objects but the proxy listener handles player capsule
        // hknpGroupCollisionFilter Config<5,5,5,16>:
        //   bits 0-4:   layer (5 bits)
        //   bits 5-9:   system group (5 bits)
        //   bits 10-14: sub-system ID (5 bits)
        //   bits 15-30: sub-system don't-collide mask (16 bits)
        // Bodies in the SAME system group with matching sub-system mask DON'T collide.
        constexpr std::uint32_t kHandCollisionLayer = 4;  // kClutter
        constexpr std::uint32_t kHandCollisionDisabledBit = (1u << 14);
        constexpr std::uint32_t kSystemGroupMask = (0x1Fu << 5);   // bits 5-9
        constexpr std::uint32_t kSubSystemMask = (0x1Fu << 10);    // bits 10-14
        constexpr std::uint32_t kSubSystemDontCollideMask = (0xFFFFu << 15); // bits 15-30

        // Helper to get bhkWorld from cell (avoids REL::Relocation in __try blocks)
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> g_GetbhkWorld{ REL::Offset(0x39b070) };
        void* GetbhkWorldForCell(RE::TESObjectCELL* cell) {
            return cell ? g_GetbhkWorld(cell) : nullptr;
        }

        // Cached player collision group — read once, reused
        static std::uint32_t g_playerCollisionFilterInfo = 0;
        static bool g_playerFilterCached = false;

        std::uint32_t ReadPlayerCollisionFilterInfo()
        {
            if (g_playerFilterCached && g_playerCollisionFilterInfo != 0) {
                return g_playerCollisionFilterInfo;
            }

            // Read from the player proxy body via the body buffer
            std::uint32_t playerBodyId = heisenberg::Physics::GetPlayerBodyId();
            if (playerBodyId == 0x7FFFFFFF || playerBodyId == 0) {
                return 0;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) return 0;
            void* bhkWorld = GetbhkWorldForCell(player->parentCell);
            if (!bhkWorld) return 0;
            void* hknpWorld = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkWorld) + 0x60);
            if (!hknpWorld) return 0;

            // Read filter from body buffer: body buffer at hknpWorld+0x20, stride 0x90, filterInfo at +0x40
            __try {
                void* bodyBuffer = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x20);
                if (!bodyBuffer) return 0;
                std::uint32_t bodyIndex = playerBodyId & 0xFFFF;
                std::uint32_t filterInfo = *reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<uintptr_t>(bodyBuffer) + bodyIndex * 0x90 + 0x40);
                g_playerCollisionFilterInfo = filterInfo;
                g_playerFilterCached = true;
                spdlog::info("[HAND_COLLISION] Read player collision filter: 0x{:08X} (group={}, layer={})",
                             filterInfo, (filterInfo >> 5) & 0x1F, filterInfo & 0x1F);
                return filterInfo;
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        std::uint32_t BuildHandCollisionFilter(bool /*isLeft*/, bool collisionEnabled)
        {
            // Start with kClutter layer
            std::uint32_t filterInfo = kHandCollisionLayer;

            // Copy player's system group so hand doesn't collide with player
            // (HIGGS pattern: same group = no collision)
            std::uint32_t playerFilter = ReadPlayerCollisionFilterInfo();
            if (playerFilter != 0) {
                // Copy system group bits (5-9) from player
                filterInfo |= (playerFilter & kSystemGroupMask);
                // Copy sub-system bits (10-14) from player
                filterInfo |= (playerFilter & kSubSystemMask);
                // Copy don't-collide mask (15-30) from player
                filterInfo |= (playerFilter & kSubSystemDontCollideMask);
            } else {
                // Fallback: use a known working filter format with disabled bit
                filterInfo = 0x02EF0004;
            }

            if (!collisionEnabled) {
                filterInfo |= kHandCollisionDisabledBit;
            }

            return filterInfo;
        }

        void ResetPlayerFilterCache()
        {
            g_playerFilterCached = false;
            g_playerCollisionFilterInfo = 0;
        }

        void MatrixToQuaternion(const RE::NiMatrix3& m, RE::NiPoint4& outQuat)
        {
            float trace = m.entry[0][0] + m.entry[1][1] + m.entry[2][2];

            if (trace > 0.0f) {
                float s = 0.5f / sqrtf(trace + 1.0f);
                outQuat.w = 0.25f / s;
                outQuat.x = (m.entry[2][1] - m.entry[1][2]) * s;
                outQuat.y = (m.entry[0][2] - m.entry[2][0]) * s;
                outQuat.z = (m.entry[1][0] - m.entry[0][1]) * s;
            } else if (m.entry[0][0] > m.entry[1][1] && m.entry[0][0] > m.entry[2][2]) {
                float s = 2.0f * sqrtf(1.0f + m.entry[0][0] - m.entry[1][1] - m.entry[2][2]);
                outQuat.w = (m.entry[2][1] - m.entry[1][2]) / s;
                outQuat.x = 0.25f * s;
                outQuat.y = (m.entry[0][1] + m.entry[1][0]) / s;
                outQuat.z = (m.entry[0][2] + m.entry[2][0]) / s;
            } else if (m.entry[1][1] > m.entry[2][2]) {
                float s = 2.0f * sqrtf(1.0f + m.entry[1][1] - m.entry[0][0] - m.entry[2][2]);
                outQuat.w = (m.entry[0][2] - m.entry[2][0]) / s;
                outQuat.x = (m.entry[0][1] + m.entry[1][0]) / s;
                outQuat.y = 0.25f * s;
                outQuat.z = (m.entry[1][2] + m.entry[2][1]) / s;
            } else {
                float s = 2.0f * sqrtf(1.0f + m.entry[2][2] - m.entry[0][0] - m.entry[1][1]);
                outQuat.w = (m.entry[1][0] - m.entry[0][1]) / s;
                outQuat.x = (m.entry[0][2] + m.entry[2][0]) / s;
                outQuat.y = (m.entry[1][2] + m.entry[2][1]) / s;
                outQuat.z = 0.25f * s;
            }
        }
    }

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    bool HandCollision::Initialize()
    {
        if (_initialized) {
            return true;
        }

        spdlog::info("[HAND_COLLISION] Physics-based hand collision system initializing...");
        
        // Clear state
        _leftHandBody.Invalidate();
        _rightHandBody.Invalidate();
        _leftContact.reset();
        _rightContact.reset();
        _bodyCreationBlockedUntil = Utils::GetTime() + kHandBodyCreationSettleDelaySeconds;
        _playerBodyRetryBlockedUntil = 0.0;

        // The custom proxy-listener vtable is still incomplete in VR.
        // During real trigger-volume callbacks the engine dispatches additional
        // listener methods beyond processConstraintsCallback, which makes the
        // synthetic listener crash inside hknpCharacterProxyInternals.
        // Keep hand collision on the safer pair-filter path until the full
        // hknpCharacterProxyListener ABI is mapped.
        spdlog::info("[HAND_COLLISION] Player proxy listener registration is disabled; using player-body pair filtering only");

        _initialized = true;
        spdlog::info("[HAND_COLLISION] Hand collision system initialized (bodies will be created on first update)");
        return true;
    }

    void HandCollision::Shutdown()
    {
        spdlog::info("[HAND_COLLISION] Shutting down hand collision system...");

        // Invalidate hand bodies — don't try to destroy them.
        // During save/load the physics world may already be torn down,
        // and calling DestroyBodies on a dead world crashes.
        {
            std::scoped_lock lock(_handBodyMutex);
            _leftHandBody.Invalidate();
            _rightHandBody.Invalidate();
        }

        _leftContact.reset();
        _rightContact.reset();
        _bodyCreationBlockedUntil = Utils::GetTime() + kHandBodyCreationSettleDelaySeconds;
        _playerBodyRetryBlockedUntil = 0.0;
        ResetPlayerFilterCache();

        _initialized = false;
    }

    // =========================================================================
    // BODY CREATION (called from HookEndUpdate)
    // =========================================================================

    void HandCollision::CreateBodiesIfNeeded(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos)
    {
        if (!g_config.enableHandCollision || !g_config.usePhysicsHandBodies) {
            return;
        }

        if (!_initialized) {
            Initialize();
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return;
        }

        void* hknpWorld = GetCurrentHknpWorld();
        void* bhkWorld = GetCurrentBhkWorld();
        if (!hknpWorld || !bhkWorld) {
            return;
        }

        // Check if bodies need creation
        bool needsCreation = false;
        {
            std::scoped_lock lock(_handBodyMutex);
            bool worldChanged = (_leftHandBody.IsValid() && _leftHandBody.hknpWorld != hknpWorld) ||
                                (_rightHandBody.IsValid() && _rightHandBody.hknpWorld != hknpWorld);
            if (worldChanged) {
                spdlog::info("[HAND_COLLISION] Physics world changed — invalidating old hand bodies (old world already cleaned them up)");
                // Don't call DestroyPhysicsHandBody — the old world is gone.
                // Just reset our tracking state.
                _leftHandBody.Invalidate();
                _rightHandBody.Invalidate();
                ResetPlayerFilterCache();
            }
            needsCreation = !_leftHandBody.IsValid() || !_rightHandBody.IsValid();
        }

        if (!needsCreation) {
            return;
        }

        if (!CanCreatePhysicsHandBodiesNow()) {
            return;
        }

        std::scoped_lock lock(_handBodyMutex);
        Physics::WorldWriteLock worldLock(reinterpret_cast<RE::bhkWorld*>(bhkWorld));
        if (!worldLock.IsLocked()) {
            spdlog::warn("[HAND_COLLISION] Failed to acquire world write lock; skipping physics hand body creation");
            return;
        }

        if (!_leftHandBody.IsValid()) {
            spdlog::info("[HAND_COLLISION] Creating LEFT hand body at ({:.1f}, {:.1f}, {:.1f})",
                         leftHandPos.x, leftHandPos.y, leftHandPos.z);
            if (!CreatePhysicsHandBody(_leftHandBody, hknpWorld, bhkWorld, leftHandPos, true)) {
                return;
            }
        }

        if (!_rightHandBody.IsValid()) {
            spdlog::info("[HAND_COLLISION] Creating RIGHT hand body at ({:.1f}, {:.1f}, {:.1f})",
                         rightHandPos.x, rightHandPos.y, rightHandPos.z);
            if (!CreatePhysicsHandBody(_rightHandBody, hknpWorld, bhkWorld, rightHandPos, false)) {
                return;
            }
        }
    }

    // =========================================================================
    // PAIR FILTER (called from EndUpdate — no physics locks)
    // =========================================================================

    void HandCollision::ApplyPlayerPairFilterIfNeeded()
    {
        if (!_initialized || !g_config.enableHandCollision || !g_config.usePhysicsHandBodies) {
            return;
        }

        std::scoped_lock lock(_handBodyMutex);

        // Skip if both hands already have the pair filter applied
        if ((!_leftHandBody.IsValid() || _leftHandBody.playerPairFilterApplied) &&
            (!_rightHandBody.IsValid() || _rightHandBody.playerPairFilterApplied)) {
            return;
        }

        std::uint32_t playerBodyId = Physics::GetPlayerBodyId();
        if (playerBodyId == 0x7FFFFFFF || playerBodyId == 0) {
            return;
        }

        void* hknpWorld = GetCurrentHknpWorld();
        if (!hknpWorld) {
            return;
        }

        // CRITICAL: if the pair filter call throws, the hand body and the player
        // capsule both stay in the world with mutual collision active. The next
        // physics step then immediately shoves the player away from wherever the
        // hand body was created, which the user perceives as being teleported on
        // load. Tear the body down rather than leave it as a permanent shover.
        //
        // Note: collision-enable is NOT toggled here. HIGGS-style: bodies spawn
        // with bit 14 set (no collision) and stay that way until per-frame Update
        // turns it on after kHandBodyCollisionEnableDelaySeconds, giving the world
        // time to settle and the body time to receive its first keyframe move.
        // Enabling immediately here was the load-shove regression.
        if (_leftHandBody.IsValid() && !_leftHandBody.playerPairFilterApplied) {
            if (Physics::DisableCollisionBetween(hknpWorld, _leftHandBody.bodyId, playerBodyId)) {
                _leftHandBody.playerPairFilterApplied = true;
                spdlog::info("[HAND_COLLISION] Applied player pair filter to LEFT hand body 0x{:08X} vs player 0x{:08X}",
                             _leftHandBody.bodyId, playerBodyId);
            } else {
                spdlog::error("[HAND_COLLISION] LEFT pair filter FAILED — destroying body 0x{:08X} so it can't shove the player",
                              _leftHandBody.bodyId);
                DestroyPhysicsHandBody(_leftHandBody);
            }
        }

        if (_rightHandBody.IsValid() && !_rightHandBody.playerPairFilterApplied) {
            if (Physics::DisableCollisionBetween(hknpWorld, _rightHandBody.bodyId, playerBodyId)) {
                _rightHandBody.playerPairFilterApplied = true;
                spdlog::info("[HAND_COLLISION] Applied player pair filter to RIGHT hand body 0x{:08X} vs player 0x{:08X}",
                             _rightHandBody.bodyId, playerBodyId);
            } else {
                spdlog::error("[HAND_COLLISION] RIGHT pair filter FAILED — destroying body 0x{:08X} so it can't shove the player",
                              _rightHandBody.bodyId);
                DestroyPhysicsHandBody(_rightHandBody);
            }
        }
    }

    // =========================================================================
    // MAIN UPDATE (called from post-physics hook — position updates only)
    // =========================================================================

    void HandCollision::Update(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos,
                                const RE::NiPoint3& leftHandVel, const RE::NiPoint3& rightHandVel,
                                const RE::NiMatrix3& leftHandRot, const RE::NiMatrix3& rightHandRot,
                                float deltaTime)
    {
        if (!g_config.enableHandCollision) {
            return;
        }

        // Store hand state for any external readers
        _leftHandPos = leftHandPos;
        _rightHandPos = rightHandPos;
        _leftHandVel = leftHandVel;
        _rightHandVel = rightHandVel;

        if (!g_config.usePhysicsHandBodies) {
            return;
        }

        void* hknpWorld = GetCurrentHknpWorld();
        if (!hknpWorld) {
            return;
        }

        const double now = Utils::GetTime();
        std::scoped_lock lock(_handBodyMutex);

        // Per-hand HIGGS-style update lambda. Mirrors hand.cpp:UpdateHandCollision
        // verbatim — toggle collision based on grab state, then move via keyframe.
        auto updateOne = [&](PhysicsHandBody& body,
                             const RE::NiPoint3& pos,
                             const RE::NiMatrix3& rot,
                             bool isLeft)
        {
            if (!body.IsValid() || body.hknpWorld != hknpWorld) {
                return;
            }
            // Pair filter must succeed before we ever turn collision on, otherwise
            // enabling collision shoves the player.
            if (!body.playerPairFilterApplied) {
                UpdateHandBodyPosition(body, pos, rot, deltaTime);
                return;
            }

            // HIGGS hand.cpp:658 — disable hand collision while physically held
            // (HeldBody) or while two-handing a long weapon. We match the spirit
            // even if Heisenberg's grab/two-hand state lives elsewhere.
            const auto& grabMgr = GrabManager::GetSingleton();
            const auto& thisGrab  = grabMgr.GetGrabState(isLeft);
            const auto& otherGrab = grabMgr.GetGrabState(!isLeft);
            const bool inHeldBody = thisGrab.usingHeldBodyGrab || thisGrab.heldBodyConstraintActive;
            auto thisRefr  = thisGrab.refrHandle.get();
            auto otherRefr = otherGrab.refrHandle.get();
            const bool isTwoHanding = thisGrab.active && otherGrab.active &&
                                      thisRefr.get() && thisRefr.get() == otherRefr.get();
            const bool shouldDisableCollision = inHeldBody || isTwoHanding;

            if (shouldDisableCollision) {
                if (body.collisionEnabled) {
                    SetHandCollisionEnabled(body, false);
                }
            } else {
                // HIGGS hand.cpp:669 — only enable once the post-create settle
                // window has elapsed. handCollisionCreatedTime is `createdTime`
                // here; kHandBodyCollisionEnableDelaySeconds is the F4VR analogue
                // of Config::options.handWeaponCollisionEnableDelay.
                if (!body.collisionEnabled
                    && (now - body.createdTime) >= kHandBodyCollisionEnableDelaySeconds)
                {
                    SetHandCollisionEnabled(body, true);
                }
            }

            // Move body via keyframed velocity (HIGGS ApplyHardKeyframeVelocityClamped).
            UpdateHandBodyPosition(body, pos, rot, deltaTime);
        };

        updateOne(_leftHandBody,  leftHandPos,  leftHandRot,  true);
        updateOne(_rightHandBody, rightHandPos, rightHandRot, false);

        // Heartbeat (~3s) so we can confirm Update is reaching the body-update path.
        static double lastUpdateLog = 0.0;
        if (now - lastUpdateLog > 3.0) {
            spdlog::info("[HAND_COLLISION] Update tick: L valid={} pairFilter={} colOn={}, R valid={} pairFilter={} colOn={}",
                         _leftHandBody.IsValid(),  _leftHandBody.playerPairFilterApplied,  _leftHandBody.collisionEnabled,
                         _rightHandBody.IsValid(), _rightHandBody.playerPairFilterApplied, _rightHandBody.collisionEnabled);
            lastUpdateLog = now;
        }
    }

#if 0  // OLD UPDATE CODE — body creation/pair filter moved to HookPlayerCharacterUpdate
        if (false && hknpWorld && bhkWorld) {
            std::uint32_t playerBodyId = 0x7FFFFFFF;
            bool needsPlayerCollisionState = false;
            bool haveHandBodies = false;

            {
                std::scoped_lock lock(_handBodyMutex);

                bool worldChanged = (_leftHandBody.IsValid() && _leftHandBody.hknpWorld != hknpWorld) ||
                                    (_rightHandBody.IsValid() && _rightHandBody.hknpWorld != hknpWorld);

                if (worldChanged) {
                    spdlog::info("[HAND_COLLISION] Physics world changed, recreating hand bodies");
                    DestroyPhysicsHandBody(_leftHandBody);
                    DestroyPhysicsHandBody(_rightHandBody);
                }

                needsPlayerCollisionState =
                    (_leftHandBody.IsValid() && !_leftHandBody.playerPairFilterApplied) ||
                    (_rightHandBody.IsValid() && !_rightHandBody.playerPairFilterApplied);

                haveHandBodies = _leftHandBody.IsValid() || _rightHandBody.IsValid();
            }

            const bool shouldResolvePlayerCollisionNow =
                needsPlayerCollisionState && (!haveHandBodies || now >= _playerBodyRetryBlockedUntil);

            if (CanCreatePhysicsHandBodiesNow(shouldResolvePlayerCollisionNow ? &playerBodyId : nullptr)) {
                const bool playerCollisionReady =
                    playerBodyId != 0 && playerBodyId != 0x7FFFFFFF && playerBodyId != 0xFFFFFFFF;

                if (playerCollisionReady) {
                    _playerBodyRetryBlockedUntil = 0.0;
                } else if (needsPlayerCollisionState && shouldResolvePlayerCollisionNow) {
                    _playerBodyRetryBlockedUntil = now + kPlayerBodyRetryCooldownSeconds;
                }

                if (!playerCollisionReady && shouldResolvePlayerCollisionNow && needsPlayerCollisionState) {
                    static double lastUnfilteredHandBodyLogTime = 0.0;
                    if (now - lastUnfilteredHandBodyLogTime >= 1.0) {
                        spdlog::warn("[HAND_COLLISION] No player controller body is exposed in this VR runtime; keeping physics hand bodies non-collidable until player filtering is available");
                        lastUnfilteredHandBodyLogTime = now;
                    }
                }

                if (GetCurrentBhkWorld() == bhkWorld && GetCurrentHknpWorld() == hknpWorld) {
                    std::scoped_lock lock(_handBodyMutex);
                    auto& proxyListener = PlayerCharacterProxyListener::GetSingleton();

                    if (_leftHandBody.IsValid() && !_leftHandBody.proxyRegistered) {
                        proxyListener.RegisterHandBodyId(_leftHandBody.bodyId);
                        _leftHandBody.proxyRegistered = true;
                    }

                    if (_rightHandBody.IsValid() && !_rightHandBody.proxyRegistered) {
                        proxyListener.RegisterHandBodyId(_rightHandBody.bodyId);
                        _rightHandBody.proxyRegistered = true;
                    }

                    if (_leftHandBody.IsValid() && !_leftHandBody.playerPairFilterApplied && playerCollisionReady) {
                        if (!TryDisableCollisionWithPlayer(_leftHandBody, playerBodyId)) {
                            static double lastLeftPairFilterLogTime = 0.0;
                            if (now - lastLeftPairFilterLogTime >= 1.0) {
                                spdlog::warn("[HAND_COLLISION] Waiting to apply player collision filter to LEFT hand body 0x{:08X}",
                                             _leftHandBody.bodyId);
                                lastLeftPairFilterLogTime = now;
                            }
                        }
                    }

                    if (_rightHandBody.IsValid() && !_rightHandBody.playerPairFilterApplied && playerCollisionReady) {
                        if (!TryDisableCollisionWithPlayer(_rightHandBody, playerBodyId)) {
                            static double lastRightPairFilterLogTime = 0.0;
                            if (now - lastRightPairFilterLogTime >= 1.0) {
                                spdlog::warn("[HAND_COLLISION] Waiting to apply player collision filter to RIGHT hand body 0x{:08X}",
                                             _rightHandBody.bodyId);
                                lastRightPairFilterLogTime = now;
                            }
                        }
                    }
                }

                if (GetCurrentBhkWorld() == bhkWorld && GetCurrentHknpWorld() == hknpWorld) {
                    std::scoped_lock lock(_handBodyMutex);
                    const bool leftReadyForMovement =
                        _leftHandBody.IsValid() &&
                        (_leftHandBody.playerPairFilterApplied || !playerCollisionReady) &&
                        (now - _leftHandBody.createdTime >= kHandBodyPostCreateMoveDelaySeconds);
                    const bool rightReadyForMovement =
                        _rightHandBody.IsValid() &&
                        (_rightHandBody.playerPairFilterApplied || !playerCollisionReady) &&
                        (now - _rightHandBody.createdTime >= kHandBodyPostCreateMoveDelaySeconds);

                    if (!leftReadyForMovement && !rightReadyForMovement) {
                        if (_leftHandBody.IsValid() || _rightHandBody.IsValid()) {
                            static double lastPostCreateDelayLogTime = 0.0;
                            if (now - lastPostCreateDelayLogTime >= 1.0) {
                                spdlog::info("[HAND_COLLISION] Waiting briefly before first keyframe update of newly created hand bodies");
                                lastPostCreateDelayLogTime = now;
                            }
                        }
                    } else {
                        Physics::WorldWriteLock moveWorldLock(reinterpret_cast<RE::bhkWorld*>(bhkWorld));
                        if (!moveWorldLock.IsLocked()) {
                            spdlog::warn("[HAND_COLLISION] Failed to reacquire world write lock; skipping hand body movement");
                        } else {
                            if (leftReadyForMovement) {
                                UpdateHandBodyPosition(_leftHandBody, leftHandPos, leftHandRot, deltaTime);

                                if (!_leftHandBody.collisionEnabled &&
                                    _leftHandBody.playerPairFilterApplied &&
                                    (now - _leftHandBody.createdTime >= kHandBodyCollisionEnableDelaySeconds)) {
                                    SetHandCollisionEnabled(_leftHandBody, true);
                                }
                            }

                            if (rightReadyForMovement) {
                                UpdateHandBodyPosition(_rightHandBody, rightHandPos, rightHandRot, deltaTime);

                                if (!_rightHandBody.collisionEnabled &&
                                    _rightHandBody.playerPairFilterApplied &&
                                    (now - _rightHandBody.createdTime >= kHandBodyCollisionEnableDelaySeconds)) {
                                    SetHandCollisionEnabled(_rightHandBody, true);
                                }
                            }

                            if ((_leftHandBody.IsValid() && !_leftHandBody.collisionEnabled && !_leftHandBody.playerPairFilterApplied) ||
                                (_rightHandBody.IsValid() && !_rightHandBody.collisionEnabled && !_rightHandBody.playerPairFilterApplied)) {
                                static double lastCollisionDeferralLogTime = 0.0;
                                if (now - lastCollisionDeferralLogTime >= 1.0) {
                                    if (playerCollisionReady) {
                                        spdlog::warn("[HAND_COLLISION] Keeping native hand-body collision staged OFF until player pair filtering is available; proximity fallback remains active");
                                    } else {
                                        spdlog::warn("[HAND_COLLISION] Keeping native hand-body collision staged OFF because no player controller body is exposed in this VR runtime; proximity fallback remains active");
                                    }
                                    lastCollisionDeferralLogTime = now;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Proximity-based collision as supplement
        CheckProximityCollisions(leftHandPos, leftHandVel, true);
        CheckProximityCollisions(rightHandPos, rightHandVel, false);
    }

#endif  // OLD UPDATE CODE

    // Old Update body creation code also disabled below
        ConstraintFunctions::PhysicsSystemDataCtor(systemDataMem);

        void* materialMem = _aligned_malloc(sizeof(hknpMaterialDescriptor), 16);
        if (!materialMem) {
            spdlog::error("[HAND_COLLISION] Failed to allocate hknpMaterialDescriptor");
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(materialMem, 0, sizeof(hknpMaterialDescriptor));

        void* bodyCinfoMem = _aligned_malloc(sizeof(hknpBodyCinfo), 16);
        if (!bodyCinfoMem) {
            spdlog::error("[HAND_COLLISION] Failed to allocate hknpBodyCinfo");
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        auto* bodyCinfo = reinterpret_cast<hknpBodyCinfo*>(bodyCinfoMem);
        ConstraintFunctions::BodyCinfoCtor(bodyCinfo);
        bodyCinfo->shape = shape;
        bodyCinfo->position = RE::NiPoint4(
            position.x * HAVOK_WORLD_SCALE_COLLISION,
            position.y * HAVOK_WORLD_SCALE_COLLISION,
            position.z * HAVOK_WORLD_SCALE_COLLISION,
            0.0f
        );
        bodyCinfo->orientation = RE::NiPoint4(0.0f, 0.0f, 0.0f, 1.0f);
        bodyCinfo->qualityId = static_cast<std::uint8_t>(hknpMotionPropertiesId::KEYFRAMED);
        bodyCinfo->materialId = 0;
        bodyCinfo->collisionFilterInfo = filterInfo;
        bodyCinfo->flags = 0;

        spdlog::info("[HAND_COLLISION] Body cinfo: qualityId={}, filterInfo=0x{:08X}, shape={:p}",
                     bodyCinfo->qualityId, filterInfo, bodyCinfo->shape);
        spdlog::info("[HAND_COLLISION] hknpWorld={:p}, bhkWorld={:p}", hknpWorld, bhkWorld);

        std::uint8_t* systemDataBytes = reinterpret_cast<std::uint8_t*>(systemDataMem);
        *reinterpret_cast<void**>(systemDataBytes + 0x10) = materialMem;
        *reinterpret_cast<std::uint32_t*>(systemDataBytes + 0x18) = 1;
        *reinterpret_cast<std::uint32_t*>(systemDataBytes + 0x1C) = 0x80000001;
        *reinterpret_cast<void**>(systemDataBytes + 0x40) = bodyCinfo;
        *reinterpret_cast<std::uint32_t*>(systemDataBytes + 0x48) = 1;
        *reinterpret_cast<std::uint32_t*>(systemDataBytes + 0x4C) = 0x80000001;

        // =====================================================================
        // 3. CREATE BETHESDA WRAPPERS AND ADD BODY TO WORLD
        // =====================================================================

        void* physicsSystemMem = _aligned_malloc(0x30, 16);
        if (!physicsSystemMem) {
            spdlog::error("[HAND_COLLISION] Failed to allocate bhkPhysicsSystem");
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(physicsSystemMem, 0, 0x30);
        ConstraintFunctions::BhkPhysicsSystemCtor(physicsSystemMem, systemDataMem);

        alignas(16) std::uint8_t identityTransform[0x40] = {};
        reinterpret_cast<float*>(identityTransform)[0] = 1.0f;
        reinterpret_cast<float*>(identityTransform)[5] = 1.0f;
        reinterpret_cast<float*>(identityTransform)[10] = 1.0f;
        reinterpret_cast<float*>(identityTransform)[15] = 1.0f;

        spdlog::info("[HAND_COLLISION] Calling BhkPhysicsSystemCreateInstance...");
        ConstraintFunctions::BhkPhysicsSystemCreateInstance(physicsSystemMem, bhkWorld, identityTransform);

        void* collisionObjMem = _aligned_malloc(0x30, 16);
        if (!collisionObjMem) {
            spdlog::error("[HAND_COLLISION] Failed to allocate bhkNPCollisionObject");
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }
        std::memset(collisionObjMem, 0, 0x30);
        ConstraintFunctions::BhkNPCollisionObjectCtor(collisionObjMem, 0, physicsSystemMem);

        if (!SafeCallAddToWorld((void*)ConstraintFunctions::BhkNPCollisionObjectAddToWorld.address(), collisionObjMem, bhkWorld)) {
            spdlog::error("[HAND_COLLISION] bhkNPCollisionObject::AddToWorld CRASHED");
            _aligned_free(collisionObjMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }

        std::uint32_t bodyId = 0x7FFFFFFF;
        ConstraintFunctions::BhkPhysicsSystemGetBodyId(physicsSystemMem, &bodyId, 0);
        if (bodyId == 0x7FFFFFFF) {
            spdlog::error("[HAND_COLLISION] Failed to get body ID from bhkPhysicsSystem");
            _aligned_free(collisionObjMem);
            _aligned_free(physicsSystemMem);
            _aligned_free(bodyCinfoMem);
            _aligned_free(materialMem);
            _aligned_free(systemDataMem);
            return false;
        }

        if (!SafeCallSetMotionType((void*)ConstraintFunctions::BhkNPCollisionObjectSetMotionType.address(), collisionObjMem, 2)) {
            spdlog::warn("[HAND_COLLISION] SetMotionType(KEYFRAMED) threw SEH exception - continuing");
        }

        ConstraintFunctions::hknpWorld_commitAddBodies(hknpWorld);
        ConstraintFunctions::hknpWorld_activateBody(hknpWorld, bodyId);

        spdlog::info("[HAND_COLLISION] SUCCESS! Created {} hand body with ID: 0x{:08X}",
                     isLeft ? "LEFT" : "RIGHT", bodyId);

        // =====================================================================
        // 4. STORE RESULTS
        // =====================================================================

        handBody.bodyId = bodyId;
        handBody.shape = shape;
        handBody.hknpWorld = hknpWorld;
        handBody.bhkWorld = bhkWorld;
        handBody.physicsSystem = physicsSystemMem;
        handBody.collisionObject = collisionObjMem;
        handBody.alignedSystemDataMem = systemDataMem;
        handBody.alignedBodyCinfoMem = bodyCinfoMem;
        handBody.alignedMaterialMem = materialMem;
        handBody.alignedPhysicsSystemMem = physicsSystemMem;
        handBody.alignedCollisionObjMem = collisionObjMem;
        handBody.valid = true;
        handBody.collisionEnabled = false;
        handBody.proxyRegistered = false;
        handBody.playerPairFilterApplied = false;
        handBody.collisionFilterInfo = filterInfo;
        handBody.createdTime = Utils::GetTime();

        spdlog::info("[HAND_COLLISION] Created {} hand body 0x{:08X} with collision staged OFF (filter=0x{:08X})",
                     isLeft ? "LEFT" : "RIGHT", bodyId, filterInfo);
        
        return true;
    }

    void HandCollision::DestroyPhysicsHandBody(PhysicsHandBody& handBody)
    {
        if (!handBody.IsValid()) {
            return;
        }
        
        spdlog::info("[HAND_COLLISION] Destroying hand body id=0x{:08X}", handBody.bodyId);

        PlayerCharacterProxyListener::GetSingleton().UnregisterHandBodyId(handBody.bodyId);
        
        // NEW PATH: if this body was created via BethesdaPhysicsBody, route destruction
        // through its own Destroy() (RemovePhysicsSystem + refcount-release). The aligned*
        // fields will all be nullptr so the legacy _aligned_free calls become no-ops.
        // Cup-shape has 3 bodies — palm + 2 walls. Destroy all.
        if (handBody.bethesdaBody) {
            auto destroyBb = [&](void*& slot) {
                if (!slot) return;
                auto* bb = reinterpret_cast<heisenberg::bethesda_physics_body::BethesdaPhysicsBody*>(slot);
                bb->Destroy(handBody.bhkWorld);
                delete bb;
                slot = nullptr;
            };
            destroyBb(handBody.bethesdaBody_wallA);
            destroyBb(handBody.bethesdaBody_wallB);
            destroyBb(handBody.bethesdaBody);
        } else if (handBody.hknpWorld) {
            std::uint32_t bodyId = handBody.bodyId;
            SafeCallDestroyBodies((void*)ConstraintFunctions::DestroyBodies.address(), handBody.hknpWorld, &bodyId, 1);
        }

        if (handBody.alignedCollisionObjMem) _aligned_free(handBody.alignedCollisionObjMem);
        if (handBody.alignedPhysicsSystemMem) _aligned_free(handBody.alignedPhysicsSystemMem);
        if (handBody.alignedBodyCinfoMem) _aligned_free(handBody.alignedBodyCinfoMem);
        if (handBody.alignedMaterialMem) _aligned_free(handBody.alignedMaterialMem);
        if (handBody.alignedSystemDataMem) _aligned_free(handBody.alignedSystemDataMem);

        handBody.Invalidate();
    }

    void HandCollision::UpdateHandBodyPosition(PhysicsHandBody& handBody, 
                                                const RE::NiPoint3& position,
                                                const RE::NiMatrix3& rotation,
                                                float deltaTime)
    {
        if (!handBody.IsValid() || !handBody.hknpWorld) {
            return;
        }
        
        // Static-lifetime aligned scratch — MSVC doesn't reliably honor
        // alignas(16) on stack NiPoint4 locals; statics do respect alignas.
        struct alignas(16) UpdatePosScratch {
            float hkPosition[4];
            float hkOrientation[4];
        };
        static UpdatePosScratch s_updScratch;
        s_updScratch.hkPosition[0] = position.x * HAVOK_WORLD_SCALE_COLLISION;
        s_updScratch.hkPosition[1] = position.y * HAVOK_WORLD_SCALE_COLLISION;
        s_updScratch.hkPosition[2] = position.z * HAVOK_WORLD_SCALE_COLLISION;
        s_updScratch.hkPosition[3] = 0.0f;

        auto& hkOrient = *reinterpret_cast<RE::NiPoint4*>(s_updScratch.hkOrientation);
        MatrixToQuaternion(rotation, hkOrient);

        float invDeltaTime = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 90.0f;
        if (!SafeCallApplyHardKeyFrame(
                (void*)ConstraintFunctions::ApplyHardKeyFrameBodyId.address(),
                handBody.hknpWorld,
                handBody.bodyId,
                reinterpret_cast<RE::NiPoint4*>(s_updScratch.hkPosition),
                reinterpret_cast<RE::NiPoint4*>(s_updScratch.hkOrientation),
                invDeltaTime)) {
            spdlog::error("[HAND_COLLISION] Exception in applyHardKeyFrame");
        }
    }

    void HandCollision::SetHandCollisionEnabled(PhysicsHandBody& handBody, bool enabled)
    {
        if (!handBody.IsValid()) {
            return;
        }

        if (!handBody.hknpWorld || handBody.collisionEnabled == enabled) {
            return;
        }

        using SetFilterFn = void(__fastcall*)(void*, std::uint32_t, std::uint32_t);
        static REL::Relocation<SetFilterFn> setFilter{ REL::Offset(0x153af00) };

        std::uint32_t newFilter = handBody.collisionFilterInfo;
        if (enabled) {
            newFilter &= ~kHandCollisionDisabledBit;
        } else {
            newFilter |= kHandCollisionDisabledBit;
        }
        if (!SafeCallSetBodyCollisionFilterInfo((void*)setFilter.address(), handBody.hknpWorld, handBody.bodyId, newFilter)) {
            spdlog::warn("[HAND_COLLISION] Failed to {} collision for hand body 0x{:08X}",
                         enabled ? "enable" : "disable", handBody.bodyId);
            return;
        }

        handBody.collisionFilterInfo = newFilter;
        handBody.collisionEnabled = enabled;

        spdlog::info("[HAND_COLLISION] Collision {} for hand body 0x{:08X} (filter=0x{:08X})",
                     enabled ? "ENABLED" : "DISABLED", handBody.bodyId, newFilter);
    }

    bool HandCollision::ResolveReadyPlayerCollisionState(std::uint32_t& playerBodyId) const
    {
        playerBodyId = Physics::GetPlayerBodyId();
        if (playerBodyId == 0 || playerBodyId == 0x7FFFFFFF || playerBodyId == 0xFFFFFFFF) {
            static double lastBodyWaitLogTime = 0.0;
            double now = Utils::GetTime();
            if (now - lastBodyWaitLogTime >= 1.0) {
                spdlog::info("[HAND_COLLISION] Waiting for player controller body before applying player pair filtering to physics hand bodies");
                lastBodyWaitLogTime = now;
            }
            return false;
        }

        return true;
    }

    bool HandCollision::CanCreatePhysicsHandBodiesNow(std::uint32_t* playerBodyId) const
    {
        double now = Utils::GetTime();
        if (MenuChecker::GetSingleton().IsLoading()) {
            static double lastLoadingLogTime = 0.0;
            if (now - lastLoadingLogTime >= 0.5) {
                spdlog::info("[HAND_COLLISION] Delaying physics hand body creation while LoadingMenu is open");
                lastLoadingLogTime = now;
            }
            return false;
        }

        if (now < _bodyCreationBlockedUntil) {
            static double lastSettleLogTime = 0.0;
            if (now - lastSettleLogTime >= 0.5) {
                spdlog::info("[HAND_COLLISION] Delaying physics hand body creation for post-load settle window ({:.0f} ms remaining)",
                             (_bodyCreationBlockedUntil - now) * 1000.0);
                lastSettleLogTime = now;
            }
            return false;
        }

        if (playerBodyId) {
            std::uint32_t resolvedPlayerBodyId = 0x7FFFFFFF;
            if (ResolveReadyPlayerCollisionState(resolvedPlayerBodyId)) {
                *playerBodyId = resolvedPlayerBodyId;
            } else {
                *playerBodyId = 0x7FFFFFFF;
            }
        }

        return true;
    }

    bool HandCollision::TryDisableCollisionWithPlayer(PhysicsHandBody& handBody, std::uint32_t playerBodyId)
    {
        if (!handBody.IsValid() || !handBody.hknpWorld || handBody.playerPairFilterApplied) {
            return handBody.playerPairFilterApplied;
        }

        if (playerBodyId == 0 || playerBodyId == 0x7FFFFFFF || playerBodyId == 0xFFFFFFFF || playerBodyId == handBody.bodyId) {
            return false;
        }

        spdlog::info("[HAND_COLLISION] Calling DisableCollisionsBetween for hand body 0x{:08X} against player body 0x{:08X}",
                     handBody.bodyId, playerBodyId);
        Physics::DisableCollisionsBetween(handBody.hknpWorld, handBody.bodyId, playerBodyId);
        handBody.playerPairFilterApplied = true;

        spdlog::info("[HAND_COLLISION] Disabled player collision for hand body 0x{:08X} against player body 0x{:08X}",
                     handBody.bodyId, playerBodyId);
        return true;
    }

    // =========================================================================
    // WORLD ACCESS
    // =========================================================================

    void* HandCollision::GetCurrentHknpWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }
        
        auto* cell = player->parentCell;
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        void* bhkWorld = GetbhkWorld(cell);
        if (!bhkWorld) {
            return nullptr;
        }
        
        void* hknpWorld = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkWorld) + 0x60);
        return hknpWorld;
    }

    void* HandCollision::GetCurrentBhkWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }
        
        auto* cell = player->parentCell;
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        return GetbhkWorld(cell);
    }

    // =========================================================================
    // COLLISION HANDLING
    // =========================================================================

    void HandCollision::CheckProximityCollisions(const RE::NiPoint3& handPos,
                                                  const RE::NiPoint3& handVel,
                                                  bool isLeft)
    {
        // Entry beacon — fires once every 3s regardless of state so we can tell
        // whether this function is running at all in the user's session. If you
        // never see this log, Update() isn't reaching here.
        {
            static double lastEntryLog = 0.0;
            double nowEntry = Utils::GetTime();
            if (nowEntry - lastEntryLog > 3.0) {
                spdlog::info("[HAND_COLLISION] CheckProximityCollisions entered ({} hand) handPos=({:.0f},{:.0f},{:.0f}) handVel=({:.1f},{:.1f},{:.1f}) radius={:.1f} thresh={:.1f}",
                             isLeft ? "L" : "R",
                             handPos.x, handPos.y, handPos.z,
                             handVel.x, handVel.y, handVel.z,
                             g_config.handCollisionRadius,
                             g_config.handPushVelocityThreshold);
                lastEntryLog = nowEntry;
            }
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            static double lastNoPlayerLog = 0.0;
            double now = Utils::GetTime();
            if (now - lastNoPlayerLog > 5.0) {
                spdlog::warn("[HAND_COLLISION] CheckProximityCollisions: no player singleton — early return");
                lastNoPlayerLog = now;
            }
            return;
        }

        float collisionRadius = g_config.handCollisionRadius;

        if (isLeft) {
            _leftContact.reset();
        } else {
            _rightContact.reset();
        }

        // SEH-protect the radius query — Havok can throw access violations during
        // load/unload transitions; an unhandled SEH would silently kill the whole
        // function so even the heartbeat below wouldn't fire.
        std::vector<RE::TESObjectREFR*> nearby;
        bool getObjectsThrew = false;
        __try {
            nearby = Physics::GetObjectsInRadius(handPos, collisionRadius, player);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            getObjectsThrew = true;
        }
        if (getObjectsThrew) {
            static double lastThrewLog = 0.0;
            double now = Utils::GetTime();
            if (now - lastThrewLog > 1.0) {
                spdlog::error("[HAND_COLLISION] GetObjectsInRadius threw SEH ({} hand) handPos=({:.0f},{:.0f},{:.0f}) — proximity push disabled this frame",
                             isLeft ? "L" : "R", handPos.x, handPos.y, handPos.z);
                lastThrewLog = now;
            }
            return;
        }
        const float velMag = sqrtf(handVel.x * handVel.x + handVel.y * handVel.y + handVel.z * handVel.z);

        // Throttle proximity diagnostic — only log when something interesting happens
        // (object found AND moving). Otherwise this would spam every frame.
        bool anyGrabbable = false;
        for (auto* refr : nearby) {
            if (refr && Physics::IsGrabbable(refr)) {
                anyGrabbable = true;
                if (isLeft) {
                    _leftContact = RE::ObjectRefHandle(refr);
                } else {
                    _rightContact = RE::ObjectRefHandle(refr);
                }
                if (velMag > g_config.handPushVelocityThreshold) {
                    spdlog::info("[HAND_COLLISION] Push {} hand → refr {:08X} velMag={:.1f} (thresh={:.1f}) handPos=({:.0f},{:.0f},{:.0f})",
                                 isLeft ? "L" : "R", refr->formID, velMag,
                                 g_config.handPushVelocityThreshold, handPos.x, handPos.y, handPos.z);
                    ApplyPushForce(refr, handPos, handVel, 1.0f / 90.0f);
                } else {
                    static double lastBelowThreshLog = 0.0;
                    double now = Utils::GetTime();
                    if (now - lastBelowThreshLog > 1.0) {
                        spdlog::info("[HAND_COLLISION] {} hand near refr {:08X} but velMag={:.1f} < thresh {:.1f} — no push",
                                     isLeft ? "L" : "R", refr->formID, velMag, g_config.handPushVelocityThreshold);
                        lastBelowThreshLog = now;
                    }
                }
                break;
            }
        }
        // Heartbeat (rare) so we can confirm CheckProximityCollisions is running at all.
        if (!anyGrabbable) {
            static double lastEmpty = 0.0;
            double now = Utils::GetTime();
            if (now - lastEmpty > 5.0) {
                spdlog::info("[HAND_COLLISION] Proximity scan running ({} hand): {} candidates, none grabbable, velMag={:.1f}",
                             isLeft ? "L" : "R", nearby.size(), velMag);
                lastEmpty = now;
            }
        }
    }

    // Helper to safely cast collision objects - follows bhkNPCollisionProxyObject to its target
    // since proxy objects don't have their own physics system
    static RE::bhkNPCollisionObject* SafeCastCollisionObject(RE::NiCollisionObject* collObj)
    {
        if (!collObj) return nullptr;
        
        auto* rtti = collObj->GetRTTI();
        if (!rtti || !rtti->GetName()) {
            return nullptr;
        }
        
        const char* typeName = rtti->GetName();
        
        if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
            spdlog::trace("[HAND_COLLISION] Found ProxyObject - following target pointer");
            RE::bhkNPCollisionObject* target = GetProxyTarget(collObj);
            if (!target) {
                spdlog::trace("[HAND_COLLISION] ProxyObject has null target!");
                return nullptr;
            }
            return target;
        }
        
        if (std::strcmp(typeName, "bhkNPCollisionObject") == 0) {
            return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
        }
        
        for (auto iter = rtti; iter; iter = iter->GetBaseRTTI()) {
            if (iter->GetName() && std::strcmp(iter->GetName(), "bhkNPCollisionObject") == 0) {
                return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
            }
        }
        
        return nullptr;
    }

    void HandCollision::ApplyPushForce(RE::TESObjectREFR* refr, const RE::NiPoint3& handPos,
                                        const RE::NiPoint3& handVel, float /*deltaTime*/)
    {
        if (!refr) return;

        // Get the collision object
        auto* node = refr->Get3D();
        if (!node) return;

        RE::bhkNPCollisionObject* colObj = nullptr;
        
        // Try to get collision object from the node directly - with safe casting
        if (node->collisionObject) {
            colObj = SafeCastCollisionObject(node->collisionObject.get());
        }
        
        // If not found, try children (for NiNode)
        if (!colObj && node->IsNode()) {
            auto* asNode = static_cast<RE::NiNode*>(node);
            // Safety bound to prevent infinite loop if children array is corrupted
            uint32_t childCount = asNode->children.size();
            if (childCount > 100) childCount = 100;
            for (uint32_t i = 0; i < childCount; ++i) {
                auto* child = asNode->children[i].get();
                if (child && child->collisionObject) {
                    colObj = SafeCastCollisionObject(child->collisionObject.get());
                    if (colObj) break;
                }
            }
        }

        if (!colObj) return;

        // Validate physics system
        if (!colObj->spSystem) {
            spdlog::trace("[HAND_COLLISION] Object {:08X} has no physics system, skipping push", refr->formID);
            return;
        }

        // Apply velocity in direction hand is moving
        float pushMultiplier = g_config.handPushForceMultiplier;
        struct alignas(16) PushScratch { float pushVel[4]; };
        static PushScratch s_pushScratch;
        s_pushScratch.pushVel[0] = handVel.x * HAVOK_WORLD_SCALE_COLLISION * pushMultiplier;
        s_pushScratch.pushVel[1] = handVel.y * HAVOK_WORLD_SCALE_COLLISION * pushMultiplier;
        s_pushScratch.pushVel[2] = handVel.z * HAVOK_WORLD_SCALE_COLLISION * pushMultiplier;
        s_pushScratch.pushVel[3] = 0.0f;
        auto& pushVel = *reinterpret_cast<RE::NiPoint4*>(s_pushScratch.pushVel);

        if (CollisionFunctions::IsCollisionObjectValid(colObj)) {
            CollisionFunctions::SetLinearVelocity(colObj, pushVel);
            spdlog::info("[HAND_COLLISION] SetLinearVelocity refr {:08X} hkVel=({:.2f},{:.2f},{:.2f}) (game handVel=({:.1f},{:.1f},{:.1f}) mult={:.2f})",
                         refr->formID, s_pushScratch.pushVel[0], s_pushScratch.pushVel[1], s_pushScratch.pushVel[2],
                         handVel.x, handVel.y, handVel.z, pushMultiplier);
        } else {
            spdlog::warn("[HAND_COLLISION] Push: collision object invalid for refr {:08X}", refr->formID);
        }
    }

    // =========================================================================
    // ACCESSORS
    // =========================================================================

    bool HandCollision::IsInContact(bool isLeft) const
    {
        const auto& handle = isLeft ? _leftContact : _rightContact;
        return static_cast<bool>(handle);
    }

    RE::TESObjectREFR* HandCollision::GetContactObject(bool isLeft) const
    {
        const auto& handle = isLeft ? _leftContact : _rightContact;
        if (!handle) return nullptr;
        RE::NiPointer<RE::TESObjectREFR> refPtr = handle.get();
        return refPtr.get();
    }

    const PhysicsHandBody& HandCollision::GetHandBody(bool isLeft) const
    {
        return isLeft ? _leftHandBody : _rightHandBody;
    }

    void HandCollision::TriggerCollisionHaptics(bool isLeft, float intensity, float duration)
    {
        if (!g_config.enableHandCollisionHaptics) {
            return;
        }

        // Map (intensity, duration) to SteamVR's TriggerHapticPulse duration.
        // Havok gives us a rough mass × speed product via ProcessHandCollision;
        // TriggerHapticPulse accepts microseconds (max ~3999 per pulse). Scale
        // the product into 300–3500µs so that a light tap is a gentle click and
        // a heavy impact buzzes more firmly without saturating the controller.
        float product = std::max(0.0f, intensity) * std::max(0.0f, duration);
        float scaled = 300.0f + product * g_config.handCollisionHapticScale;
        if (scaled > 3500.0f) scaled = 3500.0f;
        if (scaled < 200.0f) scaled = 200.0f;

        g_vrInput.TriggerHaptic(isLeft, static_cast<unsigned short>(scaled));
    }
}
#endif

// =====================================================================
// CLEAN HAND COLLISION IMPLEMENTATION — ROCK-style patterns
// Based on brunocatani/ROCK (working F4VR hand collision mod)
// =====================================================================

namespace heisenberg
{
    // Scale: 1 Havok unit = 70 game units (ROCK confirmed)
    constexpr float kGameToHavok = 1.0f / 70.0f;
    constexpr float kHavokToGame = 70.0f;

    // Layer 43 for hand collision (ROCK uses 43)
    constexpr std::uint32_t kHandLayer = 43;
    constexpr std::uint32_t kHandGroup = 11;

    // Register layer 43 in collision filter matrix
    static void RegisterHandLayer(void* hknpWorld)
    {
        if (!hknpWorld) return;
        void* modMgr = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x150);
        if (!modMgr) return;
        void* filterPtr = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(modMgr) + 0x5E8);
        if (!filterPtr) return;
        uint64_t* matrix = reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(filterPtr) + 0x1A0);

        // Hand layer (43) mask via the ported ROCK CollisionLayerPolicy. Player-push avoidance
        // is done via the BODY'S FILTER INFO instead (`(0x000B << 16) | 43` — the "hand group"
        // bits the engine's pair filter uses to reject player-attached body collisions; see
        // CreateHandBody). buildRockHandExpectedMask(weaponLayer=true, staticWorld=true) is
        // verified bit-identical to ROCK's long-standing hand mask 0x000070AFBFFF7F3E;
        // applyLayerExpectedMask writes the row + symmetric column bits. (Weapon layer 44 /
        // body layer 47 are registered by their own collider modules when those are ported on.)
        namespace clp = rock::collision_layer_policy;
        const uint64_t handMask = clp::buildRockHandExpectedMask(/*includeWeaponLayer*/ true, /*includeStaticWorld*/ true);
        if (matrix[clp::ROCK_LAYER_HAND] != handMask) {
            clp::applyLayerExpectedMask(matrix, clp::ROCK_LAYER_HAND, handMask);
            spdlog::info("[HAND_COLLISION] Registered hand layer 43 via CollisionLayerPolicy (mask 0x{:016X})", handMask);
        }
    }

    // Get bhkWorld and hknpWorld from player cell
    static bool GetWorlds(void*& outBhkWorld, void*& outHknpWorld)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return false;
        using GetBhk_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetBhk_t> getBhk{ REL::Offset(0x39b070) };
        outBhkWorld = getBhk(player->parentCell);
        if (!outBhkWorld) return false;
        outHknpWorld = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(outBhkWorld) + 0x60);
        return outHknpWorld != nullptr;
    }

    static float Dot(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static float LengthSq(const RE::NiPoint3& v)
    {
        return Dot(v, v);
    }

    static float Length(const RE::NiPoint3& v)
    {
        return std::sqrt(LengthSq(v));
    }

    static RE::NiPoint3 NormalizeOrZero(const RE::NiPoint3& v)
    {
        const float len = Length(v);
        if (len <= 1.0e-4f) {
            return RE::NiPoint3();
        }
        return v * (1.0f / len);
    }

    static RE::NiPoint3 ClosestPointOnSegment(const RE::NiPoint3& p,
                                               const RE::NiPoint3& a,
                                               const RE::NiPoint3& b)
    {
        const RE::NiPoint3 ab = b - a;
        const float lenSq = LengthSq(ab);
        if (lenSq <= 1.0e-4f) {
            return a;
        }
        const float t = (std::clamp)(Dot(p - a, ab) / lenSq, 0.0f, 1.0f);
        return a + ab * t;
    }

    static bool GetObjectBounds(RE::TESObjectREFR* refr, RE::NiPoint3& center, float& radius)
    {
        auto* node = refr ? refr->Get3D() : nullptr;
        if (!node) {
            return false;
        }

        bool ok = false;
        __try {
            center = node->worldBound.center;
            radius = node->worldBound.fRadius;
            if (!(std::isfinite)(radius) || radius <= 0.1f) {
                center = node->world.translate;
                radius = 8.0f;
            }
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

    static RE::bhkNPCollisionObject* GetProxyTarget(RE::NiCollisionObject* proxyObj)
    {
        if (!proxyObj) {
            return nullptr;
        }

        RE::bhkNPCollisionObject* target = nullptr;
        __try {
            auto proxyAddr = reinterpret_cast<std::uintptr_t>(proxyObj);
            auto** at20 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x20);
            auto** at28 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x28);
            auto** at30 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x30);
            if (at20 && *at20) {
                target = *at20;
            } else if (at28 && *at28) {
                target = *at28;
            } else if (at30 && *at30) {
                target = *at30;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            target = nullptr;
        }
        return target;
    }

    static RE::bhkNPCollisionObject* SafeCastCollisionObject(RE::NiCollisionObject* collObj)
    {
        if (!collObj) {
            return nullptr;
        }

        auto* rtti = collObj->GetRTTI();
        if (!rtti || !rtti->GetName()) {
            return nullptr;
        }

        const char* typeName = rtti->GetName();
        if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
            return GetProxyTarget(collObj);
        }
        if (std::strcmp(typeName, "bhkNPCollisionObject") == 0) {
            return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
        }

        for (auto* iter = rtti; iter; iter = iter->GetBaseRTTI()) {
            if (iter->GetName() && std::strcmp(iter->GetName(), "bhkNPCollisionObject") == 0) {
                return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
            }
        }
        return nullptr;
    }

    static RE::bhkNPCollisionObject* FindCollisionObject(RE::NiAVObject* node)
    {
        if (!node) {
            return nullptr;
        }
        if (node->collisionObject) {
            if (auto* colObj = SafeCastCollisionObject(node->collisionObject.get())) {
                return colObj;
            }
        }
        if (!node->IsNode()) {
            return nullptr;
        }

        auto* asNode = static_cast<RE::NiNode*>(node);
        auto childCount = asNode->children.size();
        if (childCount > static_cast<decltype(childCount)>(100)) {
            childCount = static_cast<decltype(childCount)>(100);
        }
        for (decltype(childCount) i = 0; i < childCount; ++i) {
            auto* child = asNode->children[i].get();
            if (auto* colObj = FindCollisionObject(child)) {
                return colObj;
            }
        }
        return nullptr;
    }

    // Hard de-penetration: directly move an object's collision body OUT of an
    // overlapping volume via bhkNPCollisionObject::SetTransform (base+0x1E08A70 —
    // the same deferred-safe call MoveHandBody uses). A velocity-only push lags a
    // frame and tunnels at speed; teleporting the body just outside the hand each
    // frame guarantees it is never left clipping through, independent of hand speed.
    // Kept in its own function so the __try has no C++ objects to unwind (SEH rule).
    static void DepenetrateCollisionObject(RE::TESObjectREFR* refr, const RE::NiPoint3& worldDelta)
    {
        if (!refr) return;
        auto* node = refr->Get3D();
        if (!node) return;
        auto* colObj = FindCollisionObject(node);
        if (!colObj || !CollisionFunctions::IsCollisionObjectValid(colObj)) return;

        const RE::NiPoint3 newPos = node->world.translate + worldDelta;
        const RE::NiMatrix3& rot = node->world.rotate;

        // 16-byte-aligned, 16-float column-major transform (translation at [12..14]),
        // built in static storage — MSVC won't reliably 16-align a stack local and
        // SetTransform does movaps on it (see MoveHandBody).
        static alignas(16) float s_depenXform[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        s_depenXform[0]  = rot.entry[0][0]; s_depenXform[1]  = rot.entry[1][0]; s_depenXform[2]  = rot.entry[2][0]; s_depenXform[3]  = 0.0f;
        s_depenXform[4]  = rot.entry[0][1]; s_depenXform[5]  = rot.entry[1][1]; s_depenXform[6]  = rot.entry[2][1]; s_depenXform[7]  = 0.0f;
        s_depenXform[8]  = rot.entry[0][2]; s_depenXform[9]  = rot.entry[1][2]; s_depenXform[10] = rot.entry[2][2]; s_depenXform[11] = 0.0f;
        s_depenXform[12] = newPos.x * kGameToHavok;
        s_depenXform[13] = newPos.y * kGameToHavok;
        s_depenXform[14] = newPos.z * kGameToHavok;
        s_depenXform[15] = 1.0f;

        auto base = REL::Module::get().base();
        auto setXform = reinterpret_cast<void(__fastcall*)(void*, const float*)>(base + 0x1E08A70);
        __try {
            setXform(colObj, s_depenXform);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static bool logged = false;
            if (!logged) {
                spdlog::warn("[HAND_COLLISION] DepenetrateCollisionObject: SEH fault in SetTransform — skipping");
                logged = true;
            }
        }
    }

    void HandCollision::SetContactObject(bool isLeft, RE::TESObjectREFR* refr)
    {
        auto& handle = isLeft ? _leftContact : _rightContact;
        auto& bodyId = isLeft ? _leftContactBodyId : _rightContactBodyId;
        auto& clear = isLeft ? _pendingLeftContactClear : _pendingRightContactClear;

        if (refr) {
            handle = RE::ObjectRefHandle(refr);
            bodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
            clear.store(false, std::memory_order_relaxed);
        } else {
            handle.reset();
            bodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
            clear.store(false, std::memory_order_relaxed);
        }
    }

    void HandCollision::ClearContactObject(bool isLeft)
    {
        SetContactObject(isLeft, nullptr);
    }

    void HandCollision::SetContactBodyId(bool isLeft, std::uint32_t bodyId)
    {
        auto& target = isLeft ? _leftContactBodyId : _rightContactBodyId;
        auto& clear = isLeft ? _pendingLeftContactClear : _pendingRightContactClear;
        target.store(bodyId, std::memory_order_relaxed);
        clear.store(false, std::memory_order_relaxed);
    }

    void HandCollision::ClearContactBodyId(bool isLeft)
    {
        auto& target = isLeft ? _leftContactBodyId : _rightContactBodyId;
        auto& clear = isLeft ? _pendingLeftContactClear : _pendingRightContactClear;
        target.store(0x7FFFFFFF, std::memory_order_relaxed);
        clear.store(true, std::memory_order_relaxed);
    }

    void HandCollision::ApplyPushForce(RE::TESObjectREFR* refr, const RE::NiPoint3& handPos,
                                       const RE::NiPoint3& handVel, float /*deltaTime*/)
    {
        if (!refr) {
            return;
        }

        auto* node = refr->Get3D();
        auto* colObj = FindCollisionObject(node);
        if (!colObj || !CollisionFunctions::IsCollisionObjectValid(colObj)) {
            return;
        }

        const float pushMultiplier = (std::max)(0.01f, g_config.handPushForceMultiplier);

        // PRESERVE GRAVITY: SetLinearVelocity REPLACES the body's velocity, so if we wrote
        // handVel.z into the body each frame the object's gravity-derived fall would be
        // overridden and a just-released item would "stick" to the hand. Read the current
        // Z, push only the horizontal components, and keep the existing Z (gravity preserved).
        // If the hand is actively pushing DOWN harder than the object is falling, allow the
        // push to take over (so smacking something down still works).
        struct alignas(16) VelScratch { float v[4]; };
        static VelScratch s_curScratch;
        s_curScratch.v[0] = 0.0f; s_curScratch.v[1] = 0.0f;
        s_curScratch.v[2] = 0.0f; s_curScratch.v[3] = 0.0f;
        auto& curVel = *reinterpret_cast<RE::NiPoint4*>(s_curScratch.v);
        __try {
            CollisionFunctions::GetLinearVelocity(colObj, curVel);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // If reading fails, fall back to zero — push will set the velocity outright.
            s_curScratch.v[0] = 0.0f; s_curScratch.v[1] = 0.0f;
            s_curScratch.v[2] = 0.0f; s_curScratch.v[3] = 0.0f;
        }

        const float pushZ_hk = handVel.z * kGameToHavok * pushMultiplier;
        const float curZ_hk  = s_curScratch.v[2];
        // Keep gravity unless the hand is actively pushing downward harder than the object's
        // current downward motion.
        float finalZ_hk = curZ_hk;
        if (pushZ_hk < curZ_hk - 0.1f) {  // pushing down faster than current fall
            finalZ_hk = pushZ_hk;
        }

        struct alignas(16) PushScratch { float pushVel[4]; };
        static PushScratch s_pushScratch;
        s_pushScratch.pushVel[0] = handVel.x * kGameToHavok * pushMultiplier;
        s_pushScratch.pushVel[1] = handVel.y * kGameToHavok * pushMultiplier;
        s_pushScratch.pushVel[2] = finalZ_hk;
        s_pushScratch.pushVel[3] = 0.0f;

        auto& pushVel = *reinterpret_cast<RE::NiPoint4*>(s_pushScratch.pushVel);
        CollisionFunctions::SetLinearVelocity(colObj, pushVel);

        spdlog::debug("[HAND_COLLISION] Swept push refr {:08X} from ({:.1f},{:.1f},{:.1f}) hkVel=({:.2f},{:.2f},{:.2f}) preservedZ={:.2f}",
                      refr->formID, handPos.x, handPos.y, handPos.z,
                      s_pushScratch.pushVel[0], s_pushScratch.pushVel[1], s_pushScratch.pushVel[2],
                      curZ_hk);
    }

    void HandCollision::CheckProximityCollisions(const RE::NiPoint3& handPos,
                                                 const RE::NiPoint3& handVel,
                                                 bool isLeft,
                                                 float deltaTime)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            ClearContactObject(isLeft);
            return;
        }

        // Mutually exclusive with the physics-body path: when usePhysicsHandBodies is on, the
        // real hand bodies do the colliding — no proximity push needed (and running both would
        // double-push / fight).
        if (g_config.usePhysicsHandBodies) {
            ClearContactObject(isLeft);
            return;
        }

        // Skip the push entirely for a hand that's currently holding an object — otherwise
        // it shoves the held object / nearby clutter and fires constant haptics. (ROCK
        // disables its hand collision while grabbing; we match that.)
        if (GrabManager::GetSingleton().IsGrabbing(isLeft)) {
            ClearContactObject(isLeft);
            return;
        }

        if (deltaTime <= 0.0001f || !(std::isfinite)(deltaTime)) {
            deltaTime = 1.0f / 90.0f;
        }

        const float handSpeed = Length(handVel);
        const float handRadius = (std::max)(1.0f, g_config.handCollisionRadius);

        // FULL-HAND TOUCH: a single sphere at the wand under-reaches fingers/back/sides. The
        // sample cloud must match the player's RENDERED hand at any approach angle, so we build
        // it from the actual first-person finger bones (the same distal bones the finger-segment
        // colliders use) — they track the live finger pose. Each fingertip bone is nudged outward
        // toward the true tip; we add the wrist (controller) and a palm-centre point. If the
        // skeleton can't be resolved this frame we fall back to a fixed controller-local cloud
        // (X mirrored for the left hand).
        const RE::NiMatrix3& handRot = isLeft ? _leftHandRot : _rightHandRot;
        auto handLocalToWorld = [&](float lx, float ly, float lz) -> RE::NiPoint3 {
            const float mx = isLeft ? -lx : lx;
            return RE::NiPoint3(
                handPos.x + handRot.entry[0][0]*mx + handRot.entry[0][1]*ly + handRot.entry[0][2]*lz,
                handPos.y + handRot.entry[1][0]*mx + handRot.entry[1][1]*ly + handRot.entry[1][2]*lz,
                handPos.z + handRot.entry[2][0]*mx + handRot.entry[2][1]*ly + handRot.entry[2][2]*lz);
        };

        RE::NiPoint3 handPts[16];
        int handPtCount = 0;

        // Preferred: real rendered finger bones → cloud follows the visible hand pose, so contact
        // is measured from where the player actually sees their fingers (no controller-relative gap).
        {
            static const char* const kFingerTipBones[2][5] = {
                { "RArm_Finger13", "RArm_Finger23", "RArm_Finger33", "RArm_Finger43", "RArm_Finger53" },
                { "LArm_Finger13", "LArm_Finger23", "LArm_Finger33", "LArm_Finger43", "LArm_Finger53" },
            };
            auto* pl = f4vr::getPlayer();
            RE::NiAVObject* root = (pl && pl->firstPersonSkeleton) ? pl->firstPersonSkeleton : nullptr;
            if (root) {
                const char* const* names = kFingerTipBones[isLeft ? 1 : 0];
                RE::NiPoint3 tipSum(0.0f, 0.0f, 0.0f);
                int tips = 0;
                for (int i = 0; i < 5; ++i) {
                    if (auto* b = f4vr::findAVObject(root, names[i])) {
                        // Distal bone origin sits at the base of the last segment; nudge ~2 units
                        // along the finger direction (wrist→bone) to approximate the actual tip.
                        const RE::NiPoint3 tip = b->world.translate +
                            NormalizeOrZero(b->world.translate - handPos) * 2.0f;
                        handPts[handPtCount++] = tip;
                        tipSum = tipSum + tip;
                        ++tips;
                    }
                }
                if (tips > 0) {
                    handPts[handPtCount++] = handPos;                                    // wrist / palm root
                    const RE::NiPoint3 tipC = tipSum * (1.0f / static_cast<float>(tips));
                    handPts[handPtCount++] = (handPos + tipC) * 0.5f;                    // palm centre
                }
            }
        }

        // Fallback: fixed controller-local cloud (skeleton not resolvable). Local Y ~ toward
        // fingers, local Z ~ up/out of palm, covering a ~14cm-long hand volume.
        if (handPtCount < 3) {
            static const float kHandLocal[][3] = {
                { 0.0f,  0.0f,  0.0f},   // wrist / wand
                { 0.0f,  6.0f,  3.0f},   // palm center
                { 0.0f, 13.0f,  1.0f},   // middle fingertip
                {-3.5f, 12.0f,  1.0f},   // index/ring fingertips
                { 3.5f, 11.0f,  2.0f},   // thumb tip
                { 0.0f,  4.0f, -4.0f},   // back of hand
                {-4.0f,  6.0f,  0.0f},   // pinky edge
                { 4.0f,  7.0f,  0.0f},   // thumb-base edge
            };
            handPtCount = 0;
            for (auto& l : kHandLocal) {
                handPts[handPtCount++] = handLocalToWorld(l[0], l[1], l[2]);
            }
        }

        RE::NiPoint3 handCentroid(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < handPtCount; ++i) {
            handCentroid = handCentroid + handPts[i];
        }
        handCentroid = handCentroid * (1.0f / static_cast<float>(handPtCount));

        // Broadphase query encloses the whole point cloud plus a margin.
        float spread = 0.0f;
        for (int i = 0; i < handPtCount; ++i) {
            spread = (std::max)(spread, Length(handPts[i] - handCentroid));
        }
        std::vector<RE::TESObjectREFR*> nearby =
            Physics::GetObjectsInRadius(handCentroid, spread + handRadius + 80.0f, player);

        RE::TESObjectREFR* bestRefr = nullptr;
        RE::NiPoint3 bestCenter;
        RE::NiPoint3 bestHandPt;
        float bestScore = (std::numeric_limits<float>::max)();

        for (auto* refr : nearby) {
            if (!refr || !Physics::IsGrabbable(refr)) {
                continue;
            }
            RE::NiPoint3 center;
            float objectRadius = 0.0f;
            if (!GetObjectBounds(refr, center, objectRadius)) {
                continue;
            }
            objectRadius = (std::clamp)(objectRadius, 2.0f, 80.0f);
            const float triggerDist = handRadius + objectRadius;

            // Nearest hand point to this object's bounds center — ANY part of the hand counts.
            RE::NiPoint3 closestHandPt = handPts[0];
            float distSq = (std::numeric_limits<float>::max)();
            for (int i = 0; i < handPtCount; ++i) {
                const float d = LengthSq(center - handPts[i]);
                if (d < distSq) { distSq = d; closestHandPt = handPts[i]; }
            }
            if (distSq > triggerDist * triggerDist) {
                continue;
            }
            if (distSq < bestScore) {
                bestScore = distSq;
                bestRefr = refr;
                bestCenter = center;
                bestHandPt = closestHandPt;
            }
        }

        if (!bestRefr) {
            ClearContactObject(isLeft);
            return;
        }

        // TRUE TOUCH: require a hand sample (a real finger bone) within handContactSlop of the
        // object's actual OUTER MESH surface. Test every hand point and keep the nearest, so any
        // finger / edge / palm contact registers. We push from the MESH point — never the
        // node/bounds centre — so contact MUST be verified against real triangles; if the mesh
        // can't be extracted, we do not push at all (the hand only shoves the visible surface).
        RE::NiPoint3 touchHandPt = bestHandPt;
        RE::NiPoint3 contactPt(0.0f, 0.0f, 0.0f);
        bool haveContact = false;
        {
            auto* bestNode = bestRefr->Get3D();
            if (bestNode) {
                std::vector<heisenberg::TriangleData> tris;
                tris.reserve(256);
                heisenberg::GetTriangles(bestNode, tris);
                if (!tris.empty()) {
                    float bestMeshDist = (std::numeric_limits<float>::max)();
                    RE::NiPoint3 meshPt;
                    float md = -1.0f;
                    for (int i = 0; i < handPtCount; ++i) {
                        if (heisenberg::GetClosestMeshPointToPoint(tris, handPts[i], meshPt, md) && md < bestMeshDist) {
                            bestMeshDist = md;
                            touchHandPt = handPts[i];
                            contactPt = meshPt;     // WHERE on the outer mesh we touched
                            haveContact = true;
                        }
                    }
                    // Within handContactSlop of the real surface = genuine touch. Beyond it the
                    // hand hasn't reached the mesh yet (was the "pushes before contact" cause).
                    if (bestMeshDist > g_config.handContactSlop) {
                        haveContact = false;
                    }
                }
            }
        }

        if (!haveContact) {
            // No verified OUTER-MESH contact (too far, or mesh not extractable). Do NOT fall back
            // to pushing from the node / bounds centre — that's the "pushes nodes not the mesh"
            // complaint. No contact → no push.
            ClearContactObject(isLeft);
            return;
        }

        SetContactObject(isLeft, bestRefr);

        // OVERALL-APPROACH GATE: only push while the HAND AS A WHOLE is closing on the object.
        // The per-point normal test below isn't enough on its own — on the RETURN stroke the
        // closest hand sample can flip to one whose local surface normal happens to align with
        // the backward motion, so intoContact goes positive again and the object gets shoved
        // toward the player. That is the "empty hand pushes the object, then it follows my hand
        // back" report. Gating on the wand→object-centre approach makes a retract impart nothing,
        // no matter which sample is closest.
        // Use the WAND→contact-point vector (handPos is the stable wand centre) rather than the
        // per-sample touchHandPt, which is what flips on the return stroke.
        const RE::NiPoint3 toContactFromWand = contactPt - handPos;
        const float overallApproach = Dot(handVel, NormalizeOrZero(toContactFromWand));

        // DIRECTIONAL NORMAL PUSH: shove the object ALONG the contact normal (hand point → mesh
        // point) by ONLY the hand's speed INTO that surface. Passing the raw handVel made the
        // object inherit the hand's whole velocity vector, so it dragged sideways and followed the
        // hand back out of contact (the "sticky" drag). Projecting handVel onto the contact normal
        // means lateral motion and the RETURN stroke impart nothing — when the hand pulls back,
        // intoContact goes <= 0 and the push stops immediately, so the object stays put.
        const RE::NiPoint3 toContact = contactPt - touchHandPt;
        if (overallApproach > 0.0f && LengthSq(toContact) > 1.0e-4f) {
            const RE::NiPoint3 dir = NormalizeOrZero(toContact);
            const float intoContact = Dot(handVel, dir);
            if (intoContact >= 1.0f) {
                const RE::NiPoint3 pushVel = dir * intoContact;  // normal-only, no lateral/return drag
                ApplyPushForce(bestRefr, contactPt, pushVel, deltaTime);
            }
        }

        spdlog::trace("[HAND_COLLISION] Full-hand proximity {} hand -> {:08X} speed={:.1f} bestDist={:.1f}",
                      isLeft ? "left" : "right", bestRefr->formID, handSpeed, std::sqrt(bestScore));
    }

    // SEH leaf (NO C++ objects) — set the body STATIC then remove it from the world.
    // Used only by the legacy (non-Bethesda) release path.
    static void SafeDestroyBodyLive(void* hknpWorld, void* collisionObject, std::uint32_t bodyId)
    {
        __try {
            if (collisionObject) {
                // bhkNPCollisionObject::SetMotionType(0) = STATIC — prevents the engine
                // from running computeHardKeyFrame on the body during teardown.
                ConstraintFunctions::BhkNPCollisionObjectSetMotionType(collisionObject, 0);
            }
            auto destroyBodies = reinterpret_cast<void(__fastcall*)(void*, const std::uint32_t*, int, int)>(
                REL::Module::get().base() + 0x1544e80);
            destroyBodies(hknpWorld, &bodyId, 1, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // World already torn down — nothing to remove.
        }
    }

    // Fully release a cup / finger-segment PhysicsHandBody. The struct comment requires
    // Destroy()+delete of the BethesdaPhysicsBody trio; the previous live teardown paths
    // only called Invalidate() (which just nulls the pointers), leaking the engine-heap
    // bodies AND leaving them registered in the hknp world — and DestroyFingerSegments
    // even _aligned_free'd the engine-owned physicsSystem/collisionObject (heap corruption).
    // This is the single correct release for all live teardown sites. Safe on invalid input.
    static void ReleaseCupBody(PhysicsHandBody& hb)
    {
        // Bethesda pipeline (default/live): each BPB owns its engine-heap allocations and
        // Destroy() does RemovePhysicsSystemInstance + refcount-release (SEH-guarded inside).
        // physicsSystem/collisionObject belong to the BPB — never _aligned_free them here.
        if (hb.bethesdaBody || hb.bethesdaBody_wallA || hb.bethesdaBody_wallB) {
            if (hb.IsValid()) {
                PlayerCharacterProxyListener::GetSingleton().UnregisterHandBodyId(hb.bodyId);
            }
            auto destroyBb = [&](void*& slot) {
                if (!slot) return;
                auto* bb = reinterpret_cast<heisenberg::bethesda_physics_body::BethesdaPhysicsBody*>(slot);
                bb->Destroy(hb.bhkWorld);
                delete bb;
                slot = nullptr;
            };
            destroyBb(hb.bethesdaBody_wallA);
            destroyBb(hb.bethesdaBody_wallB);
            destroyBb(hb.bethesdaBody);
            hb.Invalidate();
            return;
        }

        // Legacy path (bUseBethesdaPhysicsBody=false): remove the body via the engine, then
        // free the aligned allocations (matches the old DestroyPhysicsHandBody behavior).
        if (hb.IsValid()) {
            PlayerCharacterProxyListener::GetSingleton().UnregisterHandBodyId(hb.bodyId);
            if (hb.hknpWorld) SafeDestroyBodyLive(hb.hknpWorld, hb.collisionObject, hb.bodyId);
        }
        if (hb.alignedCollisionObjMem) _aligned_free(hb.alignedCollisionObjMem);
        if (hb.alignedPhysicsSystemMem) _aligned_free(hb.alignedPhysicsSystemMem);
        if (hb.alignedBodyCinfoMem)    _aligned_free(hb.alignedBodyCinfoMem);
        if (hb.alignedMaterialMem)     _aligned_free(hb.alignedMaterialMem);
        if (hb.alignedSystemDataMem)   _aligned_free(hb.alignedSystemDataMem);
        hb.Invalidate();
    }

    bool HandCollision::Initialize()
    {
        _leftHandBody.Invalidate();
        _rightHandBody.Invalidate();
        _leftContact.reset();
        _rightContact.reset();
        _leftContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
        _rightContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
        _pendingLeftContactClear.store(false, std::memory_order_relaxed);
        _pendingRightContactClear.store(false, std::memory_order_relaxed);
        _hasPrevSweepPos[0] = false;
        _hasPrevSweepPos[1] = false;
        _bodyCreationBlockedUntil = Utils::GetTime() + 0.75;
        _initialized = true;
        spdlog::info("[HAND_COLLISION] Initialized (ROCK-style, layer 43)");
        return true;
    }

    void HandCollision::Shutdown()
    {
        // Destroy bodies from the physics world before teardown. ReleaseCupBody routes
        // through BethesdaPhysicsBody::Destroy (default path) or the legacy aligned-free
        // path, and Destroy()+deletes the full cup trio — the old lambda only removed the
        // palm bodyId and leaked the two wall bodies + all three wrapper objects.
        ReleaseCupBody(_leftHandBody);
        ReleaseCupBody(_rightHandBody);
        DestroyFingerSegments(true);
        DestroyFingerSegments(false);
        ContactImpulseListener::GetSingleton().Unsubscribe();
        _leftContact.reset();
        _rightContact.reset();
        _leftContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
        _rightContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
        _pendingLeftContactClear.store(false, std::memory_order_relaxed);
        _pendingRightContactClear.store(false, std::memory_order_relaxed);
        _hasPrevSweepPos[0] = false;
        _hasPrevSweepPos[1] = false;
        _initialized = false;
        spdlog::info("[HAND_COLLISION] Shutdown");
    }

    // Create a single hand body following ROCK's BethesdaPhysicsBody pattern.
    // halfExtentsGame: box half-extents in game units (x/y/z). The main hand body uses ROCK's
    // palm dimensions by default — a rounded-box convex hull of length≈5 × width≈3.04 ×
    // thickness≈1.28 game units (≈ 7cm × 4.3cm × 1.8cm), matching ROCK's HandBoneColliderSet
    // PalmAnchor frame (radius=1.35, width=radius*2.25, thickness=radius*0.95). X=palm-forward
    // (fingers), Y=thumb↔pinky, Z=palm normal — the hand bone's local axes match this.
    // Finger segments pass their per-segment half-extents explicitly.
    static bool CreateHandBody(PhysicsHandBody& hb, void* hknpWorld, void* bhkWorld,
                               const RE::NiPoint3& pos, bool isLeft,
                               const RE::NiPoint3& halfExtentsGame = RE::NiPoint3(2.5f, 1.52f, 0.64f),
                               const char* debugLabel = nullptr)
    {
        auto base = REL::Module::get().base();
        spdlog::info("[HAND_COLLISION] Creating {} body at ({:.1f},{:.1f},{:.1f}) half=({:.2f},{:.2f},{:.2f})",
                     debugLabel ? debugLabel : (isLeft ? "LEFT" : "RIGHT"),
                     pos.x, pos.y, pos.z,
                     halfExtentsGame.x, halfExtentsGame.y, halfExtentsGame.z);

        // 1. Create shape from caller-supplied half-extents.
        // NOTE: alignas(16) on RE::NiPoint4 locals is not reliably honored by MSVC
        // in this function (stack-frame shape matters — changing args around
        // caused this alignment to break the 16B boundary and crash movaps inside
        // CreateConvexShapeFromHalfExtents). Allocate an aligned buffer instead.
        void* halfExtentsMem = _aligned_malloc(sizeof(RE::NiPoint4), 16);
        if (!halfExtentsMem) return false;
        auto* halfExtentsPtr = new (halfExtentsMem) RE::NiPoint4(
            halfExtentsGame.x * kGameToHavok,
            halfExtentsGame.y * kGameToHavok,
            halfExtentsGame.z * kGameToHavok, 0.0f);
        ConstraintFunctions::hknpConvexShapeBuildConfig buildCfg;
        ConstraintFunctions::BuildConfigCtor(&buildCfg);
        // convexRadius 0.10 matches ROCK's HandBoneColliderSet::roleConvexRadius (non-PA).
        // Convex radius is in GAME units → convert to havok (ROCK does the same). Passing
        // 0.10 raw = 0.10 havok = 10cm skin, which pushed objects ~10cm before mesh contact.
        void* shape = ConstraintFunctions::CreateConvexShapeFromHalfExtents(*halfExtentsPtr, 0.10f * kGameToHavok, &buildCfg);
        _aligned_free(halfExtentsMem);
        if (!shape) { spdlog::error("[HAND_COLLISION] Shape creation failed"); return false; }

        // ──────────────────────────────────────────────────────────────────────────
        // NEW PATH: ROCK-style BethesdaPhysicsBody pipeline (uses engine heap
        // allocator so the destructor's TBB scalable_aligned_free works).
        // Builds a CUP SHAPE from 3 convex bodies: palm-base + thumb-side wall +
        // pinky-side wall. The walls form raised rims so items dropped into the
        // hand from above (or transferred from the other hand) settle and don't
        // roll off. All 3 bodies are keyframed and driven by the same wand
        // transform each frame (MoveHandBody calls UpdateBethesdaCupWalls).
        // ──────────────────────────────────────────────────────────────────────────
        if (heisenberg::g_config.useBethesdaPhysicsBody) {
            using MT = heisenberg::bethesda_physics_body::MotionType;

            // Helper: build a convex hull shape from half-extents (game units).
            // BOTH the NiPoint4 and the hknpConvexShapeBuildConfig must be 16-byte aligned —
            // MSVC doesn't reliably honor alignas(16) on stack locals here (see the legacy
            // CreateHandBody's comment), especially inside a lambda. Allocate both on the
            // heap with _aligned_malloc — matches the proven legacy fix for halfExtentsMem.
            auto buildHull = [&](const RE::NiPoint3& he) -> void* {
                void* heMem = _aligned_malloc(sizeof(RE::NiPoint4), 16);
                if (!heMem) return nullptr;
                auto* hePtr = new (heMem) RE::NiPoint4(
                    he.x * kGameToHavok, he.y * kGameToHavok, he.z * kGameToHavok, 0.0f);
                void* bcMem = _aligned_malloc(sizeof(ConstraintFunctions::hknpConvexShapeBuildConfig), 16);
                if (!bcMem) { _aligned_free(heMem); return nullptr; }
                auto* bcPtr = reinterpret_cast<ConstraintFunctions::hknpConvexShapeBuildConfig*>(bcMem);
                ConstraintFunctions::BuildConfigCtor(bcPtr);
                void* shp = ConstraintFunctions::CreateConvexShapeFromHalfExtents(*hePtr, 0.10f * kGameToHavok, bcPtr);
                _aligned_free(bcMem);
                _aligned_free(heMem);
                return shp;
            };

            // 1. Palm base — bigger, flatter (the "catch basin" bottom).
            void* palmShape = buildHull(RE::NiPoint3(4.0f, 2.7f, 0.6f));
            // 2. Wall shapes — thin, tall slabs (the cup rims).
            void* wallShape = buildHull(RE::NiPoint3(3.5f, 0.3f, 1.5f));
            if (!palmShape || !wallShape) {
                spdlog::error("[HAND_COLLISION] Failed to build cup hull shapes");
                return false;
            }

            auto createOne = [&](void* shp, const char* name) -> heisenberg::bethesda_physics_body::BethesdaPhysicsBody* {
                auto* bb = new heisenberg::bethesda_physics_body::BethesdaPhysicsBody();
                if (!bb->Create(bhkWorld, hknpWorld, shp, kHandLayer, MT::Keyframed, name)) {
                    delete bb;
                    return nullptr;
                }
                return bb;
            };
            const char* nm = isLeft ? "Heisenberg_HandL" : "Heisenberg_HandR";
            const char* nmA = isLeft ? "Heisenberg_HandL_wA" : "Heisenberg_HandR_wA";
            const char* nmB = isLeft ? "Heisenberg_HandL_wB" : "Heisenberg_HandR_wB";

            auto* palm  = createOne(palmShape, nm);
            auto* wallA = createOne(wallShape, nmA);
            auto* wallB = createOne(wallShape, nmB);
            if (!palm || !wallA || !wallB) {
                spdlog::error("[HAND_COLLISION] Cup body creation failed (palm={} wA={} wB={})",
                              (void*)palm, (void*)wallA, (void*)wallB);
                if (palm)  { palm->Destroy(bhkWorld);  delete palm; }
                if (wallA) { wallA->Destroy(bhkWorld); delete wallA; }
                if (wallB) { wallB->Destroy(bhkWorld); delete wallB; }
                return false;
            }

            // Populate PhysicsHandBody from the palm (main body). Walls stored separately.
            hb.bodyId           = palm->GetBodyId();
            hb.shape            = palmShape;
            hb.hknpWorld        = hknpWorld;
            hb.bhkWorld         = bhkWorld;
            hb.physicsSystem    = palm->GetPhysicsSystem();
            hb.collisionObject  = palm->GetCollisionObject();
            hb.alignedSystemDataMem = nullptr;
            hb.alignedBodyCinfoMem  = nullptr;
            hb.alignedMaterialMem   = nullptr;
            hb.bethesdaBody       = palm;
            hb.bethesdaBody_wallA = wallA;
            hb.bethesdaBody_wallB = wallB;
            hb.valid              = true;
            hb.collisionEnabled   = true;
            hb.createdTime        = Utils::GetTime();

            // Initial transforms — place at pos, identity rotation. MoveHandBody updates next frame.
            struct alignas(16) HkXform64 { float m[16]; };
            static HkXform64 s_initXform = {
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1
            };
            s_initXform.m[12] = pos.x * kGameToHavok;
            s_initXform.m[13] = pos.y * kGameToHavok;
            s_initXform.m[14] = pos.z * kGameToHavok;
            palm->SetTransform(&s_initXform);
            // Walls — same identity rotation; positions offset slightly (full update in MoveHandBody).
            s_initXform.m[13] = (pos.y + 3.0f) * kGameToHavok; s_initXform.m[14] = (pos.z + 1.2f) * kGameToHavok;
            wallA->SetTransform(&s_initXform);
            s_initXform.m[13] = (pos.y - 3.0f) * kGameToHavok;
            wallB->SetTransform(&s_initXform);
            s_initXform.m[13] = pos.y * kGameToHavok; s_initXform.m[14] = pos.z * kGameToHavok;  // reset

            // Set the body+0x88 back-pointer + register layer for ALL three.
            void* bodyBuf = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x20);
            if (bodyBuf) {
                auto setBack = [&](std::uint32_t bid, void* co) {
                    uintptr_t entry = reinterpret_cast<uintptr_t>(bodyBuf) + (bid & 0xFFFF) * 0x90;
                    *reinterpret_cast<void**>(entry + 0x88) = co;
                    *reinterpret_cast<std::uint16_t*>(entry + 0x70) = 0;
                };
                setBack(palm->GetBodyId(),  palm->GetCollisionObject());
                setBack(wallA->GetBodyId(), wallA->GetCollisionObject());
                setBack(wallB->GetBodyId(), wallB->GetCollisionObject());
            }
            RegisterHandLayer(hknpWorld);
            ConstraintFunctions::hknpWorld_commitAddBodies(hknpWorld);
            auto activateBody = reinterpret_cast<void(__fastcall*)(void*, std::uint32_t)>(
                REL::Module::get().base() + 0x1546ef0);
            activateBody(hknpWorld, palm->GetBodyId());
            activateBody(hknpWorld, wallA->GetBodyId());
            activateBody(hknpWorld, wallB->GetBodyId());

            spdlog::info("[HAND_COLLISION] BethesdaPhysicsBody CUP SUCCESS: {} palm=0x{:04X} wA=0x{:04X} wB=0x{:04X}",
                         isLeft ? "LEFT" : "RIGHT",
                         palm->GetBodyId(), wallA->GetBodyId(), wallB->GetBodyId());
            return true;
        }

        // ──────────────────────────────────────────────────────────────────────────
        // LEGACY PATH (kept for fallback — crashes on cell change due to TBB
        // scalable_aligned_free mismatch; only used when bUseBethesdaPhysicsBody=false).
        // ──────────────────────────────────────────────────────────────────────────

        // 2. Body cinfo
        void* cinfoMem = _aligned_malloc(sizeof(hknpBodyCinfo), 16);
        if (!cinfoMem) return false;
        auto* cinfo = reinterpret_cast<hknpBodyCinfo*>(cinfoMem);
        ConstraintFunctions::BodyCinfoCtor(cinfo);
        cinfo->shape = shape;
        cinfo->position = RE::NiPoint4(pos.x * kGameToHavok, pos.y * kGameToHavok, pos.z * kGameToHavok, 0.0f);
        cinfo->orientation = RE::NiPoint4(0, 0, 0, 1);
        cinfo->qualityId = 0;  // STATIC — prevents engine from calling computeHardKeyFrame
        // We handle positioning via direct motion buffer writes, not engine keyframe processing.
        cinfo->materialId = 0;
        // ROCK uses just the layer (&= 0xFFFFFF80; |= layer) — upper bits left zero.
        // Adding a group/subsystem into the upper bits makes F4VR's filter reject
        // pairs we actually want to collide with.
        cinfo->collisionFilterInfo = kHandLayer;

        // 3. Physics system data (material + body cinfo)
        void* sysData = _aligned_malloc(0x90, 16);
        if (!sysData) { _aligned_free(cinfoMem); return false; }
        std::memset(sysData, 0, 0x90);
        ConstraintFunctions::PhysicsSystemDataCtor(sysData);

        // Copy world default material
        void* matMem = _aligned_malloc(0x60, 16);
        if (!matMem) { _aligned_free(cinfoMem); _aligned_free(sysData); return false; }
        std::memset(matMem, 0, 0x60);
        void* matLib = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x5c8);
        if (matLib) {
            void* matEntries = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(matLib) + 0x28);
            if (matEntries) std::memcpy(matMem, matEntries, 0x50);
        }

        auto* sd = reinterpret_cast<std::uint8_t*>(sysData);
        *reinterpret_cast<void**>(sd + 0x10) = matMem;
        *reinterpret_cast<std::uint32_t*>(sd + 0x18) = 1;
        *reinterpret_cast<std::uint32_t*>(sd + 0x1C) = 0x80000001;
        *reinterpret_cast<void**>(sd + 0x40) = cinfoMem;
        *reinterpret_cast<std::uint32_t*>(sd + 0x48) = 1;
        *reinterpret_cast<std::uint32_t*>(sd + 0x4C) = 0x80000001;

        // 4. Create Bethesda wrappers
        void* physSys = _aligned_malloc(0x30, 16);
        if (!physSys) { _aligned_free(matMem); _aligned_free(cinfoMem); _aligned_free(sysData); return false; }
        std::memset(physSys, 0, 0x30);
        ConstraintFunctions::BhkPhysicsSystemCtor(physSys, sysData);

        alignas(16) std::uint8_t identityXform[0x40] = {};
        reinterpret_cast<float*>(identityXform)[0] = 1.0f;
        reinterpret_cast<float*>(identityXform)[5] = 1.0f;
        reinterpret_cast<float*>(identityXform)[10] = 1.0f;
        reinterpret_cast<float*>(identityXform)[15] = 1.0f;
        ConstraintFunctions::BhkPhysicsSystemCreateInstance(physSys, bhkWorld, identityXform);

        // 5. AddToWorld (broadphase registration while body is STATIC)
        auto addToWorld = reinterpret_cast<void(__fastcall*)(void*)>(base + 0x1e0c580);
        addToWorld(physSys);

        // 6. Get body ID
        std::uint32_t bodyId = 0x7FFFFFFF;
        ConstraintFunctions::BhkPhysicsSystemGetBodyId(physSys, &bodyId, 0);
        if (bodyId == 0x7FFFFFFF) {
            spdlog::error("[HAND_COLLISION] Failed to get body ID");
            return false;
        }

        // 7. Create collision object wrapper
        void* collObj = _aligned_malloc(0x30, 16);
        if (!collObj) return false;
        std::memset(collObj, 0, 0x30);
        ConstraintFunctions::BhkNPCollisionObjectCtor(collObj, 0, physSys);

        // 8. SetMotionType(KEYFRAMED) via collision object
        ConstraintFunctions::BhkNPCollisionObjectSetMotionType(collObj, 2);

        // 9. Set body+0x88 back-pointer (CRITICAL — 33 engine systems read this)
        void* bodyBuf = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x20);
        if (bodyBuf) {
            uintptr_t entry = reinterpret_cast<uintptr_t>(bodyBuf) + (bodyId & 0xFFFF) * 0x90;
            *reinterpret_cast<void**>(entry + 0x88) = collObj;

            // Overwrite materialId to 0 (world default)
            *reinterpret_cast<std::uint16_t*>(entry + 0x70) = 0;
        }

        // 10. Enable body flags: contact modifier + keep-awake (ROCK: 0x08020000)
        auto enableFlags = reinterpret_cast<void(__fastcall*)(void*, std::uint32_t, std::uint32_t, int)>(base + 0x153c090);
        enableFlags(hknpWorld, bodyId, 0x08020000, 0);

        // 11. Activate
        ConstraintFunctions::hknpWorld_commitAddBodies(hknpWorld);
        auto activateBody = reinterpret_cast<void(__fastcall*)(void*, std::uint32_t)>(base + 0x1546ef0);
        activateBody(hknpWorld, bodyId);

        // 12. Register layer
        RegisterHandLayer(hknpWorld);

        // Store results
        hb.bodyId = bodyId;
        hb.shape = shape;
        hb.hknpWorld = hknpWorld;
        hb.bhkWorld = bhkWorld;
        hb.physicsSystem = physSys;
        hb.collisionObject = collObj;
        hb.alignedSystemDataMem = sysData;
        hb.alignedBodyCinfoMem = cinfoMem;
        hb.alignedMaterialMem = matMem;
        hb.valid = true;
        hb.collisionEnabled = true;
        hb.createdTime = Utils::GetTime();

        spdlog::info("[HAND_COLLISION] SUCCESS: {} hand body 0x{:04X}", isLeft ? "LEFT" : "RIGHT", bodyId);
        return true;
    }

    void HandCollision::CreateBodiesIfNeeded(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos)
    {
        if (!g_config.enableHandCollision || !g_config.usePhysicsHandBodies) return;
        // ROCK HandBoneColliderSet (16 bodies/hand) takes precedence — when it's active,
        // the 3-body cup is redundant and would just fight for filter/contact slots.
        if (heisenberg::rock_hand_collider::IsActive()) {
            // Tear down any existing cup bodies (e.g. toggle flipped at runtime).
            if (_leftHandBody.IsValid() || _rightHandBody.IsValid()) {
                auto& cl = ContactImpulseListener::GetSingleton();
                cl.UnsubscribeBody(true);
                cl.UnsubscribeBody(false);
                ClearContactObject(true);
                ClearContactObject(false);
                ReleaseCupBody(_leftHandBody);
                ReleaseCupBody(_rightHandBody);
                spdlog::info("[HAND_COLLISION] Cup bodies cleared — ROCK HandBoneColliderSet active (16-body finger pipeline owns the hand)");
            }
            return;
        }
        if (!_initialized) Initialize();

        void* bhkWorld = nullptr;
        void* hknpWorld = nullptr;
        if (!GetWorlds(bhkWorld, hknpWorld)) return;

        // Check for world change
        if ((_leftHandBody.IsValid() && _leftHandBody.hknpWorld != hknpWorld) ||
            (_rightHandBody.IsValid() && _rightHandBody.hknpWorld != hknpWorld)) {
            auto& cl = ContactImpulseListener::GetSingleton();
            cl.UnsubscribeBody(true);
            cl.UnsubscribeBody(false);
            ClearContactObject(true);
            ClearContactObject(false);
            ReleaseCupBody(_leftHandBody);
            ReleaseCupBody(_rightHandBody);
        }

        if (_leftHandBody.IsValid() && _rightHandBody.IsValid()) return;

        if (Utils::GetTime() < _bodyCreationBlockedUntil) return;

        std::scoped_lock lock(_handBodyMutex);
        bool created = false;
        if (!_leftHandBody.IsValid()) {
            if (CreateHandBody(_leftHandBody, hknpWorld, bhkWorld, leftHandPos, true)) created = true;
        }
        if (!_rightHandBody.IsValid()) {
            if (CreateHandBody(_rightHandBody, hknpWorld, bhkWorld, rightHandPos, false)) created = true;
        }

        // Subscribe contact listener after body creation
        if (created) {
            auto& cl = ContactImpulseListener::GetSingleton();
            cl.SetWorld(bhkWorld);
            if (_leftHandBody.IsValid()) cl.SubscribeForBody(_leftHandBody.bodyId, true);
            if (_rightHandBody.IsValid()) cl.SubscribeForBody(_rightHandBody.bodyId, false);

            // Mirror world to ThrownObjectTracker so it can subscribe to
            // CONTACT_STARTED on thrown bodies for impact-effect dispatch.
            ThrownObjectTracker::GetSingleton().SetWorld(bhkWorld);

            // Log body state for diagnostics
            void* bodyBuf = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x20);
            if (bodyBuf) {
                auto logBody = [&](const PhysicsHandBody& hb, const char* name) {
                    uintptr_t e = reinterpret_cast<uintptr_t>(bodyBuf) + (hb.bodyId & 0xFFFF) * 0x90;
                    std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(e + 0x40);
                    std::uint32_t filter = *reinterpret_cast<std::uint32_t*>(e + 0x44);
                    std::int32_t motId = *reinterpret_cast<std::int32_t*>(e + 0x68);
                    std::int32_t bpH = *reinterpret_cast<std::int32_t*>(e + 0x6c);
                    void* backPtr = *reinterpret_cast<void**>(e + 0x88);
                    spdlog::info("[HAND_COLLISION] {} DIAG: flags=0x{:08X} filter=0x{:08X} motId={} bpH={} backPtr={:p}",
                                 name, flags, filter, motId, bpH, backPtr);
                };
                if (_leftHandBody.IsValid()) logBody(_leftHandBody, "LEFT");
                if (_rightHandBody.IsValid()) logBody(_rightHandBody, "RIGHT");
            }
        }
    }

    void HandCollision::ApplyPlayerPairFilterIfNeeded()
    {
        if (!_initialized || !g_config.enableHandCollision || !g_config.usePhysicsHandBodies) {
            return;
        }

        std::scoped_lock lock(_handBodyMutex);

        if ((!_leftHandBody.IsValid() || _leftHandBody.playerPairFilterApplied) &&
            (!_rightHandBody.IsValid() || _rightHandBody.playerPairFilterApplied)) {
            return;
        }

        std::uint32_t playerBodyId = Physics::GetPlayerBodyId();
        if (playerBodyId == 0x7FFFFFFF || playerBodyId == 0) {
            return;
        }

        void* hknpWorld = GetCurrentHknpWorld();
        if (!hknpWorld) {
            return;
        }

        // ROCK's hand filter info: `(0x000B << 16) | (layer & 0x7F)` = 0x000B002B
        // High 16 bits 0x000B = the "hand group" / system bits the engine's pair filter
        // uses to short-circuit player-body collision resolution. ROCK uses this universally
        // for both hand bodies — no per-pair filter needed for the player avoidance.
        constexpr std::uint32_t kRockHandFilterInfo = (0x000Bu << 16) | (kHandLayer & 0x7Fu);

        auto applyAll = [&](PhysicsHandBody& hb, const char* side) {
            if (!hb.IsValid() || hb.playerPairFilterApplied) return;
            // (1) Belt-and-suspenders pair filter against the proxy.
            Physics::DisableCollisionBetween(hknpWorld, hb.bodyId, playerBodyId);
            auto wallBodyId = [](void* bb) -> std::uint32_t {
                if (!bb) return 0x7FFFFFFFu;
                return reinterpret_cast<heisenberg::bethesda_physics_body::BethesdaPhysicsBody*>(bb)->GetBodyId();
            };
            const std::uint32_t wA = wallBodyId(hb.bethesdaBody_wallA);
            const std::uint32_t wB = wallBodyId(hb.bethesdaBody_wallB);
            if (wA != 0x7FFFFFFFu) Physics::DisableCollisionBetween(hknpWorld, wA, playerBodyId);
            if (wB != 0x7FFFFFFFu) Physics::DisableCollisionBetween(hknpWorld, wB, playerBodyId);

            // (2) Stamp the ROCK hand filter info onto every cup body — this is the
            // actual player-push fix. The engine's pair filter recognizes the 0x000B
            // group bits as "hand group" and rejects pairs against player-attached bodies.
            Physics::TryWriteBodyFilterInfo(hknpWorld, hb.bodyId, kRockHandFilterInfo);
            if (wA != 0x7FFFFFFFu) Physics::TryWriteBodyFilterInfo(hknpWorld, wA, kRockHandFilterInfo);
            if (wB != 0x7FFFFFFFu) Physics::TryWriteBodyFilterInfo(hknpWorld, wB, kRockHandFilterInfo);

            hb.playerPairFilterApplied = true;
            spdlog::info("[HAND_COLLISION] {} cup filter set to 0x{:08X}: palm=0x{:04X} wA=0x{:04X} wB=0x{:04X}",
                         side, kRockHandFilterInfo, hb.bodyId, wA, wB);
        };
        applyAll(_leftHandBody,  "LEFT");
        applyAll(_rightHandBody, "RIGHT");
    }

    // Move a hand body using ROCK's approach:
    // computeHardKeyFrame → bhkNPCollisionObject::SetTransform + SetVelocity (deferred-safe)
    static void MoveHandBody(PhysicsHandBody& hb, const RE::NiPoint3& pos,
                             const RE::NiMatrix3& rot, float invDeltaTime,
                             bool isLeft, bool unsubscribeOnFault)
    {
        if (!hb.IsValid() || !hb.hknpWorld || !hb.collisionObject) return;

        // Verify world is still valid
        void* bw = nullptr; void* hw = nullptr;
        if (!GetWorlds(bw, hw) || hw != hb.hknpWorld) return;

        // Skip first few frames after creation to let body settle
        double elapsed = Utils::GetTime() - hb.createdTime;
        if (elapsed < 0.5) return;

        static int moveCount = 0;
        if (++moveCount <= 3 || moveCount % 500 == 0) {
            spdlog::info("[HAND_COLLISION] MoveHandBody #{}: pos=({:.1f},{:.1f},{:.1f}) body=0x{:04X}",
                         moveCount, pos.x, pos.y, pos.z, hb.bodyId);
        }

        auto base = REL::Module::get().base();

        // MSVC does NOT reliably honor alignas(16) on stack locals in this
        // function (observed at runtime: RCX/R15 = ...xxx8, not 16-aligned,
        // causing movaps #GP in computeHardKeyFrame/SetVelocity). Use a
        // static-lifetime aligned scratch block — static storage respects
        // alignas. Single-threaded access (main update thread only).
        struct alignas(16) MoveScratch {
            float hkPos[4];      // offset  0
            float hkOrient[4];   // offset 16
            float linVel[4];     // offset 32
            float angVel[4];     // offset 48
            float lv[4];         // offset 64
            float av[4];         // offset 80
        };
        static MoveScratch s_scratch;

        s_scratch.hkPos[0] = pos.x * kGameToHavok;
        s_scratch.hkPos[1] = pos.y * kGameToHavok;
        s_scratch.hkPos[2] = pos.z * kGameToHavok;
        s_scratch.hkPos[3] = 0.0f;

        // Orientation vector retained for scratch layout compatibility; the
        // active rotation is written into the hkTransformf below.
        s_scratch.hkOrient[0] = 0.0f;
        s_scratch.hkOrient[1] = 0.0f;
        s_scratch.hkOrient[2] = 0.0f;
        s_scratch.hkOrient[3] = 1.0f;

        auto setXform = reinterpret_cast<void(__fastcall*)(void*, const float*)>(base + 0x1E08A70);
        auto setVel = reinterpret_cast<void(__fastcall*)(void*, const float*, const float*)>(base + 0x1E082A0);
        auto activateBody = reinterpret_cast<void(__fastcall*)(void*, std::uint32_t)>(base + 0x1546ef0);

        // bhkNPCollisionObject::SetTransform expects a 16-byte-aligned 64-byte
        // hkTransformf (3 hkVector4f rotation columns + a hkVector4f translation),
        // NOT an RE::NiTransform (52 bytes, packed 3x3 + NiPoint3 + scale). The
        // engine reads all 64 bytes and hknpWorld::setBodyTransform does movaps
        // on them, so passing a stack NiTransform over-read 12 bytes and #GP-
        // faulted on the 8-aligned stack address — every frame. Each fault
        // Invalidate()'d the body, so it was destroyed and recreated ~2x/sec and
        // never tracked the hand. Build a proper aligned hkTransformf in static
        // storage (single-threaded update path, same rationale as s_scratch).
        // Layout matches the identityXform used at body creation: column-major
        // 4x4 with translation at float[12..14]. NiMatrix3 is used as the
        // rotation source so non-cubic finger-segment boxes track the bones.
        static alignas(16) float s_hkXform[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        s_hkXform[0] = rot.entry[0][0];
        s_hkXform[1] = rot.entry[1][0];
        s_hkXform[2] = rot.entry[2][0];
        s_hkXform[3] = 0.0f;
        s_hkXform[4] = rot.entry[0][1];
        s_hkXform[5] = rot.entry[1][1];
        s_hkXform[6] = rot.entry[2][1];
        s_hkXform[7] = 0.0f;
        s_hkXform[8] = rot.entry[0][2];
        s_hkXform[9] = rot.entry[1][2];
        s_hkXform[10] = rot.entry[2][2];
        s_hkXform[11] = 0.0f;
        s_hkXform[12] = s_scratch.hkPos[0];
        s_hkXform[13] = s_scratch.hkPos[1];
        s_hkXform[14] = s_scratch.hkPos[2];
        s_hkXform[15] = 1.0f;

        // Manual velocity: previous-to-current delta over the frame time. This
        // replaces engine computeHardKeyFrame, which was SEH-faulting every few
        // seconds — each fault destroyed the hand body, so the user perceived
        // the box as frozen most of the time. Manual compute is safe because we
        // own prevHavokPos in PhysicsHandBody and it's always valid by the time
        // we reach this path.
        constexpr float maxLinVel = 50.0f;
        if (hb.prevHavokPos.w == 0.0f && hb.prevHavokPos.x == 0.0f &&
            hb.prevHavokPos.y == 0.0f && hb.prevHavokPos.z == 0.0f) {
            // First frame — no prev; use zero velocity.
            s_scratch.linVel[0] = s_scratch.linVel[1] = s_scratch.linVel[2] = 0.0f;
        } else {
            s_scratch.linVel[0] = (s_scratch.hkPos[0] - hb.prevHavokPos.x) * invDeltaTime;
            s_scratch.linVel[1] = (s_scratch.hkPos[1] - hb.prevHavokPos.y) * invDeltaTime;
            s_scratch.linVel[2] = (s_scratch.hkPos[2] - hb.prevHavokPos.z) * invDeltaTime;
            for (int i = 0; i < 3; i++) {
                if (s_scratch.linVel[i] >  maxLinVel) s_scratch.linVel[i] =  maxLinVel;
                if (s_scratch.linVel[i] < -maxLinVel) s_scratch.linVel[i] = -maxLinVel;
            }
        }
        s_scratch.linVel[3] = 0.0f;
        s_scratch.angVel[0] = s_scratch.angVel[1] = s_scratch.angVel[2] = s_scratch.angVel[3] = 0.0f;
        // Cache for next frame
        hb.prevHavokPos.x = s_scratch.hkPos[0];
        hb.prevHavokPos.y = s_scratch.hkPos[1];
        hb.prevHavokPos.z = s_scratch.hkPos[2];
        hb.prevHavokPos.w = 1.0f;  // marker that we've cached at least once

        // Pinpoint which engine call faults — three separate SEH wraps so the
        // log tells us setXform vs setVel vs activateBody.
        int failedStage = 0;
        __try {
            failedStage = 1;
            setXform(hb.collisionObject, s_hkXform);

            s_scratch.lv[0] = s_scratch.linVel[0];
            s_scratch.lv[1] = s_scratch.linVel[1];
            s_scratch.lv[2] = s_scratch.linVel[2];
            s_scratch.lv[3] = 0.0f;
            s_scratch.av[0] = s_scratch.angVel[0];
            s_scratch.av[1] = s_scratch.angVel[1];
            s_scratch.av[2] = s_scratch.angVel[2];
            s_scratch.av[3] = 0.0f;
            failedStage = 2;
            setVel(hb.collisionObject, s_scratch.lv, s_scratch.av);

            failedStage = 3;
            activateBody(hb.hknpWorld, hb.bodyId);

            // CUP-SHAPE walls — drive each at hand_local offset, same rotation as palm.
            // Transform local offset to world: worldOff[i] = sum_j R[i][j] * localOff[j].
            // Wall A (thumb side): local (0, +3.0, +1.2). Wall B (pinky side): (0, -3.0, +1.2).
            if (hb.bethesdaBody_wallA || hb.bethesdaBody_wallB) {
                static alignas(16) float s_hkXformWall[16];
                std::memcpy(s_hkXformWall, s_hkXform, sizeof(s_hkXformWall));  // copy rotation

                auto applyWall = [&](void* bbPtr, float ly, float lz) {
                    if (!bbPtr) return;
                    // worldOffset = rot * (0, ly, lz)
                    const float wox = rot.entry[0][1]*ly + rot.entry[0][2]*lz;
                    const float woy = rot.entry[1][1]*ly + rot.entry[1][2]*lz;
                    const float woz = rot.entry[2][1]*ly + rot.entry[2][2]*lz;
                    s_hkXformWall[12] = s_scratch.hkPos[0] + wox * kGameToHavok;
                    s_hkXformWall[13] = s_scratch.hkPos[1] + woy * kGameToHavok;
                    s_hkXformWall[14] = s_scratch.hkPos[2] + woz * kGameToHavok;
                    auto* bb = reinterpret_cast<heisenberg::bethesda_physics_body::BethesdaPhysicsBody*>(bbPtr);
                    void* wallCo = bb->GetCollisionObject();
                    if (wallCo) {
                        setXform(wallCo, s_hkXformWall);
                        activateBody(hb.hknpWorld, bb->GetBodyId());
                    }
                };
                applyWall(hb.bethesdaBody_wallA, +3.0f, +1.2f);
                applyWall(hb.bethesdaBody_wallB, -3.0f, +1.2f);
            }

            failedStage = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // failedStage holds the LAST stage we tried before fault
        }

        if (failedStage != 0) {
            static int faultCount = 0;
            if (++faultCount <= 10 || faultCount % 500 == 0) {
                const char* stageName = failedStage == 1 ? "setXform"
                                       : failedStage == 2 ? "setVel"
                                       : "activateBody";
                spdlog::warn("[HAND_COLLISION] MoveHandBody: SEH fault in {} for body 0x{:04X} — invalidating (count={})",
                             stageName, hb.bodyId, faultCount);
            }
            if (unsubscribeOnFault) {
                ContactImpulseListener::GetSingleton().UnsubscribeBody(isLeft);
                HandCollision::GetSingleton().ClearContactObject(isLeft);
            }
            hb.Invalidate();
        }
    }

    // =====================================================================
    // Task #11: Per-finger-segment KEYFRAMED colliders
    // =====================================================================
    // F4VR distal finger bone names (thumb..pinky). Same skeleton names as
    // FingerCurves.cpp uses; duplicated here to keep HandCollision self-contained.
    static const char* const kDistalFingerBones[2][5] = {
        // Right hand
        { "RArm_Finger13", "RArm_Finger23", "RArm_Finger33", "RArm_Finger43", "RArm_Finger53" },
        // Left hand
        { "LArm_Finger13", "LArm_Finger23", "LArm_Finger33", "LArm_Finger43", "LArm_Finger53" },
    };

    static RE::NiAVObject* GetPlayerSkeletonRoot()
    {
        auto* player = f4vr::getPlayer();
        if (!player || !player->firstPersonSkeleton) return nullptr;
        return player->firstPersonSkeleton;
    }

    void HandCollision::DestroyFingerSegments(bool isLeft)
    {
        auto& segments = isLeft ? _leftFingerSegments : _rightFingerSegments;
        // Finger segments are built via the same CreateHandBody (BethesdaPhysicsBody trio),
        // so they must be released the same way. The old loop _aligned_free'd the engine-
        // owned physicsSystem/collisionObject (heap corruption in the Bethesda path) and
        // never deleted the BPB objects. ReleaseCupBody handles both paths correctly.
        for (auto& hb : segments) {
            ReleaseCupBody(hb);
        }
        _fingerSegmentsReady[isLeft ? 0 : 1] = false;
    }

    void HandCollision::UpdateFingerSegments(bool isLeft, float deltaTime)
    {
        if (!g_config.enableFingerSegmentColliders) {
            if (_fingerSegmentsReady[isLeft ? 0 : 1]) {
                DestroyFingerSegments(isLeft);
            }
            return;
        }

        void* bhkWorld = nullptr;
        void* hknpWorld = nullptr;
        if (!GetWorlds(bhkWorld, hknpWorld)) return;

        // If segments were created in a different world (cell change), tear them
        // down so the next frame recreates them in the current world.
        auto& segments = isLeft ? _leftFingerSegments : _rightFingerSegments;
        bool worldChanged = false;
        for (auto& hb : segments) {
            if (hb.IsValid() && hb.hknpWorld != hknpWorld) { worldChanged = true; break; }
        }
        if (worldChanged) {
            DestroyFingerSegments(isLeft);
        }

        RE::NiAVObject* root = GetPlayerSkeletonRoot();
        if (!root) return;

        const char* const* boneNames = kDistalFingerBones[isLeft ? 1 : 0];

        // Resolve all 5 distal bones this frame. If any are missing, skip
        // creation/update for that finger rather than blocking the whole hand.
        RE::NiAVObject* bones[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        int resolved = 0;
        for (int i = 0; i < 5; ++i) {
            bones[i] = f4vr::findAVObject(root, boneNames[i]);
            if (bones[i]) ++resolved;
        }
        if (resolved == 0) return;

        const RE::NiPoint3 halfExt(
            (std::max)(0.05f, g_config.fingerSegmentHalfExtentX),
            (std::max)(0.05f, g_config.fingerSegmentHalfExtentY),
            (std::max)(0.05f, g_config.fingerSegmentHalfExtentZ));
        const float invDt = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 90.0f;

        for (int i = 0; i < 5; ++i) {
            if (!bones[i]) continue;
            const RE::NiPoint3& pos = bones[i]->world.translate;
            const RE::NiMatrix3& rot = bones[i]->world.rotate;
            PhysicsHandBody& hb = segments[i];

            if (!hb.IsValid()) {
                char label[32];
                std::snprintf(label, sizeof(label), "%s-FINGER%d",
                              isLeft ? "LEFT" : "RIGHT", i);
                if (!CreateHandBody(hb, hknpWorld, bhkWorld, pos, isLeft, halfExt, label)) {
                    continue;
                }
            }
            if (hb.IsValid() && hb.hknpWorld == hknpWorld) {
                MoveHandBody(hb, pos, rot, invDt, isLeft, false);
            }
        }

        _fingerSegmentsReady[isLeft ? 0 : 1] = true;
    }

    void HandCollision::Update(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos,
                               const RE::NiPoint3& leftHandVel, const RE::NiPoint3& rightHandVel,
                               const RE::NiMatrix3& leftHandRot, const RE::NiMatrix3& rightHandRot,
                               float deltaTime)
    {
        if (!g_config.enableHandCollision) return;

        _leftHandPos = leftHandPos;
        _rightHandPos = rightHandPos;
        _leftHandVel = leftHandVel;
        _rightHandVel = rightHandVel;
        _leftHandRot = leftHandRot;
        _rightHandRot = rightHandRot;

        // Re-register layer 43 (cell changes reset filter matrix)
        void* bhkW = nullptr;
        void* hknpW = nullptr;
        if (GetWorlds(bhkW, hknpW)) {
            RegisterHandLayer(hknpW);
        }

        float invDt = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 90.0f;

        if (_leftHandBody.IsValid() && _leftHandBody.hknpWorld == hknpW) {
            MoveHandBody(_leftHandBody, leftHandPos, leftHandRot, invDt, true, true);
        }
        if (_rightHandBody.IsValid() && _rightHandBody.hknpWorld == hknpW) {
            MoveHandBody(_rightHandBody, rightHandPos, rightHandRot, invDt, false, true);
        }

        if (g_config.usePhysicsHandBodies) {
            UpdateFingerSegments(true, deltaTime);
            UpdateFingerSegments(false, deltaTime);
        } else {
            CheckProximityCollisions(leftHandPos, leftHandVel, true, deltaTime);
            CheckProximityCollisions(rightHandPos, rightHandVel, false, deltaTime);
        }

        // ROCK integration per-frame entry points. Each self-gates via its own IsActive()
        // and is a no-op when its toggle is off, so they run OUTSIDE the usePhysicsHandBodies
        // branch — BodyBoneCollider / WeaponCollision / TwoHandedGrip are independent of the
        // cup/finger setting, and gating them behind it meant enabling one alone silently
        // never ticked (it claimed IsActive() but Update() was unreachable).
        heisenberg::rock_hand_collider::Update();
        heisenberg::rock_body_collider::Update();
        heisenberg::rock_weapon_collision::Update();
        heisenberg::rock_two_handed_grip::Update();

        // Wake sleeping objects near hands
        if (hknpW) {
            auto activateInAabb = reinterpret_cast<void(__fastcall*)(void*, void*)>(REL::Module::get().base() + 0x1546f80);
            float r = 15.0f * kGameToHavok;  // 15 game units wake radius
            for (const auto& p : {leftHandPos, rightHandPos}) {
                float hx = p.x * kGameToHavok, hy = p.y * kGameToHavok, hz = p.z * kGameToHavok;
                alignas(16) float aabb[8] = {hx-r, hy-r, hz-r, 0, hx+r, hy+r, hz+r, 0};
                activateInAabb(hknpW, aabb);
            }
        }
    }

    bool HandCollision::IsInContact(bool isLeft) const
    {
        const auto& c = isLeft ? _leftContact : _rightContact;
        return c.operator bool();
    }

    RE::TESObjectREFR* HandCollision::GetContactObject(bool isLeft) const
    {
        const auto& c = isLeft ? _leftContact : _rightContact;
        if (c) { RE::NiPointer<RE::TESObjectREFR> ptr = c.get(); return ptr.get(); }
        return nullptr;
    }

    const PhysicsHandBody& HandCollision::GetHandBody(bool isLeft) const
    {
        return isLeft ? _leftHandBody : _rightHandBody;
    }

    void HandCollision::TriggerCollisionHaptics(bool isLeft, float intensity, float duration)
    {
        (void)duration;
        int str = static_cast<int>(intensity * 500.0f);
        if (str < 500) str = 500;
        if (str > 50000) str = 50000;
        if (isLeft) _pendingLeftHaptic = str; else _pendingRightHaptic = str;
    }

    void HandCollision::FlushPendingHaptics()
    {
        if (int leftHaptic = _pendingLeftHaptic.exchange(0, std::memory_order_relaxed); leftHaptic > 0) {
            g_vrInput.TriggerHaptic(true, static_cast<unsigned short>(leftHaptic));
        }
        if (int rightHaptic = _pendingRightHaptic.exchange(0, std::memory_order_relaxed); rightHaptic > 0) {
            g_vrInput.TriggerHaptic(false, static_cast<unsigned short>(rightHaptic));
        }

        if (_pendingLeftContactClear.exchange(false, std::memory_order_relaxed)) {
            _leftContact.reset();
        }
        if (_pendingRightContactClear.exchange(false, std::memory_order_relaxed)) {
            _rightContact.reset();
        }

        auto resolveContact = [&](bool isLeft) {
            auto& bodyId = isLeft ? _leftContactBodyId : _rightContactBodyId;
            const std::uint32_t id = bodyId.load(std::memory_order_relaxed);
            if (id == 0x7FFFFFFF || id == 0) {
                return;
            }
            void* bhkWorld = GetCurrentBhkWorld();
            auto* refr = Physics::GetRefrFromBodyId(bhkWorld, id);
            if (!refr) {
                return;
            }
            auto& handle = isLeft ? _leftContact : _rightContact;
            handle = RE::ObjectRefHandle(refr);
        };
        resolveContact(true);
        resolveContact(false);
    }

    void* HandCollision::GetCurrentBhkWorld()
    {
        void* bhk = nullptr; void* hknp = nullptr;
        return GetWorlds(bhk, hknp) ? bhk : nullptr;
    }

    void* HandCollision::GetCurrentHknpWorld()
    {
        void* bhk = nullptr; void* hknp = nullptr;
        return GetWorlds(bhk, hknp) ? hknp : nullptr;
    }
}
