#pragma once

#include "VRInput.h"

/**
 * F4VR-specific offsets and function relocations for Heisenberg.
 * Based on F4VRCommonFramework's F4VROffsets.h
 *
 * These offsets are for Fallout 4 VR version 1.2.72
 */

namespace heisenberg
{
    /**
     * Get the bhkWorld from a TESObjectCELL.
     * This is essential for performing Havok physics queries.
     */
    using _TESObjectCell_GetbhkWorld = RE::bhkWorld*(*)(RE::TESObjectCELL* a_cell);
    inline REL::Relocation<_TESObjectCell_GetbhkWorld> TESObjectCell_GetbhkWorld{ REL::Offset(0x39b070) };

    /**
     * Set the motion type of a collision object.
     * @param root The NiAVObject with collision
     * @param preset The motion preset (Dynamic, Keyframed, etc.)
     * @param bool1-bool3 Various flags
     */
    using _bhkWorld_SetMotion = void(*)(RE::NiAVObject* root, RE::hknpMotionPropertiesId::Preset preset, bool bool1, bool bool2, bool bool3);
    inline REL::Relocation<_bhkWorld_SetMotion> bhkWorld_SetMotion{ REL::Offset(0x1df95b0) };

    /**
     * Remove a collision object from the world.
     */
    using _bhkWorld_RemoveObject = void(*)(RE::NiAVObject* root, bool a_bool, bool a_bool2);
    inline REL::Relocation<_bhkWorld_RemoveObject> bhkWorld_RemoveObject{ REL::Offset(0x1df9480) };

    /**
     * Add a collision object to the world.
     */
    using _bhkNPCollisionObject_AddToWorld = void(*)(RE::bhkNPCollisionObject* a_obj, RE::bhkWorld* a_world);
    inline REL::Relocation<_bhkNPCollisionObject_AddToWorld> bhkNPCollisionObject_AddToWorld{ REL::Offset(0x1e07be0) };

    /**
     * bhkPhysicsSystem::AddToWorld() - Re-adds the physics system to its OWN world.
     * Unlike bhkNPCollisionObject_AddToWorld, this doesn't require specifying the world.
     * The physics system already knows which world it belongs to.
     * CRITICAL for interior cells with multiple physics worlds (room bounds system).
     */
    using _bhkPhysicsSystem_AddToWorld = void(__fastcall*)(void* thisPhysicsSystem);
    inline REL::Relocation<_bhkPhysicsSystem_AddToWorld> bhkPhysicsSystem_AddToWorld{ REL::Offset(0x1e0c580) };

    /**
     * Set a collision layer on an object.
     */
    using _bhkUtilFunctions_SetLayer = void(*)(RE::NiAVObject* root, std::uint32_t collisionLayer);
    inline REL::Relocation<_bhkUtilFunctions_SetLayer> bhkUtilFunctions_SetLayer{ REL::Offset(0x1e16180) };

    /**
     * Move first collision object to root node.
     */
    using _bhkUtilFunctions_MoveFirstCollisionObjectToRoot = void(*)(RE::NiAVObject* root, RE::NiAVObject* child);
    inline REL::Relocation<_bhkUtilFunctions_MoveFirstCollisionObjectToRoot> bhkUtilFunctions_MoveFirstCollisionObjectToRoot{ REL::Offset(0x1e17050) };

    /**
     * Initialize Havok for a collision object on a reference.
     */
    using _TESObjectREFR_InitHavokForCollisionObject = void(*)(RE::TESObjectREFR* a_refr);
    inline REL::Relocation<_TESObjectREFR_InitHavokForCollisionObject> TESObjectREFR_InitHavokForCollisionObject{ REL::Offset(0x3eee60) };

    /**
     * NiAVObject::Update - Updates the scene graph node transforms.
     * Call this after reparenting nodes to ensure visibility/transforms are correct.
     * VR offset: 0x1c22fb0 - Status 2 from fo4_database.csv
     * 
     * @param node The NiAVObject to update
     * @param updateData Update context (can use default NiUpdateData)
     */
    using _NiAVObject_Update = void(*)(RE::NiAVObject* node, RE::NiUpdateData* updateData);
    inline REL::Relocation<_NiAVObject_Update> NiAVObject_Update{ REL::Offset(0x1c22fb0) };

    /**
     * NiNode::UpdateWorldBound - Updates the world bounding sphere for a node.
     * CRITICAL for preventing culling when moving objects - the renderer uses
     * worldBound to determine if objects are visible.
     * VR offset: 0x1c18ab0 - from F4VRCommonFramework
     * 
     * @param node The NiNode whose worldBound should be updated
     */
    using _NiNode_UpdateWorldBound = void(*)(RE::NiNode* node);
    inline REL::Relocation<_NiNode_UpdateWorldBound> NiNode_UpdateWorldBound{ REL::Offset(0x1c18ab0) };

    /**
     * BSFadeNode::MergeWorldBounds - Merges child bounding volumes into parent.
     * Use this for BSFadeNode types (most world objects).
     * VR offset: 0x27a9930 - from F4VRCommonFramework
     */
    using _BSFadeNode_MergeWorldBounds = void*(*)(RE::NiNode* node);
    inline REL::Relocation<_BSFadeNode_MergeWorldBounds> BSFadeNode_MergeWorldBounds{ REL::Offset(0x27a9930) };

