#pragma once

// Minimal faithful subset of ROCK's RockConfig — ONLY the tuning fields the grab-core headers
// read via g_rockConfig, with ROCK's exact default values. We deliberately do NOT vendor ROCK's
// full 509-line RockConfig (its INI parser / hot-reload machinery is not wanted here). These
// fields can later be wired to Heisenberg's Config in the grab-core tuning phase; until then they
// hold ROCK's stock defaults so the ported math behaves identically out of the box.

#include "RE/NetImmerse/NiPoint.h"

#include <cstdint>

namespace heisenberg::rock_core
{
    // ODR FIX (embedded-engine architecture): this was `struct RockConfig` + `inline RockConfig
    // g_rockConfig;` — but the vendored engine (external/ROCK/src/RockConfig.h) ALSO defines
    // `rock::RockConfig` + `rock::g_rockConfig` (the real 500-field config). Once rock_engine is
    // linked, the two `rock::g_rockConfig` symbols collide. Because the engine's is a strong def
    // and this stub's was `inline` (weak/COMDAT), the linker merges them SILENTLY (no LNK2005),
    // so the wrong ctor (this Vec3-heavy stub) runs at init and the engine's resetToDefaults then
    // reads its std::string members out of Vec3 float data -> memcpy-to-null CTD on config load.
    // Renamed to GrabCoreConfig / g_grabCoreConfig so it can no longer collide. (The grab-core is
    // gated off; when it is wired up it can instead read the engine's rock::g_rockConfig, which
    // already carries all of these fields.)
    struct GrabCoreConfig
    {
        // Selection aim cones (HandFrame selection helpers). ROCK default = 0 degrees.
        int rockCloseSelectionAngleDegrees = 0;
        int rockFarSelectionAngleDegrees   = 0;

        // Grab angular motor tuning (GrabConstraintMath / GrabMotionController).
        float rockGrabAngularTau                 = 0.03f;
        float rockGrabAngularDamping             = 0.8f;
        float rockGrabAngularProportionalRecovery = 2.0f;
        float rockGrabAngularConstantRecovery    = 1.0f;

        // Inertia normalization (GrabInertiaPolicy / GrabConstraint).
        float rockGrabMaxInertiaRatio = 10.0f;
        float rockGrabMinInertia      = 0.01f;

        // Ragdoll atom decomposition mode (-1 = auto).
        int rockGrabRagdollDecompositionMode = -1;

        // Hand-space authored directions / pivots (HandFrame / GrabConstraintMath).
        RE::NiPoint3 rockGrabPinchDetectionDirectionHandspace = RE::NiPoint3(1.0f, 0.0f, 0.0f);
        RE::NiPoint3 rockPalmNormalHandspace                  = RE::NiPoint3(0.0f, 1.0f, 0.0f);
        RE::NiPoint3 rockPointingVectorHandspace              = RE::NiPoint3(0.0f, 1.0f, 0.0f);
        bool         rockReverseFarGrabNormal                 = true;
        bool         rockReversePalmNormal                    = true;

        RE::NiPoint3 rockLeftGrabLegacyPalmPivotAHandspace  = RE::NiPoint3(6.0f, -2.0f, -0.2f);
        RE::NiPoint3 rockRightGrabLegacyPalmPivotAHandspace = RE::NiPoint3(6.0f, -2.0f,  0.2f);
        RE::NiPoint3 rockLeftCustomOGAOffsetGameUnits       = RE::NiPoint3(0.0f, 0.0f, 0.0f);
        RE::NiPoint3 rockRightCustomOGAOffsetGameUnits      = RE::NiPoint3(0.0f, 0.0f, 0.0f);
        RE::NiPoint3 rockLeftGrabAuthorityProxyOffsetGameUnits  = RE::NiPoint3(0.0f, 0.0f, 0.0f);
        RE::NiPoint3 rockRightGrabAuthorityProxyOffsetGameUnits = RE::NiPoint3(0.0f, 0.0f, 0.0f);
    };

    inline GrabCoreConfig g_grabCoreConfig;
}
