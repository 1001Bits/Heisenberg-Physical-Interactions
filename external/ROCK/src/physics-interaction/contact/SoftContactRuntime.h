#pragma once

#include "physics-interaction/contact/SoftContactMath.h"
#include "physics-interaction/contact/SoftContactPolicy.h"
#include "physics-interaction/contact/NativeContactEvidence.h"
#include "physics-interaction/contact/ContactTargetIdentity.h"

#include <array>
#include <cstdint>

#include "RE/NetImmerse/NiTransform.h"

namespace RE
{
    class NiNode;
}

namespace rock
{
    class Hand;
    class DynamicHandCollisionRuntime;
    class WeaponCollision;
    struct PhysicsFrameContext;

    enum class SoftContactDebugSource : std::uint8_t
    {
        Unknown = 0,
        QueryWorld,
        CachedWorldPlane,
        NativeWorld
    };

    struct SoftContactDebugContact
    {
        bool valid = false;
        bool isLeft = false;
        bool suppressed = false;
        soft_contact_math::ContactKind kind = soft_contact_math::ContactKind::None;
        SoftContactDebugSource source = SoftContactDebugSource::Unknown;
        soft_contact_math::ContactState state = soft_contact_math::ContactState::Inactive;
        RE::NiPoint3 point{};
        RE::NiPoint3 normalEnd{};
        RE::NiPoint3 correctionEnd{};
        float penetration = 0.0f;
        float responseScale = 0.0f;
        float maxCorrection = 0.0f;
        float correctionLength = 0.0f;
        std::uint32_t movableId = 0;
        std::uint32_t targetId = 0;
        std::uint32_t targetLayer = contact_target_identity::kUnknownLayer;
        std::uint32_t targetFilterInfo = contact_target_identity::kUnknownFilterInfo;
        std::uint32_t targetRefFormId = contact_target_identity::kInvalidFormId;
        std::uint32_t targetBaseFormId = contact_target_identity::kInvalidFormId;
        contact_target_identity::SurfaceHint surfaceHint = contact_target_identity::SurfaceHint::Unknown;
    };

    struct SoftContactDebugSnapshot
    {
        static constexpr std::size_t kMaxContacts = 16;
        std::array<SoftContactDebugContact, kMaxContacts> contacts{};
        std::uint32_t contactCount = 0;
        soft_contact_math::ContactState rightState = soft_contact_math::ContactState::Inactive;
        soft_contact_math::ContactState leftState = soft_contact_math::ContactState::Inactive;
    };

    class SoftContactRuntime
    {
    public:
        static constexpr std::size_t kMaxWorldContactProbesPerHand =
            soft_contact_policy::kWeaponWorldProbeStateCapacity;

        void reset();

        void clearHandForStrongerOwner(bool isLeft, const char* reason);

        void update(const PhysicsFrameContext& frame,
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
            const contact_evidence::NativeContactEvidenceSnapshot& nativeContactEvidence);

        bool getDebugSnapshot(SoftContactDebugSnapshot& outSnapshot) const;

        bool getWeaponWorldCorrection(
            RE::NiPoint3& outCorrection,
            bool& outFiringHandIsLeft) const;

        bool getWeaponWorldStopPose(
            RE::NiTransform& outWeaponWorld,
            bool& outFiringHandIsLeft) const;

        // Read-only wall-plane view of the already-retained weapon stop. Hand
        // targets deliberately return false: this accessor does not create a
        // second contact authority or let reciprocal hand stops move the
        // player controller.
        bool getWeaponWorldStopSurface(
            RE::NiPoint3& outSurfacePointWorld,
            RE::NiPoint3& outSurfaceNormalWorld,
            bool& outTargetIsDynamicHand) const;

        /*
         * Reports the free dynamic hand targeted by the retained reciprocal
         * weapon stop.  This is presentation ownership only; callers must not
         * use it to mark the hand physically occupied or contact evidence
         * would invalidate its own stop.
         */
        bool getWeaponHandStopTarget(bool& outTargetHandIsLeft) const;

    private:
        struct HandRuntime
        {
            struct WorldProbeState
            {
                soft_contact_policy::SparseProbeQueryHistory<
                    RE::NiPoint3>
                    history{};
                float restQueryCooldownSeconds = 0.0f;
            };

