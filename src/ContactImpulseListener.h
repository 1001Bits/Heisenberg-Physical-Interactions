#pragma once

/**
 * ContactImpulseListener - Body-specific contact event subscription for F4VR
 * 
 * CRITICAL DISCOVERY (January 2026):
 * Havok 2014 hknp* API has TWO getEventSignal functions:
 *   1. hknpWorld::getEventSignal(eventType) at 0x1548170 - WORLD-WIDE events
 *   2. hknpWorld::getEventSignal(eventType, bodyId) at 0x15481d0 - BODY-SPECIFIC events
 * 
 * bhkTelekinesisListener uses the BODY-SPECIFIC version with event type 3 (CONTACT_STARTED)!
 * This ensures we only get events for OUR bodies, not every collision in the world.
 * 
 * Also, FOCollisionListener::OnContactImpulseEvent SKIPS KEYFRAMED bodies:
 *   if ((body.flags & 5) == 0) { ... } // SKIPS IS_STATIC(1) | IS_KEYFRAMED(4)
 * So CONTACT_IMPULSE (type 5) won't work for our KEYFRAMED hand bodies.
 * We must use CONTACT_STARTED (type 3) instead.
 * 
 * How it works:
 * 1. Wait until hand bodies are created (have body IDs)
 * 2. Call hknpWorld::getEventSignal(CONTACT_STARTED, leftBodyId) 
 * 3. Call hkSignal_subscribeSimple(signal, callback)
 * 4. Repeat for right hand body
 * 5. Our callback fires ONLY when those specific bodies collide
 * 
 * Key offsets (VR 1.2.72):
 * - hknpWorld::getEventSignal(eventType): VR 0x1548170 (world-wide)
 * - hknpWorld::getEventSignal(eventType, bodyId): VR 0x15481d0 (body-specific)
 * - hkSignal_subscribeSimple: VR 0x40ca60
 * - CONTACT_STARTED enum value: 3
 */

