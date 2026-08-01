#pragma once

/**
 * HeisenbergAPI_Standalone.h - Standalone consumer header for Heisenberg F4VR API
 *
 * Use this header if you are NOT using CommonLibF4 (RE:: headers).
 * If you ARE using CommonLibF4, use HeisenbergInterface001.h instead.
 *
 * This header provides:
 *   - Forward declarations matching the RE:: ABI layout
 *   - The IHeisenbergInterface001 vtable interface
 *   - Two ways to acquire the interface: F4SE messaging or DLL export
 *
 * Quick start (IsHoldingObject only):
 *
 *   #include "HeisenbergAPI_Standalone.h"
 *
 *   // In your F4SE plugin init or message handler:
 *   auto* h = HeisenbergPluginAPI::GetInterfaceFromDLL();
 *   if (h && h->IsHoldingObject(false)) {
 *       // right hand is holding something
 *   }
 */

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <Windows.h>

// ============================================================================
// Forward declarations that match CommonLibF4's ABI
// These are opaque pointers — you don't need full definitions to call the API.
// If you need to inspect the returned objects, cast to your own F4SE types.
// ============================================================================
namespace RE {
    class TESObjectREFR;  // Game object reference (NPC, item in world, etc.)
    class TESForm;        // Base form (item template)
    class NiAVObject;
    struct NiPoint3 { float x, y, z; };

    // NiPoint4: 16 bytes, used by NiMatrix3
    struct NiPoint4 { float x, y, z, w; };

    // NiMatrix3: 3 rows of NiPoint4 = 0x30 bytes, aligned to 16
    struct alignas(0x10) NiMatrix3 { NiPoint4 entry[3]; };
    static_assert(sizeof(NiMatrix3) == 0x30);

    // NiTransform: rotation matrix + translation + scale = 0x40 bytes
    struct NiTransform {
        NiMatrix3 rotate;         // 0x00
        NiPoint3  translate;      // 0x30
        float     scale = 1.0f;   // 0x3C
    };
    static_assert(sizeof(NiTransform) == 0x40);
}

namespace HeisenbergPluginAPI {

    // Build-3 dual-wield bridge ABI. These declarations intentionally mirror
    // src/DualWieldAPI.h without requiring CommonLibF4.
    enum class PhysicalHand : std::uint32_t
    {
        kLeft = 0,
        kRight = 1,
        kInvalid = 0xFFFFFFFFu
    };

    enum class ItemCategory : std::uint32_t
    {
        kNone = 0,
        kFirearm = 1,
        kMelee = 2,
        kThrowable = 3
    };

    enum HandStateFlag : std::uint32_t
    {
        kHandStateNone = 0,
        kHandStateOccupied = 1u << 0,
        kHandStateWeaponRootValid = 1u << 1,
        kHandStatePowerArmor = 1u << 2,
        kHandStateCandidateQuery = 1u << 3,
        kHandStateCandidateEligible = 1u << 4
    };

    enum InputStateFlag : std::uint32_t
    {
        kInputStateNone = 0,
        kInputTrackingValid = 1u << 0,
        kInputTriggerDown = 1u << 1,
        kInputTriggerPressed = 1u << 2,
        kInputTriggerReleased = 1u << 3,
        kInputGripDown = 1u << 4,
        kInputGripPressed = 1u << 5,
        kInputGripReleased = 1u << 6
    };

    enum ContactFlag : std::uint32_t
    {
        kContactNone = 0,
        kContactTargetHandleValid = 1u << 0
    };

    inline constexpr std::uint32_t kDualWieldHandStateVersion = 1;
    inline constexpr std::uint32_t kPhysicalHandInputStateVersion = 1;
    inline constexpr std::uint32_t kDualWieldContactVersion = 1;

    struct DualWieldHandState
    {
        std::uint32_t abiVersion;
        std::uint32_t structSize;
        PhysicalHand physicalHand;
        ItemCategory itemCategory;
        std::uint32_t flags;
        std::uint32_t equipIndex;
        std::uint32_t formID;
        std::uint32_t reserved;
        std::uint64_t instanceID;
        RE::NiAVObject* weaponRoot;
    };

    struct PhysicalHandInputState
    {
        std::uint32_t abiVersion;
        std::uint32_t structSize;
        PhysicalHand physicalHand;
        std::uint32_t flags;
        float triggerValue;
        float gripValue;
        RE::NiPoint3 linearVelocity;
        RE::NiPoint3 angularVelocity;
    };

