#pragma once

#include "RE/NetImmerse/NiPoint.h"

namespace RE
{
    class Actor;
    class bhkCharacterController;
}

namespace rock::character_controller_runtime
{
    // Resolve the controller owned by this exact actor through the verified
    // FO4VR process layout. This deliberately does not use CommonLibF4VR's
    // MiddleHighProcessData::charController member: it is declared at the flat
    // FO4 +0x3E0 offset, while FO4VR stores the pointer at +0x3E8. Every native
    // pointer hop is null-gated and the complete walk is SEH-protected.
    RE::bhkCharacterController* tryGetActorCharacterController(RE::Actor* actor) noexcept;

    // Player-specialized convenience wrapper; preserves the actor resolver's
    // fail-closed behavior.
    RE::bhkCharacterController* tryGetPlayerCharacterController() noexcept;

    // Player locomotion velocity (character-controller cachedLinearVelocity), GAME UNITS.
    //
    // Reuses the actor resolver above, then reads +0x250
    // (cachedLinearVelocity), all Ghidra-verified against the FO4VR binary
    // (PlayerCharacter::GetLinearVelocity @ 140dc80f0 + helper 140ec70b0).
    //
    // Consumer: the grab-authority room-velocity feed-forward (one-substep target prediction at the
    // physics flush). This is a physics-clock signal read on the physics thread by design.
    bool tryGetPlayerLocomotionVelocityRawGameUnits(RE::NiPoint3& outVelocityGameUnits) noexcept;

    // Applies a short, signed game-space displacement through the verified
    // native controller API. This is game-frame-thread authority: callers
    // must not invoke it from a Havok between-step callback.
    bool tryApplyPlayerDisplacementModifierGameUnits(
        const RE::NiPoint3& displacementGameUnits,
        float durationSeconds) noexcept;
}
