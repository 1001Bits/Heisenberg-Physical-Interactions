#include "FRIKInterface.h"
#include "HandAuthority.h"   // plugin-side hand placement when native FRIK lacks v5

namespace heisenberg
{
    using FRIKApi = frik::host_api::FRIKApi;

    bool FRIKInterface::Initialize()
    {
        if (_initialized) {
            return Api() != nullptr;
        }

        _initialized = true;

        // Accept FRIK v2+ for the CORE features (finger poses, grip, position reads — all in
        // the v2 ABI prefix). Do NOT require v9 globally: that would disable ALL FRIK on an
        // older install. The v9-only hand-pushback functions are gated separately by version
        // (see SupportsPushback / the pushback wrappers below).
        constexpr std::uint32_t kCoreMinVersion = 2;
        const int err = FRIKApi::initialize(kCoreMinVersion);

        switch (err) {
        case 0:
            break;  // Success
        case 1:
            spdlog::warn("FRIK module not found - hand tracking will use fallback");
            return false;
        case 2:
            spdlog::warn("FRIK API export 'FRIKAPI_GetApi' not found");
            spdlog::warn("Your FRIK version may not have the API.");
            return false;
        case 3:
            spdlog::error("FRIKAPI_GetApi returned nullptr");
            return false;
        case 4:
            spdlog::error("FRIK API version too old - Heisenberg requires API v2+ (yours is older)");
            return false;
        default:
            spdlog::error("FRIK API initialization failed with unknown error: {}", err);
            return false;
        }

        // Verify required functions
        auto* api = Api();
        if (!api->isSkeletonReady) {
            spdlog::error("FRIK API missing isSkeletonReady function");
            FRIKApi::inst = nullptr;
            return false;
        }
        if (!api->getIndexFingerTipPosition) {
            spdlog::error("FRIK API missing getIndexFingerTipPosition function");
            FRIKApi::inst = nullptr;
            return false;
        }

        const char* modVersion = api->getModVersion ? api->getModVersion() : "unknown";
        spdlog::info("FRIK mod version: {} (API v{})", modVersion, api->getVersion());
        spdlog::info("FRIK interface initialized successfully");

        return true;
    }

