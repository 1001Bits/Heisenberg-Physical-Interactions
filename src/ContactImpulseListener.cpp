#include "ContactImpulseListener.h"
#include "HandCollision.h"
#include "Config.h"
#include "Physics.h"

#include <spdlog/spdlog.h>
#include <REL/Relocation.h>
#include <chrono>
#include <cmath>

namespace heisenberg
{
    // =========================================================================
    // OFFSETS (from GhidraMCP decompilation analysis - January 2026)
    // =========================================================================
    
    // -------------------------------------------------------------------------
    // TWO VERSIONS OF getEventSignal! (CRITICAL DISCOVERY)
    // -------------------------------------------------------------------------
    // 1. WORLD-WIDE: hknpWorld::getEventSignal(eventType) - VR offset 0x1548170
    //    Returns a signal that fires for ALL bodies (used by FOCollisionListener)
    //    Signature: hkSignal2* (hknpWorld* world, int eventType)
    //
    // 2. BODY-SPECIFIC: hknpWorld::getEventSignal(eventType, bodyId) - VR offset 0x15481d0
    //    Returns a signal that fires ONLY for the specified body!
    //    This is what bhkTelekinesisListener uses!
    //    Signature: hkSignal2* (hknpWorld* world, int eventType, uint32_t bodyId)
    // -------------------------------------------------------------------------
    
    // World-wide getEventSignal (NOT what we need for per-body subscription)
    using GetEventSignalWorldFn = void* (*)(void* hknpWorld, int eventType);
    inline REL::Relocation<GetEventSignalWorldFn> hknpWorld_getEventSignal_World{ REL::Offset(0x1548170) };
    
    // BODY-SPECIFIC getEventSignal - THIS IS WHAT TELEKINESIS USES!
    // From decompilation of bhkTelekinesisListener::SubscribeForContacts (0x140c9d990):
    //   FUN_1415481d0(hknpWorld, 3, bodyId)  // event type 3 = CONTACT_STARTED
    using GetEventSignalBodyFn = void* (*)(void* hknpWorld, int eventType, std::uint32_t bodyId);
    inline REL::Relocation<GetEventSignalBodyFn> hknpWorld_getEventSignal_Body{ REL::Offset(0x15481d0) };
    
    // Simple global subscribe - VR offset 0x40ca60
    // From decompilation: allocates 0x20 byte wrapper, stores callback at [3]
    // Telekinesis uses this same function:
    //   FUN_14040ca60(signal, callback)
    using SignalSubscribeSimpleFn = void (*)(void* signal, void* callback);
    inline REL::Relocation<SignalSubscribeSimpleFn> hkSignal_subscribeSimple{ REL::Offset(0x40ca60) };
    
    // hknpEventType enum values (from Ghidra analysis)
    // -------------------------------------------------------------------------
    // IMPORTANT: bhkTelekinesisListener uses EVENT TYPE 3 (CONTACT_STARTED)!
    // NOT type 5 (CONTACT_IMPULSE) which has issues with KEYFRAMED bodies.
    // 
    // FOCollisionListener::OnContactImpulseEvent checks:
    //   if ((body.flags & 5) == 0) { ... calculate impulse ... }
    // This SKIPS bodies with IS_STATIC(1) | IS_KEYFRAMED(4) from impulse calc!
    // -------------------------------------------------------------------------
    namespace hknpEventType {
        constexpr int CONTACT_STARTED = 3;   // Bodies begin touching - USE THIS!
        constexpr int CONTACT_FINISHED = 4;  // Bodies stop touching  
        constexpr int CONTACT_IMPULSE = 5;   // Collision resolved - SKIPS KEYFRAMED!
        constexpr int TRIGGER_VOLUME = 6;    // Trigger volume entered/exited
    }
    
    // Havok scale
    constexpr float HAVOK_SCALE = 0.0142875f;
    constexpr float INV_HAVOK_SCALE = 1.0f / HAVOK_SCALE;
    constexpr std::uint32_t INVALID_BODY_ID = 0x7FFFFFFF;
    
