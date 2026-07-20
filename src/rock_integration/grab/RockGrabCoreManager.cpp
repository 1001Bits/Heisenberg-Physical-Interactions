#include "rock_integration/grab/RockGrabCoreManager.h"

#include "rock_integration/grab/RockGrabConstraint.h"
#include "rock_integration/grab/HavokRuntimeShim.h"        // havok_runtime::getBody/getMotion — object mass read
#include "rock_integration/CollisionLayerPolicy.h"        // FO4_LAYER_NONCOLLIDABLE
#include "rock_integration/CollisionSuppressionRegistry.h" // kSuppressionNoCollideBit
#include "Grab.h"                                          // GrabState
#include "GrabConstraint.h"                                // ConstraintFunctions (native REL wrappers)
#include "Physics.h"                                       // GetSharedSphereShape, filter IO, ClearPlayerProxySupportBody
#include "ThrownObjectTracker.h"
#include "Config.h"
#include "Utils.h"                                          // GetPhysicsBodyOffset / ApplyPhysicsBodyOffset (node->body frame)

#include <spdlog/spdlog.h>
#include <windows.h>
#include <cmath>

namespace heisenberg::rock_grab_core
{
    namespace
    {
        constexpr float kHavokWorldScale = 0.0142875f;
        constexpr float kMaxThrowSpeedGame = 840.0f;  // mirrors HeldBody kHeldBodyMaxThrowSpeed

        // Proxy body A filter: NONCOLLIDABLE layer + noCollide bit 14 + the 0x000B hand group.
        // The proxy exists ONLY to give the grab constraint a stable keyframed anchor — it must
        // never generate contacts.
        std::uint32_t noContactFilterInfo()
        {
            return (0x000Bu << 16)
                 | (rock_core::collision_layer_policy::FO4_LAYER_NONCOLLIDABLE & 0x7Fu)
                 | rock_csr::kSuppressionNoCollideBit;
        }

        // Quaternion from F4VR row-vector rotation matrix (same math as HeldBodyGrab's
        // MatrixToQuaternion — that one is file-static there).
        RE::NiPoint4 matrixToQuaternion(const RE::NiMatrix3& r)
        {
            RE::NiPoint4 q;
            const float trace = r.entry[0][0] + r.entry[1][1] + r.entry[2][2];
            if (trace > 0.0f) {
                const float s = 0.5f / std::sqrt(trace + 1.0f);
                q.w = 0.25f / s;
                q.x = (r.entry[2][1] - r.entry[1][2]) * s;
                q.y = (r.entry[0][2] - r.entry[2][0]) * s;
                q.z = (r.entry[1][0] - r.entry[0][1]) * s;
            } else if (r.entry[0][0] > r.entry[1][1] && r.entry[0][0] > r.entry[2][2]) {
                const float s = 2.0f * std::sqrt(1.0f + r.entry[0][0] - r.entry[1][1] - r.entry[2][2]);
                q.w = (r.entry[2][1] - r.entry[1][2]) / s;
                q.x = 0.25f * s;
                q.y = (r.entry[0][1] + r.entry[1][0]) / s;
                q.z = (r.entry[0][2] + r.entry[2][0]) / s;
            } else if (r.entry[1][1] > r.entry[2][2]) {
                const float s = 2.0f * std::sqrt(1.0f + r.entry[1][1] - r.entry[0][0] - r.entry[2][2]);
                q.w = (r.entry[0][2] - r.entry[2][0]) / s;
                q.x = (r.entry[0][1] + r.entry[1][0]) / s;
                q.y = 0.25f * s;
                q.z = (r.entry[1][2] + r.entry[2][1]) / s;
            } else {
                const float s = 2.0f * std::sqrt(1.0f + r.entry[2][2] - r.entry[0][0] - r.entry[1][1]);
                q.w = (r.entry[1][0] - r.entry[0][1]) / s;
                q.x = (r.entry[0][2] + r.entry[2][0]) / s;
                q.y = (r.entry[1][2] + r.entry[2][1]) / s;
                q.z = 0.25f * s;
            }
            return q;
        }

