#include "physics-interaction/grab/GeneratedContactBufferPolicy.h"
#include "physics-interaction/native/CharacterControllerLayoutPolicy.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    using namespace rock::held_grab_cc_policy;

    constexpr int kRowCapacity = 3;
    using ContactBuffer = std::array<char, kGeneratedContactStride * kRowCapacity>;

    void Require(const bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }

    void FillRow(ContactBuffer& buffer, const int row, const std::uint8_t marker)
    {
        std::memset(
            buffer.data() + row * kGeneratedContactStride,
            marker,
            kGeneratedContactStride);
    }

    std::uint8_t RowMarker(const ContactBuffer& buffer, const int row)
    {
        return static_cast<std::uint8_t>(buffer[row * kGeneratedContactStride]);
    }

    void SetBodyId(ContactBuffer& buffer, const int row, const std::uint32_t bodyId)
    {
        std::memcpy(
            buffer.data() + row * kGeneratedContactStride + kGeneratedContactBodyIdOffset,
            &bodyId,
            sizeof(bodyId));
    }

    std::uint32_t GetBodyId(const ContactBuffer& buffer, const int row)
    {
        std::uint32_t bodyId = 0;
        std::memcpy(
            &bodyId,
            buffer.data() + row * kGeneratedContactStride + kGeneratedContactBodyIdOffset,
            sizeof(bodyId));
        return bodyId;
    }

    GeneratedContactBufferView MakeView(
        ContactBuffer& manifolds,
        ContactBuffer& constraints,
        int& manifoldCount,
        int& constraintCount)
    {
        return GeneratedContactBufferView{
            .valid = true,
            .manifoldEntries = manifolds.data(),
            .constraintEntries = constraints.data(),
            .manifoldCountPtr = &manifoldCount,
            .constraintCountPtr = &constraintCount,
            .manifoldCount = manifoldCount,
            .constraintCount = constraintCount,
            .pairCount = manifoldCount < constraintCount ? manifoldCount : constraintCount,
            .reason = "ok",
        };
    }

    void TestFo4VrCharacterControllerLayout()
    {
        using namespace rock::character_controller_layout;

        alignas(void*) std::array<
            std::byte,
            kActorCurrentProcessOffset + sizeof(void*)>
            actor{};
        alignas(void*) std::array<
            std::byte,
            kProcessMiddleHighOffset + sizeof(void*)>
            process{};
        alignas(void*) std::array<
            std::byte,
            kVrCharacterControllerOffset + sizeof(void*)>
            middleHigh{};
        std::uint32_t flatOffsetSentinel = 0;
        std::uint32_t vrOffsetSentinel = 0;

        void* processPointer = process.data();
        void* middleHighPointer = middleHigh.data();
        void* flatPointer = &flatOffsetSentinel;
        void* vrPointer = &vrOffsetSentinel;
        std::memcpy(
            actor.data() + kActorCurrentProcessOffset,
            &processPointer,
            sizeof(processPointer));
        std::memcpy(
            process.data() + kProcessMiddleHighOffset,
            &middleHighPointer,
            sizeof(middleHighPointer));
        std::memcpy(
            middleHigh.data() + kFlatCharacterControllerOffset,
            &flatPointer,
            sizeof(flatPointer));
        std::memcpy(
            middleHigh.data() + kVrCharacterControllerOffset,
            &vrPointer,
            sizeof(vrPointer));

        Require(
            resolveActorCharacterControllerUnchecked(actor.data()) ==
                &vrOffsetSentinel,
            "FO4VR controller resolution must use +0x3E8, not CommonLib's flat +0x3E0");
        Require(
            resolveActorCharacterControllerUnchecked(nullptr) == nullptr,
            "null actors must fail closed");

        void* nullProcess = nullptr;
        std::memcpy(
            actor.data() + kActorCurrentProcessOffset,
            &nullProcess,
            sizeof(nullProcess));
        Require(
            resolveActorCharacterControllerUnchecked(actor.data()) == nullptr,
            "missing actor process state must fail closed");
    }
}