    // =========================================================================
    // hknpContactEvent structure offsets (for CONTACT_STARTED events)
    // These may differ from CONTACT_IMPULSE events - verify if issues occur
    // =========================================================================
    namespace EventOffsets
    {
        constexpr std::ptrdiff_t bodyIdA = 0x08;      // int bodyIdA
        constexpr std::ptrdiff_t bodyIdB = 0x0C;      // int bodyIdB
        constexpr std::ptrdiff_t manifoldPtr = 0x10;  // ptr to manifold data (may be null for STARTED)
        constexpr std::ptrdiff_t numContacts = 0x18;  // byte numContacts
        constexpr std::ptrdiff_t flags = 0x19;        // byte flags (1 = multiple contacts)
        constexpr std::ptrdiff_t impulseArray = 0x30; // float[] impulses (only for IMPULSE events)
    }
    
    namespace ManifoldOffsets
    {
        constexpr std::ptrdiff_t contactPoint = 0x10; // float4 in Havok scale
    }
    
    // =========================================================================
    // CALLBACK IMPLEMENTATION
    // =========================================================================
    
    void ContactImpulseListener::OnContactStartedCallback(void* worldPtr, void* eventPtr)
    {
        DispatchContact(worldPtr, eventPtr, /*started=*/true);
    }

    void ContactImpulseListener::OnContactFinishedCallback(void* worldPtr, void* eventPtr)
    {
        DispatchContact(worldPtr, eventPtr, /*started=*/false);
    }

    void ContactImpulseListener::DispatchContact(void* worldPtr, void* eventPtr, bool started)
    {
        auto& listener = ContactImpulseListener::GetSingleton();

        // Only tally STARTED events so the legacy counters still mean
        // "new contacts" rather than being doubled by FINISHED pairs.
        if (started) {
            listener._totalEvents++;
        }

        std::uint64_t count = listener._totalEvents.load();
        if (started && (count <= 10 || count % 50 == 1)) {
            spdlog::info("[CONTACT] OnContactCallback FIRED! totalEvents={}, worldPtr=0x{:X}, eventPtr=0x{:X}",
                        count, reinterpret_cast<std::uintptr_t>(worldPtr), reinterpret_cast<std::uintptr_t>(eventPtr));
        }

        std::uint32_t leftId = listener._leftHandBodyId.load(std::memory_order_relaxed);
        std::uint32_t rightId = listener._rightHandBodyId.load(std::memory_order_relaxed);
        std::uint32_t leftGrabbed  = listener._leftGrabbedBodyId.load(std::memory_order_relaxed);
        std::uint32_t rightGrabbed = listener._rightGrabbedBodyId.load(std::memory_order_relaxed);

        if (leftId == INVALID_BODY_ID && rightId == INVALID_BODY_ID
            && leftGrabbed == INVALID_BODY_ID && rightGrabbed == INVALID_BODY_ID) {
            return;
        }

        ContactImpulseEvent event;
        if (!ParseEvent(worldPtr, eventPtr, event)) {
            return;
        }

        bool leftHit  = (event.bodyIdA == leftId)  || (event.bodyIdB == leftId);
        bool rightHit = (event.bodyIdA == rightId) || (event.bodyIdB == rightId);

        // Task #5: grabbed-body collision timestamp. Only STARTED updates the
        // "recency" field — FINISHED carries no useful signal for motor soften.
        if (started) {
            const std::uint64_t nowNs = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
            const bool leftGrabbedHit =
                (leftGrabbed != INVALID_BODY_ID) &&
                (event.bodyIdA == leftGrabbed || event.bodyIdB == leftGrabbed);
            const bool rightGrabbedHit =
                (rightGrabbed != INVALID_BODY_ID) &&
                (event.bodyIdA == rightGrabbed || event.bodyIdB == rightGrabbed);
            if (leftGrabbedHit) {
                listener._leftGrabbedLastCollisionNs.store(nowNs, std::memory_order_relaxed);
            }
            if (rightGrabbedHit) {
                listener._rightGrabbedLastCollisionNs.store(nowNs, std::memory_order_relaxed);
            }
        }

        if (leftHit) {
            std::uint32_t otherId = (event.bodyIdA == leftId) ? event.bodyIdB : event.bodyIdA;
            bool stillOverlapping = true;
            {
                std::scoped_lock lock(listener._overlapMutex);
                if (started) listener._leftOverlaps.insert(otherId);
                else         listener._leftOverlaps.erase(otherId);
                stillOverlapping = !listener._leftOverlaps.empty();
            }
            if (started) {
                spdlog::info("[CONTACT] LEFT HAND contact! bodyA=0x{:X} bodyB=0x{:X}",
                    event.bodyIdA, event.bodyIdB);
                listener.ProcessHandCollision(event, true);
                listener._handCollisions++;
            } else if (!stillOverlapping) {
                HandCollision::GetSingleton().ClearContactBodyId(true);
            }
        }

        if (rightHit) {
            std::uint32_t otherId = (event.bodyIdA == rightId) ? event.bodyIdB : event.bodyIdA;
            bool stillOverlapping = true;
            {
                std::scoped_lock lock(listener._overlapMutex);
                if (started) listener._rightOverlaps.insert(otherId);
                else         listener._rightOverlaps.erase(otherId);
                stillOverlapping = !listener._rightOverlaps.empty();
            }
            if (started) {
                spdlog::info("[CONTACT] RIGHT HAND contact! bodyA=0x{:X} bodyB=0x{:X}",
                    event.bodyIdA, event.bodyIdB);
                listener.ProcessHandCollision(event, false);
                listener._handCollisions++;
            } else if (!stillOverlapping) {
                HandCollision::GetSingleton().ClearContactBodyId(false);
            }
        }
    }

