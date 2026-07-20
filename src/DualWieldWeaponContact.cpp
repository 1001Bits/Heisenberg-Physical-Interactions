#include "DualWieldWeaponContact.h"

#include "DualWieldAPI.h"

#include <REL/Relocation.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace heisenberg::dual_wield_contact
{
    namespace
    {
        constexpr std::uint32_t kInvalidBodyID = 0x7FFFFFFFu;
        constexpr int kContactStarted = 3;
        constexpr float kHavokToGame = 70.0F;

        using GetEventSignalBodyFn = void* (*)(void*, int, std::uint32_t);
        using SubscribeSimpleFn = void (*)(void*, void*);

        REL::Relocation<GetEventSignalBodyFn> g_getEventSignalBody{ REL::Offset(0x15481d0) };
        REL::Relocation<SubscribeSimpleFn> g_subscribeSimple{ REL::Offset(0x40ca60) };

        std::atomic<std::uint32_t> g_leftBodyID{ kInvalidBodyID };
        std::atomic<std::uint32_t> g_rightBodyID{ kInvalidBodyID };
        std::atomic<void*> g_leftWorld{ nullptr };
        std::atomic<void*> g_rightWorld{ nullptr };

        bool IsFinite(const RE::NiPoint3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        RE::NiPoint3 ReadBodyVelocity(void* world, std::uint32_t bodyID)
        {
            RE::NiPoint3 result{};
            if (!world || bodyID == kInvalidBodyID) {
                return result;
            }

            auto* bodyBuffer = *reinterpret_cast<std::byte**>(
                reinterpret_cast<std::byte*>(world) + 0x20);
            auto* motionBuffer = *reinterpret_cast<std::byte**>(
                reinterpret_cast<std::byte*>(world) + 0xE0);
            if (!bodyBuffer || !motionBuffer) {
                return result;
            }

            auto* body = bodyBuffer + ((bodyID & 0xFFFFu) * 0x90u);
            const auto motionID = *reinterpret_cast<std::uint32_t*>(body + 0x68);
            if (motionID == 0xFFFFFFFFu || motionID > 0x00FFFFFFu) {
                return result;
            }

            // F4VR 1.2.72: hknpMotion is 0x80 bytes; linear velocity is +0x40.
            const auto* velocity = reinterpret_cast<const float*>(
                motionBuffer + (motionID * 0x80u) + 0x40u);
            result = {
                velocity[0] * kHavokToGame,
                velocity[1] * kHavokToGame,
                velocity[2] * kHavokToGame
            };
            return IsFinite(result) ? result : RE::NiPoint3{};
        }

        void DispatchForHand(
            bool isLeft,
            void* world,
            const std::byte* event,
            std::uint32_t weaponBodyID,
            std::uint32_t otherBodyID)
        {
            using namespace HeisenbergPluginAPI;

            auto contact = MakeContact(
                isLeft ? PhysicalHand::kLeft : PhysicalHand::kRight);

            // F4VR 1.2.72 hknpManifoldProcessedEvent: normal +0x40,
            // inline-contact count +0x30, first contact point +0x70.
            const auto* normalData = reinterpret_cast<const float*>(event + 0x40);
            contact.contactNormal = { normalData[0], normalData[1], normalData[2] };
            if (!IsFinite(contact.contactNormal)) {
                contact.contactNormal = {};
            } else {
                const float lengthSquared =
                    contact.contactNormal.x * contact.contactNormal.x +
                    contact.contactNormal.y * contact.contactNormal.y +
                    contact.contactNormal.z * contact.contactNormal.z;
                if (lengthSquared > 1.0e-6F) {
                    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
                    contact.contactNormal.x *= inverseLength;
                    contact.contactNormal.y *= inverseLength;
                    contact.contactNormal.z *= inverseLength;
                } else {
                    contact.contactNormal = {};
                }
            }

            const int contactCount = *reinterpret_cast<const int*>(event + 0x30);
            if (contactCount > 0 && contactCount <= 4) {
                const auto* pointData = reinterpret_cast<const float*>(event + 0x70);
                contact.contactPoint = {
                    pointData[0] * kHavokToGame,
                    pointData[1] * kHavokToGame,
                    pointData[2] * kHavokToGame
                };
                if (!IsFinite(contact.contactPoint)) {
                    contact.contactPoint = {};
                }
            }

            const auto weaponVelocity = ReadBodyVelocity(world, weaponBodyID);
            const auto otherVelocity = ReadBodyVelocity(world, otherBodyID);
            contact.relativeVelocity = {
                weaponVelocity.x - otherVelocity.x,
                weaponVelocity.y - otherVelocity.y,
                weaponVelocity.z - otherVelocity.z
            };

            const float projected =
                contact.relativeVelocity.x * contact.contactNormal.x +
                contact.relativeVelocity.y * contact.contactNormal.y +
                contact.relativeVelocity.z * contact.contactNormal.z;
            const float relativeLength = std::sqrt(
                contact.relativeVelocity.x * contact.relativeVelocity.x +
                contact.relativeVelocity.y * contact.relativeVelocity.y +
                contact.relativeVelocity.z * contact.relativeVelocity.z);
            contact.separatingVelocity =
                (contact.contactNormal.x != 0.0F ||
                 contact.contactNormal.y != 0.0F ||
                 contact.contactNormal.z != 0.0F) ?
                    std::fabs(projected) : relativeLength;

            // Resolving a body to TESObjectREFR/handle is intentionally omitted
            // on the Havok worker. targetHandle remains invalid/zero.
            InvokeDualWieldWeaponContact(contact);
        }

        void OnContactStarted(void* worldPointer, void* eventPointer)
        {
            if (!worldPointer || !eventPointer) {
                return;
            }

            void* world = *reinterpret_cast<void**>(worldPointer);
            const auto* event = reinterpret_cast<const std::byte*>(eventPointer);
            const auto bodyA = *reinterpret_cast<const std::uint32_t*>(event + 0x08);
            const auto bodyB = *reinterpret_cast<const std::uint32_t*>(event + 0x0C);
            if (!world || bodyA == kInvalidBodyID || bodyB == kInvalidBodyID) {
                return;
            }

            const auto leftID = g_leftBodyID.load(std::memory_order_acquire);
            if (world == g_leftWorld.load(std::memory_order_acquire) &&
                (bodyA == leftID || bodyB == leftID)) {
                DispatchForHand(true, world, event, leftID, bodyA == leftID ? bodyB : bodyA);
            }

            const auto rightID = g_rightBodyID.load(std::memory_order_acquire);
            if (world == g_rightWorld.load(std::memory_order_acquire) &&
                (bodyA == rightID || bodyB == rightID)) {
                DispatchForHand(false, world, event, rightID, bodyA == rightID ? bodyB : bodyA);
            }
        }
    }

    bool Subscribe(void* hknpWorld, std::uint32_t bodyId, bool isLeft)
    {
        if (!hknpWorld || bodyId == kInvalidBodyID || bodyId == 0) {
            return false;
        }

        auto& storedID = isLeft ? g_leftBodyID : g_rightBodyID;
        auto& storedWorld = isLeft ? g_leftWorld : g_rightWorld;
        if (storedID.load(std::memory_order_acquire) == bodyId &&
            storedWorld.load(std::memory_order_acquire) == hknpWorld) {
            return true;
        }

        storedWorld.store(hknpWorld, std::memory_order_release);
        storedID.store(bodyId, std::memory_order_release);
        void* signal = g_getEventSignalBody(hknpWorld, kContactStarted, bodyId);
        if (!signal) {
            Unsubscribe(isLeft);
            return false;
        }

        try {
            g_subscribeSimple(signal, reinterpret_cast<void*>(&OnContactStarted));
        } catch (...) {
            Unsubscribe(isLeft);
            return false;
        }
        return true;
    }

    void Unsubscribe(bool isLeft)
    {
        auto& storedID = isLeft ? g_leftBodyID : g_rightBodyID;
        auto& storedWorld = isLeft ? g_leftWorld : g_rightWorld;
        storedID.store(kInvalidBodyID, std::memory_order_release);
        storedWorld.store(nullptr, std::memory_order_release);
    }

    void Reset()
    {
        Unsubscribe(true);
        Unsubscribe(false);
    }
}
