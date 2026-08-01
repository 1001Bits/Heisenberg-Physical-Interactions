#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "RE/NetImmerse/NiPoint.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <SimpleIni.h>

#ifndef MAX_PATH
#define ROCK_DEFINED_MAX_PATH_FOR_FILEWATCH 1
#define MAX_PATH 260
#endif
#include <thomasmonkman-filewatch/FileWatch.hpp>
#if defined(ROCK_DEFINED_MAX_PATH_FOR_FILEWATCH)
#undef MAX_PATH
#undef ROCK_DEFINED_MAX_PATH_FOR_FILEWATCH
#endif

#include "physics-interaction/hand/HandSelection.h"
#include "physics-interaction/hand/SelectionBeamPolicy.h"
#include "physics-interaction/debug/DebugOverlayRuntimeSettings.h"
#include "physics-interaction/input/PipboyPauseGesturePolicy.h"
#include "physics-interaction/grab/GrabLocomotionAuthorityBridge.h"
#include "physics-interaction/native/HavokTimingFixPolicy.h"
#include "physics-interaction/weapon/WeaponSemantics.h"

namespace rock
{
    class RockConfig
    {
    public:
        class NativeMutationLease
        {
        public:
            NativeMutationLease() = default;
            ~NativeMutationLease();

            NativeMutationLease(const NativeMutationLease&) = delete;
            NativeMutationLease& operator=(const NativeMutationLease&) = delete;
            NativeMutationLease(NativeMutationLease&& other) noexcept;
            NativeMutationLease& operator=(NativeMutationLease&& other) noexcept;

        private:
            friend class RockConfig;
            NativeMutationLease(RockConfig* owner, bool resumeOnExit) :
                _owner(owner),
                _resumeOnExit(resumeOnExit)
            {}

            RockConfig* _owner = nullptr;
            bool _resumeOnExit = false;
        };

        ~RockConfig() { stopFileWatch(); }

        void load();

        void reload();

        void processPendingConfigReload();
        // True when the file watcher has armed a reload that processPendingConfigReload()
        // has not consumed yet. Lets the frame loop tell "a reload was actually applied
        // this frame" from "nothing happened", so per-reload invalidation (e.g. the
        // large-object memo) runs exactly once instead of every frame.
        [[nodiscard]] bool isConfigReloadPending() const { return _reloadPending.load(std::memory_order_acquire); }

        /*
         * Native callbacks read the public scalar fields directly for speed.
         * This gate makes their read interval atomic with respect to a config
         * reload or host-side setting mutation, avoiding mixed/default
         * snapshots and C++ data races without making every field atomic.
         */
        [[nodiscard]] bool tryEnterNativeRead();
        void leaveNativeRead();
        [[nodiscard]] NativeMutationLease pauseNativeReadsForMutation();

        void stopFileWatch();

        void subscribeForConfigChanged(const std::string& key, std::function<void(const std::string&)> callback);
        void unsubscribeFromConfigChanged(const std::string& key);
        [[nodiscard]] std::filesystem::path getConfigDirectory() const;

        void suppressNextFileWatchReload() { _ignoreNextIniFileChange.store(true); }

        [[nodiscard]] bool persistPhysicsBool(const char* key, bool value);
        [[nodiscard]] bool persistGrabLegacyPalmPivotAHandspace(bool isLeft, const RE::NiPoint3& value);

        bool rockEnabled = true;
        bool rockHavokTimingFixEnabled = true;
        float rockHavokTimingFixMinPhysicsFrameRate = havok_timing_fix_policy::kDefaultMinPhysicsFrameRate;
        int rockHavokTimingFixMaxSubsteps = havok_timing_fix_policy::kDefaultMaxSubsteps;

        // EMBED: input remap is vendored for compilation only — Heisenberg owns input routing.
        // Opt-in: the NSDMI matches resetToDefaults() so the OFF default holds even if a caller
        // reads the config before load(). Upstream ships this true.
        bool rockInputRemapEnabled = false;
        bool rockSuppressRightGrabGameInput = true;
        bool rockSuppressRightFavoritesGameInput = true;
        bool rockSuppressNativeReadyWeaponAutoReady = true;
        bool rockSuppressNativeMeleeThrowGameInput = true;
        bool rockSuppressPipboyGameInputWhileHolding = true;
        float rockPipboyPauseHoldSeconds = pipboy_pause_gesture_policy::kDefaultHoldSeconds;
        bool rockSuppressTakeEquipGameInputWhileHolding = true;
        std::string rockSuppressTakeEquipFormTypes = "WEAP,ARMO,AMMO,MISC,INGR,ALCH,BOOK,KEYM,SLGM";
        bool rockSuppressNativeGrabHoverHaptics = true;
        bool rockVirtualHolstersCompatibilityEnabled = true;
        bool rockVirtualHolstersDeferGrabInZone = true;
        bool rockVirtualHolstersDeferWeaponToggleInZone = true;
        bool rockVirtualHolstersDeferOnlyMatchingButton = false;
        bool rockGrabInputIntentStateEnabled = true;
        float rockGrabInputLeewaySeconds = 0.12f;
        float rockGrabInputForceSeconds = 0.08f;

        bool rockDeveloperModeEnabled = false;

        // Release default is error-only. NOTE the NSDMI is NOT the effective default -
        // load()/reload() both call resetToDefaults() first, so RockConfig.cpp's
        // resetToDefaults() value is authoritative. Both are error (4); keep them
        // in sync or the header value is inert.
        int rockLogLevel = 4;
        // EMBED: default-empty on purpose. A >15-char std::string literal NSDMI on a namespace-scope
        // global (g_rockConfig) miscompiles in the static-lib embed: MSVC constexpr-materializes the
        // heap ("large-mode") representation but the buffer pointer comes out NULL, so the first
        // resetToDefaults() assign does an in-place memcpy to a null _Ptr -> CTD. Empty = SSO = always
        // valid; resetToDefaults() sets the real value (logging_policy::DefaultLogPattern) at runtime.
        std::string rockLogPattern;
        int rockLogSampleMilliseconds = 2000;
        // Independent, allocation-free frame-pacing benchmark:
        // 0 = off, 1 = A/full ROCK, 2 = B/host-selected ROCK live-work bypass.
        int rockPerformanceBenchmarkMode = 0;
        bool rockPerformanceProfilerEnabled = false;
        int rockPerformanceProfilerLogIntervalFrames = 300;
        int rockPerformanceProfilerWarmupFrames = 120;
        bool rockPerformanceProfilerOverlayText = false;

        RE::NiPoint3 rockPalmNormalHandspace = RE::NiPoint3(0.0f, 1.0f, 0.0f);
        RE::NiPoint3 rockPointingVectorHandspace = RE::NiPoint3(0.0f, 1.0f, 0.0f);
        bool rockReversePalmNormal = true;
        bool rockReverseFarGrabNormal = true;

