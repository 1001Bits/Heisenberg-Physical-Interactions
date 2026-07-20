#pragma once

/*
 * Weapon interaction routing is grouped here so offhand reservation and semantic route selection stay together without becoming part of collision generation.
 */


// ---- WeaponInteractionRouter.h ----

#include "rock_integration/weapon/WeaponTypes.h"
#include "rock_integration/weapon/WeaponSupport.h"

namespace heisenberg::rock_core
{
    /*
     * Left-hand weapon behavior needs one authority. Generated layer 44 weapon
     * colliders can describe authored parts, but ROCK's live weapon interaction
     * is still one equipped weapon package, not independent child bodies. This
     * router therefore keeps normal semantic contacts usable for support grip.
     * PAPER consumes reload/socket/action evidence through the provider API and
     * owns all physical reload action decisions.
     */

    inline WeaponInteractionDecision routeWeaponInteraction(const WeaponInteractionContact& contact, const WeaponInteractionRuntimeState& runtimeState)
    {
        WeaponInteractionDecision decision{};
        if (!contact.valid) {
            return decision;
        }

        decision.partKind = contact.partKind;
        decision.reloadRole = contact.reloadRole;
        decision.socketRole = contact.socketRole;
        decision.actionRole = contact.actionRole;
        decision.gripPose = contact.fallbackGripPose;
        decision.reloadActionAuthority = contact.reloadActionAuthority;
        decision.bodyId = contact.bodyId;
        decision.interactionRoot = contact.interactionRoot;
        decision.sourceRoot = contact.sourceRoot;
        decision.weaponGenerationKey = contact.weaponGenerationKey;

        if (weapon_support_grip_policy::canUseContactForSupportGrip(contact, runtimeState)) {
            decision.kind = WeaponInteractionKind::SupportGrip;
            decision.gripPose = weapon_support_grip_policy::resolveSupportGripPose(contact);
            return decision;
        }

        decision.kind = WeaponInteractionKind::PassiveTouch;
        return decision;
    }
}
