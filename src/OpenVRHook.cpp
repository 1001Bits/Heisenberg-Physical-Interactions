#include "OpenVRHook.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <DbgHelp.h>

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
    static OpenVRHookManagerAPI* g_fo4vrToolsAPI = nullptr;
    
    // Our callbacks stored for FO4VRTools mode
    static std::vector<ControllerStateCallback> g_fo4vrToolsCallbacks;
    static std::mutex g_fo4vrToolsCallbacksMutex;
    static std::atomic<vr::TrackedDeviceIndex_t> g_leftControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
    static std::atomic<vr::TrackedDeviceIndex_t> g_rightControllerIndex{vr::k_unTrackedDeviceIndexInvalid};
    
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
        
        // Update controller indices if needed
        if (g_fo4vrToolsAPI && (g_leftControllerIndex == vr::k_unTrackedDeviceIndexInvalid || 
                                g_rightControllerIndex == vr::k_unTrackedDeviceIndexInvalid)) {
            vr::IVRSystem* vrSystem = g_fo4vrToolsAPI->GetVRSystem();
            if (vrSystem) {
                g_leftControllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
                g_rightControllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
            }
        }
        
        // Determine if this is left or right controller
        bool isLeft = (unControllerDeviceIndex == g_leftControllerIndex);
        
        // Copy-under-lock: snapshot callbacks to avoid holding mutex during execution
        std::vector<ControllerStateCallback> callbacksCopy;
        {
            std::lock_guard<std::mutex> lock(g_fo4vrToolsCallbacksMutex);
            callbacksCopy = g_fo4vrToolsCallbacks;
        }
        for (auto& callback : callbacksCopy) {
            uint64_t mask = callback(isLeft, pOutputControllerState);
            pOutputControllerState->ulButtonPressed &= mask;
            pOutputControllerState->ulButtonTouched &= mask;
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

    bool VRSystemWrapper::IsLeftController(vr::TrackedDeviceIndex_t index)
    {
        // Refresh indices periodically (controllers can change)
        if (m_leftControllerIndex == vr::k_unTrackedDeviceIndexInvalid || 
            m_rightControllerIndex == vr::k_unTrackedDeviceIndexInvalid) {
            UpdateControllerIndices();
        }
        return index == m_leftControllerIndex;
    }

    // ============================================================
    // GetControllerState - THE KEY INTERCEPTION POINT
    // ============================================================

    bool VRSystemWrapper::GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex, 
                                              vr::VRControllerState_t* pControllerState, 
                                              uint32_t unControllerStateSize)
    {
        // Get real state first
        bool result = m_realSystem->GetControllerState(unControllerDeviceIndex, pControllerState, unControllerStateSize);
        
        if (result && pControllerState) {
            // Determine if this is left or right controller
            bool isLeft = IsLeftController(unControllerDeviceIndex);

            // Copy-under-lock: snapshot callbacks to avoid holding mutex during execution
            std::vector<ControllerStateCallback> callbacksCopy;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                callbacksCopy = m_callbacks;
            }
            for (auto& callback : callbacksCopy) {
                uint64_t mask = callback(isLeft, pControllerState);
                pControllerState->ulButtonPressed &= mask;
                pControllerState->ulButtonTouched &= mask;
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
        // Get real state first
        bool result = m_realSystem->GetControllerStateWithPose(eOrigin, unControllerDeviceIndex,
                                                                pControllerState, unControllerStateSize,
                                                                pTrackedDevicePose);

        if (result && pControllerState) {
            // Determine if this is left or right controller
            bool isLeft = IsLeftController(unControllerDeviceIndex);

            // Copy-under-lock: snapshot callbacks to avoid holding mutex during execution
            std::vector<ControllerStateCallback> callbacksCopy;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                callbacksCopy = m_callbacks;
            }
            for (auto& callback : callbacksCopy) {
                uint64_t mask = callback(isLeft, pControllerState);
                pControllerState->ulButtonPressed &= mask;
                pControllerState->ulButtonTouched &= mask;
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
        void* result = originalFunc(pchInterfaceVersion, peError);

        if (result && pchInterfaceVersion) {
            // Check if this is a request for IVRSystem
            if (strstr(pchInterfaceVersion, "IVRSystem") != nullptr) {
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

        // Get the real VR_GetGenericInterface address
        s_originalGetGenericInterface = GetProcAddress(openvrModule, "VR_GetGenericInterface");
        if (!s_originalGetGenericInterface) {
            spdlog::error("[OpenVRHook] Failed to find VR_GetGenericInterface in openvr_api.dll");
            return false;
        }

        spdlog::info("[OpenVRHook] Found VR_GetGenericInterface at {:p}", s_originalGetGenericInterface);

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

                            // Save original address
                            m_originalIATEntry = reinterpret_cast<void*>(thunk->u1.Function);

                            // Make the page writable
                            DWORD oldProtect;
                            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                spdlog::error("[OpenVRHook] VirtualProtect failed: {}", GetLastError());
                                return false;
                            }

                            // Replace with our hook
                            thunk->u1.Function = reinterpret_cast<ULONGLONG>(HookedVR_GetGenericInterface);

                            // Restore protection
                            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);

                            spdlog::info("[OpenVRHook] IAT patch successful!");
                            m_isHooked = true;
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
        if (!m_isHooked || !m_originalIATEntry) {
            return true;
        }

        // We would need to re-locate the IAT entry to restore it
        // For simplicity, we just mark as not hooked - the game will exit anyway
        m_isHooked = false;
        return true;
    }

    bool OpenVRHook::Initialize()
    {
        spdlog::info("[OpenVRHook] Initializing OpenVR hook...");

        if (m_isHooked) {
            spdlog::warn("[OpenVRHook] Already hooked");
            return true;
        }

        // FIRST: Check if FO4VRTools.dll is loaded AT ALL
        // If it is, we should NOT do IAT patching - we'll use their API later
        HMODULE fo4vrToolsModule = GetModuleHandleA("FO4VRTools.dll");
        if (fo4vrToolsModule != nullptr) {
            // FO4VRTools is loaded - don't do IAT patching, we'll use their API lazily
            spdlog::info("[OpenVRHook] FO4VRTools.dll detected - skipping IAT hook to avoid conflicts");
            spdlog::info("[OpenVRHook] Will use FO4VRTools API for OpenVR access");
            
            m_isHooked = true;  // Mark as "hooked" so we don't try again
            m_usingFO4VRTools = true;  // Will get API lazily when needed
            return true;
        }
        
        // FO4VRTools not loaded - use our own IAT hook
        spdlog::info("[OpenVRHook] FO4VRTools not detected - using our own IAT hook");

        // Try to patch the IAT
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
        
        if (m_usingFO4VRTools && g_fo4vrToolsAPI) {
            // Unregister our bridge callback from FO4VRTools
            g_fo4vrToolsAPI->UnregisterControllerStateCB(FO4VRToolsControllerStateCallback);
            g_fo4vrToolsCallbacks.clear();
            spdlog::info("[OpenVRHook] Unregistered from FO4VRTools");
        } else {
            RestoreIAT();
        }
        
        // Unhook vtable if we hooked it
        UnhookRealVRSystemVtable();
        
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_callbacks.clear();
        }

        m_vrSystemWrapper.reset();
    }
    
    // Helper to lazily get FO4VRTools API (called when needed, not at init time)
    static OpenVRHookManagerAPI* EnsureFO4VRToolsAPI()
    {
        if (g_fo4vrToolsAPI) {
            return g_fo4vrToolsAPI;
        }
        
        // Try to get the API now
        g_fo4vrToolsAPI = RequestFO4VRToolsAPI();
        if (g_fo4vrToolsAPI) {
            spdlog::info("[OpenVRHook] Lazily acquired FO4VRTools API");
            
            // Register our bridge callback (still useful for game's own calls)
            g_fo4vrToolsAPI->RegisterControllerStateCB(FO4VRToolsControllerStateCallback);
            
            // CRITICAL: Also hook the REAL IVRSystem's vtable
            // This catches calls from mods that use GetVRSystem()->GetControllerState()
            // (like Virtual Holsters) which bypass the FO4VRTools callback system
            vr::IVRSystem* realSystem = g_fo4vrToolsAPI->GetVRSystem();
            if (realSystem) {
                OpenVRHook::GetSingleton().HookRealVRSystemVtable(realSystem);
            }
        }
        
        return g_fo4vrToolsAPI;
    }
    
    vr::IVRSystem* OpenVRHook::GetRealVRSystem() const
    {
        if (m_usingFO4VRTools) {
            auto* api = EnsureFO4VRToolsAPI();
            if (api) {
                return api->GetVRSystem();
            }
            return nullptr;
        }
        return m_vrSystemWrapper ? m_vrSystemWrapper->GetRealSystem() : nullptr;
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
        // Always add to our internal callback list (used by vtable hook)
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_callbacks.push_back(callback);
            spdlog::info("[OpenVRHook] Registered controller state callback, total: {}", m_callbacks.size());
        }
        
        // Register in secondary callback lists ONLY if vtable hook is NOT active.
        // When vtable is hooked, ApplyCallbacksToState (using m_callbacks) already runs
        // on every GetControllerState call. Adding to the bridge/wrapper list too would
        // cause double-execution: vtable hook modifies state (strips A, injects grip),
        // then bridge/wrapper applies the same callback on the modified state, seeing
        // stripped A as "released" and corrupting hold-timing logic.
        if (m_usingFO4VRTools) {
            // Ensure API is available and hook vtable
            EnsureFO4VRToolsAPI();
            
            // After EnsureFO4VRToolsAPI, vtable should be hooked. Only add to bridge
            // as fallback if vtable hook failed.
            if (!m_vtableHooked) {
                spdlog::warn("[OpenVRHook] Vtable hook failed - using FO4VRTools bridge as fallback");
                std::lock_guard<std::mutex> lock(g_fo4vrToolsCallbacksMutex);
                g_fo4vrToolsCallbacks.push_back(callback);
            } else {
                spdlog::info("[OpenVRHook] Vtable hooked - skipping bridge (prevents double-execution)");
            }
        } else if (m_vrSystemWrapper) {
            // Wrapper mode - vtable hook handles everything, no need for wrapper callbacks
            if (!m_vtableHooked) {
                m_vrSystemWrapper->RegisterControllerStateCallback(callback);
            } else {
                spdlog::info("[OpenVRHook] Vtable hooked - skipping wrapper callback (prevents double-execution)");
            }
        }
    }
    
    void OpenVRHook::ApplyCallbacksToState(bool isLeft, vr::VRControllerState_t* state)
    {
        if (!state) return;

        // Copy-under-lock: snapshot callbacks to avoid holding mutex during execution
        std::vector<ControllerStateCallback> callbacksCopy;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            callbacksCopy = m_callbacks;
        }
        for (auto& callback : callbacksCopy) {
            uint64_t mask = callback(isLeft, state);
            state->ulButtonPressed &= mask;
            state->ulButtonTouched &= mask;
        }
    }
    
    void OpenVRHook::UpdateControllerIndices(vr::IVRSystem* vrSystem)
    {
        if (!vrSystem) return;
        m_leftControllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        m_rightControllerIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    }
    
    bool OpenVRHook::IsLeftController(vr::TrackedDeviceIndex_t deviceIndex)
    {
        return deviceIndex == m_leftControllerIndex;
    }
    
    // ============================================================
    // Vtable Hooking - hooks the REAL IVRSystem's vtable
    // This intercepts GetControllerState even when mods bypass FO4VRTools
    // ============================================================
    
    // Global pointer to OpenVRHook for the hook functions
    static OpenVRHook* g_openVRHookInstance = nullptr;
    
    // Our hooked GetControllerState - called instead of the real one
    static bool __fastcall Hooked_GetControllerState(vr::IVRSystem* thisPtr,
                                                      vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                                      vr::VRControllerState_t* pControllerState,
                                                      uint32_t unControllerStateSize)
    {
        if (!g_openVRHookInstance || !g_openVRHookInstance->m_originalGetControllerState) {
            return false;
        }
        
        // Call the original function
        bool result = g_openVRHookInstance->m_originalGetControllerState(
            thisPtr, unControllerDeviceIndex, pControllerState, unControllerStateSize);
        
        if (result && pControllerState) {
            // Update controller indices if needed
            if (g_openVRHookInstance->m_leftControllerIndex == vr::k_unTrackedDeviceIndexInvalid) {
                g_openVRHookInstance->UpdateControllerIndices(thisPtr);
            }
            
            // Determine hand and apply callbacks
            bool isLeft = g_openVRHookInstance->IsLeftController(unControllerDeviceIndex);
            g_openVRHookInstance->ApplyCallbacksToState(isLeft, pControllerState);
        }
        
        return result;
    }
    
    // Our hooked GetControllerStateWithPose
    static bool __fastcall Hooked_GetControllerStateWithPose(vr::IVRSystem* thisPtr,
                                                              vr::ETrackingUniverseOrigin eOrigin,
                                                              vr::TrackedDeviceIndex_t unControllerDeviceIndex,
                                                              vr::VRControllerState_t* pControllerState,
                                                              uint32_t unControllerStateSize,
                                                              vr::TrackedDevicePose_t* pTrackedDevicePose)
    {
        if (!g_openVRHookInstance || !g_openVRHookInstance->m_originalGetControllerStateWithPose) {
            return false;
        }
        
        // Call the original function
        bool result = g_openVRHookInstance->m_originalGetControllerStateWithPose(
            thisPtr, eOrigin, unControllerDeviceIndex, pControllerState, unControllerStateSize, pTrackedDevicePose);
        
        if (result && pControllerState) {
            // Update controller indices if needed
            if (g_openVRHookInstance->m_leftControllerIndex == vr::k_unTrackedDeviceIndexInvalid) {
                g_openVRHookInstance->UpdateControllerIndices(thisPtr);
            }
            
            // Determine hand and apply callbacks
            bool isLeft = g_openVRHookInstance->IsLeftController(unControllerDeviceIndex);
            g_openVRHookInstance->ApplyCallbacksToState(isLeft, pControllerState);
        }
        
        return result;
    }
    
    bool OpenVRHook::HookRealVRSystemVtable(vr::IVRSystem* realSystem)
    {
        if (m_vtableHooked || !realSystem) {
            return m_vtableHooked;
        }
        
        // Store instance pointer for hook functions
        g_openVRHookInstance = this;
        m_realVRSystem = realSystem;
        
        // Get the vtable pointer (first sizeof(void*) bytes of the object)
        void** vtable = *reinterpret_cast<void***>(realSystem);
        
        // Store original function pointers
        m_originalGetControllerState = reinterpret_cast<GetControllerState_t>(vtable[kVtableIndex_GetControllerState]);
        m_originalGetControllerStateWithPose = reinterpret_cast<GetControllerStateWithPose_t>(vtable[kVtableIndex_GetControllerStateWithPose]);
        
        // Make vtable writable
        DWORD oldProtect;
        void* vtableEntry1 = &vtable[kVtableIndex_GetControllerState];
        void* vtableEntry2 = &vtable[kVtableIndex_GetControllerStateWithPose];
        
        // Patch GetControllerState
        if (!VirtualProtect(vtableEntry1, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            spdlog::error("[OpenVRHook] Failed to change vtable protection for GetControllerState");
            return false;
        }
        vtable[kVtableIndex_GetControllerState] = reinterpret_cast<void*>(&Hooked_GetControllerState);
        VirtualProtect(vtableEntry1, sizeof(void*), oldProtect, &oldProtect);
        
        // Patch GetControllerStateWithPose
        if (!VirtualProtect(vtableEntry2, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            spdlog::error("[OpenVRHook] Failed to change vtable protection for GetControllerStateWithPose");
            return false;
        }
        vtable[kVtableIndex_GetControllerStateWithPose] = reinterpret_cast<void*>(&Hooked_GetControllerStateWithPose);
        VirtualProtect(vtableEntry2, sizeof(void*), oldProtect, &oldProtect);
        
        m_vtableHooked = true;
        spdlog::info("[OpenVRHook] Hooked IVRSystem vtable for button filtering");
        
        // Update controller indices
        UpdateControllerIndices(realSystem);
        
        return true;
    }
    
    void OpenVRHook::UnhookRealVRSystemVtable()
    {
        if (!m_vtableHooked || !m_realVRSystem) {
            return;
        }
        
        spdlog::info("[OpenVRHook] Unhooking real IVRSystem vtable");
        
        // Get the vtable pointer
        void** vtable = *reinterpret_cast<void***>(m_realVRSystem);
        
        // Restore original function pointers
        DWORD oldProtect;
        
        if (m_originalGetControllerState) {
            void* vtableEntry1 = &vtable[kVtableIndex_GetControllerState];
            if (VirtualProtect(vtableEntry1, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                vtable[kVtableIndex_GetControllerState] = reinterpret_cast<void*>(m_originalGetControllerState);
                VirtualProtect(vtableEntry1, sizeof(void*), oldProtect, &oldProtect);
            }
        }
        
        if (m_originalGetControllerStateWithPose) {
            void* vtableEntry2 = &vtable[kVtableIndex_GetControllerStateWithPose];
            if (VirtualProtect(vtableEntry2, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                vtable[kVtableIndex_GetControllerStateWithPose] = reinterpret_cast<void*>(m_originalGetControllerStateWithPose);
                VirtualProtect(vtableEntry2, sizeof(void*), oldProtect, &oldProtect);
            }
        }
        
        m_vtableHooked = false;
        m_originalGetControllerState = nullptr;
        m_originalGetControllerStateWithPose = nullptr;
        m_realVRSystem = nullptr;
        g_openVRHookInstance = nullptr;
        
        spdlog::info("[OpenVRHook] Vtable restored");
    }

} // namespace heisenberg
