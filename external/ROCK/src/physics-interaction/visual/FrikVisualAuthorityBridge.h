#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ROCKMain.h"
#include "RockConfig.h"
#include "../../api/FRIKApi.h"
#include "physics-interaction/visual/FrikCompatibilityPolicy.h"

namespace rock::frik_visual_authority
{
    using Hand = frik::api::FRIKApi::Hand;
    using HandPoseKind = frik::api::FRIKApi::HandPoseKind;
    using HandPoseTagState = frik::api::FRIKApi::HandPoseTagState;
    using HandPoseData = frik::api::FRIKApi::HandPoseData;
    using FingerLocalTransformOverride = frik::api::FRIKApi::FingerLocalTransformOverride;

    namespace detail
    {
        constexpr std::size_t kCachedHandPosePublicationCount = 8;
        constexpr std::size_t kCachedHandPoseTagCapacity = 64;

        struct CachedHandPosePublication
        {
            std::array<char, kCachedHandPoseTagCapacity> tag{};
            std::size_t tagLength = 0;
            Hand hand = Hand::Left;
            int priority = 0;
            HandPoseData pose{};
            bool valid = false;
        };

        struct CachedFingerLocalTransformPublication
        {
            std::array<char, kCachedHandPoseTagCapacity> tag{};
            std::size_t tagLength = 0;
            Hand hand = Hand::Left;
            int priority = 0;
            FingerLocalTransformOverride transforms{};
            bool valid = false;
        };

        inline std::array<CachedHandPosePublication, kCachedHandPosePublicationCount> g_cachedHandPosePublications{};
        inline std::array<CachedFingerLocalTransformPublication, kCachedHandPosePublicationCount> g_cachedFingerLocalTransformPublications{};
        inline std::size_t g_nextCachedHandPosePublication = 0;
        inline std::size_t g_nextCachedFingerLocalTransformPublication = 0;

        using BlockPrimaryHandWeaponPoseFn = bool(FRIK_CALL*)(const char*, bool);
        using MirrorPrimaryWeaponFingerLocalTransformsFn = bool(FRIK_CALL*)(const FingerLocalTransformOverride*, FingerLocalTransformOverride*);
        using MirrorFingerLocalTransformsFn = bool(FRIK_CALL*)(Hand, const FingerLocalTransformOverride*, FingerLocalTransformOverride*);

        [[nodiscard]] inline BlockPrimaryHandWeaponPoseFn blockPrimaryHandWeaponPoseExport()
        {
            static BlockPrimaryHandWeaponPoseFn fn = nullptr;
            static bool attemptedWithLoadedFrik = false;
            if (!fn) {
                const auto frikDll = GetModuleHandleA("FRIK.dll");
                if (!frikDll) {
                    return nullptr;
                }
                if (attemptedWithLoadedFrik) {
                    return nullptr;
                }
                attemptedWithLoadedFrik = true;
                fn = reinterpret_cast<BlockPrimaryHandWeaponPoseFn>(GetProcAddress(frikDll, "FRIKAPI_BlockPrimaryHandWeaponPose"));
            }
            return fn;
        }

        [[nodiscard]] inline MirrorPrimaryWeaponFingerLocalTransformsFn mirrorPrimaryWeaponFingerLocalTransformsExport()
        {
            static MirrorPrimaryWeaponFingerLocalTransformsFn fn = nullptr;
            static bool attemptedWithLoadedFrik = false;
            if (!fn) {
                const auto frikDll = GetModuleHandleA("FRIK.dll");
                if (!frikDll) {
                    return nullptr;
                }
                if (attemptedWithLoadedFrik) {
                    return nullptr;
                }
                attemptedWithLoadedFrik = true;
                fn = reinterpret_cast<MirrorPrimaryWeaponFingerLocalTransformsFn>(GetProcAddress(frikDll, "FRIKAPI_MirrorPrimaryWeaponFingerLocalTransforms"));
            }
            return fn;
        }

