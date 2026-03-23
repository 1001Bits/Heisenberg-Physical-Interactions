#pragma once

/**
 * WaterInteraction - Realistic water ripple and splash effects for VR hands
 * 
 * Ported from Water Interactions VR (Skyrim VR mod by Shizof)
 * Adapted for Fallout 4 VR using F4SE/CommonLibF4 APIs.
 *
 * Features:
 * - Per-hand water entry/exit detection with speed-based splash amounts
 * - 5-band splash system: VeryLight, Light, Normal, Hard, VeryHard
 * - Wake ripples while hand is submerged and moving
 * - Hover detection (hand near water surface)
 * - Velocity estimation from position sample history
 * - Splash VFX/SFX via TaskQueueInterface::QueueWaterSplashEffects
 * - Player depth suspension (deep water = disable hand detection)
 * - Sneak depth suppression
 * - Fast travel detection (suppress during high player speed)
 *
 * Integration: Called from Heisenberg::OnGrabUpdate() every frame.
 * Uses F4VR functions:
 *   - TESObjectCELL::GetWaterHeight (0x39b460) for water level detection
 *   - TaskQueueInterface::QueueAddRipple (0x0dac660) for visual ripples
 *   - TaskQueueInterface::QueueWaterSplashEffects (0x0dac570) for splash VFX/SFX
 */

#include "RE/Fallout.h"

#include <chrono>
#include <deque>

namespace heisenberg
{
    // =========================================================================
    // Splash Bands - speed-based intensity tiers
    // =========================================================================

    enum class SplashBand : int
    {
        VeryLight = 0,
        Light,
        Normal,
        Hard,
        VeryHard,
        Count
    };

    // =========================================================================
    // Water Interaction Config - tunable parameters
    // =========================================================================

    struct WaterConfig
    {
        // Master toggle
        bool enabled = true;

        // Movement detection
        float movingConfirmSeconds = 1.0f;      // How long above threshold to confirm "moving"
        float movingThreshold = 0.08f;          // m/s - threshold for "moving" vs "stationary"
        float stationaryConfirmSeconds = 1.5f;  // How long below threshold to confirm "stationary"
        float maxValidSpeed = 60.0f;            // m/s - ignore speed above this (tracking glitch)

        // Entry/exit detection
        float entryDownZThreshold = 0.5f;       // Minimum downward speed for entry splash
        float exitUpZThreshold = 0.5f;          // Minimum upward speed for exit splash
        float maxEntryDownSpeed = 1500.0f;      // Ignore impossibly fast entries
        float maxExitUpSpeed = 900.0f;          // Ignore impossibly fast exits

        // Entry splash bands (thresholds in game units/s)
        float splashVeryLightMax = 30.0f;
        float splashLightMax = 60.0f;
        float splashNormalMax = 1500.0f;
        float splashHardMax = 4500.0f;

        // Entry splash amounts (ripple radius for AddRipple displacement)
        // F4VR needs ~5x Skyrim values for visibility. Above ~0.20 causes
        // straight-line artifacts from overdriving the displacement mesh.
        float splashVeryLightAmt = 0.05f;
        float splashLightAmt = 0.08f;
        float splashNormalAmt = 0.10f;
        float splashHardAmt = 0.12f;
        float splashVeryHardAmt = 0.15f;

        // Exit splash amounts
        float splashExitVeryLightAmt = 0.05f;
        float splashExitLightAmt = 0.08f;
        float splashExitNormalAmt = 0.10f;
        float splashExitHardAmt = 0.12f;
        float splashExitVeryHardAmt = 0.15f;

        // Exit splash band thresholds (same as entry by default)
        float splashExitVeryLightMax = 30.0f;
        float splashExitLightMax = 60.0f;
        float splashExitNormalMax = 1500.0f;
        float splashExitHardMax = 4500.0f;

        // Global splash scale multiplier
        float splashScale = 1.0f;

        // Wake ripples (continuous ripples while submerged + moving)
        bool wakeEnabled = true;
        float wakeAmt = 0.008f;                // Base wake ripple radius. UV-space value: ring world radius ≈ wakeAmt * displacement_texture_radius.
                                               // At 0.008 with ~512 unit texture radius = ~4 world unit rings. Skyrim used 0.009; F4VR same scale.
                                               // Old value 0.03 created ~31-unit rings that merged into linear bands when spaced 20 units apart.
        float wakeMinDepth = 2.0f;             // Game units - minimum depth for wake ripples
        int wakeIntervalMs = 0;                // Min ms between wake ripples (0 = every frame)
        float wakeScaleMultiplier = 0.06f;     // Speed * this = scale factor
        float wakeMinMultiplier = 0.5f;        // Min scale applied to wakeAmt
        float wakeMaxMultiplier = 1.5f;        // Max scale applied to wakeAmt (reduced from 2.0 to keep rings small at high speed)
        float wakeMinDistance = 150.0f;        // Game units - minimum distance between wake ripple positions.
                                               // At 150 units spacing with ~4-6 unit ring radius, rings are well-separated = clean concentric circles.
                                               // Old value 20 caused heavily overlapping rings (radius > spacing) = continuous linear band effect.

        // Hover detection (hand near water surface)
        float hoverMaxHeight = 30.0f;          // Game units - max height above water for "hovering"
        float hoverBelowTolerance = 3.0f;      // Game units - how far below surface still counts

        // Player depth thresholds
        float playerDepthShutdown = 90.0f;     // Game units - deep water = disable hand detection
        float sneakDepthShutdown = 65.0f;      // Game units - sneak depth cutoff
        float playerSpeedShutdown = 220.0f;    // Speed above this = fast travel, suspend

