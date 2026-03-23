#include "Hooks.h"

#include "Heisenberg.h"
#include "VRInput.h"
#include "WandNodeHelper.h"
#include "Grab.h"
#include "MenuChecker.h"
#include "DropToHand.h"
#include "PipboyInteraction.h"
#include "Utils.h"
#include "F4VROffsets.h"

#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include "Config.h"
#include "ActivatorHandler.h"

#include <atomic>

namespace heisenberg::Hooks
{
    // =========================================================================
    // Hook Strategy:
    // We use a single post-physics hook (0xd8405e) - called after physics step  
    // Used for: Input processing, grab updates, physics sync
    //
    // HUD message suppression is done via INI setting toggle, not hooking.
    // This is much safer and avoids the crashes caused by xbyak trampolines.
    // =========================================================================

    // Cached INI setting pointer for bShowHUDMessages:Interface
    static RE::Setting* g_showHUDMessagesSetting = nullptr;
    static bool g_showHUDMessagesSettingSearched = false;
    static int g_deferredUnsuppressFrames = 0;  // Deferred HUD unsuppress countdown
    
    namespace
    {
        // Original function pointers
        using PrePhysicsFunc = void(*)(uint64_t rcx);
        PrePhysicsFunc g_originalPrePhysics = nullptr;
        
        using PostPhysicsFunc = void(*)(uint64_t rcx);
        PostPhysicsFunc g_originalPostPhysics = nullptr;
        
        using PreRenderFunc = void(*)(uint64_t rcx);
        PreRenderFunc g_originalPreRender = nullptr;

        // End-of-update hook (0xd84f2c) - runs AFTER bone tree updates and animation
        // This is the latest safe point before rendering begins.
        // Used for tape deck animation so our transform writes aren't overwritten
        // by the animation system or FRIK's skeleton updates.
        using EndUpdateFunc = void(*)(uint64_t rcx);
        EndUpdateFunc g_originalEndUpdate = nullptr;

        // PipboyInventoryMenu::DropItem hook
        // Signature: void DropItem(uint32_t inventoryHandle, void* stackDataArray, uint32_t count)
        // VR address: 0x140b9b9e0
        using PipboyDropItemFunc = void(*)(uint32_t, void*, uint32_t);
        PipboyDropItemFunc g_originalPipboyDropItem = nullptr;

        // TESObjectREFR::SetPosition (VR 0x3f4370) - properly updates data.location + physics + cell
        using SetPositionFunc = void(*)(RE::TESObjectREFR*, RE::NiPoint3*);

        void HookPipboyDropItem(uint32_t param1, void* param2, uint32_t param3)
        {
            spdlog::debug("[PipboyDrop] HookPipboyDropItem CALLED (param1={}, param3={})", param1, param3);

            auto& dropToHand = DropToHand::GetSingleton();

            // Get hand position before the drop
            RE::NiPoint3 handPos;
            bool hasHandPos = false;
            if (g_config.dropToHandMode > 0) {
                auto* playerNodes = f4cf::f4vr::getPlayerNodes();
                if (playerNodes) {
                    bool isLeft = true;
                    dropToHand.GetTargetHand(isLeft);
                    RE::NiNode* wandNode = heisenberg::GetWandNode(playerNodes, isLeft);
                    if (wandNode) {
                        handPos = wandNode->world.translate;
                        hasHandPos = true;
                        spdlog::debug("[PipboyDrop] Hand pos: ({:.1f},{:.1f},{:.1f}) isLeft={}",
                            handPos.x, handPos.y, handPos.z, isLeft);
                    }
                }
            }

            // Enable ref ID capture so ProcessEvent records the dropped ref FormID
            if (hasHandPos) {
                dropToHand.SetPipboyDropCapture(true);
            }

            // Call original DropItem - item spawns at player feet
            // ProcessEvent fires during this call and captures the ref ID
            if (g_originalPipboyDropItem) {
                g_originalPipboyDropItem(param1, param2, param3);
            }

            // After DropItem: call SetPosition to move the item to hand position
            if (hasHandPos) {
                uint32_t refID = dropToHand.GetPipboyDropRefID();
                dropToHand.SetPipboyDropCapture(false);
                spdlog::debug("[PipboyDrop] Captured refID: {:08X}", refID);
                if (refID != 0) {
                    auto* form = RE::TESForm::GetFormByID(refID);
                    auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
                    if (refr) {
                        // Block dropping the holotape that's currently loaded in the tape deck.
                        auto* baseObj = refr->GetObjectReference();
                        auto& pipboy = heisenberg::PipboyInteraction::GetSingleton();
                        if (baseObj && pipboy.HasHolotapeLoaded() &&
                            baseObj->formID == pipboy.GetLoadedHolotapeFormID()) {
                            spdlog::debug("[PipboyDrop] BLOCKED: {:08X} is loaded in tape deck — undoing drop",
                                         baseObj->formID);
                            heisenberg::SafeDisable(refr);
                            // No SetDelete — Inventory3DManager crash risk
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            if (player) {
                                RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                                heisenberg::AddObjectToContainer(
                                    player, static_cast<RE::TESBoundObject*>(baseObj),
                                    &nullExtra, 1, nullptr, 0);
                                spdlog::debug("[PipboyDrop] Re-added {:08X} to inventory", baseObj->formID);
                            }
                        } else if (baseObj && pipboy.IsIntroHolotape(baseObj->formID)) {
                            // Heisenberg intro holotape: route through loot-to-hand path.
                            // The world-drop path is unreliable for this ESP-added item because
                            // the engine can recycle its temp RefID before 3D loads.
                            spdlog::debug("[PipboyDrop] Intro holotape {:08X} — rerouting via loot path", baseObj->formID);
                            heisenberg::SafeDisable(refr);
                            dropToHand.CancelPendingDropByRefID(refID);
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            if (player) {
                                RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                                heisenberg::AddObjectToContainer(
                                    player, static_cast<RE::TESBoundObject*>(baseObj),
                                    &nullExtra, 1, nullptr, 0);
                                // Holotapes always go to right hand
                                bool isLeft = false;
                                dropToHand.QueueDropToHand(baseObj->formID, isLeft, 1, true, false);
                                spdlog::debug("[PipboyDrop] Intro holotape queued via loot-to-hand (isLeft={})", isLeft);
                            }
                        } else {
                            REL::Relocation<SetPositionFunc> setPosition{ REL::Offset(0x3f4370) };
                            setPosition(refr, &handPos);
                            spdlog::debug("[PipboyDrop] SetPosition on {:08X} to hand ({:.1f},{:.1f},{:.1f})",
                                refID, handPos.x, handPos.y, handPos.z);
                        }
                    }
                } else {
                    spdlog::warn("[PipboyDrop] ProcessEvent did not capture any refID!");
                }
            }
        }

        // Pre-physics hook - runs BEFORE physics simulation
        // Same hook point as FRIK's smoothMovement (0xd83ec4)
        // User should disable FRIK's SmoothMovement to avoid conflicts
        void HookPrePhysics(const uint64_t rcx)
        {
            // === PRE-PHYSICS HOOK DISABLED FOR DEBUGGING ===
            // All processing moved to post-physics hook
            
            // Call original function
            if (g_originalPrePhysics) {
                g_originalPrePhysics(rcx);
            }
        }

        // Post-physics hook - runs after engine updates, same timing as FRIK
        void HookPostPhysics(const uint64_t rcx)
        {
            // Call original function first (FRIK + game function)
            if (g_originalPostPhysics) {
                g_originalPostPhysics(rcx);
            }

            constexpr bool DEBUG_DISABLE_ALL_PROCESSING = false;
            if (DEBUG_DISABLE_ALL_PROCESSING) {
                return;
            }

            __try {
                if (MenuChecker::GetSingleton().IsLoading()) {
                    return;
                }
                
                g_vrInput.Update();
                GrabManager::GetSingleton().PrePhysicsUpdate();
                g_heisenberg.OnInputUpdate();
                g_heisenberg.OnGrabUpdate();
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                spdlog::error("[HOOKS] Exception in HookPostPhysics - Heisenberg processing skipped this frame");
            }
        }
        
        // End-of-update hook (0xd84f2c) - runs AFTER all animation/skeleton/game processing
        // This is where we write tape deck transforms and manually compute world.rotate.
        // By this point FRIK + game function have fully processed the skeleton.
        void HookEndUpdate(const uint64_t rcx)
        {
            // Call original function first
            if (g_originalEndUpdate) {
                g_originalEndUpdate(rcx);
            }

            if (!MenuChecker::GetSingleton().IsLoading()) {
                __try {
                    PipboyInteraction::GetSingleton().UpdateTapeDeckAnimation();
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    static bool loggedOnce = false;
                    if (!loggedOnce) {
                        spdlog::error("[HOOKS] Exception in HookEndUpdate - tape deck skipped");
                        loggedOnce = true;
                    }
                }

                // Process deferred VH holster requests here (NOT in PostPhysics)
                // VH's displayWeapon() does cloneNode/AttachChild/loadNifFromFile
                // which deadlocks during post-physics when scene graph locks are held.
                // EndUpdate runs AFTER all animation/skeleton/bone processing,
                // so the scene graph is fully available for NIF cloning.
                __try {
                    GrabManager::GetSingleton().ProcessPendingHolster();
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    spdlog::error("[HOOKS] Exception in ProcessPendingHolster");
                }

                // Process deferred weapon unequip from storage zone grip
                // UnEquipItem crashes during HookPostPhysics (physics locks held),
                // so we defer to EndUpdate where game state is stable.
                __try {
                    g_heisenberg.ProcessPendingWeaponUnequip();
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    spdlog::error("[HOOKS] Exception in ProcessPendingWeaponUnequip");
                }

                // Process deferred weapon re-equip from storage zone grip
                __try {
                    g_heisenberg.ProcessPendingWeaponReequip();
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    spdlog::error("[HOOKS] Exception in ProcessPendingWeaponReequip");
                }
            }
        }

        // Pre-render hook - runs just before rendering
        // This is the FINAL opportunity to update visual positions
        // By this point, player movement has been applied to roomnode
        void HookPreRender(const uint64_t rcx)
        {
            // Wrap in SEH to prevent crashes from taking down the game
            __try {
                // Update grabbed object positions with FINAL wand position
                // No prediction needed - player movement is already applied
                GrabManager::GetSingleton().PreRenderUpdate();

                // Tape deck animation now runs on separate threads (Cylon's approach)
                // PipboyInteraction::GetSingleton().UpdateTapeDeckVisuals();
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                static bool loggedOnce = false;
                if (!loggedOnce) {
                    spdlog::error("[HOOKS] Exception in HookPreRender - update skipped");
                    loggedOnce = true;
                }
            }
            
            // Call original function
            if (g_originalPreRender) {
                g_originalPreRender(rcx);
            }
        }
    }

