#pragma once

// Build-3 extension for Heisenberg's revision-1 public interface.  Everything
// in this header is append-only ABI: do not insert a virtual before the build-3
// tail in HeisenbergInterface001.h.

#include "RE/Fallout.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace HeisenbergPluginAPI
{
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
        // Candidate queries use formID as input. The provider echoes the query
        // bit and sets kHandStateCandidateEligible only for a safe indexed equip.
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

    [[nodiscard]] inline DualWieldHandState MakeHandState(PhysicalHand hand) noexcept
    {
        return {
            kDualWieldHandStateVersion,
            static_cast<std::uint32_t>(sizeof(DualWieldHandState)),
            hand,
            ItemCategory::kNone,
            kHandStateNone,
            0,
            0,
            0,
            0,
            nullptr
        };
    }

    [[nodiscard]] inline PhysicalHandInputState MakeInputState(PhysicalHand hand) noexcept
    {
        return {
            kPhysicalHandInputStateVersion,
            static_cast<std::uint32_t>(sizeof(PhysicalHandInputState)),
            hand,
            kInputStateNone,
            0.0F,
            0.0F,
            {},
            {}
        };
    }

    [[nodiscard]] inline DualWieldContact MakeContact(PhysicalHand hand) noexcept
    {
        return {
            kDualWieldContactVersion,
            static_cast<std::uint32_t>(sizeof(DualWieldContact)),
            hand,
            kContactNone,
            0,
            0,
            {},
            {},
            {},
            0.0F
        };
    }

    [[nodiscard]] constexpr bool IsPhysicalHand(PhysicalHand hand) noexcept
    {
        return hand == PhysicalHand::kLeft || hand == PhysicalHand::kRight;
    }

    [[nodiscard]] constexpr bool HasValidHeader(const DualWieldHandState& state) noexcept
    {
        return state.abiVersion == kDualWieldHandStateVersion &&
               state.structSize == sizeof(DualWieldHandState) &&
               IsPhysicalHand(state.physicalHand);
    }

    [[nodiscard]] constexpr bool HasValidHeader(const PhysicalHandInputState& state) noexcept
    {
        return state.abiVersion == kPhysicalHandInputStateVersion &&
               state.structSize == sizeof(PhysicalHandInputState) &&
               IsPhysicalHand(state.physicalHand);
    }

    [[nodiscard]] constexpr bool HasValidHeader(const DualWieldContact& contact) noexcept
    {
        return contact.abiVersion == kDualWieldContactVersion &&
               contact.structSize == sizeof(DualWieldContact) &&
               IsPhysicalHand(contact.physicalHand);
    }

    using DualWieldStateProvider = bool (*)(PhysicalHand hand, DualWieldHandState* state);
    using DualWieldWeaponContactCallback = void (*)(const DualWieldContact* contact);

    // Interface implementation helpers. QueryDualWieldHandState is main-thread
    // only. InvokeDualWieldWeaponContact is safe on the Havok callback thread.
    bool RegisterDualWieldStateProviderImpl(DualWieldStateProvider provider);
    bool UnregisterDualWieldStateProviderImpl(DualWieldStateProvider provider);
    bool GetPhysicalHandInputStateImpl(PhysicalHand hand, PhysicalHandInputState* state);
    bool RegisterDualWieldWeaponContactCallbackImpl(DualWieldWeaponContactCallback callback);
    bool UnregisterDualWieldWeaponContactCallbackImpl(DualWieldWeaponContactCallback callback);

    bool HasDualWieldStateProvider();
    bool QueryDualWieldHandState(PhysicalHand hand, DualWieldHandState& state);
    void InvokeDualWieldWeaponContact(const DualWieldContact& contact);

    static_assert(sizeof(PhysicalHand) == sizeof(std::uint32_t));
    static_assert(sizeof(ItemCategory) == sizeof(std::uint32_t));
    static_assert(sizeof(RE::NiPoint3) == 12);
    static_assert(std::is_standard_layout_v<DualWieldHandState>);
    static_assert(std::is_trivially_copyable_v<DualWieldHandState>);
    static_assert(sizeof(DualWieldHandState) == 48);
    static_assert(offsetof(DualWieldHandState, instanceID) == 32);
    static_assert(offsetof(DualWieldHandState, weaponRoot) == 40);
    static_assert(std::is_standard_layout_v<PhysicalHandInputState>);
    static_assert(std::is_trivially_copyable_v<PhysicalHandInputState>);
    static_assert(sizeof(PhysicalHandInputState) == 48);
    static_assert(offsetof(PhysicalHandInputState, linearVelocity) == 24);
    static_assert(std::is_standard_layout_v<DualWieldContact>);
    static_assert(std::is_trivially_copyable_v<DualWieldContact>);
    static_assert(sizeof(DualWieldContact) == 64);
    static_assert(offsetof(DualWieldContact, contactPoint) == 24);
    static_assert(offsetof(DualWieldContact, relativeVelocity) == 48);
}
