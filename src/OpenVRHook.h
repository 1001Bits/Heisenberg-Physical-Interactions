#pragma once

#include <openvr.h>
#include "HeisenbergControllerStateBridge.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <vector>
#include <mutex>

namespace heisenberg
{
    // Callback signature for controller state modification
    // Return a bitmask to AND with ulButtonPressed/ulButtonTouched
    // Return 0xFFFFFFFFFFFFFFFF to allow all buttons, return 0 to block all
    using ControllerStateCallback = std::function<uint64_t(bool isLeft, vr::VRControllerState_t* state)>;

    /**
     * Wrapper class for IVRSystem that intercepts GetControllerState calls
     * Forwards all other methods to the real IVRSystem
     */
    class VRSystemWrapper : public vr::IVRSystem
    {
    public:
        VRSystemWrapper(vr::IVRSystem* realSystem);

        // Register a callback to modify controller state
        // Callback returns a bitmask to AND with button pressed/touched states
        void RegisterControllerStateCallback(ControllerStateCallback callback);
        void ClearCallbacks();
        
        // Get the real IVRSystem (for our internal use - bypasses callbacks)
        vr::IVRSystem* GetRealSystem() const { return m_realSystem; }
        
        // Get controller state WITHOUT applying callbacks (for our internal use)
        bool GetControllerStateRaw(vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
                                    vr::VRControllerState_t* pControllerState, 
                                    uint32_t unControllerStateSize);

        // ============================================================
        // IVRSystem interface implementation - forwards to real system
        // ============================================================