    struct DualWieldContact
    {
        std::uint32_t abiVersion;
        std::uint32_t structSize;
        PhysicalHand physicalHand;
        std::uint32_t flags;
        std::uint32_t targetHandle;
        std::uint32_t reserved;
        RE::NiPoint3 contactPoint;
        RE::NiPoint3 contactNormal;
        RE::NiPoint3 relativeVelocity;
        float separatingVelocity;
    };

    using DualWieldStateProvider =
        bool (*)(PhysicalHand hand, DualWieldHandState* state);
    using DualWieldWeaponContactCallback =
        void (*)(const DualWieldContact* contact);

    // ========================================================================
    // IHeisenbergInterface001 — pure virtual interface (vtable)
    //
    // The vtable layout MUST match HeisenbergInterface001.h exactly.
    // Do not reorder, add, or remove virtual functions.
    // ========================================================================
    struct IHeisenbergInterface001
    {
        // --- Version ---
        virtual unsigned int GetBuildNumber() = 0;

        // --- Grab State ---
        virtual bool IsHoldingObject(bool isLeft) = 0;
        virtual bool IsPulling(bool isLeft) = 0;
        virtual bool CanGrabObject(bool isLeft) = 0;
        virtual RE::TESObjectREFR* GetGrabbedObject(bool isLeft) = 0;
        virtual const char* GetGrabbedNodeName(bool isLeft) = 0;

        // --- ViewCaster / Selection ---
        virtual RE::TESObjectREFR* GetViewCasterTarget(bool isLeft) = 0;
        virtual RE::TESObjectREFR* GetPrimaryWandTarget() = 0;
        virtual RE::TESObjectREFR* GetSecondaryWandTarget() = 0;
        virtual RE::TESObjectREFR* GetSelectedObject(bool isLeft) = 0;

        // --- Grab Control ---
        virtual bool GrabObject(RE::TESObjectREFR* object, bool isLeft) = 0;
        virtual void DropObject(bool isLeft, const RE::NiPoint3* throwVelocity = nullptr) = 0;
        virtual void ForceEndGrab(bool isLeft) = 0;

        // --- Hand Enable/Disable ---
        virtual void DisableHand(bool isLeft) = 0;
        virtual void EnableHand(bool isLeft) = 0;
        virtual bool IsHandDisabled(bool isLeft) = 0;

        // --- Finger Tracking ---
        virtual void GetFingerCurls(bool isLeft, float values[5]) = 0;
        virtual void SetFingerCurls(bool isLeft, const float values[5]) = 0;

        // --- Zone Detection ---
        virtual bool IsInStorageZone(bool isLeft) = 0;
        virtual bool IsInEquipZone(bool isLeft) = 0;
        virtual bool IsInMouthZone(bool isLeft) = 0;
        virtual bool IsInVHZone(bool isLeft) = 0;
        virtual const char* GetCurrentZoneName(bool isLeft) = 0;
        virtual int GetVHZoneIndex(bool isLeft) = 0;

        // --- Inventory ---
        virtual bool DropToHand(RE::TESForm* form, bool isLeft) = 0;
        virtual bool IsInActivationZone(bool isLeft) = 0;

        // --- Transform ---
        virtual RE::NiTransform GetGrabTransform(bool isLeft) = 0;
        virtual void SetGrabTransform(bool isLeft, const RE::NiTransform& transform) = 0;

        // --- Callbacks ---
        typedef void(*GrabbedCallback)(bool isLeft, RE::TESObjectREFR* grabbedRefr);
        typedef void(*DroppedCallback)(bool isLeft, RE::TESObjectREFR* droppedRefr);
        typedef void(*StashedCallback)(bool isLeft, RE::TESForm* stashedForm);
        typedef void(*ConsumedCallback)(bool isLeft, RE::TESForm* consumedForm);
        typedef void(*PulledCallback)(bool isLeft, RE::TESObjectREFR* pulledRefr);
        typedef void(*CollisionCallback)(bool isLeft, float mass, float separatingVelocity);
        typedef void(*PrePhysicsCallback)(void* bhkWorld);
        typedef void(*PostPhysicsCallback)(void* bhkWorld);
        typedef void(*ViewCasterTargetChangedCallback)(bool isLeft, RE::TESObjectREFR* newTarget, RE::TESObjectREFR* oldTarget);

