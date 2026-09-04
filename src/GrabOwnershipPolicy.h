#pragma once

#include <cstdint>

namespace heisenberg::grab_ownership_policy
{
    inline constexpr int kKeyframedGrabMode = 0;
    inline constexpr int kFullDynamicGrabMode = 9;

    struct Resolution
    {
        bool dynamicRequested = false;
        bool embeddedRockOwnsGrab = false;
        bool fellBackToKeyframed = false;
        int effectiveGrabMode = kKeyframedGrabMode;
    };

    enum class GripInputOwner : std::uint8_t
    {
        HostWorldGrab,
        HostActiveGrab,
        EmbeddedRockWorldGrab,
        EmbeddedRockWeaponSupport,
    };

    struct GripInputResolution
    {
        GripInputOwner owner = GripInputOwner::HostWorldGrab;
        // The host and ROCK sample the same physical controller independently.
        // When an active host grab consumes an edge, ROCK must discard its copy
        // once so it cannot re-grab the object in that same frame.
        bool consumeEmbeddedRockEdge = false;
    };

    enum class GripEdge : std::uint8_t
    {
        Pressed,
        Released,
    };

    [[nodiscard]] inline constexpr bool shouldReleaseActiveHostGrab(
        const bool stickyGrab,
        const GripEdge edge) noexcept
    {
        // Sticky placement is a toggle: the next press drops it. A
        // non-sticky programmatic grab follows normal hold semantics and must
        // survive its press until the corresponding release edge.
        return stickyGrab ?
            edge == GripEdge::Pressed :
            edge == GripEdge::Released;
    }

    /**
     * Decide whether an alternate A/X grab press belongs to Heisenberg rather
     * than Fallout's native action path.
     *
     * Pointing at an item is never input ownership. In particular, a
     * Grip->A controller binding must remain readable while either hand has a
     * selection. Durable ownership begins only once this hand is actually
     * grabbing, and remains through its short post-drop guard.
     */
    [[nodiscard]] inline constexpr bool shouldInterceptAlternateGrabPress(
        const bool sameHandGrabbing,
        const bool sameHandInPostDropBlock) noexcept
    {
        return sameHandGrabbing || sameHandInPostDropBlock;
    }

    /**
     * Resolve the single owner of grip selection and world-object grabbing.
     *
     * iGrabMode=9 is the canonical switch. bDelegateWorldGrabToRock predates
     * the embedded engine and used to address only an external ROCK.dll; keep
     * it as a compatibility alias while the embedded engine is hosted so an
     * existing "delegate=true" setup cannot silently run two different grab
     * architectures. If the embedded engine did not load, Heisenberg remains
     * the safe keyframed owner.
     */
    [[nodiscard]] inline constexpr Resolution resolve(
        const int configuredGrabMode,
        const bool legacyDelegateWorldGrabToRock,
        const bool embeddedRockHosted) noexcept
    {
        const bool dynamicRequested =
            configuredGrabMode == kFullDynamicGrabMode ||
            legacyDelegateWorldGrabToRock;
        const bool embeddedRockOwnsGrab =
            dynamicRequested && embeddedRockHosted;
        return Resolution{
            .dynamicRequested = dynamicRequested,
            .embeddedRockOwnsGrab = embeddedRockOwnsGrab,
            .fellBackToKeyframed =
                dynamicRequested && !embeddedRockHosted,
            .effectiveGrabMode = embeddedRockOwnsGrab
                ? kFullDynamicGrabMode
                : kKeyframedGrabMode,
        };
    }

    [[nodiscard]] inline constexpr bool embeddedRockOwnsGrab(
        const int configuredGrabMode,
        const bool legacyDelegateWorldGrabToRock,
        const bool embeddedRockHosted) noexcept
    {
        return resolve(
                   configuredGrabMode,
                   legacyDelegateWorldGrabToRock,
                   embeddedRockHosted)
            .embeddedRockOwnsGrab;
    }

    /**
     * Resolve an individual grip edge at the host/embedded-engine seam.
     *
     * ROCK owns ordinary world selection in full-dynamic mode, but items
     * deliberately placed in-hand by DropToHand, SmartGrab, cooking, or the
     * public API remain GrabManager objects.  An already-active host grab must
     * therefore keep its press/release edges until it has been released; ceding
     * those edges merely because mode 9 is enabled strands sticky items in the
     * hand and can also strand non-sticky programmatic grabs.
     */
    [[nodiscard]] inline constexpr GripInputResolution resolveGripInput(
        const int configuredGrabMode,
        const bool legacyDelegateWorldGrabToRock,
        const bool embeddedRockHosted,
        const bool hostGrabActive,
        const bool embeddedRockWeaponSupportEngaged) noexcept
    {
        if (hostGrabActive) {
            return GripInputResolution{
                .owner = GripInputOwner::HostActiveGrab,
                // Embedded ROCK's equipped-weapon input remains live even
                // when mode 0 keeps ordinary world grabbing in the host.
                // Every host-owned edge must therefore cross the seam while
                // the engine is hosted; ROCK decides independently whether
                // normal-grab input also needs suppressing.
                .consumeEmbeddedRockEdge = embeddedRockHosted,
            };
        }

        // An active host grab intentionally outranks this gate. Otherwise a
        // stale support-touch classification can strand a DropToHand item.
        if (embeddedRockHosted && embeddedRockWeaponSupportEngaged) {
            return GripInputResolution{
                .owner = GripInputOwner::EmbeddedRockWeaponSupport,
            };
        }

        return GripInputResolution{
            .owner = embeddedRockOwnsGrab(
                         configuredGrabMode,
                         legacyDelegateWorldGrabToRock,
                         embeddedRockHosted)
                ? GripInputOwner::EmbeddedRockWorldGrab
                : GripInputOwner::HostWorldGrab,
        };
    }

    [[nodiscard]] inline constexpr GripInputOwner resolveGripInputOwner(
        const int configuredGrabMode,
        const bool legacyDelegateWorldGrabToRock,
        const bool embeddedRockHosted,
        const bool hostGrabActive) noexcept
    {
        return resolveGripInput(
                   configuredGrabMode,
                   legacyDelegateWorldGrabToRock,
                   embeddedRockHosted,
                   hostGrabActive,
                   false)
            .owner;
    }
}
