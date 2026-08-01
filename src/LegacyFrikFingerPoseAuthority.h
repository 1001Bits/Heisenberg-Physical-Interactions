#pragma once

// Heisenberg-owned compatibility backend for the full finger-pose portion of
// FRIK's v5 API.  Stock FRIK 0.77.12 exposes API v3 and can consume only five
// curl scalars; embedded ROCK retains its canonical pose here and this module
// applies the richer result after FRIK's own skeleton pass.
//
// Payloads remain opaque at this boundary so the host table is layout-compatible
// with rock::HostFingerPoseAuthority without importing ROCK's FRIK API type into
// Heisenberg headers:
//   pose            = frik::api::FRIKApi::HandPoseData
//   localTransforms = frik::api::FRIKApi::FingerLocalTransformOverride
// hand is physical: 0=Right, 1=Left.

namespace rock
{
    struct HostFingerPoseAuthority;
}

namespace heisenberg
{
    class LegacyFrikFingerPoseAuthority
    {
    public:
        static bool ApplyPose(const char* tag, int hand, const void* pose, int priority);
        static bool BuildPoseLocalTransforms(int hand, const void* pose, void* outLocalTransforms);
        static bool ApplyLocalTransforms(const char* tag, int hand, const void* localTransforms, int priority);
        static bool Clear(const char* tag, int hand);
        static bool IsActive(const char* tag, int hand);

        // Game-thread scene-graph pass. Call after FRIK and after any late hand
        // world-authority write so proximal finger worlds inherit the final wrist.
        static void ApplyWinners();

        // Skeleton teardown / save-load / shutdown.
        static void Reset();

        static const rock::HostFingerPoseAuthority& HostTable();

    private:
        LegacyFrikFingerPoseAuthority() = default;
    };
}