        [[nodiscard]] inline MirrorFingerLocalTransformsFn mirrorFingerLocalTransformsExport()
        {
            static MirrorFingerLocalTransformsFn fn = nullptr;
            static bool attemptedWithLoadedFrik = false;
            if (!fn) {
                const auto frikDll = GetModuleHandleA("FRIK.dll");
                if (!frikDll) {
                    return nullptr;
                }
                if (attemptedWithLoadedFrik) {
                    return nullptr;
                }
                attemptedWithLoadedFrik = true;
                fn = reinterpret_cast<MirrorFingerLocalTransformsFn>(GetProcAddress(frikDll, "FRIKAPI_MirrorFingerLocalTransforms"));
            }
            return fn;
        }

        [[nodiscard]] inline bool makeCacheableTagView(const char* tag, std::string_view& outTag)
        {
            if (!tag) {
                return false;
            }

            outTag = std::string_view(tag);
            return !outTag.empty() && outTag.size() < kCachedHandPoseTagCapacity;
        }

        [[nodiscard]] inline std::string_view cachedTagView(const CachedHandPosePublication& entry)
        {
            return std::string_view(entry.tag.data(), entry.tagLength);
        }

        [[nodiscard]] inline std::string_view cachedTagView(const CachedFingerLocalTransformPublication& entry)
        {
            return std::string_view(entry.tag.data(), entry.tagLength);
        }

        [[nodiscard]] inline bool sameFingerPoseData(
            const frik::api::FRIKApi::FingerPoseData& lhs,
            const frik::api::FRIKApi::FingerPoseData& rhs)
        {
            return lhs.prox == rhs.prox &&
                   lhs.mid == rhs.mid &&
                   lhs.dist == rhs.dist &&
                   lhs.splay == rhs.splay;
        }

        [[nodiscard]] inline bool sameHandPoseData(const HandPoseData& lhs, const HandPoseData& rhs)
        {
            return sameFingerPoseData(lhs.thumb, rhs.thumb) &&
                   sameFingerPoseData(lhs.index, rhs.index) &&
                   sameFingerPoseData(lhs.middle, rhs.middle) &&
                   sameFingerPoseData(lhs.ring, rhs.ring) &&
                   sameFingerPoseData(lhs.pinky, rhs.pinky) &&
                   lhs.palmPitch == rhs.palmPitch &&
                   lhs.palmYaw == rhs.palmYaw;
        }

