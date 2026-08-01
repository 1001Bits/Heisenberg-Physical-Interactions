#include "LegacyFrikFingerPoseAuthority.h"

#include "F4VROffsets.h"

#include "../external/ROCK/src/ROCKMain.h"
#include "../external/ROCK/src/api/FRIKApi.h"
#include "physics-interaction/performance/PerformanceProfiler.h"

#include "common/MatrixUtils.h"
#include "common/Quaternion.h"
#include "f4vr/BSFlattenedBoneTree.h"
#include "f4vr/F4VRUtils.h"
#include "f4vr/PlayerNodes.h"

#include <RE/Fallout.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <numbers>
#include <type_traits>

#include <spdlog/spdlog.h>

namespace heisenberg
{
    namespace
    {
        using RockFrikApi = frik::api::FRIKApi;
        using RockHandPoseData = RockFrikApi::HandPoseData;
        using RockLocalTransformOverride = RockFrikApi::FingerLocalTransformOverride;
        using BoneTree = f4cf::f4vr::BSFlattenedBoneTree;
        using Quaternion = f4cf::common::Quaternion;
        using MatrixUtils = f4cf::common::MatrixUtils;

        static_assert(std::is_standard_layout_v<RockFrikApi::FingerPoseData>);
        static_assert(std::is_standard_layout_v<RockHandPoseData>);
        static_assert(sizeof(RockFrikApi::FingerPoseData) == sizeof(float) * 4);
        static_assert(sizeof(RockHandPoseData) == sizeof(float) * 22);
        static_assert(
            offsetof(RockLocalTransformOverride, localTransforms) >=
            sizeof(std::uint16_t) * 4);
        static_assert(
            offsetof(RockLocalTransformOverride, localTransforms) %
                alignof(RE::NiTransform) ==
            0);

        constexpr int kRightHand = 0;
        constexpr int kLeftHand = 1;
        constexpr int kFingerCount = 5;
        constexpr int kJointCount = 15;
        constexpr int kMaximumBoneTransforms = 768;
        constexpr std::uint16_t kFullLocalTransformMask = 0x7FFF;
        constexpr std::size_t kMaximumTagLength = 63;
        constexpr std::size_t kMaximumWritersPerHand = 16;

        // These are the unrounded rotations and translations from the checked-in
        // stock FRIK 0.77.12 source:
        // FRIK 77 source/Fallout-4-VR-Body-main/src/skeleton/HandPose.cpp.
        // Keep this table independent of
        // the live 0.77.12 scalar pose: ROCK's mesh-contact solver needs a clean,
        // immutable authored baseline, not a snapshot containing last frame's
        // override.
        //
        // Source SHA-256:
        // CA068F2CF68D6B34F71A8A317EE4316CC7A104C9A6EDF87D3DB82E7639C854F5
        struct BonePoseSource
        {
            const char* name;
            std::array<float, 9> closedRotation;
            std::array<float, 9> openRotation;
            std::array<float, 3> openTranslation;
            std::array<float, 3> powerArmorOpenTranslation;
        };

