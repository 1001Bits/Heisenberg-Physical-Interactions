#include "physics-interaction/contact/SoftContactRuntime.h"

#include "ROCKMain.h"
#include "RockConfig.h"
#include "RockUtils.h"
#include "physics-interaction/contact/ContactTargetIdentity.h"
#include "physics-interaction/contact/SoftContactPolicy.h"
#include "physics-interaction/contact/SoftContactWorldPolicy.h"
#include "physics-interaction/core/PhysicsFrameContext.h"
#include "physics-interaction/PhysicsLog.h"
#include "physics-interaction/hand/Hand.h"
#include "physics-interaction/hand/HandSkeleton.h"
#include "physics-interaction/hand/HandVisual.h"
#include "physics-interaction/native/HavokRuntime.h"
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "physics-interaction/native/PhysicsShapeCast.h"
#include "physics-interaction/native/PhysicsUtils.h"
#include "physics-interaction/weapon/WeaponCollision.h"

#include "RE/Havok/hknpAllHitsCollector.h"
#include "RE/Havok/hknpCollisionResult.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <vrcf/VRControllersManager.h>

namespace rock
{
    namespace
    {
        using soft_contact_math::CapsuleContact;
        using soft_contact_math::ContactKind;
        using soft_contact_math::ContactState;

        constexpr const char* RIGHT_SOFT_CONTACT_TAG = "ROCK_SoftContact_Right";
        constexpr const char* LEFT_SOFT_CONTACT_TAG = "ROCK_SoftContact_Left";
        constexpr float kCorrectionClearDistance = 0.001f;
        // NOTE (Jul-8 upstream merge): the Jul-6 embed flicker fix (exp-lerp smoothing of the
        // hand-vs-hand/weapon synthetic corrections) was REMOVED — upstream deleted those synthetic
        // channels entirely (soft contact is world-only now) and ships its own release-blend easing
        // (rockSoftContactWorldReleaseLerp*), which supersedes it.
        constexpr float kWorldProbeMinSweepDistanceGameUnits = 0.05f;
        constexpr float kWorldProbeRestQueryDistanceGameUnits = 1.0f;
        constexpr float kWorldProbeRestQueryCooldownSeconds = 0.08f;
        // Palm + grab anchor remain first priority. The remaining budget is
        // rotated across fingertips so a fast frame cannot issue seven world
        // shape casts per hand while no finger is permanently starved.
        constexpr std::uint32_t kMaxWorldProbeCastsPerChannelPerFrame = 4;
        constexpr std::uint32_t kPriorityWorldProbeCount = 2;
        constexpr float kDefaultWorldContactPaddingGameUnits = 0.35f;
        constexpr float kDefaultWorldPostReleaseReentryMinApproachDistanceGameUnits = 0.025f;
        constexpr float kDefaultWorldCachedPlaneMaxTangentDriftGameUnits = 10.0f;
        constexpr float kDefaultWorldCachedPlaneMaxClearDistanceGameUnits = 18.0f;
        constexpr std::uint32_t kNativeWorldContactMaxAgeFrames = 2;
        constexpr std::uint32_t kWorldProbeIdRightBase = 0x5000u;
        constexpr std::uint32_t kWorldProbeIdLeftBase = 0x6000u;
        constexpr std::uint32_t kWorldProbeIdWeaponBase = 0x7000u;
        constexpr std::uint32_t kInvalidWeaponSourceBodyId =
            0x7FFF'FFFFu;

        enum class CandidateSource : std::uint8_t
        {
            QueryWorld = 0,
            CachedWorldPlane,
            NativeWorld,
        };

        struct Candidate
        {
            bool valid = false;
            bool suppressed = false;
            ContactKind kind = ContactKind::None;
            CandidateSource source = CandidateSource::QueryWorld;
            CapsuleContact contact{};
            bool sourceIsWeapon = false;
            float approachSpeedGameUnits = 0.0f;
            contact_target_identity::ContactTargetIdentity targetIdentity{};
            std::uint32_t weaponSourceBodyId =
                kInvalidWeaponSourceBodyId;
            bool weaponAnchorValid = false;
            bool weaponAnchorUsesSourceLocal = false;
            RE::NiPoint3 weaponAnchorLocal{};
            float weaponProbeRadius = 0.0f;
        };

        struct CachedWorldPlaneResult
        {
            Candidate candidate{};
            bool hadCachedPlane = false;
            bool leftContact = false;
            RE::NiPoint3 normal{};
        };

        struct WorldContactProbe
        {
            bool valid = false;
            RE::NiPoint3 position{};
            float radius = 0.0f;
            std::uint32_t id = 0;
            std::size_t stateIndex = 0;
            bool sourceIsWeapon = false;
            std::uint32_t weaponSourceBodyId =
                kInvalidWeaponSourceBodyId;
            bool weaponAnchorValid = false;
            bool weaponAnchorUsesSourceLocal = false;
            RE::NiPoint3 weaponAnchorLocal{};
        };

        contact_target_identity::ContactTargetResolutionOptions contactTargetResolutionOptions()
        {
            /*
             * Contact solving only needs cheap body/filter/motion identity.
             * Ref traversal and string copies are diagnostics for mapping world
             * contacts, so keep them behind the explicit identity log toggle.
             */
            const bool richDiagnostics = g_rockConfig.rockDebugContactTargetIdentityLogging;
            return contact_target_identity::ContactTargetResolutionOptions{
                .resolveReference = richDiagnostics,
                .includeRichText = richDiagnostics,
            };
        }

        const char* softContactTag(bool isLeft)
        {
            return isLeft ? LEFT_SOFT_CONTACT_TAG : RIGHT_SOFT_CONTACT_TAG;
        }

        soft_contact_math::ArmReachProjection constrainSoftContactTargetToArmReach(
            bool isLeft,
            RE::NiTransform& target)
        {
            RE::NiPoint3 shoulderWorld{};
            float maxReach = 0.0f;
            if (!rock::HostGetPreAuthorityArmReach(
                    isLeft,
                    shoulderWorld,
                    maxReach)) {
                return {};
            }

            auto projection = soft_contact_math::projectHandTargetToArmReach(
                target.translate,
                shoulderWorld,
                maxReach);
            if (projection.valid) {
                target.translate = projection.target;
            }
            return projection;
        }

        std::size_t handIndex(bool isLeft)
        {
            return isLeft ? 1u : 0u;
        }

        std::uint32_t makeWorldProbeId(bool isLeft, std::uint32_t ordinal)
        {
            return (isLeft ? kWorldProbeIdLeftBase : kWorldProbeIdRightBase) + ordinal;
        }

        void addWorldContactProbe(
            std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& outProbes,
            std::uint32_t& outCount,
            const RE::NiPoint3& position,
            float radius,
            bool isLeft,
            bool sourceIsWeapon = false)
        {
            if (outCount >= outProbes.size() || !soft_contact_math::isFinite(position) || !std::isfinite(radius) || radius <= 0.0f) {
                return;
            }

            auto& probe = outProbes[outCount];
            probe.valid = true;
            probe.position = position;
            probe.radius = radius;
            probe.id =
                sourceIsWeapon
                    ? kWorldProbeIdWeaponBase + outCount
                    : makeWorldProbeId(isLeft, outCount);
            probe.stateIndex = outCount;
            probe.sourceIsWeapon = sourceIsWeapon;
            ++outCount;
        }

        bool addExactWeaponWorldContactProbe(
            std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& outProbes,
            std::uint32_t& outCount,
            const RE::NiPoint3& position,
            float radius,
            std::uint32_t sourceBodyId,
            const RE::NiPoint3& anchorLocal,
            bool anchorUsesSourceLocal)
        {
            if (outCount >= outProbes.size() ||
                !soft_contact_math::isFinite(position) ||
                !soft_contact_math::isFinite(anchorLocal) ||
                !std::isfinite(radius) ||
                radius <= 0.0f ||
                sourceBodyId == kInvalidWeaponSourceBodyId) {
                return false;
            }

            auto& probe = outProbes[outCount];
            probe.valid = true;
            probe.position = position;
            probe.radius = radius;
            probe.id = kWorldProbeIdWeaponBase;
            probe.stateIndex = outCount;
            probe.sourceIsWeapon = true;
            probe.weaponSourceBodyId = sourceBodyId;
            probe.weaponAnchorValid = true;
            probe.weaponAnchorUsesSourceLocal =
                anchorUsesSourceLocal;
            probe.weaponAnchorLocal = anchorLocal;
            ++outCount;
            return true;
        }

        void buildWorldContactProbes(
            bool isLeft,
            const HandFrameInput& handInput,
            const RE::NiPoint3& flattenedToCleanTranslation,
            bool includeHandProbes,
            std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& outProbes,
            std::uint32_t& outCount)
        {
            outProbes = {};
            outCount = 0;
            if (!includeHandProbes || handInput.disabled) {
                return;
            }

            addWorldContactProbe(outProbes, outCount, handInput.rawHandWorld.translate, 3.2f, isLeft);
            addWorldContactProbe(outProbes, outCount, handInput.grabAnchorWorld, 2.4f, isLeft);

            root_flattened_finger_skeleton_runtime::Snapshot snapshot{};
            if (root_flattened_finger_skeleton_runtime::
                    resolveLiveFingerSkeletonSnapshot(
                        isLeft,
                        snapshot)) {
                for (const auto& chain : snapshot.fingers) {
                    if (!chain.valid) {
                        continue;
                    }
                    /*
                     * On hosted FRIK 0.77.12 the flattened finger tree already
                     * contains last frame's external hand translation. Keep the
                     * fingertip probes in the same clean controller frame as the
                     * palm/grab probes, otherwise soft contact can solve against
                     * its own previous correction and alternate on/off.
                     */
                    addWorldContactProbe(
                        outProbes,
                        outCount,
                        soft_contact_math::add(
                            chain.points[2],
                            flattenedToCleanTranslation),
                        1.15f,
                        isLeft);
                }
            }
        }

