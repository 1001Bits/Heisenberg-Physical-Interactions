/*
 * Contact routing is kept as a separate core fragment because it bridges the native hknp contact signal, hand semantic state, weapon contacts, and push assist. Keeping it in the PhysicsInteraction translation unit preserves the existing anonymous-namespace helpers while making the frame loop readable.
 */
    void PhysicsInteraction::resolveContacts(const PhysicsFrameContext& frame)
    {
        performance_profiler::ScopedTimer profilerTimer(performance_profiler::Scope::ContactResolve);

        auto* bhk = frame.bhkWorld;
        auto* hknp = frame.hknpWorld;
        _contactSlotRight.drainEach([&](std::uint32_t rightSourceBody, std::uint32_t rightContactBody) {
            resolveAndLogContact("Right", bhk, hknp, RE::hknpBodyId{ rightContactBody });
            if (rightSourceBody == 0xFFFFFFFF) {
                rightSourceBody = _rightHand.getCollisionBodyId().value;
            }
            applyDynamicPushAssist("Right", bhk, hknp, rightSourceBody, rightContactBody, false, &_rightHand);
        });

        _contactSlotLeft.drainEach([&](std::uint32_t leftSourceBody, std::uint32_t leftContactBody) {
            resolveAndLogContact("Left", bhk, hknp, RE::hknpBodyId{ leftContactBody });
            if (leftSourceBody == 0xFFFFFFFF) {
                leftSourceBody = _leftHand.getCollisionBodyId().value;
            }
            applyDynamicPushAssist("Left", bhk, hknp, leftSourceBody, leftContactBody, false, &_leftHand);
        });

        _contactSlotWeapon.drainEach([&](std::uint32_t weaponSourceBody, std::uint32_t weaponContactBody) {
            if (weaponSourceBody != 0xFFFFFFFF) {
                applyDynamicPushAssist("Weapon", bhk, hknp, weaponSourceBody, weaponContactBody, true);
            }
        });

        auto readBodyMass = [](RE::hknpWorld* world, std::uint32_t bodyId) {
            if (!world || bodyId == 0xFFFFFFFF || bodyId == object_physics_body_set::INVALID_BODY_ID) {
                return 0.0f;
            }

            auto* motion = havok_runtime::getBodyMotion(world, RE::hknpBodyId{ bodyId });
            if (!motion) {
                return 0.0f;
            }

            const auto packedInvMass = static_cast<std::int16_t>(motion->packedInverseInertia[3]);
            if (packedInvMass == 0) {
                return 0.0f;
            }
            return grab_mass_policy::massFromInverseMass(unpackBfloat16(packedInvMass));
        };

        auto readBodySpeedGameUnits = [](RE::hknpWorld* world, std::uint32_t bodyId) {
            if (!world || bodyId == 0xFFFFFFFF || bodyId == object_physics_body_set::INVALID_BODY_ID) {
                return 0.0f;
            }

            auto* motion = havok_runtime::getBodyMotion(world, RE::hknpBodyId{ bodyId });
            if (!motion) {
                return 0.0f;
            }

            const float speedHavok = std::sqrt(
                motion->linearVelocity.x * motion->linearVelocity.x +
                motion->linearVelocity.y * motion->linearVelocity.y +
                motion->linearVelocity.z * motion->linearVelocity.z);
            return std::isfinite(speedHavok) ? speedHavok * havokToGameScale() : 0.0f;
        };

        auto processHeldImpact = [&](Hand& hand,
                                     bool isLeft,
                                     std::atomic<std::uint64_t>& pairAtomic) {
            if (pairAtomic.load(std::memory_order_acquire) ==
                kInvalidHeldImpactPair) {
                return;
            }
            std::uint32_t heldBody = kInvalidAtomicBodyId;
            std::uint32_t otherBody = kInvalidAtomicBodyId;
            const auto packedPair = pairAtomic.exchange(kInvalidHeldImpactPair, std::memory_order_acq_rel);
            if (!unpackHeldImpactPair(packedPair, heldBody, otherBody) || !hand.isHolding()) {
                return;
            }

            dispatchHeldImpactGrabEvent(
                isLeft,
                hand.getHeldRef(),
                heldBody,
                otherBody,
                readBodyMass(hknp, heldBody),
                readBodySpeedGameUnits(hknp, heldBody));
        };

        processHeldImpact(_rightHand, false, _lastHeldImpactPairRight);
        processHeldImpact(_leftHand, true, _lastHeldImpactPairLeft);
    }
    void PhysicsInteraction::applyDynamicPushAssist(const char* sourceName,
        RE::bhkWorld* bhk,
        RE::hknpWorld* hknp,
        std::uint32_t sourceBodyId,
            std::uint32_t targetBodyId,
        bool sourceIsWeapon,
        const Hand* sourceHand)
    {
        if (!bhk || !hknp || sourceBodyId == 0xFFFFFFFF || targetBodyId == 0xFFFFFFFF ||
            sourceBodyId == object_physics_body_set::INVALID_BODY_ID || targetBodyId == object_physics_body_set::INVALID_BODY_ID || sourceBodyId == targetBodyId) {
            return;
        }
        // Embedded-host coexistence: Heisenberg's body-less proximity path supplies the hand
        // impulse/depenetration in this mode. Suppress only this second scripted HAND impulse;
        // the native ROCK colliders/contact pipeline remain live, and weapon assist passes
        // sourceIsWeapon=true so it is deliberately unaffected.
        if (!sourceIsWeapon && HostIsHandDynamicPushAssistSuppressed()) {
            return;
        }
        if (::rock::provider::isExternalBodyDynamicPushSuppressed(targetBodyId)) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                // Observed fact, not a guess: the body is in the provider's external-body
                // registry with its suppressesRockDynamicPush flag set. Name the registry,
                // not a presumed owner - we do not know which provider registered it here.
                "{} dynamic push skipped: target body {} is in the provider external-body registry with dynamic-push suppression requested",
                sourceName,
                targetBodyId);
            return;
        }
        if (HostIsExternalHeldBody(hknp, targetBodyId)) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: target body {} is held by the embedded host",
                sourceName,
                targetBodyId);
            return;
        }
        if (held_object_body_set_policy::containsAnyBody(_rightHand.getHeldBodyIds(), _leftHand.getHeldBodyIds(), targetBodyId)) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: target body {} is owned by an active held grab",
                sourceName,
                targetBodyId);
            return;
        }

        auto* targetRef = resolveBodyToRef(bhk, hknp, RE::hknpBodyId{ targetBodyId });
        if (!targetRef || targetRef->IsDeleted() || targetRef->IsDisabled()) {
            ROCK_LOG_SAMPLE_DEBUG(Hand, g_rockConfig.rockLogSampleMilliseconds, "{} dynamic push skipped: target body {} has no valid ref", sourceName, targetBodyId);
            return;
        }
        // Host recent-release guard: the host just released this ref and its collision
        // restore/pair-cache rebuild is still in flight, so the object can legitimately
        // still be inside the open hand's colliders. Pushing it here is the documented
        // "shove-on-release" symptom. Native Havok collision stays enabled throughout.
        for (std::size_t handIndex = 0; handIndex < 2; ++handIndex) {
            const auto remainingFrames = _hostRecentReleaseFrames[handIndex].load(std::memory_order_acquire);
            const auto recentFormId = _hostRecentReleaseFormId[handIndex].load(std::memory_order_acquire);
            if (remainingFrames > 0 && recentFormId != 0 && targetRef->GetFormID() == recentFormId) {
                ROCK_LOG_SAMPLE_DEBUG(Hand,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "{} dynamic push skipped: target body {} formID={:08X} was just released by host "
                    "({} guard frames remain; native collision stays active)",
                    sourceName,
                    targetBodyId,
                    recentFormId,
                    remainingFrames);
                return;
            }
        }
        if (isPendingForceGrabTarget(targetRef)) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: target body {} belongs to an in-flight force-grab transaction",
                sourceName,
                targetBodyId);
            return;
        }

        object_physics_body_set::BodySetScanOptions scanOptions{};
        scanOptions.mode = physics_body_classifier::InteractionMode::PassivePush;
        scanOptions.rightHandBodyId = _rightHand.getCollisionBodyId().value;
        scanOptions.leftHandBodyId = _leftHand.getCollisionBodyId().value;
        scanOptions.sourceBodyId = sourceBodyId;
        scanOptions.sourceWeaponBodyId = sourceIsWeapon ? sourceBodyId : object_physics_body_set::INVALID_BODY_ID;
        scanOptions.maxDepth = g_rockConfig.rockObjectPhysicsTreeMaxDepth;
        if (!sourceIsWeapon && sourceHand) {
            scanOptions.heldBySameHand = &sourceHand->getHeldBodyIds();
        }

        const auto bodySet = object_physics_body_set::scanObjectPhysicsBodySet(bhk, hknp, targetRef, scanOptions);
        const auto* targetRecord = bodySet.findRecord(targetBodyId);
        if (!targetRecord) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: target body {} not found in ref tree formID={:08X} visitedNodes={} collisionObjects={}",
                sourceName,
                targetBodyId,
                targetRef->GetFormID(),
                bodySet.diagnostics.visitedNodes,
                bodySet.diagnostics.collisionObjects);
            return;
        }
        if (!targetRecord->accepted) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: target body {} rejected reason={} layer={} motionId={} motionType={}",
                sourceName,
                targetBodyId,
                physics_body_classifier::rejectReasonName(targetRecord->rejectReason),
                targetRecord->collisionLayer,
                targetRecord->motionId,
                static_cast<int>(targetRecord->motionType));
            return;
        }
        if (!sourceIsWeapon && collision_layer_policy::isActorOrBipedLayer(targetRecord->collisionLayer)) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push actor/ragdoll accepted: sourceBody={} targetBody={} layer={} motionId={} motionType={}",
                sourceName,
                sourceBodyId,
                targetBodyId,
                targetRecord->collisionLayer,
                targetRecord->motionId,
                static_cast<int>(targetRecord->motionType));
        }
        // MELEE HIT PROTECTION (Jul 31). While a melee weapon is drawn, the
        // weapon hull's push assist must NOT shove hittable targets: the
        // native VR melee hit test samples its own (smaller) collider against
        // the target, and our padded hull contacting first was applying
        // impulses across the target's whole body set (live debug log:
        // 'Baseball Bat' swings pushing 18-42 bodies on 'Takahashi'/'Percy',
        // layer 32) — deflecting the limb before the native collider arrived.
        // Reported as "1 in 5 bat sweeps misses". The game owns actor impact
        // for melee; our push assist keeps owning props, doors and clutter.
        //
        // The target test is BOTH the collision layer AND the resolved form
        // type: a turret or mounted robot is an actor (kACHR) whose bodies sit
        // on machine/animstatic layers the biped-layer test never matches, and
        // it must not be shoved out of a swing any more than a raider.
        if (sourceIsWeapon &&
            f4vr::isMeleeWeaponEquipped() &&
            (collision_layer_policy::isActorOrBipedLayer(targetRecord->collisionLayer) ||
                targetRef->GetFormType() == RE::ENUM_FORM_ID::kACHR)) {
            // Cumulative counter embedded in a SAMPLED line: the sampler emits
            // at most ~1 line per 2s, but because the count is cumulative the
            // delta between consecutive emitted lines is the exact number of
            // suppressed pushes in that interval — countable despite sampling.
            // Info level so melee sessions at the default log level keep it.
            static std::atomic<std::uint32_t> s_meleePushSkipCount{ 0 };
            const auto skipCount =
                s_meleePushSkipCount.fetch_add(1, std::memory_order_relaxed) + 1;
            ROCK_LOG_SAMPLE_INFO(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: melee weapon drawn — native melee owns actor impact (targetBody={} layer={} formType={} cumulativeCount={})",
                sourceName,
                targetBodyId,
                targetRecord->collisionLayer,
                static_cast<int>(targetRef->GetFormType()),
                skipCount);
            return;
        }

        const auto uniqueMotionRecords = bodySet.uniqueAcceptedMotionRecords();
        if (uniqueMotionRecords.empty()) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: accepted target body {} produced no unique motion bodies",
                sourceName,
                targetBodyId);
            return;
        }

        auto* sourceMotion = havok_runtime::getBodyMotion(hknp, RE::hknpBodyId{ sourceBodyId });
        if (!sourceMotion) {
            return;
        }

        RE::NiPoint3 sourceVelocityHavok{
            sourceMotion->linearVelocity.x,
            sourceMotion->linearVelocity.y,
            sourceMotion->linearVelocity.z,
        };
        const RE::NiPoint3 rawSourceVelocityHavok = sourceVelocityHavok;

        /*
         * Push only along the outward hand-to-target direction. If the hand is
         * no longer approaching while the target is moving away, the contact is
         * a bounce: bleed the configured fraction of separating radial velocity
         * so a dropped prop settles into the fingers instead of rebounding into
         * repeated deep contacts.
         */
        RE::NiTransform sourceBodyWorld{};
        const bool haveSourceBodyWorld =
            havok_runtime::tryResolveLiveBodyWorldTransform(
                hknp,
                RE::hknpBodyId{ sourceBodyId },
                sourceBodyWorld);
        const RE::NiPoint3 sourceToTargetGame{
            targetRecord->positionGame.x - sourceBodyWorld.translate.x,
            targetRecord->positionGame.y - sourceBodyWorld.translate.y,
            targetRecord->positionGame.z - sourceBodyWorld.translate.z,
        };
        const float sourceToTargetLength = std::sqrt(
            sourceToTargetGame.x * sourceToTargetGame.x +
            sourceToTargetGame.y * sourceToTargetGame.y +
            sourceToTargetGame.z * sourceToTargetGame.z);
        if (!haveSourceBodyWorld ||
            !std::isfinite(sourceToTargetLength) ||
            sourceToTargetLength <= 0.001f) {
            ROCK_LOG_SAMPLE_DEBUG(
                Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: no trustworthy outward contact direction "
                "sourceBody={} targetBody={} haveSource={} separation={:.4f}",
                sourceName,
                sourceBodyId,
                targetBodyId,
                haveSourceBodyWorld ? "yes" : "no",
                sourceToTargetLength);
            return;
        }

        const float invSeparation = 1.0f / sourceToTargetLength;
        const RE::NiPoint3 outward{
            sourceToTargetGame.x * invSeparation,
            sourceToTargetGame.y * invSeparation,
            sourceToTargetGame.z * invSeparation,
        };
        const float outwardSpeedHavok =
            sourceVelocityHavok.x * outward.x +
            sourceVelocityHavok.y * outward.y +
            sourceVelocityHavok.z * outward.z;
        if (!std::isfinite(outwardSpeedHavok) ||
            outwardSpeedHavok <= 0.0f) {
            const float restitutionDamping = (std::clamp)(
                g_rockConfig.rockHandContactRestitutionDamping,
                0.0f,
                1.0f);
            if (restitutionDamping > 0.0f && !sourceIsWeapon) {
                if (auto* bounceMotion = havok_runtime::getBodyMotion(
                        hknp,
                        RE::hknpBodyId{ targetBodyId })) {
                    const float separatingSpeed =
                        bounceMotion->linearVelocity.x * outward.x +
                        bounceMotion->linearVelocity.y * outward.y +
                        bounceMotion->linearVelocity.z * outward.z;
                    if (std::isfinite(separatingSpeed) &&
                        separatingSpeed > 0.0f) {
                        const float bleed =
                            separatingSpeed * restitutionDamping;
                        const RE::NiPoint3 dampingDelta{
                            -outward.x * bleed,
                            -outward.y * bleed,
                            -outward.z * bleed,
                        };
                        if (push_assist::applyLinearVelocityDeltaDeferred(
                                hknp,
                                targetBodyId,
                                dampingDelta)) {
                            ROCK_LOG_SAMPLE_DEBUG(
                                Hand,
                                g_rockConfig.rockLogSampleMilliseconds,
                                "{} soft-landing damper: bled {:.3f} of {:.3f} "
                                "separating speed off target {} (damping={:.2f})",
                                sourceName,
                                bleed,
                                separatingSpeed,
                                targetBodyId,
                                restitutionDamping);
                        }
                    }
                }
            }
            ROCK_LOG_SAMPLE_DEBUG(
                Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: source is not approaching target "
                "sourceBody={} targetBody={} rawVel=({:.3f},{:.3f},{:.3f}) "
                "radialSpeed={:.3f}",
                sourceName,
                sourceBodyId,
                targetBodyId,
                rawSourceVelocityHavok.x,
                rawSourceVelocityHavok.y,
                rawSourceVelocityHavok.z,
                outwardSpeedHavok);
            return;
        }
        sourceVelocityHavok = RE::NiPoint3{
            outward.x * outwardSpeedHavok,
            outward.y * outwardSpeedHavok,
            outward.z * outwardSpeedHavok,
        };

        const std::uint64_t cooldownKey = (static_cast<std::uint64_t>(sourceBodyId) << 32) | targetBodyId;
        float cooldownRemaining = 0.0f;
        if (const auto it = _dynamicPushCooldownUntil.find(cooldownKey); it != _dynamicPushCooldownUntil.end() && it->second > _dynamicPushElapsedSeconds) {
            cooldownRemaining = it->second - _dynamicPushElapsedSeconds;
        }

        const push_assist::PushAssistInput<RE::NiPoint3> pushInput{
            .enabled = g_rockConfig.rockDynamicPushAssistEnabled,
            .sourceVelocity = sourceVelocityHavok,
            .minSpeed = g_rockConfig.rockDynamicPushMinSpeed,
            .maxImpulse = g_rockConfig.rockDynamicPushMaxImpulse,
            .layerMultiplier = 1.0f,
            .cooldownRemainingSeconds = cooldownRemaining,
        };
        const auto push = push_assist::computePushImpulse(pushInput);
        if (!push.apply) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: reason={} speed=({:.3f},{:.3f},{:.3f}) targetBody={} layer={} acceptedBodies={}",
                sourceName,
                pushAssistSkipReasonName(push.skipReason),
                sourceVelocityHavok.x,
                sourceVelocityHavok.y,
                sourceVelocityHavok.z,
                targetBodyId,
                targetRecord->collisionLayer,
                bodySet.acceptedCount());
            return;
        }

        std::uint32_t appliedCount = 0;
        for (const auto* record : uniqueMotionRecords) {
            if (!record) {
                continue;
            }
            physics_recursive_wrappers::activateBody(hknp, record->bodyId);
            if (push_assist::applyLinearImpulse(record->collisionObject, push.impulse)) {
                ++appliedCount;
            }
        }

        if (appliedCount > 0) {
            _dynamicPushCooldownUntil[cooldownKey] =
                _dynamicPushElapsedSeconds + (std::max)(0.0f, g_rockConfig.rockDynamicPushCooldownSeconds);
            auto* baseObj = targetRef->GetObjectReference();
            auto objName = baseObj ? RE::TESFullName::GetFullName(*baseObj, false) : std::string_view{};
            const std::string nameStr = objName.empty() ? std::string("(unnamed)") : std::string(objName);
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push applied: '{}' formID={:08X} targetBody={} layer={} acceptedBodies={} uniqueMotions={} impulse=({:.3f},{:.3f},{:.3f})",
                sourceName,
                nameStr,
                targetRef->GetFormID(),
                targetBodyId,
                targetRecord->collisionLayer,
                bodySet.acceptedCount(),
                appliedCount,
                push.impulse.x,
                push.impulse.y,
                push.impulse.z);
        }
    }

    void PhysicsInteraction::resolveAndLogContact(const char* handName, RE::bhkWorld* bhk, RE::hknpWorld* hknp, RE::hknpBodyId bodyId)
    {
        if (!bhk || !hknp)
            return;

        std::uint32_t filterInfo = 0;
        if (!havok_runtime::tryReadFilterInfo(hknp, bodyId, filterInfo)) {
            return;
        }
        auto layer = filterInfo & 0x7F;

        auto* ref = resolveBodyToRef(bhk, hknp, bodyId);
        if (ref) {
            auto* baseObj = ref->GetObjectReference();
            const char* typeName = baseObj ? baseObj->GetFormTypeString() : "???";
            auto objName = baseObj ? RE::TESFullName::GetFullName(*baseObj, false) : std::string_view{};
            const std::string nameStr = objName.empty() ? std::string("(unnamed)") : std::string(objName);

            ROCK_LOG_DEBUG(Hand, "{} hand touched [{}] '{}' formID={:08X} body={} layer={}", handName, typeName, nameStr, ref->GetFormID(), bodyId.value, layer);

            bool isLeft = (std::string_view(handName) == "Left");
            auto& hand = isLeft ? _leftHand : _rightHand;
            RE::NiPoint3 contactPointWorld{};
            const RE::NiPoint3* contactPoint = nullptr;
            hand_semantic_contact_state::SemanticContactRecord semanticContact{};
            if (hand.getLastSemanticContact(semanticContact) &&
                semanticContact.otherBodyId == bodyId.value &&
                semanticContact.framesSinceContact <= 1 &&
                hand_semantic_contact_state::hasUsableContactPoint(semanticContact)) {
                contactPointWorld = RE::NiPoint3{
                    semanticContact.contactPointGame.x,
                    semanticContact.contactPointGame.y,
                    semanticContact.contactPointGame.z };
                contactPoint = &contactPointWorld;
            }
            hand.setTouchState(
                ref,
                ref->GetFormID(),
                layer,
                bodyId.value,
                contactPoint);
            dispatchPhysicsMessage(kPhysMsg_OnTouch, isLeft, ref, ref->GetFormID(), layer);
        } else {
            ROCK_LOG_DEBUG(Hand, "{} hand touched body={} layer={} (unresolved)", handName, bodyId.value, layer);
        }
    }

    void PhysicsInteraction::subscribeContactEvents(RE::hknpWorld* world)
    {
        if (!world) {
            ROCK_LOG_ERROR(Init, "Contact event subscription skipped because world is null");
            return;
        }

        void* signal = world->GetEventSignal(RE::hknpEventType::kContact);
        if (!signal) {
            ROCK_LOG_ERROR(Init, "Failed to get contact event signal");
            return;
        }

        auto* currentWorld = s_contactEventBridge.world.load(std::memory_order_acquire);
        auto* currentSignal = s_contactEventBridge.signal.load(std::memory_order_acquire);
        const auto currentSnapshot = contact_signal_subscription_policy::ContactSignalSubscriptionSnapshot{
            .world = reinterpret_cast<std::uintptr_t>(currentWorld),
            .signal = reinterpret_cast<std::uintptr_t>(currentSignal),
            .active = currentWorld != nullptr && currentSignal != nullptr,
        };
        const auto plan = contact_signal_subscription_policy::planSubscription(
            currentSnapshot,
            reinterpret_cast<std::uintptr_t>(world),
            reinterpret_cast<std::uintptr_t>(signal),
            s_contactEventBridge.hasRetainedNativeSlot(world, signal));

        if (plan.action == contact_signal_subscription_policy::ContactSignalSubscriptionAction::IgnoreNullSignal) {
            ROCK_LOG_ERROR(Init, "Contact event subscription skipped because world or signal is null");
            return;
        }

        if (plan.action == contact_signal_subscription_policy::ContactSignalSubscriptionAction::AlreadySubscribed) {
            _contactEventSignal.store(signal, std::memory_order_release);
            _contactEventWorld.store(world, std::memory_order_release);
            s_contactEventBridge.signal.store(signal, std::memory_order_release);
            s_contactEventBridge.world.store(world, std::memory_order_release);
            s_contactEventBridge.instance.store(this, std::memory_order_release);
            const auto epoch = s_contactEventBridge.subscriptionEpoch.load(std::memory_order_acquire);
            if (!s_contactEventBridge.rememberRetainedNativeSlot(world, signal, epoch)) {
                ROCK_LOG_WARN(Init, "Contact event retained-slot table full while reusing bridge slot; future duplicate suppression may be degraded");
            }
            ROCK_LOG_DEBUG(Init, "Contact event signal already subscribed for current world; reusing native bridge slot");
            return;
        }

        if (plan.replaceExistingRuntimeStateWithoutUnsubscribe) {
            ROCK_LOG_INFO(
                Init,
                "Replacing contact event bridge state without native unsubscribe (action={})",
                static_cast<std::uint32_t>(plan.action));
        }

        ContactEventCallbackInfo cbInfo{};
        cbInfo.fn = reinterpret_cast<void*>(&PhysicsInteraction::onContactCallback);
        cbInfo.ctx = 0;

        typedef void subscribe_ext_t(void* signal, void* userData, void* callbackInfo);
        static REL::Relocation<subscribe_ext_t> subscribeExt{ REL::Offset(offsets::kFunc_SubscribeContactEvent) };
        subscribeExt(signal, static_cast<void*>(&s_contactEventBridge), &cbInfo);

        _contactEventSignal.store(signal, std::memory_order_release);
        _contactEventWorld.store(world, std::memory_order_release);
        s_contactEventBridge.signal.store(signal, std::memory_order_release);
        s_contactEventBridge.world.store(world, std::memory_order_release);
        s_contactEventBridge.instance.store(this, std::memory_order_release);
        const auto epoch = s_contactEventBridge.subscriptionEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!s_contactEventBridge.rememberRetainedNativeSlot(world, signal, epoch)) {
            ROCK_LOG_WARN(Init, "Contact event retained-slot table full after native subscription; future duplicate suppression may be degraded");
        }
        ROCK_LOG_INFO(
            Init,
            "Subscribed contact event bridge slot (epoch={}, action={})",
            epoch,
            static_cast<std::uint32_t>(plan.action));
    }

    void PhysicsInteraction::unsubscribeContactEvents(RE::hknpWorld* liveWorld)
    {
        auto* localWorld = _contactEventWorld.exchange(nullptr, std::memory_order_acq_rel);
        void* localSignal = _contactEventSignal.exchange(nullptr, std::memory_order_acq_rel);

        auto* expectedInstance = this;
        const bool deactivatedCurrentInstance = s_contactEventBridge.instance.compare_exchange_strong(
            expectedInstance,
            nullptr,
            std::memory_order_acq_rel,
            std::memory_order_acquire);

        auto* bridgeWorld = s_contactEventBridge.world.load(std::memory_order_acquire);
        void* bridgeSignal = s_contactEventBridge.signal.load(std::memory_order_acquire);
        const auto bridgeSnapshot = contact_signal_subscription_policy::ContactSignalSubscriptionSnapshot{
            .world = reinterpret_cast<std::uintptr_t>(bridgeWorld),
            .signal = reinterpret_cast<std::uintptr_t>(bridgeSignal),
            .active = bridgeWorld != nullptr && bridgeSignal != nullptr,
        };

        if (!contact_signal_subscription_policy::isActiveSubscription(bridgeSnapshot)) {
            return;
        }

        const bool retainNativeSlot = contact_signal_subscription_policy::shouldRetainNativeSlotAfterDeactivation(
            bridgeSnapshot);
        if (retainNativeSlot) {
            ROCK_LOG_INFO(
                Init,
                "Deactivated contact event bridge; native slots retained for hknpWorld cleanup (instanceCleared={}, world={}, signal={}, liveWorld={})",
                deactivatedCurrentInstance ? "yes" : "no",
                static_cast<const void*>(bridgeWorld),
                bridgeSignal,
                static_cast<const void*>(liveWorld));
            return;
        }

        ROCK_LOG_INFO(
            Init,
            "Deactivated contact event bridge with no active native slot (instanceCleared={}, localWorld={}, localSignal={}, liveWorld={})",
            deactivatedCurrentInstance ? "yes" : "no",
            static_cast<const void*>(localWorld),
            localSignal,
            static_cast<const void*>(liveWorld));
    }

    void PhysicsInteraction::onContactCallback(void* userData, void** worldPtrHolder, void* contactEventData)
    {
        performance_profiler::addEventCount(performance_profiler::Scope::NativeContactCallback);

        // Lease the PhysicsInteraction for the duration of this physics-thread callback.
        // onContactCallbackUnsafe loads bridge->instance and then calls
        // self->handleContactEvent() on it; destroyPhysicsInteraction() stores
        // s_hooksEnabled=false and deletes the instance, so without this counter a
        // callback that already passed the s_hooksEnabled check runs on freed memory.
        // The counter deliberately lives OUTSIDE onContactCallbackSeh's __try: an RAII
        // guard inside a __try whose __except can fire would leak the count on unwind.
        // Mirrors hookedProcessConstraintsCallback (PhysicsHooks.cpp). Incrementing
        // before the s_hooksEnabled check is safe: a callback that arrives after the
        // drain completes still returns without dereferencing anything, because
        // s_hooksEnabled is false and bridge->instance has been cleared.
        PhysicsInteraction::s_inFlightCallbacks.fetch_add(1, std::memory_order_acq_rel);
        const bool configRead = g_rockConfig.tryEnterNativeRead();
        if (configRead) {
            onContactCallbackSeh(userData, worldPtrHolder, contactEventData);
            g_rockConfig.leaveNativeRead();
        }
        if (PhysicsInteraction::s_inFlightCallbacks.fetch_sub(
                1,
                std::memory_order_acq_rel) == 1) {
            PhysicsInteraction::s_inFlightCallbacks.notify_all();
        }
    }

    namespace contact_exception_detail
    {
        /*
         * The old handler used a bare EXCEPTION_EXECUTE_HANDLER and then guessed in the
         * log ("likely stale world during cell transition") at a cause it had thrown
         * away the evidence for. The exception record IS the evidence: capture the code
         * and the faulting instruction address in the FILTER (the only place they still
         * exist) so the report can name the module that faulted instead of a theory.
         *
         * These are file-local; onContactCallbackException is declared in the header and
         * its signature is not ours to change from this fragment.
         */
        inline std::atomic<std::uint32_t> s_lastExceptionCode{ 0 };
        inline std::atomic<std::uintptr_t> s_lastExceptionAddress{ 0 };

        [[nodiscard]] inline int captureContactException(EXCEPTION_POINTERS* info) noexcept
        {
            if (info && info->ExceptionRecord) {
                s_lastExceptionCode.store(static_cast<std::uint32_t>(info->ExceptionRecord->ExceptionCode), std::memory_order_release);
                s_lastExceptionAddress.store(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress), std::memory_order_release);
            } else {
                s_lastExceptionCode.store(0, std::memory_order_release);
                s_lastExceptionAddress.store(0, std::memory_order_release);
            }
            return EXCEPTION_EXECUTE_HANDLER;
        }
    }

    void PhysicsInteraction::onContactCallbackSeh(void* userData, void** worldPtrHolder, void* contactEventData)
    {
        // NOTE: no C++ objects with destructors may live in this function (project SEH
        // rule). The filter is a free noexcept function and the handler body is a single
        // call, so nothing is constructed here.
        __try {
            onContactCallbackUnsafe(userData, worldPtrHolder, contactEventData);
        } __except (contact_exception_detail::captureContactException(GetExceptionInformation())) {
            onContactCallbackException();
        }
    }

    void PhysicsInteraction::onContactCallbackUnsafe(void* userData, void** worldPtrHolder, void* contactEventData)
    {
        if (!s_hooksEnabled.load(std::memory_order_acquire))
            return;
        if (userData != static_cast<void*>(&s_contactEventBridge)) {
            return;
        }

        auto* bridge = static_cast<ContactEventSubscriptionBridge*>(userData);
        auto* self = bridge->instance.load(std::memory_order_acquire);
        if (self && self->_initialized.load(std::memory_order_acquire)) {
            auto* subscribedWorld = bridge->world.load(std::memory_order_acquire);
            auto* subscribedSignal = bridge->signal.load(std::memory_order_acquire);
            const auto snapshot = contact_signal_subscription_policy::ContactSignalSubscriptionSnapshot{
                .world = reinterpret_cast<std::uintptr_t>(subscribedWorld),
                .signal = reinterpret_cast<std::uintptr_t>(subscribedSignal),
                .active = subscribedWorld != nullptr && subscribedSignal != nullptr,
            };

            std::uintptr_t callbackWorld = 0;
            if (worldPtrHolder) {
                callbackWorld = reinterpret_cast<std::uintptr_t>(*worldPtrHolder);
            }

            const auto acceptance = contact_signal_subscription_policy::evaluateCallbackAcceptance(snapshot, callbackWorld);
            if (!acceptance.accept) {
                return;
            }

            self->handleContactEvent(reinterpret_cast<RE::hknpWorld*>(acceptance.effectiveWorld), contactEventData);
        }
    }

    void PhysicsInteraction::onContactCallbackException()
    {
        static std::atomic<int> sehLogCounter{ 0 };
        const int occurrence = sehLogCounter.fetch_add(1, std::memory_order_acq_rel) + 1;

        /*
         * This does not "note an exception". It LATCHES s_hooksEnabled off, and
         * s_hooksEnabled gates the whole native contact path:
         *   - onContactCallbackUnsafe            (every native hknp contact event)
         *   - hookedHandleBumpedCharacter        (PhysicsHooks.cpp)
         *   - hookedProcessConstraintsCallback   (PhysicsHooks.cpp)
         * The last two are what implement the player character-controller contact
         * filter, i.e. the large-object block that stops the player walking through
         * cars. Nothing on the contact path ever sets the flag back to true: the only
         * writer of `true` is ROCKMain's publishPhysicsInteractionIfReady(), which is
         * guarded by s_physicsPublished and therefore only runs again after a full
         * PhysicsInteraction teardown + republish. A save reload or a cell change does
         * not necessarily do that.
         *
         * The old message said none of this. It logged "likely stale world during cell
         * transition" - a cause nothing here tested - and never mentioned that a feature
         * had just been switched off, so the log read as a harmless warning about a
         * transient while the player's collision had silently gone dead.
         */
        const bool wasEnabled = s_hooksEnabled.exchange(false, std::memory_order_acq_rel);

        // Log the first occurrence unconditionally (that is the one that disabled the
        // feature), then rate-limit; this runs on the physics thread.
        if (wasEnabled || (occurrence % 100) == 1) {
            const auto exceptionCode = contact_exception_detail::s_lastExceptionCode.load(std::memory_order_acquire);
            const auto exceptionAddress = contact_exception_detail::s_lastExceptionAddress.load(std::memory_order_acquire);
            logger::error(
                "[ROCK::Contact] FEATURE DISABLED: hardware exception caught inside ROCK's native contact callback on the physics thread "
                "(occurrence {}, code 0x{:08X}, faulting address 0x{:X} in {}). ROCK's native contact hooks are now latched OFF and are only "
                "re-armed by a full PhysicsInteraction teardown and republish, not by a cell change or a save reload. "
                "WHAT STOPS WORKING UNTIL THEN: hand and weapon collision against world objects, held-object impacts, dynamic push assist, and "
                "the player character-controller contact filter - so large objects such as cars stop blocking the player and become walk-through "
                "again. Native game physics is unaffected and the original callbacks are still chained.",
                occurrence,
                exceptionCode,
                exceptionAddress,
                rock::hook_diagnostics::describeAddress(exceptionAddress));
        }
    }

    void PhysicsInteraction::handleContactEvent(RE::hknpWorld* world, void* contactEventData)
    {
        performance_profiler::ScopedTimer profilerTimer(performance_profiler::Scope::NativeContactCallback);

        if (!contactEventData)
            return;

        auto* data = reinterpret_cast<std::uint8_t*>(contactEventData);
        std::uint32_t bodyIdA = *reinterpret_cast<std::uint32_t*>(data + 0x08);
        std::uint32_t bodyIdB = *reinterpret_cast<std::uint32_t*>(data + 0x0C);

        if (!contact_pipeline_policy::isValidBodyId(bodyIdA) || !contact_pipeline_policy::isValidBodyId(bodyIdB) || bodyIdA == bodyIdB) {
            return;
        }

        if (!havok_runtime::bodySlotLooksReadable(world, RE::hknpBodyId{ bodyIdA }) ||
            !havok_runtime::bodySlotLooksReadable(world, RE::hknpBodyId{ bodyIdB })) {
            return;
        }

        havok_runtime::ContactSignalPointResult rawContactPoint{};
        bool rawContactPointEvaluated = false;
        bool hasRawContactPoint = false;
        auto ensureRawContactPoint = [&]() {
            if (!rawContactPointEvaluated) {
                hasRawContactPoint = havok_runtime::tryExtractContactSignalPoint(world, contactEventData, rawContactPoint);
                rawContactPointEvaluated = true;
            }
            return hasRawContactPoint;
        };

        const auto rightId = _rightHand.getCollisionBodyIdAtomic().value;
        const auto leftId = _leftHand.getCollisionBodyIdAtomic().value;

        using generated_body_contact_registry::Classification;
        using generated_body_contact_registry::GeneratedBodyKind;
        using generated_body_contact_registry::hasFiniteSampledVelocity;
        using generated_body_contact_registry::hasFlag;
        using generated_body_contact_registry::kFlagPowerArmor;
        using generated_body_contact_registry::kFlagPrimaryAnchor;

        struct HandContactSource
        {
            bool valid = false;
            bool isLeft = false;
            bool primaryAnchor = false;
            HandColliderBodyMetadata metadata{};
        };

        struct WeaponContactSource
        {
            bool valid = false;
            WeaponInteractionContact contact{};
            bool hasSampledVelocity = false;
            float sampledVelocityHavok[4]{};
        };

        Classification bodyAClassification{};
        Classification bodyBClassification{};
        const bool bodyAClassified = _generatedBodyContactRegistry.tryClassify(bodyIdA, bodyAClassification);
        const bool bodyBClassified = _generatedBodyContactRegistry.tryClassify(bodyIdB, bodyBClassification);
        (void)bodyAClassified;
        (void)bodyBClassified;

        auto classifyHandBody = [](const Classification& classification) {
            HandContactSource source{};
            if (!classification.valid ||
                (classification.kind != GeneratedBodyKind::RightHand && classification.kind != GeneratedBodyKind::LeftHand)) {
                return source;
            }

            source.valid = true;
            source.isLeft = classification.kind == GeneratedBodyKind::LeftHand;
            source.primaryAnchor = hasFlag(classification.flags, kFlagPrimaryAnchor);
            source.metadata.valid = true;
            source.metadata.isLeft = source.isLeft;
            source.metadata.primaryPalmAnchor = source.primaryAnchor;
            source.metadata.bodyId = classification.bodyId;
            source.metadata.role = static_cast<hand_collider_semantics::HandColliderRole>(classification.role);
            source.metadata.finger = static_cast<hand_collider_semantics::HandFinger>(classification.partKind);
            source.metadata.segment = static_cast<hand_collider_semantics::HandFingerSegment>(classification.subRole);
            if (hasFiniteSampledVelocity(classification)) {
                source.metadata.hasSampledLinearVelocityHavok = true;
                source.metadata.sampledLinearVelocityHavok[0] = classification.sampledVelocityHavokX;
                source.metadata.sampledLinearVelocityHavok[1] = classification.sampledVelocityHavokY;
                source.metadata.sampledLinearVelocityHavok[2] = classification.sampledVelocityHavokZ;
                source.metadata.sampledLinearVelocityHavok[3] = 0.0f;
            }
            return source;
        };

        auto classifyWeaponBody = [](const Classification& classification) {
            WeaponContactSource source{};
            if (!classification.valid || classification.kind != GeneratedBodyKind::Weapon) {
                return source;
            }

            source.valid = true;
            source.contact.valid = true;
            source.contact.bodyId = classification.bodyId;
            source.contact.partKind = static_cast<WeaponPartKind>(classification.partKind);
            source.contact.reloadRole = static_cast<WeaponReloadRole>(classification.role);
            source.contact.supportGripRole = static_cast<WeaponSupportGripRole>(classification.subRole);
            source.contact.socketRole = static_cast<WeaponSocketRole>(classification.socketRole);
            source.contact.actionRole = static_cast<WeaponActionRole>(classification.actionRole);
            source.contact.fallbackGripPose = static_cast<WeaponGripPoseId>(classification.gripPose);
            source.contact.weaponGenerationKey = classification.generationKey;
            if (hasFiniteSampledVelocity(classification)) {
                source.hasSampledVelocity = true;
                source.sampledVelocityHavok[0] = classification.sampledVelocityHavokX;
                source.sampledVelocityHavok[1] = classification.sampledVelocityHavokY;
                source.sampledVelocityHavok[2] = classification.sampledVelocityHavokZ;
                source.sampledVelocityHavok[3] = 0.0f;
            }
            return source;
        };

        auto classifyBodyCollider = [](const Classification& classification) {
            BodyBoneColliderMetadata metadata{};
            if (!classification.valid || classification.kind != GeneratedBodyKind::Body) {
                return metadata;
            }

            metadata.valid = true;
            metadata.inPowerArmor = hasFlag(classification.flags, kFlagPowerArmor);
            metadata.role = static_cast<skeleton_bone_debug_math::BoneColliderRole>(classification.role);
            metadata.zone = static_cast<body_zone::BodyZoneKind>(classification.zone);
            metadata.side = static_cast<body_zone::BodyZoneSide>(classification.side);
            metadata.bodyId = classification.bodyId;
            metadata.descriptorIndex = classification.descriptorIndex;
            metadata.lengthGameUnits = classification.lengthGameUnits;
            metadata.radiusGameUnits = classification.radiusGameUnits;
            return metadata;
        };

        const auto bodyARight = bodyAClassification.kind == GeneratedBodyKind::RightHand ? classifyHandBody(bodyAClassification) : HandContactSource{};
        const auto bodyBRight = bodyBClassification.kind == GeneratedBodyKind::RightHand ? classifyHandBody(bodyBClassification) : HandContactSource{};
        const auto bodyALeft = bodyAClassification.kind == GeneratedBodyKind::LeftHand ? classifyHandBody(bodyAClassification) : HandContactSource{};
        const auto bodyBLeft = bodyBClassification.kind == GeneratedBodyKind::LeftHand ? classifyHandBody(bodyBClassification) : HandContactSource{};
        const auto bodyAWeapon = classifyWeaponBody(bodyAClassification);
        const auto bodyBWeapon = classifyWeaponBody(bodyBClassification);
        BodyBoneColliderMetadata bodyABodyMetadata = classifyBodyCollider(bodyAClassification);
        BodyBoneColliderMetadata bodyBBodyMetadata = classifyBodyCollider(bodyBClassification);
        const bool bodyAIsRight = bodyARight.valid;
        const bool bodyBIsRight = bodyBRight.valid;
        const bool bodyAIsLeft = bodyALeft.valid;
        const bool bodyBIsLeft = bodyBLeft.valid;
        std::uint8_t bodyAHostHeldOwnerMask = 0u;
        std::uint8_t bodyBHostHeldOwnerMask = 0u;
        // Only generated-hand contacts need the host registry lookup. This
        // keeps unrelated world contact traffic allocation-free and prevents a
        // host-held BIPED_NO_CC body from being routed as a generic actor.
        if (bodyBIsRight || bodyBIsLeft) {
            bodyAHostHeldOwnerMask =
                HostExternalHeldBodyOwnerMask(world, bodyIdA);
        }
        if (bodyAIsRight || bodyAIsLeft) {
            bodyBHostHeldOwnerMask =
                HostExternalHeldBodyOwnerMask(world, bodyIdB);
        }
        const bool bodyAIsRightHeld =
            _rightHand.isHeldBodyId(bodyIdA) ||
            (bodyAHostHeldOwnerMask & 1u) != 0u;
        const bool bodyBIsRightHeld =
            _rightHand.isHeldBodyId(bodyIdB) ||
            (bodyBHostHeldOwnerMask & 1u) != 0u;
        const bool bodyAIsLeftHeld =
            _leftHand.isHeldBodyId(bodyIdA) ||
            (bodyAHostHeldOwnerMask & 2u) != 0u;
        const bool bodyBIsLeftHeld =
            _leftHand.isHeldBodyId(bodyIdB) ||
            (bodyBHostHeldOwnerMask & 2u) != 0u;
        const bool bodyAIsWeapon = bodyAWeapon.valid;
        const bool bodyBIsWeapon = bodyBWeapon.valid;
        const bool bodyAIsBody = bodyABodyMetadata.valid;
        const bool bodyBIsBody = bodyBBodyMetadata.valid;
        const bool bodyAIsRockSource = bodyAIsRight || bodyAIsLeft || bodyAIsRightHeld || bodyAIsLeftHeld || bodyAIsWeapon || bodyAIsBody;
        const bool bodyBIsRockSource = bodyBIsRight || bodyBIsLeft || bodyBIsRightHeld || bodyBIsLeftHeld || bodyBIsWeapon || bodyBIsBody;

        auto looseGrenadeImpactBodyIsWatched = [&](std::uint32_t bodyId) {
            if (isInvalidGrabBodyId(bodyId)) {
                return false;
            }
            for (const auto& watchedBodyId : _armedLooseGrenadeImpactBodyIds) {
                if (watchedBodyId.load(std::memory_order_acquire) == bodyId) {
                    return true;
                }
            }
            return false;
        };

        auto recordLooseGrenadeImpactIfArmed = [&]() {
            auto tryRecord = [&](std::uint32_t watchedBodyId,
                                 std::uint32_t otherBodyId,
                                 bool watchedIsHeld,
                                 bool otherIsRightHand,
                                 bool otherIsLeftHand) {
                if (!looseGrenadeImpactBodyIsWatched(watchedBodyId) || watchedIsHeld || otherIsRightHand || otherIsLeftHand ||
                    otherBodyId == rightId || otherBodyId == leftId || isInvalidGrabBodyId(otherBodyId)) {
                    return false;
                }

                _pendingLooseGrenadeImpactPair.store(packHeldImpactPair(watchedBodyId, otherBodyId), std::memory_order_release);
                return true;
            };

            if (tryRecord(
                    bodyIdA,
                    bodyIdB,
                    bodyAIsRightHeld || bodyAIsLeftHeld,
                    bodyBIsRight,
                    bodyBIsLeft)) {
                return;
            }
            static_cast<void>(tryRecord(
                bodyIdB,
                bodyIdA,
                bodyBIsRightHeld || bodyBIsLeftHeld,
                bodyAIsRight,
                bodyAIsLeft));
        };

        recordLooseGrenadeImpactIfArmed();

        if (contact_pipeline_policy::shouldSkipContactSignalBeforeLayerRead(contact_pipeline_policy::ContactSignalPrefilter{
                .bodyIdA = bodyIdA,
                .bodyIdB = bodyIdB,
                .bodyAIsRockSource = bodyAIsRockSource,
                .bodyBIsRockSource = bodyBIsRockSource,
            })) {
            return;
        }

        /*
         * External-body lookup takes the provider registry mutex. Defer it
         * until the allocation-free source prefilter proves this contact can
         * reach a ROCK route; unrelated world contacts then never contend on
         * the provider mutex.
         */
        const bool bodyAIsExternal =
            ::rock::provider::isExternalBodyId(bodyIdA);
        const bool bodyBIsExternal =
            ::rock::provider::isExternalBodyId(bodyIdB);

        auto readBodyFilterInfo = [world](std::uint32_t bodyId) {
            std::uint32_t filterInfo = 0;
            if (world && havok_runtime::tryReadFilterInfo(world, RE::hknpBodyId{ bodyId }, filterInfo)) {
                return filterInfo;
            }
            return contact_pipeline_policy::kUnknownLayer;
        };

        auto filterInfoToLayer = [](std::uint32_t filterInfo) {
            return filterInfo == contact_pipeline_policy::kUnknownLayer ? contact_pipeline_policy::kUnknownLayer : (filterInfo & 0x7Fu);
        };

        const std::uint32_t bodyAFilterInfo = readBodyFilterInfo(bodyIdA);
        const std::uint32_t bodyBFilterInfo = readBodyFilterInfo(bodyIdB);
        const std::uint32_t bodyALayer = filterInfoToLayer(bodyAFilterInfo);
        const std::uint32_t bodyBLayer = filterInfoToLayer(bodyBFilterInfo);

        auto makeEndpoint = [&](std::uint32_t bodyId, std::uint32_t layer, bool isRightHand, bool isLeftHand, bool isWeapon, bool isRightHeld, bool isLeftHeld, bool isBody, bool isExternal) {
            using contact_pipeline_policy::ContactEndpoint;
            using contact_pipeline_policy::ContactEndpointKind;

            ContactEndpoint endpoint{};
            endpoint.bodyId = bodyId;
            endpoint.layer = layer;
            if (isRightHand) {
                endpoint.kind = ContactEndpointKind::RightHand;
            } else if (isLeftHand) {
                endpoint.kind = ContactEndpointKind::LeftHand;
            } else if (isWeapon) {
                endpoint.kind = ContactEndpointKind::Weapon;
            } else if (isRightHeld) {
                endpoint.kind = ContactEndpointKind::RightHeldObject;
            } else if (isLeftHeld) {
                endpoint.kind = ContactEndpointKind::LeftHeldObject;
            } else if (isBody) {
                endpoint.kind = ContactEndpointKind::Body;
            } else if (isExternal) {
                endpoint.kind = ContactEndpointKind::External;
            } else {
                endpoint.kind = contact_pipeline_policy::classifyNonRockLayer(layer);
            }
            return endpoint;
        };

        const auto endpointA = makeEndpoint(bodyIdA, bodyALayer, bodyAIsRight, bodyAIsLeft, bodyAIsWeapon, bodyAIsRightHeld, bodyAIsLeftHeld, bodyAIsBody, bodyAIsExternal);
        const auto endpointB = makeEndpoint(bodyIdB, bodyBLayer, bodyBIsRight, bodyBIsLeft, bodyBIsWeapon, bodyBIsRightHeld, bodyBIsLeftHeld, bodyBIsBody, bodyBIsExternal);
        const auto contactRoute = contact_pipeline_policy::classifyContact(endpointA, endpointB);

        auto handSourceFor = [&](std::uint32_t bodyId) -> const HandContactSource* {
            if (bodyARight.valid && bodyARight.metadata.bodyId == bodyId) {
                return &bodyARight;
            }
            if (bodyBRight.valid && bodyBRight.metadata.bodyId == bodyId) {
                return &bodyBRight;
            }
            if (bodyALeft.valid && bodyALeft.metadata.bodyId == bodyId) {
                return &bodyALeft;
            }
            if (bodyBLeft.valid && bodyBLeft.metadata.bodyId == bodyId) {
                return &bodyBLeft;
            }
            return nullptr;
        };

        auto bodySourceFor = [&](std::uint32_t bodyId) -> const BodyBoneColliderMetadata* {
            if (bodyABodyMetadata.valid && bodyABodyMetadata.bodyId == bodyId) {
                return &bodyABodyMetadata;
            }
            if (bodyBBodyMetadata.valid && bodyBBodyMetadata.bodyId == bodyId) {
                return &bodyBBodyMetadata;
            }
            return nullptr;
        };

        auto weaponSourceFor = [&](std::uint32_t bodyId) -> const WeaponContactSource* {
            if (bodyAWeapon.valid && bodyAWeapon.contact.bodyId == bodyId) {
                return &bodyAWeapon;
            }
            if (bodyBWeapon.valid && bodyBWeapon.contact.bodyId == bodyId) {
                return &bodyBWeapon;
            }
            return nullptr;
        };

        auto fillSourceVelocity = [&](std::uint32_t sourceBodyId,
                                      ::rock::provider::RockProviderExternalSourceKind sourceKind,
                                      const HandColliderBodyMetadata* handMetadata,
                                      ::rock::provider::RockProviderExternalContactV1& contact) -> bool {
            if (handMetadata && handMetadata->valid && handMetadata->hasSampledLinearVelocityHavok &&
                havok_runtime::isFinite3(handMetadata->sampledLinearVelocityHavok)) {
                std::copy_n(handMetadata->sampledLinearVelocityHavok, 4, contact.sourceVelocityHavok);
                return true;
            }

            if (sourceKind == ::rock::provider::RockProviderExternalSourceKind::Weapon) {
                const auto* weaponSource = weaponSourceFor(sourceBodyId);
                if (weaponSource && weaponSource->valid && weaponSource->hasSampledVelocity &&
                    havok_runtime::isFinite3(weaponSource->sampledVelocityHavok)) {
                    std::copy_n(weaponSource->sampledVelocityHavok, 4, contact.sourceVelocityHavok);
                    return true;
                }
            }

            if (!world || sourceBodyId == INVALID_CONTACT_BODY_ID) {
                return false;
            }

            auto* motion = havok_runtime::getBodyMotion(world, RE::hknpBodyId{ sourceBodyId });
            if (!motion) {
                return false;
            }

            contact.sourceVelocityHavok[0] = motion->linearVelocity.x;
            contact.sourceVelocityHavok[1] = motion->linearVelocity.y;
            contact.sourceVelocityHavok[2] = motion->linearVelocity.z;
            return havok_runtime::isFinite3(contact.sourceVelocityHavok);
        };

        auto tryFillAggregateContactPoint = [world](std::uint32_t sourceBodyId,
                                                    std::uint32_t externalBodyId,
                                                    ::rock::provider::RockProviderExternalContactV1& contact) {
            if (!world || sourceBodyId == INVALID_CONTACT_BODY_ID || externalBodyId == INVALID_CONTACT_BODY_ID) {
                return false;
            }

            RE::NiTransform sourceTransform{};
            RE::NiTransform targetTransform{};
            if (!havok_runtime::tryResolveLiveBodyWorldTransform(world, RE::hknpBodyId{ sourceBodyId }, sourceTransform) ||
                !havok_runtime::tryResolveLiveBodyWorldTransform(world, RE::hknpBodyId{ externalBodyId }, targetTransform)) {
                return false;
            }

            if (!std::isfinite(sourceTransform.translate.x) || !std::isfinite(sourceTransform.translate.y) || !std::isfinite(sourceTransform.translate.z) ||
                !std::isfinite(targetTransform.translate.x) || !std::isfinite(targetTransform.translate.y) || !std::isfinite(targetTransform.translate.z)) {
                return false;
            }

            const float scale = gameToHavokScale();
            contact.contactPointHavok[0] = targetTransform.translate.x * scale;
            contact.contactPointHavok[1] = targetTransform.translate.y * scale;
            contact.contactPointHavok[2] = targetTransform.translate.z * scale;
            contact.contactPointHavok[3] = 0.0f;

            const float dx = (targetTransform.translate.x - sourceTransform.translate.x) * scale;
            const float dy = (targetTransform.translate.y - sourceTransform.translate.y) * scale;
            const float dz = (targetTransform.translate.z - sourceTransform.translate.z) * scale;
            const float lenSq = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(lenSq) && lenSq > 0.000001f) {
                const float invLen = 1.0f / std::sqrt(lenSq);
                contact.contactNormalHavok[0] = dx * invLen;
                contact.contactNormalHavok[1] = dy * invLen;
                contact.contactNormalHavok[2] = dz * invLen;
                contact.flags |= static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderExternalContactFlagV1::ContactNormalValid);
            }

            contact.contactPointWeightSum = 0.0f;
            contact.quality = ::rock::provider::RockProviderExternalContactQuality::AggregateImpulse;
            contact.flags |=
                static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderExternalContactFlagV1::ContactPointValid) |
                static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderExternalContactFlagV1::ContactPointEstimated);
            return true;
        };

        auto publishExternalContact = [&](std::uint32_t sourceBodyId,
                                          std::uint32_t externalBodyId,
                                          ::rock::provider::RockProviderExternalSourceKind sourceKind,
                                          ::rock::provider::RockProviderHand sourceHand,
                                          const HandColliderBodyMetadata* handMetadata = nullptr) {
            if (sourceBodyId == INVALID_CONTACT_BODY_ID || externalBodyId == INVALID_CONTACT_BODY_ID || sourceBodyId == externalBodyId) {
                return;
            }

            ::rock::provider::RockProviderExternalContactV1 contact{};
            contact.sourceBodyId = sourceBodyId;
            contact.targetExternalBodyId = externalBodyId;
            contact.sourceKind = sourceKind;
            contact.sourceHand = sourceHand;
            contact.quality = ::rock::provider::RockProviderExternalContactQuality::BodyPairOnly;
            contact.frameIndex =
                _palmClockGameFrameIndex.load(std::memory_order_acquire);
            contact.collisionGeneration =
                _collisionGenerationAtomic.load(std::memory_order_acquire);
            if (fillSourceVelocity(sourceBodyId, sourceKind, handMetadata, contact)) {
                contact.flags |= static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderExternalContactFlagV1::SourceVelocityValid);
            }

            if (ensureRawContactPoint()) {
                contact.quality = ::rock::provider::RockProviderExternalContactQuality::RawPoint;
                contact.contactPointWeightSum = rawContactPoint.contactPointWeightSum;
                std::copy_n(rawContactPoint.contactPointHavok, 4, contact.contactPointHavok);
                std::copy_n(rawContactPoint.contactNormalHavok, 4, contact.contactNormalHavok);
                contact.flags |=
                    static_cast<std::uint32_t>(
                        ::rock::provider::RockProviderExternalContactFlagV1::ContactPointValid) |
                    static_cast<std::uint32_t>(
                        ::rock::provider::RockProviderExternalContactFlagV1::ContactNormalValid) |
                    static_cast<std::uint32_t>(
                        ::rock::provider::RockProviderExternalContactFlagV1::ContactPointMeasured);
            } else {
                tryFillAggregateContactPoint(sourceBodyId, externalBodyId, contact);
            }

            if (handMetadata && handMetadata->valid) {
                contact.sourceRole = static_cast<std::uint32_t>(handMetadata->role);
                contact.sourcePartKind = static_cast<std::uint32_t>(handMetadata->finger);
                contact.sourceSubRole = static_cast<std::uint32_t>(handMetadata->segment);
            } else if (sourceKind == ::rock::provider::RockProviderExternalSourceKind::Weapon) {
                if (const auto* weaponSource = weaponSourceFor(sourceBodyId); weaponSource && weaponSource->valid) {
                    contact.sourcePartKind = static_cast<std::uint32_t>(weaponSource->contact.partKind);
                    contact.sourceRole = static_cast<std::uint32_t>(weaponSource->contact.reloadRole);
                    contact.sourceSubRole = static_cast<std::uint32_t>(weaponSource->contact.supportGripRole);
                }
            }

            const bool transitionSuppressed =
                _dynamicHandCollision.isTransitionCollisionSuppressedAtomic();
            if (transitionSuppressed) {
                contact.flags |= static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderExternalContactFlagV1::TransitionSuppressed);
            } else if ((_lifecycleFlagsAtomic.load(std::memory_order_acquire) &
                            static_cast<std::uint32_t>(
                                ::rock::provider::RockProviderLifecycleFlag::PhysicsWriteAllowed)) != 0) {
                contact.flags |= static_cast<std::uint32_t>(
                    ::rock::provider::RockProviderExternalContactFlagV1::CollisionAvailable);
            }

            ::rock::provider::recordExternalContact(
                contact,
                _worldGenerationAtomic.load(std::memory_order_acquire),
                _skeletonGenerationAtomic.load(std::memory_order_acquire),
                _providerGenerationAtomic.load(std::memory_order_acquire));
        };

        /*
         * Heisenberg-preserved native contact evidence production (upstream
         * removed the soft-contact subsystem in 9b7c7ee). This is the only
         * producer for NativeContactEvidenceCache, which SoftContactRuntime
         * consumes once per frame. It reuses upstream's own contactRoute /
         * rawContactPoint / filter-info locals so it stays in lockstep with the
         * newer routing rather than duplicating it.
         */
        auto endpointKindForEvidence = [](contact_pipeline_policy::ContactEndpointKind kind) {
            using SourceKind = contact_pipeline_policy::ContactEndpointKind;
            using EvidenceKind = contact_evidence::NativeContactEndpointKind;
            switch (kind) {
            case SourceKind::RightHand:
                return EvidenceKind::RightHand;
            case SourceKind::LeftHand:
                return EvidenceKind::LeftHand;
            case SourceKind::Weapon:
                return EvidenceKind::Weapon;
            case SourceKind::RightHeldObject:
                return EvidenceKind::RightHeldObject;
            case SourceKind::LeftHeldObject:
                return EvidenceKind::LeftHeldObject;
            case SourceKind::External:
                return EvidenceKind::External;
            case SourceKind::WorldSurface:
                return EvidenceKind::WorldSurface;
            case SourceKind::DynamicProp:
                return EvidenceKind::DynamicProp;
            case SourceKind::Actor:
                return EvidenceKind::Actor;
            case SourceKind::QueryOnly:
                return EvidenceKind::QueryOnly;
            default:
                return EvidenceKind::Unknown;
            }
        };

        auto fillNativeSourceVelocity = [world](std::uint32_t sourceBodyId, contact_evidence::NativeContactEvidenceRecord& evidence) {
            if (!world || !contact_evidence::isValidBodyId(sourceBodyId)) {
                return;
            }

            auto* motion = havok_runtime::getBodyMotion(world, RE::hknpBodyId{ sourceBodyId });
            if (!motion) {
                return;
            }

            const float scale = havokToGameScale();
            evidence.sourceVelocityGame = RE::NiPoint3{
                motion->linearVelocity.x * scale,
                motion->linearVelocity.y * scale,
                motion->linearVelocity.z * scale,
            };
        };

        auto publishNativeContactEvidence = [&](const HandColliderBodyMetadata* handMetadata = nullptr) {
            if (!contactRoute.recordWorldSurfaceEvidence ||
                (!contact_pipeline_policy::isHand(
                     contactRoute.source.kind) &&
                 contactRoute.source.kind !=
                     contact_pipeline_policy::
                         ContactEndpointKind::Weapon) ||
                !contact_evidence::isValidBodyId(contactRoute.sourceBodyId) ||
                !contact_evidence::isValidBodyId(contactRoute.targetBodyId) ||
                contactRoute.sourceBodyId == contactRoute.targetBodyId) {
                return;
            }

            if (!ensureRawContactPoint()) {
                return;
            }

            contact_evidence::NativeContactEvidenceRecord evidence{};
            evidence.frame = _handContactActivity.currentFrame();
            evidence.sourceBodyId = contactRoute.sourceBodyId;
            evidence.targetBodyId = contactRoute.targetBodyId;
            evidence.sourceLayer = contactRoute.source.layer;
            evidence.targetLayer = contactRoute.target.layer;
            evidence.sourceFilterInfo = contactRoute.sourceBodyId == bodyIdA ? bodyAFilterInfo : bodyBFilterInfo;
            evidence.targetFilterInfo = contactRoute.targetBodyId == bodyIdA ? bodyAFilterInfo : bodyBFilterInfo;
            evidence.sourceKind = endpointKindForEvidence(contactRoute.source.kind);
            evidence.targetKind = endpointKindForEvidence(contactRoute.target.kind);
            evidence.sourceIsLeft = contact_pipeline_policy::isLeftOwned(contactRoute.source.kind);
            evidence.targetIsLeft = contact_pipeline_policy::isLeftOwned(contactRoute.target.kind);
            fillNativeSourceVelocity(contactRoute.sourceBodyId, evidence);

            const float scale = havokToGameScale();
            evidence.quality = contact_evidence::NativeContactQuality::RawPoint;
            evidence.contactPointWeightSum = rawContactPoint.contactPointWeightSum;
            evidence.contactPointGame = RE::NiPoint3{
                rawContactPoint.contactPointHavok[0] * scale,
                rawContactPoint.contactPointHavok[1] * scale,
                rawContactPoint.contactPointHavok[2] * scale,
            };
            evidence.contactNormalGame = RE::NiPoint3{
                rawContactPoint.contactNormalHavok[0],
                rawContactPoint.contactNormalHavok[1],
                rawContactPoint.contactNormalHavok[2],
            };

            if (handMetadata && handMetadata->valid) {
                evidence.sourceRole = static_cast<std::uint32_t>(handMetadata->role);
                evidence.sourcePartKind = static_cast<std::uint32_t>(handMetadata->finger);
                evidence.sourceSubRole = static_cast<std::uint32_t>(handMetadata->segment);
            } else if (contactRoute.source.kind == contact_pipeline_policy::ContactEndpointKind::Weapon) {
                if (const auto* weaponSource = weaponSourceFor(contactRoute.sourceBodyId); weaponSource && weaponSource->valid) {
                    evidence.sourcePartKind = static_cast<std::uint32_t>(weaponSource->contact.partKind);
                    evidence.sourceRole = static_cast<std::uint32_t>(weaponSource->contact.reloadRole);
                    evidence.sourceSubRole = static_cast<std::uint32_t>(weaponSource->contact.supportGripRole);
                }
            }

            _nativeContactEvidence.record(evidence);
        };

        auto recordBodyContactEvidence = [&]() {
            if (!contactRoute.recordBodyContact || !contact_pipeline_policy::isBody(contactRoute.source.kind)) {
                return;
            }

            const auto* bodyMetadata = bodySourceFor(contactRoute.sourceBodyId);
            if (!bodyMetadata || !bodyMetadata->valid) {
                return;
            }

            body_contact_runtime::BodyContactRecord record{};
            record.frame = _handContactActivity.currentFrame();
            record.bodyId = contactRoute.sourceBodyId;
            record.targetBodyId = contactRoute.targetBodyId;
            record.bodyLayer = contactRoute.source.layer;
            record.targetLayer = contactRoute.target.layer;
            record.role = bodyMetadata->role;
            record.zone = bodyMetadata->zone;
            record.side = bodyMetadata->side;
            record.descriptorIndex = bodyMetadata->descriptorIndex;
            record.targetKind = contactRoute.target.kind;
            record.inPowerArmor = bodyMetadata->inPowerArmor;
            if (const auto* targetBodyMetadata = bodySourceFor(contactRoute.targetBodyId); targetBodyMetadata && targetBodyMetadata->valid) {
                record.targetRole = targetBodyMetadata->role;
                record.targetZone = targetBodyMetadata->zone;
                record.targetSide = targetBodyMetadata->side;
                record.targetDescriptorIndex = targetBodyMetadata->descriptorIndex;
                record.targetInPowerArmor = targetBodyMetadata->inPowerArmor;
            }
            if (ensureRawContactPoint()) {
                const float scale = havokToGameScale();
                record.contactPointGame = RE::NiPoint3{
                    rawContactPoint.contactPointHavok[0] * scale,
                    rawContactPoint.contactPointHavok[1] * scale,
                    rawContactPoint.contactPointHavok[2] * scale,
                };
                record.hasContactPointGame = true;
            }

            _bodyContactRuntime.record(record);
        };

        auto notifyHeldExternalContact = [&](Hand& hand,
                                             std::atomic<std::uint64_t>& impactPair,
                                             bool bodyAIsHeld,
                                             bool bodyBIsHeld) {
            if (!bodyAIsHeld && !bodyBIsHeld) {
                return;
            }

            const std::uint32_t heldId = bodyAIsHeld ? bodyIdA : bodyIdB;
            const std::uint32_t other = bodyAIsHeld ? bodyIdB : bodyIdA;
            const bool otherIsA = other == bodyIdA;
            const auto decision = held_object_contact_policy::evaluateHeldExternalContact(
                held_object_contact_policy::HeldExternalContactInput{
                    .handHolding = hand.isHoldingAtomic(),
                    .bodyAIsHeld = bodyAIsHeld,
                    .bodyBIsHeld = bodyBIsHeld,
                    .otherIsRightHand = otherIsA ? bodyAIsRight : bodyBIsRight,
                    .otherIsLeftHand = otherIsA ? bodyAIsLeft : bodyBIsLeft,
                    .otherIsRightPalmBody = other == rightId,
                    .otherIsLeftPalmBody = other == leftId,
                    .otherIsBodyCollider = otherIsA ? bodyAIsBody : bodyBIsBody,
                    .otherIsExternalProvider =
                        otherIsA ? bodyAIsExternal : bodyBIsExternal,
                });
            if (decision.sameHeldObject) {
                ROCK_LOG_SAMPLE_DEBUG(Hand,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "{} held self-contact suppressed: bodyA={} bodyB={}",
                    hand.handName(),
                    bodyIdA,
                    bodyIdB);
                return;
            }
            if (!decision.notify) {
                return;
            }

            RE::NiPoint3 contactPointHavok{};
            RE::NiPoint3 contactNormalHavok{};
            bool hasContactNormal = false;
            if (ensureRawContactPoint()) {
                contactPointHavok = RE::NiPoint3{
                    rawContactPoint.contactPointHavok[0],
                    rawContactPoint.contactPointHavok[1],
                    rawContactPoint.contactPointHavok[2],
                };
                contactNormalHavok = RE::NiPoint3{
                    rawContactPoint.contactNormalHavok[0],
                    rawContactPoint.contactNormalHavok[1],
                    rawContactPoint.contactNormalHavok[2],
                };
                const float normalLengthSq =
                    contactNormalHavok.x * contactNormalHavok.x +
                    contactNormalHavok.y * contactNormalHavok.y +
                    contactNormalHavok.z * contactNormalHavok.z;
                hasContactNormal = std::isfinite(normalLengthSq) && normalLengthSq > 1.0e-6f;
            }

            const std::uint32_t otherLayer = bodyAIsHeld ? bodyBLayer : bodyALayer;
            hand.notifyHeldBodyContact(heldId, other, otherLayer, contactPointHavok, contactNormalHavok, hasContactNormal);
            impactPair.store(packHeldImpactPair(heldId, other), std::memory_order_release);
        };

        notifyHeldExternalContact(_rightHand, _lastHeldImpactPairRight, bodyAIsRightHeld, bodyBIsRightHeld);
        notifyHeldExternalContact(_leftHand, _lastHeldImpactPairLeft, bodyAIsLeftHeld, bodyBIsLeftHeld);

        recordBodyContactEvidence();

        /*
         * hknp can still deliver a body pair from the step boundary after ROCK
         * has already leased a hand body into no-collision ownership. Keep
         * held-object contact keepalive above this point, then suppress every
         * generated hand-side effect below it: native evidence, provider
         * contacts, weapon support contact, semantic touch, and dynamic push.
         */
        const bool rightBodyPairSuppressed =
            (bodyAIsRight || bodyBIsRight) &&
            (_rightDominantWeaponCollisionSuppressed.load(std::memory_order_acquire) ||
                _rightHand.hasContactEvidenceSuppressedAtomic());
        const bool leftBodyPairSuppressed =
            (bodyAIsLeft || bodyBIsLeft) &&
            (_leftWeaponSupportCollisionSuppressed.load(std::memory_order_acquire) ||
                _leftHand.hasContactEvidenceSuppressedAtomic());
        if (rightBodyPairSuppressed || leftBodyPairSuppressed) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "Suppressed hand body contact skipped: route={} rightSuppressed={} leftSuppressed={} bodyA={} bodyB={}",
                contact_pipeline_policy::routeName(contactRoute.route),
                rightBodyPairSuppressed ? "yes" : "no",
                leftBodyPairSuppressed ? "yes" : "no",
                bodyIdA,
                bodyIdB);
            return;
        }

        auto routeSourceHandContactEvidenceSuppressed = [&]() {
            if (contact_pipeline_policy::isRightHand(contactRoute.source.kind)) {
                return isHandContactEvidenceSuppressed(false);
            }
            if (contact_pipeline_policy::isLeftHand(contactRoute.source.kind)) {
                return isHandContactEvidenceSuppressed(true);
            }
            return false;
        };
        if (routeSourceHandContactEvidenceSuppressed()) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "Contact evidence skipped for stronger hand owner: route={} sourceBody={} targetBody={}",
                contact_pipeline_policy::routeName(contactRoute.route),
                contactRoute.sourceBodyId,
                contactRoute.targetBodyId);
            return;
        }

        bool isRight = bodyAIsRight || bodyBIsRight;
        bool isLeft = bodyAIsLeft || bodyBIsLeft;
        const HandColliderBodyMetadata* routeHandMetadata = nullptr;
        if (contact_pipeline_policy::isHand(contactRoute.source.kind)) {
            const auto* handSource = handSourceFor(contactRoute.sourceBodyId);
            routeHandMetadata = handSource && handSource->valid ? &handSource->metadata : nullptr;
        }

        // Heisenberg-preserved: feed the soft-contact native evidence cache from
        // the same routed contact that upstream publishes externally.
        publishNativeContactEvidence(routeHandMetadata);

        if (contactRoute.publishExternalContact) {
            publishExternalContact(contactRoute.sourceBodyId, contactRoute.targetBodyId, contactRoute.providerSourceKind, contactRoute.providerSourceHand, routeHandMetadata);
        }

        if (contactRoute.driveWeaponDynamicPush) {
            // Heisenberg-preserved multi-slot capture (task #204).
            _contactSlotWeapon.record(contactRoute.sourceBodyId, contactRoute.targetBodyId);
        }

        auto publishWeaponContactFromPhysics = [&](bool isLeft, const WeaponInteractionContact& weaponContact, std::uint32_t bodyId) {
            auto& partKind = isLeft ? _leftWeaponContactPartKind : _rightWeaponContactPartKind;
            auto& reloadRole = isLeft ? _leftWeaponContactReloadRole : _rightWeaponContactReloadRole;
            auto& supportRole = isLeft ? _leftWeaponContactSupportRole : _rightWeaponContactSupportRole;
            auto& socketRole = isLeft ? _leftWeaponContactSocketRole : _rightWeaponContactSocketRole;
            auto& actionRole = isLeft ? _leftWeaponContactActionRole : _rightWeaponContactActionRole;
            auto& gripPose = isLeft ? _leftWeaponContactGripPose : _rightWeaponContactGripPose;
            auto& sequence = isLeft ? _leftWeaponContactSequence : _rightWeaponContactSequence;
            auto& missedFrames = isLeft ? _leftWeaponContactMissedFrames : _rightWeaponContactMissedFrames;
            auto& bodyIdAtomic = isLeft ? _leftWeaponContactBodyId : _rightWeaponContactBodyId;

            partKind.store(static_cast<std::uint32_t>(weaponContact.partKind), std::memory_order_release);
            reloadRole.store(static_cast<std::uint32_t>(weaponContact.reloadRole), std::memory_order_release);
            supportRole.store(static_cast<std::uint32_t>(weaponContact.supportGripRole), std::memory_order_release);
            socketRole.store(static_cast<std::uint32_t>(weaponContact.socketRole), std::memory_order_release);
            actionRole.store(static_cast<std::uint32_t>(weaponContact.actionRole), std::memory_order_release);
            gripPose.store(static_cast<std::uint32_t>(weaponContact.fallbackGripPose), std::memory_order_release);
            sequence.fetch_add(1, std::memory_order_acq_rel);
            missedFrames.store(0, std::memory_order_release);
            bodyIdAtomic.store(bodyId, std::memory_order_release);
        };

        if (!isRight && !isLeft) {
            return;
        }

        if (contactRoute.route == contact_pipeline_policy::ContactRoute::RockInternal) {
            return;
        }

        if (contactRoute.drivesWeaponSupportContact && contact_pipeline_policy::isHand(contactRoute.source.kind)) {
            if (const auto* weaponSource = weaponSourceFor(contactRoute.targetBodyId); weaponSource && weaponSource->valid) {
                publishWeaponContactFromPhysics(contact_pipeline_policy::isLeftHand(contactRoute.source.kind), weaponSource->contact, contactRoute.targetBodyId);
            }
        }

        const auto* handSource = contactRoute.recordHandSemanticContact ? handSourceFor(contactRoute.sourceBodyId) : nullptr;
        if (!handSource || !handSource->valid) {
            return;
        }

        const auto contactActivity = _handContactActivity.registerHandContact(handSource->isLeft, handSource->metadata.bodyId, contactRoute.targetBodyId);
        if (contactActivity.newlyActive && g_rockConfig.rockDebugVerboseLogging) {
            ROCK_LOG_DEBUG(Hand,
                "ContactActivity: {} {} body={} target={} frame={} inserted={} evictedStale={}",
                handSource->isLeft ? "Left" : "Right",
                hand_collider_semantics::roleName(handSource->metadata.role),
                handSource->metadata.bodyId,
                contactRoute.targetBodyId,
                contactActivity.frame,
                contactActivity.inserted ? "yes" : "no",
                contactActivity.evictedStale ? "yes" : "no");
        }

        hand_semantic_contact_state::SemanticContactVector semanticContactPointGame{};
        hand_semantic_contact_state::SemanticContactVector semanticContactNormalGame{};
        const hand_semantic_contact_state::SemanticContactVector* semanticContactPoint = nullptr;
        const hand_semantic_contact_state::SemanticContactVector* semanticContactNormal = nullptr;
        if (ensureRawContactPoint()) {
            const float scale = havokToGameScale();
            semanticContactPointGame = hand_semantic_contact_state::SemanticContactVector{
                rawContactPoint.contactPointHavok[0] * scale,
                rawContactPoint.contactPointHavok[1] * scale,
                rawContactPoint.contactPointHavok[2] * scale,
            };
            semanticContactNormalGame = hand_semantic_contact_state::SemanticContactVector{
                rawContactPoint.contactNormalHavok[0],
                rawContactPoint.contactNormalHavok[1],
                rawContactPoint.contactNormalHavok[2],
            };
            if (hand_semantic_contact_state::isFiniteVector(semanticContactPointGame)) {
                semanticContactPoint = &semanticContactPointGame;
            }
            const float normalLengthSquared =
                semanticContactNormalGame.x * semanticContactNormalGame.x +
                semanticContactNormalGame.y * semanticContactNormalGame.y +
                semanticContactNormalGame.z * semanticContactNormalGame.z;
            if (hand_semantic_contact_state::isFiniteVector(semanticContactNormalGame) &&
                std::isfinite(normalLengthSquared) &&
                normalLengthSquared > 1.0e-6f) {
                semanticContactNormal = &semanticContactNormalGame;
            }
        }

        if (handSource->isLeft) {
            _leftHand.recordSemanticContact(handSource->metadata, contactRoute.targetBodyId, semanticContactPoint, semanticContactNormal);
            if (contactRoute.driveHandDynamicPush) {
                // Heisenberg-preserved multi-slot capture (task #204).
                _contactSlotLeft.record(handSource->metadata.bodyId, contactRoute.targetBodyId);
            }
        } else {
            _rightHand.recordSemanticContact(handSource->metadata, contactRoute.targetBodyId, semanticContactPoint, semanticContactNormal);
            if (contactRoute.driveHandDynamicPush) {
                // Heisenberg-preserved multi-slot capture (task #204).
                _contactSlotRight.record(handSource->metadata.bodyId, contactRoute.targetBodyId);
            }
        }

        int logCount = _contactLogCounter.fetch_add(1, std::memory_order_relaxed);
        if (logCount % 30 == 0) {
            ROCK_LOG_DEBUG(Hand,
                "Contact: {} {} body={} hit body {} route={}",
                handSource->isLeft ? "Left" : "Right",
                hand_collider_semantics::roleName(handSource->metadata.role),
                handSource->metadata.bodyId,
                contactRoute.targetBodyId,
                contact_pipeline_policy::routeName(contactRoute.route));
        }
    }