        const std::array<BonePoseSource, 30> kBonePoseSources{ {
            BonePoseSource{ "LArm_Finger11", {{ 0.849409f, -0.270577f, 0.453092f, -0.382631f, 0.275533f, 0.881859f, -0.363453f, -0.922426f, 0.130509f }}, {{ 0.617716f, -0.400404f, 0.676834f, -0.65398f, 0.216427f, 0.724893f, -0.436735f, -0.890414f, -0.128165f }}, {{ 1.582972f, -1.262648f, 1.853201f }}, {{ 3.993323f, -4.156268f, 3.585619f }} },
            BonePoseSource{ "LArm_Finger12", {{ 0.698533f, -0.713903f, 0.048938f, 0.710545f, 0.700093f, 0.070685f, -0.084723f, -0.014603f, 0.996297f }}, {{ 0.899514f, -std::numbers::log10e_v<float>, -0.048362f, 0.435479f, 0.89999f, 0.019389f, 0.035107f, -0.038501f, 0.998642f }}, {{ 3.569515f, 0.000042f, 0.000004f }}, {{ 2.893830f, 0.000042f, 0.000004f }} },
            BonePoseSource{ "LArm_Finger13", {{ 0.125157f, -0.992116f, -0.006447f, 0.990953f, 0.125323f, -0.048036f, 0.048466f, -0.000376f, 0.998825f }}, {{ 0.945701f, -0.321798f, -0.045777f, 0.321435f, 0.946808f, -0.015267f, 0.048255f, -0.000276f, 0.998835f }}, {{ 2.401824f, 0, 0 }}, {{ 4.687409f, 0, 0 }} },
            BonePoseSource{ "LArm_Finger21", {{ 0.088989f, -0.995196f, 0.04083f, 0.995157f, 0.090554f, 0.038248f, -0.041762f, 0.037228f, 0.998434f }}, {{ 0.990258f, -0.114774f, 0.078839f, 0.111225f, 0.992634f, 0.048027f, -0.08377f, -0.03879f, 0.99573f }}, {{ 7.501364f, 0.430291f, 2.277657f }}, {{ 8.474635f, -2.161191f, 3.789806f }} },
            BonePoseSource{ "LArm_Finger22", {{ -0.473616f, -0.880732f, 0, 0.880732f, -0.473616f, 0, 0, 0, 1 }}, {{ 0.958294f, -0.285783f, 0, 0.285783f, 0.958294f, 0, 0, 0, 1 }}, {{ 3.018186f, 0.000026f, 0.000011f }}, {{ 2.613208f, 0.000026f, 0.000011f }} },
            BonePoseSource{ "LArm_Finger23", {{ -0.123119f, -0.992392f, 0, 0.992392f, -0.123119f, 0, 0, 0, 1 }}, {{ 0.992354f, -0.123425f, 0, 0.123425f, 0.992354f, 0, 0, 0, 1 }}, {{ 1.850236f, 0, 0 }}, {{ 5.145684f, 0, 0 }} },
            BonePoseSource{ "LArm_Finger31", {{ 0.159314f, -0.982871f, 0.09265f, 0.983889f, 0.150362f, -0.096712f, 0.081124f, 0.106565f, 0.990991f }}, {{ 0.951661f, -0.27608f, -0.134618f, 0.266956f, 0.960211f, -0.082032f, 0.151909f, 0.04213f, 0.987496f }}, {{ 7.595781f, 0.62098f, 0.457392f }}, {{ 8.151892f, -2.576661f, 1.100114f }} },
            BonePoseSource{ "LArm_Finger32", {{ -0.45663f, -0.889657f, 0, 0.889657f, -0.45663f, 0, 0, 0, 1 }}, {{ 0.902528f, -0.430632f, -0.000153f, 0.430632f, 0.902527f, 0.000674f, -0.000153f, -0.000674f, 1 }}, {{ 3.091653f, 0.000021f, -0.000004f }}, {{ 3.722714f, 0.000021f, -0.000004f }} },
            BonePoseSource{ "LArm_Finger33", {{ -0.076698f, -0.997054f, 0, 0.997054f, -0.076698f, 0, 0, 0, 1 }}, {{ 0.953147f, -0.302508f, 0.000106f, 0.302508f, 0.953147f, -0.000683f, 0.000106f, 0.000683f, 1 }}, {{ 2.187974f, 0, 0 }}, {{ 4.984375f, 0, 0 }} },
            BonePoseSource{ "LArm_Finger41", {{ 0.123006f, -0.978335f, 0.166524f, 0.978335f, 0.091386f, -0.185766f, 0.166524f, 0.185766f, 0.96838f }}, {{ 0.919043f, -0.392269f, -0.038525f, 0.384414f, 0.913631f, -0.132302f, 0.087095f, 0.106782f, 0.990461f }}, {{ 7.464033f, 0.350152f, -1.438817f }}, {{ 7.967844f, -2.258833f, -1.337387f }} },
            BonePoseSource{ "LArm_Finger42", {{ -0.366717f, -0.930333f, 0, 0.930333f, -0.366717f, 0, 0, 0, 1 }}, {{ 0.927023f, -0.375003f, 0, 0.375003f, 0.927023f, 0, 0, 0, 1 }}, {{ 2.664419f, 0.000027f, 0.000004f }}, {{ 2.933939f, 0.000027f, 0.000004f }} },
            BonePoseSource{ "LArm_Finger43", {{ 0.324171f, -0.945999f, 0, 0.945999f, 0.324171f, 0, 0, 0, 1 }}, {{ 0.984968f, -0.172734f, 0, 0.172734f, 0.984968f, 0, 0, 0, 1 }}, {{ 1.89974f, 0, 0 }}, {{ 5.102559f, 0, 0 }} },
            BonePoseSource{ "LArm_Finger51", {{ 0.204525f, -0.935955f, 0.286631f, 0.952761f, 0.123178f, -0.277623f, 0.224536f, 0.329871f, 0.916934f }}, {{ 0.825976f, -0.557004f, -0.086665f, 0.534941f, 0.822993f, -0.191102f, 0.17777f, 0.111485f, 0.977737f }}, {{ 6.637259f, -0.35742f, -3.01848f }}, {{ 8.365221f, -2.603350f, -3.706458f }} },
            BonePoseSource{ "LArm_Finger52", {{ -0.190355f, -0.981715f, -0.00044f, 0.981715f, -0.190355f, -0.000533f, 0.00044f, -0.000533f, 1 }}, {{ 0.935958f, -0.352111f, 0, 0.352111f, 0.935958f, 0, 0, 0, 1 }}, {{ 2.238261f, 0.000018f, 0.000003f }}, {{ 2.128304f, 0.000018f, 0.000003f }} },
            BonePoseSource{ "LArm_Finger53", {{ -0.188246f, -0.982122f, 0, 0.982122f, -0.188246f, 0, 0, 0, 1 }}, {{ 0.833619f, -0.552339f, 0, 0.552339f, 0.833619f, 0, 0, 0, 1 }}, {{ 1.665912f, 0, 0 }}, {{ 4.594295f, 0, 0 }} },
            BonePoseSource{ "RArm_Finger11", {{ 0.752071f, -0.282712f, -0.595368f, -0.397682f, 0.525706f, -0.751986f, 0.525584f, 0.802314f, 0.282939f }}, {{ 0.584889f, -0.400611f, -0.705277f, -0.656401f, 0.277021f, -0.70171f, 0.47649f, 0.873367f, -0.100935f }}, {{ 1.582972f, -1.262648f, -1.853201f }}, {{ 3.993090f, -4.156340f, -3.585553f }} },
            BonePoseSource{ "RArm_Finger12", {{ 0.556184f, -0.830294f, -0.035639f, 0.826703f, 0.557145f, -0.078435f, 0.084981f, 0.014162f, 0.996282f }}, {{ 0.812239f, -0.583324f, 0, 0.583324f, 0.812239f, 0, 0, 0, 1 }}, {{ 3.569515f, 0.000042f, 0.000004f }}, {{ 2.893783f, 0.000042f, 0.000004f }} },
            BonePoseSource{ "RArm_Finger13", {{ 0.620726f, -0.783447f, 0.030166f, 0.782545f, 0.621458f, 0.037589f, -0.048196f, 0.000274f, 0.998838f }}, {{ 0.970436f, -0.241361f, 0, 0.241361f, 0.970436f, 0, 0, 0, 1 }}, {{ 2.401824f, 0, 0 }}, {{ 4.686954f, 0, 0 }} },
            BonePoseSource{ "RArm_Finger21", {{ 0.38695f, -0.915355f, -0.111332f, 0.917694f, 0.394073f, -0.050434f, 0.090038f, -0.082654f, 0.992503f }}, {{ 0.969328f, -0.20464f, -0.136108f, 0.195507f, 0.977633f, -0.077531f, 0.148929f, 0.048543f, 0.987656f }}, {{ 7.501364f, 0.430291f, -2.277657f }}, {{ 8.474229f, -2.161169f, -3.789712f }} },
            BonePoseSource{ "RArm_Finger22", {{ -0.152033f, -0.988376f, 0, 0.988376f, -0.152033f, 0, 0, 0, 1 }}, {{ 0.949484f, -0.313814f, 0, 0.313814f, 0.949484f, 0, 0, 0, 1 }}, {{ 3.018186f, 0.000026f, 0.000011f }}, {{ 2.613165f, 0.000026f, 0.000011f }} },
            BonePoseSource{ "RArm_Finger23", {{ 0.397566f, -0.917574f, 0, 0.917574f, 0.397566f, 0, 0, 0, 1 }}, {{ 0.980211f, -0.197957f, 0, 0.197957f, 0.980211f, 0, 0, 0, 1 }}, {{ 1.850236f, 0, 0 }}, {{ 5.145271f, 0, 0 }} },
            BonePoseSource{ "RArm_Finger31", {{ 0.076671f, -0.99201f, -0.100188f, 0.996805f, 0.078521f, -0.014653f, 0.022403f, -0.098745f, 0.994861f }}, {{ 0.954206f, -0.298892f, 0.01245f, 0.29779f, 0.953005f, 0.055697f, -0.028512f, -0.049439f, 0.99837f }}, {{ 7.595781f, 0.62098f, -0.457392f }}, {{ 8.151529f, -2.576689f, -1.100008f }} },
            BonePoseSource{ "RArm_Finger32", {{ -0.068391f, -0.997659f, 0, 0.997659f, -0.068391f, 0, 0, 0, 1 }}, {{ 0.903441f, -0.428712f, 0, 0.428712f, 0.903441f, 0, 0, 0, 1 }}, {{ 3.091653f, 0.000021f, -0.000004f }}, {{ 3.722677f, 0.000021f, -0.000004f }} },
            BonePoseSource{ "RArm_Finger33", {{ -0.050058f, -0.998746f, 0, 0.998746f, -0.050058f, 0, 0, 0, 1 }}, {{ 0.967689f, -0.252149f, 0, 0.252149f, 0.967689f, 0, 0, 0, 1 }}, {{ 2.187974f, 0, 0 }}, {{ 4.973974f, 0, 0 }} },
            BonePoseSource{ "RArm_Finger41", {{ 0.068248f, -0.982702f, -0.172158f, 0.997656f, 0.068079f, 0.006893f, 0.004947f, -0.172225f, 0.985045f }}, {{ 0.926338f, -0.376682f, -0.002837f, 0.37216f, 0.914003f, 0.161543f, -0.058257f, -0.1507f, 0.986862f }}, {{ 7.464033f, 0.350152f, 1.438817f }}, {{ 7.967505f, -2.258873f, 1.337498f }} },
            BonePoseSource{ "RArm_Finger42", {{ 0.093539f, -0.995616f, 0, 0.995616f, 0.093539f, 0, 0, 0, 1 }}, {{ 0.914348f, -0.40493f, 0, 0.40493f, 0.914348f, 0, 0, 0, 1 }}, {{ 2.664419f, 0.000027f, 0.000004f }}, {{ 2.933841f, 0.000027f, 0.000004f }} },
            BonePoseSource{ "RArm_Finger43", {{ -0.33522f, -0.94214f, 0, 0.94214f, -0.33522f, 0, 0, 0, 1 }}, {{ 0.919149f, -0.39391f, 0, 0.39391f, 0.919149f, 0, 0, 0, 1 }}, {{ 1.89974f, 0, 0 }}, {{ 5.102017f, 0, 0 }} },
            BonePoseSource{ "RArm_Finger51", {{ 0.257096f, -0.93156f, -0.257096f, 0.955995f, 0.206258f, 0.208641f, -0.141334f, -0.299423f, 0.943595f }}, {{ 0.921646f, -0.376617f, 0.09343f, 0.345979f, 0.906603f, 0.241599f, -0.175694f, -0.190344f, 0.965868f }}, {{ 6.637259f, -0.35742f, 3.01848f }}, {{ 8.364894f, -2.603419f, 3.706582f }} },
            BonePoseSource{ "RArm_Finger52", {{ -0.21201f, -0.977267f, -0.000434f, 0.977267f, -0.21201f, -0.000538f, 0.000434f, -0.000538f, 1 }}, {{ 0.957083f, -0.289814f, 0, 0.289814f, 0.957083f, 0, 0, 0, 1 }}, {{ 2.238261f, 0.000018f, 0.000003f }}, {{ 2.128275f, 0.000018f, 0.000003f }} },
            BonePoseSource{ "RArm_Finger53", {{ -0.276492f, -0.961017f, 0, 0.961017f, -0.276492f, 0, 0, 0, 1 }}, {{ 0.758452f, -0.651728f, 0, 0.651728f, 0.758452f, 0, 0, 0, 1 }}, {{ 1.665912f, 0, 0 }}, {{ 4.593989f, 0, 0 }} },
        } };

