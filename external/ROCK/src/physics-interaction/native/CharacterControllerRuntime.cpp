#include "physics-interaction/native/CharacterControllerRuntime.h"
#include "physics-interaction/native/CharacterControllerLayoutPolicy.h"
#include "physics-interaction/native/HavokOffsets.h"

#include "RE/Bethesda/PlayerCharacter.h"
#include "REL/Relocation.h"

#include <cmath>
#include <cstdint>

#include <windows.h>

namespace rock::character_controller_runtime
{
    namespace
    {
        using SetVelocityModifier = void(__fastcall*)(
            RE::bhkCharacterController*, const float*, float);
        REL::Relocation<SetVelocityModifier> s_setVelocityModifier{
            REL::Offset(
                offsets::
                    kFunc_CharacterController_SetVelocityModifier)
        };
    }

    RE::bhkCharacterController* tryGetActorCharacterController(RE::Actor* actor) noexcept
    {
        RE::bhkCharacterController* controller = nullptr;

        __try {
            controller = static_cast<RE::bhkCharacterController*>(
                character_controller_layout::
                    resolveActorCharacterControllerUnchecked(actor));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            controller = nullptr;
        }

        return controller;
    }

    RE::bhkCharacterController* tryGetPlayerCharacterController() noexcept
    {
        return tryGetActorCharacterController(RE::PlayerCharacter::GetSingleton());
    }

    bool tryGetPlayerLocomotionVelocityRawGameUnits(RE::NiPoint3& outVelocityGameUnits) noexcept
    {
        outVelocityGameUnits = RE::NiPoint3{};
        bool ok = false;

        __try {
            const auto* charController = tryGetPlayerCharacterController();
            if (charController) {
                const auto* ccBytes =
                    reinterpret_cast<const std::uint8_t*>(charController);
                constexpr auto velocityOffset =
                    character_controller_layout::
                        kCharacterControllerLinearVelocityOffset;
                const float vx = *reinterpret_cast<const float*>(
                    ccBytes + velocityOffset);
                const float vy = *reinterpret_cast<const float*>(
                    ccBytes + velocityOffset + sizeof(float));
                const float vz = *reinterpret_cast<const float*>(
                    ccBytes + velocityOffset + 2 * sizeof(float));
                if (std::isfinite(vx) &&
                    std::isfinite(vy) &&
                    std::isfinite(vz)) {
                    outVelocityGameUnits.x = vx;
                    outVelocityGameUnits.y = vy;
                    outVelocityGameUnits.z = vz;
                    ok = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }

        return ok;
    }

    bool tryApplyPlayerDisplacementModifierGameUnits(
        const RE::NiPoint3& displacementGameUnits,
        float durationSeconds) noexcept
    {
        if (!std::isfinite(displacementGameUnits.x) ||
            !std::isfinite(displacementGameUnits.y) ||
            !std::isfinite(displacementGameUnits.z) ||
            !std::isfinite(durationSeconds) ||
            durationSeconds <= 0.0f) {
            return false;
        }

        bool applied = false;
        __try {
            auto* controller = tryGetPlayerCharacterController();
            if (controller && s_setVelocityModifier.address() != 0) {
                // FO4VR 1.2.72 begins this routine with
                // `movaps xmm5, xmmword ptr [rdx]`: the native argument must
                // be 16-byte aligned and expose a readable fourth float.
                // NiPoint3 is only three floats and carries no such alignment
                // guarantee, so never pass its address across this seam.
                alignas(16) const float nativeDisplacement[4]{
                    displacementGameUnits.x,
                    displacementGameUnits.y,
                    displacementGameUnits.z,
                    0.0f,
                };
                s_setVelocityModifier(
                    controller,
                    nativeDisplacement,
                    durationSeconds);
                applied = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            applied = false;
        }
        return applied;
    }
}