    void Install()
    {
        spdlog::info("Installing Heisenberg hooks...");

        // ===============================================================
        // DEBUG: Completely disable ALL hooks to test if just having
        // the hook installed causes the crash. If crash goes away with
        // this set to true, then the hook installation is the problem.
        // ===============================================================
        constexpr bool DEBUG_DISABLE_ALL_HOOKS = false;  // Set to true to disable ALL hooks
        if (DEBUG_DISABLE_ALL_HOOKS) {
            spdlog::warn("DEBUG: ALL HOOKS DISABLED - Heisenberg will do nothing");
            g_vrInput.Initialize();
            return;
        }

        // Allocate trampoline space
        F4SE::AllocTrampoline(1024);

        // Initialize VR input
        g_vrInput.Initialize();

        auto& trampoline = F4SE::GetTrampoline();

        // Pre-physics hook - DISABLED
        // Was causing crashes with flashlight toggle due to scene graph conflicts
        // All processing now happens in post-physics hook only
        // REL::Relocation<std::uintptr_t> prePhysicsHook{ REL::Offset(0xd83ec4) };
        // g_originalPrePhysics = reinterpret_cast<PrePhysicsFunc>(
        //     trampoline.write_call<5>(prePhysicsHook.address(), &HookPrePhysics));
        // spdlog::info("PrePhysicsHook: Installed at {:X}", prePhysicsHook.address());

        // Post-physics hook - runs AFTER physics simulation
        // Input processing and grab detection
        REL::Relocation<std::uintptr_t> postPhysicsHook{ REL::Offset(0xd8405e) };
        g_originalPostPhysics = reinterpret_cast<PostPhysicsFunc>(
            trampoline.write_call<5>(postPhysicsHook.address(), &HookPostPhysics));
        spdlog::info("PostPhysicsHook: Installed at {:X}", postPhysicsHook.address());

        // End-of-update hook (0xd84f2c) - runs AFTER all animation/skeleton/game processing
        // Tape deck animation writes here with manual world.rotate computation.
        // This is a DIFFERENT hook point from FRIK's 0xd8405e.
        REL::Relocation<std::uintptr_t> endUpdateHook{ REL::Offset(0xd84f2c) };
        g_originalEndUpdate = reinterpret_cast<EndUpdateFunc>(
            trampoline.write_call<5>(endUpdateHook.address(), &HookEndUpdate));
        spdlog::info("EndUpdateHook: Installed at {:X}", endUpdateHook.address());

        // PipboyInventoryMenu::DropItem hook - intercepts items dropped from Pipboy menu
        // so we can immediately move them to the hand position and hide them.
        // Without this, items fall to the floor visibly while the Pipboy is open.
        // Hook both call sites to catch all invocations:
        //   0xb9ae7f - main Pipboy inventory drop path
        //   0x9f7594 - deferred/message handler drop path
        REL::Relocation<std::uintptr_t> pipboyDropHook1{ REL::Offset(0xb9ae7f) };
        g_originalPipboyDropItem = reinterpret_cast<PipboyDropItemFunc>(
            trampoline.write_call<5>(pipboyDropHook1.address(), &HookPipboyDropItem));
        spdlog::info("PipboyDropHook1: Installed at {:X}", pipboyDropHook1.address());

        REL::Relocation<std::uintptr_t> pipboyDropHook2{ REL::Offset(0x9f7594) };
        trampoline.write_call<5>(pipboyDropHook2.address(), &HookPipboyDropItem);
        spdlog::info("PipboyDropHook2: Installed at {:X}", pipboyDropHook2.address());

        // ShowHUDMessage hook - REMOVED
        // The xbyak trampoline approach caused crashes when calling the original function.
        // Instead, we now use INI setting toggle (bShowHUDMessages:Interface) to suppress
        // the "X was removed" message during ActivateRef. This is much safer.
        // See SetSuppressHUDMessages() below.
        
        // Pre-render hook - DISABLED - causes crash during save load
        // The hook point (0x1C21156) is inside scene graph update, not safe
        // REL::Relocation<std::uintptr_t> preRenderHook{ REL::Offset(0x1C21156) };
        // g_originalPreRender = reinterpret_cast<PreRenderFunc>(
        //     trampoline.write_call<5>(preRenderHook.address(), &HookPreRender));
        // spdlog::info("PreRenderHook: Installed at {:X}", preRenderHook.address());

        // ReadyWeaponHandler::OnButtonEvent hook - blocks grip from triggering weapon draw
        // Same approach as STUF VR: hooks entire function, checks for "WandGrip" event
        InstallGripWeaponDrawHook();

        // ActorEquipManager::EquipObject hook - intercept consumable equips from
        // Pipboy/Favorites menus and redirect to drop-to-hand when configured
        if (g_config.consumableToHand || g_config.favoritesToHand || g_config.holotapeToHand) {
            InstallEquipObjectHook();
        }

        // ActivateRef hook - blocks activation of recently-dropped items (Grip>A anti-reactivation)
        InstallActivateRefHook();

        // HUDRollover vtable hook - nulls actionText on wand HUD
        InstallHUDRolloverHook();

        // Disable ALL native telekinesis/grab spring creation — Heisenberg handles all grabbing.
        // The game has TWO separate code paths:
        //   Non-VR: CreateMouseSpring (0x0f1bf70)
        //   VR:     FUN_140f19250 (0x0f19250) — VR-specific spring creator
        //           FUN_140f18ee0 (0x0f18ee0) — VR StartGrabObject (whole grab chain entry)
        // All three must be patched to prevent native grabs competing with our mod,
        // especially when users remap VR buttons (e.g., Grip<>A swap causes physical A
        // to send digital Grip bit, triggering the native VR grab system).

        // Patch 1: Non-VR CreateMouseSpring → RET
        {
            REL::Relocation<std::uintptr_t> addr{ REL::Offset(0x0f1bf70) };
            auto* p = reinterpret_cast<uint8_t*>(addr.address());
            DWORD oldProtect;
            if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                spdlog::info("[NativeTelekinesis] Patching CreateMouseSpring at {:X}: {:#04x} -> 0xC3",
                            addr.address(), *p);
                *p = 0xC3;
                VirtualProtect(p, 1, oldProtect, &oldProtect);
            }
        }

        // Patch 2: VR CreateMouseSpring equivalent → xor eax,eax; ret (return 0)
        // Callers check return value: 0 = no spring created (safe skip)
        {
            REL::Relocation<std::uintptr_t> addr{ REL::Offset(0x0f19250) };
            auto* p = reinterpret_cast<uint8_t*>(addr.address());
            DWORD oldProtect;
            if (VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                spdlog::info("[NativeTelekinesis] Patching VR CreateMouseSpring at {:X}: {:#04x} {:#04x} {:#04x} -> xor eax,eax; ret",
                            addr.address(), p[0], p[1], p[2]);
                p[0] = 0x31;  // xor eax, eax
                p[1] = 0xC0;
                p[2] = 0xC3;  // ret
                VirtualProtect(p, 3, oldProtect, &oldProtect);
            }
        }

