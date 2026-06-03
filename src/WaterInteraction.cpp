/**
 * WaterInteraction.cpp - Realistic water ripple and splash effects for VR hands
 *
 * Ported from Water Interactions VR (Skyrim VR mod by Shizof)
 * Adapted for Fallout 4 VR using F4SE/CommonLibF4 APIs.
 *
 * Key differences from Skyrim version:
 * - No background thread: runs in Heisenberg's frame update loop
 * - Uses F4VR TESWaterSystem::AddRipple directly (found via runtime singleton scan)
 * - Uses F4VR TESObjectCELL::GetWaterHeight instead of Skyrim's cell->GetWaterHeight
 * - Uses F4VR QueueWaterSplashEffects for splash VFX/SFX (queued through TaskQueueInterface)
 * - Uses FRIK/Heisenberg wand nodes instead of Skyrim hand skeleton nodes
 * - No spell interaction (F4VR magic system differs)
 * - No ESP dependency (no custom sound forms needed)
 */

#include "WaterInteraction.h"
#include "Config.h"
#include "WandNodeHelper.h"
#include "f4vr/PlayerNodes.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#undef max
#undef min

namespace heisenberg
{
    // =========================================================================
    // FUNCTION OFFSETS (F4VR 1.2.72)
    // =========================================================================

    // TESObjectCELL::GetWaterHeight(NiPoint3& pos, float& outHeight, BSPointerHandle*)
    // VR Offset: 0x39b460 (Status 4 - Verified) | ID: 486913
    using GetWaterHeight_t = bool (*)(RE::TESObjectCELL* cell, RE::NiPoint3& pos,
                                      float& outHeight, void* outRefHandle);
    inline REL::Relocation<GetWaterHeight_t> Cell_GetWaterHeight{ REL::Offset(0x39b460) };

    // TaskQueueInterface::QueueWaterSplashEffects(float magnitude, NiPoint3& position)
    // VR Offset: 0x0dac570 (Status 2) | ID: 60360
    using QueueWaterSplashEffects_t = void (*)(RE::TaskQueueInterface* taskQueue,
                                               float magnitude, RE::NiPoint3& position);
    inline REL::Relocation<QueueWaterSplashEffects_t> TaskQueue_WaterSplashEffects{
        REL::Offset(0x0dac570)
    };

    // =========================================================================
    // TESWaterSystem::AddRipple - DIRECT ripple creation (bypasses task queue)
    // =========================================================================
    //
    // Found via Ghidra decompilation of the task dispatcher (FUN_140db0e00):
    // QueueAddRipple creates task type 0x25, and case 0x25 calls FUN_1407cea00
    // with the TESWaterSystem singleton and the queued NiPoint3+float args.
    //
    // VR: 0x7cea00 (confirmed from Ghidra task dispatcher case 0x25)

    using AddRipple_t = void (*)(void* waterSystem, RE::NiPoint3& pos, float amount);
    inline REL::Relocation<AddRipple_t> WaterSystem_AddRipple{ REL::Offset(0x7cea00) };

    // TESWaterSystem singleton pointer (direct global at 0x5932540)
    inline REL::Relocation<void**> WaterSystem_SingletonPtr{ REL::Offset(0x5932540) };

    // Ripple enable flag - checked by AddRipple before creating displacement
    inline REL::Relocation<uint8_t*> WaterSystem_RippleEnableFlag{ REL::Offset(0x3772c98) };

    // BGSWaterCollisionManager::PlaySplashEffects magnitude thresholds (game settings)
    // These determine which VFX tier is used: CWaterLarge, CWaterMedium, CWaterSmall
    inline REL::Relocation<float*> SplashThreshold_Large{ REL::Offset(0x37536f8) };
    inline REL::Relocation<float*> SplashThreshold_Medium{ REL::Offset(0x3753710) };
    inline REL::Relocation<float*> SplashThreshold_Small{ REL::Offset(0x3753728) };
    // Scale factors applied to waterSplash.NIF per tier
    inline REL::Relocation<float*> SplashScale_Large{ REL::Offset(0x37537a0) };
    inline REL::Relocation<float*> SplashScale_Medium{ REL::Offset(0x37537b8) };
    inline REL::Relocation<float*> SplashScale_Small{ REL::Offset(0x37537d0) };