        bool rockLeftHandedMode = false;
        bool rockWeaponCollisionEnabled = true;
        bool rockWeaponCollisionBlocksProjectiles = false;
        bool rockWeaponCollisionBlocksSpells = false;
        bool rockWeaponCollisionStaticWorldEnabled = true;
        int rockWeaponCollisionGroupingMode = weapon_collision_grouping_policy::kDefaultWeaponCollisionGroupingMode;
        int rockWeaponCollisionVisualStabilizationFrames = 8;
        float rockWeaponCollisionConvexRadius = 0.01f;
        float rockWeaponCollisionPointDedupGrid = 0.002f;
        int rockWeaponCollisionSupportFitTargetPoints = 96;
        float rockWeaponCollisionSupportFitMaxErrorGameUnits = 0.5f;
        float rockWeaponCollisionMaxLinearVelocity = 50.0f;
        float rockWeaponCollisionMaxAngularVelocity = 100.0f;
        bool rockWeaponCollisionMaxSourceDistanceEnabled = true;
        float rockWeaponCollisionMaxSourceDistanceMelee = 90.0f;
        float rockWeaponCollisionMaxSourceDistancePistol = 20.0f;
        float rockWeaponCollisionMaxSourceDistanceRifle = 45.0f;
        float rockWeaponCollisionMaxSourceDistanceHeavy = 70.0f;
        float rockWeaponSizeClassPistolMaxWeight = 6.0f;
        float rockWeaponSizeClassRifleMaxWeight = 20.0f;
        float rockWeaponInteractionTouchRadius = 2.0f;
        float rockWeaponInteractionProbeRadius = 12.0f;
        // Extra mesh/collider separation accepted when reconstructing an
        // exact provider-target contact after Havok omits the callback.
        // Unlike fWeaponInteractionProbeRadius this never considers an
        // unmatched weapon part.
        float rockWeaponPartExactContactToleranceGameUnits = 0.35f;
        float rockWeaponFiringGripReattachRadius = 3.0f;
        bool rockWeaponGripHapticsEnabled = true;
        float rockWeaponGripHapticDurationSeconds = 0.10f;
        float rockWeaponFiringGripAttachHapticIntensity = 0.85f;
        float rockWeaponFiringGripDetachHapticIntensity = 0.30f;
        float rockWeaponSupportGripHapticIntensity = 0.50f;
        bool rockEquippedWeaponShoulderStashEnabled = true;
        bool rockRealisticWeaponHandlingEnabled = true;
        float rockFiringGripProximitySupportRadius = 6.0f;
        float rockRealisticGrenadeFuseSeconds = 5.0f;
        bool rockGrabbedWeaponAutoEquipEnabled = false;
        float rockGrabbedWeaponAutoEquipSettleSeconds = 0.75f;
        bool rockGrabbedWeaponGripZoneEquipEnabled = true;
        float rockGrabbedWeaponGripZoneEquipRadius = 3.0f;
        float rockGrabbedWeaponGripZoneEquipSettleSeconds = 0.15f;
        bool rockGripZoneHoverHapticsEnabled = true;
        float rockGripZoneHoverHapticIntensity = 0.75f;
        bool rockGrabbedWeaponEquipBridgeEnabled = true;
        float rockGrabbedWeaponEquipBridgeTimeoutSeconds = 2.0f;
        float rockGrabbedWeaponEquipBridgeBlendSeconds = 0.15f;
        bool rockWeaponSupportGripHandLerpEnabled = true;
        float rockWeaponSupportGripHandLerpTimeMin = 0.12f;
        float rockWeaponSupportGripHandLerpTimeMax = 0.20f;
        float rockWeaponSupportGripHandLerpMinDistance = 1.0f;
        float rockWeaponSupportGripHandLerpMaxDistance = 14.0f;
        // EMBED: the See-Through Scopes compat layer stays compiled and hooked (ROCKMain.cpp), but
        // Heisenberg owns scope presentation at runtime, so the embed default is OFF (upstream: true).
        bool rockSeeThroughScopesCompatibilityEnabled = false;
        bool rockSeeThroughScopesReticleAlignmentEnabled = true;
        bool rockSeeThroughScopesRightEyeDominant = true;
        float rockSeeThroughScopesEyeOffsetGameUnits = 2.3f;
        float rockSeeThroughScopesReticleOffsetXGameUnits = 0.372727f;
        float rockSeeThroughScopesReticleOffsetZGameUnits = -0.149692f;
        float rockSeeThroughScopesLookDotThreshold = 0.98f;
        float rockSeeThroughScopesDistanceThresholdGameUnits = 20.0f;
        bool rockWeaponVisualReturnEnabled = true;
        float rockWeaponVisualReturnTimeMin = 0.12f;
        float rockWeaponVisualReturnTimeMax = 0.20f;
        float rockWeaponVisualReturnMinDistance = 1.0f;
        float rockWeaponVisualReturnMaxDistance = 14.0f;
        float rockWeaponVisualReturnMinAngleDegrees = 5.0f;
        float rockWeaponVisualReturnMaxAngleDegrees = 90.0f;

        bool rockSoftContactWorldEnabled = true;
        // EMBED: free-hand visual wall pushback is temporarily disabled for
        // release while its FRIK 0.77.12 presentation is revisited. This is a
        // separate channel from equipped-weapon/world collision.
        bool rockHandWorldPushbackEnabled = false;
        int rockSoftContactVisualPriority = 80;
        float rockSoftContactWorldRadiusPaddingGameUnits = 1.5f;
        float rockSoftContactWorldContactPaddingGameUnits = 0.35f;
        float rockSoftContactWorldSkinGameUnits = 0.5f;
        float rockSoftContactWorldPostReleaseReentryMinApproachDistanceGameUnits = 0.025f;
        float rockSoftContactWorldCachedPlaneMaxTangentDriftGameUnits = 10.0f;
        float rockSoftContactWorldCachedPlaneMaxClearDistanceGameUnits = 18.0f;
        float rockSoftContactWorldMaxCorrectionGameUnits = 18.0f;
        bool rockSoftContactWorldReleaseLerpEnabled = true;
        float rockSoftContactWorldReleaseLerpTimeMin = 0.06f;
        float rockSoftContactWorldReleaseLerpTimeMax = 0.12f;
        float rockSoftContactWorldReleaseLerpMinDistance = 0.5f;
        float rockSoftContactWorldReleaseLerpMaxDistance = 18.0f;
        std::uint32_t rockSoftContactWorldShapeCastFilterInfo = selection_query_policy::kDefaultShapeCastFilterInfo;
        bool rockSoftContactWorldHapticsEnabled = true;
        float rockSoftContactWorldHapticDurationSeconds = 0.035f;
        float rockSoftContactWorldHapticBaseIntensity = 0.18f;
        float rockSoftContactWorldHapticMaxIntensity = 0.55f;
        float rockSoftContactWorldHapticSpeedScale = 0.006f;
        float rockSoftContactWorldHapticMinApproachSpeedGameUnits = 3.0f;
        float rockSoftContactWorldHapticCooldownSeconds = 0.12f;
        bool rockAutoActivateScope = false;
        float rockManualScopeHoldSeconds = 0.30f;
        float rockNativeScopeOverlayOffsetXGameUnits = 0.0f;
        float rockNativeScopeOverlayOffsetYGameUnits = 0.0f;
        float rockNativeScopeOverlayOffsetZGameUnits = 0.0f;
        float rockNativeScopeOverlayPitchDegrees = 0.0f;
        float rockNativeScopeOverlayYawDegrees = 0.0f;
        float rockNativeScopeOverlayRollDegrees = 0.0f;
        // EMBED: upstream's dynamic hand-collision drive is the alternative to the Heisenberg-preserved
        // soft-contact runtime; the two are mutually exclusive (see PhysicsInteraction::updateFrame).
        // The embed-wide rockHandWorldPushbackEnabled gate controls both paths
        // and currently defaults OFF; this alternative also remains OFF.
        bool rockHandCollisionDynamicDrive = false;
        float rockHandCollisionDynamicMaxLinearVelocityHavok = 15.0f;
        float rockHandCollisionDynamicContactPressMaxVelocityHavok = 1.0f;
        float rockHandCollisionDynamicDivergenceTeleportGameUnits = 40.0f;
        float rockHandCollisionDynamicDivergenceTeleportDwellSeconds = 0.3f;
        float rockHandCollisionDynamicTeleportRecoverySeconds = 0.25f;
        float rockHandCollisionDynamicRenderFollowMinDeviationGameUnits = 0.05f;
        float rockHandCollisionDynamicRenderFollowSmoothingSpeed = 45.0f;
        int rockHandCollisionDynamicVisualPriority = 80;
        bool rockHandCollisionDynamicHapticsEnabled = true;
        float rockHandCollisionDynamicHapticDurationSeconds = 0.035f;
        float rockHandCollisionDynamicHapticBaseIntensity = 0.18f;
        float rockHandCollisionDynamicHapticMaxIntensity = 0.55f;
        float rockHandCollisionDynamicHapticSpeedScale = 0.006f;
        float rockHandCollisionDynamicHapticMinApproachSpeedGameUnitsPerSecond = 3.0f;
        float rockHandCollisionDynamicHapticCooldownSeconds = 0.12f;

