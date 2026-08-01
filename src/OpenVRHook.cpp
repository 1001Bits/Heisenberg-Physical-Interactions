#include "OpenVRHook.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <DbgHelp.h>
#include <intrin.h>  // _ReturnAddress — per-consumer input routing (audit rank 1)
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>

#pragma comment(lib, "DbgHelp.lib")

namespace heisenberg
{
    // Static member initialization
    void* OpenVRHook::s_originalGetGenericInterface = nullptr;
    
    // ============================================================
    // FO4VRTools API compatibility
    // ============================================================
    
    // FO4VRTools callback signature
    typedef bool (*FO4VRTools_GetControllerState_CB)(vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
                                                      const vr::VRControllerState_t *pControllerState, 
                                                      uint32_t unControllerStateSize, 
                                                      vr::VRControllerState_t* pOutputControllerState);
    
    // FO4VRTools API interface (matches VRHookAPI.h)
    class OpenVRHookManagerAPI
    {
    public:
        virtual unsigned int GetVersion() = 0;
        virtual bool IsInitialized() = 0;
        virtual void RegisterControllerStateCB(FO4VRTools_GetControllerState_CB cbfunc) = 0;
        virtual void RegisterGetPosesCB(void* cbfunc) = 0;
        virtual void UnregisterControllerStateCB(FO4VRTools_GetControllerState_CB cbfunc) = 0;
        virtual void UnregisterGetPosesCB(void* cbfunc) = 0;
        virtual vr::IVRSystem* GetVRSystem() const = 0;
        virtual vr::IVRCompositor* GetVRCompositor() const = 0;
        virtual void StartHaptics(unsigned int trackedControllerId, float hapticTime, float hapticIntensity) = 0;
    };
    
    // Global pointer to FO4VRTools API (if available)
    static std::atomic<OpenVRHookManagerAPI*> g_fo4vrToolsAPI{nullptr};
    static std::mutex g_fo4vrToolsApiMutex;
    
    static std::atomic<vr::TrackedDeviceIndex_t> g_leftControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
    static std::atomic<vr::TrackedDeviceIndex_t> g_rightControllerIndex{vr::k_unTrackedDeviceIndexInvalid};

    namespace bridge_detail
    {
        struct Entry
        {
            std::uint64_t handle = 0;
            std::uint32_t phase = controller_bridge::kPreHeisenberg;
            std::uint32_t consumerMask = controller_bridge::kConsumerGame;
            std::int32_t priority = 0;
            std::uint64_t sequence = 0;
            controller_bridge::Callback callback = nullptr;
            void* userData = nullptr;
            std::string ownerName;
            std::atomic<bool> enabled{true};
            std::atomic<std::uint32_t> inFlight{0};
            std::mutex drainMutex;
            std::condition_variable drainCondition;
        };

        // Shutdown runs from atexit as well as normal teardown. ExitProcess may
        // already have terminated a polling thread while it owned this lock, so
        // the shutdown path must be able to give up instead of waiting forever.
        // Normal registry operations retain ordinary blocking lock semantics.
        static std::timed_mutex g_registryMutex;
        using EntryList = std::vector<std::shared_ptr<Entry>>;
        static EntryList g_entries;
        // Dispatch is a controller-poll hot path. Writers retain the mutex and
        // publish an immutable copy-on-write list; readers atomically acquire
        // it without taking the registry mutex or allocating a vector.
        static std::atomic<std::shared_ptr<const EntryList>> g_entrySnapshot;
        static std::atomic<std::uint64_t> g_nextHandle{1};
        static std::atomic<std::uint64_t> g_nextSequence{1};
        static thread_local std::uint64_t g_currentHandle = 0;

        void PublishEntrySnapshotLocked()
        {
            std::shared_ptr<const EntryList> published =
                std::make_shared<const EntryList>(g_entries);
            g_entrySnapshot.store(
                std::move(published),
                std::memory_order_release);
        }

        static bool EntryOrder(const std::shared_ptr<Entry>& lhs, const std::shared_ptr<Entry>& rhs)
        {
            if (lhs->phase != rhs->phase) {
                return lhs->phase < rhs->phase;
            }
            if (lhs->priority != rhs->priority) {
                return lhs->priority < rhs->priority;
            }
            return lhs->sequence < rhs->sequence;
        }

        bool Register(const controller_bridge::Registration* registration, std::uint64_t* handle)
        {
            if (!registration || !handle ||
                registration->structSize < sizeof(controller_bridge::Registration) ||
                registration->abiVersion != controller_bridge::kAbiVersion ||
                !registration->callback ||
                (registration->phase != controller_bridge::kPreHeisenberg &&
                 registration->phase != controller_bridge::kPostHeisenberg) ||
                (registration->consumerMask & controller_bridge::kConsumerAll) == 0) {
                return false;
            }

            auto entry = std::make_shared<Entry>();
            entry->handle = g_nextHandle.fetch_add(1, std::memory_order_relaxed);
            entry->phase = registration->phase;
            entry->consumerMask = registration->consumerMask & controller_bridge::kConsumerAll;
            entry->priority = registration->priority;
            entry->sequence = g_nextSequence.fetch_add(1, std::memory_order_relaxed);
            entry->callback = registration->callback;
            entry->userData = registration->userData;
            entry->ownerName = registration->ownerName ? registration->ownerName : "unnamed";

            {
                std::lock_guard<std::timed_mutex> lock(g_registryMutex);
                g_entries.push_back(entry);
                std::stable_sort(g_entries.begin(), g_entries.end(), EntryOrder);
                PublishEntrySnapshotLocked();
            }

            *handle = entry->handle;
            spdlog::info(
                "[OpenVRHook] Bridge callback registered: owner='{}', handle={}, phase={}, consumers=0x{:x}, priority={}",
                entry->ownerName, entry->handle, entry->phase, entry->consumerMask, entry->priority);
            return true;
        }

        bool Unregister(std::uint64_t handle, bool drainInFlight)
        {
            std::shared_ptr<Entry> entry;
            {
                std::lock_guard<std::timed_mutex> lock(g_registryMutex);
                const auto it = std::find_if(g_entries.begin(), g_entries.end(),
                    [handle](const auto& candidate) { return candidate->handle == handle; });
                if (it == g_entries.end()) {
                    return false;
                }
                entry = *it;
                entry->enabled.store(false, std::memory_order_release);
                g_entries.erase(it);
                PublishEntrySnapshotLocked();
            }

            if (drainInFlight) {
                // A callback can unregister itself, but its userData may still
                // be in use by another polling thread. Drain every concurrent
                // invocation while excluding this thread's one active frame;
                // the current callback cannot reach zero until unregister
                // returns. Non-self unregister retains the normal zero-reader
                // lifetime boundary.
                const std::uint32_t remainingSelfFrames =
                    g_currentHandle == handle ? 1u : 0u;
                std::unique_lock<std::mutex> lock(entry->drainMutex);
                // This is the lifetime boundary promised by unregister: callers
                // may destroy userData or unload callback code as soon as this
                // function returns. Unlike process-exit teardown below, an
                // ordinary unregister therefore cannot safely time out.
                entry->drainCondition.wait(lock, [&entry, remainingSelfFrames] {
                    return entry->inFlight.load(std::memory_order_acquire) <=
                           remainingSelfFrames;
                });
            }

            spdlog::info("[OpenVRHook] Bridge callback unregistered: owner='{}', handle={}",
                entry->ownerName, handle);
            return true;
        }

        std::uint64_t Dispatch(
            controller_bridge::ControllerRole role,
            vr::TrackedDeviceIndex_t deviceIndex,
            vr::VRControllerState_t* state,
            vr::VRControllerState_t* heisenbergState,
            std::uint32_t phase,
            std::uint32_t consumer,
            std::uint64_t* resultFlags)
        {
            if (!state || role == controller_bridge::kRoleUnknown) {
                return ~std::uint64_t{0};
            }

            const auto snapshot =
                g_entrySnapshot.load(std::memory_order_acquire);

            controller_bridge::CallbackContext context{
                sizeof(controller_bridge::CallbackContext),
                controller_bridge::kAbiVersion,
                phase,
                consumer,
                role,
                deviceIndex,
                0,
                heisenbergState,
                resultFlags,
            };

            std::uint64_t combinedMask = ~std::uint64_t{0};
            if (!snapshot) {
                return combinedMask;
            }
            for (const auto& entry : *snapshot) {
                if (entry->phase != phase ||
                    (entry->consumerMask & consumer) == 0 ||
                    !entry->enabled.load(std::memory_order_acquire)) {
                    continue;
                }

                entry->inFlight.fetch_add(1, std::memory_order_acq_rel);
                if (!entry->enabled.load(std::memory_order_acquire)) {
                    if (entry->inFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        entry->drainCondition.notify_all();
                    }
                    continue;
                }

                const auto previousHandle = g_currentHandle;
                g_currentHandle = entry->handle;
                std::uint64_t mask = ~std::uint64_t{0};
                try {
                    mask = entry->callback(&context, state, entry->userData);
                } catch (...) {
                    spdlog::error(
                        "[OpenVRHook] Bridge callback '{}' threw across the C ABI; disabling it",
                        entry->ownerName);
                    entry->enabled.store(false, std::memory_order_release);
                }
                g_currentHandle = previousHandle;
                combinedMask &= mask;
                state->ulButtonPressed &= mask;
                state->ulButtonTouched &= mask;

                const auto previousInFlight =
                    entry->inFlight.fetch_sub(1, std::memory_order_acq_rel);
                // Zero-reader unregister waits for 1->0; self-unregister waits
                // for the last concurrent reader's 2->1 transition.
                if (previousInFlight <= 2) {
                    entry->drainCondition.notify_all();
                }
            }
            return combinedMask;
        }

