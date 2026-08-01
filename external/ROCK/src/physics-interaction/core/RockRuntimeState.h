#pragma once

#include "RE/NetImmerse/NiPoint.h"
#include "RE/NetImmerse/NiTransform.h"

#include <cstdint>

namespace rock
{
    struct DirectSkeletonBoneSnapshot;
}

namespace rock::runtime_state
{
    struct PlayerSpaceFrame
    {
        bool valid = false;
        bool moving = false;
        const char* source = "none";
        RE::NiTransform world{};
        RE::NiPoint3 deltaGameUnits{};
    };

    struct RuntimeFrameInput
    {
        bool menuInputBlocking = false;
        bool visualAuthorityAvailable = false;
        bool visualSkeletonReadyHint = false;
        bool compatibilityConfigBlocking = false;
    };

    struct RuntimeFrameSnapshot
    {
        std::uint64_t frameIndex = 0;
        float deltaSeconds = 1.0f / 90.0f;
        bool playerAvailable = false;
        bool weaponDrawn = false;
        bool localMenuBlocking = false;
        bool localScopeMenuOpen = false;
        bool localLoadingMenuOpen = false;
        bool localGameStopped = false;
        bool inputMenuBlocking = false;
        bool compatibilityConfigBlocking = false;
        bool visualAuthorityAvailable = false;
        bool visualSkeletonReadyHint = false;
        bool localSkeletonReady = false;
        // EMBEDDED-HOST SEAM: readiness detail the host's boot diagnostics print.
        bool localSkeletonRootNodeAvailable = false;
        bool localSkeletonRootAttached = false;
        bool localSkeletonFlattenedTreeValid = false;
        bool localSkeletonRequiredHandBonesReady = false;
        std::uintptr_t localSkeletonIdentity = 0;
        std::uintptr_t localSkeletonBoneTreeIdentity = 0;
        bool localSkeletonInPowerArmor = false;
        int localSkeletonCapturedBoneCount = 0;
        int localSkeletonRequiredResolvedCount = 0;
        PlayerSpaceFrame playerSpace{};
    };

    void initialize();
    void resetTransientState();
    void updateFrame(const RuntimeFrameInput& input);

    [[nodiscard]] const RuntimeFrameSnapshot& currentFrame();
    /*
     * Immutable for the lifetime of currentFrame(). Main-loop consumers may
     * inspect it synchronously, but must not retain the pointer across the next
     * updateFrame/resetTransientState call. The frame-generation check in the
     * implementation prevents a stale skeleton snapshot from crossing a
     * lifecycle boundary.
     */
    [[nodiscard]] const DirectSkeletonBoneSnapshot* currentSkeletonSnapshot();
    [[nodiscard]] bool isLocalSkeletonReady();
    [[nodiscard]] bool isPhysicsMenuBlocked();
    [[nodiscard]] bool isCompatibilityConfigBlocked();
    [[nodiscard]] float deltaSeconds();
}