        /*
         * ── NATIVE MELEE SUPPRESSION — DEFAULT OFF, DELIBERATELY ──────────────────────────
         *
         * These three shipped as `true`, but the feature behind them had never once
         * executed. Its installer requires all five hook targets to validate, and two of
         * the five (kFunc_VRMeleeImpactCallback and
         * kVtableEntry_AttackBlockHandler_ShouldHandleEvent) were TODO_RE == 0 in every
         * released build, so validation failed on every machine and NOTHING was ever
         * patched. The `true` above described an intent, not a behaviour.
         *
         * Both offsets are now Ghidra-confirmed and filled in, which makes the feature
         * installable for the FIRST time. That is exactly why the default flips to false:
         * a tester has reported melee not working, and switching on a never-executed
         * suppression path that reaches into WeaponSwing, HitFrame, the AttackBlock input
         * gate and the VRInput velocity thresholds is not something to enable silently in
         * the same change that makes it possible.
         *
         * Enable deliberately with [PhysicsInteraction] bNativeMeleeSuppressionEnabled=1
         * (and the per-path bNativeMeleeSuppressWeaponSwing / bNativeMeleeSuppressHitFrame)
         * in Data\F4SE\Plugins\Heisenberg_F4VR.ini, then restart - hook install happens once
         * at plugin init. NOTE: the embed reads the SHARED host ini and the section is
         * "PhysicsInteraction" (RockConfig.cpp SECTION / resolveIniPath). There is no
         * standalone ROCK.ini and no [Rock] section - earlier text here named both, and a
         * user following it would create a file nothing ever opens.
         *
         * MIRROR: RockConfig::resetToDefaults() re-assigns every one of these and is called
         * by BOTH load() and reload(), so it OVERRIDES this NSDMI. Changing a default here
         * alone is inert — change it in resetToDefaults() too.
         */
        // OBSERVE-ONLY melee instrumentation (Jul 31). Installs just TWO of the
        // five suppression hooks — the VRMeleeImpact entry trampoline and the
        // PlayerCharacter::WeaponSwingCallBack vtable swap — as pure
        // pass-throughs that log each player SWING and each native IMPACT (the
        // game's own hit decision) with paired counters. A SWING with no
        // following IMPACT is a confirmed native miss; that pairing is the only
        // way to answer "is melee actually hitting?" from the log, because the
        // hit decision lives inside VRMeleeImpact and is otherwise invisible.
        // Suppression stays fully OFF: with rockNativeMeleeSuppressionEnabled
        // false every policy returns CallNative, so the hook bodies forward to
        // the native implementation unchanged.
        //
        // Default ON while melee reliability is under live investigation (the
        // installer still refuses cleanly byte-for-byte if another mod got
        // there first). Set [PhysicsInteraction] bNativeMeleeObservationEnabled=0
        // to leave the native code completely untouched.
        bool rockNativeMeleeObservationEnabled = true;
        // NATIVE-MELEE FIRST-HIT FIX (Jul 31). While a real melee weapon is
        // drawn, disable our generated colliders (weapon hull + hands + arm
        // chain) so they cannot feed contact events to FO4VR's VRMeleeImpact
        // handler. That handler arms a GLOBAL melee cooldown (PlayerCharacter
        // +0x908) on every processed contact and early-returns while it is >0,
        // so our co-located colliders were burning the cooldown at swing onset
        // and the real NPC contact landed no damage — root-caused Ghidra-
        // confirmed, reproduced as "vanilla lands the first bat hit, the mod
        // doesn't." The native melee capsule is the WEAPON'S OWN NIF collision,
        // NOT our hull, so removing our hull does not affect the game's hit
        // test — it only stops us stealing the cooldown. Default ON: landing
        // melee damage outranks weapon-vs-world/hand push while swinging (and
        // ROCK's melee push-protection already suppresses actor pushes then).
        // Drawn-scoped, NOT swing-windowed: the cooldown re-arms from resting
        // grind between swings, so a per-swing window would not close it.
        // FALLBACK now (default OFF): the precise per-contact filter below
        // supersedes it and keeps the hull collidable (bat clanks off walls).
        // Turn this on if the contact filter ever underperforms.
        bool rockNativeMeleeColliderSuppressionEnabled = false;
        // PRECISE native-melee first-hit fix (Jul 31, Ghidra-verified). Inside
        // our VRMeleeImpact observe hook, decode the OTHER contact body's
        // collision layer and drop (do not forward to native) any contact whose
        // partner is one of our generated colliders (layers 43/44/47/48), so
        // native never arms the global melee cooldown from our bodies — while
        // leaving the hull collidable for world clank. Offsets confirmed against
        // FUN_140eff000 + bhkNPCollisionObject::GetCollisionFilterInfo on FO4VR
        // 1.2.72: world=*contactEvent, slot=*(contactEvent+0x20),
        // otherId=*(collisionEvent+8+(1-slot)*4), body=*(world+0x20)+otherId*0x90,
        // layer=*(uint*)(body+0x44)&0x7F. All reads SEH-guarded; a fault or any
        // implausible value falls through to native (vanilla for that contact).
        bool rockNativeMeleeContactFilterEnabled = true;
        bool rockNativeMeleeSuppressionEnabled = false;
        bool rockNativeMeleeFullSuppression = false;
        bool rockNativeMeleeSuppressWeaponSwing = false;
        bool rockNativeMeleeSuppressHitFrame = false;
        bool rockNativeMeleeDebugLogging = false;
        bool rockNativeCharacterControllerObjectContactFilterEnabled = true;

        // ── LARGE-OBJECT POLICY (car fix, #219/#220) ─────────────────────────────────────
        // The native character-controller object suppression above is what lets the player
        // walk through parked cars: it clears the CHARCONTROLLER row against CLUTTER(4),
        // Cars are all layer 29 — but so are barrels, tyres, trash cans and the
        // baby carriage, which must stay hand-reachable. SIZE (base-form
        // BOUND_DATA largest axis x ref scale) is therefore used only for the
        // grab ceiling. Player-body collision remains Fallout-native for every
        // valid native wrapper layer.
        //
        // Measured game units (1 gu = 0.0142875 m): tyre/cone 43-44, barrel/trash can
        // 71-76, baby carriage 107 (largest keeper), motorcycle 199, sedan 427.
        // 150 is the user-approved threshold; the usable window is 110-195.
        bool rockLargeObjectPlayerBlockEnabled = true;              // master for oversized-object grab rejection
        float rockLargeObjectBoundThresholdGameUnits = 150.0f;      // clamped 40..1000 on load
        bool rockLargeObjectGrabBlockEnabled = true;                // oversized-object grab ceiling
        // Legacy compatibility keys. Vanilla player collision is preserved for
        // every valid native body regardless of object size or wrapper layer.
        bool rockLargeObjectCharacterControllerBlockEnabled = true;
        bool rockLargeObjectBlockRestrictToClutterLargeLayer = true;

        // ── Embedded-host ownership masters (added for the Heisenberg single-DLL embed) ──
        // When ROCK runs embedded inside Heisenberg, the host's iGrabMode chooses whether
        // ROCK owns grab+selection while ROCK keeps hand/weapon/world collision running.
        // Standalone ROCK leaves both true (no behaviour change).
        bool rockGrabEnabled = true;       // master: ROCK initiates / holds / throws grabs
        bool rockSelectionEnabled = true;  // master: ROCK detects / highlights / beam-selects grab candidates
        // EMBED-only feel option (objective-1 fix, Jul 5): stock ROCK no-collide-leases the whole
        // dominant-hand collider suite while a weapon is drawn — the open hand can't push objects
        // when armed. false = HIGGS behavior (hand collision stays live while armed); true = stock ROCK.
        bool rockSuppressDominantHandCollision = false;
        // EMBED: set via rock::HostSetGrabOwnership from Heisenberg's iGrabMode.
        // Once configured, both ON and OFF are forced after every shared-INI
        // load/reload so missing standalone ROCK keys cannot start a second grab system.
        bool rockHostGrabOwnershipConfigured = false;
        bool rockHostGrabOwnershipForced = false;

        bool rockHighlightEnabled = true;
        int rockHighlightIntensityMode = 3;
        std::string rockHighlightColor = "orange";
        bool rockSelectionBeamEnabled = true;
        float rockSelectionBeamSegmentSizeGameUnits = selection_beam_policy::kDefaultSegmentSizeGameUnits;
        float rockSelectionBeamCurveLiftGameUnits = selection_beam_policy::kDefaultCurveLiftGameUnits;
        float rockSelectionBeamAlpha = selection_beam_policy::kDefaultAlpha;

