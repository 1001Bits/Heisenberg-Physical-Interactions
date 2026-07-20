#pragma once

namespace heisenberg::HandBumpHook
{
    // Install the HandleBumpedCharacter (FO4VR 0x1E24980) entry trampoline that
    // suppresses bumps against the player character-controller. This is the CTD guard
    // that makes ROCK-style physics hand bodies safe on F4VR: without it, a hand body
    // bumping the player char-proxy faults next frame. Validates the native prologue
    // bytes before patching; idempotent. Call once at hook-install time.
    // Returns true if installed.
    bool Install();

    // Runtime enable/disable of the suppression. When disabled the hook just chains the
    // original (no behavioural change), so it is safe to leave installed and toggle.
    // Enable only while physics hand bodies are active to avoid over-suppressing
    // legitimate char-controller bumps.
    void SetEnabled(bool enabled);

    bool IsInstalled();
}
