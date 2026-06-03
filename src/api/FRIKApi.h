#pragma once

#include <cstdint>
#include <Windows.h>

#include "RE/NetImmerse/NiPoint.h"
#include "RE/NetImmerse/NiTransform.h"

// ----------------------------------------------------------------------------------------
// EXAMPLE USAGE:
// Copy this whole file into your project AS IS
// Use the code below as a reference of FRIK API use
// Call initialize in GameLoaded event

// {
//     const int err = frik::api::FRIKApi::initialize();
//     if (err != 0) {
//         logger::error("FRIK API init failed with error: {}!", err);
//     }
//     logger::info("FRIK (v{}) API (v{}) init successful!", frik::api::FRIKApi::inst->getModVersion(), frik::api::FRIKApi::inst->getVersion());
//
//     // later...
//     if (!frik::api::FRIKApi::inst->isSkeletonReady())
//         return;
//
//     RE::NiPoint3 tip = frik::api::FRIKApi::inst->getIndexFingerTipPosition(frik::api::FRIKApi::Hand::Left);
//
//     // Override left hand pose
//     frik::api::FRIKApi::inst->setHandPose("MyMod_Interaction", frik::api::FRIKApi::Hand::Primary, frik::api::FRIKApi::HandPoseKind::Pointing);
//
//     // Later:
//     frik::api::FRIKApi::inst->clearHandPose("MyMod_Interaction", frik::api::FRIKApi::Hand::Primary);
// }

namespace frik::api
{
#if defined(FRIK_API_EXPORTS)
#   define FRIK_API extern "C" __declspec(dllexport)
#else
#   define FRIK_API extern "C" __declspec(dllimport)
#endif

#define FRIK_CALL __cdecl

    // API version for compatibility checking
    inline constexpr std::uint32_t FRIK_API_VERSION = 5;

    struct FRIKApi
    {
        /**
         * The name of FRIK mod as registered in F4SE used to be able to send/receive messages from to FRIK.
         * Example:
         * _messaging->RegisterListener(onFRIKMessage, frik::api::FRIKApi::FRIK_F4SE_MOD_NAME);
         */
        static constexpr auto FRIK_F4SE_MOD_NAME = "F4VRBody";

        /**
         * The player hand to act on with support of left-handed if needed.
         */
        enum class Hand : std::uint8_t
        {
            Primary,
            Offhand,
            Right,
            Left,
        };

        /**
         * Predefined hand poses.
         * Values 0..7 match the upstream FRIK API v4 contract. hFRIK-specific
         * values are appended so upstream callers do not get remapped poses.
         */
        enum class HandPoseKind : std::uint8_t
        {
            // no specific pose is set
            Unset = 0,
            // pose set with custom finger positions
            Custom = 1,
            Open = 2,
            Pointing = 3,
            HoldingWeapon = 4,
            OffhandGrip = 5,
            Attaboy = 6,
            ThumbsUp = 7,

            Fist = 8,
            HoldingGun = 9,
            HoldingMelee = 10,
        };

        // Kept as a low-churn compatibility alias for current ROCK/hFRIK source.
        using HandPoses = HandPoseKind;

        /**
         * Full pose values for a single finger.
         */
        struct FingerPoseData
        {
            float prox = 0.0f;
            float mid = 0.0f;
            float dist = 0.0f;
            float splay = 0.0f;
        };

        /**
         * Full pose data for one hand, including per-joint finger values and palm motion.
         */
        struct HandPoseData
        {
            FingerPoseData thumb;
            FingerPoseData index;
            FingerPoseData middle;
            FingerPoseData ring;
            FingerPoseData pinky;
            float palmPitch = 0.0f;
            float palmYaw = 0.0f;
        };

        /**
         * The potential state of a set hand pose for specific tag as returned from FRIK.
         */
        enum class HandPoseTagState : std::uint8_t
        {
            // the tag is not set at all
            None,
            // the tag is set and actively used to override the hand pose
            Active,
            // the tag is set but currently overridden by another tag
            Overriden,
        };

        /**
         * Data needed to register a button to open external mod config from FRIK main config UI.
         */
        struct OpenExternalModConfigData
        {
            const char* buttonIconNifPath;
            const char* callbackReceiverName;
            std::uint32_t callbackMessageType;
        };

        /**
         * Optional per-finger-bone local transform overrides.
         * Bit i in enabledMask controls localTransforms[i], using the same
         * 15-bone order as setHandPoseCustomJointPositionsWithPriority:
         * thumb 11/12/13, index 21/22/23, middle 31/32/33, ring 41/42/43, pinky 51/52/53.
         */
        struct FingerLocalTransformOverride
        {
            std::uint16_t enabledMask = 0;
            std::uint16_t reserved[3] = {};
            RE::NiTransform localTransforms[15] = {};
        };

