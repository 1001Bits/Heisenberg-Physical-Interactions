#pragma once

namespace heisenberg::HavokTimingFix
{
    // Install the call-site hook on bhkWorld::SetDeltaTime (FO4VR 0x0D84BD0 → 0x1DF7120)
    // that overrides the engine's physics substep delta + count to enforce a minimum
    // physics frame rate (more substeps on long render frames → stable held-object / hand
    // physics at low FPS). 1:1 with ROCK's HAVOK_TIMING_FIX. Validates the call-site bytes
    // before patching; idempotent. Returns true if installed.
    bool Install();

    // Runtime enable/disable of the substep override. When disabled the hook just chains the
    // original (no behavioural change), so it is safe to leave installed and toggle.
    void SetEnabled(bool enabled);

    bool IsInstalled();
}
