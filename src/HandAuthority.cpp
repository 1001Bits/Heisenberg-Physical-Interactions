#include "HandAuthority.h"
#include "WandNodeHelper.h"
#include "FrikArmGoalHook.h"
#include "../external/ROCK/src/ROCKMain.h"
#include "common/MatrixUtils.h"
#include "f4vr/F4VRUtils.h"

#include "Utils.h"                 // GetLocalTransformForWorldTransform
#include "Config.h"
#include "Heisenberg.h"   // IsRockEngineHosted
#include "f4vr/PlayerNodes.h"      // f4cf::f4vr::getCommonNode / getPlayer
#include "f4vr/F4VRUtils.h"        // f4cf::f4vr::findNode / updateTransformsDown

#include <RE/Fallout.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>

namespace heisenberg
{
    namespace
    {
        // ---- small vector/matrix helpers (self-contained; Grab.cpp's are file-static) ----
        inline float vlen(const RE::NiPoint3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
        inline float vdot(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        inline RE::NiPoint3 vsub(const RE::NiPoint3& a, const RE::NiPoint3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
        inline RE::NiPoint3 vadd(const RE::NiPoint3& a, const RE::NiPoint3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
        inline RE::NiPoint3 vmul(const RE::NiPoint3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
        inline RE::NiPoint3 vcross(const RE::NiPoint3& a, const RE::NiPoint3& b)
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        }
        inline RE::NiPoint3 vnorm(const RE::NiPoint3& v)
        {
            const float l = vlen(v);
            return (l > 1e-6f) ? RE::NiPoint3{ v.x / l, v.y / l, v.z / l } : RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
        }
        inline void setRow(RE::NiMatrix3& m, int r, float a, float b, float c)
        {
            m.entry[r][0] = a; m.entry[r][1] = b; m.entry[r][2] = c;
        }
        RE::NiMatrix3 identity3()
        {
            RE::NiMatrix3 m;
            setRow(m, 0, 1, 0, 0);
            setRow(m, 1, 0, 1, 0);
            setRow(m, 2, 0, 0, 1);
            return m;
        }
        // Rotation (row-vector storage) whose action rotates `from` onto `to`.
        RE::NiMatrix3 axisAngle(const RE::NiPoint3& axis, float angle)
        {
            const RE::NiPoint3 a = vnorm(axis);
            const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
            RE::NiMatrix3 m;
            setRow(m, 0, t * a.x * a.x + c,       t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y);
            setRow(m, 1, t * a.x * a.y + s * a.z, t * a.y * a.y + c,       t * a.y * a.z - s * a.x);
            setRow(m, 2, t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, t * a.z * a.z + c);
            return m;
        }
        RE::NiMatrix3 alignRotation(const RE::NiPoint3& fromV, const RE::NiPoint3& toV)
        {
            const RE::NiPoint3 from = vnorm(fromV), to = vnorm(toV);
            const float d = std::clamp(vdot(from, to), -1.0f, 1.0f);
            if (d > 0.9999f) return identity3();
            if (d < -0.9999f) {
                const RE::NiPoint3 arb = (std::abs(from.z) < 0.9f) ? RE::NiPoint3{ 0, 0, 1 } : RE::NiPoint3{ 0, 1, 0 };
                return axisAngle(vnorm(vcross(from, arb)), 3.14159265358979f);
            }
            return axisAngle(vnorm(vcross(from, to)), std::acos(d));
        }

        // ---- skinned hand + arm chain resolvers (rendered hand is under COM, not firstPersonSkeleton) ----
        RE::NiNode* skinnedHandNode(bool isLeft)
        {
            const char* fingerProbe = isLeft ? "LArm_Finger11" : "RArm_Finger11";
            const char* handName = isLeft ? "LArm_Hand" : "RArm_Hand";
            if (RE::NiNode* com = f4cf::f4vr::getCommonNode()) {
                if (RE::NiNode* finger = f4cf::f4vr::findNode(com, fingerProbe)) {
                    if (finger->parent) return finger->parent->IsNode();
                }
                if (RE::NiNode* hand = f4cf::f4vr::findNode(com, handName)) {
                    return hand;
                }
            }
            return nullptr;
        }

        struct ArmChain
        {
            RE::NiNode* upperArm = nullptr;  // [LR]Arm_UpperArm (shoulder joint origin)
            RE::NiNode* foreArm = nullptr;   // [LR]Arm_ForeArm1 (elbow joint origin)
            RE::NiNode* hand = nullptr;      // rendered hand (wrist, owns fingers)
            // FRIK-solver chain extension (Jul 19): the faithful setArms port needs the full
            // clavicle->forearm3 chain plus chest for its lift heuristics. forearm2/3 may be
            // null (power armor) — the port handles that exactly like FRIK does.
            RE::NiNode* shoulder = nullptr;  // [LR]Arm_Collarbone
            RE::NiNode* foreArm2 = nullptr;  // [LR]Arm_ForeArm2
            RE::NiNode* foreArm3 = nullptr;  // [LR]Arm_ForeArm3
            RE::NiNode* chest = nullptr;     // Chest
        };
        bool resolveArmChain(bool isLeft, ArmChain& out)
        {
            RE::NiNode* com = f4cf::f4vr::getCommonNode();
            if (!com) return false;
            out.upperArm = f4cf::f4vr::findNode(com, isLeft ? "LArm_UpperArm" : "RArm_UpperArm");
            out.foreArm = f4cf::f4vr::findNode(com, isLeft ? "LArm_ForeArm1" : "RArm_ForeArm1");
            out.hand = skinnedHandNode(isLeft);
            out.shoulder = f4cf::f4vr::findNode(com, isLeft ? "LArm_Collarbone" : "RArm_Collarbone");
            out.foreArm2 = f4cf::f4vr::findNode(com, isLeft ? "LArm_ForeArm2" : "RArm_ForeArm2");
            out.foreArm3 = f4cf::f4vr::findNode(com, isLeft ? "LArm_ForeArm3" : "RArm_ForeArm3");
            out.chest = f4cf::f4vr::findNode(com, "Chest");
            return out.upperArm && out.foreArm && out.hand;
        }

        constexpr float kArmReachSafetyMargin = 0.15f;

        constexpr float armReachProjectionScale(float requestedDistance, float maxReach)
        {
            return requestedDistance > maxReach && requestedDistance > 0.0f ?
                maxReach / requestedDistance : 1.0f;
        }

        static_assert(armReachProjectionScale(10.0f, 20.0f) == 1.0f);
        static_assert(armReachProjectionScale(20.0f, 20.0f) == 1.0f);
        static_assert(armReachProjectionScale(40.0f, 20.0f) == 0.5f);

        bool isFinitePoint(const RE::NiPoint3& point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
        }

        // Match the segment accounting used by FRIK's arm solve. In regular armor the
        // forearm is split across ForeArm2/3 plus the hand offset; power armor omits those
        // intermediate nodes. Local translation lengths are anatomical and do not depend on
        // the requested wrist target, unlike shoulder->target distance.
        float naturalArmReach(const ArmChain& arm)
        {
            if (!arm.upperArm || !arm.foreArm || !arm.hand) {
                return 0.0f;
            }

            const float upperLen = vlen(arm.foreArm->local.translate);
            const bool inPowerArmor = !arm.foreArm2 || !arm.foreArm3;
            const float forearmLen = inPowerArmor ?
                vlen(arm.hand->local.translate) :
                vlen(arm.foreArm2->local.translate) +
                    vlen(arm.foreArm3->local.translate) +
                    vlen(arm.hand->local.translate);
            const float worldScale = std::abs(arm.upperArm->world.scale);
            if (!std::isfinite(upperLen) || !std::isfinite(forearmLen) ||
                !std::isfinite(worldScale) || worldScale < 0.01f) {
                return 0.0f;
            }

            const float reach = (upperLen + forearmLen) * worldScale - kArmReachSafetyMargin;
            return std::isfinite(reach) && reach > 1.0f && reach < 200.0f ? reach : 0.0f;
        }

        bool getPlayerWorldPosition(RE::NiPoint3& out)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            out = player->data.location;
            return isFinitePoint(out);
        }

        bool constrainTargetToArmReach(const ArmChain& arm, RE::NiTransform& target,
            float* requestedDistance, float* maxReach)
        {
            if (!arm.upperArm || !isFinitePoint(arm.upperArm->world.translate) ||
                !isFinitePoint(target.translate)) {
                return false;
            }

            const float reach = naturalArmReach(arm);
            if (reach <= 0.0f) {
                return false;
            }

            const RE::NiPoint3 shoulderToTarget = vsub(target.translate, arm.upperArm->world.translate);
            const float requested = vlen(shoulderToTarget);
            if (!std::isfinite(requested)) {
                return false;
            }
            if (requestedDistance) {
                *requestedDistance = requested;
            }
            if (maxReach) {
                *maxReach = reach;
            }

            if (requested > reach && requested > 1e-4f) {
                target.translate = vadd(arm.upperArm->world.translate,
                    vmul(shoulderToTarget, armReachProjectionScale(requested, reach)));
            }
            return true;
        }

        // Is `target` reachable in `liveRoot`'s subtree? Pointer-compare only, never deref target
        // (stale-node CTD guard). Depth-bounded.
        bool inLiveSubtree(const RE::NiAVObject* target, RE::NiAVObject* liveRoot, int depth = 0)
        {
            if (!target || !liveRoot) return false;
            if (liveRoot == target) return true;
            if (depth > 20) return false;
            if (auto* node = liveRoot->IsNode()) {
                for (const auto& child : node->children) {
                    if (child && inLiveSubtree(target, child.get(), depth + 1)) return true;
                }
            }
            return false;
        }

        // ---- registry ----
        struct Writer
        {
            char tag[32] = {};
            int priority = 0;
            RE::NiTransform world;
            std::uint32_t lastFrame = 0;
            bool active = false;
        };
        constexpr int kMaxWriters = 8;
        constexpr std::uint32_t kStaleFrames = 3;

        struct HandSlot
        {
            std::array<Writer, kMaxWriters> writers;
            bool overriding = false;
            RE::NiTransform lastApplied;
            // FRIK-GOAL latch: current winning target for the goal-swap seam.
            bool hasLatched = false;
            RE::NiTransform latched{};
            // ROCK finishes after FRIK, so this target is consumed one frame later. Preserve
            // its exact solved rotation/source frame and transport only by player locomotion.
            bool latchedTracksPlayer = false;
            // True only for the full two-hand tag whose complete weapon was reach-limited
            // upstream. Other ROCK_Weapon targets still need the legacy hand safety.
            bool latchedReachLimitedRigid = false;
            bool latchedPlayerPositionValid = false;
            RE::NiPoint3 latchedPlayerPosition{};
        };

        HandSlot g_hands[2];  // [0]=Right (isLeft=false), [1]=Left
        std::mutex g_mtx;
        std::uint32_t g_frameCounter = 1;
        bool g_registered = false;

        inline int slotIdx(bool isLeft) { return isLeft ? 1 : 0; }

        // SEH-safe raw scene-graph write (no destructor-bearing locals inside __try).
        bool sehWriteNodeLocal(RE::NiNode* node, const RE::NiTransform& localXform)
        {
            __try {
                node->local = localXform;
                f4cf::f4vr::updateTransformsDown(node, true);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Place `node` at world transform T (row-vector inverse + propagate to children).
        bool placeNodeAtWorld(RE::NiNode* node, const RE::NiTransform& worldT)
        {
            const RE::NiTransform local = heisenberg::Utils::GetLocalTransformForWorldTransform(node, worldT);
            return sehWriteNodeLocal(node, local);
        }

        // Analytic 2-bone IK: aim upperArm+foreArm so the wrist reaches worldT.translate, bending in
        // FRIK's current elbow plane. Then place the hand at the full worldT for wrist orientation.
        // ==================================================================================
        // FRIK setArms PORT (Jul 19) — the faithful v5-equivalent applied to the SKINNED arm.
        //
        // Verbatim port of Skeleton::setArms (public FRIK v0.77, tag a2cae558) from the point
        // AFTER Update1StPersonArm, with handPos/handRot = the authority target instead of the
        // controller-driven fp hand. Runs post-FRIK on the skinned chain only — nothing reads
        // skinned arm bones as input, so no feedback loop is possible (the goal-node seam fed
        // the fp skeleton, which ROCK reads back -> hands flew to the floor with the gun).
        //
        // Episode base: FRIK's solver composes corrective deltas onto the CURRENT locals and
        // multiplies local translations by a forearm ratio; running it repeatedly from its own
        // output would compound. We snapshot the chain locals at the first apply of an authority
        // episode (a clean controller-solved anatomical pose) and restore them before every
        // solve — the port of v5's restoreArmNodesToDefault.
        // ==================================================================================
        struct ArmEpisodeBase
        {
            bool valid = false;
            RE::NiTransform shoulder{}, upper{}, forearm1{}, forearm2{}, forearm3{}, hand{};
        };
        ArmEpisodeBase g_armBase[2];
        std::array<float, 2> g_frikPrevTwist = { 0.0f, 0.0f };

        void captureArmBase(const ArmChain& arm, ArmEpisodeBase& base)
        {
            base.shoulder = arm.shoulder->local;
            base.upper = arm.upperArm->local;
            base.forearm1 = arm.foreArm->local;
            if (arm.foreArm2) base.forearm2 = arm.foreArm2->local;
            if (arm.foreArm3) base.forearm3 = arm.foreArm3->local;
            base.hand = arm.hand->local;
            base.valid = true;
        }

        void restoreArmBase(const ArmChain& arm, const ArmEpisodeBase& base)
        {
            arm.shoulder->local = base.shoulder;
            arm.upperArm->local = base.upper;
            arm.foreArm->local = base.forearm1;
            if (arm.foreArm2) arm.foreArm2->local = base.forearm2;
            if (arm.foreArm3) arm.foreArm3->local = base.forearm3;
            arm.hand->local = base.hand;
            f4cf::f4vr::updateTransformsDown(arm.shoulder, true);
        }

        struct FrikSolveDiag { float hsLen = 0, reach = 0; bool stretched = false; };
        FrikSolveDiag g_frikDiag[2];

        bool solveArmFrik(const ArmChain& arm, bool isLeft, const RE::NiTransform& worldT)
        {
            using MU = f4cf::common::MatrixUtils;
            if (!arm.shoulder || !arm.chest) {
                spdlog::warn("[FRIK-SOLVER] chain incomplete: shoulder={} chest={}", (void*)arm.shoulder, (void*)arm.chest);
                return false;
            }

            RE::NiPoint3 handPos = worldT.translate;
            const RE::NiMatrix3 handRot = worldT.rotate;

            if (std::isnan(handPos.x) || std::isnan(handPos.y) || std::isnan(handPos.z) ||
                std::isinf(handPos.x) || std::isinf(handPos.y) || std::isinf(handPos.z) ||
                MU::vec3Len(arm.upperArm->world.translate - handPos) > 200.0f) {
                static std::uint32_t s_r1 = 0;
                if ((++s_r1 % 30) == 0) spdlog::debug("[FRIK-SOLVER] abort: target invalid/far ({:.0f}gu)", MU::vec3Len(arm.upperArm->world.translate - handPos));
                return false;
            }

            // FRIK defaults: g_config.armLength = 36.74 -> adjustedArmLength = 1.0.
            constexpr float kArmLength = 36.74f;
            const float adjustedArmLength = 1.0f;

            // Body direction context (FRIK derives these in its body pass): forward = the
            // skinned chest facing projected to XY; sideways = right of forward. These shape
            // elbow-direction heuristics only.
            RE::NiPoint3 fwd3 = arm.chest->world.rotate * RE::NiPoint3(0, 1, 0);
            RE::NiPoint3 forwardDir = MU::vec3Norm(RE::NiPoint3(fwd3.x, fwd3.y, 0.0f));
            if (MU::vec3Len(forwardDir) < 0.001f) forwardDir = RE::NiPoint3(0, 1, 0);
            const float negLeft = isLeft ? -1.0f : 1.0f;
            RE::NiPoint3 sidewaysDir = MU::vec3Norm(MU::rotateXY(forwardDir, MU::degreesToRads(-90.0f)) * negLeft);

            // ---- shoulder IK (verbatim) ----
            RE::NiPoint3 shoulderToHand = handPos - arm.upperArm->world.translate;
            const float armLength = kArmLength;
            const float adjustAmount = (std::clamp)(MU::vec3Len(shoulderToHand) - armLength * 0.5f, 0.0f, armLength * 0.85f) / (armLength * 0.85f);
            const RE::NiPoint3 shoulderOffset = MU::vec3Norm(shoulderToHand) * (adjustAmount * armLength * 0.08f);

            const RE::NiPoint3 clavicalToNewShoulder = arm.upperArm->world.translate + shoulderOffset - arm.shoulder->world.translate;
            const RE::NiPoint3 sLocalDir = arm.shoulder->world.rotate * (clavicalToNewShoulder / arm.shoulder->world.scale);
            arm.shoulder->local.rotate = MU::getMatrixFromRotateVectorVec(sLocalDir, RE::NiPoint3(1, 0, 0)) * arm.shoulder->local.rotate;
            f4cf::f4vr::updateTransformsDown(arm.shoulder, true);

            // ---- bone lengths (verbatim; power-armor branch = missing forearm2/3) ----
            const bool inPowerArmor = !arm.foreArm2 || !arm.foreArm3;
            const float originalUpperLen = MU::vec3Len(arm.foreArm->local.translate);
            const float originalForearmLen = inPowerArmor
                ? MU::vec3Len(arm.hand->local.translate)
                : MU::vec3Len(arm.hand->local.translate) + MU::vec3Len(arm.foreArm2->local.translate) + MU::vec3Len(arm.foreArm3->local.translate);
            float upperLen = originalUpperLen * adjustedArmLength;
            float forearmLen = originalForearmLen * adjustedArmLength;

            const RE::NiPoint3 Uwp = arm.upperArm->world.translate;
            const float armReach = upperLen + forearmLen;
            RE::NiPoint3 shoulderToRequestedHand = handPos - Uwp;
            const float requestedHsLen = (std::max)(MU::vec3Len(shoulderToRequestedHand), 0.1f);

            if (requestedHsLen > armReach * 2.25f) {
                static std::uint32_t s_r2 = 0;
                if ((++s_r2 % 30) == 0) spdlog::debug("[FRIK-SOLVER] abort: hsLen={:.1f} > 2.25x reach {:.1f}", requestedHsLen, armReach);
                return false;
            }
            g_frikDiag[isLeft ? 1 : 0] = { requestedHsLen, armReach, requestedHsLen > armReach };

            // Do not reproduce FRIK's bone-length extension here. It makes the wrist reach an
            // arbitrary weapon target by lengthening both arm segments (the visible 1.6x-long
            // arm in the Jul 19 log). Project only the visual wrist onto the reachable sphere;
            // weapon aim and wrist orientation stay untouched.
            const float reachableHsLen = (std::max)(armReach - kArmReachSafetyMargin, 0.1f);
            if (requestedHsLen > reachableHsLen) {
                shoulderToRequestedHand = MU::vec3Norm(shoulderToRequestedHand) * reachableHsLen;
                handPos = Uwp + shoulderToRequestedHand;
            }
            const RE::NiPoint3 handToShoulder = Uwp - handPos;
            const float hsLen = (std::max)(MU::vec3Len(handToShoulder), 0.1f);

            // ---- wrist twist estimation + limits (verbatim) ----
            const RE::NiPoint3 handBack = handRot.Transpose() * RE::NiPoint3(-1, 0, 0);
            float twistAngle = std::asin((std::clamp)(handBack.z, -0.999f, 0.999f));
            const RE::NiPoint3 handSide = handRot.Transpose() * RE::NiPoint3(0, -1, 0);
            const RE::NiPoint3 handInSide = handSide * negLeft;
            const float twistAngle2 = -1.0f * std::asin((std::clamp)(handSide.z, -0.599f, 0.999f));
            const float interpTwist = (std::clamp)((handBack.z + 0.866f) * 1.155f, 0.45f, 0.8f);
            twistAngle = twistAngle + interpTwist * (twistAngle2 - twistAngle);
            const int ti = slotIdx(isLeft);
            twistAngle = g_frikPrevTwist[ti] + (twistAngle - g_frikPrevTwist[ti]) * 0.25f;
            g_frikPrevTwist[ti] = twistAngle;

            const float size = 1.0f;
            const float behindD = -(forwardDir.x * arm.shoulder->world.translate.x + forwardDir.y * arm.shoulder->world.translate.y) - 10.0f;
            const float handBehindDist = -(handPos.x * forwardDir.x + handPos.y * forwardDir.y + behindD);
            const float behindAmount = (std::clamp)(handBehindDist / (40.0f * size), 0.0f, 1.0f);

            const RE::NiPoint3 planeDir = MU::rotateXY(forwardDir, negLeft * MU::degreesToRads(135.0f));
            const float planeD = -(planeDir.x * arm.shoulder->world.translate.x + planeDir.y * arm.shoulder->world.translate.y) + 16.0f * size;
            const float armCrossAmount = (std::clamp)((handPos.x * planeDir.x + handPos.y * planeDir.y + planeD) / (20.0f * size), 0.0f, 1.0f);

            const float armLiftLimitZ = arm.chest->world.translate.z * size;
            const float armLiftThreshold = 60.0f * size;
            const float armLiftLimit = (std::clamp)((armLiftLimitZ + armLiftThreshold - handPos.z) / armLiftThreshold, 0.0f, 1.0f);
            const float upLimit = (std::clamp)((1.0f - armLiftLimit) * 1.4f, 0.0f, 1.0f);

            const float adjustMinAmount = (std::max)(behindAmount, (std::min)(armCrossAmount, armLiftLimit));
            const float twistMinAngle = MU::degreesToRads(-85.0f) + MU::degreesToRads(50.0f) * adjustMinAmount;
            const float twistMaxAngle = MU::degreesToRads(55.0f) - (std::max)(MU::degreesToRads(90.0f) * armCrossAmount, MU::degreesToRads(70.0f) * upLimit);
            const float twistLimitAngle = twistMinAngle + (twistAngle + 3.14159265f / 2.0f) / 3.14159265f * (twistMaxAngle - twistMinAngle);

            const RE::NiMatrix3 rot = MU::getRotationAxisAngle(sidewaysDir * negLeft, twistLimitAngle);
            const RE::NiPoint3 bendDownDir = rot.Transpose() * forwardDir;
            const RE::NiPoint3 xDir = MU::vec3Norm(handToShoulder);

            const float sideD = -(sidewaysDir.x * arm.shoulder->world.translate.x + sidewaysDir.y * arm.shoulder->world.translate.y) - 1.0f * 8.0f;
            float acrossAmount = -(handPos.x * sidewaysDir.x + handPos.y * sidewaysDir.y + sideD) / (16.0f * 1.0f);
            const float handSideTwistOutward = MU::vec3Dot(handSide, MU::vec3Norm(sidewaysDir + forwardDir * 0.5f));
            const float armTwist = (std::clamp)(handSideTwistOutward - (std::max)(0.0f, acrossAmount + 0.25f), 0.0f, 1.0f);
            if (acrossAmount < 0) {
                acrossAmount *= 0.2f;
            }

            const float handBehindHead = (std::clamp)((handBehindDist + 0.0f * size) / (15.0f * size), 0.0f, 1.0f) * (std::clamp)(upLimit * 1.2f, 0.0f, 1.0f);
            const float elbowsTwistForward = (std::max)(acrossAmount * MU::degreesToRads(90.0f), handBehindHead * MU::degreesToRads(120.0f));
            const RE::NiPoint3 elbowDir = MU::rotateXY(bendDownDir, -negLeft * (MU::degreesToRads(150.0f) - armTwist * MU::degreesToRads(25.0f) - elbowsTwistForward));
            RE::NiPoint3 yDir = elbowDir - xDir * MU::vec3Dot(elbowDir, xDir);
            yDir = MU::vec3Norm(yDir);

            // ---- elbow placement (law of cosines, verbatim incl. degenerate fallback) ----
            float wristAngle = std::acos((forearmLen * forearmLen + hsLen * hsLen - upperLen * upperLen) / (2.0f * forearmLen * hsLen));
            if (std::isnan(wristAngle) || std::isinf(wristAngle)) {
                forearmLen = upperLen = (originalUpperLen + originalForearmLen) / 2.0f * adjustedArmLength;
                wristAngle = std::acos((forearmLen * forearmLen + hsLen * hsLen - upperLen * upperLen) / (2.0f * forearmLen * hsLen));
            }
            const float xDist = std::cos(wristAngle) * forearmLen;
            const float yDist = std::sin(wristAngle) * forearmLen;
            const RE::NiPoint3 elbowWorld = handPos + xDir * xDist + yDir * yDist;

            // ---- bone rotations (verbatim) ----
            RE::NiMatrix3 Uwr = arm.upperArm->world.rotate;
            RE::NiPoint3 pos = elbowWorld - Uwp;
            const RE::NiPoint3 uLocalDir = Uwr * (MU::vec3Norm(pos) / arm.upperArm->world.scale);
            arm.upperArm->local.rotate = MU::getMatrixFromRotateVectorVec(uLocalDir, arm.foreArm->local.translate) * arm.upperArm->local.rotate;

            Uwr = arm.upperArm->local.rotate * arm.shoulder->world.rotate;

            pos = handPos - elbowWorld;
            RE::NiPoint3 uLocalTwist = Uwr * MU::vec3Norm(pos);
            uLocalTwist.x = 0;
            const RE::NiPoint3 upperSide = arm.upperArm->world.rotate.Transpose() * RE::NiPoint3(0, 1, 0);
            RE::NiPoint3 uloc = arm.shoulder->world.rotate * upperSide;
            uloc.x = 0;
            const float upperAngle = std::acos(MU::vec3Dot(MU::vec3Norm(uLocalTwist), MU::vec3Norm(uloc))) * (uLocalTwist.z > 0 ? 1.f : -1.f);

            arm.upperArm->local.rotate = MU::getMatrixFromEulerAngles(-upperAngle, 0, 0) * arm.upperArm->local.rotate;
            Uwr = arm.upperArm->local.rotate * arm.shoulder->world.rotate;

            arm.foreArm->local.rotate = MU::getMatrixFromEulerAngles(-upperAngle, 0, 0) * arm.foreArm->local.rotate;
            RE::NiMatrix3 Fwr = arm.foreArm->local.rotate * Uwr;
            const RE::NiPoint3 elbowHand = handPos - elbowWorld;
            const RE::NiPoint3 fLocalDir = Fwr * MU::vec3Norm(elbowHand);
            arm.foreArm->local.rotate = MU::getMatrixFromRotateVectorVec(fLocalDir, RE::NiPoint3(1, 0, 0)) * arm.foreArm->local.rotate;
            Fwr = arm.foreArm->local.rotate * Uwr;

            RE::NiMatrix3 Fwr3 = Fwr;
            if (!inPowerArmor) {
                RE::NiMatrix3 Fwr2 = arm.foreArm2->local.rotate * Fwr;
                Fwr3 = arm.foreArm3->local.rotate * Fwr2;

                RE::NiPoint3 wLocalDir = Fwr3 * MU::vec3Norm(handInSide);
                wLocalDir.x = 0;
                const RE::NiPoint3 forearm3Side = Fwr3.Transpose() * RE::NiPoint3(0, 0, -1);
                RE::NiPoint3 floc = Fwr2 * MU::vec3Norm(forearm3Side);
                floc.x = 0;
                const float fcos = MU::vec3Dot(MU::vec3Norm(wLocalDir), MU::vec3Norm(floc));
                const float fsin = MU::vec3Det(MU::vec3Norm(wLocalDir), MU::vec3Norm(floc), RE::NiPoint3(-1, 0, 0));
                const float forearmAngle = -1.0f * negLeft * std::atan2(fsin, fcos);

                arm.foreArm2->local.rotate = MU::getMatrixFromEulerAngles(negLeft * forearmAngle / 2, 0, 0) * arm.foreArm2->local.rotate;
                arm.foreArm3->local.rotate = MU::getMatrixFromEulerAngles(negLeft * forearmAngle / 2, 0, 0) * arm.foreArm3->local.rotate;

                Fwr2 = arm.foreArm2->local.rotate * Fwr;
                Fwr3 = arm.foreArm3->local.rotate * Fwr2;
            }

            arm.hand->local.rotate = handRot * (inPowerArmor ? Fwr : Fwr3).Transpose();

            arm.foreArm->local.translate = Uwr * ((elbowWorld - Uwp) / arm.upperArm->world.scale);

            const float origEHLen = MU::vec3Len(arm.hand->world.translate - arm.foreArm->world.translate);
            const float forearmRatio = forearmLen / (std::max)(origEHLen, 0.001f);
            if (arm.foreArm2 && !inPowerArmor) {
                arm.foreArm2->local.translate *= forearmRatio;
                arm.foreArm3->local.translate *= forearmRatio;
            }
            arm.hand->local.translate *= forearmRatio;

            f4cf::f4vr::updateTransformsDown(arm.shoulder, true);
            return true;
        }

        void solveArmIK(const ArmChain& arm, const RE::NiTransform& worldT)
        {
            const RE::NiPoint3 S = arm.upperArm->world.translate;   // shoulder (fixed)
            const RE::NiPoint3 E0 = arm.foreArm->world.translate;   // elbow now (pole hint)
            const RE::NiPoint3 W0 = arm.hand->world.translate;      // wrist now
            const RE::NiPoint3 Wt = worldT.translate;               // wrist target

            const float L1 = vlen(vsub(E0, S));
            const float L2 = vlen(vsub(W0, E0));

            // [STRETCH-MON Jul 18] measured at ENTRY, i.e. this is the arm as FRIK/other
            // systems left it from LAST frame. L2 here is the CURRENT forearm->hand distance —
            // if it is far above the true bone length seen at rest (~18gu), something outside
            // this IK stretched the hand away from the forearm after our last write.
            {
                static float s_restL2[2] = { 0.0f, 0.0f };
                const int mi = (arm.hand && arm.hand->name.contains("L")) ? 1 : 0;
                if (s_restL2[mi] <= 0.0f || L2 < s_restL2[mi]) { s_restL2[mi] = L2; }
                if (s_restL2[mi] > 0.5f && L2 > s_restL2[mi] * 1.5f) {
                    spdlog::warn("[STRETCH-MON] arm stretched at IK entry: L2={:.1f} rest={:.1f} ratio={:.2f} (external writer deformed the arm since our last write)",
                                 L2, s_restL2[mi], L2 / s_restL2[mi]);
                }
            }
            if (L1 < 1e-3f || L2 < 1e-3f) { placeNodeAtWorld(arm.hand, worldT); return; }

            RE::NiPoint3 toW = vsub(Wt, S);
            const float dRaw = vlen(toW);
            float d = dRaw;
            const float dmin = std::abs(L1 - L2) + 1e-3f;
            const float dmax = L1 + L2 - 1e-3f;
            d = std::clamp(d, dmin, dmax);
            const RE::NiPoint3 n = vnorm(toW);
            const float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
            const float h = std::sqrt((std::max)(0.0f, L1 * L1 - a * a));
            const RE::NiPoint3 P = vadd(S, vmul(n, a));

            // Bend direction: reuse FRIK's elbow plane (component of (E0-S) perpendicular to n).
            RE::NiPoint3 uPerp = vsub(vsub(E0, S), vmul(n, vdot(vsub(E0, S), n)));
            RE::NiPoint3 bend = (vlen(uPerp) > 1e-3f) ? vnorm(uPerp) : RE::NiPoint3{ 0, 0, -1 };
            const RE::NiPoint3 Et = vadd(P, vmul(bend, h));

            // UPPER ARM: aim S->E0 toward S->Et (minimal world delta; row-vector world*=D^T).
            {
                const RE::NiMatrix3 D = alignRotation(vsub(E0, S), vsub(Et, S));
                arm.upperArm->world.rotate = arm.upperArm->world.rotate * D.Transpose();
                const RE::NiTransform lw = heisenberg::Utils::GetLocalTransformForWorldTransform(arm.upperArm, arm.upperArm->world);
                sehWriteNodeLocal(arm.upperArm, lw);  // propagates -> foreArm now near Et
            }
            // FOREARM: aim elbow->wrist toward elbow->Wt.
            {
                const RE::NiPoint3 E1 = arm.foreArm->world.translate;
                const RE::NiPoint3 W1 = arm.hand->world.translate;
                const RE::NiMatrix3 D = alignRotation(vsub(W1, E1), vsub(Wt, E1));
                arm.foreArm->world.rotate = arm.foreArm->world.rotate * D.Transpose();
                const RE::NiTransform lw = heisenberg::Utils::GetLocalTransformForWorldTransform(arm.foreArm, arm.foreArm->world);
                sehWriteNodeLocal(arm.foreArm, lw);   // propagates -> wrist now at Wt
            }
            // HAND: final wrist orientation + fingers.
            // CHEWING-GUM FIX (Jul 18): when the requested wrist is beyond the arm's reach
            // (two-handed grip point pulled away from the body), the bones above were bent to
            // max reach - but the hand used to be teleported to the FULL target anyway, tearing
            // the skinned mesh between forearm and hand ("stretches like chewing gum"). Place
            // the hand at the CLAMPED reachable wrist instead; keep the target's orientation.
            RE::NiTransform handT = worldT;
            if (dRaw > dmax) {
                handT.translate = vadd(S, vmul(n, d));
            }
            placeNodeAtWorld(arm.hand, handT);

            // [AUTH-DIAG Jul 18] chewing-gum tracer: reach math + whether the bone writes LANDED.
            // wristErr = distance between where the forearm chain actually put the wrist and
            // where we placed the hand node. Large wristErr = arm-bone writes not taking effect
            // (hand teleported, arm still on controller) = the visible stretch.
            {
                static std::uint32_t s_authLog = 0;
                if ((++s_authLog % 15) == 0 || dRaw > dmax) {
                    const RE::NiPoint3 wristNow = arm.foreArm ? vadd(arm.foreArm->world.translate,
                        vmul(vnorm(vsub(arm.hand->world.translate, arm.foreArm->world.translate)), L2)) : RE::NiPoint3{};
                    const float wristErr = vlen(vsub(wristNow, handT.translate));
                    spdlog::debug("[AUTH-DIAG] dRaw={:.1f} dmax={:.1f} clamped={} L1={:.1f} L2={:.1f} wristErr={:.1f} hand=({:.0f},{:.0f},{:.0f})",
                                  dRaw, dmax, dRaw > dmax ? "YES" : "no", L1, L2, wristErr,
                                  handT.translate.x, handT.translate.y, handT.translate.z);
                }
            }
        }

        // Host-callback adaptors (embed passes hand: 0=Right,1=Left; xform = RE::NiTransform*).
        bool hostApply(const char* tag, int hand, const void* worldXform, int priority)
        {
            if (!worldXform) return false;
            return HandAuthority::Apply(tag, hand == 1, *reinterpret_cast<const RE::NiTransform*>(worldXform), priority);
        }
        bool hostClear(const char* tag, int hand) { return HandAuthority::Clear(tag, hand == 1); }
        bool hostGet(int hand, void* outWorldXform)
        {
            if (!outWorldXform) return false;
            return HandAuthority::GetRenderedHand(hand == 1, *reinterpret_cast<RE::NiTransform*>(outWorldXform));
        }
        HandAuthorityHostTable g_hostTable{ &hostApply, &hostClear, &hostGet };
    }

    bool HandAuthority::Available()
    {
        return f4cf::f4vr::getCommonNode() != nullptr;
    }

    // Set when applyWandFeed handled a hand this frame; ApplyWinners then skips the
    // post-FRIK bone IK for that hand (single writer: FRIK itself).
    static bool g_wandFedThisFrame[2] = { false, false };

    // Frame stamp of the last shim consumption per hand (g_frameCounter units) — lets
    // ApplyWinners detect a seam that is installed but not being called (e.g. FRIK not
    // solving yet) and fall back to legacy IK for those frames.
    static std::uint32_t g_lastConsumeFrame[2] = { 0, 0 };

    namespace
    {
        // Row-vector v' = v * M (project convention: world = local * parent).
        inline RE::NiPoint3 vxm(const RE::NiPoint3& v, const RE::NiMatrix3& m)
        {
            return {
                v.x * m.entry[0][0] + v.y * m.entry[1][0] + v.z * m.entry[2][0],
                v.x * m.entry[0][1] + v.y * m.entry[1][1] + v.z * m.entry[2][1],
                v.x * m.entry[0][2] + v.y * m.entry[1][2] + v.z * m.entry[2][2],
            };
        }

        // WAND-FEED (Jul 18): make FRIK itself carry the arm to the authority target.
        // The post-FRIK bone-IK shim FIGHTS FRIK's own per-frame arm solve on old FRIK —
        // the visible chewing-gum stretch (this is exactly what FRIK's unreleased v5 API
        // solves inside FRIK; we reproduce it from outside). Rigid-transport the WAND
        // node — the target FRIK's arm IK follows — so FRIK's own solve places the
        // rendered hand at the requested world transform with a correct full arm.
        // ROCK's main-loop hook runs BEFORE FRIK's in the hook chain (ROCK.log shows
        // its trampoline 'original' pointing into the FRIK DLL), so a write made at
        // hostApply time is consumed by FRIK the SAME frame. Nothing to restore on
        // clear: the game refreshes wand nodes from VR every frame.
        bool applyWandFeed(bool isLeft, const RE::NiTransform& target)
        {
            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            if (!playerNodes) return false;
            RE::NiNode* wand = heisenberg::GetWandNode(playerNodes, isLeft);
            if (!wand) return false;
            ArmChain arm{};
            if (!resolveArmChain(isLeft, arm) || !arm.hand) return false;
            RE::NiNode* com = f4cf::f4vr::getCommonNode();
            if (!inLiveSubtree(arm.hand, com)) return false;

            const RE::NiTransform& handW = arm.hand->world;
            const RE::NiTransform& wandW = wand->world;
            // World delta D mapping the current hand pose onto the target
            // (row-vector: handRot * D = targetRot -> D = handRot^T * targetRot).
            const RE::NiMatrix3 D = handW.rotate.Transpose() * target.rotate;
            RE::NiTransform wandT;
            wandT.rotate = wandW.rotate * D;
            wandT.translate = target.translate + vxm(wandW.translate - handW.translate, D);
            wandT.scale = wandW.scale;
            placeNodeAtWorld(wand, wandT);
            return true;
        }
    }

    bool HandAuthority::Apply(const char* tag, bool isLeft, const RE::NiTransform& world, int priority)
    {
        // Finite validation (Jul 19): a NaN/Inf target must never enter the registry —
        // it would propagate into FRIK's solver through the goal swap.
        {
            const float* rm = &world.rotate.entry[0][0];
            for (int fi = 0; fi < 9; ++fi) {
                if (!std::isfinite(rm[fi])) return false;
            }
            if (!std::isfinite(world.translate.x) || !std::isfinite(world.translate.y) ||
                !std::isfinite(world.translate.z) || !std::isfinite(world.scale)) {
                return false;
            }
        }

        // WAND-FEED path: let FRIK carry the arm (see applyWandFeed above); the bone-IK
        // fallback in ApplyWinners is skipped for hands fed this frame.
        // Jul 19: WAND-FEED DISABLED — in-game it made the support arm crawl (the game does
        // NOT refresh the wand from VR before our next read, so the transport fed back into
        // itself) and the session crashed. Reverting to the post-FRIK bone IK below while a
        // correct pre-FRIK feed is designed (needs a wand baseline captured from the VR
        // update, not the possibly-already-overridden node).
        // if (applyWandFeed(isLeft, world)) {
        //     g_wandFedThisFrame[isLeft ? 1 : 0] = true;
        // }
        if (!tag) return false;
        std::scoped_lock lk(g_mtx);
        g_registered = true;
        auto& slot = g_hands[slotIdx(isLeft)];
        // find matching tag, else a free/lowest-priority slot
        Writer* found = nullptr;
        Writer* freeSlot = nullptr;
        Writer* lowest = nullptr;
        for (auto& w : slot.writers) {
            if (w.active && std::strncmp(w.tag, tag, sizeof(w.tag) - 1) == 0) { found = &w; break; }
            if (!w.active && !freeSlot) freeSlot = &w;
            if (w.active && (!lowest || w.priority < lowest->priority)) lowest = &w;
        }
        Writer* dst = found ? found : (freeSlot ? freeSlot : lowest);
        if (!dst) return false;
        std::strncpy(dst->tag, tag, sizeof(dst->tag) - 1);
        dst->tag[sizeof(dst->tag) - 1] = '\0';
        dst->priority = priority;
        dst->world = world;
        dst->lastFrame = g_frameCounter;
        dst->active = true;
        return true;
    }

    bool HandAuthority::Clear(const char* tag, bool isLeft)
    {
        if (!tag) return false;
        std::scoped_lock lk(g_mtx);
        auto& slot = g_hands[slotIdx(isLeft)];
        for (auto& w : slot.writers) {
            if (w.active && std::strncmp(w.tag, tag, sizeof(w.tag) - 1) == 0) { w.active = false; return true; }
        }
        return false;
    }

    bool HandAuthority::GetRenderedHand(bool isLeft, RE::NiTransform& out)
    {
        {
            std::scoped_lock lk(g_mtx);
            auto& slot = g_hands[slotIdx(isLeft)];
            if (slot.overriding) { out = slot.lastApplied; return true; }
        }
        if (RE::NiNode* hand = skinnedHandNode(isLeft)) { out = hand->world; return true; }
        return false;
    }

    void HandAuthority::ApplyWinners()
    {
        FrikArmGoalHook::OnFrame();  // probe + slot self-check (game thread, per frame)
        std::scoped_lock lk(g_mtx);
        const std::uint32_t frame = g_frameCounter;
        for (int i = 0; i < 2; ++i) {
            const bool isLeft = (i == 1);
            auto& slot = g_hands[i];

            // pick highest-priority active, non-stale writer
            Writer* winner = nullptr;
            for (auto& w : slot.writers) {
                if (!w.active) continue;
                if (frame - w.lastFrame > kStaleFrames) { w.active = false; continue; }
                if (!winner || w.priority > winner->priority ||
                    (w.priority == winner->priority && w.lastFrame > winner->lastFrame)) {
                    winner = &w;
                }
            }

            if (!winner) {
                slot.overriding = false;
                slot.hasLatched = false;
                slot.latchedTracksPlayer = false;
                slot.latchedReachLimitedRigid = false;
                slot.latchedPlayerPositionValid = false;
                if (g_armBase[i].valid) {
                    // authority episode ended: put the chain back on its clean base once so
                    // FRIK resumes from an uncontaminated pose, then drop the snapshot.
                    ArmChain armR{};
                    if (resolveArmChain(isLeft, armR) && armR.shoulder) {
                        restoreArmBase(armR, g_armBase[i]);
                    }
                    g_armBase[i].valid = false;
                    g_frikPrevTwist[i] = 0.0f;
                }
                continue;
            }

            const bool rigidWeaponWinner =
                std::strncmp(winner->tag, "ROCK_Weapon", 11) == 0;
            const bool rigidSupportGripWinner =
                std::strncmp(winner->tag, "ROCK_WeaponSupportGrip", 22) == 0;
            const bool reachLimitedRigidWeaponWinner =
                std::strcmp(winner->tag, "ROCK_WeaponSupportGripRigid") == 0;

            // FRIK-GOAL seam active: LATCH the winner for FrikArmGoalHook's shim (consumed
            // inside FRIK's setArms next frame, where FRIK's own solver carries the arm).
            // No scene writes from here — single writer is FRIK itself. SELF-HEAL (Jul 19):
            // if the shim has not polled this hand within the last ~5 frames (FRIK not
            // solving: loading screens, menus, seam silently bypassed), fall through to the
            // legacy bone IK below for THIS frame so authority never goes dark.
            if (FrikArmGoalHook::IsActive()) {
                slot.latched = winner->world;
                slot.hasLatched = true;
                slot.latchedTracksPlayer = rigidWeaponWinner;
                slot.latchedReachLimitedRigid = reachLimitedRigidWeaponWinner;
                slot.latchedPlayerPositionValid = slot.latchedTracksPlayer &&
                    getPlayerWorldPosition(slot.latchedPlayerPosition);
                const std::uint32_t sinceConsume = frame - g_lastConsumeFrame[i];
                // The FRIK seam consumes this latch on the NEXT frame. That remains useful
                // for carrying the arm through FRIK, but it cannot be the final rendered
                // support-hand placement: ROCK has just changed the gun at this callback's
                // tail. Fall through for the hosted rigid support grip and solve it once
                // more from the final live gun. (Never do this for the primary hand: the
                // weapon is its child and moving that chain would feed back into the gun.)
                const bool needsSameFrameSupportPin =
                    rigidSupportGripWinner && heisenberg::IsRockEngineHosted();
                if (sinceConsume <= 5 && !needsSameFrameSupportPin) {
                    continue;
                }
            } else {
                slot.hasLatched = false;
                slot.latchedTracksPlayer = false;
                slot.latchedReachLimitedRigid = false;
                slot.latchedPlayerPositionValid = false;
            }

            ArmChain arm;
            if (!resolveArmChain(isLeft, arm)) { slot.overriding = false; continue; }
            // liveness guard: the hand node is raw + held across frames; skip if the skeleton rebuilt.
            RE::NiNode* com = f4cf::f4vr::getCommonNode();
            if (!inLiveSubtree(arm.hand, com)) { slot.overriding = false; continue; }

            // ANTI-RUBBER-BAND (Jul 19): weapon-grip winners were composed from the weapon
            // transform of ROCK's pre-FRIK update — one frame stale, so the hand slides off
            // the grip in proportion to gun speed (the visible "chewing gum"). Re-derive the
            // target from the weapon node's CURRENT (post-FRIK, this-frame) world.
            RE::NiTransform liveTarget = winner->world;
            if (rigidWeaponWinner) {
                RE::NiTransform fresh;
                if (heisenberg::IsRockEngineHosted() && rock::HostGetLiveGripHandWorld(isLeft, fresh)) {
                    liveTarget = fresh;
                }
            }

            // A rigid weapon winner is already reach-limited by translating the complete
            // weapon in TwoHandedGrip. Never project just its hand here: that would break
            // the captured hand-to-gun transform. Free/non-weapon authority keeps the
            // visual-only safety projection.
            if (!rigidWeaponWinner) {
                float requestedDistance = 0.0f;
                float maxReach = 0.0f;
                if (!constrainTargetToArmReach(arm, liveTarget, &requestedDistance, &maxReach)) {
                    slot.overriding = false;
                    continue;
                }
                if (requestedDistance > maxReach) {
                    static std::uint32_t s_legacyReachClampLog = 0;
                    if ((++s_legacyReachClampLog % 30) == 1) {
                        spdlog::debug("[HAND-REACH] {} legacy free target clamped {:.1f}->{:.1f}gu",
                            isLeft ? "L" : "R", requestedDistance, maxReach);
                    }
                }
            }

            // FRIK-SOLVER PORT (Jul 19): prefer the faithful setArms port. Episode base is
            // captured on the first apply (clean controller pose) and restored before every
            // solve (v5's restoreArmNodesToDefault semantics — prevents delta compounding).
            bool applied = false;
            if (arm.shoulder && arm.chest) {
                auto& base = g_armBase[i];
                if (!base.valid) {
                    captureArmBase(arm, base);
                } else {
                    restoreArmBase(arm, base);
                }
                applied = solveArmFrik(arm, isLeft, liveTarget);
            }
            {
                static std::uint32_t s_pathDbg = 0;
                if ((++s_pathDbg % 15) == 0) {
                    const float ex = arm.hand->world.translate.x - liveTarget.translate.x;
                    const float ey = arm.hand->world.translate.y - liveTarget.translate.y;
                    const float ez = arm.hand->world.translate.z - liveTarget.translate.z;
                    const auto& dg = g_frikDiag[i];
                    spdlog::debug("[FRIK-SOLVER] {} path={} err={:.1f} hsLen={:.1f} reach={:.1f} stretched={}",
                                  isLeft ? "L" : "R", applied ? "frik-port" : "LEGACY-FALLBACK",
                                  std::sqrt(ex * ex + ey * ey + ez * ez),
                                  dg.hsLen, dg.reach, dg.stretched ? "YES" : "no");
                }
            }
            if (!applied) {
                // fallback: legacy analytic IK (chain incomplete / solver aborted).
                // FRIK-77.12 PARITY (Jul 19, tester "hand disconnects from arm, snaps
                // back"): the old small-delta shortcut placed ONLY the hand node — when
                // this fallback carries consecutive frames (77.12 solve-order quirks),
                // small deltas accumulate and the hand visibly detaches from the forearm.
                // Always solve the full arm chain; never place the hand alone.
                solveArmIK(arm, liveTarget);
            }
            slot.lastApplied = liveTarget;
            slot.overriding = true;
        }
        g_frameCounter = frame + 1;
    }

    bool HandAuthority::TryConsumeLatched(bool isLeft, RE::NiTransform& out,
        bool* rigidWeaponTarget)
    {
        std::scoped_lock lk(g_mtx);
        auto& slot = g_hands[slotIdx(isLeft)];
        g_lastConsumeFrame[slotIdx(isLeft)] = g_frameCounter;
        if (rigidWeaponTarget) {
            *rigidWeaponTarget = false;
        }
        if (!slot.hasLatched) {
            return false;
        }
        out = slot.latched;
        if (rigidWeaponTarget) {
            *rigidWeaponTarget = slot.latchedReachLimitedRigid;
        }
        if (slot.latchedTracksPlayer && slot.latchedPlayerPositionValid) {
            RE::NiPoint3 currentPlayerPosition{};
            if (getPlayerWorldPosition(currentPlayerPosition)) {
                // ROCK publishes after FRIK and this latch is consumed by FRIK on the next
                // frame. Carry the exact solved target through only the intervening player
                // locomotion translation. Recomputing it from the pre-ROCK weapon here would
                // discard ROCK's final aim/source frame and reintroduce scope wobble.
                const RE::NiPoint3 locomotionDelta =
                    vsub(currentPlayerPosition, slot.latchedPlayerPosition);
                if (isFinitePoint(locomotionDelta) && vlen(locomotionDelta) < 512.0f) {
                    out.translate = vadd(out.translate, locomotionDelta);
                }
            }
        }
        return true;
    }

    bool HandAuthority::ConstrainTargetToArmReach(bool isLeft, RE::NiTransform& target,
        float* requestedDistance, float* maxReach)
    {
        ArmChain arm{};
        if (!resolveArmChain(isLeft, arm)) {
            return false;
        }
        RE::NiNode* com = f4cf::f4vr::getCommonNode();
        if (!inLiveSubtree(arm.hand, com)) {
            return false;
        }
        return constrainTargetToArmReach(arm, target, requestedDistance, maxReach);
    }

    bool HandAuthority::GetArmReachSphere(bool isLeft, RE::NiPoint3& shoulderWorld,
        float& maxReach)
    {
        shoulderWorld = {};
        maxReach = 0.0f;
        ArmChain arm{};
        if (!resolveArmChain(isLeft, arm)) {
            return false;
        }
        RE::NiNode* com = f4cf::f4vr::getCommonNode();
        if (!inLiveSubtree(arm.hand, com) || !isFinitePoint(arm.upperArm->world.translate)) {
            return false;
        }
        const float reach = naturalArmReach(arm);
        if (!std::isfinite(reach) || reach <= 1.0f) {
            return false;
        }
        shoulderWorld = arm.upperArm->world.translate;
        maxReach = reach;
        return true;
    }

    void HandAuthority::NoteApplied(bool isLeft, const RE::NiTransform& world)
    {
        std::scoped_lock lk(g_mtx);
        auto& slot = g_hands[slotIdx(isLeft)];
        slot.lastApplied = world;
        slot.overriding = true;

        // [SEAM-DIAG Jul 19] skinned shoulder->target distance vs actual bone reach — FRIK
        // stretches the arm when this exceeds reach; >1.5x = the visible "very long arms".
        static std::uint32_t s_dbg2 = 0;
        if ((++s_dbg2 % 30) == 0) {
            ArmChain arm{};
            if (resolveArmChain(isLeft, arm) && arm.upperArm && arm.foreArm && arm.hand) {
                const float L1 = vlen(vsub(arm.foreArm->world.translate, arm.upperArm->world.translate));
                const float L2 = vlen(vsub(arm.hand->world.translate, arm.foreArm->world.translate));
                const float sd = vlen(vsub(world.translate, arm.upperArm->world.translate));
                spdlog::debug("[SEAM-DIAG] {} shoulder->target={:.1f} reach={:.1f} ratio={:.2f}",
                              isLeft ? "L" : "R", sd, L1 + L2, sd / (std::max)(L1 + L2, 0.001f));
            }
        }
    }

    bool HandAuthority::GetSkinnedHandWorld(bool isLeft, RE::NiTransform& out)
    {
        std::scoped_lock lk(g_mtx);
        ArmChain arm{};
        if (!resolveArmChain(isLeft, arm) || !arm.hand) {
            return false;
        }
        out = arm.hand->world;
        return true;
    }

    bool HandAuthority::GetSkinnedHandRotation(bool isLeft, RE::NiMatrix3& out)
    {
        std::scoped_lock lk(g_mtx);
        ArmChain arm{};
        if (!resolveArmChain(isLeft, arm) || !arm.hand) {
            return false;
        }
        out = arm.hand->world.rotate;
        return true;
    }

    void HandAuthority::Reset()
    {
        std::scoped_lock lk(g_mtx);
        for (std::size_t i = 0; i < std::size(g_hands); ++i) {
            auto& slot = g_hands[i];
            for (auto& w : slot.writers) w.active = false;
            slot.overriding = false;
            slot.hasLatched = false;
            slot.latchedTracksPlayer = false;
            slot.latchedReachLimitedRigid = false;
            slot.latchedPlayerPositionValid = false;
            // A reset can coincide with a FRIK skeleton rebuild. Never restore local
            // transforms captured from the retired skeleton in a later authority episode.
            g_armBase[i].valid = false;
            g_frikPrevTwist[i] = 0.0f;
            g_lastConsumeFrame[i] = 0;
        }
    }

    const HandAuthorityHostTable& HandAuthority::HostTable()
    {
        return g_hostTable;
    }
}