    // Water displacement flags (forced at startup in Heisenberg.cpp message handler)
    inline REL::Relocation<uint8_t*> WaterDisplacement_MasterEnable{ REL::Offset(0x37729c8) };
    inline REL::Relocation<uint8_t*> WaterDisplacement_SettingsFlag{ REL::Offset(0x3772c80) };

    // Cached TESWaterSystem singleton
    static void* g_TESWaterSystem = nullptr;
    static bool  g_TESWaterSystemSearched = false;

    // Original engine splash NIF scales (read once, used as base for user scaling)
    static float s_origScaleLarge = -1.0f;
    static float s_origScaleMedium = -1.0f;
    static float s_origScaleSmall = -1.0f;

    // SEH-safe helper: reads singleton pointer and enable flag without C++ objects
    // Returns: 0 = exception, 1 = singleton null, 2 = singleton found (flag on),
    //          3 = singleton found (flag was off, forced to 1)
    static int ReadWaterSystemSingleton_SEH(
        void** outSingleton, uintptr_t* outPtrAddr, uintptr_t* outSingletonAddr,
        uintptr_t* outFlagAddr, uint8_t* outFlagValue)
    {
        __try
        {
            void** pSingleton = WaterSystem_SingletonPtr.get();
            *outPtrAddr = reinterpret_cast<uintptr_t>(pSingleton);
            void* singleton = *pSingleton;

            if (!singleton)
            {
                *outSingleton = nullptr;
                *outSingletonAddr = 0;
                return 1;
            }

            *outSingleton = singleton;
            *outSingletonAddr = reinterpret_cast<uintptr_t>(singleton);

            uint8_t* pFlag = WaterSystem_RippleEnableFlag.get();
            *outFlagAddr = reinterpret_cast<uintptr_t>(pFlag);
            *outFlagValue = *pFlag;

            if (*pFlag == 0)
            {
                *pFlag = 1;
                return 3;
            }
            return 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *outSingleton = nullptr;
            return 0;
        }
    }