    bool FRIKInterface::IsAvailable() const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;
        auto* api = Api();
        return api && api->isSkeletonReady && api->isSkeletonReady();
    }

    std::uint32_t FRIKInterface::GetApiVersion() const
    {
        auto* api = Api();
        return api ? api->getVersion() : 0;
    }

    bool FRIKInterface::GetIndexFingerTipPosition(bool isLeft, RE::NiPoint3& outPos) const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;
        auto* api = Api();
        if (!api || !api->getIndexFingerTipPosition || !api->isSkeletonReady) return false;
        if (!api->isSkeletonReady()) return false;

        outPos = api->getIndexFingerTipPosition(ToHand(isLeft));
        return true;
    }

    bool FRIKInterface::GetHandPosition(bool isLeft, RE::NiPoint3& outPos) const
    {
        // DISABLED: Skeleton access via GetObjectByName can race with engine's UpdateDownwardPass
        // Always use FRIK API fallback instead
        return GetIndexFingerTipPosition(isLeft, outPos);
    }

    bool FRIKInterface::SetHandPoseFingerPositions(bool isLeft, float thumb, float index, float middle, float ring, float pinky) const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;
        auto* api = Api();
        if (!api) return false;

        // Use tag-based API
        if (api->setHandPoseCustomFingerPositions) {
            return api->setHandPoseCustomFingerPositions(HEISENBERG_HAND_POSE_TAG, ToHand(isLeft), thumb, index, middle, ring, pinky);
        }
        // Fallback to deprecated API
        if (api->setHandPoseFingerPositions) {
            api->setHandPoseFingerPositions(ToHand(isLeft), thumb, index, middle, ring, pinky);
            return true;
        }

        return false;
    }

    bool FRIKInterface::SetHandPoseJointPositions(bool isLeft, const float values[15]) const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;

        // Average 3 joints per finger into 5 per-finger values
        float avg[5];
        for (int f = 0; f < 5; f++) {
            avg[f] = (values[f * 3] + values[f * 3 + 1] + values[f * 3 + 2]) / 3.0f;
        }
        return SetHandPoseFingerPositions(isLeft, avg[0], avg[1], avg[2], avg[3], avg[4]);
    }

    bool FRIKInterface::ClearHandPoseFingerPositions(bool isLeft) const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;
        auto* api = Api();
        if (!api) return false;

        // Use tag-based API
        if (api->clearHandPose) {
            return api->clearHandPose(HEISENBERG_HAND_POSE_TAG, ToHand(isLeft));
        }
        // Fallback to deprecated API
        if (api->clearHandPoseFingerPositions) {
            api->clearHandPoseFingerPositions(ToHand(isLeft));
            return true;
        }

        return false;
    }

    bool FRIKInterface::IsConfigOpen() const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;
        auto* api = Api();
        return api && api->isConfigOpen && api->isConfigOpen();
    }

    bool FRIKInterface::IsOffHandGrippingWeapon() const
    {
        if (DEBUG_DISABLE_FRIK_API) return false;
        auto* api = Api();
        return api && api->isOffHandGrippingWeapon && api->isOffHandGrippingWeapon();
    }

    const char* FRIKInterface::GetModVersion() const
    {
        auto* api = Api();
        if (api && api->getModVersion) {
            return api->getModVersion();
        }
        return "unknown";
    }

    // --- v9 hand pushback API -------------------------------------------------------------
    // The external-hand-transform functions were introduced at FRIK "ROCK API v5". The header
    // we compile against is v9 (newer), but the API is append-only so these functions sit at
    // the same struct offset on v5+. We gate on getVersion() >= 5 and SEH-guard the raw calls:
    // if a layout mismatch makes the pointer bogus, the fault is caught and pushback disables
    // permanently instead of crashing.
    namespace
    {
        constexpr std::uint32_t kPushbackMinVersion = 5;  // applyExternalHandWorldTransform added here
        bool s_pushbackFaulted = false;
        bool s_loggedDisable = false;

        // SEH leaves (no C++ objects needing unwind — NiTransform is trivially destructible).
        bool ApplyExtSEH(const frik::host_api::FRIKApi* api, const char* tag, frik::host_api::FRIKApi::Hand h,
                         const RE::NiTransform& w, int prio)
        {
            __try { return api->applyExternalHandWorldTransform(tag, h, w, prio); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool ClearExtSEH(const frik::host_api::FRIKApi* api, const char* tag, frik::host_api::FRIKApi::Hand h)
        {
            __try { return api->clearExternalHandWorldTransform(tag, h); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool GetHandWorldSEH(const frik::host_api::FRIKApi* api, frik::host_api::FRIKApi::Hand h, RE::NiTransform& out)
        {
            __try { out = api->getHandWorldTransform(h); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    bool FRIKInterface::SupportsPushback() const
    {
        if (DEBUG_DISABLE_FRIK_API || s_pushbackFaulted) return false;
        auto* api = Api();
        return api && api->getVersion && api->getVersion() >= kPushbackMinVersion;
    }

    bool FRIKInterface::GetHandWorldTransform(bool isLeft, RE::NiTransform& outWorld) const
    {
        // Native FRIK v5 path.
        if (SupportsPushback()) {
            auto* api = Api();
            if (api->getHandWorldTransform && api->isSkeletonReady && api->isSkeletonReady()) {
                if (GetHandWorldSEH(api, ToHand(isLeft), outWorld)) return true;
                s_pushbackFaulted = true;
                if (!s_loggedDisable) { spdlog::error("[FRIK] pushback call faulted (API layout mismatch?) — disabling pushback"); s_loggedDisable = true; }
            }
            return false;
        }
        // Older FRIK (no v5): our plugin-side hand authority owns the rendered hand.
        return HandAuthority::GetRenderedHand(isLeft, outWorld);
    }

    bool FRIKInterface::ApplyExternalHandWorldTransform(bool isLeft, const RE::NiTransform& world, int priority) const
    {
        if (SupportsPushback()) {
            auto* api = Api();
            if (!api->applyExternalHandWorldTransform) return false;
            return ApplyExtSEH(api, HEISENBERG_HAND_PUSHBACK_TAG, ToHand(isLeft), world, priority);
        }
        // Older FRIK (no v5): route to our plugin-side authority (applied at the post-physics hook).
        return HandAuthority::Apply(HEISENBERG_HAND_PUSHBACK_TAG, isLeft, world, priority);
    }

    bool FRIKInterface::ClearExternalHandWorldTransform(bool isLeft) const
    {
        if (SupportsPushback()) {
            auto* api = Api();
            if (!api->clearExternalHandWorldTransform) return false;
            return ClearExtSEH(api, HEISENBERG_HAND_PUSHBACK_TAG, ToHand(isLeft));
        }
        return HandAuthority::Clear(HEISENBERG_HAND_PUSHBACK_TAG, isLeft);
    }
}
