#pragma once

#include "physics-interaction/contact/SoftContactMath.h"
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
        static constexpr std::size_t kMaxWorldContactProbesPerHand = 23;

        void reset();

        void clearHandForStrongerOwner(bool isLeft, const char* reason);

        void update(const PhysicsFrameContext& frame,
            const Hand& rightHand,
            const Hand& leftHand,
            bool rightHandWeaponEquipped,
            bool leftSupportGripActive,
            bool weaponFiringHandIsLeft,
            const WeaponCollision* weaponCollision,
            RE::NiNode* weaponNode,
            const contact_evidence::NativeContactEvidenceSnapshot& nativeContactEvidence);

        bool getDebugSnapshot(SoftContactDebugSnapshot& outSnapshot) const;

        bool getWeaponWorldCorrection(
            RE::NiPoint3& outCorrection,
            bool& outFiringHandIsLeft) const;

    private:
        struct HandRuntime
        {
            struct WorldProbeState
            {
                bool valid = false;
                RE::NiPoint3 previous{};
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
            CachedWorldPlane cachedWorldPlane{};
            soft_contact_math::HapticEdgeState worldHaptic{};
        };

        void clearHand(bool isLeft);
        void clearAllHands();

        std::array<HandRuntime, 2> _hands{};
        // Weapon hull/world contact is intentionally independent of both hand
        // channels. Sharing a cached plane made a palm and muzzle overwrite
        // one another and alternate visual authority from frame to frame.
        HandRuntime _weapon{};
        std::uint64_t _weaponGenerationKey = 0;
        SoftContactDebugSnapshot _debugSnapshot{};
        RE::NiPoint3 _weaponWorldCorrection{};
        bool _weaponWorldCorrectionActive = false;
        bool _weaponWorldCorrectionHandIsLeft = false;
        bool _wasHandWorldEnabled = false;
        std::uint32_t _logCounter = 0;
    };
}
