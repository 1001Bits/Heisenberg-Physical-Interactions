/*
 * ROCK performance profiling is diagnostics-only because the interaction stack
 * must not let wall-clock timing influence gameplay state. The alternative was
 * to scatter timer calls directly through hand, body, weapon, and overlay code;
 * centralizing the boundary here keeps the high-resolution clock isolated in
 * the implementation file while still giving future optimization passes
 * measured subsystem costs.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rock::performance_profiler
{
    enum class Scope : std::uint8_t
    {
        FrameUpdate = 0,
        PhysicsInteractionUpdate,
        HandColliderUpdate,
        BodyColliderUpdate,
        GeneratedColliderPhysicsFlush,
        CharacterControllerContactFilter,
        WeaponCollision,
        WeaponCollisionTransforms,
        GeneratedBodyContactRegistry,
        WeaponColliderBuild,
        WeaponColliderCreate,
        TwoHandedGripFrame,
        TwoHandedGripStart,
        EquippedWeaponFingerPoseCapture,
        SupportGripSuppression,
        HostHandAuthorityApply,
        LegacyFingerPoseApply,
        PipboyFrame,
        PipboyDeckContactQuery,
        SelectionCasts,
        DynamicHandCollisionFrame,
        DynamicHandCollisionPhysicsDrive,
        DynamicHandCollisionPostSolve,
        DebugOverlayPublish,
        DebugOverlayRender,
        ContactResolve,
        SoftContact,
        NativeContactCallback,
        GrabAcquisitionBodyScan,
        GrabAcquisitionActivePrep,
        GrabMeshExtraction,
        GrabNearbyDampingBegin,
        GrabHeldObjectUpdate,
        GrabAuthorityFlush,
        GrabAuthorityAfterSolveDiagnostics,
        Count
    };

    enum class Counter : std::uint8_t
    {
        WeaponRebuildQueued = 0,
        WeaponRebuildCanceled,
        WeaponRebuildCompleted,
        WeaponRebuildVisualRootDeferred,
        WeaponRebuildVisualStableWait,
        WeaponRebuildVisualSourceUnavailableRetained,
        WeaponRebuildVisualSourceUnavailableRetainExpired,
        WeaponRebuildReasonSettingsChanged,
        WeaponRebuildReasonDriveRequested,
        WeaponRebuildReasonKeyChanged,
        WeaponRebuildReasonMissingBodies,
        WeaponKeyChangeVisualOnly,
        WeaponKeyChangeIdentityOnly,
        WeaponKeyChangeVisualAndIdentity,
        GrabAcquisitionCachePrewarm,
        GrabAcquisitionCacheHit,
        GrabAcquisitionCacheMiss,
        GrabAcquisitionCacheInvalidated,
        Count
    };

    enum class ValueMetric : std::uint8_t
    {
        WeaponBuildVisibleTriShapes = 0,
        WeaponBuildGeneratedSources,
        WeaponBuildBodiesCreated,
        WeaponBuildTransientReloadSources,
        WeaponBuildBodyCount,
        GrabAcquisitionVisitedNodes,
        GrabAcquisitionCollisionObjects,
        GrabAcquisitionBodyIds,
        GrabMeshTriangles,
        GrabNearbyDampingMotions,
        EquippedWeaponFingerPoseSourceTriangles,
        EquippedWeaponFingerPoseSelectedTriangles,
        EquippedWeaponFingerPoseSpatialNodeVisits,
        EquippedWeaponFingerPoseTriangleTests,
        Count
    };

    inline constexpr std::size_t kOverlayMaxLines = 8;
    inline constexpr std::size_t kOverlayLineLength = 128;
    using OverlayLines = std::array<std::array<char, kOverlayLineLength>, kOverlayMaxLines>;

    void refreshSettings(bool enabled, int logIntervalFrames, int warmupFrames, bool overlayTextEnabled) noexcept;
    /*
     * Lightweight end-to-end frame-pacing benchmark, independent of the
     * detailed scope profiler above:
     *   0 = off
     *   1 = A / full ROCK runtime
     *   2 = B / host-selected ROCK live-work bypass
     *
     * Call refreshBenchmarkSettings on the game thread before the frame
     * boundary. Call observeFrameBoundary exactly once at a stable point in
     * each engine frame. `eligible=false` excludes menus, loading, and runtime
     * transition frames and breaks the timing chain so their elapsed time
     * cannot leak into the next sample.
     *
     * The first eligible boundary establishes a QPC baseline. The configured
     * warmup then consumes eligible frame deltas; subsequent deltas populate a
     * fixed histogram until logIntervalFrames samples have been collected.
     * Output is queued to the existing asynchronous ROCK_Profiler.log writer.
     * Enabling this benchmark does not enable ScopedTimer instrumentation.
     */
    void refreshBenchmarkSettings(int mode, int logIntervalFrames, int warmupFrames) noexcept;
    void observeFrameBoundary(bool eligible) noexcept;
    [[nodiscard]] int benchmarkMode() noexcept;

    void beginFrame() noexcept;
    void endFrame() noexcept;
    void addEventCount(Scope scope, std::uint64_t count = 1) noexcept;
    void addCounter(Counter counter, std::uint64_t count = 1) noexcept;
    void observeValue(ValueMetric metric, std::uint64_t value) noexcept;
    bool overlayTextEnabled() noexcept;
    std::uint32_t copyOverlayLines(OverlayLines& outLines) noexcept;

    class ScopedTimer
    {
    public:
        explicit ScopedTimer(Scope scope) noexcept;
        ~ScopedTimer();

        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;
        ScopedTimer(ScopedTimer&&) = delete;
        ScopedTimer& operator=(ScopedTimer&&) = delete;

        void stop() noexcept;

    private:
        Scope _scope{ Scope::Count };
        std::uint64_t _startTicks{ 0 };
        bool _active{ false };
    };

    class FrameScope
    {
    public:
        FrameScope() noexcept;
        ~FrameScope();

        FrameScope(const FrameScope&) = delete;
        FrameScope& operator=(const FrameScope&) = delete;
        FrameScope(FrameScope&&) = delete;
        FrameScope& operator=(FrameScope&&) = delete;

    private:
        ScopedTimer _timer;
    };
}