        // Core methods
        void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override;
        vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye eEye, float fNearZ, float fFarZ) override;
        void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override;
        bool ComputeDistortion(vr::EVREye eEye, float fU, float fV, vr::DistortionCoordinates_t* pDistortionCoordinates) override;
        vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eEye) override;
        bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) override;
        int32_t GetD3D9AdapterIndex() override;
        void GetDXGIOutputInfo(int32_t* pnAdapterIndex) override;
        void GetOutputDevice(uint64_t* pnDevice, vr::ETextureType textureType, VkInstance_T* pInstance = nullptr) override;

        // Display methods
        bool IsDisplayOnDesktop() override;
        bool SetDisplayVisibility(bool bIsVisibleOnDesktop) override;

        // Tracking methods
        void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, vr::TrackedDevicePose_t* pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) override;
        void ResetSeatedZeroPose() override;
        vr::HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override;
        vr::HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override;
        uint32_t GetSortedTrackedDeviceIndicesOfClass(vr::ETrackedDeviceClass eTrackedDeviceClass, vr::TrackedDeviceIndex_t* punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex = vr::k_unTrackedDeviceIndex_Hmd) override;
        vr::EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) override;
        void ApplyTransform(vr::TrackedDevicePose_t* pOutputPose, const vr::TrackedDevicePose_t* pTrackedDevicePose, const vr::HmdMatrix34_t* pTransform) override;
        vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override;
        vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex) override;

        // Property methods
        vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override;
        bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override;
        bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError = 0L) override;
        float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError = 0L) override;
        int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError = 0L) override;
        uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError = 0L) override;
        vr::HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pError = 0L) override;
        uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, vr::PropertyTypeTag_t propType, void* pBuffer, uint32_t unBufferSize, vr::ETrackedPropertyError* pError = 0L) override;
        uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, vr::ETrackedDeviceProperty prop, char* pchValue, uint32_t unBufferSize, vr::ETrackedPropertyError* pError = 0L) override;
        const char* GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) override;

        // Event methods
        bool PollNextEvent(vr::VREvent_t* pEvent, uint32_t uncbVREvent) override;
        bool PollNextEventWithPose(vr::ETrackingUniverseOrigin eOrigin, vr::VREvent_t* pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t* pTrackedDevicePose) override;
        const char* GetEventTypeNameFromEnum(vr::EVREventType eType) override;

        // Hidden area mesh
        vr::HiddenAreaMesh_t GetHiddenAreaMesh(vr::EVREye eEye, vr::EHiddenAreaMeshType type = vr::k_eHiddenAreaMesh_Standard) override;

        // Controller methods - THIS IS WHERE WE INTERCEPT
        bool GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t* pControllerState, uint32_t unControllerStateSize) override;
        bool GetControllerStateWithPose(vr::ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t* pControllerState, uint32_t unControllerStateSize, vr::TrackedDevicePose_t* pTrackedDevicePose) override;
        void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) override;
        const char* GetButtonIdNameFromEnum(vr::EVRButtonId eButtonId) override;
        const char* GetControllerAxisTypeNameFromEnum(vr::EVRControllerAxisType eAxisType) override;
        bool IsInputAvailable() override;
        bool IsSteamVRDrawingControllers() override;
        bool ShouldApplicationPause() override;
        bool ShouldApplicationReduceRenderingWork() override;

        // Firmware methods
        uint32_t DriverDebugRequest(vr::TrackedDeviceIndex_t unDeviceIndex, const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override;
        vr::EVRFirmwareError PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex) override;

        // Application methods
        void AcknowledgeQuit_Exiting() override;
        void AcknowledgeQuit_UserPrompt() override;

    private:
        vr::IVRSystem* m_realSystem;
        std::vector<ControllerStateCallback> m_callbacks;
        std::mutex m_callbackMutex;

        // Cached controller indices. OpenVR can poll from more than one thread.
        std::atomic<vr::TrackedDeviceIndex_t> m_leftControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
        std::atomic<vr::TrackedDeviceIndex_t> m_rightControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
        
        void UpdateControllerIndices();
        controller_bridge::ControllerRole GetControllerRole(vr::TrackedDeviceIndex_t index);
    };

    // ============================================================
    // Direct vtable hook for IVRSystem::GetControllerState
    // This hooks the REAL IVRSystem vtable so even when other mods
    // call GetVRSystem()->GetControllerState(), our filter is applied.
    // ============================================================
    
    // IVRSystem vtable indices (IVRSystem_019)
    constexpr int kVtableIndex_GetControllerState = 34;
    constexpr int kVtableIndex_GetControllerStateWithPose = 35;
    constexpr int kVtableIndex_GetTrackedDeviceIndexForControllerRole = 18;
    
    // Function signatures for the vtable entries
    using GetControllerState_t = bool(__thiscall*)(vr::IVRSystem* thisPtr, 
        vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
        vr::VRControllerState_t* pControllerState, 
        uint32_t unControllerStateSize);
        
    using GetControllerStateWithPose_t = bool(__thiscall*)(vr::IVRSystem* thisPtr,
        vr::ETrackingUniverseOrigin eOrigin,
        vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
        vr::VRControllerState_t* pControllerState, 
        uint32_t unControllerStateSize,
        vr::TrackedDevicePose_t* pTrackedDevicePose);

    /**
     * Main OpenVR hook manager
     * Handles IAT patching and interface wrapping
     *
     * Thread safety:
     *   - m_callbacks: protected by m_callbackMutex (copy-under-lock pattern for iteration)
     *   - g_leftControllerIndex/g_rightControllerIndex: std::atomic (OpenVR thread + main thread)
     *   - RegisterControllerStateCallback: main-thread only (during init)
     *   - GetControllerState/WithPose: called from OpenVR thread, iterates callback copy
     *   - GetControllerStateUnfiltered: bypasses all hooks, safe from any thread
     */
    class OpenVRHook
    {
        // Friend functions for vtable hooks (need access to m_originalGetControllerState)
        friend bool __fastcall Hooked_GetControllerState(vr::IVRSystem*, vr::TrackedDeviceIndex_t,
                                                          vr::VRControllerState_t*, uint32_t);
        friend bool __fastcall Hooked_GetControllerStateWithPose(vr::IVRSystem*, vr::ETrackingUniverseOrigin,
                                                                  vr::TrackedDeviceIndex_t, vr::VRControllerState_t*,
                                                                  uint32_t, vr::TrackedDevicePose_t*);
    public:
        static OpenVRHook& GetSingleton();

        // Initialize the hook - call early in plugin load
        bool Initialize();
        
        // Cleanup
        void Shutdown();

        // Get our wrapper (nullptr if using FO4VRTools or not hooked)
        VRSystemWrapper* GetVRSystemWrapper() { return m_vrSystemWrapper.get(); }
        
        // Get the real IVRSystem (bypasses our hooks) - for our internal input reading
        // When using FO4VRTools, returns their VR system
        vr::IVRSystem* GetRealVRSystem() const;
        
        // Get the UNFILTERED controller state (bypasses all hooks including vtable)
        // Use this for Heisenberg's own input reading
        bool GetControllerStateUnfiltered(vr::TrackedDeviceIndex_t deviceIndex,
                                           vr::VRControllerState_t* pControllerState,
                                           uint32_t unControllerStateSize);

        // Register callback for controller state modification
        void RegisterControllerStateCallback(ControllerStateCallback callback);

        // Check if hook is active
        bool IsHooked() const { return m_isHooked.load(std::memory_order_acquire); }
        bool IsVtableHooked() const { return m_vtableHooked.load(std::memory_order_acquire); }
        std::uint32_t GetBackendStatusFlags() const;
        
        // Apply callbacks to a controller state (used by vtable hooks)
        void ApplyCallbacksToState(controller_bridge::ControllerRole role,
                                   vr::VRControllerState_t* state,
                                   std::uint32_t consumer);
        // Runs only bridge callbacks registered for Heisenberg's private
        // interaction consumer. The raw controller state is never modified.
        // Returns true when a callback owns logical Grip for this sample.
        bool ApplyInteractionCallbacks(
            controller_bridge::ControllerRole role,
            vr::TrackedDeviceIndex_t deviceIndex,
            const vr::VRControllerState_t& rawState,
            vr::VRControllerState_t& interactionState);
        
        // Get controller hand side
        controller_bridge::ControllerRole GetControllerRole(vr::TrackedDeviceIndex_t deviceIndex) const;
        void UpdateControllerIndices(vr::IVRSystem* vrSystem);

    private:
        OpenVRHook() = default;
        ~OpenVRHook() = default;
        OpenVRHook(const OpenVRHook&) = delete;
        OpenVRHook& operator=(const OpenVRHook&) = delete;

        std::atomic<bool> m_isHooked{false};
        std::atomic<bool> m_usingFO4VRTools{false};  // FO4VRTools is present or being retried
        std::atomic<bool> m_vtableHooked{false};
        std::atomic<bool> m_iatHooked{false};
        std::atomic<bool> m_fo4vrToolsCallbackRegistered{false};
        std::atomic<bool> m_shuttingDown{false};
        std::unique_ptr<VRSystemWrapper> m_vrSystemWrapper;
        mutable std::mutex m_installMutex;
        
        // Callbacks for button filtering
        std::vector<ControllerStateCallback> m_callbacks;
        std::mutex m_callbackMutex;
        
        // Cached controller indices for vtable hook
        std::atomic<vr::TrackedDeviceIndex_t> m_leftControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
        std::atomic<vr::TrackedDeviceIndex_t> m_rightControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
        
        // Original vtable function pointers (before our hook)
        GetControllerState_t m_originalGetControllerState = nullptr;
        GetControllerStateWithPose_t m_originalGetControllerStateWithPose = nullptr;
        
        // Pointer to the real IVRSystem (for vtable hooking)
        vr::IVRSystem* m_realVRSystem = nullptr;

        // Original function pointer for IAT hook
        static void* s_originalGetGenericInterface;
        
        // Our hook function
        static void* HookedVR_GetGenericInterface(const char* pchInterfaceVersion, vr::EVRInitError* peError);

        // IAT patching helpers
        bool PatchIAT();
        bool RestoreIAT();
        bool TryActivateFO4VRToolsBackend();
        void* m_originalIATEntry = nullptr;
        void** m_iatSlot = nullptr;

        void BeginHookCall();
        void EndHookCall();
        void WaitForHookCallsToDrain();
        std::atomic<std::uint32_t> m_activeHookCalls{0};
        std::mutex m_drainMutex;
        std::condition_variable m_drainCondition;
        
    public:
        // Vtable hooking is public for the lazy FO4VRTools backend activation path.
        bool HookRealVRSystemVtable(vr::IVRSystem* realSystem);
    private:
        void UnhookRealVRSystemVtable();
    };

} // namespace heisenberg
