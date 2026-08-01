/**
 * HeisenbergInterface.cpp - Implementation of the Heisenberg public API
 * 
 * This provides the concrete implementation of IHeisenbergInterface001 that
 * other plugins can use to interact with Heisenberg.
 */

#include "HeisenbergInterface001.h"
#include "Heisenberg.h"
#include "../external/ROCK/src/ROCKMain.h"
#include "Grab.h"
#include "Hand.h"
#include <Windows.h>   // GetModuleHandleExA / GetModuleFileNameA — identify DisableHand caller
#include <intrin.h>    // _ReturnAddress
#include <cstring>     // strrchr
#include "Config.h"
#include "DropToHand.h"
#include "SmartGrabHandler.h"
#include "ActivatorHandler.h"
#include "HandCollision.h"
#include "MenuChecker.h"
#include "VirtualHolstersAPI.h"
#include "F4VROffsets.h"

namespace HeisenbergPluginAPI {

    // Build number - increment when API changes
    // 2 (Jul 19 2026): vtable extended — weapon-collision control, off-hand grip block,
    //   IsOffHandGrippingWeapon, per-hand DisableHandCollision/EnableHandCollision.
    //   External consumers must check GetBuildNumber() >= 2 before calling those slots.
    // 3 (Jul 20 2026): append-only dual-wield state/input/contact bridge.
    //   External consumers must check GetBuildNumber() >= 3 before calling those slots.
    constexpr unsigned int HEISENBERG_BUILD_NUMBER = 3;

    // =========================================================================
    // Callback storage
    // =========================================================================
    
    /**
     * Callback registration can race physics/main-thread delivery and a callback
     * may register another callback from inside its own invocation. Publish an
     * immutable copy-on-write snapshot so delivery is lock-free and iteration
     * can never be invalidated by a concurrent registration.
     */
    template <class Callback>
    class CallbackRegistry
    {
    public:
        CallbackRegistry() :
            _snapshot(std::make_shared<const CallbackList>())
        {}

        void Add(Callback callback)
        {
            if (!callback) {
                return;
            }

            std::lock_guard lock(_writeMutex);
            const auto current = _snapshot.load(std::memory_order_acquire);
            if (std::find(current->begin(), current->end(), callback) != current->end()) {
                return;
            }

            auto next = std::make_shared<CallbackList>(*current);
            next->push_back(callback);
            std::shared_ptr<const CallbackList> published = std::move(next);
            _snapshot.store(std::move(published), std::memory_order_release);
        }

        template <class... Args>
        void Invoke(Args&&... args) const noexcept
        {
            const auto callbacks = _snapshot.load(std::memory_order_acquire);
            for (const auto callback : *callbacks) {
                try {
                    callback(std::forward<Args>(args)...);
                } catch (...) {
                    // A consumer must never unwind through Heisenberg's game or
                    // Havok callback path.
                    spdlog::error("[HeisenbergAPI] Consumer callback threw; invocation ignored");
                }
            }
        }

    private:
        using CallbackList = std::vector<Callback>;

        std::mutex _writeMutex;
        std::atomic<std::shared_ptr<const CallbackList>> _snapshot;
    };

    static CallbackRegistry<IHeisenbergInterface001::GrabbedCallback> g_grabbedCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::DroppedCallback> g_droppedCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::StashedCallback> g_stashedCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::ConsumedCallback> g_consumedCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::PulledCallback> g_pulledCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::CollisionCallback> g_collisionCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::PrePhysicsCallback> g_prePhysicsCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::PostPhysicsCallback> g_postPhysicsCallbacks;
    static CallbackRegistry<IHeisenbergInterface001::ViewCasterTargetChangedCallback> g_viewCasterCallbacks;

    // Per-hand state.
    // Jul 19 (user directive): external disables are plain LATCHES — a feature stays disabled
    // until the caller explicitly re-enables it (no auto-expiry). The Jul-11 5s lease existed
    // because an early VirtualReloads never called EnableHand; consumers now use the
    // disable-at-start / enable-when-done pattern, and the timer caused features to pop back
    // mid-window. A caller that dies without re-enabling leaves the feature off until relaunch
    // — by design. Timestamps kept (0 = not disabled) for log attribution.
    static std::atomic_uint64_t g_leftHandDisabledAtMs{ 0 };   // 0 = not disabled
    static std::atomic_uint64_t g_rightHandDisabledAtMs{ 0 };

