#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rock::held_grab_cc_policy
{
    inline constexpr int kGeneratedContactStride = 0x40;
    inline constexpr int kGeneratedContactBodyIdOffset = 0x28;
    inline constexpr int kGeneratedConstraintRowsOffset = 0x48;
    inline constexpr int kGeneratedConstraintCountOffset = 0x50;

    struct GeneratedContactBufferView
    {
        bool valid = false;
        char* manifoldEntries = nullptr;
        char* constraintEntries = nullptr;
        int* manifoldCountPtr = nullptr;
        int* constraintCountPtr = nullptr;
        int manifoldCount = 0;
        int constraintCount = 0;
        int pairCount = 0;
        const char* reason = "uninitialized";
    };

    struct GeneratedContactFilterResult
    {
        bool valid = false;
        int originalPairCount = 0;
        int keptPairCount = 0;
        int removedPairCount = 0;
        const char* reason = "uninitialized";
    };

    /*
     * Manifold rows carry the body ID used to classify a contact. Constraint
     * rows do not. Only the paired prefix can therefore be filtered safely.
     * Any unmatched tail is identity-unknown native solver input and must be
     * preserved byte-for-byte rather than truncated with the paired rows.
     */
    template <class IsHeldBody>
    inline GeneratedContactFilterResult filterGeneratedContactBuffers(
        const GeneratedContactBufferView& view,
        IsHeldBody&& isHeldBody)
    {
        if (!view.valid) {
            return GeneratedContactFilterResult{
                .valid = false,
                .reason = view.reason,
            };
        }

        if (!view.manifoldEntries || !view.constraintEntries || !view.manifoldCountPtr || !view.constraintCountPtr) {
            return GeneratedContactFilterResult{
                .valid = false,
                .originalPairCount = view.pairCount,
                .reason = "missingFilterBuffer",
            };
        }

        if (view.manifoldCount < 0 ||
            view.constraintCount < 0 ||
            view.pairCount < 0 ||
            view.pairCount > view.manifoldCount ||
            view.pairCount > view.constraintCount) {
            return GeneratedContactFilterResult{
                .valid = false,
                .originalPairCount = view.pairCount,
                .reason = "invalidFilterBufferCounts",
            };
        }

        int writeIndex = 0;
        for (int readIndex = 0; readIndex < view.pairCount; ++readIndex) {
            char* manifoldEntry = view.manifoldEntries + readIndex * kGeneratedContactStride;
            const auto bodyId = *reinterpret_cast<std::uint32_t*>(manifoldEntry + kGeneratedContactBodyIdOffset);
            if (isHeldBody(bodyId)) {
                continue;
            }

            if (writeIndex != readIndex) {
                std::memmove(
                    view.manifoldEntries + writeIndex * kGeneratedContactStride,
                    manifoldEntry,
                    kGeneratedContactStride);
                std::memmove(
                    view.constraintEntries + writeIndex * kGeneratedContactStride,
                    view.constraintEntries + readIndex * kGeneratedContactStride,
                    kGeneratedContactStride);
            }
            ++writeIndex;
        }

        const int removedCount = view.pairCount - writeIndex;
        if (removedCount > 0) {
            const int unmatchedManifoldCount = view.manifoldCount - view.pairCount;
            const int unmatchedConstraintCount = view.constraintCount - view.pairCount;

            if (unmatchedManifoldCount > 0) {
                std::memmove(
                    view.manifoldEntries + writeIndex * kGeneratedContactStride,
                    view.manifoldEntries + view.pairCount * kGeneratedContactStride,
                    static_cast<std::size_t>(unmatchedManifoldCount) * kGeneratedContactStride);
            }
            if (unmatchedConstraintCount > 0) {
                std::memmove(
                    view.constraintEntries + writeIndex * kGeneratedContactStride,
                    view.constraintEntries + view.pairCount * kGeneratedContactStride,
                    static_cast<std::size_t>(unmatchedConstraintCount) * kGeneratedContactStride);
            }

            *view.manifoldCountPtr = writeIndex + unmatchedManifoldCount;
            *view.constraintCountPtr = writeIndex + unmatchedConstraintCount;
        }

        return GeneratedContactFilterResult{
            .valid = true,
            .originalPairCount = view.pairCount,
            .keptPairCount = writeIndex,
            .removedPairCount = removedCount,
            .reason = removedCount > 0 ? "filteredGeneratedContactRows" : "noGeneratedContactRowsFiltered",
        };
    }
}