        int candidateSourcePriority(CandidateSource source)
        {
            switch (source) {
            case CandidateSource::CachedWorldPlane:
                // Once an episode has a valid entry plane, keep that plane
                // authoritative. Fresh hknp manifold points are noisy and
                // replacing the stop plane each frame caused visible chatter.
                return 30;
            case CandidateSource::NativeWorld:
                return 20;
            case CandidateSource::QueryWorld:
                return 10;
            default:
                return 0;
            }
        }

        float maxWorldCorrection();
        RE::NiPoint3 correctionForCandidate(const Candidate& candidate);

        float candidateResponseStrength(const Candidate& candidate)
        {
            return soft_contact_math::length(correctionForCandidate(candidate));
        }

        bool candidateResponseWinsTie(const Candidate& best, const Candidate& candidate)
        {
            return soft_contact_math::preferStrongerContactResponse(
                candidateResponseStrength(candidate),
                candidate.contact.penetration,
                candidateResponseStrength(best),
                best.contact.penetration);
        }

        void keepStrongerCandidate(Candidate& best, const Candidate& candidate)
        {
            if (!candidate.valid) {
                return;
            }

            const int bestSourcePriority = best.valid ? candidateSourcePriority(best.source) : -1;
            const int candidateSourcePriorityValue = candidateSourcePriority(candidate.source);
            if (!best.valid ||
                candidateSourcePriorityValue > bestSourcePriority ||
                (candidateSourcePriorityValue == bestSourcePriority && candidateResponseWinsTie(best, candidate))) {
                best = candidate;
            }
        }

        void clearWorldContactState(auto& handState)
        {
            for (auto& probe : handState.worldProbes) {
                probe = {};
            }
            handState.cachedWorldPlane = {};
            handState.worldHaptic = {};
        }

        float worldContactPadding()
        {
            return soft_contact_math::sanitizeNonNegative(
                g_rockConfig.rockSoftContactWorldContactPaddingGameUnits,
                kDefaultWorldContactPaddingGameUnits);
        }

        float worldQueryRadiusPadding(float contactPadding)
        {
            return soft_contact_math::effectiveQueryPadding(
                g_rockConfig.rockSoftContactWorldRadiusPaddingGameUnits,
                contactPadding,
                1.5f,
                kDefaultWorldContactPaddingGameUnits);
        }

        float worldPostReleaseReentryMinApproachDistance()
        {
            return soft_contact_math::sanitizeNonNegative(
                g_rockConfig.rockSoftContactWorldPostReleaseReentryMinApproachDistanceGameUnits,
                kDefaultWorldPostReleaseReentryMinApproachDistanceGameUnits);
        }

        float worldCachedPlaneMaxTangentDrift()
        {
            return soft_contact_math::sanitizePositive(
                g_rockConfig.rockSoftContactWorldCachedPlaneMaxTangentDriftGameUnits,
                kDefaultWorldCachedPlaneMaxTangentDriftGameUnits);
        }

        float worldCachedPlaneMaxClearDistance()
        {
            return soft_contact_math::sanitizePositive(
                g_rockConfig.rockSoftContactWorldCachedPlaneMaxClearDistanceGameUnits,
                kDefaultWorldCachedPlaneMaxClearDistanceGameUnits);
        }

        float maxWorldCorrection()
        {
            return soft_contact_math::sanitizePositive(g_rockConfig.rockSoftContactWorldMaxCorrectionGameUnits, 18.0f);
        }

        float softContactReleaseLerpMinTime()
        {
            return std::clamp(std::isfinite(g_rockConfig.rockSoftContactWorldReleaseLerpTimeMin) ? g_rockConfig.rockSoftContactWorldReleaseLerpTimeMin : 0.06f,
                0.0f,
                0.5f);
        }

        float softContactReleaseLerpMaxTime()
        {
            return std::clamp(std::isfinite(g_rockConfig.rockSoftContactWorldReleaseLerpTimeMax) ? g_rockConfig.rockSoftContactWorldReleaseLerpTimeMax : 0.12f,
                softContactReleaseLerpMinTime(),
                0.5f);
        }

        float softContactReleaseLerpMinDistance()
        {
            return soft_contact_math::sanitizeNonNegative(g_rockConfig.rockSoftContactWorldReleaseLerpMinDistance, 0.5f);
        }

        float softContactReleaseLerpMaxDistance()
        {
            return std::max(
                softContactReleaseLerpMinDistance(),
                soft_contact_math::sanitizeNonNegative(g_rockConfig.rockSoftContactWorldReleaseLerpMaxDistance, 18.0f));
        }