    static bool IsExternalHandDisableActive(bool isLeft)
    {
        return (isLeft ? g_leftHandDisabledAtMs : g_rightHandDisabledAtMs)
                   .load(std::memory_order_acquire) != 0;
    }

    // Jul 19 (Virtual Reloads API, latch semantics per user directive — see the hand-disable
    // comment above): weapon-collision disable, off-hand grip block, and per-hand collision
    // disable are plain latches. 0 = not active; non-zero = the GetTickCount64() timestamp of
    // the disable (attribution only).
    static std::atomic_uint64_t g_weaponCollisionDisabledAtMs{ 0 };  // 0 = enabled
    static std::atomic_uint64_t g_offhandGripBlockedAtMs{ 0 };       // 0 = unblocked
    static std::atomic_uint64_t g_handCollisionDisabledAtMs[2] = {
        std::atomic_uint64_t{ 0 },
        std::atomic_uint64_t{ 0 }
    };  // [0]=Right [1]=Left; 0 = enabled

    // Jul 24 (user directive: "never use the lease again"): renamed from
    // IsExternalLeaseActive — the 5s-TTL lease is permanently gone and must not come
    // back; this is a plain latch check and the name now says so.
    static bool IsExternalLatchActive(const std::atomic_uint64_t& ts, bool&, const char*)
    {
        return ts.load(std::memory_order_acquire) != 0;
    }
    static bool g_unusedLeaseLogFlag = false;  // keeps legacy call sites' signature stable

    // =========================================================================
    // Callback invocation functions (called from Heisenberg internals)
    // =========================================================================
    
    void InvokeGrabbedCallbacks(bool isLeft, RE::TESObjectREFR* refr)
    {
        g_grabbedCallbacks.Invoke(isLeft, refr);
    }

    void InvokeDroppedCallbacks(bool isLeft, RE::TESObjectREFR* refr)
    {
        g_droppedCallbacks.Invoke(isLeft, refr);
    }

    void InvokeStashedCallbacks(bool isLeft, RE::TESForm* form)
    {
        g_stashedCallbacks.Invoke(isLeft, form);
    }

    void InvokeConsumedCallbacks(bool isLeft, RE::TESForm* form)
    {
        g_consumedCallbacks.Invoke(isLeft, form);
    }

    void InvokePulledCallbacks(bool isLeft, RE::TESObjectREFR* refr)
    {
        g_pulledCallbacks.Invoke(isLeft, refr);
    }

    void InvokeCollisionCallbacks(bool isLeft, float mass, float velocity)
    {
        g_collisionCallbacks.Invoke(isLeft, mass, velocity);
    }

    void InvokePrePhysicsCallbacks(void* bhkWorld)
    {
        g_prePhysicsCallbacks.Invoke(bhkWorld);
    }

    void InvokePostPhysicsCallbacks(void* bhkWorld)
    {
        g_postPhysicsCallbacks.Invoke(bhkWorld);
    }

    void InvokeViewCasterTargetChangedCallbacks(bool isLeft, RE::TESObjectREFR* newTarget, RE::TESObjectREFR* oldTarget)
    {
        g_viewCasterCallbacks.Invoke(isLeft, newTarget, oldTarget);
    }

    // =========================================================================
    // Interface implementation
    // =========================================================================
    
    class HeisenbergInterface001Impl : public IHeisenbergInterface001
    {
    public:
        // =====================================================================
        // VERSION INFO
        // =====================================================================
        
        unsigned int GetBuildNumber() override
        {
            return HEISENBERG_BUILD_NUMBER;
        }

        // =====================================================================
        // GRAB STATE QUERIES
        // =====================================================================
        
        bool IsHoldingObject(bool isLeft) override
        {
            return heisenberg::GrabManager::GetSingleton().IsGrabbing(isLeft);
        }