    static void* FindTESWaterSystemSingleton()
    {
        if (g_TESWaterSystemSearched)
            return g_TESWaterSystem;
        g_TESWaterSystemSearched = true;

        void* singleton = nullptr;
        uintptr_t ptrAddr = 0, singletonAddr = 0, flagAddr = 0;
        uint8_t flagValue = 0;

        int result = ReadWaterSystemSingleton_SEH(
            &singleton, &ptrAddr, &singletonAddr, &flagAddr, &flagValue);

        switch (result)
        {
        case 0:
            spdlog::error("[Water] Exception reading TESWaterSystem singleton!");
            break;
        case 1:
            spdlog::info("[Water] TESWaterSystem singleton = NULL (not yet initialized?)");
            break;
        case 2:
            g_TESWaterSystem = singleton;
            spdlog::info("[Water] TESWaterSystem singleton at {:#x}", singletonAddr);
            break;
        case 3:
            g_TESWaterSystem = singleton;
            spdlog::info("[Water] TESWaterSystem singleton at {:#x}", singletonAddr);
            spdlog::warn("[Water] Ripple enable flag was 0 - forced to 1");
            break;
        }

        return g_TESWaterSystem;
    }

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    void WaterInteraction::Initialize()
    {
        if (_initialized)
            return;

        spdlog::info("[Water] Initializing water interaction system");

        // Validate TaskQueueInterface singleton
        auto* taskQueue = RE::TaskQueueInterface::GetSingleton();
        if (!taskQueue)
            spdlog::error("[Water] TaskQueueInterface singleton is NULL! Splash effects will not work.");

        // Read TESWaterSystem singleton
        void* waterSystem = FindTESWaterSystemSingleton();
        if (!waterSystem)
            spdlog::warn("[Water] TESWaterSystem singleton = NULL (will use QueueAddRipple fallback)");

        // Verify displacement flags are on (primary forcing is in Heisenberg.cpp message handler)
        __try
        {
            uint8_t master = *WaterDisplacement_MasterEnable.get();
            uint8_t settings = *WaterDisplacement_SettingsFlag.get();
            spdlog::info("[Water] Displacement flags: masterEnable={} settingsFlag={}", master, settings);
            if (master == 0 || settings == 0)
                spdlog::warn("[Water] Displacement flags not fully enabled - ripples may not render");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Log splash VFX thresholds and store original NIF scales for user override
        __try
        {
            float tLarge = *SplashThreshold_Large.get();
            float tMedium = *SplashThreshold_Medium.get();
            float tSmall = *SplashThreshold_Small.get();
            s_origScaleLarge = *SplashScale_Large.get();
            s_origScaleMedium = *SplashScale_Medium.get();
            s_origScaleSmall = *SplashScale_Small.get();
            spdlog::info("[Water] Splash VFX thresholds: Large>{:.0f} Medium>{:.0f} Small>{:.0f}",
                         tLarge, tMedium, tSmall);
            spdlog::info("[Water] Splash VFX scales (original): Large={:.2f} Medium={:.2f} Small={:.2f}",
                         s_origScaleLarge, s_origScaleMedium, s_origScaleSmall);
            spdlog::info("[Water] Our magnitudes: entry={:.0f} exit={:.0f}",
                         _config.splashEffectEntryMagnitude, _config.splashEffectExitMagnitude);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Apply initial NIF scale settings
        ApplySplashNifScales();

        _leftHand.Reset();
        _rightHand.Reset();
        _initialized = true;

        spdlog::info("[Water] Water interaction system initialized (wakeAmt={:.4f} wakeInterval={}ms)",
                     _config.wakeAmt, _config.wakeIntervalMs);
    }

    void WaterInteraction::ClearState()
    {
        _leftHand.Reset();
        _rightHand.Reset();
        _suspended = false;
        _suspendedDueToSneak = false;
        _playerDepth = 0.0f;
        _playerSpeed = 0.0f;
        _hasPrevPlayerPos = false;
    }

    // =========================================================================
    // MAIN UPDATE - called every frame from OnGrabUpdate
    // =========================================================================

    void WaterInteraction::Update(float deltaTime)
    {
        if (!_initialized || !_config.enabled)
            return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell)
            return;

        // Capture frame timestamp once — subroutines use _frameNow instead of calling now() repeatedly
        _frameNow = std::chrono::steady_clock::now();

        auto* playerNodes = f4cf::f4vr::getPlayerNodes();
        if (!playerNodes)
            return;

        RE::NiNode* leftWand = heisenberg::GetWandNode(playerNodes, true);
        RE::NiNode* rightWand = heisenberg::GetWandNode(playerNodes, false);

        if (!leftWand && !rightWand)
            return;

        // Periodic debug logging (every ~5 seconds at 90fps)
        static int s_debugCounter = 0;
        if (++s_debugCounter >= 450) {
            s_debugCounter = 0;
            float lWH = -10000.0f, rWH = -10000.0f;
            if (leftWand) lWH = GetWaterHeightAt(leftWand->world.translate);
            if (rightWand) rWH = GetWaterHeightAt(rightWand->world.translate);
            spdlog::info("[Water] Update running — L={} R={} LwH={:.1f} RwH={:.1f} Lsub={} Rsub={} spd={:.1f} depth={:.1f}",
                         leftWand ? "yes" : "no", rightWand ? "yes" : "no",
                         lWH, rWH,
                         _leftHand.isSubmerged, _rightHand.isSubmerged,
                         _playerSpeed, _playerDepth);
        }

        // ── Player depth / speed checks ──────────────────────────────────────

        RE::NiPoint3 playerPos = player->data.location;
        if (player->Get3D())
            playerPos = player->Get3D()->world.translate;

        // Estimate player speed for fast-travel detection
        if (_hasPrevPlayerPos)
        {
            float dx = playerPos.x - _prevPlayerPos.x;
            float dy = playerPos.y - _prevPlayerPos.y;
            float dz = playerPos.z - _prevPlayerPos.z;
            _playerSpeed = std::sqrt(dx * dx + dy * dy + dz * dz) / deltaTime;
        }
        _prevPlayerPos = playerPos;
        _hasPrevPlayerPos = true;

        if (_playerSpeed > _config.playerSpeedShutdown)
            return;

        // Apply NIF scale overrides if config changed (MCM toggle/slider)
        if (_config.enableSplashNif != _lastNifEnabled || _config.splashNifScale != _lastNifScale)
            ApplySplashNifScales();

        // Player depth check
        _playerDepth = 0.0f;
        {
            RE::NiPoint3 checkPos = playerPos;
            float wh = GetWaterHeightAt(checkPos);
            if (wh > -9000.0f)
            {
                _playerDepth = wh - playerPos.z;
                if (_playerDepth < 0.0f)
                    _playerDepth = 0.0f;
            }
        }

        // Deep water shutdown
        if (_playerDepth >= _config.playerDepthShutdown)
        {
            if (!_suspended)
            {
                _suspended = true;
                _suspendedDueToSneak = false;
            }
            return;
        }
        if (_suspended && !_suspendedDueToSneak)
            _suspended = false;

        // Sneak depth shutdown
        bool isSneaking = player->IsSneaking();
        if (_playerDepth >= _config.sneakDepthShutdown && isSneaking)
        {
            if (!_suspended)
            {
                _suspended = true;
                _suspendedDueToSneak = true;
            }
            return;
        }
        if (_suspendedDueToSneak && (_playerDepth < _config.sneakDepthShutdown || !isSneaking))
        {
            _suspendedDueToSneak = false;
            _suspended = false;
        }

        if (_suspended)
            return;

        // ── Per-hand updates ─────────────────────────────────────────────────

        if (leftWand)
            UpdateHand(_leftHand, leftWand, deltaTime, true);
        else
        {
            _leftHand.samples.clear();
            _leftHand.hasPrevPos = false;
        }

        if (rightWand)
            UpdateHand(_rightHand, rightWand, deltaTime, false);
        else
        {
            _rightHand.samples.clear();
            _rightHand.hasPrevPos = false;
        }
    }

    // =========================================================================
    // PER-HAND UPDATE
    // =========================================================================

    void WaterInteraction::UpdateHand(HandWaterState& state, RE::NiNode* handNode,
                                      float deltaTime, bool isLeft)
    {
        if (!state.detectionActive)
            return;

        RE::NiPoint3 handPos = handNode->world.translate;

        UpdateHandVelocity(state, handPos);
        UpdateHandMovement(state);
        UpdateHandWaterState(state, handPos);
        UpdateHandHover(state, handPos);

        // Sneak depth suppression
        bool isSneaking = RE::PlayerCharacter::GetSingleton()->IsSneaking();
        state.suppressDueToSneakDepth = isSneaking && state.depth >= 2.0f;

        // ── Detect entry/exit transitions ────────────────────────────────────

        if (state.isSubmerged && !state.wasSubmerged)
        {
            HandleHandEntry(state, handPos, isLeft);
        }
        else if (!state.isSubmerged && state.wasSubmerged)
        {
            HandleHandExit(state, handPos, isLeft);
        }

        // ── Wake ripples while submerged + moving ────────────────────────────

        if (state.isSubmerged)
        {
            HandleWakeRipples(state, handPos, isLeft);
        }

        state.wasSubmerged = state.isSubmerged;
        if (state.isSubmerged)
            state.prevWaterHeight = state.waterHeight;
    }

    // =========================================================================
    // VELOCITY ESTIMATION
    // =========================================================================

    void WaterInteraction::UpdateHandVelocity(HandWaterState& state,
                                              const RE::NiPoint3& pos)
    {
        auto now = _frameNow;

        state.samples.push_back({ pos, now });
        if (state.samples.size() > HandWaterState::kMaxSamples)
            state.samples.pop_front();

        if (state.samples.size() >= 2)
        {
            const auto& prev = state.samples[state.samples.size() - 2];
            const auto& cur = state.samples[state.samples.size() - 1];
            double dt =
                std::chrono::duration<double>(cur.time - prev.time).count();

            if (dt > 1e-6)
            {
                float dx = cur.pos.x - prev.pos.x;
                float dy = cur.pos.y - prev.pos.y;
                float dz = cur.pos.z - prev.pos.z;
                float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>(dt);

                if (speed <= _config.maxValidSpeed)
                    state.recentSpeed = speed;
            }
        }

        state.prevPos = pos;
        state.hasPrevPos = true;
    }

    // =========================================================================
    // MOVEMENT DETECTION (with hysteresis to avoid jitter)
    // =========================================================================

    void WaterInteraction::UpdateHandMovement(HandWaterState& state)
    {
        auto now = _frameNow;
        float threshold = _config.movingThreshold;

        if (!state.isMoving)
        {
            if (state.recentSpeed > threshold)
            {
                if (!state.movementCandidateActive)
                {
                    state.movementCandidateTime = now;
                    state.movementCandidateActive = true;
                }
                else
                {
                    float elapsed = std::chrono::duration<float>(
                                        now - state.movementCandidateTime)
                                        .count();
                    if (elapsed >= _config.movingConfirmSeconds)
                    {
                        state.isMoving = true;
                        state.movementCandidateActive = false;
                    }
                }
                state.lastMovementTime = now;
            }
            else
            {
                state.movementCandidateActive = false;
            }
        }
        else
        {
            if (state.recentSpeed > threshold)
                state.lastMovementTime = now;

            float sinceLastMovement =
                std::chrono::duration<float>(now - state.lastMovementTime).count();
            if (sinceLastMovement >= _config.stationaryConfirmSeconds)
                state.isMoving = false;
        }
    }

    // =========================================================================
    // WATER SUBMERGED STATE
    // =========================================================================

    void WaterInteraction::UpdateHandWaterState(HandWaterState& state,
                                                const RE::NiPoint3& pos)
    {
        float wh = GetWaterHeightAt(pos);
        if (wh > -9000.0f)
        {
            state.waterHeight = wh;
            constexpr float kSubmergedThreshold = 0.02f;
            state.isSubmerged = (wh - pos.z) > kSubmergedThreshold;
            state.depth = state.isSubmerged ? (wh - pos.z) : 0.0f;
        }
        else
        {
            state.isSubmerged = false;
            state.depth = 0.0f;
        }
    }

    // =========================================================================
    // HOVER DETECTION (hand near water surface)
    // =========================================================================

    void WaterInteraction::UpdateHandHover(HandWaterState& state,
                                           const RE::NiPoint3& pos)
    {
        state.isHovering = false;
        state.hoverHeight = 0.0f;

        float waterHeight = -10000.0f;
        bool hasWater = false;

        if (state.isSubmerged)
        {
            waterHeight = state.waterHeight;
            hasWater = true;
        }
        else
        {
            RE::NiPoint3 checkPos = pos;
            checkPos.z -= 500.0f;
            float wh = GetWaterHeightAt(checkPos);
            if (wh > -9000.0f)
            {
                waterHeight = wh;
                hasWater = true;
            }
        }

        if (hasWater)
        {
            state.hoverHeight = pos.z - waterHeight;
            if (state.hoverHeight >= -_config.hoverBelowTolerance &&
                state.hoverHeight <= _config.hoverMaxHeight)
            {
                state.isHovering = true;
            }
        }
    }

    // =========================================================================
    // HAND ENTRY HANDLING
    // =========================================================================

    void WaterInteraction::HandleHandEntry(HandWaterState& state,
                                           const RE::NiPoint3& pos, bool isLeft)
    {
        state.lastTransitionTime = _frameNow;

        RE::NiPoint3 surfacePos = pos;
        surfacePos.z = state.waterHeight;

        // Calculate entry speed (downward Z velocity)
        float downSpeed = 0.0f;
        if (state.samples.size() >= 2)
        {
            const auto& prev = state.samples[state.samples.size() - 2];
            const auto& cur = state.samples[state.samples.size() - 1];
            double dt = std::chrono::duration<double>(cur.time - prev.time).count();
            if (dt > 1e-4)
            {
                downSpeed = std::max(0.0f, (prev.pos.z - cur.pos.z) / static_cast<float>(dt));
            }
        }

        if (downSpeed >= _config.entryDownZThreshold &&
            downSpeed <= _config.maxEntryDownSpeed)
        {
            float amt = ComputeEntrySplashAmount(downSpeed);
            if (amt > 0.0f)
            {
                EmitRipple(surfacePos, amt);

                if (_config.enableSplashEffects)
                {
                    float speedFraction = std::clamp(downSpeed / _config.splashHardMax, 0.1f, 1.0f);
                    float magnitude = _config.splashEffectEntryMagnitude * speedFraction;
                    EmitSplashEffect(surfacePos, magnitude);
                }
            }
        }
        else if (downSpeed < _config.entryDownZThreshold)
        {
            // Gentle entry - small ripple + small splash
            float gentleAmt = _config.splashVeryLightAmt * _config.splashScale;
            EmitRipple(surfacePos, gentleAmt);
            if (_config.enableSplashEffects)
            {
                EmitSplashEffect(surfacePos, _config.splashEffectEntryMagnitude * 0.1f);
            }
        }
    }

    // =========================================================================
    // HAND EXIT HANDLING
    // =========================================================================

    void WaterInteraction::HandleHandExit(HandWaterState& state,
                                          const RE::NiPoint3& pos, bool isLeft)
    {
        state.lastTransitionTime = _frameNow;

        RE::NiPoint3 surfacePos = pos;
        surfacePos.z = state.prevWaterHeight;

        // Calculate exit speed (upward Z velocity)
        float upSpeed = 0.0f;
        if (state.samples.size() >= 2)
        {
            const auto& prev = state.samples[state.samples.size() - 2];
            const auto& cur = state.samples[state.samples.size() - 1];
            double dt = std::chrono::duration<double>(cur.time - prev.time).count();
            if (dt > 1e-4)
            {
                upSpeed = std::max(0.0f, (cur.pos.z - prev.pos.z) / static_cast<float>(dt));
            }
        }

        if (upSpeed >= _config.exitUpZThreshold && upSpeed <= _config.maxExitUpSpeed)
        {
            float amt = ComputeExitSplashAmount(upSpeed);
            if (amt > 0.0f)
            {
                EmitRipple(surfacePos, amt);

                if (_config.enableSplashEffects)
                {
                    float speedFraction = std::clamp(upSpeed / _config.splashExitHardMax, 0.1f, 1.0f);
                    float magnitude = _config.splashEffectExitMagnitude * speedFraction;
                    EmitSplashEffect(surfacePos, magnitude);
                }
            }
        }
        else if (upSpeed < _config.exitUpZThreshold)
        {
            // Gentle exit - small ripple + small splash
            float gentleAmt = _config.splashExitVeryLightAmt * _config.splashScale;
            EmitRipple(surfacePos, gentleAmt);
            if (_config.enableSplashEffects)
            {
                EmitSplashEffect(surfacePos, _config.splashEffectExitMagnitude * 0.1f);
            }
        }
    }

    // =========================================================================
    // WAKE RIPPLES (continuous while submerged + moving)
    // =========================================================================

    void WaterInteraction::HandleWakeRipples(HandWaterState& state,
                                             const RE::NiPoint3& pos,
                                             bool isLeft)
    {
        if (!_config.wakeEnabled)
            return;

        if (state.depth < _config.wakeMinDepth)
            return;

        float wakeSpeedThreshold = std::max(0.01f, _config.movingThreshold * 0.5f);
        if (state.recentSpeed <= wakeSpeedThreshold)
            return;

        RE::NiPoint3 wakePos = pos;
        wakePos.z = state.waterHeight;

        // Minimum distance check - prevents rapid-fire ripples from merging
        // into long stretched shapes during fast hand swipes
        if (state.hasLastWakePos && _config.wakeMinDistance > 0.0f)
        {
            float dx = wakePos.x - state.lastWakePos.x;
            float dy = wakePos.y - state.lastWakePos.y;
            float distSq = dx * dx + dy * dy;
            if (distSq < _config.wakeMinDistance * _config.wakeMinDistance)
                return;
        }

        // Rate limiting (time-based, in addition to distance-based)
        auto now = _frameNow;
        if (_config.wakeIntervalMs > 0)
        {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - state.lastWakeTime)
                                 .count();
            if (elapsedMs < _config.wakeIntervalMs)
                return;
        }

        // Scale wake amount by movement speed
        float mult = std::clamp(state.recentSpeed * _config.wakeScaleMultiplier,
                                _config.wakeMinMultiplier,
                                _config.wakeMaxMultiplier);
        float wakeRadius = _config.wakeAmt * mult;

        EmitRipple(wakePos, wakeRadius);

        state.lastWakeTime = now;
        state.lastWakePos = wakePos;
        state.hasLastWakePos = true;
    }

