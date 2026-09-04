#include "physics-interaction/contact/SoftContactRuntime.h"

#include "ROCKMain.h"
#include "RockConfig.h"
#include "RockUtils.h"
#include "physics-interaction/contact/ContactTargetIdentity.h"
#include "physics-interaction/contact/SoftContactPolicy.h"
#include "physics-interaction/contact/SoftContactWorldPolicy.h"
#include "physics-interaction/collision/CollisionLayerPolicy.h"
#include "physics-interaction/core/PhysicsFrameContext.h"
#include "physics-interaction/TransformMath.h"
#include "physics-interaction/PhysicsLog.h"
#include "physics-interaction/hand/Hand.h"
#include "physics-interaction/hand/HandSkeleton.h"
#include "physics-interaction/hand/HandVisual.h"
#include "physics-interaction/hand/DynamicHandCollision.h"
#include "physics-interaction/native/HavokRuntime.h"
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "physics-interaction/native/PhysicsShapeCast.h"
#include "physics-interaction/native/PhysicsUtils.h"
#include "physics-interaction/weapon/WeaponCollision.h"
#include "physics-interaction/weapon/WeaponWallSweepPolicy.h"

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
        constexpr std::uint32_t kMaxHandWorldProbeCastsPerFrame = 4;
        // Weapon-root extrema (including muzzle/breech) are a fixed six-cast
        // CCD bank; four additional noncritical samples rotate only during
        // acquisition. This remains bounded while closing sparse rotational
        // and cached-plane blind spots.
        constexpr std::uint32_t kMaxCriticalWeaponProbeCastsPerFrame =
            static_cast<std::uint32_t>(
                soft_contact_policy::
                    kCriticalWeaponWorldProbeCapacity);
        constexpr std::uint32_t kMaxRotatingWeaponProbeCastsPerFrame =
            static_cast<std::uint32_t>(
                soft_contact_policy::
                    kRotatingWeaponWorldProbeBudget);
        constexpr std::uint32_t kMaxWeaponProbeCastsPerFrame =
            kMaxCriticalWeaponProbeCastsPerFrame +
            kMaxRotatingWeaponProbeCastsPerFrame;
        constexpr std::uint32_t kPriorityWorldProbeCount = 2;
        constexpr float kDefaultWorldContactPaddingGameUnits = 0.35f;
        constexpr float kDefaultWorldPostReleaseReentryMinApproachDistanceGameUnits = 0.025f;
        constexpr float kDefaultWorldCachedPlaneMaxTangentDriftGameUnits = 10.0f;
        constexpr float kDefaultWorldCachedPlaneMaxClearDistanceGameUnits = 18.0f;
        constexpr std::uint32_t kNativeWorldContactMaxAgeFrames = 2;
        constexpr std::uint32_t kWorldProbeIdRightBase = 0x5000u;
        constexpr std::uint32_t kWorldProbeIdLeftBase = 0x6000u;
        constexpr std::uint32_t kWorldProbeIdExactWeapon =
            soft_contact_policy::kExactWeaponWorldProbeId;
        constexpr std::uint32_t kWorldProbeIdExactWeaponSecondary =
            soft_contact_policy::
                kSecondaryExactWeaponWorldProbeId;
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
            bool correctionOverrideValid = false;
            RE::NiPoint3 correctionOverride{};
            bool validatedWeaponSweep = false;
            bool validatedWeaponHandStop = false;
            bool targetIsDynamicHand = false;
            bool targetHandIsLeft = false;
            std::uint32_t targetDynamicHandBodyId =
                contact_evidence::kInvalidBodyId;
            std::uint32_t targetHandCollisionLayer =
                collision_layer_policy::FO4_LAYER_UNIDENTIFIED;
            float weaponSweepFraction = 1.0f;
            float weaponSafeSweepFraction = 1.0f;
            bool hasBlockedWeaponWorld = false;
            RE::NiTransform blockedWeaponWorld{};
            RE::NiPoint3 blockedSurfacePointWorld{};
            RE::NiPoint3 blockedSurfaceNormalWorld{};
            std::uint32_t blockedSourceBodyId =
                kInvalidWeaponSourceBodyId;
            bool blockedAnchorUsesSourceLocal = false;
            RE::NiPoint3 blockedAnchorLocal{};
            float blockedProbeRadius = 0.0f;
        };

        struct WeaponCandidateManifold
        {
            soft_contact_policy::WeaponCorrectionManifold<
                RE::NiPoint3>
                corrections{};
            std::array<Candidate,
                soft_contact_policy::
                    WeaponCorrectionManifold<
                        RE::NiPoint3>::kCapacity>
                candidates{};
            std::array<bool,
                soft_contact_policy::
                    WeaponCorrectionManifold<
                        RE::NiPoint3>::kCapacity>
                candidateValid{};
            Candidate earliestStopCandidate{};
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
            std::uint64_t identity = 0;
            bool sourceIsWeapon = false;
            bool criticalWeaponSweep = false;
            std::uint32_t weaponSourceBodyId =
                kInvalidWeaponSourceBodyId;
            bool weaponAnchorValid = false;
            bool weaponAnchorUsesSourceLocal = false;
            RE::NiPoint3 weaponAnchorLocal{};
        };

        struct WeaponSampleProbeLayout
        {
            std::uint64_t signature = 0;
            std::uint32_t count = 0;
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
            bool isLeft)
        {
            if (outCount >= outProbes.size() || !soft_contact_math::isFinite(position) || !std::isfinite(radius) || radius <= 0.0f) {
                return;
            }

            auto& probe = outProbes[outCount];
            probe.valid = true;
            probe.position = position;
            probe.radius = radius;
            probe.id = makeWorldProbeId(isLeft, outCount);
            probe.stateIndex = outCount;
            probe.identity = probe.id;
            ++outCount;
        }

        bool addExactWeaponWorldContactProbe(
            std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& outProbes,
            std::uint32_t& outCount,
            const RE::NiPoint3& position,
            float radius,
            std::uint32_t sourceBodyId,
            const RE::NiPoint3& anchorLocal,
            bool anchorUsesSourceLocal,
            std::uint32_t probeId =
                kWorldProbeIdExactWeapon)
        {
            if (outCount >= outProbes.size() ||
                !soft_contact_math::isFinite(position) ||
                !soft_contact_math::isFinite(anchorLocal) ||
                !std::isfinite(radius) ||
                radius <= 0.0f ||
                sourceBodyId == kInvalidWeaponSourceBodyId ||
                !soft_contact_policy::
                    isExactWeaponWorldProbeId(probeId)) {
                return false;
            }

            auto& probe = outProbes[outCount];
            probe.valid = true;
            probe.position = position;
            probe.radius = radius;
            probe.id = probeId;
            probe.stateIndex =
                soft_contact_policy::
                    exactWeaponWorldProbeStateIndex(probeId);
            probe.identity =
                soft_contact_policy::
                    makeExactWeaponWorldProbeIdentity(
                        sourceBodyId,
                        probeId);
            probe.sourceIsWeapon = true;
            probe.weaponSourceBodyId = sourceBodyId;
            probe.weaponAnchorValid = true;
            probe.weaponAnchorUsesSourceLocal =
                anchorUsesSourceLocal;
            probe.weaponAnchorLocal = anchorLocal;
            ++outCount;
            return true;
        }

        WeaponSampleProbeLayout addSampledWeaponWorldContactProbes(
            const WeaponCollision* weaponCollision,
            RE::NiNode* weaponNode,
            std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& outProbes,
            std::uint32_t& outCount)
        {
            if (!weaponCollision || !weaponNode || outCount != 0) {
                return {};
            }

            constexpr std::size_t kSampleCapacity =
                soft_contact_policy::
                    kSampledWeaponWorldProbeCapacity;
            std::array<RE::NiPoint3, kSampleCapacity>
                sampleWorldPoints{};
            std::array<float, kSampleCapacity>
                sampleRadii{};
            std::array<std::uint64_t, kSampleCapacity>
                sampleIdentities{};
            std::array<std::uint8_t, kSampleCapacity>
                sampleCriticalSweepFlags{};
            std::array<std::uint32_t, kSampleCapacity>
                sampleSourceBodyIds{};
            std::array<RE::NiPoint3, kSampleCapacity>
                sampleAnchorLocals{};
            std::array<std::uint8_t, kSampleCapacity>
                sampleAnchorUsesSourceLocal{};
            const std::uint32_t sampleCount =
                (std::min)(
                    weaponCollision->copyInteractionCollisionSamples(
                        weaponNode,
                        sampleWorldPoints.data(),
                        sampleRadii.data(),
                        static_cast<std::uint32_t>(
                            kSampleCapacity),
                        sampleIdentities.data(),
                        sampleCriticalSweepFlags.data(),
                        sampleSourceBodyIds.data(),
                        sampleAnchorLocals.data(),
                        sampleAnchorUsesSourceLocal.data()),
                    static_cast<std::uint32_t>(
                        kSampleCapacity));

            WeaponSampleProbeLayout layout{};
            layout.count = sampleCount;
            for (std::uint32_t ordinal = 0;
                 ordinal < sampleCount &&
                 outCount < outProbes.size();
                 ++ordinal) {
                layout.signature =
                    soft_contact_policy::
                        appendWeaponSampleLayoutIdentity(
                            layout.signature,
                            sampleIdentities[ordinal]);
                const auto& position =
                    sampleWorldPoints[ordinal];
                const float radius =
                    sampleRadii[ordinal];
                if (!soft_contact_math::isFinite(position) ||
                    !std::isfinite(radius) ||
                    radius < 0.0f) {
                    continue;
                }

                auto& probe = outProbes[outCount];
                probe.valid = true;
                probe.position = position;
                probe.radius = (std::max)(radius, 0.5f);
                probe.id =
                    soft_contact_policy::
                        sampledWeaponWorldProbeId(ordinal);
                probe.stateIndex = ordinal;
                probe.identity = sampleIdentities[ordinal];
                probe.sourceIsWeapon = true;
                probe.weaponSourceBodyId =
                    sampleSourceBodyIds[ordinal];
                probe.weaponAnchorValid =
                    probe.weaponSourceBodyId !=
                        kInvalidWeaponSourceBodyId &&
                    soft_contact_math::isFinite(
                        sampleAnchorLocals[ordinal]);
                probe.weaponAnchorUsesSourceLocal =
                    sampleAnchorUsesSourceLocal[ordinal] != 0;
                probe.weaponAnchorLocal =
                    sampleAnchorLocals[ordinal];
                probe.criticalWeaponSweep =
                    sampleCriticalSweepFlags[ordinal] != 0;
                ++outCount;
            }
            if (layout.count != 0) {
                layout.signature =
                    soft_contact_policy::
                        appendWeaponSampleLayoutIdentity(
                            layout.signature,
                            layout.count);
            }
            return layout;
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
            handState.secondaryCachedWorldPlane = {};
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

        RE::NiPoint3 baseCorrectionForCandidate(
            const Candidate& candidate)
        {
            const float correctionLimit =
                soft_contact_policy::worldContactCorrectionLimit(
                    candidate.source ==
                        CandidateSource::CachedWorldPlane,
                    candidate.validatedWeaponSweep,
                    candidate.contact.penetration,
                    maxWorldCorrection());
            return soft_contact_math::projectTrackedMagnetCorrection(
                candidate.contact.normal,
                candidate.contact.penetration,
                correctionLimit);
        }

        RE::NiPoint3 correctionForCandidate(const Candidate& candidate)
        {
            if (candidate.correctionOverrideValid &&
                soft_contact_math::isFinite(
                    candidate.correctionOverride)) {
                return candidate.correctionOverride;
            }
            return baseCorrectionForCandidate(candidate);
        }

        void admitWeaponCandidateCorrection(
            WeaponCandidateManifold& manifold,
            const Candidate& candidate)
        {
            if (!candidate.valid ||
                !candidate.sourceIsWeapon ||
                candidate.kind != ContactKind::WorldStatic) {
                return;
            }
            if (candidate.hasBlockedWeaponWorld &&
                (candidate.validatedWeaponSweep ||
                 candidate.validatedWeaponHandStop) &&
                (!manifold.earliestStopCandidate.valid ||
                    !manifold.earliestStopCandidate.
                        hasBlockedWeaponWorld ||
                    candidate.weaponSafeSweepFraction +
                            weapon_wall_sweep_policy::
                                kSweepStartFractionEpsilon <
                        manifold.earliestStopCandidate.
                            weaponSafeSweepFraction)) {
                manifold.earliestStopCandidate = candidate;
            }
            const auto correction =
                baseCorrectionForCandidate(candidate);
            const float distance =
                soft_contact_math::length(correction);
            if (!soft_contact_policy::
                    admitWeaponCorrectionPlane(
                        manifold.corrections,
                        candidate.contact.normal,
                        distance)) {
                return;
            }

            const float normalLengthSquared =
                soft_contact_policy::weaponCorrectionPointDot(
                    candidate.contact.normal,
                    candidate.contact.normal);
            if (!std::isfinite(normalLengthSquared) ||
                normalLengthSquared <= 1.0e-8f) {
                return;
            }
            const float inverseNormalLength =
                1.0f / std::sqrt(normalLengthSquared);
            const RE::NiPoint3 candidateNormal{
                candidate.contact.normal.x * inverseNormalLength,
                candidate.contact.normal.y * inverseNormalLength,
                candidate.contact.normal.z * inverseNormalLength,
            };
            for (std::size_t index = 0;
                 index < manifold.corrections.count &&
                 index < manifold.corrections.planes.size();
                 ++index) {
                const auto& admittedPlane =
                    manifold.corrections.planes[index];
                const float alignment =
                    soft_contact_policy::
                        weaponCorrectionPointDot(
                            candidateNormal,
                            admittedPlane.normal);
                if (alignment < 0.985f ||
                    distance + 0.0001f <
                        admittedPlane.correctionDistance) {
                    continue;
                }
                manifold.candidates[index] = candidate;
                manifold.candidateValid[index] = true;
                break;
            }
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
            const auto sweepWitness =
                weapon_wall_sweep_policy::classifySweepWitness(
                    true,
                    false,
                    false,
                    0,
                    1,
                    hit.fraction.storage);
            if (!sweepWitness.fractionValid) {
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
            candidate.validatedWeaponSweep =
                probe.sourceIsWeapon &&
                sweepWitness.validatedContinuousEntry;
            candidate.weaponSweepFraction =
                sweepWitness.sweepFraction;
            candidate.weaponSafeSweepFraction =
                sweepWitness.sweepFraction;
            return candidate;
        }

        Candidate makeExactWeaponWorldSweepCandidate(
            RE::bhkWorld* bhkWorld,
            RE::hknpWorld* hknpWorld,
            const WeaponCollision::InteractionWorldSweepHit& hit,
            const WeaponCollision* weaponCollision,
            RE::NiNode* weaponNode,
            float contactPadding,
            float skin,
            std::uint32_t probeId)
        {
            Candidate candidate{};
            if (!hit.valid || !weaponCollision || !weaponNode ||
                hit.sourceBodyId == kInvalidWeaponSourceBodyId ||
                hit.targetBodyId ==
                    soft_contact_world_policy::kInvalidWorldBodyId) {
                return candidate;
            }

            RE::NiPoint3 anchorLocal{};
            RE::NiPoint3 exactProbeWorld{};
            bool anchorUsesSourceLocal = false;
            float exactProbeRadius = 0.0f;
            if (!weaponCollision->
                    tryCaptureInteractionCollisionProbeAnchor(
                        weaponNode,
                        hit.sourceBodyId,
                        hit.weaponPointWorld,
                        anchorLocal,
                        anchorUsesSourceLocal,
                        exactProbeWorld,
                        exactProbeRadius)) {
                return candidate;
            }

            const float effectiveRadius =
                (std::max)(exactProbeRadius,
                    hit.weaponProbeRadiusGame) +
                soft_contact_math::sanitizeNonNegative(
                    contactPadding,
                    0.0f);
            auto contact = soft_contact_math::solvePointPlaneContact(
                exactProbeWorld,
                hit.surfacePointWorld,
                hit.surfaceNormalWorld,
                effectiveRadius,
                skin,
                probeId,
                hit.targetBodyId);
            if (!contact.active) {
                return candidate;
            }

            candidate.valid = true;
            candidate.kind = ContactKind::WorldStatic;
            candidate.source = CandidateSource::QueryWorld;
            candidate.contact = contact;
            candidate.sourceIsWeapon = true;
            candidate.approachSpeedGameUnits =
                hit.approachSpeedGameUnits;
            candidate.targetIdentity =
                contact_target_identity::resolveContactTarget(
                    bhkWorld,
                    hknpWorld,
                    hit.targetBodyId,
                    contact_evidence::
                        NativeContactEndpointKind::WorldSurface,
                    &hit.surfacePointWorld,
                    &hit.surfaceNormalWorld,
                    contactTargetResolutionOptions());
            candidate.weaponSourceBodyId = hit.sourceBodyId;
            candidate.weaponAnchorValid = true;
            candidate.weaponAnchorUsesSourceLocal =
                anchorUsesSourceLocal;
            candidate.weaponAnchorLocal = anchorLocal;
            candidate.weaponProbeRadius = (std::max)(
                exactProbeRadius,
                hit.weaponProbeRadiusGame);
            candidate.validatedWeaponSweep =
                hit.validatedContinuousEntry;
            candidate.weaponSweepFraction =
                hit.sweepFraction;
            candidate.weaponSafeSweepFraction =
                hit.safeSweepFraction;
            candidate.hasBlockedWeaponWorld =
                hit.hasBlockedWeaponWorld &&
                isFiniteTransform(hit.blockedWeaponWorld);
            if (candidate.hasBlockedWeaponWorld) {
                candidate.blockedWeaponWorld =
                    hit.blockedWeaponWorld;
                candidate.blockedSurfacePointWorld =
                    hit.surfacePointWorld;
                candidate.blockedSurfaceNormalWorld =
                    hit.surfaceNormalWorld;
                candidate.blockedSourceBodyId =
                    hit.sourceBodyId;
                candidate.blockedAnchorUsesSourceLocal =
                    anchorUsesSourceLocal;
                candidate.blockedAnchorLocal = anchorLocal;
                candidate.blockedProbeRadius = (std::max)(
                    exactProbeRadius,
                    hit.weaponProbeRadiusGame);
            }
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
                    if (previousProbe.history.observedValid &&
                        previousProbe.history.identity ==
                            probe.identity &&
                        soft_contact_math::isFinite(
                            previousProbe.history.previousObserved)) {
                        normalOrientationPoint =
                            previousProbe.history.previousObserved;
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
            if (approachSpeed <= 0.0f &&
                probe.stateIndex < handState.worldProbes.size() &&
                handState.worldProbes[probe.stateIndex].history.observedValid &&
                handState.worldProbes[probe.stateIndex].history.identity ==
                    probe.identity) {
                const float dt = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 1.0f / 240.0f, 0.1f);
                const auto sweepDelta = soft_contact_math::sub(
                    probe.position,
                    handState.worldProbes[probe.stateIndex].history.previousObserved);
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
            const bool rightHandFreeForWeaponStop,
            const bool leftHandFreeForWeaponStop,
            const std::array<bool, 2>&
                weaponHandFirstContactBySide,
            const std::array<std::uint32_t, 2>&
                weaponHandEvidenceLayerBySide,
            const WeaponCollision::
                WeaponRootDriveSegmentSnapshot&
                    weaponDriveSegment,
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
                const auto& storedEvidence = evidenceSnapshot.records[i];
                if (!storedEvidence.valid ||
                    !contact_evidence::isFrameFresh(
                        evidenceSnapshot.currentFrame,
                        storedEvidence.frame,
                        kNativeWorldContactMaxAgeFrames) ||
                    storedEvidence.quality !=
                        contact_evidence::NativeContactQuality::RawPoint) {
                    continue;
                }

                const bool storedSourceIsHand =
                    storedEvidence.sourceKind ==
                        NativeContactEndpointKind::RightHand ||
                    storedEvidence.sourceKind ==
                        NativeContactEndpointKind::LeftHand;
                const bool reciprocalWeaponHandStop =
                    includeWeaponProbes && !includeHandProbes &&
                    storedSourceIsHand &&
                    storedEvidence.targetKind ==
                        NativeContactEndpointKind::Weapon;
                const std::size_t reciprocalTargetHandIndex =
                    handIndex(
                        storedEvidence.sourceKind ==
                        NativeContactEndpointKind::LeftHand);
                if (reciprocalWeaponHandStop &&
                    storedEvidence.sourceLayer !=
                        weaponHandEvidenceLayerBySide[
                            reciprocalTargetHandIndex]) {
                    continue;
                }

                contact_evidence::NativeContactEvidenceRecord
                    reciprocalEvidence{};
                const auto* evidencePtr = &storedEvidence;
                bool reciprocalTargetHandFree = false;
                const bool reciprocalEvidenceCurrentFrame =
                    storedEvidence.frame == evidenceSnapshot.currentFrame;
                if (reciprocalWeaponHandStop) {
                    /*
                     * Keep the cache canonical as hand->weapon so hand-state
                     * invalidation remains correct. Invert only this frame's
                     * local copy for the independent weapon stop channel.
                     */
                    reciprocalEvidence = storedEvidence;
                    std::swap(
                        reciprocalEvidence.sourceBodyId,
                        reciprocalEvidence.targetBodyId);
                    std::swap(
                        reciprocalEvidence.sourceLayer,
                        reciprocalEvidence.targetLayer);
                    std::swap(
                        reciprocalEvidence.sourceFilterInfo,
                        reciprocalEvidence.targetFilterInfo);
                    std::swap(
                        reciprocalEvidence.sourceKind,
                        reciprocalEvidence.targetKind);
                    std::swap(
                        reciprocalEvidence.sourceIsLeft,
                        reciprocalEvidence.targetIsLeft);
                    std::swap(
                        reciprocalEvidence.sourceVelocityGame,
                        reciprocalEvidence.targetVelocityGame);
                    std::swap(
                        reciprocalEvidence.sourceVelocityValid,
                        reciprocalEvidence.targetVelocityValid);
                    reciprocalEvidence.contactNormalGame =
                        soft_contact_math::negate(
                            storedEvidence.contactNormalGame);
                    reciprocalTargetHandFree =
                        storedEvidence.sourceKind ==
                                NativeContactEndpointKind::LeftHand
                            ? leftHandFreeForWeaponStop
                            : rightHandFreeForWeaponStop;
                    evidencePtr = &reciprocalEvidence;
                }

                const auto& evidence = *evidencePtr;
                const bool targetIsWorldSurface =
                    evidence.targetKind ==
                    NativeContactEndpointKind::WorldSurface;
                const bool targetIsGeneratedWeapon =
                    evidence.targetKind ==
                    NativeContactEndpointKind::Weapon;
                const bool targetIsGeneratedHand =
                    evidence.targetKind ==
                        NativeContactEndpointKind::RightHand ||
                    evidence.targetKind ==
                        NativeContactEndpointKind::LeftHand;
                if ((!reciprocalWeaponHandStop &&
                     !soft_contact_policy::
                        shouldAdmitNativeVisualStopTarget(
                            targetIsWorldSurface,
                            targetIsGeneratedWeapon,
                            targetIsGeneratedHand,
                            includeHandProbes,
                            includeWeaponProbes))) {
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
                        kWorldProbeIdExactWeapon;
                    exactWeaponProbe.stateIndex =
                        soft_contact_policy::
                            kExactWeaponWorldProbeStateIndex;
                    exactWeaponProbe.identity =
                        soft_contact_policy::
                            makeExactWeaponWorldProbeIdentity(
                                evidence.sourceBodyId);
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

                Candidate candidate =
                    makeNativeWorldStaticCandidate(
                        bhkWorld,
                        hknpWorld,
                        evidence,
                        *probe,
                        handState,
                        fallbackNormal,
                        contactPadding,
                        skin,
                        deltaSeconds);
                if (reciprocalWeaponHandStop) {
                    RE::NiPoint3 weaponPointVelocity{};
                    RE::NiPoint3 contactRootLocal{};
                    RE::NiPoint3 previousContactWorld{};
                    RE::NiPoint3 currentContactWorld{};
                    bool weaponPointVelocityValid =
                        weaponDriveSegment.valid &&
                        weaponDriveSegment.generationKey != 0 &&
                        std::isfinite(
                            weaponDriveSegment.sourceDeltaSeconds) &&
                        weaponDriveSegment.sourceDeltaSeconds > 0.0f &&
                        isFiniteTransform(
                            weaponDriveSegment.previousRootWorld) &&
                        isFiniteTransform(
                            weaponDriveSegment.currentRootWorld);
                    if (weaponPointVelocityValid) {
                        contactRootLocal =
                            transform_math::worldPointToLocal(
                                weaponDriveSegment.currentRootWorld,
                                storedEvidence.contactPointGame);
                        previousContactWorld =
                            transform_math::localPointToWorld(
                                weaponDriveSegment.previousRootWorld,
                                contactRootLocal);
                        currentContactWorld =
                            transform_math::localPointToWorld(
                                weaponDriveSegment.currentRootWorld,
                                contactRootLocal);
                        weaponPointVelocity =
                            (currentContactWorld -
                             previousContactWorld) *
                            (1.0f /
                             weaponDriveSegment.sourceDeltaSeconds);
                        weaponPointVelocityValid =
                            soft_contact_math::isFinite(
                                contactRootLocal) &&
                            soft_contact_math::isFinite(
                                previousContactWorld) &&
                            soft_contact_math::isFinite(
                                currentContactWorld) &&
                            soft_contact_math::isFinite(
                                weaponPointVelocity);
                    }
                    const float weaponNormalSpeed =
                        soft_contact_math::dot(
                            weaponPointVelocity,
                            candidate.contact.normal);
                    const float handNormalSpeed =
                        soft_contact_math::dot(
                            storedEvidence.sourceVelocityGame,
                            candidate.contact.normal);
                    const auto directionalMotion =
                        soft_contact_policy::
                            resolveWeaponHandDirectionalMotion(
                                weaponNormalSpeed,
                                storedEvidence.sourceVelocityValid &&
                                        soft_contact_math::isFinite(
                                            storedEvidence.sourceVelocityGame)
                                    ? handNormalSpeed
                                    : 0.0f);
                    const float weaponApproachSpeed =
                        directionalMotion.
                            weaponApproachSpeedGameUnits;
                    const float handApproachSpeed =
                        directionalMotion.
                            handApproachSpeedGameUnits;
                    const float relativeClosingSpeed =
                        directionalMotion.
                            relativeClosingSpeedGameUnits;

                    /*
                     * The contact belongs to the queued root segment above,
                     * not the newer live scene pose. Reconstruct the hand
                     * plane's previous position from the sampled hand target
                     * velocity and solve the actual clear-to-contact fraction
                     * for the weapon anchor. This stops at first contact
                     * instead of rolling the gun and its firing hand all the
                     * way back by one low-FPS frame.
                     */
                    const RE::NiPoint3 currentSurfaceWorld =
                        storedEvidence.contactPointGame;
                    const RE::NiPoint3 previousSurfaceWorld =
                        soft_contact_math::sub(
                            currentSurfaceWorld,
                            soft_contact_math::mul(
                                storedEvidence.sourceVelocityGame,
                                weaponDriveSegment.sourceDeltaSeconds));
                    const float previousSignedDistance =
                        soft_contact_math::dot(
                            soft_contact_math::sub(
                                previousContactWorld,
                                previousSurfaceWorld),
                            candidate.contact.normal);
                    const float currentSignedDistance =
                        soft_contact_math::dot(
                            soft_contact_math::sub(
                                currentContactWorld,
                                currentSurfaceWorld),
                            candidate.contact.normal);
                    const float requiredClearance =
                        soft_contact_math::sanitizeNonNegative(
                            candidate.weaponProbeRadius,
                            0.0f) +
                        soft_contact_math::sanitizeNonNegative(
                            contactPadding,
                            0.0f) +
                        soft_contact_math::sanitizeNonNegative(
                            skin,
                            0.0f);
                    const auto entryFraction =
                        soft_contact_policy::
                            resolveWeaponHandEntryFraction(
                                previousSignedDistance,
                                currentSignedDistance,
                                requiredClearance);
                    const bool firstWeaponHandContactFrame =
                        weaponHandFirstContactBySide[
                            reciprocalTargetHandIndex];
                    /*
                     * A first callback can arrive after the discrete hand
                     * proxy has already crossed its clearance plane.  In that
                     * case there is no trustworthy entry fraction, but the
                     * generation-current drive segment still gives us a
                     * continuous, non-teleporting pose to hold.  Rejecting the
                     * whole weapon stop here lets the competing hand response
                     * fire once, move the offhand away, and destroy the very
                     * contact that would have allowed acquisition next frame.
                     * Keep the exact TOI when it exists; otherwise stop at the
                     * segment end.  Directional arbitration below still has
                     * to prove that the weapon, rather than the hand, closed
                     * the contact.
                     */
                    const bool hasContinuousStopPose =
                        weaponDriveSegment.valid &&
                        weaponPointVelocityValid &&
                        isFiniteTransform(
                            weaponDriveSegment.currentRootWorld);
                    const bool weaponHandStopAccepted =
                        candidate.valid &&
                        soft_contact_policy::
                            shouldAcquireWeaponHandStop(
                                hasContinuousStopPose,
                                weaponCollision &&
                                    weaponCollision->
                                        isWeaponBodyIdAtomic(
                                            evidence.sourceBodyId),
                                reciprocalTargetHandFree,
                                firstWeaponHandContactFrame,
                                reciprocalEvidenceCurrentFrame,
                                weaponPointVelocityValid &&
                                    storedEvidence.sourceVelocityValid &&
                                    directionalMotion.valid,
                                weaponApproachSpeed,
                                handApproachSpeed,
                                relativeClosingSpeed);
                    if (!weaponHandStopAccepted) {
                        ROCK_LOG_SAMPLE_WARN(
                            Weapon,
                            750,
                            "Weapon/hand stop rejected hand={} candidate={} drive={} pointVelocity={} handVelocity={} free={} currentFrame={} first={} entry={} weaponApproach={:.2f} handApproach={:.2f} relativeClosing={:.2f}",
                            storedEvidence.sourceKind ==
                                    NativeContactEndpointKind::LeftHand
                                ? "Left"
                                : "Right",
                            candidate.valid ? "yes" : "no",
                            weaponDriveSegment.valid ? "yes" : "no",
                            weaponPointVelocityValid ? "yes" : "no",
                            storedEvidence.sourceVelocityValid ? "yes" : "no",
                            reciprocalTargetHandFree ? "yes" : "no",
                            reciprocalEvidenceCurrentFrame ? "yes" : "no",
                            firstWeaponHandContactFrame ? "yes" : "no",
                            entryFraction.valid ? "yes" : "no",
                            weaponApproachSpeed,
                            handApproachSpeed,
                            relativeClosingSpeed);
                        continue;
                    }

                    candidate.validatedWeaponHandStop = true;
                    candidate.targetIsDynamicHand = true;
                    candidate.targetHandIsLeft =
                        storedEvidence.sourceKind ==
                        NativeContactEndpointKind::LeftHand;
                    candidate.targetDynamicHandBodyId =
                        evidence.targetBodyId;
                    candidate.targetHandCollisionLayer =
                        storedEvidence.sourceLayer;
                    candidate.weaponAnchorValid = true;
                    candidate.weaponAnchorUsesSourceLocal = false;
                    candidate.weaponAnchorLocal = contactRootLocal;
                    candidate.hasBlockedWeaponWorld = true;
                    // A late acquisition is still inside the same continuous
                    // drive segment. resolveWeaponHandEntryFraction returns
                    // zero when its previous endpoint already overlapped, so
                    // use it on every acquisition. If native geometry cannot
                    // reconstruct a TOI, the previous endpoint is the
                    // conservative non-clipping fallback.
                    const float safeEntryFraction =
                        entryFraction.valid ?
                            entryFraction.fraction :
                            0.0f;
                    candidate.blockedWeaponWorld =
                        hand_visual_lerp_math::interpolateTransform(
                            weaponDriveSegment.previousRootWorld,
                            weaponDriveSegment.currentRootWorld,
                            safeEntryFraction);
                    candidate.blockedSurfacePointWorld =
                        candidate.contact.targetPoint;
                    candidate.blockedSurfaceNormalWorld =
                        candidate.contact.normal;
                    candidate.blockedSourceBodyId =
                        candidate.weaponSourceBodyId;
                    candidate.blockedAnchorUsesSourceLocal =
                        false;
                    candidate.blockedAnchorLocal =
                        contactRootLocal;
                    candidate.blockedProbeRadius =
                        candidate.weaponProbeRadius;
                    candidate.weaponSweepFraction =
                        safeEntryFraction;
                    candidate.weaponSafeSweepFraction =
                        safeEntryFraction;
                }
                keepStrongerCandidate(best, candidate);
            }
            return best;
        }

        CachedWorldPlaneResult solveCachedWorldPlaneContact(auto& handState,
            auto& cachedPlane,
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount,
            float contactPadding,
            float skin,
            float maxTangentDrift,
            float maxClearDistance,
            float deltaSeconds)
        {
            CachedWorldPlaneResult result{};
            if (!cachedPlane.active) {
                return result;
            }
            result.hadCachedPlane = true;
            result.normal = cachedPlane.normal;

            const WorldContactProbe* probe = nullptr;
            for (std::uint32_t i = 0; i < probeCount && i < probes.size(); ++i) {
                if (probes[i].valid && probes[i].id == cachedPlane.probeId) {
                    probe = &probes[i];
                    break;
                }
            }

            if (!probe) {
                cachedPlane = {};
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
                    cachedPlane.surfacePoint) ||
                !std::isfinite(maxClearDistance) ||
                maxClearDistance <= 0.0f) {
                cachedPlane = {};
                result.leftContact = true;
                return result;
            }

            const float effectiveProbeRadius = probe->radius + soft_contact_math::sanitizeNonNegative(contactPadding, 0.0f);
            auto contact = soft_contact_math::solvePointPlaneContact(
                probe->position,
                cachedPlane.surfacePoint,
                cachedPlane.normal,
                effectiveProbeRadius,
                skin,
                probe->id,
                cachedPlane.bodyId);

            /*
             * This is cached evidence, not a lock. The current tracked probe
             * must still be penetrating the cached plane this frame; otherwise
             * the external visual authority is released immediately.
             */
            if (!contact.active) {
                cachedPlane = {};
                result.leftContact = true;
                return result;
            }

            if (!soft_contact_math::withinTangentDriftLimit(
                    contact.targetPoint,
                    cachedPlane.surfacePoint,
                    cachedPlane.normal,
                    maxTangentDrift)) {
                cachedPlane = {};
                result.leftContact = true;
                return result;
            }

            float approachSpeed = cachedPlane.approachSpeedGameUnits;
            if (probe->stateIndex < handState.worldProbes.size() &&
                handState.worldProbes[probe->stateIndex].history.observedValid) {
                const float dt = std::clamp(std::isfinite(deltaSeconds) ? deltaSeconds : (1.0f / 90.0f), 1.0f / 240.0f, 0.1f);
                const auto sweepDelta = soft_contact_math::sub(
                    probe->position,
                    handState.worldProbes[probe->stateIndex].history.previousObserved);
                const auto velocity = soft_contact_math::mul(sweepDelta, 1.0f / dt);
                approachSpeed = std::max(0.0f, -soft_contact_math::dot(velocity, contact.normal));
            }

            result.candidate.valid = true;
            result.candidate.kind = ContactKind::WorldStatic;
            result.candidate.source = CandidateSource::CachedWorldPlane;
            result.candidate.contact = contact;
            result.candidate.sourceIsWeapon =
                cachedPlane.sourceIsWeapon;
            result.candidate.approachSpeedGameUnits = approachSpeed;
            result.candidate.targetIdentity = cachedPlane.targetIdentity;
            result.candidate.weaponSourceBodyId =
                cachedPlane.weaponSourceBodyId;
            result.candidate.weaponAnchorValid =
                cachedPlane.weaponAnchorValid;
            result.candidate.weaponAnchorUsesSourceLocal =
                cachedPlane.
                    weaponAnchorUsesSourceLocal;
            result.candidate.weaponAnchorLocal =
                cachedPlane.weaponAnchorLocal;
            result.candidate.weaponProbeRadius =
                cachedPlane.weaponProbeRadius;
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
            float deltaSeconds,
            WeaponCandidateManifold* weaponCandidateManifold = nullptr)
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

                const Candidate candidate =
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
                        deltaSeconds);
                if (weaponCandidateManifold) {
                    admitWeaponCandidateCorrection(
                        *weaponCandidateManifold,
                        candidate);
                }
                keepStrongerCandidate(best, candidate);
            }

            return best;
        }

        void storeWorldProbePositions(auto& handState,
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount,
            float deltaSeconds)
        {
            std::array<bool, SoftContactRuntime::kMaxWorldContactProbesPerHand>
                stateSeen{};
            for (std::uint32_t probeIndex = 0;
                 probeIndex < probeCount && probeIndex < probes.size();
                 ++probeIndex) {
                const auto& probe = probes[probeIndex];
                if (!probe.valid ||
                    probe.stateIndex >= handState.worldProbes.size()) {
                    continue;
                }

                auto& state =
                    handState.worldProbes[probe.stateIndex];
                if (state.history.identity != 0 &&
                    state.history.identity != probe.identity) {
                    state = {};
                }
                if (soft_contact_policy::
                        sparseProbeObservationIsDiscontinuous(
                            state.history,
                            probe.position,
                            probe.identity,
                            deltaSeconds)) {
                    // Observe every sample every frame, including probes that
                    // were outside this frame's rotating cast budget. Rebase
                    // here so a pose/menu teleport cannot be resurrected later
                    // as an apparently continuous multi-frame weapon sweep.
                    state = {};
                }
                (void)soft_contact_policy::observeSparseProbe(
                    state.history,
                    probe.position,
                    probe.identity);
                stateSeen[probe.stateIndex] = true;
            }

            for (std::size_t i = 0; i < handState.worldProbes.size(); ++i) {
                if (!stateSeen[i]) {
                    handState.worldProbes[i] = {};
                }
            }
        }

        void rebaseWorldProbeQueryPositions(auto& handState,
            const std::array<WorldContactProbe, SoftContactRuntime::kMaxWorldContactProbesPerHand>& probes,
            std::uint32_t probeCount)
        {
            for (std::uint32_t probeIndex = 0;
                 probeIndex < probeCount && probeIndex < probes.size();
                 ++probeIndex) {
                const auto& probe = probes[probeIndex];
                if (!probe.valid ||
                    probe.stateIndex >= handState.worldProbes.size()) {
                    continue;
                }
                auto& state = handState.worldProbes[probe.stateIndex];
                if (state.history.identity != 0 &&
                    state.history.identity != probe.identity) {
                    state = {};
                }
                soft_contact_policy::commitSparseProbeQuery(
                    state.history,
                    probe.position,
                    probe.identity);
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
            const bool rightHandFreeForWeaponStop,
            const bool leftHandFreeForWeaponStop,
            const std::array<bool, 2>&
                weaponHandFirstContactBySide,
            const std::array<std::uint32_t, 2>&
                weaponHandEvidenceLayerBySide,
            const Hand& rightHand,
            const Hand& leftHand,
            const DynamicHandCollisionRuntime* dynamicHandCollision,
            const WeaponCollision* weaponCollision,
            RE::NiNode* weaponNode,
            const RE::NiPoint3& fallbackNormal,
            float deltaSeconds)
        {
            Candidate best{};
            WeaponCandidateManifold weaponCandidateManifold{};
            if (includeWeaponProbes) {
                handState.weaponAuthorityDiscontinuityThisFrame = false;
            }
            const auto admitCandidate =
                [&](const Candidate& candidate) {
                    keepStrongerCandidate(best, candidate);
                    if (includeWeaponProbes) {
                        admitWeaponCandidateCorrection(
                            weaponCandidateManifold,
                            candidate);
                    }
                };
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

            WeaponSampleProbeLayout weaponSampleLayout{};
            if (includeWeaponProbes) {
                weaponSampleLayout =
                    addSampledWeaponWorldContactProbes(
                        weaponCollision,
                        weaponNode,
                    probes,
                    probeCount);

                const bool layoutChanged =
                    handState.weaponSampleLayoutInitialized &&
                    (handState.weaponSampleLayoutSignature !=
                         weaponSampleLayout.signature ||
                     handState.weaponSampleLayoutCount !=
                         weaponSampleLayout.count);
                if (layoutChanged) {
                    clearWorldContactState(handState);
                    handState.worldProbeCastCursor = 0;
                    handState.weaponAuthorityDiscontinuityThisFrame = true;
                }
                handState.weaponSampleLayoutInitialized = true;
                handState.weaponSampleLayoutSignature =
                    weaponSampleLayout.signature;
                handState.weaponSampleLayoutCount =
                    weaponSampleLayout.count;
            }
            const std::uint32_t sampledProbeCount = probeCount;

            const auto addCachedExactWeaponProbe =
                [&](const auto& cachedPlane,
                    const std::uint32_t fallbackExactProbeId) {
                    if (!includeWeaponProbes ||
                        !cachedPlane.active ||
                        !cachedPlane.sourceIsWeapon ||
                        !cachedPlane.weaponAnchorValid ||
                        !weaponCollision || !weaponNode) {
                        return;
                    }
                    RE::NiPoint3 cachedProbeWorld{};
                    float cachedProbeRadius = 0.0f;
                    if (!weaponCollision->
                            tryResolveInteractionCollisionProbeAnchor(
                                weaponNode,
                                cachedPlane.weaponSourceBodyId,
                                cachedPlane.weaponAnchorLocal,
                                cachedPlane.
                                    weaponAnchorUsesSourceLocal,
                                cachedProbeWorld,
                                cachedProbeRadius)) {
                        return;
                    }
                    (void)addExactWeaponWorldContactProbe(
                        probes,
                        probeCount,
                        cachedProbeWorld,
                        (std::max)(cachedProbeRadius, 0.5f),
                        cachedPlane.weaponSourceBodyId,
                        cachedPlane.weaponAnchorLocal,
                        cachedPlane.weaponAnchorUsesSourceLocal,
                        fallbackExactProbeId);
                };
            addCachedExactWeaponProbe(
                handState.cachedWorldPlane,
                kWorldProbeIdExactWeapon);
            addCachedExactWeaponProbe(
                handState.secondaryCachedWorldPlane,
                kWorldProbeIdExactWeaponSecondary);

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

            bool weaponRootDiscontinuity = false;
            bool exactPreviousClearWeaponWorldValid = false;
            RE::NiTransform exactPreviousClearWeaponWorld{};
            bool previousWeaponStopWorldValid = false;
            RE::NiTransform previousWeaponStopWorld{};
            WeaponCollision::WeaponRootDriveSegmentSnapshot
                weaponDriveSegment{};
            if (includeWeaponProbes && weaponCollision &&
                weaponNode) {
                weaponDriveSegment = weaponCollision->
                    getWeaponRootDriveSegmentSnapshot();
                const auto exactSweep = weaponCollision->
                    sweepInteractionCollisionAgainstWorld(
                        world,
                        weaponNode,
                        dt,
                        contactPadding + skin,
                        handState.state == ContactState::Touching ||
                            handState.state == ContactState::Penetrating ||
                            handState.cachedWorldPlane.active ||
                            handState.secondaryCachedWorldPlane.active,
                        rightHandFreeForWeaponStop,
                        leftHandFreeForWeaponStop,
                        rightHand,
                        leftHand,
                        dynamicHandCollision);
                if (exactSweep.staleAuthorityRejected) {
                    /*
                     * Replacement banks may intentionally remain physically
                     * active, but old-generation source transforms must never
                     * produce a visual correction for the new weapon root.
                     */
                    clearWorldContactState(handState);
                    handState.weaponAuthorityDiscontinuityThisFrame = true;
                    return best;
                }
                weaponRootDiscontinuity =
                    exactSweep.rebasedForRootDiscontinuity;
                exactPreviousClearWeaponWorldValid =
                    exactSweep.hasPreviousClearWeaponWorld &&
                    isFiniteTransform(
                        exactSweep.previousClearWeaponWorld);
                if (exactPreviousClearWeaponWorldValid) {
                    exactPreviousClearWeaponWorld =
                        exactSweep.previousClearWeaponWorld;
                }
                previousWeaponStopWorldValid =
                    exactPreviousClearWeaponWorldValid ||
                    (weaponDriveSegment.valid &&
                     isFiniteTransform(
                         weaponDriveSegment.previousRootWorld));
                if (previousWeaponStopWorldValid) {
                    previousWeaponStopWorld =
                        exactPreviousClearWeaponWorldValid
                            ? exactPreviousClearWeaponWorld
                            : weaponDriveSegment.previousRootWorld;
                }
                if (weaponRootDiscontinuity) {
                    clearWorldContactState(handState);
                    handState.weaponAuthorityDiscontinuityThisFrame = true;
                    // Discard an exact cached-anchor probe appended before the
                    // explicit root rebase. The sampled current-generation
                    // probes remain valid fresh-pose evidence.
                    probeCount = sampledProbeCount;
                }
                for (std::size_t hitIndex = 0;
                     hitIndex < exactSweep.hitCount &&
                     hitIndex < exactSweep.hits.size();
                     ++hitIndex) {
                    const auto& exactHit =
                        exactSweep.hits[hitIndex];
                    std::uint32_t exactProbeId =
                        hitIndex == 0
                            ? kWorldProbeIdExactWeapon
                            : kWorldProbeIdExactWeaponSecondary;
                    constexpr float kCachedPlaneAlignment =
                        0.985f;
                    const auto alignsWithCached =
                        [&](const auto& cachedPlane) {
                            return cachedPlane.active &&
                                   std::abs(
                                       soft_contact_policy::
                                           weaponCorrectionPointDot(
                                               cachedPlane.normal,
                                               exactHit.
                                                   surfaceNormalWorld)) >=
                                       kCachedPlaneAlignment;
                        };
                    if (alignsWithCached(
                            handState.cachedWorldPlane)) {
                        exactProbeId =
                            kWorldProbeIdExactWeapon;
                    } else if (alignsWithCached(
                                   handState.
                                       secondaryCachedWorldPlane)) {
                        exactProbeId =
                            kWorldProbeIdExactWeaponSecondary;
                    } else if (handState.cachedWorldPlane.active) {
                        // A genuinely independent plane receives the dedicated
                        // secondary exact state even when hit ordering changes.
                        exactProbeId =
                            kWorldProbeIdExactWeaponSecondary;
                    }
                    Candidate exactCandidate =
                        makeExactWeaponWorldSweepCandidate(
                            bhkWorld,
                            world,
                            exactHit,
                            weaponCollision,
                            weaponNode,
                            contactPadding,
                            skin,
                            exactProbeId);
                    if (collision_layer_policy::
                            isWeaponSweepHandTargetLayer(
                                exactHit.targetCollisionLayer)) {
                        bool targetResolved = false;
                        bool targetHandIsLeft = false;
                        bool targetSampledVelocityValid = false;
                        RE::NiPoint3 targetSampledVelocityHavok{};

                        if (exactHit.targetIsDynamicHandProxy) {
                            DynamicHandCollisionContactBodySnapshot
                                targetHand{};
                            targetResolved =
                                dynamicHandCollision &&
                                dynamicHandCollision->
                                    tryGetContactBodySnapshot(
                                        exactHit.targetBodyId,
                                        targetHand);
                            if (targetResolved) {
                                targetHandIsLeft = targetHand.isLeft;
                                targetSampledVelocityValid =
                                    targetHand.sampledVelocityValid;
                                targetSampledVelocityHavok =
                                    targetHand.sampledVelocityHavok;
                            }
                        } else if (exactHit.
                                       targetIsGeneratedHandCollider) {
                            HandColliderBodyMetadata metadata{};
                            if (rightHand.tryGetHandColliderMetadata(
                                    exactHit.targetBodyId,
                                    metadata) ||
                                leftHand.tryGetHandColliderMetadata(
                                    exactHit.targetBodyId,
                                    metadata)) {
                                targetResolved = metadata.valid;
                                targetHandIsLeft = metadata.isLeft;
                                targetSampledVelocityValid =
                                    metadata.
                                        hasSampledLinearVelocityHavok;
                                if (targetSampledVelocityValid) {
                                    targetSampledVelocityHavok =
                                        RE::NiPoint3{
                                            metadata.
                                                sampledLinearVelocityHavok[0],
                                            metadata.
                                                sampledLinearVelocityHavok[1],
                                            metadata.
                                                sampledLinearVelocityHavok[2],
                                        };
                                }
                            }
                        }
                        const bool targetHandFree =
                            targetResolved &&
                            (targetHandIsLeft
                                 ? leftHandFreeForWeaponStop
                                 : rightHandFreeForWeaponStop);
                        RE::NiPoint3 targetVelocityGame{};
                        const float velocityScale =
                            havokToGameScale();
                        const bool targetVelocityValid =
                            targetResolved &&
                            targetSampledVelocityValid &&
                            std::isfinite(velocityScale) &&
                            velocityScale > 0.0f;
                        if (targetVelocityValid) {
                            targetVelocityGame =
                                targetSampledVelocityHavok *
                                velocityScale;
                        }
                        const float targetHandNormalSpeed =
                            targetVelocityValid &&
                                    soft_contact_math::isFinite(
                                        targetVelocityGame)
                                ? soft_contact_math::dot(
                                      targetVelocityGame,
                                      exactHit.surfaceNormalWorld)
                                : 0.0f;
                        const auto directionalMotion =
                            soft_contact_policy::
                                resolveWeaponHandDirectionalMotion(
                                    -exactHit.
                                        approachSpeedGameUnits,
                                    targetHandNormalSpeed);
                        /*
                         * The continuous cast is itself current weapon-motion
                         * evidence. A newly created proxy or production hand
                         * hull may not yet expose a target velocity; treating
                         * that one missing sample as stationary is safer than
                         * allowing a proven gun sweep to tunnel. Once present,
                         * hand motion participates in the same dominance
                         * arbitration as native manifolds.
                         */
                        const bool weaponHandStopAccepted =
                            exactCandidate.valid &&
                            exactHit.validatedContinuousEntry &&
                            exactHit.hasBlockedWeaponWorld &&
                            targetResolved &&
                            soft_contact_policy::
                                shouldAcquireWeaponHandStop(
                                    true,
                                    weaponCollision->
                                        isWeaponBodyIdAtomic(
                                            exactHit.sourceBodyId),
                                    targetHandFree,
                                    true,
                                    true,
                                    directionalMotion.valid,
                                    directionalMotion.
                                        weaponApproachSpeedGameUnits,
                                    directionalMotion.
                                        handApproachSpeedGameUnits,
                                    directionalMotion.
                                        relativeClosingSpeedGameUnits);
                        if (weaponHandStopAccepted) {
                            exactCandidate.
                                validatedWeaponHandStop = true;
                            exactCandidate.targetIsDynamicHand =
                                true;
                            exactCandidate.targetHandIsLeft =
                                targetHandIsLeft;
                            exactCandidate.
                                targetDynamicHandBodyId =
                                    exactHit.targetBodyId;
                            exactCandidate.
                                targetHandCollisionLayer =
                                    exactHit.targetCollisionLayer;
                            ROCK_LOG_SAMPLE_WARN(
                                Weapon,
                                1000,
                                "Weapon/hand exact candidate accepted layer={} side={} body={} entry={} blocked={} weapon/hand/relative={:.2f}/{:.2f}/{:.2f}",
                                exactHit.targetCollisionLayer,
                                targetHandIsLeft ? "L" : "R",
                                exactHit.targetBodyId,
                                exactHit.validatedContinuousEntry ? "yes" : "no",
                                exactHit.hasBlockedWeaponWorld ? "yes" : "no",
                                directionalMotion.
                                    weaponApproachSpeedGameUnits,
                                directionalMotion.
                                    handApproachSpeedGameUnits,
                                directionalMotion.
                                    relativeClosingSpeedGameUnits);
                        } else {
                            // A generated-hand hit may never be cached as a
                            // static wall; the hand-driven response owns a
                            // directionally rejected pair.
                            ROCK_LOG_SAMPLE_WARN(
                                Weapon,
                                1000,
                                "Weapon/hand exact candidate rejected layer={} body={} candidate={} entry={} blocked={} resolved={} free={} direction={} weapon/hand/relative={:.2f}/{:.2f}/{:.2f}",
                                exactHit.targetCollisionLayer,
                                exactHit.targetBodyId,
                                exactCandidate.valid ? "yes" : "no",
                                exactHit.validatedContinuousEntry ? "yes" : "no",
                                exactHit.hasBlockedWeaponWorld ? "yes" : "no",
                                targetResolved ? "yes" : "no",
                                targetHandFree ? "yes" : "no",
                                directionalMotion.valid ? "yes" : "no",
                                directionalMotion.
                                    weaponApproachSpeedGameUnits,
                                directionalMotion.
                                    handApproachSpeedGameUnits,
                                directionalMotion.
                                    relativeClosingSpeedGameUnits);
                            exactCandidate = {};
                        }
                    }
                    admitCandidate(exactCandidate);
                }
            }

            const auto primaryCachedPlaneContact = solveCachedWorldPlaneContact(
                handState,
                handState.cachedWorldPlane,
                probes,
                probeCount,
                contactPadding,
                skin,
                maxTangentDrift,
                maxClearDistance,
                deltaSeconds);
            admitCandidate(primaryCachedPlaneContact.candidate);
            CachedWorldPlaneResult secondaryCachedPlaneContact{};
            if (includeWeaponProbes) {
                secondaryCachedPlaneContact =
                    solveCachedWorldPlaneContact(
                        handState,
                        handState.secondaryCachedWorldPlane,
                        probes,
                        probeCount,
                        contactPadding,
                        skin,
                        maxTangentDrift,
                        maxClearDistance,
                        deltaSeconds);
                admitCandidate(
                    secondaryCachedPlaneContact.candidate);
            }

            /*
             * A valid cached weapon plane remains the stable native-manifold
             * representative for this episode, so do not rescan the callback
             * point cloud merely to replace it. The fixed critical hull sweeps
             * below still run and can add an independent second plane when the
             * weapon slides from this wall into a corner.
            */
            if (!includeWeaponProbes ||
                (!primaryCachedPlaneContact.candidate.valid &&
                 !secondaryCachedPlaneContact.candidate.valid)) {
                const Candidate nativeCandidate =
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
                        rightHandFreeForWeaponStop,
                        leftHandFreeForWeaponStop,
                        weaponHandFirstContactBySide,
                        weaponHandEvidenceLayerBySide,
                        weaponDriveSegment,
                        probes,
                        probeCount,
                        fallbackNormal,
                        contactPadding,
                        skin,
                        deltaSeconds);
                const bool previouslyInContact =
                    handState.state == ContactState::Touching ||
                    handState.state == ContactState::Penetrating;
                const bool deferNativeToWeaponSweep =
                    nativeCandidate.source ==
                            CandidateSource::NativeWorld &&
                    soft_contact_policy::
                        shouldDeferNativeWeaponFirstWitnessToSweep(
                            includeWeaponProbes,
                            nativeCandidate.valid,
                            nativeCandidate.sourceIsWeapon,
                            previouslyInContact,
                            nativeCandidate.contact.penetration,
                            maxWorldCorrection());
                if (deferNativeToWeaponSweep) {
                    ROCK_LOG_SAMPLE_WARN(
                        Weapon,
                        1000,
                        "Deep first native weapon witness deferred to validated hull sweep probe={} body={} penetration={:.2f} maxCorrection={:.2f}",
                        nativeCandidate.contact.movableId,
                        nativeCandidate.contact.targetId,
                        nativeCandidate.contact.penetration,
                        maxWorldCorrection());
                } else {
                    admitCandidate(nativeCandidate);
                }
            }

            const bool hasAuthoritativeWorldCandidate =
                best.valid &&
                best.kind == ContactKind::WorldStatic &&
                (best.source == CandidateSource::NativeWorld || best.source == CandidateSource::CachedWorldPlane);
            const bool releasedCachedPlaneThisFrame =
                !hasAuthoritativeWorldCandidate &&
                ((primaryCachedPlaneContact.hadCachedPlane &&
                  primaryCachedPlaneContact.leftContact) ||
                 (secondaryCachedPlaneContact.hadCachedPlane &&
                  secondaryCachedPlaneContact.leftContact));
            const RE::NiPoint3 releasedCachedPlaneNormal =
                primaryCachedPlaneContact.leftContact
                    ? primaryCachedPlaneContact.normal
                    : secondaryCachedPlaneContact.normal;

            if (!hasAuthoritativeWorldCandidate ||
                includeWeaponProbes) {
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
                    includeWeaponProbes
                        ? kMaxWeaponProbeCastsPerFrame
                        : kMaxHandWorldProbeCastsPerFrame;
                const auto tryProbeCast = [&](const std::uint32_t index) {
                    if (castsRemaining == 0 ||
                        index >= probeCount ||
                        index >= probes.size()) {
                        return;
                    }

                    const auto& probe = probes[index];
                    if (!probe.valid ||
                        probe.stateIndex >=
                            handState.worldProbes.size()) {
                        return;
                    }

                    auto& previousState =
                        handState.worldProbes[probe.stateIndex];
                    if (previousState.history.identity != 0 &&
                        previousState.history.identity != probe.identity) {
                        previousState = {};
                    }
                    const bool sparseObservationDiscontinuous =
                        soft_contact_policy::
                            sparseProbeObservationIsDiscontinuous(
                                previousState.history,
                                probe.position,
                                probe.identity,
                                dt);
                    if (sparseObservationDiscontinuous &&
                        (!probe.sourceIsWeapon ||
                         weaponRootDiscontinuity)) {
                        // Selected probes must reject the discontinuity before
                        // casting. storeWorldProbePositions performs the same
                        // rebase for probes outside this frame's sparse budget.
                        previousState = {};
                        (void)soft_contact_policy::observeSparseProbe(
                            previousState.history,
                            probe.position,
                            probe.identity);
                        return;
                    }

                    const auto& history = previousState.history;
                    const RE::NiPoint3 sweepDelta = history.queryValid ?
                                                      soft_contact_math::sub(
                                                          probe.position,
                                                          history.lastQueryPosition) :
                                                      RE::NiPoint3{};
                    const float sweepDistance = history.queryValid ?
                                                    soft_contact_math::length(sweepDelta) :
                                                    0.0f;
                    const bool canSweep =
                        history.queryValid &&
                        std::isfinite(sweepDistance) &&
                        sweepDistance > kWorldProbeMinSweepDistanceGameUnits;
                    const bool allowPostReleaseSweep =
                        soft_contact_math::shouldAllowPostReleaseReentrySweep(
                            releasedCachedPlaneThisFrame,
                            sweepDelta,
                            releasedCachedPlaneNormal,
                            reentryMinApproachDistance);

                    if (canSweep && allowPostReleaseSweep) {
                        --castsRemaining;
                        admitCandidate(
                            runWorldProbeCast(bhkWorld,
                                world,
                                probe,
                                history.lastQueryPosition,
                                sweepDelta,
                                sweepDistance,
                                history.observedValid ?
                                    history.previousObserved :
                                    history.lastQueryPosition,
                                fallbackNormal,
                                queryRadiusPadding,
                                contactPadding,
                                skin,
                                deltaSeconds,
                                includeWeaponProbes
                                    ? &weaponCandidateManifold
                                    : nullptr));
                        soft_contact_policy::commitSparseProbeQuery(
                            previousState.history,
                            probe.position,
                            probe.identity);
                    } else if (canSweep &&
                               !allowPostReleaseSweep) {
                        // A release-direction gate deliberately rejects this
                        // path. Rebase it so a later rotating admission cannot
                        // resurrect the rejected crossing from an ancient pose.
                        soft_contact_policy::commitSparseProbeQuery(
                            previousState.history,
                            probe.position,
                            probe.identity);
                    }

                    // Weapon hull acquisition is continuous sweep-only. A
                    // short rest query in the fallback palm direction has no
                    // meaningful relation to an arbitrary hull sample and can
                    // create a contact on the wrong side of the gun.
                    if (probe.sourceIsWeapon) {
                        return;
                    }

                    if (canSweep || releasedCachedPlaneThisFrame || previousState.restQueryCooldownSeconds > 0.0f) {
                        return;
                    }

                    if (castsRemaining == 0) {
                        return;
                    }
                    previousState.restQueryCooldownSeconds = kWorldProbeRestQueryCooldownSeconds;
                    const RE::NiPoint3 restDirection = soft_contact_math::normalizeOr(fallbackNormal, RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
                    const RE::NiPoint3 previousPosition =
                        previousState.history.observedValid ?
                            previousState.history.previousObserved :
                            probe.position;
                    --castsRemaining;
                    admitCandidate(
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
                            deltaSeconds,
                            includeWeaponProbes
                                ? &weaponCandidateManifold
                                : nullptr));
                    soft_contact_policy::commitSparseProbeQuery(
                        previousState.history,
                        probe.position,
                        probe.identity);
                };

                if (includeWeaponProbes) {
                    std::uint32_t criticalAdmissions = 0;
                    for (std::uint32_t index = 0;
                         index < probeCount &&
                         index < probes.size() &&
                         castsRemaining != 0;
                         ++index) {
                        const auto& probe = probes[index];
                        if (!probe.valid ||
                            !soft_contact_policy::
                                shouldAdmitWeaponProbeSweep(
                                    probe.criticalWeaponSweep,
                                    hasAuthoritativeWorldCandidate,
                                    false)) {
                            continue;
                        }
                        if (++criticalAdmissions >
                            kMaxCriticalWeaponProbeCastsPerFrame) {
                            break;
                        }
                        tryProbeCast(index);
                    }

                    std::array<std::uint32_t,
                        SoftContactRuntime::
                            kMaxWorldContactProbesPerHand>
                        nonCriticalProbeIndices{};
                    std::uint32_t nonCriticalProbeCount = 0;
                    for (std::uint32_t index = 0;
                         index < probeCount &&
                         index < probes.size() &&
                         nonCriticalProbeCount <
                             nonCriticalProbeIndices.size();
                         ++index) {
                        if (probes[index].valid &&
                            !probes[index].criticalWeaponSweep) {
                            nonCriticalProbeIndices[
                                nonCriticalProbeCount++] = index;
                        }
                    }

                    if (!hasAuthoritativeWorldCandidate &&
                        castsRemaining != 0 &&
                        nonCriticalProbeCount != 0) {
                        const auto window =
                            soft_contact_policy::
                                resolveBoundedRotatingProbeWindow(
                                    handState.worldProbeCastCursor,
                                    nonCriticalProbeCount,
                                    kMaxRotatingWeaponProbeCastsPerFrame);
                        for (std::size_t offset = 0;
                             offset < window.count &&
                             castsRemaining != 0;
                             ++offset) {
                            const std::size_t ordinal =
                                soft_contact_policy::
                                    boundedRotatingProbeIndex(
                                        window,
                                        offset,
                                        nonCriticalProbeCount);
                            if (ordinal >= nonCriticalProbeCount) {
                                continue;
                            }
                            const auto index =
                                nonCriticalProbeIndices[ordinal];
                            if (soft_contact_policy::
                                    shouldAdmitWeaponProbeSweep(
                                        false,
                                        false,
                                        true)) {
                                tryProbeCast(index);
                            }
                        }
                        handState.worldProbeCastCursor =
                            static_cast<std::uint32_t>(
                                window.nextCursor);
                    } else if (hasAuthoritativeWorldCandidate) {
                        /*
                         * Do not bulk-rebase the critical extrema: their
                         * previous-frame query position is the CCD witness that
                         * can discover a perpendicular wall while plane A owns
                         * the episode. Noncritical probes are intentionally
                         * rebased so a later rotating acquisition cannot cast
                         * from an ancient pre-contact pose.
                         */
                        for (std::uint32_t ordinal = 0;
                             ordinal < nonCriticalProbeCount;
                             ++ordinal) {
                            const auto& probe = probes[
                                nonCriticalProbeIndices[ordinal]];
                            if (probe.stateIndex >=
                                handState.worldProbes.size()) {
                                continue;
                            }
                            auto& state = handState.worldProbes[
                                probe.stateIndex];
                            soft_contact_policy::
                                commitSparseProbeQuery(
                                    state.history,
                                    probe.position,
                                    probe.identity);
                        }
                    }
                } else {
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
            } else {
                // Native/cached evidence already owns this frame. Rebase sparse
                // query history intentionally so release/reacquisition never
                // sweeps from a pre-contact pose several frames in the past.
                rebaseWorldProbeQueryPositions(
                    handState,
                    probes,
                    probeCount);
            }

            if (includeWeaponProbes &&
                best.valid &&
                weaponCandidateManifold.corrections.count != 0) {
                const RE::NiPoint3 manifoldCorrection =
                    soft_contact_policy::
                        solveWeaponCorrectionManifold(
                            weaponCandidateManifold.corrections);
                if (soft_contact_math::isFinite(
                        manifoldCorrection) &&
                    soft_contact_math::length(
                        manifoldCorrection) >
                        kCorrectionClearDistance) {
                    best.correctionOverrideValid = true;
                    best.correctionOverride = manifoldCorrection;
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
                            best.contact.movableId) {
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
                        best.weaponAnchorUsesSourceLocal,
                        soft_contact_policy::
                                isExactWeaponWorldProbeId(
                                    best.contact.movableId)
                            ? best.contact.movableId
                            : kWorldProbeIdExactWeapon);
                }
            }

            storeWorldProbePositions(
                handState,
                probes,
                probeCount,
                deltaSeconds);

            const auto cacheCandidate =
                [](auto& cachedPlane,
                    const Candidate& candidate,
                    const soft_contact_policy::
                        WeaponCachedPlaneSlot destinationSlot =
                            soft_contact_policy::
                                WeaponCachedPlaneSlot::None) {
                    cachedPlane = {};
                    if (!candidate.valid ||
                        candidate.targetIsDynamicHand ||
                        candidate.kind != ContactKind::WorldStatic) {
                        return;
                    }
                    cachedPlane.active = true;
                    cachedPlane.sourceIsWeapon =
                        candidate.sourceIsWeapon;
                    cachedPlane.bodyId = candidate.contact.targetId;
                    cachedPlane.probeId = soft_contact_policy::
                        normalizeWeaponCachedPlaneProbeId(
                            destinationSlot,
                            candidate.contact.movableId,
                            candidate.weaponAnchorValid);
                    cachedPlane.surfacePoint =
                        candidate.contact.targetPoint;
                    cachedPlane.normal = candidate.contact.normal;
                    cachedPlane.approachSpeedGameUnits =
                        candidate.approachSpeedGameUnits;
                    cachedPlane.targetIdentity = candidate.targetIdentity;
                    cachedPlane.weaponSourceBodyId =
                        candidate.weaponSourceBodyId;
                    cachedPlane.weaponAnchorValid =
                        candidate.weaponAnchorValid;
                    cachedPlane.weaponAnchorUsesSourceLocal =
                        candidate.weaponAnchorUsesSourceLocal;
                    cachedPlane.weaponAnchorLocal =
                        candidate.weaponAnchorLocal;
                    cachedPlane.weaponProbeRadius =
                        candidate.weaponProbeRadius;
                };

            if (!includeWeaponProbes) {
                if (best.valid &&
                    best.kind == ContactKind::WorldStatic) {
                    const bool targetIsGeneratedWeapon =
                        best.targetIdentity.endpointKind ==
                        contact_evidence::
                            NativeContactEndpointKind::Weapon;
                    if (!soft_contact_policy::
                            shouldCacheNativeVisualStopPlane(
                                targetIsGeneratedWeapon)) {
                        /*
                         * The generated weapon is keyframed and may move every
                         * frame. Its fresh native manifold can stop a free
                         * legacy hand, but retaining that point as a static
                         * cached plane would make the hand stick to the
                         * weapon's old pose and reintroduce feedback/jitter.
                         */
                        handState.cachedWorldPlane = {};
                    } else {
                        cacheCandidate(
                            handState.cachedWorldPlane,
                            best);
                    }
                }
            } else {
                /*
                 * Keep the original entry plane stable. If it separates while
                 * the sampled secondary remains valid, promote that secondary
                 * before admitting a new plane. This makes corner persistence
                 * deterministic without allocating another exact-anchor state
                 * slot or allowing more than two constraints.
                 */
                if (!handState.cachedWorldPlane.active &&
                    handState.secondaryCachedWorldPlane.active) {
                    handState.cachedWorldPlane =
                        handState.secondaryCachedWorldPlane;
                    handState.secondaryCachedWorldPlane = {};
                    if (handState.cachedWorldPlane.
                            weaponAnchorValid &&
                        handState.cachedWorldPlane.probeId ==
                            kWorldProbeIdExactWeaponSecondary) {
                        // Promotion moves the persisted anchor to the primary
                        // exact state slot, leaving the secondary slot free for
                        // a newly independent corner plane.
                        handState.cachedWorldPlane.probeId =
                            kWorldProbeIdExactWeapon;
                    }
                }
                if (!handState.cachedWorldPlane.active &&
                    best.valid &&
                    best.sourceIsWeapon &&
                    best.kind == ContactKind::WorldStatic) {
                    cacheCandidate(
                        handState.cachedWorldPlane,
                        best,
                        soft_contact_policy::
                            WeaponCachedPlaneSlot::Primary);
                }

                if (handState.cachedWorldPlane.active &&
                    handState.secondaryCachedWorldPlane.active &&
                    !soft_contact_policy::
                        weaponCorrectionNormalsAreIndependent(
                            handState.cachedWorldPlane.normal,
                            handState.secondaryCachedWorldPlane.normal)) {
                    handState.secondaryCachedWorldPlane = {};
                }

                for (std::size_t index = 0;
                     index < weaponCandidateManifold.corrections.count &&
                     index < weaponCandidateManifold.candidates.size();
                     ++index) {
                    if (!weaponCandidateManifold.candidateValid[index]) {
                        continue;
                    }
                    const Candidate& candidate =
                        weaponCandidateManifold.candidates[index];
                    const bool independentFromPrimary =
                        handState.cachedWorldPlane.active &&
                        soft_contact_policy::
                            weaponCorrectionNormalsAreIndependent(
                                handState.cachedWorldPlane.normal,
                                candidate.contact.normal);
                    const auto selectedSlot =
                        soft_contact_policy::
                            selectWeaponCachedPlaneSlot(
                                handState.cachedWorldPlane.active,
                                handState.secondaryCachedWorldPlane.active,
                                independentFromPrimary,
                                soft_contact_policy::
                                    secondaryWeaponPlaneMayUseProbe(
                                        candidate.contact.movableId,
                                        candidate.weaponAnchorValid));
                    if (selectedSlot ==
                        soft_contact_policy::
                            WeaponCachedPlaneSlot::Primary) {
                        cacheCandidate(
                            handState.cachedWorldPlane,
                            candidate,
                            soft_contact_policy::
                                WeaponCachedPlaneSlot::Primary);
                    } else if (selectedSlot ==
                               soft_contact_policy::
                                   WeaponCachedPlaneSlot::Secondary) {
                        cacheCandidate(
                            handState.secondaryCachedWorldPlane,
                            candidate,
                            soft_contact_policy::
                                WeaponCachedPlaneSlot::Secondary);
                    }
                }
            }

            const Candidate& earliestStop =
                weaponCandidateManifold.earliestStopCandidate;
            if (includeWeaponProbes && earliestStop.valid &&
                earliestStop.hasBlockedWeaponWorld) {
                best.hasBlockedWeaponWorld = true;
                best.validatedWeaponSweep =
                    earliestStop.validatedWeaponSweep;
                best.validatedWeaponHandStop =
                    earliestStop.validatedWeaponHandStop;
                best.targetIsDynamicHand =
                    earliestStop.targetIsDynamicHand;
                best.targetHandIsLeft =
                    earliestStop.targetHandIsLeft;
                best.targetDynamicHandBodyId =
                    earliestStop.targetDynamicHandBodyId;
                best.targetHandCollisionLayer =
                    earliestStop.targetHandCollisionLayer;
                best.weaponSweepFraction =
                    earliestStop.weaponSweepFraction;
                best.weaponSafeSweepFraction =
                    earliestStop.weaponSafeSweepFraction;
                best.blockedWeaponWorld =
                    earliestStop.blockedWeaponWorld;
                best.blockedSurfacePointWorld =
                    earliestStop.blockedSurfacePointWorld;
                best.blockedSurfaceNormalWorld =
                    earliestStop.blockedSurfaceNormalWorld;
                best.blockedSourceBodyId =
                    earliestStop.blockedSourceBodyId;
                best.blockedAnchorUsesSourceLocal =
                    earliestStop.blockedAnchorUsesSourceLocal;
                best.blockedAnchorLocal =
                    earliestStop.blockedAnchorLocal;
                best.blockedProbeRadius =
                    earliestStop.blockedProbeRadius;
            }

            /*
             * QueryWorld can report an exact weapon overlap without a cast
             * fraction (native broadphase/filter timing). Prefer the
             * generation-current previous safe pose whenever one exists. If
             * history was rebased, a bounded finite first witness freezes the
             * current pose instead: that creates no depenetration snap while
             * still letting the implausible-witness guard reject huge query
             * glitches. Both paths publish one immutable full-pose stop rather
             * than a frame-to-frame correction.
             */
            const auto unvalidatedStopPoseSource =
                weapon_wall_sweep_policy::
                    chooseUnvalidatedWeaponStopPoseSource(
                        includeWeaponProbes && best.valid,
                        best.sourceIsWeapon,
                        best.hasBlockedWeaponWorld,
                        best.weaponAnchorValid &&
                            best.weaponSourceBodyId !=
                                kInvalidWeaponSourceBodyId,
                        previousWeaponStopWorldValid,
                        weaponNode &&
                            isFiniteTransform(weaponNode->world),
                        best.contact.penetration,
                        maxWorldCorrection());
            if (unvalidatedStopPoseSource !=
                weapon_wall_sweep_policy::
                    UnvalidatedWeaponStopPoseSource::None) {
                best.hasBlockedWeaponWorld = true;
                // This synthetic provenance has the same authority contract as
                // a sweep stop: a finite exact weapon anchor plus one frozen
                // full pose. It never authorizes a correction/depenetration.
                best.validatedWeaponSweep = true;
                best.weaponSweepFraction = 0.0f;
                best.weaponSafeSweepFraction = 0.0f;
                best.blockedWeaponWorld =
                    unvalidatedStopPoseSource ==
                            weapon_wall_sweep_policy::
                                UnvalidatedWeaponStopPoseSource::
                                    PreviousSafe
                        ? previousWeaponStopWorld
                        : weaponNode->world;
                best.blockedSurfacePointWorld =
                    best.contact.targetPoint;
                best.blockedSurfaceNormalWorld =
                    best.contact.normal;
                best.blockedSourceBodyId =
                    best.weaponSourceBodyId;
                best.blockedAnchorUsesSourceLocal =
                    best.weaponAnchorUsesSourceLocal;
                best.blockedAnchorLocal =
                    best.weaponAnchorLocal;
                best.blockedProbeRadius =
                    best.weaponProbeRadius;
                if (unvalidatedStopPoseSource ==
                    weapon_wall_sweep_policy::
                        UnvalidatedWeaponStopPoseSource::
                            CurrentStable) {
                    ROCK_LOG_SAMPLE_WARN(
                        Weapon,
                        1000,
                        "Weapon wall first overlap froze current pose "
                        "without depenetration penetration={:.2f} "
                        "maxStable={:.2f}",
                        best.contact.penetration,
                        maxWorldCorrection() *
                            weapon_wall_sweep_policy::
                                kStableCurrentPoseAcquisitionLimitMultiplier);
                }
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
        _weaponWorldStop = {};
        _weaponHandContactEpisodes = {};
        _weaponGenerationKey = 0;
        _debugSnapshot = {};
        _weaponWorldCorrection = {};
        _weaponWorldCorrectionActive = false;
        _weaponWorldCorrectionHandIsLeft = false;
        _wasHandWorldEnabled = {};
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

    bool SoftContactRuntime::getWeaponWorldStopPose(
        RE::NiTransform& outWeaponWorld,
        bool& outFiringHandIsLeft) const
    {
        outWeaponWorld = _weaponWorldStop.blockedWeaponWorld;
        outFiringHandIsLeft =
            _weaponWorldCorrectionHandIsLeft;
        return _weaponWorldStop.active &&
               _weaponWorldStop.generationKey != 0 &&
               isFiniteTransform(outWeaponWorld);
    }

    bool SoftContactRuntime::getWeaponWorldStopSurface(
        RE::NiPoint3& outSurfacePointWorld,
        RE::NiPoint3& outSurfaceNormalWorld,
        bool& outTargetIsDynamicHand) const
    {
        outSurfacePointWorld = _weaponWorldStop.surfacePointWorld;
        outSurfaceNormalWorld = _weaponWorldStop.surfaceNormalWorld;
        outTargetIsDynamicHand = _weaponWorldStop.targetIsDynamicHand;
        return _weaponWorldStop.active &&
               _weaponWorldStop.generationKey != 0 &&
               !_weaponWorldStop.targetIsDynamicHand &&
               soft_contact_math::isFinite(outSurfacePointWorld) &&
               soft_contact_math::isFinite(outSurfaceNormalWorld) &&
               soft_contact_math::length(outSurfaceNormalWorld) > 0.5f;
    }

    bool SoftContactRuntime::getWeaponHandStopTarget(
        bool& outTargetHandIsLeft) const
    {
        outTargetHandIsLeft = _weaponWorldStop.targetHandIsLeft;
        return _weaponWorldStop.active &&
               _weaponWorldStop.generationKey != 0 &&
               _weaponWorldStop.targetIsDynamicHand;
    }

    void SoftContactRuntime::update(const PhysicsFrameContext& frame,
        const Hand& rightHand,
        const Hand& leftHand,
        bool rightDynamicHandWorldStopOperational,
        bool leftDynamicHandWorldStopOperational,
        bool rightHandWeaponOwned,
        bool leftHandWeaponOwned,
        bool rightHandVisualReturnActive,
        bool leftHandVisualReturnActive,
        bool weaponFiringHandIsLeft,
        const DynamicHandCollisionRuntime* dynamicHandCollision,
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
                    rightDynamicHandWorldStopOperational,
                    leftDynamicHandWorldStopOperational,
                    rightHandWeaponOwned,
                    leftHandWeaponOwned,
                    g_rockConfig.
                        rockWeaponCollisionEnabled,
                    g_rockConfig.
                        rockWeaponCollisionStaticWorldEnabled,
                    frame.worldReady,
                    frame.menuBlocked);
        if (!channels.anyEnabled()) {
            if (_wasHandWorldEnabled[0] ||
                _wasHandWorldEnabled[1]) {
                clearAllHands();
            }
            _weapon = {};
            _weaponWorldStop = {};
            _weaponHandContactEpisodes = {};
            _weaponGenerationKey = 0;
            if (weaponCollision) {
                weaponCollision->
                    resetInteractionCollisionWorldSweepHistory();
            }
            _debugSnapshot = {};
            _wasHandWorldEnabled = {};
            return;
        }

        if (!channels.rightHandPushback &&
            _wasHandWorldEnabled[0]) {
            clearHand(false);
        }
        if (!channels.leftHandPushback &&
            _wasHandWorldEnabled[1]) {
            clearHand(true);
        }
        _wasHandWorldEnabled[0] =
            channels.rightHandPushback;
        _wasHandWorldEnabled[1] =
            channels.leftHandPushback;

        if (++_logCounter >= 360) {
            _logCounter = 0;
            const float loggedWorldContactPadding = worldContactPadding();
            const float loggedWorldQueryPadding = worldQueryRadiusPadding(loggedWorldContactPadding);
            ROCK_LOG_DEBUG(Hand,
                "SoftContact channels: handPushback={}{} dynamicStop={}{} weaponWall={} worldHaptics={} worldMaxCorrection={:.2f} worldQueryPadding={:.2f} worldContactPadding={:.2f} worldPostReleaseReentryMinApproach={:.3f} worldCachedPlaneMaxTangentDrift={:.2f} worldCachedPlaneMaxClearDistance={:.2f} releaseLerp={} releaseLerpTime={:.2f}-{:.2f}s priority={}",
                channels.rightHandPushback ? "R" : "-",
                channels.leftHandPushback ? "L" : "-",
                rightDynamicHandWorldStopOperational ? "R" : "-",
                leftDynamicHandWorldStopOperational ? "L" : "-",
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

        const bool rightHandFreeForWeaponStop =
            soft_contact_policy::isHandPhysicallyFreeForWeaponStop(
                rightHandWeaponOwned,
                rightHandVisualReturnActive,
                frame.right.disabled,
                soft_contact_policy::hasHeldObjectAuthorityForWeaponStop(
                    rightHand.isHoldingAtomic(),
                    rock::HostIsHandHoldingObject(false),
                    rock::HostIsHandCollisionSuppressed(false)));
        const bool leftHandFreeForWeaponStop =
            soft_contact_policy::isHandPhysicallyFreeForWeaponStop(
                leftHandWeaponOwned,
                leftHandVisualReturnActive,
                frame.left.disabled,
                soft_contact_policy::hasHeldObjectAuthorityForWeaponStop(
                    leftHand.isHoldingAtomic(),
                    rock::HostIsHandHoldingObject(true),
                    rock::HostIsHandCollisionSuppressed(true)));

        std::array<std::uint32_t, 2>
            legacyHandWeaponTargetThisFrame{
                kInvalidWeaponSourceBodyId,
                kInvalidWeaponSourceBodyId,
            };

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
            // Weapon-attached hands are removed by the channel policy above.
            // Their rigid hand/weapon wall response is owned by the separate
            // generation-keyed weapon channel later in this update; running a
            // second hand target here would feed the attachment back into
            // itself and duplicate its contact haptic.
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
            const bool hostedCleanFrameRequired =
                rock::HostRequiresPreAuthorityHandWorld();
            const bool cleanSnapshotAvailable =
                rock::HostGetCleanPreAuthorityHandWorld(
                    isLeft,
                    preAuthorityHandWorld);
            const bool cleanTransformFinite =
                cleanSnapshotAvailable &&
                isFiniteTransform(preAuthorityHandWorld);
            bool cleanTranslationRebaseValid = false;
            if (cleanTransformFinite) {
                flattenedToCleanTranslation =
                    soft_contact_math::sub(preAuthorityHandWorld.translate, handInput.rawHandWorld.translate);
                const float rebaseDistance =
                    soft_contact_math::length(flattenedToCleanTranslation);
                cleanTranslationRebaseValid =
                    soft_contact_math::isFinite(
                        flattenedToCleanTranslation) &&
                    std::isfinite(rebaseDistance);
                if (cleanTranslationRebaseValid) {
                    cleanHandInput.rawHandWorld.translate =
                        preAuthorityHandWorld.translate;
                    cleanHandInput.grabAnchorWorld =
                        soft_contact_math::add(
                            handInput.grabAnchorWorld,
                            flattenedToCleanTranslation);
                }
            }
            if (hostedCleanFrameRequired &&
                (!cleanSnapshotAvailable || !cleanTransformFinite ||
                    !cleanTranslationRebaseValid)) {
                /*
                 * Dynamic collision deliberately yields to this legacy path
                 * while its clean proxy bank is unavailable. Do not let that
                 * fallback immediately recreate the same feedback loop from
                 * the contaminated flattened hand. Clear both visual/contact
                 * state until fresh clean truth returns.
                 */
                clearHand(isLeft);
                handState.state = ContactState::Suppressed;
                updateWorldContactHaptics(
                    handState,
                    isLeft,
                    false,
                    0.0f,
                    frame.deltaSeconds);
                ROCK_LOG_SAMPLE_WARN(
                    Hand,
                    2000,
                    "{} soft-contact hand skipped: hosted clean pre-authority frame unavailable or non-finite",
                    isLeft ? "Left" : "Right");
                return;
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
                        rightHandFreeForWeaponStop,
                        leftHandFreeForWeaponStop,
                        std::array<bool, 2>{},
                        std::array<std::uint32_t, 2>{
                            collision_layer_policy::ROCK_LAYER_HAND,
                            collision_layer_policy::ROCK_LAYER_HAND,
                        },
                        rightHand,
                        leftHand,
                        dynamicHandCollision,
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

            const bool validatedWeaponSweep =
                best.valid &&
                best.validatedWeaponSweep;
            if (best.valid &&
                soft_contact_policy::
                    shouldRejectImplausibleFirstWorldWitness(
                        previouslyInContact,
                        validatedWeaponSweep,
                        best.contact.penetration,
                        maxWorldCorrection())) {
                // A newly acquired unvalidated contact must be solvable without
                // hitting the correction cap. Query/native glitches have produced
                // 750+ gu "penetrations"; accepting one launches the visible
                // hand by the full 18-gu cap and then release-blends from that
                // bogus pose. A weapon QueryWorld result is different: it owns
                // a validated cast entry plane and must retain a legitimate
                // fast thin-wall crossing. Existing cached contact may also
                // exceed the cap while the controller continues through a wall.
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
                handState.secondaryCachedWorldPlane = {};
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
                    if (weaponCollision && !best.sourceIsWeapon &&
                        weaponCollision->isWeaponBodyIdAtomic(
                            best.contact.targetId)) {
                        legacyHandWeaponTargetThisFrame[
                            handIndex(isLeft)] =
                            best.contact.targetId;
                    }
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

        if (channels.rightHandPushback) {
            solveForHand(false, frame.right, rightHand);
        }
        if (channels.leftHandPushback) {
            solveForHand(true, frame.left, leftHand);
        }

        /*
         * Weapon hull/world contact is solved in its own generation-keyed
         * channel. It must not share probe history or a cached plane with the
         * firing palm: otherwise a hand manifold and a muzzle manifold replace
         * one another and the complete gun/hand assembly flickers.
         *
         * Generated hull samples are copied every valid armed frame so their
         * stable per-generation IDs can sweep against static world even when
         * the native contact callback has not produced a manifold record.
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
            _weaponWorldStop = {};
            _weaponHandContactEpisodes = {};
            _weaponGenerationKey = 0;
            if (weaponCollision) {
                weaponCollision->
                    resetInteractionCollisionWorldSweepHistory();
            }
        } else {
            if (_weaponGenerationKey !=
                currentWeaponGenerationKey) {
                _weapon = {};
                _weaponWorldStop = {};
                _weaponHandContactEpisodes = {};
                _weaponGenerationKey =
                    currentWeaponGenerationKey;
            }

            const std::array<std::uint32_t, 2>
                weaponHandEvidenceLayerBySide{
                    rightDynamicHandWorldStopOperational
                        ? collision_layer_policy::
                              ROCK_LAYER_DYNAMIC_HAND_PROXY
                        : collision_layer_policy::ROCK_LAYER_HAND,
                    leftDynamicHandWorldStopOperational
                        ? collision_layer_policy::
                              ROCK_LAYER_DYNAMIC_HAND_PROXY
                        : collision_layer_policy::ROCK_LAYER_HAND,
                };
            std::array<bool, 2> weaponHandEvidenceSeenNow{};
            std::array<bool, 2> weaponHandFirstContactBySide{};
            using contact_evidence::NativeContactEndpointKind;
            for (std::uint32_t index = 0;
                 index < nativeContactEvidence.count &&
                 index < nativeContactEvidence.records.size();
                 ++index) {
                const auto& record =
                    nativeContactEvidence.records[index];
                const bool sourceIsLeft =
                    record.sourceKind ==
                    NativeContactEndpointKind::LeftHand;
                const bool sourceIsRight =
                    record.sourceKind ==
                    NativeContactEndpointKind::RightHand;
                if (!record.valid ||
                    record.frame !=
                        nativeContactEvidence.currentFrame ||
                    record.quality !=
                        contact_evidence::
                            NativeContactQuality::RawPoint ||
                    (!sourceIsLeft && !sourceIsRight) ||
                    record.targetKind !=
                        NativeContactEndpointKind::Weapon ||
                    !weaponCollision->isWeaponBodyIdAtomic(
                        record.targetBodyId)) {
                    continue;
                }
                const bool sourceIsKnownHandBank =
                    record.sourceLayer ==
                        collision_layer_policy::ROCK_LAYER_HAND ||
                    record.sourceLayer ==
                        collision_layer_policy::
                            ROCK_LAYER_DYNAMIC_HAND_PROXY;
                if (!sourceIsKnownHandBank) {
                    continue;
                }
                const std::size_t sourceHandIndex =
                    handIndex(sourceIsLeft);
                if (!soft_contact_policy::
                        isAuthoritativeWeaponHandEvidenceLayer(
                            record.sourceLayer,
                            weaponHandEvidenceLayerBySide[
                                sourceHandIndex])) {
                    continue;
                }
                weaponHandEvidenceSeenNow[sourceHandIndex] = true;
            }
            for (std::size_t index = 0;
                 index < _weaponHandContactEpisodes.size();
                 ++index) {
                const auto observation =
                    soft_contact_policy::
                        observeWeaponHandContactEpisode(
                            _weaponHandContactEpisodes[index],
                            weaponHandEvidenceSeenNow[index],
                            currentWeaponGenerationKey,
                            nativeContactEvidence.currentFrame,
                            kNativeWorldContactMaxAgeFrames);
                _weaponHandContactEpisodes[index] =
                    observation.next;
                weaponHandFirstContactBySide[index] =
                    observation.freshEntry;
            }

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
                        rightHandFreeForWeaponStop,
                        leftHandFreeForWeaponStop,
                        weaponHandFirstContactBySide,
                        weaponHandEvidenceLayerBySide,
                        rightHand,
                        leftHand,
                        dynamicHandCollision,
                        weaponCollision,
                        weaponNode,
                        fallbackNormal,
                        frame.deltaSeconds);

                const bool validatedWeaponSweep =
                    weaponBest.valid &&
                    weaponBest.validatedWeaponSweep;
                const bool validatedWeaponStop =
                    validatedWeaponSweep ||
                    (weaponBest.valid &&
                     weaponBest.validatedWeaponHandStop);
                if (weaponBest.valid &&
                    soft_contact_policy::
                        shouldRejectImplausibleFirstWorldWitness(
                            previouslyInContact,
                            validatedWeaponStop,
                            weaponBest.contact.penetration,
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
                    _weapon.secondaryCachedWorldPlane = {};
                    weaponBest = {};
                }

                if (weaponBest.valid &&
                    weaponBest.validatedWeaponHandStop &&
                    weaponBest.blockedSourceBodyId !=
                        kInvalidWeaponSourceBodyId) {
                    for (std::size_t index = 0;
                         index <
                             legacyHandWeaponTargetThisFrame.size();
                         ++index) {
                        if (legacyHandWeaponTargetThisFrame[index] ==
                            weaponBest.blockedSourceBodyId) {
                            // Directional arbitration selected the moving gun.
                            // Remove only the legacy target published for this
                            // same pair; an unrelated hand/wall stop survives.
                            clearHand(index == 1u);
                        }
                    }
                }

                if (_weapon.
                        weaponAuthorityDiscontinuityThisFrame) {
                    _weaponWorldStop = {};
                }
                if (!_weaponWorldStop.active &&
                    weaponBest.valid &&
                    weaponBest.hasBlockedWeaponWorld &&
                    (weaponBest.validatedWeaponSweep ||
                     weaponBest.validatedWeaponHandStop) &&
                    weaponBest.blockedSourceBodyId !=
                        kInvalidWeaponSourceBodyId &&
                    isFiniteTransform(
                        weaponBest.blockedWeaponWorld) &&
                    soft_contact_math::isFinite(
                        weaponBest.blockedSurfacePointWorld) &&
                    soft_contact_math::isFinite(
                        weaponBest.blockedSurfaceNormalWorld) &&
                    soft_contact_math::isFinite(
                        weaponBest.blockedAnchorLocal)) {
                    _weaponWorldStop.active = true;
                    _weaponWorldStop.generationKey =
                        currentWeaponGenerationKey;
                    _weaponWorldStop.blockedWeaponWorld =
                        weaponBest.blockedWeaponWorld;
                    _weaponWorldStop.surfacePointWorld =
                        weaponBest.blockedSurfacePointWorld;
                    _weaponWorldStop.surfaceNormalWorld =
                        soft_contact_math::normalizeOr(
                            weaponBest.
                                blockedSurfaceNormalWorld,
                            fallbackNormal);
                    _weaponWorldStop.sourceBodyId =
                        weaponBest.blockedSourceBodyId;
                    _weaponWorldStop.targetIsDynamicHand =
                        weaponBest.targetIsDynamicHand;
                    _weaponWorldStop.targetHandIsLeft =
                        weaponBest.targetHandIsLeft;
                    _weaponWorldStop.targetDynamicHandBodyId =
                        weaponBest.targetDynamicHandBodyId;
                    _weaponWorldStop.targetHandCollisionLayer =
                        weaponBest.targetHandCollisionLayer;
                    _weaponWorldStop.anchorUsesSourceLocal =
                        weaponBest.
                            blockedAnchorUsesSourceLocal;
                    _weaponWorldStop.anchorLocal =
                        weaponBest.blockedAnchorLocal;
                    _weaponWorldStop.probeRadiusGame =
                        weaponBest.blockedProbeRadius;
                    _weaponWorldStop.missElapsedSeconds = 0.0f;
                    _weaponWorldStop.releaseActive = false;
                    _weaponWorldStop.previousRawProbeValid = false;
                    _weaponWorldStop.previousRawWeaponWorld =
                        weaponNode->world;
                    _weaponWorldStop.previousRawWeaponWorldValid =
                        isFiniteTransform(weaponNode->world);
                    ROCK_LOG_DEBUG(
                        Weapon,
                        "Weapon {} stop captured fraction={:.4f} safeFraction={:.4f} body={} target={} blockedT=({:.2f},{:.2f},{:.2f})",
                        weaponBest.targetIsDynamicHand ? "hand" : "wall",
                        weaponBest.weaponSweepFraction,
                        weaponBest.weaponSafeSweepFraction,
                        weaponBest.blockedSourceBodyId,
                        weaponBest.contact.targetId,
                        weaponBest.blockedWeaponWorld.translate.x,
                        weaponBest.blockedWeaponWorld.translate.y,
                        weaponBest.blockedWeaponWorld.translate.z);
                    if (weaponBest.targetIsDynamicHand) {
                        ROCK_LOG_SAMPLE_WARN(
                            Weapon,
                            1000,
                            "Weapon/hand stop published layer={} side={} source={} target={} fraction={:.3f}/{:.3f}",
                            weaponBest.targetHandCollisionLayer,
                            weaponBest.targetHandIsLeft ? "L" : "R",
                            weaponBest.blockedSourceBodyId,
                            weaponBest.contact.targetId,
                            weaponBest.weaponSweepFraction,
                            weaponBest.weaponSafeSweepFraction);
                    }
                }

                bool stopRetained = false;
                bool stopRetreatMotionConfirmed = false;
                if (_weaponWorldStop.active) {
                    const auto dynamicHandEvidenceIsCurrent = [&]() {
                        if (!_weaponWorldStop.targetIsDynamicHand) {
                            return true;
                        }
                        if (weaponBest.valid &&
                            weaponBest.validatedWeaponHandStop &&
                            weaponBest.targetIsDynamicHand &&
                            weaponBest.targetHandIsLeft ==
                                _weaponWorldStop.targetHandIsLeft &&
                            weaponBest.targetDynamicHandBodyId ==
                                _weaponWorldStop.
                                    targetDynamicHandBodyId &&
                            weaponBest.targetHandCollisionLayer ==
                                _weaponWorldStop.
                                    targetHandCollisionLayer &&
                            weaponBest.blockedSourceBodyId ==
                                _weaponWorldStop.sourceBodyId) {
                            return true;
                        }
                        using contact_evidence::
                            NativeContactEndpointKind;
                        const auto expectedHandKind =
                            _weaponWorldStop.targetHandIsLeft
                                ? NativeContactEndpointKind::LeftHand
                                : NativeContactEndpointKind::RightHand;
                        for (std::uint32_t index = 0;
                             index < nativeContactEvidence.count &&
                             index < nativeContactEvidence.records.size();
                             ++index) {
                            const auto& record =
                                nativeContactEvidence.records[index];
                            const bool targetHandFree =
                                _weaponWorldStop.targetHandIsLeft
                                    ? leftHandFreeForWeaponStop
                                    : rightHandFreeForWeaponStop;
                            if (record.valid &&
                                record.quality ==
                                    contact_evidence::
                                        NativeContactQuality::RawPoint &&
                                contact_evidence::isFrameFresh(
                                    nativeContactEvidence.currentFrame,
                                    record.frame,
                                    kNativeWorldContactMaxAgeFrames) &&
                                targetHandFree &&
                                record.sourceKind ==
                                    expectedHandKind &&
                                record.sourceLayer ==
                                    _weaponWorldStop.
                                        targetHandCollisionLayer &&
                                record.targetBodyId ==
                                    _weaponWorldStop.sourceBodyId &&
                                record.targetKind ==
                                    NativeContactEndpointKind::Weapon) {
                                return true;
                            }
                        }
                        return false;
                    };
                    bool identityCurrent =
                        _weaponWorldStop.generationKey ==
                            currentWeaponGenerationKey &&
                        !_weapon.
                            weaponAuthorityDiscontinuityThisFrame;
                    if (identityCurrent &&
                        _weaponWorldStop.targetIsDynamicHand) {
                        identityCurrent =
                            _weaponWorldStop.targetHandIsLeft
                                ? leftHandFreeForWeaponStop
                                : rightHandFreeForWeaponStop;
                    }
                    const bool immutableWallStop =
                        !_weaponWorldStop.targetIsDynamicHand;
                    if (identityCurrent &&
                        _weaponWorldStop.
                            previousRawWeaponWorldValid &&
                        isFiniteTransform(weaponNode->world)) {
                        identityCurrent =
                            !weapon_wall_sweep_policy::
                                poseIsDiscontinuous(
                                    hand_visual_lerp_math::
                                        distanceGameUnits(
                                            _weaponWorldStop.
                                                previousRawWeaponWorld.
                                                translate,
                                            weaponNode->world.translate),
                                    hand_visual_lerp_math::
                                        rotationDistanceDegrees(
                                            _weaponWorldStop.
                                                previousRawWeaponWorld,
                                            weaponNode->world),
                                    weapon_wall_sweep_policy::
                                        kRootDiscontinuityDistanceGameUnits,
                                    weapon_wall_sweep_policy::
                                        kRootDiscontinuityRotationDegrees);
                    }
                    RE::NiPoint3 rawProbeWorld{};
                    float rawProbeRadius = 0.0f;
                    const bool probeResolved =
                        identityCurrent &&
                        weaponCollision->
                            tryResolveInteractionCollisionProbeAnchor(
                                weaponNode,
                                _weaponWorldStop.sourceBodyId,
                                _weaponWorldStop.anchorLocal,
                                _weaponWorldStop.
                                    anchorUsesSourceLocal,
                                rawProbeWorld,
                                rawProbeRadius);
                    const bool contactProbeCurrent =
                        probeResolved &&
                        dynamicHandEvidenceIsCurrent();
                    const auto latchedWallStopAction =
                        weapon_wall_sweep_policy::
                            chooseImmutableWorldStopAction(
                                _weaponWorldStop.active,
                                identityCurrent,
                                _weaponWorldStop.releaseActive,
                                false,
                                false,
                                false);
                    if (immutableWallStop &&
                        latchedWallStopAction ==
                            weapon_wall_sweep_policy::
                                ImmutableWorldStopAction::
                                    ContinueRelease) {
                        // Outward plane exit already won. Keep advancing the
                        // bounded release below; a stationary outside probe
                        // must not re-latch the partially released pose.
                    } else if (contactProbeCurrent) {
                        const float contactPadding =
                            worldContactPadding();
                        const float skin =
                            soft_contact_math::sanitizeNonNegative(
                                g_rockConfig.
                                    rockSoftContactWorldSkinGameUnits,
                                0.5f);
                        const auto heldContact =
                            soft_contact_math::
                                solvePointPlaneContact(
                                    rawProbeWorld,
                                    _weaponWorldStop.
                                        surfacePointWorld,
                                    _weaponWorldStop.
                                        surfaceNormalWorld,
                                    (std::max)(
                                        rawProbeRadius,
                                        _weaponWorldStop.
                                            probeRadiusGame) +
                                        contactPadding,
                                    skin,
                                    kWorldProbeIdExactWeapon,
                                    0u);
                        const bool tangentCurrent =
                            heldContact.active &&
                            soft_contact_math::
                                withinTangentDriftLimit(
                                    heldContact.targetPoint,
                                    _weaponWorldStop.
                                        surfacePointWorld,
                                    _weaponWorldStop.
                                        surfaceNormalWorld,
                                    worldCachedPlaneMaxTangentDrift());
                        const float rawProbeSignedDistance =
                            soft_contact_math::dot(
                                soft_contact_math::sub(
                                    rawProbeWorld,
                                    _weaponWorldStop.
                                        surfacePointWorld),
                                _weaponWorldStop.
                                    surfaceNormalWorld);
                        stopRetreatMotionConfirmed =
                            weapon_wall_sweep_policy::
                                rawProbeRetreatConfirmed(
                                    _weaponWorldStop.
                                        previousRawProbeValid,
                                    _weaponWorldStop.
                                        previousRawProbeSignedDistance,
                                    rawProbeSignedDistance);
                        const auto resolvedWallStopAction =
                            weapon_wall_sweep_policy::
                                chooseImmutableWorldStopAction(
                                    _weaponWorldStop.active,
                                    identityCurrent,
                                    false,
                                    true,
                                    heldContact.active,
                                    stopRetreatMotionConfirmed);
                        const bool resolvedStopRetained =
                            immutableWallStop
                                ? resolvedWallStopAction ==
                                      weapon_wall_sweep_policy::
                                          ImmutableWorldStopAction::Retain
                                : weapon_wall_sweep_policy::
                                      retainStopForResolvedProbe(
                                          identityCurrent,
                                          heldContact.active,
                                          tangentCurrent);
                        // The captured stop is a complete immutable pose.
                        // Raw motion remains evidence only. It must not drag
                        // the weapon or either attached hand along the wall;
                        // a wall stop releases solely after confirmed outward
                        // plane exit (hand stops retain their tangent policy).
                        _weaponWorldStop.
                            previousRawProbeSignedDistance =
                                rawProbeSignedDistance;
                        _weaponWorldStop.previousRawProbeValid =
                            std::isfinite(rawProbeSignedDistance);
                        _weaponWorldStop.previousRawWeaponWorld =
                            weaponNode->world;
                        _weaponWorldStop.
                            previousRawWeaponWorldValid =
                                isFiniteTransform(
                                    weaponNode->world);
                        if (resolvedStopRetained) {
                            _weaponWorldStop.
                                missElapsedSeconds = 0.0f;
                            stopRetained = true;
                        } else if (immutableWallStop &&
                                   resolvedWallStopAction ==
                                       weapon_wall_sweep_policy::
                                           ImmutableWorldStopAction::
                                               BeginRelease) {
                            _weaponWorldStop.releaseActive = true;
                        }
                    } else if (
                        (immutableWallStop &&
                         weapon_wall_sweep_policy::
                             chooseImmutableWorldStopAction(
                                 _weaponWorldStop.active,
                                 identityCurrent,
                                 false,
                                 false,
                                 false,
                                 false) ==
                             weapon_wall_sweep_policy::
                                 ImmutableWorldStopAction::Retain) ||
                        (!immutableWallStop &&
                         weapon_wall_sweep_policy::
                             retainStopAcrossContactMiss(
                                 _weaponWorldStop.active,
                                 identityCurrent,
                                 false,
                                 _weaponWorldStop.
                                     missElapsedSeconds,
                                 frame.deltaSeconds))) {
                        if (immutableWallStop) {
                            _weaponWorldStop.missElapsedSeconds = 0.0f;
                        } else {
                            _weaponWorldStop.missElapsedSeconds +=
                                std::clamp(
                                    std::isfinite(frame.deltaSeconds) ?
                                        frame.deltaSeconds :
                                        (1.0f / 90.0f),
                                    0.0f,
                                    0.1f);
                        }
                        stopRetained = true;
                    }

                    if (!stopRetained) {
                        // Never hand an expired absolute stop directly to a
                        // possibly deep raw end pose. Rotation can diverge as
                        // well as translation while the controller presses
                        // through the plane, so confirmed wall retreat (or a
                        // hand-stop escape) must ease the complete rigid pose
                        // back to raw instead of clearing authority in one
                        // frame.
                        _weapon.cachedWorldPlane = {};
                        _weapon.secondaryCachedWorldPlane = {};
                        weaponBest = {};
                        const bool canBoundRelease =
                            identityCurrent &&
                            hand_visual_lerp_math::
                                heldHandTransformIsUsable(
                                    _weaponWorldStop.
                                        blockedWeaponWorld) &&
                            hand_visual_lerp_math::
                                heldHandTransformIsUsable(
                                    weaponNode->world);
                        if (canBoundRelease) {
                            const auto releaseStep =
                                hand_visual_lerp_math::advanceTransform(
                                    _weaponWorldStop.
                                        blockedWeaponWorld,
                                    weaponNode->world,
                                    weapon_wall_sweep_policy::
                                        kStopReleaseLinearSpeedGameUnitsPerSecond,
                                    weapon_wall_sweep_policy::
                                        kStopReleaseAngularSpeedDegreesPerSecond,
                                    frame.deltaSeconds);
                            if (!releaseStep.reachedTarget &&
                                hand_visual_lerp_math::
                                    heldHandTransformIsUsable(
                                        releaseStep.transform)) {
                                _weaponWorldStop.blockedWeaponWorld =
                                    releaseStep.transform;
                                _weaponWorldStop.missElapsedSeconds = 0.0f;
                                stopRetained = true;
                                ROCK_LOG_SAMPLE_DEBUG(
                                    Weapon,
                                    500,
                                    "Weapon wall stop releasing through a bounded full-pose step after plane exit");
                            } else {
                                _weaponWorldStop = {};
                            }
                        } else {
                            _weaponWorldStop = {};
                        }
                    } else if (stopRetreatMotionConfirmed) {
                        ROCK_LOG_SAMPLE_DEBUG(
                            Weapon,
                            500,
                            "Weapon wall full-pose stop retained during controller retreat until the raw probe exits the contact plane");
                    }
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
                } else if (stopRetained) {
                    updateWorldContactHaptics(
                        _weapon,
                        weaponFiringHandIsLeft,
                        false,
                        0.0f,
                        frame.deltaSeconds);
                    _weapon.state = ContactState::Touching;
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
                if (stopRetained) {
                    _weaponWorldCorrection = {};
                    _weaponWorldCorrectionActive = false;
                    _weaponWorldCorrectionHandIsLeft =
                        weaponFiringHandIsLeft;
                }
        }

        _debugSnapshot.rightState = _hands[0].state;
        _debugSnapshot.leftState = _hands[1].state;
    }
}