        bool rockDebugShowColliders = false;
        bool rockDebugShowTargetColliders = false;
        bool rockDebugShowHandAxes = false;
        bool rockDebugShowGrabPivots = false;
        bool rockDebugShowGrabPocketNormal = false;
        bool rockDebugDrawGrabContactPatch = false;
        bool rockDebugDrawGrabForceTorque = false;
        bool rockDebugDrawGrabForceTorqueText = false;
        bool rockDebugDrawGrabPivotSourceCollider = false;
        bool rockDebugDrawGrabPivotSourceEvidence = false;
        bool rockDebugDrawGrabSupportFrame = false;
        bool rockDebugDrawGrabPockets = false;
        bool rockDebugShowGrabFingerProbes = false;
        bool rockDebugShowGrabFingerSweptArc = false;
        bool rockDebugShowGrabFingerSweptArcText = true;
        bool rockDebugShowGrabFingerSweptArcLiveSkeleton = true;
        bool rockDebugShowPalmVectors = false;
        bool rockDebugDrawHandColliders = false;
        bool rockDebugDrawHandBoneColliders = false;
        bool rockDebugDrawDynamicHandColliders = false;
        bool rockDebugDrawHandBoneContacts = false;
        bool rockDebugDrawSoftContacts = false;
        bool rockDebugDrawGrabAuthorityProxy = false;
        int rockDebugMaxHandBoneBodiesDrawn = 48;
        int rockDebugMaxBodyBoneBodiesDrawn = 32;
        bool rockDebugDrawWeaponColliders = false;
        bool rockDebugDrawNativeScopeActivation = false;
        bool rockDebugDrawDynamicWeaponColliders = false;
        bool rockDebugDumpWeaponAnimNodes = false;
        int rockDebugMaxWeaponBodiesDrawn = 100;
        int rockDebugWeaponAnimNodeDumpIntervalFrames = 120;
        int rockDebugMaxShapeGenerationsPerFrame = 100;
        int rockDebugMaxConvexSupportVertices = 6;
        int rockDebugMaxShapeCapturesPerFrame = static_cast<int>(debug_overlay_runtime::kDefaultMaxShapeCapturesPerFrame);
        int rockDebugMaxCompoundChildren = static_cast<int>(debug_overlay_policy::kDefaultMaxCompoundChildren);
        int rockDebugMaxCompoundDepth = static_cast<int>(debug_overlay_policy::kDefaultMaxCompoundDepth);
        int rockDebugMaxShapeQueuedJobs = static_cast<int>(debug_overlay_runtime::kDefaultMaxShapeQueuedJobs);
        int rockDebugMaxShapeCompletedJobs = static_cast<int>(debug_overlay_runtime::kDefaultMaxShapeCompletedJobs);
        int rockDebugMaxShapeUploadsPerFrame = static_cast<int>(debug_overlay_runtime::kDefaultMaxShapeUploadsPerFrame);
        int rockDebugMaxShapeCacheEntries = static_cast<int>(debug_overlay_policy::kDefaultShapeCacheBudget);
        int rockDebugMaxShapeCacheBytes = static_cast<int>(debug_overlay_runtime::kDefaultMaxShapeCacheBytes);
        int rockDebugMaxBodyInstances = static_cast<int>(debug_overlay_runtime::kDefaultMaxBodyInstances);
        int rockDebugMaxLineVertices = static_cast<int>(debug_overlay_policy::kDefaultLineVertexBudget);
        int rockDebugMaxTextVertices = static_cast<int>(debug_overlay_runtime::kDefaultMaxTextVertices);
        bool rockDebugUseBoundsForHeavyConvex = true;
        bool rockDebugContactTargetIdentityLogging = false;
        int rockDebugContactTargetIdentitySampleMilliseconds = 500;
        bool rockDebugVerboseLogging = false;
        bool rockDebugGrabFrameLogging = false;
        bool rockDebugGrabFingerPoseLogging = false;
        bool rockDebugGrabTimelineTrace = false;
        bool rockDebugGrabAfterSolveAnomalySampling = false;
        bool rockDebugGrabTransformTelemetry = false;
        bool rockDebugGrabTransformTelemetryText = false;
        bool rockDebugGrabTransformTelemetryAxes = false;
        int rockDebugGrabTimelineTraceIntervalFrames = 1;
        int rockDebugGrabTransformTelemetryLogIntervalFrames = 1;
        int rockDebugGrabTransformTelemetryTextMode = 0;
        bool rockDebugShowGrabNotifications = false;
        bool rockDebugShowWeaponNotifications = false;
        bool rockDebugWeaponOmodDumpEnabled = false;
        bool rockDebugWeaponOmodCoverageAudit = false;
        int rockDebugWeaponOmodCoverageAuditIntervalFrames = 450;
        bool rockDebugWeaponOmodSelfHeal = false;
        bool rockDebugWorkbenchWeaponReattach = false;
        bool rockDebugHandTransformParity = false;
        bool rockDebugWorldObjectOriginDiagnostics = false;
        int rockDebugWorldObjectOriginLogIntervalFrames = 120;
        float rockDebugWorldObjectOriginMismatchWarnGameUnits = 5.0f;
        bool rockDebugCustomCalibrationOffset = false;
        bool rockDebugShowRootFlattenedFingerSkeletonMarkers = false;
        bool rockDebugShowSkeletonBoneVisualizer = false;
        bool rockDebugDrawSkeletonBoneAxes = false;
        bool rockDebugLogSkeletonBones = false;
        int rockDebugSkeletonBoneMode = 1;
        int rockDebugSkeletonBoneSource = 1;
        int rockDebugMaxSkeletonBonesDrawn = 256;
        int rockDebugMaxSkeletonBoneAxesDrawn = 80;
        int rockDebugSkeletonBoneLogIntervalFrames = 120;
        bool rockDebugLogSkeletonBoneTruncation = false;
        float rockDebugRootFlattenedFingerSkeletonMarkerSize = 1.4f;
        float rockDebugSkeletonBonePointSize = 1.4f;
        float rockDebugSkeletonBoneAxisLength = 4.0f;
        std::string rockDebugSkeletonBoneLogFilter;  // EMBED: default-empty (see rockLogPattern note); resetToDefaults() sets "RArm_Hand,LArm_Hand,RArm_Finger23,LArm_Finger23,Chest,Pelvis" at runtime.
        std::string rockDebugSkeletonAxisBoneFilter = "";

        int rockHandColliderRuntimeMode = 1;
        bool rockBodyBoneCollidersEnabled = true;
        bool rockBodyBoneLegAndFootCollidersEnabled = false;
        bool rockBodyBoneCollisionStaticWorldEnabled = true;
        float rockBodyBoneColliderStandardRadiusScale = 1.0f;
        float rockBodyBoneColliderStandardLengthScale = 1.0f;
        float rockBodyBoneColliderStandardConvexRadiusScale = 1.0f;
        float rockBodyBoneColliderPowerArmorRadiusScale = 1.0f;
        float rockBodyBoneColliderPowerArmorLengthScale = 1.0f;
        float rockBodyBoneColliderPowerArmorConvexRadiusScale = 1.0f;
        float rockBodyBoneColliderTorsoRadiusScale = 1.0f;
        float rockBodyBoneColliderArmRadiusScale = 1.0f;
        float rockBodyBoneColliderLegRadiusScale = 1.0f;
        float rockBodyBoneColliderFootRadiusScale = 1.0f;
        float rockBodyBoneColliderTorsoLengthScale = 1.0f;
        float rockBodyBoneColliderArmLengthScale = 1.0f;
        float rockBodyBoneColliderLegLengthScale = 1.0f;
        float rockBodyBoneColliderFootLengthScale = 1.0f;
        std::string rockBodyBoneColliderZoneScaleOverrides = "";
        std::string rockBodyBoneColliderRadiusScaleOverrides = "";
        bool rockHandCollisionStaticWorldEnabled = true;
        std::string rockHandBoneColliderRadiusScaleOverrides = "";
        // Additive padding (game units) on every hand collider radius. The rendered FRIK hand
        // mesh extends past the physics capsules, so small resting objects (coins, duct tape)
        // visually clip through fingers/forearm at the stock radii. Host-driven: Heisenberg
        // bridges its MCM-tunable fHandColliderRadiusPadding here every frame via
        // rock::HostSetHandColliderRadiusPadding — the ini key below is only the standalone
        // fallback. Participates in the hand-collider tuning signature, so a change forces a
        // rebuild mid-session.
        float rockHandBoneColliderRadiusPadding = 0.2f;
        std::string rockHandPalmColliderDimensionScaleOverrides = "";
        bool rockHandBoneCollidersRequirePalmAnchor = true;
        bool rockHandBoneCollidersRequireAllFingerBones = true;
        float rockHandBoneColliderMaxLinearVelocity = 200.0f;
        float rockHandBoneColliderMaxAngularVelocity = 500.0f;
        // DEAD SETTING - parsed and defaulted, but read by nothing. Left in place only so an
        // existing ini key does not start warning as unknown.
        //
        // CORRECTED 2026-07-28. The text here used to assert that the native's float argument
        // is invDeltaTime "NOT dt", and that passing 1/dt makes the solver do full
        // momentum-carrying collision. That is INVERTED and was already myth-busted once.
        // Disassembly of hknpWorld::computeHardKeyFrame (0x14153A6A0) shows the native takes
        // DELTA TIME and forms 1/dt internally (rcpps + one NR refine) before multiplying the
        // position delta. Passing dt is correct and is what both call sites do; passing 1/dt
        // would scale drive velocity by dt^2 and silently kill the drive. See the block above
        // kFunc_ComputeHardKeyFrame in HavokOffsets.h.
        bool rockKeyframeDriveFullSolverVelocity = false;

