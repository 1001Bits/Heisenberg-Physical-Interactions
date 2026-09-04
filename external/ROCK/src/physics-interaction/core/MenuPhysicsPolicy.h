#pragma once

namespace rock::menu_physics_policy
{
    struct Input
    {
        bool inputMenuBlocking = false;
        bool favoritesMenuOpen = false;
        bool pipboyMenuOpen = false;
        bool nonOverlayGameStoppingMenuOpen = false;
    };

    struct Decision
    {
        bool physicsMenuBlocking = false;
        bool gameplayInputBlocking = false;
    };

    /*
     * Favorites and the Pip-Boy are gameplay-transparent overlays: the physical
     * world, locomotion/input, hands, held items, grabbing, and the equipped
     * weapon all remain live. Any simultaneously open non-overlay stopping menu
     * still blocks both physics and gameplay input.
     */
    [[nodiscard]] inline constexpr Decision evaluate(const Input& input) noexcept
    {
        return Decision{
            .physicsMenuBlocking =
                input.nonOverlayGameStoppingMenuOpen ||
                (input.inputMenuBlocking &&
                    !input.favoritesMenuOpen &&
                    !input.pipboyMenuOpen),
            .gameplayInputBlocking =
                input.nonOverlayGameStoppingMenuOpen ||
                (input.inputMenuBlocking &&
                    !input.favoritesMenuOpen &&
                    !input.pipboyMenuOpen),
        };
    }
}
