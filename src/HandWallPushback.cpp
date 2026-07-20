#include "HandWallPushback.h"

#include "Config.h"
#include "Physics.h"
#include "FRIKInterface.h"
#include "HandAuthority.h"
#include "WandNodeHelper.h"
#include "Utils.h"

#include "rock_integration/SoftContactMath.h"     // capsule-solve foundation for mode 3 (multi-target soft contact)
#include "rock_integration/HandBoneColliderSet.h"  // BuildHandCapsules — finger geometry for mode 3
#include "rock_integration/BodyBoneColliderSet.h"  // BuildBodyCapsules — body geometry for mode 3
#include "rock_integration/WeaponCollision.h"       // BuildWeaponCapsule — weapon geometry for mode 3
#include "rock_integration/contact/NativeContactEvidenceRuntime.h"  // native Havok manifold evidence (mode 3 world)
#include "Grab.h"                                   // GrabManager — ownership suppression (don't fight grab)

#include "f4vr/PlayerNodes.h"
#include <RE/Fallout.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <spdlog/spdlog.h>

namespace heisenberg::hand_wall_pushback
{
    namespace
    {
        // ---- small vector helpers -------------------------------------------------------
        inline float vlen(const RE::NiPoint3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
        inline RE::NiPoint3 vmul(const RE::NiPoint3& v, float s) { return { v.x * s, v.y * s, v.z * s }; }
        inline RE::NiPoint3 vadd(const RE::NiPoint3& a, const RE::NiPoint3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        inline RE::NiPoint3 vsub(const RE::NiPoint3& a, const RE::NiPoint3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }

        inline RE::NiPoint3 normalizeOr(const RE::NiPoint3& v, const RE::NiPoint3& fallback)
        {
            const float L = vlen(v);
            if (L < 1e-5f) return fallback;
            return { v.x / L, v.y / L, v.z / L };
        }
        inline RE::NiPoint3 clampLength(const RE::NiPoint3& v, float maxLen)
        {
            const float L = vlen(v);
            if (maxLen <= 0.0f) return { 0, 0, 0 };
            if (L <= maxLen || L < 1e-6f) return v;
            return vmul(v, maxLen / L);
        }

        // ---- ROCK SoftContactMath (ported; mode 2) --------------------------------------
        // Hard-stop magnet correction = normal * penetration, clamped to maxCorrection.
        inline RE::NiPoint3 projectTrackedMagnetCorrection(const RE::NiPoint3& normal, float penetration, float maxCorrection)
        {
            if (!std::isfinite(penetration) || penetration <= 0.0f) return { 0, 0, 0 };
            return clampLength(vmul(normalizeOr(normal, RE::NiPoint3(0.0f, 0.0f, 1.0f)), penetration),
                               (maxCorrection > 0.0f ? maxCorrection : 0.0f));
        }
        // Compliant response scale: smoothstep ramp from `scale` (shallow) to 1.0 (>= hardStop).
        inline float compliantHardStopResponseScale(float penetration, float correctionScale, float hardStop)
        {
            if (!std::isfinite(penetration) || penetration <= 0.0f) return 0.0f;
            const float safeScale = std::clamp(std::isfinite(correctionScale) ? correctionScale : 1.0f, 0.0f, 1.0f);
            const float safeHardStop = (hardStop > 0.0f ? hardStop : 0.0f);
            if (safeHardStop <= 0.0f || penetration >= safeHardStop) return 1.0f;
            const float t = std::clamp(penetration / safeHardStop, 0.0f, 1.0f);
            const float ramp = t * t * (3.0f - 2.0f * t);
            return safeScale + (1.0f - safeScale) * ramp;
        }

        // ---- per-hand runtime state -----------------------------------------------------
        struct HandState
        {
            bool overriding = false;
            RE::NiPoint3 rawHandPos{};          // controller-tracked hand pos (override-independent)
            RE::NiMatrix3 handRot{};            // hand rotation applied to the override (tracks the controller)
            bool haveRot = false;
            RE::NiMatrix3 handRotOffsetFromWand{}; // hand-vs-wand rotation offset, captured when not overriding
            bool haveRotOffset = false;
            float handScale = 1.0f;             // FRIK hand scale, captured with the transform (don't pop to 1 while overriding)
            RE::NiPoint3 lastWandPos{};
            bool haveWand = false;
            RE::NiPoint3 smoothedCorr{};        // smoothed correction vector
        };
        HandState g_state[2];  // [0]=left, [1]=right
        double g_lastTime = 0.0;

        // Mode 3 (multi-target soft contact): per-frame finger capsules for both hands,
        // built from the FRIK skeleton. [0]=left, [1]=right (matching g_state).
        namespace scm = heisenberg::rock_core::soft_contact_math;
        std::array<scm::Capsule, heisenberg::rock_hand_collider::kCapsulesPerHand> g_handCaps[2];
        int g_handCapCount[2] = { 0, 0 };
        std::array<scm::Capsule, heisenberg::rock_body_collider::kCapsuleCount> g_bodyCaps;
        int g_bodyCapCount = 0;
        scm::Capsule g_weaponCaps[2];   // [0]=left, [1]=right; .valid = equipped weapon present
        bool g_weaponCapValid[2] = { false, false };

        // Native Havok contact evidence snapshot (mode 3 world candidate): refreshed once per
        // frame in Update(), consumed per-hand in UpdateHand. The physics-thread contact
        // callback (ContactImpulseListener) fills the cache from real manifolds.
        heisenberg::rock_core::contact_evidence::NativeContactEvidenceSnapshot g_nativeSnapshot;

        // 6 axis probe directions.
        const RE::NiPoint3 kProbeDirs[6] = {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
        };

        // ROCK collider layers whose bodies are OUR OWN rig, not walls: 43=hand, 44=weapon,
        // 47=player body capsules. WRIST-SNAP FIX (Jul 5): the probe used to skip only layer 43,
        // so the equipped WEAPON hull (layer 44) sitting right at/in front of the hand registered
        // as a "wall" every frame a weapon was drawn → constant ~14gu pushback → the wrist
        // over-bent/snapped. Skip all three (mirrors the mode-3 native path's filter).
        [[maybe_unused]] static bool isOwnColliderLayer(std::uint32_t layer)
        {
            return layer == 43u || layer == 44u || layer == 47u;
        }

        // Jul 6: ALLOWLIST — only real STATIC WORLD geometry (walls/floors) pushes the hand/weapon.
        // The instrumentation proved the hand probe was hitting the PLAYER'S OWN body / char-proxy
        // (layer 30) + arms every frame (constant jitter, 154/235 frames), not walls. Restricting to
        // static layers means the probe responds ONLY to walls/floors — never the player body,
        // actors, clutter, held objects, or our own colliders. This is the real fix for the
        // "constant weapon-hand jitter"; the barrel probe's absurd 20-42gu depths were the same
        // body-hit and are excluded too.
        static bool isStaticWorldLayer(std::uint32_t layer)
        {
            return layer == 1u    // STATIC   (walls, most level geometry)
                || layer == 2u    // ANIMSTATIC (doors, drawbridges, moving statics)
                || layer == 13u   // TERRAIN
                || layer == 17u;  // GROUND
        }

        // Probe geometry around `pos`. Returns true + deepest penetration (depth, normal).
        // SELF-HIT FILTER: skips hits on our own colliders (layers 43/44/47) — the physics hand
        // bodies (16/hand) + weapon hull + 23 body capsules surround the hand, so without this a
        // ray hits our own rig → constant false "wall" → the rendered hand flickers/contorts.
        bool ProbeHand(const RE::NiPoint3& pos, void* hknpWorld, float probeRadius, float& outDepth, RE::NiPoint3& outNormal)
        {
            float bestDepth = 0.0f;
            bool any = false;
            for (const auto& dir : kProbeDirs) {
                auto r = heisenberg::Physics::CastRay(pos, dir, probeRadius, nullptr);
                if (!r.hit) continue;
                // FAIL-CLOSED filter (Jul 5): only accept hits whose hknp body filter is readable
                // and is NOT one of our own collider layers (43/44/47). CastRay's Phase-1 path
                // (ray-vs-refr bounding sphere) returns hits with NO bodyId and a FABRICATED
                // (0,0,1) normal — treating those as walls produced a constant upward push near
                // any loose refr (worst: the held object). Real walls/floors are static hknp
                // bodies and always come through Phase-2 with a readable filter.
                {
                    std::uint32_t bodyIdRaw = 0;
                    std::memcpy(&bodyIdRaw, &r.bodyId, sizeof(bodyIdRaw));
                    std::uint32_t filter = 0;
                    if (!hknpWorld || !heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, bodyIdRaw, filter)) {
                        continue;  // Phase-1 refr hit (fake normal) — not a wall
                    }
                    // Only STATIC WORLD (walls/floors) pushes the hand — never the player's own
                    // body/char-proxy (which was the constant-jitter source), actors, clutter, held
                    // objects, or our own colliders (43/44/47, also non-static so excluded here).
                    if (!isStaticWorldLayer(filter & 0x7Fu)) {
                        continue;
                    }
                }
                const float dist = r.hitFraction * probeRadius;
                const float depth = probeRadius - dist;
                if (depth > bestDepth) {
                    bestDepth = depth;
                    // hitNormal points away from the surface; push the hand along it (out of the wall).
                    outNormal = r.hitNormal;
                    any = true;
                }
            }
            outDepth = bestDepth;
            return any && bestDepth > 0.01f;
        }

        void UpdateHand(bool isLeft, f4cf::f4vr::PlayerNodes* playerNodes, void* hknpWorld, float dt)
        {
            auto& st = g_state[isLeft ? 0 : 1];
            auto& frik = heisenberg::FRIKInterface::GetSingleton();

            RE::NiNode* wand = heisenberg::GetWandNode(playerNodes, isLeft);
            if (!wand) { st.overriding = false; st.haveWand = false; return; }
            const RE::NiPoint3 wandPos = wand->world.translate;

            // Track the raw (controller-driven) hand position independent of our override:
            //  - not overriding: read FRIK's true hand transform.
            //  - overriding: integrate the wand-node delta onto the tracked raw position
            //    (controller motion maps 1:1 to hand motion), so we never probe the
            //    pushed-back transform → no feedback oscillation.
            if (!st.overriding) {
                RE::NiTransform handWorld;
                if (frik.GetHandWorldTransform(isLeft, handWorld)) {
                    st.rawHandPos = handWorld.translate;
                    st.handRot = handWorld.rotate;
                    st.handScale = handWorld.scale > 0.0f ? handWorld.scale : 1.0f;
                    st.haveRot = true;
                    // Capture the hand's rotation offset from the wand (row-vector: hand = offset * wand)
                    // so we can reconstruct a LIVE hand rotation from the controller while overriding.
                    st.handRotOffsetFromWand = handWorld.rotate * wand->world.rotate.Transpose();
                    st.haveRotOffset = true;
                } else {
                    st.rawHandPos = wandPos;  // fallback
                }
            } else if (st.haveWand) {
                st.rawHandPos = vadd(st.rawHandPos, vsub(wandPos, st.lastWandPos));
                // WRIST-SNAP FIX (Jul 5): refresh the applied hand rotation to track the LIVE
                // controller while pushed back, instead of freezing the rotation from override
                // start. A frozen rotation forces the wrist to absorb the mismatch as the user
                // turns their wrist during the pushback -> visible wrist bend/snap.
                if (st.haveRotOffset) {
                    st.handRot = st.handRotOffsetFromWand * wand->world.rotate;
                }
            }
            st.lastWandPos = wandPos;
            st.haveWand = true;

            const float probeRadius = (std::max)(1.0f, heisenberg::g_config.handPushbackProbeRadius);
            float depth = 0.0f;
            RE::NiPoint3 normal{ 0, 0, 1 };
            // Ownership gate for modes 1/2 (mode 3 has its own below): while this hand is
            // GRABBING, the grab system owns the hand pose — and the held object itself sits
            // right at the hand, so probing would false-hit it constantly (esp. the Phase-1
            // refr-sphere path). Skipping also saves 6 raycasts/hand on the heaviest frames.
            const bool grabbingThisHand = heisenberg::GrabManager::GetSingleton().IsGrabbing(isLeft);
            const bool penetrating = !grabbingThisHand
                && ProbeHand(st.rawHandPos, hknpWorld, probeRadius, depth, normal);

            // Target correction for this frame. Modes 1/2 are world-only; mode 3 computes the
            // full multi-target candidate below (so it owns targetCorr entirely).
            RE::NiPoint3 targetCorr{ 0, 0, 0 };
            if (penetrating && heisenberg::g_config.handWallPushbackMode != 3) {
                const float maxPush = (std::max)(0.0f, heisenberg::g_config.handPushbackMaxPush);
                if (heisenberg::g_config.handWallPushbackMode == 2) {
                    // ROCK soft-contact: compliant ramp -> hard stop.
                    const RE::NiPoint3 hardStopCorr = projectTrackedMagnetCorrection(normal, depth, maxPush);
                    const float scale = compliantHardStopResponseScale(
                        depth, heisenberg::g_config.handPushbackScale, heisenberg::g_config.handPushbackHardStop);
                    targetCorr = vmul(hardStopCorr, scale);
                } else {
                    // Mode 1: linear depenetration.
                    targetCorr = projectTrackedMagnetCorrection(normal, depth, maxPush);
                }
            }

            // Mode 3 — full multi-target soft contact (ROCK SoftContactRuntime::solveForHand,
            // hand-ported): world + hand-vs-hand + hand-vs-body, with a priority winner and a
            // per-kind correction clamp, gated by grab ownership. Capsules come from the
            // rendered skeleton, so while overriding this can feed back — opt-in + needs
            // in-game tuning (fHandPushbackHandRadiusPadding / fHandPushbackMaxPush).
            if (heisenberg::g_config.handWallPushbackMode == 3) {
                // Ownership gate: suppress generated hand contact while this hand is grabbing
                // (the grab system owns the hand pose; fighting it produces jitter).
                if (heisenberg::GrabManager::GetSingleton().IsGrabbing(isLeft)) {
                    targetCorr = { 0, 0, 0 };
                } else {
                    const int me = isLeft ? 0 : 1;
                    const int other = isLeft ? 1 : 0;
                    const float pad = (std::max)(0.0f, heisenberg::g_config.handPushbackHandRadiusPadding);

                    // ROCK SoftContactRuntime candidate model (1:1):
                    //  - per-kind ABSOLUTE clamps: world=18, hand/body=7, weapon=3 (config, ROCK stock)
                    //  - weapon-hand correction is COMPLIANT (scale 0.35 ramping to full at 4u penetration)
                    //  - winner: kind priority (World=20 > others=10) -> source priority
                    //    (NativeWorld=30 > QueryWorld/raycast=10 > Shape=0) -> stronger RESPONSE
                    //    length (then penetration) via preferStrongerContactResponse.
                    enum class CKind : int { HandHand, Body, Weapon, World };
                    auto kindPriority = [](CKind k) { return k == CKind::World ? 20 : 10; };
                    auto maxForKind = [&](CKind k) {
                        switch (k) {
                        case CKind::World:  return scm::sanitizePositive(heisenberg::g_config.softContactWorldMaxCorrection, 18.0f);
                        case CKind::Weapon: return scm::sanitizePositive(heisenberg::g_config.softContactWeaponMaxCorrection, 3.0f);
                        default:            return scm::sanitizePositive(heisenberg::g_config.softContactMaxCorrection, 7.0f);
                        }
                    };
                    auto correctionFor = [&](CKind k, const RE::NiPoint3& nrm, float pen) {
                        const RE::NiPoint3 hardStop = scm::projectTrackedMagnetCorrection(nrm, pen, maxForKind(k));
                        if (k == CKind::Weapon) {
                            return scm::mul(hardStop, scm::compliantHardStopResponseScale(
                                pen, heisenberg::g_config.softContactWeaponCorrectionScale,
                                heisenberg::g_config.softContactWeaponHardStop));
                        }
                        return hardStop;
                    };

                    bool  bestValid = false;
                    CKind bestKind  = CKind::HandHand;
                    int   bestSrc   = 0;
                    float bestPen   = 0.0f;
                    RE::NiPoint3 bestN{ 0, 0, 1 };
                    auto consider = [&](float pen, const RE::NiPoint3& nrm, CKind kind, int srcPrio) {
                        if (pen <= 0.0f) return;
                        if (!bestValid) {
                            bestValid = true; bestKind = kind; bestSrc = srcPrio; bestPen = pen; bestN = nrm;
                            return;
                        }
                        const int bp = kindPriority(bestKind), cp = kindPriority(kind);
                        if (cp > bp ||
                            (cp == bp && (srcPrio > bestSrc ||
                             (srcPrio == bestSrc && scm::preferStrongerContactResponse(
                                 scm::length(correctionFor(kind, nrm, pen)), pen,
                                 scm::length(correctionFor(bestKind, bestN, bestPen)), bestPen))))) {
                            bestKind = kind; bestSrc = srcPrio; bestPen = pen; bestN = nrm;
                        }
                    };

                    // hand-vs-world (the 6-axis probe above) — ROCK source class QueryWorld.
                    if (penetrating && heisenberg::g_config.softContactWorld) {
                        consider(depth, normal, CKind::World, 10);
                    }

                    // hand-vs-world (NATIVE Havok manifold evidence). When physics hand bodies
                    // are active they generate real contacts more accurate than the raycast;
                    // each fresh penetrating contact for THIS hand becomes a world candidate.
                    // Skips the player's own hand bodies (layers 43/47 — handled by the capsule
                    // solves) so we don't double-count or fight ourselves.
                    if (heisenberg::g_config.softContactWorld) {
                        for (std::uint32_t r = 0; r < g_nativeSnapshot.count; ++r) {
                            const auto& rec = g_nativeSnapshot.records[r];
                            if (!rec.valid || rec.sourceIsLeft != isLeft) continue;
                            // Need a real contact point + the manifold's own positive depth.
                            // Normal-only (reused-jacobian) records carry no usable depth here.
                            if (!rec.hasContactPoint || !(rec.penetrationGame > 0.0f)) continue;
                            if (!heisenberg::rock_core::contact_evidence::isFrameFresh(g_nativeSnapshot.currentFrame, rec.frame, 2)) continue;
                            std::uint32_t targetFilter = 0;
                            if (hknpWorld && heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, rec.targetBodyId, targetFilter)) {
                                const std::uint32_t layer = targetFilter & 0x7Fu;
                                if (layer == 43 || layer == 47) continue;  // our own hand bodies
                            }
                            // Orient the surface normal toward the hand (away from the surface) so
                            // the correction depenetrates regardless of the manifold's sign.
                            RE::NiPoint3 n = rec.contactNormalGame;
                            if (scm::dot(scm::sub(st.rawHandPos, rec.contactPointGame), n) < 0.0f) {
                                n = scm::negate(n);
                            }
                            // Use the manifold's OWN penetration depth — NOT a raycast-radius
                            // point-plane test (rawHandPos is the palm, not the contact body's
                            // centre, so that test systematically mismeasures). ROCK source
                            // class NativeWorld — outranks the raycast on equal kind.
                            consider(rec.penetrationGame, n, CKind::World, 30);
                        }
                    }

                    // The Build*Capsule builders write each capsule at its DESCRIPTOR index and
                    // skip bones they cannot resolve, leaving gaps — so iterate the full fixed-size
                    // arrays and filter on .valid (matching ROCK), NOT 0..count which would drop
                    // valid capsules past a gap on a partially-resolved skeleton.
                    constexpr int kHandCaps = heisenberg::rock_hand_collider::kCapsulesPerHand;
                    constexpr int kBodyCaps = heisenberg::rock_body_collider::kCapsuleCount;

                    // hand-vs-hand — this hand's finger capsules vs the other hand's.
                    if (heisenberg::g_config.softContactHandHand) {
                        for (int i = 0; i < kHandCaps; ++i) {
                            if (!g_handCaps[me][i].valid) continue;
                            for (int j = 0; j < kHandCaps; ++j) {
                                if (!g_handCaps[other][j].valid) continue;
                                const auto cc = scm::solveCapsulePair(g_handCaps[me][i], g_handCaps[other][j], bestN, pad);
                                if (cc.active) consider(cc.penetration, cc.normal, CKind::HandHand, 0);
                            }
                        }
                    }

                    // hand-vs-body — vs the avatar capsules, skipping this hand's own arm.
                    // (g_bodyCapCount is 0 when softContactBody is off → loop body never runs.)
                    if (g_bodyCapCount > 0) {
                        for (int i = 0; i < kHandCaps; ++i) {
                            if (!g_handCaps[me][i].valid) continue;
                            for (int j = 0; j < kBodyCaps; ++j) {
                                if (!g_bodyCaps[j].valid) continue;
                                if (heisenberg::rock_body_collider::isSameSideArmCapsuleId(g_bodyCaps[j].id, isLeft)) continue;
                                const auto cc = scm::solveCapsulePair(g_handCaps[me][i], g_bodyCaps[j], bestN, pad);
                                if (cc.active) consider(cc.penetration, cc.normal, CKind::Body, 0);
                            }
                        }
                    }

                    // hand-vs-weapon — vs the weapon held by the OTHER hand (e.g. the support
                    // hand touching the rifle). The hand holding the weapon grips it, so its own
                    // weapon is skipped. Weapon-hand uses ROCK's weapon-specific padding (0.25)
                    // and the compliant clamp (3u, scale 0.35, hard stop 4u) via correctionFor.
                    if (g_weaponCapValid[other]) {
                        const float weaponPad = scm::sanitizeNonNegative(heisenberg::g_config.softContactWeaponRadiusPadding, 0.25f);
                        for (int i = 0; i < kHandCaps; ++i) {
                            if (!g_handCaps[me][i].valid) continue;
                            const auto cc = scm::solveCapsulePair(g_handCaps[me][i], g_weaponCaps[other], bestN, weaponPad);
                            if (cc.active) consider(cc.penetration, cc.normal, CKind::Weapon, 0);
                        }
                    }

                    targetCorr = bestValid ? correctionFor(bestKind, bestN, bestPen)
                                           : RE::NiPoint3{ 0, 0, 0 };
                }
            }

            // INSTRUMENTATION (Jul 5): diag values populated by the weapon-tip block below,
            // read by the sampled [HWP-DIAG] log after the apply/clear decision.
            float dbgWeaponDepth = 0.0f;
            RE::NiPoint3 dbgWeaponNormal{ 0, 0, 1 };
            bool dbgWeaponBuilt = false;

            // --- WEAPON-TIP PUSHBACK (modes 1/2): probe the equipped weapon's capsule; on
            // penetration push the HAND back along the wall normal (the weapon is welded to
            // the hand, so retracting the hand pulls the barrel out of the wall). Mode 3 has
            // its own weapon channel; grabbing hands are already gated above.
            if (heisenberg::g_config.weaponWallPushback
                && heisenberg::g_config.handWallPushbackMode != 3
                && !grabbingThisHand)
            {
                scm::Capsule cap;
                if (heisenberg::rock_weapon_collision::BuildWeaponCapsule(isLeft, cap)) {
                    dbgWeaponBuilt = true;
                    // The capsule is built from the RENDERED weapon node. While overriding, the
                    // rendered hand (and welded weapon) already sits at raw+smoothedCorr, so
                    // subtract our own correction to probe RAW space (no feedback oscillation —
                    // same principle as the wand-delta rawHandPos integration above). Exact
                    // because the override is translation-only relative to raw (rotation is
                    // live-tracked = same as raw).
                    if (st.overriding) {
                        cap.start = vsub(cap.start, st.smoothedCorr);
                        cap.end = vsub(cap.end, st.smoothedCorr);
                    }
                    // Muzzle = capsule end farther from the hand (axis direction grip/tip agnostic).
                    const bool endIsTip = vlen(vsub(cap.end, st.rawHandPos)) >= vlen(vsub(cap.start, st.rawHandPos));
                    const RE::NiPoint3 tip = endIsTip ? cap.end : cap.start;
                    const RE::NiPoint3 mid = vmul(vadd(cap.start, cap.end), 0.5f);
                    const float wr = (std::max)(2.0f, cap.radius + 1.0f);  // probe radius = hull + margin

                    float wDepth = 0.0f;
                    RE::NiPoint3 wNormal{ 0, 0, 1 };
                    for (const RE::NiPoint3& p : { tip, mid }) {
                        float d = 0.0f;
                        RE::NiPoint3 n{ 0, 0, 1 };
                        if (ProbeHand(p, hknpWorld, wr, d, n) && d > wDepth) { wDepth = d; wNormal = n; }
                    }
                    dbgWeaponDepth = wDepth;
                    dbgWeaponNormal = wNormal;
                    if (wDepth > 0.0f) {
                        const float maxW = (std::max)(0.0f, heisenberg::g_config.weaponPushbackMaxPush);
                        RE::NiPoint3 wCorr = projectTrackedMagnetCorrection(wNormal, wDepth, maxW);
                        if (heisenberg::g_config.handWallPushbackMode == 2) {
                            wCorr = vmul(wCorr, compliantHardStopResponseScale(
                                wDepth, heisenberg::g_config.handPushbackScale, heisenberg::g_config.handPushbackHardStop));
                        }
                        // Single winner vs the hand probe (ROCK-style arbitration) — never sum,
                        // or a hand+barrel double hit on the same wall over-corrects to 2x depth.
                        if (vlen(wCorr) > vlen(targetCorr)) targetCorr = wCorr;
                    }
                }
            }

            // Smooth toward the target correction (exponential, frame-rate independent).
            const float speed = (std::max)(0.0f, heisenberg::g_config.handPushbackSmoothing);
            float alpha = (dt > 0.0f) ? (1.0f - std::exp(-speed * dt)) : 1.0f;
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            st.smoothedCorr = vadd(st.smoothedCorr, vmul(vsub(targetCorr, st.smoothedCorr), alpha));

            const bool wasOverriding = st.overriding;
            if (vlen(st.smoothedCorr) > 0.05f && st.haveRot) {
                RE::NiTransform target;
                target.rotate = st.handRot;
                target.translate = vadd(st.rawHandPos, st.smoothedCorr);
                target.scale = st.handScale;  // preserve FRIK hand scale (was 1.0f — popped scaled hands during pushback)
                frik.ApplyExternalHandWorldTransform(isLeft, target, 100);
                st.overriding = true;
            } else if (st.overriding) {
                frik.ClearExternalHandWorldTransform(isLeft);
                st.overriding = false;
                st.smoothedCorr = { 0, 0, 0 };
            }

            // INSTRUMENTATION (Jul 5): reveal the weapon-hand jitter oscillator — which channel
            // writes the priority-100 FRIK hand override each frame, and by how much. Sampled
            // ~6 Hz per hand; only when a correction is active or the override toggled, so a
            // still, un-pushed hand stays quiet. Read from HeisenbergF4VR.log next session.
            if (spdlog::should_log(spdlog::level::debug)) {
                const bool toggled = (wasOverriding != st.overriding);
                const bool active = vlen(st.smoothedCorr) > 0.001f || vlen(targetCorr) > 0.001f;
                static int s_diagCounter[2] = { 0, 0 };
                const int di = isLeft ? 0 : 1;
                if ((active || toggled) && (((++s_diagCounter[di]) % 15) == 0 || toggled)) {
                    spdlog::debug(
                        "[HWP-DIAG] {} mode={} grab={} handHit={} handDepth={:.2f} handN=({:.2f},{:.2f},{:.2f}) "
                        "wpnBuilt={} wpnDepth={:.2f} wpnN=({:.2f},{:.2f},{:.2f}) targetCorr={:.2f} smoothed={:.2f} override={}{}",
                        isLeft ? "L" : "R",
                        heisenberg::g_config.handWallPushbackMode,
                        grabbingThisHand,
                        penetrating, (penetrating ? depth : 0.0f), normal.x, normal.y, normal.z,
                        dbgWeaponBuilt, dbgWeaponDepth, dbgWeaponNormal.x, dbgWeaponNormal.y, dbgWeaponNormal.z,
                        vlen(targetCorr), vlen(st.smoothedCorr), st.overriding,
                        toggled ? " [TOGGLE]" : "");
                }
            }
        }
    }

