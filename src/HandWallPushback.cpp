#include "HandWallPushback.h"

#include "Config.h"
#include "Physics.h"
#include "FRIKInterface.h"
#include "WandNodeHelper.h"
#include "Utils.h"

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
            RE::NiMatrix3 handRot{};            // captured rotation (kept while overriding)
            bool haveRot = false;
            RE::NiPoint3 lastWandPos{};
            bool haveWand = false;
            RE::NiPoint3 smoothedCorr{};        // smoothed correction vector
        };
        HandState g_state[2];  // [0]=left, [1]=right
        double g_lastTime = 0.0;

        // 6 axis probe directions.
        const RE::NiPoint3 kProbeDirs[6] = {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
        };

        constexpr std::uint32_t kOurColliderLayer = 43;  // hand/body/weapon collider layer

        // Probe geometry around `pos`. Returns true + deepest penetration (depth, normal).
        // SELF-HIT FILTER: skips hits on our own colliders (layer 43) — the physics hand
        // bodies (16/hand) + 23 body capsules surround the hand, so without this every ray
        // hits our own body → constant false "wall" → the rendered hand flickers/contorts.
        bool ProbeHand(const RE::NiPoint3& pos, void* hknpWorld, float probeRadius, float& outDepth, RE::NiPoint3& outNormal)
        {
            float bestDepth = 0.0f;
            bool any = false;
            for (const auto& dir : kProbeDirs) {
                auto r = heisenberg::Physics::CastRay(pos, dir, probeRadius, nullptr);
                if (!r.hit) continue;
                // Ignore our own keyframed colliders (layer 43). Real walls/floors are on
                // other (static) layers. If the filter can't be read, keep the hit (fail open).
                if (hknpWorld) {
                    std::uint32_t bodyIdRaw = 0;
                    std::memcpy(&bodyIdRaw, &r.bodyId, sizeof(bodyIdRaw));
                    std::uint32_t filter = 0;
                    if (heisenberg::Physics::TryReadBodyFilterInfo(hknpWorld, bodyIdRaw, filter)
                        && (filter & 0x7Fu) == kOurColliderLayer) {
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
                    st.haveRot = true;
                } else {
                    st.rawHandPos = wandPos;  // fallback
                }
            } else if (st.haveWand) {
                st.rawHandPos = vadd(st.rawHandPos, vsub(wandPos, st.lastWandPos));
            }
            st.lastWandPos = wandPos;
            st.haveWand = true;

            const float probeRadius = (std::max)(1.0f, heisenberg::g_config.handPushbackProbeRadius);
            float depth = 0.0f;
            RE::NiPoint3 normal{ 0, 0, 1 };
            const bool penetrating = ProbeHand(st.rawHandPos, hknpWorld, probeRadius, depth, normal);

            // Target correction for this frame.
            RE::NiPoint3 targetCorr{ 0, 0, 0 };
            if (penetrating) {
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

            // Smooth toward the target correction (exponential, frame-rate independent).
            const float speed = (std::max)(0.0f, heisenberg::g_config.handPushbackSmoothing);
            float alpha = (dt > 0.0f) ? (1.0f - std::exp(-speed * dt)) : 1.0f;
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            st.smoothedCorr = vadd(st.smoothedCorr, vmul(vsub(targetCorr, st.smoothedCorr), alpha));

            if (vlen(st.smoothedCorr) > 0.05f && st.haveRot) {
                RE::NiTransform target;
                target.rotate = st.handRot;
                target.translate = vadd(st.rawHandPos, st.smoothedCorr);
                target.scale = 1.0f;
                frik.ApplyExternalHandWorldTransform(isLeft, target, 100);
                st.overriding = true;
            } else if (st.overriding) {
                frik.ClearExternalHandWorldTransform(isLeft);
                st.overriding = false;
                st.smoothedCorr = { 0, 0, 0 };
            }
        }
    }

    void Update()
    {
        if (heisenberg::g_config.handWallPushbackMode == 0) {
            // Ensure any leftover overrides are released exactly once.
            static bool wasActive = false;
            if (wasActive) { Reset(); wasActive = false; }
            return;
        }
        static bool wasActive = true;
        wasActive = true;

        // Pushback uses the FRIK v9 external-hand-transform API. On older FRIK it's a no-op
        // (the rest of FRIK still works). Warn once so it's obvious why nothing happens.
        if (!heisenberg::FRIKInterface::GetSingleton().SupportsPushback()) {
            static bool warned = false;
            if (!warned) {
                spdlog::warn("[HandWallPushback] enabled (mode={}) but FRIK is older than v9 — pushback disabled. Install FRIK v9+.",
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
    }
}
