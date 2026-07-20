#include "HavokTimingFix.h"

#include "Config.h"
#include "rock_integration/HavokTimingFixPolicy.h"

#include <atomic>
#include <cstdint>
#include <spdlog/spdlog.h>

namespace heisenberg::HavokTimingFix
{
    namespace
    {
        // ABI NOTE (do not "fix" to a 4-arg signature): Ghidra's bhkWorld::SetDeltaTime reads
        // the delta from XMM0 — i.e. the delta is the FIRST argument, so the effective ABI is
        // void(float). A `(bhkWorld* this, float, bool, bool)` signature would put the delta in
        // XMM1 and break it. The function body uses only globals + XMM0, ignoring this/R8/R9.
        // ROCK ships the identical void(*)(float). This is correct and faithful.
        using SetDeltaTime_t = void (*)(float);

        SetDeltaTime_t    g_original = nullptr;
        std::atomic<bool> g_enabled{ false };
        bool              g_installed = false;

        // FO4VR 1.2.72 RVAs (REL::Offset). The substep globals were Ghidra-confirmed from
        // bhkWorld::SetDeltaTime: DAT_1465a3d84 (accumulated), DAT_1465a3d74 (substep delta),
        // DAT_1465a3d8c (substep count, engine-clamped to 6).
        constexpr std::uintptr_t kHookSite          = 0x0D84BD0;  // CALL site in the main update
        constexpr std::uintptr_t kFunc_SetDeltaTime = 0x1DF7120;  // bhkWorld::SetDeltaTime
        constexpr std::uintptr_t kData_Accumulated  = 0x65A3D84;
        constexpr std::uintptr_t kData_SubstepDelta = 0x65A3D74;
        constexpr std::uintptr_t kData_SubstepCount = 0x65A3D8C;

        float readF(std::uintptr_t rva, float fallback)
        {
            REL::Relocation<float*> p{ REL::Offset(rva) };
            return p.address() ? *p : fallback;
        }
        bool writeF(std::uintptr_t rva, float value)
        {
            REL::Relocation<float*> p{ REL::Offset(rva) };
            if (!p.address()) return false;
            *p = value;
            return true;
        }
        bool writeU(std::uintptr_t rva, std::uint32_t value)
        {
            REL::Relocation<std::uint32_t*> p{ REL::Offset(rva) };
            if (!p.address()) return false;
            *p = value;
            return true;
        }

        // Replaces the engine's call to bhkWorld::SetDeltaTime. Runs the original (which
        // computes the engine's substep delta/count), then overrides them per the policy so
        // physics steps at >= minPhysicsFrameRate even on long render frames.
        void hookedSetDeltaTime(float rawDeltaSeconds)
        {
            if (g_original) {
                g_original(rawDeltaSeconds);
            }
            if (!g_enabled.load(std::memory_order_acquire)) {
                return;
            }

            const float accumulated = readF(kData_Accumulated, rawDeltaSeconds);
            const auto decision = heisenberg::rock_core::havok_timing_fix_policy::evaluateTimingFix(
                heisenberg::rock_core::havok_timing_fix_policy::TimingFixInput{
                    .rawDeltaSeconds = rawDeltaSeconds,
                    .accumulatedDeltaSeconds = accumulated,
                    .minPhysicsFrameRate = heisenberg::g_config.havokTimingMinFrameRate,
                    .maxSubsteps = heisenberg::g_config.havokTimingMaxSubsteps,
                });
            if (!decision.valid) {
                return;
            }
            writeF(kData_SubstepDelta, decision.substepDeltaSeconds);
            writeU(kData_SubstepCount, decision.substepCount);
        }

        // Validate the call site is `E8 rel32` targeting SetDeltaTime before patching.
        bool validateCallSite()
        {
            REL::Relocation<std::uintptr_t> cs{ REL::Offset(kHookSite) };
            const auto* b = reinterpret_cast<const std::uint8_t*>(cs.address());
            if (!b || b[0] != 0xE8) {
                return false;
            }
            const auto rel = *reinterpret_cast<const std::int32_t*>(b + 1);
            const auto target = cs.address() + 5u + static_cast<std::uintptr_t>(rel);
            return target == REL::Offset(kFunc_SetDeltaTime).address();
        }
    }

    bool Install()
    {
        if (g_installed) {
            return true;
        }
        if (!validateCallSite()) {
            spdlog::error("[HavokTimingFix] call-site validation failed at 0x{:X} — not installing (native bytes changed)",
                          REL::Offset(kHookSite).address());
            return false;
        }
        REL::Relocation<std::uintptr_t> cs{ REL::Offset(kHookSite) };
        auto& trampoline = F4SE::GetTrampoline();
        const auto original = trampoline.write_call<5>(cs.address(), &hookedSetDeltaTime);
        g_original = reinterpret_cast<SetDeltaTime_t>(original);
        g_installed = g_original != nullptr;
        if (g_installed) {
            spdlog::info("[HavokTimingFix] installed call-site hook at 0x{:X} (original 0x{:X})", cs.address(), original);
        } else {
            spdlog::error("[HavokTimingFix] install failed: original target null");
        }
        return g_installed;
    }

    void SetEnabled(bool enabled)
    {
        if (!g_installed && enabled) {
            spdlog::warn("[HavokTimingFix] enable requested but hook not installed — no effect");
        }
        g_enabled.store(enabled, std::memory_order_release);
        spdlog::info("[HavokTimingFix] substep override {}", enabled ? "ENABLED" : "disabled");
    }

    bool IsInstalled()
    {
        return g_installed;
    }
}
