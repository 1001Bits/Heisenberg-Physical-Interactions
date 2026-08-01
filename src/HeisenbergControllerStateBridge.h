#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <openvr.h>

// Stable C ABI used by input remappers that need to run in Heisenberg's
// controller-state pipeline.  Keep this header free of STL types.
namespace heisenberg::controller_bridge
{
    inline constexpr std::uint32_t kAbiVersion = 1;

    enum Phase : std::uint32_t
    {
        kPreHeisenberg = 100,
        kPostHeisenberg = 300,
    };

    enum Consumer : std::uint32_t
    {
        kConsumerGame = 1u << 0,
        kConsumerThirdParty = 1u << 1,
        kConsumerHeisenberg = 1u << 2,
        kConsumerAll = kConsumerGame | kConsumerThirdParty | kConsumerHeisenberg,
    };

    enum ControllerRole : std::uint32_t
    {
        kRoleUnknown = 0,
        kRoleLeft = 1,
        kRoleRight = 2,
    };

    enum BackendFlag : std::uint32_t
    {
        kBackendNone = 0,
        kBackendIATInstalled = 1u << 0,
        kBackendFO4VRToolsAvailable = 1u << 1,
        kBackendFO4VRToolsCallback = 1u << 2,
        kBackendVtableInstalled = 1u << 3,
        kBackendOperational = 1u << 4,
        kBackendShuttingDown = 1u << 5,
        kBackendInteractionStateConsumed = 1u << 6,
    };

    enum CallbackResultFlag : std::uint64_t
    {
        kResultNone = 0,
        kResultOwnsHeisenbergGrip = 1ull << 0,
    };

    struct CallbackContext
    {
        std::uint32_t structSize;
        std::uint32_t abiVersion;
        std::uint32_t phase;
        std::uint32_t consumer;
        std::uint32_t controllerRole;
        std::uint32_t trackedDeviceIndex;
        std::uint64_t reserved;
        // Private per-poll state consumed by Heisenberg's interaction callbacks.
        // A pre-phase remapper can inject an interaction-only Grip here without
        // returning that Grip to Fallout in the normal `state` callback argument.
        vr::VRControllerState_t* heisenbergState;
        // Optional additive ABI field. Check structSize before using it.
        std::uint64_t* resultFlags;
    };

    using Callback = std::uint64_t(__cdecl*)(
        const CallbackContext* context,
        vr::VRControllerState_t* state,
        void* userData);

    struct Registration
    {
        std::uint32_t structSize;
        std::uint32_t abiVersion;
        std::uint32_t phase;
        std::uint32_t consumerMask;
        std::int32_t priority;
        std::uint32_t reserved;
        Callback callback;
        void* userData;
        const char* ownerName;
    };

    struct BackendStatus
    {
        std::uint32_t structSize;
        std::uint32_t abiVersion;
        std::uint32_t flags;
        std::uint32_t registeredCallbacks;
    };

    using RegisterCallback = bool(__cdecl*)(
        const Registration* registration,
        std::uint64_t* registrationHandle);
    using UnregisterCallback = bool(__cdecl*)(
        std::uint64_t registrationHandle,
        bool drainInFlight);
    using GetBackendStatus = bool(__cdecl*)(BackendStatus* status);

    struct API
    {
        std::uint32_t structSize;
        std::uint32_t abiVersion;
        RegisterCallback registerCallback;
        UnregisterCallback unregisterCallback;
        GetBackendStatus getBackendStatus;
    };

    using GetAPI = const API* (__cdecl*)(std::uint32_t requestedAbiVersion);

    static_assert(sizeof(void*) == 8, "F4VR controller bridge requires x64");
    static_assert(std::is_standard_layout_v<CallbackContext>);
    static_assert(std::is_trivially_copyable_v<CallbackContext>);
    static_assert(sizeof(CallbackContext) == 48);
    static_assert(offsetof(CallbackContext, reserved) == 24);
    static_assert(offsetof(CallbackContext, heisenbergState) == 32);
    static_assert(offsetof(CallbackContext, resultFlags) == 40);
    static_assert(std::is_standard_layout_v<Registration>);
    static_assert(std::is_trivially_copyable_v<Registration>);
    static_assert(sizeof(Registration) == 48);
    static_assert(offsetof(Registration, callback) == 24);
    static_assert(offsetof(Registration, ownerName) == 40);
    static_assert(sizeof(BackendStatus) == 16);
    static_assert(sizeof(API) == 32);
    static_assert(offsetof(API, registerCallback) == 8);
}

extern "C"
{
    __declspec(dllexport)
    const heisenberg::controller_bridge::API* __cdecl
        Heisenberg_GetControllerStateBridgeAPI(std::uint32_t requestedAbiVersion);
}
