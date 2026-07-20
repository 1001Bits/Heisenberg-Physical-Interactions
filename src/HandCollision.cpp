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
#include "rock_integration/PushAssist.h"
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
        namespace clp = heisenberg::rock_core::collision_layer_policy;
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

        // ROCK PushAssist model (toggle bPushAssist): instead of replacing the object's
        // velocity, compute a push IMPULSE along the hand velocity (clamped + layer-scaled,
        // gated by min speed) and ACCUMULATE it onto the object's current velocity, so the
        // object's existing motion is preserved. 1:1 with rock::push_assist::computePushImpulse.
        if (g_config.rockPushAssist) {
            heisenberg::rock_push_assist::PushAssistInput<RE::NiPoint3> pin{};
            pin.sourceVelocity = RE::NiPoint3(handVel.x * kGameToHavok, handVel.y * kGameToHavok, handVel.z * kGameToHavok);
            pin.minSpeed = g_config.handPushVelocityThreshold * kGameToHavok;
            pin.maxImpulse = g_config.pushAssistMaxImpulse;
            pin.layerMultiplier = pushMultiplier;
            const auto pa = heisenberg::rock_push_assist::computePushImpulse(pin);
            if (!pa.apply) {
                return;  // below threshold / invalid — no push this frame
            }
            s_pushScratch.pushVel[0] = s_curScratch.v[0] + pa.impulse.x;
            s_pushScratch.pushVel[1] = s_curScratch.v[1] + pa.impulse.y;
            s_pushScratch.pushVel[2] = s_curScratch.v[2] + pa.impulse.z;
            s_pushScratch.pushVel[3] = 0.0f;
        }

        CollisionFunctions::SetLinearVelocity(colObj, pushVel);

        // WAKE the pushed object — a settled (asleep) body ignores SetLinearVelocity, which is the
        // "sometimes pushes, sometimes not". The per-frame hand wake only covers ~15u around the
        // wand; a large object's body centre can sit outside that box, so wake THIS object directly.
        if (node) {
            if (void* hknpW = GetCurrentHknpWorld()) {
                auto activateInAabb = reinterpret_cast<void(__fastcall*)(void*, void*)>(
                    REL::Module::get().base() + 0x1546f80);
                const RE::NiPoint3& op = node->world.translate;
                const float wr = 12.0f * kGameToHavok;
                const float ox = op.x * kGameToHavok, oy = op.y * kGameToHavok, oz = op.z * kGameToHavok;
                alignas(16) float aabb[8] = { ox - wr, oy - wr, oz - wr, 0.0f, ox + wr, oy + wr, oz + wr, 0.0f };
                activateInAabb(hknpW, aabb);
            }
        }

        spdlog::debug("[HAND_COLLISION] Swept push refr {:08X} from ({:.1f},{:.1f},{:.1f}) hkVel=({:.2f},{:.2f},{:.2f}) preservedZ={:.2f}",
                      refr->formID, handPos.x, handPos.y, handPos.z,
                      s_pushScratch.pushVel[0], s_pushScratch.pushVel[1], s_pushScratch.pushVel[2],
                      curZ_hk);
    }

    void HandCollision::PushObjectsToward(const RE::NiPoint3& center, const RE::NiPoint3& velocity,
                                          float radius, RE::TESObjectREFR* ignore)
    {
        const float speed = Length(velocity);
        if (speed < 5.0f) return;  // held object barely moving — nothing to shove

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        std::vector<RE::TESObjectREFR*> nearby = Physics::GetObjectsInRadius(center, radius, player);
        for (auto* refr : nearby) {
            if (!refr || refr == ignore || !Physics::IsGrabbable(refr)) continue;
            RE::NiPoint3 c;
            float r = 0.0f;
            if (!GetObjectBounds(refr, c, r)) continue;
            const RE::NiPoint3 sep = c - center;
            const float dist = Length(sep);
            if (dist > radius) continue;
            const RE::NiPoint3 toClutter = NormalizeOrZero(sep);
            // Only shove clutter the held object is moving TOWARD — what's in front of it.
            const float into = Dot(velocity, toClutter);
            if (into < 1.0f) continue;
            ApplyPushForce(refr, c, toClutter * into, 1.0f / 90.0f);  // SetLinearVelocity + wake
        }
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
        // (Restored from 0.8 per user — the cup pushed reliably with this cloud.)
        {
            static const char* const kFingerTipBones[2][5] = {
                { "RArm_Finger13", "RArm_Finger23", "RArm_Finger33", "RArm_Finger43", "RArm_Finger53" },
                { "LArm_Finger13", "LArm_Finger23", "LArm_Finger33", "LArm_Finger43", "LArm_Finger53" },
            };
            auto* pl = f4vr::getPlayer();
            RE::NiAVObject* root = (pl && pl->firstPerson3D.get()) ? pl->firstPerson3D.get() : nullptr;
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

        // SWEPT CONTACT: a single-frame snapshot of the hand points tunnels straight through a small
        // object on a FAST swipe (the hand teleports past it between frames, so no sample ever lands
        // on it). Densify the cloud along the hand's MOTION since last frame — interpolate each point
        // from its previous-frame position to its current one, with more sub-steps the further the
        // hand moved. All the contact tests below run on this swept set, so a fast pass still drops a
        // sample onto the object.
        const int handIdx = isLeft ? 1 : 0;
        static RE::NiPoint3 s_prevHandPts[2][16];
        static int s_prevHandPtCount[2] = { 0, 0 };

        RE::NiPoint3 sweptPts[16 * 16];
        int sweptCount = 0;
        if (s_prevHandPtCount[handIdx] == handPtCount && handPtCount > 0) {
            float maxDisp = 0.0f;
            for (int i = 0; i < handPtCount; ++i) {
                maxDisp = (std::max)(maxDisp, Length(handPts[i] - s_prevHandPts[handIdx][i]));
            }
            // ~2 game-unit spacing along the sweep so nothing slips between samples; 1..6 sub-steps.
            // ~1.5 game-unit spacing along the sweep, up to 16 sub-steps, so even a fast swipe never
            // skips over a SMALL object (the coffee cup). The triangle cap now bounds the per-object
            // cost, so dense swept sampling is affordable. handPtCount(≤16)*steps(≤16) ≤ 256 = array.
            const int steps = (std::clamp)(static_cast<int>(std::ceil(maxDisp / 1.5f)), 1, 16);
            for (int i = 0; i < handPtCount; ++i) {
                for (int st = 0; st < steps; ++st) {
                    const float t = (steps > 1) ? static_cast<float>(st) / static_cast<float>(steps - 1) : 1.0f;
                    sweptPts[sweptCount++] = s_prevHandPts[handIdx][i] * (1.0f - t) + handPts[i] * t;
                }
            }
        } else {
            // No usable history (first frame, or the cloud shape changed) — just use current points.
            for (int i = 0; i < handPtCount; ++i) sweptPts[sweptCount++] = handPts[i];
        }
        // Remember this frame's cloud for next frame's sweep.
        for (int i = 0; i < handPtCount; ++i) s_prevHandPts[handIdx][i] = handPts[i];
        s_prevHandPtCount[handIdx] = handPtCount;

        RE::NiPoint3 handCentroid(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < sweptCount; ++i) {
            handCentroid = handCentroid + sweptPts[i];
        }
        handCentroid = handCentroid * (1.0f / static_cast<float>(sweptCount));

        // Broadphase query encloses the whole SWEPT point cloud plus a margin.
        float spread = 0.0f;
        for (int i = 0; i < sweptCount; ++i) {
            spread = (std::max)(spread, Length(sweptPts[i] - handCentroid));
        }
        std::vector<RE::TESObjectREFR*> nearby =
            Physics::GetObjectsInRadius(handCentroid, spread + handRadius + 80.0f, player);

        RE::TESObjectREFR* bestRefr = nullptr;   // nearest TOUCHED object — drives the haptic pulse
        float bestScore = (std::numeric_limits<float>::max)();
        int pushedCount = 0;

        // Push EVERY object the hand cloud is genuinely touching — not just the closest. Pushing
        // only the single nearest was the "pot pushes but the cup right next to it doesn't" report.
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

            // Nearest SWEPT hand point to this object's bounds center — ANY point on the hand's
            // path this frame counts (so a fast pass still registers).
            RE::NiPoint3 closestHandPt = sweptPts[0];
            float distSq = (std::numeric_limits<float>::max)();
            for (int i = 0; i < sweptCount; ++i) {
                const float d = LengthSq(center - sweptPts[i]);
                if (d < distSq) { distSq = d; closestHandPt = sweptPts[i]; }
            }
            if (distSq > triggerDist * triggerDist) {
                continue;  // hand not near this object's bounds
            }

            // TRUE TOUCH for THIS object: require a hand sample within handContactSlop of its OUTER
            // MESH. Push from the mesh point — never the node/bounds centre — so contact MUST be
            // verified against real triangles; if the mesh can't be extracted, don't push it.
            RE::NiPoint3 touchHandPt = closestHandPt;
            RE::NiPoint3 contactPt(0.0f, 0.0f, 0.0f);
            bool haveContact = false;
            float bestMeshDist = (std::numeric_limits<float>::max)();  // hoisted — used by depenetration
            if (auto* objNode = refr->Get3D()) {
                std::vector<heisenberg::TriangleData> tris;
                tris.reserve(256);
                heisenberg::GetTriangles(objNode, tris, 4000);  // perf cap — high-poly meshes froze the game
                if (!tris.empty()) {
                    RE::NiPoint3 meshPt;
                    float md = -1.0f;
                    for (int i = 0; i < sweptCount; ++i) {
                        if (heisenberg::GetClosestMeshPointToPoint(tris, sweptPts[i], meshPt, md) && md < bestMeshDist) {
                            bestMeshDist = md;
                            touchHandPt = sweptPts[i];
                            contactPt = meshPt;
                            haveContact = true;
                        }
                    }
                    if (bestMeshDist > g_config.handContactSlop) {
                        haveContact = false;  // hand hasn't reached the real surface yet
                    }
                }
            }
            // BOUNDS FALLBACK for SMALL objects (the coffee cup): the precise per-triangle mesh test
            // can miss a small/thin-walled object — the swept samples land just outside the contact
            // band, or between the cup's walls — so the hand "passes through" it. When the hand is
            // clearly inside a SMALL object's bounds sphere, push from the bounds surface instead.
            // Restricted to small objects (objectRadius small) so large objects still require the
            // precise mesh test and can't be shoved from a distance.
            if (!haveContact && objectRadius <= 9.0f) {
                const RE::NiPoint3 toCenter = center - closestHandPt;
                const float dC = Length(toCenter);
                if (dC <= objectRadius + g_config.handContactSlop) {
                    const RE::NiPoint3 nrm = NormalizeOrZero(toCenter);
                    contactPt   = center - nrm * objectRadius;  // bounds surface nearest the hand
                    touchHandPt = closestHandPt;
                    haveContact = true;
                }
            }

            if (!haveContact) {
                continue;  // no verified outer-mesh contact → don't push this object
            }

            // Nearest touched object drives the haptic/contact pulse.
            if (distSq < bestScore) { bestScore = distSq; bestRefr = refr; }

            // DIRECTIONAL NORMAL PUSH. Shove the object ONLY along the contact normal (swept hand
            // sample → mesh point), by the hand's speed INTO that surface. The into-surface test is
            // what makes lateral motion and the RETURN stroke impart nothing (no sticky drag /
            // follow-back) — on retract handVel points away from the surface so intoContact goes < 0.
            // We deliberately do NOT gate on a wand→contact "approach" here: on a FAST swipe the wand
            // overshoots the object by the time we detect the swept contact, which would (wrongly)
            // read as moving away and skip the push. intoContact (computed at the contact sample) is
            // the correct, swept-safe gate. The velocity is direction-resolved here; ApplyPushForce
            // just scales it into Havok space and wakes the body.
            // PUSH = swat + depenetration, ALWAYS along the contact normal (swept hand sample → mesh
            // point) so it is only ever directed AWAY from the hand:
            //  - SWAT: a fast hit INTO the surface (hand speed along the normal) knocks the object
            //    flying — the satisfying fast-hand knock.
            //  - DEPENETRATION: while the hand is within the contact band, eject the object out of the
            //    hand proportional to how far IN the hand is, INDEPENDENT of hand speed. This is what
            //    stops a SLOW hand clipping through, and because it is purely outward it can NEVER
            //    pull the object back on the retract stroke (the old velocity-only push did when the
            //    hand was penetrating).
            const RE::NiPoint3 toContact = contactPt - touchHandPt;
            if (LengthSq(toContact) > 1.0e-4f) {
                const RE::NiPoint3 dir = NormalizeOrZero(toContact);  // hand → surface = away from hand
                float pushMag = 0.0f;
                const float intoContact = Dot(handVel, dir);
                if (intoContact >= 1.0f) {
                    pushMag += intoContact;  // swat (fast hand into the surface)
                }
                const float penetration = g_config.handContactSlop - bestMeshDist;  // >0 inside the band
                if (penetration > 0.0f) {
                    constexpr float kDepenStiffness = 25.0f;  // 1/s — eject rate for slow/penetrating
                    pushMag += penetration * kDepenStiffness;
                }
                if (pushMag > 0.0f) {
                    ApplyPushForce(refr, contactPt, dir * pushMag, deltaTime);
                    ++pushedCount;
                }
            }
        }

        if (bestRefr) {
            SetContactObject(isLeft, bestRefr);
        } else {
            ClearContactObject(isLeft);
        }

        if (pushedCount > 0) {
            static int s_proxPushLog = 0;
            if ((s_proxPushLog++ % 90) == 0) {
                spdlog::info("[HAND_COLLISION] {} hand pushing {} object(s) speed={:.1f}",
                             isLeft ? "left" : "right", pushedCount, handSpeed);
            }
        }
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
        // CRITICAL (char-proxy dangling-support crash): the player char-proxy caches its
        // "ground" support body REFCOUNTED (SetSupportBody 0x1e23150). These cup/finger bodies
        // are keyframed on layer 43 — the proxy can pick them up as support exactly like the
        // ROCK collider bodies — so freeing one the proxy still references dangles supportBody._ptr
        // → next-frame vf066 faults. Null the cached support body FIRST, mirroring
        // HandBoneColliderSet::DestroyHand / BodyBoneColliderSet / WeaponCollision teardown.
        heisenberg::Physics::ClearPlayerProxySupportBody();

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
            // Fully release (Destroy + remove-from-world + delete), not just NULL the pointers.
            // Invalidate() alone leaks the three BethesdaPhysicsBody wrappers and orphans their
            // hknp bodies in the world. ReleaseCupBody is SEH-guarded internally, clears the
            // char-proxy support body first, and ends by invalidating hb.
            ReleaseCupBody(hb);
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
        if (!player || !player->firstPerson3D.get()) return nullptr;
        return player->firstPerson3D.get();
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

        // NOTE: the ROCK integration per-frame entry points (rock_hand_collider /
        // rock_body_collider / rock_weapon_collision / rock_two_handed_grip) were hoisted out
        // of here into Hooks.cpp::UpdateHandCollisionBodies, ABOVE the bEnableHandCollision
        // early-return, so each ticks whenever its own [RockIntegration] toggle is on,
        // independent of the cup/finger hand-collision setting. (This Update() is itself
        // unreachable when bEnableHandCollision=false.)

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
        // Honor the [HandCollision] INI/MCM toggle + scale (were loaded but never consumed;
        // the scale was hardcoded at 500). Default scale is 500 so the shipped feel is unchanged.
        if (!heisenberg::g_config.enableHandCollisionHaptics) {
            return;
        }
        int str = static_cast<int>(intensity * heisenberg::g_config.handCollisionHapticScale);
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