        std::uint32_t Count()
        {
            const auto snapshot =
                g_entrySnapshot.load(std::memory_order_acquire);
            return snapshot
                ? static_cast<std::uint32_t>(snapshot->size())
                : 0u;
        }

        void DisableAndDrainAll()
        {
            std::vector<std::shared_ptr<Entry>> entries;
            {
                constexpr auto kRegistryLockTimeout =
                    std::chrono::milliseconds(500);
                std::unique_lock<std::timed_mutex> lock(
                    g_registryMutex,
                    std::defer_lock);
                if (!lock.try_lock_for(kRegistryLockTimeout)) {
                    // Do not inspect or mutate g_entries without the lock. At
                    // process exit, retaining this process-lifetime registry is
                    // safer than racing a surviving callback, and (critically)
                    // it cannot leave Fallout4VR.exe stuck as a zombie.
                    spdlog::warn(
                        "[OpenVRHook] Process-exit registry lock timed out after "
                        "{} ms; preserving callback registry as process-lifetime state",
                        kRegistryLockTimeout.count());
                    return;
                }
                entries.swap(g_entries);
                for (const auto& entry : entries) {
                    entry->enabled.store(false, std::memory_order_release);
                }
                PublishEntrySnapshotLocked();
            }
            for (const auto& entry : entries) {
                if (g_currentHandle == entry->handle) {
                    continue;
                }
                std::unique_lock<std::mutex> lock(entry->drainMutex);
                if (!entry->drainCondition.wait_for(
                        lock,
                        std::chrono::milliseconds(500),
                        [&entry] {
                            return entry->inFlight.load(std::memory_order_acquire) == 0;
                        })) {
                    spdlog::warn(
                        "[OpenVRHook] Process-exit drain timed out after 500 ms for "
                        "owner='{}', handle={}, inFlight={}; proceeding with process-lifetime state",
                        entry->ownerName,
                        entry->handle,
                        entry->inFlight.load(std::memory_order_acquire));
                }
            }
        }
    }
    
    // FO4VRTools callback that bridges to our callback system
    static bool FO4VRToolsControllerStateCallback(vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
                                                   const vr::VRControllerState_t *pControllerState, 
                                                   uint32_t unControllerStateSize, 
                                                   vr::VRControllerState_t* pOutputControllerState)
    {
        if (!pControllerState || !pOutputControllerState) {
            return true;  // Continue processing
        }
        
        // Copy input to output first
        *pOutputControllerState = *pControllerState;
        
        auto left = g_leftControllerIndex.load(std::memory_order_acquire);
        auto right = g_rightControllerIndex.load(std::memory_order_acquire);
        auto* fo4vrToolsAPI = g_fo4vrToolsAPI.load(std::memory_order_acquire);
        if (fo4vrToolsAPI &&
            (left == vr::k_unTrackedDeviceIndexInvalid ||
             right == vr::k_unTrackedDeviceIndexInvalid ||
             (unControllerDeviceIndex != left && unControllerDeviceIndex != right))) {
            vr::IVRSystem* vrSystem = fo4vrToolsAPI->GetVRSystem();
            if (vrSystem) {
                left = vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
                right = vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
                g_leftControllerIndex.store(left, std::memory_order_release);
                g_rightControllerIndex.store(right, std::memory_order_release);
                OpenVRHook::GetSingleton().UpdateControllerIndices(vrSystem);
            }
        }

        controller_bridge::ControllerRole role = controller_bridge::kRoleUnknown;
        if (unControllerDeviceIndex == left) {
            role = controller_bridge::kRoleLeft;
        } else if (unControllerDeviceIndex == right) {
            role = controller_bridge::kRoleRight;
        }

        // FO4VRTools is a fallback only. Once the vtable path is active, that path
        // already applied the pipeline and running it here would double-advance
        // edge/hold state.
        auto& hook = OpenVRHook::GetSingleton();
        if (role != controller_bridge::kRoleUnknown && !hook.IsVtableHooked()) {
            hook.ApplyCallbacksToState(
                role, pOutputControllerState, controller_bridge::kConsumerGame);
        }

        return true;  // Continue processing
    }
    
    // Try to get FO4VRTools API
    static OpenVRHookManagerAPI* RequestFO4VRToolsAPI()
    {
        typedef OpenVRHookManagerAPI* (*GetVRHookMgrFuncPtr_t)();
        
        // Check if FO4VRTools is loaded (use GetModuleHandle, not LoadLibrary, to avoid loading it)
        HMODULE fo4vrToolsModule = GetModuleHandleA("FO4VRTools.dll");
        if (fo4vrToolsModule != nullptr) {
            GetVRHookMgrFuncPtr_t vrHookGetFunc = (GetVRHookMgrFuncPtr_t)GetProcAddress(fo4vrToolsModule, "GetVRHookManager");
            if (vrHookGetFunc) {
                OpenVRHookManagerAPI* api = vrHookGetFunc();
                if (api && api->IsInitialized()) {
                    spdlog::info("[OpenVRHook] FO4VRTools detected and initialized - using their API for compatibility");
                    return api;
                }
            }
        }
        
        return nullptr;
    }

    // ============================================================
    // VRSystemWrapper Implementation
    // ============================================================

    VRSystemWrapper::VRSystemWrapper(vr::IVRSystem* realSystem)
        : m_realSystem(realSystem)
    {
        spdlog::info("[OpenVRHook] VRSystemWrapper created, wrapping real IVRSystem at {:p}", (void*)realSystem);
        UpdateControllerIndices();
    }

    void VRSystemWrapper::RegisterControllerStateCallback(ControllerStateCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callbacks.push_back(callback);
        spdlog::info("[OpenVRHook] Registered controller state callback, total: {}", m_callbacks.size());
    }