        struct CanonicalPose
        {
            std::array<float, kJointCount> flex{};
            std::array<float, kFingerCount> splay{};
            float palmPitch = 0.0f;
            float palmYaw = 0.0f;
        };

        struct Writer
        {
            std::array<char, kMaximumTagLength + 1> tag{};
            CanonicalPose pose{};
            std::array<RE::NiTransform, kJointCount> localTransforms{};
            std::uint16_t localTransformMask = 0;
            int priority = 0;
            std::uint64_t sequence = 0;
            bool active = false;
        };

        struct HandSlot
        {
            std::array<Writer, kMaximumWritersPerHand> writers{};
        };

        struct SkeletonWitness
        {
            BoneTree* tree = nullptr;
            BoneTree::BoneTransforms* transforms = nullptr;
            int transformCount = 0;
            bool inPowerArmor = false;
            bool valid = false;
        };

        std::array<HandSlot, 2> g_handSlots{};
        SkeletonWitness g_skeletonWitness{};
        std::mutex g_registryMutex;
        std::uint64_t g_nextSequence = 0;
        bool g_loggedFirstApply = false;
        bool g_loggedInvalidTree = false;

        bool validPhysicalHand(const int hand)
        {
            return hand == kRightHand || hand == kLeftHand;
        }

        bool getTagLength(const char* const tag, std::size_t& outLength)
        {
            outLength = 0;
            if (!tag) {
                return false;
            }

            bool hasText = false;
            while (outLength <= kMaximumTagLength && tag[outLength] != '\0') {
                hasText = hasText ||
                    std::isspace(static_cast<unsigned char>(tag[outLength])) == 0;
                ++outLength;
            }
            return hasText && outLength > 0 && outLength <= kMaximumTagLength &&
                   tag[outLength] == '\0';
        }

        bool writerTagEquals(const Writer& writer, const char* const tag, const std::size_t tagLength)
        {
            return writer.active &&
                   writer.tag[tagLength] == '\0' &&
                   std::memcmp(writer.tag.data(), tag, tagLength) == 0;
        }