        float rockNearDetectionRange = 25.0f;
        float rockFarDetectionRange = 350.0f;
        float rockNearCastRadiusGameUnits = 3.5f;
        float rockNearCastDistanceGameUnits = 7.0f;
        float rockFarCastRadiusGameUnits = 21.0f;
        int rockCloseSelectionAngleDegrees = selection_query_policy::kDefaultSelectionAimAngleDegrees;
        int rockFarSelectionAngleDegrees = selection_query_policy::kDefaultSelectionAimAngleDegrees;
        bool rockFarSelectionHmdConeEnabled = true;
        float rockFarSelectionHmdConeHalfAngleDegrees = selection_query_policy::kDefaultFarSelectionHmdConeHalfAngleDegrees;
        std::string rockFarSelectionBlockedReferenceFormIds = "";
        std::string rockFarSelectionBlockedBaseFormIds = "";
        std::string rockFarSelectionBlockedFormTypes = "";
        std::string rockFarSelectionBlockedLayers = "";
        float rockCloseSelectionBehindPalmToleranceGameUnits = 2.0f;
        std::uint32_t rockSelectionShapeCastFilterInfo = selection_query_policy::kDefaultShapeCastFilterInfo;
        std::uint32_t rockFarClipRayFilterInfo = selection_query_policy::kDefaultFarClipRayFilterInfo;
        float rockPullApplyVelocityTime = 0.2f;
        float rockPullOwnerGraceSeconds = 1.0f;
        float rockPullTrackHandTime = 0.1f;
        float rockPullDestinationZOffsetHavok = 0.01f;
        float rockPullDurationA = 0.715619f;
        float rockPullDurationB = -0.415619f;
        float rockPullDurationC = 0.656256f;
        float rockPullMaxVelocityHavok = 10.0f;
        float rockPullAutoGrabDistanceGameUnits = 18.0f;
        float rockPullCatchRetryMaxTimeSeconds = 0.65f;
        bool rockPullCatchWideReacquireEnabled = true;
        float rockPullCatchWideReacquireRadiusGameUnits = 32.0f;
        float rockPullCatchWideReacquireMaxBodyDistanceGameUnits = 42.0f;
        int rockObjectPhysicsTreeMaxDepth = 12;
        bool rockDynamicPushAssistEnabled = true;
        // 0.02, NOT 0.35: RockConfig.cpp resetToDefaults() is the authoritative assignment
        // (both load() and reload() call it before reading the ini), and the tuned value there
        // is 0.02 - the old 0.10-0.35 gate rejected every press. Kept in sync so this header
        // states the value that actually runs.
        float rockDynamicPushMinSpeed = 0.02f;
        float rockDynamicPushMaxImpulse = 2.0f;
        float rockDynamicPushCooldownSeconds = 0.08f;
        // The impulse above is sized from hand speed alone — it has no idea what it's hitting.
        // impulse = mass * deltaV, so the same impulse gives a light object (a coin, a token) a
        // far bigger velocity kick than a heavy one. Confirmed via log: a "gentle bump" on a
        // Subway Token launched it at speed. This caps the resulting velocity change per body
        // (Havok units) regardless of how light the target is; heavier objects are unaffected
        // since their computed deltaV rarely reaches this ceiling anyway.
        float rockDynamicPushMaxVelocityDelta = 2.5f;

        float rockGrabLinearTau = 0.03f;
        float rockGrabLinearDamping = 0.8f;
        float rockGrabLinearProportionalRecovery = 2.0f;
        float rockGrabLinearConstantRecovery = 1.0f;

        float rockGrabAngularTau = 0.03f;
        float rockGrabAngularDamping = 0.8f;
        float rockGrabAngularProportionalRecovery = 2.0f;
        float rockGrabAngularConstantRecovery = 1.0f;

        float rockGrabConstraintMaxForce = 2000.0f;
        float rockGrabMaxForceToMassRatio = 500.0f;
        float rockForceGrabAttachSettleSeconds = 0.10f;
        bool rockGrabEffectiveMotorMassFloorEnabled = true;
        float rockGrabEffectiveMotorMassFloor = 2.0f;
        bool rockGrabPhysicsRateForceScalingEnabled = true;
        float rockGrabPhysicsRateReferenceHz = 90.0f;
        float rockGrabPhysicsRateForceScaleExponent = 0.5f;
        float rockGrabPhysicsRateMinForceScale = 0.75f;
        float rockGrabPhysicsRateMaxForceScale = 1.35f;
        bool rockGrabRoomVelocityFeedForward = false;
        bool rockGrabLocomotionTransport = false;
        bool rockGrabSmoothVelocityDrive = false;
        float rockGrabSmoothVelocityCorrectorGain = 0.2f;

        float rockGrabForceFadeInTime = 0.1f;
        int rockGrabRagdollDecompositionMode = -1;
        // (0, -2, 0), NOT the zero vector: resetToDefaults() assigns (0,-2,0) and overrides
        // these NSDMIs on every load()/reload(). Kept in sync so the header states the value
        // that actually runs.
        RE::NiPoint3 rockRightGrabAuthorityProxyOffsetGameUnits = RE::NiPoint3(0.0f, -2.0f, 0.0f);
        RE::NiPoint3 rockLeftGrabAuthorityProxyOffsetGameUnits = RE::NiPoint3(0.0f, -2.0f, 0.0f);
        RE::NiPoint3 rockRightCustomOGAOffsetGameUnits = RE::NiPoint3(0.0f, 0.0f, 0.0f);
        RE::NiPoint3 rockLeftCustomOGAOffsetGameUnits = RE::NiPoint3(0.0f, 0.0f, 0.0f);
        float rockGrabLooseWeaponSharedConstraintLinearTauMultiplier = 1.0f;
        float rockGrabLooseWeaponSharedConstraintAngularTauMultiplier = 1.0f;
        float rockGrabLooseWeaponSharedConstraintCollisionTauMultiplier = 1.0f;
        float rockGrabLooseWeaponSharedConstraintLinearDampingMultiplier = 1.0f;
        float rockGrabLooseWeaponSharedConstraintAngularDampingMultiplier = 1.0f;
        float rockGrabLooseWeaponSharedConstraintMaxForceMultiplier = 4.5f;
        float rockGrabLooseWeaponSharedConstraintAngularForceMultiplier = 2.0f;
        float rockGrabLooseWeaponSharedConstraintLinearRecoveryMultiplier = 1.0f;
        float rockGrabLooseWeaponSharedConstraintAngularRecoveryMultiplier = 1.0f;
        float rockGrabTauMin = 0.01f;
        float rockGrabTauLerpSpeed = 0.5f;
        bool rockGrabLongObjectAngularScalingEnabled = true;
        float rockGrabLongObjectReferenceLeverGameUnits = 24.0f;
        float rockGrabLongObjectMinAngularScale = 0.35f;
        bool rockGrabPivotQualityAngularScalingEnabled = true;
        float rockGrabPositionOnlyAngularScale = 0.55f;
        float rockGrabSmallObjectReferenceLeverGameUnits = 12.0f;
        float rockGrabSmallObjectAngularScale = 0.65f;
        float rockGrabLowContactSupportAngularScale = 0.75f;
        float rockGrabMinAngularAuthorityScale = 0.30f;
        float rockGrabWeakPivotTwistScale = 0.35f;

        float rockGrabMaxInertiaRatio = 10.0f;
        float rockGrabMinInertia = 0.01f;