    /**
     * Activate a reference (pick up items, open doors, etc.)
     * This is the native function that handles item pickup, door opening, etc.
     * VR offset: 0x3f4a60 - Status 4 (Verified) from fo4_database.csv
     * 
     * @param refr The object to activate (e.g., weapon on ground)
     * @param activator The actor doing the activation (e.g., player)
     * @param objectToGet Optional specific object to get (usually nullptr)
     * @param count Number of items to get (usually 1)
     * @param defaultProcessingOnly If true, only default processing
     * @param fromScript If called from script
     * @param looping If looping activation
     * @return true if activation succeeded
     */
    using _TESObjectREFR_ActivateRef = bool(*)(RE::TESObjectREFR* refr, RE::TESObjectREFR* activator, 
                                                RE::TESBoundObject* objectToGet, std::int32_t count,
                                                bool defaultProcessingOnly, bool fromScript, bool looping);
    inline REL::Relocation<_TESObjectREFR_ActivateRef> TESObjectREFR_ActivateRef{ REL::Offset(0x3f4a60) };

    // =========================================================================
    // WEAPON EQUIPPING
    // =========================================================================
    
    /**
     * BGSObjectInstance constructor
     * Creates an object instance for use with equip functions.
     * VR offset: 0x2dd930 - Status 4 (Verified) from fo4_database.csv
     */
    using _BGSObjectInstance_ctor = RE::BGSObjectInstance*(*)(RE::BGSObjectInstance* instance, 
                                                               RE::TESForm* object, 
                                                               RE::TBO_InstanceData* instanceData);
    inline REL::Relocation<_BGSObjectInstance_ctor> BGSObjectInstance_ctor{ REL::Offset(0x2dd930) };

    /**
     * GetEquippedWeapon - Get the currently equipped weapon for an actor
     * VR offset: 0x140d790 - from VirtualHolsters/VirtualReloads source
     * Returns nullptr if no weapon is equipped (Unarmed)
     * 
     * @param actor The actor to check
     * @param equipIndex 0 for primary weapon
     * @return TESObjectWEAP* or nullptr if unarmed
     */
    using _GetEquippedWeapon = RE::TESObjectWEAP*(*)(RE::Actor* actor, std::uint64_t equipIndex);
    inline REL::Relocation<_GetEquippedWeapon> GetEquippedWeapon{ REL::Offset(0x140d790) };

    // =========================================================================
    // AMMO & RELOAD FUNCTIONS (from Virtual Reloads VR)
    // =========================================================================

    /**
     * Actor_GetCurrentAmmoCount - Get the current ammo count in magazine
     * VR offset: 0xddf690 - from Virtual Reloads
     * 
     * @param actor The actor to check
     * @param equipIndex The equip index (from BGSEquipIndex)
     * @return Current ammo count in the magazine
     */
    using _Actor_GetCurrentAmmoCount = float(*)(RE::Actor* actor, RE::BGSEquipIndex equipIndex);
    inline REL::Relocation<_Actor_GetCurrentAmmoCount> Actor_GetCurrentAmmoCount{ REL::Offset(0xddf690) };

    /**
     * Actor_SetCurrentAmmoCount - Set the current ammo count in magazine
     * VR offset: 0xddf790 - from Virtual Reloads
     * 
     * @param actor The actor to set ammo for
     * @param equipIndex The equip index (from BGSEquipIndex)
     * @param count New ammo count to set in magazine
     */
    using _Actor_SetCurrentAmmoCount = void(*)(RE::Actor* actor, RE::BGSEquipIndex equipIndex, int count);
    inline REL::Relocation<_Actor_SetCurrentAmmoCount> Actor_SetCurrentAmmoCount{ REL::Offset(0xddf790) };

    /**
     * Actor_GetAmmoClipPercentage - Get the clip fill percentage (0.0 to 1.0)
     * VR offset: 0xddf6c0 - from Virtual Reloads
     * 
     * @param actor The actor to check
     * @param equipIndex The equip index
     * @return Percentage of magazine filled (0.0 to 1.0)
     */
    using _Actor_GetAmmoClipPercentage = float(*)(RE::Actor* actor, RE::BGSEquipIndex equipIndex);
    inline REL::Relocation<_Actor_GetAmmoClipPercentage> Actor_GetAmmoClipPercentage{ REL::Offset(0xddf6c0) };

    /**
     * Actor_GetWeaponEquipIndex - Get the equip index for a weapon
     * VR offset: 0xe50e70 - from Virtual Reloads
     * 
     * @param actor The actor
     * @param outEquipIndex Output parameter for the equip index
     * @param instance The weapon instance
     */
    using _Actor_GetWeaponEquipIndex = void(*)(RE::Actor* actor, RE::BGSEquipIndex* outEquipIndex, RE::BGSObjectInstance* instance);
    inline REL::Relocation<_Actor_GetWeaponEquipIndex> Actor_GetWeaponEquipIndex{ REL::Offset(0xe50e70) };

    /**
     * ActorEquipManager singleton pointer
     * VR offset: 0x5a38bf8 - Status 2 from fo4_database.csv
     */
    inline REL::Relocation<RE::ActorEquipManager**> g_ActorEquipManager{ REL::Offset(0x5a38bf8) };