    std::vector<std::uint32_t> ContactImpulseListener::GetOverlappingBodies(bool isLeft) const
    {
        std::scoped_lock lock(_overlapMutex);
        const auto& set = isLeft ? _leftOverlaps : _rightOverlaps;
        return std::vector<std::uint32_t>(set.begin(), set.end());
    }
    
    bool ContactImpulseListener::ParseEvent(void* worldPtr, void* eventPtr, ContactImpulseEvent& outEvent)
    {
        if (!worldPtr || !eventPtr) {
            return false;
        }
        
        // From telekinesis callback decompilation (FUN_140c9db40):
        //   lVar4 = *param_1;  // Dereference to get hknpWorld
        // So worldPtr is hknpWorld** (pointer to pointer)
        void** worldPtrPtr = reinterpret_cast<void**>(worldPtr);
        outEvent.hknpWorld = *worldPtrPtr;
        
        // Extract body IDs (same for all contact event types)
        // From telekinesis: *(int*)(event + 8) and *(int*)(event + 0xc)
        std::uint8_t* eventBytes = reinterpret_cast<std::uint8_t*>(eventPtr);
        outEvent.bodyIdA = *reinterpret_cast<std::uint32_t*>(eventBytes + EventOffsets::bodyIdA);
        outEvent.bodyIdB = *reinterpret_cast<std::uint32_t*>(eventBytes + EventOffsets::bodyIdB);
        
        // Validate body IDs (0x7FFFFFFF = invalid)
        if (outEvent.bodyIdA == INVALID_BODY_ID || outEvent.bodyIdB == INVALID_BODY_ID) {
            return false;  // Invalid event
        }

        outEvent.totalImpulse = 0.0f;

        // For CONTACT_IMPULSE events, numContacts > 0 and impulseArray at +0x30 holds
        // one float per contact. STARTED/FINISHED events leave these zero, so the read
        // is a no-op. We still do it defensively in case a caller routes IMPULSE
        // subscriptions through us (e.g. Task #8 follow-up for grabbed dynamic bodies).
        std::uint8_t numContacts = *reinterpret_cast<std::uint8_t*>(eventBytes + EventOffsets::numContacts);
        if (numContacts > 0 && numContacts <= 16) {
            float* impulses = reinterpret_cast<float*>(eventBytes + EventOffsets::impulseArray);
            float sum = 0.0f;
            for (std::uint8_t i = 0; i < numContacts; ++i) {
                float v = impulses[i];
                // Guard against garbage (NaN/inf or absurd magnitudes from STARTED events
                // where the field is uninitialised).
                if (std::isfinite(v) && v >= 0.0f && v < 1.0e6f) {
                    sum += v;
                }
            }
            outEvent.totalImpulse = sum;
        }

        return true;
    }
    
