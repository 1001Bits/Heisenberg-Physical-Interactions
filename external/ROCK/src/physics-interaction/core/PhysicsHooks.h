#pragma once

#include <cstdint>

namespace rock
{
    void installBumpHook();

    /*
     * True only when the HandleBumpedCharacter entry hook is actually in place.
     * This is the truth source for "is ROCK suppressing player character-controller
     * bumps?" - NOT rockNativeCharacterControllerObjectContactFilterEnabled, which
     * additionally drives the CC-vs-object filter, the CHARCONTROLLER layer-matrix
     * rewrite and the large-object player block.
     */
    bool isNativeCharacterBumpFilterInstalled();

    bool installHavokTimingFixHook();
    // Idempotent shared owner for the module+0xF19250 native VR-grab
    // suppression patch. Returns the verified final suppression state.
    bool installNativeGrabHook();
    bool validateNativeMeleeSuppressionHookTargets();
    bool installNativeMeleeSuppressionHooks();
    void enforceNativeMeleeRuntimeSuppression(bool forceCheck = false);
    void enforceNativeGrabHapticRuntimeSuppression(bool forceCheck = false);
    void installRefreshManifoldHook();

    void advanceNativeMeleeFrameClock();
    void clearNativeMeleePhysicalSwingLeases();
    void setNativeMeleePhysicalSwingActive(bool isLeft, bool active);
    bool isNativeMeleePhysicalSwingActive(bool isLeft);
}