        // SEH-safe native wrappers (no C++ objects inside __try). HeldBodyGrab's twins are
        // file-static there, so replicate.
        bool safeSetBodyVelocity(void* hknpWorld, std::uint32_t bodyId,
                                 const RE::NiPoint4& linear, const RE::NiPoint4& angular)
        {
            __try {
                heisenberg::ConstraintFunctions::hknpWorld_setBodyVelocity(hknpWorld, bodyId, linear, angular);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool safeApplyHardKeyFrame(void* hknpWorld, std::uint32_t bodyId,
                                   const RE::NiPoint4& position, const RE::NiPoint4& orientation,
                                   float invDeltaTime)
        {
            __try {
                heisenberg::ConstraintFunctions::ApplyHardKeyFrameBodyId(hknpWorld, bodyId, position, orientation, invDeltaTime);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool safeActivateBody(void* hknpWorld, std::uint32_t bodyId)
        {
            __try {
                heisenberg::ConstraintFunctions::hknpWorld_activateBody(hknpWorld, bodyId);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ROCK/HIGGS motor-force-by-mass constants (GrabMotionController.h:73-78 / hand.cpp:3983):
        // cap the motor force at mass*ratio so a light object doesn't get over-driven (=jitter) and
        // a heavy one gets proportional authority (=no trail). Floor keeps tiny/zero-mass sane.
        constexpr float kForceToMassRatio = 500.0f;
        constexpr float kMotorMassFloor = 2.0f;

        // Read the dynamic object's mass (proven standalone readBodyMass, HandGrab.cpp:3776 — which
        // is likewise NOT SEH-wrapped; getBody/getMotion validate via null returns):
        // body(+0x68 motionIndex) -> motion(+0x20 packedInverseInertia int16[4], [3]=invMass packed).
        float readObjectMass(void* hknpWorld, std::uint32_t bodyId)
        {
            if (!hknpWorld || bodyId == 0x7FFFFFFFu) {
                return 0.0f;
            }
            auto* body = rock_core::havok_runtime::getBody(hknpWorld, RE::hknpBodyId{ bodyId });
            if (!body) {
                return 0.0f;
            }
            void* motion = rock_core::havok_runtime::getMotion(hknpWorld, body->motionIndex);
            if (!motion) {
                return 0.0f;
            }
            auto* m = reinterpret_cast<RE::hknpMotion*>(motion);
            const std::uint16_t packedInvMass = static_cast<std::uint16_t>(m->packedInverseInertia[3]);
            if (packedInvMass == 0) {
                return 0.0f;
            }
            const std::uint32_t asUint = static_cast<std::uint32_t>(packedInvMass) << 16;
            float invMass = 0.0f;
            std::memcpy(&invMass, &asUint, sizeof(float));
            if (!std::isfinite(invMass) || invMass <= 0.0001f) {
                return 0.0f;
            }
            return 1.0f / invMass;
        }

        // Current player hknpWorld (HeldBodyGrabManager::GetCurrentHknpWorld pattern). Used to
        // gate teardown: native calls against a CACHED world are only safe while that world is
        // still the live one (review B2 — door/elevator transitions fire no F4SE message).
        void* currentHknpWorld()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) {
                return nullptr;
            }
            using GetbhkWorld_t = void* (*)(RE::TESObjectCELL*);
            static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
            void* bhkWorld = GetbhkWorld(player->parentCell);
            if (!bhkWorld) {
                return nullptr;
            }
            return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhkWorld) + 0x60);
        }

        // Drive the (keyframed, no-contact) proxy body A to the live hand pose.
        void driveProxyToHand(bethesda_physics_body::BethesdaPhysicsBody& proxy, void* hknpWorld,
                              const RE::NiPoint3& handPosGame, const RE::NiMatrix3& handRot, float deltaTime)
        {
            if (!proxy.IsValid() || !hknpWorld) {
                return;
            }
            const RE::NiPoint4 hkPos(
                handPosGame.x * kHavokWorldScale,
                handPosGame.y * kHavokWorldScale,
                handPosGame.z * kHavokWorldScale,
                0.0f);
            const RE::NiPoint4 hkQuat = matrixToQuaternion(handRot);
            // invDeltaTime — velocity = displacement/dt = full solver velocity (the documented
            // dt-vs-invDt keyframe-drive bug: passing dt here under-drives by ~dt^2). This sets a
            // solver VELOCITY for smoothness/feed-forward.
            const float invDt = deltaTime > 0.0001f ? 1.0f / deltaTime : 90.0f;
            safeApplyHardKeyFrame(hknpWorld, proxy.GetBodyId(), hkPos, hkQuat, invDt);
            // FOLLOW FIX (Jul 6): the object's motor targets proxy body A's WORLD ORIGIN, so the
            // object only tracks if A is AT the hand each frame. ApplyHardKeyFrame only sets a
            // velocity (deferred, and it decays when the object loads the motor) — so the anchor
            // lagged and the object "settled beside the hand and stopped following". Teleport A to
            // the exact hand pose every frame (same SetTransform the commit uses), pairing the
            // velocity feed-forward with an exact position — mirrors HeldBody UpdateHandBodyPosition
            // (ApplyHardKeyFrame + setBodyPosition, HeldBodyGrab.cpp:1978-2001).
            RE::hkTransformf proxyXform;
            proxyXform.rotation = handRot;
            proxyXform.translation = hkPos;
            proxy.SetTransform(&proxyXform);
            safeActivateBody(hknpWorld, proxy.GetBodyId());
        }
    }

    RockGrabCoreManager& RockGrabCoreManager::GetSingleton()
    {
        static RockGrabCoreManager instance;
        return instance;
    }

    void RockGrabCoreManager::Initialize()
    {
        if (_initialized) {
            return;
        }
        _initialized = true;
        _active[0] = _active[1] = false;
        _committed[0] = _committed[1] = false;
        spdlog::info("[ROCK::GrabCore] Initialized (P4 — motor-constraint grab live behind iGrabMode=8)");
    }

    void RockGrabCoreManager::Shutdown()
    {
        if (!_initialized) {
            return;
        }
        // Shutdown runs on EVERY kPreLoadGame/kNewGame — tear down per-hand state, but keep
        // _initialized true (review B3: dropping it made IsAvailable() false forever after the
        // first reload, silently degrading iGrabMode=8 to keyframed for the rest of the session;
        // Initialize() only runs once per process).
        void* liveWorld = currentHknpWorld();
        for (int h = 0; h < 2; ++h) {
            if (_committed[h] && _cachedWorld[h]) {
                if (_cachedWorld[h] == liveWorld) {
                    // World still live — full native teardown.
                    Physics::ClearPlayerProxySupportBody();
                    rock_core::destroyGrabConstraint(_cachedWorld[h], _constraint[h]);
                    rock_core::restoreGrabbedInertia(_cachedWorld[h], _saved[h]);
                } else {
                    // World already gone (review B2): the constraint/bodies died with it —
                    // issuing native calls against the stale pointer would UAF. Abandon.
                    spdlog::warn("[ROCK::GrabCore] Shutdown: cached world stale — abandoning constraint cid=0x{:08X}",
                                 _constraint[h].constraintId);
                    _constraint[h].clear();
                }
                _committed[h] = false;
            }
            // Proxy: destroy only against its own STILL-LIVE creating world; else abandon.
            if (_proxy[h].IsValid()) {
                if (_proxyWorld[h] && _proxyWorld[h] == liveWorld && _proxyBhkWorld[h]) {
                    _proxy[h].Destroy(_proxyBhkWorld[h]);
                } else {
                    _proxy[h].Abandon();
                }
            }
            _proxyWorld[h] = nullptr;
            _proxyBhkWorld[h] = nullptr;
            _saved[h].clear();
            _constraint[h].clear();
            _cachedWorld[h] = nullptr;
            _cachedBhkWorld[h] = nullptr;
            _objectBodyId[h] = 0x7FFFFFFF;
        }
        // Do NOT force-drain the retire ring or drop the JIT vtable here (review F2): at
        // kPreLoadGame the old world can still step, and freeing retired payloads at T+0
        // recreates the exact UAF the 8-step ring prevents. Payloads age out via
        // ServicePhysicsStep; the vtable persists for the process (rebuilt lazily anyway).
        _active[0] = _active[1] = false;
        spdlog::info("[ROCK::GrabCore] Shutdown (per-hand teardown; manager stays available)");
    }

    bool RockGrabCoreManager::IsAvailable()
    {
        return GetSingleton()._initialized;
    }

    bool RockGrabCoreManager::StartGrab(GrabState& /*state*/, bool isLeft, const RE::NiPoint3& handPos, const RE::NiMatrix3& /*handRot*/)
    {
        // P4: mark-only. The object keeps Heisenberg's keyframed pull-in/placement; the real
        // constraint work fires from TryCommitGrab once the settle gate passes.
        _active[isLeft ? 0 : 1] = true;
        spdlog::info("[ROCK::GrabCore] StartGrab {} hand @ ({:.1f},{:.1f},{:.1f}) — awaiting settle-commit",
                     isLeft ? "left" : "right", handPos.x, handPos.y, handPos.z);
        return true;
    }

    bool RockGrabCoreManager::TryCommitGrab(GrabState& state, bool isLeft, void* bhkWorld, void* hknpWorld,
                                            std::uint32_t objectBodyId, const RE::NiTransform& handWorld)
    {
        const int h = isLeft ? 0 : 1;
        if (_committed[h]) {
            return true;
        }
        if (!bhkWorld || !hknpWorld || objectBodyId == 0x7FFFFFFF || !state.node) {
            return false;
        }

        // ── Snapshot object state (restored on release/abort) ──
        _saved[h].clear();
        _saved[h].bodyId = RE::hknpBodyId{ objectBodyId };
        _saved[h].refr = state.GetRefr();
        // Use the TRUE pre-grab filter the host captured BEFORE it applied the held-object pair
        // filter (state.heldOriginalFilterInfo, set in TryDisablePlayerHeldObjectCollision at
        // Grab.cpp:1377/1393 — which runs in the settle gate just BEFORE this commit). Reading the
        // LIVE filter here would capture the POLLUTED (0x000B no-collide) value, and EndGrab would
        // then "restore" collision-DISABLED — leaving the released object un-grabbable and falling
        // through the floor (user report Jul 6). Fall back to a live read only if unset.
        if (state.heldOriginalFilterInfo != 0) {
            _saved[h].originalFilterInfo = state.heldOriginalFilterInfo;
        } else {
            std::uint32_t curFilter = 0;
            if (Physics::TryReadBodyFilterInfo(hknpWorld, objectBodyId, curFilter)) {
                _saved[h].originalFilterInfo = curFilter;
            }
        }

        // ── Inertia normalize (HIGGS 10x clamp — lets the ragdoll motors rotate any axis).
        //    Object is already DYNAMIC here (caller flipped it), matching ROCK's own order
        //    (setMotionRecursive(Dynamic) THEN normalizeGrabbedInertiaForBodies). ──
        rock_core::normalizeGrabbedInertia(hknpWorld, RE::hknpBodyId{ objectBodyId }, _saved[h]);

        // ── Start at rest so the motors ramp from zero instead of fighting stale velocity ──
        const RE::NiPoint4 zero(0.0f, 0.0f, 0.0f, 0.0f);
        safeSetBodyVelocity(hknpWorld, objectBodyId, zero, zero);

        // ── Proxy body A: hidden keyframed no-contact anchor (created once per hand, reused).
        //    WORLD-CHANGE GUARD (review B2): door/elevator transitions fire no F4SE message —
        //    a proxy minted in a previous world is a stale body index in this one (UB/CTD in
        //    CreateConstraint). Recreate on world mismatch; the old wrappers are abandoned
        //    (destroying against the dead world would itself UAF). ──
        if (_proxy[h].IsValid() && _proxyWorld[h] != hknpWorld) {
            spdlog::info("[ROCK::GrabCore] proxy world changed ({} hand) — recreating proxy body",
                         isLeft ? "left" : "right");
            _proxy[h].Abandon();
            _proxyWorld[h] = nullptr;
            _proxyBhkWorld[h] = nullptr;
        }
        if (!_proxy[h].IsValid()) {
            auto* sphere = Physics::GetSharedSphereShape();
            if (!sphere ||
                !_proxy[h].Create(bhkWorld, hknpWorld, sphere, noContactFilterInfo(),
                                  bethesda_physics_body::MotionType::Keyframed,
                                  isLeft ? "Heisenberg_GrabProxy8_L" : "Heisenberg_GrabProxy8_R")) {
                spdlog::warn("[ROCK::GrabCore] proxy body create FAILED ({} hand) — keeping keyframed hold",
                             isLeft ? "left" : "right");
                rock_core::restoreGrabbedInertia(hknpWorld, _saved[h]);
                _saved[h].clear();
                return false;
            }
            // Track the creating worlds IMMEDIATELY (review F4: previously only set on commit
            // success, so a create-fail leaked an untracked proxy).
            _proxyWorld[h] = hknpWorld;
            _proxyBhkWorld[h] = bhkWorld;
        }
        // ── Snap the proxy onto the hand via an IMMEDIATE TELEPORT (SetTransform), not a
        //    velocity keyframe. createGrabConstraint below runs THIS post-physics frame BEFORE
        //    any Havok step, and ApplyHardKeyFrame only sets a solver *velocity* — so without a
        //    real teleport the proxy body A is still at the Havok ORIGIN (0,0,0) when the motor
        //    is created, and the motor flings dynamic object B across the world toward the origin
        //    (the "fly away" — RCA Jul 6, FLY cause #1). RE::hkTransformf layout per Grab.cpp:1548. ──
        {
            RE::hkTransformf proxyXform;
            proxyXform.rotation = handWorld.rotate;
            proxyXform.translation = RE::NiPoint4(
                handWorld.translate.x * kHavokWorldScale,
                handWorld.translate.y * kHavokWorldScale,
                handWorld.translate.z * kHavokWorldScale,
                0.0f);
            _proxy[h].SetTransform(&proxyXform);
        }
        safeSetBodyVelocity(hknpWorld, _proxy[h].GetBodyId(), zero, zero);  // at rest at commit; per-frame UpdateGrab drives it after

        // ── The grab pocket: object's CURRENT pose in hand space (row-vector convention:
        //    world = local * parent  =>  local = world * inverse(parent)). Captured at commit;
        //    the motors hold this relative pose. (Geometry-refined pockets = P5.)
        //    Use the RIGID-BODY frame, NOT the graphics node — body B's local frame differs from
        //    the node by the bhkRigidBodyT offset; omitting it engages the motor with an instant
        //    violation (translation yank + bad angular target that spins off-center objects) =
        //    FLY cause #2 (RCA Jul 6). Same helper the proven HeldBody/keyframed paths use. ──
        const RE::NiTransform bodyOffset =
            heisenberg::Utils::GetPhysicsBodyOffset(state.collisionObject, state.node.get());
        const RE::NiTransform objectWorld =
            heisenberg::Utils::ApplyPhysicsBodyOffset(state.node->world, bodyOffset);
        RE::NiTransform desiredBodyTransformHandSpace;
        const RE::NiMatrix3 invHandRot = handWorld.rotate.Transpose();
        desiredBodyTransformHandSpace.rotate = objectWorld.rotate * invHandRot;
        const RE::NiPoint3 rel{ objectWorld.translate.x - handWorld.translate.x,
                                objectWorld.translate.y - handWorld.translate.y,
                                objectWorld.translate.z - handWorld.translate.z };
        // Row-vector: hand-local position = handRot * relWorld (rows are the hand's axes).
        desiredBodyTransformHandSpace.translate = handWorld.rotate * rel;
        desiredBodyTransformHandSpace.scale = 1.0f;

        // ── Motor tuning: HIGGS numbers. The critical line is angular = linear / 12.5 — the
        //    thin overload (RockGrabConstraint.cpp:618) drops the ratio (12.5x too stiff). ──
        rock_core::GrabConstraintMotorTuning tuning{};
        tuning.linearTau = 0.03f;
        tuning.linearDamping = 0.8f;
        tuning.linearProportionalRecovery = g_config.grabConstraintLinearProportionalRecoveryVelocity;
        tuning.linearConstantRecovery = g_config.grabConstraintLinearConstantRecoveryVelocity;
        // MASS-SCALED FORCE (Jul 6): both HIGGS (hand.cpp:3983) and standalone ROCK
        // (GrabMotionController.h:721 capForceByMass) cap the motor force at mass*ratio. A flat
        // 2000 for a light clutter item (mass~1 -> should be 500) over-drives it 4x -> overshoot/
        // jitter; heavy items got too little relative authority -> trail. This is the ROOT of
        // mode-8's not-rock-solid feel (the user's #1). Cap = min(2000, max(mass,2.0)*500).
        const float objMass = readObjectMass(hknpWorld, objectBodyId);
        const float effMass = (objMass > kMotorMassFloor) ? objMass : kMotorMassFloor;
        const float cappedLinearForce =
            (std::min)(g_config.grabConstraintLinearMaxForce, effMass * kForceToMassRatio);
        tuning.linearMaxForce = cappedLinearForce;
        tuning.angularTau = 0.03f;
        tuning.angularDamping = 0.8f;
        tuning.angularProportionalRecovery = g_config.grabConstraintLinearProportionalRecoveryVelocity;
        tuning.angularConstantRecovery = g_config.grabConstraintLinearConstantRecoveryVelocity;
        tuning.angularMaxForce = cappedLinearForce
                               / (std::max)(1.0f, g_config.grabConstraintAngularToLinearForceRatio);
        spdlog::info("[ROCK::GrabCore] {} hand mass={:.2f} -> motor force lin={:.0f} ang={:.0f} (cap {:.0f})",
                     isLeft ? "left" : "right", objMass, tuning.linearMaxForce, tuning.angularMaxForce,
                     g_config.grabConstraintLinearMaxForce);

        _constraint[h] = rock_core::createGrabConstraint(
            hknpWorld,
            RE::hknpBodyId{ _proxy[h].GetBodyId() },
            RE::hknpBodyId{ objectBodyId },
            handWorld,
            handWorld.translate,  // palm ~ hand origin for P4 (palm-refined pivot = P5)
            desiredBodyTransformHandSpace,
            tuning);

        if (!_constraint[h].isValid()) {
            spdlog::warn("[ROCK::GrabCore] createGrabConstraint FAILED ({} hand) — keeping keyframed hold",
                         isLeft ? "left" : "right");
            rock_core::restoreGrabbedInertia(hknpWorld, _saved[h]);
            _saved[h].clear();
            return false;
        }

        _cachedWorld[h] = hknpWorld;
        _cachedBhkWorld[h] = bhkWorld;
        _objectBodyId[h] = objectBodyId;
        _committed[h] = true;
        _diagFrames[h] = 0;  // [GC-DIAG] arm the post-commit tracer
        _diagPrevObjPos[h] = {};
        safeActivateBody(hknpWorld, objectBodyId);
        spdlog::info("[ROCK::GrabCore] constraint CREATED {} hand: cid=0x{:08X} objBody=0x{:08X} proxyBody=0x{:08X}",
                     isLeft ? "left" : "right", _constraint[h].constraintId, objectBodyId, _proxy[h].GetBodyId());
        return true;
    }

    void RockGrabCoreManager::UpdateGrab(GrabState& /*state*/, bool isLeft, const RE::NiPoint3& handPos, const RE::NiMatrix3& handRot, float deltaTime)
    {
        const int h = isLeft ? 0 : 1;
        if (!_committed[h] || !_cachedWorld[h]) {
            return;  // pre-commit frames: Heisenberg's keyframed drive owns the object
        }
        // P4 minimal: keyframe the proxy to the live hand; the finite-force motors pull the
        // dynamic object toward the pocket. (Per-frame motor-target rewrite + tau ramp = P5.)
        driveProxyToHand(_proxy[h], _cachedWorld[h], handPos, handRot, deltaTime);

        // [GC-DIAG Jul 18] Post-commit tracer: the object was observed FALLING out of the hand
        // the instant the settle-gate commits (2.9cm -> 28.9cm lag in 0.2s, then rests). Trace
        // the first 12 frames: object/proxy body positions, measured object velocity, and the
        // constraint runtime's solver-result words. All-zero solver words while the object
        // free-falls = constraint never entered the solver; nonzero = solving toward a wrong
        // target. One line per frame at debug level, self-disarms after 12.
        if (_diagFrames[h] < 12) {
            const float toGame = rock_core::havokToGameScale();
            auto* objBody = rock_core::havok_runtime::getBody(_cachedWorld[h], RE::hknpBodyId{ _objectBodyId[h] });
            auto* proxyBody = rock_core::havok_runtime::getBody(_cachedWorld[h], RE::hknpBodyId{ _proxy[h].GetBodyId() });
            RE::NiPoint3 objPos{}, proxyPos{};
            if (objBody) { objPos = { objBody->position[0] * toGame, objBody->position[1] * toGame, objBody->position[2] * toGame }; }
            if (proxyBody) { proxyPos = { proxyBody->position[0] * toGame, proxyBody->position[1] * toGame, proxyBody->position[2] * toGame }; }
            RE::NiPoint3 measuredVel{};
            if (_diagFrames[h] > 0 && deltaTime > 0.0001f) {
                measuredVel = (objPos - _diagPrevObjPos[h]) * (1.0f / deltaTime);
            }
            _diagPrevObjPos[h] = objPos;
            std::uint32_t rt[4] = { 0, 0, 0, 0 };
            int rtSize = 0;
            if (void* runtime = rock_core::getLastConstraintRuntime(&rtSize); runtime && _constraint[h].isValid() && rtSize >= 16) {
                std::memcpy(rt, runtime, sizeof(rt));
            }
            spdlog::debug("[GC-DIAG] {} f{} obj=({:.1f},{:.1f},{:.1f}) proxy=({:.1f},{:.1f},{:.1f}) hand=({:.1f},{:.1f},{:.1f}) vel=({:.0f},{:.0f},{:.0f})u/s cid=0x{:08X} rt=[{:08X} {:08X} {:08X} {:08X}] rtSize={}",
                          isLeft ? "L" : "R", _diagFrames[h],
                          objPos.x, objPos.y, objPos.z,
                          proxyPos.x, proxyPos.y, proxyPos.z,
                          handPos.x, handPos.y, handPos.z,
                          measuredVel.x, measuredVel.y, measuredVel.z,
                          _constraint[h].constraintId, rt[0], rt[1], rt[2], rt[3], rtSize);
            ++_diagFrames[h];
        }
    }

    void RockGrabCoreManager::EndGrab(GrabState& state, bool isLeft, const RE::NiPoint3* throwVelocity)
    {
        const int h = isLeft ? 0 : 1;
        _active[h] = false;
        if (!_committed[h]) {
            return;
        }
        void* hknpWorld = _cachedWorld[h];

        // WORLD-CHANGE GUARD (review B2): if the commit-time world is no longer the live one,
        // the constraint/bodies died with it — native teardown against the stale pointer would
        // UAF. Abandon bookkeeping and bail.
        if (hknpWorld != currentHknpWorld()) {
            spdlog::warn("[ROCK::GrabCore] EndGrab {}: cached world stale — abandoning constraint cid=0x{:08X}",
                         isLeft ? "left" : "right", _constraint[h].constraintId);
            _constraint[h].clear();
            _proxy[h].Abandon();
            _proxyWorld[h] = nullptr;
            _proxyBhkWorld[h] = nullptr;
            _saved[h].clear();
            _cachedWorld[h] = nullptr;
            _cachedBhkWorld[h] = nullptr;
            _objectBodyId[h] = 0x7FFFFFFF;
            _committed[h] = false;
            state.rockGrabCoreConstraintCreated = false;
            return;
        }

        // 1. Char-proxy guard FIRST — the proxy may cache our object/proxy body as its support
        //    body; clear before anything it references is torn down (dangling-body CTD class).
        Physics::ClearPlayerProxySupportBody();

        // 2. Constraint teardown: motors off -> RemoveConstraintBodyMap -> DestroyConstraints ->
        //    retire ring (frees the 0x168 data + motors only after 8 post-physics steps; the
        //    ring is drained by ServicePhysicsStep — NEVER free inline, that's the UAF guard).
        rock_core::destroyGrabConstraint(hknpWorld, _constraint[h]);

        // 3. Restore inertia while the body is still resolvable.
        rock_core::restoreGrabbedInertia(hknpWorld, _saved[h]);

        // 4. Restore the object's original collision filter (pair-filter bits were host-applied;
        //    the host's own restore also runs, this is the byte-exact backstop).
        if (_saved[h].originalFilterInfo != 0) {
            Physics::TryWriteBodyFilterInfo(hknpWorld, _objectBodyId[h], _saved[h].originalFilterInfo);
        }

        // 5. Release velocity: throw or settle. Object STAYS DYNAMIC (ROCK/HIGGS semantics —
        //    it settles naturally; no keyframed restore).
        if (throwVelocity && (throwVelocity->x != 0.0f || throwVelocity->y != 0.0f || throwVelocity->z != 0.0f)) {
            RE::NiPoint3 v = *throwVelocity;
            const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (speed > kMaxThrowSpeedGame && speed > 0.0f) {
                const float s = kMaxThrowSpeedGame / speed;
                v.x *= s; v.y *= s; v.z *= s;
            }
            const RE::NiPoint4 hkLin(v.x * kHavokWorldScale, v.y * kHavokWorldScale, v.z * kHavokWorldScale, 0.0f);
            const RE::NiPoint4 zero(0.0f, 0.0f, 0.0f, 0.0f);
            safeSetBodyVelocity(hknpWorld, _objectBodyId[h], hkLin, zero);
            safeActivateBody(hknpWorld, _objectBodyId[h]);
            // NOTE: no ThrownObjectTracker::OnThrown here — EndGrabKeyframed (which still runs
            // for mode-8 releases) is the single OnThrown owner (review F3: double-fire caused
            // duplicate impact-damage/aggro tracking).
        } else {
            const RE::NiPoint4 zero(0.0f, 0.0f, 0.0f, 0.0f);
            safeSetBodyVelocity(hknpWorld, _objectBodyId[h], zero, zero);
            safeActivateBody(hknpWorld, _objectBodyId[h]);  // awake so it settles under gravity
        }

        // 6. Park proxy A (keep the body — persistent per hand; destroyed only in Shutdown).
        const RE::NiPoint4 zero(0.0f, 0.0f, 0.0f, 0.0f);
        safeSetBodyVelocity(hknpWorld, _proxy[h].GetBodyId(), zero, zero);

        spdlog::info("[ROCK::GrabCore] constraint RETIRED {} hand: cid=0x{:08X} (8-step deferred free)",
                     isLeft ? "left" : "right", _constraint[h].constraintId);
        _constraint[h].clear();
        _saved[h].clear();
        _cachedWorld[h] = nullptr;
        _objectBodyId[h] = 0x7FFFFFFF;
        _committed[h] = false;
        state.rockGrabCoreConstraintCreated = false;
    }

    void RockGrabCoreManager::ServicePhysicsStep(std::uint32_t completedSteps)
    {
        heisenberg::rock_core::serviceRetiredGrabConstraintPayloads(completedSteps);
    }
}