    // =========================================================================
    // SPLASH AMOUNT COMPUTATION (speed -> splash band -> radius)
    // =========================================================================

    float WaterInteraction::ComputeEntrySplashAmount(float downSpeed) const
    {
        if (downSpeed <= 0.1f)
            return 0.0f;

        float amt = 0.0f;
        if (downSpeed <= _config.splashVeryLightMax)
            amt = _config.splashVeryLightAmt;
        else if (downSpeed <= _config.splashLightMax)
            amt = _config.splashLightAmt;
        else if (downSpeed <= _config.splashNormalMax)
            amt = _config.splashNormalAmt;
        else if (downSpeed <= _config.splashHardMax)
            amt = _config.splashHardAmt;
        else
            amt = _config.splashVeryHardAmt;

        return amt * _config.splashScale;
    }

    float WaterInteraction::ComputeExitSplashAmount(float upSpeed) const
    {
        if (upSpeed <= 0.1f)
            return 0.0f;

        float amt = 0.0f;
        if (upSpeed <= _config.splashExitVeryLightMax)
            amt = _config.splashExitVeryLightAmt;
        else if (upSpeed <= _config.splashExitLightMax)
            amt = _config.splashExitLightAmt;
        else if (upSpeed <= _config.splashExitNormalMax)
            amt = _config.splashExitNormalAmt;
        else if (upSpeed <= _config.splashExitHardMax)
            amt = _config.splashExitHardAmt;
        else
            amt = _config.splashExitVeryHardAmt;

        return amt * _config.splashScale;
    }