        bool IsPulling(bool isLeft) override
        {
            return heisenberg::GrabManager::GetSingleton().IsPulling(isLeft);
        }

        bool CanGrabObject(bool isLeft) override
        {
            // Check if hand is disabled
            if (IsExternalHandDisableActive(isLeft)) {
                return false;
            }
            
            // Check if already holding
            if (IsHoldingObject(isLeft)) {
                return false;
            }
            
            // Check if in a menu (game stopped)
            if (heisenberg::MenuChecker::GetSingleton().IsGameStopped()) {
                return false;
            }
            
            return true;
        }

        RE::TESObjectREFR* GetGrabbedObject(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.GetRefr();
        }

        const char* GetGrabbedNodeName(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            if (state.active && state.node) {
                return state.node->name.c_str();
            }
            return "";
        }

        // =====================================================================
        // VIEWCASTER / SELECTION QUERIES
        // =====================================================================

        RE::TESObjectREFR* GetViewCasterTarget(bool isLeft) override
        {
            // In F4VR: primary wand (isLeft=true typically) = left hand
            // secondary wand = right hand
            // Note: bLeftHandedMode swaps these
            return isLeft ? GetPrimaryWandTarget() : GetSecondaryWandTarget();
        }

        RE::TESObjectREFR* GetPrimaryWandTarget() override
        {
            RE::ObjectRefHandle handle = heisenberg::GetPrimaryViewCasterTarget();
            if (handle) {
                RE::NiPointer<RE::TESObjectREFR> ptr = handle.get();
                return ptr.get();
            }
            return nullptr;
        }

        RE::TESObjectREFR* GetSecondaryWandTarget() override
        {
            RE::ObjectRefHandle handle = heisenberg::GetSecondaryViewCasterTarget();
            if (handle) {
                RE::NiPointer<RE::TESObjectREFR> ptr = handle.get();
                return ptr.get();
            }
            return nullptr;
        }

        RE::TESObjectREFR* GetSelectedObject(bool isLeft) override
        {
            // Get from Heisenberg's own selection system (may differ from ViewCaster)
            auto& modInst = heisenberg::Heisenberg::GetSingleton();
            auto* hand = isLeft ? modInst.GetLeftHand() : modInst.GetRightHand();
            if (hand) {
                return hand->GetSelectedObject();
            }
            return nullptr;
        }

        // =====================================================================
        // GRAB CONTROL
        // =====================================================================

        bool GrabObject(RE::TESObjectREFR* object, bool isLeft) override
        {
            if (!object || !CanGrabObject(isLeft)) {
                return false;
            }
            
            return heisenberg::GrabManager::GetSingleton().StartGrabOnRef(object, isLeft, false, false);
        }

        void DropObject(bool isLeft, const RE::NiPoint3* throwVelocity) override
        {
            if (IsHoldingObject(isLeft)) {
                heisenberg::GrabManager::GetSingleton().EndGrab(isLeft, throwVelocity, false);
            }
        }

        void ForceEndGrab(bool isLeft) override
        {
            if (IsHoldingObject(isLeft)) {
                heisenberg::GrabManager::GetSingleton().EndGrab(isLeft, nullptr, true);
            }
        }

        // =====================================================================
        // HAND STATE CONTROL
        // =====================================================================