        float rockGrabMaxDeviation = 50.0f;
        float rockGrabMaxDeviationTime = 2.0f;
        int rockGrabButtonID = 2;
        float rockThrowVelocityMultiplier = 1.5f;
        bool rockGrabControllerDerivedThrowVelocityEnabled = true;
        float rockGrabThrowObjectVelocityBlend = 0.35f;
        float rockGrabThrowTangentialVelocityScale = 1.0f;
        float rockGrabThrowMaxVelocityHavok = 12.0f;
        float rockGrabThrowAngularVelocityScale = 1.0f;
        float rockGrabThrowMaxAngularVelocityRadiansPerSecond = 18.0f;
        float rockGrabReleaseHandCollisionDelaySeconds = 0.10f;
        bool rockShoulderStashEnabled = true;
        bool rockShoulderStashUseBodyZoneColliders = true;
        bool rockShoulderStashUseHmdBackVolume = true;
        float rockShoulderStashEnterPaddingGameUnits = 5.0f;
        float rockShoulderStashExitPaddingGameUnits = 8.0f;
        float rockShoulderStashMinDwellSeconds = 0.08f;
        float rockShoulderStashMaxSpeedGameUnitsPerSecond = 140.0f;
        int rockShoulderStashRecentContactFrames = 4;
        int rockShoulderStashSustainedContactMissFrames = 18;
        RE::NiPoint3 rockShoulderStashHmdBackRightOffsetGameUnits = RE::NiPoint3(14.0f, -18.0f, -6.85f);
        RE::NiPoint3 rockShoulderStashHmdBackLeftOffsetGameUnits = RE::NiPoint3(-14.0f, -18.0f, -6.85f);
        float rockShoulderStashHmdBackRadiusGameUnits = 11.0f;
        float rockShoulderStashHmdBackEnterPaddingGameUnits = 0.0f;
        float rockShoulderStashHmdBackExitPaddingGameUnits = 2.0f;
        float rockShoulderStashHmdBackMinBehindGameUnits = 4.0f;
        bool rockShoulderStashShowCollectedNotifications = true;
        bool rockMouthConsumeEnabled = true;
        bool rockMouthConsumeAllowPoison = false;
        RE::NiPoint3 rockMouthConsumeHmdOffsetGameUnits = RE::NiPoint3(0.0f, 7.0f, -7.0f);
        float rockMouthConsumeRadiusGameUnits = 5.5f;
        float rockMouthConsumeEnterPaddingGameUnits = 0.0f;
        float rockMouthConsumeExitPaddingGameUnits = 1.0f;
        float rockMouthConsumeMinDwellSeconds = 0.08f;
        float rockMouthConsumeMaxSpeedGameUnitsPerSecond = 120.0f;
        float rockGrabVelocityDamping = 0.25f;
        bool rockGrabPlayerSpaceCompensation = true;
        float rockGrabPlayerSpaceWarpDistance = 35.0f;
        float rockGrabPlayerSpaceWarpMinRotationDegrees = 0.6f;
        bool rockGrabPlayerSpaceTransformWarpEnabled = true;
        bool rockGrabPlayerSpaceContinuousWarp = true;
        float rockGrabVisualHandMaxOffsetGameUnits = 10.0f;
        bool rockGrabLocomotionAuthorityBridgeEnabled = true;
        float rockGrabLocomotionAuthorityMaxLeadSeconds = grab_locomotion_authority_bridge::kDefaultMaxLeadSeconds;
        float rockGrabLocomotionAuthoritySmoothingHz = grab_locomotion_authority_bridge::kDefaultSmoothingHz;
        float rockGrabLocomotionAuthorityMaxOffsetGameUnits = grab_locomotion_authority_bridge::kDefaultMaxOffsetGameUnits;
        float rockGrabLocomotionAuthorityResetDistanceGameUnits = grab_locomotion_authority_bridge::kDefaultResetDistanceGameUnits;
        bool rockGrabResidualVelocityDamping = true;
        bool rockGrabNearbyDampingEnabled = true;
        float rockGrabNearbyDampingRadius = 90.0f;
        float rockGrabNearbyDampingSeconds = 0.35f;
        float rockGrabNearbyLinearDamping = 3.0f;
        float rockGrabNearbyAngularDamping = 5.5f;
        bool rockGrabHeldMassMovementSlowdownEnabled = true;
        float rockGrabHeldMassMovementMassProportion = 0.675f;
        float rockGrabHeldMassMovementMassExponent = 1.0f;
        float rockGrabHeldMassMovementMaxReduction = 75.0f;
        float rockGrabHeldMassMovementFadeOutSeconds = 5.0f;
        float rockGrabTouchAcquireDistanceGameUnits = 4.0f;
        float rockGrabNearConvergeDistanceGameUnits = 28.0f;
        float rockGrabPocketDepthGameUnits = 7.0f;
        float rockGrabPocketRadiusGameUnits = 9.0f;
        float rockGrabSeatDepthMaxGameUnits = 30.0f;
        float rockGrabSeatDepthFootprintRadiusGameUnits = 10.0f;
        float rockGrabSeatDepthSkinGameUnits = 0.5f;
        float rockGrabGripInsetGameUnits = 2.0f;
        float rockGrabGripMaxInsetGameUnits = 6.0f;
        float rockGrabConvergeMaxTimeSeconds = 0.35f;
        int rockGrabConvergeStableFrames = 3;
        float rockGrabConvergeMaxSeparatingSpeedGameUnitsPerSecond = 40.0f;
        float rockGrabAcquisitionVisualStartDistanceGameUnits = 28.0f;
        bool rockGrabMultiFingerContactValidationEnabled = true;
        int rockGrabContactQualityMode = 1;
        int rockGrabMinFingerContactGroups = 3;
        float rockGrabMinFingerContactSpreadGameUnits = 1.0f;
        float rockGrabFingerContactMeshSnapMaxDistanceGameUnits = 10.0f;
        float rockGrabSurfaceBehindPalmToleranceGameUnits = 1.5f;
        float rockSupportGripPalmClearanceGameUnits = 1.2f;
        float rockSupportGripSeatRadiusGameUnits = 10.0f;    // Jul 19: seat only on surface NEAR the palm (stops muzzle->receiver capture jumps)
        bool rockSupportGripRequiresPressEdge = true;        // Jul 19: capture only on a grip PRESS near the weapon (held grip walked into the weapon must not two-hand)  // Jul 19 seat audit: seats are zero-clearance; pull the hand out along the surface normal by this much
        // Jul 25 (pistol grip fix): lever-arm-conditioned support steering authority.
        // Below min lever the support grip retains the configured non-zero
        // authority floor; between min and full the steering blends toward the
        // firing controller's own aim; at/above full the solver is bit-exact
        // legacy (all observed rifle captures measure >= 22.4gu, pistol cups
        // ~13.3gu).
        bool rockTwoHandedLeverArmAuthorityGate = true;
        float rockTwoHandedMinSteeringLeverArmGameUnits = 15.0f;
        float rockTwoHandedFullSteeringLeverArmGameUnits = 22.0f;
        // Jul 25 (pistol grip fix B): roll about the aim axis is referenced to the LIVE
        // FIRING controller's palm normal (full strength) instead of the support palm
        // normal at half strength — the firing hand owns roll, so the gun can no longer
        // roll/pitch out of the captured firing grip. 0.0 = exact legacy twist path.
        float rockTwoHandedPrimaryRollAuthorityFactor = 1.0f;
        // Jul 27 (user: "my offhand still has no authority over the pistol ... give it authority
        // without disturbing the weapon grip"). FLOOR under the lever-arm gate above: a short
        // pistol lever no longer collapses to zero steering, it keeps this fraction. Authority is
        // then made SAFE by a rate limit rather than by removal — see rockTwoHandedMaxSteeringDegreesPerSecond.
        // Jul 27 (user: "make sure objects that fall into the hand don't bounce so much, make them
        // a bit less springy"). Software restitution damper.
        //
        // CORRECTED 2026-07-28 - the previous text here asserted that both engine levers were
        // dead ends, and BOTH claims are false in this tree. HavokMaterialRegistry::
        // registerGeneratedBodyMaterial is a real implementation (HavokMaterialRegistry.cpp:
        // constructs an hknpMaterial, calls hknpMaterialLibrary::addEntry, returns a live id)
        // and it runs on every generated collider. kMotionProperties_Linear/AngularDamping are
        // resolved at 0x18/0x1C, and NearbyGrabDamping.cpp already reads and writes through them.
        //
        // The software damper is nevertheless RETAINED, for reasons that are about fitness and
        // not availability: the material path never writes the restitution field (material+0x16),
        // whose default is already 0, and the motion-property damping slots are per-body linear/
        // angular DRAG on a SHARED motion-properties preset - damping one held prop there would
        // damp every body sharing that preset. Per-object damping needs a private
        // motion-properties addEntry, which does not exist yet. Until then: while a HAND collider
        // is in contact and the prop is moving AWAY from it
        // (that is the bounce), remove this fraction of the separating radial velocity per frame.
        // Measured justification: penetration depth tracks impact speed (39.9gu/s -> 1.14gu overlap
        // vs 11.6gu/s -> 0.11gu), so killing the bounce removes the deep re-impacts at the source
        // instead of padding the hand out to hide them. 0 = off/legacy.
        float rockHandContactRestitutionDamping = 0.55f;
        // BARE-FIST GUARD. When true, a DRAWN unarmed state with no real melee
        // weapon equipped is force-holstered (DrawWeaponMagicHands(false)) to
        // close FO4's spurious "drawn melee with nothing in hand" fallback.
        //
        // DEFAULT FALSE (Jul 31): the guard cannot tell that fallback apart from
        // a player who deliberately raised their fists, so with it on, UNARMED
        // MELEE IS IMPOSSIBLE — you throw a punch and the mod sheathes your
        // hands. Reported live as "melee not working"; a tester's 2026-07-30 log
        // shows "Bare-fist draw state blocked: holstering unarmed fallback
        // (equippedForm=00000000)" firing on the sampler while they tried.
        // Real hand-to-hand weapons (knuckles, power fists) were never affected
        // either way — they set realMeleeWeaponEquipped and are already exempt.
        bool rockBareFistGuardEnabled = false;
        // Jul 27/29: minimum support-hand steering authority. Heisenberg exposes this
        // through MCM: 0.35 preserves the balanced pre-slider floor, while 1.0 makes
        // even a short lever/pistol use the full solve. Wide long-gun grips already
        // derive 1.0 naturally. Wrist rigidity is a separate knob below, so raising
        // this no longer trades against firing-hand grip protection.
        float rockTwoHandedMinSteeringAuthority = 0.35f;
        // INERT since Jul 31: the firing hand is RIGIDLY WELDED to the weapon
        // (rotation AND translation) for the whole two-handed hold, every
        // weapon class, per owner directive — "we absolutely cannot have the
        // gun hand lose its grip on the gun in any way when 2 handing."
        // applyFiringHandLockedVisual publishes the pure weld and no longer
        // consumes this scalar; the grip-capture log echoes wristFollow=1.00.
        //
        // HISTORY (why this knob exists and must not be naively revived): a
        // follow scalar (Jul 27, protecting the sidearm wrist from off-hand
        // steering), then a lever-scoped floor (Jul 30, restoring the long-gun
        // weld), then a palm-pivot anchored blend (Jul 31 am) each tried to
        // serve wrist-fidelity AND grip integrity with one partial blend —
        // every variant leaves some palm/grip divergence, which the owner
        // rejected. Grip integrity outranks controller fidelity during
        // two-handed holds. blendFiringWristTowardController and
        // firingWristFollowFactorForLeverArm remain in TwoHandedWeaponPolicy.h
        // (tested, unconsumed) for the record.
        float rockTwoHandedFiringWristFollowFactor = 0.0f;
        // SIDEARM TWO-HANDED PISTOL HOLD.
        //
        // *** OWNER DIRECTIVE (Jul 31, final): DEFAULT OFF. ***
        // "stop wrapping the hand around the gun hand and allow 2 handing like bigger guns."
        // The owner has seen BOTH behaviours live and chose the long-gun one. With this false a
        // pistol is handled EXACTLY like a rifle: the support hand keeps whatever seat the mesh
        // contact solve produced, and no class-specific presentation rule runs at all. This flag
        // is now the ONLY thing that ever made a sidearm differ from a long gun anywhere in the
        // two-handed path (single consumer, TwoHandedGrip.cpp) — false means there is no
        // divergence left.
        //
        // KNOWN TRADEOFF, accepted by the owner: a pistol grip is small and mostly occupied by
        // the firing hand, so the contact solve can only land on the nearest real weapon surface
        // (slide/barrel — measured 10.9-20.4gu from the firing grip) while the player's real
        // off-hand stays back at the grip. Where those diverge the rendered support arm stretches
        // to reach the seat. That is the Jul-31 "offhand stretches unnaturally long" report. It is
        // NOT a bug to re-fix by re-enabling the wrap without asking — the wrap was rejected.
        // The `supportRenderedVsReal=` witness at the reseat site measures the divergence directly.
        //
        // The gun hand is unaffected either way, by construction: applyFiringHandLockedVisual
        // publishes a PURE RIGID WELD of the firing hand to the weapon, and HandAuthority gives
        // the firing arm the SAME 1.40 reach allowance as the support arm (keyed on rigidTarget),
        // so the off-hand having authority can never drag the gun hand off its grip.
        //
        // Set true to restore the authored wrap. Offsets are in the FIRING hand's RAW local basis,
        // authored for a RIGHT firing hand:
        //   X = distal (+ toward fingertips), +Y = back of hand (-Y = palm face), Z = cross-palm
        //   (+Z is PINKY-ward on the right hand). Z and the roll auto-negate for a left firing hand.
        //
        // GEOMETRY OF THE ROLL (derived analytically Jul 31, do not re-tune blindly): the support
        // palm direction is -cos(roll)*Y_F - sin(roll)*Z_F. roll=0 gives palm-to-palm; roll=+/-90
        // points the palm ALONG the grip axis (perpendicular to any wrap — this was the old
        // default and read as a karate-chop hover, never a wrap); roll~180 is the only family
        // member whose palm faces onto the firing hand — the classic support-cup. The old
        // crossPalm=-5 also parked the wrist 5gu THUMB-ward (up toward the slide). These values
        // had near-zero live screen time before Jul 31: the reseat gate was silently
        // unsatisfiable for most of its life (see TwoHandedWeaponPolicy.h).
        bool  rockSidearmTwoHandedGripReseat = false;
        float rockSidearmSupportGripOffsetFingers = -1.0f;    // gu: support wrist slightly proximal of the firing wrist
        float rockSidearmSupportGripOffsetPalmDepth = -3.5f;  // gu: displaced to the PALM side, in front of the firing fingers
        float rockSidearmSupportGripOffsetCrossPalm = -1.5f;  // gu: slight thumb-ward bias, heels nearly together
        float rockSidearmSupportGripRollDegrees = 180.0f;     // palm wraps onto the firing hand (see geometry note)
        int   rockSidearmSupportGripPoseId = 3;               // WeaponGripPoseId::VerticalForegrip — curl for wrapping the firing fingers' column
        // The real reason a pistol's off-hand broke the firing grip: solver angular gain is ~1/leverArm,
        // so a short lever swings the gun ~2x faster per unit of off-hand travel than a rifle, and on
        // FRIK v3 the firing hand's visual delivery cannot track that fast — the grip visibly diverges.
        // Capping the SPEED of the solved rotation (not its total range) lets the off-hand steer the gun
        // anywhere it likes while never outrunning the hand that has to follow it. 0 = uncapped/legacy.
        float rockTwoHandedMaxSteeringDegreesPerSecond = 220.0f;
        int rockGrabOppositionContactMaxAgeFrames = 5;
        bool rockGrabPinchPocketEnabled = true;
        bool rockGrabPinchCloseSelectionEnabled = true;
        float rockGrabPinchCompactMaxExtentGameUnits = 8.0f;
        float rockGrabPinchThinRodMaxLengthGameUnits = 18.0f;
        float rockGrabPinchThinRodMaxCrossSectionGameUnits = 4.0f;
        float rockGrabPinchMaxPocketDistanceGameUnits = 8.0f;
        float rockGrabPinchMinFingerGapGameUnits = 1.0f;
        float rockGrabPinchMaxFingerGapGameUnits = 12.0f;
        float rockGrabPinchThumbIndexMaxOpenValue = 0.45f;
        float rockGrabPinchOtherFingerCurlValue = 0.20f;
        float rockGrabPinchSurfaceInsetGameUnits = 0.5f;
        RE::NiPoint3 rockGrabPinchDetectionDirectionHandspace = RE::NiPoint3(1.0f, 0.0f, 0.0f);
        float rockGrabPinchDetectionAxisBlend = 0.65f;
        bool rockGrabHandLerpEnabled = true;
        float rockGrabHandLerpTimeMin = 0.10f;
        float rockGrabHandLerpTimeMax = 0.20f;
        float rockGrabHandLerpMinDistance = 7.0f;
        float rockGrabHandLerpMaxDistance = 14.0f;
        bool rockGrabHandReturnEnabled = true;
        float rockGrabHandReturnTimeMin = 0.10f;
        float rockGrabHandReturnTimeMax = 0.20f;
        float rockGrabHandReturnMinDistance = 7.0f;
        float rockGrabHandReturnMaxDistance = 14.0f;
        float rockGrabHandReturnMinAngleDegrees = 5.0f;
        float rockGrabHandReturnMaxAngleDegrees = 90.0f;
        bool rockGrabMeshFingerPoseEnabled = true;
        bool rockGrabMeshJointPoseEnabled = true;
        int rockGrabFingerPoseUpdateInterval = 3;
        float rockGrabFingerMinValue = 0.2f;
        float rockGrabFingerPoseSmoothingSpeed = 14.0f;
        bool rockGrabMeshLocalTransformPoseEnabled = true;
        float rockGrabFingerLocalTransformSmoothingSpeed = 14.0f;
        float rockGrabFingerLocalTransformMaxCorrectionDegrees = 35.0f;
        float rockGrabFingerSurfaceAimStrength = 0.75f;
        bool rockGrabFingerRejectBacksideHits = true;
        float rockGrabFingerSurfacePlaneToleranceGameUnits = 1.5f;
        float rockGrabFingerSweepContactRadiusGameUnits = 1.0f;
        float rockGrabFingerSweepMaxOpenValue = 2.0f;
        float rockGrabThumbSweepMaxOpenValue = 2.0f;
        float rockGrabFingerPoseResolveWindowSeconds = 2.0f;
        float rockGrabThumbOppositionStrength = 1.0f;
        float rockGrabThumbAlternateCurveStrength = 0.65f;
        bool rockGrabThumbSurfaceSafetyEnabled = true;
        float rockGrabThumbSurfaceSafetyMarginGameUnits = 1.0f;
        float rockGrabLateralWeight = 0.6f;
        float rockGrabDirectionalWeight = 0.4f;
        float rockGrabMaxTriangleDistance = 100.0f;
        bool rockGrabMeshContactOnly = true;
        bool rockGrabRequireMeshContact = true;
        bool rockGrabContactPatchEnabled = true;
        int rockGrabContactPatchProbeCount = 9;
        float rockGrabContactPatchProbeSpacingGameUnits = 3.0f;
        float rockGrabContactPatchProbeRadiusGameUnits = 2.0f;
        float rockGrabContactPatchMeshSnapMaxDistanceGameUnits = 6.0f;
        float rockGrabContactPatchMaxNormalAngleDegrees = 35.0f;
        float rockGrabAlignmentMaxSelectionToMeshDistance = 8.0f;
        bool rockGrabNodeAnchorsEnabled = true;
        bool rockGrabNodeRejectOppositeHandAnchor = true;
        bool rockPrintGrabNodeInfo = false;
        std::string rockGrabNodeNameRight = "ROCK:GrabR";
        std::string rockGrabNodeNameLeft = "ROCK:GrabL";
        std::string rockGrabNodeNameBlacklist;  // EMBED: default-empty (see rockLogPattern note, >15 chars); resetToDefaults() sets grab_node_name_policy::kDefaultGrabNodeNameBlacklist at runtime.
        bool rockSelectedCloseFingerCurlEnabled = true;
        // 0 disables the speed cutoff. A fast approach must retain the protective
        // pre-curl; dropping it for one frame lets the rendered fingertips enter
        // thin clutter before the native contact response arrives.
        float rockSelectedCloseFingerAnimMaxHandSpeed = 0.0f;
        float rockSelectedCloseFingerAnimValue = 0.9f;
        float rockPulledAngularDamping = 8.0f;
        float rockPulledGrabHandAdjustDistanceGameUnits = 10.5f;
        bool rockPullToObjectCenterEnabled = true;
        bool rockPullLongAxisPresentationEnabled = true;
        bool rockForceGrabSeatAlignmentEnabled = true;
        float rockPullPresentationMinElongationRatio = 2.0f;
        float rockPullPresentationAngularGainPerSecond = 6.0f;
        float rockPullPresentationMaxAngularSpeedRadiansPerSecond = 8.0f;
        float rockPullPresentationGripAxisTiltDegrees = 10.0f;