        virtual void AddGrabbedCallback(GrabbedCallback callback) = 0;
        virtual void AddDroppedCallback(DroppedCallback callback) = 0;
        virtual void AddStashedCallback(StashedCallback callback) = 0;
        virtual void AddConsumedCallback(ConsumedCallback callback) = 0;
        virtual void AddPulledCallback(PulledCallback callback) = 0;
        virtual void AddCollisionCallback(CollisionCallback callback) = 0;
        virtual void AddPrePhysicsCallback(PrePhysicsCallback callback) = 0;
        virtual void AddPostPhysicsCallback(PostPhysicsCallback callback) = 0;
        virtual void AddViewCasterTargetChangedCallback(ViewCasterTargetChangedCallback callback) = 0;

        // --- Settings ---
        virtual bool GetSettingDouble(const char* name, double& out) = 0;
        virtual bool SetSettingDouble(const char* name, double val) = 0;

        // --- Hand Collision ---
        virtual bool IsHandCollisionEnabled() = 0;
        virtual void* GetHandRigidBody(bool isLeft) = 0;
        virtual bool IsHandInContact(bool isLeft) = 0;
        virtual RE::TESObjectREFR* GetHandContactObject(bool isLeft) = 0;

        // --- Build 1 tail ---
        virtual void SuppressItemToHand(unsigned int durationMs) = 0;

        // --- Build 2 tail (guard every call with GetBuildNumber() >= 2) ---
        virtual void EnableWeaponCollision(bool enable) = 0;
        virtual bool IsWeaponCollisionDisabled() = 0;
        virtual void BlockOffHandWeaponGripping(const char* tag, bool block) = 0;
        virtual bool IsOffHandWeaponGrippingBlocked() = 0;
        virtual bool IsOffHandGrippingWeapon() = 0;
        virtual void DisableHandCollision(bool isLeft) = 0;
        virtual void EnableHandCollision(bool isLeft) = 0;
        virtual bool IsHandCollisionDisabled(bool isLeft) = 0;

        // --- Build 3 tail (guard every call with GetBuildNumber() >= 3) ---
        virtual bool RegisterDualWieldStateProvider(DualWieldStateProvider provider) = 0;
        virtual bool UnregisterDualWieldStateProvider(DualWieldStateProvider provider) = 0;
        virtual bool GetPhysicalHandInputState(
            PhysicalHand hand,
            PhysicalHandInputState* state) = 0;
        virtual bool RegisterDualWieldWeaponContactCallback(
            DualWieldWeaponContactCallback callback) = 0;
        virtual bool UnregisterDualWieldWeaponContactCallback(
            DualWieldWeaponContactCallback callback) = 0;
    };

    static_assert(sizeof(RE::NiPoint3) == 12);
    static_assert(sizeof(DualWieldHandState) == 48);
    static_assert(offsetof(DualWieldHandState, instanceID) == 32);
    static_assert(offsetof(DualWieldHandState, weaponRoot) == 40);
    static_assert(sizeof(PhysicalHandInputState) == 48);
    static_assert(offsetof(PhysicalHandInputState, linearVelocity) == 24);
    static_assert(sizeof(DualWieldContact) == 64);
    static_assert(offsetof(DualWieldContact, contactPoint) == 24);
    static_assert(offsetof(DualWieldContact, relativeVelocity) == 48);

    // ========================================================================
    // Method 1: Get interface via DLL export (simplest, no F4SE messaging needed)
    //
    // Heisenberg exports "GetHeisenbergAPI" as a C function.
    // Call with revision=1 to get IHeisenbergInterface001*.
    // ========================================================================
    inline IHeisenbergInterface001* GetInterfaceFromDLL()
    {
        HMODULE hMod = GetModuleHandleA("Heisenberg_F4VR.dll");
        if (!hMod) return nullptr;

        using GetApiFn = void*(*)(unsigned int);
        auto getApi = reinterpret_cast<GetApiFn>(GetProcAddress(hMod, "GetHeisenbergAPI"));
        if (!getApi) return nullptr;

        return static_cast<IHeisenbergInterface001*>(getApi(1));
    }

} // namespace HeisenbergPluginAPI
