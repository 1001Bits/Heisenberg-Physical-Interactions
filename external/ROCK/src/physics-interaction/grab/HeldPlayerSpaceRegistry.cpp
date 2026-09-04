#include "physics-interaction/grab/HeldPlayerSpaceRegistry.h"

#include "physics-interaction/native/HavokRuntime.h"
#include "physics-interaction/grab/GrabHeldObject.h"
#include "physics-interaction/native/PhysicsUtils.h"

#include "RE/Havok/hknpMotion.h"
#include "RE/Havok/hknpWorld.h"

namespace rock::held_player_space_registry
{
    namespace
    {
        PureVec3 toPure(const RE::NiPoint3& value) { return PureVec3{ .x = value.x, .y = value.y, .z = value.z }; }

        PureVec3 scaled(PureVec3 value, float scale)
        {
            return PureVec3{ .x = value.x * scale, .y = value.y * scale, .z = value.z * scale };
        }

        PureVec3 removePreviousPlayerVelocity(PureVec3 currentVelocity, PureVec3 previousPlayerVelocity)
        {
            return PureVec3{
                .x = currentVelocity.x - previousPlayerVelocity.x,
                .y = currentVelocity.y - previousPlayerVelocity.y,
                .z = currentVelocity.z - previousPlayerVelocity.z,
            };
        }

        bool setBodyVelocityDeferred(RE::hknpWorld* world, std::uint32_t bodyId, const PureVec3& linear, const PureVec3& angular)
        {
            return havok_runtime::setBodyVelocityDeferred(
                world,
                bodyId,
                RE::hkVector4f{ linear.x, linear.y, linear.z, 0.0f },
                RE::hkVector4f{ angular.x, angular.y, angular.z, 0.0f });
        }

        bool setBodyTransformDeferred(RE::hknpWorld* world, std::uint32_t bodyId, const RE::NiTransform& transform)
        {
            return havok_runtime::setBodyTransformDeferred(world, bodyId, transform, 1);
        }

        struct WarpBodyPlan
        {
            std::uint32_t bodyId = kInvalidBodyId;
            std::uint32_t motionIndex = 0;
            RE::NiTransform originalWorld{};
            RE::NiTransform warpedWorld{};
            bool proxyBody = false;
        };

        struct WarpMotionPlan
        {
            std::uint32_t motionIndex = 0;
            std::uint32_t representativeBodyId = kInvalidBodyId;
            PureVec3 originalLinear{};
            PureVec3 originalAngular{};
            PureVec3 warpedLinear{};
            PureVec3 warpedAngular{};
            bool preserveFullVelocity = false;
        };

