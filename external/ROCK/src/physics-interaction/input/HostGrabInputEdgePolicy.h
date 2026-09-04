#pragma once

#include <cstdint>

namespace rock::host_grab_input_edge_policy
{
    enum class ConsumptionStage : std::uint8_t
    {
        BeforeWeaponOwnership,
        NormalGrab,
    };

    enum class Action : std::uint8_t
    {
        None,
        DiscardHostOwnedEdge,
        PreserveActiveRockOwner,
    };

    struct Decision
    {
        Action action = Action::None;
        bool discardEdge = false;
        bool suppressWeaponInputThisFrame = false;
        bool suppressNormalGrabInputThisFrame = false;
    };

    /**
     * A host-consumed edge is discarded only while ROCK has no active grab of
     * its own.  This prevents a just-released DropToHand item from being
     * acquired again in the chained ROCK update without ever starving the
     * release path of an existing ROCK or provider touch grab if an invariant
     * violation briefly presents two object owners. Equipped/support weapon
     * input is always masked: a host object's edge cannot detach or acquire a
     * weapon grip before normal-grab arbitration runs.
     */
    [[nodiscard]] inline constexpr Decision resolve(
        const bool hostConsumedEdge,
        const bool rockHeldObject,
        const bool rockTouchGrabActive) noexcept
    {
        if (!hostConsumedEdge) {
            return {};
        }
        if (rockHeldObject || rockTouchGrabActive) {
            return Decision{
                .action = Action::PreserveActiveRockOwner,
                .discardEdge = false,
                // Weapon input is a separate owner and must never see an edge
                // consumed by the host. Preserve the raw edge only for the
                // already-active ROCK/touch object release path below.
                .suppressWeaponInputThisFrame = true,
                .suppressNormalGrabInputThisFrame = false,
            };
        }
        return Decision{
            .action = Action::DiscardHostOwnedEdge,
            .discardEdge = true,
            .suppressWeaponInputThisFrame = true,
            .suppressNormalGrabInputThisFrame = true,
        };
    }

    [[nodiscard]] inline constexpr bool shouldDrainRawStateAtStage(
        const Decision& decision,
        const ConsumptionStage stage) noexcept
    {
        // The first possible ROCK consumer owns the only raw-state drain.
        // Later normal-grab suppression uses the frame mask and must never
        // consume a second time.
        return decision.discardEdge &&
               stage == ConsumptionStage::BeforeWeaponOwnership;
    }

    [[nodiscard]] inline constexpr bool
    shouldRetainPendingEdgeForNextConsumableFrame(
        const bool hostConsumedEdge,
        const bool inputConsumerFrameAvailable) noexcept
    {
        // The raw OpenVR/remap edge bits are persistent until a consumer
        // drains them. If ROCK cannot reach its first input boundary this
        // frame, retain the host ownership token alongside those bits so the
        // eventual consumer can never observe the stale edge unmasked.
        return hostConsumedEdge && !inputConsumerFrameAvailable;
    }

    [[nodiscard]] inline constexpr bool resolveWeaponGripHeldLevel(
        const bool hostInputMasked,
        const bool weaponGripAlreadyActive,
        const bool rawHeld) noexcept
    {
        // A masked host edge is neutral to weapon ownership: retain an
        // existing grip, but never use the host button level to acquire one.
        return hostInputMasked ? weaponGripAlreadyActive : rawHeld;
    }
}