int main()
{
    using namespace rock::held_grab_cc_policy;

    TestFo4VrCharacterControllerLayout();

    {
        alignas(std::uint32_t) ContactBuffer manifolds{};
        alignas(std::uint32_t) ContactBuffer constraints{};
        FillRow(constraints, 0, 0x51);
        FillRow(constraints, 1, 0x52);
        int manifoldCount = 0;
        int constraintCount = 2;

        const auto result = filterGeneratedContactBuffers(
            GeneratedContactBufferView{
                .valid = false,
                .constraintEntries = constraints.data(),
                .manifoldCountPtr = &manifoldCount,
                .constraintCountPtr = &constraintCount,
                .manifoldCount = manifoldCount,
                .constraintCount = constraintCount,
                .reason = "missingManifoldEntries",
            },
            [](std::uint32_t) { return true; });

        Require(!result.valid,
            "constraint-only buffers must fail open when no body IDs are available");
        Require(manifoldCount == 0 && constraintCount == 2,
            "failing open must preserve constraint-only native contact counts");
        Require(RowMarker(constraints, 0) == 0x51 && RowMarker(constraints, 1) == 0x52,
            "failing open must leave constraint-only native contact rows untouched");
    }

    {
        alignas(std::uint32_t) ContactBuffer manifolds{};
        alignas(std::uint32_t) ContactBuffer constraints{};
        FillRow(manifolds, 0, 0x11);
        FillRow(manifolds, 1, 0x12);
        FillRow(constraints, 0, 0x21);
        FillRow(constraints, 1, 0x22);
        FillRow(constraints, 2, 0x23);
        SetBodyId(manifolds, 0, 100);
        SetBodyId(manifolds, 1, 200);
        int manifoldCount = 2;
        int constraintCount = 3;

        const auto result = filterGeneratedContactBuffers(
            MakeView(manifolds, constraints, manifoldCount, constraintCount),
            [](const std::uint32_t bodyId) { return bodyId == 100; });

        Require(result.valid && result.removedPairCount == 1,
            "a classified generated row must still be filtered");
        Require(manifoldCount == 1 && constraintCount == 2,
            "an unmatched identity-unknown constraint tail must be preserved");
        Require(GetBodyId(manifolds, 0) == 200,
            "the retained paired manifold row must be compacted into the first slot");
        Require(RowMarker(constraints, 0) == 0x22 && RowMarker(constraints, 1) == 0x23,
            "the retained paired constraint and unmatched native tail must remain ordered");
    }

    {
        alignas(std::uint32_t) ContactBuffer manifolds{};
        alignas(std::uint32_t) ContactBuffer constraints{};
        FillRow(manifolds, 0, 0x31);
        FillRow(manifolds, 1, 0x32);
        FillRow(manifolds, 2, 0x33);
        FillRow(constraints, 0, 0x41);
        FillRow(constraints, 1, 0x42);
        SetBodyId(manifolds, 0, 100);
        SetBodyId(manifolds, 1, 200);
        SetBodyId(manifolds, 2, 300);
        int manifoldCount = 3;
        int constraintCount = 2;

        const auto result = filterGeneratedContactBuffers(
            MakeView(manifolds, constraints, manifoldCount, constraintCount),
            [](const std::uint32_t bodyId) { return bodyId == 100; });

        Require(result.valid && result.removedPairCount == 1,
            "paired generated rows must be removed when the manifold buffer is longer");
        Require(manifoldCount == 2 && constraintCount == 1,
            "an unmatched manifold tail must be preserved instead of truncated");
        Require(GetBodyId(manifolds, 0) == 200 && GetBodyId(manifolds, 1) == 300,
            "retained paired and unmatched manifold rows must remain ordered");
        Require(RowMarker(constraints, 0) == 0x42,
            "the retained paired constraint row must remain aligned");
    }

    std::cout << "GeneratedContactBufferPolicyTests passed\n";
    return 0;
}
