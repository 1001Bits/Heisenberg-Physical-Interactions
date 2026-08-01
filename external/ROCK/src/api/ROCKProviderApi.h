#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOMMNOSOUND
#define NOMMNOSOUND
#endif

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#if !defined(_WINDOWS_) && !defined(_INC_WINDOWS)
struct HINSTANCE__;
using HMODULE = HINSTANCE__*;
using LPCSTR = const char*;
using FARPROC = std::intptr_t(__stdcall*)();
extern "C" __declspec(dllimport) HMODULE __stdcall GetModuleHandleA(LPCSTR lpModuleName);
extern "C" __declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError(void);
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(LPCSTR lpOutputString);
#endif

// TEMPORARY DIAGNOSTIC INSTRUMENTATION (2026-07-23) — tracking down a case where
// initialize() returns code 1 (module not found) even after the caller confirms
// Heisenberg_F4VR.dll is loaded via other means. Visible in DebugView / an attached
// debugger's Output window, no logging framework dependency required. Safe to strip
// once resolved.
#include <cstdio>
namespace rock::provider::diag
{
    inline void log(const char* msg)
    {
        OutputDebugStringA(msg);
    }

    inline void logModuleLookupFailure(const char* moduleName)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "[ROCK-DIAG] GetModuleHandleA(\"%s\") failed, GetLastError()=%lu\n",
            moduleName, GetLastError());
        OutputDebugStringA(buf);
    }
}

namespace rock::provider
{
#if defined(ROCK_API_EXPORTS)
#define ROCK_PROVIDER_API extern "C" __declspec(dllexport)
#else
#define ROCK_PROVIDER_API extern "C" __declspec(dllimport)
#endif

#define ROCK_PROVIDER_CALL __cdecl

    inline constexpr std::uint32_t ROCK_PROVIDER_API_VERSION = 1;
    inline constexpr std::uint32_t ROCK_PROVIDER_FRAME_SNAPSHOT_V1_SIZE = 256;
    inline constexpr std::uint32_t ROCK_PROVIDER_INTERACTION_COMMAND_RESULT_V1_PREFIX_SIZE = 112;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_BODIES = 8;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_EVIDENCE_DETAILS_V1 =
        ROCK_PROVIDER_MAX_WEAPON_BODIES;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_EVIDENCE_POINTS_PER_DETAIL_V1 = 252;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_EVIDENCE_NAME = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_EXTERNAL_BODIES_V1 = 2048;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_EXTERNAL_CONTACTS_V1 = 512;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_BODY_CONTACTS_V1 = 128;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_FRAME_CALLBACKS_V1 = 16;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_CONSUMERS_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_INTERACTION_COMMANDS_V1 = 32;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_COMPLETED_INTERACTION_COMMANDS_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_HAND_INPUT_SUPPRESSIONS_V1 = 32;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_HAND_INPUT_SUPPRESSION_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_TARGETS_V1 = 128;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_DRIVES_V1 = 64;
    /*
     * The value previously shown in the public dry-positioning example is now
     * an actual set-and-forget lease: ROCK keeps applying the drive until its
     * owner clears/replaces the drive set, unregisters, or the provider loses
     * its lifecycle. Values at or above this sentinel have the same meaning.
     */
    inline constexpr std::uint32_t ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1 = 0x00FF'FFFFu;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_DRIVE_LEASE_FRAMES_V1 =
        ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1 - 1u;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_MOTION_CONSTRAINTS_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_INTERACTION_ZONES_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_LEARNED_WEAPON_PART_PROFILES_V1 = 32;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_PLUGIN_NAME_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_NODE_PATH_V1 = 192;
    inline constexpr std::uint32_t ROCK_PROVIDER_WEAPON_PART_KIND_ANY_V1 = 0xFFFF'FFFFu;
    inline constexpr std::uint32_t ROCK_PROVIDER_WEAPON_ACTION_ROLE_ANY_V1 = 0xFFFF'FFFFu;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_EMITTERS_V1 = 32;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_NATIVE_ANIMATION_AUTHORITY_LEASE_FRAMES_V1 = 1200;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_ANIMATION_PHASE_CALLBACKS_V1 = 16;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_HAND_VISUAL_AUTHORITY_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_NATIVE_ANIMATION_RUNTIME_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_EQUIPPED_WEAPON_HANDLING_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_DEBUG_OVERLAY_PUBLICATION_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_DEBUG_OVERLAY_PUBLISHERS_V1 = 8;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_DEBUG_OVERLAY_LINES_PER_PUBLISHER_V1 = 1024;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_DEBUG_OVERLAY_TEXT_PER_PUBLISHER_V1 = 16;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_DEBUG_OVERLAY_LINES_V1 = 2048;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_DEBUG_OVERLAY_TEXT_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_DEBUG_OVERLAY_TEXT_CAPACITY_V1 = 128;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_PROVIDER_EVENTS_V1 = 256;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_EXTERNAL_SCOPES_V1 = 256;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_HAND_HELD_BODIES_V1 = 8;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_COMPOSITION_ENTRIES_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_SEMANTIC_HAND_CONTACTS_V1 = 20;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_PLAYER_COLLIDER_DESCRIPTORS_V1 = 96;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_POSES_V1 = 128;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_WEAPON_PART_DRIVE_RESULTS_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_OFFHAND_RESERVATION_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_TOUCH_GRAB_TARGETS_V1 = 256;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_TOUCH_GRAB_SCOPES_V1 = 64;
    inline constexpr std::uint32_t ROCK_PROVIDER_MAX_TOUCH_GRAB_TARGET_LEASE_FRAMES_V1 = 120;
    inline constexpr std::uint16_t ROCK_PROVIDER_ALL_FINGER_LOCAL_TRANSFORMS_V1 = 0x7FFFu;

    /*
     * Every V1 lease uses the same exclusive expiry fence. A publication made
     * at frame F with leaseFrames N is active while currentFrame < F + N and
     * expires at F + N. Zero is invalid; values above the published family
     * limit are clamped. Refresh replaces the prior expiry and generations.
     */

    enum class RockProviderHand : std::uint32_t
    {
        None = 0,
        Right = 1,
        Left = 2,
    };

    enum class RockProviderHandStateFlag : std::uint32_t
    {
        None = 0,
        Touching = 1u << 0,
        Holding = 1u << 1,
        PhysicsDisabled = 1u << 2,
    };