#include <RE/Fallout.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace heisenberg
{
    /**
     * Contact event data extracted from hknpContactEvent.
     *
     * totalImpulse semantics:
     *   - For CONTACT_IMPULSE events, ParseEvent sums the per-contact float array
     *     at offset 0x30 (numContacts entries) into a true impulse magnitude.
     *   - For CONTACT_STARTED events, Havok doesn't populate that array; the field
     *     stays 0.0f here, and ProcessHandCollision synthesizes a momentum proxy
     *     (relSpeed * otherMass) at dispatch time for haptics/adaptive-grip.
     */
    struct ContactImpulseEvent
    {
        std::uint32_t bodyIdA = 0x7FFFFFFF;
        std::uint32_t bodyIdB = 0x7FFFFFFF;
        void* hknpWorld = nullptr;
        float totalImpulse = 0.0f;
        RE::NiPoint3 contactPoint{};
    };

    /**
     * ContactImpulseListener - Subscribes to CONTACT_STARTED events per-body via hkSignal2
     */
    class ContactImpulseListener
    {
    public:
        static ContactImpulseListener& GetSingleton()
        {
            static ContactImpulseListener instance;
            return instance;
        }

        /**
         * Store the bhkWorld for later body-specific subscriptions
         * Called once when the world becomes available
         */
        void SetWorld(void* bhkWorld);

        /**
         * Subscribe to CONTACT_STARTED events for a specific hand body
         * Call this AFTER the hand body is created with a valid bodyId
         * @param bodyId The hand body's hknpBodyId
         * @param isLeft true for left hand, false for right
         * @return true if subscription succeeded
         */
        bool SubscribeForBody(std::uint32_t bodyId, bool isLeft);

        /**
         * Unsubscribe a hand body
         */
        void UnsubscribeBody(bool isLeft);

        /**
         * Unsubscribe all and clear state
         */
        void Unsubscribe();

        /**
         * Legacy API - Set hand body IDs (will auto-subscribe if world is set)
         */
        void SetHandBodyIds(std::uint32_t leftBodyId, std::uint32_t rightBodyId);

        /**
         * Legacy API - Clear hand body IDs (will unsubscribe)
         */
        void ClearHandBodyIds();

        /**
         * Check if subscribed
         */
        bool IsSubscribed() const { return _leftSubscribed || _rightSubscribed; }
        bool IsLeftSubscribed() const { return _leftSubscribed; }
        bool IsRightSubscribed() const { return _rightSubscribed; }

        /**
         * Statistics
         */
        std::uint64_t GetTotalEventsReceived() const { return _totalEvents; }
        std::uint64_t GetHandCollisions() const { return _handCollisions; }

        /**
         * HIGGS-style overlap set: every body currently in CONTACT_STARTED with
         * the hand (and not yet CONTACT_FINISHED). Gated by
         * g_config.useCollisionOverlapForGrabCandidates; Grab.cpp consults this
         * as a candidate pool before falling back to its spatial query.
         * Returns a snapshot to avoid holding the internal mutex across reads.
         */
        std::vector<std::uint32_t> GetOverlappingBodies(bool isLeft) const;

        /**
         * HIGGS motor-soften path (Task #5): when a HeldBody-grabbed object
         * impacts the world, we need to soften the constraint motor tau so the
         * object doesn't fight the contact. We subscribe to CONTACT_STARTED on
         * the grabbed object's body and record the most recent collision time;
         * UpdateConstraintMotors reads it and lerps tau toward the configured
         * "colliding" value while contact is recent.
         */
        bool SubscribeForGrabbedBody(std::uint32_t bodyId, bool isLeft);
        void UnsubscribeGrabbedBody(bool isLeft);

        /**
         * Seconds since the last CONTACT_STARTED event on this hand's grabbed
         * body. Returns a large value (>= 1e6) when no collision has been
         * recorded yet, so callers can use `timeSince < threshold` without a
         * separate "has collision" flag.
         */
        double GetGrabbedBodyTimeSinceCollision(bool isLeft) const;

    private:
        ContactImpulseListener() = default;
        ~ContactImpulseListener() = default;
        ContactImpulseListener(const ContactImpulseListener&) = delete;
        ContactImpulseListener& operator=(const ContactImpulseListener&) = delete;

        /**
         * The callback function that Havok calls for contact events
         * From telekinesis decompilation - signature:
         *   void callback(void* worldPtr, void* eventPtr)
         * There are separate entry points for STARTED and FINISHED so the per-
         * body signal subscription can distinguish event types (the event
         * struct itself does not carry the type).
         */
        static void OnContactStartedCallback(void* worldPtr, void* eventPtr);
        static void OnContactFinishedCallback(void* worldPtr, void* eventPtr);

        /**
         * Parse raw event data into our struct
         */
        static bool ParseEvent(void* worldPtr, void* eventPtr, ContactImpulseEvent& outEvent);

        /**
         * Process a hand collision event
         */
        void ProcessHandCollision(const ContactImpulseEvent& event, bool isLeftHand);

        // World reference
        void* _bhkWorld = nullptr;
        void* _hknpWorld = nullptr;

        // Hand body IDs
        std::atomic<std::uint32_t> _leftHandBodyId{ 0x7FFFFFFF };
        std::atomic<std::uint32_t> _rightHandBodyId{ 0x7FFFFFFF };

        // Per-body subscription state (two signals per hand: started + finished)
        bool _leftSubscribed = false;
        bool _rightSubscribed = false;
        void* _leftStartedSignal = nullptr;
        void* _leftFinishedSignal = nullptr;
        void* _rightStartedSignal = nullptr;
        void* _rightFinishedSignal = nullptr;

        // Grabbed-body subscription (Task #5: motor soften on collision).
        // Holds the last CONTACT_STARTED steady-clock timestamp in nanoseconds.
        // uint64_t is always lock-free on x64; a sentinel of 0 means "no event yet".
        std::atomic<std::uint32_t> _leftGrabbedBodyId{ 0x7FFFFFFF };
        std::atomic<std::uint32_t> _rightGrabbedBodyId{ 0x7FFFFFFF };
        std::atomic<std::uint64_t> _leftGrabbedLastCollisionNs{ 0 };
        std::atomic<std::uint64_t> _rightGrabbedLastCollisionNs{ 0 };
        void* _leftGrabbedStartedSignal = nullptr;
        void* _rightGrabbedStartedSignal = nullptr;

        // Overlap set (Task #12). Mutated on the physics callback thread and
        // read from the main thread via GetOverlappingBodies.
        mutable std::mutex _overlapMutex;
        std::unordered_set<std::uint32_t> _leftOverlaps;
        std::unordered_set<std::uint32_t> _rightOverlaps;

        // Shared processing path for both callbacks; started=true adds to the
        // overlap set, started=false removes from it. Haptics only fire on
        // started to avoid double-pulsing on release.
        static void DispatchContact(void* worldPtr, void* eventPtr, bool started);

        // Statistics
        std::atomic<std::uint64_t> _totalEvents{ 0 };
        std::atomic<std::uint64_t> _handCollisions{ 0 };
    };
}
