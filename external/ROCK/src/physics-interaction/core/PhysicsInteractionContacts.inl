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
                "{} dynamic push skipped: target body {} is registered as external suppressing ROCK dynamic push",
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
        for (std::size_t handIndex = 0; handIndex < 2; ++handIndex) {
            const auto remainingFrames =
                _hostRecentReleaseFrames[handIndex].load(std::memory_order_acquire);
            const auto recentFormId =
                _hostRecentReleaseFormId[handIndex].load(std::memory_order_acquire);
            if (remainingFrames > 0 &&
                recentFormId != 0 &&
                targetRef->GetFormID() == recentFormId) {
                ROCK_LOG_SAMPLE_DEBUG(Hand,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "{} dynamic push skipped: target body {} formID={:08X} "
                    "was just released by host ({} guard frames remain; "
                    "native collision stays active)",
                    sourceName,
                    targetBodyId,
                    recentFormId,
                    remainingFrames);
                return;
            }
        }

        /*
         * [TOUCH-DIAG] Root-caused and fixed 2026-07-22 (PhysicsScale.h fallback
         * constant mismatch - see PhysicsScale.h's own comment). This block was
         * built for that investigation and is no longer needed by default, but is
         * kept (gated, not deleted) in case a related symptom resurfaces - it is
         * the only place that cross-checks source/target body position against
         * several independent visual references (native ref, node, FRIK) in one
         * shot. Gated behind rockDebugVerboseLogging so the extra Havok/FRIK reads
         * this block performs are skipped entirely during normal play, not just
         * the log line.
         */
        if (g_rockConfig.rockDebugVerboseLogging) {
            RE::NiTransform sourceBodyWorld{};
            RE::NiTransform targetBodyWorld{};
            const bool haveSourceBody = havok_runtime::tryResolveLiveBodyWorldTransform(hknp, RE::hknpBodyId{ sourceBodyId }, sourceBodyWorld);
            const bool haveTargetBody = havok_runtime::tryResolveLiveBodyWorldTransform(hknp, RE::hknpBodyId{ targetBodyId }, targetBodyWorld);

            const RE::NiPoint3 targetRefVisualPos = targetRef->GetPosition();
            auto* targetNode = targetRef->Get3D();
            const RE::NiPoint3 targetNodeVisualPos = targetNode ? targetNode->world.translate : targetRefVisualPos;

            // [TOUCH-DIAG] The one comparison still missing: the SOURCE's (gun/hand's)
            // own raw Havok body position against ITS own true visual position - the
            // user's report (Jul 22) is a fixed, perfectly repeatable per-DIRECTION
            // offset that is the SAME for every different target object, which points
            // at the SOURCE's own collision geometry/placement, not the target's (the
            // target-vs-visual gap was already proven tight and identical in both
            // locations - this checks the other half of the pair). Weapon: the real
            // live weapon NiNode (f4vr::getWeaponNode(), same accessor WeaponCollision.cpp
            // itself uses to place the weapon's own generated bodies). Hand: the exact
            // ground-truth transform ROCK's own keyframe-drive code is fed each frame
            // (getInteractionHandTransform) - mirrors the already-working hand origin
            // sample from earlier, just inlined here so it is guaranteed to fire on
            // every touch instead of depending on that separate periodic sample's own
            // gate (which did not fire for this weapon at all in the last test).
            RE::NiPoint3 sourceVisualPos{};
            bool haveSourceVisual = false;
            const char* sourceVisualKind = "none";
            if (sourceIsWeapon) {
                if (auto* weaponNode = f4vr::getWeaponNode()) {
                    sourceVisualPos = weaponNode->world.translate;
                    haveSourceVisual = true;
                    sourceVisualKind = "weaponNode";
                }
            } else if (sourceHand) {
                const bool sourceIsLeftHand = (sourceHand == &_leftHand);
                sourceVisualPos = getInteractionHandTransform(sourceIsLeftHand).translate;
                haveSourceVisual = true;
                sourceVisualKind = sourceIsLeftHand ? "handTransform-left" : "handTransform-right";
            }

            // [TOUCH-DIAG] The comparison above (getInteractionHandTransform) came back
            // showing a ~10-13 unit gap in BOTH Diamond City and Red Rocket - proving
            // that transform is the wrong reference: it is ROCK's own internal
            // "root-flattened" abstraction (HandFrameResolver's own doc comment: "Scene
            // nodes from another tree are not returned as authority"), not the actual
            // rendered hand mesh the player sees. frik_visual_authority::getHandWorldTransform
            // is FRIK's real, live-rendered hand transform - the same API this file's own
            // sampleHandTransformParity() already uses for exactly this kind of
            // internal-vs-real comparison. This is the one that should actually answer
            // "is the collision body offset from the visible hand mesh."
            RE::NiPoint3 sourceFrikVisualPos{};
            bool haveSourceFrikVisual = false;
            if (!sourceIsWeapon && sourceHand && frik_visual_authority::isAvailable()) {
                const bool sourceIsLeftHand = (sourceHand == &_leftHand);
                sourceFrikVisualPos = frik_visual_authority::getHandWorldTransform(
                    sourceIsLeftHand ? frik_visual_authority::Hand::Left : frik_visual_authority::Hand::Right).translate;
                haveSourceFrikVisual = true;
            }

            auto dist3 = [](const RE::NiPoint3& a, const RE::NiPoint3& b) {
                const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };

            const float sourceTargetBodyDist = (haveSourceBody && haveTargetBody) ? dist3(sourceBodyWorld.translate, targetBodyWorld.translate) : -1.0f;
            const float targetBodyVsRefPosDist = haveTargetBody ? dist3(targetBodyWorld.translate, targetRefVisualPos) : -1.0f;
            const float targetBodyVsNodeDist = haveTargetBody ? dist3(targetBodyWorld.translate, targetNodeVisualPos) : -1.0f;
            const float sourceBodyVsVisualDist = (haveSourceBody && haveSourceVisual) ? dist3(sourceBodyWorld.translate, sourceVisualPos) : -1.0f;
            const float sourceBodyVsFrikDist = (haveSourceBody && haveSourceFrikVisual) ? dist3(sourceBodyWorld.translate, sourceFrikVisualPos) : -1.0f;

            auto* playerCell = RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetParentCell() : nullptr;
            const auto scaleSnapshot = physics_scale::current();

            ROCK_LOG_DEBUG(Hand,
                "[TOUCH-DIAG] {} sourceBody={} targetBody={} sourceIsWeapon={} | sourceBodyPos=({:.3f},{:.3f},{:.3f}) haveSourceBody={} | "
                "sourceVisualPos=({:.3f},{:.3f},{:.3f}) sourceVisualKind={} haveSourceVisual={} sourceBodyVsVisualDist={:.4f} | "
                "sourceFrikVisualPos=({:.3f},{:.3f},{:.3f}) haveSourceFrikVisual={} sourceBodyVsFrikDist={:.4f} | "
                "targetBodyPos=({:.3f},{:.3f},{:.3f}) haveTargetBody={} | targetRefVisualPos=({:.3f},{:.3f},{:.3f}) targetNodeVisualPos=({:.3f},{:.3f},{:.3f}) "
                "targetNodeName={} | sourceTargetBodyDist={:.4f} targetBodyVsRefPosDist={:.4f} targetBodyVsNodeDist={:.4f} | "
                "gameToHavok={:.8f} havokToGame={:.5f} scaleRevision={} | bhk={:p} hknp={:p} cellFormID={:08X} | playerPos=({:.3f},{:.3f},{:.3f})",
                sourceName,
                sourceBodyId,
                targetBodyId,
                sourceIsWeapon ? "yes" : "no",
                sourceBodyWorld.translate.x, sourceBodyWorld.translate.y, sourceBodyWorld.translate.z,
                haveSourceBody ? "yes" : "no",
                sourceVisualPos.x, sourceVisualPos.y, sourceVisualPos.z,
                sourceVisualKind,
                haveSourceVisual ? "yes" : "no",
                sourceBodyVsVisualDist,
                sourceFrikVisualPos.x, sourceFrikVisualPos.y, sourceFrikVisualPos.z,
                haveSourceFrikVisual ? "yes" : "no",
                sourceBodyVsFrikDist,
                targetBodyWorld.translate.x, targetBodyWorld.translate.y, targetBodyWorld.translate.z,
                haveTargetBody ? "yes" : "no",
                targetRefVisualPos.x, targetRefVisualPos.y, targetRefVisualPos.z,
                targetNodeVisualPos.x, targetNodeVisualPos.y, targetNodeVisualPos.z,
                targetNode ? targetNode->name.c_str() : "(none)",
                sourceTargetBodyDist,
                targetBodyVsRefPosDist,
                targetBodyVsNodeDist,
                scaleSnapshot.gameToHavok,
                scaleSnapshot.havokToGame,
                scaleSnapshot.revision,
                static_cast<void*>(bhk),
                static_cast<void*>(hknp),
                playerCell ? playerCell->GetFormID() : 0,
                RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetPosition().x : 0.0f,
                RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetPosition().y : 0.0f,
                RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetPosition().z : 0.0f);
        }

        // EMBED (Jul 18, catch/anti-tunneling): every dynamic body the hand/weapon touches
        // gets per-body CCD look-ahead ONCE, so an object resting on the open palm cannot
        // tunnel through the other (thin) hand colliders when it falls off — it was never
        // grabbed, so the release-path CCD never applied to it. Small ring de-dupes the
        // native call (hknpWorld::setBodyCollisionLookAheadDistance @0x14153B120; the vec
        // arg is only read when dist<=0).
        // EMBED (Jul 20, ordering fix): this block used to run BEFORE the resolveBodyToRef
        // validity check above, on a targetBodyId published by the physics-step contact
        // callback and consumed one game frame later - a hand brushing an object deleted
        // that same frame (grenade detonates, item consumed, cell unload) reaches this with
        // a freed body slot. Now gated on the same ref-liveness check every other native
        // body call in this file uses, plus an explicit bodySlotLooksReadable guard (the
        // one native call in this function that lacked one).
        if (havok_runtime::bodySlotLooksReadable(hknp, RE::hknpBodyId{ targetBodyId })) {
            static std::uint32_t s_ccdRing[16] = {};
            static std::uint32_t s_ccdRingNext = 0;
            bool seen = false;
            for (std::uint32_t id : s_ccdRing) {
                if (id == targetBodyId) { seen = true; break; }
            }
            if (!seen) {
                s_ccdRing[s_ccdRingNext++ & 15u] = targetBodyId;
                using SetLookAheadFn = void (*)(void*, std::uint32_t, float, const float*);
                static REL::Relocation<SetLookAheadFn> s_setLookAhead{ REL::Offset(0x153B120) };
                static const float kZeroVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                s_setLookAhead(hknp, targetBodyId, 0.25f, kZeroVec);
                ROCK_LOG_SAMPLE_DEBUG(Hand, g_rockConfig.rockLogSampleMilliseconds,
                    "{} touched body {}: CCD look-ahead enabled (anti-tunneling)", sourceName, targetBodyId);
            }
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
                "{} dynamic push skipped: target body {} not found in ref tree formID={:08X} visitedNodes={} collisionObjects={} maxDepth={}",
                sourceName,
                targetBodyId,
                targetRef->GetFormID(),
                bodySet.diagnostics.visitedNodes,
                bodySet.diagnostics.collisionObjects,
                scanOptions.maxDepth);
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
            // Previously a SILENT return - the only point in this whole function with
            // no log at all. Kept (throttled) so a source-side Havok motion lookup
            // failure never again goes untraced.
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push skipped: source body {} has no Havok motion (getBodyMotion failed)",
                sourceName,
                sourceBodyId);
            return;
        }

        RE::NiPoint3 sourceVelocityHavok{ sourceMotion->linearVelocity.x, sourceMotion->linearVelocity.y, sourceMotion->linearVelocity.z };
        // PRECISION FIX (Jul 6): for a HAND collider, use the clean KINEMATIC velocity the drive
        // sampled from the per-frame target deltas (sampledLinearVelocityHavok) instead of the live
        // keyframed SOLVER velocity read above. The solver velocity decays to ~0 the instant the hand
        // is blocked by the object you're pressing on — so a deliberate slow press reads below the
        // min-speed gate and never pushes (the "hand clips through, only sometimes pushes"). The
        // soft-contact/provider path already uses this sampled value; the dynamic-push path was the
        // odd one out. Weapon pushes (sourceHand==null) keep the live velocity (a swing is fast+clean).
        bool pushUsedSampledVelocity = false;
        if (sourceHand) {
            HandColliderBodyMetadata md{};
            // Jul 19 (workflow-verified): guard like the correct consumer at :811-813 AND make
            // the substitution max-of-two — the sampled bone-target velocity may only REPLACE
            // the live solver velocity when it reports MORE motion. An invalid/authority-pinned
            // sample (rendered hand held at a soft-contact plane reads ~0.01 regardless of
            // controller speed) can then never MASK real motion; the Jul-6 blocked-solver fix
            // (solver velocity decays to ~0 when pressing) is preserved by the max.
            if (sourceHand->tryGetHandColliderMetadata(sourceBodyId, md) &&
                md.hasSampledLinearVelocityHavok &&
                havok_runtime::isFinite3(md.sampledLinearVelocityHavok)) {
                const RE::NiPoint3 sampled{
                    md.sampledLinearVelocityHavok[0],
                    md.sampledLinearVelocityHavok[1],
                    md.sampledLinearVelocityHavok[2] };
                const float sampledSq = sampled.x * sampled.x + sampled.y * sampled.y + sampled.z * sampled.z;
                const float solverSq = sourceVelocityHavok.x * sourceVelocityHavok.x +
                    sourceVelocityHavok.y * sourceVelocityHavok.y +
                    sourceVelocityHavok.z * sourceVelocityHavok.z;
                if (sampledSq >= solverSq) {
                    sourceVelocityHavok = sampled;
                    pushUsedSampledVelocity = true;
                }
            }
        }

        // The assist is supplemental to Havok's real contact solver. Applying
        // the hand's raw XYZ velocity after a contact can add an impulse back
        // INTO a thin finger collider (the fast Subway Token repro logged a
        // -Z scripted impulse while the token was landing), so the native
        // solver then needs several frames to depenetrate it. Project the
        // assist onto the outward source-body -> contacted-body direction.
        // Tangential friction and all separating/away motion remain native;
        // the scripted assist can now only separate/push, never deepen overlap.
        const RE::NiPoint3 rawSourceVelocityHavok = sourceVelocityHavok;
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
                "{} dynamic push skipped: no trustworthy outward contact "
                "direction sourceBody={} targetBody={} haveSource={} separation={:.4f}",
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
        if (!std::isfinite(outwardSpeedHavok) || outwardSpeedHavok <= 0.0f) {
            /*
             * SOFT-LANDING DAMPER (Jul 27). Reaching here means the hand is NOT driving into the
             * object — so if the object is moving away from the hand it is BOUNCING off it. That
             * bounce is what makes a dropped prop hammer the fingers repeatedly, and penetration
             * depth tracks impact speed (measured: 39.9gu/s -> 1.14gu overlap, 11.6gu/s -> 0.11gu),
             * so each bounce buys another deep clip. Bleed off the separating radial velocity to
             * make the hand behave like a soft catch instead of a trampoline.
             *
             * Deliberately only on this branch: a hand actively pushing INTO an object takes the
             * normal push-assist path above and is completely unaffected, so throwing and shoving
             * keep their current feel. Hands only (a weapon hull should still knock things away).
             */
            const float restitutionDamping = (std::clamp)(
                g_rockConfig.rockHandContactRestitutionDamping, 0.0f, 1.0f);
            if (restitutionDamping > 0.0f && !sourceIsWeapon) {
                if (auto* bounceMotion = havok_runtime::getBodyMotion(
                        hknp, RE::hknpBodyId{ targetBodyId })) {
                    const float separatingSpeed =
                        bounceMotion->linearVelocity.x * outward.x +
                        bounceMotion->linearVelocity.y * outward.y +
                        bounceMotion->linearVelocity.z * outward.z;
                    if (std::isfinite(separatingSpeed) && separatingSpeed > 0.0f) {
                        const float bleed = separatingSpeed * restitutionDamping;
                        const RE::NiPoint3 dampingDelta{
                            -outward.x * bleed,
                            -outward.y * bleed,
                            -outward.z * bleed,
                        };
                        if (push_assist::applyLinearVelocityDeltaDeferred(
                                hknp, targetBodyId, dampingDelta)) {
                            ROCK_LOG_SAMPLE_DEBUG(
                                Hand,
                                g_rockConfig.rockLogSampleMilliseconds,
                                "{} soft-landing damper: bled {:.3f} of {:.3f} separating "
                                "speed off target {} (damping={:.2f})",
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
                "outward=({:.3f},{:.3f},{:.3f}) radialSpeed={:.3f}",
                sourceName,
                sourceBodyId,
                targetBodyId,
                rawSourceVelocityHavok.x,
                rawSourceVelocityHavok.y,
                rawSourceVelocityHavok.z,
                outward.x,
                outward.y,
                outward.z,
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
                "{} dynamic push skipped: reason={} radialSpeed=({:.3f},{:.3f},{:.3f}) "
                "rawSpeed=({:.3f},{:.3f},{:.3f}) sampled={} targetBody={} "
                "layer={} acceptedBodies={} separation={:.2f}gu",
                sourceName,
                pushAssistSkipReasonName(push.skipReason),
                sourceVelocityHavok.x,
                sourceVelocityHavok.y,
                sourceVelocityHavok.z,
                rawSourceVelocityHavok.x,
                rawSourceVelocityHavok.y,
                rawSourceVelocityHavok.z,
                pushUsedSampledVelocity ? "yes" : "no",
                targetBodyId,
                targetRecord->collisionLayer,
                bodySet.acceptedCount(),
                sourceToTargetLength);
            return;
        }

        std::uint32_t appliedCount = 0;
        RE::NiPoint3 lastAppliedImpulse = push.impulse;
        bool velocityClampEngaged = false;
        for (const auto* record : uniqueMotionRecords) {
            if (!record) {
                continue;
            }
            physics_recursive_wrappers::activateBody(hknp, record->bodyId);

            // The impulse above was sized from hand speed alone, with no idea what body it's
            // about to hit. impulse = mass * deltaV, so the identical impulse that feels right
            // on a normal-mass prop gives a light one (a coin, a token) a wildly larger velocity
            // kick. Scale it down per-body so the resulting deltaV never exceeds the configured
            // ceiling — heavier objects rarely reach it, so their feel is unchanged.
            RE::NiPoint3 impulseToApply = push.impulse;
            const float targetMass = readGrabEventBodyMass(hknp, record->bodyId);
            if (targetMass > 0.0f) {
                const float velocityDelta = push.impulseMagnitude / targetMass;
                const float maxVelocityDelta = (std::max)(0.0f, g_rockConfig.rockDynamicPushMaxVelocityDelta);
                if (maxVelocityDelta > 0.0f && velocityDelta > maxVelocityDelta) {
                    const float scale = maxVelocityDelta / velocityDelta;
                    impulseToApply.x *= scale;
                    impulseToApply.y *= scale;
                    impulseToApply.z *= scale;
                    velocityClampEngaged = true;
                }
            }

            if (push_assist::applyLinearImpulse(record->collisionObject, impulseToApply)) {
                ++appliedCount;
                lastAppliedImpulse = impulseToApply;
            }
        }

        if (appliedCount > 0) {
            _dynamicPushCooldownUntil[cooldownKey] =
                _dynamicPushElapsedSeconds + (std::max)(0.0f, g_rockConfig.rockDynamicPushCooldownSeconds);
            auto* baseObj = targetRef->GetObjectReference();
            auto objName = baseObj ? RE::TESFullName::GetFullName(*baseObj, false) : std::string_view{};
            const std::string nameStr = objName.empty() ? std::string("(unnamed)") : std::string(objName);
            // Log the impulse ACTUALLY applied (post velocity-delta clamp), plus whether the
            // clamp engaged — printing the pre-clamp value would send the next log-forensics
            // session chasing pushes that never landed at that magnitude.
            const float targetMass = readGrabEventBodyMass(hknp, targetBodyId);
            auto* targetMotion =
                havok_runtime::getBodyMotion(hknp, RE::hknpBodyId{ targetBodyId });
            const RE::NiPoint3 targetVelocity = targetMotion
                ? RE::NiPoint3{
                      targetMotion->linearVelocity.x,
                      targetMotion->linearVelocity.y,
                      targetMotion->linearVelocity.z }
                : RE::NiPoint3{};
            HandColliderBodyMetadata sourceMetadata{};
            const bool haveSourceMetadata =
                sourceHand &&
                sourceHand->tryGetHandColliderMetadata(
                    sourceBodyId,
                    sourceMetadata);
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} dynamic push applied: '{}' formID={:08X} targetBody={} "
                "layer={} acceptedBodies={} uniqueMotions={} "
                "impulse=({:.3f},{:.3f},{:.3f}) velClamped={} "
                "rawHandVel=({:.3f},{:.3f},{:.3f}) "
                "radialHandVel=({:.3f},{:.3f},{:.3f}) "
                "outward=({:.3f},{:.3f},{:.3f}) separation={:.2f}gu "
                "targetMass={:.4f} targetVel=({:.3f},{:.3f},{:.3f}) "
                "sourceRole={} sourceFinger={} sourceSegment={}",
                sourceName,
                nameStr,
                targetRef->GetFormID(),
                targetBodyId,
                targetRecord->collisionLayer,
                bodySet.acceptedCount(),
                appliedCount,
                lastAppliedImpulse.x,
                lastAppliedImpulse.y,
                lastAppliedImpulse.z,
                velocityClampEngaged ? "yes" : "no",
                rawSourceVelocityHavok.x,
                rawSourceVelocityHavok.y,
                rawSourceVelocityHavok.z,
                sourceVelocityHavok.x,
                sourceVelocityHavok.y,
                sourceVelocityHavok.z,
                outward.x,
                outward.y,
                outward.z,
                sourceToTargetLength,
                targetMass,
                targetVelocity.x,
                targetVelocity.y,
                targetVelocity.z,
                haveSourceMetadata
                    ? static_cast<std::uint32_t>(sourceMetadata.role)
                    : 0xFFFF'FFFFu,
                haveSourceMetadata
                    ? static_cast<std::uint32_t>(sourceMetadata.finger)
                    : 0xFFFF'FFFFu,
                haveSourceMetadata
                    ? static_cast<std::uint32_t>(sourceMetadata.segment)
                    : 0xFFFF'FFFFu);
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

            ROCK_LOG_SAMPLE_DEBUG(
                Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} hand touched [{}] '{}' formID={:08X} body={} layer={}",
                handName,
                typeName,
                nameStr,
                ref->GetFormID(),
                bodyId.value,
                layer);

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
            ROCK_LOG_SAMPLE_DEBUG(
                Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "{} hand touched body={} layer={} (unresolved)",
                handName,
                bodyId.value,
                layer);
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
            // This function is polled from the frame loop. Logging the steady
            // already-subscribed state every frame buried the contact evidence
            // needed for real collision diagnostics; the actual subscription
            // and world-transition paths below remain event-logged.
            return;
        }

        if (plan.replaceExistingRuntimeStateWithoutUnsubscribe) {
            ROCK_LOG_INFO(
                Init,
                "Replacing contact event bridge state without native unsubscribe (action={})",
                static_cast<std::uint32_t>(plan.action));
        }

        // Native FUN_14040ca60 is a 2-arg hkSignal global subscribe: (signal, callback). It stores
        // `callback` directly in the slot; the dispatcher later calls callback(worldPtr, eventPtr) where
        // worldPtr is hknpWorld** (proven src/ContactImpulseListener.cpp:439 + ParseEvent:246, which use
        // this exact function and a 2-arg callback that FIRES in-game). The prior 3-arg call passed
        // &s_contactEventBridge as `callback` (arg2) — a DATA pointer — so the slot's callback became
        // static data; when a contact fired, the dispatcher jumped into the logger's s_mutex region and
        // hit an EXECUTE access violation (crash 2026-07-04 06-41-49). Match the proven ABI exactly.
        typedef void subscribe_simple_t(void* signal, void* callback);
        static REL::Relocation<subscribe_simple_t> subscribeSimple{ REL::Offset(offsets::kFunc_SubscribeContactEvent) };
        subscribeSimple(signal, reinterpret_cast<void*>(&PhysicsInteraction::onContactCallback));

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

    void PhysicsInteraction::onContactCallback(void** worldPtrHolder, void* contactEventData)
    {
        performance_profiler::addEventCount(performance_profiler::Scope::NativeContactCallback);
        onContactCallbackSeh(worldPtrHolder, contactEventData);
    }

    void PhysicsInteraction::onContactCallbackSeh(void** worldPtrHolder, void* contactEventData)
    {
        // Counted OUTSIDE the __try (a destructor-bearing RAII guard inside a __try whose
        // __except can fire would leak the count on unwind - __except skips destructors).
        // onContactCallbackUnsafe's own early returns are fine here: they return from that
        // function back into this one, still before reaching the fetch_sub below.
        PhysicsInteraction::s_inFlightCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try {
            onContactCallbackUnsafe(worldPtrHolder, contactEventData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            onContactCallbackException();
        }
        PhysicsInteraction::s_inFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void PhysicsInteraction::onContactCallbackUnsafe(void** worldPtrHolder, void* contactEventData)
    {
        if (!s_hooksEnabled.load(std::memory_order_acquire))
            return;

        // Native 2-arg hkSignal ABI: the slot invokes callback(worldPtr, eventPtr) with NO userData
        // argument (subscribe stores only the raw callback). The subscription bridge is the fixed
        // process-wide static, so derive it directly rather than expecting it as a (nonexistent) arg.
        void* userData = static_cast<void*>(&s_contactEventBridge);
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
        static int sehLogCounter = 0;
        if (sehLogCounter++ % 100 == 0) {
            logger::error(
                "[ROCK::Contact] SEH exception caught on physics thread (count={}) — "
                "likely stale world during cell transition",
                sehLogCounter);
        }
        s_hooksEnabled.store(false, std::memory_order_release);
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

        // [CONTACT-DIAG] Registry::tryClassify is a pure atomic-array lookup (see
        // GeneratedBodyContactRegistry.h) with no Havok dependency, so it is safe to
        // call ahead of the body-slot readability gate below. Used only to decide
        // whether this raw event involves a hand or the equipped weapon, so every
        // stage a hand/weapon+object pair passes through (or is dropped by) can be
        // traced end to end - e.g. a hand or weapon that clips through a
        // just-released/thrown object with no push ever applied. Throttled
        // (SAMPLE_DEBUG) now that the scale-mismatch investigation this was built
        // for is closed - see PhysicsScale.h.
        generated_body_contact_registry::Classification diagClassA{};
        generated_body_contact_registry::Classification diagClassB{};
        const bool diagBodyAClassified = _generatedBodyContactRegistry.tryClassify(bodyIdA, diagClassA);
        const bool diagBodyBClassified = _generatedBodyContactRegistry.tryClassify(bodyIdB, diagClassB);
        const bool diagBodyAIsHand = diagBodyAClassified &&
            (diagClassA.kind == generated_body_contact_registry::GeneratedBodyKind::RightHand ||
                diagClassA.kind == generated_body_contact_registry::GeneratedBodyKind::LeftHand);
        const bool diagBodyBIsHand = diagBodyBClassified &&
            (diagClassB.kind == generated_body_contact_registry::GeneratedBodyKind::RightHand ||
                diagClassB.kind == generated_body_contact_registry::GeneratedBodyKind::LeftHand);
        const bool diagBodyAIsWeapon = diagBodyAClassified && diagClassA.kind == generated_body_contact_registry::GeneratedBodyKind::Weapon;
        const bool diagBodyBIsWeapon = diagBodyBClassified && diagClassB.kind == generated_body_contact_registry::GeneratedBodyKind::Weapon;
        const bool diagBodyAIsSource = diagBodyAIsHand || diagBodyAIsWeapon;
        const bool diagBodyBIsSource = diagBodyBIsHand || diagBodyBIsWeapon;
        const bool diagInvolvesSource = diagBodyAIsSource || diagBodyBIsSource;
        const std::uint32_t diagSourceBodyId = diagBodyAIsSource ? bodyIdA : bodyIdB;
        const std::uint32_t diagOtherBodyId = diagBodyAIsSource ? bodyIdB : bodyIdA;
        const char* diagSourceKindName = diagBodyAIsSource
            ? (diagBodyAIsHand ? "Hand" : "Weapon")
            : (diagBodyBIsHand ? "Hand" : "Weapon");
        if (diagInvolvesSource) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "[CONTACT-DIAG] native event seen: sourceKind={} sourceBody={} otherBody={}",
                diagSourceKindName,
                diagSourceBodyId,
                diagOtherBodyId);
        }

        if (!havok_runtime::bodySlotLooksReadable(world, RE::hknpBodyId{ bodyIdA }) ||
            !havok_runtime::bodySlotLooksReadable(world, RE::hknpBodyId{ bodyIdB })) {
            if (diagInvolvesSource) {
                ROCK_LOG_SAMPLE_DEBUG(Hand,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "[CONTACT-DIAG] native event DROPPED (body slot unreadable): sourceKind={} sourceBody={} otherBody={} aReadable={} bReadable={}",
                    diagSourceKindName,
                    diagSourceBodyId,
                    diagOtherBodyId,
                    havok_runtime::bodySlotLooksReadable(world, RE::hknpBodyId{ bodyIdA }) ? "yes" : "no",
                    havok_runtime::bodySlotLooksReadable(world, RE::hknpBodyId{ bodyIdB }) ? "yes" : "no");
            }
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

        const auto rightId = _rightHand.getCollisionBodyId().value;
        const auto leftId = _leftHand.getCollisionBodyId().value;

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
        const bool bodyAIsExternal = ::rock::provider::isExternalBodyId(bodyIdA);
        const bool bodyBIsExternal = ::rock::provider::isExternalBodyId(bodyIdB);
        const bool bodyAIsRightHeld = _rightHand.isHeldBodyId(bodyIdA);
        const bool bodyBIsRightHeld = _rightHand.isHeldBodyId(bodyIdB);
        const bool bodyAIsLeftHeld = _leftHand.isHeldBodyId(bodyIdA);
        const bool bodyBIsLeftHeld = _leftHand.isHeldBodyId(bodyIdB);
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
            if (diagInvolvesSource) {
                ROCK_LOG_SAMPLE_DEBUG(Hand,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "[CONTACT-DIAG] native event DROPPED (prefilter): sourceKind={} sourceBody={} otherBody={} aIsRockSource={} bIsRockSource={}",
                    diagSourceKindName,
                    diagSourceBodyId,
                    diagOtherBodyId,
                    bodyAIsRockSource ? "yes" : "no",
                    bodyBIsRockSource ? "yes" : "no");
            }
            return;
        }

        auto readBodyFilterInfo = [world](std::uint32_t bodyId) {
            std::uint32_t filterInfo = 0;
            if (world && havok_runtime::tryReadFilterInfo(world, RE::hknpBodyId{ bodyId }, filterInfo)) {
                return filterInfo;
            }
            return contact_evidence::kUnknownFilterInfo;
        };

        auto filterInfoToLayer = [](std::uint32_t filterInfo) {
            return filterInfo == contact_evidence::kUnknownFilterInfo ? contact_pipeline_policy::kUnknownLayer : (filterInfo & 0x7Fu);
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

        if (diagInvolvesSource) {
            auto diagEndpointKindName = [](contact_pipeline_policy::ContactEndpointKind kind) -> const char* {
                using Kind = contact_pipeline_policy::ContactEndpointKind;
                switch (kind) {
                case Kind::Unknown: return "Unknown";
                case Kind::RightHand: return "RightHand";
                case Kind::LeftHand: return "LeftHand";
                case Kind::Weapon: return "Weapon";
                case Kind::RightHeldObject: return "RightHeldObject";
                case Kind::LeftHeldObject: return "LeftHeldObject";
                case Kind::Body: return "Body";
                case Kind::External: return "External";
                case Kind::WorldSurface: return "WorldSurface";
                case Kind::DynamicProp: return "DynamicProp";
                case Kind::Actor: return "Actor";
                case Kind::QueryOnly: return "QueryOnly";
                default: return "?";
                }
            };
            const auto& diagOtherEndpoint = diagBodyAIsSource ? endpointB : endpointA;
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "[CONTACT-DIAG] native event ROUTED: sourceKind={} sourceBody={} otherBody={} otherKind={} otherLayer={} route={} driveHandPush={} driveWeaponPush={} recordSemantic={} drivesWeaponSupport={}",
                diagSourceKindName,
                diagSourceBodyId,
                diagOtherBodyId,
                diagEndpointKindName(diagOtherEndpoint.kind),
                diagOtherEndpoint.layer == contact_pipeline_policy::kUnknownLayer ? 0xFFFFFFFFu : diagOtherEndpoint.layer,
                contact_pipeline_policy::routeName(contactRoute.route),
                contactRoute.driveHandDynamicPush ? "yes" : "no",
                contactRoute.driveWeaponDynamicPush ? "yes" : "no",
                contactRoute.recordHandSemanticContact ? "yes" : "no",
                contactRoute.drivesWeaponSupportContact ? "yes" : "no");
        }

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
                                      ::rock::provider::RockProviderExternalContactV1& contact) {
            if (handMetadata && handMetadata->valid && handMetadata->hasSampledLinearVelocityHavok &&
                havok_runtime::isFinite3(handMetadata->sampledLinearVelocityHavok)) {
                std::copy_n(handMetadata->sampledLinearVelocityHavok, 4, contact.sourceVelocityHavok);
                return;
            }

            if (sourceKind == ::rock::provider::RockProviderExternalSourceKind::Weapon) {
                const auto* weaponSource = weaponSourceFor(sourceBodyId);
                if (weaponSource && weaponSource->valid && weaponSource->hasSampledVelocity &&
                    havok_runtime::isFinite3(weaponSource->sampledVelocityHavok)) {
                    std::copy_n(weaponSource->sampledVelocityHavok, 4, contact.sourceVelocityHavok);
                    return;
                }
            }

            if (!world || sourceBodyId == INVALID_CONTACT_BODY_ID) {
                return;
            }

            auto* motion = havok_runtime::getBodyMotion(world, RE::hknpBodyId{ sourceBodyId });
            if (!motion) {
                return;
            }

            contact.sourceVelocityHavok[0] = motion->linearVelocity.x;
            contact.sourceVelocityHavok[1] = motion->linearVelocity.y;
            contact.sourceVelocityHavok[2] = motion->linearVelocity.z;
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
            }

            contact.contactPointWeightSum = 0.0f;
            contact.quality = ::rock::provider::RockProviderExternalContactQuality::AggregateImpulse;
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
            fillSourceVelocity(sourceBodyId, sourceKind, handMetadata, contact);

            if (ensureRawContactPoint()) {
                contact.quality = ::rock::provider::RockProviderExternalContactQuality::RawPoint;
                contact.contactPointWeightSum = rawContactPoint.contactPointWeightSum;
                std::copy_n(rawContactPoint.contactPointHavok, 4, contact.contactPointHavok);
                std::copy_n(rawContactPoint.contactNormalHavok, 4, contact.contactNormalHavok);
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

            ::rock::provider::recordExternalContact(contact);
        };

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
                !contact_pipeline_policy::isHand(contactRoute.source.kind) ||
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
                    .otherIsExternalProvider = ::rock::provider::isExternalBodyId(other),
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
        // [CONTACT-DIAG] Local, file-scope-only name lookup - HandState's real name
        // helper (Hand.cpp's handStateName) is anonymous-namespace-private to that
        // translation unit, so this mirrors it rather than exposing it, purely to
        // make the unthrottled suppression logs below self-explanatory.
        auto diagStateName = [](HandState state) -> const char* {
            switch (state) {
            case HandState::Idle: return "Idle";
            case HandState::SelectedClose: return "SelectedClose";
            case HandState::SelectedFar: return "SelectedFar";
            case HandState::SelectionLocked: return "SelectionLocked";
            case HandState::PreGrabItem: return "PreGrabItem";
            case HandState::PrePullItem: return "PrePullItem";
            case HandState::HeldInit: return "HeldInit";
            case HandState::HeldBody: return "HeldBody";
            case HandState::Pulled: return "Pulled";
            case HandState::GrabFromOtherHand: return "GrabFromOtherHand";
            case HandState::GrabExternal: return "GrabExternal";
            case HandState::LootOtherHand: return "LootOtherHand";
            case HandState::SelectedTwoHand: return "SelectedTwoHand";
            case HandState::HeldTwoHanded: return "HeldTwoHanded";
            case HandState::StashCandidate: return "StashCandidate";
            case HandState::ConsumeCandidate: return "ConsumeCandidate";
            default: return "?";
            }
        };

        const bool rightDominantWeaponSuppressed = _rightDominantWeaponCollisionSuppressed.load(std::memory_order_acquire);
        const bool rightHandStateSuppressed = _rightHand.hasContactEvidenceSuppressedAtomic();
        const bool leftWeaponSupportSuppressed = _leftWeaponSupportCollisionSuppressed.load(std::memory_order_acquire);
        const bool leftHandStateSuppressed = _leftHand.hasContactEvidenceSuppressedAtomic();
        const bool rightBodyPairSuppressed = (bodyAIsRight || bodyBIsRight) && (rightDominantWeaponSuppressed || rightHandStateSuppressed);
        const bool leftBodyPairSuppressed = (bodyAIsLeft || bodyBIsLeft) && (leftWeaponSupportSuppressed || leftHandStateSuppressed);
        if (rightBodyPairSuppressed || leftBodyPairSuppressed) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "[CONTACT-DIAG] Suppressed hand body contact skipped: route={} rightSuppressed={} (dominantWeapon={} state={}/{}) leftSuppressed={} (weaponSupport={} state={}/{}) bodyA={} bodyB={}",
                contact_pipeline_policy::routeName(contactRoute.route),
                rightBodyPairSuppressed ? "yes" : "no",
                rightDominantWeaponSuppressed ? "yes" : "no",
                rightHandStateSuppressed ? "yes" : "no",
                diagStateName(_rightHand.getStateAtomic()),
                leftBodyPairSuppressed ? "yes" : "no",
                leftWeaponSupportSuppressed ? "yes" : "no",
                leftHandStateSuppressed ? "yes" : "no",
                diagStateName(_leftHand.getStateAtomic()),
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
            const bool sourceIsLeft = contact_pipeline_policy::isLeftHand(contactRoute.source.kind);
            const auto& sourceHandForLog = sourceIsLeft ? _leftHand : _rightHand;
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                g_rockConfig.rockLogSampleMilliseconds,
                "[CONTACT-DIAG] Contact evidence skipped for stronger hand owner: route={} sourceBody={} targetBody={} sourceIsLeft={} state={} dominantWeaponSuppressed={} supportSuppressed={} stateSuppressed={}",
                contact_pipeline_policy::routeName(contactRoute.route),
                contactRoute.sourceBodyId,
                contactRoute.targetBodyId,
                sourceIsLeft ? "yes" : "no",
                diagStateName(sourceHandForLog.getStateAtomic()),
                (!sourceIsLeft && rightDominantWeaponSuppressed) ? "yes" : "no",
                (!sourceIsLeft ? _rightWeaponSupportCollisionSuppressed.load(std::memory_order_acquire) : leftWeaponSupportSuppressed) ? "yes" : "no",
                sourceHandForLog.hasContactEvidenceSuppressedAtomic() ? "yes" : "no");
            return;
        }

        bool isRight = bodyAIsRight || bodyBIsRight;
        bool isLeft = bodyAIsLeft || bodyBIsLeft;
        const HandColliderBodyMetadata* routeHandMetadata = nullptr;
        if (contact_pipeline_policy::isHand(contactRoute.source.kind)) {
            const auto* handSource = handSourceFor(contactRoute.sourceBodyId);
            routeHandMetadata = handSource && handSource->valid ? &handSource->metadata : nullptr;
        }

        publishNativeContactEvidence(routeHandMetadata);

        if (contactRoute.publishExternalContact) {
            publishExternalContact(contactRoute.sourceBodyId, contactRoute.targetBodyId, contactRoute.providerSourceKind, contactRoute.providerSourceHand, routeHandMetadata);
        }

        if (contactRoute.driveWeaponDynamicPush) {
            _contactSlotWeapon.record(contactRoute.sourceBodyId, contactRoute.targetBodyId);
        }

        if (contactRoute.recordWorldSurfaceEvidence) {
            int logCount = _contactLogCounter.fetch_add(1, std::memory_order_relaxed);
            if (logCount % 60 == 0) {
                ROCK_LOG_DEBUG(Hand,
                    "Surface contact evidence: route={} sourceBody={} targetBody={} targetLayer={}",
                    contact_pipeline_policy::routeName(contactRoute.route),
                    contactRoute.sourceBodyId,
                    contactRoute.targetBodyId,
                    contactRoute.target.layer == contact_pipeline_policy::kUnknownLayer ? 0xFFFFFFFFu : contactRoute.target.layer);
            }
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

        /*
         * A reused hknp contact pair does not always expose a fresh raw
         * manifold point.  Speed and the main-thread collider/visible-mesh
         * envelope check do not depend on that point, so always publish the
         * record and mark the raw fields explicitly.  The previous all-or-
         * nothing gate is why fast token impacts produced native contact logs
         * but no [CONTACT-PENETRATION] evidence.
         */
        const bool hasRawManifoldPoint = ensureRawContactPoint();
        const float scale = havokToGameScale();
        float sourceSpeedGamePerSecond = 0.0f;
        if (handSource->metadata.hasSampledLinearVelocityHavok) {
            const auto* velocity =
                handSource->metadata.sampledLinearVelocityHavok;
            const float speedSquared =
                velocity[0] * velocity[0] +
                velocity[1] * velocity[1] +
                velocity[2] * velocity[2];
            if (std::isfinite(speedSquared) &&
                speedSquared > 0.0f) {
                sourceSpeedGamePerSecond =
                    std::sqrt(speedSquared) * scale;
            }
        }
        float targetSpeedGamePerSecond = 0.0f;
        if (auto* targetMotion = havok_runtime::getBodyMotion(
                world,
                RE::hknpBodyId{ contactRoute.targetBodyId })) {
            const float speedSquared =
                targetMotion->linearVelocity.x *
                    targetMotion->linearVelocity.x +
                targetMotion->linearVelocity.y *
                    targetMotion->linearVelocity.y +
                targetMotion->linearVelocity.z *
                    targetMotion->linearVelocity.z;
            if (std::isfinite(speedSquared) &&
                speedSquared > 0.0f) {
                targetSpeedGamePerSecond =
                    std::sqrt(speedSquared) * scale;
            }
        }

        {
            std::scoped_lock lock(
                _contactPenetrationDiagnosticMutex);
            auto& diagnostic =
                _contactPenetrationDiagnosticRecords[
                    _nextContactPenetrationDiagnosticSlot++ %
                    _contactPenetrationDiagnosticRecords.size()];
            diagnostic = {};
            diagnostic.valid = true;
            diagnostic.isLeft = handSource->isLeft;
            diagnostic.hasRawManifoldPoint = hasRawManifoldPoint;
            diagnostic.sequence =
                ++_contactPenetrationDiagnosticSequence;
            diagnostic.sourceBodyId =
                contactRoute.sourceBodyId;
            diagnostic.targetBodyId =
                contactRoute.targetBodyId;
            diagnostic.role = handSource->metadata.role;
            diagnostic.finger = handSource->metadata.finger;
            diagnostic.segment = handSource->metadata.segment;
            // Captured on EVERY record, success or failure: when extraction fails the caller
            // otherwise sees only `false` and all the manifold fields below stay at their
            // zero-initialised defaults, which is indistinguishable from "measured zero".
            diagnostic.rawInlineCount = rawContactPoint.rawInlineCount;
            diagnostic.extractFailStage =
                static_cast<std::uint8_t>(rawContactPoint.failStage);
            diagnostic.rawNormalFinite = rawContactPoint.normalFinite;
            if (hasRawManifoldPoint) {
                diagnostic.manifoldContactCount =
                    rawContactPoint.manifoldContactCount;
                diagnostic.validContactPointCount =
                    rawContactPoint.validContactPointCount;
                diagnostic.contactPointGame = RE::NiPoint3{
                    rawContactPoint.contactPointHavok[0] * scale,
                    rawContactPoint.contactPointHavok[1] * scale,
                    rawContactPoint.contactPointHavok[2] * scale,
                };
                diagnostic.contactNormalGame = RE::NiPoint3{
                    rawContactPoint.contactNormalHavok[0],
                    rawContactPoint.contactNormalHavok[1],
                    rawContactPoint.contactNormalHavok[2],
                };
                diagnostic.averageSignedSeparationGame =
                    rawContactPoint.contactPointHavok[3] * scale;
                diagnostic.minimumSignedSeparationGame =
                    rawContactPoint.minimumSignedSeparationHavok * scale;
                diagnostic.maximumSignedSeparationGame =
                    rawContactPoint.maximumSignedSeparationHavok * scale;
            }
            diagnostic.sourceSpeedGamePerSecond =
                sourceSpeedGamePerSecond;
            diagnostic.targetSpeedGamePerSecond =
                targetSpeedGamePerSecond;
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
                _contactSlotLeft.record(handSource->metadata.bodyId, contactRoute.targetBodyId);
            }
        } else {
            _rightHand.recordSemanticContact(handSource->metadata, contactRoute.targetBodyId, semanticContactPoint, semanticContactNormal);
            if (contactRoute.driveHandDynamicPush) {
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