        // -----------------------------------------------------------------
        // FRIK lifecycle event message types.
        // Dispatched via F4SE messaging on channel "F4VRBody".
        // Listen with: _messaging->RegisterListener(callback, FRIKApi::FRIK_F4SE_MOD_NAME);
        // -----------------------------------------------------------------
        enum class LifecycleEvent : std::uint32_t
        {
            /// Fired AFTER skeleton is fully initialized.
            /// Consumers that own physics/world state must still gate creation on their
            /// own frame, menu, and world-readiness checks.
            /// Data: nullptr. Dispatched synchronously from FRIK's initSkeleton().
            kSkeletonReady = 100,

            /// Fired BEFORE skeleton teardown begins (must destroy physics bodies NOW).
            /// Data: nullptr. Dispatched synchronously from FRIK's releaseSkeleton().
            /// The skeleton is still valid when this fires — it will be deleted after
            /// all listeners return.
            kSkeletonDestroying = 101,

            /// Fired when power armor state changes (enter or exit).
            /// Data: bool* pointing to the new isInPowerArmor state.
            /// NOTE: This always fires AFTER kSkeletonDestroying + kSkeletonReady pair,
            /// since PA transitions trigger skeleton recreation. Provided for state queries
            /// by mods that don't track skeleton lifecycle but need PA awareness.
            kPowerArmorChanged = 102,
        };

        /**
         * Get the API version number.
         * Use this to check compatibility before calling other functions.
         */
        std::uint32_t (FRIK_CALL*getVersion)();

        /**
         * Get the mod version string. i.e. "0.12.5"
         */
        const char* (FRIK_CALL*getModVersion)();

        /**
         * Check if FRIK is ready and the skeleton is initialized.
         */
        bool (FRIK_CALL*isSkeletonReady)();

        /**
         * Is any of the FRIK config UI is open (main, Pipboy, weapon adjustment)
         */
        bool (FRIK_CALL*isConfigOpen)();

        /**
         * Is FRIK selfie mode is currently on or off.
         */
        bool (FRIK_CALL*isSelfieModeOn)();

        /**
         * Set FRIK selfie mode on or off.
         */
        void (FRIK_CALL*setSelfieModeOn)(bool setOn);

        /**
         * Is the player currently holding the weapon with two hands. i.e. offhand is holding the weapon.
         */
        bool (FRIK_CALL*isOffHandGrippingWeapon)();

        /**
         * Is the player currently have the FRIK Pipboy open.
         */
        bool (FRIK_CALL*isWristPipboyOpen)();

        /**
         * Get the world position of the index fingertip .
         */
        RE::NiPoint3 (FRIK_CALL*getIndexFingerTipPosition)(Hand hand);

        /**
         * Get the current state of given hand pose tag to identify if it is active in FRIK.
         * Can be used to identify if another system overriding the hand pose and your mod should react accordingly.
         */
        HandPoseTagState (FRIK_CALL*getHandPoseSetTagState)(const char* tag, Hand hand);

        /**
         * Get the current hand pose as active in FRIK.
         */
        HandPoseKind (FRIK_CALL*getCurrentHandPose)(Hand hand);

        /**
         * Set a hand pose override to specific values for each finger.
         * Use the tag to unique identify different systems using hand pose overrides.
         * Each value is between 0 and 1 where 0 is bent and 1 is straight.
         * @return true if successful.
         */
        bool (FRIK_CALL*setHandPose)(const char* tag, Hand hand, HandPoseKind handPose);

        /**
         * Set a hand pose override to specific values for each finger.
         * Use the tag to unique identify different systems using hand pose overrides.
         * Each value is between 0 and 1 where 0 is bent and 1 is straight.
         * @return true if successful.
         */
        bool (FRIK_CALL*setHandPoseCustomFingerPositions)(const char* tag, Hand hand, float thumb, float index, float middle, float ring, float pinky);

        /**
         * Clear the set values in "setHandPoseFingerPositions" for FRIK to have control over the hand pose.
         * Only clears the specific tag hand pose override.
         * @return true if successful.
         */
        bool (FRIK_CALL*clearHandPose)(const char* tag, Hand hand);

        /**
         * @deprecated Use setHandPoseCustomFingerPositions instead.
         */
        void (FRIK_CALL*setHandPoseFingerPositions)(Hand hand, float thumb, float index, float middle, float ring, float pinky);

        /**
         * @deprecated Use clearHandPose instead.
         */
        void (FRIK_CALL*clearHandPoseFingerPositions)(Hand hand);

        /**
         * Adds a button to open external mod config via a button in FRIK main config UI.
         */
        bool (FRIK_CALL*registerOpenModSettingButtonToMainConfig)(const OpenExternalModConfigData& data);

