#pragma once

namespace rock
{
    void installBumpHook();
    bool installHavokTimingFixHook();
    void installNativeGrabHook();
    bool validateNativeMeleeSuppressionHookTargets();
    bool installNativeMeleeSuppressionHooks();
    void enforceNativeMeleeRuntimeSuppression(bool forceCheck = false);
    void enforceNativeGrabHapticRuntimeSuppression(bool forceCheck = false);
    void installRefreshManifoldHook();

    /*
     * Car fix (#219/#220). Drops the base-FormID -> "is a large blocking object"
     * memo used by the player character-controller contact policy. Call on world
     * change / engine shutdown; the memo itself is form-scoped and thread-safe.
     */
    void clearLargeBlockingObjectCache();

    void advanceNativeMeleeFrameClock();
    void clearNativeMeleePhysicalSwingLeases();
    void setNativeMeleePhysicalSwingActive(bool isLeft, bool active);
    bool isNativeMeleePhysicalSwingActive(bool isLeft);
}