        Writer* findWriter(HandSlot& slot, const char* const tag, const std::size_t tagLength)
        {
            for (auto& writer : slot.writers) {
                if (writerTagEquals(writer, tag, tagLength)) {
                    return &writer;
                }
            }
            return nullptr;
        }

        Writer* findFreeWriter(HandSlot& slot)
        {
            for (auto& writer : slot.writers) {
                if (!writer.active) {
                    return &writer;
                }
            }
            return nullptr;
        }

        const Writer* selectWinner(const HandSlot& slot)
        {
            const Writer* winner = nullptr;
            for (const auto& writer : slot.writers) {
                if (!writer.active) {
                    continue;
                }
                if (!winner ||
                    writer.priority > winner->priority ||
                    (writer.priority == winner->priority &&
                        writer.sequence > winner->sequence)) {
                    winner = &writer;
                }
            }
            return winner;
        }

        void copyTag(Writer& writer, const char* const tag, const std::size_t tagLength)
        {
            writer.tag.fill('\0');
            std::memcpy(writer.tag.data(), tag, tagLength);
        }

        bool decodePose(const void* const opaquePose, CanonicalPose& outPose)
        {
            if (!opaquePose) {
                return false;
            }

            const auto& pose = *static_cast<const RockHandPoseData*>(opaquePose);
            const RockFrikApi::FingerPoseData* const fingers[kFingerCount] = {
                &pose.thumb,
                &pose.index,
                &pose.middle,
                &pose.ring,
                &pose.pinky,
            };

            for (int finger = 0; finger < kFingerCount; ++finger) {
                const auto& source = *fingers[finger];
                const float values[4] = {
                    source.prox,
                    source.mid,
                    source.dist,
                    source.splay,
                };
                for (const float value : values) {
                    if (!std::isfinite(value)) {
                        return false;
                    }
                }
                outPose.flex[finger * 3] = source.prox;
                outPose.flex[finger * 3 + 1] = source.mid;
                outPose.flex[finger * 3 + 2] = source.dist;
                outPose.splay[finger] = source.splay;
            }

            if (!std::isfinite(pose.palmPitch) || !std::isfinite(pose.palmYaw)) {
                return false;
            }
            outPose.palmPitch = pose.palmPitch;
            outPose.palmYaw = pose.palmYaw;
            return true;
        }

