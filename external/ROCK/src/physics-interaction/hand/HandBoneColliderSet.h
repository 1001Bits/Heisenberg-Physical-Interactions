#pragma once

#include "physics-interaction/native/BethesdaPhysicsBody.h"
#include "physics-interaction/hand/HandSkeleton.h"
#include "physics-interaction/native/GeneratedKeyframedBodyDrive.h"
#include "physics-interaction/hand/HandColliderTypes.h"
#include "physics-interaction/native/HavokPhysicsTiming.h"

#include "RE/Havok/hknpShape.h"
#include "RE/Havok/hknpWorld.h"
#include "RE/NetImmerse/NiPoint.h"
#include "RE/NetImmerse/NiTransform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace rock
{
    struct HandColliderBodyMetadata
    {
        bool valid = false;
        bool isLeft = false;
        bool primaryPalmAnchor = false;
        hand_collider_semantics::HandColliderRole role = hand_collider_semantics::HandColliderRole::PalmAnchor;
        hand_collider_semantics::HandFinger finger = hand_collider_semantics::HandFinger::None;
        hand_collider_semantics::HandFingerSegment segment = hand_collider_semantics::HandFingerSegment::None;
        std::uint32_t bodyId = hand_collider_semantics::kInvalidBodyId;
        bool hasSampledLinearVelocityHavok = false;
        float sampledLinearVelocityHavok[4]{};
    };

    struct HandColliderFrameSnapshot
    {
        bool valid = false;
        hand_collider_semantics::HandColliderRole role =
            hand_collider_semantics::HandColliderRole::PalmAnchor;
        RE::NiTransform transform{};
        float lengthGameUnits = 0.0f;
        float radiusGameUnits = 0.0f;
        float convexRadiusGameUnits = 0.0f;
    };

    class HandBoneColliderSet
    {
    public:
        HandBoneColliderSet();

        bool create(
            RE::hknpWorld* world,
            void* bhkWorld,
            bool isLeft,
            const RE::NiTransform& rollAuthorityWorld,
            BethesdaPhysicsBody& palmAnchorBody,
            const RE::NiPoint3& authorityTranslationOffsetGame = RE::NiPoint3{});
        void destroy(void* bhkWorld, BethesdaPhysicsBody& palmAnchorBody);
        void reset();
        // suppressionActive: true while ANY Hand collision-suppression lease (grab hold,
        // host-external, held-loose-weapon) references these colliders' bodyIds. Mirrors
        // the existing palmAnchorBody.isConstrained() rebuild-deferral: an internal rebuild
        // (drive failure, source/tuning change) destroys all 16 bodies and creates
        // replacements with FRESH bodyIds and the plain unsuppressed filter, and nothing
        // re-arms the caller's suppression leases for the new ids afterward - without this,
        // a mid-hold rebuild (skeleton refresh, power-armor enter/exit, tuning INI reload)
        // silently un-suppresses the hand colliders while the caller still believes
        // collision is off, producing contact-force jitter on the held object.
        void update(
            RE::hknpWorld* world,
            bool isLeft,
            const RE::NiTransform& rollAuthorityWorld,
            BethesdaPhysicsBody& palmAnchorBody,
            float deltaTime,
            const RE::NiPoint3& authorityTranslationOffsetGame = RE::NiPoint3{},
            bool suppressionActive = false);
        void flushPendingPhysicsDrive(RE::hknpWorld* world, const havok_physics_timing::PhysicsTimingSample& timing, BethesdaPhysicsBody& palmAnchorBody);

        bool hasBodies() const { return _created; }
        std::uint32_t getBodyCount() const { return _bodyCountAtomic.load(std::memory_order_acquire); }
        std::uint32_t getBodyIdAtomic(std::size_t index) const;
        bool isColliderBodyIdAtomic(std::uint32_t bodyId) const;
        bool tryGetBodyMetadataAtomic(std::uint32_t bodyId, HandColliderBodyMetadata& outMetadata) const;
        bool tryGetBodyRoleAtomic(std::uint32_t bodyId, hand_collider_semantics::HandColliderRole& outRole) const;
        bool tryGetCollisionFrame(
            std::uint32_t bodyId,
            HandColliderFrameSnapshot& outFrame) const;
        bool tryGetPalmAnchorTarget(RE::NiTransform& outTarget) const;
        std::uint32_t copyCollisionSamples(
            RE::NiPoint3* outWorldPoints,
            float* outRadiiGame,
            std::uint32_t maxSamples) const;

    private:
        static constexpr std::size_t MAX_SEGMENT_BODIES = hand_collider_semantics::kHandSegmentColliderBodyCountPerHand;
        static constexpr std::uint32_t kInvalidPublicationIndex = 0xFFFF'FFFFu;

        struct BodyInstance
        {
            BethesdaPhysicsBody body;
            const RE::hknpShape* shape = nullptr;
            hand_collider_semantics::HandColliderRole role = hand_collider_semantics::HandColliderRole::PalmFace;
            bool ownsShapeRef = false;
            GeneratedKeyframedBodyDriveState driveState{};
            std::uint32_t publicationIndex = kInvalidPublicationIndex;
            RE::NiTransform collisionFrameTransform{};
            float collisionFrameLength = 1.0f;
            float collisionFrameRadius = 0.5f;
            float collisionFrameConvexRadius = 0.1f;
            bool collisionFrameValid = false;
        };

        struct BoneFrameLookup
        {
            bool valid = false;
            RE::NiTransform hand{};
            RE::NiTransform rollAuthorityWorld{};
            RE::NiTransform forearm3{};
            std::array<std::array<RE::NiTransform, 3>, hand_collider_semantics::kHandFingerCount> fingers{};
            std::array<bool, hand_collider_semantics::kHandFingerCount> fingerValid{};
            std::array<RE::NiPoint3, hand_collider_semantics::kHandFingerCount> fingerBases{};
            RE::NiPoint3 crossPalmDirection{};
            bool hasForearm3 = false;
        };

        struct RoleFrameResult
        {
            bool valid = false;
            RE::NiTransform transform{};
            float length = 1.0f;
            float radius = 0.5f;
            float convexRadius = 0.1f;
        };

        bool captureBoneLookup(
            bool isLeft,
            const RE::NiTransform& rollAuthorityWorld,
            const RE::NiPoint3& authorityTranslationOffsetGame,
            BoneFrameLookup& outLookup);
        void applyAuthorityTranslationOffset(BoneFrameLookup& lookup, const RE::NiPoint3& authorityTranslationOffsetGame) const;
        bool makeRoleFrame(const BoneFrameLookup& lookup, bool isLeft, hand_collider_semantics::HandColliderRole role, RoleFrameResult& outFrame) const;
        std::vector<RE::NiPoint3> makeLocalCollisionPointsForRole(
            const RoleFrameResult& frame,
            hand_collider_semantics::HandColliderRole role) const;
        RE::hknpShape* buildShapeForRole(const RoleFrameResult& frame, hand_collider_semantics::HandColliderRole role) const;
        bool createBodyForRole(RE::hknpWorld* world, void* bhkWorld, bool isLeft, hand_collider_semantics::HandColliderRole role, const RoleFrameResult& frame, BodyInstance& instance);
        void queueBodyTarget(BethesdaPhysicsBody& body, const RE::NiTransform& target, float sourceDeltaSeconds, GeneratedKeyframedBodyDriveState& driveState, std::uint32_t publicationIndex);
        void handleGeneratedBodyDriveResult(const GeneratedKeyframedBodyDriveResult& result, const char* ownerName, std::uint32_t bodyIndex);
        void clearInstance(BodyInstance& instance, bool releaseShapeRef);
        void publishAtomicBodyIds(const BethesdaPhysicsBody& palmAnchorBody, bool isLeft);
        void publishSampledVelocityAtomic(std::uint32_t publicationIndex, const GeneratedKeyframedBodyDriveQueueResult& queueResult);
        void clearAtomicBodyIds();

        DirectSkeletonBoneReader _reader;
        std::array<BodyInstance, MAX_SEGMENT_BODIES> _bodies{};
        RE::hknpWorld* _cachedWorld = nullptr;
        void* _cachedBhkWorld = nullptr;
        GeneratedKeyframedBodyDriveState _palmAnchorDriveState{};
        RE::NiTransform _latestPalmAnchorTarget{};
        bool _hasLatestPalmAnchorTarget = false;
        RoleFrameResult _palmAnchorCollisionFrame{};
        const void* _cachedSkeleton = nullptr;
        const void* _cachedBoneTree = nullptr;
        bool _cachedPowerArmor = false;
        std::uint64_t _cachedTuningSignature = 0;
        const void* _lastCapturedSkeleton = nullptr;
        const void* _lastCapturedBoneTree = nullptr;
        bool _lastCapturedPowerArmor = false;
        std::atomic<std::uint32_t> _isLeftAtomic{ 0 };
        std::atomic<bool> _driveRebuildRequested{ false };
        std::atomic<std::uint32_t> _driveFailureCount{ 0 };
        std::uint32_t _palmAnchorPublicationIndex = kInvalidPublicationIndex;
        bool _created = false;
        int _updateLogCounter = 0;

        std::array<std::atomic<std::uint32_t>, hand_collider_semantics::kHandColliderBodyCountPerHand> _bodyIdsAtomic{};
        std::array<std::atomic<std::uint32_t>, hand_collider_semantics::kHandColliderBodyCountPerHand> _rolesAtomic{};
        std::array<std::atomic<std::uint32_t>, hand_collider_semantics::kHandColliderBodyCountPerHand> _fingersAtomic{};
        std::array<std::atomic<std::uint32_t>, hand_collider_semantics::kHandColliderBodyCountPerHand> _segmentsAtomic{};
        std::array<std::atomic<std::uint32_t>, hand_collider_semantics::kHandColliderBodyCountPerHand> _primaryAnchorAtomic{};
        std::array<std::atomic<float>, hand_collider_semantics::kHandColliderBodyCountPerHand> _sampledVelocityHavokXAtomic{};
        std::array<std::atomic<float>, hand_collider_semantics::kHandColliderBodyCountPerHand> _sampledVelocityHavokYAtomic{};
        std::array<std::atomic<float>, hand_collider_semantics::kHandColliderBodyCountPerHand> _sampledVelocityHavokZAtomic{};
        std::array<std::atomic<std::uint32_t>, hand_collider_semantics::kHandColliderBodyCountPerHand> _sampledVelocityValidAtomic{};
        std::atomic<std::uint32_t> _bodyCountAtomic{ 0 };
    };
}