        RE::NiPoint3 rockRightGrabLegacyPalmPivotAHandspace = RE::NiPoint3(6.0f, -2.0f, 0.2f);
        RE::NiPoint3 rockLeftGrabLegacyPalmPivotAHandspace = RE::NiPoint3(6.0f, -2.0f, -0.2f);

        bool rockGrabHapticsEnabled = true;
        float rockGrabHapticDurationSeconds = 0.055f;
        float rockGrabHapticBaseIntensity = 0.12f;
        float rockGrabHapticMaxIntensity = 0.80f;
        float rockGrabHapticMassScale = 0.06f;
        float rockGrabHapticMassExponent = 0.60f;
        float rockPullStartHapticIntensity = 0.18f;
        float rockPullCatchHapticIntensity = 0.22f;
        float rockSelectionLockHapticIntensity = 0.15f;
        float rockSelectionLockReleaseHapticIntensity = 0.10f;
        float rockSelectionLockReleaseHapticDurationSeconds = 0.02f;
        bool rockHeldImpactHapticsEnabled = true;
        float rockHeldImpactHapticDurationSeconds = 0.035f;
        float rockHeldImpactHapticBaseIntensity = 0.12f;
        float rockHeldImpactHapticMaxIntensity = 0.85f;
        float rockHeldImpactHapticSpeedScale = 0.006f;
        float rockHeldImpactHapticMassScale = 0.035f;
        float rockHeldImpactHapticMassExponent = 0.55f;
        float rockHeldImpactHapticMinSpeedGameUnits = 8.0f;
        float rockHeldImpactHapticCooldownSeconds = 0.12f;
        float rockHeldImpactHapticDampedMultiplier = 0.55f;
        bool rockShoulderStashHapticsEnabled = true;
        float rockShoulderStashCandidateHapticDurationSeconds = 0.075f;
        float rockShoulderStashCandidateHapticBaseIntensity = 0.20f;
        float rockShoulderStashCandidateHapticIntensity = 0.42f;
        float rockShoulderStashCandidateHapticIntervalSeconds = 0.075f;
        float rockShoulderStashCommitHapticDurationSeconds = 0.12f;
        float rockShoulderStashCommitHapticIntensity = 0.85f;
        bool rockMouthConsumeHapticsEnabled = true;
        float rockMouthConsumeCandidateHapticDurationSeconds = 0.050f;
        float rockMouthConsumeCandidateHapticBaseIntensity = 0.22f;
        float rockMouthConsumeCandidateHapticIntensity = 0.45f;
        float rockMouthConsumeCandidateHapticIntervalSeconds = 0.075f;
        float rockMouthConsumeCommitHapticDurationSeconds = 0.12f;
        float rockMouthConsumeCommitHapticIntensity = 0.85f;

