#pragma once

#include "physics-interaction/hand/DynamicHandCollisionFeedbackPolicy.h"
#include "physics-interaction/hand/DynamicHandCollisionStabilityPolicy.h"
#include "physics-interaction/hand/DynamicHandCollisionTransitionPolicy.h"
#include "physics-interaction/hand/DynamicHandCollisionTelemetry.h"
#include "physics-interaction/hand/DynamicHandTwinTargets.h"
#include "physics-interaction/native/BethesdaPhysicsBody.h"
#include "physics-interaction/native/GeneratedKeyframedBodyDrive.h"
#include "physics-interaction/native/HavokPhysicsTiming.h"
#include "physics-interaction/native/PhysicsCallbackQuiescenceGate.h"

#include "RE/Havok/hknpShape.h"
#include "RE/Havok/hknpWorld.h"
#include "RE/NetImmerse/NiTransform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace rock
{
    class BodyBoneColliderSet;
    class Hand;
    struct PhysicsFrameContext;
    struct HandFrameInput;

    struct DynamicHandCollisionFrameReadiness
    {
        bool rightWorldStopOperational = false;
        bool leftWorldStopOperational = false;
    };

    struct DynamicHandCollisionContactBodySnapshot
    {
        bool valid = false;
        bool isLeft = false;
        bool primaryPalmAnchor = false;
        bool sampledVelocityValid = false;
        std::uint32_t bodyId = 0x7FFF'FFFFu;
        hand_collider_semantics::HandColliderRole role =
            hand_collider_semantics::HandColliderRole::PalmAnchor;
        RE::NiPoint3 sampledVelocityHavok{};
    };

    /*
     * Dynamic world collision uses DYNAMIC twins of all twenty production hand
     * roles plus one optional merged ForeArm1->Hand proxy per side. The
     * forearm is admitted only when its source frame is independently clean;
     * the current embedded host seam publishes clean hand truth only, so its
     * IK-ancestor forearm stays retired rather than receiving an invalid full
     * hand translation rebase. The admitted twins
     * chase their published role frames with engine hard-keyframe velocities
     * every physics substep, on a dedicated extended layer that collides with
     * static world surfaces and generated equipped-weapon hulls. Those
     * surfaces clip proxy velocity inside the solver (true multi-plane
     * contact); the rendered FRIK hand follows the COMBINED position deviation
     * (sequential projection over per-body deviations). Sustained contact is
     * stabilized as a world-space stop target, so additional controller
     * pressure cannot creep the rendered hand through a wall. Authority is
     * strictly one-directional (clean pre-authority hand -> twins -> render):
     * the live HandBoneColliderSet/BodyBoneColliderSet role publications are
     * translated back into FRIK's clean same-frame hand basis before they are
     * queued. Dynamic-body readback never becomes a later physics target, and
     * the previous render correction cannot feed back into the next frame.
     * The twins are not gameplay contact evidence and remain excluded from
     * clutter, actors, projectiles, other hands, and generated body colliders.
     * When a firing/support grip owns a hand, its stronger visual authority
     * still gates this one-way proxy correction, preventing a hand/weapon
     * feedback loop while retaining free-hand world and weapon stopping.
     *
     * Threading: updateFrame runs on the main game thread; the drive flush runs
     * on the physics step thread and publishes fixed per-body telemetry through
     * atomics that updateFrame consumes one substep later (~1/270 s).
     */
    class DynamicHandCollisionRuntime
    {
    public:
        static constexpr std::size_t kPalmSlot = dynamic_hand_collision_telemetry::kPalmSlot;
        static constexpr std::size_t kFirstForearmSlot = dynamic_hand_collision_telemetry::kFirstForearmSlot;
        static constexpr std::size_t kBodiesPerHand = dynamic_hand_collision_telemetry::kBodiesPerHand;
        static_assert(
            kFirstForearmSlot ==
                hand_collider_semantics::kHandColliderBodyCountPerHand,
            "dynamic hand twins must cover every production hand role");
        static_assert(kBodiesPerHand <= 32u, "contact telemetry uses a fixed 32-bit body mask");

        void setPhysicsCallbackGate(PhysicsCallbackQuiescenceGate* gate) { _physicsCallbackGate = gate; }

        [[nodiscard]] DynamicHandCollisionFrameReadiness updateFrame(const PhysicsFrameContext& frame,
            bool physicsWritesAllowed,
            const Hand& rightHand,
            const Hand& leftHand,
            const BodyBoneColliderSet& bodyBoneColliders,
            bool rightHandWeaponEquipped,
            bool leftSupportGripActive,
            bool rightVisualReturnActive,
            bool leftVisualReturnActive);
        void flushPendingPhysicsDrive(RE::hknpWorld* world, const havok_physics_timing::PhysicsTimingSample& timing);
        /*
         * Post-solve deviation sampling (physics step thread, after-solve
         * phase). Two-stage measurement against the SAME substep's targets:
         * the residual vs the COMMANDED (velocity-limited) target detects
         * contact — an unobstructed hard-keyframe drive lands exactly on it,
         * so tracking motion (hand, locomotion, room scale) produces exactly
         * zero — and only in contact is the render deviation published,
         * measured vs the REQUESTED (pre-limit) target so it equals the true
         * blocked depth. Sampling pre-collide leaks one substep of tracking
         * lag (sessions 1-2 twitch/drag); rendering the commanded residual
         * saturates at the dt-dependent limiter distance (sessions 3-5
         * milli-punch pulsing).
         */
        void samplePostSolveDeviations(RE::hknpWorld* world);
        void retireAll(void* bhkWorld);
        /*
         * World-loss teardown path. The cached body/world pointers are no
         * longer authoritative, so this only drops ROCK-owned references and
         * wrapper state; it must not invoke native body retirement.
         */
        void abandonAllAfterWorldLoss();
        void reset();
        // Main-thread snapshot/event access. Future provider adapters must copy
        // from here on the main thread rather than retain runtime-owned state.
        [[nodiscard]] bool getTelemetrySnapshot(dynamic_hand_collision_telemetry::Snapshot& outSnapshot) const;
        [[nodiscard]] bool isTransitionCollisionSuppressedAtomic() const
        {
            return _transitionCollisionSuppressedAtomic.load(
                std::memory_order_acquire);
        }
        [[nodiscard]] dynamic_hand_collision_telemetry::HapticEvents consumeHapticEvents();
        /*
         * Relinquish only rendered-hand authority to a later, stronger
         * same-frame owner.  The dynamic proxy bank remains alive and keeps
         * producing contact evidence, so a reciprocal weapon/hand stop can
         * retain and release cleanly without the two visual writers fighting.
         * Main thread only.
         */
        void clearVisualForStrongerOwner(bool isLeft);
        // Main-thread publication seam for the callback's immutable generated
        // body registry.  Forearm twins are intentionally excluded: reciprocal
        // gun stopping is a hand-surface behavior.
        [[nodiscard]] bool tryGetContactBodySnapshot(
            bool isLeft,
            std::size_t bodyIndex,
            DynamicHandCollisionContactBodySnapshot& outSnapshot) const;

        [[nodiscard]] bool tryGetContactBodySnapshot(
            std::uint32_t bodyId,
            DynamicHandCollisionContactBodySnapshot& outSnapshot) const;

        /*
         * Debug-overlay accessor; main thread only (creation/retire happen on
         * the same thread as the overlay publish).
         */
        [[nodiscard]] RE::hknpBodyId proxyBodyIdForDebug(bool isLeft, std::size_t bodyIndex) const
        {
            if (bodyIndex >= kBodiesPerHand) {
                return RE::hknpBodyId{ 0x7FFF'FFFF };
            }
            const auto& slot = _hands[isLeft ? 1u : 0u].bodies[bodyIndex];
            return slot.created ? slot.body.getBodyId() : RE::hknpBodyId{ 0x7FFF'FFFF };
        }

    private:
        struct PhysicsTelemetrySample
        {
            RE::NiPoint3 requestedTargetWorldGame{};
            RE::NiPoint3 commandedTargetWorldGame{};
            RE::NiPoint3 liveBodyWorldGame{};
            RE::NiPoint3 targetVelocityWorldGameUnitsPerSecond{};
            float approachSpeedGameUnitsPerSecond = 0.0f;
            float physicsDeltaSeconds = 0.0f;
            bool valid = false;
            bool targetVelocityValid = false;
            bool contactActive = false;
            bool recoveryTeleport = false;
        };

        /*
         * Physics writes and the main thread reads these fields. Components
         * stay atomic to avoid a C++ data race; the odd/even sequence is a
         * bounded seqlock that prevents accepting a mixed-substep sample.
         */
        struct AtomicPhysicsTelemetry
        {
            std::atomic<std::uint64_t> sequence{ 0 };
            std::atomic<float> requestedX{ 0.0f };
            std::atomic<float> requestedY{ 0.0f };
            std::atomic<float> requestedZ{ 0.0f };
            std::atomic<float> commandedX{ 0.0f };
            std::atomic<float> commandedY{ 0.0f };
            std::atomic<float> commandedZ{ 0.0f };
            std::atomic<float> liveX{ 0.0f };
            std::atomic<float> liveY{ 0.0f };
            std::atomic<float> liveZ{ 0.0f };
            std::atomic<float> targetVelocityX{ 0.0f };
            std::atomic<float> targetVelocityY{ 0.0f };
            std::atomic<float> targetVelocityZ{ 0.0f };
            std::atomic<float> approachSpeed{ 0.0f };
            std::atomic<float> physicsDeltaSeconds{ 0.0f };
            std::atomic<bool> valid{ false };
            std::atomic<bool> targetVelocityValid{ false };
            std::atomic<bool> contactActive{ false };
            std::atomic<bool> recoveryTeleport{ false };
        };

        struct ProxySlot
        {
            BethesdaPhysicsBody body{};
            // BethesdaPhysicsBody::create stores the supplied body-name
            // pointer in persistent physics-system data. Keep its backing
            // storage in this fixed slot for the body's complete lifetime.
            std::string bodyName{};
            RE::hknpShape* shape = nullptr;
            GeneratedKeyframedBodyDriveState driveState{};
            RE::hknpWorld* createdWorld = nullptr;
            void* createdBhkWorld = nullptr;
            std::uint64_t createdGeometryGeneration = 0;
            bool created = false;
            /*
             * Physics-thread-only handshake between the pre-collide drive and
             * the after-solve deviation sample of the same substep. Two targets
             * with different jobs: the COMMANDED (velocity-limited) target
             * detects contact — an unobstructed drive lands exactly on it, so
             * any residual means the solver blocked the body — while the
             * REQUESTED (pre-limit wand intent) target measures how deep the
             * blocked intent is. Rendering the commanded residual instead
             * saturates the deviation at maxLinearVelocity * driveDt, a value
             * that steps with every substep-count/framerate change: the
             * in-and-out "milli-punch" pulsing of in-game sessions 3-5.
             */
            bool droveThisSubstep = false;
            RE::NiPoint3 commandedTargetGame{};
            RE::NiPoint3 requestedTargetGame{};
            // Physics-thread copy of the last post-solve CONTACT deviation
            // (zero while tracking freely); feeds the next substep's contact
            // press cap (drive must lean on an established contact, not slam
            // the full deficit into it). The hysteresis flag keeps a grazing
            // contact from flapping around the enter threshold.
            RE::NiPoint3 lastPostSolveDeviationGame{};
            bool lastPostSolveDeviationValid = false;
            bool lastPostSolveContact = false;
            RE::NiPoint3 droveTargetVelocityGameUnitsPerSecond{};
            float drovePhysicsDeltaSeconds = 0.0f;
            bool droveTargetVelocityValid = false;
            bool droveRecoveryTeleport = false;
            /*
             * Divergence must PERSIST before a recovery teleport fires
             * (physics thread only). Without the dwell, a hand fighting a wall
             * near the divergence threshold limit-cycles: teleport into the
             * geometry, solver ejection, divergence again — the harsh
             * position-reset stutter of the third in-game session. The dwell
             * resets after each teleport, so it doubles as the re-fire
             * cooldown.
            */
            float divergenceDwellSeconds = 0.0f;
            std::atomic<bool> teleportedAtomic{ false };
            std::atomic<bool> rebuildRequestedAtomic{ false };
            AtomicPhysicsTelemetry physicsTelemetry{};
        };

        struct HandSlots
        {
            std::array<ProxySlot, kBodiesPerHand> bodies{};
            RE::NiPoint3 appliedDeviation{};
            dynamic_hand_collision_stability::
                ContactHoldState<RE::NiPoint3> contactHold{};
            RE::NiPoint3 lastCleanHandTranslation{};
            bool lastCleanHandTranslationValid = false;
            bool visualActive = false;
            /*
             * Post-teleport visual recovery: while this window is open the
             * render-side filter uses a slow eased glide instead of the snappy
             * contact smoothing, so a divergence recovery reads as the hand
             * smoothly rejoining the controller instead of a position snap.
             */
            float teleportRecoverySecondsRemaining = 0.0f;
            bool physicsContactActive = false;
            std::atomic<std::uint64_t> contactEntrySequenceAtomic{ 0 };
            std::atomic<float> contactEntryApproachSpeedAtomic{ 0.0f };
            std::atomic<std::uint32_t> contactEntryMaskAtomic{ 0 };
            dynamic_hand_collision_feedback::ContactPulseState hapticState{};
        };

        bool ensureSlotCreated(ProxySlot& slot,
            bool isLeft,
            std::size_t bodyIndex,
            const PhysicsFrameContext& frame,
            const Hand& hand,
            const BodyBoneColliderSet& bodyBoneColliders,
            const dynamic_hand_twin::TwinSlotFrame& twinFrame,
            std::uint64_t geometryGeneration);
        void retireSlot(ProxySlot& slot, void* bhkWorld);
        void retireHand(HandSlots& handSlots, void* bhkWorld, bool isLeft);
        void clearVisual(HandSlots& handSlots, bool isLeft);
        void applyTransitionCollisionSuppression(RE::hknpWorld* world, bool suppressCollision);
        static void publishPhysicsTelemetry(ProxySlot& slot, const PhysicsTelemetrySample& sample);
        [[nodiscard]] static bool readPhysicsTelemetry(const ProxySlot& slot, PhysicsTelemetrySample& outSample, std::uint64_t& outSequence);
        static void clearPhysicsContactState(ProxySlot& slot);
        void updateHandHaptic(
            HandSlots& handSlots,
            dynamic_hand_collision_telemetry::HandSample& handTelemetry,
            bool authorityAllowsFeedback,
            float deltaSeconds);

        std::array<HandSlots, 2> _hands{};
        dynamic_hand_collision_telemetry::Snapshot _telemetrySnapshot{};
        dynamic_hand_collision_telemetry::HapticEvents _pendingHapticEvents{};
        std::uint64_t _telemetryUpdateSequence = 0;
        std::uint32_t _logCounter = 0;
        PhysicsCallbackQuiescenceGate* _physicsCallbackGate = nullptr;
        dynamic_hand_collision_transition::State _transitionState{};
        bool _transitionCollisionSuppressed = false;
        std::atomic<bool> _transitionCollisionSuppressedAtomic{ false };
    };
}