    enum class RockProviderHandFrameFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        Left = 1u << 1,
        Primary = 1u << 2,
        Offhand = 1u << 3,
        HasSceneNode = 1u << 4,
        RootFlattenedAuthority = 1u << 5,
        PresentedVisual = 1u << 6,
    };

    enum class RockProviderExternalBodyRole : std::uint32_t
    {
        Unknown = 0,
        ReloadMobile = 1,
        ReloadSocket = 2,
        ReloadAction = 3,
        ReloadVisualProxy = 4,
        ActorRagdollBone = 100,
    };

    enum class RockProviderExternalBodyContactPolicy : std::uint32_t
    {
        None = 0,
        ReportHandContacts = 1u << 0,
        ReportAllSourceKinds = 1u << 1,
        SuppressRockDynamicPush = 1u << 2,
    };

    enum class RockProviderExternalSourceKind : std::uint32_t
    {
        Unknown = 0,
        Hand = 1,
        Weapon = 2,
        HeldObject = 3,
    };

    enum class RockProviderExternalContactQuality : std::uint32_t
    {
        BodyPairOnly = 0,
        AggregateImpulse = 1,
        RawPoint = 2,
    };

    /*
     * Touch-grab targets are a separate opt-in authority from ROCK's ordinary
     * loose-object grab. Limited mechanisms may name one live dynamic or
     * keyframed body. FixedAnchor may additionally match a bounded collision
     * layer/motion mask without changing that body's motion type; this is the
     * observation/ownership primitive needed by future climbing consumers.
     */
    enum class RockProviderTouchGrabKindV1 : std::uint32_t
    {
        FixedAnchor = 0,
        LimitedHinge = 1,
        LimitedPrismatic = 2,
    };

    enum class RockProviderTouchGrabTargetFlagV1 : std::uint32_t
    {
        None = 0,
        AllowRightHand = 1u << 0,
        AllowLeftHand = 1u << 1,
        AllowTwoHands = 1u << 2,
        LatchOnRelease = 1u << 3,
        MatchAnyBody = 1u << 4,
        MatchStaticMotion = 1u << 5,
        MatchKeyframedMotion = 1u << 6,
        MatchDynamicMotion = 1u << 7,
    };

    enum class RockProviderTouchGrabPhaseV1 : std::uint32_t
    {
        Inactive = 0,
        Armed = 1,
        Held = 2,
        Latched = 3,
        Yielded = 4,
        Invalidated = 5,
    };

    enum class RockProviderTouchGrabReleaseReasonV1 : std::uint32_t
    {
        None = 0,
        GripReleased = 1,
        OwnerYield = 2,
        TargetRemoved = 3,
        RegistrationExpired = 4,
        GenerationChanged = 5,
        WorldLost = 6,
        TargetInvalid = 7,
        HandUnavailable = 8,
    };

    enum class RockProviderTouchGrabStateFlagV1 : std::uint32_t
    {
        None = 0,
        ContactPointValid = 1u << 0,
        ContactNormalValid = 1u << 1,
        CoordinateValid = 1u << 2,
        FixedAnchor = 1u << 3,
        OriginalMotionKeyframed = 1u << 4,
        OriginalMotionDynamic = 1u << 5,
    };

    enum class RockProviderTouchGrabHandMaskV1 : std::uint32_t
    {
        None = 0,
        Right = 1u << 0,
        Left = 1u << 1,
    };

    enum class RockProviderOffhandReservation : std::uint32_t
    {
        Normal = 0,
        ReloadReserved = 1,
        ReloadPoseOverride = 2,
    };

    enum class RockProviderBodyZoneSide : std::uint32_t
    {
        Center = 0,
        Left = 1,
        Right = 2,
    };

    enum class RockProviderBodyZoneKind : std::uint32_t
    {
        Unknown = 0,
        Pelvis = 1,
        SpineLower = 2,
        SpineUpper = 3,
        Chest = 4,
        NeckHead = 5,
        LeftShoulder = 6,
        LeftUpperArm = 7,
        LeftForearmUpper = 8,
        LeftForearmLower = 9,
        LeftHand = 10,
        RightShoulder = 11,
        RightUpperArm = 12,
        RightForearmUpper = 13,
        RightForearmLower = 14,
        RightHand = 15,
        LeftHip = 16,
        LeftThigh = 17,
        LeftCalf = 18,
        LeftFoot = 19,
        RightHip = 20,
        RightThigh = 21,
        RightCalf = 22,
        RightFoot = 23,
    };

    enum class RockProviderBodyContactTargetKind : std::uint32_t
    {
        Unknown = 0,
        Hand = 1,
        Weapon = 2,
        HeldObject = 3,
        Body = 4,
        External = 5,
        WorldSurface = 6,
        DynamicProp = 7,
        Actor = 8,
        QueryOnly = 9,
    };

    enum class RockProviderLifecycleFlag : std::uint32_t
    {
        None = 0,
        WorldAvailable = 1u << 0,
        SkeletonReady = 1u << 1,
        ProviderReady = 1u << 2,
        MenuBlocking = 1u << 3,
        ConfigBlocking = 1u << 4,
        LoadingOrWorldTransition = 1u << 5,
        GeneratedBodiesValid = 1u << 6,
        PhysicsWriteAllowed = 1u << 7,
        VisualWriteAllowed = 1u << 8,
    };

    enum class RockProviderLifecycleReason : std::uint32_t
    {
        None = 0,
        GameLoaded = 1,
        SkeletonReady = 2,
        SkeletonDestroying = 3,
        PowerArmorChanged = 4,
        WorldAvailable = 5,
        WorldChanged = 6,
        WorldUnavailable = 7,
        ProviderReady = 8,
        ProviderLost = 9,
        MenuBlocked = 10,
        ConfigBlocked = 11,
        GeneratedBodiesRebuilt = 12,
        GeneratedBodiesInvalidated = 13,
        TransitionSettled = 14,
        Shutdown = 15,
    };

    enum class RockProviderResultV1 : std::uint32_t
    {
        Ok = 0,
        NotReady = 1,
        InvalidArgument = 2,
        InvalidSize = 3,
        UnsupportedVersion = 4,
        CapacityFull = 5,
        OwnerNotRegistered = 6,
        OwnerConflict = 7,
        PermissionDenied = 8,
        WorldNotReady = 9,
        TargetInvalid = 10,
        TargetUnavailable = 11,
        HandUnavailable = 12,
        HandBusy = 13,
        ObjectAlreadyOwned = 14,
        RequestQueued = 15,
        RequestNotFound = 16,
        WrongThread = 17,
        AlreadyCommitted = 18,
        // Heisenberg/Cylon learned-weapon-part results (renumbered after
        // upstream took 17/18 for WrongThread/AlreadyCommitted).
        ProfileUnavailable = 19,
        ProfileAmbiguous = 20,
    };

    enum class RockProviderConsumerCapabilityV1 : std::uint32_t
    {
        None = 0,
        FrameSnapshots = 1u << 0,
        ExternalBodies = 1u << 1,
        ExternalContacts = 1u << 2,
        OffhandReservation = 1u << 3,
        InteractionCommands = 1u << 4,
        HandInputSuppression = 1u << 5,
        WeaponPartInteraction = 1u << 6,
        /*
         * ---- FROZEN: bits 7 and 8 belong to Cylon ----
         * Cylon's VirtualReloads is a PREBUILT BINARY. Its
         * registration.requestedCapabilities mask is baked into its machine code
         * from the header shipped in dist/Cylon_ROCKProvider_Complete_2026-07-24,
         * which defines WeaponPartMotionConstraint = 1u << 7 (its enum stops
         * there). Heisenberg 0.8.4 shipped the same numbering plus
         * LearnedWeaponPartProfiles = 1u << 8.
         *
         * The attempt-6 port moved these to 1u<<25 / 1u<<26 and let upstream's
         * NativeAnimationAuthority/AnimationPhases take 7/8. Because both of those
         * are in kImplementedConsumerCapabilitiesV1, the grant mask
         * (requested & kImplemented) did not strip Cylon's bit 7 - it silently
         * REINTERPRETED it as NativeAnimationAuthority. Cylon then passed its own
         * "(granted & required) == required" check, believed it was connected, and
         * every setWeaponPartMotionConstraintsV1/clearWeaponPartMotionConstraintsV1
         * call was rejected by validateRegisteredOwnerCapabilityLocked with no
         * crash and no error - the whole weapon-part motion-constraint API built
         * for Cylon's reload part-grab stopped working, invisibly.
         *
         * Upstream's block therefore starts at bit 9. Do not renumber these two
         * without recompiling Cylon's binary; the static_asserts below enforce it.
         */
        WeaponPartMotionConstraint = 1u << 7,
        LearnedWeaponPartProfiles = 1u << 8,
        // ---- Upstream capabilities: relocated to bit 9 and above ----
        NativeAnimationAuthority = 1u << 9,
        AnimationPhases = 1u << 10,
        EquippedWeaponGripState = 1u << 11,
        HandVisualAuthority = 1u << 12,
        NativeAnimationRuntimeProvider = 1u << 13,
        EquippedWeaponHandlingAuthority = 1u << 14,
        DebugOverlayPublication = 1u << 15,
        ProviderEvents = 1u << 16,
        HandInteractionState = 1u << 17,
        ExternalBodyScopes = 1u << 18,
        WeaponPartObservability = 1u << 19,
        WeaponComposition = 1u << 20,
        PoseReadback = 1u << 21,
        SemanticHandContacts = 1u << 22,
        PlayerColliderDescriptors = 1u << 23,
        ScopeSightState = 1u << 24,
        InputObservability = 1u << 25,
        TouchGrabTargets = 1u << 26,
    };

    /*
     * FROZEN ABI pins for the two consumer-capability bits Cylon's prebuilt binary
     * hardcodes. The existing FROZEN block below pins the function-table offsets and
     * the feature-bit mask but had no pin on this enum, which is exactly why the
     * renumbering above compiled green and shipped silently broken.
     */
    static_assert(
        static_cast<std::uint32_t>(RockProviderConsumerCapabilityV1::WeaponPartInteraction) == (1u << 6),
        "FROZEN ABI: Cylon's prebuilt binary requests this exact bit.");
    static_assert(
        static_cast<std::uint32_t>(RockProviderConsumerCapabilityV1::WeaponPartMotionConstraint) == (1u << 7),
        "FROZEN ABI: Cylon's prebuilt binary requests this exact bit.");
    static_assert(
        static_cast<std::uint32_t>(RockProviderConsumerCapabilityV1::LearnedWeaponPartProfiles) == (1u << 8),
        "FROZEN ABI: shipped in Heisenberg 0.8.4 at this bit.");

    enum class RockProviderFeatureBitV1 : std::uint32_t
    {
        None = 0,
        FrameCallbacks = 1u << 0,
        LifecycleFields = 1u << 1,
        HandFrames = 1u << 2,
        WeaponEvidence = 1u << 3,
        BodyContacts = 1u << 4,
        ExternalContacts = 1u << 5,
        ConsumerRegistrationV1 = 1u << 8,
        OwnerFilteredExternalContactsV1 = 1u << 9,
        InteractionCommandQueue = 1u << 10,
        ForceGrabCommand = 1u << 11,
        ForceReleaseCommand = 1u << 12,
        ThrownDropCommand = 1u << 13,
        HandInputSuppression = 1u << 14,
        WeaponPartInteraction = 1u << 15,
        WeaponPartGripState = 1u << 16,
        WeaponPartRecordIdentity = 1u << 17,
        WeaponPartTargetNonExclusive = 1u << 18,
        RawWandButtonState = 1u << 19,
        PipboyInputSuppression = 1u << 20,
        /*
         * ---- FROZEN: bits 21..26 belong to shipped Heisenberg 0.8.4 ----
         *
         * These six values are a shipped compatibility contract, exactly like
         * the frozen function-table prefix further down this header. Existing
         * consumers test these bit positions numerically, so the NUMBERS may
         * never move.
         *
         * A previous integration pass had this backwards. Its comment read
         * "Upstream owns 21..29 in this enum, so these use the slots upstream
         * left free (6, 7, 30, 31)" and it relocated these four Cylon bits to
         * 6/7/30/31, handing 21..24 to four new upstream features. That is a
         * silent, severe break rather than a loud one: the bits still exist
         * under the same NAMES, so everything compiles and every by-name call
         * site keeps working, while over the wire Cylon reads bit 23 (its
         * WeaponPartExclusiveExactContact) and is actually told the state of
         * upstream's AnimationPhases, and bit 21 (its WeaponPartMotionConstraint)
         * and is told the state of WeaponEmitters. Cylon would then invoke
         * setWeaponPartMotionConstraintsV1 / rely on exclusive exact contact on
         * the strength of unrelated flags.
         *
         * Upstream is compiled from source in this tree, so upstream's bits are
         * the ones that are free to move. The four displaced upstream features
         * are rehomed below at 6, 7, 30 and 31.
         */
        WeaponPartMotionConstraint = 1u << 21,
        WeaponPartDrivePersistentLease = 1u << 22,
        // Exclusive weapon-part targets require collision with a matching
        // generated body. ROCK accepts the native contact callback or an
        // exact reconstruction from the live hand-collider hull samples and
        // that matched body's mesh triangles. The ordinary whole-weapon
        // palm/AABB proximity probe is never used.
        WeaponPartExclusiveExactContact = 1u << 23,
        /*
         * Drive targets and motion constraints may select a collision source
         * with their normal matcher while writing a separate, related scene
         * node through controlledRoot. This is required when the generated
         * collision leaf and the consumer's authored bolt/slide node are not
         * the same NiAVObject (and therefore do not share a parent-local
         * transform or even the same immediate scene-graph branch).
         */
        WeaponPartControlledRoot = 1u << 24,
        /*
         * Consumer-authored, exact-body interaction zones. This bit shipped in
         * Heisenberg 0.8.4 and is read numerically by already-built consumers.
         */
        WeaponPartInteractionZone = 1u << 25,
        /*
         * Provider-owned profiles learned from a bounded vanilla reload
         * animation. This bit also shipped in Heisenberg 0.8.4.
         */
        LearnedWeaponPartProfiles = 1u << 26,
        // ---- END OF THE FROZEN SHIPPED REGION ----

        /*
         * Upstream additions. Bits 6 and 7 are unallocated in both Cylon's
         * header and shipped Heisenberg 0.8.4 -- no released consumer binds a
         * name to them, so nothing can misread them -- which makes them the
         * correct home for the two upstream features displaced out of 21/22.
         * Likewise 30/31, vacated by returning Cylon's bits to 23/24. The
         * count works out exactly: 4 displaced features, 4 genuinely free
         * slots. Bits 27..29 remain the homes already assigned to their
         * upstream additions. HandVisualAuthority and
         * NativeAnimationRuntimeProvider overflow into the second feature word
         * below rather than stealing the shipped 25/26 meanings.
         */
        WeaponEmitters = 1u << 6,           // moved from 21 (Cylon's)
        NativeAnimationAuthority = 1u << 7, // moved from 22 (Cylon's)
        AnimationPhases = 1u << 30,         // moved from 23 (Cylon's)
        EquippedWeaponGripState = 1u << 31, // moved from 24 (Cylon's)
        EquippedWeaponHandlingAuthority = 1u << 27,
        DebugOverlayPublication = 1u << 28,
        PresentedHandFrames = 1u << 29,
    };

    /*
     * Pins for the frozen Cylon feature bits.
     *
     * The four-bit break these catch was invisible to every other check in the
     * tree: the names all still resolved, the table size was unchanged, and no
     * call site referenced a bit numerically, so it compiled and linked clean
     * and only misbehaved across the process boundary against a binary that
     * cannot be rebuilt. The offsetof pins further down protect the function
     * table the same way; this block protects the flag word that tells a
     * consumer which of those functions are safe to call.
     *
     * All 25 shipped bits are pinned, not just the ones that broke, because
     * the failure mode is silent and the cost of full coverage is nil.
     *
     * Bits 6 and 7 are deliberately absent from Cylon's header. They are
     * asserted free of any Cylon meaning by their absence here, and are
     * reused above for upstream features.
     */
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::FrameCallbacks) == (1u << 0));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::LifecycleFields) == (1u << 1));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::HandFrames) == (1u << 2));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponEvidence) == (1u << 3));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::BodyContacts) == (1u << 4));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::ExternalContacts) == (1u << 5));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::ConsumerRegistrationV1) == (1u << 8));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::OwnerFilteredExternalContactsV1) == (1u << 9));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::InteractionCommandQueue) == (1u << 10));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::ForceGrabCommand) == (1u << 11));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::ForceReleaseCommand) == (1u << 12));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::ThrownDropCommand) == (1u << 13));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::HandInputSuppression) == (1u << 14));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartInteraction) == (1u << 15));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartGripState) == (1u << 16));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartRecordIdentity) == (1u << 17));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartTargetNonExclusive) == (1u << 18));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::RawWandButtonState) == (1u << 19));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::PipboyInputSuppression) == (1u << 20));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartMotionConstraint) == (1u << 21));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartDrivePersistentLease) == (1u << 22));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartExclusiveExactContact) == (1u << 23));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartControlledRoot) == (1u << 24));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartInteractionZone) == (1u << 25),
        "FROZEN ABI: shipped in Heisenberg 0.8.4 at bit 25.");
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBitV1::LearnedWeaponPartProfiles) == (1u << 26),
        "FROZEN ABI: shipped in Heisenberg 0.8.4 at bit 26.");

    /*
     * The frozen region must also stay collision-free: every bit Cylon owns
     * appears exactly once across the whole enum. If a future edit rehomes an
     * upstream feature onto a Cylon bit, the OR below stops being a disjoint
     * union and this fires.
     */
    inline constexpr std::uint32_t ROCK_PROVIDER_FROZEN_CYLON_FEATURE_BITS_V1 = 0x07FFFF3Fu;
    static_assert(
        ROCK_PROVIDER_FROZEN_CYLON_FEATURE_BITS_V1 ==
            (static_cast<std::uint32_t>(RockProviderFeatureBitV1::FrameCallbacks) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::LifecycleFields) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::HandFrames) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponEvidence) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::BodyContacts) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::ExternalContacts) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::ConsumerRegistrationV1) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::OwnerFilteredExternalContactsV1) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::InteractionCommandQueue) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::ForceGrabCommand) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::ForceReleaseCommand) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::ThrownDropCommand) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::HandInputSuppression) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartInteraction) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartGripState) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartRecordIdentity) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartTargetNonExclusive) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::RawWandButtonState) |
             static_cast<std::uint32_t>(RockProviderFeatureBitV1::PipboyInputSuppression) |
              static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartMotionConstraint) |
              static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartDrivePersistentLease) |
              static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartExclusiveExactContact) |
              static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartControlledRoot) |
              static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponPartInteractionZone) |
              static_cast<std::uint32_t>(RockProviderFeatureBitV1::LearnedWeaponPartProfiles)),
        "Cylon's frozen feature bits have moved. These bit positions are a "
        "shipped ABI contract with a binary that cannot be recompiled; rehome "
        "the upstream feature instead.");
    static_assert(
        (ROCK_PROVIDER_FROZEN_CYLON_FEATURE_BITS_V1 &
         (static_cast<std::uint32_t>(RockProviderFeatureBitV1::WeaponEmitters) |
           static_cast<std::uint32_t>(RockProviderFeatureBitV1::NativeAnimationAuthority) |
           static_cast<std::uint32_t>(RockProviderFeatureBitV1::AnimationPhases) |
           static_cast<std::uint32_t>(RockProviderFeatureBitV1::EquippedWeaponGripState) |
           static_cast<std::uint32_t>(RockProviderFeatureBitV1::EquippedWeaponHandlingAuthority) |
           static_cast<std::uint32_t>(RockProviderFeatureBitV1::DebugOverlayPublication) |
           static_cast<std::uint32_t>(RockProviderFeatureBitV1::PresentedHandFrames))) == 0u,
        "An upstream feature bit overlaps Cylon's frozen region.");

    enum class RockProviderFeatureBit2V1 : std::uint32_t
    {
        None = 0,
        SafeDescriptor = 1u << 0,
        ExtendedLimits = 1u << 1,
        PublicStructureSizes = 1u << 2,
        OwnerFrameCallbacks = 1u << 3,
        HandInteractionState = 1u << 4,
        ProviderEvents = 1u << 5,
        EquippedWeaponState = 1u << 6,
        ExternalBodyScopes = 1u << 7,
        ExternalContactCursor = 1u << 8,
        WeaponPartResolution = 1u << 9,
        WeaponPartPoses = 1u << 10,
        WeaponPartDriveResults = 1u << 11,
        ScopeSightState = 1u << 12,
        WeaponComposition = 1u << 13,
        AuthoredGripSnapshot = 1u << 14,
        PresentedHandPose = 1u << 15,
        SemanticHandContacts = 1u << 16,
        PlayerColliderDescriptors = 1u << 17,
        HandCollisionAvailability = 1u << 18,
        CommandCancellation = 1u << 19,
        InputSuppressionState = 1u << 20,
        OffhandReservationLeases = 1u << 21,
        SnapshotEnrichment = 1u << 22,
        NativeAnimationRuntimeLeases = 1u << 23,
        StatefulPublicationLeases = 1u << 24,
        CommandLifecycle = 1u << 25,
        InputSampleMetadata = 1u << 26,
        WeaponClassificationEnrichment = 1u << 27,
        ExternalContactEnrichment = 1u << 28,
        TouchGrabTargets = 1u << 29,
        /*
         * New post-0.8.4 features live in the overflow word. Bits 30/31 became
         * available here when the shipped WeaponPartInteractionZone and
         * LearnedWeaponPartProfiles meanings were restored to featureBits
         * bits 25/26.
         */
        HandVisualAuthority = 1u << 30,
        NativeAnimationRuntimeProvider = 1u << 31,
    };

    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBit2V1::HandVisualAuthority) == (1u << 30));
    static_assert(static_cast<std::uint32_t>(RockProviderFeatureBit2V1::NativeAnimationRuntimeProvider) == (1u << 31));

    /*
     * Selective Bethesda animation authority. ROCK preserves the authored
     * relationship between both arms, hands, and weapon, then rigidly anchors
     * both skeleton trees to the visible first-person weapon/controller world
     * frame while retaining the weapon's authored motion since lease acquisition.
     * Arms covers only the two
     * collarbone-to-hand chains; Hands adds the hand roots and finger/thumb
     * descendants; Weapon covers only Weapon and WeaponLeft. The character
     * root, COM, torso, head, and legs remain owned by the live VR body.
     */
    enum class RockProviderNativeAnimationAuthorityFlagV1 : std::uint32_t
    {
        None = 0,
        Arms = 1u << 0,
        Hands = 1u << 1,
        Weapon = 1u << 2,
        ReloadPose = (1u << 0) | (1u << 1) | (1u << 2),
    };

    enum class RockProviderNativeAnimationAuthorityStatusFlagV1 : std::uint32_t
    {
        None = 0,
        HookInstalled = 1u << 0,
        RuntimeEnabled = 1u << 1,
        CaptureValid = 1u << 2,
        LocalReloadTestLeaseActive = 1u << 3,
        HookInstallFailed = 1u << 4,
        ThreadMismatch = 1u << 5,
        CaptureFault = 1u << 6,
        RuntimeProviderAvailable = 1u << 7,
    };

    enum class RockProviderAnimationPhaseV1 : std::uint32_t
    {
        BeforeRock = 1,
        AfterRock = 2,
        Complete = 3,
        NativeGraphOutput = 4,
    };

    enum class RockProviderAnimationPhaseContextFlagV1 : std::uint32_t
    {
        None = 0,
        RockEnabled = 1u << 0,
        ProviderReady = 1u << 1,
        SkeletonReady = 1u << 2,
        MenuBlocking = 1u << 3,
        ConfigBlocking = 1u << 4,
        VisualWritesAllowed = 1u << 5,
    };

    enum class RockProviderEquippedWeaponGripStateFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        TwoHandGripActive = 1u << 1,
        FiringHandLeft = 1u << 2,
        WeaponTransformOwned = 1u << 3,
        WeaponWorldValid = 1u << 4,
        RightHandInWeaponValid = 1u << 5,
        LeftHandInWeaponValid = 1u << 6,
    };

    /*
     * Owner-bound policy supplied by a standalone equipped-weapon addon.
     * ROCK remains the low-level hand, weapon-node, physics, input-routing,
     * and inventory executor. The consumer selects which optional behaviors
     * are active and supplies their bounded tuning without changing physical
     * controller identity or Fallout 4 VR's native handedness setting.
     */
    enum class RockProviderEquippedWeaponHandlingFlagV1 : std::uint32_t
    {
        None = 0,
        FiringGripOwnership = 1u << 0,
        PrimaryDetach = 1u << 1,
        AmbidextrousHandoff = 1u << 2,
        GripZoneEquip = 1u << 3,
        GripZoneHoverHaptics = 1u << 4,
        // Replaces ROCK's configured radius while this authority lease lives;
        // the core VisualOnlySupport behavior itself remains always enabled.
        FiringGripProximitySupport = 1u << 5,
        EquippedWeaponShoulderStash = 1u << 6,
        PipboyTriggerHandEquip = 1u << 7,
        // Retained in V1 for source/ABI compatibility. The visual bridge and
        // native attach recovery are now unconditional ROCK correctness
        // services; addons may still supply the bounded blend/timeout tuning.
        EquipVisualBridge = 1u << 8,
    };

    enum class RockProviderEquippedWeaponHandlingRuntimeFlagV1 : std::uint32_t
    {
        None = 0,
        AuthorityActive = 1u << 0,
        FixedHandLeft = 1u << 1,
        FiringHandLeft = 1u << 2,
        LeftFiringInfrastructureAvailable = 1u << 3,
        ManualOwnershipActive = 1u << 4,
        PartCarryActive = 1u << 5,
        FiringGripOccupied = 1u << 6,
        WeaponPresent = 1u << 7,
    };

    enum class RockProviderHandVisualAuthorityFlagV1 : std::uint32_t
    {
        None = 0,
        WorldTransform = 1u << 0,
        FingerLocalTransforms = 1u << 1,
    };

    enum class RockProviderDebugOverlayTextFlagV1 : std::uint32_t
    {
        None = 0,
        WorldAnchored = 1u << 0,
    };

    enum class RockProviderWeaponEmitterKindV1 : std::uint32_t
    {
        Unknown = 0,
        Flashlight = 1,
        Laser = 2,
        Reticle = 3,
    };

    enum class RockProviderWeaponEmitterSourceV1 : std::uint32_t
    {
        Unknown = 0,
        EffectGeometry = 1,
        AddOnNode = 2,
    };

    enum class RockProviderWeaponEmitterFlagV1 : std::uint32_t
    {
        None = 0,
        TransformValid = 1u << 0,
        DirectionValid = 1u << 1,
        EffectStateKnown = 1u << 2,
        HasAddOnNodeValue = 1u << 3,
        HasOmod = 1u << 4,
        HasAttachPoint = 1u << 5,
    };

    enum class RockProviderInteractionCommandKindV1 : std::uint32_t
    {
        Unknown = 0,
        ForceGrab = 1,
        ForceRelease = 2,
        ThrownDrop = 3,
    };

    enum class RockProviderInteractionCommandStateV1 : std::uint32_t
    {
        Unknown = 0,
        Queued = 1,
        Succeeded = 2,
        Rejected = 3,
        Cancelled = 4,
    };

    enum class RockProviderInteractionFailureV1 : std::uint32_t
    {
        None = 0,
        ProviderNotReady = 1,
        PhysicsWritesBlocked = 2,
        OwnerNotRegistered = 3,
        InvalidRequest = 4,
        StaleWorldGeneration = 5,
        StaleSkeletonGeneration = 6,
        StaleProviderGeneration = 7,
        TargetMissing = 8,
        TargetUnavailable = 9,
        TargetBodyMissing = 10,
        TargetAlreadyOwned = 11,
        HandInvalid = 12,
        HandDisabled = 13,
        HandBusy = 14,
        HandNotHolding = 15,
        HeldObjectMismatch = 16,
    };

    enum class RockProviderForceGrabFlagV1 : std::uint32_t
    {
        None = 0,
        UsePreferredGrabPointGame = 1u << 0,
    };

    enum class RockProviderForceReleaseFlagV1 : std::uint32_t
    {
        None = 0,
        ImmediateCollisionRestore = 1u << 0,
        RequireMatchingTarget = 1u << 1,
        UseVelocityHavok = 1u << 2,
    };

    enum class RockProviderThrownDropFlagV1 : std::uint32_t
    {
        None = 0,
        ImmediateCollisionRestore = 1u << 0,
        RequireMatchingTarget = 1u << 1,
        UseVelocityHavok = 1u << 2,
    };

    enum class RockProviderHandInputSuppressionFlagV1 : std::uint32_t
    {
        None = 0,
        SuppressNormalGrabPress = 1u << 0,
        SuppressGrabRelease = 1u << 1,
        SuppressHeldWeaponTriggerEquip = 1u << 2,
        SuppressGameplayCandidates = 1u << 3,
        SuppressOpenVrGameInput = 1u << 4,
        SuppressConfigModeChord =
            static_cast<std::uint32_t>(SuppressNormalGrabPress) |
            static_cast<std::uint32_t>(SuppressGrabRelease) |
            static_cast<std::uint32_t>(SuppressHeldWeaponTriggerEquip) |
            static_cast<std::uint32_t>(SuppressGameplayCandidates),
    };

    enum class RockProviderWeaponPartGrabModeV1 : std::uint32_t
    {
        None = 0,
        FullTwoHandAuthority = 1,
        AttachOnly = 2,
    };

    enum class RockProviderWeaponPartTargetFlagV1 : std::uint32_t
    {
        None = 0,
        MatchBodyId = 1u << 0,
        MatchSourceRoot = 1u << 1,
        MatchSourceName = 1u << 2,
        MatchPartKind = 1u << 3,
        MatchReloadRole = 1u << 4,
        MatchSupportRole = 1u << 5,
        MatchSocketRole = 1u << 6,
        MatchActionRole = 1u << 7,
        /*
         * Non-exclusive target (feature bit WeaponPartTargetNonExclusive):
         * grants its grab mode on match without activating whitelist gating,
         * so unmatched parts keep their normal grip behavior. Omit the flag
         * for reload-session semantics where every unmatched part grip is
         * rejected while the whitelist is active. Providers advertising
         * WeaponPartExclusiveExactContact additionally require the live hand
         * colliders to touch the matched generated body; nearby geometry
         * cannot satisfy the target through the normal palm probe.
         */
        NonExclusive = 1u << 8,
    };

    enum class RockProviderWeaponPartDriveSpaceV1 : std::uint32_t
    {
        WeaponRootLocal = 0,
        /*
         * Parent-local space of controlledRoot when one is supplied; otherwise
         * parent-local space of the collision source selected by the matcher.
         */
        ControlledRootParentLocal = 1,
        SourceParentLocal = ControlledRootParentLocal,
    };

    /*
     * How a matched part's grabbed motion should be constrained instead of
     * following the hand freely in 3D. Linear projects hand displacement onto
     * a single axis through axisOrigin (a slide/detach path - magazine, bolt,
     * charging handle); Rotational projects it onto an angle swept around
     * axisDirection through axisOrigin (a hinge - cylinder, lever, break
     * action). Both clamp to [minValue, maxValue]. None (the default when no
     * constraint matches a part) leaves the existing free hand-relative
     * motion unchanged.
     */
    enum class RockProviderWeaponPartMotionKindV1 : std::uint32_t
    {
        None = 0,
        Linear = 1,
        Rotational = 2,
    };

    enum class RockProviderWeaponPartInteractionZoneShapeV1 : std::uint32_t
    {
        Box = 0,
        Sphere = 1,
    };

    enum class RockProviderWeaponPartInteractionZoneSpaceV1 : std::uint32_t
    {
        WeaponRootLocal = 0,
        SourceRootLocal = 1,
        ControlledRootLocal = 2,
    };

    /*
     * ClosestTargetMeshSurface is the responsive default: entering the larger
     * authored zone makes the exact target body eligible, then ROCK seats the
     * hand on that body's nearest real mesh surface. AnchorPosition preserves
     * the tracked hand orientation and translates its palm to snapAnchor.
     * FullHandTransform uses the handed transform supplied below verbatim.
     */
    enum class RockProviderWeaponPartInteractionZoneSnapModeV1 : std::uint32_t
    {
        ClosestTargetMeshSurface = 0,
        AnchorPosition = 1,
        FullHandTransform = 2,
    };

    enum class RockProviderWeaponPartInteractionZoneFlagV1 : std::uint32_t
    {
        None = 0,
        SnapAnchorValid = 1u << 0,
        RightHandTransformValid = 1u << 1,
        LeftHandTransformValid = 1u << 2,
    };

    enum class RockProviderLearnedWeaponPartProfilePoseV1 : std::uint32_t
    {
        Current = 0,
        Closed = 1,
        Open = 2,
        SyntheticDryFire = 3,
    };

    enum class RockProviderLearnedWeaponPartDryFireSourceV1 : std::uint32_t
    {
        Unknown = 0,
        InferredReloadHold = 1,
        GeneratedFromOpenEndpoint = 2,
        ConsumerOverride = 3,
    };

    enum class RockProviderLearnedWeaponPartProfileFlagV1 : std::uint32_t
    {
        None = 0,
        ClosedPoseObserved = 1u << 0,
        OpenPoseObserved = 1u << 1,
        DryFirePoseSynthetic = 1u << 2,
        DryFireFromReloadHold = 1u << 3,
        DryFireFromOpenEndpoint = 1u << 4,
        ControlledNodeLearned = 1u << 5,
        ExactEvidenceRequired = 1u << 6,
        GeneratedInteractionZone = 1u << 7,
        NeedsReview = 1u << 8,
        LiveEvidenceAvailable = 1u << 16,
        LiveControlledNodeAvailable = 1u << 17,
        Activatable = 1u << 18,
    };

    enum class RockProviderLearnedWeaponPartActivationFlagV1 : std::uint32_t
    {
        None = 0,
        InstallGrabTarget = 1u << 0,
        InstallMotionConstraint = 1u << 1,
        HoldInitialPose = 1u << 2,
        InstallInteractionZone = 1u << 3,
        OverrideInitialValue = 1u << 8,
        OverrideZone = 1u << 9,
        OverrideSnapAnchor = 1u << 10,
        OverrideRightHandTransform = 1u << 11,
        OverrideLeftHandTransform = 1u << 12,
        Default =
            static_cast<std::uint32_t>(InstallGrabTarget) |
            static_cast<std::uint32_t>(InstallMotionConstraint) |
            static_cast<std::uint32_t>(HoldInitialPose) |
            static_cast<std::uint32_t>(InstallInteractionZone),
    };

    /*
     * What a physical hand currently holds on the equipped weapon.
     * FiringGrip: the hand owns the firing grip (weapon rides this hand).
     * SupportFullAuthority: offhand support grip driving the two-hand solver.
     * SupportVisualOnly: visual-only support (sidearm shooting cup).
     * PartCarry: carry-authority part grip while the firing hand is detached.
     * AttachOnly: whitelist-mandated glue — the hand follows the part
     * (including provider part drives) but never steers the weapon.
     */
    enum class RockProviderWeaponPartGripKindV1 : std::uint32_t
    {
        None = 0,
        FiringGrip = 1,
        SupportFullAuthority = 2,
        SupportVisualOnly = 3,
        PartCarry = 4,
        AttachOnly = 5,
    };

    /*
     * Coarse physical size class for the currently equipped weapon. ROCK uses
     * this to pick a generated-collision max-distance-from-origin budget;
     * exposed here so other providers (reload/scope logic) don't need to
     * reimplement weapon-size classification.
     */
    enum class RockProviderWeaponSizeClassV1 : std::uint32_t
    {
        Melee = 0,
        Pistol = 1,
        Rifle = 2,
        Heavy = 3,
    };

    /*
     * Fallout4.esm's WeaponType* keyword tagging is reliable on vanilla weapons
     * but author-discretion on mods (verified directly: of two installed real
     * pistol mods, one tags every weapon, the other tags none). This records
     * which signal actually produced the size class so a consumer can tell a
     * confident keyword match from a weight-based guess.
     */
    enum class RockProviderWeaponClassificationSourceV1 : std::uint32_t
    {
        None = 0,
        Keyword = 1,
        WeightFallback = 2,
        Default = 3,
    };

    /*
     * One bit per Fallout4.esm WeaponType* keyword found on the equipped
     * weapon's own form. A bitmask rather than a single value because vanilla
     * weapons can legitimately carry more than one bucket keyword at once
     * (e.g. CombatShotgun carries both Rifle, the grip/animation category, and
     * Shotgun, the specific family) - callers that need the more specific tag
     * (e.g. a future reload/scope mod picking a shotgun-specific animation)
     * should prefer the most specific flag present rather than assuming
     * mutual exclusivity.
     */
    enum class RockProviderWeaponKeywordFlagV1 : std::uint64_t
    {
        None = 0,
        Pistol = 1ull << 0,
        Rifle = 1ull << 1,
        Shotgun = 1ull << 2,
        AssaultRifle = 1ull << 3,
        Sniper = 1ull << 4,
        GaussRifle = 1ull << 5,
        LaserMusket = 1ull << 6,
        HeavyGun = 1ull << 7,
        HandToHand = 1ull << 8,
        Melee1H = 1ull << 9,
        Melee2H = 1ull << 10,
        Unarmed = 1ull << 11,
        Minigun = 1ull << 12,
        Fatman = 1ull << 13,
        MissileLauncher = 1ull << 14,
        GatlingLaser = 1ull << 15,
        Flamer = 1ull << 16,
        Cryolater = 1ull << 17,
        JunkJet = 1ull << 18,
        RailwayRifle = 1ull << 19,
        Broadsider = 1ull << 20,
        Syringer = 1ull << 21,
        FlareGun = 1ull << 22,
        GammaGun = 1ull << 23,
        AlienBlaster = 1ull << 24,
        Ripper = 1ull << 25,
        Shishkebab = 1ull << 26,
        Laser = 1ull << 27,
        Plasma = 1ull << 28,
        Ballistic = 1ull << 29,
        Thrown = 1ull << 30,
        Grenade = 1ull << 31,
        Mine = 1ull << 32,
        Explosive = 1ull << 33,
        Automatic = 1ull << 34,
    };

    enum class RockProviderStructureIdV1 : std::uint32_t
    {
        ApiDescriptor = 1,
        ConsumerRegistration = 2,
        ConsumerHandle = 3,
        Limits = 4,
        LimitsExt = 5,
        FrameSnapshot = 6,
        HandFrame = 7,
        HandInteractionState = 8,
        ProviderEvent = 9,
        ProviderEventStreamState = 10,
        EquippedWeaponState = 11,
        ExternalBodyRegistration = 12,
        ExternalContact = 13,
        ExternalContactRecord = 14,
        ExternalContactStreamState = 15,
        WeaponPartTargetQuery = 16,
        WeaponPartTargetResolution = 17,
        WeaponPartPose = 18,
        WeaponPartDriveResult = 19,
        ScopeSightState = 20,
        WeaponCompositionState = 21,
        WeaponCompositionEntry = 22,
        AuthoredGripPose = 23,
        PresentedHandPose = 24,
        SemanticHandContact = 25,
        PlayerColliderDescriptor = 26,
        HandCollisionAvailability = 27,
        InputSuppressionState = 28,
        OffhandReservationRequest = 29,
        OffhandReservationState = 30,
        ForceGrabRequest = 31,
        ForceReleaseRequest = 32,
        ThrownDropRequest = 33,
        InteractionCommandResult = 34,
        HandInputSuppressionRequest = 35,
        RawWandButtonState = 36,
        WeaponPartTarget = 37,
        Transform = 38,
        WeaponPartDriveTarget = 39,
        WeaponPartGripState = 40,
        WeaponContactQuery = 41,
        WeaponContactResult = 42,
        WeaponClassification = 43,
        Point3 = 44,
        Bounds3 = 45,
        WeaponEmitter = 46,
        NativeAnimationAuthorityRequest = 47,
        NativeAnimationAuthorityState = 48,
        AnimationPhaseContext = 49,
        EquippedWeaponGripState = 50,
        EquippedWeaponHandlingRequest = 51,
        EquippedWeaponHandlingState = 52,
        HandVisualAuthorityRequest = 53,
        NativeAnimationRuntimePublication = 54,
        DebugOverlayLine = 55,
        DebugOverlayText = 56,
        DebugOverlayPublication = 57,
        WeaponEvidenceDetail = 58,
        BodyContact = 59,
        ApiFunctionTable = 60,
        TouchGrabTarget = 61,
        TouchGrabState = 62,
    };

    enum class RockProviderHandInteractionPhaseV1 : std::uint32_t
    {
        Idle = 0,
        Touching = 1,
        Selecting = 2,
        Pulling = 3,
        Catching = 4,
        Holding = 5,
        Releasing = 6,
        StashCandidate = 7,
        ConsumeCandidate = 8,
    };

    enum class RockProviderHandInteractionFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        Primary = 1u << 1,
        Offhand = 1u << 2,
        LooseObject = 1u << 3,
        LooseWeapon = 1u << 4,
        FiringGrip = 1u << 5,
        PartGrip = 1u << 6,
        PartCarry = 1u << 7,
        InputSuppressed = 1u << 8,
        CollisionAvailable = 1u << 9,
        TransitionSuppressed = 1u << 10,
        HeldBodyListTruncated = 1u << 11,
    };

    enum class RockProviderEventKindV1 : std::uint32_t
    {
        Unknown = 0,
        LifecycleChanged = 1,
        EquippedWeaponTransitionTerminal = 2,
        AuthorityLost = 3,
        InteractionCommandTerminal = 4,
        GrabStateChanged = 5,
    };

    enum class RockProviderEventStreamFlagV1 : std::uint32_t
    {
        None = 0,
        GapBeforeFirstCopied = 1u << 0,
        RingOverwroteRecords = 1u << 1,
    };

    enum class RockProviderAuthorityKindV1 : std::uint32_t
    {
        Unknown = 0,
        HandInputSuppression = 1,
        WeaponPartDrive = 2,
        NativeAnimation = 3,
        NativeAnimationRuntime = 4,
        EquippedWeaponHandling = 5,
        OffhandReservation = 6,
        HandVisual = 7,
        DebugOverlay = 8,
        WeaponPartTargets = 9,
    };

    enum class RockProviderEquippedWeaponTransitionSourceV1 : std::uint32_t
    {
        Unknown = 0,
        ObservedEquip = 1,
        HeldTriggerEquip = 2,
        HeldGripZoneEquip = 3,
        MenuExit = 4,
        WorkbenchExit = 5,
    };

    enum class RockProviderEquippedWeaponTransitionResultV1 : std::uint32_t
    {
        None = 0,
        Completed = 1,
        WeaponUnequipped = 2,
        IdentityLost = 3,
        ExpectedIdentityTimeout = 4,
        NativeAnimationHandoff = 5,
        WeaponNoLongerDrawn = 6,
        RecoveryExhausted = 7,
        ProviderLost = 8,
        Shutdown = 9,
    };

    enum class RockProviderEquippedWeaponStateFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        IdentityPending = 1u << 1,
        DrawPending = 1u << 2,
        BridgePresented = 1u << 3,
        NativeRenderable = 1u << 4,
        HandPoseHandoffComplete = 1u << 5,
        RecoveryExhausted = 1u << 6,
        TransitionActive = 1u << 7,
    };

    enum class RockProviderExternalContactFlagV1 : std::uint32_t
    {
        None = 0,
        SourceVelocityValid = 1u << 0,
        ContactPointValid = 1u << 1,
        ContactNormalValid = 1u << 2,
        ContactPointMeasured = 1u << 3,
        ContactPointEstimated = 1u << 4,
        CollisionAvailable = 1u << 5,
        TransitionSuppressed = 1u << 6,
    };

    enum class RockProviderExternalContactStreamFlagV1 : std::uint32_t
    {
        None = 0,
        GapBeforeFirstCopied = 1u << 0,
        RingOverwroteRecords = 1u << 1,
    };

    enum class RockProviderWeaponPartDriveApplicationV1 : std::uint32_t
    {
        Unknown = 0,
        Applied = 1,
        Unresolved = 2,
        StaleGeneration = 3,
        MissingParent = 4,
        LostPriority = 5,
        InvalidTransform = 6,
        CapacityRejected = 7,
        Restored = 8,
    };

    enum class RockProviderScopeActivationSourceV1 : std::uint32_t
    {
        None = 0,
        NativeGeometry = 1,
        RockGeometry = 2,
        ManualInput = 3,
    };

    enum class RockProviderScopeSightFlagV1 : std::uint32_t
    {
        None = 0,
        Available = 1u << 0,
        Active = 1u << 1,
        MenuOpen = 1u << 2,
        AnchorValid = 1u << 3,
        BoundsValid = 1u << 4,
        NativeOverlayValid = 1u << 5,
        ManualDirectTransitionRequired = 1u << 6,
    };

    enum class RockProviderWeaponPartPoseFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        SourceParentLocalValid = 1u << 1,
        WeaponRootLocalValid = 1u << 2,
    };

    enum class RockProviderWeaponClassificationProvenanceFlagV1 : std::uint32_t
    {
        None = 0,
        KeywordEvidence = 1u << 0,
        MeshBoundsFallback = 1u << 1,
        GenerationBound = 1u << 2,
    };

    enum class RockProviderWeaponCompositionFlagV1 : std::uint32_t
    {
        None = 0,
        Active = 1u << 0,
        Disabled = 1u << 1,
        AttachPointResolved = 1u << 2,
        SemanticEvidenceMatched = 1u << 3,
    };

    enum class RockProviderAuthoredGripSourceV1 : std::uint32_t
    {
        Unknown = 0,
        LiveEquippedGraph = 1,
        NativeIdlePreharvest = 2,
        RuntimeCanonical = 3,
    };

    enum class RockProviderAuthoredGripPoseFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        RightHandValid = 1u << 1,
        LeftHandValid = 1u << 2,
        RightFingersValid = 1u << 3,
        LeftFingersValid = 1u << 4,
    };

    enum class RockProviderPresentedHandPoseFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        HandWorldValid = 1u << 1,
        FingerLocalsValid = 1u << 2,
        RootFlattenedReadback = 1u << 3,
    };

    enum class RockProviderSemanticHandContactFlagV1 : std::uint32_t
    {
        None = 0,
        ContactPointValid = 1u << 0,
        ContactNormalValid = 1u << 1,
        TargetFormResolved = 1u << 2,
        HeldObjectRelation = 1u << 3,
        CollisionAvailable = 1u << 4,
        TransitionSuppressed = 1u << 5,
    };

    enum class RockProviderSemanticContactStateV1 : std::uint32_t
    {
        Begin = 1,
        Continued = 2,
        End = 3,
    };

    enum class RockProviderPlayerColliderKindV1 : std::uint32_t
    {
        Hand = 1,
        Body = 2,
    };

    enum class RockProviderPlayerColliderFlagV1 : std::uint32_t
    {
        None = 0,
        Valid = 1u << 0,
        Enabled = 1u << 1,
        PrimaryPalmAnchor = 1u << 2,
        TransformValid = 1u << 3,
        InPowerArmor = 1u << 4,
    };

    enum class RockProviderHandCollisionAvailabilityFlagV1 : std::uint32_t
    {
        None = 0,
        BodiesReady = 1u << 0,
        DynamicTwinsReady = 1u << 1,
        PhysicsWritesAllowed = 1u << 2,
        CollisionAvailable = 1u << 3,
        TransitionSuppressed = 1u << 4,
        MenuSuppressed = 1u << 5,
        HandDisabled = 1u << 6,
    };

    enum class RockProviderInputAvailabilityReasonV1 : std::uint32_t
    {
        Available = 0,
        HookNotSampled = 1,
        BlockingMenu = 2,
        ReleaseToRearm = 3,
        InvalidButton = 4,
    };

    enum class RockProviderSuppressionInvalidationReasonV1 : std::uint32_t
    {
        None = 0,
        Expired = 1,
        GenerationChanged = 2,
        OwnerUnregistered = 3,
        ProviderLost = 4,
        ExplicitClear = 5,
        CallbackFault = 6,
    };

    enum class RockProviderCommandStageV1 : std::uint32_t
    {
        Unknown = 0,
        Accepted = 1,
        Queued = 2,
        Committed = 3,
        Applied = 4,
        Terminal = 5,
    };

    enum class RockProviderFrameEnrichmentFlagV1 : std::uint32_t
    {
        None = 0,
        DeltaSecondsValid = 1u << 0,
        HmdTransformValid = 1u << 1,
        HmdForwardValid = 1u << 2,
        CoherentHandRoles = 1u << 3,
        StateSequenceValid = 1u << 4,
        CollisionGenerationValid = 1u << 5,
        EquippedTransitionSequenceValid = 1u << 6,
    };

    enum class RockProviderFrameStateChangeFlagV1 : std::uint32_t
    {
        None = 0,
        Lifecycle = 1u << 0,
        RightHand = 1u << 1,
        LeftHand = 1u << 2,
        Weapon = 1u << 3,
        EquippedTransition = 1u << 4,
        Collision = 1u << 5,
        HandRoles = 1u << 6,
    };

    [[nodiscard]] inline constexpr bool hasLifecycleFlag(std::uint32_t flags, RockProviderLifecycleFlag flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasConsumerCapabilityV1(std::uint32_t capabilities, RockProviderConsumerCapabilityV1 capability)
    {
        return (capabilities & static_cast<std::uint32_t>(capability)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasFeatureBitV1(std::uint32_t featureBits, RockProviderFeatureBitV1 feature)
    {
        return (featureBits & static_cast<std::uint32_t>(feature)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasHandInputSuppressionFlagV1(
        std::uint32_t flags,
        RockProviderHandInputSuppressionFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasWeaponPartTargetFlagV1(
        std::uint32_t flags,
        RockProviderWeaponPartTargetFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasWeaponPartInteractionZoneFlagV1(
        std::uint32_t flags,
        RockProviderWeaponPartInteractionZoneFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasLearnedWeaponPartActivationFlagV1(
        std::uint32_t flags,
        RockProviderLearnedWeaponPartActivationFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasWeaponKeywordFlagV1(std::uint64_t flags, RockProviderWeaponKeywordFlagV1 flag)
    {
        return (flags & static_cast<std::uint64_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasFeatureBit2V1(std::uint32_t featureBits, RockProviderFeatureBit2V1 feature)
    {
        return (featureBits & static_cast<std::uint32_t>(feature)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasTouchGrabTargetFlagV1(
        std::uint32_t flags,
        RockProviderTouchGrabTargetFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasNativeAnimationAuthorityFlagV1(
        std::uint32_t flags,
        RockProviderNativeAnimationAuthorityFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    [[nodiscard]] inline constexpr bool hasEquippedWeaponHandlingFlagV1(
        std::uint32_t flags,
        RockProviderEquippedWeaponHandlingFlagV1 flag)
    {
        return (flags & static_cast<std::uint32_t>(flag)) != 0;
    }

    struct RockProviderApi;

    /*
     * Immutable export-owned descriptor. Consumers read this before touching
     * any function-table slot, which makes minimum-extent negotiation safe
     * even when an older V1 provider returns a deliberately shorter table.
     */
    struct RockProviderApiDescriptorV1
    {
        std::uint32_t size{ sizeof(RockProviderApiDescriptorV1) };
        std::uint32_t apiVersion{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t tableByteSize{ 0 };
        std::uint32_t featureBits{ 0 };
        std::uint32_t featureBits2{ 0 };
        std::uint32_t reserved[3]{};
        const RockProviderApi* table{ nullptr };
    };
    static_assert(std::is_standard_layout_v<RockProviderApiDescriptorV1>);
    static_assert(offsetof(RockProviderApiDescriptorV1, size) == 0);
    static_assert(offsetof(RockProviderApiDescriptorV1, apiVersion) == 4);
    static_assert(offsetof(RockProviderApiDescriptorV1, tableByteSize) == 8);
    static_assert(offsetof(RockProviderApiDescriptorV1, featureBits) == 12);
    static_assert(offsetof(RockProviderApiDescriptorV1, featureBits2) == 16);
    static_assert(offsetof(RockProviderApiDescriptorV1, reserved) == 20);
    static_assert(offsetof(RockProviderApiDescriptorV1, table) == 32);
    static_assert(sizeof(RockProviderApiDescriptorV1) == 40);

    struct RockProviderConsumerRegistrationV1
    {
        std::uint32_t size{ sizeof(RockProviderConsumerRegistrationV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        char modName[64]{};
        std::uint32_t requestedCapabilities{ 0 };
        std::uint32_t reserved[7]{};
    };

    struct RockProviderConsumerHandleV1
    {
        std::uint32_t size{ sizeof(RockProviderConsumerHandleV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t ownerToken{ 0 };
        std::uint32_t grantedCapabilities{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[6]{};
    };

    struct RockProviderLimitsV1
    {
        std::uint32_t size{ sizeof(RockProviderLimitsV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t featureBits{ 0 };
        std::uint32_t maxFrameCallbacks{ 0 };
        std::uint32_t maxConsumers{ 0 };
        std::uint32_t maxExternalBodies{ 0 };
        std::uint32_t maxExternalContacts{ 0 };
        std::uint32_t maxBodyContacts{ 0 };
        std::uint32_t maxWeaponBodies{ 0 };
        std::uint32_t maxInteractionCommands{ 0 };
        std::uint32_t maxCompletedInteractionCommands{ 0 };
        std::uint32_t providerApiByteSize{ 0 };
        std::uint32_t maxWeaponEmitters{ 0 };
        std::uint32_t maxAnimationPhaseCallbacks{ 0 };
        std::uint32_t maxHandVisualAuthorityPublications{ 0 };
        std::uint32_t maxNativeAnimationRuntimeProviders{ 0 };
        std::uint32_t maxEquippedWeaponHandlingAuthorities{ 0 };
        std::uint32_t maxEquippedWeaponHandlingLeaseFrames{ 0 };
        std::uint32_t maxDebugOverlayPublishers{ 0 };
        std::uint32_t maxDebugOverlayLinesPerPublisher{ 0 };
        std::uint32_t maxDebugOverlayTextPerPublisher{ 0 };
        std::uint32_t maxDebugOverlayLines{ 0 };
        std::uint32_t maxDebugOverlayText{ 0 };
    };

    /*
     * Extensible limits surface. Callers set size to their local structure
     * size; ROCK prefix-copies the supported bytes and returns the copied size.
     */
    struct RockProviderLimitsExtV1
    {
        std::uint32_t size{ sizeof(RockProviderLimitsExtV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t featureBits{ 0 };
        std::uint32_t featureBits2{ 0 };
        std::uint32_t providerApiByteSize{ 0 };
        std::uint32_t maxConsumers{ 0 };
        std::uint32_t maxFrameCallbacks{ 0 };
        std::uint32_t maxExternalBodies{ 0 };
        std::uint32_t maxExternalScopes{ 0 };
        std::uint32_t maxExternalContacts{ 0 };
        std::uint32_t maxBodyContacts{ 0 };
        std::uint32_t maxWeaponBodies{ 0 };
        std::uint32_t maxWeaponEmitters{ 0 };
        std::uint32_t maxInteractionCommands{ 0 };
        std::uint32_t maxCompletedInteractionCommands{ 0 };
        std::uint32_t maxHandInputSuppressions{ 0 };
        std::uint32_t maxHandInputSuppressionLeaseFrames{ 0 };
        std::uint32_t maxWeaponPartTargets{ 0 };
        std::uint32_t maxWeaponPartDrives{ 0 };
        std::uint32_t maxWeaponPartDriveLeaseFrames{ 0 };
        std::uint32_t maxWeaponPartPoses{ 0 };
        std::uint32_t maxWeaponPartDriveResults{ 0 };
        std::uint32_t maxNativeAnimationAuthorityLeaseFrames{ 0 };
        std::uint32_t maxAnimationPhaseCallbacks{ 0 };
        std::uint32_t maxHandVisualAuthorityPublications{ 0 };
        std::uint32_t maxNativeAnimationRuntimeProviders{ 0 };
        std::uint32_t maxEquippedWeaponHandlingAuthorities{ 0 };
        std::uint32_t maxEquippedWeaponHandlingLeaseFrames{ 0 };
        std::uint32_t maxDebugOverlayPublishers{ 0 };
        std::uint32_t maxDebugOverlayLinesPerPublisher{ 0 };
        std::uint32_t maxDebugOverlayTextPerPublisher{ 0 };
        std::uint32_t maxDebugOverlayLines{ 0 };
        std::uint32_t maxDebugOverlayText{ 0 };
        std::uint32_t maxProviderEvents{ 0 };
        std::uint32_t maxWeaponCompositionEntries{ 0 };
        std::uint32_t maxSemanticHandContacts{ 0 };
        std::uint32_t maxPlayerColliderDescriptors{ 0 };
        std::uint32_t maxOffhandReservationLeaseFrames{ 0 };
        std::uint32_t maxHandVisualAuthorityLeaseFrames{ 0 };
        std::uint32_t maxNativeAnimationRuntimeLeaseFrames{ 0 };
        std::uint32_t maxDebugOverlayPublicationLeaseFrames{ 0 };
        std::uint32_t maxNativeAnimationAuthorityOwners{ 0 };
        std::uint32_t maxWeaponEvidenceDetails{ 0 };
        std::uint32_t maxWeaponEvidencePointsPerDetail{ 0 };
        std::uint32_t maxTouchGrabTargets{ 0 };
        std::uint32_t maxTouchGrabScopes{ 0 };
        std::uint32_t maxTouchGrabTargetLeaseFrames{ 0 };
        std::uint32_t reserved[1]{};
    };

    /*
     * Pointer-sized fields retained by the original V1 prefix are non-owning
     * identity witnesses, never ownership or mutation authority. They may be
     * compared only on ROCK's game-thread callback/query frame while the
     * accompanying frame and generation identities still match, and must not
     * be retained or dereferenced by a consumer. Command targetRefr inputs are
     * ABI-retained but ignored; use targetFormId and/or targetBodyId. Command
     * result targetRefr is zero. New V1 structures use value identity instead.
     */

    struct RockProviderForceGrabRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderForceGrabRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::uintptr_t targetRefr{ 0 };
        std::uint32_t targetFormId{ 0 };
        std::uint32_t targetBodyId{ 0x7FFF'FFFF };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        float maxDistanceGame{ 0.0f };
        float preferredGrabPointGame[3]{};
        std::uint32_t reserved[5]{};
    };

    struct RockProviderForceReleaseRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderForceReleaseRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::uintptr_t targetRefr{ 0 };
        std::uint32_t targetFormId{ 0 };
        std::uint32_t targetBodyId{ 0x7FFF'FFFF };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        float linearVelocityHavok[3]{};
        float angularVelocityRadiansPerSecond[3]{};
        std::uint32_t reserved[1]{};
    };

    struct RockProviderThrownDropRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderThrownDropRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::uintptr_t targetRefr{ 0 };
        std::uint32_t targetFormId{ 0 };
        std::uint32_t targetBodyId{ 0x7FFF'FFFF };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved0{ 0 };
        float linearVelocityHavok[3]{};
        float angularVelocityRadiansPerSecond[3]{};
        std::uint32_t reserved[6]{};
    };

    struct RockProviderInteractionCommandResultV1
    {
        std::uint32_t size{ sizeof(RockProviderInteractionCommandResultV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t ownerToken{ 0 };
        std::uint64_t commandId{ 0 };
        RockProviderInteractionCommandKindV1 kind{ RockProviderInteractionCommandKindV1::Unknown };
        RockProviderInteractionCommandStateV1 state{ RockProviderInteractionCommandStateV1::Unknown };
        RockProviderInteractionFailureV1 failure{ RockProviderInteractionFailureV1::None };
        RockProviderHand hand{ RockProviderHand::None };
        std::uintptr_t targetRefr{ 0 };
        std::uint32_t targetFormId{ 0 };
        std::uint32_t targetBodyId{ 0x7FFF'FFFF };
        std::uint64_t frameIndex{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        RockProviderCommandStageV1 stage{ RockProviderCommandStageV1::Unknown };
        RockProviderInteractionFailureV1 failureStage{ RockProviderInteractionFailureV1::None };
        std::uint64_t acceptedFrame{ 0 };
        std::uint64_t committedFrame{ 0 };
        std::uint64_t appliedFrame{ 0 };
        std::uint32_t reserved{ 0 };
    };

    struct RockProviderHandInputSuppressionRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderHandInputSuppressionRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[8]{};
    };

    /*
     * Raw OpenVR wand button state as sampled by ROCK's controller-state hook
     * (feature bit RawWandButtonState). Level state only: press/release edge
     * tracking is deliberately not exposed because ROCK consumes its edge
     * queues internally every frame (grab and trigger-equip logic); consumers
     * derive their own edges from held transitions. available reads 0 until
     * the hook has sampled that wand; held reads 0 while a game-stopping menu
     * owns input, with the same release-to-rearm gating ROCK applies to its
     * own gameplay reads. This state stays readable while ROCK suppresses the
     * matching native game action (e.g. the pipboy trigger) - that is the
     * point: the game action is silenced, the physical button is not.
     */
    struct RockProviderRawWandButtonStateV1
    {
        std::uint32_t size{ sizeof(RockProviderRawWandButtonStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t available{ 0 };
        std::uint32_t held{ 0 };
        std::uint64_t sampleSequence{ 0 };
        std::uint32_t sampleAgeMilliseconds{ 0 };
        RockProviderInputAvailabilityReasonV1 availabilityReason{
            RockProviderInputAvailabilityReasonV1::HookNotSampled
        };
    };

    struct RockProviderWeaponPartTargetV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartTargetV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        RockProviderWeaponPartGrabModeV1 grabMode{ RockProviderWeaponPartGrabModeV1::None };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uint32_t groupId{ 0 };
        std::uint32_t priority{ 0 };
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[8]{};
    };

    struct RockProviderTransform
    {
        float rotate[9]{};
        float translate[3]{};
        float scale{ 1.0f };

        /*
         * Legacy F4SE/Fallout 4 NiTransform adapter (.rot.data, .pos, .scale).
         * It deliberately remains header-only and templated so the provider
         * ABI does not depend on a consumer's engine headers or type layout:
         *
         *   drive.targetTransform = node->m_localTransform;
         */
        template <class LegacyNiTransform>
        RockProviderTransform& operator=(const LegacyNiTransform& source) noexcept
        {
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t column = 0; column < 3; ++column) {
                    rotate[row * 3 + column] = static_cast<float>(source.rot.data[row][column]);
                }
            }
            translate[0] = static_cast<float>(source.pos.x);
            translate[1] = static_cast<float>(source.pos.y);
            translate[2] = static_cast<float>(source.pos.z);
            scale = static_cast<float>(source.scale);
            return *this;
        }
    };

    struct RockProviderWeaponPartDriveTargetV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartDriveTargetV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        RockProviderWeaponPartDriveSpaceV1 driveSpace{ RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t groupId{ 0 };
        std::uint32_t priority{ 0 };
        // Finite provider frames, or ROCK_PROVIDER_WEAPON_PART_DRIVE_LEASE_UNTIL_CLEARED_V1.
        std::uint32_t leaseFrames{ 1 };
        RockProviderTransform targetTransform{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        /*
         * Optional NiAVObject to write after the matcher has selected the
         * collision source. It must be inside the current weapon tree. The
         * consumer explicitly defines its semantic association with the
         * selected body; no scene-graph ancestry is assumed. Zero preserves the
         * legacy behavior of writing the matched collision source itself.
         *
         * Feature bit: WeaponPartControlledRoot.
         */
        std::uintptr_t controlledRoot{ 0 };
        std::uint32_t reserved[4]{};
    };

    /*
     * Consumer-supplied motion constraint for a matched weapon part (matcher
     * fields/semantics identical to RockProviderWeaponPartTargetV1 - the same
     * flags, the same "first applicable wins by priority" resolution). Unlike
     * setWeaponPartTargetsV1 (which only says a part is grabbable) or
     * setWeaponPartDriveTargetsV1 (which drives an exact transform supplied by
     * the consumer, either temporarily or until explicitly cleared), this
     * lets the consumer describe HOW a
     * part should move ONCE and have ROCK project the grabbing hand's motion
     * onto that path every frame - no per-frame drive submission needed. Only
     * applies to AttachOnly grips (glue-only holds); FullTwoHandAuthority
     * grips already steer the whole weapon and have no single part path to
     * constrain against.
     */
    struct RockProviderWeaponPartMotionConstraintV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartMotionConstraintV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        RockProviderWeaponPartMotionKindV1 kind{ RockProviderWeaponPartMotionKindV1::None };
        RockProviderWeaponPartDriveSpaceV1 axisSpace{ RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uint32_t priority{ 0 };
        // Pivot for Rotational constraints, in axisSpace/game units. Linear
        // travel is relative to the part transform captured at grab time, so
        // axisOrigin is ignored for Linear constraints.
        float axisOrigin[3]{};
        // Unit vector in axisSpace: slide direction (Linear) or hinge axis (Rotational).
        float axisDirection[3]{};
        // Linear: game units relative to the at-grab transform. Rotational:
        // degrees swept around axisDirection through axisOrigin. Zero must be
        // inside [minValue, maxValue] so capture cannot move the part before
        // the hand itself moves.
        float minValue{ 0.0f };
        float maxValue{ 0.0f };
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        /*
         * Optional NiAVObject moved by the constraint. Matching still uses the
         * contacted collision source/body; this field only selects the related
         * authored node whose transform ROCK writes. See the identically named
         * drive-target field above.
         *
         * Feature bit: WeaponPartControlledRoot.
         */
        std::uintptr_t controlledRoot{ 0 };
        std::uint32_t reserved[4]{};
    };

    // Resolved against the same RockProviderWeaponPartTargetQueryV1 shape used by
    // resolveWeaponPartTargetV1 - a motion constraint is matched by the same
    // contact identity (bodyId/sourceRoot/sourceName/part-role fields) as a
    // weapon-part target, just against the separate constraint set.
    struct RockProviderWeaponPartMotionConstraintResolutionV1
    {
        std::uint32_t matched{ 0 };
        RockProviderWeaponPartMotionKindV1 kind{ RockProviderWeaponPartMotionKindV1::None };
        RockProviderWeaponPartDriveSpaceV1 axisSpace{ RockProviderWeaponPartDriveSpaceV1::WeaponRootLocal };
        float axisOrigin[3]{};
        float axisDirection[3]{};
        float minValue{ 0.0f };
        float maxValue{ 0.0f };
        std::uint64_t ownerToken{ 0 };
        std::uintptr_t controlledRoot{ 0 };
    };

    /*
     * Gameplay-friendly acquisition volume for one exact live weapon body.
     * Unlike a broad part-kind/proximity fallback, the pair
     * (weaponGenerationKey, bodyId) is mandatory. A hand inside the zone may
     * acquire only that body, and the normal target whitelist must resolve to
     * the same owner/group before capture is accepted.
     *
     * zoneCenter/zoneHalfExtents use zoneSpace. snapAnchor and handed
     * transforms are local to controlledRoot when supplied, otherwise the
     * exact evidence source root. For Sphere, zoneHalfExtents[0] is radius.
     */
    struct RockProviderWeaponPartInteractionZoneV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartInteractionZoneV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t groupId{ 0 };
        std::uint32_t priority{ 0 };
        std::uint32_t flags{ 0 };
        RockProviderWeaponPartInteractionZoneShapeV1 shape{
            RockProviderWeaponPartInteractionZoneShapeV1::Box
        };
        RockProviderWeaponPartInteractionZoneSpaceV1 zoneSpace{
            RockProviderWeaponPartInteractionZoneSpaceV1::ControlledRootLocal
        };
        RockProviderWeaponPartInteractionZoneSnapModeV1 snapMode{
            RockProviderWeaponPartInteractionZoneSnapModeV1::ClosestTargetMeshSurface
        };
        std::uintptr_t sourceRoot{ 0 };
        std::uintptr_t controlledRoot{ 0 };
        float zoneCenter[3]{};
        float zoneHalfExtents[3]{};
        float snapAnchor[3]{};
        RockProviderTransform rightHandPartLocal{};
        RockProviderTransform leftHandPartLocal{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[8]{};
    };

    /*
     * One provider-generated stable profile for the currently equipped weapon.
     * closed/open transforms and rail/hinge values are observed during a
     * vanilla reload boundary. syntheticDryFireTransform/value are explicitly
     * generated policy: Fallout does not author a persistent "dry fire" state.
     *
     * No persisted body ID or scene pointer appears here. current* fields are
     * live conveniences populated only while the matching weapon generation is
     * equipped; activation re-resolves them and fails closed if either identity
     * is unavailable or ambiguous.
     */
    struct RockProviderLearnedWeaponPartProfileV1
    {
        std::uint32_t size{ sizeof(RockProviderLearnedWeaponPartProfileV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t profileId{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint32_t partKind{ ROCK_PROVIDER_WEAPON_PART_KIND_ANY_V1 };
        std::uint32_t actionRole{ ROCK_PROVIDER_WEAPON_ACTION_ROLE_ANY_V1 };
        RockProviderWeaponPartMotionKindV1 motionKind{ RockProviderWeaponPartMotionKindV1::None };
        RockProviderWeaponPartDriveSpaceV1 axisSpace{
            RockProviderWeaponPartDriveSpaceV1::ControlledRootParentLocal
        };
        std::uint32_t flags{ 0 };
        RockProviderLearnedWeaponPartDryFireSourceV1 dryFireSource{
            RockProviderLearnedWeaponPartDryFireSourceV1::Unknown
        };
        float confidence{ 0.0f };
        float axisOrigin[3]{};
        float axisDirection[3]{};
        // Absolute profile coordinate relative to the learned closed pose.
        float minValue{ 0.0f };
        float maxValue{ 0.0f };
        float openValue{ 0.0f };
        float syntheticDryFireValue{ 0.0f };
        float generatedZoneCenter[3]{};
        float generatedZoneHalfExtents[3]{};
        float generatedSnapAnchor[3]{};
        RockProviderTransform closedTransform{};
        RockProviderTransform openTransform{};
        RockProviderTransform syntheticDryFireTransform{};
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint32_t captureFrameCount{ 0 };
        std::uint32_t captureSampleCount{ 0 };
        std::uint32_t currentMatchingBodyCount{ 0 };
        std::uint32_t reserved0{ 0 };
        std::uint64_t currentWeaponGenerationKey{ 0 };
        std::uintptr_t currentControlledRoot{ 0 };
        char weaponPlugin[ROCK_PROVIDER_MAX_PLUGIN_NAME_V1]{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        char controlledNodeName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        char controlledNodePath[ROCK_PROVIDER_MAX_WEAPON_NODE_PATH_V1]{};
        std::uint32_t reserved[8]{};
    };

    /*
     * High-level activation request. profileId is the unambiguous path. When it
     * is zero, ROCK filters current profiles by partKind/actionRole/sourceName
     * and returns ProfileAmbiguous instead of guessing if more than one remains.
     *
     * min/max constraints are rebuilt relative to whichever initial pose/value
     * is selected, so zero is always legal on the capture frame: squeezing grip
     * alone can never move the part. Interaction-zone overrides and snap data
     * are in the learned controlled-root local space.
     */
    struct RockProviderLearnedWeaponPartActivationRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderLearnedWeaponPartActivationRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t profileId{ 0 };
        std::uint32_t partKind{ ROCK_PROVIDER_WEAPON_PART_KIND_ANY_V1 };
        std::uint32_t actionRole{ ROCK_PROVIDER_WEAPON_ACTION_ROLE_ANY_V1 };
        RockProviderWeaponPartGrabModeV1 grabMode{ RockProviderWeaponPartGrabModeV1::AttachOnly };
        RockProviderLearnedWeaponPartProfilePoseV1 initialPose{
            RockProviderLearnedWeaponPartProfilePoseV1::SyntheticDryFire
        };
        std::uint32_t flags{
            static_cast<std::uint32_t>(RockProviderLearnedWeaponPartActivationFlagV1::Default)
        };
        std::uint32_t groupId{ 0 };
        std::uint32_t priority{ 100 };
        RockProviderWeaponPartInteractionZoneSnapModeV1 snapMode{
            RockProviderWeaponPartInteractionZoneSnapModeV1::ClosestTargetMeshSurface
        };
        float initialValueOverride{ 0.0f };
        float zoneCenterOverride[3]{};
        float zoneHalfExtentsOverride[3]{};
        float snapAnchorOverride[3]{};
        RockProviderTransform rightHandPartLocalOverride{};
        RockProviderTransform leftHandPartLocalOverride{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[8]{};
    };

    struct RockProviderLearnedWeaponPartActivationResultV1
    {
        std::uint32_t size{ sizeof(RockProviderLearnedWeaponPartActivationResultV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t profileId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint32_t partKind{ ROCK_PROVIDER_WEAPON_PART_KIND_ANY_V1 };
        std::uint32_t matchingBodyCount{ 0 };
        std::uint32_t installedTargetCount{ 0 };
        std::uint32_t installedConstraintCount{ 0 };
        std::uint32_t installedZoneCount{ 0 };
        std::uint32_t installedDriveCount{ 0 };
        RockProviderLearnedWeaponPartProfilePoseV1 initialPose{
            RockProviderLearnedWeaponPartProfilePoseV1::Current
        };
        float initialValue{ 0.0f };
        float constraintMinValue{ 0.0f };
        float constraintMaxValue{ 0.0f };
        std::uintptr_t controlledRoot{ 0 };
        std::uint32_t reserved[8]{};
    };

    enum class RockProviderWeaponPartGripLocalSpaceV1 : std::uint32_t
    {
        WeaponRootLocal = 0,
        PartSourceLocal = 1,
    };

    /*
     * Which signal classified a weapon part. NameToken is NIF-name matching
     * (author discretion, least trustworthy); SlotAnchor means the part sits
     * under a connect-point slot and carries record-authored identity
     * (attach point, owning OMOD); RigAnchor means an engine-animated rig
     * node (bolt, magazine display) supplied the function;
     * AttachmentEvidence means the installed OMOD or its discovered emitter
     * capabilities refined the physical module kind.
     */
    enum class RockProviderWeaponPartClassificationSourceV1 : std::uint32_t
    {
        NameToken = 0,
        SlotAnchor = 1,
        RigAnchor = 2,
        AttachmentEvidence = 3,
    };

    /*
     * Numeric contract for the partKind / actionRole payloads that already
     * flow through weapon evidence details, part targets, drive targets, and
     * grip states as raw uint32 values. Values mirror ROCK's internal
     * classification enums one-to-one (static_asserted inside ROCK, so drift
     * breaks ROCK's build, never a consumer at runtime). External consumers
     * use these to build part whitelists and gate grips without including
     * ROCK internals. Scope is reserved for an installed OMOD carrying
     * Fallout's native scope-overlay property; Sight covers every other optic,
     * including red-dot and holographic sights. LaserFlashlightCombo means one
     * physical module owns both emitter capabilities. MuzzleDevice covers the
     * dedicated muzzle attachment slot (suppressors, compensators, brakes, and
     * flash hiders). Bipod identifies an authored bipod component without
     * implying deployed/folded state.
     */
    enum class RockProviderWeaponPartKindV1 : std::uint32_t
    {
        Receiver = 0,
        Barrel = 1,
        Handguard = 2,
        Foregrip = 3,
        Pump = 4,
        Stock = 5,
        Grip = 6,
        Magazine = 7,
        Magwell = 8,
        Bolt = 9,
        Slide = 10,
        ChargingHandle = 11,
        BreakAction = 12,
        Cylinder = 13,
        Chamber = 14,
        Shell = 15,
        Round = 16,
        LaserCell = 17,
        Lever = 18,
        Sight = 19,
        Accessory = 20,
        CosmeticAmmo = 21,
        Other = 22,
        LaserSight = 23,
        Flashlight = 24,
        LaserFlashlightCombo = 25,
        Scope = 26,
        MuzzleDevice = 27,
        Bipod = 28,
    };

    enum class RockProviderWeaponActionRoleV1 : std::uint32_t
    {
        None = 0,
        Bolt = 1,
        Slide = 2,
        ChargingHandle = 3,
        Pump = 4,
        BreakAction = 5,
        Cylinder = 6,
        Lever = 7,
        Latch = 8,
    };

    /*
     * Per-hand grip report: which weapon part (if any) the hand is attached
     * to this frame. Polled; gripSequence increases on every fresh capture so
     * consumers detect re-grabs without frame callbacks. sourceRoot is a
     * non-owning engine pointer valid only while weaponGenerationKey matches
     * the frame snapshot. handPartLocal is the hand frame captured at grip
     * start in handPartLocalSpace; composing it with the part's current world
     * transform yields the glued hand target, so a consumer driving the part
     * via setWeaponPartDriveTargetsV1 can also derive controller-to-part
     * displacement from it.
     */
    struct RockProviderWeaponPartGripStateV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartGripStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        RockProviderWeaponPartGripKindV1 gripKind{ RockProviderWeaponPartGripKindV1::None };
        std::uint32_t active{ 0 };
        std::uint32_t attachOnly{ 0 };
        std::uint64_t gripSequence{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        std::uint64_t providerOwnerToken{ 0 };
        std::uint32_t providerGroupId{ 0 };
        std::uint32_t providerGrabMode{ 0 };
        std::uint32_t hasHandPartLocal{ 0 };
        RockProviderWeaponPartGripLocalSpaceV1 handPartLocalSpace{ RockProviderWeaponPartGripLocalSpaceV1::WeaponRootLocal };
        RockProviderTransform handPartLocal{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        // Record-authored identity of the gripped part; see the evidence
        // detail struct for field semantics (WeaponPartRecordIdentity bit).
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint32_t classificationSource{ 0 };
        std::uint32_t reserved[6]{};
    };

    /*
     * bhkWorld, hknpWorld, and weaponNode are legacy V1 witnesses governed by
     * the pointer rule above. All other appended enrichment is copied value
     * state and remains interpretable after the callback returns.
     */
    struct RockProviderFrameSnapshot
    {
        std::uint32_t size{ sizeof(RockProviderFrameSnapshot) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        std::uintptr_t bhkWorld{ 0 };
        std::uintptr_t hknpWorld{ 0 };
        std::uint32_t frikSkeletonReady{ 0 };
        std::uint32_t menuBlocking{ 0 };
        std::uint32_t configBlocking{ 0 };
        std::uint32_t providerReady{ 0 };
        std::uintptr_t weaponNode{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint32_t weaponBodyCount{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        RockProviderTransform rightHandTransform{};
        RockProviderTransform leftHandTransform{};
        std::uint32_t rightHandBodyId{ 0x7FFF'FFFF };
        std::uint32_t leftHandBodyId{ 0x7FFF'FFFF };
        std::uint32_t weaponBodyIds[ROCK_PROVIDER_MAX_WEAPON_BODIES]{};
        std::uint32_t rightHandState{ 0 };
        std::uint32_t leftHandState{ 0 };
        RockProviderOffhandReservation offhandReservation{ RockProviderOffhandReservation::Normal };
        std::uint32_t externalBodyCount{ 0 };
        float gameToHavokScale{ 0.0f };
        float havokToGameScale{ 0.0f };
        std::uint32_t physicsScaleRevision{ 0 };
        std::uint32_t lifecycleFlags{ 0 };
        RockProviderLifecycleReason lastLifecycleReason{ RockProviderLifecycleReason::None };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t stableFrameCount{ 0 };
        float deltaSeconds{ 0.0f };
        std::uint32_t enrichmentFlags{ 0 };
        RockProviderTransform hmdTransform{};
        float hmdForwardWorld[3]{};
        RockProviderHand primaryHand{ RockProviderHand::Right };
        RockProviderHand offhandHand{ RockProviderHand::Left };
        std::uint64_t stateSequence{ 0 };
        std::uint32_t stateChangeMask{ 0 };
        std::uint32_t collisionGeneration{ 0 };
        std::uint64_t equippedWeaponTransitionSequence{ 0 };
    };

    struct RockProviderHandFrameV1
    {
        std::uint32_t size{ sizeof(RockProviderHandFrameV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::uintptr_t node{ 0 };
        RockProviderTransform transform{};
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t state{ 0 };
        std::uint32_t reserved[7]{};
        std::uint64_t frameIndex{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t collisionGeneration{ 0 };
        std::uint64_t stateSequence{ 0 };
    };

    struct RockProviderWeaponContactQuery
    {
        std::uint32_t size{ sizeof(RockProviderWeaponContactQuery) };
        float pointGame[3]{};
        float radiusGame{ 0.0f };
        std::uint32_t flags{ 0 };
        std::uint32_t reserved[2]{};
    };

    struct RockProviderWeaponContactResult
    {
        std::uint32_t size{ sizeof(RockProviderWeaponContactResult) };
        std::uint32_t valid{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uintptr_t interactionRoot{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        float probeDistanceGame{ 0.0f };
        std::uint32_t reserved{ 0 };
    };

    /*
     * Weapon size class plus the raw keyword bitmask it was (or wasn't) derived
     * from. Consumers that only need the collider-style bucket can read
     * sizeClass directly; consumers that need finer distinctions (e.g. a future
     * reload/scope mod picking a shotgun- or minigun-specific behavior) can
     * inspect keywordFlags with hasWeaponKeywordFlagV1.
     */
    struct RockProviderWeaponClassificationV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponClassificationV1) };
        std::uint32_t valid{ 0 };
        std::uint64_t keywordFlags{ 0 };
        RockProviderWeaponSizeClassV1 sizeClass{ RockProviderWeaponSizeClassV1::Rifle };
        RockProviderWeaponClassificationSourceV1 source{ RockProviderWeaponClassificationSourceV1::None };
        std::uint32_t formId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        float confidence{ 0.0f };
        std::uint32_t provenanceFlags{ 0 };
    };

    struct RockProviderPoint3
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    struct RockProviderBounds3
    {
        RockProviderPoint3 min{};
        RockProviderPoint3 max{};
        std::uint32_t valid{ 0 };
        std::uint32_t reserved{ 0 };
    };

    /*
     * One value snapshot of a weapon-mounted visual emitter. The transform and
     * forward vector are expressed in the equipped weapon root's local game-
     * unit space and contain no retained engine object. Active follows the
     * effect geometry's effective scene visibility; Visible follows the node
     * that supplied the transform. EffectStateKnown distinguishes an inactive
     * effect from an AddOnNode marker for which no live effect was found.
     */
    struct RockProviderWeaponEmitterV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponEmitterV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderWeaponEmitterKindV1 kind{ RockProviderWeaponEmitterKindV1::Unknown };
        RockProviderWeaponEmitterSourceV1 source{ RockProviderWeaponEmitterSourceV1::Unknown };
        std::uint32_t flags{ 0 };
        std::uint32_t active{ 0 };
        std::uint32_t visible{ 0 };
        std::uint32_t addOnNodeValue{ 0 };
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        RockProviderTransform weaponLocalTransform{};
        RockProviderPoint3 forwardWeaponLocal{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[8]{};
    };

    /*
     * Authority is always a rolling bounded lease. leaseFrames must be nonzero,
     * is bounded by
     * ROCK_PROVIDER_MAX_NATIVE_ANIMATION_AUTHORITY_LEASE_FRAMES_V1 and should
     * be refreshed by a consumer that wants rolling temporary authority.
     * Generation guards follow the same optional-zero contract as the other
     * V1 request surfaces.
     */
    struct RockProviderNativeAnimationAuthorityRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderNativeAnimationAuthorityRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[9]{};
    };

    struct RockProviderNativeAnimationAuthorityStateV1
    {
        std::uint32_t size{ sizeof(RockProviderNativeAnimationAuthorityStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t activeFlags{ 0 };
        std::uint32_t statusFlags{ 0 };
        std::uint32_t activeOwnerCount{ 0 };
        std::uint32_t capturedTransformCount{ 0 };
        std::uint64_t captureSequence{ 0 };
        std::uint32_t reserved[8]{};
    };

    /*
     * Owner-bound main-thread animation phases expose stable capture and
     * presentation boundaries without exposing ROCK's native detours.
     * NativeGraphOutput runs at the byte-validated player graph-output entry,
     * before downstream native scene and hFRIK presentation writers; callbacks
     * at that phase may capture data but must not mutate the engine graph.
     * BeforeRock runs before ROCK mutates weapon/hand presentation, AfterRock
     * runs after the interaction update, and Complete closes the frame after
     * all visual writers. Unregister prevents future dispatch copies but does
     * not wait for an already copied invocation; callback and userData storage
     * must remain alive through that invocation. Faulting callbacks revoke all
     * stateful resources and callbacks owned by that consumer.
     */
    struct RockProviderAnimationPhaseContextV1
    {
        std::uint32_t size{ sizeof(RockProviderAnimationPhaseContextV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderAnimationPhaseV1 phase{ RockProviderAnimationPhaseV1::BeforeRock };
        std::uint32_t flags{ 0 };
        std::uint64_t frameIndex{ 0 };
        float deltaSeconds{ 0.0f };
        std::uint32_t activeNativeAnimationAuthorityFlags{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[7]{};
    };

    /*
     * Value-only snapshot of ROCK's equipped-weapon grip solution. Query only
     * from ROCK's animation/frame callbacks on the game thread; wrong-thread
     * reads fail closed. Scene pointers are identity witnesses for the current
     * frame and must never be retained. Hand transforms are exact ROCK targets
     * in Weapon local space.
     */
    struct RockProviderEquippedWeaponGripStateV1
    {
        std::uint32_t size{ sizeof(RockProviderEquippedWeaponGripStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uintptr_t weaponNode{ 0 };
        RockProviderTransform weaponWorld{};
        RockProviderTransform rightHandInWeapon{};
        RockProviderTransform leftHandInWeapon{};
        std::uint32_t reserved[8]{};
    };

    /*
     * leaseFrames must be non-zero and is clamped to the public maximum.
     * A rolling lease fails closed to ROCK's fixed configured firing hand if
     * the addon stops publishing, unregisters, faults, or loses the provider.
     * Generation guards use the established optional-zero V1 contract.
     */
    struct RockProviderEquippedWeaponHandlingRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderEquippedWeaponHandlingRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        std::uint32_t leaseFrames{ 0 };
        float gripZoneEquipRadiusGameUnits{ 3.0f };
        float gripZoneEquipSettleSeconds{ 0.15f };
        float firingGripReattachRadiusGameUnits{ 3.0f };
        float gripZoneHoverHapticIntensity{ 0.75f };
        float firingGripProximitySupportRadiusGameUnits{ 6.0f };
        float weaponGripHapticDurationSeconds{ 0.10f };
        float firingGripAttachHapticIntensity{ 0.85f };
        float firingGripDetachHapticIntensity{ 0.30f };
        float supportGripHapticIntensity{ 0.50f };
        float firingGripPromotionRadiusGameUnits{ 5.0f };
        float leftFiringAimYawDegrees{ 0.0f };
        float leftFiringAimPitchDegrees{ 0.0f };
        float leftFiringAimOffsetGameUnits[3]{};
        float equipVisualBridgeTimeoutSeconds{ 2.0f };
        float equipVisualBridgeBlendSeconds{ 0.15f };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[8]{};
    };

    struct RockProviderEquippedWeaponHandlingStateV1
    {
        std::uint32_t size{ sizeof(RockProviderEquippedWeaponHandlingStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t authorityFlags{ 0 };
        std::uint32_t runtimeFlags{ 0 };
        std::uint64_t ownerToken{ 0 };
        std::uint64_t expiresAfterFrame{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t weaponFormId{ 0 };
        RockProviderHand fixedFiringHand{ RockProviderHand::Right };
        RockProviderHand currentFiringHand{ RockProviderHand::Right };
        std::uint32_t reserved[9]{};
    };

    /*
     * A consumer publishes one hand world target and/or an exact 15-bone
     * finger-local pose through ROCK's FRIK authority bridge. Set/clear only
     * from ROCK's animation/frame callbacks on the game thread; wrong-thread
     * writes are rejected. ROCK derives a unique tag from ownerToken. Every
     * publication is a rolling bounded lease with generation guards and is
     * cleared on expiry, generation change, explicit clear, consumer
     * unregister, provider loss, or callback fault.
     */
    struct RockProviderHandVisualAuthorityRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderHandVisualAuthorityRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::int32_t priority{ 0 };
        std::uint16_t fingerLocalTransformMask{ 0 };
        std::uint16_t reserved0{ 0 };
        RockProviderTransform worldTransform{};
        RockProviderTransform fingerLocalTransforms[15]{};
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[4]{};
    };

    /*
     * The addon that actually executes native animation authority publishes
     * capture health here. ROCK remains the V1 lease coordinator and folds
     * this status into getNativeAnimationAuthorityStateV1.
     */
    struct RockProviderNativeAnimationRuntimePublicationV1
    {
        std::uint32_t size{ sizeof(RockProviderNativeAnimationRuntimePublicationV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t statusFlags{ 0 };
        std::uint32_t capturedTransformCount{ 0 };
        std::uint64_t captureSequence{ 0 };
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[6]{};
    };

    /*
     * Diagnostic-only colored geometry submitted to ROCK's single OpenVR/D3D
     * overlay renderer. Consumer memory is copied during publish and is never
     * retained. Publications are owner-scoped, bounded, game-thread-only, and
     * retained. Every publication is a rolling bounded lease and is cleared on
     * expiry, generation change, explicit clear, unregister, callback fault,
     * or provider loss.
     */
    struct RockProviderDebugOverlayLineV1
    {
        std::uint32_t size{ sizeof(RockProviderDebugOverlayLineV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        float startGame[3]{};
        float endGame[3]{};
        float color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        std::uint32_t reserved[2]{};
    };

    struct RockProviderDebugOverlayTextV1
    {
        std::uint32_t size{ sizeof(RockProviderDebugOverlayTextV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t flags{ 0 };
        std::uint32_t reserved0{ 0 };
        char text[ROCK_PROVIDER_DEBUG_OVERLAY_TEXT_CAPACITY_V1]{};
        float x{ 18.0f };
        float y{ 18.0f };
        float textSize{ 2.0f };
        float color[4]{ 0.90f, 1.0f, 0.95f, 0.92f };
        float worldAnchorGame[3]{};
        std::uint32_t reserved[4]{};
    };

    struct RockProviderDebugOverlayPublicationV1
    {
        std::uint32_t size{ sizeof(RockProviderDebugOverlayPublicationV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t lineCount{ 0 };
        std::uint32_t textCount{ 0 };
        const RockProviderDebugOverlayLineV1* lines{ nullptr };
        const RockProviderDebugOverlayTextV1* textEntries{ nullptr };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t reserved[4]{};
    };

    /*
     * Detailed weapon evidence carries semantic body identity, local generated
     * bounds, and total point count without making the fixed function table own
     * variable-length buffers. Callers fetch the local mesh point cloud through
     * the body-id keyed copy function below.
     */
    struct RockProviderWeaponEvidenceDetailV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponEvidenceDetailV1) };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uint32_t fallbackGripPose{ 0 };
        std::uintptr_t interactionRoot{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        RockProviderBounds3 localBoundsGame{};
        std::uint32_t pointCount{ 0 };
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        /*
         * Record-authored identity (feature bit WeaponPartRecordIdentity;
         * fields were reserved zeros before it). omodFormId is the installed
         * OMOD occupying this part's slot (0 when unpaired); attachPointFormId
         * is the vanilla attach-point keyword of that slot; classification-
         * Source is RockProviderWeaponPartClassificationSourceV1.
         */
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint32_t classificationSource{ 0 };
        std::uint32_t reserved[6]{};
    };

    struct RockProviderBodyContactV1
    {
        std::uint32_t size{ sizeof(RockProviderBodyContactV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t targetBodyId{ 0x7FFF'FFFF };
        std::uint32_t bodyLayer{ 0xFFFF'FFFF };
        std::uint32_t targetLayer{ 0xFFFF'FFFF };
        RockProviderBodyZoneKind zone{ RockProviderBodyZoneKind::Unknown };
        RockProviderBodyZoneSide side{ RockProviderBodyZoneSide::Center };
        std::uint32_t role{ 0 };
        std::uint32_t descriptorIndex{ 0 };
        RockProviderBodyContactTargetKind targetKind{ RockProviderBodyContactTargetKind::Unknown };
        RockProviderBodyZoneKind targetZone{ RockProviderBodyZoneKind::Unknown };
        RockProviderBodyZoneSide targetSide{ RockProviderBodyZoneSide::Center };
        std::uint32_t targetRole{ 0 };
        std::uint32_t targetDescriptorIndex{ 0 };
        std::uint32_t inPowerArmor{ 0 };
        std::uint32_t targetInPowerArmor{ 0 };
        std::uint32_t hasContactPointGame{ 0 };
        std::uint32_t reserved0{ 0 };
        RockProviderPoint3 contactPointGame{};
        std::uint32_t reserved[8]{};
    };

    struct RockProviderExternalBodyRegistration
    {
        std::uint32_t size{ sizeof(RockProviderExternalBodyRegistration) };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint64_t ownerToken{ 0 };
        std::uint32_t generation{ 0 };
        RockProviderExternalBodyRole role{ RockProviderExternalBodyRole::Unknown };
        RockProviderExternalBodyContactPolicy contactPolicy{ RockProviderExternalBodyContactPolicy::None };
        RockProviderHand ownerHand{ RockProviderHand::None };
    };

    struct RockProviderExternalContactV1
    {
        std::uint32_t size{ sizeof(RockProviderExternalContactV1) };
        std::uint32_t sourceBodyId{ 0x7FFF'FFFF };
        std::uint32_t targetExternalBodyId{ 0x7FFF'FFFF };
        std::uint32_t generation{ 0 };
        std::uint64_t ownerToken{ 0 };
        std::uint64_t sequence{ 0 };
        std::uint64_t frameIndex{ 0 };
        RockProviderExternalSourceKind sourceKind{ RockProviderExternalSourceKind::Unknown };
        RockProviderHand sourceHand{ RockProviderHand::None };
        RockProviderExternalBodyRole targetRole{ RockProviderExternalBodyRole::Unknown };
        RockProviderExternalContactQuality quality{ RockProviderExternalContactQuality::BodyPairOnly };
        float sourceVelocityHavok[4]{};
        float contactPointHavok[4]{};
        float contactNormalHavok[4]{};
        // Sum of Bethesda contact point weights at contact-signal +0x30; this is not an impulse magnitude.
        union
        {
            float contactPointWeightSum{ 0.0f };
            float aggregateImpulseMagnitude;
        };
        std::uint32_t sourcePartKind{ 0 };
        std::uint32_t sourceRole{ 0 };
        std::uint32_t sourceSubRole{ 0 };
        std::uint32_t flags{ 0 };
        std::uint32_t collisionGeneration{ 0 };
    };

    struct RockProviderHandInteractionStateV1
    {
        std::uint32_t size{ sizeof(RockProviderHandInteractionStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        RockProviderHand hand{ RockProviderHand::None };
        RockProviderHandInteractionPhaseV1 phase{ RockProviderHandInteractionPhaseV1::Idle };
        RockProviderBodyContactTargetKind targetKind{ RockProviderBodyContactTargetKind::Unknown };
        std::uint32_t flags{ 0 };
        std::uint64_t reservedTargetIdentity{ 0 };
        std::uint32_t targetFormId{ 0 };
        std::uint32_t primaryBodyId{ 0x7FFF'FFFF };
        std::uint32_t heldBodyCount{ 0 };
        std::uint32_t heldBodyIds[ROCK_PROVIDER_MAX_HAND_HELD_BODIES_V1]{};
        std::uint32_t effectiveInputSuppressionFlags{ 0 };
        std::uint32_t collisionAvailabilityFlags{ 0 };
        std::uint64_t stateSequence{ 0 };
        std::uint64_t targetSequence{ 0 };
        std::uint64_t gripSequence{ 0 };
        std::uint64_t releaseSequence{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t collisionGeneration{ 0 };
        std::uint32_t reserved[4]{};
    };

    struct RockProviderEventV1
    {
        std::uint32_t size{ sizeof(RockProviderEventV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t sequence{ 0 };
        std::uint64_t frameIndex{ 0 };
        RockProviderEventKindV1 kind{ RockProviderEventKindV1::Unknown };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint64_t ownerToken{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t formId{ 0 };
        std::uint32_t result{ 0 };
        std::uint64_t subjectSequence{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t data[5]{};
    };

    struct RockProviderEventStreamStateV1
    {
        std::uint32_t size{ sizeof(RockProviderEventStreamStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t oldestRetainedSequence{ 0 };
        std::uint64_t latestEmittedSequence{ 0 };
        std::uint64_t firstCopiedSequence{ 0 };
        std::uint64_t lastCopiedSequence{ 0 };
        std::uint32_t copiedCount{ 0 };
        std::uint32_t flags{ 0 };
        std::uint64_t overwrittenCount{ 0 };
    };

    struct RockProviderEquippedWeaponStateV1
    {
        std::uint32_t size{ sizeof(RockProviderEquippedWeaponStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        std::uint32_t flags{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint64_t transitionSequence{ 0 };
        std::uint64_t terminalSequence{ 0 };
        RockProviderEquippedWeaponTransitionSourceV1 transitionSource{
            RockProviderEquippedWeaponTransitionSourceV1::Unknown
        };
        RockProviderEquippedWeaponTransitionResultV1 terminalResult{
            RockProviderEquippedWeaponTransitionResultV1::None
        };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[7]{};
    };

    struct RockProviderExternalContactRecordV1
    {
        std::uint32_t size{ sizeof(RockProviderExternalContactRecordV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t parentOwnerToken{ 0 };
        std::uint64_t scopeToken{ 0 };
        std::uint64_t sequence{ 0 };
        std::uint64_t frameIndex{ 0 };
        std::uint32_t sourceBodyId{ 0x7FFF'FFFF };
        std::uint32_t targetExternalBodyId{ 0x7FFF'FFFF };
        std::uint32_t bodyGeneration{ 0 };
        RockProviderExternalSourceKind sourceKind{ RockProviderExternalSourceKind::Unknown };
        RockProviderHand sourceHand{ RockProviderHand::None };
        RockProviderExternalBodyRole targetRole{ RockProviderExternalBodyRole::Unknown };
        RockProviderExternalContactQuality quality{ RockProviderExternalContactQuality::BodyPairOnly };
        std::uint32_t flags{ 0 };
        float sourceVelocityHavok[3]{};
        float contactPointHavok[3]{};
        float contactNormalHavok[3]{};
        float contactPointWeightSum{ 0.0f };
        std::uint32_t sourcePartKind{ 0 };
        std::uint32_t sourceRole{ 0 };
        std::uint32_t sourceSubRole{ 0 };
        std::uint32_t collisionGeneration{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[3]{};
    };

    struct RockProviderExternalContactStreamStateV1
    {
        std::uint32_t size{ sizeof(RockProviderExternalContactStreamStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t oldestRetainedSequence{ 0 };
        std::uint64_t latestEmittedSequence{ 0 };
        std::uint64_t firstCopiedSequence{ 0 };
        std::uint64_t lastCopiedSequence{ 0 };
        std::uint64_t overwrittenCount{ 0 };
        std::uint32_t copiedCount{ 0 };
        std::uint32_t flags{ 0 };
    };

    struct RockProviderWeaponPartResolutionQueryV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartResolutionQueryV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t reloadRole{ 0 };
        std::uint32_t supportRole{ 0 };
        std::uint32_t socketRole{ 0 };
        std::uint32_t actionRole{ 0 };
        std::uintptr_t sourceRoot{ 0 };
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[4]{};
    };

    struct RockProviderWeaponPartResolutionResultV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartResolutionResultV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t whitelistActive{ 0 };
        std::uint32_t matched{ 0 };
        RockProviderWeaponPartGrabModeV1 grabMode{ RockProviderWeaponPartGrabModeV1::None };
        std::uint32_t groupId{ 0 };
        std::uint32_t priority{ 0 };
        std::uint32_t reserved0{ 0 };
        std::uint64_t winningOwnerToken{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint64_t frameIndex{ 0 };
        std::uint32_t reserved[4]{};
    };

    struct RockProviderWeaponPartPoseV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartPoseV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t partKind{ 0 };
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint32_t flags{ 0 };
        std::uint32_t reserved0{ 0 };
        RockProviderTransform sourceParentLocal{};
        RockProviderTransform weaponRootLocal{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[4]{};
    };

    struct RockProviderWeaponPartDriveApplicationResultV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponPartDriveApplicationResultV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        std::uint64_t ownerToken{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t groupId{ 0 };
        std::uint32_t priority{ 0 };
        RockProviderWeaponPartDriveApplicationV1 result{
            RockProviderWeaponPartDriveApplicationV1::Unknown
        };
        RockProviderTransform appliedSourceParentLocal{};
        char sourceName[ROCK_PROVIDER_MAX_EVIDENCE_NAME]{};
        std::uint32_t reserved[4]{};
    };

    struct RockProviderScopeSightStateV1
    {
        std::uint32_t size{ sizeof(RockProviderScopeSightStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        std::uint64_t publicationSequence{ 0 };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint32_t flags{ 0 };
        RockProviderScopeActivationSourceV1 activationSource{
            RockProviderScopeActivationSourceV1::None
        };
        std::uint32_t nativeScopeOverlayIndex{ 0 };
        RockProviderPoint3 anchorWeaponLocal{};
        RockProviderBounds3 sightBoundsWeaponLocal{};
        std::uint32_t sightBodyCount{ 0 };
        std::uint32_t sightBodyId{ 0x7FFF'FFFF };
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[5]{};
    };

    struct RockProviderWeaponCompositionStateV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponCompositionStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint64_t compositionSignature{ 0 };
        std::uint32_t weaponFormId{ 0 };
        std::uint32_t entryCount{ 0 };
        std::uint64_t semanticCoverageMask{ 0 };
        std::uint64_t missingCoverageMask{ 0 };
        std::uint64_t publicationSequence{ 0 };
        std::uint32_t reserved[6]{};
    };

    struct RockProviderWeaponCompositionEntryV1
    {
        std::uint32_t size{ sizeof(RockProviderWeaponCompositionEntryV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint32_t omodFormId{ 0 };
        std::uint32_t attachPointFormId{ 0 };
        std::uint32_t stableIndex{ 0 };
        std::uint32_t flags{ 0 };
        std::uint64_t semanticCoverageMask{ 0 };
        std::uint32_t reserved[6]{};
    };

    struct RockProviderAuthoredGripPoseV1
    {
        std::uint32_t size{ sizeof(RockProviderAuthoredGripPoseV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t weaponGenerationKey{ 0 };
        std::uint32_t weaponFormId{ 0 };
        RockProviderAuthoredGripSourceV1 source{ RockProviderAuthoredGripSourceV1::Unknown };
        std::uint64_t variantKey{ 0 };
        std::uint64_t captureSequence{ 0 };
        std::uint32_t flags{ 0 };
        std::uint16_t rightFingerLocalTransformMask{ 0 };
        std::uint16_t leftFingerLocalTransformMask{ 0 };
        RockProviderTransform rightHandInWeapon{};
        RockProviderTransform leftHandInWeapon{};
        RockProviderTransform rightFingerLocalTransforms[15]{};
        RockProviderTransform leftFingerLocalTransforms[15]{};
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[5]{};
    };

    struct RockProviderPresentedHandPoseV1
    {
        std::uint32_t size{ sizeof(RockProviderPresentedHandPoseV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        RockProviderTransform handWorld{};
        std::uint16_t fingerLocalTransformMask{ 0 };
        std::uint16_t reserved0{ 0 };
        RockProviderTransform fingerLocalTransforms[15]{};
        std::uint64_t presentationSequence{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[5]{};
    };

    struct RockProviderSemanticHandContactV1
    {
        std::uint32_t size{ sizeof(RockProviderSemanticHandContactV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t role{ 0 };
        std::uint32_t finger{ 0 };
        std::uint32_t segment{ 0 };
        std::uint32_t handBodyId{ 0x7FFF'FFFF };
        std::uint32_t targetBodyId{ 0x7FFF'FFFF };
        std::uint32_t targetFormId{ 0 };
        std::uint32_t flags{ 0 };
        RockProviderSemanticContactStateV1 contactState{
            RockProviderSemanticContactStateV1::Continued
        };
        std::uint32_t framesSinceContact{ 0 };
        std::uint32_t contactSequence{ 0 };
        RockProviderPoint3 contactPointGame{};
        RockProviderPoint3 contactNormalGame{};
        std::uint32_t collisionGeneration{ 0 };
        std::uint32_t reserved[5]{};
    };

    struct RockProviderPlayerColliderDescriptorV1
    {
        std::uint32_t size{ sizeof(RockProviderPlayerColliderDescriptorV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        RockProviderPlayerColliderKindV1 kind{ RockProviderPlayerColliderKindV1::Hand };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t role{ 0 };
        RockProviderBodyZoneKind zone{ RockProviderBodyZoneKind::Unknown };
        RockProviderBodyZoneSide side{ RockProviderBodyZoneSide::Center };
        std::uint32_t descriptorIndex{ 0 };
        std::uint32_t flags{ 0 };
        float lengthGameUnits{ 0.0f };
        float radiusGameUnits{ 0.0f };
        RockProviderTransform transform{};
        std::uint32_t collisionGeneration{ 0 };
        std::uint32_t reserved[5]{};
    };

    struct RockProviderHandCollisionAvailabilityV1
    {
        std::uint32_t size{ sizeof(RockProviderHandCollisionAvailabilityV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t flags{ 0 };
        std::uint64_t collisionSequence{ 0 };
        std::uint32_t collisionGeneration{ 0 };
        std::uint32_t handBodyCount{ 0 };
        std::uint32_t dynamicTwinCount{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[6]{};
    };

    struct RockProviderHandInputSuppressionStateV1
    {
        std::uint32_t size{ sizeof(RockProviderHandInputSuppressionStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t frameIndex{ 0 };
        RockProviderHand hand{ RockProviderHand::None };
        std::uint32_t callerFlags{ 0 };
        std::uint32_t effectiveFlags{ 0 };
        std::uint32_t callerLeaseActive{ 0 };
        std::uint64_t callerExpiresAfterFrame{ 0 };
        std::uint32_t callerRemainingFrames{ 0 };
        RockProviderSuppressionInvalidationReasonV1 lastInvalidationReason{
            RockProviderSuppressionInvalidationReasonV1::None
        };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[5]{};
    };

    struct RockProviderOffhandReservationRequestV1
    {
        std::uint32_t size{ sizeof(RockProviderOffhandReservationRequestV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderOffhandReservation reservation{ RockProviderOffhandReservation::Normal };
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[7]{};
    };

    struct RockProviderOffhandReservationStateV1
    {
        std::uint32_t size{ sizeof(RockProviderOffhandReservationStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        RockProviderOffhandReservation reservation{ RockProviderOffhandReservation::Normal };
        std::uint32_t active{ 0 };
        std::uint64_t ownerToken{ 0 };
        std::uint64_t expiresAfterFrame{ 0 };
        std::uint32_t remainingFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t reserved[6]{};
    };

    /*
     * Publications are copied into ROCK's bounded registry and replace one
     * owner/scope transactionally. Every target must carry the current,
     * nonzero world/skeleton/provider generations and a nonzero owner-defined
     * targetGeneration. Bump targetGeneration whenever the resolved body or
     * any behavioral geometry/policy changes; refresh the unchanged value only
     * to renew its lease.
     *
     * LimitedHinge coordinates and limits are radians. LimitedPrismatic
     * coordinates and limits are game units. Their pivot and normalized axis
     * are in current world/game space. ROCK temporarily converts a keyframed
     * mechanism body to dynamic while held, owns all constraints, and restores
     * the original motion class before publishing Latched/Yielded/Invalidated.
     *
     * FixedAnchor either names one body or uses MatchAnyBody plus a nonzero
     * allowedLayerMask. It records hand/contact ownership only: ROCK never
     * changes, activates, constrains, or writes velocity to the matched body.
     * Exact body registrations are resolved before wildcard registrations.
     * One wildcard descriptor owns at most one resolved body concurrently;
     * publish disjoint right/left wildcard descriptors when a consumer needs
     * two independent surfaces at once (for example, two-hand climbing).
     */
    struct RockProviderTouchGrabTargetV1
    {
        std::uint32_t size{ sizeof(RockProviderTouchGrabTargetV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t targetId{ 0 };
        std::uint32_t targetGeneration{ 0 };
        RockProviderTouchGrabKindV1 kind{
            RockProviderTouchGrabKindV1::FixedAnchor
        };
        std::uint32_t flags{ 0 };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t referenceFormId{ 0 };
        std::uint32_t referenceNativeHandle{ 0 };
        std::uint64_t allowedLayerMask{ 0 };
        std::uint32_t leaseFrames{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        RockProviderPoint3 pivotWorldGame{};
        RockProviderPoint3 axisWorldGame{};
        float minimumCoordinate{ 0.0f };
        float maximumCoordinate{ 1.0f };
        float currentCoordinate{ 0.0f };
        std::uint32_t reserved0{ 0 };
        std::uint64_t reserved[3]{};
    };

    struct RockProviderTouchGrabStateV1
    {
        std::uint32_t size{ sizeof(RockProviderTouchGrabStateV1) };
        std::uint32_t version{ ROCK_PROVIDER_API_VERSION };
        std::uint64_t targetId{ 0 };
        std::uint32_t targetGeneration{ 0 };
        RockProviderTouchGrabKindV1 kind{
            RockProviderTouchGrabKindV1::FixedAnchor
        };
        RockProviderTouchGrabPhaseV1 phase{
            RockProviderTouchGrabPhaseV1::Inactive
        };
        RockProviderTouchGrabReleaseReasonV1 releaseReason{
            RockProviderTouchGrabReleaseReasonV1::None
        };
        std::uint32_t bodyId{ 0x7FFF'FFFF };
        std::uint32_t referenceFormId{ 0 };
        std::uint32_t referenceNativeHandle{ 0 };
        std::uint32_t activeHandMask{ 0 };
        std::uint32_t flags{ 0 };
        std::uint32_t reserved0{ 0 };
        float currentCoordinate{ 0.0f };
        float coordinateVelocity{ 0.0f };
        RockProviderPoint3 contactPointGame{};
        RockProviderPoint3 contactNormalGame{};
        std::uint64_t frameIndex{ 0 };
        std::uint64_t sequence{ 0 };
        std::uint32_t worldGeneration{ 0 };
        std::uint32_t skeletonGeneration{ 0 };
        std::uint32_t providerGeneration{ 0 };
        std::uint32_t collisionGeneration{ 0 };
        std::uint64_t reserved[2]{};
    };

    using RockProviderFrameCallback = void(ROCK_PROVIDER_CALL*)(const RockProviderFrameSnapshot* snapshot, void* userData);
    using RockProviderAnimationPhaseCallbackV1 = void(ROCK_PROVIDER_CALL*)(
        const RockProviderAnimationPhaseContextV1* context,
        void* userData);

    struct RockProviderApi
    {
        static constexpr auto ROCK_F4SE_MOD_NAME = "ROCK";

        std::uint32_t(ROCK_PROVIDER_CALL* getVersion)();
        const char*(ROCK_PROVIDER_CALL* getModVersion)();
        bool(ROCK_PROVIDER_CALL* isProviderReady)();
        std::uint64_t(ROCK_PROVIDER_CALL* registerFrameCallback)(RockProviderFrameCallback callback, void* userData);
        bool(ROCK_PROVIDER_CALL* unregisterFrameCallback)(std::uint64_t callbackToken);
        bool(ROCK_PROVIDER_CALL* getFrameSnapshot)(RockProviderFrameSnapshot* outSnapshot);
        bool(ROCK_PROVIDER_CALL* queryWeaponContactAtPoint)(const RockProviderWeaponContactQuery* query, RockProviderWeaponContactResult* outResult);
        void(ROCK_PROVIDER_CALL* clearExternalBodies)(std::uint64_t ownerToken);
        bool(ROCK_PROVIDER_CALL* setOffhandInteractionReservation)(std::uint64_t ownerToken, RockProviderOffhandReservation reservation);
        bool(ROCK_PROVIDER_CALL* registerExternalBodiesV1)(
            std::uint64_t ownerToken,
            const RockProviderExternalBodyRegistration* bodies,
            std::uint32_t bodyCount);
        std::uint32_t(ROCK_PROVIDER_CALL* getWeaponEvidenceDetailCountV1)();
        std::uint32_t(ROCK_PROVIDER_CALL* copyWeaponEvidenceDetailsV1)(RockProviderWeaponEvidenceDetailV1* outDetails, std::uint32_t maxDetails);
        std::uint32_t(ROCK_PROVIDER_CALL* getWeaponEvidenceDetailPointCountV1)(std::uint32_t bodyId);
        std::uint32_t(ROCK_PROVIDER_CALL* copyWeaponEvidenceDetailPointsV1)(
            std::uint32_t bodyId,
            RockProviderPoint3* outPoints,
            std::uint32_t maxPoints);
        std::uint32_t(ROCK_PROVIDER_CALL* getBodyContactSnapshotV1)(RockProviderBodyContactV1* outContacts, std::uint32_t maxContacts);
        RockProviderHand(ROCK_PROVIDER_CALL* getPrimaryHandV1)();
        RockProviderHand(ROCK_PROVIDER_CALL* getOffhandHandV1)();
        bool(ROCK_PROVIDER_CALL* getHandFrameV1)(RockProviderHand hand, RockProviderHandFrameV1* outFrame);
        RockProviderResultV1(ROCK_PROVIDER_CALL* registerConsumerV1)(
            const RockProviderConsumerRegistrationV1* registration,
            RockProviderConsumerHandleV1* outHandle);
        RockProviderResultV1(ROCK_PROVIDER_CALL* unregisterConsumerV1)(std::uint64_t ownerToken);
        std::uint32_t(ROCK_PROVIDER_CALL* getGrantedCapabilitiesV1)(std::uint64_t ownerToken);
        bool(ROCK_PROVIDER_CALL* getProviderLimitsV1)(RockProviderLimitsV1* outLimits);
        std::uint32_t(ROCK_PROVIDER_CALL* getExternalContactSnapshotForOwnerV1)(
            std::uint64_t ownerToken,
            RockProviderExternalContactV1* outContacts,
            std::uint32_t maxContacts);
        RockProviderResultV1(ROCK_PROVIDER_CALL* requestForceGrabV1)(
            std::uint64_t ownerToken,
            const RockProviderForceGrabRequestV1* request,
            std::uint64_t* outCommandId);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getInteractionCommandResultV1)(
            std::uint64_t ownerToken,
            std::uint64_t commandId,
            RockProviderInteractionCommandResultV1* outResult);
        RockProviderResultV1(ROCK_PROVIDER_CALL* requestForceReleaseV1)(
            std::uint64_t ownerToken,
            const RockProviderForceReleaseRequestV1* request,
            std::uint64_t* outCommandId);
        RockProviderResultV1(ROCK_PROVIDER_CALL* requestThrownDropV1)(
            std::uint64_t ownerToken,
            const RockProviderThrownDropRequestV1* request,
            std::uint64_t* outCommandId);
        RockProviderResultV1(ROCK_PROVIDER_CALL* setHandInputSuppressionV1)(
            std::uint64_t ownerToken,
            const RockProviderHandInputSuppressionRequestV1* request);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearHandInputSuppressionV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand);
        RockProviderResultV1(ROCK_PROVIDER_CALL* setWeaponPartTargetsV1)(
            std::uint64_t ownerToken,
            const RockProviderWeaponPartTargetV1* targets,
            std::uint32_t targetCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearWeaponPartTargetsV1)(std::uint64_t ownerToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* setWeaponPartDriveTargetsV1)(
            std::uint64_t ownerToken,
            const RockProviderWeaponPartDriveTargetV1* targets,
            std::uint32_t targetCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearWeaponPartDriveTargetsV1)(std::uint64_t ownerToken);
        /*
         * FROZEN ABI SLOTS 33/34 (byte offsets 0x108/0x110). Cylon's
         * VirtualReloads ships as a PRE-BUILT BINARY compiled against
         * dist/Cylon_ROCKProvider_Complete_2026-07-24/ROCKProviderApi.h, where
         * these two members sit here - immediately after
         * clearWeaponPartDriveTargetsV1 and immediately before
         * queryEquippedWeaponClassificationV1. Its call sites are baked-in
         * `call [rcx+0x108]` / `call [rcx+0x110]`, so relocating these members
         * (e.g. "appending" them after upstream's newer table entries) silently
         * redirects them into whatever now occupies those offsets and faults on
         * the first weapon-part motion-constraint call. Neither the version
         * check nor a `>=` table-size check can detect a REORDER. New members -
         * ours or upstream's - go at the TAIL of this struct, never here.
         * Pinned by the static_assert block directly after this struct.
         */
        RockProviderResultV1(ROCK_PROVIDER_CALL* setWeaponPartMotionConstraintsV1)(
            std::uint64_t ownerToken,
            const RockProviderWeaponPartMotionConstraintV1* constraints,
            std::uint32_t constraintCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearWeaponPartMotionConstraintsV1)(std::uint64_t ownerToken);
        bool(ROCK_PROVIDER_CALL* queryEquippedWeaponClassificationV1)(RockProviderWeaponClassificationV1* outResult);
        bool(ROCK_PROVIDER_CALL* getWeaponPartGripStateV1)(RockProviderHand hand, RockProviderWeaponPartGripStateV1* outState);
        bool(ROCK_PROVIDER_CALL* getRawWandButtonStateV1)(RockProviderHand hand, std::uint32_t buttonId, RockProviderRawWandButtonStateV1* outState);
        /*
         * True while ROCK suppresses the pipboy-hand trigger's remaining
         * native game actions: its flashlight hold during a ROCK interaction,
         * or all native game input while a provider suppression lease is
         * active. Consumers that repurpose the trigger should treat it as
         * exclusively theirs only while this reads true; otherwise a hold may
         * still toggle the flashlight. While ROCK input remapping is enabled,
         * the legacy trigger-release Pip-Boy open is always moved to a short
         * release of the native Pause button and is not represented here.
         */
        bool(ROCK_PROVIDER_CALL* isNativePipboyInputSuppressedV1)();
        /*
         * ---- END OF THE FROZEN CYLON PREFIX (slots 0..38, 0x000..0x137) ----
         * Everything above this line has the exact order and byte offsets of
         * dist/Cylon_ROCKProvider_Complete_2026-07-24/ROCKProviderApi.h and may
         * never be reordered, removed, or have anything inserted into it.
         *
         * Slots 39..44 below are Heisenberg's own shipped-0.8.4 additions. They
         * are held at their 0.8.4 offsets for the same reason (cheap, and it
         * keeps every offset that has ever been published stable); upstream
         * ROCK's newer members are appended after them starting at slot 45.
         */
        RockProviderResultV1(ROCK_PROVIDER_CALL* setWeaponPartInteractionZonesV1)(
            std::uint64_t ownerToken,
            const RockProviderWeaponPartInteractionZoneV1* zones,
            std::uint32_t zoneCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearWeaponPartInteractionZonesV1)(
            std::uint64_t ownerToken);
        std::uint32_t(ROCK_PROVIDER_CALL* getLearnedWeaponPartProfileCountV1)();
        std::uint32_t(ROCK_PROVIDER_CALL* copyLearnedWeaponPartProfilesV1)(
            RockProviderLearnedWeaponPartProfileV1* outProfiles,
            std::uint32_t maxProfiles);
        RockProviderResultV1(ROCK_PROVIDER_CALL* activateLearnedWeaponPartProfileV1)(
            std::uint64_t ownerToken,
            const RockProviderLearnedWeaponPartActivationRequestV1* request,
            RockProviderLearnedWeaponPartActivationResultV1* outResult);
        /*
         * Clears only the high-level learned activation (its generated target,
         * drive, constraint, and zone). Manual V1 sets owned by the same token
         * remain intact.
         */
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearLearnedWeaponPartProfileV1)(
            std::uint64_t ownerToken);
        /* ---- Upstream ROCK additions (slot 45 onwards). Append only. ---- */
        std::uint32_t(ROCK_PROVIDER_CALL* getWeaponEmitterCountV1)();
        std::uint32_t(ROCK_PROVIDER_CALL* copyWeaponEmittersV1)(RockProviderWeaponEmitterV1* outEmitters, std::uint32_t maxEmitters);
        RockProviderResultV1(ROCK_PROVIDER_CALL* setNativeAnimationAuthorityV1)(
            std::uint64_t ownerToken,
            const RockProviderNativeAnimationAuthorityRequestV1* request);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearNativeAnimationAuthorityV1)(std::uint64_t ownerToken);
        bool(ROCK_PROVIDER_CALL* getNativeAnimationAuthorityStateV1)(RockProviderNativeAnimationAuthorityStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* registerAnimationPhaseCallbackV1)(
            std::uint64_t ownerToken,
            RockProviderAnimationPhaseCallbackV1 callback,
            void* userData,
            std::uint64_t* outCallbackToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* unregisterAnimationPhaseCallbackV1)(
            std::uint64_t ownerToken,
            std::uint64_t callbackToken);
        bool(ROCK_PROVIDER_CALL* getEquippedWeaponGripStateV1)(
            std::uint64_t ownerToken,
            RockProviderEquippedWeaponGripStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* setHandVisualAuthorityV1)(
            std::uint64_t ownerToken,
            const RockProviderHandVisualAuthorityRequestV1* request);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearHandVisualAuthorityV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand);
        RockProviderResultV1(ROCK_PROVIDER_CALL* publishNativeAnimationRuntimeV1)(
            std::uint64_t ownerToken,
            const RockProviderNativeAnimationRuntimePublicationV1* publication);
        RockProviderResultV1(ROCK_PROVIDER_CALL* setEquippedWeaponHandlingAuthorityV1)(
            std::uint64_t ownerToken,
            const RockProviderEquippedWeaponHandlingRequestV1* request);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearEquippedWeaponHandlingAuthorityV1)(
            std::uint64_t ownerToken);
        bool(ROCK_PROVIDER_CALL* getEquippedWeaponHandlingStateV1)(
            RockProviderEquippedWeaponHandlingStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* publishDebugOverlayV1)(
            std::uint64_t ownerToken,
            const RockProviderDebugOverlayPublicationV1* publication);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearDebugOverlayV1)(
            std::uint64_t ownerToken);
        /*
         * Game-thread-only value snapshot of hFRIK's current presented hand
         * after visual-authority writers. Unlike getHandFrameV1, this is not
         * ROCK's root-flattened physics authority and exposes no scene node.
         */
        bool(ROCK_PROVIDER_CALL* getPresentedHandFrameV1)(
            RockProviderHand hand,
            RockProviderHandFrameV1* outFrame);
        bool(ROCK_PROVIDER_CALL* getProviderLimitsExtV1)(RockProviderLimitsExtV1* outLimits);
        std::uint32_t(ROCK_PROVIDER_CALL* getPublicStructureSizeV1)(RockProviderStructureIdV1 structureId);
        /*
         * Owner callbacks run on ROCK's game-thread frame boundary. Removal
         * prevents future copies but is not a quiescence barrier for an
         * invocation already copied for dispatch; userData must therefore
         * remain alive until that invocation returns. A callback fault revokes
         * every stateful resource and callback owned by that consumer.
         */
        RockProviderResultV1(ROCK_PROVIDER_CALL* registerFrameCallbackForOwnerV1)(
            std::uint64_t ownerToken,
            RockProviderFrameCallback callback,
            void* userData,
            std::uint64_t* outCallbackToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* unregisterFrameCallbackForOwnerV1)(
            std::uint64_t ownerToken,
            std::uint64_t callbackToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getHandInteractionStateV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand,
            RockProviderHandInteractionStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyProviderEventsSinceV1)(
            std::uint64_t ownerToken,
            std::uint64_t afterSequence,
            RockProviderEventV1* outEvents,
            std::uint32_t maxEvents,
            RockProviderEventStreamStateV1* outStreamState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getEquippedWeaponStateV1)(
            std::uint64_t ownerToken,
            RockProviderEquippedWeaponStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* registerExternalBodiesForScopeV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken,
            const RockProviderExternalBodyRegistration* bodies,
            std::uint32_t bodyCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearExternalBodiesForScopeV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyExternalContactsSinceV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken,
            std::uint64_t afterSequence,
            RockProviderExternalContactRecordV1* outContacts,
            std::uint32_t maxContacts,
            RockProviderExternalContactStreamStateV1* outStreamState);
        /*
         * Live scene/physics readbacks are game-thread-only and return
         * WrongThread outside ROCK's owner frame callbacks. This applies to
         * weapon-part poses and drive results, scope state, authored/presented
         * poses, semantic contacts, player colliders, and hand-collision
         * availability. Target resolution and weapon-composition snapshots
         * are independently synchronized value queries.
         */
        RockProviderResultV1(ROCK_PROVIDER_CALL* queryWeaponPartTargetResolutionV1)(
            std::uint64_t ownerToken,
            const RockProviderWeaponPartResolutionQueryV1* query,
            RockProviderWeaponPartResolutionResultV1* outResolution);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyWeaponPartPoseSnapshotV1)(
            std::uint64_t ownerToken,
            RockProviderWeaponPartPoseV1* outParts,
            std::uint32_t maxParts,
            std::uint32_t* outPartCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyWeaponPartDriveApplicationResultsV1)(
            std::uint64_t ownerToken,
            RockProviderWeaponPartDriveApplicationResultV1* outResults,
            std::uint32_t maxResults,
            std::uint32_t* outResultCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getScopeSightStateV1)(
            std::uint64_t ownerToken,
            RockProviderScopeSightStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getWeaponCompositionStateV1)(
            std::uint64_t ownerToken,
            RockProviderWeaponCompositionStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyWeaponCompositionEntriesV1)(
            std::uint64_t ownerToken,
            RockProviderWeaponCompositionEntryV1* outEntries,
            std::uint32_t maxEntries,
            std::uint32_t* outEntryCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getSelectedAuthoredGripPoseV1)(
            std::uint64_t ownerToken,
            RockProviderAuthoredGripPoseV1* outPose);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getPresentedHandPoseV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand,
            RockProviderPresentedHandPoseV1* outPose);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copySemanticHandContactsV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand,
            std::uint32_t maxFramesSinceContact,
            RockProviderSemanticHandContactV1* outContacts,
            std::uint32_t maxContacts,
            std::uint32_t* outContactCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyPlayerColliderDescriptorsV1)(
            std::uint64_t ownerToken,
            RockProviderPlayerColliderDescriptorV1* outDescriptors,
            std::uint32_t maxDescriptors,
            std::uint32_t* outDescriptorCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getHandCollisionAvailabilityV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand,
            RockProviderHandCollisionAvailabilityV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* cancelInteractionCommandV1)(
            std::uint64_t ownerToken,
            std::uint64_t commandId);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getHandInputSuppressionStateV1)(
            std::uint64_t ownerToken,
            RockProviderHand hand,
            RockProviderHandInputSuppressionStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* acquireOffhandReservationV1)(
            std::uint64_t ownerToken,
            const RockProviderOffhandReservationRequestV1* request);
        RockProviderResultV1(ROCK_PROVIDER_CALL* renewOffhandReservationV1)(
            std::uint64_t ownerToken,
            const RockProviderOffhandReservationRequestV1* request);
        RockProviderResultV1(ROCK_PROVIDER_CALL* releaseOffhandReservationV1)(
            std::uint64_t ownerToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* getOffhandReservationStateV1)(
            std::uint64_t ownerToken,
            RockProviderOffhandReservationStateV1* outState);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearNativeAnimationRuntimeV1)(
            std::uint64_t ownerToken);
        /*
         * Touch registrations are owner/capability gated. A zero-count set is
         * a valid transactional scope clear. copyTouchGrabStatesForScopeV1
         * returns owner-scoped snapshots; their sequence changes on every
         * provider state transition/publication. Yield is asynchronous:
         * requestTouchGrabYieldV1 blocks new acquisition immediately, and the
         * owner waits for Yielded before starting native/scripted motion. A
         * yielded or invalidated descriptor stays non-acquirable until removed
         * or republished with a new targetGeneration.
         */
        RockProviderResultV1(ROCK_PROVIDER_CALL* setTouchGrabTargetsForScopeV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken,
            const RockProviderTouchGrabTargetV1* targets,
            std::uint32_t targetCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* clearTouchGrabTargetsForScopeV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken);
        RockProviderResultV1(ROCK_PROVIDER_CALL* copyTouchGrabStatesForScopeV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken,
            RockProviderTouchGrabStateV1* outStates,
            std::uint32_t maxStates,
            std::uint32_t* outStateCount);
        RockProviderResultV1(ROCK_PROVIDER_CALL* requestTouchGrabYieldV1)(
            std::uint64_t ownerToken,
            std::uint64_t scopeToken,
            std::uint64_t targetId,
            std::uint32_t targetGeneration);

        [[nodiscard]] static int initialize(
            const std::uint32_t minVersion = ROCK_PROVIDER_API_VERSION,
            const std::uint32_t minProviderApiByteSize = 0)
        {
            if (inst) {
                if (negotiatedApiVersion < minVersion) {
                    return 4;
                }
                return minProviderApiByteSize == 0 ||
                               (negotiatedTableByteSize != 0 &&
                                   negotiatedTableByteSize >= minProviderApiByteSize) ?
                    0 :
                    5;
            }

            /*
             * Two build variants coexist in the wild (see MasterModTemplate's
             * "MO2 embed build selection" notes): a standalone ROCK.dll host,
             * and the current embedded architecture where this API is compiled
             * directly into Heisenberg_F4VR.dll with no separate ROCK.dll ever
             * loaded. Exactly one provider identity may be active. Silently
             * preferring ROCK.dll when both hosts export the provider would
             * split ownership, callback, and lifecycle state between runtimes.
             */
            const HMODULE standaloneRockDll = GetModuleHandleA("ROCK.dll");
            if (!standaloneRockDll) {
                diag::logModuleLookupFailure("ROCK.dll");
            }
            const HMODULE embeddedRockDll =
                GetModuleHandleA("Heisenberg_F4VR.dll");
            if (!embeddedRockDll) {
                diag::logModuleLookupFailure("Heisenberg_F4VR.dll");
            }

            const auto exportsProvider = [](const HMODULE module) {
                return module &&
                       (GetProcAddress(module, "ROCKAPI_GetDescriptorV1") ||
                           GetProcAddress(module, "ROCKAPI_GetProviderApi"));
            };
            const bool standaloneExportsProvider =
                exportsProvider(standaloneRockDll);
            const bool embeddedExportsProvider =
                exportsProvider(embeddedRockDll);
            if (standaloneRockDll != embeddedRockDll &&
                standaloneExportsProvider && embeddedExportsProvider) {
                diag::log(
                    "[ROCK-DIAG] both ROCK.dll and Heisenberg_F4VR.dll export "
                    "the ROCK provider; refusing ambiguous dual runtime\n");
                return 7;
            }

            const HMODULE rockDll = standaloneExportsProvider ?
                standaloneRockDll :
                (embeddedExportsProvider ? embeddedRockDll : nullptr);
            if (!rockDll) {
                return 1;
            }
            diag::log("[ROCK-DIAG] module handle resolved OK\n");

            using GetDescriptorFn = const RockProviderApiDescriptorV1*(ROCK_PROVIDER_CALL*)();
            const auto getDescriptor = reinterpret_cast<GetDescriptorFn>(
                GetProcAddress(rockDll, "ROCKAPI_GetDescriptorV1"));
            if (getDescriptor) {
                const auto* descriptor = getDescriptor();
                constexpr auto minimumDescriptorBytes = static_cast<std::uint32_t>(
                    offsetof(RockProviderApiDescriptorV1, table) +
                    sizeof(std::declval<RockProviderApiDescriptorV1>().table));
                constexpr auto minimumTableBytes = static_cast<std::uint32_t>(
                    sizeof(std::declval<RockProviderApi>().getVersion));
                if (!descriptor || descriptor->size < minimumDescriptorBytes ||
                    !descriptor->table || descriptor->tableByteSize < minimumTableBytes) {
                    return 6;
                }
                if (descriptor->apiVersion < minVersion) {
                    return 4;
                }
                if (minProviderApiByteSize != 0 &&
                    descriptor->tableByteSize < minProviderApiByteSize) {
                    return 5;
                }
                inst = descriptor->table;
                negotiatedApiVersion = descriptor->apiVersion;
                negotiatedTableByteSize = descriptor->tableByteSize;
                negotiatedFeatureBits = descriptor->featureBits;
                negotiatedFeatureBits2 = descriptor->featureBits2;
                return 0;
            }

            const auto getApi = reinterpret_cast<const RockProviderApi*(ROCK_PROVIDER_CALL*)()>(GetProcAddress(rockDll, "ROCKAPI_GetProviderApi"));
            if (!getApi) {
                return 2;
            }

            const auto api = getApi();
            if (!api) {
                return 3;
            }

            if (api->getVersion() < minVersion) {
                return 4;
            }

            /*
             * Without a descriptor, the only table extent that can be proven
             * without reading past an older provider allocation is slot zero,
             * which was just invoked above. Publish that conservative extent
             * instead of zero: zero made later helpers treat the size as
             * "unknown" and probe getProviderLimitsV1 beyond an arbitrarily
             * short legacy table. A consumer requesting any larger prefix must
             * require the safe descriptor export.
             */
            constexpr auto descriptorlessKnownTableBytes =
                static_cast<std::uint32_t>(
                    sizeof(std::declval<RockProviderApi>().getVersion));
            if (minProviderApiByteSize > descriptorlessKnownTableBytes) {
                return 6;
            }

            inst = api;
            negotiatedApiVersion = api->getVersion();
            negotiatedTableByteSize = descriptorlessKnownTableBytes;
            negotiatedFeatureBits = 0;
            negotiatedFeatureBits2 = 0;
            return 0;
        }

        inline static const RockProviderApi* inst = nullptr;
        inline static std::uint32_t negotiatedApiVersion = 0;
        inline static std::uint32_t negotiatedTableByteSize = 0;
        inline static std::uint32_t negotiatedFeatureBits = 0;
        inline static std::uint32_t negotiatedFeatureBits2 = 0;
    };

    /*
     * ======================= FROZEN PROVIDER ABI =========================
     * Cylon's VirtualReloads is distributed as a PRE-BUILT BINARY. It was
     * compiled against
     *     dist/Cylon_ROCKProvider_Complete_2026-07-24/ROCKProviderApi.h
     * (byte-identical function table to the ExactContact package of the same
     * date), and it reaches this table through indirect calls at fixed byte
     * offsets baked into its machine code. It is never recompiled when we sync
     * upstream ROCK, so the offsets below are part of its binary contract.
     *
     * HOW THESE NUMBERS WERE DERIVED: every member of this struct is a single
     * function pointer, so on x86-64 each occupies exactly 8 bytes at natural
     * 8-byte alignment and the struct has no padding. Therefore
     *     offset == declaration_index * 8
     * with no arithmetic beyond that. The declaration indices are read
     * straight off the shipped header's struct RockProviderApi: getVersion is
     * index 0 and isNativePipboyInputSuppressedV1 is index 38, giving a
     * 39 * 8 == 0x138-byte table. Slots 39..44 are Heisenberg's own additions
     * as shipped in 0.8.4, whose table was that same 39-member prefix plus
     * those 6 members, i.e. 45 * 8 == 0x168 bytes.
     *
     * WHY static_assert AND NOT JUST A SIZE CHECK: the version handshake
     * cannot see a reorder (both sides are ROCK_PROVIDER_API_VERSION 1), and
     * every table-size gate in this header is a ">=" test, which a REORDERED
     * table passes trivially - it is the same size or larger. Only a pinned
     * offsetof fails when a member moves. If one of these fires, do not edit
     * the number: move the member back to its slot and append the new member
     * at the TAIL of the struct instead.
     * =====================================================================
     */
    static_assert(std::is_standard_layout_v<RockProviderApi>,
        "FROZEN ABI: RockProviderApi must stay standard-layout for offsetof to be meaningful.");
    static_assert(sizeof(void (*)()) == 8,
        "FROZEN ABI: the pinned offsets below assume 8-byte function pointers (x86-64).");

    // --- Slots 0..38: the exact table Cylon's shipped binary was built on. ---
    static_assert(offsetof(RockProviderApi, getVersion) == 0x000,
        "FROZEN ABI: getVersion moved off slot 0 (0x000) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getModVersion) == 0x008,
        "FROZEN ABI: getModVersion moved off slot 1 (0x008) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, isProviderReady) == 0x010,
        "FROZEN ABI: isProviderReady moved off slot 2 (0x010) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, registerFrameCallback) == 0x018,
        "FROZEN ABI: registerFrameCallback moved off slot 3 (0x018) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, unregisterFrameCallback) == 0x020,
        "FROZEN ABI: unregisterFrameCallback moved off slot 4 (0x020) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getFrameSnapshot) == 0x028,
        "FROZEN ABI: getFrameSnapshot moved off slot 5 (0x028) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, queryWeaponContactAtPoint) == 0x030,
        "FROZEN ABI: queryWeaponContactAtPoint moved off slot 6 (0x030) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, clearExternalBodies) == 0x038,
        "FROZEN ABI: clearExternalBodies moved off slot 7 (0x038) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, setOffhandInteractionReservation) == 0x040,
        "FROZEN ABI: setOffhandInteractionReservation moved off slot 8 (0x040) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, registerExternalBodiesV1) == 0x048,
        "FROZEN ABI: registerExternalBodiesV1 moved off slot 9 (0x048) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getWeaponEvidenceDetailCountV1) == 0x050,
        "FROZEN ABI: getWeaponEvidenceDetailCountV1 moved off slot 10 (0x050) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, copyWeaponEvidenceDetailsV1) == 0x058,
        "FROZEN ABI: copyWeaponEvidenceDetailsV1 moved off slot 11 (0x058) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getWeaponEvidenceDetailPointCountV1) == 0x060,
        "FROZEN ABI: getWeaponEvidenceDetailPointCountV1 moved off slot 12 (0x060) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, copyWeaponEvidenceDetailPointsV1) == 0x068,
        "FROZEN ABI: copyWeaponEvidenceDetailPointsV1 moved off slot 13 (0x068) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getBodyContactSnapshotV1) == 0x070,
        "FROZEN ABI: getBodyContactSnapshotV1 moved off slot 14 (0x070) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getPrimaryHandV1) == 0x078,
        "FROZEN ABI: getPrimaryHandV1 moved off slot 15 (0x078) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getOffhandHandV1) == 0x080,
        "FROZEN ABI: getOffhandHandV1 moved off slot 16 (0x080) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getHandFrameV1) == 0x088,
        "FROZEN ABI: getHandFrameV1 moved off slot 17 (0x088) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, registerConsumerV1) == 0x090,
        "FROZEN ABI: registerConsumerV1 moved off slot 18 (0x090) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, unregisterConsumerV1) == 0x098,
        "FROZEN ABI: unregisterConsumerV1 moved off slot 19 (0x098) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getGrantedCapabilitiesV1) == 0x0A0,
        "FROZEN ABI: getGrantedCapabilitiesV1 moved off slot 20 (0x0A0) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getProviderLimitsV1) == 0x0A8,
        "FROZEN ABI: getProviderLimitsV1 moved off slot 21 (0x0A8) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getExternalContactSnapshotForOwnerV1) == 0x0B0,
        "FROZEN ABI: getExternalContactSnapshotForOwnerV1 moved off slot 22 (0x0B0) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, requestForceGrabV1) == 0x0B8,
        "FROZEN ABI: requestForceGrabV1 moved off slot 23 (0x0B8) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getInteractionCommandResultV1) == 0x0C0,
        "FROZEN ABI: getInteractionCommandResultV1 moved off slot 24 (0x0C0) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, requestForceReleaseV1) == 0x0C8,
        "FROZEN ABI: requestForceReleaseV1 moved off slot 25 (0x0C8) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, requestThrownDropV1) == 0x0D0,
        "FROZEN ABI: requestThrownDropV1 moved off slot 26 (0x0D0) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, setHandInputSuppressionV1) == 0x0D8,
        "FROZEN ABI: setHandInputSuppressionV1 moved off slot 27 (0x0D8) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, clearHandInputSuppressionV1) == 0x0E0,
        "FROZEN ABI: clearHandInputSuppressionV1 moved off slot 28 (0x0E0) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, setWeaponPartTargetsV1) == 0x0E8,
        "FROZEN ABI: setWeaponPartTargetsV1 moved off slot 29 (0x0E8) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, clearWeaponPartTargetsV1) == 0x0F0,
        "FROZEN ABI: clearWeaponPartTargetsV1 moved off slot 30 (0x0F0) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, setWeaponPartDriveTargetsV1) == 0x0F8,
        "FROZEN ABI: setWeaponPartDriveTargetsV1 moved off slot 31 (0x0F8) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, clearWeaponPartDriveTargetsV1) == 0x100,
        "FROZEN ABI: clearWeaponPartDriveTargetsV1 moved off slot 32 (0x100) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, setWeaponPartMotionConstraintsV1) == 0x108,
        "FROZEN ABI: setWeaponPartMotionConstraintsV1 moved off slot 33 (0x108) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, clearWeaponPartMotionConstraintsV1) == 0x110,
        "FROZEN ABI: clearWeaponPartMotionConstraintsV1 moved off slot 34 (0x110) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, queryEquippedWeaponClassificationV1) == 0x118,
        "FROZEN ABI: queryEquippedWeaponClassificationV1 moved off slot 35 (0x118) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getWeaponPartGripStateV1) == 0x120,
        "FROZEN ABI: getWeaponPartGripStateV1 moved off slot 36 (0x120) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, getRawWandButtonStateV1) == 0x128,
        "FROZEN ABI: getRawWandButtonStateV1 moved off slot 37 (0x128) - Cylon's prebuilt binary calls this offset.");
    static_assert(offsetof(RockProviderApi, isNativePipboyInputSuppressedV1) == 0x130,
        "FROZEN ABI: isNativePipboyInputSuppressedV1 moved off slot 38 (0x130) - Cylon's prebuilt binary calls this offset.");

    /*
     * Total byte size of the frozen Cylon prefix. isNativePipboyInputSuppressedV1
     * is slot 38, so the shipped table ends at 0x130 + 8 == 0x138 (312). This
     * catches an insertion inside the prefix even if an individual assert above
     * were ever deleted together with its member.
     */
    inline constexpr std::uint32_t ROCK_PROVIDER_API_FROZEN_CYLON_PREFIX_BYTES = 0x138u;
    static_assert(offsetof(RockProviderApi, isNativePipboyInputSuppressedV1) +
            sizeof(std::declval<RockProviderApi>().isNativePipboyInputSuppressedV1) ==
            ROCK_PROVIDER_API_FROZEN_CYLON_PREFIX_BYTES,
        "FROZEN ABI: the Cylon prefix must be exactly 39 slots / 0x138 bytes.");

    // --- Slots 39..44: Heisenberg's own additions, at their shipped-0.8.4 offsets. ---
    static_assert(offsetof(RockProviderApi, setWeaponPartInteractionZonesV1) == 0x138,
        "FROZEN ABI: setWeaponPartInteractionZonesV1 moved off slot 39 (0x138) - this is a published Heisenberg 0.8.4 offset.");
    static_assert(offsetof(RockProviderApi, clearWeaponPartInteractionZonesV1) == 0x140,
        "FROZEN ABI: clearWeaponPartInteractionZonesV1 moved off slot 40 (0x140) - this is a published Heisenberg 0.8.4 offset.");
    static_assert(offsetof(RockProviderApi, getLearnedWeaponPartProfileCountV1) == 0x148,
        "FROZEN ABI: getLearnedWeaponPartProfileCountV1 moved off slot 41 (0x148) - this is a published Heisenberg 0.8.4 offset.");
    static_assert(offsetof(RockProviderApi, copyLearnedWeaponPartProfilesV1) == 0x150,
        "FROZEN ABI: copyLearnedWeaponPartProfilesV1 moved off slot 42 (0x150) - this is a published Heisenberg 0.8.4 offset.");
    static_assert(offsetof(RockProviderApi, activateLearnedWeaponPartProfileV1) == 0x158,
        "FROZEN ABI: activateLearnedWeaponPartProfileV1 moved off slot 43 (0x158) - this is a published Heisenberg 0.8.4 offset.");
    static_assert(offsetof(RockProviderApi, clearLearnedWeaponPartProfileV1) == 0x160,
        "FROZEN ABI: clearLearnedWeaponPartProfileV1 moved off slot 44 (0x160) - this is a published Heisenberg 0.8.4 offset.");

    inline constexpr std::uint32_t ROCK_PROVIDER_API_HEISENBERG_084_PREFIX_BYTES = 0x168u;
    static_assert(offsetof(RockProviderApi, clearLearnedWeaponPartProfileV1) +
            sizeof(std::declval<RockProviderApi>().clearLearnedWeaponPartProfileV1) ==
            ROCK_PROVIDER_API_HEISENBERG_084_PREFIX_BYTES,
        "FROZEN ABI: the Heisenberg 0.8.4 prefix must be exactly 45 slots / 0x168 bytes.");

    /*
     * Upstream ROCK's members start here (slot 45, 0x168) and are deliberately
     * NOT pinned: every upstream consumer is recompiled from source in this
     * build, so moving them is free. This assert records where the append
     * region begins, so a future sync that inserts into the frozen region
     * above is caught even if it keeps sizeof() plausible.
     */
    static_assert(offsetof(RockProviderApi, getWeaponEmitterCountV1) ==
            ROCK_PROVIDER_API_HEISENBERG_084_PREFIX_BYTES,
        "FROZEN ABI: upstream's append region must start at slot 45 (0x168).");

    /*
     * PUBLIC ABI - exported to other F4SE plugins (e.g. Cylon's VirtualReloads)
     * via GetProcAddress against this exact symbol name. Do not rename, remove,
     * or make conditional; the ROCK_PROVIDER_API/ROCK_PROVIDER_CALL macros above
     * must keep resolving to extern "C" __declspec(dllexport)/__cdecl whenever
     * ROCK_API_EXPORTS is defined (see ROCKProviderApi.cpp, which defines it
     * before including this header) - that #define is load-bearing for external
     * consumers, not incidental. Widen the API by bumping ROCK_PROVIDER_API_VERSION
     * and appending new function-table members, never by altering this signature.
     */
    ROCK_PROVIDER_API const RockProviderApi* ROCK_PROVIDER_CALL ROCKAPI_GetProviderApi();
    ROCK_PROVIDER_API const RockProviderApiDescriptorV1* ROCK_PROVIDER_CALL ROCKAPI_GetDescriptorV1();

    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_FORCE_GRAB_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getInteractionCommandResultV1) + sizeof(std::declval<RockProviderApi>().getInteractionCommandResultV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_FORCE_RELEASE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, requestForceReleaseV1) + sizeof(std::declval<RockProviderApi>().requestForceReleaseV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_THROWN_DROP_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, requestThrownDropV1) + sizeof(std::declval<RockProviderApi>().requestThrownDropV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_HAND_INPUT_SUPPRESSION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearHandInputSuppressionV1) + sizeof(std::declval<RockProviderApi>().clearHandInputSuppressionV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_PART_INTERACTION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearWeaponPartDriveTargetsV1) + sizeof(std::declval<RockProviderApi>().clearWeaponPartDriveTargetsV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_CLASSIFICATION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, queryEquippedWeaponClassificationV1) + sizeof(std::declval<RockProviderApi>().queryEquippedWeaponClassificationV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_PART_GRIP_STATE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getWeaponPartGripStateV1) + sizeof(std::declval<RockProviderApi>().getWeaponPartGripStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_PART_MOTION_CONSTRAINT_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearWeaponPartMotionConstraintsV1) + sizeof(std::declval<RockProviderApi>().clearWeaponPartMotionConstraintsV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_RAW_WAND_BUTTON_STATE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getRawWandButtonStateV1) + sizeof(std::declval<RockProviderApi>().getRawWandButtonStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_PIPBOY_INPUT_SUPPRESSION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, isNativePipboyInputSuppressedV1) + sizeof(std::declval<RockProviderApi>().isNativePipboyInputSuppressedV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_PART_INTERACTION_ZONE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearWeaponPartInteractionZonesV1) + sizeof(std::declval<RockProviderApi>().clearWeaponPartInteractionZonesV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_LEARNED_WEAPON_PART_PROFILE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearLearnedWeaponPartProfileV1) + sizeof(std::declval<RockProviderApi>().clearLearnedWeaponPartProfileV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_EMITTERS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, copyWeaponEmittersV1) + sizeof(std::declval<RockProviderApi>().copyWeaponEmittersV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_NATIVE_ANIMATION_AUTHORITY_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getNativeAnimationAuthorityStateV1) + sizeof(std::declval<RockProviderApi>().getNativeAnimationAuthorityStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_ANIMATION_PHASES_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, unregisterAnimationPhaseCallbackV1) + sizeof(std::declval<RockProviderApi>().unregisterAnimationPhaseCallbackV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_EQUIPPED_WEAPON_GRIP_STATE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getEquippedWeaponGripStateV1) + sizeof(std::declval<RockProviderApi>().getEquippedWeaponGripStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_HAND_VISUAL_AUTHORITY_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearHandVisualAuthorityV1) + sizeof(std::declval<RockProviderApi>().clearHandVisualAuthorityV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_NATIVE_ANIMATION_RUNTIME_PROVIDER_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, publishNativeAnimationRuntimeV1) + sizeof(std::declval<RockProviderApi>().publishNativeAnimationRuntimeV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_EQUIPPED_WEAPON_HANDLING_AUTHORITY_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getEquippedWeaponHandlingStateV1) + sizeof(std::declval<RockProviderApi>().getEquippedWeaponHandlingStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_DEBUG_OVERLAY_PUBLICATION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearDebugOverlayV1) + sizeof(std::declval<RockProviderApi>().clearDebugOverlayV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_PRESENTED_HAND_FRAMES_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getPresentedHandFrameV1) + sizeof(std::declval<RockProviderApi>().getPresentedHandFrameV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_EXTENDED_LIMITS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getPublicStructureSizeV1) + sizeof(std::declval<RockProviderApi>().getPublicStructureSizeV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_OWNER_FRAME_CALLBACKS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, unregisterFrameCallbackForOwnerV1) + sizeof(std::declval<RockProviderApi>().unregisterFrameCallbackForOwnerV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_HAND_INTERACTION_STATE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getHandInteractionStateV1) + sizeof(std::declval<RockProviderApi>().getHandInteractionStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_PROVIDER_EVENTS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, copyProviderEventsSinceV1) + sizeof(std::declval<RockProviderApi>().copyProviderEventsSinceV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_EQUIPPED_WEAPON_STATE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getEquippedWeaponStateV1) + sizeof(std::declval<RockProviderApi>().getEquippedWeaponStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_EXTERNAL_BODY_SCOPES_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, copyExternalContactsSinceV1) + sizeof(std::declval<RockProviderApi>().copyExternalContactsSinceV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_PART_OBSERVABILITY_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, copyWeaponPartDriveApplicationResultsV1) + sizeof(std::declval<RockProviderApi>().copyWeaponPartDriveApplicationResultsV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_SCOPE_SIGHT_STATE_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getScopeSightStateV1) + sizeof(std::declval<RockProviderApi>().getScopeSightStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_WEAPON_COMPOSITION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, copyWeaponCompositionEntriesV1) + sizeof(std::declval<RockProviderApi>().copyWeaponCompositionEntriesV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_POSE_READBACK_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getPresentedHandPoseV1) + sizeof(std::declval<RockProviderApi>().getPresentedHandPoseV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_SEMANTIC_HAND_CONTACTS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, copySemanticHandContactsV1) + sizeof(std::declval<RockProviderApi>().copySemanticHandContactsV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_PLAYER_COLLIDERS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getHandCollisionAvailabilityV1) + sizeof(std::declval<RockProviderApi>().getHandCollisionAvailabilityV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_COMMAND_CANCELLATION_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, cancelInteractionCommandV1) + sizeof(std::declval<RockProviderApi>().cancelInteractionCommandV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_INPUT_OBSERVABILITY_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getHandInputSuppressionStateV1) + sizeof(std::declval<RockProviderApi>().getHandInputSuppressionStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_OFFHAND_RESERVATION_LEASES_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, getOffhandReservationStateV1) + sizeof(std::declval<RockProviderApi>().getOffhandReservationStateV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_NATIVE_ANIMATION_RUNTIME_CLEAR_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, clearNativeAnimationRuntimeV1) + sizeof(std::declval<RockProviderApi>().clearNativeAnimationRuntimeV1));
    inline constexpr std::uint32_t ROCK_PROVIDER_API_V1_TOUCH_GRAB_TARGETS_TABLE_BYTES = static_cast<std::uint32_t>(
        offsetof(RockProviderApi, requestTouchGrabYieldV1) + sizeof(std::declval<RockProviderApi>().requestTouchGrabYieldV1));

    [[nodiscard]] inline bool queryProviderLimitsV1(RockProviderLimitsV1& outLimits)
    {
        constexpr auto limitsTableBytes = static_cast<std::uint32_t>(
            offsetof(RockProviderApi, getProviderLimitsV1) +
            sizeof(std::declval<RockProviderApi>().getProviderLimitsV1));
        if (!RockProviderApi::inst ||
            (RockProviderApi::negotiatedTableByteSize != 0 &&
                RockProviderApi::negotiatedTableByteSize < limitsTableBytes) ||
            !RockProviderApi::inst->getProviderLimitsV1) {
            return false;
        }

        outLimits = {};
        return RockProviderApi::inst->getProviderLimitsV1(&outLimits);
    }

    [[nodiscard]] inline bool queryProviderLimitsExtV1(RockProviderLimitsExtV1& outLimits)
    {
        if (!RockProviderApi::inst) {
            return false;
        }
        if (RockProviderApi::negotiatedTableByteSize != 0) {
            if (RockProviderApi::negotiatedTableByteSize <
                ROCK_PROVIDER_API_V1_EXTENDED_LIMITS_TABLE_BYTES) {
                return false;
            }
        } else {
            RockProviderLimitsV1 baseLimits{};
            if (!queryProviderLimitsV1(baseLimits) ||
                baseLimits.providerApiByteSize <
                    ROCK_PROVIDER_API_V1_EXTENDED_LIMITS_TABLE_BYTES) {
                return false;
            }
        }
        if (!RockProviderApi::inst->getProviderLimitsExtV1) {
            return false;
        }
        outLimits = {};
        return RockProviderApi::inst->getProviderLimitsExtV1(&outLimits);
    }

    [[nodiscard]] inline bool providerApiTableSupportsV1(const RockProviderLimitsV1& limits, std::uint32_t requiredByteSize)
    {
        return requiredByteSize != 0 && limits.providerApiByteSize >= requiredByteSize;
    }

    [[nodiscard]] inline bool providerApiTableSupportsV1(std::uint32_t requiredByteSize)
    {
        if (RockProviderApi::negotiatedTableByteSize != 0) {
            return requiredByteSize != 0 &&
                   RockProviderApi::negotiatedTableByteSize >= requiredByteSize;
        }
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && providerApiTableSupportsV1(limits, requiredByteSize);
    }

    [[nodiscard]] inline bool providerSupportsFeature2V1(
        std::uint32_t requiredByteSize,
        RockProviderFeatureBit2V1 feature)
    {
        if (!providerApiTableSupportsV1(requiredByteSize)) {
            return false;
        }
        if (RockProviderApi::negotiatedTableByteSize != 0) {
            return hasFeatureBit2V1(
                RockProviderApi::negotiatedFeatureBits2,
                feature);
        }
        RockProviderLimitsExtV1 limits{};
        return queryProviderLimitsExtV1(limits) &&
               hasFeatureBit2V1(limits.featureBits2, feature);
    }

    [[nodiscard]] inline bool supportsForceGrabCommandV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_FORCE_GRAB_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::InteractionCommandQueue) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::ForceGrabCommand);
    }

    [[nodiscard]] inline bool supportsForceGrabCommandV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsForceGrabCommandV1(limits);
    }

    [[nodiscard]] inline bool supportsForceReleaseCommandV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_FORCE_RELEASE_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::InteractionCommandQueue) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::ForceReleaseCommand);
    }

    [[nodiscard]] inline bool supportsForceReleaseCommandV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsForceReleaseCommandV1(limits);
    }

    [[nodiscard]] inline bool supportsThrownDropCommandV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_THROWN_DROP_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::InteractionCommandQueue) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::ThrownDropCommand);
    }

    [[nodiscard]] inline bool supportsThrownDropCommandV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsThrownDropCommandV1(limits);
    }

    [[nodiscard]] inline bool supportsHandInputSuppressionV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_HAND_INPUT_SUPPRESSION_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::HandInputSuppression);
    }

    [[nodiscard]] inline bool supportsHandInputSuppressionV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsHandInputSuppressionV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartInteractionV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_WEAPON_PART_INTERACTION_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartInteraction);
    }

    [[nodiscard]] inline bool supportsWeaponPartInteractionV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartInteractionV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartDrivePersistentLeaseV1(const RockProviderLimitsV1& limits)
    {
        return supportsWeaponPartInteractionV1(limits) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartDrivePersistentLease);
    }

    [[nodiscard]] inline bool supportsWeaponPartDrivePersistentLeaseV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartDrivePersistentLeaseV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartExclusiveExactContactV1(const RockProviderLimitsV1& limits)
    {
        return supportsWeaponPartInteractionV1(limits) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartExclusiveExactContact);
    }

    [[nodiscard]] inline bool supportsWeaponPartExclusiveExactContactV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartExclusiveExactContactV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartControlledRootV1(const RockProviderLimitsV1& limits)
    {
        return supportsWeaponPartInteractionV1(limits) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartControlledRoot);
    }

    [[nodiscard]] inline bool supportsWeaponPartControlledRootV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartControlledRootV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartInteractionZoneV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_WEAPON_PART_INTERACTION_ZONE_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartInteractionZone);
    }

    [[nodiscard]] inline bool supportsWeaponPartInteractionZoneV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartInteractionZoneV1(limits);
    }

    [[nodiscard]] inline bool supportsLearnedWeaponPartProfilesV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_LEARNED_WEAPON_PART_PROFILE_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::LearnedWeaponPartProfiles);
    }

    [[nodiscard]] inline bool supportsLearnedWeaponPartProfilesV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsLearnedWeaponPartProfilesV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartGripStateV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_WEAPON_PART_GRIP_STATE_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartGripState);
    }

    [[nodiscard]] inline bool supportsWeaponPartGripStateV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartGripStateV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartMotionConstraintV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_WEAPON_PART_MOTION_CONSTRAINT_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartMotionConstraint);
    }

    [[nodiscard]] inline bool supportsWeaponPartMotionConstraintV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartMotionConstraintV1(limits);
    }

    [[nodiscard]] inline bool supportsRawWandButtonStateV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_RAW_WAND_BUTTON_STATE_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::RawWandButtonState);
    }

    [[nodiscard]] inline bool supportsRawWandButtonStateV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsRawWandButtonStateV1(limits);
    }

    [[nodiscard]] inline bool supportsPipboyInputSuppressionV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_PIPBOY_INPUT_SUPPRESSION_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::PipboyInputSuppression);
    }

    [[nodiscard]] inline bool supportsPipboyInputSuppressionV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsPipboyInputSuppressionV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponPartRecordIdentityV1(const RockProviderLimitsV1& limits)
    {
        return hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponPartRecordIdentity);
    }

    [[nodiscard]] inline bool supportsWeaponPartRecordIdentityV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponPartRecordIdentityV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponClassificationV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(
            limits,
            ROCK_PROVIDER_API_V1_WEAPON_CLASSIFICATION_TABLE_BYTES);
    }

    [[nodiscard]] inline bool supportsWeaponClassificationV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponClassificationV1(limits);
    }

    [[nodiscard]] inline bool supportsWeaponEmittersV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_WEAPON_EMITTERS_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::WeaponEmitters);
    }

    [[nodiscard]] inline bool supportsWeaponEmittersV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsWeaponEmittersV1(limits);
    }

    [[nodiscard]] inline bool supportsNativeAnimationAuthorityV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_NATIVE_ANIMATION_AUTHORITY_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::NativeAnimationAuthority);
    }

    [[nodiscard]] inline bool supportsNativeAnimationAuthorityV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsNativeAnimationAuthorityV1(limits);
    }

    [[nodiscard]] inline bool supportsAnimationPhasesV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_ANIMATION_PHASES_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::AnimationPhases);
    }

    [[nodiscard]] inline bool supportsAnimationPhasesV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsAnimationPhasesV1(limits);
    }

    [[nodiscard]] inline bool supportsEquippedWeaponGripStateV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_EQUIPPED_WEAPON_GRIP_STATE_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::EquippedWeaponGripState);
    }

    [[nodiscard]] inline bool supportsEquippedWeaponGripStateV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsEquippedWeaponGripStateV1(limits);
    }

    [[nodiscard]] inline bool supportsHandVisualAuthorityV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_HAND_VISUAL_AUTHORITY_TABLE_BYTES) &&
               providerSupportsFeature2V1(
                   ROCK_PROVIDER_API_V1_HAND_VISUAL_AUTHORITY_TABLE_BYTES,
                   RockProviderFeatureBit2V1::HandVisualAuthority);
    }

    [[nodiscard]] inline bool supportsHandVisualAuthorityV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsHandVisualAuthorityV1(limits);
    }

    [[nodiscard]] inline bool supportsNativeAnimationRuntimeProviderV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_NATIVE_ANIMATION_RUNTIME_PROVIDER_TABLE_BYTES) &&
               providerSupportsFeature2V1(
                   ROCK_PROVIDER_API_V1_NATIVE_ANIMATION_RUNTIME_PROVIDER_TABLE_BYTES,
                   RockProviderFeatureBit2V1::NativeAnimationRuntimeProvider);
    }

    [[nodiscard]] inline bool supportsNativeAnimationRuntimeProviderV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsNativeAnimationRuntimeProviderV1(limits);
    }

    [[nodiscard]] inline bool supportsEquippedWeaponHandlingAuthorityV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_EQUIPPED_WEAPON_HANDLING_AUTHORITY_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::EquippedWeaponHandlingAuthority);
    }

    [[nodiscard]] inline bool supportsEquippedWeaponHandlingAuthorityV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsEquippedWeaponHandlingAuthorityV1(limits);
    }

    [[nodiscard]] inline bool supportsDebugOverlayPublicationV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_DEBUG_OVERLAY_PUBLICATION_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::DebugOverlayPublication);
    }

    [[nodiscard]] inline bool supportsDebugOverlayPublicationV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsDebugOverlayPublicationV1(limits);
    }

    [[nodiscard]] inline bool supportsPresentedHandFramesV1(const RockProviderLimitsV1& limits)
    {
        return providerApiTableSupportsV1(limits, ROCK_PROVIDER_API_V1_PRESENTED_HAND_FRAMES_TABLE_BYTES) &&
               hasFeatureBitV1(limits.featureBits, RockProviderFeatureBitV1::PresentedHandFrames);
    }

    [[nodiscard]] inline bool supportsPresentedHandFramesV1()
    {
        RockProviderLimitsV1 limits{};
        return queryProviderLimitsV1(limits) && supportsPresentedHandFramesV1(limits);
    }

    [[nodiscard]] inline bool supportsExtendedLimitsV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_EXTENDED_LIMITS_TABLE_BYTES,
            RockProviderFeatureBit2V1::ExtendedLimits);
    }

    [[nodiscard]] inline bool supportsOwnerFrameCallbacksV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_OWNER_FRAME_CALLBACKS_TABLE_BYTES,
            RockProviderFeatureBit2V1::OwnerFrameCallbacks);
    }

    [[nodiscard]] inline bool supportsHandInteractionStateV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_HAND_INTERACTION_STATE_TABLE_BYTES,
            RockProviderFeatureBit2V1::HandInteractionState);
    }

    [[nodiscard]] inline bool supportsProviderEventsV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_PROVIDER_EVENTS_TABLE_BYTES,
            RockProviderFeatureBit2V1::ProviderEvents);
    }

    [[nodiscard]] inline bool supportsEquippedWeaponStateV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_EQUIPPED_WEAPON_STATE_TABLE_BYTES,
            RockProviderFeatureBit2V1::EquippedWeaponState);
    }

    [[nodiscard]] inline bool supportsExternalBodyScopesV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_EXTERNAL_BODY_SCOPES_TABLE_BYTES,
            RockProviderFeatureBit2V1::ExternalBodyScopes);
    }

    [[nodiscard]] inline bool supportsWeaponPartObservabilityV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_WEAPON_PART_OBSERVABILITY_TABLE_BYTES,
            RockProviderFeatureBit2V1::WeaponPartDriveResults);
    }

    [[nodiscard]] inline bool supportsScopeSightStateV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_SCOPE_SIGHT_STATE_TABLE_BYTES,
            RockProviderFeatureBit2V1::ScopeSightState);
    }

    [[nodiscard]] inline bool supportsWeaponCompositionV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_WEAPON_COMPOSITION_TABLE_BYTES,
            RockProviderFeatureBit2V1::WeaponComposition);
    }

    [[nodiscard]] inline bool supportsPoseReadbackV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_POSE_READBACK_TABLE_BYTES,
            RockProviderFeatureBit2V1::PresentedHandPose);
    }

    [[nodiscard]] inline bool supportsSemanticHandContactsV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_SEMANTIC_HAND_CONTACTS_TABLE_BYTES,
            RockProviderFeatureBit2V1::SemanticHandContacts);
    }

    [[nodiscard]] inline bool supportsPlayerColliderDescriptorsV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_PLAYER_COLLIDERS_TABLE_BYTES,
            RockProviderFeatureBit2V1::PlayerColliderDescriptors);
    }

    [[nodiscard]] inline bool supportsCommandCancellationV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_COMMAND_CANCELLATION_TABLE_BYTES,
            RockProviderFeatureBit2V1::CommandCancellation);
    }

    [[nodiscard]] inline bool supportsInputObservabilityV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_INPUT_OBSERVABILITY_TABLE_BYTES,
            RockProviderFeatureBit2V1::InputSuppressionState);
    }

    [[nodiscard]] inline bool supportsOffhandReservationLeasesV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_OFFHAND_RESERVATION_LEASES_TABLE_BYTES,
            RockProviderFeatureBit2V1::OffhandReservationLeases);
    }

    [[nodiscard]] inline bool supportsTouchGrabTargetsV1()
    {
        return providerSupportsFeature2V1(
            ROCK_PROVIDER_API_V1_TOUCH_GRAB_TARGETS_TABLE_BYTES,
            RockProviderFeatureBit2V1::TouchGrabTargets);
    }

    static_assert(std::is_standard_layout_v<RockProviderTransform>);
    static_assert(std::is_trivially_copyable_v<RockProviderTransform>);
    static_assert(sizeof(RockProviderConsumerRegistrationV1) == 104);
    static_assert(alignof(RockProviderConsumerRegistrationV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderConsumerRegistrationV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderConsumerRegistrationV1>);
    static_assert(sizeof(RockProviderConsumerHandleV1) == 48);
    static_assert(alignof(RockProviderConsumerHandleV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderConsumerHandleV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderConsumerHandleV1>);
    static_assert(sizeof(RockProviderLimitsV1) == 92);
    static_assert(alignof(RockProviderLimitsV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderLimitsV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderLimitsV1>);
    static_assert(sizeof(RockProviderForceGrabRequestV1) == 80);
    static_assert(alignof(RockProviderForceGrabRequestV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderForceGrabRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderForceGrabRequestV1>);
    static_assert(sizeof(RockProviderForceReleaseRequestV1) == 72);
    static_assert(alignof(RockProviderForceReleaseRequestV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderForceReleaseRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderForceReleaseRequestV1>);
    static_assert(sizeof(RockProviderThrownDropRequestV1) == 96);
    static_assert(alignof(RockProviderThrownDropRequestV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderThrownDropRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderThrownDropRequestV1>);
    static_assert(sizeof(RockProviderInteractionCommandResultV1) == 120);
    static_assert(alignof(RockProviderInteractionCommandResultV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderInteractionCommandResultV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderInteractionCommandResultV1>);
    static_assert(sizeof(RockProviderHandInputSuppressionRequestV1) == 64);
    static_assert(alignof(RockProviderHandInputSuppressionRequestV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderHandInputSuppressionRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderHandInputSuppressionRequestV1>);
    static_assert(sizeof(RockProviderNativeAnimationAuthorityRequestV1) == 64);
    static_assert(alignof(RockProviderNativeAnimationAuthorityRequestV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderNativeAnimationAuthorityRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderNativeAnimationAuthorityRequestV1>);
    static_assert(sizeof(RockProviderNativeAnimationAuthorityStateV1) == 64);
    static_assert(alignof(RockProviderNativeAnimationAuthorityStateV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderNativeAnimationAuthorityStateV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderNativeAnimationAuthorityStateV1>);
    static_assert(sizeof(RockProviderAnimationPhaseContextV1) == 72);
    static_assert(alignof(RockProviderAnimationPhaseContextV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderAnimationPhaseContextV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderAnimationPhaseContextV1>);
    static_assert(sizeof(RockProviderEquippedWeaponGripStateV1) == 224);
    static_assert(alignof(RockProviderEquippedWeaponGripStateV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderEquippedWeaponGripStateV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderEquippedWeaponGripStateV1>);
    static_assert(sizeof(RockProviderHandVisualAuthorityRequestV1) == 888);
    static_assert(alignof(RockProviderHandVisualAuthorityRequestV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderHandVisualAuthorityRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderHandVisualAuthorityRequestV1>);
    static_assert(sizeof(RockProviderNativeAnimationRuntimePublicationV1) == 64);
    static_assert(alignof(RockProviderNativeAnimationRuntimePublicationV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderNativeAnimationRuntimePublicationV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderNativeAnimationRuntimePublicationV1>);
    static_assert(sizeof(RockProviderDebugOverlayLineV1) == 56);
    static_assert(alignof(RockProviderDebugOverlayLineV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderDebugOverlayLineV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderDebugOverlayLineV1>);
    static_assert(sizeof(RockProviderDebugOverlayTextV1) == 200);
    static_assert(alignof(RockProviderDebugOverlayTextV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderDebugOverlayTextV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderDebugOverlayTextV1>);
    static_assert(sizeof(RockProviderDebugOverlayPublicationV1) == 64);
    static_assert(alignof(RockProviderDebugOverlayPublicationV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderDebugOverlayPublicationV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderDebugOverlayPublicationV1>);
    static_assert(sizeof(RockProviderEquippedWeaponHandlingRequestV1) == 128);
    static_assert(alignof(RockProviderEquippedWeaponHandlingRequestV1) == 4);
    static_assert(std::is_standard_layout_v<RockProviderEquippedWeaponHandlingRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderEquippedWeaponHandlingRequestV1>);
    static_assert(sizeof(RockProviderEquippedWeaponHandlingStateV1) == 88);
    static_assert(alignof(RockProviderEquippedWeaponHandlingStateV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderEquippedWeaponHandlingStateV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderEquippedWeaponHandlingStateV1>);
    static_assert(sizeof(RockProviderRawWandButtonStateV1) == 32);
    static_assert(alignof(RockProviderRawWandButtonStateV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderRawWandButtonStateV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderRawWandButtonStateV1>);
    static_assert(sizeof(RockProviderWeaponPartTargetV1) == 160);
    static_assert(alignof(RockProviderWeaponPartTargetV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderWeaponPartTargetV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponPartTargetV1>);
    static_assert(sizeof(RockProviderTransform) == 52);
    static_assert(sizeof(RockProviderWeaponPartDriveTargetV1) == 192);
    static_assert(alignof(RockProviderWeaponPartDriveTargetV1) == 8);
    static_assert(offsetof(RockProviderWeaponPartDriveTargetV1, controlledRoot) == 168);
    static_assert(std::is_standard_layout_v<RockProviderWeaponPartDriveTargetV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponPartDriveTargetV1>);
    static_assert(sizeof(RockProviderWeaponPartMotionConstraintV1) == 192);
    static_assert(alignof(RockProviderWeaponPartMotionConstraintV1) == 8);
    static_assert(offsetof(RockProviderWeaponPartMotionConstraintV1, controlledRoot) == 168);
    static_assert(std::is_standard_layout_v<RockProviderWeaponPartMotionConstraintV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponPartMotionConstraintV1>);
    static_assert(sizeof(RockProviderWeaponPartMotionConstraintResolutionV1) == 64);
    static_assert(alignof(RockProviderWeaponPartMotionConstraintResolutionV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderWeaponPartMotionConstraintResolutionV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponPartMotionConstraintResolutionV1>);
    static_assert(std::is_standard_layout_v<RockProviderWeaponPartInteractionZoneV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponPartInteractionZoneV1>);
    static_assert(std::is_standard_layout_v<RockProviderLearnedWeaponPartProfileV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderLearnedWeaponPartProfileV1>);
    static_assert(std::is_standard_layout_v<RockProviderLearnedWeaponPartActivationRequestV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderLearnedWeaponPartActivationRequestV1>);
    static_assert(std::is_standard_layout_v<RockProviderLearnedWeaponPartActivationResultV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderLearnedWeaponPartActivationResultV1>);
    static_assert(sizeof(RockProviderWeaponClassificationV1) == 48);
    static_assert(alignof(RockProviderWeaponClassificationV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderWeaponClassificationV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponClassificationV1>);
    static_assert(sizeof(RockProviderWeaponPartGripStateV1) == 248);
    static_assert(alignof(RockProviderWeaponPartGripStateV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderWeaponPartGripStateV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponPartGripStateV1>);
    static_assert(sizeof(RockProviderFrameSnapshot) == 376);
    static_assert(alignof(RockProviderFrameSnapshot) == 8);
    static_assert(sizeof(RockProviderHandFrameV1) == 144);
    static_assert(alignof(RockProviderHandFrameV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderHandFrameV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderHandFrameV1>);
    static_assert(sizeof(RockProviderExternalBodyRegistration) == 32);
    static_assert(sizeof(RockProviderExternalContactV1) == 128);
    static_assert(alignof(RockProviderExternalContactV1) == 8);
    static_assert(sizeof(RockProviderPoint3) == 12);
    static_assert(sizeof(RockProviderBounds3) == 32);
    static_assert(sizeof(RockProviderWeaponEvidenceDetailV1) == 192);
    static_assert(alignof(RockProviderWeaponEvidenceDetailV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderWeaponEvidenceDetailV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponEvidenceDetailV1>);
    static_assert(sizeof(RockProviderWeaponEmitterV1) == 208);
    static_assert(alignof(RockProviderWeaponEmitterV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderWeaponEmitterV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderWeaponEmitterV1>);
    static_assert(sizeof(RockProviderBodyContactV1) == 128);
    static_assert(alignof(RockProviderBodyContactV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderBodyContactV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderBodyContactV1>);
    static_assert(sizeof(RockProviderTouchGrabTargetV1) == 128);
    static_assert(alignof(RockProviderTouchGrabTargetV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderTouchGrabTargetV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderTouchGrabTargetV1>);
    static_assert(sizeof(RockProviderTouchGrabStateV1) == 136);
    static_assert(alignof(RockProviderTouchGrabStateV1) == 8);
    static_assert(std::is_standard_layout_v<RockProviderTouchGrabStateV1>);
    static_assert(std::is_trivially_copyable_v<RockProviderTouchGrabStateV1>);
    static_assert(sizeof(RockProviderApi) == 752);
    static_assert(alignof(RockProviderApi) == 8);
    /*
     * Upstream's own spot-check anchors on its append region.
     *
     * RE-ANCHORED (+8 slots), NOT WEAKENED. These are still exact `==`
     * equality tests on offsetof; only the expected index changed.
     *
     * Why: upstream authored these indices when its additions began at slot
     * 37, i.e. when the two weapon-part motion-constraint members and the six
     * Heisenberg-0.8.4 members had been displaced to the END of the table.
     * That displacement broke Cylon's frozen ABI prefix (slots 0..38 /
     * 0x000..0x137), which is a hard compatibility contract with an already-
     * shipped consumer binary we cannot recompile. Those eight members have
     * been restored to their contractual slots 33..44, which pushes upstream's
     * 49 additions down by exactly 8 slots (+0x40). That shift is free because
     * upstream is compiled from source in this tree.
     *
     * The shift is uniform and was verified mechanically over the whole
     * declaration list, not inferred from these three members:
     *     getProviderLimitsExtV1         54 -> 62   (0x1F0)
     *     clearNativeAnimationRuntimeV1  81 -> 89   (0x2C8)
     *     requestTouchGrabYieldV1        85 -> 93   (0x2E8)
     * every upstream member moved +8, no member moved by any other amount,
     * and the table size is unchanged at 94 slots / 752 bytes (the
     * `sizeof(RockProviderApi) == 752` assert above still holds untouched,
     * which is the independent confirmation that this was a pure reorder and
     * not an insertion).
     *
     * The authoritative pins for the frozen region are the 48 offsetof
     * asserts further up this header; these three remain as upstream's
     * independent check that its own append region has not drifted.
     */
    static_assert(
        offsetof(RockProviderApi, getProviderLimitsExtV1) == 62 * sizeof(void*));
    static_assert(
        offsetof(RockProviderApi, clearNativeAnimationRuntimeV1) ==
        89 * sizeof(void*));
    static_assert(
        offsetof(RockProviderApi, requestTouchGrabYieldV1) ==
        93 * sizeof(void*));
}