    /**
     * ActorEquipManager::EquipObject
     * Equips an object to an actor.
     * VR offset: 0xe6fea0 - Status 4 (Verified) from fo4_database.csv
     * 
     * @param equipManager The ActorEquipManager singleton
     * @param actor The actor to equip the object on
     * @param instance The BGSObjectInstance to equip
     * @param stackID Stack ID for inventory tracking (use 0 for most cases)
     * @param number Number of items (usually 1)
     * @param slot The equip slot (from weapon->GetEquipSlot())
     * @param queueEquip Whether to queue the equip
     * @param forceEquip Force equip even if conditions not met
     * @param playSounds Play equip sounds
     * @param applyNow Apply immediately
     * @param locked Whether slot is locked
     */
    using _ActorEquipManager_EquipObject = bool(*)(RE::ActorEquipManager* equipManager,
                                                    RE::Actor* actor,
                                                    RE::BGSObjectInstance* instance,
                                                    std::uint32_t stackID,
                                                    std::uint32_t number,
                                                    RE::BGSEquipSlot* slot,
                                                    bool queueEquip,
                                                    bool forceEquip,
                                                    bool playSounds,
                                                    bool applyNow,
                                                    bool locked);
    inline REL::Relocation<_ActorEquipManager_EquipObject> ActorEquipManager_EquipObject{ REL::Offset(0xe6fea0) };

    /**
     * ActorEquipManager::UnequipObject
     * Unequips an object from an actor.
     * VR offset: 0xe70280 - Status 4 (Verified) from fo4_database.csv
     * 
     * @param equipManager The ActorEquipManager singleton (as uint64_t for ABI compatibility)
     * @param actor The actor to unequip from
     * @param instance The BGSObjectInstance to unequip
     * @param number Number of items (usually 1)
     * @param slot The equip slot (nullptr to auto-detect)
     * @param stackID Stack ID for inventory tracking (-1 for any)
     * @param queueEquip Whether to queue the unequip
     * @param playSounds Play unequip sounds
     * @param applyNow Apply immediately
     * @param locked Whether slot is locked
     * @param slotBeingReplaced Slot being replaced (nullptr usually)
     */
    using _ActorEquipManager_UnequipObject = void(*)(std::uint64_t equipManager,
                                                      RE::Actor* actor,
                                                      RE::BGSObjectInstance* instance,
                                                      std::uint32_t number,
                                                      RE::BGSEquipSlot* slot,
                                                      std::int32_t stackID,
                                                      bool queueEquip,
                                                      bool playSounds,
                                                      bool applyNow,
                                                      bool locked,
                                                      RE::BGSEquipSlot* slotBeingReplaced);
    inline REL::Relocation<_ActorEquipManager_UnequipObject> ActorEquipManager_UnequipObject{ REL::Offset(0xe70280) };

    /**
     * UnEquipItem - Simple unequip by form.
     * Unequips a TESForm from an actor without needing BGSObjectInstance.
     * VR offset: 0xe707b0 - Used by Virtual Holsters VR mod.
     * 
     * @param actor The actor to unequip from
     * @param form The TESForm (weapon, armor, etc.) to unequip
     * @return true if unequipped successfully
     */
    using _UnEquipItem = bool(*)(RE::Actor* actor, RE::TESForm* form);
    inline REL::Relocation<_UnEquipItem> UnEquipItem{ REL::Offset(0xe707b0) };

    /**
     * Actor::IsInActiveCombat - Stricter combat check than virtual IsInCombat().
     * Returns false if IsInCombat() is true but the combat group has already ended
     * (ended flag at combat group +0xE1). Use this instead of IsInCombat() to avoid
     * false positives from stale/ended combat groups.
     * VR offset: 0xe50350 - Status 4 (Verified) - ID 84790
     */
    using _IsInActiveCombat = bool(*)(RE::Actor* actor);
    inline REL::Relocation<_IsInActiveCombat> IsInActiveCombat{ REL::Offset(0xe50350) };

    /**
     * Actor::DrinkPotion
     * Makes an actor drink/consume an AlchemyItem (potion, food, drink, chem).
     * This properly triggers all effects and removes/consumes the item.
     * VR offset: 0xe666d0 - Status 4 (Verified) from fo4_database.csv
     * 
     * @param actor The actor consuming the item
     * @param alchemyItem The AlchemyItem to consume
     * @param count Number to consume (usually 1)
     * @return true if consumed successfully
     */
    using _Actor_DrinkPotion = bool(*)(RE::Actor* actor, RE::AlchemyItem* alchemyItem, std::uint32_t count);
    inline REL::Relocation<_Actor_DrinkPotion> Actor_DrinkPotion{ REL::Offset(0xe666d0) };

    /**
     * PlaySoundAtActor - Plays a sound descriptor at an actor's location.
     * VR offset: 0x30f9f0 (from Virtual Reloads VR mod)
     * 
     * @param sound The BGSSoundDescriptorForm to play
     * @param actor The actor at whose location to play the sound
     */
    using _PlaySoundAtActor = void(*)(RE::BGSSoundDescriptorForm* sound, RE::TESObjectREFR* actor);
    inline REL::Relocation<_PlaySoundAtActor> PlaySoundAtActor{ REL::Offset(0x30f9f0) };

    /**
     * TESObjectREFR::AddObjectToContainer (virtual function)
     * VR offset: N/A (use virtual call)
     * Can also use the direct function at 0x03e9e90 if needed
     */
    using _AddObjectToContainer = void(*)(RE::TESObjectREFR* container, RE::TESBoundObject* obj, 
                                          RE::BSTSmartPointer<RE::ExtraDataList>* extra, 
                                          std::int32_t count, RE::TESObjectREFR* oldContainer, 
                                          std::uint32_t reason);
    inline REL::Relocation<_AddObjectToContainer> AddObjectToContainer{ REL::Offset(0x3e9e90) };

    /**
     * BSUtilities::SetAlwaysDraw - Forces an object to always be rendered.
     * This bypasses all culling checks for the object.
     * VR offset: 0x1d13710
     * 
     * @param node The NiAVObject to set always draw on
     * @param alwaysDraw True to enable always draw, false to disable
     */
    using _BSUtilities_SetAlwaysDraw = void(*)(RE::NiAVObject* node, bool alwaysDraw);
    inline REL::Relocation<_BSUtilities_SetAlwaysDraw> BSUtilities_SetAlwaysDraw{ REL::Offset(0x1d13710) };

