#include "physics-interaction/native/CharacterControllerRuntime.h"
#include "physics-interaction/native/CharacterControllerLayoutPolicy.h"

#include "RE/Bethesda/PlayerCharacter.h"

#include <cmath>
#include <cstdint>

#include <windows.h>

namespace rock::character_controller_runtime
{
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
}
