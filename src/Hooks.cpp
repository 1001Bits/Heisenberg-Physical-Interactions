#include "Hooks.h"

#include "Heisenberg.h"
#include "PostConsumeActivationPolicy.h"
#include "HandBumpHook.h"
#include "HavokTimingFix.h"
#include "VRInput.h"
#include "WandNodeHelper.h"
#include "Grab.h"
#include "MenuChecker.h"
#include "DropToHand.h"
#include "PipboyInteraction.h"
#include "SS2Integration.h"
#include "rock/RockBridge.h"
#include "Utils.h"
#include "F4VROffsets.h"

#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"
#include "Config.h"
#include "ActivatorHandler.h"
#include "HeisenbergInterface001.h"
#include "PlayerCharacterProxyListener.h"
#include "HandCollision.h"
#include "Physics.h"
#include "ThrownObjectTracker.h"
#include "WaterInteraction.h"
#include "HandAuthority.h"
#include "FrikArmGoalHook.h"
#include "../external/ROCK/src/ROCKMain.h"
#include "../external/ROCK/src/physics-interaction/core/PhysicsHooks.h"

#include <SimpleIni.h>

#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>

namespace heisenberg::Hooks
{
    // Controls Config marks only its synthetic semantic grenade lifecycle.
    // Physical WandGrip events still pass through Heisenberg's normal grenade
    // arbitration. Thread-local state preserves the engine call's p3/p4 ABI.
    // Calls are accepted only from Hooks::Install's game thread, and a leaked
    // true expires quickly instead of bypassing arbitration for the rest of
    // the session.
    // Defined with the MeleeThrow hook further down; consumed by the per-frame
    // host-API push, which appears earlier in this file.
    bool IsProjectileLaunchGraceActive();

    static std::atomic<DWORD> g_hooksGameThreadId{ 0 };
    static thread_local bool g_externalSemanticGrenadeDispatch = false;
    static thread_local ULONGLONG g_externalSemanticGrenadeDispatchDeadline = 0;
    static constexpr ULONGLONG kExternalGrenadeDispatchLeaseMilliseconds = 250;

    static const char* ResolveCallerModuleName(
        const void* returnAddress,
        char (&modulePath)[MAX_PATH]) noexcept
    {
        HMODULE module = nullptr;
        if (!returnAddress ||
            !GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(returnAddress),
                &module) ||
            !module ||
            GetModuleFileNameA(module, modulePath, MAX_PATH) == 0) {
            return "<unknown>";
        }