        RuntimeHeldPlayerSpaceResult applyWarpTransaction(
            RE::hknpWorld* world,
            const std::vector<std::uint32_t>& heldBodyIds,
            const std::vector<std::uint32_t>* proxyBodyIds,
            const RE::NiPoint3& previousPlayerVelocityHavok,
            const float residualVelocityKeep,
            const bool writeVelocity,
            const RE::NiTransform* previousPlayerSpaceWorld,
            const RE::NiTransform* currentPlayerSpaceWorld)
        {
            RuntimeHeldPlayerSpaceResult result{};
            struct RequiredBody
            {
                std::uint32_t bodyId = kInvalidBodyId;
                bool proxyBody = false;
            };
            std::vector<RequiredBody> requiredBodies;
            requiredBodies.reserve(
                heldBodyIds.size() +
                (proxyBodyIds ? proxyBodyIds->size() : 0));
            const auto appendRequired = [&](const std::uint32_t bodyId,
                                            const bool proxyBody) {
                if (bodyId == kInvalidBodyId && !proxyBody) {
                    return;
                }
                const auto found = std::find_if(
                    requiredBodies.begin(),
                    requiredBodies.end(),
                    [&](const RequiredBody& entry) {
                        return entry.bodyId == bodyId;
                    });
                if (found != requiredBodies.end()) {
                    found->proxyBody = found->proxyBody || proxyBody;
                    return;
                }
                requiredBodies.push_back(RequiredBody{
                    .bodyId = bodyId,
                    .proxyBody = proxyBody,
                });
            };
            for (const auto bodyId : heldBodyIds) {
                appendRequired(bodyId, false);
            }
            if (proxyBodyIds) {
                for (const auto bodyId : *proxyBodyIds) {
                    appendRequired(bodyId, true);
                }
            }

            result.warpTransaction.requiredTransformWrites =
                requiredBodies.size();
            if (!previousPlayerSpaceWorld || !currentPlayerSpaceWorld ||
                requiredBodies.empty()) {
                return result;
            }

            std::vector<WarpBodyPlan> bodyPlans;
            std::vector<WarpMotionPlan> motionPlans;
            bodyPlans.reserve(requiredBodies.size());
            motionPlans.reserve(requiredBodies.size());
            bool preflightComplete = true;
            for (const auto& required : requiredBodies) {
                auto* body = havok_runtime::getBody(
                    world,
                    RE::hknpBodyId{ required.bodyId });
                if (!body || !body_frame::hasUsableMotionIndex(
                                 body->motionIndex)) {
                    preflightComplete = false;
                    break;
                }
                auto* motion = havok_runtime::getMotion(
                    world,
                    body->motionIndex);
                RE::NiTransform bodyWorld{};
                if (!motion || !tryGetBodyWorldTransform(
                                   world,
                                   RE::hknpBodyId{ required.bodyId },
                                   bodyWorld)) {
                    preflightComplete = false;
                    break;
                }

                bodyPlans.push_back(WarpBodyPlan{
                    .bodyId = required.bodyId,
                    .motionIndex = body->motionIndex,
                    .originalWorld = bodyWorld,
                    .warpedWorld =
                        held_player_space_math::
                            warpBodyWorldThroughPlayerSpace(
                                *previousPlayerSpaceWorld,
                                *currentPlayerSpaceWorld,
                                bodyWorld),
                    .proxyBody = required.proxyBody,
                });

                const auto existingMotion = std::find_if(
                    motionPlans.begin(),
                    motionPlans.end(),
                    [&](const WarpMotionPlan& entry) {
                        return entry.motionIndex == body->motionIndex;
                    });
                if (existingMotion != motionPlans.end()) {
                    existingMotion->preserveFullVelocity =
                        existingMotion->preserveFullVelocity ||
                        required.proxyBody;
                    ++result.duplicateMotionSkips;
                    continue;
                }
                motionPlans.push_back(WarpMotionPlan{
                    .motionIndex = body->motionIndex,
                    .representativeBodyId = required.bodyId,
                    .originalLinear = PureVec3{
                        .x = motion->linearVelocity.x,
                        .y = motion->linearVelocity.y,
                        .z = motion->linearVelocity.z,
                    },
                    .originalAngular = PureVec3{
                        .x = motion->angularVelocity.x,
                        .y = motion->angularVelocity.y,
                        .z = motion->angularVelocity.z,
                    },
                    .preserveFullVelocity = required.proxyBody,
                });
            }

            result.registeredBodies =
                static_cast<std::uint32_t>(bodyPlans.size());
            result.warpTransaction.requiredVelocityWrites =
                motionPlans.size();
            result.warpTransaction.preflightComplete =
                preflightComplete &&
                bodyPlans.size() == requiredBodies.size() &&
                !motionPlans.empty();
            if (!result.warpTransaction.preflightComplete) {
                return result;
            }

            const PureVec3 previousPlayerVelocity =
                toPure(previousPlayerVelocityHavok);
            for (auto& motionPlan : motionPlans) {
                const PureVec3 localLinear =
                    writeVelocity &&
                            !motionPlan.preserveFullVelocity ?
                        scaled(
                            removePreviousPlayerVelocity(
                                motionPlan.originalLinear,
                                previousPlayerVelocity),
                            residualVelocityKeep) :
                        motionPlan.originalLinear;
                const PureVec3 localAngular =
                    writeVelocity &&
                            !motionPlan.preserveFullVelocity ?
                        scaled(
                            motionPlan.originalAngular,
                            residualVelocityKeep) :
                        motionPlan.originalAngular;
                motionPlan.warpedLinear =
                    held_player_space_math::
                        reorientWorldVectorThroughPlayerSpace(
                            *previousPlayerSpaceWorld,
                            *currentPlayerSpaceWorld,
                            localLinear);
                motionPlan.warpedAngular =
                    held_player_space_math::
                        reorientWorldVectorThroughPlayerSpace(
                            *previousPlayerSpaceWorld,
                            *currentPlayerSpaceWorld,
                            localAngular);
            }

            for (const auto& bodyPlan : bodyPlans) {
                if (!setBodyTransformDeferred(
                        world,
                        bodyPlan.bodyId,
                        bodyPlan.warpedWorld)) {
                    break;
                }
                ++result.warpTransaction.queuedTransformWrites;
            }
            if (result.warpTransaction.queuedTransformWrites !=
                result.warpTransaction.requiredTransformWrites) {
                for (std::size_t index = 0;
                     index < result.warpTransaction.queuedTransformWrites;
                     ++index) {
                    if (setBodyTransformDeferred(
                            world,
                            bodyPlans[index].bodyId,
                            bodyPlans[index].originalWorld)) {
                        ++result.warpTransaction.
                            rollbackTransformWritesQueued;
                    }
                }
                return result;
            }

            for (const auto& motionPlan : motionPlans) {
                if (!setBodyVelocityDeferred(
                        world,
                        motionPlan.representativeBodyId,
                        motionPlan.warpedLinear,
                        motionPlan.warpedAngular)) {
                    break;
                }
                ++result.warpTransaction.queuedVelocityWrites;
            }
            if (result.warpTransaction.queuedVelocityWrites !=
                result.warpTransaction.requiredVelocityWrites) {
                for (std::size_t index = 0;
                     index < result.warpTransaction.queuedVelocityWrites;
                     ++index) {
                    if (setBodyVelocityDeferred(
                            world,
                            motionPlans[index].representativeBodyId,
                            motionPlans[index].originalLinear,
                            motionPlans[index].originalAngular)) {
                        ++result.warpTransaction.
                            rollbackVelocityWritesQueued;
                    }
                }
                for (std::size_t index = 0;
                     index < result.warpTransaction.queuedTransformWrites;
                     ++index) {
                    if (setBodyTransformDeferred(
                            world,
                            bodyPlans[index].bodyId,
                            bodyPlans[index].originalWorld)) {
                        ++result.warpTransaction.
                            rollbackTransformWritesQueued;
                    }
                }
                return result;
            }

            result.transformsWarped = static_cast<std::uint32_t>(
                bodyPlans.size());
            result.motionsWritten = static_cast<std::uint32_t>(
                motionPlans.size());
            result.motionsReoriented = result.motionsWritten;
            result.warpedMotionIndices.reserve(motionPlans.size());
            for (const auto& motionPlan : motionPlans) {
                result.warpedMotionIndices.push_back(
                    motionPlan.motionIndex);
            }
            return result;
        }
    }