        bool isFiniteRotation(const RE::NiMatrix3& rotation)
        {
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if (!std::isfinite(rotation.entry[row][column])) {
                        return false;
                    }
                }
            }
            return true;
        }

        bool isFiniteTransform(const RE::NiTransform& transform)
        {
            return isFiniteRotation(transform.rotate) &&
                   std::isfinite(transform.translate.x) &&
                   std::isfinite(transform.translate.y) &&
                   std::isfinite(transform.translate.z) &&
                   std::isfinite(transform.scale) &&
                   std::abs(transform.scale) > 0.0001f;
        }

        RE::NiMatrix3 matrixFromSource(const std::array<float, 9>& source)
        {
            RE::NiMatrix3 result{};
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    result.entry[row][column] =
                        source[static_cast<std::size_t>(row * 3 + column)];
                }
            }
            return result;
        }

        bool buildPoseLocalTransforms(
            const CanonicalPose& pose,
            const bool isLeft,
            const bool inPowerArmor,
            std::array<RE::NiTransform, kJointCount>& outTransforms)
        {
            const std::size_t sourceOffset = isLeft ? 0 : kJointCount;
            for (int joint = 0; joint < kJointCount; ++joint) {
                const auto& source =
                    kBonePoseSources[sourceOffset + static_cast<std::size_t>(joint)];

                Quaternion closed;
                Quaternion open;
                closed.fromMatrix(matrixFromSource(source.closedRotation));
                open.fromMatrix(matrixFromSource(source.openRotation));
                closed.slerp(std::clamp(pose.flex[joint], -1.0f, 2.0f), open);

                RE::NiTransform local{};
                local.rotate = closed.getMatrix();
                if ((joint % 3) == 0 && pose.splay[joint / 3] != 0.0f) {
                    const float sign = isLeft ? -1.0f : 1.0f;
                    local.rotate =
                        MatrixUtils::getMatrixFromEulerAngles(
                            0.0f,
                            sign * pose.splay[joint / 3],
                            0.0f) *
                        local.rotate;
                }

                const auto& translation = inPowerArmor ?
                    source.powerArmorOpenTranslation :
                    source.openTranslation;
                local.translate = RE::NiPoint3{
                    translation[0],
                    translation[1],
                    translation[2],
                };
                local.scale = 1.0f;
                if (!isFiniteTransform(local)) {
                    outTransforms = {};
                    return false;
                }
                outTransforms[static_cast<std::size_t>(joint)] = local;
            }
            return true;
        }

        bool decodeLocalTransforms(
            const void* const opaqueTransforms,
            std::uint16_t& outMask,
            std::array<RE::NiTransform, kJointCount>& outTransforms)
        {
            if (!opaqueTransforms) {
                return false;
            }

            const auto& source =
                *static_cast<const RockLocalTransformOverride*>(opaqueTransforms);
            outMask = static_cast<std::uint16_t>(
                source.enabledMask & kFullLocalTransformMask);
            outTransforms = {};
            for (int joint = 0; joint < kJointCount; ++joint) {
                const std::uint16_t bit =
                    static_cast<std::uint16_t>(1u << joint);
                if ((outMask & bit) == 0) {
                    continue;
                }
                if (!isFiniteTransform(source.localTransforms[joint])) {
                    outMask = 0;
                    outTransforms = {};
                    return false;
                }
                outTransforms[static_cast<std::size_t>(joint)] =
                    source.localTransforms[joint];
            }
            return true;
        }

        bool validBoneTree(const BoneTree* const tree)
        {
            return tree &&
                   tree->transforms &&
                   tree->numTransforms > 0 &&
                   tree->numTransforms <= kMaximumBoneTransforms;
        }

        struct ResolvedHandBones
        {
            std::array<int, kJointCount> fingerIndices{};
            int handRootIndex = -1;
        };

        struct ResolvedHandBoneCache
        {
            BoneTree* tree = nullptr;
            BoneTree::BoneTransforms* transforms = nullptr;
            int transformCount = 0;
            bool inPowerArmor = false;
            std::array<ResolvedHandBones, 2> hands{};
            std::array<bool, 2> handValid{};
        };

        // Protected by g_registryMutex. The flattened tree keeps stable indices
        // for its lifetime, so resolving all 16 names per active hand every
        // frame only repeats work. Keep the exact tree allocation witness and
        // still validate the 16 cached names before every use; an in-place
        // reorder therefore falls back to a fresh full scan instead of writing
        // a similarly-sized but unrelated bone.
        ResolvedHandBoneCache g_resolvedHandBoneCache{};

        void invalidateResolvedHandBoneCache()
        {
            g_resolvedHandBoneCache = {};
        }

        bool validateResolvedHandBones(
            const BoneTree* const tree,
            const bool isLeft,
            const ResolvedHandBones& resolved)
        {
            if (!validBoneTree(tree)) {
                return false;
            }

            const std::size_t sourceOffset = isLeft ? 0 : kJointCount;
            const char* const handRootName =
                isLeft ? "LArm_Hand" : "RArm_Hand";
            if (resolved.handRootIndex < 0 ||
                resolved.handRootIndex >= tree->numTransforms) {
                return false;
            }
            const char* const resolvedHandRootName =
                tree->transforms[resolved.handRootIndex].name.c_str();
            if (!resolvedHandRootName ||
                std::strcmp(resolvedHandRootName, handRootName) != 0) {
                return false;
            }

            for (int joint = 0; joint < kJointCount; ++joint) {
                const int treeIndex = resolved.fingerIndices[joint];
                if (treeIndex < 0 || treeIndex >= tree->numTransforms) {
                    return false;
                }
                const char* const resolvedName =
                    tree->transforms[treeIndex].name.c_str();
                if (!resolvedName ||
                    std::strcmp(
                        resolvedName,
                        kBonePoseSources[
                            sourceOffset +
                            static_cast<std::size_t>(joint)]
                            .name) != 0) {
                    return false;
                }

                const int parentIndex =
                    tree->transforms[treeIndex].parPos;
                if (parentIndex < 0 ||
                    parentIndex >= tree->numTransforms ||
                    parentIndex == treeIndex) {
                    return false;
                }
                for (int earlier = 0; earlier < joint; ++earlier) {
                    if (resolved.fingerIndices[earlier] == treeIndex) {
                        return false;
                    }
                }
            }
            return true;
        }

        bool scanHandBones(
            BoneTree* const tree,
            const bool isLeft,
            ResolvedHandBones& out)
        {
            out.fingerIndices.fill(-1);
            out.handRootIndex = -1;
            if (!validBoneTree(tree)) {
                return false;
            }

            const std::size_t sourceOffset = isLeft ? 0 : kJointCount;
            const char* const handRootName =
                isLeft ? "LArm_Hand" : "RArm_Hand";

            for (int treeIndex = 0; treeIndex < tree->numTransforms; ++treeIndex) {
                const char* const name =
                    tree->transforms[treeIndex].name.c_str();
                if (!name || name[0] == '\0') {
                    continue;
                }
                if (std::strcmp(name, handRootName) == 0) {
                    if (out.handRootIndex >= 0) {
                        return false;
                    }
                    out.handRootIndex = treeIndex;
                }
                for (int joint = 0; joint < kJointCount; ++joint) {
                    if (std::strcmp(
                            name,
                            kBonePoseSources[
                                sourceOffset +
                                static_cast<std::size_t>(joint)]
                                .name) != 0) {
                        continue;
                    }
                    if (out.fingerIndices[joint] >= 0) {
                        return false;
                    }
                    out.fingerIndices[joint] = treeIndex;
                    break;
                }
            }

            if (out.handRootIndex < 0) {
                return false;
            }
            for (int joint = 0; joint < kJointCount; ++joint) {
                const int treeIndex = out.fingerIndices[joint];
                if (treeIndex < 0 || treeIndex >= tree->numTransforms) {
                    return false;
                }
                const int parentIndex = tree->transforms[treeIndex].parPos;
                if (parentIndex < 0 || parentIndex >= tree->numTransforms ||
                    parentIndex == treeIndex) {
                    return false;
                }
                for (int earlier = 0; earlier < joint; ++earlier) {
                    if (out.fingerIndices[earlier] == treeIndex) {
                        return false;
                    }
                }
            }
            return true;
        }

        // Caller holds g_registryMutex.
        bool resolveHandBonesCached(
            BoneTree* const tree,
            const bool isLeft,
            const bool inPowerArmor,
            ResolvedHandBones& out)
        {
            if (!validBoneTree(tree)) {
                invalidateResolvedHandBoneCache();
                return false;
            }

            if (g_resolvedHandBoneCache.tree != tree ||
                g_resolvedHandBoneCache.transforms != tree->transforms ||
                g_resolvedHandBoneCache.transformCount !=
                    tree->numTransforms ||
                g_resolvedHandBoneCache.inPowerArmor != inPowerArmor) {
                invalidateResolvedHandBoneCache();
                g_resolvedHandBoneCache.tree = tree;
                g_resolvedHandBoneCache.transforms = tree->transforms;
                g_resolvedHandBoneCache.transformCount =
                    tree->numTransforms;
                g_resolvedHandBoneCache.inPowerArmor = inPowerArmor;
            }

            const std::size_t handIndex =
                isLeft ?
                static_cast<std::size_t>(kLeftHand) :
                static_cast<std::size_t>(kRightHand);
            if (g_resolvedHandBoneCache.handValid[handIndex] &&
                validateResolvedHandBones(
                    tree,
                    isLeft,
                    g_resolvedHandBoneCache.hands[handIndex])) {
                out = g_resolvedHandBoneCache.hands[handIndex];
                return true;
            }

            g_resolvedHandBoneCache.handValid[handIndex] = false;
            ResolvedHandBones resolved;
            if (!scanHandBones(tree, isLeft, resolved)) {
                return false;
            }

            g_resolvedHandBoneCache.hands[handIndex] = resolved;
            g_resolvedHandBoneCache.handValid[handIndex] = true;
            out = resolved;
            return true;
        }

        int fingerSlotForTreeIndex(
            const int* const fingerIndices,
            const int treeIndex)
        {
            for (int slot = 0; slot < kJointCount; ++slot) {
                if (fingerIndices[slot] == treeIndex) {
                    return slot;
                }
            }
            return -1;
        }

        bool refreshFingerWorldRecursive(
            BoneTree* const tree,
            const int* const fingerIndices,
            const int slot,
            std::uint8_t* const visitState,
            const int depth)
        {
            if (slot < 0 || slot >= kJointCount || depth > kJointCount) {
                return false;
            }
            if (visitState[slot] == 2) {
                return true;
            }
            if (visitState[slot] == 1) {
                return false;
            }
            visitState[slot] = 1;

            const int treeIndex = fingerIndices[slot];
            if (treeIndex < 0 || treeIndex >= tree->numTransforms) {
                return false;
            }
            auto& transform = tree->transforms[treeIndex];
            const int parentIndex = transform.parPos;
            if (parentIndex < 0 || parentIndex >= tree->numTransforms) {
                return false;
            }

            const int parentSlot =
                fingerSlotForTreeIndex(fingerIndices, parentIndex);
            if (parentSlot >= 0 &&
                !refreshFingerWorldRecursive(
                    tree,
                    fingerIndices,
                    parentSlot,
                    visitState,
                    depth + 1)) {
                return false;
            }

            const RE::NiTransform& parentWorld =
                transform.refNode && transform.refNode->parent ?
                    transform.refNode->parent->world :
                    tree->transforms[parentIndex].world;
            if (!isFiniteTransform(parentWorld) ||
                !isFiniteTransform(transform.local)) {
                return false;
            }

            RE::NiPoint3 translated = transform.local.translate;
            translated =
                parentWorld.rotate.Transpose() *
                (translated * parentWorld.scale);
            transform.world.translate =
                parentWorld.translate + translated;
            transform.world.rotate =
                transform.local.rotate * parentWorld.rotate;
            transform.world.scale =
                transform.local.scale * parentWorld.scale;
            if (!isFiniteTransform(transform.world)) {
                return false;
            }

            if (transform.refNode) {
                transform.refNode->local = transform.local;
                transform.refNode->world = transform.world;
            }
            visitState[slot] = 2;
            return true;
        }

        // Isolate raw scene-graph writes in an SEH leaf. No pointer from this
        // function is retained: the live tree identity, transform allocation,
        // count, and bone-name mapping are resolved again on every frame.
        bool writeFingerLocalsSeh(
            BoneTree* const tree,
            BoneTree::BoneTransforms* const expectedTransforms,
            const int expectedTransformCount,
            const ResolvedHandBones& resolved,
            const std::array<RE::NiTransform, kJointCount>& locals,
            const RE::NiMatrix3& palmOffset,
            const bool applyPalmOffset)
        {
            __try {
                if (!tree ||
                    tree->transforms != expectedTransforms ||
                    tree->numTransforms != expectedTransformCount ||
                    tree->numTransforms <= 0 ||
                    tree->numTransforms > kMaximumBoneTransforms) {
                    return false;
                }

                auto& handRoot =
                    tree->transforms[resolved.handRootIndex];
                if (handRoot.refNode) {
                    // HandAuthority may have performed a same-frame wrist write
                    // after FRIK. Pull that final basis into the flat tree before
                    // deriving proximal finger worlds.
                    handRoot.local = handRoot.refNode->local;
                    handRoot.world = handRoot.refNode->world;
                }

                if (applyPalmOffset) {
                    handRoot.local.rotate =
                        handRoot.local.rotate * palmOffset;
                    const int parentIndex = handRoot.parPos;
                    if (parentIndex < 0 ||
                        parentIndex >= tree->numTransforms) {
                        return false;
                    }
                    const RE::NiTransform& parentWorld =
                        handRoot.refNode && handRoot.refNode->parent ?
                            handRoot.refNode->parent->world :
                            tree->transforms[parentIndex].world;
                    if (!isFiniteTransform(parentWorld) ||
                        !isFiniteTransform(handRoot.local)) {
                        return false;
                    }

                    RE::NiPoint3 translated =
                        handRoot.local.translate;
                    translated =
                        parentWorld.rotate.Transpose() *
                        (translated * parentWorld.scale);
                    handRoot.world.translate =
                        parentWorld.translate + translated;
                    handRoot.world.rotate =
                        handRoot.local.rotate * parentWorld.rotate;
                    handRoot.world.scale =
                        handRoot.local.scale * parentWorld.scale;
                    if (!isFiniteTransform(handRoot.world)) {
                        return false;
                    }
                    if (handRoot.refNode) {
                        handRoot.refNode->local = handRoot.local;
                        handRoot.refNode->world = handRoot.world;
                    }
                }

                for (int joint = 0; joint < kJointCount; ++joint) {
                    const int treeIndex =
                        resolved.fingerIndices[joint];
                    if (treeIndex < 0 ||
                        treeIndex >= tree->numTransforms ||
                        !isFiniteTransform(locals[joint])) {
                        return false;
                    }
                    tree->transforms[treeIndex].local =
                        locals[joint];
                    if (tree->transforms[treeIndex].refNode) {
                        tree->transforms[treeIndex].refNode->local =
                            locals[joint];
                    }
                }

                std::uint8_t visitState[kJointCount] = {};
                for (int joint = 0; joint < kJointCount; ++joint) {
                    if (!refreshFingerWorldRecursive(
                            tree,
                            resolved.fingerIndices.data(),
                            joint,
                            visitState,
                            0)) {
                        return false;
                    }
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool finalizeSkinningSeh(
            BoneTree* const tree,
            RE::NiNode* const worldRoot)
        {
            __try {
                BSFlattenedBoneTree_UpdateBoneArray(tree);
                if (worldRoot) {
                    BSFadeNode_UpdateGeomArray(worldRoot, 1);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool applyWinnerToTree(
            BoneTree* const tree,
            const Writer& winner,
            const ResolvedHandBones& resolved,
            const bool isLeft,
            const bool inPowerArmor)
        {
            std::array<RE::NiTransform, kJointCount> locals{};
            if (winner.localTransformMask ==
                kFullLocalTransformMask) {
                // ROCK's mesh-contact solver normally publishes all 15 exact
                // joint locals. Building the canonical curl/splay pose first
                // performs 15 quaternion interpolations only to overwrite
                // every result immediately.
                locals = winner.localTransforms;
            } else {
                if (!buildPoseLocalTransforms(
                        winner.pose,
                        isLeft,
                        inPowerArmor,
                        locals)) {
                    return false;
                }
                for (int joint = 0; joint < kJointCount; ++joint) {
                    const std::uint16_t bit =
                        static_cast<std::uint16_t>(1u << joint);
                    if ((winner.localTransformMask & bit) != 0) {
                        locals[joint] =
                            winner.localTransforms[joint];
                    }
                }
            }
            for (int joint = 0; joint < kJointCount; ++joint) {
                if (!isFiniteTransform(locals[joint])) {
                    return false;
                }
            }

            const bool applyPalmOffset =
                winner.pose.palmPitch != 0.0f ||
                winner.pose.palmYaw != 0.0f;
            const float deviationSign = isLeft ? -1.0f : 1.0f;
            const RE::NiMatrix3 palmOffset =
                MatrixUtils::getMatrixFromEulerAngles(
                    0.0f,
                    MatrixUtils::degreesToRads(
                        deviationSign * winner.pose.palmYaw),
                    MatrixUtils::degreesToRads(
                        winner.pose.palmPitch));
            if (!isFiniteRotation(palmOffset)) {
                return false;
            }

            return writeFingerLocalsSeh(
                tree,
                tree->transforms,
                tree->numTransforms,
                resolved,
                locals,
                palmOffset,
                applyPalmOffset);
        }

        bool hostApplyPose(
            const char* const tag,
            const int hand,
            const void* const pose,
            const int priority)
        {
            return LegacyFrikFingerPoseAuthority::ApplyPose(
                tag,
                hand,
                pose,
                priority);
        }

        bool hostBuildPoseLocalTransforms(
            const int hand,
            const void* const pose,
            void* const outLocalTransforms)
        {
            return LegacyFrikFingerPoseAuthority::
                BuildPoseLocalTransforms(
                    hand,
                    pose,
                    outLocalTransforms);
        }

        bool hostApplyLocalTransforms(
            const char* const tag,
            const int hand,
            const void* const localTransforms,
            const int priority)
        {
            return LegacyFrikFingerPoseAuthority::
                ApplyLocalTransforms(
                    tag,
                    hand,
                    localTransforms,
                    priority);
        }

        bool hostClear(const char* const tag, const int hand)
        {
            return LegacyFrikFingerPoseAuthority::Clear(
                tag,
                hand);
        }

        bool hostIsActive(const char* const tag, const int hand)
        {
            return LegacyFrikFingerPoseAuthority::IsActive(
                tag,
                hand);
        }

        const rock::HostFingerPoseAuthority g_hostTable{
            &hostApplyPose,
            &hostBuildPoseLocalTransforms,
            &hostApplyLocalTransforms,
            &hostClear,
            &hostIsActive,
        };
    }

    bool LegacyFrikFingerPoseAuthority::ApplyPose(
        const char* const tag,
        const int hand,
        const void* const pose,
        const int priority)
    {
        std::size_t tagLength = 0;
        CanonicalPose decodedPose;
        if (!validPhysicalHand(hand) ||
            priority < 0 ||
            !getTagLength(tag, tagLength) ||
            !decodePose(pose, decodedPose)) {
            return false;
        }

        std::scoped_lock lock(g_registryMutex);
        auto& slot = g_handSlots[static_cast<std::size_t>(hand)];
        Writer* writer = findWriter(slot, tag, tagLength);
        if (!writer) {
            writer = findFreeWriter(slot);
        }
        if (!writer) {
            return false;
        }

        *writer = Writer{};
        copyTag(*writer, tag, tagLength);
        writer->pose = decodedPose;
        writer->priority = priority;
        writer->sequence = ++g_nextSequence;
        writer->active = true;
        // Deliberately left at zero: publishing a new canonical pose clears the
        // old local-transform payload for this tag, matching native FRIK v5.
        writer->localTransformMask = 0;
        return true;
    }

    bool LegacyFrikFingerPoseAuthority::BuildPoseLocalTransforms(
        const int hand,
        const void* const pose,
        void* const outLocalTransforms)
    {
        if (!outLocalTransforms) {
            return false;
        }

        auto& output =
            *static_cast<RockLocalTransformOverride*>(
                outLocalTransforms);
        output = RockLocalTransformOverride{};

        CanonicalPose decodedPose;
        if (!validPhysicalHand(hand) ||
            !decodePose(pose, decodedPose)) {
            return false;
        }

        std::array<RE::NiTransform, kJointCount> transforms{};
        const bool isLeft = hand == kLeftHand;
        if (!buildPoseLocalTransforms(
                decodedPose,
                isLeft,
                f4cf::f4vr::isInPowerArmor(),
                transforms)) {
            return false;
        }

        output.enabledMask = kFullLocalTransformMask;
        for (int joint = 0; joint < kJointCount; ++joint) {
            output.localTransforms[joint] = transforms[joint];
        }
        return true;
    }

    bool LegacyFrikFingerPoseAuthority::ApplyLocalTransforms(
        const char* const tag,
        const int hand,
        const void* const localTransforms,
        const int priority)
    {
        std::size_t tagLength = 0;
        std::uint16_t decodedMask = 0;
        std::array<RE::NiTransform, kJointCount>
            decodedTransforms{};
        if (!validPhysicalHand(hand) ||
            priority < 0 ||
            !getTagLength(tag, tagLength) ||
            !decodeLocalTransforms(
                localTransforms,
                decodedMask,
                decodedTransforms)) {
            return false;
        }

        std::scoped_lock lock(g_registryMutex);
        auto& slot = g_handSlots[static_cast<std::size_t>(hand)];
        Writer* const writer =
            findWriter(slot, tag, tagLength);
        if (!writer) {
            // Native v5 rejects transform-only tags. The scalar FRIK v3 call
            // must establish the corresponding rich pose first.
            return false;
        }

        writer->localTransformMask = decodedMask;
        writer->localTransforms = decodedTransforms;
        writer->priority = priority;
        writer->sequence = ++g_nextSequence;
        return true;
    }

    bool LegacyFrikFingerPoseAuthority::Clear(
        const char* const tag,
        const int hand)
    {
        std::size_t tagLength = 0;
        if (!validPhysicalHand(hand) ||
            !getTagLength(tag, tagLength)) {
            return false;
        }

        std::scoped_lock lock(g_registryMutex);
        auto& slot = g_handSlots[static_cast<std::size_t>(hand)];
        if (Writer* const writer =
                findWriter(slot, tag, tagLength)) {
            *writer = Writer{};
        }
        invalidateResolvedHandBoneCache();
        // A valid no-op clear succeeds, matching the native tagged API.
        return true;
    }

    bool LegacyFrikFingerPoseAuthority::IsActive(
        const char* const tag,
        const int hand)
    {
        std::size_t tagLength = 0;
        if (!validPhysicalHand(hand) ||
            !getTagLength(tag, tagLength)) {
            return false;
        }

        std::scoped_lock lock(g_registryMutex);
        const auto& slot =
            g_handSlots[static_cast<std::size_t>(hand)];
        const Writer* const winner = selectWinner(slot);
        return winner &&
               writerTagEquals(*winner, tag, tagLength);
    }

    void LegacyFrikFingerPoseAuthority::ApplyWinners()
    {
        rock::performance_profiler::ScopedTimer profilerTimer(
            rock::performance_profiler::Scope::LegacyFingerPoseApply);

        BoneTree* const tree =
            f4cf::f4vr::getFlattenedBoneTree();
        if (!validBoneTree(tree)) {
            bool invalidated = false;
            {
                std::scoped_lock lock(g_registryMutex);
                invalidateResolvedHandBoneCache();
                if (g_skeletonWitness.valid) {
                    g_handSlots = {};
                    g_nextSequence = 0;
                    g_skeletonWitness = {};
                    invalidated = true;
                }
            }
            if (invalidated) {
                spdlog::info(
                    "[FRIK-FINGERS] Cleared full-pose publications "
                    "because the flattened skeleton became invalid");
            }
            if (!g_loggedInvalidTree) {
                spdlog::warn(
                    "[FRIK-FINGERS] Legacy full-pose authority "
                    "waiting for a valid flattened bone tree");
                g_loggedInvalidTree = true;
            }
            return;
        }
        g_loggedInvalidTree = false;

        BoneTree::BoneTransforms* const expectedTransforms =
            tree->transforms;
        const int expectedTransformCount = tree->numTransforms;
        const bool inPowerArmor =
            f4cf::f4vr::isInPowerArmor();
        std::array<Writer, 2> winners{};
        std::array<bool, 2> hasWinner{};
        std::array<ResolvedHandBones, 2> resolvedHands{};
        std::array<bool, 2> hasResolvedHand{};
        bool identityChanged = false;
        {
            std::scoped_lock lock(g_registryMutex);
            identityChanged =
                g_skeletonWitness.valid &&
                (g_skeletonWitness.tree != tree ||
                    g_skeletonWitness.transforms != tree->transforms ||
                    g_skeletonWitness.transformCount != tree->numTransforms ||
                    g_skeletonWitness.inPowerArmor != inPowerArmor);

            g_skeletonWitness = SkeletonWitness{
                .tree = tree,
                .transforms = tree->transforms,
                .transformCount = tree->numTransforms,
                .inPowerArmor = inPowerArmor,
                .valid = true,
            };

            if (identityChanged) {
                // Exact local payloads contain normal/PA authored translations.
                // Never carry them across a skeleton allocation or armor mode.
                // Clearing also makes host isActive() false, which causes the
                // ROCK duplicate cache to rebuild and republish next frame.
                g_handSlots = {};
                invalidateResolvedHandBoneCache();
                g_nextSequence = 0;
            } else {
                for (int hand = kRightHand; hand <= kLeftHand; ++hand) {
                    if (const Writer* const winner =
                            selectWinner(g_handSlots[hand])) {
                        winners[hand] = *winner;
                        hasWinner[hand] = true;
                        hasResolvedHand[hand] =
                            resolveHandBonesCached(
                                tree,
                                hand == kLeftHand,
                                inPowerArmor,
                                resolvedHands[hand]);
                    }
                }
            }
        }

        if (identityChanged) {
            spdlog::info(
                "[FRIK-FINGERS] Cleared full-pose publications "
                "after skeleton or power-armor identity changed");
            return;
        }

        if (!hasWinner[kRightHand] &&
            !hasWinner[kLeftHand]) {
            return;
        }

        bool appliedAny = false;
        for (int hand = kRightHand; hand <= kLeftHand; ++hand) {
            if (!hasWinner[hand] || !hasResolvedHand[hand]) {
                continue;
            }
            const bool applied = applyWinnerToTree(
                tree,
                winners[hand],
                resolvedHands[hand],
                hand == kLeftHand,
                inPowerArmor);
            if (!applied) {
                // A same-address in-place skeleton mutation can race the cheap
                // validation above. Do not reuse this hand's mapping after any
                // failed scene write; the next active frame performs a full
                // name scan again.
                std::scoped_lock lock(g_registryMutex);
                if (g_resolvedHandBoneCache.tree == tree &&
                    g_resolvedHandBoneCache.transforms ==
                        expectedTransforms &&
                    g_resolvedHandBoneCache.transformCount ==
                        expectedTransformCount &&
                    g_resolvedHandBoneCache.inPowerArmor ==
                        inPowerArmor) {
                    g_resolvedHandBoneCache.handValid[
                        static_cast<std::size_t>(hand)] = false;
                }
            }
            appliedAny = appliedAny || applied;
        }

        if (!appliedAny) {
            return;
        }

        finalizeSkinningSeh(
            tree,
            f4cf::f4vr::getWorldRootNode());
        if (!g_loggedFirstApply) {
            spdlog::info(
                "[FRIK-FINGERS] FRIK 0.77.12 full-pose host "
                "authority ACTIVE (15 joints, splay, local "
                "surface corrections, normal/PA baselines)");
            g_loggedFirstApply = true;
        }
    }

    void LegacyFrikFingerPoseAuthority::Reset()
    {
        std::scoped_lock lock(g_registryMutex);
        g_handSlots = {};
        g_skeletonWitness = {};
        invalidateResolvedHandBoneCache();
        g_nextSequence = 0;
        g_loggedFirstApply = false;
        g_loggedInvalidTree = false;
    }

    const rock::HostFingerPoseAuthority&
        LegacyFrikFingerPoseAuthority::HostTable()
    {
        return g_hostTable;
    }
}