    // =========================================================================
    // WATER HEIGHT DETECTION
    // =========================================================================

    float WaterInteraction::GetWaterHeightAt(const RE::NiPoint3& pos)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell)
            return -10000.0f;

        auto* cell = player->parentCell;
        RE::NiPoint3 checkPos = pos;
        float waterHeight = 0.0f;

        __try
        {
            if (Cell_GetWaterHeight(cell, checkPos, waterHeight, nullptr))
            {
                if (std::isfinite(waterHeight))
                    return waterHeight;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::error("[Water] Exception in GetWaterHeight!");
        }

        return -10000.0f;
    }

    // =========================================================================
    // RIPPLE EMISSION (visual water displacement via AddRipple)
    // =========================================================================

    void WaterInteraction::EmitRipple(const RE::NiPoint3& pos, float radius)
    {
        RE::NiPoint3 ripplePos = pos;

        if (g_TESWaterSystem)
        {
            // Periodically ensure the ripple enable flag stays on
            static int s_flagCheckCounter = 0;
            if (++s_flagCheckCounter >= 900)
            {
                s_flagCheckCounter = 0;
                __try
                {
                    uint8_t* pFlag = WaterSystem_RippleEnableFlag.get();
                    if (*pFlag == 0)
                    {
                        spdlog::warn("[Water] Ripple enable flag was reset to 0, forcing back to 1");
                        *pFlag = 1;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            __try
            {
                WaterSystem_AddRipple(g_TESWaterSystem, ripplePos, radius);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                spdlog::error("[Water] Exception in AddRipple - disabling direct path");
                g_TESWaterSystem = nullptr;
                g_TESWaterSystemSearched = true;
            }
        }
    }

    // =========================================================================
    // SPLASH EFFECT (VFX + SFX via TaskQueueInterface::QueueWaterSplashEffects)
    // =========================================================================

    void WaterInteraction::EmitSplashEffect(const RE::NiPoint3& pos, float magnitude)
    {
        auto* taskQueue = RE::TaskQueueInterface::GetSingleton();
        if (!taskQueue)
            return;

        RE::NiPoint3 splashPos = pos;

        __try
        {
            TaskQueue_WaterSplashEffects(taskQueue, magnitude, splashPos);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::error("[Water] Exception in QueueWaterSplashEffects!");
        }
    }

    // =========================================================================
    // SPLASH NIF SCALE OVERRIDE
    // Modifies engine's waterSplash.NIF scale globals based on user config.
    // When NIF disabled: scales set to 0 (sound still plays, no particle).
    // When NIF enabled: scales = original * user multiplier.
    // =========================================================================

    void WaterInteraction::ApplySplashNifScales()
    {
        if (s_origScaleLarge < 0.0f)
            return;  // original scales not read yet

        __try
        {
            if (!_config.enableSplashNif)
            {
                *SplashScale_Large.get() = 0.0f;
                *SplashScale_Medium.get() = 0.0f;
                *SplashScale_Small.get() = 0.0f;
            }
            else
            {
                *SplashScale_Large.get() = s_origScaleLarge * _config.splashNifScale;
                *SplashScale_Medium.get() = s_origScaleMedium * _config.splashNifScale;
                *SplashScale_Small.get() = s_origScaleSmall * _config.splashNifScale;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            spdlog::error("[Water] Exception applying splash NIF scales!");
            return;
        }

        _lastNifEnabled = _config.enableSplashNif;
        _lastNifScale = _config.splashNifScale;

        spdlog::info("[Water] Splash NIF: enabled={} scale={:.2f} (L={:.2f} M={:.2f} S={:.2f})",
                     _config.enableSplashNif, _config.splashNifScale,
                     _config.enableSplashNif ? s_origScaleLarge * _config.splashNifScale : 0.0f,
                     _config.enableSplashNif ? s_origScaleMedium * _config.splashNifScale : 0.0f,
                     _config.enableSplashNif ? s_origScaleSmall * _config.splashNifScale : 0.0f);
    }

} // namespace heisenberg
