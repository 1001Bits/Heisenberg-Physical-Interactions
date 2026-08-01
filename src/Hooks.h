#pragma once

namespace heisenberg
{
    /**
     * Game function hooks for Heisenberg.
     */
    namespace Hooks
    {
        void Install();
        
        /**
         * Suppress HUD messages by temporarily disabling the bShowHUDMessages INI setting.
         * Call before ActivateRef to prevent "X was removed" message, then call with false
         * after to restore normal HUD message behavior.
         * 
         * This is much safer than hooking ShowHUDMessage, which caused crashes.
         * 
         * @param suppress true to disable HUD messages, false to restore
         */
        void SetSuppressHUDMessages(bool suppress);

        /**
         * Schedule deferred HUD unsuppress after N frames.
         * Use after ActivateRef to catch deferred native messages that fire on later frames.
         * Call SetSuppressHUDMessages(true) first, then this to auto-unsuppress later.
         */
        void ScheduleDeferredHUDUnsuppress(int frames = 3, const char* queuedMessage = nullptr);

        /**
         * Tick the deferred HUD unsuppress counter. Call once per frame.
         */
        void UpdateDeferredHUDUnsuppress();

        /**
         * Show a HUD message directly.
         * Uses the native ShowHUDMessage_VR function from F4VROffsets.
         * 
         * @param message The message to display
         * @param sound Sound to play (usually nullptr)
         * @param throttle Throttle rapid messages
         * @param warning Display as warning
         */
        void ShowHUDMessageDirect(const char* message, const char* sound = nullptr,
                                   bool throttle = false, bool warning = false);

        /**
         * Enable or disable grip weapon draw blocking.
         * When disabled, grip button "WandGrip" events are intercepted in
         * ReadyWeaponHandler::OnButtonEvent and skipped entirely.
         * Same approach as the STUF VR mod - full function hook at 0xFC9220.
         * Safe to call repeatedly — only updates a boolean flag.
         */
        void SetGripWeaponDrawDisabled(bool disabled);

        /**
         * Temporarily block ALL weapon draws (grip + trigger) in ReadyWeaponHandler.
         * Used after storage zone weapon unequip to prevent the game from re-equipping
         * the weapon on the next trigger press. Blocks for the specified number of frames.
         */
        void BlockAllWeaponDraws(int frames);

        /**
         * Tick the weapon draw block counter. Call once per frame.
         * Decrements the counter set by BlockAllWeaponDraws()
         * and the auto-equip block failsafe timer.
         */
        void TickWeaponDrawBlock();

        /**
         * Block the next weapon auto-equip in ActorEquipManager::EquipObject.
         * Used after storage zone weapon unequip so that the next trigger press
         * activates unarmed (fists) instead of re-equipping the last weapon.
         * One-shot: clears automatically after blocking one weapon equip,
         * or after a 10-second failsafe timeout.
         */
        void SetBlockWeaponAutoEquip(bool block);

        /**
         * Install the ReadyWeaponHandler::OnButtonEvent detour hook.
         * Must be called during Install() after trampoline allocation.
         */
        void InstallGripWeaponDrawHook();

        /**
         * Install the ActorEquipManager::EquipObject detour hook (0xE6FEA0).
         * Intercepts consumable equips from Pipboy/Favorites menus
         * and redirects them to drop-to-hand when configured.
         *
         * SOLE OWNER of that entry: a function entry can hold exactly one raw detour, and this
         * one is installed first (Hooks::Install runs before rock::HostLoad). The embedded ROCK
         * engine's loose-grenade equip interception is therefore CALLED from HookEquipObject
         * (rock::HostTryInterceptEquipObject) rather than hooked. Every exit from this function
         * reports its outcome via rock::HostSetEquipObjectDetourInstalled, which is the gate on
         * ROCK's pending-grenade-equip queue.
         */
        void InstallEquipObjectHook();

        /**
         * Apply or revert terminal-on-Pipboy binary patches.
         * Must be called AFTER config is loaded (not during Install()).
         */
        void ApplyTerminalPatches(bool enable);

        /**
         * Install vtable hooks on both HUDRollover instances (left/right wand)
         * to null out actionText at +0x618 before rendering.
         * This hides "[A] Take" / "[A] Store" prompts while keeping item names visible.
         */
        void InstallHUDRolloverHook();
        void InitRolloverStrings();
        void CheckHUDRolloverVtableIntegrity();

        /**
         * Recompute, on the main thread, whether each wand is pointing at a tracked activator
         * or terminal. The rollover vtable hook runs on the rendering thread and reads the
         * cached answer instead of walking ActivatorHandler's vectors, which the main thread
         * clears and rebuilds on every cell scan.
         */
        void RefreshWandActivatorTargets();

        /**
         * Apply or revert binary patches on ShowActivateButton (0xab7610)
         * and ShowSecondaryButton (0xab7700). Patches first byte with RET (0xC3)
         * to prevent "[A] Take/Store", "[Grab] Play Holotape" prompts.
         * Must be called AFTER config is loaded.
         */
        void ApplyHUDRolloverButtonPatches(bool enable);

        /**
         * Check if a holotape was recently redirected to hand (within last ~500ms).
         * Used to suppress terminal opening from the game's separate play-holotape path.
         */
        bool WasHolotapeJustRedirected();

        /**
         * Restore holotape type temporarily changed during redirect-to-hand.
         */
        void RestoreRedirectedHolotapeType();

        /**
         * Record that an object was just dropped (released from grab, not stored).
         * Used by ActivateRef hook to block immediate re-activation after drop.
         * @param formID The formID of the dropped TESObjectREFR
         */
        void RecordDroppedRef(uint32_t formID);

        /**
         * Clear the recently-dropped ring buffer.
         * Called on kPreLoadGame so stale form IDs from a save made shortly
         * after a drop cannot block activation in the next session.
         */
        void ClearRecentDrops();

        /**
         * Install ActivateRef detour hook.
         * Blocks player activation of recently-dropped objects (within ~0.5s)
         * to prevent Grip>A binding from immediately re-activating dropped items.
         */
        void InstallActivateRefHook();

        /**
         * Set/clear internal activation bypass.
         * When true, the ActivateRef hook allows the call through without
         * checking grab/selection state. Used by StoreGrabbedItem and other
         * internal code that intentionally calls ActivateRef on held items.
         */
        void SetInternalActivation(bool active);
    }
}