    void ContactImpulseListener::ProcessHandCollision(const ContactImpulseEvent& event, bool isLeftHand)
    {
        std::uint32_t handBodyId = isLeftHand ?
            _leftHandBodyId.load(std::memory_order_relaxed) :
            _rightHandBodyId.load(std::memory_order_relaxed);

        std::uint32_t otherBodyId = (event.bodyIdA == handBodyId) ? event.bodyIdB : event.bodyIdA;
        HandCollision::GetSingleton().SetContactBodyId(isLeftHand, otherBodyId);

        // Read both bodies' state. We need the hand's velocity in addition to the
        // other body's velocity/mass to synthesize an impulse magnitude for
        // CONTACT_STARTED events, which don't carry one natively.
        float handVelX = 0, handVelY = 0, handVelZ = 0;
        float otherVelX = 0, otherVelY = 0, otherVelZ = 0;
        float otherInvMass = 0.0f;
        std::uint16_t otherMotionPropsId = 0;
        std::uint32_t otherFlags = 0, otherFilter = 0;
        std::int32_t handMotId = -1, otherMotId = -1;

        if (event.hknpWorld) {
            void* bodyBuf = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(event.hknpWorld) + 0x20);
            void* motBuf  = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(event.hknpWorld) + 0xE0);
            if (bodyBuf) {
                uintptr_t handEntry  = reinterpret_cast<uintptr_t>(bodyBuf) + (handBodyId  & 0xFFFF) * 0x90;
                uintptr_t otherEntry = reinterpret_cast<uintptr_t>(bodyBuf) + (otherBodyId & 0xFFFF) * 0x90;

                handMotId   = *reinterpret_cast<std::int32_t*>(handEntry  + 0x68);
                otherMotId  = *reinterpret_cast<std::int32_t*>(otherEntry + 0x68);
                otherFlags  = *reinterpret_cast<std::uint32_t*>(otherEntry + 0x40);
                otherFilter = *reinterpret_cast<std::uint32_t*>(otherEntry + 0x44);
            }
            if (motBuf) {
                if (handMotId > 0) {
                    uintptr_t m = reinterpret_cast<uintptr_t>(motBuf) + handMotId * 0x80;
                    handVelX = *reinterpret_cast<float*>(m + 0x00);
                    handVelY = *reinterpret_cast<float*>(m + 0x04);
                    handVelZ = *reinterpret_cast<float*>(m + 0x08);
                }
                if (otherMotId > 0) {
                    uintptr_t m = reinterpret_cast<uintptr_t>(motBuf) + otherMotId * 0x80;
                    otherVelX           = *reinterpret_cast<float*>(m + 0x00);
                    otherVelY           = *reinterpret_cast<float*>(m + 0x04);
                    otherVelZ           = *reinterpret_cast<float*>(m + 0x08);
                    otherInvMass        = *reinterpret_cast<float*>(m + 0x20);
                    otherMotionPropsId  = *reinterpret_cast<std::uint16_t*>(m + 0x38);
                }
            }
        }

        // Relative-velocity magnitude (Havok units/sec; 1 game unit ~= 0.0142875 hku).
        const float dvx = handVelX - otherVelX;
        const float dvy = handVelY - otherVelY;
        const float dvz = handVelZ - otherVelZ;
        const float relSpeed = std::sqrt(dvx*dvx + dvy*dvy + dvz*dvz);

        // Effective mass of the other body. invMass = 0 means infinite/static — clamp
        // to a sensible upper bound so impulse doesn't blow up against fixed geometry.
        const float otherMass = (otherInvMass > 1.0e-4f) ? (1.0f / otherInvMass) : 50.0f;

        // Prefer the real summed impulse when present (CONTACT_IMPULSE events); fall
        // back to a synthetic momentum-based proxy for CONTACT_STARTED.
        float impulseMag = event.totalImpulse;
        if (impulseMag <= 0.0f) {
            impulseMag = relSpeed * otherMass;
        }

        spdlog::info("[CONTACT] {} HAND 0x{:X} hit OTHER 0x{:X}: "
            "otherFlags=0x{:08X} otherFilter=0x{:08X} otherInvMass={:.4f} otherMotProps={} "
            "handVel=({:.2f},{:.2f},{:.2f}) otherVel=({:.2f},{:.2f},{:.2f}) "
            "relSpeed={:.2f} otherMass={:.2f} impulse={:.2f} (real={:.2f})",
            isLeftHand ? "LEFT" : "RIGHT", handBodyId, otherBodyId,
            otherFlags, otherFilter, otherInvMass, otherMotionPropsId,
            handVelX, handVelY, handVelZ,
            otherVelX, otherVelY, otherVelZ,
            relSpeed, otherMass, impulseMag, event.totalImpulse);

        // Haptics: intensity carries the collision magnitude; duration is a short
        // fixed pulse (HandCollision scales intensity*duration internally).
        constexpr float kPulseSeconds = 0.05f;
        HandCollision::GetSingleton().TriggerCollisionHaptics(isLeftHand, impulseMag, kPulseSeconds);
    }
    
    // =========================================================================
    // SUBSCRIPTION API (BODY-SPECIFIC, LIKE TELEKINESIS)
    // =========================================================================
    
    void ContactImpulseListener::SetWorld(void* bhkWorld)
    {
        if (!bhkWorld) {
            spdlog::error("[CONTACT_LISTENER] SetWorld called with null bhkWorld");
            return;
        }
        
        _bhkWorld = bhkWorld;
        _hknpWorld = Physics::GetHknpWorldFromBhk(reinterpret_cast<RE::bhkWorld*>(bhkWorld));
        
        if (_hknpWorld) {
            spdlog::info("[CONTACT_LISTENER] World set: bhkWorld=0x{:X}, hknpWorld=0x{:X}",
                        reinterpret_cast<std::uintptr_t>(_bhkWorld),
                        reinterpret_cast<std::uintptr_t>(_hknpWorld));
        } else {
            spdlog::error("[CONTACT_LISTENER] Failed to get hknpWorld from bhkWorld");
        }
    }
    
    bool ContactImpulseListener::SubscribeForBody(std::uint32_t bodyId, bool isLeft)
    {
        if (!_hknpWorld) {
            spdlog::error("[CONTACT_LISTENER] Cannot subscribe - no hknpWorld set");
            return false;
        }
        
        if (bodyId == INVALID_BODY_ID) {
            spdlog::error("[CONTACT_LISTENER] Cannot subscribe - invalid bodyId");
            return false;
        }
        
        // If a recreated hand body gets a new ID, replace the logical
        // subscription state before subscribing the new body. We do not yet have
        // the Havok signal unsubscribe call, so stale callbacks are made harmless
        // by DispatchContact's current-body-ID gate.
        const bool alreadySubscribed = isLeft ? _leftSubscribed : _rightSubscribed;
        const std::uint32_t previousBodyId = isLeft
            ? _leftHandBodyId.load(std::memory_order_relaxed)
            : _rightHandBodyId.load(std::memory_order_relaxed);
        if (alreadySubscribed) {
            if (previousBodyId == bodyId) {
                spdlog::trace("[CONTACT_LISTENER] {} hand already subscribed to body 0x{:X}",
                              isLeft ? "Left" : "Right", bodyId);
                return true;
            }
            spdlog::warn("[CONTACT_LISTENER] {} hand replacing stale subscription body 0x{:X} -> 0x{:X}",
                         isLeft ? "Left" : "Right", previousBodyId, bodyId);
            UnsubscribeBody(isLeft);
        }
        
        spdlog::info("[CONTACT_LISTENER] Subscribing {} hand (bodyId=0x{:X}) to CONTACT_STARTED/FINISHED events...",
                    isLeft ? "left" : "right", bodyId);

        // Subscribe to both STARTED (type 3) and FINISHED (type 4) so we can
        // maintain the overlap set used for HIGGS-style grab candidate selection.
        void* startedSignal  = hknpWorld_getEventSignal_Body(_hknpWorld, hknpEventType::CONTACT_STARTED,  bodyId);
        void* finishedSignal = hknpWorld_getEventSignal_Body(_hknpWorld, hknpEventType::CONTACT_FINISHED, bodyId);

        if (!startedSignal) {
            spdlog::error("[CONTACT_LISTENER] getEventSignal_Body(STARTED) returned null for bodyId=0x{:X}", bodyId);
            return false;
        }
        // Finished signal is best-effort — if it's missing we still get the core
        // haptics path from STARTED; the overlap set will just accumulate.
        if (!finishedSignal) {
            spdlog::warn("[CONTACT_LISTENER] getEventSignal_Body(FINISHED) returned null for bodyId=0x{:X} — overlap set will not prune", bodyId);
        }

        try {
            hkSignal_subscribeSimple(startedSignal, reinterpret_cast<void*>(&OnContactStartedCallback));
            if (finishedSignal) {
                hkSignal_subscribeSimple(finishedSignal, reinterpret_cast<void*>(&OnContactFinishedCallback));
            }
            spdlog::info("[CONTACT_LISTENER] Subscribe succeeded (started={:p}, finished={:p})",
                         startedSignal, finishedSignal ? finishedSignal : nullptr);
        }
        catch (...) {
            spdlog::error("[CONTACT_LISTENER] Exception during subscribe");
            return false;
        }

        if (isLeft) {
            _leftHandBodyId.store(bodyId, std::memory_order_relaxed);
            _leftSubscribed = true;
            _leftStartedSignal  = startedSignal;
            _leftFinishedSignal = finishedSignal;
        } else {
            _rightHandBodyId.store(bodyId, std::memory_order_relaxed);
            _rightSubscribed = true;
            _rightStartedSignal  = startedSignal;
            _rightFinishedSignal = finishedSignal;
        }

        spdlog::info("[CONTACT_LISTENER] Successfully subscribed {} hand", isLeft ? "left" : "right");
        return true;
    }
    
    void ContactImpulseListener::UnsubscribeBody(bool isLeft)
    {
        bool& subscribed = isLeft ? _leftSubscribed : _rightSubscribed;
        void*& startedSignal  = isLeft ? _leftStartedSignal  : _rightStartedSignal;
        void*& finishedSignal = isLeft ? _leftFinishedSignal : _rightFinishedSignal;
        std::atomic<std::uint32_t>& bodyId = isLeft ? _leftHandBodyId : _rightHandBodyId;

        if (!subscribed) {
            return;
        }

        // TODO: Implement proper unsubscribe when we figure out the API.
        // Also clear the overlap set so a later re-subscribe starts clean.
        {
            std::scoped_lock lock(_overlapMutex);
            auto& set = isLeft ? _leftOverlaps : _rightOverlaps;
            set.clear();
        }

        subscribed = false;
        startedSignal  = nullptr;
        finishedSignal = nullptr;
        bodyId.store(INVALID_BODY_ID, std::memory_order_relaxed);

        spdlog::info("[CONTACT_LISTENER] Unsubscribed {} hand", isLeft ? "left" : "right");
    }
    
    void ContactImpulseListener::Unsubscribe()
    {
        UnsubscribeBody(true);   // Left
        UnsubscribeBody(false);  // Right
        _bhkWorld = nullptr;
        _hknpWorld = nullptr;
        spdlog::info("[CONTACT_LISTENER] Fully unsubscribed");
    }
    
    // =========================================================================
    // GRABBED-BODY SUBSCRIPTION (Task #5 — motor soften on collision)
    // =========================================================================
    //
    // Subscribes to CONTACT_STARTED on the grabbed object so HeldBodyGrab can
    // read GetGrabbedBodyTimeSinceCollision() each frame and lerp motor tau
    // toward the "colliding" config value while contact is recent. Only
    // STARTED is needed here — we want an edge-triggered "has it just hit
    // something?" signal, not the overlap tracking the hand uses.

    bool ContactImpulseListener::SubscribeForGrabbedBody(std::uint32_t bodyId, bool isLeft)
    {
        if (!_hknpWorld) {
            spdlog::warn("[CONTACT_LISTENER] Grab subscribe: no hknpWorld — skip");
            return false;
        }
        if (bodyId == INVALID_BODY_ID) {
            return false;
        }

        // Unsubscribe any previous grabbed body for this hand first — the grab
        // manager may hand us a different body if the previous release was
        // skipped (e.g. scripted drop).
        UnsubscribeGrabbedBody(isLeft);

        void* startedSignal = hknpWorld_getEventSignal_Body(_hknpWorld,
                                                            hknpEventType::CONTACT_STARTED, bodyId);
        if (!startedSignal) {
            spdlog::warn("[CONTACT_LISTENER] Grab subscribe: null CONTACT_STARTED signal for body 0x{:08X}", bodyId);
            return false;
        }

        try {
            hkSignal_subscribeSimple(startedSignal, reinterpret_cast<void*>(&OnContactStartedCallback));
        } catch (...) {
            spdlog::error("[CONTACT_LISTENER] Grab subscribe: exception during subscribeSimple");
            return false;
        }

        if (isLeft) {
            _leftGrabbedBodyId.store(bodyId, std::memory_order_relaxed);
            _leftGrabbedStartedSignal = startedSignal;
            _leftGrabbedLastCollisionNs.store(0, std::memory_order_relaxed);
        } else {
            _rightGrabbedBodyId.store(bodyId, std::memory_order_relaxed);
            _rightGrabbedStartedSignal = startedSignal;
            _rightGrabbedLastCollisionNs.store(0, std::memory_order_relaxed);
        }

        spdlog::info("[CONTACT_LISTENER] Subscribed {} grabbed body 0x{:08X} for motor-soften",
                     isLeft ? "left" : "right", bodyId);
        return true;
    }

    void ContactImpulseListener::UnsubscribeGrabbedBody(bool isLeft)
    {
        // We intentionally do not try to "un-hook" the signal — Havok's
        // subscribeSimple has no matching unsubscribe path we've mapped. Dropping
        // the body ID is enough: DispatchContact checks the ID before marking
        // the timestamp, so stale callbacks are harmless.
        if (isLeft) {
            _leftGrabbedBodyId.store(INVALID_BODY_ID, std::memory_order_relaxed);
            _leftGrabbedStartedSignal = nullptr;
            _leftGrabbedLastCollisionNs.store(0, std::memory_order_relaxed);
        } else {
            _rightGrabbedBodyId.store(INVALID_BODY_ID, std::memory_order_relaxed);
            _rightGrabbedStartedSignal = nullptr;
            _rightGrabbedLastCollisionNs.store(0, std::memory_order_relaxed);
        }
    }

    double ContactImpulseListener::GetGrabbedBodyTimeSinceCollision(bool isLeft) const
    {
        const std::uint64_t lastNs = isLeft
            ? _leftGrabbedLastCollisionNs.load(std::memory_order_relaxed)
            : _rightGrabbedLastCollisionNs.load(std::memory_order_relaxed);
        if (lastNs == 0) {
            return 1.0e9;  // No collision recorded — huge sentinel
        }
        const std::uint64_t nowNs = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t deltaNs = (nowNs > lastNs) ? (nowNs - lastNs) : 0;
        return static_cast<double>(deltaNs) * 1.0e-9;
    }

    // Legacy API compatibility
    void ContactImpulseListener::SetHandBodyIds(std::uint32_t leftBodyId, std::uint32_t rightBodyId)
    {
        if (leftBodyId != INVALID_BODY_ID) {
            SubscribeForBody(leftBodyId, true);
        }
        if (rightBodyId != INVALID_BODY_ID) {
            SubscribeForBody(rightBodyId, false);
        }
    }
    
    void ContactImpulseListener::ClearHandBodyIds()
    {
        Unsubscribe();
    }
}
