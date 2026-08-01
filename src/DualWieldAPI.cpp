#include "DualWieldAPI.h"

#include "Heisenberg.h"
#include "Hand.h"
#include "VRInput.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace HeisenbergPluginAPI
{
    namespace
    {
        std::mutex g_registryMutex;
        std::condition_variable g_registryCV;

        DualWieldStateProvider g_stateProvider = nullptr;
        std::uint32_t g_stateProviderCalls = 0;
        bool g_stateProviderDraining = false;
        bool g_stateProviderSelfDrain = false;
        thread_local DualWieldStateProvider g_invokingStateProvider = nullptr;

        DualWieldWeaponContactCallback g_contactCallback = nullptr;
        std::uint32_t g_contactCallbackCalls = 0;
        bool g_contactCallbackDraining = false;
        bool g_contactCallbackSelfDrain = false;
        thread_local DualWieldWeaponContactCallback g_invokingContactCallback = nullptr;

        void FinishStateProviderCall()
        {
            std::lock_guard lock(g_registryMutex);
            if (g_stateProviderCalls > 0) {
                --g_stateProviderCalls;
            }
            if (g_stateProviderCalls == 0) {
                // A provider is allowed to unregister itself. That call cannot
                // wait for its own stack frame, so the final in-flight call
                // completes the deferred drain here.
                if (g_stateProviderSelfDrain) {
                    g_stateProviderDraining = false;
                    g_stateProviderSelfDrain = false;
                }
                g_registryCV.notify_all();
            }
        }

        void FinishContactCallbackCall()
        {
            std::lock_guard lock(g_registryMutex);
            if (g_contactCallbackCalls > 0) {
                --g_contactCallbackCalls;
            }
            if (g_contactCallbackCalls == 0) {
                if (g_contactCallbackSelfDrain) {
                    g_contactCallbackDraining = false;
                    g_contactCallbackSelfDrain = false;
                }
                g_registryCV.notify_all();
            }
        }
    }

    bool RegisterDualWieldStateProviderImpl(DualWieldStateProvider provider)
    {
        if (!provider) {
            return false;
        }

        std::lock_guard lock(g_registryMutex);
        if (g_stateProviderDraining) {
            return false;
        }
        if (g_stateProvider) {
            return g_stateProvider == provider;
        }
        g_stateProvider = provider;
        return true;
    }

    bool UnregisterDualWieldStateProviderImpl(DualWieldStateProvider provider)
    {
        std::unique_lock lock(g_registryMutex);
        if (!g_stateProvider) {
            if (g_stateProviderDraining) {
                if (g_invokingStateProvider == provider) {
                    return true;
                }
                g_registryCV.wait(lock, [] { return g_stateProviderCalls == 0; });
            }
            return true;
        }
        if (!provider || g_stateProvider != provider) {
            return false;
        }

        g_stateProvider = nullptr;
        g_stateProviderDraining = true;
        if (g_invokingStateProvider == provider) {
            // Waiting here would deadlock on this very invocation. Keep the
            // registry closed until FinishStateProviderCall observes the last
            // in-flight call.
            g_stateProviderSelfDrain = true;
            return true;
        }
        g_stateProviderSelfDrain = false;
        g_registryCV.wait(lock, [] { return g_stateProviderCalls == 0; });
        g_stateProviderDraining = false;
        return true;
    }

    bool RegisterDualWieldWeaponContactCallbackImpl(DualWieldWeaponContactCallback callback)
    {
        if (!callback) {
            return false;
        }

        std::lock_guard lock(g_registryMutex);
        if (g_contactCallbackDraining) {
            return false;
        }
        if (g_contactCallback) {
            return g_contactCallback == callback;
        }
        g_contactCallback = callback;
        return true;
    }

    bool UnregisterDualWieldWeaponContactCallbackImpl(DualWieldWeaponContactCallback callback)
    {
        std::unique_lock lock(g_registryMutex);
        if (!g_contactCallback) {
            if (g_contactCallbackDraining) {
                if (g_invokingContactCallback == callback) {
                    return true;
                }
                g_registryCV.wait(lock, [] { return g_contactCallbackCalls == 0; });
            }
            return true;
        }
        if (!callback || g_contactCallback != callback) {
            return false;
        }

        g_contactCallback = nullptr;
        g_contactCallbackDraining = true;
        if (g_invokingContactCallback == callback) {
            g_contactCallbackSelfDrain = true;
            return true;
        }
        g_contactCallbackSelfDrain = false;
        g_registryCV.wait(lock, [] { return g_contactCallbackCalls == 0; });
        g_contactCallbackDraining = false;
        return true;
    }

    bool HasDualWieldStateProvider()
    {
        std::lock_guard lock(g_registryMutex);
        return g_stateProvider != nullptr && !g_stateProviderDraining;
    }

    bool QueryDualWieldHandState(PhysicalHand hand, DualWieldHandState& state)
    {
        if (!IsPhysicalHand(hand)) {
            return false;
        }

        DualWieldStateProvider provider = nullptr;
        {
            std::lock_guard lock(g_registryMutex);
            provider = g_stateProvider;
            if (!provider || g_stateProviderDraining) {
                return false;
            }
            ++g_stateProviderCalls;
        }

        const auto requestFlags = state.flags & kHandStateCandidateQuery;
        const auto requestFormID = requestFlags != 0 ? state.formID : 0;

        state = MakeHandState(hand);
        state.flags = requestFlags;
        state.formID = requestFormID;
        bool succeeded = false;
        const auto previousProvider = g_invokingStateProvider;
        g_invokingStateProvider = provider;
        try {
            succeeded = provider(hand, &state);
        } catch (...) {
            succeeded = false;
        }
        g_invokingStateProvider = previousProvider;
        FinishStateProviderCall();

        return succeeded && HasValidHeader(state) && state.physicalHand == hand;
    }

    void InvokeDualWieldWeaponContact(const DualWieldContact& contact)
    {
        if (!HasValidHeader(contact)) {
            return;
        }

        DualWieldWeaponContactCallback callback = nullptr;
        {
            std::lock_guard lock(g_registryMutex);
            callback = g_contactCallback;
            if (!callback || g_contactCallbackDraining) {
                return;
            }
            ++g_contactCallbackCalls;
        }

        const auto previousCallback = g_invokingContactCallback;
        g_invokingContactCallback = callback;
        try {
            callback(&contact);
        } catch (...) {
            // Never unwind a consumer exception through the Havok callback path.
        }
        g_invokingContactCallback = previousCallback;
        FinishContactCallbackCall();
    }

    bool GetPhysicalHandInputStateImpl(PhysicalHand hand, PhysicalHandInputState* state)
    {
        if (!state || !HasValidHeader(*state) || state->physicalHand != hand) {
            return false;
        }

        const bool isLeft = hand == PhysicalHand::kLeft;
        auto& input = heisenberg::VRInput::GetSingleton();
        if (!input.IsControllerTracked(isLeft)) {
            return false;
        }

        auto result = MakeInputState(hand);
        result.flags = kInputTrackingValid;
        result.triggerValue = std::clamp(input.GetTriggerValue(isLeft), 0.0F, 1.0F);
        result.gripValue = std::clamp(input.GetGripValue(isLeft), 0.0F, 1.0F);

        if (input.IsPressed(isLeft, heisenberg::VRButton::Trigger)) {
            result.flags |= kInputTriggerDown;
        }
        if (input.JustPressed(isLeft, heisenberg::VRButton::Trigger)) {
            result.flags |= kInputTriggerPressed;
        }
        if (input.JustReleased(isLeft, heisenberg::VRButton::Trigger)) {
            result.flags |= kInputTriggerReleased;
        }
        if (input.IsPressed(isLeft, heisenberg::VRButton::Grip)) {
            result.flags |= kInputGripDown;
        }
        if (input.JustPressed(isLeft, heisenberg::VRButton::Grip)) {
            result.flags |= kInputGripPressed;
        }
        if (input.JustReleased(isLeft, heisenberg::VRButton::Grip)) {
            result.flags |= kInputGripReleased;
        }

        auto& mod = heisenberg::Heisenberg::GetSingleton();
        auto* trackedHand = isLeft ? mod.GetLeftHand() : mod.GetRightHand();
        if (trackedHand) {
            result.linearVelocity = trackedHand->GetVelocity();
            result.angularVelocity = trackedHand->GetAngularVelocity();
        }

        *state = result;
        return true;
    }
}
