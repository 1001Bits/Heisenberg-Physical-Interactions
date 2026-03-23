#include "FRIKInterface.h"

namespace heisenberg
{
    using FRIKApi = frik::api::FRIKApi;

    bool FRIKInterface::Initialize()
    {
        if (_initialized) {
            return Api() != nullptr;
        }

        _initialized = true;

        // Use the official FRIK API initialization
        const int err = FRIKApi::initialize();
        
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
            spdlog::error("FRIK API version too old - Heisenberg requires API v{}", frik::api::FRIK_API_VERSION);
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
}
