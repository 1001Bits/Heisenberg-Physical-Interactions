#include "FrikArmGoalHook.h"

#include "HandAuthority.h"
#include "Config.h"
#include "../external/ROCK/src/ROCKMain.h"

#include "f4vr/PlayerNodes.h"
#include "RE/Bethesda/PlayerCharacter.h"

#include <Windows.h>
#include <atomic>
#include <cstring>
#include <cmath>
#include <spdlog/spdlog.h>

namespace heisenberg::FrikArmGoalHook
{
    namespace
    {
        // ---- Pinned constants for FRIK 0.76.12.0 (byte-asserted before use) ----
        constexpr std::uint32_t kPinnedSlotRva = 0x20C690;   // .data qword: REL::Relocation<U1PA> storage
        constexpr std::uint32_t kPinnedCallRva = 0x818ac;    // the single FF15 reader inside Skeleton::setArms
        constexpr std::uint32_t kPinnedSetArmsRva = 0x811a0; // Skeleton::setArms prologue
        constexpr std::uint32_t kPinnedInitRva = 0x4321;     // slot initializer (mov rax,[base]; add rax,0xef6280; mov [slot],rax)
        constexpr std::uint64_t kU1PAGameOffset = 0xef6280;  // F4VR 1.2.72 Update1StPersonArm

        constexpr unsigned char kCallBytes[6] = { 0xFF, 0x15, 0xDE, 0xAD, 0x18, 0x00 };
        constexpr unsigned char kSetArmsPrologue[16] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55,
            0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56
        };
        constexpr unsigned char kInitBytes[20] = {
            0x48, 0x8B, 0x05, 0x68, 0x5F, 0x20, 0x00,        // mov rax,[rip+...] (game base)
            0x48, 0x05, 0x80, 0x62, 0xEF, 0x00,              // add rax,0xef6280
            0x48, 0x89, 0x05, 0x5B, 0x83, 0x20, 0x00         // mov [rip+...->slot],rax
        };

        using U1PAFn = void* (*)(RE::PlayerCharacter*, RE::NiNode**, RE::NiNode**);

        std::uintptr_t s_frikBase = 0;
        std::uintptr_t s_slotAddr = 0;      // absolute address of the pointer slot
        U1PAFn s_original = nullptr;        // saved original slot value
        std::atomic<bool> s_active{ false };
        std::atomic<bool> s_disarmed{ false };  // one-shot: SEH fault or failed probe
        std::atomic<bool> s_cleanTruthPublished{ false };
        std::atomic<std::uint8_t> s_cleanTruthSeenMask{ 0 };

        // Behavioral probe: both FRIK offset nodes must be seen within the window.
        std::atomic<std::uint32_t> s_probePrimarySeen{ 0 };
        std::atomic<std::uint32_t> s_probeSecondarySeen{ 0 };
        std::uint32_t s_framesSinceInstall = 0;
        bool s_probePassed = false;
        std::uint32_t s_slotRecheckCountdown = 300;
        bool s_reinstalledOnce = false;

        // ROTATION BASIS CONVERSION (Jul 19): ROCK's authority targets orient the SKINNED
        // hand (the convention the legacy shim consumed), but the goal node we write is the
        // FIRST-PERSON Arm_Hand whose basis differs by FRIK's fixed wrist mapping — writing
        // the raw rotation flipped the arm upside-down. Row-vector model: skinned = fp * K
        // (K constant). Capture Kinv = skinned^T * fp at the FIRST consume of each authority
        // episode (skinned pose still controller-driven, uncontaminated by our writes), then
        // write fpGoal.rot = target.rot * Kinv so FRIK's mapping lands the skinned hand at
        // exactly target.rot.
        bool s_basisValid[2] = { false, false };
        RE::NiMatrix3 s_basisKinv[2]{};

        // Cached goal nodes (per fp-skeleton instance; revalidated on skeleton change).
        RE::NiNode* s_cachedFpSkeleton = nullptr;
        RE::NiNode* s_cachedHandNode[2] = { nullptr, nullptr };  // [0]=Right, [1]=Left