    void Update()
    {
        // Single latch shared by both branches. Two same-named function-local statics here
        // used to shadow each other: the mode-0 cleanup copy was never set true by the active
        // path, so a live mode>0 -> 0 config switch (MCM hot-reload) left the last priority-100
        // FRIK hand override latched — fighting ROCK soft-contact (max 99) until a loading
        // screen. Same shadowed-static bug class as the Hooks.cpp fix.
        static bool s_wasActive = false;
        if (heisenberg::g_config.handWallPushbackMode == 0) {
            // Ensure any leftover overrides are released exactly once.
            if (s_wasActive) { Reset(); s_wasActive = false; }
            return;
        }
        s_wasActive = true;

        // Pushback moves the rendered hand. Native FRIK v5 does it via applyExternalHandWorldTransform;
        // on OLDER FRIK our plugin-side HandAuthority does it instead — so run whenever EITHER is
        // available (ApplyExternalHandWorldTransform routes to the right one).
        if (!heisenberg::FRIKInterface::GetSingleton().SupportsPushback()
            && !heisenberg::HandAuthority::Available()) {
            static bool warned = false;
            if (!warned) {
                spdlog::warn("[HandWallPushback] enabled (mode={}) but neither FRIK v5 nor plugin-side hand authority is available — pushback disabled.",
                             heisenberg::g_config.handWallPushbackMode);
                warned = true;
            }
            return;
        }

        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes) return;

