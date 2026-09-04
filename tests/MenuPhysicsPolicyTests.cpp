#include "physics-interaction/core/MenuPhysicsPolicy.h"

#include <cstdlib>
#include <iostream>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using rock::menu_physics_policy::evaluate;

    const auto gameplay = evaluate({});
    Require(!gameplay.physicsMenuBlocking && !gameplay.gameplayInputBlocking,
        "ordinary gameplay must leave physics and input enabled");

    const auto favorites = evaluate({
        .inputMenuBlocking = true,
        .favoritesMenuOpen = true,
    });
    Require(!favorites.physicsMenuBlocking && !favorites.gameplayInputBlocking,
        "Favorites must leave physics and gameplay input enabled");

    const auto pipboy = evaluate({
        .inputMenuBlocking = true,
        .pipboyMenuOpen = true,
    });
    Require(!pipboy.physicsMenuBlocking && !pipboy.gameplayInputBlocking,
        "the Pip-Boy must leave physics and gameplay input enabled");

    const auto inventory = evaluate({
        .inputMenuBlocking = true,
        .nonOverlayGameStoppingMenuOpen = true,
    });
    Require(inventory.physicsMenuBlocking && inventory.gameplayInputBlocking,
        "ordinary stopping menus must block both physics writes and input");

    const auto overlappingMenu = evaluate({
        .inputMenuBlocking = true,
        .favoritesMenuOpen = true,
        .nonOverlayGameStoppingMenuOpen = true,
    });
    Require(overlappingMenu.physicsMenuBlocking && overlappingMenu.gameplayInputBlocking,
        "Favorites must not exempt a simultaneously open stopping menu");

    const auto pipboyOverlappingMenu = evaluate({
        .inputMenuBlocking = true,
        .pipboyMenuOpen = true,
        .nonOverlayGameStoppingMenuOpen = true,
    });
    Require(pipboyOverlappingMenu.physicsMenuBlocking &&
            pipboyOverlappingMenu.gameplayInputBlocking,
        "the Pip-Boy must not exempt a simultaneously open stopping menu");

    const auto bothOverlays = evaluate({
        .inputMenuBlocking = true,
        .favoritesMenuOpen = true,
        .pipboyMenuOpen = true,
    });
    Require(!bothOverlays.physicsMenuBlocking &&
            !bothOverlays.gameplayInputBlocking,
        "Favorites and Pip-Boy together must remain gameplay-transparent");

    const auto inputContextOnly = evaluate({
        .inputMenuBlocking = true,
    });
    Require(inputContextOnly.physicsMenuBlocking && inputContextOnly.gameplayInputBlocking,
        "an unclassified blocking input context must fail closed for physics");

    std::cout << "MenuPhysicsPolicyTests passed\n";
    return 0;
}