    void VRSystemWrapper::ClearCallbacks()
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callbacks.clear();
    }
    
    bool VRSystemWrapper::GetControllerStateRaw(vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
                                                 vr::VRControllerState_t* pControllerState, 
                                                 uint32_t unControllerStateSize)
    {
        // Get the REAL state without applying any callbacks
        return m_realSystem->GetControllerState(unControllerDeviceIndex, pControllerState, unControllerStateSize);
    }

    void VRSystemWrapper::UpdateControllerIndices()
    {
        m_leftControllerIndex = m_realSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        m_rightControllerIndex = m_realSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    }

    controller_bridge::ControllerRole VRSystemWrapper::GetControllerRole(vr::TrackedDeviceIndex_t index)
    {
        auto left = m_leftControllerIndex.load(std::memory_order_acquire);
        auto right = m_rightControllerIndex.load(std::memory_order_acquire);
        if (left == vr::k_unTrackedDeviceIndexInvalid ||
            right == vr::k_unTrackedDeviceIndexInvalid ||
            (index != left && index != right)) {
            UpdateControllerIndices();
            left = m_leftControllerIndex.load(std::memory_order_acquire);
            right = m_rightControllerIndex.load(std::memory_order_acquire);
        }
        if (index == left) {
            return controller_bridge::kRoleLeft;
        }
        if (index == right) {
            return controller_bridge::kRoleRight;
        }
        return controller_bridge::kRoleUnknown;
    }

    // ============================================================
    // GetControllerState - THE KEY INTERCEPTION POINT
    // ============================================================

    // Defined below near the vtable hooks (audit rank 1 — per-consumer input routing).
    static bool IsCallerInOwnModule(const void* returnAddress);

    bool VRSystemWrapper::GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                              vr::VRControllerState_t* pControllerState,
                                              uint32_t unControllerStateSize)
    {
        const void* callerRa = _ReturnAddress();
        // Get real state first
        bool result = m_realSystem->GetControllerState(unControllerDeviceIndex, pControllerState, unControllerStateSize);

        if (result && pControllerState && !IsCallerInOwnModule(callerRa)) {
            // Apply the SHARED OpenVRHook callback list (audit rank 1 fix). In IAT-patch mode
            // the game reads through THIS wrapper, which forwards to the (separately) patched
            // real vtable — whose Hooked_ handler now sees the wrapper's own-module return
            // address and skips filtering to avoid double-application. So the wrapper is the
            // single point that must apply the game-facing callbacks (A+Grip block, A/X
            // hold-to-grab, sticky-grab, holotape/throwable strips). The wrapper's own
            // the wrapper callback list is empty whenever the vtable hook succeeded (registration is skipped
            // then), and populated only in the vtable-hook-failed fallback — but every callback
            // is ALSO in OpenVRHook's published callback snapshot, so the shared list is correct in both cases.
            const auto role = GetControllerRole(unControllerDeviceIndex);
            if (role != controller_bridge::kRoleUnknown) {
                OpenVRHook::GetSingleton().ApplyCallbacksToState(
                    role, pControllerState, controller_bridge::kConsumerGame);
            }
        }

        return result;
    }

    bool VRSystemWrapper::GetControllerStateWithPose(vr::ETrackingUniverseOrigin eOrigin,
                                                      vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                                      vr::VRControllerState_t* pControllerState,
                                                      uint32_t unControllerStateSize,
                                                      vr::TrackedDevicePose_t* pTrackedDevicePose)
    {
        const void* callerRa = _ReturnAddress();
        // Get real state first
        bool result = m_realSystem->GetControllerStateWithPose(eOrigin, unControllerDeviceIndex,
                                                                pControllerState, unControllerStateSize,
                                                                pTrackedDevicePose);

        if (result && pControllerState && !IsCallerInOwnModule(callerRa)) {
            // Apply the SHARED OpenVRHook callback list (audit rank 1 fix — see the
            // GetControllerState twin above for the full rationale).
            const auto role = GetControllerRole(unControllerDeviceIndex);
            if (role != controller_bridge::kRoleUnknown) {
                OpenVRHook::GetSingleton().ApplyCallbacksToState(
                    role, pControllerState, controller_bridge::kConsumerGame);
            }
        }

        return result;
    }

    // ============================================================
    // All other IVRSystem methods - simple forwarding
    // ============================================================

    void VRSystemWrapper::GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) {
        m_realSystem->GetRecommendedRenderTargetSize(pnWidth, pnHeight);
    }

    vr::HmdMatrix44_t VRSystemWrapper::GetProjectionMatrix(vr::EVREye eEye, float fNearZ, float fFarZ) {
        return m_realSystem->GetProjectionMatrix(eEye, fNearZ, fFarZ);
    }

    void VRSystemWrapper::GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) {
        m_realSystem->GetProjectionRaw(eEye, pfLeft, pfRight, pfTop, pfBottom);
    }

    bool VRSystemWrapper::ComputeDistortion(vr::EVREye eEye, float fU, float fV, vr::DistortionCoordinates_t* pDistortionCoordinates) {
        return m_realSystem->ComputeDistortion(eEye, fU, fV, pDistortionCoordinates);
    }

    vr::HmdMatrix34_t VRSystemWrapper::GetEyeToHeadTransform(vr::EVREye eEye) {
        return m_realSystem->GetEyeToHeadTransform(eEye);
    }

    bool VRSystemWrapper::GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) {
        return m_realSystem->GetTimeSinceLastVsync(pfSecondsSinceLastVsync, pulFrameCounter);
    }

    int32_t VRSystemWrapper::GetD3D9AdapterIndex() {
        return m_realSystem->GetD3D9AdapterIndex();
    }

    void VRSystemWrapper::GetDXGIOutputInfo(int32_t* pnAdapterIndex) {
        m_realSystem->GetDXGIOutputInfo(pnAdapterIndex);
    }

    void VRSystemWrapper::GetOutputDevice(uint64_t* pnDevice, vr::ETextureType textureType, VkInstance_T* pInstance) {
        m_realSystem->GetOutputDevice(pnDevice, textureType, pInstance);
    }

    bool VRSystemWrapper::IsDisplayOnDesktop() {
        return m_realSystem->IsDisplayOnDesktop();
    }

    bool VRSystemWrapper::SetDisplayVisibility(bool bIsVisibleOnDesktop) {
        return m_realSystem->SetDisplayVisibility(bIsVisibleOnDesktop);
    }

    void VRSystemWrapper::GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, vr::TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) {
        m_realSystem->GetDeviceToAbsoluteTrackingPose(eOrigin, fPredictedSecondsToPhotonsFromNow, pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount);
    }

    void VRSystemWrapper::ResetSeatedZeroPose() {
        m_realSystem->ResetSeatedZeroPose();
    }

    vr::HmdMatrix34_t VRSystemWrapper::GetSeatedZeroPoseToStandingAbsoluteTrackingPose() {
        return m_realSystem->GetSeatedZeroPoseToStandingAbsoluteTrackingPose();
    }

    vr::HmdMatrix34_t VRSystemWrapper::GetRawZeroPoseToStandingAbsoluteTrackingPose() {
        return m_realSystem->GetRawZeroPoseToStandingAbsoluteTrackingPose();
    }

    uint32_t VRSystemWrapper::GetSortedTrackedDeviceIndicesOfClass(vr::ETrackedDeviceClass eTrackedDeviceClass, vr::TrackedDeviceIndex_t* punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex) {
        return m_realSystem->GetSortedTrackedDeviceIndicesOfClass(eTrackedDeviceClass, punTrackedDeviceIndexArray, unTrackedDeviceIndexArrayCount, unRelativeToTrackedDeviceIndex);
    }

    vr::EDeviceActivityLevel VRSystemWrapper::GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) {
        return m_realSystem->GetTrackedDeviceActivityLevel(unDeviceId);
    }

    void VRSystemWrapper::ApplyTransform(vr::TrackedDevicePose_t* pOutputPose, const vr::TrackedDevicePose_t* pTrackedDevicePose, const vr::HmdMatrix34_t* pTransform) {
        m_realSystem->ApplyTransform(pOutputPose, pTrackedDevicePose, pTransform);
    }

    vr::TrackedDeviceIndex_t VRSystemWrapper::GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) {
        return m_realSystem->GetTrackedDeviceIndexForControllerRole(unDeviceType);
    }

    vr::ETrackedControllerRole VRSystemWrapper::GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex) {
        return m_realSystem->GetControllerRoleForTrackedDeviceIndex(unDeviceIndex);
    }

    vr::ETrackedDeviceClass VRSystemWrapper::GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) {
        return m_realSystem->GetTrackedDeviceClass(unDeviceIndex);
    }

    bool VRSystemWrapper::IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) {
        return m_realSystem->IsTrackedDeviceConnected(unDeviceIndex);
    }

    bool VRSystemWrapper::GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetBoolTrackedDeviceProperty(unDeviceIndex, prop, pError);
    }

    float VRSystemWrapper::GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetFloatTrackedDeviceProperty(unDeviceIndex, prop, pError);
    }

    int32_t VRSystemWrapper::GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetInt32TrackedDeviceProperty(unDeviceIndex, prop, pError);
    }

    uint64_t VRSystemWrapper::GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetUint64TrackedDeviceProperty(unDeviceIndex, prop, pError);
    }

    vr::HmdMatrix34_t VRSystemWrapper::GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetMatrix34TrackedDeviceProperty(unDeviceIndex, prop, pError);
    }

    uint32_t VRSystemWrapper::GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::PropertyTypeTag_t propType, void* pBuffer, uint32_t unBufferSize, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetArrayTrackedDeviceProperty(unDeviceIndex, prop, propType, pBuffer, unBufferSize, pError);
    }

    uint32_t VRSystemWrapper::GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pError) {
        return m_realSystem->GetStringTrackedDeviceProperty(unDeviceIndex, prop, pchValue, unBufferSize, pError);
    }

    const char* VRSystemWrapper::GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) {
        return m_realSystem->GetPropErrorNameFromEnum(error);
    }

    bool VRSystemWrapper::PollNextEvent(vr::VREvent_t* pEvent, uint32_t uncbVREvent) {
        return m_realSystem->PollNextEvent(pEvent, uncbVREvent);
    }

    bool VRSystemWrapper::PollNextEventWithPose(vr::ETrackingUniverseOrigin eOrigin, vr::VREvent_t* pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t* pTrackedDevicePose) {
        return m_realSystem->PollNextEventWithPose(eOrigin, pEvent, uncbVREvent, pTrackedDevicePose);
    }

    const char* VRSystemWrapper::GetEventTypeNameFromEnum(vr::EVREventType eType) {
        return m_realSystem->GetEventTypeNameFromEnum(eType);
    }

    vr::HiddenAreaMesh_t VRSystemWrapper::GetHiddenAreaMesh(vr::EVREye eEye, vr::EHiddenAreaMeshType type) {
        return m_realSystem->GetHiddenAreaMesh(eEye, type);
    }

    void VRSystemWrapper::TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) {
        m_realSystem->TriggerHapticPulse(unControllerDeviceIndex, unAxisId, usDurationMicroSec);
    }

    const char* VRSystemWrapper::GetButtonIdNameFromEnum(vr::EVRButtonId eButtonId) {
        return m_realSystem->GetButtonIdNameFromEnum(eButtonId);
    }

    const char* VRSystemWrapper::GetControllerAxisTypeNameFromEnum(vr::EVRControllerAxisType eAxisType) {
        return m_realSystem->GetControllerAxisTypeNameFromEnum(eAxisType);
    }

    bool VRSystemWrapper::IsInputAvailable() {
        return m_realSystem->IsInputAvailable();
    }

    bool VRSystemWrapper::IsSteamVRDrawingControllers() {
        return m_realSystem->IsSteamVRDrawingControllers();
    }

    bool VRSystemWrapper::ShouldApplicationPause() {
        return m_realSystem->ShouldApplicationPause();
    }

    bool VRSystemWrapper::ShouldApplicationReduceRenderingWork() {
        return m_realSystem->ShouldApplicationReduceRenderingWork();
    }

    vr::EVRFirmwareError VRSystemWrapper::PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex) {
        return m_realSystem->PerformFirmwareUpdate(unDeviceIndex);
    }

    uint32_t VRSystemWrapper::DriverDebugRequest(vr::TrackedDeviceIndex_t unDeviceIndex, const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) {
        return m_realSystem->DriverDebugRequest(unDeviceIndex, pchRequest, pchResponseBuffer, unResponseBufferSize);
    }

    void VRSystemWrapper::AcknowledgeQuit_Exiting() {
        m_realSystem->AcknowledgeQuit_Exiting();
    }

    void VRSystemWrapper::AcknowledgeQuit_UserPrompt() {
        m_realSystem->AcknowledgeQuit_UserPrompt();
    }

    // ============================================================
    // OpenVRHook Implementation
    // ============================================================

    OpenVRHook& OpenVRHook::GetSingleton()
    {
        static OpenVRHook instance;
        return instance;
    }

    // Hooked VR_GetGenericInterface - returns our wrapper for IVRSystem
    void* OpenVRHook::HookedVR_GetGenericInterface(const char* pchInterfaceVersion, vr::EVRInitError* peError)
    {
        // Call original function
        using VR_GetGenericInterface_t = void* (*)(const char*, vr::EVRInitError*);
        auto originalFunc = reinterpret_cast<VR_GetGenericInterface_t>(s_originalGetGenericInterface);
        if (!originalFunc) {
            if (peError) {
                *peError = vr::VRInitError_Init_InterfaceNotFound;
            }
            return nullptr;
        }
        void* result = originalFunc(pchInterfaceVersion, peError);

        if (result && pchInterfaceVersion) {
            // The hard-coded vtable indices below are valid only for IVRSystem_019.
            if (std::strcmp(pchInterfaceVersion, "IVRSystem_019") == 0) {
                spdlog::info("[OpenVRHook] Intercepted IVRSystem request: {}", pchInterfaceVersion);
                
                auto& hook = GetSingleton();
                vr::IVRSystem* realSystem = static_cast<vr::IVRSystem*>(result);
                
                // CRITICAL: Hook the REAL IVRSystem's vtable
                // This catches calls that bypass our wrapper (e.g., Virtual Holsters, game's cached pointer)
                hook.HookRealVRSystemVtable(realSystem);
                
                if (!hook.m_vrSystemWrapper) {
                    // Create wrapper around the real IVRSystem
                    hook.m_vrSystemWrapper = std::make_unique<VRSystemWrapper>(realSystem);
                    spdlog::info("[OpenVRHook] Created VRSystemWrapper");
                }
                
                // Return our wrapper instead of the real system
                return hook.m_vrSystemWrapper.get();
            }
        }

        return result;
    }

    bool OpenVRHook::PatchIAT()
    {
        // Get the game's main module
        HMODULE gameModule = GetModuleHandle(nullptr);
        if (!gameModule) {
            spdlog::error("[OpenVRHook] Failed to get game module handle");
            return false;
        }

        // Get openvr_api.dll module
        HMODULE openvrModule = GetModuleHandleA("openvr_api.dll");
        if (!openvrModule) {
            spdlog::error("[OpenVRHook] openvr_api.dll not loaded yet");
            return false;
        }

        // Verify that the export exists, but chain through the function currently in
        // Fallout's IAT slot. It may already be another mod's compatible hook.
        if (!GetProcAddress(openvrModule, "VR_GetGenericInterface")) {
            spdlog::error("[OpenVRHook] Failed to find VR_GetGenericInterface in openvr_api.dll");
            return false;
        }

        // Parse the PE headers to find the IAT
        PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(gameModule);
        PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
            reinterpret_cast<BYTE*>(gameModule) + dosHeader->e_lfanew);

        // Get the import directory
        PIMAGE_IMPORT_DESCRIPTOR importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
            reinterpret_cast<BYTE*>(gameModule) + 
            ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

        // Iterate through imported DLLs
        while (importDesc->Name) {
            const char* dllName = reinterpret_cast<const char*>(
                reinterpret_cast<BYTE*>(gameModule) + importDesc->Name);

            // Check if this is openvr_api.dll
            if (_stricmp(dllName, "openvr_api.dll") == 0) {
                spdlog::info("[OpenVRHook] Found openvr_api.dll in imports");

                // Get the thunk data (IAT entries)
                PIMAGE_THUNK_DATA origThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
                    reinterpret_cast<BYTE*>(gameModule) + importDesc->OriginalFirstThunk);
                PIMAGE_THUNK_DATA thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
                    reinterpret_cast<BYTE*>(gameModule) + importDesc->FirstThunk);

                // Iterate through functions
                while (origThunk->u1.AddressOfData) {
                    // Check if imported by name (not ordinal)
                    if (!(origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                        PIMAGE_IMPORT_BY_NAME importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                            reinterpret_cast<BYTE*>(gameModule) + origThunk->u1.AddressOfData);

                        // Check if this is VR_GetGenericInterface
                        if (strcmp(importByName->Name, "VR_GetGenericInterface") == 0) {
                            spdlog::info("[OpenVRHook] Found VR_GetGenericInterface in IAT");

                            m_iatSlot = reinterpret_cast<void**>(&thunk->u1.Function);
                            m_originalIATEntry = *m_iatSlot;
                            if (m_originalIATEntry == reinterpret_cast<void*>(&HookedVR_GetGenericInterface)) {
                                m_iatHooked.store(true, std::memory_order_release);
                                m_isHooked.store(true, std::memory_order_release);
                                return true;
                            }
                            s_originalGetGenericInterface = m_originalIATEntry;

                            // Make the page writable
                            DWORD oldProtect;
                            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                spdlog::error("[OpenVRHook] VirtualProtect failed: {}", GetLastError());
                                return false;
                            }

                            // Replace with our hook
                            *m_iatSlot = reinterpret_cast<void*>(&HookedVR_GetGenericInterface);

                            // Restore protection
                            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);

                            spdlog::info("[OpenVRHook] IAT patch successful!");
                            m_iatHooked.store(true, std::memory_order_release);
                            m_isHooked.store(true, std::memory_order_release);
                            return true;
                        }
                    }

                    origThunk++;
                    thunk++;
                }
            }

            importDesc++;
        }

        spdlog::error("[OpenVRHook] VR_GetGenericInterface not found in game's IAT");
        return false;
    }

    bool OpenVRHook::RestoreIAT()
    {
        if (!m_iatHooked.load(std::memory_order_acquire) || !m_originalIATEntry) {
            return true;
        }
        if (!m_iatSlot) {
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(m_iatSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            spdlog::error("[OpenVRHook] Failed to make IAT writable during shutdown: {}", GetLastError());
            return false;
        }

        const bool owned =
            *m_iatSlot ==
            reinterpret_cast<void*>(&HookedVR_GetGenericInterface);
        if (owned) {
            *m_iatSlot = m_originalIATEntry;
        } else {
            spdlog::warn("[OpenVRHook] IAT ownership changed; preserving the newer hook");
        }
        DWORD ignored = 0;
        const bool protectionRestored =
            VirtualProtect(
                m_iatSlot,
                sizeof(void*),
                oldProtect,
                &ignored) != FALSE;
        if (!protectionRestored) {
            spdlog::error(
                "[OpenVRHook] Failed to restore IAT page protection during "
                "shutdown: {}",
                GetLastError());
        }

        if (owned) {
            m_iatHooked.store(false, std::memory_order_release);
        }
        // Keep the slot and predecessor pointers for process life. A newer
        // hook may have captured our thunk, and a thread can also enter a
        // thunk after fetching the old IAT entry immediately before restore.
        return owned && protectionRestored;
    }

    bool OpenVRHook::Initialize()
    {
        spdlog::info("[OpenVRHook] Initializing OpenVR hook...");

        if (m_isHooked.load(std::memory_order_acquire)) {
            spdlog::warn("[OpenVRHook] Already hooked");
            return true;
        }
        m_shuttingDown.store(false, std::memory_order_release);

        HMODULE fo4vrToolsModule = GetModuleHandleA("FO4VRTools.dll");
        if (fo4vrToolsModule != nullptr) {
            m_usingFO4VRTools.store(true, std::memory_order_release);
            spdlog::info("[OpenVRHook] FO4VRTools.dll detected; probing backend health");
            if (TryActivateFO4VRToolsBackend()) {
                m_isHooked.store(true, std::memory_order_release);
                return true;
            }
            spdlog::warn(
                "[OpenVRHook] FO4VRTools is present but not operational yet; installing chained IAT fallback");
        }
        
        if (!PatchIAT()) {
            spdlog::error("[OpenVRHook] Failed to patch IAT");
            return false;
        }

        spdlog::info("[OpenVRHook] OpenVR hook initialized successfully");
        return true;
    }

    void OpenVRHook::Shutdown()
    {
        spdlog::info("[OpenVRHook] Shutting down OpenVR hook");
        m_shuttingDown.store(true, std::memory_order_release);
        bridge_detail::DisableAndDrainAll();
        
        auto* fo4vrToolsAPI = g_fo4vrToolsAPI.load(std::memory_order_acquire);
        if (m_fo4vrToolsCallbackRegistered.exchange(false, std::memory_order_acq_rel) &&
            fo4vrToolsAPI) {
            fo4vrToolsAPI->UnregisterControllerStateCB(FO4VRToolsControllerStateCallback);
            spdlog::info("[OpenVRHook] Unregistered from FO4VRTools");
        }
        RestoreIAT();
        
        UnhookRealVRSystemVtable();
        WaitForHookCallsToDrain();
        
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_callbackSnapshot.store(
                std::shared_ptr<const ControllerStateCallbackList>{},
                std::memory_order_release);
        }

        // The game or another plugin may have cached the wrapper returned by
        // HookedVR_GetGenericInterface. Keep it as a disabled forwarding proxy
        // for process life instead of creating a shutdown-time use-after-free.
        m_isHooked.store(false, std::memory_order_release);
    }
    
    bool OpenVRHook::TryActivateFO4VRToolsBackend()
    {
        if (m_shuttingDown.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> apiLock(g_fo4vrToolsApiMutex);
        auto* fo4vrToolsAPI = g_fo4vrToolsAPI.load(std::memory_order_acquire);
        if (!fo4vrToolsAPI || !fo4vrToolsAPI->IsInitialized()) {
            fo4vrToolsAPI = RequestFO4VRToolsAPI();
            g_fo4vrToolsAPI.store(fo4vrToolsAPI, std::memory_order_release);
        }
        if (!fo4vrToolsAPI || !fo4vrToolsAPI->IsInitialized()) {
            return false;
        }

        m_usingFO4VRTools.store(true, std::memory_order_release);
        if (!m_fo4vrToolsCallbackRegistered.exchange(true, std::memory_order_acq_rel)) {
            fo4vrToolsAPI->RegisterControllerStateCB(FO4VRToolsControllerStateCallback);
            spdlog::info("[OpenVRHook] Registered FO4VRTools fallback callback");
        }

        if (auto* realSystem = fo4vrToolsAPI->GetVRSystem()) {
            HookRealVRSystemVtable(realSystem);
        }
        const bool operational =
            m_vtableHooked.load(std::memory_order_acquire) ||
            m_fo4vrToolsCallbackRegistered.load(std::memory_order_acquire);
        if (operational) {
            m_isHooked.store(true, std::memory_order_release);
        }
        return operational;
    }
    
    vr::IVRSystem* OpenVRHook::GetRealVRSystem() const
    {
        if (m_usingFO4VRTools.load(std::memory_order_acquire) ||
            GetModuleHandleA("FO4VRTools.dll")) {
            auto* self = const_cast<OpenVRHook*>(this);
            self->m_usingFO4VRTools.store(true, std::memory_order_release);
            self->TryActivateFO4VRToolsBackend();
            std::lock_guard<std::mutex> apiLock(g_fo4vrToolsApiMutex);
            auto* fo4vrToolsAPI =
                g_fo4vrToolsAPI.load(std::memory_order_acquire);
            if (fo4vrToolsAPI && fo4vrToolsAPI->IsInitialized()) {
                return fo4vrToolsAPI->GetVRSystem();
            }
        }
        return m_realVRSystem ? m_realVRSystem :
            (m_vrSystemWrapper ? m_vrSystemWrapper->GetRealSystem() : nullptr);
    }
    
    bool OpenVRHook::GetControllerStateUnfiltered(vr::TrackedDeviceIndex_t deviceIndex,
                                                   vr::VRControllerState_t* pControllerState,
                                                   uint32_t unControllerStateSize)
    {
        // Call the ORIGINAL GetControllerState (before our vtable hook)
        if (m_originalGetControllerState && m_realVRSystem) {
            return m_originalGetControllerState(m_realVRSystem, deviceIndex, pControllerState, unControllerStateSize);
        }
        
        // Fallback: use wrapper's raw method if available
        if (m_vrSystemWrapper) {
            return m_vrSystemWrapper->GetControllerStateRaw(deviceIndex, pControllerState, unControllerStateSize);
        }
        
        return false;
    }

    void OpenVRHook::RegisterControllerStateCallback(ControllerStateCallback callback)
    {
        if (!callback || m_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        // Always add to our internal callback list (used by vtable hook)
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            const auto current =
                m_callbackSnapshot.load(std::memory_order_acquire);
            auto next =
                current
                    ? std::make_shared<ControllerStateCallbackList>(*current)
                    : std::make_shared<ControllerStateCallbackList>();
            next->push_back(callback);
            const auto callbackCount = next->size();
            std::shared_ptr<const ControllerStateCallbackList> published =
                std::move(next);
            m_callbackSnapshot.store(
                std::move(published),
                std::memory_order_release);
            spdlog::info(
                "[OpenVRHook] Registered controller state callback, total: {}",
                callbackCount);
        }
        
        // Register in secondary callback lists ONLY if vtable hook is NOT active.
        // When vtable is hooked, ApplyCallbacksToState (using the published callback snapshot) already runs
        // on every GetControllerState call. Adding to the bridge/wrapper list too would
        // cause double-execution: vtable hook modifies state (strips A, injects grip),
        // then bridge/wrapper applies the same callback on the modified state, seeing
        // stripped A as "released" and corrupting hold-timing logic.
        if (m_usingFO4VRTools.load(std::memory_order_acquire) ||
            GetModuleHandleA("FO4VRTools.dll")) {
            m_usingFO4VRTools.store(true, std::memory_order_release);
            // Retry every time another consumer registers. FO4VRTools may have been
            // loaded at process start but initialized only after F4SE plugin load.
            TryActivateFO4VRToolsBackend();
        } else if (m_vrSystemWrapper) {
            // Wrapper mode - vtable hook handles everything, no need for wrapper callbacks
            if (!m_vtableHooked.load(std::memory_order_acquire)) {
                m_vrSystemWrapper->RegisterControllerStateCallback(callback);
            } else {
                spdlog::info("[OpenVRHook] Vtable hooked - skipping wrapper callback (prevents double-execution)");
            }
        }
    }
    
    void OpenVRHook::ApplyCallbacksToState(
        controller_bridge::ControllerRole role,
        vr::VRControllerState_t* state,
        std::uint32_t consumer)
    {
        if (!state || role == controller_bridge::kRoleUnknown ||
            m_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }

        const auto deviceIndex = role == controller_bridge::kRoleLeft
            ? m_leftControllerIndex.load(std::memory_order_acquire)
            : m_rightControllerIndex.load(std::memory_order_acquire);
        vr::VRControllerState_t heisenbergState = *state;

        // External remappers run first for game input, so virtual Grip/button
        // outputs can be directed either to Fallout (`state`) or exclusively to
        // Heisenberg (`heisenbergState`).
        bridge_detail::Dispatch(
            role,
            deviceIndex,
            state,
            &heisenbergState,
            controller_bridge::kPreHeisenberg,
            consumer,
            nullptr);

        // This hook is polled for both controllers every rendered frame. The
        // former copy-under-mutex path allocated a vector and contended on that
        // mutex for every poll even though registration happens only at init.
        // Writers publish an immutable copy; readers acquire one shared
        // snapshot with no heap allocation or callback-list mutex.
        const auto callbacks =
            m_callbackSnapshot.load(std::memory_order_acquire);
        if (callbacks) {
            for (const auto& callback : *callbacks) {
                const auto before = heisenbergState;
                uint64_t mask = ~std::uint64_t{0};
                try {
                    mask = callback(
                        role == controller_bridge::kRoleLeft, &heisenbergState);
                } catch (...) {
                    // Never unwind a controller filter through OpenVR or the
                    // game executable.
                    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
                    if (!logged.test_and_set(std::memory_order_relaxed)) {
                        spdlog::error(
                            "[OpenVRHook] Internal controller-state callback threw; "
                            "allowing samples unchanged (further reports suppressed)");
                    }
                    heisenbergState = before;
                }
                heisenbergState.ulButtonPressed &= mask;
                heisenbergState.ulButtonTouched &= mask;

                // Merge only changes made by Heisenberg itself. Bits injected solely
                // into the private interaction state by a bridge callback remain
                // private when Heisenberg leaves them unchanged.
                const auto pressedChanges =
                    before.ulButtonPressed ^ heisenbergState.ulButtonPressed;
                const auto touchedChanges =
                    before.ulButtonTouched ^ heisenbergState.ulButtonTouched;
                state->ulButtonPressed =
                    (state->ulButtonPressed & ~pressedChanges) |
                    (heisenbergState.ulButtonPressed & pressedChanges);
                state->ulButtonTouched =
                    (state->ulButtonTouched & ~touchedChanges) |
                    (heisenbergState.ulButtonTouched & touchedChanges);

                for (std::size_t axis = 0; axis < std::size(state->rAxis); ++axis) {
                    if (before.rAxis[axis].x != heisenbergState.rAxis[axis].x ||
                        before.rAxis[axis].y != heisenbergState.rAxis[axis].y) {
                        state->rAxis[axis] = heisenbergState.rAxis[axis];
                    }
                }
                state->ulButtonPressed &= mask;
                state->ulButtonTouched &= mask;
            }
        }

        // THUMB-TWITCH FIX (Jul 19): see the FO4VRTools-path strip above — same rationale.
        if (m_suppressThumbstickTouch.load(std::memory_order_acquire)) {
            state->ulButtonTouched &= ~vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
        }

        bridge_detail::Dispatch(
            role,
            deviceIndex,
            state,
            &heisenbergState,
            controller_bridge::kPostHeisenberg,
            consumer,
            nullptr);
    }

    bool OpenVRHook::ApplyInteractionCallbacks(
        controller_bridge::ControllerRole role,
        vr::TrackedDeviceIndex_t deviceIndex,
        const vr::VRControllerState_t& rawState,
        vr::VRControllerState_t& interactionState)
    {
        interactionState = rawState;
        if (role == controller_bridge::kRoleUnknown ||
            m_shuttingDown.load(std::memory_order_acquire)) {
            return false;
        }

        auto gameFacingCopy = rawState;
        std::uint64_t resultFlags =
            controller_bridge::kResultNone;
        bridge_detail::Dispatch(
            role,
            deviceIndex,
            &gameFacingCopy,
            &interactionState,
            controller_bridge::kPreHeisenberg,
            controller_bridge::kConsumerHeisenberg,
            &resultFlags);
        bridge_detail::Dispatch(
            role,
            deviceIndex,
            &gameFacingCopy,
            &interactionState,
            controller_bridge::kPostHeisenberg,
            controller_bridge::kConsumerHeisenberg,
            &resultFlags);
        return (resultFlags &
                controller_bridge::kResultOwnsHeisenbergGrip) != 0;
    }
    
    void OpenVRHook::UpdateControllerIndices(vr::IVRSystem* vrSystem)
    {
        if (!vrSystem) return;
        const auto left =
            vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        const auto right =
            vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        m_leftControllerIndex.store(left, std::memory_order_release);
        m_rightControllerIndex.store(right, std::memory_order_release);
        g_leftControllerIndex.store(left, std::memory_order_release);
        g_rightControllerIndex.store(right, std::memory_order_release);
    }
    
    controller_bridge::ControllerRole OpenVRHook::GetControllerRole(
        vr::TrackedDeviceIndex_t deviceIndex) const
    {
        const auto left = m_leftControllerIndex.load(std::memory_order_acquire);
        const auto right = m_rightControllerIndex.load(std::memory_order_acquire);
        if (deviceIndex == left) {
            return controller_bridge::kRoleLeft;
        }
        if (deviceIndex == right) {
            return controller_bridge::kRoleRight;
        }
        return controller_bridge::kRoleUnknown;
    }

    std::uint32_t OpenVRHook::GetBackendStatusFlags() const
    {
        std::uint32_t flags = controller_bridge::kBackendNone;
        if (m_iatHooked.load(std::memory_order_acquire)) {
            flags |= controller_bridge::kBackendIATInstalled;
        }
        if (m_usingFO4VRTools.load(std::memory_order_acquire)) {
            flags |= controller_bridge::kBackendFO4VRToolsAvailable;
        }
        if (m_fo4vrToolsCallbackRegistered.load(std::memory_order_acquire)) {
            flags |= controller_bridge::kBackendFO4VRToolsCallback;
        }
        if (m_vtableHooked.load(std::memory_order_acquire)) {
            flags |= controller_bridge::kBackendVtableInstalled;
        }
        if ((flags & (controller_bridge::kBackendIATInstalled |
                      controller_bridge::kBackendFO4VRToolsCallback |
                      controller_bridge::kBackendVtableInstalled)) != 0) {
            flags |= controller_bridge::kBackendOperational;
        }
        if (m_shuttingDown.load(std::memory_order_acquire)) {
            flags |= controller_bridge::kBackendShuttingDown;
        }
        flags |= controller_bridge::kBackendInteractionStateConsumed;
        return flags;
    }

    void OpenVRHook::BeginHookCall()
    {
        m_activeHookCalls.fetch_add(1, std::memory_order_acq_rel);
    }

    void OpenVRHook::EndHookCall()
    {
        if (m_activeHookCalls.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_drainCondition.notify_all();
        }
    }

    void OpenVRHook::WaitForHookCallsToDrain()
    {
        std::unique_lock<std::mutex> lock(m_drainMutex);
        if (!m_drainCondition.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return m_activeHookCalls.load(std::memory_order_acquire) == 0;
            })) {
            spdlog::warn(
                "[OpenVRHook] Timed out waiting for {} in-flight OpenVR call(s); preserving process-lifetime state",
                m_activeHookCalls.load(std::memory_order_acquire));
        }
    }
    
    // ============================================================
    // Vtable Hooking - hooks the REAL IVRSystem's vtable
    // This intercepts GetControllerState even when mods bypass FO4VRTools
    // ============================================================
    
    // Global pointer to OpenVRHook for the hook functions
    static std::atomic<OpenVRHook*> g_openVRHookInstance{nullptr};

    // FAITHFULNESS FIX (2026-07-05 audit rank 1): per-consumer input routing. The embedded
    // ROCK engine's framework polls GetControllerState(WithPose) through this SAME patched
    // vtable, so Heisenberg's A+Grip strip (armed while a hand is grabbing / in post-drop
    // cooldown) reached ROCK's two-handed support-grip reader — standalone ROCK always reads
    // clean hardware state (its own input filter zeroes GAME-exe callers only). Exempt
    // callers inside OUR OWN module (embedded ROCK + Heisenberg itself) from the callback
    // masking; the game and third-party mods (FRIK, Virtual Holsters) keep the filtered
    // view they get today. Return-address module check, resolved once.
    static bool IsCallerInOwnModule(const void* returnAddress)
    {
        static uintptr_t s_base = 0;
        static uintptr_t s_size = 0;
        static bool s_resolved = false;
        if (!s_resolved) {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&IsCallerInOwnModule), &mod) && mod) {
                const auto base = reinterpret_cast<uintptr_t>(mod);
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE) {
                        s_base = base;
                        s_size = nt->OptionalHeader.SizeOfImage;
                    }
                }
            }
            s_resolved = true;
        }
        const auto ra = reinterpret_cast<uintptr_t>(returnAddress);
        return s_base != 0 && ra >= s_base && ra < s_base + s_size;
    }

    static std::uint32_t ClassifyConsumer(const void* returnAddress)
    {
        if (IsCallerInOwnModule(returnAddress)) {
            return controller_bridge::kConsumerHeisenberg;
        }
        HMODULE callerModule = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(returnAddress),
                &callerModule) &&
            callerModule) {
            if (callerModule == GetModuleHandle(nullptr) ||
                callerModule == GetModuleHandleA("FO4VRTools.dll")) {
                // FO4VRTools can be the immediate caller while proxying Fallout's
                // cached IVRSystem. Mods using its exposed real system still enter
                // from their own DLL and remain third-party consumers.
                return controller_bridge::kConsumerGame;
            }
        }
        return controller_bridge::kConsumerThirdParty;
    }

    static bool ReplacePointerSlot(void** slot, void* expected, void* replacement)
    {
        if (!slot || !expected || !replacement) {
            return false;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        const auto observed = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(slot), replacement, expected);
        DWORD ignored = 0;
        const bool protectionRestored =
            VirtualProtect(slot, sizeof(void*), oldProtect, &ignored) != FALSE;
        if (!protectionRestored) {
            spdlog::error("[OpenVRHook] Failed to restore vtable page protection: {}", GetLastError());
        }
        return observed == expected && protectionRestored;
    }
    
    // Our hooked GetControllerState - called instead of the real one
    bool __fastcall Hooked_GetControllerState(vr::IVRSystem* thisPtr,
                                                      vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                                      vr::VRControllerState_t* pControllerState,
                                                      uint32_t unControllerStateSize)
    {
        const void* callerRa = _ReturnAddress();
        auto* hook = g_openVRHookInstance.load(std::memory_order_acquire);
        if (!hook) {
            return false;
        }
        hook->BeginHookCall();
        const auto original = hook->m_originalGetControllerState;
        if (!original) {
            hook->EndHookCall();
            return false;
        }

        // Call the original function
        bool result = original(
            thisPtr, unControllerDeviceIndex, pControllerState, unControllerStateSize);

        if (result && pControllerState) {
            // Update controller indices if needed: either never resolved yet, or a
            // controller died mid-session (battery swap) and reconnected under a NEW
            // tracked-device index (SteamVR re-enumeration) - both cached indices stayed
            // "valid" (pointing at the OLD index), so this poll's device index matches
            // NEITHER cached slot. Without this, the real left controller (say) polls as
            // IsLeftController()==false forever after a reconnect, and ApplyCallbacksToState
            // below misroutes the A+Grip strip/inject to the wrong physical hand.
            const auto left = hook->m_leftControllerIndex.load(std::memory_order_acquire);
            const auto right = hook->m_rightControllerIndex.load(std::memory_order_acquire);
            if (left == vr::k_unTrackedDeviceIndexInvalid ||
                right == vr::k_unTrackedDeviceIndexInvalid ||
                (unControllerDeviceIndex != left && unControllerDeviceIndex != right)) {
                hook->UpdateControllerIndices(thisPtr);
            }

            const auto consumer = ClassifyConsumer(callerRa);
            const auto role = hook->GetControllerRole(unControllerDeviceIndex);
            if (consumer != controller_bridge::kConsumerHeisenberg &&
                role != controller_bridge::kRoleUnknown) {
                hook->ApplyCallbacksToState(role, pControllerState, consumer);
            }
        }

        hook->EndHookCall();
        return result;
    }

    // Our hooked GetControllerStateWithPose
    bool __fastcall Hooked_GetControllerStateWithPose(vr::IVRSystem* thisPtr,
                                                              vr::ETrackingUniverseOrigin eOrigin,
                                                              vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                                              vr::VRControllerState_t* pControllerState,
                                                              uint32_t unControllerStateSize,
                                                              vr::TrackedDevicePose_t* pTrackedDevicePose)
    {
        const void* callerRa = _ReturnAddress();
        auto* hook = g_openVRHookInstance.load(std::memory_order_acquire);
        if (!hook) {
            return false;
        }
        hook->BeginHookCall();
        const auto original = hook->m_originalGetControllerStateWithPose;
        if (!original) {
            hook->EndHookCall();
            return false;
        }

        // Call the original function
        bool result = original(
            thisPtr, eOrigin, unControllerDeviceIndex, pControllerState, unControllerStateSize, pTrackedDevicePose);
        
        if (result && pControllerState) {
            // Update controller indices if needed (see the identical comment in
            // Hooked_GetControllerState above - same mid-session reconnect fix).
            const auto left = hook->m_leftControllerIndex.load(std::memory_order_acquire);
            const auto right = hook->m_rightControllerIndex.load(std::memory_order_acquire);
            if (left == vr::k_unTrackedDeviceIndexInvalid ||
                right == vr::k_unTrackedDeviceIndexInvalid ||
                (unControllerDeviceIndex != left && unControllerDeviceIndex != right)) {
                hook->UpdateControllerIndices(thisPtr);
            }

            const auto consumer = ClassifyConsumer(callerRa);
            const auto role = hook->GetControllerRole(unControllerDeviceIndex);
            if (consumer != controller_bridge::kConsumerHeisenberg &&
                role != controller_bridge::kRoleUnknown) {
                hook->ApplyCallbacksToState(role, pControllerState, consumer);
            }
        }

        hook->EndHookCall();
        return result;
    }

    bool OpenVRHook::HookRealVRSystemVtable(vr::IVRSystem* realSystem)
    {
        if (!realSystem || m_shuttingDown.load(std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> installLock(m_installMutex);
        if (m_vtableHooked.load(std::memory_order_acquire)) {
            return m_realVRSystem == realSystem;
        }
        
        void** vtable = *reinterpret_cast<void***>(realSystem);
        if (!vtable) {
            return false;
        }

        const auto originalState =
            reinterpret_cast<GetControllerState_t>(vtable[kVtableIndex_GetControllerState]);
        const auto originalPose =
            reinterpret_cast<GetControllerStateWithPose_t>(
                vtable[kVtableIndex_GetControllerStateWithPose]);
        if (!originalState || !originalPose ||
            reinterpret_cast<void*>(originalState) == reinterpret_cast<void*>(&Hooked_GetControllerState) ||
            reinterpret_cast<void*>(originalPose) == reinterpret_cast<void*>(&Hooked_GetControllerStateWithPose)) {
            spdlog::error("[OpenVRHook] Refusing invalid or recursive IVRSystem vtable chain");
            return false;
        }

        m_realVRSystem = realSystem;
        m_originalGetControllerState = originalState;
        m_originalGetControllerStateWithPose = originalPose;
        g_openVRHookInstance.store(this, std::memory_order_release);

        void** stateSlot = &vtable[kVtableIndex_GetControllerState];
        void** poseSlot = &vtable[kVtableIndex_GetControllerStateWithPose];
        if (!ReplacePointerSlot(
                stateSlot,
                reinterpret_cast<void*>(originalState),
                reinterpret_cast<void*>(&Hooked_GetControllerState))) {
            spdlog::error("[OpenVRHook] Transaction failed while patching GetControllerState");
            g_openVRHookInstance.store(nullptr, std::memory_order_release);
            m_realVRSystem = nullptr;
            m_originalGetControllerState = nullptr;
            m_originalGetControllerStateWithPose = nullptr;
            return false;
        }
        if (!ReplacePointerSlot(
                poseSlot,
                reinterpret_cast<void*>(originalPose),
                reinterpret_cast<void*>(&Hooked_GetControllerStateWithPose))) {
            spdlog::error(
                "[OpenVRHook] Transaction failed while patching GetControllerStateWithPose; rolling back");
            if (!ReplacePointerSlot(
                    stateSlot,
                    reinterpret_cast<void*>(&Hooked_GetControllerState),
                    reinterpret_cast<void*>(originalState))) {
                // We lost ownership between the two operations. Keep the chain state
                // alive because a newer hook may now call through our function.
                spdlog::critical(
                    "[OpenVRHook] Vtable rollback lost ownership; preserving process-lifetime predecessor state");
                return false;
            }
            g_openVRHookInstance.store(nullptr, std::memory_order_release);
            m_realVRSystem = nullptr;
            m_originalGetControllerState = nullptr;
            m_originalGetControllerStateWithPose = nullptr;
            return false;
        }

        m_vtableHooked.store(true, std::memory_order_release);
        m_isHooked.store(true, std::memory_order_release);
        spdlog::info("[OpenVRHook] Hooked IVRSystem vtable for button filtering");
        
        // Update controller indices
        UpdateControllerIndices(realSystem);
        
        return true;
    }
    
    void OpenVRHook::UnhookRealVRSystemVtable()
    {
        std::lock_guard<std::mutex> installLock(m_installMutex);
        if (!m_vtableHooked.load(std::memory_order_acquire) || !m_realVRSystem) {
            return;
        }
        
        spdlog::info("[OpenVRHook] Unhooking real IVRSystem vtable");
        
        // Get the vtable pointer
        void** vtable = *reinterpret_cast<void***>(m_realVRSystem);
        
        const bool stateOwned =
            vtable[kVtableIndex_GetControllerState] == reinterpret_cast<void*>(&Hooked_GetControllerState);
        const bool poseOwned =
            vtable[kVtableIndex_GetControllerStateWithPose] ==
                reinterpret_cast<void*>(&Hooked_GetControllerStateWithPose);

        bool stateRestored = !stateOwned;
        bool poseRestored = !poseOwned;
        if (stateOwned && m_originalGetControllerState) {
            stateRestored = ReplacePointerSlot(
                &vtable[kVtableIndex_GetControllerState],
                reinterpret_cast<void*>(&Hooked_GetControllerState),
                reinterpret_cast<void*>(m_originalGetControllerState));
        }
        if (poseOwned && m_originalGetControllerStateWithPose) {
            poseRestored = ReplacePointerSlot(
                &vtable[kVtableIndex_GetControllerStateWithPose],
                reinterpret_cast<void*>(&Hooked_GetControllerStateWithPose),
                reinterpret_cast<void*>(m_originalGetControllerStateWithPose));
        }

        if (!stateOwned || !poseOwned) {
            // A newer hook owns at least one slot and may have captured our function
            // as its predecessor. Clearing the function pointers would break that
            // chain. F4SE plugins are process-lifetime modules, so leave a disabled
            // passthrough predecessor in place.
            spdlog::warn(
                "[OpenVRHook] Newer vtable owner detected; preserving process-lifetime passthrough chain");
            return;
        }
        if (!stateRestored || !poseRestored) {
            spdlog::error("[OpenVRHook] Could not safely restore every owned vtable slot");
            return;
        }

        m_vtableHooked.store(false, std::memory_order_release);
        WaitForHookCallsToDrain();
        // Keep predecessor pointers and the singleton address for the remainder of
        // the process. A call that fetched our vtable entry immediately before the
        // restore can still arrive after the drain observation; process-lifetime
        // storage lets that late call safely pass through.
        
        spdlog::info("[OpenVRHook] Vtable restored");
    }

} // namespace heisenberg

// Versioned cross-plugin controller-state broker. The legacy exports below are
// retained for Controls Config releases that predate the C ABI.
using HeisenbergExternalControllerStateCallback =
    uint64_t(*)(bool isLeft, vr::VRControllerState_t* state);
namespace
{
    bool __cdecl BridgeRegister(
        const heisenberg::controller_bridge::Registration* registration,
        std::uint64_t* handle)
    {
        if ((heisenberg::OpenVRHook::GetSingleton().GetBackendStatusFlags() &
             heisenberg::controller_bridge::kBackendShuttingDown) != 0) {
            return false;
        }
        return heisenberg::bridge_detail::Register(registration, handle);
    }

    bool __cdecl BridgeUnregister(std::uint64_t handle, bool drainInFlight)
    {
        return heisenberg::bridge_detail::Unregister(handle, drainInFlight);
    }

    bool __cdecl BridgeGetBackendStatus(heisenberg::controller_bridge::BackendStatus* status)
    {
        if (!status ||
            status->structSize < sizeof(heisenberg::controller_bridge::BackendStatus)) {
            return false;
        }
        status->abiVersion = heisenberg::controller_bridge::kAbiVersion;
        status->flags = heisenberg::OpenVRHook::GetSingleton().GetBackendStatusFlags();
        status->registeredCallbacks = heisenberg::bridge_detail::Count();
        return true;
    }

    const heisenberg::controller_bridge::API g_bridgeAPI{
        sizeof(heisenberg::controller_bridge::API),
        heisenberg::controller_bridge::kAbiVersion,
        &BridgeRegister,
        &BridgeUnregister,
        &BridgeGetBackendStatus,
    };

    struct LegacyAdapter
    {
        HeisenbergExternalControllerStateCallback callback = nullptr;
    };

    struct LegacyRegistration
    {
        std::uint64_t handle = 0;
        std::unique_ptr<LegacyAdapter> adapter;
    };

    std::mutex g_legacyBridgeMutex;
    std::unordered_map<HeisenbergExternalControllerStateCallback, LegacyRegistration>
        g_legacyBridgeRegistrations;

    std::uint64_t __cdecl LegacyDispatch(
        const heisenberg::controller_bridge::CallbackContext* context,
        vr::VRControllerState_t* state,
        void* userData)
    {
        auto* adapter = static_cast<LegacyAdapter*>(userData);
        if (!context || !state || !adapter || !adapter->callback ||
            context->controllerRole == heisenberg::controller_bridge::kRoleUnknown) {
            return ~std::uint64_t{0};
        }
        const auto before = *state;
        const auto mask = adapter->callback(
            context->controllerRole == heisenberg::controller_bridge::kRoleLeft,
            state);
        // Legacy callbacks had one state view. Mirror their delta into the
        // interaction copy to preserve old behavior; versioned clients should
        // write interaction-only Grip directly to context->heisenbergState.
        if (context->heisenbergState) {
            const auto pressedChanges = before.ulButtonPressed ^ state->ulButtonPressed;
            const auto touchedChanges = before.ulButtonTouched ^ state->ulButtonTouched;
            context->heisenbergState->ulButtonPressed =
                (context->heisenbergState->ulButtonPressed & ~pressedChanges) |
                (state->ulButtonPressed & pressedChanges);
            context->heisenbergState->ulButtonTouched =
                (context->heisenbergState->ulButtonTouched & ~touchedChanges) |
                (state->ulButtonTouched & touchedChanges);
            for (std::size_t axis = 0;
                 axis < std::size(context->heisenbergState->rAxis);
                 ++axis) {
                if (before.rAxis[axis].x != state->rAxis[axis].x ||
                    before.rAxis[axis].y != state->rAxis[axis].y) {
                    context->heisenbergState->rAxis[axis] = state->rAxis[axis];
                }
            }
            context->heisenbergState->ulButtonPressed &= mask;
            context->heisenbergState->ulButtonTouched &= mask;
        }
        return mask;
    }
}

extern "C" __declspec(dllexport)
const heisenberg::controller_bridge::API* __cdecl
Heisenberg_GetControllerStateBridgeAPI(std::uint32_t requestedAbiVersion)
{
    return requestedAbiVersion == heisenberg::controller_bridge::kAbiVersion
        ? &g_bridgeAPI
        : nullptr;
}

extern "C" __declspec(dllexport) bool Heisenberg_RegisterControllerStateCallback(
    HeisenbergExternalControllerStateCallback callback)
{
    if (!callback) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_legacyBridgeMutex);
    if (g_legacyBridgeRegistrations.find(callback) != g_legacyBridgeRegistrations.end()) {
        return true;
    }

    auto adapter = std::make_unique<LegacyAdapter>();
    adapter->callback = callback;
    heisenberg::controller_bridge::Registration descriptor{
        sizeof(heisenberg::controller_bridge::Registration),
        heisenberg::controller_bridge::kAbiVersion,
        heisenberg::controller_bridge::kPreHeisenberg,
        heisenberg::controller_bridge::kConsumerGame,
        0,
        0,
        &LegacyDispatch,
        adapter.get(),
        "legacy-controls-remapper",
    };
    std::uint64_t handle = 0;
    if (!BridgeRegister(&descriptor, &handle)) {
        return false;
    }
    g_legacyBridgeRegistrations.emplace(
        callback, LegacyRegistration{handle, std::move(adapter)});
    spdlog::info(
        "[OpenVRHook] Registered legacy game-only controller-state callback in pre-Heisenberg phase");
    return true;
}

extern "C" __declspec(dllexport) bool Heisenberg_UnregisterControllerStateCallback(
    HeisenbergExternalControllerStateCallback callback)
{
    std::unique_lock<std::mutex> lock(g_legacyBridgeMutex);
    const auto it = g_legacyBridgeRegistrations.find(callback);
    if (it == g_legacyBridgeRegistrations.end()) {
        return false;
    }
    auto registration = std::move(it->second);
    g_legacyBridgeRegistrations.erase(it);
    lock.unlock();

    const bool removed = BridgeUnregister(registration.handle, true);
    if (removed) {
        spdlog::info("[OpenVRHook] Unregistered legacy controller-state callback");
    }
    return removed;
}