        const char* slash = std::strrchr(modulePath, '\\');
        const char* altSlash = std::strrchr(modulePath, '/');
        if (!slash || (altSlash && altSlash > slash)) {
            slash = altSlash;
        }
        return slash ? slash + 1 : modulePath;
    }

    extern "C" __declspec(dllexport)
    void __cdecl Heisenberg_SetExternalGrenadeDispatch(bool enabled)
    {
        const void* caller = _ReturnAddress();
        char modulePath[MAX_PATH]{};
        const char* moduleName = ResolveCallerModuleName(caller, modulePath);
        const DWORD currentThreadId = GetCurrentThreadId();
        const DWORD gameThreadId =
            g_hooksGameThreadId.load(std::memory_order_acquire);

        if (gameThreadId == 0 || currentThreadId != gameThreadId) {
            // A wrong-thread clear cannot clear another thread's TLS state, so make
            // the rejection explicit instead of pretending the request succeeded.
            g_externalSemanticGrenadeDispatch = false;
            g_externalSemanticGrenadeDispatchDeadline = 0;
            spdlog::error(
                "[GrenadeDispatch] Ignored {} from '{}' on thread {} "
                "(game thread={}); export is game-thread only",
                enabled ? "registration" : "clear",
                moduleName,
                currentThreadId,
                gameThreadId);
            return;
        }

        g_externalSemanticGrenadeDispatch = enabled;
        g_externalSemanticGrenadeDispatchDeadline =
            enabled
                ? GetTickCount64() + kExternalGrenadeDispatchLeaseMilliseconds
                : 0;
        spdlog::info(
            "[GrenadeDispatch] External semantic dispatch {} by '{}' "
            "on game thread {}{}",
            enabled ? "registered" : "cleared",
            moduleName,
            currentThreadId,
            enabled ? " (250 ms fail-safe lease)" : "");
    }

    static bool IsExternalSemanticGrenadeDispatchActive()
    {
        if (!g_externalSemanticGrenadeDispatch) {
            return false;
        }

        const DWORD currentThreadId = GetCurrentThreadId();
        const DWORD gameThreadId =
            g_hooksGameThreadId.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        if (currentThreadId != gameThreadId ||
            g_externalSemanticGrenadeDispatchDeadline == 0 ||
            now > g_externalSemanticGrenadeDispatchDeadline) {
            g_externalSemanticGrenadeDispatch = false;
            g_externalSemanticGrenadeDispatchDeadline = 0;
            spdlog::error(
                "[GrenadeDispatch] Cleared leaked/invalid external semantic "
                "dispatch (thread={}, gameThread={})",
                currentThreadId,
                gameThreadId);
            return false;
        }
        return true;
    }

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
    static bool g_hudMessageSuppressionActive = false;
    static bool g_hudMessagesWereEnabled = true;
    static int g_deferredUnsuppressFrames = 0;  // Deferred HUD unsuppress countdown
    static std::string g_deferredHUDMessage;    // Message to show when deferred unsuppress fires
    
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

        // PlayerCharacter::Update vtable hook — runs BEFORE physics step.
        // All world-modifying operations (body creation, pair filter, constraint setup)
        // happen here, matching HIGGS Skyrim's architecture.
        using PlayerCharacterUpdateFunc = void(*)(RE::PlayerCharacter*, float);
        PlayerCharacterUpdateFunc g_originalPlayerCharacterUpdate = nullptr;

        enum class HandCollisionUpdatePhase
        {
            PrePhysics,
            PostPhysics
        };

        // Forward declaration — defined before HookEndUpdate
        void UpdateHandCollisionBodies(HandCollisionUpdatePhase phase);

        // PipboyInventoryMenu::DropItem hook
        // Signature: void DropItem(uint32_t inventoryHandle, void* stackDataArray, uint32_t count)
        // VR address: 0x140b9b9e0
        using PipboyDropItemFunc = void(*)(uint32_t, void*, uint32_t);
        PipboyDropItemFunc g_originalPipboyDropItem = nullptr;

        // TESObjectREFR::SetPosition (VR 0x3f4370) - properly updates data.location + physics + cell
        using SetPositionFunc = void(*)(RE::TESObjectREFR*, RE::NiPoint3*);

        // Check if the item about to be dropped from Pipboy is a placement mod item.
        // param2 points to an inventory entry; the base form is at the start of the
        // BGSInventoryItem structure. We read it defensively.
        static bool IsPipboyDropPlacementItem(void* param2)
        {
            if (!param2) return false;
            // BGSInventoryItem starts with TESBoundObject* object
            auto* baseObj = *reinterpret_cast<RE::TESBoundObject**>(param2);
            if (!baseObj) return false;
            if (baseObj->GetFormType() != RE::ENUM_FORM_ID::kMISC) return false;
            const char* editorID = baseObj->GetFormEditorID();
            if (editorID && std::strstr(editorID, "CampingKit_")) {
                spdlog::info("[PipboyDrop] Detected Campsite placement item: '{}' ({:08X}) — full bypass",
                    editorID, baseObj->formID);
                return true;
            }
            // SS2 plans
            std::string itemName(RE::TESFullName::GetFullName(*baseObj, false));
            if (itemName.find("Plan:") != std::string::npos ||
                itemName.find("Plan -") != std::string::npos ||
                itemName.find("SS2") != std::string::npos) {
                spdlog::info("[PipboyDrop] Detected SS2 placement item: '{}' ({:08X}) — full bypass",
                    itemName, baseObj->formID);
                return true;
            }
            return false;
        }

        void HookPipboyDropItem(uint32_t param1, void* param2, uint32_t param3)
        {
            spdlog::debug("[PipboyDrop] HookPipboyDropItem CALLED (param1={}, param3={})", param1, param3);

            // Never run drop-to-hand logic during a loading screen. Hand nodes,
            // grab state and captured ref IDs are not meaningful until the world
            // is live — just pass through to the original drop.
            if (MenuChecker::GetSingleton().IsLoading()) {
                if (g_originalPipboyDropItem) {
                    g_originalPipboyDropItem(param1, param2, param3);
                }
                return;
            }

            // Placement mod items (Campsite tents, SS2 plans) must bypass all our
            // drop-to-hand logic. Call the original function and return immediately
            // so the item drops at the player's feet and the mod's script handles it.
            if (IsPipboyDropPlacementItem(param2)) {
                if (g_originalPipboyDropItem) {
                    g_originalPipboyDropItem(param1, param2, param3);
                }
                return;
            }

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

            // FRAME-ORDER SEAM (Jul 19): the U1PA shim publishes clean pass-1 controller
            // hands from inside FRIK, before its authority pass. Do not overwrite that
            // loop-free snapshot here with the post-authority skinned support hand; doing so
            // feeds ROCK's prior output back into the next weapon solve and makes a scope
            // wander while walking. If the shim did not publish this frame (inactive,
            // disarmed, or temporarily bypassed), the skinned result is the fallback.
            if (heisenberg::IsRockEngineHosted()) {
                const bool cleanTruthPublished =
                    heisenberg::FrikArmGoalHook::ConsumeCleanTruthPublished();
                if (!cleanTruthPublished) {
                    RE::NiTransform preL, preR;
                    if (heisenberg::HandAuthority::GetSkinnedHandWorld(true, preL) &&
                        heisenberg::HandAuthority::GetSkinnedHandWorld(false, preR)) {
                        rock::HostSetFallbackPreAuthorityHandWorlds(
                            preL,
                            preR);
                        RE::NiPoint3 shoulder{};
                        float maxReach = 0.0f;
                        if (heisenberg::HandAuthority::GetArmReachSphere(true, shoulder, maxReach)) {
                            rock::HostSetPreAuthorityArmReach(true, shoulder, maxReach);
                        }
                        if (heisenberg::HandAuthority::GetArmReachSphere(false, shoulder, maxReach)) {
                            rock::HostSetPreAuthorityArmReach(false, shoulder, maxReach);
                        }
                    }
                }
            }

            // HOST API leases (Jul 19, Virtual Reloads): push the effective external
            // weapon-collision / two-handing state into the embedded engine every frame —
            // the 5s lease TTL is evaluated host-side, so a caller that never re-enables
            // self-heals here.
            if (heisenberg::IsRockEngineHosted()) {
                // Projectile-launch grace: a just-thrown grenade projectile spawns at
                // the hand on the native weapon layer, which our colliders pair with —
                // an impact-fused throwable (Molotov) detonated on them instantly.
                // OR-ing here means expiry self-restores on the next frame's push.
                const bool projectileGrace = IsProjectileLaunchGraceActive();

                // DIALOGUE POSE-SNAP grace (Jul 31): during a conversation the game
                // animates the player's third-person body (talking idle / auto-face) —
                // the bones our colliders keyframe-follow stop tracking the controllers.
                // On dialogue exit the skeleton snaps back to the controller pose, and
                // every generated collider (weapon hull, 20 hand bodies, body bodies)
                // SWEEPS through the arc between the two poses at drive speed, shoving
                // whatever it crosses — live repro: a beer bottle knocked off a table
                // BEHIND the player at DialogueMenu close with a drawn Baseball Bat
                // (17.8u hull). The snap arc (~50-100u/frame) sails under the drive's
                // 1000u teleport threshold, so it resolves as a fast swept push.
                // Suppress our colliders for the whole dialogue plus a settle window.
                static ULONGLONG s_dialoguePoseGraceUntil = 0;
                static bool s_dialoguePoseGraceLogged = false;
                if (MenuChecker::GetSingleton().IsDialogueOpen()) {
                    s_dialoguePoseGraceUntil = GetTickCount64() + 600;
                    if (!s_dialoguePoseGraceLogged) {
                        s_dialoguePoseGraceLogged = true;
                        spdlog::info(
                            "[MenuPoseGrace] Dialogue open — suppressing generated colliders "
                            "until 600ms after close (pose-snap sweep guard)");
                    }
                } else if (GetTickCount64() >= s_dialoguePoseGraceUntil) {
                    // Re-arm the one-shot log for the next conversation.
                    s_dialoguePoseGraceLogged = false;
                }
                const bool dialoguePoseGrace =
                    GetTickCount64() < s_dialoguePoseGraceUntil;
                const bool colliderGrace = projectileGrace || dialoguePoseGrace;
                rock::HostSetWeaponCollisionSuppressed(
                    HeisenbergPluginAPI::IsWeaponCollisionDisabledByAPI() || colliderGrace);
                rock::HostSetTwoHandedGripBlocked(HeisenbergPluginAPI::IsOffHandGripBlockedByAPI());
                rock::HostSetTwoHandedFingerPoseMode(
                    heisenberg::g_config.twoHandedFingerPoseMode);
                rock::HostSetHandCollisionSuppressed(true,
                    HeisenbergPluginAPI::IsHandCollisionDisabledByAPI(true) || colliderGrace);
                rock::HostSetHandCollisionSuppressed(false,
                    HeisenbergPluginAPI::IsHandCollisionDisabledByAPI(false) || colliderGrace);
                rock::HostSetHandHoldingObject(true, heisenberg::GrabManager::GetSingleton().IsGrabbing(true));
                rock::HostSetHandHoldingObject(false, heisenberg::GrabManager::GetSingleton().IsGrabbing(false));
                // MCM-tunable collider padding lives in Heisenberg's config, but the ACTIVE
                // hand colliders are the embedded engine's (the Heisenberg-side
                // HandBoneColliderSet module is disabled in the embed profile) — bridge the
                // value in so the slider actually reaches the physics bodies.
                rock::HostSetHandColliderRadiusPadding(heisenberg::g_config.handColliderRadiusPadding);
                // MCM is merged into Heisenberg's config rather than ROCK's standalone
                // [PhysicsInteraction] config, so support-hand authority needs the same
                // per-frame host bridge to participate in MCM hot reload.
                rock::HostSetTwoHandedMinSteeringAuthority(
                    heisenberg::g_config.offHandSteeringAuthority);
            }

            // Two-handed support-hand finger pose (Jul 19): drive FRIK's base finger API
            // from ROCK's grip pose or the geometry solver while a support grip is engaged.
            heisenberg::UpdateTwoHandedSupportFingerPose();

            constexpr bool DEBUG_DISABLE_ALL_PROCESSING = false;
            if (DEBUG_DISABLE_ALL_PROCESSING) {
                return;
            }

            // No outer SEH: /EHsc means __except skips C++ destructors on unwind,
            // so wrapping WorldWriteLock-taking code in a catch-all leaks the
            // physics lock on any fault (→ next-frame deadlock). Narrow SEH
            // stays only around specific raw pointer reads (e.g. Physics.cpp
            // proxy body resolution) where no RAII is on the stack.
            if (MenuChecker::GetSingleton().IsLoading()) {
                return;
            }

            g_vrInput.Update();
            GrabManager::GetSingleton().PostPhysicsGrabUpdate();
            g_heisenberg.OnInputUpdate();
            WaterInteraction::GetSingleton().SetEnabled(
                g_config.enableWaterInteraction);
            g_heisenberg.OnGrabUpdate();

            // Plugin-side hand authority (Jul 19 frame-order audit): when the embedded ROCK
            // engine is hosted, ApplyWinners runs via rock::HostSetPostUpdateCallback at the
            // TAIL of ROCK's onFrameUpdate — AFTER ROCK writes the weapon node the renderer
            // consumes (applying here would solve the hand against FRIK's pre-aim weapon pose
            // that never renders = the chewing-gum offset). Non-embed sessions apply here.
            if (!heisenberg::IsRockEngineHosted()) {
                heisenberg::HandAuthority::ApplyWinners();
            }

            // Fire post-physics callbacks for external plugins
            heisenberg::HandCollision::GetSingleton().FlushPendingHaptics();
            HeisenbergPluginAPI::InvokePostPhysicsCallbacks(nullptr);

            // Drain thrown-object impacts captured on the Havok worker thread and run
            // their damage/knockback/aggro here on the game thread (engine-safe).
            heisenberg::ThrownObjectTracker::GetSingleton().DrainPendingImpacts();

            // Hand collision body management.
            {
                static bool wasLoading = false;
                if (MenuChecker::GetSingleton().IsLoading()) {
                    if (!wasLoading) {
                        heisenberg::HandCollision::GetSingleton().Shutdown();
                        wasLoading = true;
                    }
                } else {
                    wasLoading = false;
                    static int hcFrameCount = 0;
                    if (++hcFrameCount == 100) {
                        spdlog::info("[HOOKS] HookPostPhysics running (frame 100), calling UpdateHandCollisionBodies");
                    }
                    UpdateHandCollisionBodies(HandCollisionUpdatePhase::PostPhysics);
                }
            }
        }
        
        // =====================================================================
        // PlayerCharacter::Update hook — runs BEFORE physics step each frame.
        // This is where HIGGS does all world-modifying operations.
        // Safe to create/destroy bodies, set collision filters, etc.
        // =====================================================================
        void HookPlayerCharacterUpdate(RE::PlayerCharacter* player, float delta)
        {
            // Call original first — game logic, animation, etc.
            if (g_originalPlayerCharacterUpdate) {
                g_originalPlayerCharacterUpdate(player, delta);
            }

            // Skip during loading.
            // NOTE: a single latch declared before the branch. The previous code declared
            // two separate function-local statics (one per branch) that shadowed each other,
            // so the loading-edge Shutdown only ever fired on the FIRST loading screen of the
            // process; every cell-transition load afterward left stale KEYFRAMED bodies that
            // crash the engine physics step during teardown.
            static bool s_pcuWasLoading = false;
            if (MenuChecker::GetSingleton().IsLoading()) {
                // Destroy hand collision bodies immediately when loading starts.
                if (!s_pcuWasLoading) {
                    heisenberg::HandCollision::GetSingleton().Shutdown();
                    s_pcuWasLoading = true;
                }
                return;
            }
            s_pcuWasLoading = false;

            // Hand collision body creation
            static int frameCount = 0;
            if (++frameCount == 100) {
                spdlog::info("[HOOKS] PlayerCharacterUpdate running (frame 100), calling UpdateHandCollisionBodies");
            }
            UpdateHandCollisionBodies(HandCollisionUpdatePhase::PrePhysics);

            // Proxy listener DISABLED — addListener causes vtable corruption on save/load.
            // Hand collision uses pair filter (disableCollisionsBetween) instead.
            // Grabbed objects use kNonCollidable fallback when pair filter unavailable.
        }

        // Separate function for hand collision body management.
        // Uses WorldWriteLock (C++ RAII) which can't be in __try functions.
        void UpdateHandCollisionBodies(HandCollisionUpdatePhase phase)
        {
            // ROCK integration — AUTOMATIC hand-collision ownership:
            //   * External ROCK present AND actually running its physics (IsRunning) -> ROCK
            //     owns hands; Heisenberg's hand collision is OFF (return here).
            //   * Embedded ROCK with an initialized PhysicsInteraction -> ROCK's
            //     real hand/arm collider bodies and DynamicPushAssist are the
            //     sole collision path. Running Heisenberg's body-less swept
            //     proximity pass as well duplicated finger lookup, radius casts,
            //     object scans, haptics, and impulses every frame.
            //   * ROCK present but DISABLED / torn down at runtime -> Heisenberg reclaims
            //     hands automatically (fall through to the proximity push below). No INI
            //     change needed — this is the "turn ROCK off, get our push back" case.
            //   * ROCK absent -> Heisenberg owns hands.
            // Heisenberg never creates its own physics hand bodies (two-scenario cleanup);
            // the proximity push creates no bodies and is safe during ROCK's startup window.
            auto& rockBridge =
                heisenberg::RockBridge::GetSingleton();
            const bool embeddedRockHosted = heisenberg::IsRockEngineHosted();
            const bool embeddedRockOperational =
                embeddedRockHosted &&
                rock::HostIsPhysicsInteractionReady();
            const bool embeddedRockBenchmarkBaseline =
                embeddedRockHosted &&
                rock::HostIsPerformanceBenchmarkBaseline();

            // A previous configuration could leave this host suppression set
            // while the legacy proximity pass supplied the impulse. ROCK is now
            // the sole active path whenever it is operational, so explicitly
            // restore its native hand DynamicPushAssist.
            if (embeddedRockHosted) {
                rock::HostSetHandDynamicPushAssistSuppressed(false);
            }

            const bool rockOwnsHandPhysics =
                rockBridge.IsRunning() ||
                embeddedRockOperational ||
                embeddedRockBenchmarkBaseline;
            if (rockOwnsHandPhysics) {
                static bool loggedRock = false;
                if (!loggedRock) {
                    const char* owner =
                        rockBridge.IsRunning()
                            ? "external ROCK.dll"
                            : (embeddedRockBenchmarkBaseline
                                   ? "embedded ROCK benchmark baseline"
                                   : "embedded ROCK engine");
                    spdlog::info(
                        "[HOOKS] ROCK owns hand physics ({}) — built-in "
                        "hand collision disabled.",
                        owner);
                    loggedRock = true;
                }
                return;
            }

            if (embeddedRockHosted && !embeddedRockOperational) {
                static bool loggedEmbeddedFallback = false;
                if (!loggedEmbeddedFallback) {
                    spdlog::info(
                        "[HOOKS] Embedded ROCK physics interaction is not "
                        "operational - using Heisenberg body-less hand "
                        "collision fallback.");
                    loggedEmbeddedFallback = true;
                }
            }

            // (Two-scenario cleanup: the legacy [RockIntegration] per-frame ticks —
            // rock_hand_collider / rock_body_collider / rock_weapon_collision /
            // rock_two_handed_grip — were removed with those hand-ported modules; the
            // embedded engine or the external ROCK.dll owns those subsystems now.)

            if (!heisenberg::g_config.enableHandCollision) {
                static bool loggedOnce = false;
                if (!loggedOnce) {
                    spdlog::warn("[HOOKS] Hand collision disabled (bEnableHandCollision=false) — "
                                 "hand strikes won't push objects. Set bEnableHandCollision=true in "
                                 "Heisenberg_F4VR.ini or via MCM to enable.");
                    loggedOnce = true;
                }
                return;
            }

            auto* playerNodes = f4cf::f4vr::getPlayerNodes();
            if (!playerNodes) {
                return;
            }

            RE::NiNode* leftWand = heisenberg::GetWandNode(playerNodes, true);
            RE::NiNode* rightWand = heisenberg::GetWandNode(playerNodes, false);
            if (!leftWand || !rightWand) {
                return;
            }

            auto& hc = heisenberg::HandCollision::GetSingleton();

            // Two-scenario cleanup: Heisenberg no longer creates its own physics hand
            // bodies — real hand colliders come from the embedded ROCK engine (S1) or
            // the external ROCK.dll (S2). The body-less push/contact work remains
            // post-physics.
            if (phase == HandCollisionUpdatePhase::PrePhysics) {
                return;
            }

            // Position updates — use wand nodes directly. The wand world transforms
            // are set by the OpenVR pose pipeline and update with the controller in
            // real time. Earlier code preferred firstPersonSkeleton's LArm_Hand bone,
            // but at HookPostPhysics timing FRIK has only refreshed the LOCAL bone
            // transform; the world transform is recomputed later in the scene graph
            // traversal, so reading world.translate here yielded a stale (frozen)
            // position and the collision body never followed the hand.
            {
                RE::NiPoint3 leftPos = leftWand->world.translate;
                RE::NiPoint3 rightPos = rightWand->world.translate;
                RE::NiMatrix3 leftRot = leftWand->world.rotate;
                RE::NiMatrix3 rightRot = rightWand->world.rotate;

                static RE::NiPoint3 prevL = leftPos, prevR = rightPos;
                static double prevTime = Utils::GetTime();
                const double now = Utils::GetTime();
                float dt = static_cast<float>(now - prevTime);
                if (dt <= 0.0001f || dt > 0.1f) {
                    dt = 1.0f / 90.0f;
                }
                prevTime = now;
                RE::NiPoint3 leftVel = (leftPos - prevL) * (1.0f / dt);
                RE::NiPoint3 rightVel = (rightPos - prevR) * (1.0f / dt);
                prevL = leftPos; prevR = rightPos;

                hc.Update(leftPos, rightPos, leftVel, rightVel, leftRot, rightRot, dt);
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
                // NO catch-all SEH around these four calls (Jul 20 audit fix): this
                // project's own rule (see the narrow SEH used elsewhere in this file) is
                // that /EHsc means __except skips C++ destructors on unwind, so wrapping
                // RAII-holding code (WorldWriteLock, NiPointer refcounts, scene-graph
                // locks - all of which these callees can hold internally) in a catch-all
                // leaks whatever was held on any fault, turning a diagnosable crash into a
                // next-frame deadlock or a permanently-leaked reference. A catch-all here
                // is WORSE than no guard. Narrow SEH belongs only around the specific raw
                // pointer reads inside these functions, not around calls that own RAII.
                PipboyInteraction::GetSingleton().UpdateTapeDeckAnimation();

                // Process deferred VH holster requests here (NOT in PostPhysics)
                // VH's displayWeapon() does cloneNode/AttachChild/loadNifFromFile
                // which deadlocks during post-physics when scene graph locks are held.
                // EndUpdate runs AFTER all animation/skeleton/bone processing,
                // so the scene graph is fully available for NIF cloning.
                GrabManager::GetSingleton().ProcessPendingHolster();

                // Process deferred weapon unequip from storage zone grip
                // UnEquipItem crashes during HookPostPhysics (physics locks held),
                // so we defer to EndUpdate where game state is stable.
                g_heisenberg.ProcessPendingWeaponUnequip();

                // Process deferred weapon re-equip from storage zone grip
                g_heisenberg.ProcessPendingWeaponReequip();

                // Hand collision: body creation + pair filter
                // Uses BSWriteLocker like HIGGS to safely modify the physics world.
                // Hand collision moved to HookPlayerCharacterUpdate (pre-physics).
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
        g_hooksGameThreadId.store(GetCurrentThreadId(), std::memory_order_release);
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

        // Allocate trampoline space. Sized to cover BOTH Heisenberg's own hooks AND the embedded
        // ROCK engine's hooks (its main-loop write_call, etc). CommonLibF4's AllocTrampoline calls
        // set_trampoline()->release(), which FREES any previously-allocated trampoline buffer — so
        // there must be exactly ONE AllocTrampoline for the whole DLL. rock::HostLoad therefore does
        // NOT allocate its own; it appends to THIS shared trampoline via F4SE::GetTrampoline().
        // (A second alloc freed this buffer mid-session → dangling Heisenberg hook stubs → execute-AV
        //  when a per-frame PlayerUpdateEvent sink first fired after the player spawned.)
        F4SE::AllocTrampoline(4096);

        // Initialize VR input
        g_vrInput.Initialize();

        // Install the HandleBumpedCharacter guard (CTD protection for ROCK-style
        // physics hand bodies). Installed always; suppression toggled on only while
        // physics hand bodies are active (see HandBumpHook::SetEnabled).
        heisenberg::HandBumpHook::Install();

        // Install the ROCK Havok timing-fix call-site hook (bhkWorld::SetDeltaTime substep
        // override). Installed always; the override is gated on via config (bHavokTimingFix).
        heisenberg::HavokTimingFix::Install();

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

        // PlayerCharacter::Update vtable hook — runs BEFORE physics step.
        // Matches HIGGS Skyrim architecture: all world-modifying operations here.
        // Vtable: 0x2D80F88, slot 0xCF (Actor::Update override)
        {
            REL::Relocation<std::uintptr_t> pcVtable{ REL::Offset(0x2D80F88) };
            auto* vtablePtr = reinterpret_cast<PlayerCharacterUpdateFunc*>(pcVtable.address() + 0xCF * 8);

            DWORD oldProtect;
            if (VirtualProtect(vtablePtr, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                g_originalPlayerCharacterUpdate = *vtablePtr;
                *vtablePtr = &HookPlayerCharacterUpdate;
                VirtualProtect(vtablePtr, sizeof(void*), oldProtect, &oldProtect);
                spdlog::info("PlayerCharacterUpdate vtable hook: slot 0xCF, orig={:p}", (void*)g_originalPlayerCharacterUpdate);
            } else {
                spdlog::error("PlayerCharacterUpdate vtable hook: VirtualProtect FAILED");
            }
        }

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
        // Also install when drop-to-hand is on: the hook tells a consumable USE apart from a DROP
        // (only a USE goes through EquipObject), so DropToHand can skip consumes — needed even when
        // consume-to-hand itself is off.
        //
        // ORDER NOTE (2026-07-28): Install() runs from F4SEPlugin_Load, which is BEFORE
        // g_config.Load() (that happens on kGameLoaded, Heisenberg.cpp). The four flags below
        // therefore hold their compile-time defaults here, not the user's INI values — and with
        // today's defaults (holotapeToHand=true, dropToHandMode=1) this gate is always true. It is
        // kept as the documented statement of intent; the fifth term is the one that can change.
        //
        // FIFTH TERM — GRENADE ARBITRATION. This detour is now also the ONLY entry point for the
        // embedded ROCK engine's loose-grenade equip interception (rock::HostTryInterceptEquipObject
        // in HookEquipObject below). Only ONE raw entry detour can exist at 0xE6FEA0 and this one
        // owns it, so skipping the install leaves ROCK's grenade feature with nothing to be called
        // from. The master toggle is read straight from the INI here — exactly as Heisenberg::Load
        // does before rock::HostLoad — because g_config is not populated yet at this point.
        bool rockEngineRequested = false;
        {
            CSimpleIniA bootIni;
            bootIni.SetUnicode();
            bootIni.LoadFile("Data/F4SE/Plugins/Heisenberg_F4VR.ini");
            rockEngineRequested = bootIni.GetBoolValue("RockEngine", "bUseRockEngineArchitecture", false);
        }
        const bool hostFeaturesWantEquipHook = g_config.consumableToHand || g_config.favoritesToHand
            || g_config.holotapeToHand || g_config.dropToHandMode > 0;
        if (hostFeaturesWantEquipHook || rockEngineRequested) {
            InstallEquipObjectHook();
        } else {
            // Report the finding and the consequence for BOTH owners of this entry. Naming the
            // exact keys matters: this is the only thing that can silently switch off both
            // consumable/holotape-to-hand AND the embedded engine's grenade interception.
            // ERROR, not warn: at this point in startup the log level is still err (Heisenberg.cpp
            // holds it there until Config::Load), so anything quieter would never reach the log —
            // and this branch silently switches off two separate user-visible features.
            spdlog::error("[EquipHook] Hook NOT installed: bConsumableToHand=0, bFavoritesToHand=0, "
                          "bHolotapeToHand=0 and iDropToHandMode=0, and [RockEngine]bUseRockEngineArchitecture=0. "
                          "Consequence: consumables/holotapes are used natively instead of dropping to hand, and the "
                          "embedded ROCK engine's loose-grenade equip interception has no entry point and stays off.");
            rock::HostSetEquipObjectDetourInstalled(false,
                "Heisenberg did not install its ActorEquipManager::EquipObject detour: bConsumableToHand, "
                "bFavoritesToHand, bHolotapeToHand and iDropToHandMode are all off and "
                "[RockEngine]bUseRockEngineArchitecture=0, so the hook that would call ROCK was never installed");
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

        // Patch 2: VR CreateMouseSpring equivalent. The shared ROCK routine is
        // the sole writer for RVA 0xF19250: it validates the complete Fallout4VR
        // 1.2.72 prologue and atomically installs xor eax,eax; ret. Calling it
        // here preserves host-only Heisenberg behavior; the later embedded-ROCK
        // initialization call is idempotent and only verifies the final state.
        {
            const bool suppressed = rock::installNativeGrabHook();
            if (suppressed) {
                spdlog::info("[NativeTelekinesis] VR CreateMouseSpring at module+0xF19250 is verified DISABLED "
                             "by the shared validate-before-write owner");
            } else {
                spdlog::error("[NativeTelekinesis] VR CreateMouseSpring at module+0xF19250 was NOT modified: "
                              "the shared owner could not verify a safe suppressed final state");
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
            // Preserve the user's actual setting. Repeated suppression calls
            // extend the same lease instead of replacing the saved value with
            // our own temporary false.
            if (!g_hudMessageSuppressionActive) {
                g_hudMessagesWereEnabled =
                    g_showHUDMessagesSetting->GetBinary();
                g_hudMessageSuppressionActive = true;
            }
            g_showHUDMessagesSetting->SetBinary(false);
            spdlog::debug("[HUD] Suppressing HUD messages via INI setting");
        } else {
            if (g_hudMessageSuppressionActive) {
                g_showHUDMessagesSetting->SetBinary(
                    g_hudMessagesWereEnabled);
                g_hudMessageSuppressionActive = false;
                spdlog::debug(
                    "[HUD] Restored HUD messages via INI setting (original={})",
                    g_hudMessagesWereEnabled);
            }
        }
    }
    
    void ScheduleDeferredHUDUnsuppress(int frames, const char* queuedMessage)
    {
        g_deferredUnsuppressFrames = frames;
        g_deferredHUDMessage = queuedMessage ? queuedMessage : "";
        spdlog::debug("[HUD] Scheduled deferred unsuppress in {} frames, message='{}'", frames, g_deferredHUDMessage);
    }

    void UpdateDeferredHUDUnsuppress()
    {
        if (g_deferredUnsuppressFrames > 0) {
            g_deferredUnsuppressFrames--;
            if (g_deferredUnsuppressFrames == 0) {
                SetSuppressHUDMessages(false);
                // Show queued message now that native messages have been discarded
                if (!g_deferredHUDMessage.empty()) {
                    ShowHUDMessage_VR(g_deferredHUDMessage.c_str(), nullptr, false, false);
                    spdlog::info("[HUD] Showed deferred message: '{}'", g_deferredHUDMessage);
                    g_deferredHUDMessage.clear();
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
                if (!player || !player->GetWeaponMagicDrawn()) {
                    // Weapon not drawn — block the draw event to prevent re-equip
                    spdlog::debug("[GripHook] Blocked weapon draw (post-unequip cooldown)");
                    return;
                }
            }
        }

        // Block the ready-weapon handler ONLY when the WEAPON (primary) hand is holding an
        // object and the weapon is holstered. Ghidra (vf011 @ 0xFC9220) shows a TRIGGER
        // auto-ready path here (held-duration >= DAT_1437d5d58 -> DrawWeaponMagicHands) that
        // would unsheathe the weapon THROUGH the held item. We gate on the PRIMARY hand only:
        // if you're holding something in the OFF hand, the weapon hand is free, so its trigger
        // must still draw normally. When a weapon is already drawn, behave normally.
        if (a_event) {
            const bool primaryIsLeft = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
            auto& grabMgr = heisenberg::GrabManager::GetSingleton();
            if (grabMgr.IsGrabbing(primaryIsLeft)) {
                auto* player = f4vr::getPlayer();
                if (!player || !player->GetWeaponMagicDrawn()) {
                    spdlog::debug("[GripHook] Blocked ready-weapon draw (weapon hand holding object, weapon holstered)");
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
                if (player && player->GetWeaponMagicDrawn()) {
                    spdlog::debug("[GripHook] Weapon drawn - passing WandGrip through (reload/holster)");
                } else {
                    // Weapon holstered: normally block WandGrip here so grip doesn't draw the
                    // weapon. BUT a mod secondary action on an activator/furniture (Tune The
                    // Radios, Sentinel PA) triggers on the primary-hand grip while aiming at it,
                    // and was getting eaten — only the offhand grip (which never reaches
                    // ReadyWeaponHandler) still worked. When the primary wand is aimed at an
                    // ACTI/FURN and we're NOT grabbing, let the grip reach the original handler
                    // so the secondary action fires. Empty-air / non-activator grips still block.
                    bool passToActivator = false;
                    if (heisenberg::g_config.passGripToActivatorSecondary) {
                        auto& grabMgr2 = heisenberg::GrabManager::GetSingleton();
                        if (!grabMgr2.IsGrabbing(true) && !grabMgr2.IsGrabbing(false)) {
                            const bool primaryIsLeft = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
                            auto tgt = heisenberg::GetVRWandTargetHandle(primaryIsLeft).get();
                            auto* base = tgt ? tgt->data.objectReference : nullptr;
                            if (base) {
                                const auto ft = base->GetFormType();
                                if (ft == RE::ENUM_FORM_ID::kACTI || ft == RE::ENUM_FORM_ID::kFURN) {
                                    passToActivator = true;
                                    spdlog::debug("[GripHook] WandGrip -> activator/furniture secondary action ({:08X})", tgt->formID);
                                }
                            }
                        }
                    }
                    if (!passToActivator) {
                        spdlog::debug("[GripHook] Blocked WandGrip from ReadyWeaponHandler (weapon not drawn)");
                        return;
                    }
                }
            }
        }

        // Not a grip event (or feature disabled or weapon drawn) - call original handler
        if (g_originalReadyWeaponOnButtonEvent) {
            g_originalReadyWeaponOnButtonEvent(thisPtr, a_event);
        }
    }

    // =====================================================================
    // MELEE THROW / GRENADE-READY HOOK (MeleeThrowHandler::vf011 @ 0xFC8AE0)
    // =====================================================================
    // This handler readies/throws a held grenade: when the throw input (grip
    // in VR) is held >= DAT_1437d5b18 (0.3s) and a throwable is equipped in
    // slot 0x2B, it readies the grenade (trajectory arc) and throws on release.
    // We hook it to control grenade readying at the GAME level per our rules,
    // instead of stripping/injecting grip in the OpenVR stream (which broke
    // VirtualHolsters equip/holster, grip-reload, and mod secondary actions).
    // "Block" = cap the event's held-duration just below the 0.3s ready
    // threshold so the handler runs normally (state machine intact, DrawWeapon
    // fallback already NOP'd below) but never reaches the ready/throw branch.
    using MeleeThrowOnButtonEvent_t = void(*)(void* thisPtr, RE::ButtonEvent* a_event, void* p3, void* p4);
    static MeleeThrowOnButtonEvent_t g_originalMeleeThrowOnButtonEvent = nullptr;

    // Safe "is a throwable equipped" probe — replicates the game's own check
    // (AttackBlockHandler::IsPlayerThrowingWeapon @ 0xFCBCD0): player+0x300 = currentProcess,
    // +8 = middleHigh, +0x290 = equippedItems BSTArray (data@0, size@+0x10, 40 bytes/item;
    // item[0] -> form, form+0x1a = equip slot byte; 0x2B = the grenade/throwable slot).
    // SEH-guarded so any layout mismatch returns false instead of access-violating — unlike
    // GetEquippedWeapon(player, 2), which crashed on grab (index 2 out of range). Gating the
    // grenade hook on this means melee + POWER ATTACKS are never touched: those are
    // AttackBlockHandler with its OWN attackTimer (Ghidra-confirmed), unrelated to this handler.
    static bool PlayerHasThrowableEquipped()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;
        __try {
            const uintptr_t base = reinterpret_cast<uintptr_t>(player);
            const uintptr_t process = *reinterpret_cast<uintptr_t*>(base + 0x300);
            if (!process) return false;
            const uintptr_t middleHigh = *reinterpret_cast<uintptr_t*>(process + 8);
            if (!middleHigh) return false;
            const uintptr_t arrObj = middleHigh + 0x290;
            const uintptr_t data = *reinterpret_cast<uintptr_t*>(arrObj);
            const uint32_t size = *reinterpret_cast<uint32_t*>(arrObj + 0x10);
            if (!data || size == 0 || size > 32) return false;
            for (uint32_t i = 0; i < size; ++i) {
                const uintptr_t item = data + static_cast<uintptr_t>(i) * 40;
                const uintptr_t form = *reinterpret_cast<uintptr_t*>(item);
                if (form && *reinterpret_cast<uint8_t*>(form + 0x1a) == 0x2B) {
                    return true;
                }
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // PROJECTILE LAUNCH GRACE (Jul 31). A thrown grenade's live projectile
    // reuses the WEAP world model, whose collision is authored on the native
    // weapon layer (5) — NOT the projectile layer (6) our collider masks
    // already exclude. All three ROCK collider sets (hand=43, weapon=44,
    // body=47) pair with layer 5, and the projectile spawns AT the throwing
    // hand, inside the hand hull and — with a melee weapon drawn — inside the
    // generated weapon hull too. The game's shooter-exclusion covers the
    // player's own ragdoll, not our generated bodies, so an impact-fused
    // throwable (Molotov) detonated in the player's face the moment it spawned.
    // Suppress our colliders for a short window around the throw; the flags are
    // OR-ed into the per-frame host push below, so expiry self-restores.
    static std::atomic<ULONGLONG> g_projectileLaunchGraceUntil{ 0 };
    constexpr ULONGLONG kProjectileLaunchGraceMs = 400;

    bool IsProjectileLaunchGraceActive()
    {
        const ULONGLONG until = g_projectileLaunchGraceUntil.load(std::memory_order_acquire);
        return until != 0 && GetTickCount64() < until;
    }

    void HookMeleeThrowOnButtonEvent(void* thisPtr, RE::ButtonEvent* a_event, void* p3, void* p4)
    {
        // Throw detection for the grace window: the handler throws on the grip
        // RELEASE after a >=0.3s ready hold. Detect it regardless of the
        // grenade-ready gating below — a blocked ready never throws, so the
        // only cost of a false positive is 0.4s without generated collision.
        if (a_event &&
            a_event->value == 0.0f &&
            a_event->heldDownSecs >= 0.30f &&
            PlayerHasThrowableEquipped()) {
            g_projectileLaunchGraceUntil.store(
                GetTickCount64() + kProjectileLaunchGraceMs,
                std::memory_order_release);
            spdlog::info(
                "[GrenadeThrow] Throwable released after {:.2f}s hold — suppressing generated "
                "hand/weapon/body colliders for {}ms so the projectile cannot impact-fuse on them",
                a_event->heldDownSecs,
                kProjectileLaunchGraceMs);
        }
        // The block mechanism (below) caps the event's held-duration just under the 0.3s throw
        // delay. That is intentionally NON-destructive to the sub-0.3 path: a grip held <0.3s
        // still does its normal thing (power attack / nothing), only the >=0.3s ready/throw/draw
        // is suppressed. We never strip grip from the OpenVR stream, so grip still reaches the
        // game / VirtualHolsters. Power attacks are AttackBlockHandler (own timer) — untouched.
        bool blockReady = false;
        if (a_event &&
            !IsExternalSemanticGrenadeDispatchActive() &&
            heisenberg::g_config.useGrenadeReadyHook &&
            heisenberg::g_config.enableGrenadeHandling) {
            auto& mod = heisenberg::Heisenberg::GetSingleton();
            const bool primaryIsLeft = heisenberg::VRInput::GetSingleton().IsLeftHandedMode();
            const bool hasThrowable = PlayerHasThrowableEquipped();
            if (hasThrowable) {
                // A throwable is equipped → gate GRENADE readying per our rules.
                if (heisenberg::g_config.remapGrenadeButtonToA) {
                    // ALTERNATE CONTROL (remap to A) is AUTHORITATIVE: a throwable must NEVER ready
                    // from a grip hold. The ONLY way to ready is the A long-press, which injects a
                    // brief grip while IsAButtonHeldLongEnough() is true — allow only that. This
                    // takes priority over the weapon-on-hand / zone exceptions below: with alt
                    // control on, grenades come out via A, period.
                    if (!mod.IsAButtonHeldLongEnough()) {
                        blockReady = true;
                    }
                }
                else {
                    // Default control (grip readies grenades). When a real weapon is also on the
                    // throwing hand, NEVER block — you're throwing a grenade, not grabbing, so let
                    // the game ready it on that hand.
                    const bool weaponOnThrowHand = mod.GetCachedHasRealWeapon();

                    // HAND-TO-HAND TRANSFER GUARD: if the OTHER hand is holding an object and the
                    // hands are brought close together, the player is transferring an object hand-
                    // to-hand — NOT throwing. Gripping the (empty) throwing hand to receive it has
                    // no world grab-target, so without this the equipped grenade readies by
                    // accident. Block while a transfer is plausibly in progress.
                    bool handToHandTransfer = false;
                    if (heisenberg::GrabManager::GetSingleton().IsGrabbing(!primaryIsLeft)) {
                        if (auto* nodes = f4cf::f4vr::getPlayerNodes()) {
                            RE::NiNode* primN = heisenberg::GetWandNode(nodes, primaryIsLeft);
                            RE::NiNode* secN  = heisenberg::GetWandNode(nodes, !primaryIsLeft);
                            if (primN && secN) {
                                const float handDist = (primN->world.translate - secN->world.translate).Length();
                                handToHandTransfer = handDist < 25.0f;  // ~35cm — hands together for a transfer
                            }
                        }
                    }

                    if (weaponOnThrowHand) {
                        // no block
                    }
                    else if (handToHandTransfer) {
                        blockReady = true;  // bringing an object to the throwing hand → don't ready
                    }
                    else if (mod.HasGrabTarget(primaryIsLeft)) {
                        blockReady = true;  // grab target → grab, don't ready
                    }
                    else if (heisenberg::g_config.throwableActivationZone != 0 && !mod.IsInChestPocketZone()) {
                        blockReady = true;  // chest-pocket zone mode → only ready inside the zone
                    }
                }
            }
            else if (heisenberg::g_config.disableGripWeaponDraw) {
                // No throwable: a grip-hold >= 0.3s here DRAWS/sheathes the weapon (the byte
                // patches only cover one of the two draw paths; the PerformAction else-branch
                // still toggles it — that's why grip was unsheathing weapons on grab). Heisenberg
                // repurposes grip for grabbing, so cap the hold below 0.3s: the weapon never
                // draws, and the sub-0.3 power-attack path is preserved.
                blockReady = true;
            }
        }

        if (blockReady && a_event) {
            // Pin held-duration just below the 0.3s ready threshold for this call only,
            // then restore so any downstream consumers see the real value.
            const float saved = a_event->heldDownSecs;
            constexpr float kBelowThreshold = 0.29f;  // < DAT_1437d5b18 (0.3s)
            if (a_event->heldDownSecs > kBelowThreshold) {
                a_event->heldDownSecs = kBelowThreshold;
            }
            if (g_originalMeleeThrowOnButtonEvent) {
                g_originalMeleeThrowOnButtonEvent(thisPtr, a_event, p3, p4);
            }
            a_event->heldDownSecs = saved;
            return;
        }

        if (g_originalMeleeThrowOnButtonEvent) {
            g_originalMeleeThrowOnButtonEvent(thisPtr, a_event, p3, p4);
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

        // =====================================================================
        // MELEE THROW / GRENADE-READY entry trampoline (control readying)
        // =====================================================================
        // Trampoline-hook MeleeThrowHandler::vf011 @ 0xFC8AE0 so we can block/allow
        // grenade readying per our rules (HookMeleeThrowOnButtonEvent). Prologue
        // (Ghidra-verified, F4VR 1.2.72): 40 55 56 57 41 56 48 8B EC 48 83 EC 78
        // 0F 29 74 24 60 = PUSH RBP/RSI/RDI/R14; MOV RBP,RSP; SUB RSP,0x78;
        // MOVAPS [RSP+0x60],XMM6 = 18 bytes (clean instruction boundary).
        {
            constexpr size_t MT_STOLEN = 18;
            REL::Relocation<std::uintptr_t> meleeThrow{ REL::Offset(0xfc8ae0) };
            uintptr_t mtAddr = meleeThrow.address();
            uint8_t mtOrig[MT_STOLEN];
            std::memcpy(mtOrig, reinterpret_cast<void*>(mtAddr), MT_STOLEN);
            const uint8_t mtExpected[] = { 0x40,0x55,0x56,0x57,0x41,0x56,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x78,0x0F,0x29,0x74,0x24,0x60 };
            if (std::memcmp(mtOrig, mtExpected, MT_STOLEN) != 0) {
                spdlog::error("[GripHook] MeleeThrow prologue mismatch @ 0x{:X} — grenade-ready hook NOT installed", mtAddr);
            } else {
                void* mtMem = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (!mtMem) {
                    spdlog::error("[GripHook] MeleeThrow VirtualAlloc failed — grenade-ready hook NOT installed");
                } else {
                    uint8_t* tr = reinterpret_cast<uint8_t*>(mtMem);
                    size_t tp = 0;
                    std::memcpy(tr, reinterpret_cast<void*>(mtAddr), MT_STOLEN); tp += MT_STOLEN;
                    tr[tp++] = 0xFF; tr[tp++] = 0x25; tr[tp++] = 0x00; tr[tp++] = 0x00; tr[tp++] = 0x00; tr[tp++] = 0x00;
                    uintptr_t mtCont = mtAddr + MT_STOLEN;
                    std::memcpy(&tr[tp], &mtCont, 8); tp += 8;
                    g_originalMeleeThrowOnButtonEvent = reinterpret_cast<MeleeThrowOnButtonEvent_t>(mtMem);

                    uint8_t mtPatch[MT_STOLEN];
                    mtPatch[0] = 0xFF; mtPatch[1] = 0x25; mtPatch[2] = 0x00; mtPatch[3] = 0x00; mtPatch[4] = 0x00; mtPatch[5] = 0x00;
                    uintptr_t mtHookAddr = reinterpret_cast<uintptr_t>(&HookMeleeThrowOnButtonEvent);
                    std::memcpy(&mtPatch[6], &mtHookAddr, 8);
                    for (size_t i = 14; i < MT_STOLEN; i++) mtPatch[i] = 0x90;
                    REL::safe_write(mtAddr, mtPatch, MT_STOLEN);
                    spdlog::info("[GripHook] Grenade-ready hook installed @ 0x{:X} (trampoline 0x{:X})", mtAddr, reinterpret_cast<uintptr_t>(mtMem));
                }
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
    static std::atomic<bool> g_internalEquip{ false };

    void SetInternalActivation(bool active)
    {
        g_internalActivation.store(active, std::memory_order_relaxed);
    }

    void SetInternalEquip(bool active)
    {
        g_internalEquip.store(active, std::memory_order_relaxed);
    }

    void RecordDroppedRef(uint32_t formID)
    {
        if (formID == 0) return;
        s_recentDrops[s_recentDropIdx].formID = formID;
        s_recentDrops[s_recentDropIdx].tickMs = GetTickCount64();
        s_recentDropIdx = (s_recentDropIdx + 1) % MAX_RECENT_DROPS;
        spdlog::debug("[ActivateHook] Recorded drop of {:08X}", formID);
    }

    void ClearRecentDrops()
    {
        // Called on kPreLoadGame so a save made shortly after a drop can't
        // carry stale formIDs into the next session and block activation.
        for (int i = 0; i < MAX_RECENT_DROPS; ++i) {
            s_recentDrops[i].formID = 0;
            s_recentDrops[i].tickMs = 0;
        }
        s_recentDropIdx = 0;
        spdlog::debug("[ActivateHook] Cleared recently-dropped buffer (pre-load)");
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
        // Never block during a loading screen — the engine may activate refs
        // while placing/initializing cell contents, and our grab/drop state is
        // not meaningful until the world is live.
        if (MenuChecker::GetSingleton().IsLoading()) {
            if (g_originalActivateRef) {
                return g_originalActivateRef(refr, activator, objectToGet, count,
                                              defaultProcessingOnly, fromScript, looping);
            }
            return false;
        }

        // Allow internal activations (storage, consume, etc.) through without checks
        if (g_internalActivation.load(std::memory_order_relaxed)) {
            if (g_originalActivateRef) {
                return g_originalActivateRef(refr, activator, objectToGet, count,
                                              defaultProcessingOnly, fromScript, looping);
            }
            return false;
        }

        // Input-enable flags cover the native handler, but SteamVR action
        // paths can reach ActivateRef directly. Block only non-script player
        // activations during the short post-consume grip-release tail.
        if (refr && activator) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool playerInitiated =
                activator == reinterpret_cast<RE::TESObjectREFR*>(player);
            if (post_consume_activation_policy::
                    shouldBlockPlayerActivation(
                        Heisenberg::GetSingleton().
                            IsPostConsumeActivationSuppressed(),
                        false,
                        playerInitiated,
                        fromScript)) {
                spdlog::debug(
                    "[ActivateHook] BLOCKED player activation during post-consume tail");
                return false;
            }
        }

        // Only block player-initiated activations on grabbable item types.
        // Skip furniture (power armor frames), NPCs, activators, doors, etc.
        if (refr && activator && !fromScript) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (activator == reinterpret_cast<RE::TESObjectREFR*>(player)) {
                // Only apply blocking to item types our grab system can target
                auto* baseObj = refr->GetObjectReference();

                // IDENTIFY WHAT THE PLAYER JUST ACTIVATED.
                //
                // Adding something to the touch-activator whitelist needs its BASE form ID,
                // and there is otherwise no way to read one in VR - the console is not usable
                // with a headset on. Pressing the native activate key on the object and reading
                // this line back is the whole workflow.
                //
                // Player-initiated only, so the volume is exactly "things the player pressed" -
                // no per-frame cost and nothing to sample. ACTI/DOOR/TERM/FURN are called out
                // because those are the interactable classes worth whitelisting; everything
                // else logs at debug so ordinary looting does not fill the file.
                if (baseObj) {
                    const auto ft = baseObj->GetFormType();
                    const bool interactable =
                        ft == RE::ENUM_FORM_ID::kACTI || ft == RE::ENUM_FORM_ID::kDOOR ||
                        ft == RE::ENUM_FORM_ID::kTERM || ft == RE::ENUM_FORM_ID::kFURN;

                    const auto nameView = RE::TESFullName::GetFullName(*baseObj, false);
                    const std::string name = nameView.empty() ? "<no name>" : std::string(nameView);
                    const char* edId = baseObj->GetFormEditorID();

                    if (interactable) {
                        spdlog::info("[ACTIVATED] base={:08X} '{}' edID='{}' type={} ref={:08X} "
                                     "-- add to HeisenbergActivators.ini [Whitelist] as: {}=0x{:08X}",
                            baseObj->GetFormID(), name, edId ? edId : "<none>",
                            static_cast<int>(ft), refr->GetFormID(),
                            name, baseObj->GetFormID());
                    } else {
                        spdlog::debug("[ACTIVATED] base={:08X} '{}' type={} ref={:08X}",
                            baseObj->GetFormID(), name, static_cast<int>(ft), refr->GetFormID());
                    }
                }
                bool isGrabbableType = false;
                if (baseObj) {
                    auto ft = baseObj->GetFormType();
                    // Player is harvesting flora — open the harvest-to-hand window so
                    // the item the game is about to add (old=0->player) is routed to
                    // the hand. Only a real FLOR/TREE activation stamps this window;
                    // unrelated script AddItems (Fill'em Up, cooking/crafting) never
                    // do, so they correctly stay in inventory.
                    if (ft == RE::ENUM_FORM_ID::kFLOR || ft == RE::ENUM_FORM_ID::kTREE) {
                        DropToHand::OpenHarvestWindow();
                    }
                    // Terminal about to open: if in projected mode + force-on-wrist, flip to
                    // wrist NOW (before TerminalMenu opens) so the terminal initializes on the
                    // wrist Pipboy renderer instead of the projected VR overlay (which can't be
                    // redirected). Same timing as the holotape override (applied before display open).
                    if (ft == RE::ENUM_FORM_ID::kTERM) {
                        PipboyInteraction::GetSingleton().PrepareProjectedTerminalOnWrist();
                    }
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

                // Block native activation only for the reference already held
                // by Heisenberg. A mere selection in either hand must not own
                // Grip/A input.
                uint32_t targetFormID = refr->formID;
                auto& grabMgr = GrabManager::GetSingleton();

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
    // Small array, not a single slot: a second holotape redirect within the ~500ms window
    // (two terminal/program holotapes activated in quick succession) used to overwrite
    // both single-slot statics with no restore of the first one, permanently latching its
    // BGSNote::type to kVoice (playing it as a voice recording / never opening its
    // terminal program until the game restarts).
    struct RedirectedNoteEntry
    {
        std::uint32_t formID = 0;
        int origType = -1;
    };
    static constexpr std::size_t kMaxPendingHolotapeRedirects = 4;
    static std::array<RedirectedNoteEntry, kMaxPendingHolotapeRedirects> s_redirectedNotes{};

    // ── EMBEDDED ROCK RE-ENTRY BRACKET ───────────────────────────────────────────────────
    // Held across every call this detour makes to the ORIGINAL EquipObject, so an equip the
    // game issues from inside that call is not offered to ROCK a second time. This reproduces
    // exactly the guard ROCK's own (now removed) detour used to hold across its pass-through.
    // The two seams are thread-local counter ops with no engine dependency, so they are safe
    // and effectively free when the engine is dormant.
    struct RockEquipPassThroughGuard
    {
        RockEquipPassThroughGuard() noexcept { rock::HostEquipObjectPassThroughBegin(); }
        ~RockEquipPassThroughGuard() noexcept { rock::HostEquipObjectPassThroughEnd(); }
        RockEquipPassThroughGuard(const RockEquipPassThroughGuard&) = delete;
        RockEquipPassThroughGuard& operator=(const RockEquipPassThroughGuard&) = delete;
    };

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
        // Never intercept during a loading screen. The engine re-equips all
        // worn gear and applies status effects on save-load (actor == player),
        // and our redirect logic must not run before the world/menus settle.
        if (MenuChecker::GetSingleton().IsLoading()) {
            if (g_originalEquipObject) {
                const RockEquipPassThroughGuard passThroughGuard;
                return g_originalEquipObject(equipManager, actor, instance, stackID, number,
                                              slot, queueEquip, forceEquip, playSounds, applyNow, locked);
            }
            return false;
        }

        // Only intercept for the player
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (actor == player && instance) {
            // Access the base form from BGSObjectInstance (first member is TESForm*)
            RE::TESForm* baseForm = *reinterpret_cast<RE::TESForm**>(instance);
            const bool isDiseaseCure =
                baseForm && heisenberg::ss2::IsDiseaseCureForm(baseForm);

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

            // Skip non-playable ALCH items. Survival mode status effects (Tired,
            // Overtired, Weary, Incapacitated, Rested, Hungry, Parched, etc.) are
            // non-playable ALCH items the engine equips on the player each tick.
            // Without this skip, the Pipboy-open check below redirects every status
            // effect to drop-to-hand. Since these items have no 3D model, the
            // TryGrabPendingDrop queue loops forever "waiting for 3D" and the pickup
            // sound spams continuously.
            // GetPlayable() ALONE IS NOT ENOUGH - this is the third recurrence of the same bug.
            //
            // Some Survival status tokens are PLAYABLE ALCH with an empty editor ID (DropToHand
            // learned this the hard way; see its own guard stack in OnContainerChanged). They
            // pass the GetPlayable() test above, get redirected to drop-to-hand, and because
            // they are invisible they never load 3D: the queue times out, re-adds the token -
            // RE-APPLYING ITS EFFECT - and re-queues, forever. With a Hunger token caught in
            // that loop the tier can never fall, so the player is permanently Starving and
            // eating appears to do nothing. Confirmed by the owner: turning Consume To Hand
            // off makes it stop, which is exactly this interception.
            //
            // A model-less item can never be held, so redirecting one to the hand is always
            // wrong regardless of what it is. Testing capability rather than identity is what
            // stops this recurring - the identity tests (non-playable, HC_ prefix) have now
            // been defeated twice by tokens shaped slightly differently.
            // Our own CONSUME path equips the item deliberately (that is the signal Survival
            // watches for). Never bounce that back to the hand - it is already in the hand.
            if (g_internalEquip.load(std::memory_order_relaxed)) {
                if (isDiseaseCure) {
                    spdlog::error(
                        "[EquipHook] Blocked internal PlayerRef use of SS2 Disease Cure {:08X}",
                        baseForm->formID);
                    ShowHUDMessageDirect(
                        "Disease Cure can only be used on a settler");
                    return true;
                }
                if (g_originalEquipObject) {
                    return g_originalEquipObject(equipManager, actor, instance, stackID, number,
                                                 slot, queueEquip, forceEquip, playSounds,
                                                 applyNow, locked);
                }
                return false;
            }

            const bool alchHasModel = [&]() -> bool {
                if (!baseForm) return false;
                auto* asModel = baseForm->As<RE::TESModel>();
                if (!asModel) return false;
                const char* modelPath = asModel->GetModel();
                return modelPath && modelPath[0] != char(0);
            }();

            // Witness: a PLAYABLE, model-less ALCH arriving here is a survival status token
            // that would have been redirected before this guard existed. Logging it proves the
            // guard is doing real work rather than sitting inert, and names the token so a
            // future variant can be identified from a log instead of another bug report.
            if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kALCH
                && baseForm->GetPlayable(nullptr) && !alchHasModel) {
                spdlog::info("[EquipHook] Not redirecting model-less ALCH {:08X} '{}' edID='{}' "
                             "- playable but cannot be held (survival status token)",
                    baseForm->formID,
                    RE::TESFullName::GetFullName(*baseForm, false),
                    baseForm->GetFormEditorID() ? baseForm->GetFormEditorID() : "<none>");
            }

            if (baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kALCH && !blockPA
                && baseForm->GetPlayable(nullptr) && alchHasModel) {
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

                // Mark this consumable as handled-via-equip so DropToHand's drop path won't ALSO
                // yank it to the hand: USING a chem/food/drink fires the same old=player->new=0
                // container event as a DROP. A real DROP never goes through EquipObject, so it's
                // never marked and still grabs normally.
                DropToHand::GetSingleton().MarkAsRecentlyStored(baseForm->formID);

                auto& menuChecker = MenuChecker::GetSingleton();
                bool shouldRedirect = false;
                const char* source = nullptr;

                // Consume-to-hand fires when the player picks a consumable from
                // either the Pipboy inventory or the Favorites quick-wheel. Any
                // other menu (workbench, cooking, container, dialogue) is blocked.
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

            // If the cure was not redirected to a free hand above (feature
            // disabled, both hands occupied, Power Armor, or a non-menu equip),
            // swallow the native player-use call. SS2's settler treatment path
            // removes the inventory item through Papyrus and never equips it,
            // so this cannot block a valid NPC cure.
            if (isDiseaseCure) {
                spdlog::warn(
                    "[EquipHook] Blocked native PlayerRef use of SS2 Disease Cure {:08X}",
                    baseForm->formID);
                ShowHUDMessageDirect(
                    "Disease Cure can only be used on a settler; free a hand");
                return true;
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
                // Selecting the loaded intro tape means "take it out". It already exists
                // in player inventory as the deck's backing item, so swallowing this equip
                // made it impossible to retrieve through the Pip-Boy menu.
                if (pipboy.HasHolotapeLoaded() && pipboy.GetLoadedHolotapeFormID() == baseForm->formID) {
                    if (pipboy.IsIntroHolotape(baseForm->formID)) {
                        const bool queued = pipboy.TakeLoadedIntroHolotapeToRightHand(baseForm->formID);
                        if (queued) {
                            // Suppress any secondary holotape-menu path fired by the same UI action.
                            s_holotapeRedirectTick = GetTickCount64();
                            spdlog::info("[EquipHook] Loaded intro holotape {:08X} moved from deck to right hand",
                                         baseForm->formID);
                        } else {
                            spdlog::warn("[EquipHook] Loaded intro holotape {:08X} could not be moved to hand",
                                         baseForm->formID);
                        }
                    } else {
                        spdlog::debug("[EquipHook] Holotape {:08X} already in tape deck — ignoring inventory activation",
                                     baseForm->formID);
                    }
                    return true;
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
                    if (vrPlayer && vrPlayer->GetWeaponMagicDrawn()) {
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
                        // First still-pending (formID != 0) entry, or overwrite the same
                        // formID if it's already pending (re-redirect before its restore
                        // ran) - never silently overwrite a DIFFERENT pending entry, which
                        // is what the old single-slot statics did on a second redirect
                        // within the ~500ms window.
                        RedirectedNoteEntry* slot = nullptr;
                        for (auto& entry : s_redirectedNotes) {
                            if (entry.formID == baseForm->formID) { slot = &entry; break; }
                        }
                        if (!slot) {
                            for (auto& entry : s_redirectedNotes) {
                                if (entry.formID == 0) { slot = &entry; break; }
                            }
                        }
                        if (slot) {
                            slot->formID = baseForm->formID;
                            slot->origType = static_cast<int>(note->type);
                        } else {
                            spdlog::warn("[EquipHook] Holotape redirect slots full ({}) - {:08X} type restore will be skipped",
                                kMaxPendingHolotapeRedirects, baseForm->formID);
                        }
                        const int loggedOrigType = slot ? slot->origType : -1;
                        note->type = RE::BGSNote::NOTE_TYPE::kVoice;
                        spdlog::debug("[EquipHook] Temporarily changed holotape {:08X} type {} → kVoice",
                            baseForm->formID, loggedOrigType);
                    }

                    return true;  // Skip original equip (prevents holotape playback)
                }
                }  // menuChecker check
            }
        }

        // ── EMBEDDED ROCK: loose-grenade equip interception ──────────────────────────────
        // ROCK does not patch 0xE6FEA0 — THIS detour owns the entry, and an entry can hold
        // exactly one raw detour. ROCK's interception is therefore called, not hooked. It runs
        // here, deliberately:
        //   * AFTER every Heisenberg path above, so none of them change behaviour. Grenades are
        //     kWEAP forms and nothing above claims a kWEAP except the storage-zone auto-equip
        //     block, which is an explicit host feature and rightly wins.
        //   * BEFORE the fall-through, so a consumed call never reaches the native function.
        // Historically ROCK installed a second detour here, memcmp'd the prologue this hook had
        // already overwritten, failed, and latched itself off — which is why grenade interception
        // had never once run ("grenades drop at my feet").
        if (heisenberg::IsRockEngineHosted()) {
            switch (rock::HostTryInterceptEquipObject(actor, instance, stackID, number)) {
            case rock::HostEquipInterception::ConsumedEquipped:
                // ROCK queued the grenade transaction; the native equip must not also run.
                return true;
            case rock::HostEquipInterception::ConsumedBlocked:
                // It IS a grenade, but its projectile/explosion/fuse data would not resolve.
                // ROCK refuses the equip rather than let it run half-configured.
                return false;
            case rock::HostEquipInterception::NotIntercepted:
            default:
                break;  // fall through to Heisenberg's normal pass-through below
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
            const RockEquipPassThroughGuard passThroughGuard;
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
        // Drain ALL pending entries, not just one - a second redirect within the ~500ms
        // window used to overwrite the single slot with no restore of the first form,
        // permanently latching its BGSNote::type to kVoice.
        for (auto& entry : s_redirectedNotes) {
            if (entry.formID == 0 || entry.origType < 0) {
                continue;
            }
            auto* form = RE::TESForm::GetFormByID(entry.formID);
            if (form && form->GetFormType() == RE::ENUM_FORM_ID::kNOTE) {
                auto* note = static_cast<RE::BGSNote*>(form);
                note->type = static_cast<RE::BGSNote::NOTE_TYPE>(entry.origType);
                spdlog::debug("[EquipHook] Restored holotape {:08X} type to {}",
                    entry.formID, entry.origType);
            }
            entry = RedirectedNoteEntry{};
        }
    }

    void InstallEquipObjectHook()
    {
        // =====================================================================
        // VirtualAlloc detour for ActorEquipManager::EquipObject at 0xe6fea0
        // Same pattern as the GripWeaponDraw hook.
        // We read the first N bytes of prologue, verify them, then install
        // an absolute jump to our hook function.
        //
        // SOLE OWNER OF THIS ENTRY (2026-07-28). The embedded ROCK engine used to
        // install a second raw detour here and always failed, because this one is
        // installed first (Hooks::Install runs before rock::HostLoad) and ROCK then
        // memcmp'd a prologue we had already replaced. ROCK's loose-grenade equip
        // interception is now CALLED from HookEquipObject instead. Consequently every
        // exit from this function must report its outcome through
        // rock::HostSetEquipObjectDetourInstalled — that flag is the gate on ROCK's
        // whole pending-grenade-equip queue, and a queue nothing can feed has to be
        // latched off rather than left waiting for a request that can never arrive.
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
            spdlog::error("[EquipHook] Unknown prologue pattern! First bytes: {:02X} {:02X} {:02X}. Hook NOT installed. "
                          "Consequence: consumable/holotape/drop-to-hand redirection is off, and the embedded ROCK engine's "
                          "loose-grenade equip interception has no entry point.",
                origBytes[0], origBytes[1], origBytes[2]);
            rock::HostSetEquipObjectDetourInstalled(false,
                "Heisenberg's ActorEquipManager::EquipObject detour was NOT installed: the first prologue bytes at "
                "0xE6FEA0 matched none of the known entry patterns (see the [EquipHook] Prologue line above for the "
                "20 bytes actually found there)");
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
                spdlog::error("[EquipHook] Unknown instruction at offset {} (byte {:02X}). Cannot determine safe boundary. Hook NOT installed. "
                              "Consequence: consumable/holotape/drop-to-hand redirection is off, and the embedded ROCK engine's "
                              "loose-grenade equip interception has no entry point.",
                    pos, b);
                rock::HostSetEquipObjectDetourInstalled(false,
                    "Heisenberg's ActorEquipManager::EquipObject detour was NOT installed: instruction-length decoding of the "
                    "0xE6FEA0 prologue could not reach a safe 14-byte boundary (see the [EquipHook] Prologue line above for the "
                    "20 bytes actually found there)");
                return;
            }
        }
        stolenBytes = pos;
        spdlog::info("[EquipHook] Stealing {} bytes of prologue", stolenBytes);
        
        // Allocate executable memory for trampoline
        void* hookMemory = VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!hookMemory) {
            spdlog::error("[EquipHook] VirtualAlloc failed (GetLastError={})! Hook NOT installed. "
                          "Consequence: consumable/holotape/drop-to-hand redirection is off, and the embedded ROCK engine's "
                          "loose-grenade equip interception has no entry point.",
                GetLastError());
            rock::HostSetEquipObjectDetourInstalled(false,
                "Heisenberg's ActorEquipManager::EquipObject detour was NOT installed: VirtualAlloc for its 128-byte "
                "trampoline failed");
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

        // Tell the embedded ROCK engine its grenade interception now has a live entry point.
        // Read back later by rock::loose_grenade_runtime::installEquipHook() during HostLoad,
        // which runs after this. Harmless when the engine is dormant.
        rock::HostSetEquipObjectDetourInstalled(true, nullptr);
        spdlog::info("[EquipHook] Detour owns 0x{:X}; embedded ROCK loose-grenade interception will be called from it "
                     "(ROCK installs no hook of its own there).", targetAddr);
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

        // Which fields this pass actually wrote (kWrote* bits) and the values written.
        // The restore is ALL-OR-NOTHING: if ANY written field no longer holds the value we
        // wrote, the producer ran during the render window - its state is authoritative and
        // refcount-correct, so we restore none of it. Per-field compares are not enough: the
        // byte value space of playFlag collides (0==0 proves nothing), so one field's mismatch
        // must veto every restore.
        static constexpr uint8_t kWroteItemName  = 1 << 0;
        static constexpr uint8_t kWroteActivate  = 1 << 1;
        static constexpr uint8_t kWroteSecondary = 1 << 2;
        static constexpr uint8_t kWrotePlayFlag  = 1 << 3;
        uint8_t   wroteMask = 0;
        uintptr_t wroteItemName = 0;
        uintptr_t wroteActivateText = 0;
        uintptr_t wroteSecondaryText = 0;
        uint8_t   wrotePlayFlag = 0;
    };
    static thread_local RolloverSaveState _savedLeft;
    static thread_local RolloverSaveState _savedRight;

    // Pre-cached BSFixedString raw values — initialized in InstallHUDRolloverHook,
    // NEVER inside the vtable hook itself (BSFixedString init inside a rendering
    // callback corrupts the HUD display).
    static uintptr_t g_holotapeHintEntry = 0;
    static uintptr_t g_touchHintEntry = 0;

    // -------------------------------------------------------------------------
    // ShowRolloverParameters is SHARED state, not private scratch space.
    //
    // Ghidra (F4VR 1.2.72) — the fields we edit are one BSTOptional<ShowRolloverParameters>
    // whose base is instance+0x608, and every engine access to it is wrapped in the
    // BSSpinLock at +0x648. The producer FUN_140abf6e0 (BSTValueEventSink<HUDRolloverStateEvent>
    // ::ProcessEvent) confirms the layout exactly: lock +0x40, itemName +0x08, activateText
    // +0x10, secondaryText +0x18, has-value +0x30, show-latch +0x38, lockCount +0x44 — i.e.
    // +0x648 / +0x610 / +0x618 / +0x620 / +0x638 / +0x640 / +0x64C on the instance.
    //
    // Both producer paths RELEASE whatever entry is sitting in those three fields: the update
    // path via ShowRolloverParameters::operator= (0xABC640 → BSFixedString::operator= x3) and
    // the clear path via ~BSFixedString x3. So a raw pointer parked in one of them without a
    // matching addref is a live grenade. If the producer runs while our substitute is in place
    // it releases OUR static entry; if it runs while we are inside the original, our restore
    // re-parks a pointer whose refcount the producer already dropped. Either way an entry can
    // reach refcount zero, and BSFixedString::~BSFixedString (0x1BC1770) then unlinks it from
    // the BSStringPool bucket chain and Frees it — corrupting the pool that every string in the
    // game shares. That is why the damage surfaces as unrelated item names going blank for a
    // while and then coming back once the string gets re-interned.
    //
    // The fix has three parts, and deliberately does NOT hold the lock across the render:
    //
    //  1. Both edit and restore happen inside a SHORT critical section under the engine's own
    //     BSSpinLock (0x24E10, re-entrant via an _owningThread == GetCurrentThreadId() check).
    //     That makes the reference handoff on the displaced entry atomic against the producer.
    //     The lock is released before calling the original — mirroring vf004 itself, which locks,
    //     snapshots, unlocks, and only then does Scaleform work. Holding it across the render
    //     would risk an AB/BA against the model lock the producer already holds when it fires the
    //     HUDRolloverStateEvent, and BSSpinLock::Lock spins 10000 times before it starts sleeping.
    //     Note this matters most on a DEFAULT install: hideWandHUD defaults to true, so that
    //     branch substitutes on every vf004 for both wands.
    //
    //  2. Our two substitute entries are made immortal, so a producer assignment that lands while
    //     one is parked can no longer drive it to zero.
    //
    //  3. Restores are compare-and-restore, so we never write a stale pointer over an entry the
    //     producer assigned while we were rendering.
    //
    // Worst case is then a bounded refcount leak on a displaced entry (it keeps the reference the
    // field used to own) — never an over-release, never a dangling pointer, never pool corruption.
    // -------------------------------------------------------------------------
    using BSSpinLockLockFunc = uint32_t(__fastcall*)(void*, const char*);

    static constexpr uintptr_t kRolloverParamsLockOffset = 0x648;

    // Witnesses. All are ATOMICS ONLY on the hook path - no logging happens on the render
    // thread, and certainly not while holding the engine's spinlock (spdlog formats, takes the
    // sink mutex, and this codebase flushes on warn - a synchronous file write). They are
    // drained and logged one-shot from the main thread in RefreshWandActivatorTargets.
    //
    // Interpreting them: the AUTHORITATIVE concurrency witness is the restore-decline counter -
    // it fires whenever the producer wrote during the render window, which is the precondition
    // for the old corruption. The lock-contention sample is only a lucky catch: it samples the
    // lock words for one instant ~2x/frame while the producer's critical section is
    // sub-microsecond, so its silence proves nothing.
    static std::atomic<uint32_t> g_rolloverLockContended{ 0 };
    static std::atomic<uint32_t> g_rolloverContendedOwner{ 0 };
    static std::atomic<uint32_t> g_rolloverRestoreDeclines{ 0 };
    static std::atomic<uint64_t> g_rolloverDeclineDetail{ 0 };   // (wrote<<32)|found, low dwords
    static std::atomic<uint32_t> g_rolloverOrigFaults{ 0 };
    static std::atomic<uint32_t> g_rolloverOrigFaultCode{ 0 };
    static std::atomic<uint32_t> g_rolloverLockRepairs{ 0 };

    class RolloverParamsLock
    {
    public:
        explicit RolloverParamsLock(void* thisPtr)
            : _lock(reinterpret_cast<uintptr_t>(thisPtr) + kRolloverParamsLockOffset)
        {
            const DWORD selfThread  = GetCurrentThreadId();
            const DWORD ownerBefore = *reinterpret_cast<volatile DWORD*>(_lock);
            const LONG  countBefore = *reinterpret_cast<volatile LONG*>(_lock + 4);
            if (countBefore != 0 && ownerBefore != 0 && ownerBefore != selfThread) {
                g_rolloverLockContended.fetch_add(1, std::memory_order_relaxed);
                g_rolloverContendedOwner.store(ownerBefore, std::memory_order_relaxed);
            }

            static const auto lockFn =
                reinterpret_cast<BSSpinLockLockFunc>(REL::Offset(0x24e10).address());
            lockFn(reinterpret_cast<void*>(_lock), nullptr);
        }

        ~RolloverParamsLock() { unlock(); }

        // Mirrors the unlock the engine inlines at the top of vf004: drop the owning thread
        // only on the outermost release, otherwise just decrement the recursion count.
        void unlock()
        {
            if (!_held) return;
            _held = false;
            auto* owningThread = reinterpret_cast<volatile LONG*>(_lock);
            auto* lockCount    = reinterpret_cast<volatile LONG*>(_lock + 4);
            if (*lockCount == 1) {
                *owningThread = 0;
                InterlockedCompareExchange(lockCount, 0, 1);
            } else {
                InterlockedDecrement(lockCount);
            }
        }

        RolloverParamsLock(const RolloverParamsLock&) = delete;
        RolloverParamsLock& operator=(const RolloverParamsLock&) = delete;

    private:
        uintptr_t _lock;
        bool      _held = true;
    };

    // A BSFixedString entry whose masked refcount reaches 0x3FFF is immortal: the destructor
    // (0x1BC1770) breaks out without decrementing once (refCount & 0x3FFF) > 0x3FFE, and the
    // copy ctor likewise stops incrementing there. Pinning our two substitute entries means a
    // producer assignment landing on a field where we parked one is a no-op instead of an
    // unbalanced release. The upper bits — the wide-string flag at bit 14 and the bucket index at
    // 16..22, both of which the destructor reads — are preserved.
    static void ImmortalizeStringEntry(uintptr_t entry)
    {
        if (!entry) return;
        auto* refCount = reinterpret_cast<volatile LONG*>(entry + 8);
        for (;;) {
            const LONG cur = *refCount;
            if ((cur & 0x3fff) == 0x3fff) return;
            const LONG want = static_cast<LONG>((static_cast<ULONG>(cur) & ~0x3fffu) | 0x3fffu);
            if (InterlockedCompareExchange(refCount, want, cur) == cur) return;
        }
    }

    // Whether each WAND (not physical hand) is currently pointing at a tracked activator or
    // terminal. Indexed by wand identity to match the vtable thunks that consume it:
    // [0] = PRIMARY wand rollover (HookShowRolloverRight), [1] = SECONDARY (HookShowRolloverLeft).
    //
    // This is answered on the MAIN thread (not in the vtable hook) so the hook never walks
    // ActivatorHandler's vectors while the main thread clears and rebuilds them on a cell scan;
    // the hook reads one atomic.
    static std::atomic<bool> g_wandOnTrackedActivator[2] = { false, false };

    // Pre-write state of the rollover prompt fields, per wand, recorded on the render thread
    // and drained on the main thread. Bit31 = observed at all, bit0 = activateText populated,
    // bit1 = secondaryText populated, bit2 = play flag. Answers "where do the TWO prompts on
    // one elevator actually live" without guessing: both fields on one widget, or one field on
    // each wand's widget.
    static std::atomic<std::uint32_t> g_rolloverFieldWitness[2] = { 0u, 0u };

    // One-shot drain of the render-thread witnesses. Runs on the MAIN thread, never under any
    // engine lock, which is the only place logging is allowed for this hook family.
    static void DrainRolloverWitnesses()
    {
        static bool contentionLogged = false;
        static bool declineLogged = false;
        static bool faultLogged = false;

        if (!contentionLogged && g_rolloverLockContended.load(std::memory_order_relaxed) > 0) {
            contentionLogged = true;
            spdlog::warn("[HUDRollover] Rollover params lock was observed held by another thread "
                         "(owner={} n={}) - producer and render hook are concurrent. (Lucky-catch "
                         "sample; the decline counter is the authoritative witness.)",
                         g_rolloverContendedOwner.load(std::memory_order_relaxed),
                         g_rolloverLockContended.load(std::memory_order_relaxed));
        }
        if (!declineLogged && g_rolloverRestoreDeclines.load(std::memory_order_relaxed) > 0) {
            declineLogged = true;
            const uint64_t detail = g_rolloverDeclineDetail.load(std::memory_order_relaxed);
            spdlog::warn("[HUDRollover] Producer wrote the rollover params during a render window; "
                         "restore declined all fields (wrote={:X} found={:X} n={}). This CONFIRMS "
                         "producer/render concurrency - the pre-fix unlocked edit could corrupt "
                         "the string pool on this machine.",
                         static_cast<uint32_t>(detail >> 32), static_cast<uint32_t>(detail),
                         g_rolloverRestoreDeclines.load(std::memory_order_relaxed));
        }
        // Which prompt fields the engine had populated when we intervened, per wand.
        static std::uint32_t fieldsLogged[2] = { 0u, 0u };
        for (int w = 0; w < 2; ++w) {
            const std::uint32_t bits = g_rolloverFieldWitness[w].load(std::memory_order_relaxed);
            if ((bits & 0x80000000u) == 0 || bits == fieldsLogged[w]) {
                continue;
            }
            fieldsLogged[w] = bits;
            spdlog::info("[ROLLOVER-FIELDS] wand={} pre-write: activateText={} secondaryText={} "
                         "playFlag={} — a second visible prompt means BOTH were populated here; "
                         "if only activateText was, the other message belongs to the other wand",
                         w,
                         (bits & 1u) ? "SET" : "empty",
                         (bits & 2u) ? "SET" : "empty",
                         (bits & 4u) ? "SET" : "empty");
        }

        if (!faultLogged && g_rolloverOrigFaults.load(std::memory_order_relaxed) > 0) {
            faultLogged = true;
            spdlog::error("[HUDRollover] Native ShowRollover faulted and was contained "
                          "(code={:#X} n={} lockRepairs={}). Each fault leaks up to 3 snapshot "
                          "refs; a growing count means the HUD is faulting every frame.",
                          g_rolloverOrigFaultCode.load(std::memory_order_relaxed),
                          g_rolloverOrigFaults.load(std::memory_order_relaxed),
                          g_rolloverLockRepairs.load(std::memory_order_relaxed));
        }
    }

    void RefreshWandActivatorTargets()
    {
        // Main-thread drain point for the hook witnesses - runs even when the activator
        // feature is off, so keep this call unconditional in the main loop.
        DrainRolloverWitnesses();

        if (!g_config.enableInteractiveActivators) {
            // Feature off (possibly toggled off mid-session via MCM reload): the snapshot must
            // not stay latched at its last value, or the hook keeps rewriting prompts forever.
            g_wandOnTrackedActivator[0].store(false, std::memory_order_relaxed);
            g_wandOnTrackedActivator[1].store(false, std::memory_order_relaxed);
            return;
        }

        auto& actHandler = heisenberg::ActivatorHandler::GetSingleton();

        // GetVRWandTargetHandle takes a PHYSICAL hand and swaps viewcasters in left-handed
        // mode; our consumers are keyed by WAND identity (vtable). Map wand->physical here so
        // slot 0 always describes the PRIMARY wand's target and slot 1 the SECONDARY's,
        // in both handedness modes.
        const bool isLH = VRInput::GetSingleton().IsLeftHandedMode();

        for (int wand = 0; wand < 2; ++wand) {
            const bool physicalIsLeft = (wand == 0) ? isLH : !isLH;
            bool onTracked = false;

            RE::ObjectRefHandle handle = heisenberg::GetVRWandTargetHandle(physicalIsLeft);

            // PROXIMITY IS THE ONLY SIGNAL. The prompt answers "what happens if I reach out?",
            // so it must track the HAND, not the VR wand's ray.
            //
            // Two failures came from keying on the ray. It fired when it should not: pointing
            // at a tracked activator from across the room advertised "Touch to activate" for
            // something out of arm's reach. And it stayed silent when it should have fired: at
            // an elevator the ray resolves to the DOOR (a kDOOR ref, deliberately excluded from
            // tracking because doors keep native VR interaction) while the pressable button
            // beside it is a separate ACTI that IS tracked - so the prompt still read "press X"
            // on a button the player could already push. Proven by [ROLLOVER-WHYNOT]:
            // 'target ref=000C4485 base=000E8DCA type=32 NOT in tracked list' logged repeatedly
            // while button 000E8DC9 (type 27) was being touch-activated successfully.
            //
            // IsHandInPointingRange is the handler's own latch, derived from the CLOSEST
            // activator's freshly measured distance and explicitly reset when the tracked list
            // empties. Deliberately NOT a scan over the per-activator isLeft/isRightHandInRange
            // flags: those are skipped for any activator whose 3D is unloaded, so a stale true
            // survives and claims proximity to something no longer even loaded.
            //
            // Consequence worth knowing: the prompt is about the hand, so standing at a button
            // while pointing the ray elsewhere still reads "Touch to activate". That is correct
            // - reaching out WOULD press it.
            onTracked = actHandler.IsHandInPointingRange(physicalIsLeft);

            // ALSO: is the thing being POINTED AT touch-operable?
            //
            // Proximity alone is too narrow. Looking at an elevator from a step back, hands
            // down, the prompt read "press X to activate" even though the panel beside the
            // door is a button the player can simply push. The rollover belongs to the DOOR
            // (kDOOR, excluded from tracking because doors keep native VR interaction) while
            // the pressable ACTI is a separate, nearby reference - so a direct-hit test alone
            // never fires there either.
            //
            // Two tests, cheap, only when proximity has not already answered:
            //   1. the pointed-at reference IS a tracked activator (a button dead ahead), or
            //   2. a tracked activator sits right beside it - the elevator case, where the
            //      panel and the door are parts of one assembly.
            //
            // The co-location radius is deliberately assembly-sized, not room-sized: it must
            // cover a panel next to its door without claiming every activator down a corridor
            // is what you are pointing at.
            if (!onTracked && handle) {
                RE::NiPointer<RE::TESObjectREFR> refr = handle.get();
                if (refr) {
                    const std::uint32_t targetId = refr->formID;
                    const RE::NiPoint3 targetPos = refr->GetPosition();
                    constexpr float kAssemblyRadius = 150.0f;          // ~2m
                    const float radiusSq = kAssemblyRadius * kAssemblyRadius;

                    const auto nearTarget = [&](const auto& tracked) {
                        if (tracked.formID == targetId) return true;   // pointed straight at it
                        RE::NiPointer<RE::TESObjectREFR> actRef = tracked.refrHandle.get();
                        if (!actRef) return false;
                        const RE::NiPoint3 d = actRef->GetPosition() - targetPos;
                        return (d.x * d.x + d.y * d.y + d.z * d.z) <= radiusSq;
                    };

                    for (const auto& tracked : actHandler.GetTrackedActivators()) {
                        if (nearTarget(tracked)) { onTracked = true; break; }
                    }
                    if (!onTracked) {
                        for (const auto& tracked : actHandler.GetTrackedTerminals()) {
                            if (tracked.formID == targetId) { onTracked = true; break; }
                        }
                    }
                }
            }

            // WHY-NOT WITNESS. The owner can touch-activate a button while its rollover still
            // reads "press X to activate", which means this flag was false for a target that is
            // demonstrably a working activator. Only two things can cause that: the tracked list
            // does not contain it (scan never ran, ran in another cell, or dropped it), or the
            // wand's target handle is a different reference than the one we tracked. Logging
            // both the target and the list size distinguishes those without guessing.
            // Sampled hard - this runs every frame, for both wands.
            if (!onTracked && handle) {
                static std::uint32_t s_lastUntrackedRef[2] = { 0, 0 };
                RE::NiPointer<RE::TESObjectREFR> refr = handle.get();
                if (refr && refr->formID != s_lastUntrackedRef[wand]) {
                    s_lastUntrackedRef[wand] = refr->formID;
                    auto* baseObj = refr->GetObjectReference();
                    spdlog::info("[ROLLOVER-WHYNOT] wand={} target ref={:08X} base={:08X} type={} "
                                 "NOT in tracked list (activators={} terminals={})",
                        wand,
                        refr->formID,
                        baseObj ? baseObj->GetFormID() : 0,
                        baseObj ? static_cast<int>(baseObj->GetFormType()) : -1,
                        actHandler.GetTrackedActivators().size(),
                        actHandler.GetTrackedTerminals().size());
                }
            }

            // DECISIVE WITNESS for "this button still shows the X glyph". ROLLOVER-FIELDS only
            // fires when the pre-write field bits CHANGE, so it cannot answer "did the
            // replacement run while I was pointing at the lift panel". This logs the resolved
            // target and the tracked decision per wand, on change, so a specific complaint can
            // be matched to a specific line.
            {
                static std::uint32_t s_lastTarget[2] = { 0, 0 };
                static bool s_lastTracked[2] = { false, false };
                std::uint32_t curTarget = 0;
                if (handle) {
                    RE::NiPointer<RE::TESObjectREFR> r = handle.get();
                    if (r) curTarget = r->formID;
                }
                if (curTarget != s_lastTarget[wand] || onTracked != s_lastTracked[wand]) {
                    s_lastTarget[wand] = curTarget;
                    s_lastTracked[wand] = onTracked;
                    spdlog::info("[ROLLOVER-DECIDE] wand={} target={:08X} tracked={}",
                                 wand, curTarget, onTracked);
                }
            }

            g_wandOnTrackedActivator[wand].store(onTracked, std::memory_order_relaxed);
        }
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

        save.wroteMask = RolloverSaveState::kWroteActivate | RolloverSaveState::kWroteSecondary |
                         RolloverSaveState::kWrotePlayFlag;
        save.wroteActivateText  = 0;
        save.wroteSecondaryText = 0;
        save.wrotePlayFlag      = 0;
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

        save.wroteMask = RolloverSaveState::kWroteItemName | RolloverSaveState::kWroteActivate |
                         RolloverSaveState::kWroteSecondary | RolloverSaveState::kWrotePlayFlag;
        save.wroteItemName      = 0;
        save.wroteActivateText  = 0;
        save.wroteSecondaryText = 0;
        save.wrotePlayFlag      = 0;
    }

    // Replace prompts for tracked activators and holotapes (hideWandHUD = OFF).
    // Returns true if any replacement was made (caller must RestoreRolloverButtons after).
    // `onTrackedActivator` is the main-thread snapshot from RefreshWandActivatorTargets — this
    // runs with the params spinlock held, so it must not walk containers or resolve handles.
    static bool ReplaceRolloverPrompts(void* thisPtr, bool onTrackedActivator, bool isLeftWand, RolloverSaveState& save)
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

            save.wroteMask = RolloverSaveState::kWroteActivate | RolloverSaveState::kWroteSecondary |
                             RolloverSaveState::kWrotePlayFlag;
            save.wroteActivateText  = 0;
            save.wroteSecondaryText = g_holotapeHintEntry;
            save.wrotePlayFlag      = 0;
            return true;
        }

        // Tracked activator: "Elevator  [A] Activate"  ->  "Elevator  Touch to activate".
        //
        // The button GLYPH is not a separate field - the engine draws it because activateText
        // is non-empty (FUN_141bc1a80 is a string-length test on that pointer). So writing the
        // hint INTO activateText, as this used to, keeps the [A] pictogram and produces the
        // nonsense "Elevator [A] Touch to activate": a button prompt telling you not to press
        // a button. Nulling activateText removes the glyph, and the hint moves to secondaryText,
        // which renders as plain text - exactly the arrangement the holotape branch above
        // already uses for "Insert to Play", and proven by it.
        //
        // itemName (+0x610) is untouched, so the object keeps its name.
        if (*activateText != 0 && onTrackedActivator) {
            // WITNESS: which fields actually carried a prompt before we wrote anything.
            // The owner reports TWO prompts on one elevator - "Touch to activate" AND a
            // separate "X/A Push". Only two arrangements can produce that: both fields
            // populated on THIS widget (secondary survived because the old code never wrote
            // it), or the other wand's rollover showing its own untouched prompt. Recording
            // the pre-write state of both fields, per wand, distinguishes them without
            // another round of guessing. Fires only on change, so it cannot spam.
            {
                const std::uint32_t bits =
                    0x80000000u |
                    (*activateText != 0 ? 1u : 0u) |
                    (*secondaryText != 0 ? 2u : 0u) |
                    (*playFlag != 0 ? 4u : 0u);
                g_rolloverFieldWitness[isLeftWand ? 1 : 0].store(
                    bits, std::memory_order_relaxed);
            }

            // Both hint slots blanked (no pictogram of any kind) and itemName replaced with a
            // bare "Touch to activate" - the object's own name is deliberately NOT shown.
            //
            // itemName is the only plain-text field in the struct (-> UIUtils::SetText); the
            // other two are ButtonHintData slots whose glyph is baked into the widget and
            // cannot be suppressed while keeping their text. Using the pre-cached static entry
            // means nothing is composed per object, so there is no per-name cache, no
            // allocation, and no string built anywhere near the render thread.
            save.activateText  = *activateText;
            save.secondaryText = *secondaryText;
            *activateText  = 0;                  // no [A]/[X] button hint
            *secondaryText = 0;                  // no grab hint
            save.wroteMask = RolloverSaveState::kWroteActivate |
                             RolloverSaveState::kWroteSecondary;
            save.wroteActivateText  = 0;
            save.wroteSecondaryText = 0;

            if (g_touchHintEntry != 0) {
                auto* itemName = reinterpret_cast<uintptr_t*>(base + 0x610);
                save.itemName = *itemName;
                *itemName     = g_touchHintEntry;
                save.wroteMask |= RolloverSaveState::kWroteItemName;
                save.wroteItemName = g_touchHintEntry;
            }
            return true;
        }

        return false;
    }

    // All-or-nothing restore. First a verify pass: every field we wrote must still hold exactly
    // the value we wrote. Any mismatch proves the producer ran during the render window, and its
    // state - the whole ShowRolloverParameters, written under one lock - is authoritative and
    // refcount-correct, so we put back NOTHING. (A per-field compare cannot make that call:
    // playFlag's byte space collides, and a producer write can leave individual fields
    // bit-identical. One field's mismatch is proof for all of them.)
    //
    // Leak bound when we decline: the displaced saved entries keep the one reference each that
    // their field used to own - at most 3 refs per raced producer event (hideAll), 2 (hideButtons
    // / holotape), 1 (activator); the immortal substitutes leak nothing. Bounded per event,
    // race-gated, and only ever pins entries that were already live.
    //
    // Residual (accepted): a producer write that leaves every written field bit-identical is
    // indistinguishable from "untouched", so stale values can be re-parked for at most one
    // producer event. In the hide branches that is invisible (the next frame re-nulls before
    // render); in the replace branches it requires the producer to assign the identical interned
    // string entry our substitute uses.
    static void RestoreRolloverState(void* thisPtr, const RolloverSaveState& save)
    {
        if (!save.wroteMask) return;

        auto base = reinterpret_cast<uintptr_t>(thisPtr);
        auto* itemName      = reinterpret_cast<uintptr_t*>(base + 0x610);
        auto* activateText  = reinterpret_cast<uintptr_t*>(base + 0x618);
        auto* secondaryText = reinterpret_cast<uintptr_t*>(base + 0x620);
        auto* playFlag      = reinterpret_cast<uint8_t*>(base + 0x62A);

        uintptr_t wrote = 0, found = 0;
        bool intact = true;
        if ((save.wroteMask & RolloverSaveState::kWroteItemName) && *itemName != save.wroteItemName) {
            intact = false; wrote = save.wroteItemName; found = *itemName;
        }
        if ((save.wroteMask & RolloverSaveState::kWroteActivate) && *activateText != save.wroteActivateText) {
            intact = false; wrote = save.wroteActivateText; found = *activateText;
        }
        if ((save.wroteMask & RolloverSaveState::kWroteSecondary) && *secondaryText != save.wroteSecondaryText) {
            intact = false; wrote = save.wroteSecondaryText; found = *secondaryText;
        }
        if ((save.wroteMask & RolloverSaveState::kWrotePlayFlag) && *playFlag != save.wrotePlayFlag) {
            intact = false; wrote = save.wrotePlayFlag; found = *playFlag;
        }

        if (!intact) {
            g_rolloverRestoreDeclines.fetch_add(1, std::memory_order_relaxed);
            g_rolloverDeclineDetail.store((static_cast<uint64_t>(wrote & 0xFFFFFFFFu) << 32) |
                                          (found & 0xFFFFFFFFu), std::memory_order_relaxed);
            return;
        }

        if (save.wroteMask & RolloverSaveState::kWroteItemName)  *itemName      = save.itemName;
        if (save.wroteMask & RolloverSaveState::kWroteActivate)  *activateText  = save.activateText;
        if (save.wroteMask & RolloverSaveState::kWroteSecondary) *secondaryText = save.secondaryText;
        if (save.wroteMask & RolloverSaveState::kWrotePlayFlag)  *playFlag      = save.playFlag;
    }

    // Both wands share one body. The primary and secondary vf004 are byte-identical in the
    // engine, and every past bug in this hook came from the two copies drifting apart, so there
    // is deliberately only one copy to drift.
    // SEH only — a function using __try may not hold C++ objects with destructors, so the lock
    // scopes live in the caller. The original is known to be able to fault (the sibling
    // HideRollover is SEH-wrapped in PipboyInteraction for exactly that reason), and if it
    // unwound past us our substitute would stay parked in the engine's live parameters for the
    // rest of the session — so the restore must run no matter how the original exits.
    // Returns 0 on success, else the swallowed exception code. STATUS_STACK_OVERFLOW is NOT
    // swallowed: continuing after one leaves the guard page unarmed, so the next exhaustion
    // would kill the process with no exception dispatch at all (no Buffout dump, no minidump) -
    // letting it crash here instead produces a usable dump.
    static DWORD CallOrigRolloverGuarded(ShowRolloverFunc orig, void* thisPtr)
    {
        if (!orig) return 0;
        __try {
            orig(thisPtr);
        } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW
                        ? EXCEPTION_CONTINUE_SEARCH
                        : EXCEPTION_EXECUTE_HANDLER) {
            return GetExceptionCode();
        }
        return 0;
    }

    // If the original faulted inside one of its three inlined spinlock windows, SEH unwinds
    // past the inline unlock (it is straight-line code, not a funclet) and the lock stays
    // owner=render-thread / count>0 forever. That is silent and fatal-at-a-distance: the render
    // thread keeps re-entering happily while the producer thread spins on it for the rest of
    // the session. Repairable precisely because the orphan is OURS to detect: owner == this
    // thread with a nonzero count, observed from OUTSIDE any lock scope of ours, can only be
    // vf004's abandoned acquisition. vf004's three locks: params +0x648/+0x64C, warn-color
    // +0x65C/+0x660, refresh-flag +0x54C/+0x550.
    static void RepairOrphanedRolloverLocks(void* thisPtr)
    {
        const DWORD self = GetCurrentThreadId();
        const auto base  = reinterpret_cast<uintptr_t>(thisPtr);
        static constexpr uintptr_t kLockPairs[][2] = {
            { 0x648, 0x64C }, { 0x65C, 0x660 }, { 0x54C, 0x550 },
        };
        for (const auto& pair : kLockPairs) {
            auto* owner = reinterpret_cast<volatile DWORD*>(base + pair[0]);
            auto* count = reinterpret_cast<volatile LONG*>(base + pair[1]);
            if (*owner == self && *count > 0) {
                // Engine unlock order: clear owner, then count. No other thread can touch
                // count while it is nonzero, so plain writes + one interlocked store suffice.
                *owner = 0;
                InterlockedExchange(count, 0);
                g_rolloverLockRepairs.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    static void RunRolloverHook(void* thisPtr, bool isLeft, ShowRolloverFunc orig, RolloverSaveState& save)
    {
        // When a blocking menu (PauseMenu, LoadingMenu, etc.) is open, skip entirely —
        // Scaleform state may be torn down/invalid, calling the original crashes.
        if (!thisPtr || MenuChecker::GetSingleton().IsGameStopped()) {
            return;
        }

        // Read before the lock is taken — see RefreshWandActivatorTargets.
        const bool onTrackedActivator =
            g_wandOnTrackedActivator[isLeft ? 1 : 0].load(std::memory_order_relaxed);

        save.wroteMask = 0;

        {
            RolloverParamsLock paramsLock(thisPtr);
            if (g_config.hideAllWandHUD) {
                NullAllRolloverFields(thisPtr, save);
            } else if (g_config.hideWandHUD) {
                HideWandButtons(thisPtr, save);
            } else {
                ReplaceRolloverPrompts(thisPtr, onTrackedActivator, isLeft, save);
            }
        }

        // Lock released: the original snapshots the parameters under its own lock and then
        // renders without it, exactly as it would unhooked.
        const DWORD faultCode = CallOrigRolloverGuarded(orig, thisPtr);
        if (faultCode != 0) {
            g_rolloverOrigFaults.fetch_add(1, std::memory_order_relaxed);
            g_rolloverOrigFaultCode.store(faultCode, std::memory_order_relaxed);
            // Must run BEFORE our restore lock: a lock orphaned by the fault would otherwise be
            // silently re-entered (same thread) and never returned to zero.
            RepairOrphanedRolloverLocks(thisPtr);
        }

        if (!save.wroteMask) return;

        RolloverParamsLock paramsLock(thisPtr);
        RestoreRolloverState(thisPtr, save);
    }

    static void __fastcall HookShowRolloverLeft(void* thisPtr)
    {
        RunRolloverHook(thisPtr, true, g_origShowRolloverLeft, _savedLeft);
    }

    static void __fastcall HookShowRolloverRight(void* thisPtr)
    {
        RunRolloverHook(thisPtr, false, g_origShowRolloverRight, _savedRight);
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
        // The engine renders this next to the button glyph, so it reads as the whole prompt.
        // "Touch" alone was ambiguous - it sat where "Activate" had been and looked like a
        // relabelled button press rather than an instruction to reach out and touch the thing.
        static RE::BSFixedString sTouch("Touch to activate");
        g_touchHintEntry = *reinterpret_cast<uintptr_t*>(&sTouch);

        // We hand these raw pointers to the engine without an addref, so they must not be
        // freeable — see ImmortalizeStringEntry.
        ImmortalizeStringEntry(g_holotapeHintEntry);
        ImmortalizeStringEntry(g_touchHintEntry);
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
