#pragma once

/**
 * FrikArmGoalHook — "FRIK v5 behavior from within Heisenberg" (Jul 19, workflow-designed).
 *
 * Old FRIK (0.76.x public lineage) resolves the game-native arm solver
 * Update1StPersonArm (F4VR 1.2.72 REL::Offset 0xef6280) once at static-init into a
 * writable REL::Relocation storage qword inside FRIK.dll, and calls it through that
 * slot from exactly ONE site inside Skeleton::setArms. setArms then reads its IK goal
 * from the first-person skeleton hand node ([LR]Arm_Hand->world) that the native call
 * just positioned, and runs FRIK's own full arm solve toward it (shoulder IK,
 * law-of-cosines elbow, bone stretch, twist smoothing).
 *
 * The unreleased "FRIK v5" builds implement applyExternalHandWorldTransform by
 * resetting the arm chain and re-running that same solver with the caller's world
 * target. We reproduce the identical result on the old build by swapping the slot
 * pointer with a shim that (1) calls the REAL native solver first — the goal node is
 * recomputed from the controller on every call, so no state of ours ever feeds back —
 * and (2) when Heisenberg's HandAuthority has an active winner for that hand,
 * overwrites the goal node's world with the authority target before returning.
 * FRIK's own solver then carries the whole arm to our target, natively.
 *
 * Guards: byte-asserted pinned RVAs for the known build, a Tier-1 pattern scan for
 * public-lineage drift, a decisive semantic check (slot value == F4VRbase+0xef6280),
 * a post-install behavioral probe (both offset nodes must be observed within ~300
 * frames or we auto-uninstall), a periodic slot self-check, and SEH passthrough with
 * one-shot disarm. Degrade path is always HandAuthority's legacy post-FRIK bone IK.
 * If the loaded FRIK reports API >= 5, everything stands down (native path exists).
 */

#include <cstdint>

namespace heisenberg::FrikArmGoalHook
{
    // Attempt installation (idempotent). Returns true if the goal-swap seam is live.
    bool Install();

    // Restore the saved slot pointer (safe to call repeatedly).
    void Uninstall();

    // True while the seam is installed and not disarmed.
    bool IsActive();

    // HookPostPhysics fallback seam: true once when the U1PA clean pass published
    // controller-truth hands since the previous query. Consuming this prevents the
    // post-authority skinned hand from overwriting the loop-free input snapshot.
    bool ConsumeCleanTruthPublished();

    // Per-frame housekeeping: behavioral probe evaluation + periodic slot self-check.
    // Called from HandAuthority::ApplyWinners (host post-physics tail, game thread).
    void OnFrame();
}