        // Splash effect (visual VFX + SFX via QueueWaterSplashEffects)
        bool enableSplashEffects = true;
        float splashEffectEntryMagnitude = 300.0f;  // Splash magnitude for hand entry
        float splashEffectExitMagnitude = 150.0f;   // Splash magnitude for hand exit
        bool enableSplashNif = true;                // Enable waterSplash.NIF particle (false = sound only)
        float splashNifScale = 1.0f;                // Scale multiplier for waterSplash.NIF
    };

    // =========================================================================
    // Per-hand tracking state
    // =========================================================================

    struct HandWaterState
    {
        // Submerged state
        bool isSubmerged = false;
        bool wasSubmerged = false;
        float waterHeight = -10000.0f;
        float prevWaterHeight = -10000.0f;
        float depth = 0.0f;                    // How deep below water surface

        // Position tracking for velocity estimation
        struct Sample
        {
            RE::NiPoint3 pos{ 0.0f, 0.0f, 0.0f };
            std::chrono::steady_clock::time_point time;
        };
        std::deque<Sample> samples;
        static constexpr size_t kMaxSamples = 7;

        RE::NiPoint3 prevPos{ 0.0f, 0.0f, 0.0f };
        bool hasPrevPos = false;
        float recentSpeed = 0.0f;

        // Movement state
        bool isMoving = false;
        std::chrono::steady_clock::time_point lastMovementTime;
        std::chrono::steady_clock::time_point movementCandidateTime;
        bool movementCandidateActive = false;

        // Ripple timing
        std::chrono::steady_clock::time_point lastWakeTime;
        std::chrono::steady_clock::time_point lastTransitionTime;
        RE::NiPoint3 lastWakePos{ 0.0f, 0.0f, 0.0f };
        bool hasLastWakePos = false;

        // Hover state
        bool isHovering = false;
        float hoverHeight = 0.0f;              // Height above water (positive = above)

        // Sneak suppression
        bool suppressDueToSneakDepth = false;

        // Detection active
        bool detectionActive = true;

        void Reset()
        {
            isSubmerged = false;
            wasSubmerged = false;
            waterHeight = -10000.0f;
            prevWaterHeight = -10000.0f;
            depth = 0.0f;
            samples.clear();
            prevPos = { 0.0f, 0.0f, 0.0f };
            hasPrevPos = false;
            recentSpeed = 0.0f;
            isMoving = false;
            movementCandidateActive = false;
            isHovering = false;
            hoverHeight = 0.0f;
            suppressDueToSneakDepth = false;
            detectionActive = true;
            lastWakePos = { 0.0f, 0.0f, 0.0f };
            hasLastWakePos = false;
        }
    };

    // =========================================================================
    // WaterInteraction Singleton
    // =========================================================================

    class WaterInteraction
    {
    public:
        static WaterInteraction& GetSingleton()
        {
            static WaterInteraction instance;
            return instance;
        }

        // Lifecycle
        void Initialize();
        void Update(float deltaTime);
        void ClearState();

        // Public accessors for other systems
        bool IsLeftHandInWater() const { return _leftHand.isSubmerged; }
        bool IsRightHandInWater() const { return _rightHand.isSubmerged; }
        float GetLeftHandDepth() const { return _leftHand.depth; }
        float GetRightHandDepth() const { return _rightHand.depth; }
        bool IsLeftHandHovering() const { return _leftHand.isHovering; }
        bool IsRightHandHovering() const { return _rightHand.isHovering; }

        // Config access
        WaterConfig& GetConfig() { return _config; }
        const WaterConfig& GetConfig() const { return _config; }

    private:
        WaterInteraction() = default;

        // Per-hand state
        HandWaterState _leftHand;
        HandWaterState _rightHand;
        bool _initialized = false;
        bool _suspended = false;
        bool _suspendedDueToSneak = false;

        // Player tracking
        float _playerDepth = 0.0f;
        float _playerSpeed = 0.0f;
        RE::NiPoint3 _prevPlayerPos{ 0.0f, 0.0f, 0.0f };
        bool _hasPrevPlayerPos = false;

        // Per-frame timestamp (captured once in Update, reused by subroutines)
        std::chrono::steady_clock::time_point _frameNow;

        // Config
        WaterConfig _config;

        // Core functions
        float GetWaterHeightAt(const RE::NiPoint3& pos);
        void EmitRipple(const RE::NiPoint3& pos, float radius);
        void EmitSplashEffect(const RE::NiPoint3& pos, float magnitude);

        // Per-hand processing
        void UpdateHand(HandWaterState& state, RE::NiNode* handNode, 
                        float deltaTime, bool isLeft);
        void UpdateHandVelocity(HandWaterState& state, const RE::NiPoint3& pos);
        void UpdateHandMovement(HandWaterState& state);
        void UpdateHandWaterState(HandWaterState& state, const RE::NiPoint3& pos);
        void UpdateHandHover(HandWaterState& state, const RE::NiPoint3& pos);
        void HandleHandEntry(HandWaterState& state, const RE::NiPoint3& pos, 
                            bool isLeft);
        void HandleHandExit(HandWaterState& state, const RE::NiPoint3& pos,
                           bool isLeft);
        void HandleWakeRipples(HandWaterState& state, const RE::NiPoint3& pos,
                              bool isLeft);

        // Splash NIF scale override
        void ApplySplashNifScales();
        bool _lastNifEnabled = true;
        float _lastNifScale = 1.0f;

        // Splash amount computation
        float ComputeEntrySplashAmount(float downSpeed) const;
        float ComputeExitSplashAmount(float upSpeed) const;
    };

} // namespace heisenberg