    /**
     * Actor::GetCurrentWeapon - Gets the currently equipped weapon for an actor
     * VR offset: 0xe50da0 (from VirtualHolsters/FRIK)
     * 
     * @param actor The actor to check
     * @param outWeapon Output parameter - receives the weapon pointer
     * @param equipIndex The equip slot index to check (use index 0 for primary weapon)
     * @return The TESObjectWEAP* if equipped, nullptr otherwise
     */
    using _Actor_GetCurrentWeapon = RE::TESObjectWEAP*(*)(RE::Actor* actor, RE::TESObjectWEAP* outWeapon, RE::BGSEquipIndex equipIndex);
    inline REL::Relocation<_Actor_GetCurrentWeapon> Actor_GetCurrentWeapon{ REL::Offset(0xe50da0) };

    /**
     * @brief Check if activating a reference would be a crime (stealing)
     * @param refr The reference to check
     * @return true if picking up this item would be stealing
     */
    using _TESObjectREFR_IsCrimeToActivate = bool(*)(RE::TESObjectREFR* refr);
    inline REL::Relocation<_TESObjectREFR_IsCrimeToActivate> TESObjectREFR_IsCrimeToActivate{ REL::Offset(0x3f8450) };

    // ========================================================================
    // HUD Messages - VR-specific wrapper
    // ========================================================================
    // RE::SendHUDMessage::ShowHUDMessage uses REL::ID which requires the VR
    // Address Library. This wrapper uses REL::Offset directly for reliability.
    
    /**
     * ShowHUDMessage - Display a message on the HUD (like "Item was stored")
     * VR offset: 0x0afbe30 (ID 1163005)
     * 
     * @param message The message text to display
     * @param sound Optional sound to play (usually nullptr)
     * @param throttle If true, throttle rapid messages
     * @param warning If true, display as warning style
     */
    using _ShowHUDMessage = void(*)(const char* message, const char* sound, bool throttle, bool warning);
    inline REL::Relocation<_ShowHUDMessage> ShowHUDMessage_VR{ REL::Offset(0x0afbe30) };
    
    /**
     * Helper function to show a HUD message with default parameters
     */
    inline void ShowHUDMessage(const char* message)
    {
        if (message) {
            ShowHUDMessage_VR(message, nullptr, false, false);
        }
    }

    // ========================================================================
    // SubtitleManager - Native subtitle bar at bottom of screen
    // ========================================================================
    // Used by the game for NPC dialogue and holotape subtitles.
    // Reverse-engineered via Ghidra from FO4VR 1.2.72.
    
    /**
     * SubtitleManager singleton pointer
     * Points to the static-buffer singleton instance.
     */
    inline REL::Relocation<void**> SubtitleManager_Singleton{ REL::Offset(0x5932568) };

    /**
     * SubtitleManager::ShowSubtitle(TESObjectREFR* speaker, BSFixedStringCS& text, TESTopicInfo* topicInfo, bool highPriority)
     * Adds a subtitle entry to the subtitle array. The text appears in the
     * standard subtitle bar at the bottom of the screen.
     * 
     * @param thisPtr  SubtitleManager singleton
     * @param speaker  The "speaking" reference (use player for custom text)
     * @param text     BSFixedString with the subtitle text
     * @param topicInfo Optional TESTopicInfo* (nullptr for custom subtitles)
     * @param highPriority If true, uses priority 2 instead of 1
     */
    using _SubtitleManager_ShowSubtitle = void(*)(void* thisPtr, RE::TESObjectREFR* speaker,
        const RE::BSFixedString& text, void* topicInfo, bool highPriority);
    inline REL::Relocation<_SubtitleManager_ShowSubtitle> SubtitleManager_ShowSubtitle{ REL::Offset(0x1330400) };

    /**
     * SubtitleManager::HideSubtitle(TESObjectREFR* speaker)
     * Removes subtitle entries for the given speaker.
     * 
     * @param thisPtr  SubtitleManager singleton
     * @param speaker  The reference whose subtitles to remove
     */
    using _SubtitleManager_HideSubtitle = void(*)(void* thisPtr, RE::TESObjectREFR* speaker);
    inline REL::Relocation<_SubtitleManager_HideSubtitle> SubtitleManager_HideSubtitle{ REL::Offset(0x13306b0) };

    /**
     * Show a native subtitle on the subtitle bar.
     * Uses the player as the speaker.
     */
    inline void ShowSubtitle(const char* text)
    {
        if (!text) return;
        void** singletonPtr = SubtitleManager_Singleton.get();
        if (!singletonPtr || !*singletonPtr) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        RE::BSFixedString bsText(text);
        SubtitleManager_ShowSubtitle(*singletonPtr, player, bsText, nullptr, false);
    }

    /**
     * Hide the current subtitle for the player.
     */
    inline void HideSubtitle()
    {
        void** singletonPtr = SubtitleManager_Singleton.get();
        if (!singletonPtr || !*singletonPtr) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        SubtitleManager_HideSubtitle(*singletonPtr, player);
    }

    // ========================================================================
    // Reference Display Name - Native "Press X to pick up <name>" system
    // ========================================================================
    // The game uses this function to get the proper display name for references,
    // including weapon modifications. This is the authoritative source for item names.
    
