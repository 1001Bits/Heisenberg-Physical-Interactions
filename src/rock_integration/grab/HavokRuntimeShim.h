#pragma once

// The ONE engine adapter for the ported grab-constraint TU. ROCK's GrabConstraint.cpp calls a
// `heisenberg::rock_core::havok_runtime` API (allocateHavok/freeHavok/getBody/getMotion/rebuildMotionMassProperties)
// that ROCK ships as a stub; here those 5 forward to Heisenberg's already-proven engine plumbing,
// so the verbatim constraint code runs unchanged. All offsets are FO4VR 1.2.72, cross-checked
// against Heisenberg's Physics.cpp body/motion accessors.

#include "GrabConstraint.h"          // heisenberg::ConstraintFunctions::HkHeapAlloc / HkHeapFree
#include "HavokPhysicsKeyframe.h"    // heisenberg::hknpBSWorld_accessMotion (REL 0x1df5ba0)
#include "rock_integration/grab/PhysicsScale.h"  // heisenberg::rock_core::physics_scale (game<->Havok units)

#include "RE/Havok/hknpBodyId.h"

#include <REL/Relocation.h>
#include <cstddef>
#include <cstdint>

namespace heisenberg::rock_core::havok_runtime
{
    // FO4VR body/motion layout (Heisenberg Physics.cpp-confirmed).
    inline constexpr std::uint32_t  kBodyIdIndexMask        = 0x0000FFFFu;  // GetBodyBufferIndex mask
    inline constexpr std::uint32_t  kMaxReasonableBodyIndex = 0x00100000u;
    inline constexpr std::uintptr_t kWorldBodyBufferOffset  = 0x20;         // hknpWorld -> bodies.data ptr
    inline constexpr std::uintptr_t kBodyStride             = 0x90;         // sizeof(hknpBody slot)
    inline constexpr std::size_t    kBodyMotionIndexOffset  = 0x68;         // hknpBody::motionIndex

    // Thin view of the FO4VR hknpBody slot — every field offset Ghidra-verified (Physics.cpp
    // hknpBody_Offsets: transform 0x00, flags 0x40, collisionFilterInfo 0x44, motionId 0x68,
    // stride 0x90). The transform's translation column sits at +0x30 (Havok hkTransform layout);
    // ROCK reads it as bodyFloats[12..14]. Avoids coupling to CommonLibF4's hknpBody.
    struct BodyView
    {
        float         transformColumns[12];   // 0x00..0x30: rotation columns (3 x hkVector4)
        float         position[4];            // 0x30..0x40: translation (xyz, Havok scale) + w
        std::uint32_t flags;                  // 0x40: hknpBody flags (motion-type evidence)
        std::uint32_t collisionFilterInfo;    // 0x44: layer in low 7 bits
        unsigned char _pad48[kBodyMotionIndexOffset - 0x48];
        std::uint32_t motionIndex;            // 0x68
    };
    static_assert(offsetof(BodyView, position) == 0x30, "position must sit at +0x30");
    static_assert(offsetof(BodyView, flags) == 0x40, "flags must sit at +0x40");
    static_assert(offsetof(BodyView, collisionFilterInfo) == 0x44, "collisionFilterInfo must sit at +0x44");
    static_assert(offsetof(BodyView, motionIndex) == kBodyMotionIndexOffset, "motionIndex must sit at +0x68");

    // hknpMotion::motionPropertiesId (uint16 @ motion+0x02 — Heisenberg Offsets, Ghidra-verified).
    inline std::uint16_t motionPropertiesIdOf(const void* motion)
    {
        return motion ? *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<std::uintptr_t>(motion) + 0x02) : 0;
    }

    inline void* allocateHavok(int numBytes)
    {
        return heisenberg::ConstraintFunctions::HkHeapAlloc(numBytes);
    }

    inline void freeHavok(void* ptr, int numBytes)
    {
        if (ptr) {
            heisenberg::ConstraintFunctions::HkHeapFree(ptr, numBytes);
        }
    }

    inline BodyView* getBody(void* world, RE::hknpBodyId bodyId)
    {
        if (!world) {
            return nullptr;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(bodyId.value) & kBodyIdIndexMask;
        if (index == kBodyIdIndexMask || index >= kMaxReasonableBodyIndex) {
            return nullptr;
        }
        void* buffer = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(world) + kWorldBodyBufferOffset);
        if (!buffer) {
            return nullptr;
        }
        return reinterpret_cast<BodyView*>(
            reinterpret_cast<std::uintptr_t>(buffer) + static_cast<std::uintptr_t>(index) * kBodyStride);
    }

    inline void* getMotion(void* world, std::uint32_t motionIndex)
    {
        return heisenberg::hknpBSWorld_accessMotion(reinterpret_cast<void*>(world), motionIndex);
    }

    inline void rebuildMotionMassProperties(void* world, std::uint32_t motionIndex)
    {
        using Fn = void (*)(void*, std::uint32_t);
        static REL::Relocation<Fn> fn{ REL::Offset(0x1546570) };  // hknpWorld::rebuildMotionMassProperties
        fn(reinterpret_cast<void*>(world), motionIndex);
    }
}

namespace heisenberg::rock_core
{
    // ROCK's PhysicsUtils::gameToHavokScale / havokToGameScale. PhysicsScale.cpp (which defines
    // physics_scale::gameToHavok) is not vendored, so use the literal FO4VR scale that matches
    // Heisenberg's HAVOK_WORLD_SCALE (1 game unit = 0.0142875 Havok units ~= 1/70).
    inline float gameToHavokScale() { return 0.0142875f; }
    inline float havokToGameScale() { return 1.0f / 0.0142875f; }
}