            struct CachedWorldPlane
            {
                bool active = false;
                bool sourceIsWeapon = false;
                std::uint32_t bodyId = 0x7FFF'FFFFu;
                std::uint32_t probeId = 0;
                std::uint32_t weaponSourceBodyId = 0x7FFF'FFFFu;
                bool weaponAnchorValid = false;
                bool weaponAnchorUsesSourceLocal = false;
                RE::NiPoint3 weaponAnchorLocal{};
                float weaponProbeRadius = 0.0f;
                RE::NiPoint3 surfacePoint{};
                RE::NiPoint3 normal{};
                float approachSpeedGameUnits = 0.0f;
                contact_target_identity::ContactTargetIdentity targetIdentity{};
            };

            struct ReleaseBlend
            {
                bool active = false;
                RE::NiTransform startWorld{};
                float elapsedSeconds = 0.0f;
                float durationSeconds = 0.0f;
            };

            soft_contact_math::ContactState state = soft_contact_math::ContactState::Inactive;
            soft_contact_math::ContactKind lastContactKind = soft_contact_math::ContactKind::None;
            bool lastContactSourceIsWeapon = false;
            RE::NiPoint3 correction{};
            RE::NiTransform lastAppliedWorld{};
            bool externalTransformActive = false;
            ReleaseBlend releaseBlend{};
            std::array<WorldProbeState, kMaxWorldContactProbesPerHand> worldProbes{};
            std::uint32_t worldProbeCastCursor = 0;
            bool weaponSampleLayoutInitialized = false;
            std::uint64_t weaponSampleLayoutSignature = 0;
            std::uint32_t weaponSampleLayoutCount = 0;
            bool weaponAuthorityDiscontinuityThisFrame = false;
            CachedWorldPlane cachedWorldPlane{};
            // A bounded sampled-probe-only companion keeps a perpendicular
            // corner plane alive after its acquisition sweep. Exact native
            // anchors remain primary-only so state IDs never alias.
            CachedWorldPlane secondaryCachedWorldPlane{};
            soft_contact_math::HapticEdgeState worldHaptic{};
        };

        struct WeaponWorldStopState
        {
            bool active = false;
            std::uint64_t generationKey = 0;
            RE::NiTransform blockedWeaponWorld{};
            RE::NiPoint3 surfacePointWorld{};
            RE::NiPoint3 surfaceNormalWorld{};
            std::uint32_t sourceBodyId = 0x7FFF'FFFFu;
            bool targetIsDynamicHand = false;
            bool targetHandIsLeft = false;
            std::uint32_t targetDynamicHandBodyId =
                0x7FFF'FFFFu;
            std::uint32_t targetHandCollisionLayer = 0;
            bool anchorUsesSourceLocal = false;
            RE::NiPoint3 anchorLocal{};
            float probeRadiusGame = 0.0f;
            float missElapsedSeconds = 0.0f;
            // Once a resolved raw probe proves outward plane exit, continue
            // the bounded full-pose return until completion. Without this
            // latch, the next stationary outside sample could re-retain the
            // partially released pose because it contains no new retreat.
            bool releaseActive = false;
            bool previousRawProbeValid = false;
            float previousRawProbeSignedDistance = 0.0f;
            bool previousRawWeaponWorldValid = false;
            RE::NiTransform previousRawWeaponWorld{};
        };

        void clearHand(bool isLeft);
        void clearAllHands();

        std::array<HandRuntime, 2> _hands{};
        // Weapon hull/world contact is intentionally independent of both hand
        // channels. Sharing a cached plane made a palm and muzzle overwrite
        // one another and alternate visual authority from frame to frame.
        HandRuntime _weapon{};
        WeaponWorldStopState _weaponWorldStop{};
        std::array<
            soft_contact_policy::WeaponHandContactEpisodeState,
            2>
            _weaponHandContactEpisodes{};
        std::uint64_t _weaponGenerationKey = 0;
        SoftContactDebugSnapshot _debugSnapshot{};
        RE::NiPoint3 _weaponWorldCorrection{};
        bool _weaponWorldCorrectionActive = false;
        bool _weaponWorldCorrectionHandIsLeft = false;
        std::array<bool, 2> _wasHandWorldEnabled{};
        std::uint32_t _logCounter = 0;
    };
}