    RuntimeHeldPlayerSpaceResult applyCentralPlayerSpaceVelocity(RE::hknpWorld* world,
        const std::vector<std::uint32_t>& bodyIds,
        const RE::NiPoint3& currentPlayerVelocityHavok,
        const RE::NiPoint3& previousPlayerVelocityHavok,
        float residualVelocityKeep,
        bool enabled,
        bool writeVelocity,
        bool warp,
        const RE::NiTransform* previousPlayerSpaceWorld,
        const RE::NiTransform* currentPlayerSpaceWorld,
        const std::vector<std::uint32_t>* warpProxyBodyIds)
    {
        RuntimeHeldPlayerSpaceResult result{};
        if (!world || !enabled || (!writeVelocity && !warp) ||
            bodyIds.empty()) {
            return result;
        }

        HeldPlayerSpaceRegistry registry;
        registry.beginFrame(toPure(currentPlayerVelocityHavok), toPure(previousPlayerVelocityHavok), residualVelocityKeep);
        if (warp) {
            result = applyWarpTransaction(
                world,
                bodyIds,
                warpProxyBodyIds,
                previousPlayerVelocityHavok,
                residualVelocityKeep,
                writeVelocity,
                previousPlayerSpaceWorld,
                currentPlayerSpaceWorld);
            registry.recordWriter(WriterKind::ConstraintTarget);
            registry.recordWriter(WriterKind::PlayerSpaceCentral);
            result.writerMask = registry.writerMask();
            return result;
        }

        for (const auto bodyId : bodyIds) {
            if (bodyId == kInvalidBodyId) {
                continue;
            }
            auto* body = havok_runtime::getBody(world, RE::hknpBodyId{ bodyId });
            if (!body) {
                continue;
            }
            const auto motionIndex = body->motionIndex;
            registry.registerBody(bodyId, motionIndex);
        }

        result.registeredBodies = static_cast<std::uint32_t>(registry.registeredBodyCount());
        for (const auto& registration : registry.registrations()) {
            auto* motion = havok_runtime::getMotion(world, registration.motionIndex);
            if (!motion) {
                continue;
            }

            const HeldMotionSample sample{
                .bodyId = registration.bodyId,
                .motionIndex = registration.motionIndex,
                .linearVelocity = PureVec3{ .x = motion->linearVelocity.x, .y = motion->linearVelocity.y, .z = motion->linearVelocity.z },
                .angularVelocity = PureVec3{ .x = motion->angularVelocity.x, .y = motion->angularVelocity.y, .z = motion->angularVelocity.z },
            };

            const auto write = registry.solveBodyVelocity(sample);
            if (write.duplicateMotion) {
                ++result.duplicateMotionSkips;
                continue;
            }
            if (!write.shouldWrite) {
                continue;
            }
            if (!writeVelocity) {
                continue;
            }
            if (setBodyVelocityDeferred(world, write.bodyId, write.linearVelocity, write.angularVelocity)) {
                ++result.motionsWritten;
            }
        }

        registry.recordWriter(WriterKind::ConstraintTarget);
        if (writeVelocity || warp) {
            registry.recordWriter(WriterKind::PlayerSpaceCentral);
        }
        result.writerMask = registry.writerMask();
        return result;
    }
}
