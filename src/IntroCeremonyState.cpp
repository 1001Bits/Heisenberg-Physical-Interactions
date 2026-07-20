#include "IntroCeremonyState.h"

#include <F4SE/F4SE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include <spdlog/spdlog.h>

namespace heisenberg::IntroCeremonyState
{
    namespace
    {
        enum class CeremonyState : std::uint8_t
        {
            kLegacyUnresolved = 0,
            kIncomplete,
            kComplete
        };

        constexpr std::uint32_t MakeFourCC(char a, char b, char c, char d)
        {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(a))
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8)
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16)
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
        }

        constexpr std::uint32_t kSerializationID = MakeFourCC('H', 'S', 'B', 'G');
        constexpr std::uint32_t kIntroRecordType = MakeFourCC('I', 'N', 'T', 'R');
        constexpr std::uint32_t kIntroRecordVersion = 1;
        constexpr std::uint32_t kFlagCeremonyComplete = 1u << 0;
        constexpr std::uint32_t kIntroRecordSize = static_cast<std::uint32_t>(sizeof(std::uint32_t));

        std::atomic<CeremonyState> g_state{ CeremonyState::kLegacyUnresolved };
        std::atomic_bool g_initialized{ false };

        void ConsumeRecordRemainder(const F4SE::SerializationInterface* intfc, std::uint32_t length)
        {
            std::array<std::uint8_t, 256> scratch{};
            while (length > 0) {
                const auto chunk = (std::min)(length, static_cast<std::uint32_t>(scratch.size()));
                const auto read = intfc->ReadRecordData(scratch.data(), chunk);
                if (read == 0) {
                    break;
                }
                length -= (std::min)(length, read);
                if (read != chunk) {
                    break;
                }
            }
        }

        void F4SEAPI OnRevert(const F4SE::SerializationInterface*)
        {
            // A load callback or kNewGame will establish the next save's state.
            g_state.store(CeremonyState::kLegacyUnresolved, std::memory_order_release);
            spdlog::debug("[INTRO-SAVE] Revert — cleared per-save ceremony state");
        }

        void F4SEAPI OnSave(const F4SE::SerializationInterface* intfc)
        {
            const auto state = g_state.load(std::memory_order_acquire);
            if (state == CeremonyState::kLegacyUnresolved) {
                // Do not turn an as-yet-uninspected legacy save into an authoritative
                // incomplete record. In normal gameplay migration resolves on the first frame.
                spdlog::warn("[INTRO-SAVE] Save requested before legacy ceremony state was resolved — record omitted");
                return;
            }

            const std::uint32_t flags = state == CeremonyState::kComplete ? kFlagCeremonyComplete : 0u;
            if (!intfc->WriteRecord(kIntroRecordType, kIntroRecordVersion, &flags, kIntroRecordSize)) {
                spdlog::error("[INTRO-SAVE] Failed to write intro ceremony record");
                return;
            }

            spdlog::debug("[INTRO-SAVE] Saved ceremonyComplete={}", state == CeremonyState::kComplete);
        }

        void F4SEAPI OnLoad(const F4SE::SerializationInterface* intfc)
        {
            // Missing/invalid record means an old save. It must be inferred from
            // that save's inventory later, never from the process-global INI.
            g_state.store(CeremonyState::kLegacyUnresolved, std::memory_order_release);

            bool loadedRecord = false;
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            while (intfc->GetNextRecordInfo(type, version, length)) {
                if (type != kIntroRecordType || version != kIntroRecordVersion || length < kIntroRecordSize) {
                    ConsumeRecordRemainder(intfc, length);
                    continue;
                }

                std::uint32_t flags = 0;
                const auto read = intfc->ReadRecordData(&flags, kIntroRecordSize);
                if (length > read) {
                    ConsumeRecordRemainder(intfc, length - read);
                }
                if (read != kIntroRecordSize) {
                    spdlog::warn("[INTRO-SAVE] Intro record was truncated ({} of {} bytes)", read, kIntroRecordSize);
                    continue;
                }

                const bool complete = (flags & kFlagCeremonyComplete) != 0;
                g_state.store(complete ? CeremonyState::kComplete : CeremonyState::kIncomplete,
                              std::memory_order_release);
                loadedRecord = true;
                spdlog::info("[INTRO-SAVE] Loaded ceremonyComplete={}", complete);
            }

            if (!loadedRecord) {
                spdlog::info("[INTRO-SAVE] No per-save record — awaiting legacy inventory inference");
            }
        }
    }

    bool Initialize()
    {
        bool expected = false;
        if (!g_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return true;
        }

        const auto* serialization = F4SE::GetSerializationInterface();
        if (!serialization) {
            g_initialized.store(false, std::memory_order_release);
            spdlog::warn("[INTRO-SAVE] F4SE serialization interface unavailable — co-save persistence disabled");
            return false;
        }

        serialization->SetUniqueID(kSerializationID);
        serialization->SetRevertCallback(OnRevert);
        serialization->SetSaveCallback(OnSave);
        serialization->SetLoadCallback(OnLoad);
        spdlog::info("[INTRO-SAVE] Registered per-save intro ceremony serialization");
        return true;
    }

    bool IsResolved()
    {
        return g_state.load(std::memory_order_acquire) != CeremonyState::kLegacyUnresolved;
    }

    bool IsComplete()
    {
        return g_state.load(std::memory_order_acquire) == CeremonyState::kComplete;
    }

    bool NeedsLegacyInference()
    {
        return !IsResolved();
    }

    void PrepareForLoad()
    {
        g_state.store(CeremonyState::kLegacyUnresolved, std::memory_order_release);
        spdlog::debug("[INTRO-SAVE] Pre-load — awaiting co-save record or legacy inference");
    }

    void ResolveLegacyFromHolotapePresence(bool hasIntroHolotape)
    {
        auto expected = CeremonyState::kLegacyUnresolved;
        const auto resolved = hasIntroHolotape ? CeremonyState::kComplete : CeremonyState::kIncomplete;
        if (g_state.compare_exchange_strong(expected, resolved, std::memory_order_acq_rel)) {
            spdlog::info("[INTRO-SAVE] Legacy save inferred ceremonyComplete={} from intro holotape presence",
                         hasIntroHolotape);
        }
    }

    void ResetForNewGame()
    {
        g_state.store(CeremonyState::kIncomplete, std::memory_order_release);
        spdlog::info("[INTRO-SAVE] New game — ceremony state reset to incomplete");
    }

    void MarkComplete()
    {
        const auto previous = g_state.exchange(CeremonyState::kComplete, std::memory_order_acq_rel);
        if (previous != CeremonyState::kComplete) {
            spdlog::info("[INTRO-SAVE] Opening ceremony marked complete for this save");
        }
    }
}
