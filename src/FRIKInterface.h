#pragma once

/**
 * FRIK Interface for Heisenberg
 * 
 * Thin wrapper around the official FRIK v0.77 API (frik::api::FRIKApi).
 * Uses the official FRIKApi.h header from:
 *   https://github.com/rollingrock/Fallout-4-VR-Body/releases/tag/v0.77
 *
 * Provides:
 *  - Singleton access pattern matching the rest of Heisenberg
 *  - bool isLeft → FRIKApi::Hand conversion
 *  - Null-safety checks on every call
 *  - Tagged hand pose management via HEISENBERG_HAND_POSE_TAG
 */

#include "api/FRIKApi.h"

namespace heisenberg
{
    // Convenient aliases for the official FRIK types
    using FRIKHand            = frik::host_api::FRIKApi::Hand;
    using FRIKHandPoses       = frik::host_api::FRIKApi::HandPoseKind;  // v5 renamed HandPoses -> HandPoseKind
    using FRIKHandPoseTagState = frik::host_api::FRIKApi::HandPoseTagState;

    // Tag used for Heisenberg hand pose overrides
    constexpr const char* HEISENBERG_HAND_POSE_TAG = "Heisenberg_Grab";

    // Tag used for the v9 external-hand-world-transform override (wall pushback).
    constexpr const char* HEISENBERG_HAND_PUSHBACK_TAG = "Heisenberg_HandPushback";

    // DEBUG: Set to true to disable all FRIK API calls (for crash debugging)
    constexpr bool DEBUG_DISABLE_FRIK_API = false;

    /**
     * Interface to communicate with FRIK mod.
     * Requires FRIK 0.77+ (API version 2).
     */
    class FRIKInterface
    {
    public:
        static FRIKInterface& GetSingleton()
        {
            static FRIKInterface instance;
            return instance;
        }

        // Initialize the interface - call after mods are loaded
        bool Initialize();

        // Check if FRIK is available and ready
        bool IsAvailable() const;

        // Get the detected API version (0 if not initialized)
        std::uint32_t GetApiVersion() const;

        // Get the index finger tip position for a hand (isLeft: true=left hand, false=right hand)
        bool GetIndexFingerTipPosition(bool isLeft, RE::NiPoint3& outPos) const;

        // Get the hand position (uses finger tip position - FRIK doesn't expose hand bone directly)
        bool GetHandPosition(bool isLeft, RE::NiPoint3& outPos) const;

        // Set hand pose override with per-finger positions (5 values: 0=bent, 1=straight)
        // Uses tag-based API (setHandPoseCustomFingerPositions)
        bool SetHandPoseFingerPositions(bool isLeft, float thumb, float index, float middle, float ring, float pinky) const;

        // Set hand pose override with per-joint positions (15 values: 5 fingers x 3 joints)
        // Averages 3 joints per finger and uses 5-value API
        bool SetHandPoseJointPositions(bool isLeft, const float values[15]) const;

        // Clear hand pose override (releases control back to FRIK's controller tracking)
        bool ClearHandPoseFingerPositions(bool isLeft) const;

        // Check if any FRIK config UI is open
        bool IsConfigOpen() const;

        // Check if offhand is gripping weapon (two-handed mode)
        bool IsOffHandGrippingWeapon() const;

        // Get FRIK mod version string (e.g. "0.77.1")
        const char* GetModVersion() const;

        // API v2-v4 have no runtime blocker for FRIK's own two-handed weapon solver.
        // When embedded ROCK owns that job, enforce the live FRIK INI setting instead.
        bool ReconcileLegacyOffHandGrippingIni() const;

        // --- v9 hand pushback API ---------------------------------------------------------
        // True only when the loaded FRIK is API v9+ (the external-hand-transform functions
        // exist). Core FRIK features work on older FRIK; pushback requires v9.
        bool SupportsPushback() const;
        // Read the rendered hand's current world transform (FRIK's IK result this frame).
        bool GetHandWorldTransform(bool isLeft, RE::NiTransform& outWorld) const;
        // Override the rendered hand to a world target — FRIK solves the arm to it. Used to
        // push the visible hand out of walls. Re-call each frame while overriding.
        bool ApplyExternalHandWorldTransform(bool isLeft, const RE::NiTransform& world, int priority = 10) const;
        // Release the override so FRIK resumes normal controller tracking for that hand.
        bool ClearExternalHandWorldTransform(bool isLeft) const;

    private:
        FRIKInterface() = default;
        ~FRIKInterface() = default;

        FRIKInterface(const FRIKInterface&) = delete;
        FRIKInterface& operator=(const FRIKInterface&) = delete;

        bool _initialized = false;

        // Helper: convert bool isLeft to FRIK Hand enum
        static FRIKHand ToHand(bool isLeft) { return isLeft ? FRIKHand::Left : FRIKHand::Right; }

        // Helper: get the API pointer (null if not initialized)
        const frik::host_api::FRIKApi* Api() const { return frik::host_api::FRIKApi::inst; }
    };
}