        // Patch 3: VR StartGrabObject → RET (prevents entire VR grab chain including
        // collision filter changes, state updates, and guard notifications)
        {
            REL::Relocation<std::uintptr_t> addr{ REL::Offset(0x0f18ee0) };
            auto* p = reinterpret_cast<uint8_t*>(addr.address());
            DWORD oldProtect;
            if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                spdlog::info("[NativeTelekinesis] Patching VR StartGrabObject at {:X}: {:#04x} -> 0xC3",
                            addr.address(), *p);
                *p = 0xC3;
                VirtualProtect(p, 1, oldProtect, &oldProtect);
            }
        }

        // NOTE: Terminal patches moved to ApplyTerminalPatches() — called after config load

        spdlog::info("Heisenberg hooks installed");
    }

    void ApplyTerminalPatches(bool enable)
    {
        // Patch 1: 0xc33d00 — original 0x75 (JNZ), patched 0xEB (JMP)
        {
            REL::Relocation<std::uintptr_t> patchAddr{ REL::Offset(0xc33d00) };
            auto* patchByte = reinterpret_cast<uint8_t*>(patchAddr.address());
            uint8_t target = enable ? 0xEB : 0x75;
            DWORD oldProtect;
            if (VirtualProtect(patchByte, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                spdlog::info("[TermPatch1] Byte at {:X} is {:#04x}, setting to {:#04x} (enable={})",
                            patchAddr.address(), *patchByte, target, enable);
                *patchByte = target;
                VirtualProtect(patchByte, 1, oldProtect, &oldProtect);
            }
        }

        // Patch 2: 0x2ede5e — original 0x74 (JZ), patched 0xEB (JMP)
        {
            REL::Relocation<std::uintptr_t> patchAddr{ REL::Offset(0x2ede5e) };
            auto* patchByte = reinterpret_cast<uint8_t*>(patchAddr.address());
            uint8_t target = enable ? 0xEB : 0x74;
            DWORD oldProtect;
            if (VirtualProtect(patchByte, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                spdlog::info("[TermPatch2] Byte at {:X} is {:#04x}, setting to {:#04x} (enable={})",
                            patchAddr.address(), *patchByte, target, enable);
                *patchByte = target;
                VirtualProtect(patchByte, 1, oldProtect, &oldProtect);
            }
        }

        spdlog::info("Terminal-on-Pipboy patches {} ", enable ? "applied" : "reverted");
    }

    // ---- HUDRollover button binary patches ----
    // Patch ShowActivateButton (0xab7610) and ShowSecondaryButton (0xab7700) with RET (0xC3).
    // These are non-virtual member functions that set button text/visibility on the
    // HUDRollover instance — vtable hooks on ShowRollover can't intercept them.
    // Patching with RET prevents any button prompt from being set, while HideRollover
    // still works normally (clears button data).
    // NOTE: QuickContainer patch (0xa8c800) removed — it was right-wand-only (asymmetric).

    static uint8_t g_origShowActivateButtonByte = 0;
    static uint8_t g_origShowSecondaryButtonByte = 0;
    static bool g_rolloverButtonPatchesApplied = false;

    void ApplyHUDRolloverButtonPatches(bool enable)
    {
        auto activateAddr   = REL::Offset(0xab7610).address();
        auto secondaryAddr  = REL::Offset(0xab7700).address();
        auto* pActivate     = reinterpret_cast<uint8_t*>(activateAddr);
        auto* pSecondary    = reinterpret_cast<uint8_t*>(secondaryAddr);

        // Save originals on first call
        if (g_origShowActivateButtonByte == 0) {
            g_origShowActivateButtonByte = *pActivate;
            g_origShowSecondaryButtonByte = *pSecondary;
        }

        if (enable == g_rolloverButtonPatchesApplied) return;  // already in desired state

        auto patchByte = [](uint8_t* addr, uint8_t target, const char* name, uintptr_t address) {
            DWORD oldProtect;
            if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                spdlog::info("[HUDPatch] {} at {:X}: {:#04x} -> {:#04x}", name, address, *addr, target);
                *addr = target;
                VirtualProtect(addr, 1, oldProtect, &oldProtect);
            }
        };

        patchByte(pActivate,  enable ? 0xC3 : g_origShowActivateButtonByte,    "ShowActivateButton",      activateAddr);
        patchByte(pSecondary, enable ? 0xC3 : g_origShowSecondaryButtonByte,   "ShowSecondaryButton",     secondaryAddr);

        g_rolloverButtonPatchesApplied = enable;
        spdlog::info("[HUDPatch] HUD button patches {}", enable ? "applied" : "reverted");
    }
    
    void SetSuppressHUDMessages(bool suppress)
    {
        // Use INI setting toggle to suppress HUD messages
        // This is much safer than hooking ShowHUDMessage
        
        // Cache the setting pointer on first call
        if (!g_showHUDMessagesSettingSearched) {
            g_showHUDMessagesSettingSearched = true;
            g_showHUDMessagesSetting = f4cf::f4vr::getIniSetting("bShowHUDMessages:Interface");
            if (g_showHUDMessagesSetting) {
                spdlog::info("[HUD] Found bShowHUDMessages:Interface setting");
            } else {
                spdlog::warn("[HUD] Could not find bShowHUDMessages:Interface setting - message suppression disabled");
            }
        }
        
        if (!g_showHUDMessagesSetting) {
            return;  // Setting not found, can't suppress
        }
        
        if (suppress) {
            // Disable HUD messages
            g_showHUDMessagesSetting->SetBinary(false);
            spdlog::debug("[HUD] Suppressing HUD messages via INI setting");
        } else {
            // Re-enable HUD messages
            g_showHUDMessagesSetting->SetBinary(true);
            spdlog::debug("[HUD] Restored HUD messages via INI setting");
        }
    }
    
    void ScheduleDeferredHUDUnsuppress(int frames)
    {
        g_deferredUnsuppressFrames = frames;
        spdlog::debug("[HUD] Scheduled deferred unsuppress in {} frames", frames);
    }

    void UpdateDeferredHUDUnsuppress()
    {
        if (g_deferredUnsuppressFrames > 0) {
            g_deferredUnsuppressFrames--;
            if (g_deferredUnsuppressFrames == 0) {
                if (g_showHUDMessagesSetting) {
                    g_showHUDMessagesSetting->SetBinary(true);
                    spdlog::debug("[HUD] Deferred unsuppress complete - HUD messages restored");
                }
            }
        }
    }

    // Call ShowHUDMessage directly using the native function
    // Use this for our own "stored" and "consumed" messages
    void ShowHUDMessageDirect(const char* message, const char* sound, bool throttle, bool warning)
    {
        if (!message) return;

        spdlog::debug("[HUD] Displaying message: '{}'", message);
        ShowHUDMessage_VR(message, sound, throttle, warning);
    }

    // =========================================================================
    // GRIP WEAPON DRAW HOOK (same approach as STUF VR)
    // Hooks ReadyWeaponHandler::OnButtonEvent to prevent grip from
    // triggering weapon draw/sheathe. Other draw methods (trigger, favorites)
    // are unaffected because they don't go through ReadyWeaponHandler.
    //
    // Unlike our old 4-byte patch at 0xfc92a1, this hooks the ENTIRE function
    // at its entry point 0xfc9220, checking the button event string.
    // If the event is "WandGrip", we skip the handler entirely.
    // For any other event, we call the original function normally.
    // =========================================================================
    
    // Original function pointer (set by trampoline)
    using ReadyWeaponOnButtonEvent_t = void(*)(void* thisPtr, const RE::ButtonEvent* a_event);
    static ReadyWeaponOnButtonEvent_t g_originalReadyWeaponOnButtonEvent = nullptr;
    static bool g_gripWeaponDrawDisabled = false;
    static std::atomic<int> g_blockAllWeaponDrawFrames{0};  // >0 = block ALL weapon draws

    void HookReadyWeaponOnButtonEvent(void* thisPtr, const RE::ButtonEvent* a_event)
    {
        // Block ALL weapon draws temporarily (after storage zone unequip)
        if (g_blockAllWeaponDrawFrames.load(std::memory_order_relaxed) > 0) {
            if (a_event) {
                auto* player = f4vr::getPlayer();
                if (!player || !player->actorState.IsWeaponDrawn()) {
                    // Weapon not drawn — block the draw event to prevent re-equip
                    spdlog::debug("[GripHook] Blocked weapon draw (post-unequip cooldown)");
                    return;
                }
            }
        }

        // If grip weapon draw is disabled, check if this is a grip event
        if (g_gripWeaponDrawDisabled && a_event) {
            const auto& userEvent = a_event->QUserEvent();
            if (userEvent == "WandGrip") {
                // Allow grip through when weapon is already drawn (for reload/holster).
                // Only block when weapon is NOT drawn (prevents grip from drawing weapon).
                auto* player = f4vr::getPlayer();
                // Block WandGrip passthrough when GrabManager is holding something —
                // otherwise the game's native pickup/activate consumes the held object.
                // EXCEPTION: When a blocking menu is open, let grip through — the game
                // uses grip to close menus and won't try to pick up objects during menus.
                auto& grabMgr = heisenberg::GrabManager::GetSingleton();
                if (grabMgr.IsGrabbing(true) || grabMgr.IsGrabbing(false)) {
                    if (!MenuChecker::GetSingleton().IsGameStopped()) {
                        spdlog::debug("[GripHook] Blocked WandGrip - GrabManager has active grab");
                        return;
                    }
                    spdlog::debug("[GripHook] Menu open - passing WandGrip through despite active grab");
                }
                // When scoping, don't pass grip to ReadyWeaponHandler (would sheathe weapon).
                // Let the game's scope system handle it as "hold breath".
                if (MenuChecker::GetSingleton().IsScopeOpen()) {
                    spdlog::debug("[GripHook] ScopeMenu open - skipping ReadyWeaponHandler (hold breath)");
                    return;
                }
                if (player && player->actorState.IsWeaponDrawn()) {
                    spdlog::debug("[GripHook] Weapon drawn - passing WandGrip through (reload/holster)");
                } else {
                    spdlog::debug("[GripHook] Blocked WandGrip from ReadyWeaponHandler (weapon not drawn)");
                    return;
                }
            }
        }

        // Not a grip event (or feature disabled or weapon drawn) - call original handler
        if (g_originalReadyWeaponOnButtonEvent) {
            g_originalReadyWeaponOnButtonEvent(thisPtr, a_event);
        }
    }
    
    void SetGripWeaponDrawDisabled(bool disabled)
    {
        if (disabled == g_gripWeaponDrawDisabled) {
            return;  // Already in desired state
        }
        g_gripWeaponDrawDisabled = disabled;
        spdlog::debug("[GripHook] Grip weapon draw {}", disabled ? "DISABLED" : "ENABLED");
    }

    void BlockAllWeaponDraws(int frames)
    {
        g_blockAllWeaponDrawFrames.store(frames, std::memory_order_relaxed);
        spdlog::debug("[GripHook] Blocking ALL weapon draws for {} frames", frames);
    }

    // Weapon auto-equip block (used by EquipObject hook after storage zone unequip)
    static std::atomic<bool> g_blockWeaponAutoEquip{false};
    static std::atomic<int> g_blockWeaponAutoEquipFrames{0};

    void TickWeaponDrawBlock()
    {
        int current = g_blockAllWeaponDrawFrames.load(std::memory_order_relaxed);
        if (current > 0) {
            g_blockAllWeaponDrawFrames.store(current - 1, std::memory_order_relaxed);
        }

        // Failsafe: auto-clear weapon auto-equip block after timeout
        if (g_blockWeaponAutoEquip.load(std::memory_order_relaxed)) {
            int frames = g_blockWeaponAutoEquipFrames.load(std::memory_order_relaxed);
            if (frames > 0) {
                g_blockWeaponAutoEquipFrames.store(frames - 1, std::memory_order_relaxed);
            } else {
                g_blockWeaponAutoEquip.store(false, std::memory_order_relaxed);
                spdlog::debug("[EquipHook] Weapon auto-equip block expired (failsafe timeout)");
            }
        }
    }

    void SetBlockWeaponAutoEquip(bool block)
    {
        g_blockWeaponAutoEquip.store(block, std::memory_order_relaxed);
        g_blockWeaponAutoEquipFrames.store(block ? 600 : 0, std::memory_order_relaxed);  // 10s failsafe
        spdlog::debug("[EquipHook] Weapon auto-equip block {}", block ? "ENABLED" : "DISABLED");
    }

    void InstallGripWeaponDrawHook()
    {
        // =====================================================================
        // Manual VirtualAlloc detour - same approach as STUF VR deployed DLL
        // =====================================================================
        // ReadyWeaponHandler::OnButtonEvent at VR offset 0xFC9220
        // Prologue (17 bytes, verified from STUF VR log):
        //   48 89 5C 24 10    mov [rsp+10h], rbx   (5)
        //   55                push rbp              (1)
        //   57                push rdi              (1)
        //   41 55             push r13              (2)
        //   41 56             push r14              (2)
        //   41 57             push r15              (2)
        //   48 83 EC ??       sub rsp, ??           (4)  ← 4-byte instruction
        //                                    total = 17 bytes (clean boundary)
        //
        // We steal 17 bytes, install a 14-byte absolute jump + 3 NOPs.
        // The trampoline replays stolen bytes then jumps to original+17.
        // =====================================================================
        
        constexpr size_t STOLEN_BYTES = 17;
        
        REL::Relocation<std::uintptr_t> readyWeaponHandler{ REL::Offset(0xfc9220) };
        uintptr_t targetAddr = readyWeaponHandler.address();
        
        // Log original bytes for verification
        uint8_t origBytes[STOLEN_BYTES];
        std::memcpy(origBytes, reinterpret_cast<void*>(targetAddr), STOLEN_BYTES);
        spdlog::info("[GripHook] Target: 0x{:X}", targetAddr);
        spdlog::info("[GripHook] Original bytes: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
            origBytes[0], origBytes[1], origBytes[2], origBytes[3], origBytes[4], origBytes[5], origBytes[6],
            origBytes[7], origBytes[8], origBytes[9], origBytes[10], origBytes[11], origBytes[12], origBytes[13]);
        
        // Verify prologue matches expected bytes (first 13 bytes are the push instructions)
        const uint8_t expectedPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
        if (std::memcmp(origBytes, expectedPrologue, sizeof(expectedPrologue)) != 0) {
            spdlog::error("[GripHook] Prologue mismatch! Expected 48 89 5C 24 10 55 57 41 55 41 56 41 57. Hook NOT installed.");
            return;
        }
        
        // Allocate executable memory for trampoline (stolen bytes + absolute jump back)
        void* hookMemory = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!hookMemory) {
            spdlog::error("[GripHook] VirtualAlloc failed! Hook NOT installed.");
            return;
        }
        
        uint8_t* trampoline = reinterpret_cast<uint8_t*>(hookMemory);
        size_t pos = 0;
        
        // 1. Copy stolen bytes (17 bytes of function prologue)
        std::memcpy(trampoline, reinterpret_cast<void*>(targetAddr), STOLEN_BYTES);
        pos += STOLEN_BYTES;
        
        // 2. Absolute jump back to original function + STOLEN_BYTES
        //    FF 25 00 00 00 00 [8-byte absolute address]
        trampoline[pos++] = 0xFF;
        trampoline[pos++] = 0x25;
        trampoline[pos++] = 0x00;
        trampoline[pos++] = 0x00;
        trampoline[pos++] = 0x00;
        trampoline[pos++] = 0x00;
        uintptr_t continueAddr = targetAddr + STOLEN_BYTES;
        std::memcpy(&trampoline[pos], &continueAddr, 8);
        pos += 8;
        
        spdlog::info("[GripHook] Hook memory: 0x{:X}", reinterpret_cast<uintptr_t>(hookMemory));
        spdlog::info("[GripHook] Continue addr: 0x{:X}", continueAddr);
        spdlog::info("[GripHook] Hook code size: {} bytes", pos);
        
        // Store trampoline as the "original" function pointer
        g_originalReadyWeaponOnButtonEvent = reinterpret_cast<ReadyWeaponOnButtonEvent_t>(hookMemory);
        
        // 3. Patch the original function entry with absolute jump to our hook
        //    FF 25 00 00 00 00 [8-byte address] + NOP padding
        uint8_t patch[STOLEN_BYTES];
        patch[0] = 0xFF;   // jmp qword ptr [rip+0]
        patch[1] = 0x25;
        patch[2] = 0x00;
        patch[3] = 0x00;
        patch[4] = 0x00;
        patch[5] = 0x00;
        uintptr_t hookFuncAddr = reinterpret_cast<uintptr_t>(&HookReadyWeaponOnButtonEvent);
        std::memcpy(&patch[6], &hookFuncAddr, 8);
        // NOP remaining bytes (17 - 14 = 3)
        for (size_t i = 14; i < STOLEN_BYTES; i++) {
            patch[i] = 0x90;
        }
        
        spdlog::info("[GripHook] Installing {}-byte jump patch...", STOLEN_BYTES);
        REL::safe_write(targetAddr, patch, STOLEN_BYTES);
        
        // Verify patch was written
        uint8_t verifyBytes[STOLEN_BYTES];
        std::memcpy(verifyBytes, reinterpret_cast<void*>(targetAddr), STOLEN_BYTES);
        spdlog::info("[GripHook] Patched bytes: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
            verifyBytes[0], verifyBytes[1], verifyBytes[2], verifyBytes[3], verifyBytes[4], verifyBytes[5], verifyBytes[6],
            verifyBytes[7], verifyBytes[8], verifyBytes[9], verifyBytes[10], verifyBytes[11], verifyBytes[12], verifyBytes[13],
            verifyBytes[14], verifyBytes[15], verifyBytes[16]);
        
        spdlog::debug("[GripHook] === Grip Weapon Draw Hook Ready ===");
        spdlog::debug("[GripHook] Grip will be blocked, Trigger will work normally");

        // NOTE: STUF VR binary patch at 0xFC92A1 REMOVED.
        // The old patch unconditionally zeroed grip detection inside ReadyWeaponHandler,
        // which blocked native reload (also handled by this function).
        // The detour hook above now conditionally blocks grip only when weapon is NOT drawn,
        // allowing reload/holster to work when weapon IS drawn.

        // =====================================================================
        // MELEE THROW HANDLER PATCH — block DrawWeapon(true) fallback
        // =====================================================================
        // MeleeThrowHandler::OnButtonEvent (0xFC8AE0) is a SEPARATE handler
        // from ReadyWeaponHandler that also processes WandGrip in VR.
        // When grip is held >= fThrowDelay and NO throwable is equipped, it
        // calls player->vtable[0xC9](player, 1) = DrawWeapon(true), which
        // equips unarmed fists. This happens at two locations:
        //
        //   0xFC8C88: 7D 0D  (JGE +13 — skip DrawWeapon if weapon state >= 3)
        //   0xFC8E7E: 7D 0D  (JGE +13 — same pattern on button release path)
        //
        // Patch: change JGE (0x7D) to JMP (0xEB) so DrawWeapon is ALWAYS
        // skipped. Grenade/throwable readying uses a different code path
        // inside the same function and is NOT affected.
        // =====================================================================
        struct MeleeThrowPatch {
            uintptr_t rva;
            const char* label;
        };
        MeleeThrowPatch meleePatches[] = {
            { 0xfc8c88, "MeleeThrow DrawWeapon call 1 (grip hold)" },
            { 0xfc8e7e, "MeleeThrow DrawWeapon call 2 (grip release)" },
        };
        for (auto& p : meleePatches) {
            REL::Relocation<std::uintptr_t> loc{ REL::Offset(p.rva) };
            uintptr_t addr = loc.address();
            uint8_t origByte;
            std::memcpy(&origByte, reinterpret_cast<void*>(addr), 1);
            if (origByte == 0x7D) {
                // JGE → JMP (always skip the DrawWeapon call)
                uint8_t jmp = 0xEB;
                REL::safe_write(addr, &jmp, 1);
                spdlog::info("[GripHook] {} patched at 0x{:X} (JGE → JMP)", p.label, addr);
            } else if (origByte == 0xEB) {
                spdlog::info("[GripHook] {} already patched at 0x{:X}", p.label, addr);
            } else {
                spdlog::warn("[GripHook] {} unexpected byte 0x{:02X} at 0x{:X} — NOT patched",
                    p.label, origByte, addr);
            }
        }
    }

    // =========================================================================
    // ACTIVATE REF HOOK - Post-Drop Activation Blocker
    // Hooks TESObjectREFR::ActivateRef (VR offset 0x3f4a60) to prevent
    // immediately re-activating an item after dropping it.
    //
    // Problem: With Grip<>A SteamVR binding, releasing grip sends digital A
    // (activate) to the game. The ViewCaster selects the falling item within
    // ~20ms, and the game's activate system picks it right back up.
    // The OpenVR callback cooldown blocks GetControllerState, but the game
    // may also use SteamVR Input action API which bypasses that.
    //
    // Solution: Track recently-dropped ref formIDs with timestamps. When
    // ActivateRef is called by the player on a recently-dropped item (within
    // ~0.75s), return false (block activation).
    // =========================================================================

    // Recently dropped refs tracker — small circular buffer
    static constexpr int MAX_RECENT_DROPS = 8;
    static struct {
        uint32_t formID = 0;
        ULONGLONG tickMs = 0;
    } s_recentDrops[MAX_RECENT_DROPS];
    static int s_recentDropIdx = 0;
    static constexpr ULONGLONG DROP_COOLDOWN_MS = 750;  // Block activation for 0.75s after drop

    // Bypass flag for internal ActivateRef calls (storage, consume, etc.)
    static std::atomic<bool> g_internalActivation{false};

    void SetInternalActivation(bool active)
    {
        g_internalActivation.store(active, std::memory_order_relaxed);
    }

    void RecordDroppedRef(uint32_t formID)
    {
        if (formID == 0) return;
        s_recentDrops[s_recentDropIdx].formID = formID;
        s_recentDrops[s_recentDropIdx].tickMs = GetTickCount64();
        s_recentDropIdx = (s_recentDropIdx + 1) % MAX_RECENT_DROPS;
        spdlog::debug("[ActivateHook] Recorded drop of {:08X}", formID);
    }

    static bool WasRecentlyDropped(uint32_t formID)
    {
        if (formID == 0) return false;
        ULONGLONG now = GetTickCount64();
        for (int i = 0; i < MAX_RECENT_DROPS; ++i) {
            if (s_recentDrops[i].formID == formID &&
                (now - s_recentDrops[i].tickMs) < DROP_COOLDOWN_MS)
            {
                return true;
            }
        }
        return false;
    }

    using ActivateRef_t = bool(*)(RE::TESObjectREFR* refr,
                                   RE::TESObjectREFR* activator,
                                   RE::TESBoundObject* objectToGet,
                                   int32_t count,
                                   bool defaultProcessingOnly,
                                   bool fromScript,
                                   bool looping);
    static ActivateRef_t g_originalActivateRef = nullptr;

    bool HookActivateRef(RE::TESObjectREFR* refr,
                          RE::TESObjectREFR* activator,
                          RE::TESBoundObject* objectToGet,
                          int32_t count,
                          bool defaultProcessingOnly,
                          bool fromScript,
                          bool looping)
    {
        // Allow internal activations (storage, consume, etc.) through without checks
        if (g_internalActivation.load(std::memory_order_relaxed)) {
            if (g_originalActivateRef) {
                return g_originalActivateRef(refr, activator, objectToGet, count,
                                              defaultProcessingOnly, fromScript, looping);
            }
            return false;
        }

        // Only block player-initiated activations on grabbable item types.
        // Skip furniture (power armor frames), NPCs, activators, doors, etc.
        if (refr && activator && !fromScript) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (activator == reinterpret_cast<RE::TESObjectREFR*>(player)) {
                // Only apply blocking to item types our grab system can target
                auto* baseObj = refr->GetObjectReference();
                bool isGrabbableType = false;
                if (baseObj) {
                    auto ft = baseObj->GetFormType();
                    isGrabbableType = (ft == RE::ENUM_FORM_ID::kMISC ||
                                       ft == RE::ENUM_FORM_ID::kWEAP ||
                                       ft == RE::ENUM_FORM_ID::kARMO ||
                                       ft == RE::ENUM_FORM_ID::kALCH ||
                                       ft == RE::ENUM_FORM_ID::kAMMO ||
                                       ft == RE::ENUM_FORM_ID::kNOTE ||
                                       ft == RE::ENUM_FORM_ID::kBOOK ||
                                       ft == RE::ENUM_FORM_ID::kKEYM ||
                                       ft == RE::ENUM_FORM_ID::kFLOR);
                }
                if (!isGrabbableType) goto passthrough;

                // Block activation of recently-dropped items (post-drop cooldown)
                if (WasRecentlyDropped(refr->formID)) {
                    spdlog::debug("[ActivateHook] BLOCKED activation of recently-dropped {:08X} (post-drop cooldown)",
                                refr->formID);
                    return false;
                }

                // Block native activation when Heisenberg has a valid grab target or active grab.
                // Prevents Grip>A binding from picking items to inventory instead of grabbing.
                uint32_t targetFormID = refr->formID;
                auto& grabMgr = GrabManager::GetSingleton();
                auto& heisenberg = Heisenberg::GetSingleton();

                // Check if either hand is currently grabbing this item
                if (grabMgr.IsGrabbing(true) || grabMgr.IsGrabbing(false)) {
                    auto checkGrab = [&](bool isLeft) -> bool {
                        if (!grabMgr.IsGrabbing(isLeft)) return false;
                        auto* heldRefr = grabMgr.GetGrabState(isLeft).GetRefr();
                        return heldRefr && heldRefr->formID == targetFormID;
                    };
                    if (checkGrab(true) || checkGrab(false)) {
                        spdlog::debug("[ActivateHook] BLOCKED activation of held item {:08X}", targetFormID);
                        return false;
                    }
                }

                // Check if either hand has this item as a selected grab target
                auto checkSelection = [&](Hand* hand) -> bool {
                    if (!hand) return false;
                    auto* selRefr = hand->GetSelection().GetRefr();
                    return selRefr && selRefr->formID == targetFormID;
                };
                if (checkSelection(heisenberg.GetLeftHand()) || checkSelection(heisenberg.GetRightHand())) {
                    spdlog::debug("[ActivateHook] BLOCKED activation of grab target {:08X}", targetFormID);
                    return false;
                }
            }
        }

    passthrough:
        if (g_originalActivateRef) {
            return g_originalActivateRef(refr, activator, objectToGet, count,
                                          defaultProcessingOnly, fromScript, looping);
        }
        return false;
    }

    void InstallActivateRefHook()
    {
        // VirtualAlloc detour for TESObjectREFR::ActivateRef at 0x3f4a60
        // Same pattern as EquipObject hook.
        REL::Relocation<std::uintptr_t> activateFunc{ REL::Offset(0x3f4a60) };
        uintptr_t targetAddr = activateFunc.address();

        constexpr size_t READ_BYTES = 20;
        uint8_t origBytes[READ_BYTES];
        std::memcpy(origBytes, reinterpret_cast<void*>(targetAddr), READ_BYTES);

        spdlog::info("[ActivateHook] Target: 0x{:X}", targetAddr);
        spdlog::info("[ActivateHook] Prologue: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
            origBytes[0], origBytes[1], origBytes[2], origBytes[3], origBytes[4],
            origBytes[5], origBytes[6], origBytes[7], origBytes[8], origBytes[9],
            origBytes[10], origBytes[11], origBytes[12], origBytes[13], origBytes[14],
            origBytes[15], origBytes[16], origBytes[17], origBytes[18], origBytes[19]);

        // Detect prologue pattern
        size_t stolenBytes = 0;
        if (origBytes[0] == 0x48 && origBytes[1] == 0x8B && origBytes[2] == 0xC4) {
            stolenBytes = 3;  // mov rax, rsp
        }
        else if (origBytes[0] == 0x48 && origBytes[1] == 0x89 && origBytes[2] == 0x5C) {
            stolenBytes = 5;  // mov [rsp+xx], rbx
        }
        else if (origBytes[0] == 0x40 && origBytes[1] == 0x53) {
            stolenBytes = 2;  // push rbx
        }
        else if (origBytes[0] == 0x48 && origBytes[1] == 0x89 && origBytes[2] == 0x54) {
            stolenBytes = 5;  // mov [rsp+xx], rdx
        }
        else if (origBytes[0] == 0x4C && origBytes[1] == 0x8B && origBytes[2] == 0xDC) {
            stolenBytes = 3;  // mov r11, rsp
        }

        if (stolenBytes == 0) {
            spdlog::error("[ActivateHook] Unknown prologue pattern! First bytes: {:02X} {:02X} {:02X}. Hook NOT installed.",
                origBytes[0], origBytes[1], origBytes[2]);
            return;
        }

        // Continue counting instructions until >= 14 bytes
        size_t pos = stolenBytes;
        while (pos < 14) {
            uint8_t b = origBytes[pos];
            bool hasRex = (b >= 0x40 && b <= 0x4F);
            if (hasRex) { pos++; b = origBytes[pos]; }

            if (b == 0x53 || b == 0x54 || b == 0x55 || b == 0x56 || b == 0x57) {
                pos += 1;
            }
            else if (b == 0x83 && origBytes[pos+1] == 0xEC) {
                pos += 3;
            }
            else if (b == 0x81 && origBytes[pos+1] == 0xEC) {
                pos += 6;
            }
            else if (b == 0x89 && (origBytes[pos+1] & 0xC7) == 0x44 && origBytes[pos+2] == 0x24) {
                pos += 4;  // mov [rsp+disp8], reg (any reg) — ModRM mod=01 r/m=100 + SIB(0x24) + disp8
            }
            else if (b == 0x89 && (origBytes[pos+1] & 0xC0) == 0x40) {
                pos += 3;  // mov [reg+disp8], reg (no SIB)
            }
            else if (b == 0x89 && (origBytes[pos+1] & 0xC0) == 0x80) {
                pos += 6;
            }
            else if (b == 0x8B && origBytes[pos+1] == 0xC4) {
                pos += 2;
            }
            else if (b == 0x8B && origBytes[pos+1] == 0xEC) {
                pos += 2;
            }
            else if (b == 0x8B && origBytes[pos+1] == 0xDC) {
                pos += 2;
            }
            else if (b == 0x8D && (origBytes[pos+1] & 0xC0) == 0x40) {
                pos += 3;
            }
            else if (b == 0x8D && (origBytes[pos+1] & 0xC0) == 0x80) {
                pos += 6;
            }
            else if (b == 0x8D && (origBytes[pos+1] & 0xC0) == 0x00) {
                pos += 2;
            }
            else {
                spdlog::error("[ActivateHook] Unknown instruction at offset {} (byte {:02X}). Hook NOT installed.", pos, b);
                return;
            }
        }
        stolenBytes = pos;
        spdlog::info("[ActivateHook] Stealing {} bytes of prologue", stolenBytes);

        void* hookMemory = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!hookMemory) {
            spdlog::error("[ActivateHook] VirtualAlloc failed! Hook NOT installed.");
            return;
        }

        uint8_t* trampoline = reinterpret_cast<uint8_t*>(hookMemory);
        size_t tpos = 0;

        // 1. Copy stolen bytes
        std::memcpy(trampoline, reinterpret_cast<void*>(targetAddr), stolenBytes);
        tpos += stolenBytes;

        // 2. Absolute jump back to original + stolenBytes
        trampoline[tpos++] = 0xFF;
        trampoline[tpos++] = 0x25;
        trampoline[tpos++] = 0x00;
        trampoline[tpos++] = 0x00;
        trampoline[tpos++] = 0x00;
        trampoline[tpos++] = 0x00;
        uintptr_t continueAddr = targetAddr + stolenBytes;
        std::memcpy(&trampoline[tpos], &continueAddr, 8);
        tpos += 8;

        g_originalActivateRef = reinterpret_cast<ActivateRef_t>(hookMemory);

        // 3. Patch original function entry
        std::vector<uint8_t> patch(stolenBytes, 0x90);
        patch[0] = 0xFF;
        patch[1] = 0x25;
        patch[2] = 0x00;
        patch[3] = 0x00;
        patch[4] = 0x00;
        patch[5] = 0x00;
        uintptr_t hookFuncAddr = reinterpret_cast<uintptr_t>(&HookActivateRef);
        std::memcpy(&patch[6], &hookFuncAddr, 8);

        spdlog::info("[ActivateHook] Installing {}-byte jump patch...", stolenBytes);
        REL::safe_write(targetAddr, patch.data(), stolenBytes);

        spdlog::info("[ActivateHook] === ActivateRef Hook Ready ===");
    }

    // =========================================================================
    // EQUIP OBJECT HOOK - Consumable/Favorite to Hand
    // Hooks ActorEquipManager::EquipObject to intercept consumable usage
    // from Pipboy and Favorites menus, redirecting to drop-to-hand.
    //
    // When consumableToHand is enabled and PipboyMenu is open, consumables
    // (AlchemyItems) are dropped to hand instead of being consumed.
    // When favoritesToHand is enabled and FavoritesMenu is open, same thing.
    // =========================================================================
    
    using EquipObject_t = bool(*)(RE::ActorEquipManager* equipManager,
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
    static EquipObject_t g_originalEquipObject = nullptr;
    static ULONGLONG s_holotapeRedirectTick = 0;
    // Temporarily changed holotape type to prevent game's secondary TerminalMenu open
    static std::uint32_t s_redirectedNoteFormID = 0;
    static int s_redirectedNoteOrigType = -1;

    bool HookEquipObject(RE::ActorEquipManager* equipManager,
                          RE::Actor* actor,
                          RE::BGSObjectInstance* instance,
                          std::uint32_t stackID,
                          std::uint32_t number,
                          RE::BGSEquipSlot* slot,
                          bool queueEquip,
                          bool forceEquip,
                          bool playSounds,
                          bool applyNow,
                          bool locked)
    {
        // Only intercept for the player
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (actor == player && instance) {
            // Access the base form from BGSObjectInstance (first member is TESForm*)
            RE::TESForm* baseForm = *reinterpret_cast<RE::TESForm**>(instance);

            // Block weapon auto-equip after storage zone unequip.
            // This lets DrawWeaponMagicHands(true) fall through to unarmed (fists)
            // instead of re-equipping the last weapon from inventory.
            if (g_blockWeaponAutoEquip.load(std::memory_order_relaxed)) {
                if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
                    g_blockWeaponAutoEquip.store(false, std::memory_order_relaxed);
                    spdlog::debug("[EquipHook] Blocked weapon auto-equip {:08X} after storage zone unequip",
                        baseForm->formID);
                    return true;  // Skip equip — game falls back to unarmed
                }
            }

            // In Power Armor (when configured): let consumables and holotapes use native activation
            // (no drop-to-hand). Consumables can't be eaten with PA helmet, and
            // holotapes cause physics issues when spawned in PA.
            bool blockPA = g_config.blockConsumptionInPA && Utils::IsPlayerInPowerArmor();

            if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kALCH && !blockPA) {
                // Guard: skip if we just redirected this same item (game may call EquipObject twice)
                static RE::TESFormID s_lastRedirectedFormID = 0;
                static ULONGLONG s_lastRedirectedTick = 0;
                ULONGLONG now = GetTickCount64();
                if (baseForm->formID == s_lastRedirectedFormID &&
                    (now - s_lastRedirectedTick) < 200) {  // 200ms window
                    spdlog::debug("[EquipHook] BLOCKED duplicate equip call for {:08X} ({} ms ago)",
                        baseForm->formID, now - s_lastRedirectedTick);
                    return true;  // Skip — already redirected
                }

                auto& menuChecker = MenuChecker::GetSingleton();
                bool shouldRedirect = false;
                const char* source = nullptr;

                if (g_config.consumableToHand && menuChecker.IsPipboyOpen()) {
                    shouldRedirect = true;
                    source = "Pipboy";
                }
                else if (g_config.favoritesToHand && menuChecker.IsFavoritesOpen()) {
                    shouldRedirect = true;
                    source = "Favorites";
                }
                
                if (shouldRedirect) {
                    auto& grabMgr = GrabManager::GetSingleton();
                    
                    // Check if any hand is free — if both hands are occupied, let consume happen normally
                    bool leftHolding = grabMgr.IsGrabbing(true);
                    bool rightHolding = grabMgr.IsGrabbing(false);
                    if (leftHolding && rightHolding) {
                        spdlog::debug("[EquipHook] Both hands occupied — letting consumable be used normally");
                        shouldRedirect = false;
                    }
                }
                
                if (shouldRedirect) {
                    auto& dropToHand = DropToHand::GetSingleton();
                    bool isLeft = true;
                    dropToHand.GetTargetHand(isLeft);
                    
                    const char* itemName = "unknown";
                    auto* alchItem = static_cast<RE::AlchemyItem*>(baseForm);
                    if (alchItem->GetFullName()) {
                        itemName = alchItem->GetFullName();
                    }
                    
                    spdlog::debug("[EquipHook] Redirecting consumable '{}' ({:08X}) from {} to hand (isLeft={})",
                        itemName, baseForm->formID, source, isLeft);
                    
                    // Play pickup sound for feedback
                    auto* boundObj = baseForm->As<RE::TESBoundObject>();
                    if (boundObj) {
                        player->PlayPickUpSound(boundObj, true, false);
                    }
                    
                    // Always drop exactly 1 — `number` from EquipObject is an opaque stack param, not a count
                    dropToHand.QueueDropToHand(baseForm->formID, isLeft, 1, true, false);

                    // Track redirect for dedup guard (game may call EquipObject twice)
                    s_lastRedirectedFormID = baseForm->formID;
                    s_lastRedirectedTick = now;
                    return true;  // Skip original equip (prevents consumption)
                }
            }

            // Holotapes: redirect to right hand instead of playing.
            // Always free the right hand: holster weapon or store held item.
            // Skip in Power Armor (when configured) — holotapes use native activation to avoid physics issues.
            // Game holotapes (kProgram) also drop to hand — they only play when inserted into the holotape deck.
            // Block ALL holotape equips while a holotape game is actively playing on Pipboy
            // (right trigger on Pipboy can trigger re-equip — just swallow it).
            if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kNOTE && !blockPA) {
                auto& pipboy = PipboyInteraction::GetSingleton();
                if (pipboy.IsProgramSWFActive()) {
                    spdlog::debug("[EquipHook] Blocked holotape equip during active game playback");
                    return true;  // Swallow — don't redirect, don't play
                }
                // Block if this holotape is already loaded in the tape deck
                if (pipboy.HasHolotapeLoaded() && pipboy.GetLoadedHolotapeFormID() == baseForm->formID) {
                    spdlog::debug("[EquipHook] Holotape {:08X} already in tape deck — ignoring inventory activation",
                        baseForm->formID);
                    return true;  // Swallow — holotape is already inserted
                }
                if (g_config.holotapeToHand) {
                auto& menuChecker = MenuChecker::GetSingleton();
                if (menuChecker.IsPipboyOpen() || menuChecker.IsFavoritesOpen()) {
                    auto nameView = RE::TESFullName::GetFullName(*baseForm, false);
                    std::string itemName = nameView.empty() ? "holotape" : std::string(nameView);

                    auto& grabMgr = GrabManager::GetSingleton();

                    // Free the right hand if occupied
                    if (grabMgr.IsGrabbing(false)) {
                        const auto& grabState = grabMgr.GetGrabState(false);
                        auto* heldRefr = grabState.GetRefr();
                        if (heldRefr) {
                            // Keep a smart pointer so ref survives EndGrab
                            RE::NiPointer<RE::TESObjectREFR> heldRef(heldRefr);
                            auto* heldBase = heldRefr->GetObjectReference();

                            // Mark as recently stored to prevent loot-to-hand re-grab
                            if (heldBase) {
                                DropToHand::GetSingleton().MarkAsRecentlyStored(heldBase->formID);
                            }

                            // End the grab first (releases physics hold)
                            grabMgr.EndGrab(false, nullptr, true);

                            // Store to inventory
                            SetSuppressHUDMessages(true);
                            if (heldBase && heldBase->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                                // Holotapes: use AddObjectToContainer to avoid triggering playback
                                RE::BSTSmartPointer<RE::ExtraDataList> nullExtra;
                                heisenberg::AddObjectToContainer(
                                    player, static_cast<RE::TESBoundObject*>(heldBase),
                                    &nullExtra, 1, nullptr, 0);
                                heisenberg::SafeDisable(heldRef.get());
                            } else {
                                SetInternalActivation(true);
                                heldRef->ActivateRef(player, nullptr, 1, false, false, false);
                                SetInternalActivation(false);
                            }
                            SetSuppressHUDMessages(false);

                            spdlog::debug("[EquipHook] Stored held item to free right hand for holotape '{}'", itemName);
                        }
                    }

                    // Holster weapon if drawn (vrPlayer for state check, RE player for vtable call)
                    auto* vrPlayer = f4vr::getPlayer();
                    if (vrPlayer && vrPlayer->actorState.IsWeaponDrawn()) {
                        player->DrawWeaponMagicHands(false);
                        spdlog::debug("[EquipHook] Holstered weapon for holotape '{}'", itemName);
                    }

                    spdlog::debug("[EquipHook] Redirecting holotape '{}' ({:08X}) to right hand",
                        itemName, baseForm->formID);

                    auto* boundObj = baseForm->As<RE::TESBoundObject>();
                    if (boundObj) {
                        player->PlayPickUpSound(boundObj, true, false);
                    }

                    auto& dropToHand = DropToHand::GetSingleton();
                    dropToHand.QueueDropToHand(baseForm->formID, false, 1, true, false);  // false = right hand

                    // Mark timestamp so PipboyInteraction can suppress the terminal that
                    // the game opens through a separate code path (not EquipObject).
                    s_holotapeRedirectTick = GetTickCount64();

                    // Temporarily change kTerminal holotapes to kVoice so the game's
                    // secondary code path won't open TerminalMenu (which plays typing sounds).
                    // Type is restored in RestoreRedirectedHolotapeType() after a few frames.
                    auto* note = baseForm->As<RE::BGSNote>();
                    if (note && (note->type == RE::BGSNote::NOTE_TYPE::kTerminal ||
                                 note->type == RE::BGSNote::NOTE_TYPE::kProgram)) {
                        s_redirectedNoteFormID = baseForm->formID;
                        s_redirectedNoteOrigType = static_cast<int>(note->type);
                        note->type = RE::BGSNote::NOTE_TYPE::kVoice;
                        spdlog::debug("[EquipHook] Temporarily changed holotape {:08X} type {} → kVoice",
                            baseForm->formID, s_redirectedNoteOrigType);
                    }

                    return true;  // Skip original equip (prevents holotape playback)
                }
                }  // menuChecker check
            }
        }

        // Not intercepted - call original
        if (g_originalEquipObject) {
            // Debug: log non-intercepted player equips to diagnose PA sound issue
            if (actor == RE::PlayerCharacter::GetSingleton() && instance) {
                RE::TESForm* dbgForm = *reinterpret_cast<RE::TESForm**>(instance);
                if (dbgForm) {
                    spdlog::debug("[EquipHook] Passthrough: {:08X} type={} playSounds={} queueEquip={} forceEquip={}",
                        dbgForm->formID, static_cast<int>(dbgForm->GetFormType()),
                        playSounds, queueEquip, forceEquip);
                }
            }
            return g_originalEquipObject(equipManager, actor, instance, stackID, number,
                                          slot, queueEquip, forceEquip, playSounds, applyNow, locked);
        }
        return false;
    }
    
    bool WasHolotapeJustRedirected()
    {
        if (s_holotapeRedirectTick == 0) return false;
        ULONGLONG now = GetTickCount64();
        if ((now - s_holotapeRedirectTick) < 500) return true;
        s_holotapeRedirectTick = 0;  // expired
        return false;
    }

    void RestoreRedirectedHolotapeType()
    {
        if (s_redirectedNoteFormID == 0 || s_redirectedNoteOrigType < 0) return;
        auto* form = RE::TESForm::GetFormByID(s_redirectedNoteFormID);
        if (form && form->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
            auto* note = static_cast<RE::BGSNote*>(form);
            note->type = static_cast<RE::BGSNote::NOTE_TYPE>(s_redirectedNoteOrigType);
            spdlog::debug("[EquipHook] Restored holotape {:08X} type to {}",
                s_redirectedNoteFormID, s_redirectedNoteOrigType);
        }
        s_redirectedNoteFormID = 0;
        s_redirectedNoteOrigType = -1;
    }

    void InstallEquipObjectHook()
    {
        // =====================================================================
        // VirtualAlloc detour for ActorEquipManager::EquipObject at 0xe6fea0
        // Same pattern as the GripWeaponDraw hook.
        // We read the first N bytes of prologue, verify them, then install
        // an absolute jump to our hook function.
        // =====================================================================
        
        REL::Relocation<std::uintptr_t> equipObjectFunc{ REL::Offset(0xe6fea0) };
        uintptr_t targetAddr = equipObjectFunc.address();
        
        // Read prologue bytes for verification and logging
        constexpr size_t READ_BYTES = 20;
        uint8_t origBytes[READ_BYTES];
        std::memcpy(origBytes, reinterpret_cast<void*>(targetAddr), READ_BYTES);
        
        spdlog::info("[EquipHook] Target: 0x{:X}", targetAddr);
        spdlog::info("[EquipHook] Prologue: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
            origBytes[0], origBytes[1], origBytes[2], origBytes[3], origBytes[4],
            origBytes[5], origBytes[6], origBytes[7], origBytes[8], origBytes[9],
            origBytes[10], origBytes[11], origBytes[12], origBytes[13], origBytes[14],
            origBytes[15], origBytes[16], origBytes[17], origBytes[18], origBytes[19]);
        
        // Common F4VR function prologues:
        // Pattern A: 48 89 5C 24 xx  mov [rsp+xx], rbx  (5 bytes)
        // Pattern B: 40 53           push rbx            (2 bytes)
        //            48 83 EC xx     sub rsp, xx         (4 bytes)
        // Pattern C: 48 8B C4        mov rax, rsp        (3 bytes)
        // We need at least 14 bytes for a 64-bit absolute jump.
        // Detect the prologue pattern to determine safe stolen byte count.
        
        size_t stolenBytes = 0;
        
        // Try to find a safe boundary >= 14 bytes by simple instruction length counting
        // Common x64 instruction patterns at function entries:
        if (origBytes[0] == 0x48 && origBytes[1] == 0x8B && origBytes[2] == 0xC4) {
            // mov rax, rsp (3 bytes) - common prologue start
            stolenBytes = 3;
        }
        else if (origBytes[0] == 0x48 && origBytes[1] == 0x89 && origBytes[2] == 0x5C) {
            // mov [rsp+xx], rbx (5 bytes)
            stolenBytes = 5;
        }
        else if (origBytes[0] == 0x40 && origBytes[1] == 0x53) {
            // push rbx (2 bytes)
            stolenBytes = 2;
        }
        else if (origBytes[0] == 0x48 && origBytes[1] == 0x89 && origBytes[2] == 0x54) {
            // mov [rsp+xx], rdx (5 bytes)
            stolenBytes = 5;
        }
        else if (origBytes[0] == 0x4C && origBytes[1] == 0x8B && origBytes[2] == 0xDC) {
            // mov r11, rsp (3 bytes)
            stolenBytes = 3;
        }
        
        if (stolenBytes == 0) {
            spdlog::error("[EquipHook] Unknown prologue pattern! First bytes: {:02X} {:02X} {:02X}. Hook NOT installed.",
                origBytes[0], origBytes[1], origBytes[2]);
            return;
        }
        
        // Continue counting instructions until we reach >= 14 bytes
        // Simple decoder for common x64 instructions
        size_t pos = stolenBytes;
        while (pos < 14) {
            uint8_t b = origBytes[pos];
            // REX prefixes: 0x40-0x4F
            bool hasRex = (b >= 0x40 && b <= 0x4F);
            if (hasRex) {
                pos++;
                b = origBytes[pos];
            }
            
            if (b == 0x53 || b == 0x54 || b == 0x55 || b == 0x56 || b == 0x57) {
                pos += 1;  // push rbx/rsp/rbp/rsi/rdi (and r12-r15 with REX, 1 byte + optional REX)
            }
            else if (b == 0x83 && origBytes[pos+1] == 0xEC) {
                pos += 3;  // sub rsp, imm8 (3 bytes)
            }
            else if (b == 0x81 && origBytes[pos+1] == 0xEC) {
                pos += 6;  // sub rsp, imm32 (6 bytes)
            }
            else if (b == 0x89 && (origBytes[pos+1] & 0xC0) == 0x40) {
                pos += 3;  // mov [reg+disp8], reg (3 bytes)
            }
            else if (b == 0x89 && (origBytes[pos+1] & 0xC0) == 0x80) {
                pos += 6;  // mov [reg+disp32], reg (6 bytes)
            }
            else if (b == 0x89 && origBytes[pos+1] == 0x5C && origBytes[pos+2] == 0x24) {
                pos += 4;  // mov [rsp+disp8], reg (4 bytes w/ SIB)
            }
            else if (b == 0x8B && origBytes[pos+1] == 0xC4) {
                pos += 2;  // mov eax/rax, esp/rsp (2 bytes)
            }
            else if (b == 0x8B && origBytes[pos+1] == 0xEC) {
                pos += 2;  // mov ebp, esp (2 bytes)
            }
            else if (b == 0x8B && origBytes[pos+1] == 0xDC) {
                pos += 2;  // mov ebx, esp (2 bytes) or mov r11, rsp with REX
            }
            else if (b == 0x8D && (origBytes[pos+1] & 0xC0) == 0x40) {
                pos += 3;  // lea reg, [reg+disp8]  (opcode + ModRM + disp8)
            }
            else if (b == 0x8D && (origBytes[pos+1] & 0xC0) == 0x80) {
                pos += 6;  // lea reg, [reg+disp32] (opcode + ModRM + disp32)
            }
            else if (b == 0x8D && (origBytes[pos+1] & 0xC0) == 0x00) {
                pos += 2;  // lea reg, [reg]        (opcode + ModRM, no disp)
            }
            else {
                spdlog::error("[EquipHook] Unknown instruction at offset {} (byte {:02X}). Cannot determine safe boundary. Hook NOT installed.",
                    pos, b);
                return;
            }
        }
        stolenBytes = pos;
        spdlog::info("[EquipHook] Stealing {} bytes of prologue", stolenBytes);
        
        // Allocate executable memory for trampoline
        void* hookMemory = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!hookMemory) {
            spdlog::error("[EquipHook] VirtualAlloc failed! Hook NOT installed.");
            return;
        }
        
        uint8_t* trampoline = reinterpret_cast<uint8_t*>(hookMemory);
        size_t tpos = 0;
        
        // 1. Copy stolen bytes
        std::memcpy(trampoline, reinterpret_cast<void*>(targetAddr), stolenBytes);
        tpos += stolenBytes;
        
        // 2. Absolute jump back to original function + stolenBytes
        trampoline[tpos++] = 0xFF;
        trampoline[tpos++] = 0x25;
        trampoline[tpos++] = 0x00;
        trampoline[tpos++] = 0x00;
        trampoline[tpos++] = 0x00;
        trampoline[tpos++] = 0x00;
        uintptr_t continueAddr = targetAddr + stolenBytes;
        std::memcpy(&trampoline[tpos], &continueAddr, 8);
        tpos += 8;
        
        // Store trampoline as original function pointer
        g_originalEquipObject = reinterpret_cast<EquipObject_t>(hookMemory);
        
        // 3. Patch original function entry with absolute jump to our hook
        std::vector<uint8_t> patch(stolenBytes, 0x90); // Fill with NOPs
        patch[0] = 0xFF;   // jmp qword ptr [rip+0]
        patch[1] = 0x25;
        patch[2] = 0x00;
        patch[3] = 0x00;
        patch[4] = 0x00;
        patch[5] = 0x00;
        uintptr_t hookFuncAddr = reinterpret_cast<uintptr_t>(&HookEquipObject);
        std::memcpy(&patch[6], &hookFuncAddr, 8);
        // Remaining bytes already NOPs
        
        spdlog::info("[EquipHook] Installing {}-byte jump patch...", stolenBytes);
        REL::safe_write(targetAddr, patch.data(), stolenBytes);
        
        spdlog::info("[EquipHook] === EquipObject Hook Ready ===");
        spdlog::info("[EquipHook] consumableToHand={}, favoritesToHand={}, holotapeToHand={}",
            g_config.consumableToHand, g_config.favoritesToHand, g_config.holotapeToHand);
    }

    // =========================================================================
    // HUDRollover vtable hook — null actionText at +0x618 before ShowRollover
    // VR has two HUDRollover instances (left/right wand)
    // Right wand vtable (Primary): 0x2D4E438, slot[4] → 0xAB7B20 (HUDPrimaryWandRollover::vf004)
    // Left wand vtable (Secondary): 0x2D4E508, slot[4] → 0xAB7D40 (HUDSecondaryWandRollover::vf004)
    // =========================================================================

    // ShowRollover signature: void(HUDRollover* this)
    // vf004 — reads button data from instance fields at +0x610, builds params internally
    using ShowRolloverFunc = void(__fastcall*)(void*);
    static ShowRolloverFunc g_origShowRolloverLeft = nullptr;
    static ShowRolloverFunc g_origShowRolloverRight = nullptr;

    // Data layout at +0x610 (ShowRolloverParameters, copied by FUN_140abb230 into local stack frame):
    //   +0x610: BSFixedString itemName      (null/empty → hides item name display)
    //   +0x618: BSFixedString activateText  (null/empty → hides [A] Store/Take button)
    //   +0x620: BSFixedString secondaryText (null/empty → hides [B] Transfer button)
    //   +0x62A: byte playFlag              (zero → prevents "$PLAY" holotape prompt)
    // Ghidra-confirmed: HUDPrimaryWandRollover and HUDSecondaryWandRollover use IDENTICAL vf004.
    // Each field is independently checked via FUN_141bc1a80 (string-length test — null ptr → 0).
    // itemName and activateText are independent: nulling activateText does NOT affect itemName.
    struct RolloverSaveState {
        uintptr_t itemName = 0;
        uintptr_t activateText = 0;
        uintptr_t secondaryText = 0;
        uint8_t   playFlag = 0;
    };
    static thread_local RolloverSaveState _savedLeft;
    static thread_local RolloverSaveState _savedRight;

    // Pre-cached BSFixedString raw values — initialized in InstallHUDRolloverHook,
    // NEVER inside the vtable hook itself (BSFixedString init inside a rendering
    // callback corrupts the HUD display).
    static uintptr_t g_holotapeHintEntry = 0;
    static uintptr_t g_touchHintEntry = 0;

    // Check if the ref the wand is pointing at is a tracked activator or terminal
    static bool IsWandTargetTrackedActivator(bool isLeft)
    {
        RE::ObjectRefHandle handle = heisenberg::GetVRWandTargetHandle(isLeft);
        if (!handle) return false;

        RE::NiPointer<RE::TESObjectREFR> refr = handle.get();
        if (!refr) return false;

        auto& actHandler = heisenberg::ActivatorHandler::GetSingleton();
        for (const auto& tracked : actHandler.GetTrackedActivators()) {
            if (tracked.formID == refr->formID) return true;
        }
        for (const auto& tracked : actHandler.GetTrackedTerminals()) {
            if (tracked.formID == refr->formID) return true;
        }
        return false;
    }

    // Hide wand button prompts while preserving item name display.
    // Ghidra-confirmed (F4VR 1.2.72): both HUDPrimaryWandRollover and HUDSecondaryWandRollover
    // use IDENTICAL ShowRollover code. Item name visibility is controlled by itemName (+0x610),
    // which we never touch. Button visibility is determined by FUN_141bc1a80 on activateText —
    // a string-length check that returns 0 for null (0 pointer). Null is correct for both wands.
    static void HideWandButtons(void* thisPtr, RolloverSaveState& save)
    {
        auto base = reinterpret_cast<uintptr_t>(thisPtr);
        auto* activateText  = reinterpret_cast<uintptr_t*>(base + 0x618);
        auto* secondaryText = reinterpret_cast<uintptr_t*>(base + 0x620);
        auto* playFlag      = reinterpret_cast<uint8_t*>(base + 0x62A);

        save.activateText  = *activateText;
        save.secondaryText = *secondaryText;
        save.playFlag      = *playFlag;

        *activateText  = 0;
        *secondaryText = 0;
        *playFlag      = 0;
    }

    // Null ALL rollover fields including item name (for hideAllWandHUD)
    static void NullAllRolloverFields(void* thisPtr, RolloverSaveState& save)
    {
        auto base = reinterpret_cast<uintptr_t>(thisPtr);
        auto* itemName      = reinterpret_cast<uintptr_t*>(base + 0x610);
        auto* activateText  = reinterpret_cast<uintptr_t*>(base + 0x618);
        auto* secondaryText = reinterpret_cast<uintptr_t*>(base + 0x620);
        auto* playFlag      = reinterpret_cast<uint8_t*>(base + 0x62A);

        save.itemName      = *itemName;
        save.activateText  = *activateText;
        save.secondaryText = *secondaryText;
        save.playFlag      = *playFlag;

        *itemName      = 0;
        *activateText  = 0;
        *secondaryText = 0;
        *playFlag      = 0;
    }

    // Replace prompts for tracked activators and holotapes (hideWandHUD = OFF).
    // Returns true if any replacement was made (caller must RestoreRolloverButtons after).
    static bool ReplaceRolloverPrompts(void* thisPtr, bool isLeft, RolloverSaveState& save)
    {
        auto base = reinterpret_cast<uintptr_t>(thisPtr);
        auto* activateText  = reinterpret_cast<uintptr_t*>(base + 0x618);
        auto* secondaryText = reinterpret_cast<uintptr_t*>(base + 0x620);
        auto* playFlag      = reinterpret_cast<uint8_t*>(base + 0x62A);

        // Holotape: replace "$PLAY" button with "Insert to Play" hint text
        if (*playFlag != 0) {
            save.activateText  = *activateText;
            save.secondaryText = *secondaryText;
            save.playFlag      = *playFlag;

            *activateText  = 0;
            *playFlag      = 0;
            *secondaryText = g_holotapeHintEntry;
            return true;
        }

        // Tracked activator: replace "[A] Activate" with "[A] Touch"
        if (*activateText != 0 && IsWandTargetTrackedActivator(isLeft)) {
            save.activateText  = *activateText;
            save.secondaryText = *secondaryText;
            save.playFlag      = *playFlag;

            *activateText = g_touchHintEntry;
            return true;
        }

        return false;
    }

    static void RestoreRolloverButtons(void* thisPtr, const RolloverSaveState& save)
    {
        auto base = reinterpret_cast<uintptr_t>(thisPtr);
        *reinterpret_cast<uintptr_t*>(base + 0x618) = save.activateText;
        *reinterpret_cast<uintptr_t*>(base + 0x620) = save.secondaryText;
        *reinterpret_cast<uint8_t*>(base + 0x62A)   = save.playFlag;
    }

    static void RestoreAllRolloverFields(void* thisPtr, const RolloverSaveState& save)
    {
        auto base = reinterpret_cast<uintptr_t>(thisPtr);
        *reinterpret_cast<uintptr_t*>(base + 0x610) = save.itemName;
        *reinterpret_cast<uintptr_t*>(base + 0x618) = save.activateText;
        *reinterpret_cast<uintptr_t*>(base + 0x620) = save.secondaryText;
        *reinterpret_cast<uint8_t*>(base + 0x62A)   = save.playFlag;
    }

    static void __fastcall HookShowRolloverLeft(void* thisPtr)
    {
        // When a blocking menu (PauseMenu, LoadingMenu, etc.) is open, skip entirely —
        // Scaleform state may be torn down/invalid, calling the original crashes.
        if (!thisPtr || MenuChecker::GetSingleton().IsGameStopped()) {
            return;
        }

        if (g_config.hideAllWandHUD) {
            NullAllRolloverFields(thisPtr, _savedLeft);
            if (g_origShowRolloverLeft) g_origShowRolloverLeft(thisPtr);
            RestoreAllRolloverFields(thisPtr, _savedLeft);
            return;
        }
        if (g_config.hideWandHUD) {
            HideWandButtons(thisPtr, _savedLeft);
            if (g_origShowRolloverLeft) g_origShowRolloverLeft(thisPtr);
            RestoreRolloverButtons(thisPtr, _savedLeft);
            return;
        }
        bool replaced = ReplaceRolloverPrompts(thisPtr, true, _savedLeft);
        if (g_origShowRolloverLeft) g_origShowRolloverLeft(thisPtr);
        if (replaced) RestoreRolloverButtons(thisPtr, _savedLeft);
    }

    static void __fastcall HookShowRolloverRight(void* thisPtr)
    {
        // When a blocking menu (PauseMenu, LoadingMenu, etc.) is open, skip entirely —
        // Scaleform state may be torn down/invalid, calling the original crashes.
        if (!thisPtr || MenuChecker::GetSingleton().IsGameStopped()) {
            return;
        }

        if (g_config.hideAllWandHUD) {
            NullAllRolloverFields(thisPtr, _savedRight);
            if (g_origShowRolloverRight) g_origShowRolloverRight(thisPtr);
            RestoreAllRolloverFields(thisPtr, _savedRight);
            return;
        }
        if (g_config.hideWandHUD) {
            HideWandButtons(thisPtr, _savedRight);
            if (g_origShowRolloverRight) g_origShowRolloverRight(thisPtr);
            RestoreRolloverButtons(thisPtr, _savedRight);
            return;
        }
        bool replaced = ReplaceRolloverPrompts(thisPtr, false, _savedRight);
        if (g_origShowRolloverRight) g_origShowRolloverRight(thisPtr);
        if (replaced) RestoreRolloverButtons(thisPtr, _savedRight);
    }

    // =========================================================================
    // HUDQuickContainer vtable hook — force-hide [A] Take / [B] Transfer buttons
    // Single vtable at 0x2D4D498, slot[4] → vf004 (0xa8c1a0)
    // After calling original vf004, call FUN_140b237d0 (ButtonVisible setter)
    // on ButtonHintData at +0x418, +0x4c8, +0x578 to force them invisible.
    // =========================================================================
    using QuickContainerVf004Func = void(__fastcall*)(void*);
    static QuickContainerVf004Func g_origQuickContainerVf004 = nullptr;

    // FUN_140b237d0: void(ButtonHintData* this, bool visible)
    using SetButtonVisibleFunc = void(*)(void*, bool);

    static void __fastcall HookQuickContainerVf004(void* thisPtr)
    {
        // Just pass through to original — no asymmetric hiding.
        // QuickContainer is a single vtable with no left-wand counterpart,
        // so any hiding here would create a left/right mismatch.
        if (g_origQuickContainerVf004) {
            g_origQuickContainerVf004(thisPtr);
        }
    }

    // Called once from the first frame update to pre-warm BSFixedString entries.
    // Cannot be done during F4SEPlugin_Load (string pool not ready) or inside
    // vtable hooks (corrupts HUD display). First frame update is the safe spot.
    void InitRolloverStrings()
    {
        static bool initialized = false;
        if (initialized) return;
        initialized = true;

        static RE::BSFixedString sHolotape("Insert to Play");
        g_holotapeHintEntry = *reinterpret_cast<uintptr_t*>(&sHolotape);
        static RE::BSFixedString sTouch("Touch");
        g_touchHintEntry = *reinterpret_cast<uintptr_t*>(&sTouch);
    }

    void InstallHUDRolloverHook()
    {
        uintptr_t base = REL::Module::get().base();

        // Two HUDRollover subclass vtables for wand interact rollovers:
        //   0x2D4E438 — Right wand rollover (HUDPrimaryWandRollover, slot[4] → 0xAB7B20)
        //   0x2D4E508 — Left wand rollover  (HUDSecondaryWandRollover, slot[4] → 0xAB7D40)
        // Hook slot[4] (ShowRollover) on each to null actionText at +0x618.

        uintptr_t rightVtable = base + 0x2D4E438;
        uintptr_t leftVtable  = base + 0x2D4E508;

        uintptr_t rightVtableSlot = rightVtable + (4 * sizeof(uintptr_t));
        uintptr_t leftVtableSlot  = leftVtable  + (4 * sizeof(uintptr_t));

        // Patch left wand vtable
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(reinterpret_cast<void*>(leftVtableSlot), sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                g_origShowRolloverLeft = reinterpret_cast<ShowRolloverFunc>(*reinterpret_cast<uintptr_t*>(leftVtableSlot));
                *reinterpret_cast<uintptr_t*>(leftVtableSlot) = reinterpret_cast<uintptr_t>(&HookShowRolloverLeft);
                VirtualProtect(reinterpret_cast<void*>(leftVtableSlot), sizeof(uintptr_t), oldProtect, &oldProtect);
                spdlog::info("[HUDRollover] Left wand vtable slot[4] hooked: orig={:X} -> hook={:X}",
                    reinterpret_cast<uintptr_t>(g_origShowRolloverLeft),
                    reinterpret_cast<uintptr_t>(&HookShowRolloverLeft));
            } else {
                spdlog::error("[HUDRollover] Failed to VirtualProtect left wand vtable slot");
            }
        }

        // Patch right wand vtable
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(reinterpret_cast<void*>(rightVtableSlot), sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                g_origShowRolloverRight = reinterpret_cast<ShowRolloverFunc>(*reinterpret_cast<uintptr_t*>(rightVtableSlot));
                *reinterpret_cast<uintptr_t*>(rightVtableSlot) = reinterpret_cast<uintptr_t>(&HookShowRolloverRight);
                VirtualProtect(reinterpret_cast<void*>(rightVtableSlot), sizeof(uintptr_t), oldProtect, &oldProtect);
                spdlog::info("[HUDRollover] Right wand vtable slot[4] hooked: orig={:X} -> hook={:X}",
                    reinterpret_cast<uintptr_t>(g_origShowRolloverRight),
                    reinterpret_cast<uintptr_t>(&HookShowRolloverRight));
            } else {
                spdlog::error("[HUDRollover] Failed to VirtualProtect right wand vtable slot");
            }
        }

        // HUDQuickContainer vtable hook — single vtable at 0x2D4D498
        {
            uintptr_t qcVtable = base + 0x2D4D498;
            uintptr_t qcSlot = qcVtable + (4 * sizeof(uintptr_t));

            DWORD oldProtect = 0;
            if (VirtualProtect(reinterpret_cast<void*>(qcSlot), sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
                g_origQuickContainerVf004 = reinterpret_cast<QuickContainerVf004Func>(*reinterpret_cast<uintptr_t*>(qcSlot));
                *reinterpret_cast<uintptr_t*>(qcSlot) = reinterpret_cast<uintptr_t>(&HookQuickContainerVf004);
                VirtualProtect(reinterpret_cast<void*>(qcSlot), sizeof(uintptr_t), oldProtect, &oldProtect);
                spdlog::info("[HUDQuickContainer] vtable slot[4] hooked: orig={:X} -> hook={:X}",
                    reinterpret_cast<uintptr_t>(g_origQuickContainerVf004),
                    reinterpret_cast<uintptr_t>(&HookQuickContainerVf004));
            } else {
                spdlog::error("[HUDQuickContainer] Failed to VirtualProtect vtable slot");
            }
        }
    }

    void CheckHUDRolloverVtableIntegrity()
    {
        uintptr_t base = REL::Module::get().base();

        uintptr_t rightVtableSlot = base + 0x2D4E438 + (4 * sizeof(uintptr_t));
        uintptr_t leftVtableSlot  = base + 0x2D4E508 + (4 * sizeof(uintptr_t));

        auto currentRight = *reinterpret_cast<uintptr_t*>(rightVtableSlot);
        auto currentLeft  = *reinterpret_cast<uintptr_t*>(leftVtableSlot);

        auto expectedRight = reinterpret_cast<uintptr_t>(&HookShowRolloverRight);
        auto expectedLeft  = reinterpret_cast<uintptr_t>(&HookShowRolloverLeft);

        bool rightOk = (currentRight == expectedRight);
        bool leftOk  = (currentLeft == expectedLeft);

        if (!rightOk || !leftOk) {
            spdlog::error("[HUD_VTABLE] INTEGRITY FAILURE! Right: current={:X} expected={:X} ({}), "
                         "Left: current={:X} expected={:X} ({})",
                         currentRight, expectedRight, rightOk ? "OK" : "BROKEN",
                         currentLeft, expectedLeft, leftOk ? "OK" : "BROKEN");
        } else {
            spdlog::debug("[HUD_VTABLE] Integrity OK — both vtable slots still point to our hooks");
        }
    }
}