        void DisableHand(bool isLeft) override
        {
            if (isLeft) {
                g_leftHandDisabledAtMs.store(GetTickCount64(), std::memory_order_release);
            } else {
                g_rightHandDisabledAtMs.store(GetTickCount64(), std::memory_order_release);
            }
            // Jul 6: identify WHICH plugin disabled the hand — an external mod was disabling the LEFT
            // hand and never re-enabling it, killing left-hand grab. Log the calling module so the
            // culprit is named. (Same _ReturnAddress->module trick as the OpenVR input routing fix.)
            const char* callerModule = "unknown";
            char moduleName[MAX_PATH] = {};
            HMODULE hmod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(_ReturnAddress()), &hmod)
                && hmod && GetModuleFileNameA(hmod, moduleName, MAX_PATH) > 0) {
                if (const char* slash = std::strrchr(moduleName, '\\')) {
                    callerModule = slash + 1;
                } else {
                    callerModule = moduleName;
                }
            }
            spdlog::info("[HeisenbergAPI] {} hand DISABLED by module '{}' (grab/collision on this hand now suppressed until it calls EnableHand)",
                         isLeft ? "Left" : "Right", callerModule);
        }

        void EnableHand(bool isLeft) override
        {
            if (isLeft) {
                g_leftHandDisabledAtMs.store(0, std::memory_order_release);
            } else {
                g_rightHandDisabledAtMs.store(0, std::memory_order_release);
            }
            spdlog::debug("[HeisenbergAPI] {} hand enabled", isLeft ? "Left" : "Right");
        }

        bool IsHandDisabled(bool isLeft) override
        {
            return IsExternalHandDisableActive(isLeft);
        }

        // =====================================================================
        // FINGER TRACKING
        // =====================================================================

        void GetFingerCurls(bool isLeft, float values[5]) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            if (state.hasRuntimeFingerCurls) {
                values[0] = state.runtimeThumbCurl;
                values[1] = state.runtimeIndexCurl;
                values[2] = state.runtimeMiddleCurl;
                values[3] = state.runtimeRingCurl;
                values[4] = state.runtimePinkyCurl;
            } else {
                // Default - return current single curl value for all fingers
                float curl = heisenberg::Heisenberg::GetSingleton().GetFingerCurlValue(isLeft);
                for (int i = 0; i < 5; i++) {
                    values[i] = curl;
                }
            }
        }

        void SetFingerCurls(bool isLeft, const float values[5]) override
        {
            auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            state.SetRuntimeFingerCurls(values[0], values[1], values[2], values[3], values[4]);
            state.pendingFingerCurls = false;
            
            // Also set the single curl value used by Heisenberg for FRIK
            float avgCurl = (values[0] + values[1] + values[2] + values[3] + values[4]) / 5.0f;
            heisenberg::Heisenberg::GetSingleton().SetFingerCurlValue(isLeft, avgCurl);
        }

        // =====================================================================
        // ZONE DETECTION
        // =====================================================================

        bool IsInStorageZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInStorageZone;
        }

        bool IsInEquipZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInEquipZone;
        }

        bool IsInMouthZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInMouthZone;
        }

        bool IsInVHZone(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            return state.isInVHZone;
        }

        const char* GetCurrentZoneName(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            if (state.isInStorageZone) return "STORAGE";
            if (state.isInMouthZone) return "MOUTH";
            if (state.isInVHZone) return "VH_HOLSTER";
            if (state.isInEquipZone) return state.currentZoneName;
            return "";
        }

        int GetVHZoneIndex(bool /*isLeft*/) override
        {
            auto* vhApi = VirtualHolsters::RequestVirtualHolstersAPI();
            if (!vhApi) return 0;
            return static_cast<int>(vhApi->GetCurrentHolster());
        }

        // =====================================================================
        // INVENTORY INTEGRATION
        // =====================================================================

        bool DropToHand(RE::TESForm* form, bool isLeft) override
        {
            if (!form) return false;
            // Queue the item to drop to hand
            heisenberg::DropToHand::GetSingleton().QueueDropToHand(form->formID, isLeft, 1, true, false);
            return true;
        }

        bool IsInActivationZone(bool isLeft) override
        {
            return heisenberg::ActivatorHandler::GetSingleton().IsHandInPointingRange(isLeft);
        }

        // =====================================================================
        // TRANSFORM CONTROL
        // =====================================================================

        RE::NiTransform GetGrabTransform(bool isLeft) override
        {
            const auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            RE::NiTransform transform;
            transform.translate = state.grabOffsetLocal;
            transform.rotate = state.grabRotationLocal;
            transform.scale = 1.0f;
            return transform;
        }

        void SetGrabTransform(bool isLeft, const RE::NiTransform& transform) override
        {
            auto& state = heisenberg::GrabManager::GetSingleton().GetGrabState(isLeft);
            state.grabOffsetLocal = transform.translate;
            state.grabRotationLocal = transform.rotate;
        }

        // =====================================================================
        // CALLBACKS
        // =====================================================================

        void AddGrabbedCallback(GrabbedCallback callback) override
        {
            g_grabbedCallbacks.Add(callback);
        }

        void AddDroppedCallback(DroppedCallback callback) override
        {
            g_droppedCallbacks.Add(callback);
        }

        void AddStashedCallback(StashedCallback callback) override
        {
            g_stashedCallbacks.Add(callback);
        }

        void AddConsumedCallback(ConsumedCallback callback) override
        {
            g_consumedCallbacks.Add(callback);
        }

        void AddPulledCallback(PulledCallback callback) override
        {
            g_pulledCallbacks.Add(callback);
        }

        void AddCollisionCallback(CollisionCallback callback) override
        {
            g_collisionCallbacks.Add(callback);
        }

        void AddPrePhysicsCallback(PrePhysicsCallback callback) override
        {
            g_prePhysicsCallbacks.Add(callback);
        }

        void AddPostPhysicsCallback(PostPhysicsCallback callback) override
        {
            g_postPhysicsCallbacks.Add(callback);
        }

        void AddViewCasterTargetChangedCallback(ViewCasterTargetChangedCallback callback) override
        {
            g_viewCasterCallbacks.Add(callback);
        }

        // =====================================================================
        // SETTINGS
        // =====================================================================

        bool GetSettingDouble(const char* name, double& out) override
        {
            return heisenberg::g_config.GetNumericSetting(name, out);
        }

        bool SetSettingDouble(const char* name, double val) override
        {
            return heisenberg::g_config.SetNumericSetting(name, val);
        }

        // =====================================================================
        // HAND COLLISION
        // =====================================================================

        bool IsHandCollisionEnabled() override
        {
            return heisenberg::g_config.enableHandCollision;
        }

        void* GetHandRigidBody(bool isLeft) override
        {
            // Two-scenario cleanup: Heisenberg no longer creates its own physics hand
            // bodies (real hand colliders live in the embedded ROCK engine or the
            // external ROCK.dll). Kept as a benign no-op to preserve the published
            // HeisenbergInterface001 vtable ABI — always returns nullptr, which the
            // documented contract already allows ("nullptr if not available").
            (void)isLeft;
            return nullptr;
        }

        bool IsHandInContact(bool isLeft) override
        {
            return heisenberg::HandCollision::GetSingleton().IsInContact(isLeft);
        }

        RE::TESObjectREFR* GetHandContactObject(bool isLeft) override
        {
            return heisenberg::HandCollision::GetSingleton().GetContactObject(isLeft);
        }

        void SuppressItemToHand(unsigned int durationMs) override
        {
            heisenberg::DropToHand::SuppressItemToHand(
                durationMs ? durationMs : 1500);
        }

        // =====================================================================
        // WEAPON COLLISION + TWO-HANDING CONTROL (Virtual Reloads API, Jul 19)
        // =====================================================================

        void EnableWeaponCollision(bool enable) override
        {
            if (enable) {
                if (g_weaponCollisionDisabledAtMs.exchange(0, std::memory_order_acq_rel) != 0) {
                    spdlog::info("[HeisenbergAPI] weapon collision RE-ENABLED by external module");
                }
            } else {
                const auto previous = g_weaponCollisionDisabledAtMs.exchange(
                    GetTickCount64(), std::memory_order_acq_rel);
                if (previous == 0) {
                    spdlog::info("[HeisenbergAPI] weapon collision DISABLED by external module (latched until it calls EnableWeaponCollision(true))");
                }
            }
        }

        bool IsWeaponCollisionDisabled() override
        {
            return IsWeaponCollisionDisabledByAPI();
        }

        void BlockOffHandWeaponGripping(const char* tag, bool block) override
        {
            if (block) {
                const auto previous = g_offhandGripBlockedAtMs.exchange(
                    GetTickCount64(), std::memory_order_acq_rel);
                if (previous == 0) {
                    spdlog::info("[HeisenbergAPI] off-hand weapon gripping BLOCKED by '{}' (latched until unblocked)",
                                 tag ? tag : "(null)");
                }
            } else {
                if (g_offhandGripBlockedAtMs.exchange(0, std::memory_order_acq_rel) != 0) {
                    spdlog::info("[HeisenbergAPI] off-hand weapon gripping UNBLOCKED by '{}'", tag ? tag : "(null)");
                }
            }
        }

        bool IsOffHandWeaponGrippingBlocked() override
        {
            return IsOffHandGripBlockedByAPI();
        }

        bool IsOffHandGrippingWeapon() override
        {
            // Embedded ROCK owns two-handing; FRIK's isOffHandGrippingWeapon cannot see it.
            // Uses HostIsWeaponSupportGripped (gripping only), NOT HostIsWeaponSupportEngaged
            // (which also counts the Touching state - resting/brushing the off hand against
            // the weapon's foregrip with no grip input, which this build-2 API's documented
            // contract does not include). A consumer like VirtualReloads polling this to
            // decide whether the support hand is actually gripping would otherwise get true
            // on a bare hover and misclassify it as an engaged two-handed grip.
            return heisenberg::IsRockEngineHosted() &&
                   (rock::HostIsWeaponSupportGripped(true) || rock::HostIsWeaponSupportGripped(false));
        }

        void DisableHandCollision(bool isLeft) override
        {
            auto& ts = g_handCollisionDisabledAtMs[isLeft ? 1 : 0];
            const auto previous = ts.exchange(GetTickCount64(), std::memory_order_acq_rel);
            if (previous == 0) {
                spdlog::info("[HeisenbergAPI] {} hand collision DISABLED by external module (latched until it calls EnableHandCollision)",
                             isLeft ? "Left" : "Right");
            }
        }

        void EnableHandCollision(bool isLeft) override
        {
            auto& ts = g_handCollisionDisabledAtMs[isLeft ? 1 : 0];
            if (ts.exchange(0, std::memory_order_acq_rel) != 0) {
                spdlog::info("[HeisenbergAPI] {} hand collision RE-ENABLED by external module", isLeft ? "Left" : "Right");
            }
        }

        bool IsHandCollisionDisabled(bool isLeft) override
        {
            return IsHandCollisionDisabledByAPI(isLeft);
        }

        bool RegisterDualWieldStateProvider(DualWieldStateProvider provider) override
        {
            return RegisterDualWieldStateProviderImpl(provider);
        }

        bool UnregisterDualWieldStateProvider(DualWieldStateProvider provider) override
        {
            return UnregisterDualWieldStateProviderImpl(provider);
        }

        bool GetPhysicalHandInputState(
            PhysicalHand hand,
            PhysicalHandInputState* state) override
        {
            return GetPhysicalHandInputStateImpl(hand, state);
        }

        bool RegisterDualWieldWeaponContactCallback(DualWieldWeaponContactCallback callback) override
        {
            return RegisterDualWieldWeaponContactCallbackImpl(callback);
        }

        bool UnregisterDualWieldWeaponContactCallback(DualWieldWeaponContactCallback callback) override
        {
            return UnregisterDualWieldWeaponContactCallbackImpl(callback);
        }
    };

    // =========================================================================
    // Singleton instance
    // =========================================================================
    
    static HeisenbergInterface001Impl g_interfaceImpl;

    // =========================================================================
    // API FUNCTION CALLBACK (for messaging system)
    // =========================================================================
    
    /**
     * Returns the interface pointer for the requested revision.
     * Called by other plugins via the HeisenbergMessage callback.
     * 
     * @param revisionNumber 1 for IHeisenbergInterface001
     * @return Interface pointer, or nullptr if revision not supported
     */
    void* GetApi(unsigned int revisionNumber)
    {
        switch (revisionNumber)
        {
        case 1:
            spdlog::info("[HeisenbergAPI] GetApi(1) - returning IHeisenbergInterface001");
            return static_cast<void*>(&g_interfaceImpl);
        default:
            spdlog::warn("[HeisenbergAPI] GetApi({}) - unknown revision", revisionNumber);
            return nullptr;
        }
    }

    /**
     * Handle interface request messages from other plugins.
     * Call this from your F4SE message handler when receiving kMessage_GetInterface.
     * 
     * @param message The HeisenbergMessage struct from the dispatch
     */
    void HandleInterfaceRequest(HeisenbergMessage* message, std::size_t messageSize)
    {
        constexpr auto kLegacyMessageSize = offsetof(HeisenbergMessage, structSize);
        if (!message || messageSize < kLegacyMessageSize) {
            spdlog::warn(
                "[HeisenbergAPI] Rejected undersized interface request ({} bytes, need at least {})",
                messageSize,
                kLegacyMessageSize);
            return;
        }

        message->GetApiFunction = &GetApi;
        if (messageSize >= offsetof(HeisenbergMessage, structSize) + sizeof(message->structSize)) {
            message->structSize = static_cast<std::uint32_t>(sizeof(HeisenbergMessage));
        }
        if (messageSize >= offsetof(HeisenbergMessage, abiVersion) + sizeof(message->abiVersion)) {
            message->abiVersion = HeisenbergMessage::kMessageAbiVersion;
        }
        if (messageSize >= offsetof(HeisenbergMessage, interfaceBuild) + sizeof(message->interfaceBuild)) {
            message->interfaceBuild = HEISENBERG_BUILD_NUMBER;
        }
        spdlog::info("[HeisenbergAPI] Interface request handled, callback set");
    }

    // =========================================================================
    // CLIENT-SIDE: Get interface from messaging
    // =========================================================================

    IHeisenbergInterface001* GetHeisenbergInterface001(
        const F4SE::PluginHandle& pluginHandle,
        F4SE::MessagingInterface* messagingInterface)
    {
        static std::mutex s_cacheMutex;
        static IHeisenbergInterface001* s_cachedInterface = nullptr;
        std::lock_guard cacheLock(s_cacheMutex);
        if (s_cachedInterface) {
            return s_cachedInterface;
        }

        if (!messagingInterface)
        {
            spdlog::warn("[HeisenbergAPI] GetHeisenbergInterface001: messagingInterface is null");
            return nullptr;
        }

        // Dispatch message to Heisenberg to get the API callback
        HeisenbergMessage msg{};
        msg.structSize = static_cast<std::uint32_t>(sizeof(msg));
        msg.abiVersion = HeisenbergMessage::kMessageAbiVersion;
        bool dispatched = messagingInterface->Dispatch(
            HeisenbergMessage::kMessage_GetInterface,
            &msg,
            sizeof(HeisenbergMessage),
            "Heisenberg_F4VR");

        if (!dispatched || !msg.GetApiFunction)
        {
            spdlog::warn("[HeisenbergAPI] GetHeisenbergInterface001: Heisenberg not loaded or dispatch failed");
            return nullptr;
        }

        // Get the interface for revision 1
        s_cachedInterface = static_cast<IHeisenbergInterface001*>(msg.GetApiFunction(1));
        if (s_cachedInterface)
        {
            spdlog::info("[HeisenbergAPI] GetHeisenbergInterface001: obtained interface build {}",
                         s_cachedInterface->GetBuildNumber());
        }
        
        return s_cachedInterface;
    }

    // =========================================================================
    // INTERNAL QUERY (for Hand.cpp / Grab.cpp)
    // =========================================================================

    bool IsHandDisabledByAPI(bool isLeft)
    {
        return IsExternalHandDisableActive(isLeft);
    }

    bool IsWeaponCollisionDisabledByAPI()
    {
        return IsExternalLatchActive(g_weaponCollisionDisabledAtMs, g_unusedLeaseLogFlag, "weapon-collision disable");
    }

    bool IsOffHandGripBlockedByAPI()
    {
        return IsExternalLatchActive(g_offhandGripBlockedAtMs, g_unusedLeaseLogFlag, "off-hand grip block");
    }

    bool IsHandCollisionDisabledByAPI(bool isLeft)
    {
        const int idx = isLeft ? 1 : 0;
        return IsExternalLatchActive(g_handCollisionDisabledAtMs[idx], g_unusedLeaseLogFlag,
                                     isLeft ? "left hand-collision disable" : "right hand-collision disable");
    }

} // namespace HeisenbergPluginAPI

// Global interface pointer (for internal use)
HeisenbergPluginAPI::IHeisenbergInterface001* g_heisenbergInterface = nullptr;