        // hknp world for the self-hit filter (read hit-body layer). Null = filter off (fail open).
        void* hknpWorld = nullptr;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (player->parentCell) {
                if (auto* bhk = player->parentCell->GetbhkWorld()) {
                    hknpWorld = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
                }
            }
        }

        const double now = Utils::GetTime();
        float dt = (g_lastTime > 0.0) ? static_cast<float>(now - g_lastTime) : (1.0f / 90.0f);
        if (dt <= 0.0001f || dt > 0.1f) dt = 1.0f / 90.0f;
        g_lastTime = now;

        // Mode 3: build both hands' finger capsules + the body capsules once per frame (from
        // the FRIK skeleton) so each hand's solve can test against the other hand / the body.
        if (heisenberg::g_config.handWallPushbackMode == 3) {
            g_handCapCount[0] = heisenberg::rock_hand_collider::BuildHandCapsules(true, g_handCaps[0]);
            g_handCapCount[1] = heisenberg::rock_hand_collider::BuildHandCapsules(false, g_handCaps[1]);
            g_bodyCapCount = heisenberg::g_config.softContactBody
                ? heisenberg::rock_body_collider::BuildBodyCapsules(g_bodyCaps) : 0;
            g_weaponCapValid[0] = heisenberg::g_config.softContactWeapon
                && heisenberg::rock_weapon_collision::BuildWeaponCapsule(true, g_weaponCaps[0]);
            g_weaponCapValid[1] = heisenberg::g_config.softContactWeapon
                && heisenberg::rock_weapon_collision::BuildWeaponCapsule(false, g_weaponCaps[1]);
        }

        // Refresh the native Havok contact evidence once per frame (advances the freshness
        // frame counter) so both hands consume the same snapshot below. Publish the recording
        // gate as an atomic the Havok worker-thread callback reads (instead of racing the plain
        // g_config int/bool fields).
        if (heisenberg::g_config.handWallPushbackMode == 3) {
            heisenberg::native_contact_evidence::SetRecordingEnabled(heisenberg::g_config.softContactWorld);
            heisenberg::native_contact_evidence::Snapshot(g_nativeSnapshot);
        } else {
            heisenberg::native_contact_evidence::SetRecordingEnabled(false);
        }

        UpdateHand(true, playerNodes, hknpWorld, dt);
        UpdateHand(false, playerNodes, hknpWorld, dt);
    }

    void Reset()
    {
        auto& frik = heisenberg::FRIKInterface::GetSingleton();
        for (int i = 0; i < 2; ++i) {
            if (g_state[i].overriding) {
                frik.ClearExternalHandWorldTransform(i == 0);
            }
            g_state[i] = HandState{};
        }
        g_lastTime = 0.0;
        // Stop worker-thread recording and drop stale native contact evidence (bodyIds are
        // world-specific).
        heisenberg::native_contact_evidence::SetRecordingEnabled(false);
        heisenberg::native_contact_evidence::Reset();
        g_nativeSnapshot = {};
    }
}