        /**
         * Enable/disable FRIK offhand weapon gripping for a specific tag.
         * The tag must be unique per external system using this API.
         * FRIK keeps only the blocked state, so gripping remains disabled while any tag is blocking it.
         * @return true if successful.
         */
        bool (FRIK_CALL*blockOffHandWeaponGripping)(const char* tag, bool block);

        /**
         * Set a full hand pose override with per-joint finger values, per-finger splay, and palm motion.
         * Use clearHandPose to release the override.
         * forceTop maps to FRIK's forced external compatibility priority.
         */
        bool (FRIK_CALL*setHandPoseCustom)(const char* tag, Hand hand, const HandPoseData& handPose, bool forceTop);

        /**
         * Set a hand pose override with explicit priority level.
         * Higher priority wins when multiple tags are active.
         * Known levels: External=50, OffhandGrip=60, Attaboy=65, Pipboy=70, ConfigMode=70, ForcedExternal=90, PhysicsGrab=100.
         */
        bool (FRIK_CALL*setHandPoseWithPriority)(const char* tag, Hand hand, HandPoseKind handPose, int priority);

        // --- ROCK visual-authority additions (API v5) ---

        /**
         * Get the world transform of the hand bone (after IK solving).
         */
        RE::NiTransform (FRIK_CALL*getHandWorldTransform)(Hand hand);

        /**
         * Set a custom 5-finger hand pose override with explicit priority.
         * Values: 0.0 = fully bent, 1.0 = fully straight.
         */
        bool (FRIK_CALL*setHandPoseCustomFingerPositionsWithPriority)(const char* tag, Hand hand, float thumb, float index, float middle, float ring, float pinky, int priority);

        /**
         * Set a custom 15-joint hand pose override with explicit priority.
         * Values: 0.0 = fully bent, 1.0 = fully straight.
         */
        bool (FRIK_CALL*setHandPoseCustomJointPositionsWithPriority)(const char* tag, Hand hand, const float values[15], int priority);

        /**
         * Apply an immediate world-space visual hand target through FRIK's first-person arm chain.
         * This is intentionally skeleton-only: external mods still own their weapon/object solve.
         */
        bool (FRIK_CALL*applyExternalHandWorldTransform)(const char* tag, Hand hand, const RE::NiTransform& worldTarget, int priority);

        /**
         * Clear a previously submitted external world-space visual hand target.
         */
        bool (FRIK_CALL*clearExternalHandWorldTransform)(const char* tag, Hand hand);

        /**
         * Set local transforms for individual finger bones with explicit priority.
         * This augments scalar hand poses under the same tag instead of replacing
         * them, allowing callers to override specific joints that need a different
         * local rotation basis. The tag must already have an active scalar or
         * per-joint pose entry; local transforms are not a standalone hand pose.
         */
        bool (FRIK_CALL*setHandPoseCustomLocalTransformsWithPriority)(const char* tag, Hand hand, const FingerLocalTransformOverride* overrideData, int priority);

        /**
         * Build FRIK-authored 15-bone local transforms for the same per-joint
         * scalar values accepted by setHandPoseCustomJointPositions*. This lets
         * clients start from FRIK's current open/closed hand tables before
         * applying mesh-contact corrections, instead of duplicating those
         * tables out of process.
         */
        bool (FRIK_CALL*getHandPoseLocalTransformsForJointPositions)(Hand hand, const float values[15], FingerLocalTransformOverride* outTransforms);

        /**
         * Initialize the FRIK API object.
         * NOTE: call after all mods have been loaded in the game (GameLoaded event).
         *
         * @param minVersion the minimal version required (default: the compiled against version)
         * @return error codes:
         * 0 - Successful
         * 1 - Failed to find FRIK.dll (trying to init too early?)
         * 2 - No FRIKAPI_GetApi API found
         * 3 - Failed FRIKAPI_GetApi call
         * 4 - FRIK API version is older than the minimal required version
         */
        [[nodiscard]] static int initialize(const uint32_t minVersion = FRIK_API_VERSION)
        {
            if (inst) {
                return 0;
            }

            // get FRIK.dll
            const auto frikDll = GetModuleHandleA("FRIK.dll");
            if (!frikDll) {
                return 1;
            }

            const auto getApi = reinterpret_cast<const FRIKApi* (FRIK_CALL*)()>(GetProcAddress(frikDll, "FRIKAPI_GetApi"));
            if (!getApi) {
                return 2;
            }

            const auto frikApi = getApi();
            if (!frikApi) {
                return 3;
            }

            // check against expected version
            if (frikApi->getVersion() < minVersion) {
                return 4;
            }

            inst = frikApi;
            return 0;
        }

        /**
         * The initialized instance of FRIK API interface.
         * Use after successful call to initialize.
         */
        inline static const FRIKApi* inst = nullptr;
    };

    static_assert(FRIK_API_VERSION == 5, "FRIK API v5 is upstream v4 hand-pose compatibility plus ROCK visual-authority support");
}