        bool isFiniteTransform(const RE::NiTransform& transform)
        {
            if (!soft_contact_math::isFinite(transform.translate) || !std::isfinite(transform.scale)) {
                return false;
            }

            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if (!std::isfinite(transform.rotate.entry[row][column])) {
                        return false;
                    }
                }
            }
            return true;
        }

        float responseScaleForCandidate(const Candidate& candidate)
        {
            return candidate.contact.penetration > 0.0f ? 1.0f : 0.0f;
        }

        RE::NiPoint3 correctionForCandidate(const Candidate& candidate)
        {
            const float correctionLimit =
                soft_contact_policy::worldContactCorrectionLimit(
                    candidate.source ==
                        CandidateSource::CachedWorldPlane,
                    candidate.sourceIsWeapon,
                    candidate.contact.penetration,
                    maxWorldCorrection());
            return soft_contact_math::projectTrackedMagnetCorrection(
                candidate.contact.normal,
                candidate.contact.penetration,
                correctionLimit);
        }

        std::uint32_t resolveWorldHitFilterInfo(RE::hknpWorld* world, const RE::hknpCollisionResult& hit)
        {
            const std::uint32_t shapeFilterInfo = hit.hitBodyInfo.m_shapeCollisionFilterInfo.storage;
            if (soft_contact_world_policy::acceptsWorldSurfaceFilterInfo(shapeFilterInfo)) {
                return shapeFilterInfo;
            }

            const auto bodyId = hit.hitBodyInfo.m_bodyId;
            if (bodyId.value == soft_contact_world_policy::kInvalidWorldBodyId || !bodySlotLooksReadable(world, bodyId)) {
                return shapeFilterInfo;
            }

            auto* body = havok_runtime::getBody(world, bodyId);
            return body ? body->collisionFilterInfo : shapeFilterInfo;
        }

        const char* candidateSourceName(CandidateSource source)
        {
            switch (source) {
            case CandidateSource::QueryWorld:
                return "QueryWorld";
            case CandidateSource::CachedWorldPlane:
                return "CachedWorldPlane";
            case CandidateSource::NativeWorld:
                return "NativeWorld";
            default:
                return "Unknown";
            }
        }

        SoftContactDebugSource debugSourceForCandidateSource(CandidateSource source)
        {
            switch (source) {
            case CandidateSource::QueryWorld:
                return SoftContactDebugSource::QueryWorld;
            case CandidateSource::CachedWorldPlane:
                return SoftContactDebugSource::CachedWorldPlane;
            case CandidateSource::NativeWorld:
                return SoftContactDebugSource::NativeWorld;
            default:
                return SoftContactDebugSource::Unknown;
            }
        }

        Candidate makeWorldStaticCandidate(RE::bhkWorld* bhkWorld,
            RE::hknpWorld* hknpWorld,
            const RE::hknpCollisionResult& hit,
            const WorldContactProbe& probe,
            const RE::NiPoint3& castStartPosition,
            const RE::NiPoint3& previousProbePosition,
            const RE::NiPoint3& fallbackNormal,
            float radiusPadding,
            float skin,
            float deltaSeconds)
        {
            Candidate candidate{};
            const auto hitBodyId = hit.hitBodyInfo.m_bodyId;
            if (hitBodyId.value == soft_contact_world_policy::kInvalidWorldBodyId) {
                return candidate;
            }

            const RE::NiPoint3 hitPoint = hkVectorToNiPoint(hit.position);
            const RE::NiPoint3 sweepDelta = soft_contact_math::sub(probe.position, previousProbePosition);
            const RE::NiPoint3 rawHitNormal(hit.normal.x, hit.normal.y, hit.normal.z);
            const RE::NiPoint3 castFallbackNormal = soft_contact_math::normalizeOr(soft_contact_math::negate(sweepDelta), fallbackNormal);
            /*
             * A sweep's start is the known entry side of the surface.  Orienting
             * the normal toward the current probe instead makes the normal flip
             * as soon as a fast controller sample crosses the plane.  The
             * resulting correction then pushes the rendered hand farther
             * through the wall, drops the cached plane, and reacquires it on the
             * next frame -- the visible launch/flicker loop.
             */
            const RE::NiPoint3 surfaceNormal =
                soft_contact_math::orientNormalTowardPoint(hitPoint, rawHitNormal, castStartPosition, castFallbackNormal);

            const float effectiveProbeRadius =
                probe.radius + soft_contact_math::sanitizeNonNegative(radiusPadding, 0.0f);
            auto contact = soft_contact_math::solvePointPlaneContact(
                probe.position,
                hitPoint,
                surfaceNormal,
                effectiveProbeRadius,
                skin,
                probe.id,
                hitBodyId.value);
            if (!contact.active) {
                return candidate;
            }

            const float dt = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 1.0f / 240.0f, 0.1f);
            const RE::NiPoint3 velocity = soft_contact_math::mul(sweepDelta, 1.0f / dt);
            candidate.valid = true;
            candidate.kind = ContactKind::WorldStatic;
            candidate.source = CandidateSource::QueryWorld;
            candidate.contact = contact;
            candidate.sourceIsWeapon = probe.sourceIsWeapon;
            candidate.approachSpeedGameUnits = std::max(0.0f, -soft_contact_math::dot(velocity, contact.normal));
            candidate.targetIdentity = contact_target_identity::resolveContactTarget(
                bhkWorld,
                hknpWorld,
                hitBodyId.value,
                contact_evidence::NativeContactEndpointKind::WorldSurface,
                &hitPoint,
                &surfaceNormal,
                contactTargetResolutionOptions());
            return candidate;
        }

        const WorldContactProbe* findClosestWorldProbe(
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount,
            const RE::NiPoint3& point,
            bool sourceIsWeapon)
        {
            const WorldContactProbe* best = nullptr;
            float bestDistanceSquared = std::numeric_limits<float>::max();
            if (!soft_contact_math::isFinite(point)) {
                return nullptr;
            }

            for (std::uint32_t i = 0; i < probeCount && i < probes.size(); ++i) {
                const auto& probe = probes[i];
                if (!probe.valid ||
                    probe.sourceIsWeapon != sourceIsWeapon) {
                    continue;
                }
                const float distanceSquared = soft_contact_math::lengthSquared(soft_contact_math::sub(probe.position, point));
                if (!std::isfinite(distanceSquared) || distanceSquared >= bestDistanceSquared) {
                    continue;
                }
                best = &probe;
                bestDistanceSquared = distanceSquared;
            }
            return best;
        }

        Candidate makeNativeWorldStaticCandidate(
            RE::bhkWorld* bhkWorld,
            RE::hknpWorld* hknpWorld,
            const contact_evidence::NativeContactEvidenceRecord& evidence,
            const WorldContactProbe& probe,
            auto& handState,
            const RE::NiPoint3& fallbackNormal,
            float contactPadding,
            float skin,
            float deltaSeconds)
        {
            Candidate candidate{};
            if (evidence.quality != contact_evidence::NativeContactQuality::RawPoint ||
                !soft_contact_math::isFinite(evidence.contactPointGame) ||
                !soft_contact_math::isFinite(evidence.contactNormalGame)) {
                return candidate;
            }

            /*
             * Native evidence acquires the cached plane. Once the probe has
             * crossed a surface, last frame's probe is also on the
             * far side and cannot remain the orientation witness: it would
             * flip the fresh native normal and overwrite the still-correct
             * cache.  For the same nearby contact episode on one body, keep fresh
             * normals in the cached entry normal's hemisphere until the
             * cached-plane solver's existing separation/drift/escape checks
             * release that episode.  Previous-probe orientation is only the
             * first-acquisition fallback.
             */
            RE::NiPoint3 surfaceNormal{};
            const bool matchesCachedContact =
                handState.cachedWorldPlane.active &&
                handState.cachedWorldPlane.sourceIsWeapon ==
                    probe.sourceIsWeapon &&
                handState.cachedWorldPlane.bodyId == evidence.targetBodyId &&
                soft_contact_math::withinClearDistanceLimit(
                    evidence.contactPointGame,
                    handState.cachedWorldPlane.surfacePoint,
                    worldCachedPlaneMaxClearDistance()) &&
                soft_contact_math::isFinite(handState.cachedWorldPlane.normal);
            if (matchesCachedContact) {
                const RE::NiPoint3 cachedEntryNormal =
                    soft_contact_math::normalizeOr(handState.cachedWorldPlane.normal, fallbackNormal);
                const RE::NiPoint3 freshNormal =
                    soft_contact_math::normalizeOr(evidence.contactNormalGame, cachedEntryNormal);
                surfaceNormal =
                    soft_contact_math::dot(freshNormal, cachedEntryNormal) < 0.0f ?
                        soft_contact_math::negate(freshNormal) :
                        freshNormal;
            } else {
                RE::NiPoint3 normalOrientationPoint = probe.position;
                if (probe.stateIndex < handState.worldProbes.size()) {
                    const auto& previousProbe = handState.worldProbes[probe.stateIndex];
                    if (previousProbe.valid && soft_contact_math::isFinite(previousProbe.previous)) {
                        normalOrientationPoint = previousProbe.previous;
                    }
                }
                surfaceNormal = soft_contact_math::orientNormalTowardPoint(
                    evidence.contactPointGame,
                    evidence.contactNormalGame,
                    normalOrientationPoint,
                    fallbackNormal);
            }
            const float effectiveProbeRadius = probe.radius + soft_contact_math::sanitizeNonNegative(contactPadding, 0.0f);
            auto contact = soft_contact_math::solvePointPlaneContact(
                probe.position,
                evidence.contactPointGame,
                surfaceNormal,
                effectiveProbeRadius,
                skin,
                probe.id,
                evidence.targetBodyId);
            if (!contact.active) {
                return candidate;
            }

            float approachSpeed = 0.0f;
            if (soft_contact_math::isFinite(evidence.sourceVelocityGame)) {
                approachSpeed = std::max(0.0f, -soft_contact_math::dot(evidence.sourceVelocityGame, contact.normal));
            }
            if (approachSpeed <= 0.0f && probe.stateIndex < handState.worldProbes.size() && handState.worldProbes[probe.stateIndex].valid) {
                const float dt = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 1.0f / 240.0f, 0.1f);
                const auto sweepDelta = soft_contact_math::sub(probe.position, handState.worldProbes[probe.stateIndex].previous);
                const auto velocity = soft_contact_math::mul(sweepDelta, 1.0f / dt);
                approachSpeed = std::max(0.0f, -soft_contact_math::dot(velocity, contact.normal));
            }

            candidate.valid = true;
            candidate.kind = ContactKind::WorldStatic;
            candidate.source = CandidateSource::NativeWorld;
            candidate.contact = contact;
            candidate.sourceIsWeapon = probe.sourceIsWeapon;
            candidate.approachSpeedGameUnits = approachSpeed;
            candidate.targetIdentity = contact_target_identity::resolveContactTarget(
                bhkWorld,
                hknpWorld,
                evidence.targetBodyId,
                evidence.targetKind,
                &evidence.contactPointGame,
                &surfaceNormal,
                contactTargetResolutionOptions());
            if (candidate.targetIdentity.filterInfo == contact_target_identity::kUnknownFilterInfo &&
                evidence.targetFilterInfo != contact_evidence::kUnknownFilterInfo) {
                candidate.targetIdentity.filterInfo = evidence.targetFilterInfo;
                candidate.targetIdentity.layer = evidence.targetLayer;
            }
            candidate.weaponSourceBodyId =
                probe.weaponSourceBodyId;
            candidate.weaponAnchorValid =
                probe.weaponAnchorValid;
            candidate.weaponAnchorUsesSourceLocal =
                probe.weaponAnchorUsesSourceLocal;
            candidate.weaponAnchorLocal =
                probe.weaponAnchorLocal;
            candidate.weaponProbeRadius =
                probe.radius;
            return candidate;
        }

        Candidate solveNativeWorldStaticContact(
            RE::bhkWorld* bhkWorld,
            RE::hknpWorld* hknpWorld,
            const contact_evidence::NativeContactEvidenceSnapshot& evidenceSnapshot,
            auto& handState,
            bool isLeft,
            bool includeHandProbes,
            bool includeWeaponProbes,
            const WeaponCollision* weaponCollision,
            RE::NiNode* weaponNode,
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount,
            const RE::NiPoint3& fallbackNormal,
            float contactPadding,
            float skin,
            float deltaSeconds)
        {
            Candidate best{};
            using contact_evidence::NativeContactEndpointKind;
            for (std::uint32_t i = 0; i < evidenceSnapshot.count && i < evidenceSnapshot.records.size(); ++i) {
                const auto& evidence = evidenceSnapshot.records[i];
                if (!evidence.valid ||
                    !contact_evidence::isFrameFresh(evidenceSnapshot.currentFrame, evidence.frame, kNativeWorldContactMaxAgeFrames) ||
                    evidence.targetKind != NativeContactEndpointKind::WorldSurface ||
                    evidence.quality != contact_evidence::NativeContactQuality::RawPoint) {
                    continue;
                }

                const bool sourceIsWeapon =
                    evidence.sourceKind ==
                    NativeContactEndpointKind::Weapon;
                const bool sourceIsExpectedHand =
                    (!isLeft && evidence.sourceKind == NativeContactEndpointKind::RightHand) ||
                    (isLeft && evidence.sourceKind == NativeContactEndpointKind::LeftHand);
                if ((sourceIsWeapon &&
                     (!includeWeaponProbes ||
                      !weaponCollision ||
                      !weaponCollision->
                          isWeaponBodyIdAtomic(
                              evidence.sourceBodyId))) ||
                    (!sourceIsWeapon &&
                     (!includeHandProbes ||
                      !sourceIsExpectedHand ||
                      evidence.sourceIsLeft != isLeft))) {
                    continue;
                }

                WorldContactProbe exactWeaponProbe{};
                const WorldContactProbe* probe = nullptr;
                if (sourceIsWeapon) {
                    RE::NiPoint3 anchorLocal{};
                    bool anchorUsesSourceLocal = false;
                    RE::NiPoint3 probeWorld{};
                    float probeRadius = 0.0f;
                    if (!weaponNode ||
                        !weaponCollision->
                            tryCaptureInteractionCollisionProbeAnchor(
                                weaponNode,
                                evidence.sourceBodyId,
                                evidence.contactPointGame,
                                anchorLocal,
                                anchorUsesSourceLocal,
                                probeWorld,
                                probeRadius)) {
                        continue;
                    }

                    exactWeaponProbe.valid = true;
                    exactWeaponProbe.position = probeWorld;
                    exactWeaponProbe.radius =
                        (std::max)(probeRadius, 0.5f);
                    exactWeaponProbe.id =
                        kWorldProbeIdWeaponBase;
                    exactWeaponProbe.stateIndex = 0;
                    exactWeaponProbe.sourceIsWeapon = true;
                    exactWeaponProbe.weaponSourceBodyId =
                        evidence.sourceBodyId;
                    exactWeaponProbe.weaponAnchorValid = true;
                    exactWeaponProbe.weaponAnchorUsesSourceLocal =
                        anchorUsesSourceLocal;
                    exactWeaponProbe.weaponAnchorLocal =
                        anchorLocal;
                    probe = &exactWeaponProbe;
                } else {
                    probe = findClosestWorldProbe(
                        probes,
                        probeCount,
                        evidence.contactPointGame,
                        false);
                }
                if (!probe) {
                    continue;
                }

                keepStrongerCandidate(best,
                    makeNativeWorldStaticCandidate(bhkWorld, hknpWorld, evidence, *probe, handState, fallbackNormal, contactPadding, skin, deltaSeconds));
            }
            return best;
        }

        CachedWorldPlaneResult solveCachedWorldPlaneContact(auto& handState,
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount,
            float contactPadding,
            float skin,
            float maxTangentDrift,
            float maxClearDistance,
            float deltaSeconds)
        {
            CachedWorldPlaneResult result{};
            if (!handState.cachedWorldPlane.active) {
                return result;
            }
            result.hadCachedPlane = true;
            result.normal = handState.cachedWorldPlane.normal;

            const WorldContactProbe* probe = nullptr;
            for (std::uint32_t i = 0; i < probeCount && i < probes.size(); ++i) {
                if (probes[i].valid && probes[i].id == handState.cachedWorldPlane.probeId) {
                    probe = &probes[i];
                    break;
                }
            }

            if (!probe) {
                handState.cachedWorldPlane = {};
                result.leftContact = true;
                return result;
            }

            /*
             * Do not clear merely because the tracked probe moved farther
             * inward. That let a controller ratchet through a wall: the entry
             * plane vanished at the radial limit and a deeper native point
             * became the new plane. Outward separation is handled by the
             * inactive contact below, and sideways escape by the tangent
             * limit. Keep maxClearDistance as a finite-data sanity guard only.
             */
            if (!soft_contact_math::isFinite(probe->position) ||
                !soft_contact_math::isFinite(
                    handState.cachedWorldPlane.surfacePoint) ||
                !std::isfinite(maxClearDistance) ||
                maxClearDistance <= 0.0f) {
                handState.cachedWorldPlane = {};
                result.leftContact = true;
                return result;
            }

            const float effectiveProbeRadius = probe->radius + soft_contact_math::sanitizeNonNegative(contactPadding, 0.0f);
            auto contact = soft_contact_math::solvePointPlaneContact(
                probe->position,
                handState.cachedWorldPlane.surfacePoint,
                handState.cachedWorldPlane.normal,
                effectiveProbeRadius,
                skin,
                probe->id,
                handState.cachedWorldPlane.bodyId);

            /*
             * This is cached evidence, not a lock. The current tracked probe
             * must still be penetrating the cached plane this frame; otherwise
             * the external visual authority is released immediately.
             */
            if (!contact.active) {
                handState.cachedWorldPlane = {};
                result.leftContact = true;
                return result;
            }

            if (!soft_contact_math::withinTangentDriftLimit(
                    contact.targetPoint,
                    handState.cachedWorldPlane.surfacePoint,
                    handState.cachedWorldPlane.normal,
                    maxTangentDrift)) {
                handState.cachedWorldPlane = {};
                result.leftContact = true;
                return result;
            }

            float approachSpeed = handState.cachedWorldPlane.approachSpeedGameUnits;
            if (probe->stateIndex < handState.worldProbes.size() && handState.worldProbes[probe->stateIndex].valid) {
                const float dt = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 1.0f / 240.0f, 0.1f);
                const auto sweepDelta = soft_contact_math::sub(probe->position, handState.worldProbes[probe->stateIndex].previous);
                const auto velocity = soft_contact_math::mul(sweepDelta, 1.0f / dt);
                approachSpeed = std::max(0.0f, -soft_contact_math::dot(velocity, contact.normal));
            }

            result.candidate.valid = true;
            result.candidate.kind = ContactKind::WorldStatic;
            result.candidate.source = CandidateSource::CachedWorldPlane;
            result.candidate.contact = contact;
            result.candidate.sourceIsWeapon =
                handState.cachedWorldPlane.sourceIsWeapon;
            result.candidate.approachSpeedGameUnits = approachSpeed;
            result.candidate.targetIdentity = handState.cachedWorldPlane.targetIdentity;
            result.candidate.weaponSourceBodyId =
                handState.cachedWorldPlane.weaponSourceBodyId;
            result.candidate.weaponAnchorValid =
                handState.cachedWorldPlane.weaponAnchorValid;
            result.candidate.weaponAnchorUsesSourceLocal =
                handState.cachedWorldPlane.
                    weaponAnchorUsesSourceLocal;
            result.candidate.weaponAnchorLocal =
                handState.cachedWorldPlane.weaponAnchorLocal;
            result.candidate.weaponProbeRadius =
                handState.cachedWorldPlane.weaponProbeRadius;
            return result;
        }

        Candidate runWorldProbeCast(RE::bhkWorld* bhkWorld,
            RE::hknpWorld* world,
            const WorldContactProbe& probe,
            const RE::NiPoint3& start,
            const RE::NiPoint3& displacement,
            float distance,
            const RE::NiPoint3& previousPosition,
            const RE::NiPoint3& fallbackNormal,
            float queryRadiusPadding,
            float contactPadding,
            float skin,
            float deltaSeconds)
        {
            Candidate best{};
            if (!world || !probe.valid || !std::isfinite(distance) || distance <= 0.001f) {
                return best;
            }

            RE::hknpAllHitsCollector collector;
            if (!physics_shape_cast::castSelectionSphere(
                    world,
                    physics_shape_cast::SphereCastInput{
                        .startGame = start,
                        .directionGame = displacement,
                        .distanceGame = distance,
                        .radiusGame = probe.radius + queryRadiusPadding,
                        .collisionFilterInfo = g_rockConfig.rockSoftContactWorldShapeCastFilterInfo },
                    collector,
                    nullptr)) {
                return best;
            }

            auto* hits = collector.hits._data;
            const int hitCount = collector.hits._size;
            for (int hitIndex = 0; hitIndex < hitCount; ++hitIndex) {
                const auto& hit = hits[hitIndex];
                const std::uint32_t filterInfo = resolveWorldHitFilterInfo(world, hit);
                if (!soft_contact_world_policy::acceptsWorldSurfaceFilterInfo(filterInfo)) {
                    continue;
                }

                keepStrongerCandidate(best,
                    makeWorldStaticCandidate(
                        bhkWorld,
                        world,
                        hit,
                        probe,
                        start,
                        previousPosition,
                        fallbackNormal,
                        contactPadding,
                        skin,
                        deltaSeconds));
            }

            return best;
        }

        void storeWorldProbePositions(auto& handState,
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount)
        {
            for (std::size_t i = 0; i < handState.worldProbes.size(); ++i) {
                if (i < probeCount && probes[i].valid) {
                    handState.worldProbes[i].valid = true;
                    handState.worldProbes[i].previous = probes[i].position;
                } else {
                    handState.worldProbes[i] = {};
                }
            }
        }

        Candidate solveWorldStaticContact(RE::bhkWorld* bhkWorld,
            RE::hknpWorld* world,
            const contact_evidence::NativeContactEvidenceSnapshot& nativeContactEvidence,
            auto& handState,
            bool isLeft,
            const HandFrameInput& handInput,
            const RE::NiPoint3& flattenedToCleanTranslation,
            bool includeHandProbes,
            bool includeWeaponProbes,
            const WeaponCollision* weaponCollision,
            RE::NiNode* weaponNode,
            const RE::NiPoint3& fallbackNormal,
            float deltaSeconds)
        {
            Candidate best{};
            if (!world ||
                (!includeWeaponProbes &&
                 (!includeHandProbes || handInput.disabled))) {
                clearWorldContactState(handState);
                return best;
            }

            std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand> probes{};
            std::uint32_t probeCount = 0;
            buildWorldContactProbes(
                isLeft,
                handInput,
                flattenedToCleanTranslation,
                includeHandProbes,
                probes,
                probeCount);

            if (includeWeaponProbes &&
                handState.cachedWorldPlane.active &&
                handState.cachedWorldPlane.sourceIsWeapon &&
                handState.cachedWorldPlane.weaponAnchorValid &&
                weaponCollision &&
                weaponNode) {
                RE::NiPoint3 cachedProbeWorld{};
                float cachedProbeRadius = 0.0f;
                if (weaponCollision->
                        tryResolveInteractionCollisionProbeAnchor(
                            weaponNode,
                            handState.cachedWorldPlane.
                                weaponSourceBodyId,
                            handState.cachedWorldPlane.
                                weaponAnchorLocal,
                            handState.cachedWorldPlane.
                                weaponAnchorUsesSourceLocal,
                            cachedProbeWorld,
                            cachedProbeRadius)) {
                    (void)addExactWeaponWorldContactProbe(
                        probes,
                        probeCount,
                        cachedProbeWorld,
                        (std::max)(cachedProbeRadius, 0.5f),
                        handState.cachedWorldPlane.
                            weaponSourceBodyId,
                        handState.cachedWorldPlane.
                            weaponAnchorLocal,
                        handState.cachedWorldPlane.
                            weaponAnchorUsesSourceLocal);
                }
            }

            if (probeCount == 0 && !includeWeaponProbes) {
                clearWorldContactState(handState);
                return best;
            }

            const float contactPadding = worldContactPadding();
            const float queryRadiusPadding = worldQueryRadiusPadding(contactPadding);
            const float skin =
                soft_contact_math::sanitizeNonNegative(g_rockConfig.rockSoftContactWorldSkinGameUnits, 0.5f);
            const float reentryMinApproachDistance = worldPostReleaseReentryMinApproachDistance();
            const float maxTangentDrift = worldCachedPlaneMaxTangentDrift();
            const float maxClearDistance = worldCachedPlaneMaxClearDistance();
            const float dt = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 0.0f, 0.1f);

            const auto cachedPlaneContact = solveCachedWorldPlaneContact(
                handState,
                probes,
                probeCount,
                contactPadding,
                skin,
                maxTangentDrift,
                maxClearDistance,
                deltaSeconds);
            keepStrongerCandidate(best, cachedPlaneContact.candidate);

            /*
             * A valid cached weapon plane is already the authoritative result
             * for this episode. Avoid rescanning exact hull point clouds for
             * fresh manifold records that cannot outrank it; acquisition and
             * reacquisition remain event-driven when no cached result survives.
             */
            if (!includeWeaponProbes ||
                !cachedPlaneContact.candidate.valid) {
                keepStrongerCandidate(best,
                    solveNativeWorldStaticContact(
                        bhkWorld,
                        world,
                        nativeContactEvidence,
                        handState,
                        isLeft,
                        includeHandProbes,
                        includeWeaponProbes,
                        weaponCollision,
                        weaponNode,
                        probes,
                        probeCount,
                        fallbackNormal,
                        contactPadding,
                        skin,
                        deltaSeconds));
            }

            const bool hasAuthoritativeWorldCandidate =
                best.valid &&
                best.kind == ContactKind::WorldStatic &&
                (best.source == CandidateSource::NativeWorld || best.source == CandidateSource::CachedWorldPlane);
            const bool releasedCachedPlaneThisFrame =
                cachedPlaneContact.hadCachedPlane && cachedPlaneContact.leftContact;

            if (!hasAuthoritativeWorldCandidate) {
                for (std::uint32_t index = 0;
                     index < probeCount && index < probes.size();
                     ++index) {
                    const auto& probe = probes[index];
                    if (probe.valid &&
                        !probe.sourceIsWeapon &&
                        probe.stateIndex < handState.worldProbes.size()) {
                        auto& previousState =
                            handState.worldProbes[probe.stateIndex];
                        previousState.restQueryCooldownSeconds =
                            std::max(
                                0.0f,
                                previousState.restQueryCooldownSeconds - dt);
                    }
                }

                std::uint32_t castsRemaining =
                    kMaxWorldProbeCastsPerChannelPerFrame;
                const auto tryProbeCast = [&](const std::uint32_t index) {
                    if (castsRemaining == 0 ||
                        index >= probeCount ||
                        index >= probes.size()) {
                        return;
                    }

                    const auto& probe = probes[index];
                    if (!probe.valid ||
                        probe.sourceIsWeapon ||
                        probe.stateIndex >=
                            handState.worldProbes.size()) {
                        return;
                    }

                    auto& previousState =
                        handState.worldProbes[probe.stateIndex];

                    const RE::NiPoint3 sweepDelta = previousState.valid ?
                                                      soft_contact_math::sub(probe.position, previousState.previous) :
                                                      RE::NiPoint3{};
                    const float sweepDistance = previousState.valid ? soft_contact_math::length(sweepDelta) : 0.0f;
                    const bool canSweep =
                        previousState.valid &&
                        std::isfinite(sweepDistance) &&
                        sweepDistance > kWorldProbeMinSweepDistanceGameUnits;
                    const bool allowPostReleaseSweep =
                        soft_contact_math::shouldAllowPostReleaseReentrySweep(
                            releasedCachedPlaneThisFrame,
                            sweepDelta,
                            cachedPlaneContact.normal,
                            reentryMinApproachDistance);

                    if (canSweep && allowPostReleaseSweep) {
                        --castsRemaining;
                        keepStrongerCandidate(best,
                            runWorldProbeCast(bhkWorld,
                                world,
                                probe,
                                previousState.previous,
                                sweepDelta,
                                sweepDistance,
                                previousState.previous,
                                fallbackNormal,
                                queryRadiusPadding,
                                contactPadding,
                                skin,
                                deltaSeconds));
                    }

                    if (canSweep || releasedCachedPlaneThisFrame || previousState.restQueryCooldownSeconds > 0.0f) {
                        return;
                    }

                    if (castsRemaining == 0) {
                        return;
                    }
                    previousState.restQueryCooldownSeconds = kWorldProbeRestQueryCooldownSeconds;
                    const RE::NiPoint3 restDirection = soft_contact_math::normalizeOr(fallbackNormal, RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
                    const RE::NiPoint3 previousPosition = previousState.valid ? previousState.previous : probe.position;
                    --castsRemaining;
                    keepStrongerCandidate(best,
                        runWorldProbeCast(bhkWorld,
                            world,
                            probe,
                            probe.position,
                            restDirection,
                            kWorldProbeRestQueryDistanceGameUnits,
                            previousPosition,
                            fallbackNormal,
                            queryRadiusPadding,
                            contactPadding,
                            skin,
                            deltaSeconds));
                };

                const auto priorityCount =
                    (std::min)(
                        probeCount,
                        kPriorityWorldProbeCount);
                for (std::uint32_t index = 0;
                     index < priorityCount && castsRemaining != 0;
                     ++index) {
                    tryProbeCast(index);
                }

                if (castsRemaining != 0 &&
                    probeCount > kPriorityWorldProbeCount) {
                    const std::uint32_t rotatingProbeCount =
                        probeCount - kPriorityWorldProbeCount;
                    const std::uint32_t start =
                        handState.worldProbeCastCursor %
                        rotatingProbeCount;
                    for (std::uint32_t offset = 0;
                         offset < rotatingProbeCount &&
                         castsRemaining != 0;
                         ++offset) {
                        const std::uint32_t index =
                            kPriorityWorldProbeCount +
                            ((start + offset) % rotatingProbeCount);
                        tryProbeCast(index);
                    }
                    handState.worldProbeCastCursor =
                        (start + 1) % rotatingProbeCount;
                }
            }

            if (best.valid &&
                best.sourceIsWeapon &&
                best.weaponAnchorValid) {
                bool alreadyTracked = false;
                for (std::uint32_t i = 0;
                     i < probeCount && i < probes.size();
                     ++i) {
                    if (probes[i].valid &&
                        probes[i].id ==
                            kWorldProbeIdWeaponBase) {
                        alreadyTracked = true;
                        break;
                    }
                }
                if (!alreadyTracked) {
                    (void)addExactWeaponWorldContactProbe(
                        probes,
                        probeCount,
                        best.contact.movablePoint,
                        (std::max)(
                            best.weaponProbeRadius,
                            0.5f),
                        best.weaponSourceBodyId,
                        best.weaponAnchorLocal,
                        best.weaponAnchorUsesSourceLocal);
                }
            }

            storeWorldProbePositions(handState, probes, probeCount);

            if (best.valid && best.kind == ContactKind::WorldStatic) {
                handState.cachedWorldPlane.active = true;
                handState.cachedWorldPlane.sourceIsWeapon =
                    best.sourceIsWeapon;
                handState.cachedWorldPlane.bodyId = best.contact.targetId;
                handState.cachedWorldPlane.probeId = best.contact.movableId;
                handState.cachedWorldPlane.surfacePoint = best.contact.targetPoint;
                handState.cachedWorldPlane.normal = best.contact.normal;
                handState.cachedWorldPlane.approachSpeedGameUnits = best.approachSpeedGameUnits;
                handState.cachedWorldPlane.targetIdentity = best.targetIdentity;
                handState.cachedWorldPlane.weaponSourceBodyId =
                    best.weaponSourceBodyId;
                handState.cachedWorldPlane.weaponAnchorValid =
                    best.weaponAnchorValid;
                handState.cachedWorldPlane.
                    weaponAnchorUsesSourceLocal =
                    best.weaponAnchorUsesSourceLocal;
                handState.cachedWorldPlane.weaponAnchorLocal =
                    best.weaponAnchorLocal;
                handState.cachedWorldPlane.weaponProbeRadius =
                    best.weaponProbeRadius;
            }

            return best;
        }

        void updateWorldContactHaptics(
            auto& handState,
            bool isLeft,
            bool active,
            float approachSpeedGameUnits,
            float deltaSeconds)
        {
            soft_contact_math::HapticEdgeConfig config{};
            config.enabled = g_rockConfig.rockSoftContactWorldHapticsEnabled;
            config.baseIntensity = g_rockConfig.rockSoftContactWorldHapticBaseIntensity;
            config.maxIntensity = g_rockConfig.rockSoftContactWorldHapticMaxIntensity;
            config.speedScale = g_rockConfig.rockSoftContactWorldHapticSpeedScale;
            config.minApproachSpeed = g_rockConfig.rockSoftContactWorldHapticMinApproachSpeedGameUnits;
            config.cooldownSeconds = g_rockConfig.rockSoftContactWorldHapticCooldownSeconds;

            const auto decision = soft_contact_math::updateHapticEdge(handState.worldHaptic, active, approachSpeedGameUnits, deltaSeconds, config);
            const float duration = std::clamp(
                std::isfinite(g_rockConfig.rockSoftContactWorldHapticDurationSeconds) ? g_rockConfig.rockSoftContactWorldHapticDurationSeconds : 0.035f,
                0.0f,
                0.2f);
            if (decision.fire && duration > 0.0f && decision.intensity > 0.0f) {
                f4cf::vrcf::VRControllers.triggerHaptic(
                    isLeft ? f4cf::vrcf::Hand::Left : f4cf::vrcf::Hand::Right,
                    duration,
                    decision.intensity);
            }
        }

        void logContactTargetIdentity(bool isLeft, const Candidate& candidate)
        {
            if (!g_rockConfig.rockDebugContactTargetIdentityLogging || candidate.kind != ContactKind::WorldStatic) {
                return;
            }

            const auto& identity = candidate.targetIdentity;
            const auto sampleMilliseconds = std::max(1, g_rockConfig.rockDebugContactTargetIdentitySampleMilliseconds);
            const auto bodyId = contact_evidence::isValidBodyId(identity.bodyId) ? identity.bodyId : candidate.contact.targetId;
            const auto layer = identity.layer;
            const auto filterInfo = identity.filterInfo;
            const auto motionIndex = identity.motionIndex;
            const auto status = identity.status;
            const auto surfaceHint = identity.surfaceHint;
            const char* formType = identity.formType.empty() ? "???" : identity.formType.c_str();
            const char* displayName = identity.displayName.empty() ? "(unnamed)" : identity.displayName.c_str();
            const char* refEditorId = identity.refEditorId.empty() ? "(none)" : identity.refEditorId.c_str();
            const char* baseEditorId = identity.baseEditorId.empty() ? "(none)" : identity.baseEditorId.c_str();
            const auto& point = candidate.contact.targetPoint;
            const auto& normal = candidate.contact.normal;

            ROCK_LOG_SAMPLE_DEBUG(ContactTarget,
                sampleMilliseconds,
                "Contact target identity: hand={} source={} status={} endpoint={} body={} layer={} filter=0x{:08X} motion={} ref={:08X} base={:08X} type={} name='{}' refEditor='{}' baseEditor='{}' surface={} point=({:.2f},{:.2f},{:.2f}) normal=({:.3f},{:.3f},{:.3f})",
                isLeft ? "Left" : "Right",
                candidateSourceName(candidate.source),
                contact_target_identity::resolutionStatusName(status),
                contact_target_identity::endpointKindName(identity.endpointKind),
                bodyId,
                layer,
                filterInfo,
                motionIndex,
                identity.refFormId,
                identity.baseFormId,
                formType,
                displayName,
                refEditorId,
                baseEditorId,
                contact_target_identity::surfaceHintName(surfaceHint),
                point.x,
                point.y,
                point.z,
                normal.x,
                normal.y,
                normal.z);
        }

        void addDebugContact(SoftContactDebugSnapshot& snapshot, bool isLeft, ContactState state, const Candidate& candidate, const RE::NiPoint3& correction)
        {
            if (!candidate.valid || snapshot.contactCount >= snapshot.contacts.size()) {
                return;
            }

            auto& entry = snapshot.contacts[snapshot.contactCount++];
            entry.valid = true;
            entry.isLeft = isLeft;
            entry.suppressed = candidate.suppressed;
            entry.kind = candidate.kind;
            entry.source = debugSourceForCandidateSource(candidate.source);
            entry.state = state;
            entry.point = candidate.contact.movablePoint;
            entry.normalEnd = soft_contact_math::add(candidate.contact.movablePoint, soft_contact_math::mul(candidate.contact.normal, 8.0f));
            entry.correctionEnd = soft_contact_math::add(candidate.contact.movablePoint, correction);
            entry.penetration = candidate.contact.penetration;
            entry.responseScale = responseScaleForCandidate(candidate);
            entry.maxCorrection = maxWorldCorrection();
            entry.correctionLength = soft_contact_math::length(correction);
            entry.movableId = candidate.contact.movableId;
            entry.targetId = candidate.contact.targetId;
            entry.targetLayer = candidate.targetIdentity.layer;
            entry.targetFilterInfo = candidate.targetIdentity.filterInfo;
            entry.targetRefFormId = candidate.targetIdentity.refFormId;
            entry.targetBaseFormId = candidate.targetIdentity.baseFormId;
            entry.surfaceHint = candidate.targetIdentity.surfaceHint;
        }

        void beginReleaseBlend(auto& handState, const RE::NiTransform& rawHandWorld)
        {
            handState.releaseBlend = {};
            if (!g_rockConfig.rockSoftContactWorldReleaseLerpEnabled ||
                !handState.externalTransformActive ||
                !isFiniteTransform(handState.lastAppliedWorld) ||
                !isFiniteTransform(rawHandWorld)) {
                return;
            }

            const float distance = hand_visual_lerp_math::distanceGameUnits(handState.lastAppliedWorld.translate, rawHandWorld.translate);
            const float duration = hand_visual_lerp_math::computeDistanceMappedDurationGameUnits(
                distance,
                softContactReleaseLerpMinTime(),
                softContactReleaseLerpMaxTime(),
                softContactReleaseLerpMinDistance(),
                softContactReleaseLerpMaxDistance());
            if (duration <= 0.0f) {
                return;
            }

            handState.releaseBlend.active = true;
            handState.releaseBlend.startWorld = handState.lastAppliedWorld;
            handState.releaseBlend.elapsedSeconds = 0.0f;
            handState.releaseBlend.durationSeconds = duration;
        }

        enum class ReleaseBlendStep : std::uint8_t
        {
            Inactive,
            Applied,
            Finished,
            Failed
        };

        ReleaseBlendStep applyReleaseBlend(auto& handState, bool isLeft, const RE::NiTransform& rawHandWorld, float deltaSeconds)
        {
            if (!handState.releaseBlend.active) {
                beginReleaseBlend(handState, rawHandWorld);
            }
            if (!handState.releaseBlend.active) {
                return ReleaseBlendStep::Inactive;
            }

            auto& release = handState.releaseBlend;
            release.elapsedSeconds = hand_visual_lerp_math::advanceTimedBlendElapsed(
                release.elapsedSeconds,
                std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 0.0f, 0.1f),
                release.durationSeconds);
            const auto blend = hand_visual_lerp_math::blendTransformOverDuration(
                release.startWorld,
                rawHandWorld,
                release.elapsedSeconds,
                release.durationSeconds);
            RE::NiTransform reachableBlend = blend.transform;
            const auto reachProjection =
                constrainSoftContactTargetToArmReach(isLeft, reachableBlend);
            if (!frik_visual_authority::applyExternalHandWorldTransform(
                    softContactTag(isLeft),
                    handFromBool(isLeft),
                    reachableBlend,
                    g_rockConfig.rockSoftContactVisualPriority)) {
                return ReleaseBlendStep::Failed;
            }

            handState.lastAppliedWorld = reachableBlend;
            handState.externalTransformActive = true;
            handState.correction =
                soft_contact_math::sub(
                    reachableBlend.translate,
                    rawHandWorld.translate);
            handState.state = ContactState::Inactive;
            if (reachProjection.projected) {
                ROCK_LOG_SAMPLE_DEBUG(
                    Hand,
                    1000,
                    "{} soft contact release target projected to arm reach "
                    "requested={:.2f}gu applied={:.2f}gu",
                    isLeft ? "Left" : "Right",
                    reachProjection.requestedDistance,
                    reachProjection.appliedDistance);
            }
            return blend.reachedTarget ? ReleaseBlendStep::Finished : ReleaseBlendStep::Applied;
        }
    }

    void SoftContactRuntime::reset()
    {
        clearAllHands();
        _weapon = {};
        _weaponGenerationKey = 0;
        _debugSnapshot = {};
        _weaponWorldCorrection = {};
        _weaponWorldCorrectionActive = false;
        _weaponWorldCorrectionHandIsLeft = false;
        _wasHandWorldEnabled = false;
        _logCounter = 0;
    }

    void SoftContactRuntime::clearHandForStrongerOwner(bool isLeft, const char* reason)
    {
        auto& handState = _hands[handIndex(isLeft)];
        const bool hadVisualAuthority =
            handState.externalTransformActive ||
            handState.state != ContactState::Inactive ||
            soft_contact_math::length(handState.correction) > kCorrectionClearDistance;
        clearHand(isLeft);
        _hands[handIndex(isLeft)].state = ContactState::Suppressed;
        if (hadVisualAuthority) {
            ROCK_LOG_SAMPLE_DEBUG(Hand,
                500,
                "{} soft contact cleared for stronger hand owner ({})",
                isLeft ? "Left" : "Right",
                reason ? reason : "unknown");
        }
    }

    void SoftContactRuntime::clearHand(bool isLeft)
    {
        auto& handState = _hands[handIndex(isLeft)];
        (void)frik_visual_authority::clearExternalHandWorldTransform(softContactTag(isLeft), handFromBool(isLeft));
        handState = {};
    }

    void SoftContactRuntime::clearAllHands()
    {
        clearHand(false);
        clearHand(true);
    }

    bool SoftContactRuntime::getDebugSnapshot(SoftContactDebugSnapshot& outSnapshot) const
    {
        outSnapshot = _debugSnapshot;
        return outSnapshot.contactCount > 0 ||
               outSnapshot.rightState != ContactState::Inactive ||
               outSnapshot.leftState != ContactState::Inactive;
    }

    bool SoftContactRuntime::getWeaponWorldCorrection(
        RE::NiPoint3& outCorrection,
        bool& outFiringHandIsLeft) const
    {
        outCorrection = _weaponWorldCorrection;
        outFiringHandIsLeft =
            _weaponWorldCorrectionHandIsLeft;
        return _weaponWorldCorrectionActive &&
               soft_contact_math::isFinite(
                   _weaponWorldCorrection) &&
               soft_contact_math::length(
                   _weaponWorldCorrection) >
                   kCorrectionClearDistance;
    }

    void SoftContactRuntime::update(const PhysicsFrameContext& frame,
        const Hand& rightHand,
        const Hand& leftHand,
        bool rightHandWeaponEquipped,
        bool leftSupportGripActive,
        bool weaponFiringHandIsLeft,
        const WeaponCollision* weaponCollision,
        RE::NiNode* weaponNode,
        const contact_evidence::NativeContactEvidenceSnapshot& nativeContactEvidence)
    {
        performance_profiler::ScopedTimer profilerTimer(performance_profiler::Scope::SoftContact);

        _debugSnapshot = {};
        _weaponWorldCorrection = {};
        _weaponWorldCorrectionActive = false;
        _weaponWorldCorrectionHandIsLeft =
            weaponFiringHandIsLeft;
        _debugSnapshot.rightState = _hands[0].state;
        _debugSnapshot.leftState = _hands[1].state;

        const auto channels =
            soft_contact_policy::
                resolveWorldContactChannels(
                    g_rockConfig.
                        rockSoftContactWorldEnabled,
                    g_rockConfig.
                        rockHandWorldPushbackEnabled,
                    g_rockConfig.
                        rockWeaponCollisionEnabled,
                    g_rockConfig.
                        rockWeaponCollisionStaticWorldEnabled,
                    frame.worldReady,
                    frame.menuBlocked);
        if (!channels.anyEnabled()) {
            if (_wasHandWorldEnabled) {
                clearAllHands();
            }
            _weapon = {};
            _weaponGenerationKey = 0;
            _debugSnapshot = {};
            _wasHandWorldEnabled = false;
            return;
        }

        if (!channels.handPushback &&
            _wasHandWorldEnabled) {
            clearAllHands();
        }
        _wasHandWorldEnabled =
            channels.handPushback;

        if (++_logCounter >= 360) {
            _logCounter = 0;
            const float loggedWorldContactPadding = worldContactPadding();
            const float loggedWorldQueryPadding = worldQueryRadiusPadding(loggedWorldContactPadding);
            ROCK_LOG_DEBUG(Hand,
                "SoftContact channels: handPushback={} weaponWall={} worldHaptics={} worldMaxCorrection={:.2f} worldQueryPadding={:.2f} worldContactPadding={:.2f} worldPostReleaseReentryMinApproach={:.3f} worldCachedPlaneMaxTangentDrift={:.2f} worldCachedPlaneMaxClearDistance={:.2f} releaseLerp={} releaseLerpTime={:.2f}-{:.2f}s priority={}",
                channels.handPushback ? "on" : "off",
                channels.weaponWallCollision ? "on" : "off",
                g_rockConfig.rockSoftContactWorldHapticsEnabled ? "yes" : "no",
                maxWorldCorrection(),
                loggedWorldQueryPadding,
                loggedWorldContactPadding,
                worldPostReleaseReentryMinApproachDistance(),
                worldCachedPlaneMaxTangentDrift(),
                worldCachedPlaneMaxClearDistance(),
                g_rockConfig.rockSoftContactWorldReleaseLerpEnabled ? "yes" : "no",
                softContactReleaseLerpMinTime(),
                softContactReleaseLerpMaxTime(),
                g_rockConfig.rockSoftContactVisualPriority);
        }

        auto solveForHand = [&](bool isLeft, const HandFrameInput& handInput, const Hand& hand) {
            auto& handState = _hands[handIndex(isLeft)];
            const bool previouslyInContact =
                handState.state == ContactState::Touching ||
                handState.state == ContactState::Penetrating;
            /*
             * ROCK soft contact is production world-only visual authority. It
             * still yields to held objects and weapon owners because those
             * systems own the hand transform when active. Pull/locked-selection are
             * also grab ownership states: they can transition into a capture in
             * the same frame, so stale visual contact must not survive there and
             * poison the raw tracked hand frame used by grab setup.
             */
            // EMBED (Jul 6, kept through the Jul-8 upstream merge): only the GRAB flow fully owns the
            // hand. A WEAPON-armed hand still gets the WORLD contact so the hand + welded weapon are
            // retracted from walls (upstream suppresses the armed hand entirely; the embed keeps the
            // world channel — soft contact is world-only upstream now, so this is the whole solve).
            const bool ownedByGrabFlow =
                suppressesGeneratedHandContactEvidence(hand.getState());
            if (ownedByGrabFlow) {
                clearHand(isLeft);
                handState.state = ContactState::Suppressed;
                return;
            }

            /*
             * The hosted FRIK 0.77.12 goal seam consumes the previous frame's
             * authority target before ROCK runs. Consequently the root-
             * flattened hand sampled into frame.rawHandWorld can already carry
             * our previous soft-contact translation. Solving from that output
             * makes a stopped hand look separated, releases the cached plane,
             * then reacquires once FRIK returns to controller truth: visible
             * wall flicker.
             *
             * Use the clean same-frame hand published by FRIK's pass 1 for
             * translation only. Preserve the sampled flattened rotation/scale
             * because those are the skinned-hand basis expected by the visual
             * authority bridge. The same translation delta rebases the grab
             * anchor and live fingertip probes below.
             */
            HandFrameInput cleanHandInput = handInput;
            RE::NiPoint3 flattenedToCleanTranslation{};
            RE::NiTransform preAuthorityHandWorld{};
            if (rock::HostGetPreAuthorityHandWorld(isLeft, preAuthorityHandWorld) &&
                soft_contact_math::isFinite(preAuthorityHandWorld.translate)) {
                flattenedToCleanTranslation =
                    soft_contact_math::sub(preAuthorityHandWorld.translate, handInput.rawHandWorld.translate);
                const float rebaseDistance =
                    soft_contact_math::length(flattenedToCleanTranslation);
                if (std::isfinite(rebaseDistance) && rebaseDistance <= 64.0f) {
                    cleanHandInput.rawHandWorld.translate =
                        preAuthorityHandWorld.translate;
                    cleanHandInput.grabAnchorWorld =
                        soft_contact_math::add(
                            handInput.grabAnchorWorld,
                            flattenedToCleanTranslation);
                } else {
                    flattenedToCleanTranslation = {};
                }
            }

            Candidate best{};
            const RE::NiPoint3 fallbackNormal = soft_contact_math::normalizeOr(cleanHandInput.palmNormalWorld, RE::NiPoint3(0.0f, 0.0f, 1.0f));

            if (!cleanHandInput.disabled) {
                keepStrongerCandidate(best,
                    solveWorldStaticContact(
                        frame.bhkWorld,
                        frame.hknpWorld,
                        nativeContactEvidence,
                        _hands[handIndex(isLeft)],
                        isLeft,
                        cleanHandInput,
                        flattenedToCleanTranslation,
                        true,
                        false,
                        weaponCollision,
                        weaponNode,
                        fallbackNormal,
                        frame.deltaSeconds));
            }

            if (!frik_visual_authority::isAvailable() || cleanHandInput.disabled) {
                clearHand(isLeft);
                updateWorldContactHaptics(handState, isLeft, false, 0.0f, frame.deltaSeconds);
                return;
            }

            if (best.valid &&
                !previouslyInContact &&
                (!std::isfinite(best.contact.penetration) ||
                 best.contact.penetration >
                     maxWorldCorrection())) {
                // A newly acquired contact must be solvable without hitting
                // the correction cap. Query/native glitches have produced
                // 750+ gu "penetrations"; accepting one launches the visible
                // hand by the full 18-gu cap and then release-blends from that
                // bogus pose. Existing cached contact may legitimately reach
                // the cap while the controller continues through a wall, but
                // first-frame saturated evidence is a discontinuity, not a
                // stable surface witness.
                ROCK_LOG_SAMPLE_WARN(
                    Hand,
                    1000,
                    "{} soft contact rejected implausible first witness "
                    "source={} probe={} body={} penetration={:.2f} "
                    "maxCorrection={:.2f}",
                    isLeft ? "Left" : "Right",
                    candidateSourceName(best.source),
                    best.contact.movableId,
                    best.contact.targetId,
                    best.contact.penetration,
                    maxWorldCorrection());
                handState.cachedWorldPlane = {};
                best = {};
            }

            if (best.valid) {
                logContactTargetIdentity(isLeft, best);
                updateWorldContactHaptics(handState, isLeft, true, best.approachSpeedGameUnits, frame.deltaSeconds);
                handState.releaseBlend = {};
                handState.correction = correctionForCandidate(best);
                handState.lastContactKind = best.kind;
                handState.lastContactSourceIsWeapon =
                    best.sourceIsWeapon;
                handState.state = best.contact.penetration >= 0.5f ? ContactState::Penetrating : ContactState::Touching;

                RE::NiTransform target = cleanHandInput.rawHandWorld;
                target.translate = soft_contact_math::add(target.translate, handState.correction);
                const auto reachProjection =
                    constrainSoftContactTargetToArmReach(isLeft, target);
                /*
                 * Keep every downstream consumer on the exact target that FRIK
                 * receives. In particular, weapon contact transport must not
                 * retain the pre-projection correction and pull a welded gun
                 * away from the anatomically bounded hand.
                 */
                handState.correction =
                    soft_contact_math::sub(
                        target.translate,
                        cleanHandInput.rawHandWorld.translate);
                const float appliedCorrectionLength =
                    soft_contact_math::length(
                        handState.correction);
                if (best.source ==
                        CandidateSource::CachedWorldPlane &&
                    !best.sourceIsWeapon &&
                    appliedCorrectionLength >
                        maxWorldCorrection() + 0.01f) {
                    ROCK_LOG_SAMPLE_DEBUG(
                        Hand,
                        1000,
                        "{} cached wall plane retained beyond acquisition "
                        "limit required={:.2f}gu applied={:.2f}gu limit={:.2f}gu",
                        isLeft ? "Left" : "Right",
                        best.contact.penetration,
                        appliedCorrectionLength,
                        maxWorldCorrection());
                }
                if (best.sourceIsWeapon) {
                    _weaponWorldCorrection =
                        handState.correction;
                    _weaponWorldCorrectionActive = true;
                    _weaponWorldCorrectionHandIsLeft =
                        isLeft;
                }
                if (frik_visual_authority::applyExternalHandWorldTransform(softContactTag(isLeft), handFromBool(isLeft), target, g_rockConfig.rockSoftContactVisualPriority)) {
                    handState.lastAppliedWorld = target;
                    handState.externalTransformActive = true;
                    if (reachProjection.projected) {
                        ROCK_LOG_SAMPLE_DEBUG(
                            Hand,
                            1000,
                            "{} soft contact target projected to arm reach "
                            "requested={:.2f}gu applied={:.2f}gu correction={:.2f}gu",
                            isLeft ? "Left" : "Right",
                            reachProjection.requestedDistance,
                            reachProjection.appliedDistance,
                            soft_contact_math::length(handState.correction));
                    }
                    if (!previouslyInContact) {
                        ROCK_LOG_DEBUG(Hand,
                            "{} soft contact entered source={} probe={} body={} penetration={:.2f} correction={:.2f} cleanRebase={:.2f}",
                            isLeft ? "Left" : "Right",
                            candidateSourceName(best.source),
                            best.contact.movableId,
                            best.contact.targetId,
                            best.contact.penetration,
                            soft_contact_math::length(handState.correction),
                            soft_contact_math::length(flattenedToCleanTranslation));
                    }
                } else {
                    ROCK_LOG_SAMPLE_WARN(Hand,
                        1000,
                        "{} soft contact external transform apply failed; clearing visual contact state",
                        isLeft ? "Left" : "Right");
                    clearHand(isLeft);
                    handState.state = ContactState::Inactive;
                    return;
                }
                addDebugContact(_debugSnapshot, isLeft, handState.state, best, handState.correction);
                return;
            }

            updateWorldContactHaptics(handState, isLeft, false, 0.0f, frame.deltaSeconds);
            if (previouslyInContact) {
                ROCK_LOG_DEBUG(Hand,
                    "{} soft contact released cleanRebase={:.2f}",
                    isLeft ? "Left" : "Right",
                    soft_contact_math::length(flattenedToCleanTranslation));
            }
            if (handState.externalTransformActive || handState.releaseBlend.active || soft_contact_math::length(handState.correction) > kCorrectionClearDistance) {
                const auto releaseStep = applyReleaseBlend(handState, isLeft, cleanHandInput.rawHandWorld, frame.deltaSeconds);
                if (releaseStep == ReleaseBlendStep::Applied) {
                    if (handState.lastContactSourceIsWeapon) {
                        _weaponWorldCorrection =
                            handState.correction;
                        _weaponWorldCorrectionActive =
                            true;
                        _weaponWorldCorrectionHandIsLeft =
                            isLeft;
                    }
                    return;
                }
                if (releaseStep == ReleaseBlendStep::Failed) {
                    ROCK_LOG_SAMPLE_WARN(Hand,
                        1000,
                        "{} soft contact release blend apply failed; clearing visual contact state",
                        isLeft ? "Left" : "Right");
                }
                clearHand(isLeft);
                return;
            }

            handState.state = ContactState::Inactive;
        };

        if (channels.handPushback) {
            solveForHand(false, frame.right, rightHand);
            solveForHand(true, frame.left, leftHand);
        }

        /*
         * Weapon hull/world contact is solved in its own generation-keyed
         * channel. It must not share probe history or a cached plane with the
         * firing palm: otherwise a hand manifold and a muzzle manifold replace
         * one another and the complete gun/hand assembly flickers.
         *
         * Sampling generated hull points is deliberately event-driven. The
         * scene-tree walk is only paid while there is fresh native weapon/world
         * evidence or an already-active cached stop plane, not on every armed
         * frame.
         */
        const std::uint64_t currentWeaponGenerationKey =
            weaponCollision && weaponNode
                ? weaponCollision->
                      getCurrentWeaponGenerationKey()
                : 0;
        if (!channels.weaponWallCollision ||
            currentWeaponGenerationKey == 0 ||
            !weaponCollision ||
            !weaponNode) {
            _weapon = {};
            _weaponGenerationKey = 0;
        } else {
            if (_weaponGenerationKey !=
                currentWeaponGenerationKey) {
                _weapon = {};
                _weaponGenerationKey =
                    currentWeaponGenerationKey;
            }

            bool hasFreshCurrentWeaponEvidence = false;
            using contact_evidence::
                NativeContactEndpointKind;
            for (std::uint32_t i = 0;
                 i < nativeContactEvidence.count &&
                 i < nativeContactEvidence.records.size();
                 ++i) {
                const auto& evidence =
                    nativeContactEvidence.records[i];
                if (evidence.valid &&
                    evidence.sourceKind ==
                        NativeContactEndpointKind::Weapon &&
                    evidence.targetKind ==
                        NativeContactEndpointKind::
                            WorldSurface &&
                    evidence.quality ==
                        contact_evidence::
                            NativeContactQuality::RawPoint &&
                    contact_evidence::isFrameFresh(
                        nativeContactEvidence.currentFrame,
                        evidence.frame,
                        kNativeWorldContactMaxAgeFrames) &&
                    weaponCollision->
                        isWeaponBodyIdAtomic(
                            evidence.sourceBodyId)) {
                    hasFreshCurrentWeaponEvidence = true;
                    break;
                }
            }

            if (_weapon.cachedWorldPlane.active ||
                hasFreshCurrentWeaponEvidence) {
                const bool previouslyInContact =
                    _weapon.state ==
                        ContactState::Touching ||
                    _weapon.state ==
                        ContactState::Penetrating;
                const HandFrameInput& firingHandInput =
                    weaponFiringHandIsLeft
                        ? frame.left
                        : frame.right;
                const RE::NiPoint3 fallbackNormal =
                    soft_contact_math::normalizeOr(
                        firingHandInput.palmNormalWorld,
                        RE::NiPoint3(
                            0.0f,
                            0.0f,
                            1.0f));
                Candidate weaponBest =
                    solveWorldStaticContact(
                        frame.bhkWorld,
                        frame.hknpWorld,
                        nativeContactEvidence,
                        _weapon,
                        weaponFiringHandIsLeft,
                        firingHandInput,
                        RE::NiPoint3{},
                        false,
                        true,
                        weaponCollision,
                        weaponNode,
                        fallbackNormal,
                        frame.deltaSeconds);

                if (weaponBest.valid &&
                    !previouslyInContact &&
                    (!std::isfinite(
                         weaponBest.contact.penetration) ||
                     weaponBest.contact.penetration >
                         maxWorldCorrection())) {
                    ROCK_LOG_SAMPLE_WARN(
                        Weapon,
                        1000,
                        "Weapon wall contact rejected implausible "
                        "first witness probe={} body={} "
                        "penetration={:.2f} maxCorrection={:.2f}",
                        weaponBest.contact.movableId,
                        weaponBest.contact.targetId,
                        weaponBest.contact.penetration,
                        maxWorldCorrection());
                    _weapon.cachedWorldPlane = {};
                    weaponBest = {};
                }

                if (weaponBest.valid) {
                    updateWorldContactHaptics(
                        _weapon,
                        weaponFiringHandIsLeft,
                        true,
                        weaponBest.approachSpeedGameUnits,
                        frame.deltaSeconds);
                    _weapon.correction =
                        correctionForCandidate(
                            weaponBest);
                    _weapon.lastContactKind =
                        weaponBest.kind;
                    _weapon.lastContactSourceIsWeapon =
                        true;
                    _weapon.state =
                        weaponBest.contact.penetration >=
                                0.5f
                            ? ContactState::Penetrating
                            : ContactState::Touching;
                    _weaponWorldCorrection =
                        _weapon.correction;
                    _weaponWorldCorrectionActive =
                        soft_contact_math::length(
                            _weaponWorldCorrection) >
                        kCorrectionClearDistance;
                    _weaponWorldCorrectionHandIsLeft =
                        weaponFiringHandIsLeft;
                    addDebugContact(
                        _debugSnapshot,
                        weaponFiringHandIsLeft,
                        _weapon.state,
                        weaponBest,
                        _weapon.correction);
                } else {
                    updateWorldContactHaptics(
                        _weapon,
                        weaponFiringHandIsLeft,
                        false,
                        0.0f,
                        frame.deltaSeconds);
                    _weapon.correction = {};
                    _weapon.state =
                        ContactState::Inactive;
                }
            } else {
                _weapon = {};
            }
        }

        (void)rightHandWeaponEquipped;
        (void)leftSupportGripActive;
        _debugSnapshot.rightState = _hands[0].state;
        _debugSnapshot.leftState = _hands[1].state;
    }
}
