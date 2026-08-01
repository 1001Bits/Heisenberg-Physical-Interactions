#include "ThrownObjectTracker.h"

#include "Config.h"
#include "F4VROffsets.h"
#include "Physics.h"

#include <RE/Bethesda/TESBoundObjects.h>
#include <RE/Bethesda/MagicItems.h>
#include <REL/Relocation.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace heisenberg
{
    namespace
    {
        // Per-body Havok signal API. These offsets are owned locally now that the
        // former standalone contact-listener module has been removed.
        using GetEventSignalBodyFn = void* (*)(void* hknpWorld, int eventType, std::uint32_t bodyId);
        REL::Relocation<GetEventSignalBodyFn> g_getEventSignalBody{ REL::Offset(0x15481d0) };

        using SignalSubscribeSimpleFn = void (*)(void* signal, void* callback);
        REL::Relocation<SignalSubscribeSimpleFn> g_subscribeSimple{ REL::Offset(0x40ca60) };

        // hknpWorld_enableBodyFlags(world, bodyId, flags, mode) — allocates per-body contact
        // signal slots when CONTACT_MODIFIER is enabled. Without this, getEventSignal_Body
        // returns null for ordinary clutter (only bodies created with the flag have signals).
        // Same offset HandCollision uses.
        using EnableBodyFlagsFn = void (*)(void* hknpWorld, std::uint32_t bodyId, std::uint32_t flags, int mode);
        REL::Relocation<EnableBodyFlagsFn> g_enableBodyFlags{ REL::Offset(0x153c090) };
        constexpr std::uint32_t kBodyFlagsContactModifierPlusKeepAwake = 0x08020000;

        // TESHavokUtilities::FindBodyIdRef(hknpBSWorld&, hknpBodyId) -> TESObjectREFR*
        // Same offset Physics.cpp uses; we re-declare so we don't depend on
        // its static-namespace helper.
        using FindBodyIdRefFn = RE::TESObjectREFR* (*)(void* hknpBSWorld, RE::hknpBodyId bodyId);
        REL::Relocation<FindBodyIdRefFn> g_findBodyIdRef{ REL::Offset(0x626690) };

        // Actor::AttackAlarm(victim, aggressor, bool) — the assault-crime core the engine's
        // real weapon-hit path (Actor::AttackedBy) calls. Builds a kAttack(3) Crime, sets the
        // victim's crime faction PC-enemy (SetPCAnEnemy), enumerates witness guards +
        // SetCrimeReaction, applies bounty, CallforHelp, fires the assault StoryEvent. This —
        // NOT boolFlags or ProcessHurtfulBody (which attributes no aggressor) — is what makes
        // protected NPCs (Diamond City Security) aggro the player. VR 0x140dd46b0.
        // (Offsets RE-verified by decompilation against Fallout4VR.exe, image base 0x140000000.)
        using AttackAlarmFn = void (*)(RE::Actor* victim, RE::Actor* aggressor, bool a_arg3);
        REL::Relocation<AttackAlarmFn> g_attackAlarm{ REL::Offset(0xdd46b0) };

        // AIProcess::EnterCombat(victimProc, victim, aggressor, null) — mirrors what the
        // Papyrus SendAssaultAlarm engine impl does right after AttackAlarm. VR 0x140e8f700.
        using EnterCombatFn = void (*)(RE::AIProcess* victimProc, RE::Actor* victim, RE::Actor* aggressor, RE::Actor* a_null);
        REL::Relocation<EnterCombatFn> g_enterCombat{ REL::Offset(0xe8f700) };

        constexpr int kEventContactStarted = 3;
        constexpr std::uint32_t kInvalidBodyId = 0x7FFFFFFF;
        constexpr float kHavokWorldScale = 0.0142875f;       // game units → havok units
        constexpr std::uint64_t kEntryLifetimeNs = 5'000'000'000ULL;  // 5 seconds
        constexpr float kMinThrowSpeed = 50.0f;              // game units/s; below this we don't track

        // F4 SOUND_LEVEL: 0=Silent, 1=Normal, 2=Loud, 3=VeryLoud (256/512/1536)
        constexpr int kSoundLevelSilent   = 0;
        constexpr int kSoundLevelNormal   = 1;
        constexpr int kSoundLevelLoud     = 2;
        constexpr int kSoundLevelVeryLoud = 3;

        // HIGGS Skyrim mass thresholds for sound binning (form-weight proxy).
        // F4 clutter weights: caps ~0.1, bottle ~0.5, toaster ~3, coffee pot ~5.
        constexpr float kMassThreshSilent = 0.5f;
        constexpr float kMassThreshNormal = 1.0f;
        constexpr float kMassThreshLoud   = 5.0f;

        int GetSoundLevelByMass(float mass)
        {
            if (mass < kMassThreshSilent) return kSoundLevelSilent;
            if (mass < kMassThreshNormal) return kSoundLevelNormal;
            if (mass < kMassThreshLoud)   return kSoundLevelLoud;
            return kSoundLevelVeryLoud;
        }

        float GetFormWeight(RE::TESObjectREFR* refr)
        {
            if (!refr) return 0.0f;
            auto* base = refr->GetObjectReference();
            if (!base) return 0.0f;
            // Dispatch by form type — TESWeightForm is a base class but the
            // weight member lives at different offsets per concrete type, so
            // we cast to the correct subclass and call its GetFormWeight().
            // Forms that publicly inherit TESWeightForm (verified in
            // CommonLibF4 headers). WEAP/ARMO use per-instance data instead
            // and need a different path — left at 0 for now since they're
            // not typical thrown clutter.
            switch (base->GetFormType()) {
                case RE::ENUM_FORM_ID::kMISC:
                    return static_cast<RE::TESObjectMISC*>(base)->GetFormWeight();
                case RE::ENUM_FORM_ID::kALCH:
                    return static_cast<RE::AlchemyItem*>(base)->GetFormWeight();
                case RE::ENUM_FORM_ID::kAMMO:
                    return static_cast<RE::TESAmmo*>(base)->GetFormWeight();
                case RE::ENUM_FORM_ID::kKEYM:
                    return static_cast<RE::TESKey*>(base)->GetFormWeight();
                case RE::ENUM_FORM_ID::kNOTE:
                    return static_cast<RE::BGSNote*>(base)->GetFormWeight();
                default:
                    return 0.0f;
            }
        }

        std::uint64_t NowNs()
        {
            return static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        }

        // SEH-only helpers — __try cannot coexist with C++ unwinding in the
        // same function, so each protected operation lives in its own leaf.

        void SetTelekinesisObjectSeh(RE::TESObjectREFR* refr, RE::PlayerCharacter* player)
        {
            __try {
                TESHavokUtilities_SetTelekinesisObject(refr, player);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        void ClearTelekinesisObjectSeh(RE::TESObjectREFR* refr)
        {
            __try {
                TESHavokUtilities_ClearTelekinesisObject(refr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        void* GetEventSignalBodySeh(void* hknpWorld, std::uint32_t bodyId)
        {
            __try {
                return g_getEventSignalBody(hknpWorld, kEventContactStarted, bodyId);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        bool EnableBodyFlagsSeh(void* hknpWorld, std::uint32_t bodyId, std::uint32_t flags)
        {
            __try {
                g_enableBodyFlags(hknpWorld, bodyId, flags, 0);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Engine-correct assault escalation — same pipeline a real weapon hit runs via
        // Actor::AttackedBy. Files the kAttack crime against the victim's crime faction so
        // guards/faction members aggro the PLAYER (works for protected NPCs without the
        // permanent boolFlags mutation). MUST run on the game thread (we do — drained from
        // HookPostPhysics). Gated by the victim being able to detect the player; the caller
        // raises a detection event first when configured.
        bool SendAssaultAlarmSeh(RE::Actor* victim, RE::Actor* player)
        {
            __try {
                g_attackAlarm(victim, player, false);
                if (victim->currentProcess) {
                    g_enterCombat(victim->currentProcess, victim, player, nullptr);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool SubscribeSimpleSeh(void* signal, void* callback)
        {
            __try {
                g_subscribeSimple(signal, callback);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ParseEventSeh(void* worldPtr, void* eventPtr,
                           void*& outHknpWorld, std::uint32_t& outBodyA, std::uint32_t& outBodyB)
        {
            __try {
                outHknpWorld = *reinterpret_cast<void**>(worldPtr);
                auto* bytes = reinterpret_cast<std::uint8_t*>(eventPtr);
                outBodyA = *reinterpret_cast<std::uint32_t*>(bytes + 0x08);
                outBodyB = *reinterpret_cast<std::uint32_t*>(bytes + 0x0C);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Pass the bodyId via opaque pointer so this leaf has no C++ object locals.
        RE::TESObjectREFR* FindBodyIdRefSeh(void* hknpWorld, void* bodyIdPtr)
        {
            __try {
                RE::TESObjectREFR* r = g_findBodyIdRef(hknpWorld, *static_cast<RE::hknpBodyId*>(bodyIdPtr));
                if (r && r->formID == 0) r = nullptr;
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
        }

        // Apply a controlled amount of health damage (positive = hurt) via the
        // engine's health-damage handler, attributed to the player. This is the
        // sole damage owner; ProcessHurtfulBody would apply native damage again.
        bool ApplyHealthDamageSeh(RE::Actor* actor, RE::Actor* attacker, float damage)
        {
            __try {
                actor->HandleHealthDamage(attacker, damage);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Force a visible push on the actor's character controller. Engine path is
        // bhkCharacterController::SetVelocityModifier(NiPoint3A& dir, float mag).
        // direction MUST be a 16-byte aligned float[4] (NiPoint3A layout).
        bool SetVelocityModifierSeh(void* charController, void* alignedDir, float magnitude)
        {
            __try {
                bhkCharacterController_SetVelocityModifier(charController, alignedDir, magnitude);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Native push-actor-away helper that Game.PushActorAway uses internally.
        // Queues the actual knock to the game thread via TaskQueueInterface so it's
        // safe to call from the contact callback.
        bool KnockExplosionSeh(void* aiProcess, RE::Actor* actor,
                               RE::NiPoint3* sourcePos, float magnitude)
        {
            __try {
                AIProcess_KnockExplosion(aiProcess, actor, sourcePos, magnitude);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool QueueDestructibleSeh(void* taskQueue, RE::TESObjectREFR* refr, float damage)
        {
            __try {
                TaskQueueInterface_QueueUpdateDestructibleObject(
                    taskQueue, refr, damage, nullptr, false);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool DetectionEventSeh(void* aiProcess, RE::Actor* player,
                               const RE::NiPoint3& pos, int level, RE::TESObjectREFR* source)
        {
            __try {
                AIProcess_SetActorsDetectionEvent(aiProcess, player, pos, level, source);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    void ThrownObjectTracker::SetWorld(void* bhkWorld)
    {
        std::scoped_lock lock(_mutex);

        void* nextHknpWorld = nullptr;
        if (bhkWorld) {
            nextHknpWorld =
                Physics::GetHknpWorldFromBhk(reinterpret_cast<RE::bhkWorld*>(bhkWorld));
        }

        // Body IDs and their native signal subscriptions are scoped to one hknpWorld.
        // Reusing this set after an interior/world transition makes a recycled body ID
        // look "already subscribed", silently disabling thrown-object impacts there.
        if (bhkWorld != _bhkWorld || nextHknpWorld != _hknpWorld) {
            _subscribedBodyIds.clear();
        }

        _bhkWorld = bhkWorld;
        _hknpWorld = nextHknpWorld;
        if (!bhkWorld) {
            return;
        }
        if (_hknpWorld) {
            spdlog::info("[THROWN] World set: bhkWorld={:p}, hknpWorld={:p}", bhkWorld, _hknpWorld);
        } else {
            spdlog::warn("[THROWN] SetWorld: failed to resolve hknpWorld from bhkWorld {:p}", bhkWorld);
        }
    }

    void ThrownObjectTracker::Reset()
    {
        std::vector<RE::NiPointer<RE::TESObjectREFR>> taggedRefs;
        {
            std::scoped_lock lock(_mutex);
            taggedRefs.reserve(_byBodyId.size());
            for (auto& [bodyId, entry] : _byBodyId) {
                (void)bodyId;
                if (auto ref = entry.refrHandle.get()) {
                    taggedRefs.push_back(std::move(ref));
                }
            }
            // We intentionally don't try to "unsubscribe" — Havok subscribeSimple
            // has no symmetric remove API mapped, and stale callbacks check the
            // bodyId map before doing anything.
            _byBodyId.clear();
            _actorHitCooldown.clear();
            _pendingImpacts.clear();
            _subscribedBodyIds.clear();
            _bhkWorld = nullptr;
            _hknpWorld = nullptr;
        }
        for (auto& ref : taggedRefs) {
            ClearTelekinesisObjectSeh(ref.get());
        }
    }

    void ThrownObjectTracker::OnThrown(RE::TESObjectREFR* refr, std::uint32_t bodyId,
                                       const RE::NiPoint3& throwVelocity)
    {
        if (!g_config.impactDamageEnabled && !g_config.impactDestroyEnabled
            && !g_config.impactDetectionEvent && !g_config.impactHitEvent) {
            return;  // Whole feature disabled
        }
        if (!refr || bodyId == kInvalidBodyId) {
            return;
        }

        // Opportunistic GC runs BEFORE the throwable early-out below, so a
        // session that only ever throws grenades still expires stale entries
        // instead of letting the map grow.
        Tick();

        // THROWABLES ARE VANILLA'S. A grenade/mine/Molotov already carries its
        // own impact contract — the engine decides when it detonates, how much
        // damage it does and who it aggros. This tracker would run a SECOND,
        // competing pass over the same throw:
        //   - 9999 "self damage" destructible destruction on the FIRST contact
        //     (see the destructible block below, which explicitly handles kWEAP
        //     base forms), so a Molotov detonates the instant it grazes
        //     anything — reported as "molotovs explode too quickly";
        //   - a capped impact-damage pass on top of the explosion;
        //   - a duplicate AI detection event.
        // None of that is wanted for something that is already a weapon, so
        // leave the whole throw to the engine.
        if (auto* baseForm = refr->GetObjectReference();
            baseForm && baseForm->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
            const auto weaponType =
                static_cast<RE::TESObjectWEAP*>(baseForm)->weaponData.type.get();
            if (weaponType == RE::WEAPON_TYPE::kGrenade ||
                weaponType == RE::WEAPON_TYPE::kMine) {
                spdlog::debug(
                    "[THROWN] {:08X} is a throwable weapon — leaving impact handling to the game",
                    refr->formID);
                return;
            }
        }

        const float speedSq = throwVelocity.x * throwVelocity.x
                            + throwVelocity.y * throwVelocity.y
                            + throwVelocity.z * throwVelocity.z;
        if (speedSq < kMinThrowSpeed * kMinThrowSpeed) {
            return;  // gentle drop, not a throw
        }
        const float speed = std::sqrt(speedSq);

        // Resolve the thrown reference's current world on every release. A
        // non-null cached world can still be stale after an interior/cell
        // transition whose load message did not bracket this exact frame.
        void* thrownBhkWorld = nullptr;
        if (auto* cell = refr->GetParentCell()) {
            thrownBhkWorld = cell->GetbhkWorld();
        }
        bool needsWorldRefresh = false;
        {
            std::scoped_lock lock(_mutex);
            needsWorldRefresh =
                thrownBhkWorld &&
                thrownBhkWorld != _bhkWorld;
        }
        if (needsWorldRefresh) {
            SetWorld(thrownBhkWorld);
        }

        void* hknpWorld = nullptr;
        {
            std::scoped_lock lock(_mutex);
            hknpWorld = _hknpWorld;
        }
        // Lazy-init from the thrown refr's parent cell. HandCollision normally
        // primes the world via SetWorld, but it's gated behind bEnableHandCollision;
        // users with that off would otherwise lose impact effects entirely.
        if (!hknpWorld) {
            if (auto* cell = refr->GetParentCell()) {
                if (auto* bhkWorld = cell->GetbhkWorld()) {
                    SetWorld(bhkWorld);
                    std::scoped_lock lock(_mutex);
                    hknpWorld = _hknpWorld;
                }
            }
        }
        if (!hknpWorld) {
            spdlog::warn("[THROWN] OnThrown: no hknpWorld available, skipping bodyId=0x{:08X}", bodyId);
            return;
        }

        // Enable the contact-modifier flag on the body. Without this, the engine never
        // allocates per-body signal slots and getEventSignal_Body returns null — ordinary
        // clutter doesn't carry these flags by default (only bodies created with them,
        // like HandCollision's hand bodies, do). Add+keep-awake matches what HandCollision sets.
        if (!EnableBodyFlagsSeh(hknpWorld, bodyId, kBodyFlagsContactModifierPlusKeepAwake)) {
            spdlog::warn("[THROWN] enableBodyFlags AV for body 0x{:08X} — skipping", bodyId);
            return;
        }

        // Subscribe to per-body CONTACT_STARTED - but only once per body per world session.
        // There is no unsubscribe path (see the class comment), so re-throwing the same
        // object (pick up, throw, repeat) would otherwise add another identical
        // subscription every time: after N throws every contact of that body invokes the
        // callback N times, each doing a mutex-guarded map lookup, and the engine-side
        // subscriber list grows for the world's lifetime.
        bool alreadySubscribed = false;
        {
            std::scoped_lock lock(_mutex);
            alreadySubscribed = !_subscribedBodyIds.insert(bodyId).second;
        }
        if (!alreadySubscribed) {
            void* signal = GetEventSignalBodySeh(hknpWorld, bodyId);
            if (!signal) {
                spdlog::warn("[THROWN] OnThrown: getEventSignal_Body returned null for body 0x{:08X} (post-enableFlags)", bodyId);
                std::scoped_lock lock(_mutex);
                _subscribedBodyIds.erase(bodyId);
                return;
            }
            if (!SubscribeSimpleSeh(signal, reinterpret_cast<void*>(&OnContactStartedCallback))) {
                spdlog::error("[THROWN] subscribeSimple threw for body 0x{:08X}", bodyId);
                std::scoped_lock lock(_mutex);
                _subscribedBodyIds.erase(bodyId);
                return;
            }
        }

        Entry entry;
        entry.refrHandle = refr->GetHandle();
        entry.hknpWorld = hknpWorld;
        entry.throwSpeed = speed;
        entry.thrownMass = GetFormWeight(refr);
        entry.expiryNs = NowNs() + kEntryLifetimeNs;
        entry.dispatched = false;

        {
            std::scoped_lock lock(_mutex);
            entry.generation = _nextThrowGeneration++;
            if (_nextThrowGeneration == 0) {
                _nextThrowGeneration = 1;
            }
            _byBodyId[bodyId] = std::move(entry);
        }

        // Tag only after the complete tracking setup has committed. Earlier
        // code tagged first and returned on any flag/signal/subscription
        // failure, leaving the object permanently attributed to the player.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            SetTelekinesisObjectSeh(refr, player);
        }

        spdlog::info("[THROWN] Tracking refr {:08X} body 0x{:08X} speed={:.1f} u/s",
                     refr->formID, bodyId, speed);
    }

    void ThrownObjectTracker::Tick()
    {
        const std::uint64_t now = NowNs();
        struct ExpiredEntry
        {
            RE::NiPointer<RE::TESObjectREFR> refr;
            std::uint32_t bodyId = kInvalidBodyId;
            void* world = nullptr;
        };
        std::vector<ExpiredEntry> expired;
        {
            std::scoped_lock lock(_mutex);
            for (auto it = _byBodyId.begin(); it != _byBodyId.end();) {
                if (it->second.expiryNs != 0 && now > it->second.expiryNs) {
                    expired.push_back(
                        ExpiredEntry{
                            it->second.refrHandle.get(),
                            it->first,
                            it->second.hknpWorld });
                    it = _byBodyId.erase(it);
                } else {
                    ++it;
                }
            }

            // Cooldown entries need only survive beyond their configured
            // suppression window. Expire them during the same cheap Tick so
            // hitting new actors over a long session cannot grow the map
            // without bound.
            const auto cooldownRetentionNs =
                static_cast<std::uint64_t>(
                    (std::max)(
                        5.0f,
                        g_config.impactActorCooldown * 2.0f) *
                    1'000'000'000.0f);
            for (auto it = _actorHitCooldown.begin();
                 it != _actorHitCooldown.end();) {
                if (now >= it->second &&
                    now - it->second > cooldownRetentionNs) {
                    it = _actorHitCooldown.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& entry : expired) {
            if (entry.refr) {
                ClearTelekinesisObjectSeh(entry.refr.get());
            }
            // CATCH FIX (Jul 18): flight is over — drop the CCD look-ahead that the
            // release path enabled (tiny epsilon: the <=0 path doesn't write the field).
            if (entry.world && entry.bodyId != kInvalidBodyId) {
                heisenberg::Physics::TrySetBodyCollisionLookAhead(
                    entry.world,
                    entry.bodyId,
                    0.001f);
            }
        }
    }

    void ThrownObjectTracker::OnContactStartedCallback(void* worldPtr, void* eventPtr)
    {
        if (!worldPtr || !eventPtr) {
            return;
        }

        // ParseEventSeh owns the native callback layout: bodyIds at +8/+0xC,
        // worldPtr is hknpWorld** (one indirection to the actual hknpWorld).
        std::uint32_t bodyA = 0;
        std::uint32_t bodyB = 0;
        void* hknpWorld = nullptr;
        if (!ParseEventSeh(worldPtr, eventPtr, hknpWorld, bodyA, bodyB)) {
            return;
        }
        if (bodyA == kInvalidBodyId || bodyB == kInvalidBodyId) {
            return;
        }

        auto& self = GetSingleton();

        // Figure out which side is the thrown body. Either bodyA or bodyB must
        // be in our map (we subscribed per-body, so callbacks for unrelated
        // bodies should not arrive — but be defensive).
        std::uint32_t thrownId = kInvalidBodyId;
        std::uint32_t otherId  = kInvalidBodyId;
        std::uint64_t throwGeneration = 0;
        {
            std::scoped_lock lock(self._mutex);
            auto itA = self._byBodyId.find(bodyA);
            auto itB = self._byBodyId.find(bodyB);
            if (itA != self._byBodyId.end() &&
                itA->second.hknpWorld == hknpWorld &&
                !itA->second.dispatched) {
                thrownId = bodyA;
                otherId  = bodyB;
                throwGeneration = itA->second.generation;
                itA->second.dispatched = true;
            } else if (itB != self._byBodyId.end() &&
                       itB->second.hknpWorld == hknpWorld &&
                       !itB->second.dispatched) {
                thrownId = bodyB;
                otherId  = bodyA;
                throwGeneration = itB->second.generation;
                itB->second.dispatched = true;
            }
        }
        if (thrownId == kInvalidBodyId) {
            return;
        }

        // Resolve the CONTACTED body to a refr HERE, on the worker thread, while `otherId`
        // is the fresh contact-instant body index. (FindBodyIdRef is a read of the body→refr
        // map — the original inline path did exactly this on the worker thread. Deferring it
        // to the game-thread drain a frame later resolved a recycled/stale index to nothing,
        // so every hit logged target=world.) Only the resolved formID is queued; all actor
        // MUTATION still happens on the game thread in DispatchImpact.
        std::uint32_t targetFormId = 0;
        if (hknpWorld) {
            RE::hknpBodyId rawOther{};
            std::memcpy(&rawOther, &otherId, sizeof(otherId));
            RE::TESObjectREFR* targetRefr = FindBodyIdRefSeh(hknpWorld, &rawOther);
            if (targetRefr) {
                targetFormId = targetRefr->formID;
            }
        }
        if (targetFormId) {
            spdlog::info("[THROWN] contact: thrown 0x{:08X} vs body 0x{:08X} -> refr {:08X}",
                         thrownId, otherId, targetFormId);
        } else {
            spdlog::info("[THROWN] contact: thrown 0x{:08X} vs body 0x{:08X} -> world/unresolved",
                         thrownId, otherId);
        }

        // THREADING: this callback runs on a Havok WORKER thread. Do NOT touch actor
        // health/AI/flags here. Queue the resolved formID + thrown body id; the game-thread
        // drain re-looks-up the refr (LookupByID is game-thread-safe) and applies effects.
        {
            std::scoped_lock lock(self._mutex);
            self._pendingImpacts.push_back(
                PendingImpact{
                    hknpWorld,
                    thrownId,
                    throwGeneration,
                    targetFormId });
        }
    }

    void ThrownObjectTracker::DrainPendingImpacts()
    {
        // Game thread only (called from HookPostPhysics). Move the queue out under the
        // lock, then dispatch each impact with no lock held (HandleContact re-locks to
        // snapshot the entry, and DispatchImpact makes engine calls).
        std::vector<PendingImpact> pending;
        {
            std::scoped_lock lock(_mutex);
            if (_pendingImpacts.empty()) {
                return;
            }
            pending.swap(_pendingImpacts);
        }
        for (const auto& imp : pending) {
            HandleContact(
                imp.hknpWorld,
                imp.thrownBodyId,
                imp.throwGeneration,
                imp.targetFormId);
        }
    }

    void ThrownObjectTracker::HandleContact(
        void* hknpWorld,
        std::uint32_t thrownBodyId,
        std::uint64_t throwGeneration,
        std::uint32_t targetFormId)
    {
        // Snapshot the entry under the lock, but do all engine calls outside it.
        Entry entrySnapshot;
        {
            std::scoped_lock lock(_mutex);
            auto it = _byBodyId.find(thrownBodyId);
            if (it == _byBodyId.end() ||
                it->second.hknpWorld != hknpWorld ||
                it->second.generation != throwGeneration) {
                return;
            }
            entrySnapshot = it->second;
        }

        auto thrownPtr = entrySnapshot.refrHandle.get();
        if (!thrownPtr) {
            return;
        }
        RE::TESObjectREFR* thrownRefr = thrownPtr.get();

        // Target was resolved to a formID at contact time (callback). Re-look it up here on
        // the game thread (LookupByID is safe). 0 = world / no refr.
        RE::TESObjectREFR* targetRefr = nullptr;
        if (targetFormId != 0) {
            if (auto* form = RE::TESForm::GetFormByID(targetFormId)) {
                targetRefr = form->As<RE::TESObjectREFR>();
            }
        }

        DispatchImpact(thrownRefr, targetRefr, entrySnapshot.throwSpeed,
                       entrySnapshot.thrownMass, thrownBodyId);
    }

    void ThrownObjectTracker::DispatchImpact(RE::TESObjectREFR* thrownRefr,
                                             RE::TESObjectREFR* targetRefr,
                                             float impactSpeed,
                                             float thrownMass,
                                             std::uint32_t thrownBodyId)
    {
        // The body ID is used to select and retire the tracked throw before
        // dispatch.  Impact mutation intentionally uses the retained reference.
        (void)thrownBodyId;

        if (!thrownRefr) {
            return;
        }

        spdlog::info("[THROWN] Impact: thrown={:08X} target={} speed={:.1f} mass={:.2f}",
                     thrownRefr->formID,
                     targetRefr ? std::to_string(targetRefr->formID) : "world",
                     impactSpeed, thrownMass);

        const RE::NiPoint3 contactPos = thrownRefr->data.location;
        auto* player = RE::PlayerCharacter::GetSingleton();

        // 1. ACTOR HIT — damage (gated by bImpactDamageEnabled) + aggro (ALWAYS). The
        // aggro path used to be nested inside the damage gate AND inside the charController
        // / currentProcess null-checks, so disabling impact damage — or a null charController
        // / process — silently suppressed aggro. Now: outer gate is just "hit an actor";
        // damage is sub-gated; aggro always runs.
        // Resolve the victim only from the body that actually participated in this contact.
        // A former proximity fallback selected the nearest live actor after a floor/wall
        // impact and then applied the complete damage/knockback/assault pipeline to an NPC
        // the object never touched.
        RE::Actor* actor = targetRefr ? targetRefr->As<RE::Actor>() : nullptr;
        if (actor && (actor == player || actor->IsDead(true))) actor = nullptr;

        if (!actor) {
            spdlog::info("[THROWN] impact has no directly contacted live actor (resolved to {}) — no actor mutation",
                         targetRefr ? "a non-actor refr" : "world geometry");
        } else if (g_config.impactActorCooldown > 0.0f && [&] {
            // Per-actor cooldown: don't re-process the same NPC when a single thrown object
            // bounces off them. Mirrors PLANCK's contact-cooldown pattern.
            const std::uint64_t now = NowNs();
            const std::uint64_t cdNs = static_cast<std::uint64_t>(
                g_config.impactActorCooldown * 1'000'000'000.0f);
            std::scoped_lock lock(_mutex);
            auto it = _actorHitCooldown.find(actor->formID);
            if (it != _actorHitCooldown.end() && now - it->second < cdNs) {
                return true;
            }
            _actorHitCooldown[actor->formID] = now;
            return false;
        }()) {
            spdlog::debug("[THROWN] actor {:08X} on cooldown ({:.2f}s) — skipping",
                          actor->formID, g_config.impactActorCooldown);
        } else {
                // --- DAMAGE + explicit physics reaction (only when bImpactDamageEnabled) ---
                if (g_config.impactDamageEnabled) {
                    // Controlled impact damage. Native ProcessHurtfulBody is
                    // velocity-dominated and also applies health damage, so it
                    // must not run after this capped manual path.
                    const float kMaxImpactDamage = 50.0f;
                    float damage =
                        thrownMass * impactSpeed * 0.01f *
                        g_config.impactDamageMult;
                    if (damage < 1.0f) damage = 1.0f;
                    const float cap =
                        kMaxImpactDamage *
                        g_config.impactDamageMult;
                    if (damage > cap) damage = cap;

                    if (ApplyHealthDamageSeh(
                            actor,
                            player,
                            damage)) {
                        spdlog::info(
                            "[THROWN] Impact damage {:.1f} → actor {:08X} "
                            "(mass={:.2f} speed={:.1f})",
                            damage,
                            actor->formID,
                            thrownMass,
                            impactSpeed);
                    } else {
                        spdlog::warn(
                            "[THROWN] HandleHealthDamage threw for actor {:08X}",
                            actor->formID);
                    }

                    // Force a visible reaction via AIProcess::KnockExplosion — the native
                    // function `Game.PushActorAway` calls internally. Engine computes the
                    // push direction as (actor_pos - source_pos), so we pass the thrown
                    // object's location as the source. This is deliberately
                    // separate from health damage and cannot apply a second
                    // native physics-damage pass.
                    //
                    // Magnitude scaling (Bethesda convention from PushActorAway docs):
                    //   <1.0   = no visible reaction
                    //   1.0-2.0 = stumble / hit reaction
                    //   3.0+   = ragdoll
                    if (auto* aiProcess = actor->currentProcess) {
                        // Two-tier reaction tuned by object MASS (not just speed):
                        //   * Light objects (bowl, bottle, plate; mass < kRagdollMinMass):
                        //     STAGGER only — magnitude capped below the ragdoll threshold, so
                        //     the NPC is knocked back / stumbles but stays on their feet no
                        //     matter how hard you throw. (Previously a light object thrown fast
                        //     reached mag 3+ via speed alone and ragdolled them.)
                        //   * Heavy objects (mass >= kRagdollMinMass, thrown fast): may knock
                        //     the NPC down.
                        // PushActorAway magnitude convention: <1 none, ~1-2 stumble, 3+ ragdoll.
                        constexpr float kKnockMinMass   = 0.2f;    // below: no push (caps, screws)
                        constexpr float kKnockMinSpeed  = 250.0f;
                        constexpr float kRagdollMinMass = 3.0f;    // only objects this heavy can knock down
                        constexpr float kStaggerMagCap  = 1.2f;    // light-object ceiling: stumble, never fall
                        constexpr float kRagdollMagCap  = 5.0f;    // heavy-object ceiling
                        float magnitude = 0.0f;
                        const bool canRagdoll = (thrownMass >= kRagdollMinMass);
                        if (thrownMass >= kKnockMinMass && impactSpeed >= kKnockMinSpeed) {
                            const float speedHku = impactSpeed * kHavokWorldScale;
                            const float massClamped = (std::min)(thrownMass, 10.0f);
                            magnitude = speedHku * massClamped * 0.15f;

                            // NPC size resists knockback: refScale (×100) cubed is a mass
                            // proxy, so a super mutant/behemoth needs a far harder hit than
                            // a human to go down, while a human (scale 1.0) is unaffected.
                            float npcScale = actor->refScale > 0 ? actor->refScale / 100.0f : 1.0f;
                            float sizeResist = npcScale * npcScale * npcScale;
                            if (sizeResist > 1.0f) magnitude /= sizeResist;

                            // Mass-gated cap: light objects can never reach ragdoll magnitude.
                            const float cap = canRagdoll ? kRagdollMagCap : kStaggerMagCap;
                            if (magnitude > cap) magnitude = cap;
                        }

                        if (magnitude > 0.0f) {
                            RE::NiPoint3 sourcePos = thrownRefr->data.location;
                            if (KnockExplosionSeh(aiProcess, actor, &sourcePos, magnitude)) {
                                spdlog::info("[THROWN] KnockExplosion → actor {:08X} mag={:.2f} [{}] (speed={:.1f} mass={:.2f} scale={:.2f})",
                                             actor->formID, magnitude, canRagdoll ? "ragdoll-capable" : "stagger-only",
                                             impactSpeed, thrownMass, actor->refScale / 100.0f);
                            } else {
                                spdlog::warn("[THROWN] KnockExplosion threw for actor {:08X}", actor->formID);
                            }
                        } else {
                            spdlog::debug("[THROWN] actor {:08X} hit but not knocked (mass={:.2f}<{:.1f} or speed={:.1f}<{:.1f})",
                                          actor->formID, thrownMass, kKnockMinMass, impactSpeed, kKnockMinSpeed);
                        }
                    }
                }

                // --- AGGRO (ALWAYS — independent of bImpactDamageEnabled / charController /
                // currentProcess; those nullable gates used to silently suppress aggro). ---
                if (player) {
                    // ENGINE-CORRECT ASSAULT (RE-verified): file the kAttack crime via
                    // Actor::AttackAlarm — the exact path a real weapon hit runs through
                    // (Actor::AttackedBy). This sets the victim's crime faction PC-enemy and
                    // alerts witness guards, so protected NPCs (Diamond City Security) aggro
                    // the player WITHOUT permanently mutating boolFlags (which was leaving
                    // NPCs killable). ProcessHurtfulBody attributes no aggressor, which is why
                    // the old path never escalated. Runs on the game thread (drain context).
                    const bool assaultAlarmSent =
                        SendAssaultAlarmSeh(actor, player);
                    if (assaultAlarmSent) {
                        spdlog::info("[THROWN] Assault alarm filed (kAttack crime) vs actor {:08X} — guards/faction will aggro player",
                                     actor->formID);
                    } else {
                        spdlog::warn("[THROWN] AV in SendAssaultAlarm for actor {:08X} — falling back to StartCombat only", actor->formID);
                    }

                    // AttackAlarm performs the engine's witness/faction
                    // propagation. Only fall back to direct combat for the
                    // actual victim if that path faulted; never start combat
                    // on every unrelated actor in a radius.
                    if (!assaultAlarmSent) {
                        if (auto* tq =
                                RE::TaskQueueInterface::GetSingleton()) {
                            TaskQueueInterface_QueueActorStartCombat(
                                tq, actor, player, true);
                            spdlog::info(
                                "[THROWN] Aggro: queued StartCombat actor {:08X} vs player",
                                actor->formID);
                        }
                    }
                }
            }

        // 2. DESTRUCTIBLE DAMAGE — bottles, glass, etc. break above min speed.
        if (g_config.impactDestroyEnabled && impactSpeed >= g_config.impactMinDestroySpeed) {
            auto* taskQueue = RE::TaskQueueInterface::GetSingleton();
            if (!taskQueue) {
                spdlog::warn("[THROWN] no TaskQueueInterface singleton — destruction skipped");
            } else {
                // Self-damage is overwhelming so the thrown object breaks regardless of
                // its HP — once you've thrown a bottle into a wall, gameplay expects it
                // to shatter. Target damage scales with speed (and the user multiplier).
                const float selfDamage = 9999.0f;
                float targetDamage = impactSpeed * 0.5f * g_config.impactDamageMult;
                targetDamage = std::clamp(targetDamage, 1.0f, 9999.0f);

                // Diagnostic: check whether the thrown form actually has a destructible
                // form attached. If `data` is null the engine has nothing to break.
                auto* baseObj = thrownRefr ? thrownRefr->GetObjectReference() : nullptr;
                RE::BGSDestructibleObjectForm* destForm = nullptr;
                if (baseObj) {
                    switch (baseObj->GetFormType()) {
                        case RE::ENUM_FORM_ID::kMISC:
                            destForm = static_cast<RE::TESObjectMISC*>(baseObj);
                            break;
                        case RE::ENUM_FORM_ID::kALCH:
                            destForm = static_cast<RE::AlchemyItem*>(baseObj);
                            break;
                        case RE::ENUM_FORM_ID::kWEAP:
                            destForm = static_cast<RE::TESObjectWEAP*>(baseObj);
                            break;
                        case RE::ENUM_FORM_ID::kARMO:
                            destForm = static_cast<RE::TESObjectARMO*>(baseObj);
                            break;
                        default:
                            break;
                    }
                }
                const bool hasDestData = destForm && destForm->data;
                spdlog::info("[THROWN] destructible check thrown={:08X} baseForm={:08X} formType={} hasDestructibleForm={} hasData={}",
                             thrownRefr ? thrownRefr->formID : 0u,
                             baseObj ? baseObj->formID : 0u,
                             baseObj ? static_cast<int>(baseObj->GetFormType()) : -1,
                             destForm != nullptr, hasDestData);

                // Damage the THROWN object (glass bottles shatter). Gated on
                // hasDestData: with no destruction data the engine queue is a
                // guaranteed no-op, and the unconditional call made the log
                // claim "Queued destruction ... damage=9999.0" for objects that
                // could never break — which misled a live investigation into
                // treating it as a detonation mechanism.
                if (!hasDestData) {
                    spdlog::debug("[THROWN] thrown refr {:08X} has no destruction data — skipping self-destruction queue",
                                  thrownRefr->formID);
                } else if (QueueDestructibleSeh(taskQueue, thrownRefr, selfDamage)) {
                    spdlog::info("[THROWN] Queued destruction on thrown refr {:08X} damage={:.1f}",
                                 thrownRefr->formID, selfDamage);
                } else {
                    spdlog::warn("[THROWN] QueueUpdateDestructibleObject threw on thrown refr {:08X}",
                                 thrownRefr->formID);
                }

                // Damage the TARGET object (if it has BGSDestructibleObjectForm).
                // HIGGS-style mass gate: a tiny pebble shouldn't break a steel
                // barrel. Only damage the hit object if the thrown object is at
                // least as heavy as it.
                // NEVER apply destructible damage to actors — they're handled by the
                // capped actor-damage path above. Destructible damage scales with speed
                // (e.g. ~200) with no mass scaling, so it would one-shot any NPC and
                // bypass the impact-damage cap. Only break non-actor objects here.
                if (targetRefr && targetRefr != player && !targetRefr->As<RE::Actor>()) {
                    const float targetMass = GetFormWeight(targetRefr);
                    const bool gateOpen = !g_config.impactMassGate
                                       || thrownMass + 0.001f >= targetMass;
                    if (!gateOpen) {
                        spdlog::debug("[THROWN] mass gate: thrown {:.2f} < target {:.2f} — skipping target damage",
                                      thrownMass, targetMass);
                    } else if (QueueDestructibleSeh(taskQueue, targetRefr, targetDamage)) {
                        spdlog::info("[THROWN] Queued destruction on target refr {:08X} damage={:.1f}",
                                     targetRefr->formID, targetDamage);
                    } else {
                        spdlog::warn("[THROWN] QueueUpdateDestructibleObject threw on target refr {:08X}",
                                     targetRefr->formID);
                    }
                }
            }
        } else if (g_config.impactDestroyEnabled) {
            spdlog::debug("[THROWN] speed {:.1f} below destroy threshold {:.1f}",
                          impactSpeed, g_config.impactMinDestroySpeed);
        }

        // 3. AI DETECTION EVENT — NPCs hear the impact.
        // Speed gate is independent of destruction speed: a soft bump still
        // alerts NPCs even though it doesn't break anything (HIGGS pattern).
        // Sound level optionally scales with thrown mass so a tossed cap is
        // silent while a heaved coffee pot is very-loud.
        if (g_config.impactDetectionEvent && impactSpeed >= g_config.impactMinDetectionSpeed
            && player && player->currentProcess) {
            const int soundLevel = g_config.impactMassScaledSound
                ? GetSoundLevelByMass(thrownMass)
                : kSoundLevelLoud;
            if (soundLevel == kSoundLevelSilent) {
                spdlog::debug("[THROWN] mass {:.2f} → silent — no detection event",
                              thrownMass);
            } else {
                void* aiProcess = player->currentProcess;
                if (DetectionEventSeh(aiProcess, player, contactPos, soundLevel, thrownRefr)) {
                    spdlog::info("[THROWN] Posted detection event level={} at ({:.0f},{:.0f},{:.0f})",
                                 soundLevel, contactPos.x, contactPos.y, contactPos.z);
                } else {
                    spdlog::warn("[THROWN] SetActorsDetectionEvent threw for refr {:08X}", thrownRefr->formID);
                }
            }
        } else if (g_config.impactDetectionEvent && impactSpeed < g_config.impactMinDetectionSpeed) {
            spdlog::debug("[THROWN] speed {:.1f} below detection threshold {:.1f}",
                          impactSpeed, g_config.impactMinDetectionSpeed);
        }

        // 4. HIT EVENT — reserved for a future manual TESHitEvent dispatch.
        // Native ProcessHurtfulBody is intentionally not used because it would
        // apply uncapped health damage in addition to the manual path above.

        // Done — clear the tag so AI doesn't keep treating the bottle as a
        // telekinesis object after it lands.
        ClearTelekinesisObjectSeh(thrownRefr);
    }
}