        [[nodiscard]] inline bool sameNiTransform(const RE::NiTransform& lhs, const RE::NiTransform& rhs)
        {
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if (lhs.rotate.entry[row][column] != rhs.rotate.entry[row][column]) {
                        return false;
                    }
                }
            }
            return lhs.translate.x == rhs.translate.x && lhs.translate.y == rhs.translate.y && lhs.translate.z == rhs.translate.z && lhs.scale == rhs.scale;
        }

        [[nodiscard]] inline bool sameFingerLocalTransforms(const FingerLocalTransformOverride& lhs, const FingerLocalTransformOverride& rhs)
        {
            if (lhs.enabledMask != rhs.enabledMask) {
                return false;
            }
            for (std::size_t index = 0; index < std::size(lhs.localTransforms); ++index) {
                const std::uint16_t bit = static_cast<std::uint16_t>(1U << index);
                if ((lhs.enabledMask & bit) != 0 && !sameNiTransform(lhs.localTransforms[index], rhs.localTransforms[index])) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] inline CachedHandPosePublication* findCachedHandPosePublication(std::string_view tag, Hand hand)
        {
            for (auto& entry : g_cachedHandPosePublications) {
                if (entry.valid && entry.hand == hand && cachedTagView(entry) == tag) {
                    return &entry;
                }
            }
            return nullptr;
        }

        [[nodiscard]] inline CachedFingerLocalTransformPublication* findCachedFingerLocalTransformPublication(std::string_view tag, Hand hand)
        {
            for (auto& entry : g_cachedFingerLocalTransformPublications) {
                if (entry.valid && entry.hand == hand && cachedTagView(entry) == tag) {
                    return &entry;
                }
            }
            return nullptr;
        }

        inline void invalidateCachedHandPosePublication(const char* tag, Hand hand)
        {
            std::string_view tagView;
            if (!makeCacheableTagView(tag, tagView)) {
                return;
            }

            if (auto* entry = findCachedHandPosePublication(tagView, hand)) {
                entry->valid = false;
            }
        }

        inline void invalidateCachedFingerLocalTransformPublication(const char* tag, Hand hand)
        {
            std::string_view tagView;
            if (!makeCacheableTagView(tag, tagView)) {
                return;
            }

            if (auto* entry = findCachedFingerLocalTransformPublication(tagView, hand)) {
                entry->valid = false;
            }
        }

        inline void rememberCachedHandPosePublication(std::string_view tag, Hand hand, const HandPoseData& handPose, int priority)
        {
            auto* entry = findCachedHandPosePublication(tag, hand);
            if (!entry) {
                for (auto& candidate : g_cachedHandPosePublications) {
                    if (!candidate.valid) {
                        entry = &candidate;
                        break;
                    }
                }
            }
            if (!entry) {
                entry = &g_cachedHandPosePublications[g_nextCachedHandPosePublication % g_cachedHandPosePublications.size()];
                ++g_nextCachedHandPosePublication;
            }

            entry->tag.fill('\0');
            std::copy(tag.begin(), tag.end(), entry->tag.begin());
            entry->tagLength = tag.size();
            entry->hand = hand;
            entry->priority = priority;
            entry->pose = handPose;
            entry->valid = true;
        }

        inline void rememberCachedFingerLocalTransformPublication(std::string_view tag,
            Hand hand, const FingerLocalTransformOverride& transforms,
            int priority)
        {
            auto* entry = findCachedFingerLocalTransformPublication(tag, hand);
            if (!entry) {
                for (auto& candidate : g_cachedFingerLocalTransformPublications) {
                    if (!candidate.valid) {
                        entry = &candidate;
                        break;
                    }
                }
            }
            if (!entry) {
                entry = &g_cachedFingerLocalTransformPublications[g_nextCachedFingerLocalTransformPublication % g_cachedFingerLocalTransformPublications.size()];
                ++g_nextCachedFingerLocalTransformPublication;
            }

            entry->tag.fill('\0');
            std::copy(tag.begin(), tag.end(), entry->tag.begin());
            entry->tagLength = tag.size();
            entry->hand = hand;
            entry->priority = priority;
            entry->transforms = transforms;
            entry->valid = true;
        }

        [[nodiscard]] inline bool shouldSkipCachedHandPosePublication(
            const char* tag,
            Hand hand,
            const HandPoseData& handPose,
            int priority,
            bool publicationStillActive,
            std::string_view& outTagView)
        {
            if (!makeCacheableTagView(tag, outTagView)) {
                return false;
            }

            const auto* entry = findCachedHandPosePublication(outTagView, hand);
            return entry &&
                   entry->priority == priority &&
                   sameHandPoseData(entry->pose, handPose) &&
                   publicationStillActive;
        }

        [[nodiscard]] inline bool shouldSkipCachedFingerLocalTransformPublication(
            const char* tag,
            Hand hand,
            const FingerLocalTransformOverride& transforms,
            int priority,
            bool publicationStillActive,
            std::string_view& outTagView)
        {
            if (!makeCacheableTagView(tag, outTagView)) {
                return false;
            }

            const auto* entry = findCachedFingerLocalTransformPublication(outTagView, hand);
            return entry && entry->priority == priority && sameFingerLocalTransforms(entry->transforms, transforms) &&
                   publicationStillActive;
        }
    }

    [[nodiscard]] inline float finiteOrZero(float value)
    {
        return std::isfinite(value) ? value : 0.0f;
    }

    // FRIK 0.77.12 exposes the API-v3 prefix only. Keep that prefix available
    // for readiness/config/finger-pose calls, but never expose it to callers
    // that probe the appended v5 members.
    [[nodiscard]] inline const frik::api::FRIKApi* coreApi()
    {
        return frik::api::FRIKApi::inst;
    }

    [[nodiscard]] inline std::uint32_t apiVersion()
    {
        const auto* frikApi = coreApi();
        return frikApi && frikApi->getVersion ? frikApi->getVersion() : 0;
    }

    [[nodiscard]] inline bool hasApiVersion(std::uint32_t minimum)
    {
        return apiVersion() >= minimum;
    }

    [[nodiscard]] inline HandPoseData makeHandPoseDataFromJointValues(
        const float values[15],
        const std::array<float, 5>& splayRadians,
        float palmPitchDegrees = 0.0f,
        float palmYawDegrees = 0.0f)
    {
        return HandPoseData{
            .thumb = { values[0], values[1], values[2], finiteOrZero(splayRadians[0]) },
            .index = { values[3], values[4], values[5], finiteOrZero(splayRadians[1]) },
            .middle = { values[6], values[7], values[8], finiteOrZero(splayRadians[2]) },
            .ring = { values[9], values[10], values[11], finiteOrZero(splayRadians[3]) },
            .pinky = { values[12], values[13], values[14], finiteOrZero(splayRadians[4]) },
            .palmPitch = finiteOrZero(palmPitchDegrees),
            .palmYaw = finiteOrZero(palmYawDegrees),
        };
    }

    [[nodiscard]] inline HandPoseData makeHandPoseDataFromJointValues(const float values[15])
    {
        return makeHandPoseDataFromJointValues(values, std::array<float, 5>{});
    }

    [[nodiscard]] inline HandPoseData makeHandPoseDataFromJointValues(
        const std::array<float, 15>& values,
        const std::array<float, 5>& splayRadians,
        float palmPitchDegrees = 0.0f,
        float palmYawDegrees = 0.0f)
    {
        return makeHandPoseDataFromJointValues(values.data(), splayRadians, palmPitchDegrees, palmYawDegrees);
    }

    [[nodiscard]] inline HandPoseData makeHandPoseDataFromJointValues(const std::array<float, 15>& values)
    {
        return makeHandPoseDataFromJointValues(values.data());
    }

    [[nodiscard]] inline HandPoseData makeUniformHandPoseData(
        float thumb,
        float index,
        float middle,
        float ring,
        float pinky,
        const std::array<float, 5>& splayRadians = {},
        float palmPitchDegrees = 0.0f,
        float palmYawDegrees = 0.0f)
    {
        return HandPoseData{
            .thumb = { thumb, thumb, thumb, finiteOrZero(splayRadians[0]) },
            .index = { index, index, index, finiteOrZero(splayRadians[1]) },
            .middle = { middle, middle, middle, finiteOrZero(splayRadians[2]) },
            .ring = { ring, ring, ring, finiteOrZero(splayRadians[3]) },
            .pinky = { pinky, pinky, pinky, finiteOrZero(splayRadians[4]) },
            .palmPitch = finiteOrZero(palmPitchDegrees),
            .palmYaw = finiteOrZero(palmYawDegrees),
        };
    }

    // Stock FRIK 0.77.12's named-pose API is a stub that reports success
    // without changing any fingers. Re-express the named poses that have an
    // exact 0.77.12 authored equivalent as canonical joint data so the normal
    // scalar + rich-host publication path can render them for real.
    [[nodiscard]] inline bool makeStockV3NamedHandPoseData(
        HandPoseKind kind,
        HandPoseData& outPose)
    {
        std::array<float, 15> values{};
        switch (kind) {
        case HandPoseKind::Open:
            values.fill(1.0f);
            break;
        case HandPoseKind::Fist:
            values.fill(0.0f);
            break;
        case HandPoseKind::Pointing:
            values = {
                0.0f, 0.0f, 0.0f,
                1.0f, 1.0f, 1.0f,
                0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f,
            };
            break;
        case HandPoseKind::HoldingGun:
            values = {
                0.7f, 0.4f, 0.5f,
                0.9f, 0.6f, 0.5f,
                0.3f, 0.5f, 0.5f,
                0.1f, 0.5f, 0.5f,
                0.0f, 0.5f, 0.7f,
            };
            break;
        case HandPoseKind::HoldingMelee:
            values = {
                0.7f, 0.5f, 0.8f,
                0.4f, 0.3f, 0.9f,
                0.1f, 0.5f, 0.9f,
                0.0f, 0.5f, 0.9f,
                0.0f, 0.4f, 0.9f,
            };
            break;
        default:
            outPose = {};
            return false;
        }

        outPose = makeHandPoseDataFromJointValues(values);
        return true;
    }

    [[nodiscard]] inline const frik::api::FRIKApi* api()
    {
        return frik_compatibility_policy::mayReadNativeVisualAuthorityTail(
                   coreApi() != nullptr,
                   apiVersion()) ?
            coreApi() :
            nullptr;
    }

    [[nodiscard]] inline Hand handFromBool(bool isLeft)
    {
        return isLeft ? Hand::Left : Hand::Right;
    }

    [[nodiscard]] inline bool isAvailable()
    {
        // This flag gates the whole physics runtime, not just optional visual
        // hand placement. The stock v3 core is sufficient for collision.
        return coreApi() != nullptr;
    }

    [[nodiscard]] inline int hostPhysicalHandIndex(Hand hand)
    {
        const auto mapping = frik_compatibility_policy::mapFrikHandToHost(
            static_cast<frik_compatibility_policy::FrikHand>(hand),
            g_rockConfig.rockLeftHandedMode);
        return static_cast<int>(mapping.hand);
    }

    [[nodiscard]] inline bool hasHostHandAuthority()
    {
        const auto* host = rock::getHostHandAuthority();
        return host && host->apply && host->clear && host->get;
    }

    [[nodiscard]] inline bool hasHostFingerPoseAuthority()
    {
        const auto* host = rock::getHostFingerPoseAuthority();
        return host && host->applyPose && host->clear && host->isActive;
    }

    [[nodiscard]] inline bool handPosePublicationStillActive(
        const char* tag,
        Hand hand)
    {
        if (!tag || tag[0] == '\0') {
            return false;
        }

        if (const auto* native = api();
            native && native->getHandPoseSetTagState) {
            return native->getHandPoseSetTagState(tag, hand) ==
                   HandPoseTagState::Active;
        }

        const auto* host = rock::getHostFingerPoseAuthority();
        if (host && host->isActive) {
            return host->isActive(
                tag,
                hostPhysicalHandIndex(hand));
        }

        // Keep compatibility with any pre-v5 provider that implements the
        // core tag-state member. Stock 0.77.12 returns None here, so it keeps
        // the historical fail-open per-frame scalar publication when no host
        // finger authority is installed.
        const auto* core = coreApi();
        return core && core->getHandPoseSetTagState &&
               core->getHandPoseSetTagState(tag, hand) ==
                   HandPoseTagState::Active;
    }

    [[nodiscard]] inline bool canBuildHandPoseLocalTransforms()
    {
        const auto* native = api();
        if (native && native->getHandPoseLocalTransformsForPose) {
            return true;
        }
        const auto* host = rock::getHostFingerPoseAuthority();
        return host && host->buildPoseLocalTransforms;
    }

    [[nodiscard]] inline bool canSetHandPoseCustomLocalTransforms()
    {
        const auto* native = api();
        if (native && native->setHandPoseCustomLocalTransformsWithPriority) {
            return true;
        }
        const auto* host = rock::getHostFingerPoseAuthority();
        return host && host->applyLocalTransforms;
    }

    [[nodiscard]] inline bool canApplyExternalHandWorldTransform()
    {
        const auto* frikApi = api();
        return (frikApi && frikApi->applyExternalHandWorldTransform &&
                   frikApi->clearExternalHandWorldTransform) ||
               hasHostHandAuthority();
    }

    [[nodiscard]] inline bool canReadHandWorldTransform()
    {
        const auto* frikApi = api();
        return (frikApi && frikApi->getHandWorldTransform) ||
               hasHostHandAuthority();
    }

    [[nodiscard]] inline bool isSkeletonReadyHint()
    {
        const auto* frikApi = coreApi();
        return frikApi && frikApi->isSkeletonReady && frikApi->isSkeletonReady();
    }

    [[nodiscard]] inline bool isCompatibilityConfigBlocking()
    {
        const auto* frikApi = coreApi();
        return frikApi &&
               frikApi->isConfigOpen &&
               frikApi->isConfigOpen();
    }

    [[nodiscard]] inline bool clearHandPose(const char* tag, Hand hand)
    {
        detail::invalidateCachedHandPosePublication(tag, hand);
        detail::invalidateCachedFingerLocalTransformPublication(tag, hand);

        const auto* host = rock::getHostFingerPoseAuthority();
        const bool hostCleared = host && host->clear &&
            host->clear(tag, hostPhysicalHandIndex(hand));

        const auto* frikApi = coreApi();
        const bool coreCleared =
            frikApi && frikApi->clearHandPose &&
            frikApi->clearHandPose(tag, hand);
        return coreCleared || hostCleared;
    }

    [[nodiscard]] inline bool setHandPoseCustomWithPriority(const char* tag, Hand hand, const HandPoseData& handPose, int priority)
    {
        const auto* core = coreApi();
        if (!core) {
            detail::invalidateCachedHandPosePublication(tag, hand);
            return false;
        }

        std::string_view tagView;
        if (detail::shouldSkipCachedHandPosePublication(
                tag,
                hand,
                handPose,
                priority,
                handPosePublicationStillActive(tag, hand),
                tagView)) {
            return true;
        }

        bool published = false;
        bool richHostPublished = true;
        const auto* native = api();
        if (native && native->setHandPoseCustomWithPriority) {
            published = native->setHandPoseCustomWithPriority(tag, hand, handPose, priority);
        } else if (core->setHandPoseCustomFingerPositions) {
            // API v3 has only one scalar per finger. Preserve all working
            // stock-FRIK finger interaction by reducing the three joint values
            // to the same 0..1 scalar contract used by 0.77.12.
            const auto scalar = [](const frik::api::FRIKApi::FingerPoseData& finger) {
                return (finiteOrZero(finger.prox) +
                           finiteOrZero(finger.mid) +
                           finiteOrZero(finger.dist)) /
                       3.0f;
            };
            published = core->setHandPoseCustomFingerPositions(
                tag,
                hand,
                scalar(handPose.thumb),
                scalar(handPose.index),
                scalar(handPose.middle),
                scalar(handPose.ring),
                scalar(handPose.pinky));

            // The scalar call above is the fail-open stock-FRIK baseline. The
            // Heisenberg host backend retains the complete 15-joint+splay+palm
            // payload and applies it after FRIK's own v3 skeleton pass.
            if (published) {
                const auto* host = rock::getHostFingerPoseAuthority();
                if (host && host->applyPose) {
                    richHostPublished = host->applyPose(
                        tag,
                        hostPhysicalHandIndex(hand),
                        &handPose,
                        priority);
                    if (!richHostPublished && host->clear) {
                        // Never leave an older rich payload under this tag
                        // rendering over the newly accepted scalar fallback.
                        (void)host->clear(
                            tag,
                            hostPhysicalHandIndex(hand));
                    }
                }
            }
        }
        if (published) {
            // Updating the scalar pose replaces hFRIK's tagged entry and
            // clears any local-transform payload previously attached to it.
            detail::invalidateCachedFingerLocalTransformPublication(tag, hand);
            if (richHostPublished && !tagView.empty()) {
                detail::rememberCachedHandPosePublication(tagView, hand, handPose, priority);
            } else if (!richHostPublished) {
                detail::invalidateCachedHandPosePublication(tag, hand);
            }
        } else {
            detail::invalidateCachedHandPosePublication(tag, hand);
            detail::invalidateCachedFingerLocalTransformPublication(tag, hand);
        }
        return published;
    }

    [[nodiscard]] inline bool setHandPoseWithPriority(const char* tag, Hand hand, HandPoseKind handPose, int priority)
    {
        const auto* core = coreApi();
        if (!core) {
            detail::invalidateCachedHandPosePublication(tag, hand);
            detail::invalidateCachedFingerLocalTransformPublication(tag, hand);
            return false;
        }

        if (const auto* native = api(); native && native->setHandPoseWithPriority) {
            detail::invalidateCachedHandPosePublication(tag, hand);
            detail::invalidateCachedFingerLocalTransformPublication(tag, hand);
            return native->setHandPoseWithPriority(tag, hand, handPose, priority);
        }

        if (handPose == HandPoseKind::Unset) {
            return clearHandPose(tag, hand);
        }

        HandPoseData authoredPose{};
        if (!makeStockV3NamedHandPoseData(handPose, authoredPose)) {
            return false;
        }

        return setHandPoseCustomWithPriority(
            tag,
            hand,
            authoredPose,
            priority);
    }

    [[nodiscard]] inline bool applyExternalHandWorldTransform(const char* tag, Hand hand, const RE::NiTransform& worldTarget, int priority)
    {
        const auto* frikApi = api();
        if (frikApi && frikApi->applyExternalHandWorldTransform) {
            return frikApi->applyExternalHandWorldTransform(tag, hand, worldTarget, priority);
        }

        const auto* host = rock::getHostHandAuthority();
        return host && host->apply &&
               host->apply(tag, hostPhysicalHandIndex(hand), &worldTarget, priority);
    }

    [[nodiscard]] inline bool clearExternalHandWorldTransform(const char* tag, Hand hand)
    {
        const auto* frikApi = api();
        if (frikApi && frikApi->clearExternalHandWorldTransform) {
            return frikApi->clearExternalHandWorldTransform(tag, hand);
        }

        const auto* host = rock::getHostHandAuthority();
        return host && host->clear &&
               host->clear(tag, hostPhysicalHandIndex(hand));
    }

    [[nodiscard]] inline bool setHandPoseCustomLocalTransformsWithPriority(
        const char* tag,
        Hand hand,
        const FingerLocalTransformOverride* overrideData,
        int priority)
    {
        if (!overrideData) {
            detail::invalidateCachedFingerLocalTransformPublication(tag, hand);
            return false;
        }

        std::string_view tagView;
        if (detail::shouldSkipCachedFingerLocalTransformPublication(
                tag,
                hand,
                *overrideData,
                priority,
                handPosePublicationStillActive(tag, hand),
                tagView)) {
            return true;
        }

        bool published = false;
        if (const auto* native = api();
            native && native->setHandPoseCustomLocalTransformsWithPriority) {
            published = native->setHandPoseCustomLocalTransformsWithPriority(
                tag,
                hand,
                overrideData,
                priority);
        } else {
            const auto* host = rock::getHostFingerPoseAuthority();
            published = host && host->applyLocalTransforms &&
                host->applyLocalTransforms(
                    tag,
                    hostPhysicalHandIndex(hand),
                    overrideData,
                    priority);
        }
        if (published && !tagView.empty()) {
            detail::rememberCachedFingerLocalTransformPublication(tagView, hand, *overrideData, priority);
        } else if (!published) {
            detail::invalidateCachedFingerLocalTransformPublication(tag, hand);
        }
        return published;
    }

    [[nodiscard]] inline bool getHandPoseLocalTransformsForPose(
        Hand hand,
        const HandPoseData& handPose,
        FingerLocalTransformOverride* outTransforms)
    {
        if (!outTransforms) {
            return false;
        }

        if (const auto* native = api();
            native && native->getHandPoseLocalTransformsForPose) {
            return native->getHandPoseLocalTransformsForPose(
                hand,
                handPose,
                outTransforms);
        }

        const auto* host = rock::getHostFingerPoseAuthority();
        return host && host->buildPoseLocalTransforms &&
            host->buildPoseLocalTransforms(
                hostPhysicalHandIndex(hand),
                &handPose,
                outTransforms);
    }

    [[nodiscard]] inline bool blockOffHandWeaponGripping(const char* tag, bool block)
    {
        const auto* frikApi = coreApi();
        return hasApiVersion(3) &&
               frikApi &&
               frikApi->blockOffHandWeaponGripping &&
               frikApi->blockOffHandWeaponGripping(tag, block);
    }

    [[nodiscard]] inline bool blockPrimaryHandWeaponPose(const char* tag, bool block)
    {
        const auto fn = detail::blockPrimaryHandWeaponPoseExport();
        return fn && fn(tag, block);
    }

    [[nodiscard]] inline bool canBlockPrimaryHandWeaponPose()
    {
        return detail::blockPrimaryHandWeaponPoseExport() != nullptr;
    }

    [[nodiscard]] inline bool mirrorFingerLocalTransforms(Hand sourceHand, const FingerLocalTransformOverride& sourceTransforms, FingerLocalTransformOverride& outTargetTransforms)
    {
        if (const auto fn = detail::mirrorFingerLocalTransformsExport()) {
            return fn(sourceHand, &sourceTransforms, &outTargetTransforms);
        }
        if (sourceHand == Hand::Right) {
            const auto legacyFn = detail::mirrorPrimaryWeaponFingerLocalTransformsExport();
            return legacyFn && legacyFn(&sourceTransforms, &outTargetTransforms);
        }
        return false;
    }

    [[nodiscard]] inline bool mirrorPrimaryWeaponFingerLocalTransforms(const FingerLocalTransformOverride& rightTransforms, FingerLocalTransformOverride& outLeftTransforms)
    {
        return mirrorFingerLocalTransforms(Hand::Right, rightTransforms, outLeftTransforms);
    }

    [[nodiscard]] inline bool canMirrorPrimaryWeaponFingerLocalTransforms()
    {
        return detail::mirrorFingerLocalTransformsExport() != nullptr || detail::mirrorPrimaryWeaponFingerLocalTransformsExport() != nullptr;
    }

    [[nodiscard]] inline bool canMirrorFingerLocalTransforms() { return detail::mirrorFingerLocalTransformsExport() != nullptr; }

    // This callback was appended after the canonical v5 table without a version
    // bump.  Stock/canonical v5 uses that tail slot for a different Heisenberg
    // callback, so reading it through FRIKApi would be an out-of-bounds or
    // wrong-signature call.  Keep the capability disabled until FRIK exposes a
    // separately versioned contract (or a dedicated export) for it.
    [[nodiscard]] inline bool blockPrimaryWeaponNodeOwnership(const char* tag, bool block)
    {
        (void)tag;
        (void)block;
        return false;
    }

    [[nodiscard]] inline bool canBlockPrimaryWeaponNodeOwnership()
    {
        return false;
    }

    [[nodiscard]] inline RE::NiTransform getHandWorldTransform(Hand hand)
    {
        const auto* frikApi = api();
        if (frikApi && frikApi->getHandWorldTransform) {
            return frikApi->getHandWorldTransform(hand);
        }

        RE::NiTransform result{};
        const auto* host = rock::getHostHandAuthority();
        if (host && host->get &&
            host->get(hostPhysicalHandIndex(hand), &result)) {
            return result;
        }
        return RE::NiTransform();
    }
}