    /**
     * TESObjectREFR::GetDisplayFullName - Get the full display name including mods
     * Non-VR offset: 0x3f3a70 (ID 1212056)
     * VR offset: 0x40b760 (from fo4_database.csv - Status 4 Verified)
     * 
     * This is the native function the game uses for "Press X to pick up" prompts.
     * It properly handles:
     * - Weapon modifications (e.g., "Laser Pistol" with mods)
     * - Custom names set by player
     * - ExtraTextDisplayData
     * - Base form fallback
     * 
     * @param refr The TESObjectREFR to get the name for
     * @return const char* pointing to the display name (may be in game memory, copy if needed)
     */
    using _TESObjectREFR_GetDisplayFullName = const char*(__fastcall*)(RE::TESObjectREFR* refr);
    inline REL::Relocation<_TESObjectREFR_GetDisplayFullName> TESObjectREFR_GetDisplayFullName{ REL::Offset(0x40b760) };
    
    /**
     * Helper function to get the display name as a std::string (safe copy)
     */
    inline std::string GetReferenceDisplayName(RE::TESObjectREFR* refr)
    {
        if (!refr) return "";
        
        const char* name = TESObjectREFR_GetDisplayFullName(refr);
        if (name && name[0] != '\0') {
            return std::string(name);
        }
        
        // Fallback to base form name
        auto* baseForm = refr->GetObjectReference();
        if (baseForm) {
            auto fullName = RE::TESFullName::GetFullName(*baseForm, false);
            if (!fullName.empty()) {
                return std::string(fullName);
            }
        }
        
        return "";
    }

    // ========================================================================
    // ViewCaster - Native crosshair/activation target system
    // ========================================================================
    // The ViewCaster is the game's system for determining what object the
    // player's crosshair is pointing at. It's the authoritative source for
    // "Press X to pick up" prompts.
    
    /**
     * ViewCasterBase::QActivatePickRef - Get current activation target
     * Returns the ObjectRefHandle of what the crosshair is pointing at.
     * VR offset: 0x1409d0ce0 - 0x140000000 = 0x9d0ce0
     * 
     * CORRECTED SIGNATURE (January 2026):
     *   param_1 (RCX): ViewCasterBase* this
     *   param_2 (RDX): uint32_t* OUTPUT - where to store the handle
     *   Returns: param_2 (same as input)
     * 
     * @param thisPtr The ViewCaster instance
     * @param outHandle OUTPUT - receives the ObjectRefHandle value
     * @return Pointer to outHandle
     */
    using _ViewCasterBase_QActivatePickRef = std::uint32_t*(__fastcall*)(void* thisPtr, std::uint32_t* outHandle);
    inline REL::Relocation<_ViewCasterBase_QActivatePickRef> ViewCasterBase_QActivatePickRef{ REL::Offset(0x9d0ce0) };
    
    /**
     * VR Wand ViewCaster Offsets (CORRECTED January 29, 2026)
     * 
     * VR uses DIFFERENT ViewCaster globals than flat mode!
     * These are the correct offsets for VR wand target detection.
     * 
     * ViewCasterPrimaryWand - the "off-hand" (usually LEFT, holds Pipboy)
     *   - VR offset: 0x5AC72B0 (VERIFIED via Ghidra decompilation)
     * 
     * ViewCasterSecondaryWand - the "aiming" hand (usually RIGHT, holds weapon)
     *   - VR offset: 0x5AC7F10 (VERIFIED via Ghidra decompilation)
     * 
     * NOTE: Primary/Secondary mapping swaps when bLeftHandedMode:VR is enabled.
     */
    static constexpr std::uintptr_t VIEWCASTER_PRIMARY_OFFSET = 0x5AC72B0;    // LEFT hand ViewCaster
    static constexpr std::uintptr_t VIEWCASTER_SECONDARY_OFFSET = 0x5AC7F10;  // RIGHT hand ViewCaster
    
    inline void* g_ViewCasterSecondaryWand = nullptr;
    inline void* g_ViewCasterPrimaryWand = nullptr;
    inline bool g_ViewCasterSearched = false;
    
    /**
     * LookupByHandle - Converts ObjectRefHandle (uint32) to TESObjectREFR*
     * VR offset: 0xAB60
     * 
     * DEPRECATED: Use RE::ObjectRefHandle::get() instead for proper reference counting.
     * Our direct call to this function was causing crashes by not properly
     * incrementing reference counts, leading to use-after-free on worker threads.
     * 
     * @param handle Pointer to the raw handle value
     * @param outRefr Output pointer to receive the TESObjectREFR*
     * @return True if lookup succeeded, false otherwise
     */
    using _LookupByHandle = bool(*)(std::uint32_t* handle, RE::TESObjectREFR** outRefr);
    inline REL::Relocation<_LookupByHandle> LookupByHandle{ REL::Offset(0xAB60) };
    
