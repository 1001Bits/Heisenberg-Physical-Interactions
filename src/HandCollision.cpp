#include "HandCollision.h"
#include "Config.h"
#include "Physics.h"
#include "Grab.h"
#include "FingerCurves.h"   // GetTriangles / GetClosestMeshPointToPoint for touch-only mesh check
#include "VRInput.h"
#include "f4vr/PlayerNodes.h"
#include <f4vr/F4VRUtils.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>


// =====================================================================
// BODY-LESS PROXIMITY HAND COLLISION
// Two-scenario cleanup (Jul 2026): the physics hand-body path (cup /
// finger Havok bodies) was removed; real hand colliders come from the
// embedded ROCK engine (S1) or the external ROCK.dll (S2). This file
// keeps the proven proximity push + contact tracking + haptics.
// =====================================================================

namespace heisenberg
{
    // Scale: 1 Havok unit = 70 game units (ROCK confirmed)
    constexpr float kGameToHavok = 1.0f / 70.0f;

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

        // (Two-scenario cleanup: the ported rock_push_assist impulse-accumulate variant was
        // removed with the rock_integration PushAssist module — the embedded engine's own
        // DynamicPushAssist covers that model; this path is the classic velocity push.)

        // colObj's physics system can be torn down between the guarded GET above and here
        // (cell detach / Havok rebuild racing this call — the exact race the __try on the
        // read exists for). IsCollisionObjectValid only checks spSystem non-null, the same
        // precondition that already held when the guarded Get faulted, so it doesn't rule
        // this out. SEH-guard the write + wake the same way the read is guarded, instead of
        // converting a survivable fault into a guaranteed follow-up crash two statements
        // later.
        __try {
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
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            spdlog::warn("[HAND_COLLISION] ApplyPushForce: SetLinearVelocity/wake faulted for refr {:08X} — object's physics system torn down mid-push", refr->formID);
            return;
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

    bool HandCollision::Initialize()
    {
        _leftContact.reset();
        _rightContact.reset();
        _leftContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
        _rightContactBodyId.store(0x7FFFFFFF, std::memory_order_relaxed);
        _pendingLeftContactClear.store(false, std::memory_order_relaxed);
        _pendingRightContactClear.store(false, std::memory_order_relaxed);
        _hasPrevSweepPos[0] = false;
        _hasPrevSweepPos[1] = false;
        _initialized = true;
        spdlog::info("[HAND_COLLISION] Initialized (body-less proximity push)");
        return true;
    }

    void HandCollision::Shutdown()
    {
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

        void* bhkW = nullptr;
        void* hknpW = nullptr;
        GetWorlds(bhkW, hknpW);

        CheckProximityCollisions(leftHandPos, leftHandVel, true, deltaTime);
        CheckProximityCollisions(rightHandPos, rightHandVel, false, deltaTime);

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