        bool bytesMatch(std::uintptr_t addr, const unsigned char* expect, std::size_t n)
        {
            __try {
                return std::memcmp(reinterpret_cast<const void*>(addr), expect, n) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        std::uint64_t readQword(std::uintptr_t addr)
        {
            __try {
                return *reinterpret_cast<const volatile std::uint64_t*>(addr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        bool writeQword(std::uintptr_t addr, std::uint64_t value)
        {
            __try {
                *reinterpret_cast<volatile std::uint64_t*>(addr) = value;  // aligned 8-byte store
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // SEH rule: raw goal-node world write, no destructor-bearing locals inside __try.
        // Writes rotate (9 floats) + translate (3 floats), PRESERVES scale.
        bool sehWriteGoalWorld(RE::NiNode* node, const RE::NiTransform* target)
        {
            __try {
                node->world.rotate = target->rotate;
                node->world.translate = target->translate;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        RE::NiNode* resolveGoalNode(bool isLeft)
        {
            RE::NiNode* fp = f4cf::f4vr::getFirstPersonSkeleton();
            if (!fp) {
                return nullptr;
            }
            const int i = isLeft ? 1 : 0;
            if (fp != s_cachedFpSkeleton) {
                s_cachedFpSkeleton = fp;
                s_cachedHandNode[0] = s_cachedHandNode[1] = nullptr;
            }
            if (!s_cachedHandNode[i]) {
                s_cachedHandNode[i] = f4cf::f4vr::findNode(fp, isLeft ? "LArm_Hand" : "RArm_Hand");
            }
            return s_cachedHandNode[i];
        }

        // ---- The shim (v2, Jul 19 frame-order audit) ----
        //
        // DOUBLE-SOLVE input override. The Pip-Boy and all first-person accessories ride the FP
        // arm, which the game solves toward the OFFSET node (wand-attached aim node) inside this
        // very call — no post-FRIK write can move them (the audit's chewing-gum + pipboy-detach
        // mechanism). v2 therefore works at the INPUT:
        //   PASS 1 (clean): call the real U1PA with FRIK's own inputs. The FP hand result is
        //     CONTROLLER TRUTH — published to the embed (rock::HostSetPreAuthorityHandWorlds)
        //     so ROCK's two-handed weapon solve never reads authority output (loop-proof), and
        //     used to capture the offset->hand rigid mapping M for this pass.
        //   PASS 2 (authority): if a hand-authority target is latched, rewrite the offset node
        //     world to target ∘ M⁻¹ and call the real U1PA AGAIN. The FP arm — and with it the
        //     Pip-Boy, and the skinned arm that FRIK's setArms then solves onto the FP result,
        //     and FRIK's fingers/weapon phases — all land on the grip COHERENTLY, exactly as
        //     the unreleased FRIK v5 does from inside.
        // No feedback by construction: M and truth come from the clean same-frame pass; nothing
        // we write is ever an input to pass 1; ROCK consumes clean truth.
        RE::NiTransform s_truthHand[2]{};     // clean U1PA hand world per physical hand
        bool s_truthValid[2] = { false, false };
        RE::NiPoint3 s_truthShoulder[2]{};
        float s_truthMaxReach[2] = { 0.0f, 0.0f };
        bool s_truthReachValid[2] = { false, false };

        // SEH raw world write for the offset node (no destructor-bearing locals in __try).
        bool sehWriteOffsetWorld(RE::NiNode* node, const RE::NiTransform* w)
        {
            __try {
                node->world = *w;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void* FrikU1PAShim(RE::PlayerCharacter* pc, RE::NiNode** weapon, RE::NiNode** offset)
        {
            void* ret = s_original ? s_original(pc, weapon, offset) : nullptr;
            if (s_disarmed.load(std::memory_order_relaxed)) {
                return ret;
            }

            RE::NiNode* off = offset ? *offset : nullptr;
            if (!off) {
                return ret;
            }
            auto* nodes = f4cf::f4vr::getPlayerNodes();
            if (!nodes) {
                return ret;
            }
            bool offhandPass;
            if (off == nodes->primaryWeaponOffsetNOde) {
                offhandPass = false;
                s_probePrimarySeen.fetch_add(1, std::memory_order_relaxed);
            } else if (off == nodes->SecondaryMeleeWeaponOffsetNode2) {
                offhandPass = true;
                s_probeSecondarySeen.fetch_add(1, std::memory_order_relaxed);
            } else {
                return ret;  // not a FRIK setArms invocation we recognize — pass through
            }
            const bool isLeft = offhandPass ^ f4cf::f4vr::isLeftHandedMode();
            const int hi = isLeft ? 1 : 0;

            // FP hand node = the goal U1PA just produced (clean, pass 1).
            RE::NiNode* fpHand = resolveGoalNode(isLeft);
            if (!fpHand) {
                return ret;
            }

            // Publish controller truth for this hand (ROCK's weapon-solve input seam).
            s_truthHand[hi] = fpHand->world;
            s_truthValid[hi] = true;
            // Capture the anatomical frame beside the clean pass, before this shim's
            // authority re-solve can move the arm. ROCK consumes the copied center/radius
            // with this same hand pair, so common locomotion cannot become weapon pitch.
            s_truthReachValid[hi] = HandAuthority::GetArmReachSphere(
                isLeft, s_truthShoulder[hi], s_truthMaxReach[hi]);
            const std::uint8_t seenMask = static_cast<std::uint8_t>(
                s_cleanTruthSeenMask.fetch_or(static_cast<std::uint8_t>(1u << hi),
                    std::memory_order_relaxed) |
                static_cast<std::uint8_t>(1u << hi));
            if (seenMask == 0x3u) {
                rock::HostSetPreAuthorityHandWorlds(s_truthHand[1], s_truthHand[0]);
                if (s_truthReachValid[1]) {
                    rock::HostSetPreAuthorityArmReach(
                        true, s_truthShoulder[1], s_truthMaxReach[1]);
                }
                if (s_truthReachValid[0]) {
                    rock::HostSetPreAuthorityArmReach(
                        false, s_truthShoulder[0], s_truthMaxReach[0]);
                }
                s_cleanTruthPublished.store(true, std::memory_order_release);
                s_cleanTruthSeenMask.store(0, std::memory_order_relaxed);
            }

            // WEAPON-SUBTREE GUARD (Jul 19, forward-runaway fix): the rendered weapon is a
            // CHILD of the primary hand's fp arm. Input-overriding the PRIMARY pass moves the
            // weapon itself -> ROCK re-aims from the moved weapon -> next frame's target sits
            // further forward -> runaway ("gun and hand pulled way to the front"). This is why
            // FRIK v5's internal re-solve excludes the weapon subtree. Pass-2 is only legal on
            // the OFFHAND pass (its weapon node is the empty left dummy). Primary-hand
            // authority falls to the skinned-side solver via ApplyWinners' self-heal.
            if (!offhandPass) {
                return ret;
            }

            RE::NiTransform target;
            bool reachLimitedRigidWeaponTarget = false;
            if (!HandAuthority::TryConsumeLatched(
                    isLeft, target, &reachLimitedRigidWeaponTarget)) {
                return ret;
            }

            // Rigid weapon targets are an exact hand-to-gun weld. ROCK has already limited
            // reach upstream by translating the complete weapon, so neither the controller
            // escape envelope nor a hand-only reach projection is legal here: either would
            // visibly separate the palm from the rifle. Keep those safeguards for free hand
            // authority writers only.
            if (!reachLimitedRigidWeaponTarget) {
                constexpr float kFreeRadius = 22.0f;   // perp: anchor fully released beyond this
                constexpr float kAlongEscape = 40.0f;  // along-barrel bailout distance
                const RE::NiPoint3 truthPos = s_truthHand[hi].translate;
                const float ddx = truthPos.x - target.translate.x;
                const float ddy = truthPos.y - target.translate.y;
                const float ddz = truthPos.z - target.translate.z;
                const int otherHi = 1 - hi;
                float axx = target.translate.x - s_truthHand[otherHi].translate.x;
                float axy = target.translate.y - s_truthHand[otherHi].translate.y;
                float axz = target.translate.z - s_truthHand[otherHi].translate.z;
                const float axLen = std::sqrt(axx * axx + axy * axy + axz * axz);
                if (s_truthValid[otherHi] && axLen > 1.0f) {
                    axx /= axLen; axy /= axLen; axz /= axLen;
                    const float along = ddx * axx + ddy * axy + ddz * axz;
                    const float px = ddx - axx * along;
                    const float py = ddy - axy * along;
                    const float pz = ddz - axz * along;
                    const float dPerp = std::sqrt(px * px + py * py + pz * pz);
                    if (dPerp >= kFreeRadius || std::fabs(along) >= kAlongEscape) {
                        return ret;  // fully free — hand is the controller's
                    }
                } else {
                    // No primary-hand truth yet: retain the isotropic safety envelope only.
                    const float d = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                    if (d >= kFreeRadius) {
                        return ret;
                    }
                }
                float requestedReach = 0.0f;
                float maxReach = 0.0f;
                if (!HandAuthority::ConstrainTargetToArmReach(isLeft, target, &requestedReach, &maxReach)) {
                    return ret;
                }
                if (requestedReach > maxReach) {
                    static std::uint32_t s_reachClampLog = 0;
                    if ((++s_reachClampLog % 30) == 1) {
                        spdlog::debug("[HAND-REACH] {} seam free target clamped {:.1f}->{:.1f}gu",
                            isLeft ? "L" : "R", requestedReach, maxReach);
                    }
                }
            }

            // Rigid mapping M: offsetWorld -> fpHandWorld, captured from the CLEAN pass.
            // Row-vector convention (world = local * parent):
            //   handRot = K * offRot with K = handRot * offRot^T  (world-basis link)
            //   handPos = offPos + d, d in offset basis: d_local = (handPos - offPos) * offRot^T
            // Desired: hand' = target  =>  offRot' = Kinv * targetRot = (offRot * handRot^T) * targetRot
            //          offPos' = targetPos - d_world' where d_world' = d_local * offRot'
            const RE::NiMatrix3 offRot = off->world.rotate;
            const RE::NiMatrix3 handRot = fpHand->world.rotate;
            const RE::NiPoint3 dWorld = fpHand->world.translate - off->world.translate;

            RE::NiTransform offT;
            offT.rotate = offRot * handRot.Transpose() * target.rotate;
            // transport the offset->hand displacement into the new orientation:
            // d_local = dWorld * offRot^T ; d_world' = d_local * offT.rotate
            const RE::NiMatrix3 dXfer = offRot.Transpose() * offT.rotate;
            const RE::NiPoint3 dWorldNew = {
                dWorld.x * dXfer.entry[0][0] + dWorld.y * dXfer.entry[1][0] + dWorld.z * dXfer.entry[2][0],
                dWorld.x * dXfer.entry[0][1] + dWorld.y * dXfer.entry[1][1] + dWorld.z * dXfer.entry[2][1],
                dWorld.x * dXfer.entry[0][2] + dWorld.y * dXfer.entry[1][2] + dWorld.z * dXfer.entry[2][2],
            };
            offT.translate = target.translate - dWorldNew;
            offT.scale = off->world.scale;

            const RE::NiTransform savedOffWorld = off->world;
            if (!sehWriteOffsetWorld(off, &offT)) {
                s_disarmed.store(true, std::memory_order_release);
                spdlog::warn("[FRIK-GOAL] SEH fault writing offset node — seam disarmed");
                return ret;
            }

            // PASS 2: the real solver carries the FP arm (and the Pip-Boy with it) to the grip.
            // CRASH FIX (Jul 19): FRIK IncRefCount()s the weapon node before EVERY U1PA call —
            // the solver consumes a reference. Our second call must add its own reference or
            // the weapon node underflows and is freed mid-use (the two-handing CTD).
            if (weapon && *weapon) {
                (*weapon)->IncRefCount();
            }
            ret = s_original ? s_original(pc, weapon, offset) : ret;

            // Restore the offset node so nothing downstream reads our override as input.
            (void)sehWriteOffsetWorld(off, &savedOffWorld);

            HandAuthority::NoteApplied(isLeft, target);
            if (g_config.handAuthorityGoalHookLog) {
                spdlog::debug("[FRIK-GOAL] v2 {} target=({:.0f},{:.0f},{:.0f}) applied via offset re-solve",
                              isLeft ? "L" : "R", target.translate.x, target.translate.y, target.translate.z);
            }
            return ret;
        }

        bool validatePinned(std::uintptr_t frikBase, std::uintptr_t gameBase)
        {
            if (!bytesMatch(frikBase + kPinnedCallRva, kCallBytes, sizeof(kCallBytes))) return false;
            if (!bytesMatch(frikBase + kPinnedSetArmsRva, kSetArmsPrologue, sizeof(kSetArmsPrologue))) return false;
            if (!bytesMatch(frikBase + kPinnedInitRva, kInitBytes, sizeof(kInitBytes))) return false;
            // Decisive semantic check: the slot must currently hold the game's U1PA.
            const std::uint64_t slotVal = readQword(frikBase + kPinnedSlotRva);
            return slotVal == static_cast<std::uint64_t>(gameBase) + kU1PAGameOffset;
        }

        // Tier-1: discover the slot in an unknown public-lineage FRIK build.
        //   initializer idiom: 48 05 80 62 EF 00 (add rax,0xef6280) then 48 89 05 disp32.
        bool discoverSlot(std::uintptr_t frikBase, std::uintptr_t gameBase, std::uintptr_t& outSlotAddr)
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(frikBase);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(frikBase + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
            const auto* sec = IMAGE_FIRST_SECTION(nt);
            const unsigned char pat[6] = { 0x48, 0x05, 0x80, 0x62, 0xEF, 0x00 };
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                if (std::memcmp(sec[i].Name, ".text", 5) != 0) continue;
                const auto* p = reinterpret_cast<const unsigned char*>(frikBase + sec[i].VirtualAddress);
                const std::size_t n = sec[i].Misc.VirtualSize;
                for (std::size_t k = 0; k + 13 < n; ++k) {
                    if (std::memcmp(p + k, pat, 6) != 0) continue;
                    if (p[k + 6] != 0x48 || p[k + 7] != 0x89 || p[k + 8] != 0x05) continue;
                    std::int32_t disp;
                    std::memcpy(&disp, p + k + 9, 4);
                    const std::uintptr_t insnEnd = frikBase + sec[i].VirtualAddress + k + 13;
                    const std::uintptr_t slot = insnEnd + disp;
                    if (readQword(slot) == static_cast<std::uint64_t>(gameBase) + kU1PAGameOffset) {
                        outSlotAddr = slot;
                        return true;
                    }
                }
            }
            return false;
        }
    }

    bool Install()
    {
        if (s_active.load(std::memory_order_acquire)) {
            return true;
        }
        if (s_disarmed.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_config.handAuthorityApplyMode == 0) {
            spdlog::info("[FRIK-GOAL] iHandAuthorityApplyMode=0 — legacy post-FRIK bone IK retained");
            return false;
        }

        HMODULE frik = GetModuleHandleW(L"FRIK.dll");
        if (!frik) {
            spdlog::warn("[FRIK-GOAL] FRIK.dll not loaded — seam unavailable");
            return false;
        }
        s_frikBase = reinterpret_cast<std::uintptr_t>(frik);
        const std::uintptr_t gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

        std::uintptr_t slot = 0;
        if (validatePinned(s_frikBase, gameBase)) {
            slot = s_frikBase + kPinnedSlotRva;
            spdlog::info("[FRIK-GOAL] pinned-build validation passed (FRIK 0.76.12 lineage)");
        } else if (discoverSlot(s_frikBase, gameBase, slot)) {
            spdlog::info("[FRIK-GOAL] Tier-1 discovery located the U1PA slot at FRIK+0x{:X}",
                         slot - s_frikBase);
        } else {
            spdlog::warn("[FRIK-GOAL] no valid U1PA slot found in this FRIK build — degrading to legacy bone IK");
            return false;
        }

        s_slotAddr = slot;
        s_original = reinterpret_cast<U1PAFn>(readQword(slot));
        if (!s_original) {
            spdlog::warn("[FRIK-GOAL] slot read failed — degrading");
            return false;
        }
        if (!writeQword(slot, reinterpret_cast<std::uint64_t>(&FrikU1PAShim))) {
            spdlog::warn("[FRIK-GOAL] slot write failed — degrading");
            return false;
        }
        s_framesSinceInstall = 0;
        s_probePassed = false;
        s_probePrimarySeen.store(0, std::memory_order_relaxed);
        s_probeSecondarySeen.store(0, std::memory_order_relaxed);
        s_cleanTruthPublished.store(false, std::memory_order_relaxed);
        s_cleanTruthSeenMask.store(0, std::memory_order_relaxed);
        s_truthValid[0] = false;
        s_truthValid[1] = false;
        s_truthReachValid[0] = false;
        s_truthReachValid[1] = false;
        s_slotRecheckCountdown = 300;
        s_active.store(true, std::memory_order_release);
        spdlog::info("[FRIK-GOAL] INSTALLED — FRIK's own arm solver now consumes Heisenberg hand-authority targets (v5-equivalent seam)");
        return true;
    }

    void Uninstall()
    {
        s_cleanTruthPublished.store(false, std::memory_order_release);
        s_cleanTruthSeenMask.store(0, std::memory_order_relaxed);
        s_truthReachValid[0] = false;
        s_truthReachValid[1] = false;
        if (!s_active.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (s_slotAddr && s_original) {
            writeQword(s_slotAddr, reinterpret_cast<std::uint64_t>(s_original));
        }
        spdlog::info("[FRIK-GOAL] uninstalled (slot restored)");
    }

    bool IsActive()
    {
        return s_active.load(std::memory_order_acquire) && !s_disarmed.load(std::memory_order_acquire);
    }

    bool ConsumeCleanTruthPublished()
    {
        const bool published = s_cleanTruthPublished.exchange(false, std::memory_order_acq_rel);
        // Do not combine one arm pass from this frame with the other arm from a later frame.
        s_cleanTruthSeenMask.store(0, std::memory_order_relaxed);
        return published;
    }

    void OnFrame()
    {
        if (!s_active.load(std::memory_order_acquire)) {
            return;
        }
        if (s_disarmed.load(std::memory_order_acquire)) {
            Uninstall();
            return;
        }

        // Behavioral probe: within ~300 SKELETON-READY frames after install, FRIK must have
        // routed BOTH arm passes through the shim. Jul 19 fix: the counter previously ran
        // during the LOADING screen (kGameLoaded fires before FRIK's first arm solve), so
        // the probe expired at primary=0 secondary=0 and uninstalled a healthy seam. Count
        // only frames where the first-person skeleton exists (FRIK can actually be solving),
        // and on timeout WARN without uninstalling — the shim is passthrough-harmless when
        // never called, and ApplyWinners self-heals by applying legacy IK whenever the seam
        // is not consuming the latch.
        if (!s_probePassed) {
            if (!f4cf::f4vr::getFirstPersonSkeleton()) {
                return;
            }
            ++s_framesSinceInstall;
            const bool both = s_probePrimarySeen.load(std::memory_order_relaxed) > 0 &&
                              s_probeSecondarySeen.load(std::memory_order_relaxed) > 0;
            if (both) {
                s_probePassed = true;
                spdlog::info("[FRIK-GOAL] behavioral probe passed (primary={} secondary={})",
                             s_probePrimarySeen.load(), s_probeSecondarySeen.load());
            } else if (s_framesSinceInstall == 301) {
                spdlog::warn("[FRIK-GOAL] behavioral probe timeout (primary={} secondary={}) — seam stays installed (harmless passthrough); legacy IK covers unconsumed frames",
                             s_probePrimarySeen.load(), s_probeSecondarySeen.load());
            }
        }

        // Periodic slot self-check: nothing in the known build re-resolves the slot, but a
        // hostile writer would silently bypass us. Reinstall once; degrade on recurrence.
        if (--s_slotRecheckCountdown == 0) {
            s_slotRecheckCountdown = 300;
            const std::uint64_t cur = readQword(s_slotAddr);
            if (cur != reinterpret_cast<std::uint64_t>(&FrikU1PAShim)) {
                if (!s_reinstalledOnce) {
                    s_reinstalledOnce = true;
                    s_original = reinterpret_cast<U1PAFn>(cur);
                    writeQword(s_slotAddr, reinterpret_cast<std::uint64_t>(&FrikU1PAShim));
                    spdlog::warn("[FRIK-GOAL] slot was externally rewritten — re-installed once (new original 0x{:X})", cur);
                } else {
                    spdlog::warn("[FRIK-GOAL] slot rewritten AGAIN — permanent degrade to legacy bone IK");
                    s_disarmed.store(true, std::memory_order_release);
                    Uninstall();
                }
            }
        }
    }
}