    /**
     * Initialize VR Wand ViewCasters using the VERIFIED offsets.
     * VR uses DIFFERENT globals than flat mode!
     * 
     * CRITICAL: These are POINTER HOLDER addresses that need dereferencing!
     *   void** ptr = (void**)(moduleBase + offset);
     *   ViewCasterBase* viewCaster = (ViewCasterBase*)(*ptr);
     */
    inline void* FindViewCasterSingleton()
    {
        // Track validation state
        static bool g_SecondaryValidated = false;
        static bool g_PrimaryValidated = false;
        
        // If both validated, just return cached pointer
        if (g_SecondaryValidated && g_PrimaryValidated) {
            return g_ViewCasterSecondaryWand;
        }
        
        // Get module base for address calculation
        auto moduleBase = REL::Module::get().base();
        
        // Log once on first call
        static bool loggedFirst = false;
        if (!loggedFirst) {
            spdlog::info("[ViewCaster-VR] Initializing with VR offsets:");
            spdlog::info("[ViewCaster-VR]   Primary (LEFT) holder:    0x{:X}", VIEWCASTER_PRIMARY_OFFSET);
            spdlog::info("[ViewCaster-VR]   Secondary (RIGHT) holder: 0x{:X}", VIEWCASTER_SECONDARY_OFFSET);
            loggedFirst = true;
        }
        
        g_ViewCasterSearched = true;
        
        __try {
            // === VR WAND ViewCasters - MUST DEREFERENCE! ===
            // These offsets are POINTER HOLDERS, not direct ViewCaster addresses!
            
            // SecondaryWand (LEFT hand / Pipboy) - dereference pointer at 0x5AC7F10
            if (!g_SecondaryValidated) {
                void** secondaryHolder = reinterpret_cast<void**>(moduleBase + VIEWCASTER_SECONDARY_OFFSET);
                void* secondaryPtr = *secondaryHolder;  // DEREFERENCE!
                g_ViewCasterSecondaryWand = secondaryPtr;
                g_SecondaryValidated = true;
                spdlog::info("[ViewCaster-VR] SecondaryWand (LEFT hand): holder at {:X} -> dereferenced to {:X}",
                             reinterpret_cast<std::uintptr_t>(secondaryHolder),
                             reinterpret_cast<std::uintptr_t>(secondaryPtr));
            }
            
            // PrimaryWand (RIGHT hand / aiming) - dereference pointer at 0x5AC72B0
            if (!g_PrimaryValidated) {
                void** primaryHolder = reinterpret_cast<void**>(moduleBase + VIEWCASTER_PRIMARY_OFFSET);
                void* primaryPtr = *primaryHolder;  // DEREFERENCE!
                g_ViewCasterPrimaryWand = primaryPtr;
                g_PrimaryValidated = true;
                spdlog::info("[ViewCaster-VR] PrimaryWand (RIGHT hand): holder at {:X} -> dereferenced to {:X}",
                             reinterpret_cast<std::uintptr_t>(primaryHolder),
                             reinterpret_cast<std::uintptr_t>(primaryPtr));
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            static int exCount = 0;
            if (exCount++ < 3) {
                spdlog::error("[ViewCaster-VR] Exception during ViewCaster initialization");
            }
        }
        
        return g_ViewCasterSecondaryWand;
    }
    
    /**
     * Get the PrimaryWand (off-hand/Pipboy hand) ViewCaster.
     * Returns nullptr if not found.
     */
    inline void* FindViewCasterPrimaryWand()
    {
        FindViewCasterSingleton();  // Ensure search has been done
        return g_ViewCasterPrimaryWand;
    }
    
    /**
     * Get the ObjectRefHandle for what the specified VR wand is pointing at.
     * Uses QActivatePickRef() function at VR offset 0x9D0CE0.
     * 
     * SAFETY: Returns ObjectRefHandle for proper reference counting.
     * Use handle.get() to get a reference-counted NiPointer<TESObjectREFR>.
     * 
     * WARNING: Do NOT call during loading! Check UI::GetMenuOpen("LoadingMenu") first.
     * 
     * @param isLeft True for primary/left hand, false for secondary/right hand
     * @return ObjectRefHandle (check with operator bool(), use .get() for NiPointer)
     */
    inline RE::ObjectRefHandle GetVRWandTargetHandle(bool isLeft)
    {
        // Ensure ViewCasters are initialized
        FindViewCasterSingleton();
        
        // Get the appropriate ViewCaster
        // ViewCasters follow wand node swapping: they're paired with VRWandControllers
        // that track primaryWandNode/SecondaryWandNode, which the game swaps in LH mode.
        // Normal: PrimaryWand=RIGHT physical, SecondaryWand=LEFT physical
        // LH mode: PrimaryWand=LEFT physical, SecondaryWand=RIGHT physical
        bool isLH = VRInput::GetSingleton().IsLeftHandedMode();
        void* viewCaster;
        if (isLH) {
            viewCaster = isLeft ? g_ViewCasterPrimaryWand : g_ViewCasterSecondaryWand;
        } else {
            viewCaster = isLeft ? g_ViewCasterSecondaryWand : g_ViewCasterPrimaryWand;
        }
        
        if (!viewCaster) {
            return RE::ObjectRefHandle();
        }
        
        // Use QActivatePickRef to get the handle
        // CORRECTED: Uses OUTPUT PARAMETER, not return value!
        std::uint32_t handle = 0;
        ViewCasterBase_QActivatePickRef(viewCaster, &handle);
        
        if (handle == 0) {
            return RE::ObjectRefHandle();
        }
        
        // Create ObjectRefHandle from raw handle value for proper reference counting
        // The RE::ObjectRefHandle constructor from uint32_t isn't available directly,
        // but we can use the native_handle approach via BSUntypedPointerHandle
        RE::BSUntypedPointerHandle<> untypedHandle(handle);
        RE::ObjectRefHandle refHandle;
        *reinterpret_cast<RE::BSUntypedPointerHandle<>*>(&refHandle) = untypedHandle;
        return refHandle;
    }
    
    /**
     * DEPRECATED: Use GetVRWandTargetHandle() and handle.get() instead!
     * 
     * Get the TESObjectREFR* that the specified VR wand is pointing at.
     * WARNING: This returns a raw pointer without proper reference counting.
     * The pointer may become invalid at any time. For safety, use:
     *   RE::ObjectRefHandle handle = GetVRWandTargetHandle(isLeft);
     *   RE::NiPointer<RE::TESObjectREFR> refr = handle.get();
     * 
     * @param isLeft True for primary/left hand, false for secondary/right hand
     * @return The TESObjectREFR* or nullptr if nothing is targeted (UNSAFE!)
     */
    inline RE::TESObjectREFR* GetVRWandTarget(bool isLeft)
    {
        RE::ObjectRefHandle handle = GetVRWandTargetHandle(isLeft);
        if (!handle) {
            return nullptr;
        }
        
        // Get smart pointer - this increases ref count temporarily
        RE::NiPointer<RE::TESObjectREFR> refrPtr = handle.get();
        
        // WARNING: Ref count decrements when refrPtr goes out of scope!
        // This is why this function is deprecated - the returned pointer
        // is only safe if the caller immediately stores it in a handle.
        return refrPtr.get();
    }
    
    /**
     * Get the TESObjectREFR* from the PRIMARY (left) wand ViewCaster.
     */
    inline RE::TESObjectREFR* GetPrimaryWandTarget()
    {
        return GetVRWandTarget(true);
    }
    
    /**
     * Get the ObjectRefHandle from the PRIMARY (left) wand ViewCaster.
     * Use this for safe reference counting.
     */
    inline RE::ObjectRefHandle GetPrimaryViewCasterTarget()
    {
        return GetVRWandTargetHandle(true);
    }
    
    /**
     * Get the TESObjectREFR* from the SECONDARY (right) wand ViewCaster.
     */
    inline RE::TESObjectREFR* GetSecondaryWandTarget()
    {
        return GetVRWandTarget(false);
    }
    
    /**
     * Get the ObjectRefHandle from the SECONDARY (right) wand ViewCaster.
     * Use this for safe reference counting.
     */
    inline RE::ObjectRefHandle GetSecondaryViewCasterTarget()
    {
        return GetVRWandTargetHandle(false);
    }
    
    /**
     * DEPRECATED: Use GetVRWandTarget(isLeft) instead.
     * Redirects to secondary wand for backward compatibility.
     */
    inline RE::TESObjectREFR* GetViewCasterTarget()
    {
        return GetSecondaryWandTarget();
    }

    // Note: For raycasting, we use Bethesda's bhkPickData wrapper instead of direct Havok calls.
    // This is cleaner and handles all the Havok 2014+ complexity internally.
    // See: RE::bhkPickData in CommonLibF4/include/RE/Bethesda/bhkPickData.h
    // Usage:
    //   RE::bhkPickData pickData;
    //   pickData.SetStartEnd(origin, endPoint);
    //   RE::NiAVObject* hitObject = cell->Pick(pickData);
    //   if (pickData.HasHit()) { ... }

    // ========================================================================
    // HUDRollover - Native "Press X to pick up" display system
    // ========================================================================
    // The HUDRollover displays the activation prompt when pointing at objects.
    // These functions allow hiding/showing the prompt.
    
    /**
     * HUDRollover::HideRollover - Hide the "Press X to pick up" prompt
     * VR offset: 0x140ab7590
     * 
     * Call this to suppress the native activation prompt when Heisenberg is
     * handling its own grab highlighting.
     */
    using _HUDRollover_HideRollover = void(__fastcall*)(void* thisPtr);
    inline REL::Relocation<_HUDRollover_HideRollover> HUDRollover_HideRollover{ REL::Offset(0xab7590) };
    
    /**
     * HUDRollover singleton pointer
     * VR address: 0x1437a1b48
     * Read this to get the HUDRollover instance, then call HideRollover on it.
     */
    inline constexpr std::uintptr_t kHUDRolloverSingletonOffset = 0x37a1b48;
    
    /**
     * Hide the native "Press X to pick up" prompt.
     * Call this when Heisenberg is showing its own grab indicator.
     */
    inline void HideNativeActivationPrompt()
    {
        REL::Relocation<void**> singletonPtr{ REL::Offset(kHUDRolloverSingletonOffset) };
        void* hudRollover = *singletonPtr.get();
        if (hudRollover) {
            HUDRollover_HideRollover(hudRollover);
        }
    }

    // =========================================================================
    // SKINNED GEOMETRY / BONE MATRIX UPDATES
    // =========================================================================
    // F4VR uses BSFlattenedBoneTree for skinned mesh bone transforms.
    // After moving a grabbed object, we need to update skinned geometry so
    // it doesn't lag behind visually.
    // =========================================================================

    /**
     * BSFlattenedBoneTree::UpdateBoneArray
     * Updates bone transforms for skinned geometry after the node has moved.
     * VR offset: 0x1c214b0 - Status 4 (Verified) from fo4_database.csv
     * ID: 1421730
     * 
     * Call this on nodes with skinned geometry after moving them to prevent
     * visual lag on character/creature bodies and armors.
     * 
     * @param boneTree The BSFlattenedBoneTree instance (cast from NiObject)
     */
    using _BSFlattenedBoneTree_UpdateBoneArray = void(__fastcall*)(void* boneTree);
    inline REL::Relocation<_BSFlattenedBoneTree_UpdateBoneArray> BSFlattenedBoneTree_UpdateBoneArray{ REL::Offset(0x1c214b0) };

    /**
     * BSSkin::Instance::UpdateModelBound
     * Updates the model bounding box after bone transforms change.
     * VR offset: 0x1c33850 - Status 4 (Verified) from fo4_database.csv
     * ID: 1084965
     *
     * @param skinInstance The BSSkin::Instance pointer
     * @param bound The NiBound to update
     */
    using _BSSkinInstance_UpdateModelBound = void(__fastcall*)(void* skinInstance, RE::NiBound* bound);
    inline REL::Relocation<_BSSkinInstance_UpdateModelBound> BSSkinInstance_UpdateModelBound{ REL::Offset(0x1c33850) };

    // =========================================================================
    // PROCESSLISTS - Actor queries (companion detection, radius search)
    // =========================================================================

    /**
     * ProcessLists singleton pointer (VR-specific offset)
     * VR offset: 0x5930968 (fo4_database ID 1569706, status 2)
     *
     * NOTE: CommonLibF4's ProcessLists::GetSingleton() uses REL::ID(4796160)
     * which is for non-VR. We use the raw VR offset here.
     */
    inline RE::ProcessLists* GetProcessListsSingleton()
    {
        static REL::Relocation<RE::ProcessLists**> singleton{ REL::Offset(0x5930968) };
        return *singleton;
    }

    /**
     * ProcessLists::GetClosestActorWithinRangeOfPoint
     * Finds the closest actor within a radius of a world position.
     * VR offset: 0xf8e010 (fo4_database ID 1000030, status 4 verified)
     *
     * @param processLists The ProcessLists singleton
     * @param a_point Center point for the search
     * @param a_radius Search radius
     * @param a_outActor Output: closest actor found (NiPointer for ref safety)
     * @return true if an actor was found
     */
    // =========================================================================
    // PICKPOCKET - Native functions from Ghidra decompilation (VR offsets)
    // =========================================================================

    /**
     * AiFormulas::ComputePickpocketSuccess - Native pickpocket chance formula.
     * Uses all fPickPocket* game settings, applies perk entry point 0x38
     * (kModPickpocketChance), and applies combat multiplier.
     * VR offset: 0x64b6f0 (Ghidra: 0x14064b6f0)
     *
     * @param playerAgility   Player's Agility SPECIAL stat value
     * @param npcPerception    NPC's Perception SPECIAL stat value
     * @param stealValue       Gold value of item being stolen (min 1)
     * @param totalWeight      Item weight * count
     * @param player           Player Actor*
     * @param npc              Target NPC Actor*
     * @param itemForm         TESForm* of the item being stolen
     * @param inCombat         true if NPC is in combat (applies combat mult)
     * @return Success chance 0-100 (caller does BSRandom(0,100) < chance)
     */
    using _AiFormulas_ComputePickpocketSuccess = int(*)(float playerAgility, float npcPerception,
        std::uint32_t stealValue, float totalWeight, RE::Actor* player, RE::Actor* npc,
        RE::TESForm* itemForm, bool inCombat);
    inline REL::Relocation<_AiFormulas_ComputePickpocketSuccess>
        AiFormulas_ComputePickpocketSuccess{ REL::Offset(0x64b6f0) };

    /**
     * Actor::PickpocketAlarm - Triggers the full crime system on pickpocket failure.
     * Handles: ForceDetect, crime value calculation, bounty, witness detection,
     * crime dialogue, and faction response.
     * VR offset: 0xdd3fc0 (Ghidra: 0x140dd3fc0)
     *
     * This is a member function on Actor (the player who was caught).
     * @param thisPlayer   The player Actor* (this pointer)
     * @param victim       The NPC who caught the player
     * @param itemInstance BGSObjectInstance* of stolen item (can be nullptr)
     * @param count        Number of items stolen (0 if caught before taking)
     */
    using _Actor_PickpocketAlarm = void(*)(RE::Actor* thisPlayer, RE::TESObjectREFR* victim,
        void* itemInstance, int count);
    inline REL::Relocation<_Actor_PickpocketAlarm>
        Actor_PickpocketAlarm{ REL::Offset(0xdd3fc0) };

    // =========================================================================
    // PICKPOCKET - Story event for quest tracking and misc stat increment
    // =========================================================================

    /**
     * BGSPickpocketEvent - Story event fired on pickpocket attempt.
     * Follows same pattern as BGSHackTerminal (8-byte struct: handle + success).
     * 
     * The engine's ItemsPickpocketedToMiscStatHandler listens for this event
     * and automatically increments two MiscStats:
     *   - "Successful Pickpockets" (if success == 1)
     *   - "Pickpocket Attempts"    (always)
     * 
     * VR event index: 0x3773348 (Ghidra: DAT_143773348)
     * VR constructor: 0x7d87d0 (Ghidra: FUN_1407d87d0)
     */
    struct BGSPickpocketEvent
    {
        BGSPickpocketEvent(RE::TESObjectREFR* a_npc, std::uint32_t a_success) :
            success(a_success)
        {
            if (a_npc) {
                npc = a_npc->GetHandle();
            }
        }

        [[nodiscard]] static std::uint32_t EVENT_INDEX()
        {
            static REL::Relocation<std::uint32_t*> eventIdx{ REL::Offset(0x3773348) };
            return *eventIdx;
        }

        // members
        RE::ObjectRefHandle npc;       // 00
        std::uint32_t       success;   // 04
    };
    static_assert(sizeof(BGSPickpocketEvent) == 0x08);

    using _ProcessLists_GetClosestActorWithinRangeOfPoint = bool(*)(
        RE::ProcessLists* processLists, RE::NiPoint3& a_point, float a_radius, RE::NiPointer<RE::Actor>& a_outActor);
    inline REL::Relocation<_ProcessLists_GetClosestActorWithinRangeOfPoint>
        ProcessLists_GetClosestActorWithinRangeOfPoint{ REL::Offset(0xf8e010) };
}