    private:
        void resetToDefaults();

        void readValuesFromIni(CSimpleIniA& ini);

        [[nodiscard]] bool saveRuntimeIni(CSimpleIniA& ini, const char* reason);

        void startFileWatch();

        std::string _iniFilePath;

        std::unique_ptr<filewatch::FileWatch<std::string>> _fileWatch;

        std::atomic<std::filesystem::file_time_type> _lastIniFileWriteTime;

        std::unordered_map<std::string, std::function<void(const std::string&)>> _onConfigChangedSubscribers;

        std::atomic<bool> _ignoreNextIniFileChange = false;

        std::atomic<bool> _selfIniWriteInProgress = false;

        std::atomic<std::filesystem::file_time_type> _lastSelfIniWriteTime{};

        std::atomic<bool> _reloadPending = false;

        std::atomic<bool> _nativeReadsPaused = false;
        std::atomic<std::uint32_t> _nativeReadsInFlight = 0;

        std::thread _fileWatchInitThread;
    };

    // EMBED FIX (was `inline RockConfig g_rockConfig;`): an inline global is emitted by EVERY TU that
    // includes this header and COMDAT-folded at link. In the single-DLL embed, main-DLL TUs (e.g.
    // RockGrabCoreSmoke.cpp via HandFrame.h) also include this header, so the linker could fold in a
    // main-DLL copy of the definition/constructor and construct g_rockConfig with that TU's view while
    // rock_engine's RockConfig::resetToDefaults() reads it with rock_engine's view — any offset skew
    // left a std::string member with a null buffer → memcpy-to-null CTD in resetToDefaults on first
    // config load. Making it a single, non-inline definition owned by rock_engine (RockConfig.cpp)
    // guarantees ONE constructor, matching the code that uses it.
    extern RockConfig g_rockConfig;
}
