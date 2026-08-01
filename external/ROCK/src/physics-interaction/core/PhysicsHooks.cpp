#include "physics-interaction/core/PhysicsHooks.h"

#include "ROCKMain.h"
#include "physics-interaction/native/HavokOffsets.h"
#include "physics-interaction/grab/GrabHeldObject.h"
#include "physics-interaction/input/InputRemapRuntime.h"
#include "physics-interaction/NativeMeleeSuppressionPolicy.h"
#include "physics-interaction/collision/CollisionLayerPolicy.h"
#include "physics-interaction/core/PhysicsInteraction.h"
#include "physics-interaction/PhysicsLog.h"
#include "physics-interaction/native/BodyCollisionControl.h"
#include "physics-interaction/native/CharacterControllerRuntime.h"
#include "physics-interaction/native/HavokRuntime.h"
#include "physics-interaction/native/HavokTimingFixPolicy.h"
#include "physics-interaction/native/HookAddressDiagnostics.h"
#include "physics-interaction/native/NativeGrabHapticSuppressionPolicy.h"
#include "physics-interaction/performance/PerformanceProfiler.h"

#include "RockConfig.h"

#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/InputEvent.h"
#include "RE/Bethesda/Settings.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace rock
{
    namespace
    {
        using NativeMeleeHandler_t = bool (*)(void*, RE::Actor*, RE::BSFixedString*);
        using NativeMeleeInputGateHandler_t = bool (*)(void*, const RE::InputEvent*);
        using NativePlayerWeaponSwingCallback_t = void (*)(RE::Actor*, std::uint32_t);
        using NativeVrMeleeImpactCallback_t = void (*)(RE::Actor*, void*, void*);
        using BhkWorldSetDeltaTime_t = void (*)(float);

        static NativeMeleeHandler_t g_originalWeaponSwingHandler = nullptr;
        static NativeMeleeHandler_t g_originalHitFrameHandler = nullptr;
        static NativeMeleeInputGateHandler_t g_originalAttackBlockShouldHandleEvent = nullptr;
        static NativePlayerWeaponSwingCallback_t g_originalPlayerWeaponSwingCallback = nullptr;
        static NativeVrMeleeImpactCallback_t g_originalVrMeleeImpactCallback = nullptr;
        static BhkWorldSetDeltaTime_t g_originalBhkWorldSetDeltaTime = nullptr;
        static std::atomic<bool> g_havokTimingFixMissingOriginalLogged{ false };
        static std::atomic<bool> g_havokTimingFixWriteFailureLogged{ false };

        /*
         * CONFIRMED against Fallout4VR.exe 1.2.72 (Ghidra, image base 0x140000000):
         *   0x140D84BD0  E8 4B 25 07 01   CALL 0x141DF7120
         *   ... inside Main::OnIdle_UpdateTimer (entry 0x140D84B40); the next
         *   instruction is CMP byte ptr [RBX+0x1D0],0x0.
         *   0x141DF7120 is the entry of bhkWorld::SetDeltaTime (48 89 5C 24 10 57 ...)
         *   and 0x140D84BD0 is one of its five xrefs.
         * Both constants are correct. A mismatch at runtime therefore means the CALL
         * was already redirected, never that the offsets drifted.
         */
        constexpr std::uintptr_t kFunc_BhkWorldSetDeltaTime = 0x1DF7120;
        constexpr std::uintptr_t kHookSite_BhkWorldSetDeltaTimeMainCall = 0x0D84BD0;
        /*
         * Native melee suppression can be queried from animation-event hooks, so
         * its physical-swing bridge needs a tiny shared clock. This is advanced
         * by ROCK update frames instead of wall milliseconds: debugger stalls,
         * menus, loading, and frame hitches must not expire a gameplay lease
         * behind the simulation.
         */
        static std::atomic<std::uint64_t> g_nativeMeleeFrameClock{ 1 };
        static std::array<std::atomic<std::uint64_t>, 2> g_nativeMeleePhysicalSwingExpiresAtFrame{};
        static std::atomic<bool> g_nativeMeleeSuppressionHooksInstalled{ false };
        constexpr std::uint64_t kNativeMeleePhysicalSwingLeaseFrames = 24;
        constexpr std::uint64_t kNativeMeleeRuntimeSettingCheckIntervalFrames = 90;
        constexpr std::uint64_t kNativeGrabHapticRuntimeSettingCheckIntervalFrames = 90;
        constexpr char kNativeMeleeVelocityCheckSetting[] = "bMeleeVelocityCheck:VRInput";
        constexpr char kNativeMeleeLinearVelocityThresholdSetting[] = "fMeleeLinearVelocityThreshold:VRInput";
        constexpr char kNativeMeleeAngularVelocityThresholdSetting[] = "fMeleeAngularVelocityThreshold:VRInput";
        constexpr float kNativeMeleeSuppressedVelocityThreshold = 1.0e9f;
        constexpr std::array<std::uint8_t, 14> kVrMeleeImpactExpectedPrefix{
            0x48, 0x8B, 0xC4,
            0x4C, 0x89, 0x40, 0x18,
            0x48, 0x89, 0x50, 0x10,
            0x55,
            0x53,
            0x56
        };
        constexpr DWORD kVirtualMemoryCommitReserve = 0x00001000u | 0x00002000u;
        constexpr DWORD kVirtualMemoryRelease = 0x00008000u;
        constexpr DWORD kPageExecuteRead = 0x00000020u;
        constexpr DWORD kPageExecuteReadWrite = 0x00000040u;

        struct NativeBinaryRuntimeSettingState
        {
            RE::Setting* setting = nullptr;
            bool originalValue = false;
            bool originalCaptured = false;
            bool missingLogged = false;
            bool typeMismatchLogged = false;
            bool confirmedLogged = false;
            bool applied = false;
            std::atomic<std::uint32_t> reapplyCount{ 0 };
        };

        struct NativeFloatRuntimeSettingState
        {
            RE::Setting* setting = nullptr;
            float originalValue = 0.0f;
            bool originalCaptured = false;
            bool missingLogged = false;
            bool typeMismatchLogged = false;
            bool confirmedLogged = false;
            bool applied = false;
            std::atomic<std::uint32_t> reapplyCount{ 0 };
        };

        struct NativeGrabHapticBinarySettingState
        {
            RE::Setting* setting = nullptr;
            bool originalValue = false;
            bool originalCaptured = false;
            bool missingLogged = false;
            bool typeMismatchLogged = false;
            bool confirmedLogged = false;
            bool applied = false;
            std::atomic<std::uint32_t> reapplyCount{ 0 };
        };

        struct NativeGrabHapticFloatSettingState
        {
            RE::Setting* setting = nullptr;
            float originalValue = 0.0f;
            bool originalCaptured = false;
            bool missingLogged = false;
            bool typeMismatchLogged = false;
            bool confirmedLogged = false;
            bool applied = false;
            std::atomic<std::uint32_t> reapplyCount{ 0 };
        };

        static std::atomic<std::uint64_t> g_nativeMeleeRuntimeSettingNextCheckFrame{ 0 };
        static NativeBinaryRuntimeSettingState g_nativeMeleeVelocityCheckState;
        static NativeFloatRuntimeSettingState g_nativeMeleeLinearThresholdState;
        static NativeFloatRuntimeSettingState g_nativeMeleeAngularThresholdState;
        static std::atomic<std::uint64_t> g_nativeGrabHapticRuntimeSettingNextCheckFrame{ 0 };
        static NativeGrabHapticBinarySettingState g_nativeGrabHapticRolloverState;
        static NativeGrabHapticFloatSettingState g_nativeGrabHapticHoverIntensityState;
        static NativeGrabHapticFloatSettingState g_nativeGrabHapticHoverDurationState;

        bool isAddressInGameText(std::uintptr_t address)
        {
            const auto text = REL::Module::get().segment(REL::Segment::text);
            return address >= text.address() && address < text.address() + text.size();
        }

        bool validateNativeMeleeVtableTarget(std::uintptr_t entryOffset, std::uintptr_t expectedFunctionOffset, const char* label)
        {
            /*
             * UNSET-CONSTANT GUARD. A TODO_RE (== 0) offset does not fail loudly on
             * its own: REL::Offset(0) resolves to the module base, which is readable
             * PE header, so the checks below read 0x0000000300905A4D out of "MZ\x90..."
             * and the WARN path returns TRUE. That is how shipped 0.8.4 logged a
             * phantom "external target 0x300905A4D" for two different slots, and it is
             * one step away from installNativeMeleeVtableHook writing a hook pointer
             * into the PE header. Refuse before any of that can happen.
             */
            if (entryOffset == 0 || expectedFunctionOffset == 0) {
                ROCK_LOG_ERROR(Init,
                    "{} vtable validation failed: offset constant is UNSET (vtableEntry=0x{:X} function=0x{:X}); this is a missing "
                    "reverse-engineering result in HavokOffsets.h, not a runtime condition",
                    label,
                    entryOffset,
                    expectedFunctionOffset);
                return false;
            }

            REL::Relocation<std::uintptr_t> entry{ REL::Offset(entryOffset) };
            auto* slot = reinterpret_cast<std::uintptr_t*>(entry.address());
            if (!slot) {
                ROCK_LOG_ERROR(Init, "{} vtable validation failed: slot is null", label);
                return false;
            }

            const auto current = *slot;
            const auto expected = REL::Offset(expectedFunctionOffset).address();
            if (!current) {
                ROCK_LOG_ERROR(Init, "{} vtable validation failed: slot 0x{:X} in {} holds a null function pointer",
                    label,
                    entry.address(),
                    rock::hook_diagnostics::describeAddress(entry.address()));
                return false;
            }

            if (current == expected) {
                return true;
            }

            if (isAddressInGameText(current)) {
                ROCK_LOG_ERROR(Init,
                    "{} vtable validation failed: slot 0x{:X} ({}) holds 0x{:X} ({}), expected 0x{:X} ({}) - both are inside the game image, so the "
                    "slot constant and the function constant disagree",
                    label,
                    entry.address(),
                    rock::hook_diagnostics::describeAddress(entry.address()),
                    current,
                    rock::hook_diagnostics::describeAddress(current),
                    expected,
                    rock::hook_diagnostics::describeAddress(expected));
                return false;
            }

            /*
             * Fail open for native melee. Chaining an unverified target would make
             * suppression depend on a foreign ABI and could strand the player with
             * only part of the native damage pipeline. Exact vanilla ownership is
             * required before ROCK writes any of the five hook sites.
             */
            ROCK_LOG_ERROR(Init,
                "{} vtable validation failed: slot 0x{:X} ({}) holds 0x{:X}, expected exact native target 0x{:X}. Found pointer resolves to: {}. "
                "ROCK will not chain it; native melee suppression stays off.",
                label,
                entry.address(),
                rock::hook_diagnostics::describeAddress(entry.address()),
                current,
                expected,
                rock::hook_diagnostics::describeAddress(current));
            return false;
        }

        bool validateEntryTrampolineTarget(const char* label, std::uintptr_t targetOffset, const std::uint8_t* expectedPrefix, std::size_t stolenBytes)
        {
            if (stolenBytes < 14) {
                ROCK_LOG_ERROR(Init, "{} entry validation failed: stolen byte count {} cannot hold an absolute jump", label, stolenBytes);
                return false;
            }

            // UNSET-CONSTANT GUARD, same reasoning as validateNativeMeleeVtableTarget:
            // REL::Offset(0) is the module base, whose PE header is readable and never
            // matches, so a TODO_RE constant reports as "native bytes changed" on every
            // machine forever. Name the real defect instead of blaming the bytes.
            if (targetOffset == 0) {
                ROCK_LOG_ERROR(Init,
                    "{} entry validation failed: target offset constant is UNSET (0x0). REL::Offset(0) resolves to the module base "
                    "(PE header), which can never match a function prologue. Fix the constant in HavokOffsets.h.",
                    label);
                return false;
            }

            REL::Relocation<std::uintptr_t> target{ REL::Offset(targetOffset) };
            auto* targetAddr = reinterpret_cast<const std::uint8_t*>(target.address());
            if (!targetAddr || !expectedPrefix) {
                ROCK_LOG_ERROR(Init, "{} entry validation failed: target or validation bytes are null", label);
                return false;
            }

            if (std::memcmp(targetAddr, expectedPrefix, stolenBytes) != 0) {
                // Found vs expected, plus the owning module for the target and for any
                // detour branch now sitting on it. No presumed culprit.
                ROCK_LOG_ERROR(Init,
                    "{} entry validation FAILED: {}",
                    label,
                    rock::hook_diagnostics::describePrefixMismatch(target.address(), expectedPrefix, stolenBytes));
                return false;
            }

            return true;
        }

        float readBhkWorldFloatGlobal(std::uintptr_t offset, float fallback)
        {
            REL::Relocation<float*> value{ REL::Offset(offset) };
            return value.address() ? *value : fallback;
        }

        std::uint32_t readBhkWorldUintGlobal(std::uintptr_t offset, std::uint32_t fallback)
        {
            REL::Relocation<std::uint32_t*> value{ REL::Offset(offset) };
            return value.address() ? *value : fallback;
        }

        bool writeBhkWorldFloatGlobal(std::uintptr_t offset, float newValue)
        {
            REL::Relocation<float*> value{ REL::Offset(offset) };
            if (!value.address()) {
                return false;
            }
            *value = newValue;
            return true;
        }

        bool writeBhkWorldUintGlobal(std::uintptr_t offset, std::uint32_t newValue)
        {
            REL::Relocation<std::uint32_t*> value{ REL::Offset(offset) };
            if (!value.address()) {
                return false;
            }
            *value = newValue;
            return true;
        }

        /*
         * ── WHO OWNS AN ALREADY-PATCHED HOOK SITE ─────────────────────────────────────
         *
         * ROCK is compiled INTO the host plugin DLL. So if a patched hook site branches to
         * an address that the SAME loaded module owns as this very function, the host
         * plugin put it there. That is a measured fact from GetModuleHandleEx, not the kind
         * of guess ("another mod likely hooks this") that previously got read back as
         * evidence and sent two investigations chasing a plugin nobody had installed.
         *
         * This distinction is what separates a real failure from a benign duplicate: for
         * HAVOK_TIMING_FIX and HandleBumpedCharacter the host installs its own hook FIRST
         * and the feature is fully active — ROCK simply must not patch the site twice.
         */
        [[nodiscard]] HMODULE moduleOwning(std::uintptr_t address) noexcept
        {
            if (!address) {
                return nullptr;
            }
            HMODULE module = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address),
                    &module)) {
                return nullptr;
            }
            return module;
        }

        /// Any object with static storage in this TU; its address is inside this module.
        const char kHostModuleAnchor = 0;

        /// The module that contains ROCK's own code — i.e. the host plugin DLL.
        [[nodiscard]] HMODULE hostPluginModule() noexcept
        {
            static HMODULE self = moduleOwning(reinterpret_cast<std::uintptr_t>(&kHostModuleAnchor));
            return self;
        }

        [[nodiscard]] bool isOwnedByHostPlugin(std::uintptr_t address) noexcept
        {
            auto* self = hostPluginModule();
            return self != nullptr && address != 0 && moduleOwning(address) == self;
        }

        /*
         * Numeric twin of hook_diagnostics::describeBranchAtAddress (which returns prose).
         * Same three detour encodings; returns 0 when the bytes are not a recognised
         * branch, so a caller can never mistake "not a branch" for a real destination.
         */
        [[nodiscard]] std::uintptr_t decodeBranchDestination(std::uintptr_t address) noexcept
        {
            if (!rock::hook_diagnostics::isReadable(address, 16)) {
                return 0;
            }
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);

            // FF 25 <rel32> : jmp qword ptr [rip + rel32]   (14-byte absolute detour)
            if (bytes[0] == 0xFF && bytes[1] == 0x25) {
                const auto rel = *reinterpret_cast<const std::int32_t*>(bytes + 2);
                const auto slot = address + 6u + static_cast<std::intptr_t>(rel);
                if (!rock::hook_diagnostics::isReadable(slot, sizeof(std::uintptr_t))) {
                    return 0;
                }
                return *reinterpret_cast<const std::uintptr_t*>(slot);
            }

            // E9/E8 <rel32> : jmp/call rel32
            if (bytes[0] == 0xE9 || bytes[0] == 0xE8) {
                const auto rel = *reinterpret_cast<const std::int32_t*>(bytes + 1);
                return address + 5u + static_cast<std::intptr_t>(rel);
            }

            // 48 B8 <imm64> FF E0 : mov rax, imm64 ; jmp rax
            if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
                return *reinterpret_cast<const std::uintptr_t*>(bytes + 2);
            }

            return 0;
        }

        /*
         * Outcome of inspecting a hook site that ROCK wants to patch. Three states, because
         * "not patchable by ROCK" and "broken" are different things and must not share a
         * log severity.
         */
        enum class HookSiteOwnership
        {
            VerifiedNative,  ///< Site still holds the expected native bytes — ROCK may patch.
            HostPluginOwns,  ///< Already redirected into this same plugin DLL — benign, feature active.
            Unrecognised,    ///< Neither — a genuine validation failure.
        };

        HookSiteOwnership validateBhkWorldSetDeltaTimeMainCallSite()
        {
            /*
             * Ghidra verified the main update call at 0x140D84BD0 targets
             * bhkWorld::SetDeltaTime at 0x141DF7120. The SetDeltaTime entry
             * prologue includes RIP-relative instructions, so ROCK patches the
             * call site instead of using the relocated-entry trampoline helper.
             */
            REL::Relocation<std::uintptr_t> callSite{ REL::Offset(kHookSite_BhkWorldSetDeltaTimeMainCall) };
            const auto callSiteAddress = callSite.address();
            auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSiteAddress);
            if (!callBytes) {
                ROCK_LOG_ERROR(Init, "HAVOK_TIMING_FIX call-site validation failed: call site is null");
                return HookSiteOwnership::Unrecognised;
            }

            if (callBytes[0] != 0xE8) {
                ROCK_LOG_ERROR(
                    Init,
                    "HAVOK_TIMING_FIX call-site validation FAILED at 0x{:X} ({}): expected a CALL rel32 (opcode 0xE8), found bytes [{}]",
                    callSiteAddress,
                    rock::hook_diagnostics::describeAddress(callSiteAddress),
                    rock::hook_diagnostics::formatBytes(callBytes, 5));
                return HookSiteOwnership::Unrecognised;
            }

            const auto relativeTarget = *reinterpret_cast<const std::int32_t*>(callBytes + 1);
            const auto decodedTarget = callSiteAddress + 5u + relativeTarget;
            const auto expectedTarget = REL::Offset(kFunc_BhkWorldSetDeltaTime).address();
            if (decodedTarget != expectedTarget) {
                /*
                 * Resolve BOTH pointers to module+offset. That single fact separates
                 * the two explanations this line previously could not tell apart:
                 *   - decoded target inside a loaded DLL  -> that image owns the site,
                 *   - decoded target owned by NO module   -> the call was redirected to
                 *     a VirtualAlloc'd detour trampoline (which is what an already-
                 *     installed hook on this same site looks like).
                 * Neither conclusion is asserted here; the resolved names are printed
                 * and the reader draws it from measured data.
                 */
                /*
                 * FOLLOW ONE MORE HOP. CommonLibF4's Trampoline::write_call<5> does NOT
                 * point the E8 at the hook function: it allocates a 14-byte
                 * `FF 25 00000000 <addr>` stub inside a VirtualAlloc'd trampoline buffer
                 * and points the E8 at that stub. The stub belongs to no mapped module, so
                 * GetModuleHandleEx(FROM_ADDRESS) on the first-level decode always fails
                 * and the host-ownership branch could never fire - the validator reported
                 * a working, host-owned, host-installed feature as FAILED/DISABLED.
                 * decodeBranchDestination already understands the FF 25 shape, so one extra
                 * hop resolves the stub to the real hook function inside the host module.
                 */
                const auto viaTrampoline = decodeBranchDestination(decodedTarget);
                const auto resolvedTarget = viaTrampoline != 0 ? viaTrampoline : decodedTarget;
                if (isOwnedByHostPlugin(resolvedTarget)) {
                    // NOT A FAILURE - see HookSiteOwnership. The host plugin's own
                    // heisenberg::HavokTimingFix installs a write_call<5> detour at this
                    // exact site during plugin init. The caller reports it at INFO.
                    return HookSiteOwnership::HostPluginOwns;
                }
                ROCK_LOG_ERROR(
                    Init,
                    "HAVOK_TIMING_FIX call-site validation FAILED at 0x{:X} ({}): call bytes [{}] decode to 0x{:X} ({}), "
                    "which resolves through any trampoline stub to 0x{:X} ({}); expected 0x{:X} ({})",
                    callSiteAddress,
                    rock::hook_diagnostics::describeAddress(callSiteAddress),
                    rock::hook_diagnostics::formatBytes(callBytes, 5),
                    decodedTarget,
                    rock::hook_diagnostics::describeAddress(decodedTarget),
                    resolvedTarget,
                    rock::hook_diagnostics::describeAddress(resolvedTarget),
                    expectedTarget,
                    rock::hook_diagnostics::describeAddress(expectedTarget));
                return HookSiteOwnership::Unrecognised;
            }

            return HookSiteOwnership::VerifiedNative;
        }

        void hookedBhkWorldSetDeltaTimeWithConfigRead(float rawDeltaSeconds)
        {
            if (!g_originalBhkWorldSetDeltaTime) {
                if (!g_havokTimingFixMissingOriginalLogged.exchange(true, std::memory_order_acq_rel)) {
                    ROCK_LOG_CRITICAL(Init, "HAVOK_TIMING_FIX missing original bhkWorld::SetDeltaTime; timing hook cannot safely continue");
                }
                return;
            }

            g_originalBhkWorldSetDeltaTime(rawDeltaSeconds);

            if (!g_rockConfig.rockHavokTimingFixEnabled) {
                return;
            }

            const float accumulatedDeltaSeconds =
                readBhkWorldFloatGlobal(offsets::kData_BhkWorldAccumulatedDeltaSeconds, rawDeltaSeconds);
            const float oldSubstepDeltaSeconds =
                readBhkWorldFloatGlobal(offsets::kData_BhkWorldSubstepDeltaSeconds, rawDeltaSeconds);
            const auto oldSubstepCount =
                readBhkWorldUintGlobal(offsets::kData_BhkWorldSubstepCount, 1);
            const auto decision = havok_timing_fix_policy::evaluateTimingFix(havok_timing_fix_policy::TimingFixInput{
                .rawDeltaSeconds = rawDeltaSeconds,
                .accumulatedDeltaSeconds = accumulatedDeltaSeconds,
                .minPhysicsFrameRate = g_rockConfig.rockHavokTimingFixMinPhysicsFrameRate,
                .maxSubsteps = g_rockConfig.rockHavokTimingFixMaxSubsteps,
            });

            if (!decision.valid) {
                if (g_rockConfig.rockDebugVerboseLogging || g_rockConfig.rockDebugGrabFrameLogging) {
                    ROCK_LOG_SAMPLE_DEBUG(Physics,
                        g_rockConfig.rockLogSampleMilliseconds,
                        "HAVOK_TIMING_FIX skipped reason={} rawDt={:.6f} accumDt={:.6f} oldSubDt={:.6f} oldSubsteps={}",
                        decision.reason,
                        rawDeltaSeconds,
                        accumulatedDeltaSeconds,
                        oldSubstepDeltaSeconds,
                        oldSubstepCount);
                }
                return;
            }

            const bool wroteSubstepDelta =
                writeBhkWorldFloatGlobal(offsets::kData_BhkWorldSubstepDeltaSeconds, decision.substepDeltaSeconds);
            const bool wroteSubstepCount =
                writeBhkWorldUintGlobal(offsets::kData_BhkWorldSubstepCount, decision.substepCount);
            if (!wroteSubstepDelta || !wroteSubstepCount) {
                if (!g_havokTimingFixWriteFailureLogged.exchange(true, std::memory_order_acq_rel)) {
                    ROCK_LOG_ERROR(Init,
                        "HAVOK_TIMING_FIX failed to write FO4VR timing globals: wroteSubstepDelta={} wroteSubstepCount={}",
                        wroteSubstepDelta ? "yes" : "no",
                        wroteSubstepCount ? "yes" : "no");
                }
                return;
            }

            if (g_rockConfig.rockDebugVerboseLogging || g_rockConfig.rockDebugGrabFrameLogging) {
                ROCK_LOG_SAMPLE_DEBUG(Physics,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "HAVOK_TIMING_FIX rawDt={:.6f} accumDt={:.6f} oldSubDt={:.6f} oldSubsteps={} newSubDt={:.6f} newSubsteps={} minHz={:.2f} maxFrameDt={:.6f} maxSubsteps={}",
                    rawDeltaSeconds,
                    accumulatedDeltaSeconds,
                    oldSubstepDeltaSeconds,
                    oldSubstepCount,
                    decision.substepDeltaSeconds,
                    decision.substepCount,
                    g_rockConfig.rockHavokTimingFixMinPhysicsFrameRate,
                    decision.maxPhysicsFrameSeconds,
                    decision.clampedMaxSubsteps);
            }
        }

        void hookedBhkWorldSetDeltaTime(float rawDeltaSeconds)
        {
            if (!g_rockConfig.tryEnterNativeRead()) {
                /*
                 * The native timing update is never optional. During the
                 * very short config mutation window, bypass only ROCK's
                 * adjustment and preserve Bethesda's original call.
                 */
                if (g_originalBhkWorldSetDeltaTime) {
                    g_originalBhkWorldSetDeltaTime(rawDeltaSeconds);
                } else if (!g_havokTimingFixMissingOriginalLogged.exchange(
                               true,
                               std::memory_order_acq_rel)) {
                    ROCK_LOG_CRITICAL(
                        Init,
                        "HAVOK_TIMING_FIX missing original bhkWorld::SetDeltaTime; timing hook cannot safely continue");
                }
                return;
            }

            hookedBhkWorldSetDeltaTimeWithConfigRead(rawDeltaSeconds);
            g_rockConfig.leaveNativeRead();
        }

        const RE::Actor* resolveNativePlayerActorGlobal()
        {
            static REL::Relocation<RE::Actor**> nativePlayerActor{ REL::Offset(offsets::kData_PlayerActorSingleton) };
            return *nativePlayerActor;
        }

        bool isPlayerActor(const RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }

            const auto* commonLibPlayer = RE::PlayerCharacter::GetSingleton();
            const auto* nativePlayerActor = resolveNativePlayerActorGlobal();
            const auto* actorPtr = reinterpret_cast<const void*>(actor);
            const bool matchesCommonLib = commonLibPlayer && actorPtr == reinterpret_cast<const void*>(commonLibPlayer);
            const bool matchesNative = nativePlayerActor && actorPtr == reinterpret_cast<const void*>(nativePlayerActor);

            if (commonLibPlayer && nativePlayerActor && reinterpret_cast<const void*>(commonLibPlayer) != reinterpret_cast<const void*>(nativePlayerActor)) {
                static std::atomic<bool> loggedDivergence{ false };
                if (!loggedDivergence.exchange(true, std::memory_order_acq_rel)) {
                    ROCK_LOG_WARN(Combat,
                        "Native melee player pointer divergence: CommonLib player={:p}, native actor global={:p}; accepting either for suppression",
                        static_cast<const void*>(commonLibPlayer),
                        static_cast<const void*>(nativePlayerActor));
                }
            }

            return matchesCommonLib || matchesNative;
        }

        bool shouldSuppressNativeVrMeleeVelocity()
        {
            return g_nativeMeleeSuppressionHooksInstalled.load(std::memory_order_acquire) && g_rockConfig.rockEnabled && g_rockConfig.rockNativeMeleeSuppressionEnabled &&
                   g_rockConfig.rockNativeMeleeFullSuppression;
        }

        native_melee_suppression::NativeMeleeInputEvent classifyNativeMeleeInputEvent(const RE::InputEvent* event)
        {
            if (!event) {
                return native_melee_suppression::NativeMeleeInputEvent::Unknown;
            }

            const auto& userEvent = event->QUserEvent();
            const auto* userEventText = userEvent.c_str();
            const std::string_view userEventName{ userEventText ? userEventText : "", userEvent.length() };

            if (userEventName == "RightStick") {
                return native_melee_suppression::NativeMeleeInputEvent::RightStick;
            }
            if (userEventName == "PrimaryAttack") {
                return native_melee_suppression::NativeMeleeInputEvent::PrimaryAttack;
            }
            if (userEventName == "SecondaryAttack") {
                return native_melee_suppression::NativeMeleeInputEvent::SecondaryAttack;
            }

            return native_melee_suppression::NativeMeleeInputEvent::Unknown;
        }

        const char* nativeMeleeInputEventName(native_melee_suppression::NativeMeleeInputEvent event)
        {
            switch (event) {
            case native_melee_suppression::NativeMeleeInputEvent::RightStick:
                return "RightStick";
            case native_melee_suppression::NativeMeleeInputEvent::PrimaryAttack:
                return "PrimaryAttack";
            case native_melee_suppression::NativeMeleeInputEvent::SecondaryAttack:
                return "SecondaryAttack";
            case native_melee_suppression::NativeMeleeInputEvent::Unknown:
            default:
                return "Unknown";
            }
        }

        native_melee_suppression::NativeMeleeInputGatePolicyInput makeNativeMeleeInputGatePolicyInput(
            native_melee_suppression::NativeMeleeInputEvent event)
        {
            return native_melee_suppression::NativeMeleeInputGatePolicyInput{ .rockEnabled = g_rockConfig.rockEnabled,
                .suppressionEnabled = g_rockConfig.rockNativeMeleeSuppressionEnabled,
                .fullSuppression = g_rockConfig.rockNativeMeleeFullSuppression,
                .inputEvent = event };
        }

        native_melee_suppression::NativeMeleeImpactPolicyInput makeNativeMeleeImpactPolicyInput(const RE::Actor* actor)
        {
            return native_melee_suppression::NativeMeleeImpactPolicyInput{ .rockEnabled = g_rockConfig.rockEnabled,
                .suppressionEnabled = g_rockConfig.rockNativeMeleeSuppressionEnabled,
                .fullSuppression = g_rockConfig.rockNativeMeleeFullSuppression,
                .actorIsPlayer = isPlayerActor(actor) };
        }

        RE::Setting* resolveNativeMeleeRuntimeSetting(RE::Setting*& cachedSetting, const char* settingName, bool& missingLogged)
        {
            if (!cachedSetting) {
                cachedSetting = RE::GetINISetting(settingName);
            }

            if (!cachedSetting && !missingLogged) {
                missingLogged = true;
                ROCK_LOG_WARN(Combat, "Native VR melee suppression could not resolve INI setting '{}'", settingName);
            }

            return cachedSetting;
        }

        bool enforceNativeMeleeBinarySetting(NativeBinaryRuntimeSettingState& state, const char* settingName, bool desiredValue, const char* label)
        {
            auto* setting = resolveNativeMeleeRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Combat, "Native VR melee suppression found non-binary setting '{}'", settingName);
                }
                return false;
            }

            if (!state.originalCaptured) {
                state.originalValue = setting->GetBinary();
                state.originalCaptured = true;
                ROCK_LOG_INFO(Combat, "Native VR melee {} setting '{}' resolved: original={}", label, settingName, state.originalValue ? "true" : "false");
            }

            const bool currentValue = setting->GetBinary();
            if (currentValue != desiredValue) {
                setting->SetBinary(desiredValue);
                state.applied = true;
                const auto reapplyCount = state.reapplyCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reapplyCount == 1 || g_rockConfig.rockNativeMeleeDebugLogging || reapplyCount % 30 == 0) {
                    ROCK_LOG_WARN(Combat,
                        "Set FO4VR native VR melee {} setting '{}' to {} (original={} reapplyCount={})",
                        label,
                        settingName,
                        desiredValue ? "true" : "false",
                        state.originalValue ? "true" : "false",
                        reapplyCount);
                }
                state.confirmedLogged = true;
                return true;
            }

            if (!state.confirmedLogged) {
                ROCK_LOG_INFO(Combat, "FO4VR native VR melee {} setting '{}' is {}", label, settingName, desiredValue ? "true" : "false");
                state.confirmedLogged = true;
            }

            return true;
        }

        bool enforceNativeMeleeFloatMinimumSetting(NativeFloatRuntimeSettingState& state, const char* settingName, float desiredMinimum, const char* label)
        {
            auto* setting = resolveNativeMeleeRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kFloat) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Combat, "Native VR melee suppression found non-float setting '{}'", settingName);
                }
                return false;
            }

            if (!state.originalCaptured) {
                state.originalValue = setting->GetFloat();
                state.originalCaptured = true;
                ROCK_LOG_INFO(Combat, "Native VR melee {} setting '{}' resolved: original={:.3f}", label, settingName, state.originalValue);
            }

            const float currentValue = setting->GetFloat();
            if (!std::isfinite(currentValue) || currentValue < desiredMinimum) {
                setting->SetFloat(desiredMinimum);
                state.applied = true;
                const auto reapplyCount = state.reapplyCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reapplyCount == 1 || g_rockConfig.rockNativeMeleeDebugLogging || reapplyCount % 30 == 0) {
                    ROCK_LOG_WARN(Combat,
                        "Raised FO4VR native VR melee {} setting '{}' to {:.1f} (original={:.3f} reapplyCount={})",
                        label,
                        settingName,
                        desiredMinimum,
                        state.originalValue,
                        reapplyCount);
                }
                state.confirmedLogged = true;
                return true;
            }

            if (!state.confirmedLogged) {
                ROCK_LOG_INFO(Combat, "FO4VR native VR melee {} setting '{}' is armed at {:.1f}", label, settingName, currentValue);
                state.confirmedLogged = true;
            }

            return true;
        }

        bool restoreNativeMeleeBinarySetting(NativeBinaryRuntimeSettingState& state, const char* settingName, const char* label)
        {
            if (!state.originalCaptured || !state.applied) {
                return true;
            }

            auto* setting = resolveNativeMeleeRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Combat, "Native VR melee suppression found non-binary setting '{}' while restoring", settingName);
                }
                return false;
            }

            if (setting->GetBinary() != state.originalValue) {
                setting->SetBinary(state.originalValue);
                ROCK_LOG_INFO(Combat,
                    "Restored FO4VR native VR melee {} setting '{}' to {}",
                    label,
                    settingName,
                    state.originalValue ? "true" : "false");
            }
            state.applied = false;
            state.confirmedLogged = false;
            return true;
        }

        bool restoreNativeMeleeFloatSetting(NativeFloatRuntimeSettingState& state, const char* settingName, const char* label)
        {
            if (!state.originalCaptured || !state.applied) {
                return true;
            }

            auto* setting = resolveNativeMeleeRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kFloat) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Combat, "Native VR melee suppression found non-float setting '{}' while restoring", settingName);
                }
                return false;
            }

            const float currentValue = setting->GetFloat();
            if (!std::isfinite(currentValue) || currentValue != state.originalValue) {
                setting->SetFloat(state.originalValue);
                ROCK_LOG_INFO(Combat, "Restored FO4VR native VR melee {} setting '{}' to {:.3f}", label, settingName, state.originalValue);
            }
            state.applied = false;
            state.confirmedLogged = false;
            return true;
        }

        [[nodiscard]] bool nativeMeleeSuppressionApplied()
        {
            return g_nativeMeleeVelocityCheckState.applied ||
                   g_nativeMeleeLinearThresholdState.applied ||
                   g_nativeMeleeAngularThresholdState.applied;
        }

        RE::Setting* resolveNativeGrabHapticRuntimeSetting(RE::Setting*& cachedSetting, const char* settingName, bool& missingLogged)
        {
            if (!cachedSetting) {
                cachedSetting = RE::GetINISetting(settingName);
            }

            if (!cachedSetting && !missingLogged) {
                missingLogged = true;
                ROCK_LOG_WARN(Haptics, "Native grab-hover haptic suppression could not resolve INI setting '{}'", settingName);
            }

            return cachedSetting;
        }

        bool enforceNativeGrabHapticBinarySetting(NativeGrabHapticBinarySettingState& state, const char* settingName, bool desiredValue, const char* label)
        {
            auto* setting = resolveNativeGrabHapticRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Haptics, "Native grab-hover haptic suppression found non-binary setting '{}'", settingName);
                }
                return false;
            }

            if (!state.originalCaptured) {
                state.originalValue = setting->GetBinary();
                state.originalCaptured = true;
                ROCK_LOG_INFO(Haptics, "Native grab-hover {} setting '{}' resolved: original={}", label, settingName, state.originalValue ? "true" : "false");
            }

            const bool currentValue = setting->GetBinary();
            if (currentValue != desiredValue) {
                setting->SetBinary(desiredValue);
                state.applied = true;
                const auto reapplyCount = state.reapplyCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reapplyCount == 1 || reapplyCount % 30 == 0) {
                    ROCK_LOG_WARN(Haptics,
                        "Set FO4VR native grab-hover {} setting '{}' to {} (original={} reapplyCount={})",
                        label,
                        settingName,
                        desiredValue ? "true" : "false",
                        state.originalValue ? "true" : "false",
                        reapplyCount);
                }
                state.confirmedLogged = true;
                return true;
            }

            if (!state.confirmedLogged) {
                ROCK_LOG_INFO(Haptics, "FO4VR native grab-hover {} setting '{}' is {}", label, settingName, desiredValue ? "true" : "false");
                state.confirmedLogged = true;
            }

            return true;
        }

        bool enforceNativeGrabHapticFloatSetting(NativeGrabHapticFloatSettingState& state, const char* settingName, float desiredValue, const char* label)
        {
            auto* setting = resolveNativeGrabHapticRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kFloat) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Haptics, "Native grab-hover haptic suppression found non-float setting '{}'", settingName);
                }
                return false;
            }

            if (!state.originalCaptured) {
                state.originalValue = setting->GetFloat();
                state.originalCaptured = true;
                ROCK_LOG_INFO(Haptics, "Native grab-hover {} setting '{}' resolved: original={:.3f}", label, settingName, state.originalValue);
            }

            const float currentValue = setting->GetFloat();
            if (!std::isfinite(currentValue) || currentValue != desiredValue) {
                setting->SetFloat(desiredValue);
                state.applied = true;
                const auto reapplyCount = state.reapplyCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reapplyCount == 1 || reapplyCount % 30 == 0) {
                    ROCK_LOG_WARN(Haptics,
                        "Set FO4VR native grab-hover {} setting '{}' to {:.3f} (original={:.3f} reapplyCount={})",
                        label,
                        settingName,
                        desiredValue,
                        state.originalValue,
                        reapplyCount);
                }
                state.confirmedLogged = true;
                return true;
            }

            if (!state.confirmedLogged) {
                ROCK_LOG_INFO(Haptics, "FO4VR native grab-hover {} setting '{}' is {:.3f}", label, settingName, desiredValue);
                state.confirmedLogged = true;
            }

            return true;
        }

        bool restoreNativeGrabHapticBinarySetting(NativeGrabHapticBinarySettingState& state, const char* settingName, const char* label)
        {
            if (!state.originalCaptured || !state.applied) {
                return true;
            }

            auto* setting = resolveNativeGrabHapticRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kBinary) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Haptics, "Native grab-hover haptic suppression found non-binary setting '{}' while restoring", settingName);
                }
                return false;
            }

            if (setting->GetBinary() != state.originalValue) {
                setting->SetBinary(state.originalValue);
                ROCK_LOG_INFO(Haptics,
                    "Restored FO4VR native grab-hover {} setting '{}' to {}",
                    label,
                    settingName,
                    state.originalValue ? "true" : "false");
            }
            state.applied = false;
            state.confirmedLogged = false;
            return true;
        }

        bool restoreNativeGrabHapticFloatSetting(NativeGrabHapticFloatSettingState& state, const char* settingName, const char* label)
        {
            if (!state.originalCaptured || !state.applied) {
                return true;
            }

            auto* setting = resolveNativeGrabHapticRuntimeSetting(state.setting, settingName, state.missingLogged);
            if (!setting) {
                return false;
            }

            if (setting->GetType() != RE::Setting::SETTING_TYPE::kFloat) {
                if (!state.typeMismatchLogged) {
                    state.typeMismatchLogged = true;
                    ROCK_LOG_ERROR(Haptics, "Native grab-hover haptic suppression found non-float setting '{}' while restoring", settingName);
                }
                return false;
            }

            const float currentValue = setting->GetFloat();
            if (!std::isfinite(currentValue) || currentValue != state.originalValue) {
                setting->SetFloat(state.originalValue);
                ROCK_LOG_INFO(Haptics, "Restored FO4VR native grab-hover {} setting '{}' to {:.3f}", label, settingName, state.originalValue);
            }
            state.applied = false;
            state.confirmedLogged = false;
            return true;
        }

        [[nodiscard]] bool nativeGrabHapticSuppressionApplied()
        {
            return g_nativeGrabHapticRolloverState.applied || g_nativeGrabHapticHoverIntensityState.applied || g_nativeGrabHapticHoverDurationState.applied;
        }

        bool isLeftSideString(const RE::BSFixedString* side)
        {
            if (!side) {
                return false;
            }

            const char* text = side->c_str();
            if (!text) {
                return false;
            }

            return std::string_view(text) == "Left";
        }

        bool isAnyNativeMeleePhysicalSwingActive()
        {
            const auto currentFrame = g_nativeMeleeFrameClock.load(std::memory_order_acquire);
            return native_melee_suppression::isPhysicalSwingLeaseActive(currentFrame, g_nativeMeleePhysicalSwingExpiresAtFrame[0].load(std::memory_order_acquire)) ||
                   native_melee_suppression::isPhysicalSwingLeaseActive(currentFrame, g_nativeMeleePhysicalSwingExpiresAtFrame[1].load(std::memory_order_acquire));
        }

        bool isNativeMeleePhysicalSwingActiveForSide(const RE::BSFixedString* side)
        {
            if (!side) {
                return isAnyNativeMeleePhysicalSwingActive();
            }

            const bool isLeft = isLeftSideString(side);
            const auto currentFrame = g_nativeMeleeFrameClock.load(std::memory_order_acquire);
            return native_melee_suppression::isPhysicalSwingLeaseActive(
                currentFrame, g_nativeMeleePhysicalSwingExpiresAtFrame[isLeft ? 1 : 0].load(std::memory_order_acquire));
        }

        native_melee_suppression::NativeMeleePolicyInput makeNativeMeleePolicyInput(
            const native_melee_suppression::NativeMeleeEvent event, const RE::Actor* actor, const RE::BSFixedString* side)
        {
            return native_melee_suppression::NativeMeleePolicyInput{ .rockEnabled = g_rockConfig.rockEnabled,
                .suppressionEnabled = g_rockConfig.rockNativeMeleeSuppressionEnabled,
                .fullSuppression = g_rockConfig.rockNativeMeleeFullSuppression,
                .suppressWeaponSwing = g_rockConfig.rockNativeMeleeSuppressWeaponSwing,
                .suppressHitFrame = g_rockConfig.rockNativeMeleeSuppressHitFrame,
                .actorIsPlayer = isPlayerActor(actor),
                .physicalSwingActive = event == native_melee_suppression::NativeMeleeEvent::HitFrame ? isNativeMeleePhysicalSwingActiveForSide(side)
                                                                                                     : isAnyNativeMeleePhysicalSwingActive() };
        }

        bool applyNativeMeleeDecision(const native_melee_suppression::NativeMeleeEvent event,
            const native_melee_suppression::NativeMeleePolicyInput& input,
            const native_melee_suppression::NativeMeleePolicyDecision& decision)
        {
            using native_melee_suppression::NativeMeleeEvent;
            using native_melee_suppression::NativeMeleeSuppressionAction;

            if (g_rockConfig.rockNativeMeleeDebugLogging || decision.action != NativeMeleeSuppressionAction::CallNative) {
                static std::atomic<std::uint32_t> weaponLogCounter{ 0 };
                static std::atomic<std::uint32_t> hitFrameLogCounter{ 0 };
                auto& counter = event == NativeMeleeEvent::WeaponSwing ? weaponLogCounter : hitFrameLogCounter;
                const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1;

                if (count == 1 || (g_rockConfig.rockNativeMeleeDebugLogging && count % 45 == 0) || count % 180 == 0) {
                    ROCK_LOG_DEBUG(Combat, "Native melee {} decision={} reason={} player={} physicalSwing={} count={}",
                        event == NativeMeleeEvent::WeaponSwing ? "WeaponSwing" : "HitFrame",
                        decision.action == NativeMeleeSuppressionAction::CallNative      ? "native"
                            : decision.action == NativeMeleeSuppressionAction::ReturnHandled ? "handled"
                                                                                              : "unhandled",
                        decision.reason, input.actorIsPlayer ? "yes" : "no", input.physicalSwingActive ? "yes" : "no", count);
                }
            }

            switch (decision.action) {
            case native_melee_suppression::NativeMeleeSuppressionAction::CallNative:
                return true;
            case native_melee_suppression::NativeMeleeSuppressionAction::ReturnHandled:
                return true;
            case native_melee_suppression::NativeMeleeSuppressionAction::ReturnUnhandled:
                return false;
            }

            return true;
        }

        bool applyNativeMeleeImpactDecision(const native_melee_suppression::NativeMeleeImpactPolicyInput& input,
            const native_melee_suppression::NativeMeleeImpactPolicyDecision& decision)
        {
            using native_melee_suppression::NativeMeleeImpactAction;

            if (g_rockConfig.rockNativeMeleeDebugLogging || decision.action != NativeMeleeImpactAction::CallNative) {
                static std::atomic<std::uint32_t> impactLogCounter{ 0 };
                const auto count = impactLogCounter.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count == 1 || (g_rockConfig.rockNativeMeleeDebugLogging && count % 45 == 0) || count % 180 == 0) {
                    ROCK_LOG_DEBUG(Combat,
                        "Native melee VRMeleeImpact decision={} reason={} player={} full={} count={}",
                        decision.action == NativeMeleeImpactAction::CallNative ? "native" : "suppressed",
                        decision.reason,
                        input.actorIsPlayer ? "yes" : "no",
                        input.fullSuppression ? "yes" : "no",
                        count);
                }
            }

            return decision.action == NativeMeleeImpactAction::Suppress;
        }

        bool hookedWeaponSwingHandler(void* handler, RE::Actor* actor, RE::BSFixedString* side)
        {
            if (!g_rockConfig.tryEnterNativeRead()) {
                return g_originalWeaponSwingHandler ?
                           g_originalWeaponSwingHandler(handler, actor, side) :
                           false;
            }

            const auto input = makeNativeMeleePolicyInput(native_melee_suppression::NativeMeleeEvent::WeaponSwing, actor, side);
            const auto decision = native_melee_suppression::evaluateNativeMeleeSuppression(native_melee_suppression::NativeMeleeEvent::WeaponSwing, input);

            const bool shouldCallNative = decision.action == native_melee_suppression::NativeMeleeSuppressionAction::CallNative;
            const bool decisionResult = applyNativeMeleeDecision(native_melee_suppression::NativeMeleeEvent::WeaponSwing, input, decision);
            g_rockConfig.leaveNativeRead();
            return shouldCallNative ? (g_originalWeaponSwingHandler ? g_originalWeaponSwingHandler(handler, actor, side) : false) : decisionResult;
        }

        bool hookedHitFrameHandler(void* handler, RE::Actor* actor, RE::BSFixedString* side)
        {
            if (!g_rockConfig.tryEnterNativeRead()) {
                return g_originalHitFrameHandler ?
                           g_originalHitFrameHandler(handler, actor, side) :
                           false;
            }

            const auto input = makeNativeMeleePolicyInput(native_melee_suppression::NativeMeleeEvent::HitFrame, actor, side);
            const auto decision = native_melee_suppression::evaluateNativeMeleeSuppression(native_melee_suppression::NativeMeleeEvent::HitFrame, input);

            const bool shouldCallNative = decision.action == native_melee_suppression::NativeMeleeSuppressionAction::CallNative;
            const bool decisionResult = applyNativeMeleeDecision(native_melee_suppression::NativeMeleeEvent::HitFrame, input, decision);
            g_rockConfig.leaveNativeRead();
            return shouldCallNative ? (g_originalHitFrameHandler ? g_originalHitFrameHandler(handler, actor, side) : false) : decisionResult;
        }

        /*
         * MELEE OBSERVATION counters (Jul 31). Written at the top of the two
         * observation hooks, BEFORE the config read-lock (the lock can be held
         * by a live INI reload, and this logging must not miss events during
         * one). Paired semantics: a SWING line with no following IMPACT line
         * within ~1s is a native miss; SWING->IMPACT is a hit. Counters make
         * interleaved or dropped lines recoverable. Pointer VALUES only are
         * ever logged from the impact callback — contactEvent/collisionEvent
         * have no verified layout (prior hknp manifold offsets in this project
         * were proven wrong), so they are never dereferenced.
         */
        static std::atomic<std::uint32_t> g_playerMeleeSwingCount{ 0 };
        static std::atomic<std::uint32_t> g_playerMeleeImpactCount{ 0 };
        static std::atomic<std::uint64_t> g_playerMeleeLastSwingMs{ 0 };
        static std::atomic<std::uint32_t> g_playerMeleeFilteredCount{ 0 };

        // Read the OTHER contact body's collision filter word from a
        // VRMeleeImpact (contactEvent, collisionEvent) pair. Offsets are
        // Ghidra-verified against FUN_140eff000 and
        // bhkNPCollisionObject::GetCollisionFilterInfo on FO4VR 1.2.72:
        //   world   = *(uintptr*)contactEvent
        //   slot    = *(uint*)(contactEvent + 0x20)            (native uses low 32, expects 0/1)
        //   otherId = *(int*)(collisionEvent + 8 + (1-slot)*4)
        //   body    = *(uintptr*)(world + 0x20) + otherId*0x90
        //   filter  = *(uint*)(body + 0x44)
        // SEH-guarded (pure C, no C++ objects in scope, per the SEH-vs-C++
        // rule): any fault or implausible value returns false so the caller
        // forwards the contact to native unchanged (vanilla behaviour).
        static bool tryReadMeleeContactPartnerFilter(
            const void* contactEvent,
            const void* collisionEvent,
            std::uint32_t& outFilter) noexcept
        {
            __try {
                if (!contactEvent || !collisionEvent) {
                    return false;
                }
                const auto* ce = static_cast<const std::uint8_t*>(contactEvent);
                const auto* ke = static_cast<const std::uint8_t*>(collisionEvent);
                const std::uintptr_t world =
                    *reinterpret_cast<const std::uintptr_t*>(ce);
                if (world == 0) {
                    return false;
                }
                const std::uint32_t slot =
                    *reinterpret_cast<const std::uint32_t*>(ce + 0x20);
                if (slot > 1u) {
                    return false;  // native assumes the pair selector is 0 or 1
                }
                const std::size_t otherOffset =
                    static_cast<std::size_t>(8u + (1u - slot) * 4u);
                const std::int32_t otherId =
                    *reinterpret_cast<const std::int32_t*>(ke + otherOffset);
                if (otherId < 0 || otherId > 0x000FFFFF) {
                    return false;  // implausible body index
                }
                const std::uintptr_t bodyArray =
                    *reinterpret_cast<const std::uintptr_t*>(world + 0x20);
                if (bodyArray == 0) {
                    return false;
                }
                const std::uintptr_t body =
                    bodyArray + static_cast<std::uintptr_t>(otherId) * 0x90u;
                outFilter = *reinterpret_cast<const std::uint32_t*>(body + 0x44);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // True when a decoded collision filter word belongs to one of our
        // generated collider families (hand/weapon/body/dynamic-hand-proxy) —
        // a contact the native melee handler must NOT process, or it arms the
        // global melee cooldown and eats the real hit. (A body already carrying
        // the suppression no-collide bit generates no contacts at all, so it
        // never reaches here as a partner; the layer test is the discriminator.)
        [[nodiscard]] static bool isRockGeneratedMeleeContact(std::uint32_t filterInfo) noexcept
        {
            const std::uint32_t layer = filterInfo & collision_layer_policy::FO4_LAYER_FILTER_MASK;
            return layer == collision_layer_policy::ROCK_LAYER_HAND ||
                   layer == collision_layer_policy::ROCK_LAYER_WEAPON ||
                   layer == collision_layer_policy::ROCK_LAYER_BODY ||
                   layer == collision_layer_policy::ROCK_LAYER_DYNAMIC_HAND_PROXY;
        }

        void hookedPlayerWeaponSwingCallback(RE::Actor* actor, std::uint32_t equipIndex)
        {
            if (isPlayerActor(actor)) {
                const auto swingNumber =
                    g_playerMeleeSwingCount.fetch_add(1, std::memory_order_acq_rel) + 1;
                g_playerMeleeLastSwingMs.store(GetTickCount64(), std::memory_order_release);
                ROCK_LOG_INFO(Combat,
                    "Player melee SWING #{} (equipIndex={})",
                    swingNumber,
                    equipIndex);
            }

            /*
             * FO4VR can reach PlayerCharacter::WeaponSwingCallBack separately
             * from the WeaponSwing animation handler vtable. Ghidra verified the
             * PlayerCharacter vtable slot and showed that the callback dispatches
             * weapon-swing side effects, so full native suppression must stop it
             * at this player-only boundary without changing NPC swing behavior.
             */
            if (!g_rockConfig.tryEnterNativeRead()) {
                if (g_originalPlayerWeaponSwingCallback) {
                    g_originalPlayerWeaponSwingCallback(actor, equipIndex);
                }
                return;
            }

            const auto input = makeNativeMeleePolicyInput(native_melee_suppression::NativeMeleeEvent::WeaponSwing, actor, nullptr);
            const auto decision = native_melee_suppression::evaluateNativeMeleeSuppression(native_melee_suppression::NativeMeleeEvent::WeaponSwing, input);
            const bool shouldCallNative = decision.action == native_melee_suppression::NativeMeleeSuppressionAction::CallNative;

            if (!shouldCallNative) {
                applyNativeMeleeDecision(native_melee_suppression::NativeMeleeEvent::WeaponSwing, input, decision);
                g_rockConfig.leaveNativeRead();
                return;
            }

            g_rockConfig.leaveNativeRead();
            if (g_originalPlayerWeaponSwingCallback) {
                g_originalPlayerWeaponSwingCallback(actor, equipIndex);
            }
        }

        void hookedVrMeleeImpactCallback(RE::Actor* actor, void* contactEvent, void* collisionEvent)
        {
            if (isPlayerActor(actor)) {
                // LIVE-CONFIRMED (Jul 31, 06:59): VRMeleeImpact fires per contact
                // TICK, not per hit — one swing with the bat left resting in
                // contact produced 421 callbacks over 4.5s and flooded the log.
                // Log per-event only the FIRST contact of each swing (that is
                // the hit/contact confirmation the miss diagnostic needs); the
                // continuous tail goes through the cumulative-counter sampled
                // pattern so its exact count stays recoverable from deltas.
                const auto impactNumber =
                    g_playerMeleeImpactCount.fetch_add(1, std::memory_order_acq_rel) + 1;
                const auto swingNumber =
                    g_playerMeleeSwingCount.load(std::memory_order_acquire);
                const auto lastSwingMs =
                    g_playerMeleeLastSwingMs.load(std::memory_order_acquire);
                const auto nowMs = GetTickCount64();
                const auto sinceSwingMs =
                    lastSwingMs != 0 && nowMs >= lastSwingMs
                        ? static_cast<std::int64_t>(nowMs - lastSwingMs)
                        : static_cast<std::int64_t>(-1);
                static std::atomic<std::uint32_t> s_lastImpactLoggedSwing{ 0 };
                auto expectedSwing = s_lastImpactLoggedSwing.load(std::memory_order_acquire);
                const bool firstContactOfSwing =
                    expectedSwing != swingNumber &&
                    s_lastImpactLoggedSwing.compare_exchange_strong(
                        expectedSwing, swingNumber, std::memory_order_acq_rel);
                if (firstContactOfSwing) {
                    ROCK_LOG_INFO(Combat,
                        "Native melee CONTACT for swing #{} ({} ms after swing) — the game's melee "
                        "collider touched something (impact #{} total) actor={:p} contactEvent={:p} collisionEvent={:p}",
                        swingNumber,
                        sinceSwingMs,
                        impactNumber,
                        static_cast<void*>(actor),
                        contactEvent,
                        collisionEvent);
                } else {
                    ROCK_LOG_SAMPLE_INFO(Combat,
                        g_rockConfig.rockLogSampleMilliseconds,
                        "Native melee contact continuing (swing #{}, cumulativeImpacts={}, {} ms after swing)",
                        swingNumber,
                        impactNumber,
                        sinceSwingMs);
                }
            }

            /*
             * PRECISE FIRST-HIT FIX (Jul 31): drop contacts whose partner is one
             * of OUR generated colliders BEFORE native sees them. The native
             * handler arms a global melee cooldown (Actor staminaModifiers,
             * player+0x908) on every processed contact and early-returns while
             * it is >0 — so our co-located hull/hand/body bodies were burning
             * the cooldown at swing onset and the real NPC contact landed no
             * damage. Not forwarding our-body contacts means native never arms
             * the cooldown from us; genuine NPC/world contacts (different layers)
             * pass straight through and the hull stays collidable for world
             * clank. Runs for the player only; the decode is SEH-guarded and
             * fails safe to forwarding.
             */
            if (isPlayerActor(actor) && g_rockConfig.rockNativeMeleeContactFilterEnabled) {
                std::uint32_t partnerFilter = 0;
                if (tryReadMeleeContactPartnerFilter(contactEvent, collisionEvent, partnerFilter) &&
                    isRockGeneratedMeleeContact(partnerFilter)) {
                    const auto filtered =
                        g_playerMeleeFilteredCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    ROCK_LOG_SAMPLE_INFO(Combat,
                        g_rockConfig.rockLogSampleMilliseconds,
                        "Native melee contact DROPPED — partner is a ROCK collider (filter=0x{:08X} layer={}); "
                        "protected the native melee cooldown (cumulativeDropped={})",
                        partnerFilter,
                        partnerFilter & 0x7Fu,
                        filtered);
                    return;
                }
            }

            /*
             * FO4VR registers this callback while attaching native VR melee
             * collision to the first-person weapon nodes. It owns the native
             * contact-to-hit path, including target filtering, action dispatch,
             * impulse direction, and melee cooldown writes. Full ROCK native
             * suppression skips the player callback here so SCISSORS can own the
             * replacement point-collision damage path without a duplicate native
             * impact firing from the same swing.
             */
            if (!g_rockConfig.tryEnterNativeRead()) {
                if (g_originalVrMeleeImpactCallback) {
                    g_originalVrMeleeImpactCallback(actor, contactEvent, collisionEvent);
                }
                return;
            }

            const auto input = makeNativeMeleeImpactPolicyInput(actor);
            const auto decision = native_melee_suppression::evaluateNativeMeleeImpactSuppression(input);

            if (applyNativeMeleeImpactDecision(input, decision)) {
                ROCK_LOG_SAMPLE_DEBUG(Combat,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "Suppressed FO4VR native VRMeleeImpact callback actor={:p} contactEvent={:p} collisionEvent={:p}",
                    static_cast<void*>(actor),
                    contactEvent,
                    collisionEvent);
                g_rockConfig.leaveNativeRead();
                return;
            }

            g_rockConfig.leaveNativeRead();
            if (g_originalVrMeleeImpactCallback) {
                g_originalVrMeleeImpactCallback(actor, contactEvent, collisionEvent);
            }
        }

        bool hookedAttackBlockShouldHandleEvent(void* handler, const RE::InputEvent* event)
        {
            if (!g_rockConfig.tryEnterNativeRead()) {
                return g_originalAttackBlockShouldHandleEvent ?
                           g_originalAttackBlockShouldHandleEvent(handler, event) :
                           false;
            }

            if (input_remap_runtime::shouldSuppressNativeTriggerAction(event)) {
                ROCK_LOG_SAMPLE_DEBUG(Input,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "Suppressed native WandTrigger AttackBlock gate while ROCK owns holstered or held-weapon trigger input");
                g_rockConfig.leaveNativeRead();
                return false;
            }

            /*
             * FO4VR has a third native melee path before the animation events:
             * AttackBlockHandler::ShouldHandleEvent accepts the VR RightStick
             * input and can classify fast rotation as melee. The hook is kept
             * at this input gate and only returns false for RightStick during
             * full ROCK suppression; PrimaryAttack and SecondaryAttack remain
             * native so normal weapon inputs are not segregated or broken.
             */
            const auto inputEvent = classifyNativeMeleeInputEvent(event);
            const auto policyInput = makeNativeMeleeInputGatePolicyInput(inputEvent);
            const auto decision = native_melee_suppression::evaluateNativeMeleeInputGate(policyInput);

            if (g_rockConfig.rockNativeMeleeDebugLogging || decision.action != native_melee_suppression::NativeMeleeInputGateAction::CallNative) {
                static std::atomic<std::uint32_t> inputGateLogCounter{ 0 };
                const auto count = inputGateLogCounter.fetch_add(1, std::memory_order_relaxed) + 1;
                if (count == 1 || (g_rockConfig.rockNativeMeleeDebugLogging && count % 45 == 0) || count % 180 == 0) {
                    ROCK_LOG_DEBUG(Combat,
                        "Native melee AttackBlock input gate event={} decision={} reason={} count={}",
                        nativeMeleeInputEventName(inputEvent),
                        decision.action == native_melee_suppression::NativeMeleeInputGateAction::CallNative ? "native" : "false",
                        decision.reason,
                        count);
                }
            }

            if (decision.action == native_melee_suppression::NativeMeleeInputGateAction::ReturnFalse) {
                g_rockConfig.leaveNativeRead();
                return false;
            }

            g_rockConfig.leaveNativeRead();
            return g_originalAttackBlockShouldHandleEvent ? g_originalAttackBlockShouldHandleEvent(handler, event) : false;
        }

        template <class HandlerT>
        bool installNativeMeleeVtableHook(
            std::uintptr_t entryOffset,
            std::uintptr_t expectedFunctionOffset,
            HandlerT hook,
            HandlerT& original,
            const char* label)
        {
            if (entryOffset == 0 || expectedFunctionOffset == 0) {
                ROCK_LOG_ERROR(Init,
                    "FAILED to install {} hook: entry or expected native target offset is unset",
                    label);
                return false;
            }

            REL::Relocation<std::uintptr_t> entry{ REL::Offset(entryOffset) };
            auto* slot = reinterpret_cast<std::uintptr_t*>(entry.address());
            if (!slot) {
                ROCK_LOG_ERROR(Init, "FAILED to install {} hook: vtable slot is null", label);
                return false;
            }

            const auto hookAddress = reinterpret_cast<std::uintptr_t>(hook);
            const auto currentTarget = *slot;
            const auto expectedTarget = REL::Offset(expectedFunctionOffset).address();
            if (currentTarget == hookAddress) {
                ROCK_LOG_INFO(Init, "{} hook already installed at 0x{:X}", label, entry.address());
                return original != nullptr;
            }

            if (currentTarget != expectedTarget) {
                ROCK_LOG_ERROR(Init,
                    "FAILED to install {} hook at 0x{:X}: target changed after validation (found=0x{:X}, expected exact native=0x{:X}); no write performed",
                    label,
                    entry.address(),
                    currentTarget,
                    expectedTarget);
                return false;
            }
            original = reinterpret_cast<HandlerT>(expectedTarget);

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(*slot), kPageExecuteReadWrite, &oldProtect)) {
                ROCK_LOG_ERROR(Init, "FAILED to install {} hook at 0x{:X}: VirtualProtect failed", label, entry.address());
                original = nullptr;
                return false;
            }

            if (*slot != expectedTarget) {
                ROCK_LOG_ERROR(Init,
                    "FAILED to install {} hook at 0x{:X}: target changed while page protection was open (found=0x{:X}, expected exact native=0x{:X}); no write performed",
                    label,
                    entry.address(),
                    *slot,
                    expectedTarget);
                VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
                original = nullptr;
                return false;
            }

            *slot = hookAddress;
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);

            ROCK_LOG_INFO(Init, "Installed {} vtable hook at 0x{:X}, original=0x{:X}, hook=0x{:X}", label, entry.address(), reinterpret_cast<std::uintptr_t>(original),
                hookAddress);
            return true;
        }

        template <class HandlerT>
        bool restoreNativeMeleeVtableHook(std::uintptr_t entryOffset, HandlerT hook, HandlerT& original, bool& installed, const char* label)
        {
            if (!installed) {
                return true;
            }

            REL::Relocation<std::uintptr_t> entry{ REL::Offset(entryOffset) };
            auto* slot = reinterpret_cast<std::uintptr_t*>(entry.address());
            if (!slot || !original) {
                ROCK_LOG_ERROR(Init, "FAILED to restore {} hook: slot or original is null", label);
                return false;
            }

            const auto hookAddress = reinterpret_cast<std::uintptr_t>(hook);
            const auto originalAddress = reinterpret_cast<std::uintptr_t>(original);
            if (*slot != hookAddress) {
                ROCK_LOG_WARN(Init, "Skipped {} rollback: slot 0x{:X} no longer points to ROCK hook", label, entry.address());
                installed = false;
                original = nullptr;
                return true;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(*slot), kPageExecuteReadWrite, &oldProtect)) {
                ROCK_LOG_ERROR(Init, "FAILED to restore {} hook at 0x{:X}: VirtualProtect failed", label, entry.address());
                return false;
            }

            *slot = originalAddress;
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(*slot));
            VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);

            ROCK_LOG_WARN(Init, "Rolled back {} vtable hook at 0x{:X}", label, entry.address());
            installed = false;
            original = nullptr;
            return true;
        }
    }

    bool validateNativeMeleeSuppressionHookTargets()
    {
        const bool swingValid = validateNativeMeleeVtableTarget(
            offsets::kVtableEntry_WeaponSwingHandler_Handle, offsets::kFunc_WeaponSwingHandler_Handle, "WeaponSwingHandler::Handle");
        const bool hitFrameValid =
            validateNativeMeleeVtableTarget(offsets::kVtableEntry_HitFrameHandler_Handle, offsets::kFunc_HitFrameHandler_Handle, "HitFrameHandler::Handle");
        const bool attackBlockValid = validateNativeMeleeVtableTarget(offsets::kVtableEntry_AttackBlockHandler_ShouldHandleEvent,
            offsets::kFunc_AttackBlockHandler_ShouldHandleEvent,
            "AttackBlockHandler::ShouldHandleEvent");
        const bool playerSwingCallbackValid = validateNativeMeleeVtableTarget(offsets::kVtableEntry_PlayerCharacter_WeaponSwingCallBack,
            offsets::kFunc_PlayerCharacter_WeaponSwingCallBack,
            "PlayerCharacter::WeaponSwingCallBack");
        const bool vrMeleeImpactValid = validateEntryTrampolineTarget(
            "VRMeleeImpact", offsets::kFunc_VRMeleeImpactCallback, kVrMeleeImpactExpectedPrefix.data(), kVrMeleeImpactExpectedPrefix.size());

        const bool allValid = swingValid && hitFrameValid && attackBlockValid && playerSwingCallbackValid && vrMeleeImpactValid;
        if (!allValid) {
            // Name the failing target(s) explicitly. The old single-line failure said
            // only "validation failed", so a reader had to guess which of the five it
            // was - and the per-target lines above scroll far apart in a busy log.
            ROCK_LOG_ERROR(Init,
                "Native melee suppression target validation summary: weaponSwing={} hitFrame={} attackBlock={} playerSwingCallback={} vrMeleeImpact={} "
                "(each failing entry printed its found-vs-expected report above)",
                swingValid ? "ok" : "FAILED",
                hitFrameValid ? "ok" : "FAILED",
                attackBlockValid ? "ok" : "FAILED",
                playerSwingCallbackValid ? "ok" : "FAILED",
                vrMeleeImpactValid ? "ok" : "FAILED");
        }
        return allValid;
    }

    void setNativeMeleePhysicalSwingActive(bool isLeft, bool active)
    {
        const auto currentFrame = g_nativeMeleeFrameClock.load(std::memory_order_acquire);
        const auto expiresAtFrame = active ? (currentFrame + kNativeMeleePhysicalSwingLeaseFrames) : 0;
        g_nativeMeleePhysicalSwingExpiresAtFrame[isLeft ? 1 : 0].store(expiresAtFrame, std::memory_order_release);
    }

    bool isNativeMeleePhysicalSwingActive(bool isLeft)
    {
        const auto currentFrame = g_nativeMeleeFrameClock.load(std::memory_order_acquire);
        return native_melee_suppression::isPhysicalSwingLeaseActive(
            currentFrame, g_nativeMeleePhysicalSwingExpiresAtFrame[isLeft ? 1 : 0].load(std::memory_order_acquire));
    }

    void advanceNativeMeleeFrameClock()
    {
        g_nativeMeleeFrameClock.fetch_add(1, std::memory_order_acq_rel);
    }

    void clearNativeMeleePhysicalSwingLeases()
    {
        g_nativeMeleePhysicalSwingExpiresAtFrame[0].store(0, std::memory_order_release);
        g_nativeMeleePhysicalSwingExpiresAtFrame[1].store(0, std::memory_order_release);
    }

    void enforceNativeMeleeRuntimeSuppression(bool forceCheck)
    {
        /*
         * FO4VR owns two native player melee paths: animation events
         * (WeaponSwing/HitFrame) and a VRInput velocity gate that can classify
         * controller or HMD motion as melee. Full suppression keeps that gate
         * enabled and pushes its thresholds out of reach; disabling the boolean
         * bypasses the gate on some builds and causes cooldown-paced false melee
         * swings.
         */
        const auto currentFrame = g_nativeMeleeFrameClock.load(std::memory_order_acquire);
        const bool shouldSuppress = shouldSuppressNativeVrMeleeVelocity();
        const bool shouldRestore = !shouldSuppress && nativeMeleeSuppressionApplied();
        if (!shouldSuppress && !shouldRestore) {
            return;
        }

        const auto nextCheckFrame = g_nativeMeleeRuntimeSettingNextCheckFrame.load(std::memory_order_acquire);
        if (!forceCheck && currentFrame < nextCheckFrame) {
            return;
        }
        g_nativeMeleeRuntimeSettingNextCheckFrame.store(currentFrame + kNativeMeleeRuntimeSettingCheckIntervalFrames, std::memory_order_release);

        if (shouldSuppress) {
            enforceNativeMeleeBinarySetting(g_nativeMeleeVelocityCheckState, kNativeMeleeVelocityCheckSetting, true, "velocity gate");
            enforceNativeMeleeFloatMinimumSetting(
                g_nativeMeleeLinearThresholdState, kNativeMeleeLinearVelocityThresholdSetting, kNativeMeleeSuppressedVelocityThreshold, "linear threshold");
            enforceNativeMeleeFloatMinimumSetting(
                g_nativeMeleeAngularThresholdState, kNativeMeleeAngularVelocityThresholdSetting, kNativeMeleeSuppressedVelocityThreshold, "angular threshold");
            return;
        }

        restoreNativeMeleeBinarySetting(g_nativeMeleeVelocityCheckState, kNativeMeleeVelocityCheckSetting, "velocity gate");
        restoreNativeMeleeFloatSetting(g_nativeMeleeLinearThresholdState, kNativeMeleeLinearVelocityThresholdSetting, "linear threshold");
        restoreNativeMeleeFloatSetting(g_nativeMeleeAngularThresholdState, kNativeMeleeAngularVelocityThresholdSetting, "angular threshold");
    }

    void enforceNativeGrabHapticRuntimeSuppression(bool forceCheck)
    {
        /*
         * FO4VR's native grabbable-object affordance is exposed through the VR
         * hover/rollover rumble settings. Suppressing those settings leaves
         * ROCK-owned haptics untouched because ROCK emits haptics directly
         * through its feedback pipeline instead of through native rollover UI.
         */
        const native_grab_haptic_suppression::RuntimeInput input{
            .rockEnabled = g_rockConfig.rockEnabled,
            .suppressionEnabled = g_rockConfig.rockSuppressNativeGrabHoverHaptics,
        };
        const bool shouldSuppress = native_grab_haptic_suppression::shouldSuppressNativeGrabHoverHaptics(input);
        const bool shouldRestore = native_grab_haptic_suppression::shouldRestoreNativeGrabHoverHaptics(nativeGrabHapticSuppressionApplied(), input);
        if (!shouldSuppress && !shouldRestore) {
            return;
        }

        const auto currentFrame = g_nativeMeleeFrameClock.load(std::memory_order_acquire);
        const auto nextCheckFrame = g_nativeGrabHapticRuntimeSettingNextCheckFrame.load(std::memory_order_acquire);
        if (!forceCheck && currentFrame < nextCheckFrame) {
            return;
        }
        g_nativeGrabHapticRuntimeSettingNextCheckFrame.store(currentFrame + kNativeGrabHapticRuntimeSettingCheckIntervalFrames, std::memory_order_release);

        if (shouldSuppress) {
            enforceNativeGrabHapticBinarySetting(g_nativeGrabHapticRolloverState,
                native_grab_haptic_suppression::kRolloverRumbleEnabledSetting,
                native_grab_haptic_suppression::kSuppressedRolloverRumbleEnabled,
                "rollover rumble");
            enforceNativeGrabHapticFloatSetting(g_nativeGrabHapticHoverIntensityState,
                native_grab_haptic_suppression::kHoverRumbleIntensitySetting,
                native_grab_haptic_suppression::kSuppressedHoverRumbleFloat,
                "hover intensity");
            enforceNativeGrabHapticFloatSetting(g_nativeGrabHapticHoverDurationState,
                native_grab_haptic_suppression::kHoverRumbleDurationSetting,
                native_grab_haptic_suppression::kSuppressedHoverRumbleFloat,
                "hover duration");
            return;
        }

        restoreNativeGrabHapticBinarySetting(g_nativeGrabHapticRolloverState,
            native_grab_haptic_suppression::kRolloverRumbleEnabledSetting,
            "rollover rumble");
        restoreNativeGrabHapticFloatSetting(g_nativeGrabHapticHoverIntensityState,
            native_grab_haptic_suppression::kHoverRumbleIntensitySetting,
            "hover intensity");
        restoreNativeGrabHapticFloatSetting(g_nativeGrabHapticHoverDurationState,
            native_grab_haptic_suppression::kHoverRumbleDurationSetting,
            "hover duration");
    }

    using HandleBumpedCharacter_t = void (*)(void*, void*, void*);
    static HandleBumpedCharacter_t g_originalHandleBumped = nullptr;
    /*
     * FAIL-SAFE LATCH for the character-bump filter. installBumpHook() sets this;
     * isNativeCharacterBumpFilterInstalled() is the single truth source for "is
     * ROCK suppressing player character-controller bumps?". Nothing may infer that
     * from the config flag alone: rockNativeCharacterControllerObjectContactFilterEnabled
     * also drives the CC-vs-OBJECT filter, the CHARCONTROLLER layer-matrix rewrite
     * and the large-blocking-object car fix (#219/#220), so this hook failing must
     * not switch those off as collateral damage.
     */
    static std::atomic<bool> g_nativeCharacterBumpFilterInstalled{ false };

    static void writeAbsoluteJump(std::uint8_t* target, std::uintptr_t destination)
    {
        target[0] = 0xFF;
        target[1] = 0x25;
        target[2] = 0x00;
        target[3] = 0x00;
        target[4] = 0x00;
        target[5] = 0x00;
        *reinterpret_cast<std::uintptr_t*>(target + 6) = destination;
    }

    static bool installEntryTrampolineHook(const char* label,
        std::uintptr_t targetOffset,
        const std::uint8_t* expectedPrefix,
        std::size_t stolenBytes,
        void* hook,
        void*& original)
    {
        if (stolenBytes < 14) {
            ROCK_LOG_ERROR(Init, "{} hook install failed: stolen byte count {} cannot hold an absolute jump", label, stolenBytes);
            return false;
        }

        REL::Relocation<std::uintptr_t> target{ REL::Offset(targetOffset) };
        auto* targetAddr = reinterpret_cast<std::uint8_t*>(target.address());
        if (!targetAddr || !expectedPrefix) {
            ROCK_LOG_ERROR(Init, "{} hook install failed: target or validation bytes are null", label);
            return false;
        }

        if (std::memcmp(targetAddr, expectedPrefix, stolenBytes) != 0) {
            // Found vs expected, with module attribution for the target and for any
            // detour branch now occupying the entry. State findings only - do not
            // name a suspected owner that has not been resolved.
            ROCK_LOG_ERROR(Init,
                "{} hook NOT INSTALLED - entry prefix validation failed: {}",
                label,
                rock::hook_diagnostics::describePrefixMismatch(target.address(), expectedPrefix, stolenBytes));
            return false;
        }

        constexpr std::size_t kJumpBytes = 14;
        const std::size_t trampolineBytes = stolenBytes + kJumpBytes;
        auto* trampolineMem = reinterpret_cast<std::uint8_t*>(VirtualAlloc(nullptr, trampolineBytes, kVirtualMemoryCommitReserve, kPageExecuteReadWrite));
        if (!trampolineMem) {
            ROCK_LOG_ERROR(Init, "{} hook install failed: trampoline allocation failed", label);
            return false;
        }

        std::memcpy(trampolineMem, targetAddr, stolenBytes);
        writeAbsoluteJump(trampolineMem + stolenBytes, target.address() + stolenBytes);

        DWORD oldTrampolineProtect = 0;
        if (!VirtualProtect(trampolineMem, trampolineBytes, kPageExecuteRead, &oldTrampolineProtect)) {
            ROCK_LOG_ERROR(Init, "{} hook install failed: trampoline protection failed", label);
            VirtualFree(trampolineMem, 0, kVirtualMemoryRelease);
            return false;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(targetAddr, stolenBytes, kPageExecuteReadWrite, &oldProtect)) {
            ROCK_LOG_ERROR(Init, "{} hook install failed at 0x{:X}: target protection failed", label, target.address());
            VirtualFree(trampolineMem, 0, kVirtualMemoryRelease);
            return false;
        }

        if (std::memcmp(targetAddr, expectedPrefix, stolenBytes) != 0) {
            ROCK_LOG_ERROR(Init,
                "{} hook install failed at 0x{:X}: entry changed after exact validation; no ROCK bytes were written",
                label,
                target.address());
            VirtualProtect(targetAddr, stolenBytes, oldProtect, &oldProtect);
            VirtualFree(trampolineMem, 0, kVirtualMemoryRelease);
            return false;
        }

        writeAbsoluteJump(targetAddr, reinterpret_cast<std::uintptr_t>(hook));
        for (std::size_t i = kJumpBytes; i < stolenBytes; ++i) {
            targetAddr[i] = 0x90;
        }

        FlushInstructionCache(GetCurrentProcess(), targetAddr, stolenBytes);
        VirtualProtect(targetAddr, stolenBytes, oldProtect, &oldProtect);

        original = trampolineMem;
        ROCK_LOG_INFO(Init, "Installed {} hook at 0x{:X}, original trampoline=0x{:X}", label, target.address(), reinterpret_cast<std::uintptr_t>(trampolineMem));
        return true;
    }

    static bool restoreEntryTrampolineHook(const char* label,
        std::uintptr_t targetOffset,
        const std::uint8_t* originalPrefix,
        std::size_t stolenBytes,
        void* hook,
        void*& original,
        bool& installed)
    {
        if (!installed) {
            return true;
        }

        REL::Relocation<std::uintptr_t> target{ REL::Offset(targetOffset) };
        auto* targetAddr = reinterpret_cast<std::uint8_t*>(target.address());
        if (!targetAddr || !originalPrefix || !original) {
            ROCK_LOG_ERROR(Init, "{} rollback failed: target, original bytes, or trampoline is null", label);
            return false;
        }

        const auto hookAddress = reinterpret_cast<std::uintptr_t>(hook);
        const bool hasRockJump = targetAddr[0] == 0xFF && targetAddr[1] == 0x25 && targetAddr[2] == 0x00 && targetAddr[3] == 0x00 && targetAddr[4] == 0x00 &&
                                 targetAddr[5] == 0x00 && *reinterpret_cast<std::uintptr_t*>(targetAddr + 6) == hookAddress;
        if (!hasRockJump) {
            ROCK_LOG_WARN(Init, "Skipped {} rollback: target 0x{:X} no longer points to ROCK hook", label, target.address());
            installed = false;
            original = nullptr;
            return true;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(targetAddr, stolenBytes, kPageExecuteReadWrite, &oldProtect)) {
            ROCK_LOG_ERROR(Init, "{} rollback failed at 0x{:X}: target protection failed", label, target.address());
            return false;
        }

        std::memcpy(targetAddr, originalPrefix, stolenBytes);
        FlushInstructionCache(GetCurrentProcess(), targetAddr, stolenBytes);
        VirtualProtect(targetAddr, stolenBytes, oldProtect, &oldProtect);

        VirtualFree(original, 0, kVirtualMemoryRelease);
        original = nullptr;
        installed = false;
        ROCK_LOG_WARN(Init, "Rolled back {} entry hook at 0x{:X}", label, target.address());
        return true;
    }

    void* resolvePlayerCharacterController()
    {
        return static_cast<void*>(character_controller_runtime::tryGetPlayerCharacterController());
    }

    bool isPlayerCharacterController(void* controller)
    {
        void* playerController = resolvePlayerCharacterController();
        return controller && playerController && controller == playerController;
    }

    RE::bhkWorld* resolvePlayerBhkWorld()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        return cell ? cell->GetbhkWorld() : nullptr;
    }

    collision_layer_policy::PlayerCharacterControllerContactPolicyDecision evaluatePlayerControllerTargetBody(
        RE::hknpWorld* world,
        std::uint32_t rawBodyId)
    {
        // CRASH FIX (Jul 6; restored after the attempt-6 port reverted it): an UNREADABLE/transient
        // contact target on the player char-proxy is the two-handed CTD source. During two-handing the
        // weapon-collision hull + hand colliders are created/destroyed each frame; a manifold that
        // references a body mid-rebuild (or already freed) fails tryReadFilterInfo. If we PRESERVE it
        // (suppress=false) it is passed to the native bhkCharProxyController::processConstraints solve,
        // which then dereferences the dying body -> AV; the caught SEH can't undo the half-corrupted
        // solver state -> hard crash a frame later. Readable generated-collider layers
        // (43/44/47/48) are suppressed explicitly. The only remaining unsafe path to the native solve is the unreadable
        // case -> SUPPRESS it. Safe: the player's real support (static/ground) is readable and preserved,
        // so this only drops contacts with transient/freed bodies for a frame - no fall-through-floor risk.
        if (!world || rawBodyId == body_frame::kInvalidBodyId) {
            return collision_layer_policy::PlayerCharacterControllerContactPolicyDecision{ .suppress = true, .reason = "unreadableTargetSuppressed" };
        }

        const RE::hknpBodyId bodyId{ rawBodyId };
        std::uint32_t filterInfo = 0;
        if (!body_collision::tryReadFilterInfo(world, bodyId, filterInfo)) {
            return collision_layer_policy::PlayerCharacterControllerContactPolicyDecision{ .suppress = true, .reason = "unreadableTargetSuppressed" };
        }

        const std::uint32_t layer = filterInfo & collision_layer_policy::FO4_LAYER_FILTER_MASK;
        return collision_layer_policy::evaluatePlayerCharacterControllerContact(
            collision_layer_policy::PlayerCharacterControllerContactPolicyInput{
                .filterEnabled = true,
                .playerController = true,
                .targetLayerKnown = true,
                .targetLayer = layer,
            });
    }

    void hookedHandleBumpedCharacterWithConfigRead(void* controller, void* bumpedCC, void* contactInfo)
    {
        bool originalAttempted = false;
        if (!PhysicsInteraction::s_hooksEnabled.load(std::memory_order_acquire)) {
            if (g_originalHandleBumped) {
                originalAttempted = true;
                g_originalHandleBumped(controller, bumpedCC, contactInfo);
            }
            return;
        }

        __try {
            const auto decision = collision_layer_policy::evaluatePlayerCharacterControllerContact(
                collision_layer_policy::PlayerCharacterControllerContactPolicyInput{
                    .filterEnabled = g_rockConfig.rockNativeCharacterControllerObjectContactFilterEnabled,
                    .playerController = isPlayerCharacterController(controller),
                    .targetLayerKnown = bumpedCC != nullptr,
                    .targetLayer = collision_layer_policy::FO4_LAYER_CHARCONTROLLER,
                });

            if (decision.suppress) {
                ROCK_LOG_SAMPLE_DEBUG(Bump,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "Suppressed player HandleBumpedCharacter target={:p} reason={}",
                    bumpedCC,
                    decision.reason);
                return;
            }

            if (g_originalHandleBumped) {
                originalAttempted = true;
                g_originalHandleBumped(controller, bumpedCC, contactInfo);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int sehLogCounter = 0;
            if (sehLogCounter++ % 100 == 0) {
                logger::error(
                    "[ROCK::Bump] SEH exception caught on physics thread (count={}) — "
                    "trampoline or stale pointer issue",
                    sehLogCounter);
            }
            if (!originalAttempted && g_originalHandleBumped) {
                __try {
                    originalAttempted = true;
                    g_originalHandleBumped(controller, bumpedCC, contactInfo);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }

    void hookedHandleBumpedCharacter(void* controller, void* bumpedCC, void* contactInfo)
    {
        if (!g_rockConfig.tryEnterNativeRead()) {
            if (g_originalHandleBumped) {
                __try {
                    g_originalHandleBumped(controller, bumpedCC, contactInfo);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            return;
        }

        hookedHandleBumpedCharacterWithConfigRead(
            controller,
            bumpedCC,
            contactInfo);
        g_rockConfig.leaveNativeRead();
    }

    bool installHavokTimingFixHook()
    {
        static bool installed = false;
        static bool installAttempted = false;
        if (installed) {
            return true;
        }
        if (installAttempted) {
            return false;
        }
        installAttempted = true;

        const auto ownership = validateBhkWorldSetDeltaTimeMainCallSite();

        if (ownership == HookSiteOwnership::HostPluginOwns) {
            /*
             * BENIGN DUPLICATE OWNERSHIP — NOT A FAILURE, so this is INFO.
             *
             * The Havok timing fix exists twice in this binary: ROCK's copy here and the
             * host plugin's heisenberg::HavokTimingFix (src/HavokTimingFix.cpp), which
             * patches the SAME call site and wins the race at plugin init. Only one detour
             * can live at one call site, so ROCK stands down — the FEATURE IS ACTIVE, just
             * owned by the host.
             *
             * This used to log at ERROR as "validation FAILED", which read as a broken
             * install of a working feature and was one of the five "failing validators"
             * that turned out not to be address bugs at all.
             */
            g_rockConfig.rockHavokTimingFixEnabled = false;
            g_originalBhkWorldSetDeltaTime = nullptr;
            const auto ownedCallSite = REL::Offset(kHookSite_BhkWorldSetDeltaTimeMainCall).address();
            // Same two-level decode the validator uses: the E8 points at CommonLibF4's
            // 14-byte VirtualAlloc'd trampoline stub, and only the second hop lands on the
            // host's hook function. Report both so the line states what was measured
            // rather than asserting a module the first-level address does not belong to.
            const auto ownedStub = decodeBranchDestination(ownedCallSite);
            const auto ownedViaStub = decodeBranchDestination(ownedStub);
            const auto ownedTarget = ownedViaStub != 0 ? ownedViaStub : ownedStub;
            ROCK_LOG_INFO(Init,
                "HAVOK_TIMING_FIX is owned by the HOST PLUGIN and is ACTIVE - the call site at module+0x{:X} (0x{:X}) already calls 0x{:X} ({}), "
                "which resolves to 0x{:X} ({}), i.e. heisenberg::HavokTimingFix installed its detour first. Only one detour can live at one call "
                "site, so ROCK does not double-patch it; ROCK's own bHavokTimingFixEnabled is cleared so it stops claiming a detour it does not "
                "own. Havok substep timing IS being corrected - by the host, under the host's own settings.",
                kHookSite_BhkWorldSetDeltaTimeMainCall,
                ownedCallSite,
                ownedStub,
                rock::hook_diagnostics::describeAddress(ownedStub),
                ownedTarget,
                rock::hook_diagnostics::describeAddress(ownedTarget));
            return false;
        }

        if (ownership != HookSiteOwnership::VerifiedNative) {
            /*
             * FAIL-SAFE. The call-site detour IS the timing fix: hookedBhkWorldSetDeltaTime
             * is the only place that reads rockHavokTimingFixEnabled and the only place that
             * rewrites the substep globals. Leaving the flag set after a failed install left
             * ROCK's config echo, MCM readback and every "is the timing fix on?" reader
             * claiming a feature that had nothing behind it.
             *
             * Clearing it here is safe and narrow: grep confirms hookedBhkWorldSetDeltaTime
             * (this file) is the flag's ONLY functional reader; it does not gate the host
             * plugin's separate heisenberg::HavokTimingFix, which owns its own settings.
             */
            const bool wasEnabled = g_rockConfig.rockHavokTimingFixEnabled;
            g_rockConfig.rockHavokTimingFixEnabled = false;
            g_originalBhkWorldSetDeltaTime = nullptr;
            ROCK_LOG_ERROR(Init,
                "FEATURE DISABLED: ROCK's Havok timing fix (bHavokTimingFixEnabled, was {}) is OFF for this session - its call-site "
                "detour at module+0x{:X} was not installed because the site did not hold the verified CALL (see the preceding "
                "found-vs-expected report). Havok substep timing is left exactly as the game/other owners set it.",
                wasEnabled ? "on" : "off",
                kHookSite_BhkWorldSetDeltaTimeMainCall);
            return false;
        }

        REL::Relocation<std::uintptr_t> hookCallSite{ REL::Offset(kHookSite_BhkWorldSetDeltaTimeMainCall) };
        auto& trampoline = F4SE::GetTrampoline();
        const auto original = trampoline.write_call<5>(hookCallSite.address(), &hookedBhkWorldSetDeltaTime);
        g_originalBhkWorldSetDeltaTime = reinterpret_cast<BhkWorldSetDeltaTime_t>(original);
        installed = g_originalBhkWorldSetDeltaTime != nullptr;

        if (!installed) {
            // FAIL-SAFE, same reasoning as the validation branch above: no detour means
            // no timing fix, so the flag must not keep claiming otherwise.
            g_rockConfig.rockHavokTimingFixEnabled = false;
            ROCK_LOG_CRITICAL(Init,
                "FEATURE DISABLED: ROCK's Havok timing fix is OFF - write_call<5> at 0x{:X} ({}) returned a null original, so there is "
                "no callable native bhkWorld::SetDeltaTime to chain to.",
                hookCallSite.address(),
                rock::hook_diagnostics::describeAddress(hookCallSite.address()));
            return false;
        }

        ROCK_LOG_INFO(Init,
            "HAVOK_TIMING_FIX hook installed at 0x{:X}; original=0x{:X} enabled={} minHz={:.2f} maxSubsteps={}",
            hookCallSite.address(),
            original,
            g_rockConfig.rockHavokTimingFixEnabled ? "yes" : "no",
            g_rockConfig.rockHavokTimingFixMinPhysicsFrameRate,
            g_rockConfig.rockHavokTimingFixMaxSubsteps);
        return true;
    }

    void installBumpHook()
    {
        static bool installed = false;
        static bool installAttempted = false;
        if (installed)
            return;
        if (installAttempted)
            return;
        installAttempted = true;

        /*
         * CONFIRMED against Fallout4VR.exe 1.2.72 (Ghidra):
         *   0x141E24980 is the exact entry of bhkCharacterController::HandleBumpedCharacter
         *   and its first 15 bytes read
         *     48 89 5C 24 08   MOV [RSP+08],RBX
         *     48 89 74 24 18   MOV [RSP+18],RSI
         *     57               PUSH RDI
         *     48 83 EC 70      SUB RSP,0x70
         *   - byte-for-byte the array below, ending on an instruction boundary.
         * Ordinary prologue instructions, not an existing branch/call: CommonLib
         * write_branch cannot derive a callable original from those bytes, so this
         * hook uses an explicit relocated-entry trampoline and validates the exact
         * whole instructions before patching. Constant and prefix are both correct;
         * a mismatch means the entry was already patched.
         */
        constexpr std::array<std::uint8_t, 15> expectedPrefix{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x74, 0x24, 0x18,
            0x57,
            0x48, 0x83, 0xEC, 0x70
        };

        /*
         * BENIGN DUPLICATE OWNERSHIP CHECK — run BEFORE attempting the patch.
         *
         * The host plugin's heisenberg::HandBumpHook (src/HandBumpHook.cpp) hooks this exact
         * entry, writing an FF 25 absolute jump to its own handler, and it installs first.
         * ROCK then memcmp'd the prologue the host had already overwritten and reported
         * "native bytes changed" at ERROR — a working feature that logged like a broken one.
         *
         * Ask who owns the branch instead of assuming. A destination inside THIS module can
         * only be the host plugin; anything else still falls through to the real validator
         * below and is reported as the failure it is.
         */
        const auto bumpEntryAddress = REL::Offset(offsets::kFunc_HandleBumpedCharacter).address();
        if (const auto existingBranch = decodeBranchDestination(bumpEntryAddress); isOwnedByHostPlugin(existingBranch)) {
            installed = false;
            g_originalHandleBumped = nullptr;
            g_nativeCharacterBumpFilterInstalled.store(false, std::memory_order_release);
            ROCK_LOG_INFO(Init,
                "HandleBumpedCharacter is hooked by the HOST PLUGIN and the bump guard is ACTIVE - the entry at module+0x{:X} already branches "
                "to 0x{:X} ({}), which is this same module, i.e. heisenberg::HandBumpHook installed first. Only one entry detour can exist at "
                "one address, so ROCK stands down rather than double-patching. ROCK's own character-bump suppression is therefore not installed; "
                "the host's equivalent guard covers it, and the CC-vs-object contact filter, the CHARCONTROLLER layer matrix and the large-object "
                "player block are separate mechanisms that remain active.",
                offsets::kFunc_HandleBumpedCharacter,
                existingBranch,
                rock::hook_diagnostics::describeAddress(existingBranch));
            return;
        }

        void* original = reinterpret_cast<void*>(g_originalHandleBumped);
        installed = installEntryTrampolineHook(
            "HandleBumpedCharacter", offsets::kFunc_HandleBumpedCharacter, expectedPrefix.data(), expectedPrefix.size(), &hookedHandleBumpedCharacter, original);
        g_originalHandleBumped = reinterpret_cast<HandleBumpedCharacter_t>(original);

        if (!installed || !g_originalHandleBumped) {
            /*
             * FAIL-SAFE: unwind the partial install and latch the feature OFF.
             *
             * Dropping the trampoline pointer matters even though the detour never
             * landed: a non-null g_originalHandleBumped with no detour in front of it
             * is a callable pointer into a code copy nothing will ever reach through
             * the intended path, and any future caller of hookedHandleBumpedCharacter
             * would silently double-run the native bump.
             *
             * Deliberately NOT touched: rockNativeCharacterControllerObjectContactFilterEnabled.
             * That flag is shared with the CC-vs-object filter, the layer-matrix
             * rewrite and the car-blocking fix; clearing it here would disable three
             * working features to report one missing hook.
             */
            installed = false;
            g_originalHandleBumped = nullptr;
            g_nativeCharacterBumpFilterInstalled.store(false, std::memory_order_release);
            ROCK_LOG_ERROR(Init,
                "FEATURE DISABLED: ROCK's player character-controller BUMP suppression is OFF for this session - its "
                "HandleBumpedCharacter entry hook (module+0x{:X}) was not installed (see the preceding found-vs-expected report). "
                "Character-vs-character bumps keep native behaviour. The CC-vs-object contact filter, the CHARCONTROLLER layer "
                "matrix and the large-object player block are separate mechanisms and remain active.",
                offsets::kFunc_HandleBumpedCharacter);
            return;
        }

        g_nativeCharacterBumpFilterInstalled.store(true, std::memory_order_release);
    }

    bool isNativeCharacterBumpFilterInstalled()
    {
        return g_nativeCharacterBumpFilterInstalled.load(std::memory_order_acquire);
    }

    bool installNativeGrabHook()
    {
        static REL::Relocation<std::uintptr_t> target{ REL::Offset(offsets::kFunc_VRGrabInitiate) };
        auto* addr = reinterpret_cast<std::uint8_t*>(target.address());

        /*
         * VALIDATE BEFORE PATCHING. This was the only hook in this file that blind-wrote
         * over bytes it never checked, and in 0.8.4 it never executed at all
         * (kFunc_VRGrabInitiate was TODO_RE == 0 behind an explicit skip). The constant is
         * now filled in, so the write is live for the first time - it must fail closed like
         * every sibling hook rather than silently corrupt whatever is at the address.
         *
         * Expected prologue verified against the decrypted Fallout4VR.exe 1.2.72
         * analysis image (SHA-256
         * 875A9A3FB50A0C41D8BB20848977106498DF1FFA8B4F7D5FEDA01F6F346C307A):
         * RVA 0xF19250 maps to .text file offset 0xF18650 and starts with the
         * exact 16 bytes below.
         * NOTE ON SCOPE: nulling this function suppresses the native VR hand grab (the
         * intended effect while ROCK owns grab), but it ALSO nulls
         * TelekinesisEffect::CreateSpring, which reaches the same MouseSpring helper with
         * GrabbingType=2. Telekinesis-style spring grabs stop working as a side effect.
         */
        static constexpr std::array<std::uint8_t, 16> kVrGrabInitiateExpectedPrefix{
            0x44, 0x89, 0x4C, 0x24, 0x20,
            0x89, 0x54, 0x24, 0x10,
            0x55,
            0x53,
            0x56,
            0x57,
            0x41, 0x54, 0x41
        };
        static constexpr std::array<std::uint8_t, 3> kVrGrabSuppressedPrefix{
            0x31, 0xC0, 0xC3
        };
        // Include the untouched fourth native byte in the atomic exchange.
        // 0xF19250 is 16-byte aligned, so an interlocked LONG exchange cannot
        // expose 31 / C0 / C3 as three separate writes to another thread.
        static_assert((offsets::kFunc_VRGrabInitiate % alignof(LONG)) == 0);
        constexpr LONG kExpectedEntryWord = static_cast<LONG>(0x244C8944u);
        constexpr LONG kSuppressedEntryWord = static_cast<LONG>(0x24C3C031u);

        if (!rock::hook_diagnostics::isReadable(
                target.address(),
                kVrGrabInitiateExpectedPrefix.size())) {
            ROCK_LOG_ERROR(Init,
                "Native VR-grab suppression unresolved: module+0x{:X} (0x{:X}) is not readable for the required "
                "{}-byte validation. No bytes were written; ROCK cannot claim that native grab is disabled.",
                offsets::kFunc_VRGrabInitiate,
                target.address(),
                kVrGrabInitiateExpectedPrefix.size());
            return false;
        }

        if (std::memcmp(
                addr,
                kVrGrabSuppressedPrefix.data(),
                kVrGrabSuppressedPrefix.size()) == 0) {
            ROCK_LOG_INFO(Init,
                "Native VR-grab suppression already ACTIVE at module+0x{:X} (0x{:X}): compatible "
                "[31 C0 C3] xor-eax/ret prefix is present. No bytes were written.",
                offsets::kFunc_VRGrabInitiate,
                target.address());
            return true;
        }

        if (std::memcmp(
                addr,
                kVrGrabInitiateExpectedPrefix.data(),
                kVrGrabInitiateExpectedPrefix.size()) != 0) {
            ROCK_LOG_ERROR(Init,
                "Native VR-grab suppression unresolved: module+0x{:X} (0x{:X}) matches neither the verified native "
                "prologue nor the compatible [31 C0 C3] suppressed state. Expected native [{}], found [{}]. No bytes "
                "were written; the final behavior of the existing code is UNKNOWN (it must not be reported as active "
                "or disabled without evidence).",
                offsets::kFunc_VRGrabInitiate,
                target.address(),
                rock::hook_diagnostics::formatBytes(
                    kVrGrabInitiateExpectedPrefix.data(),
                    kVrGrabInitiateExpectedPrefix.size()),
                rock::hook_diagnostics::formatBytes(
                    addr,
                    kVrGrabInitiateExpectedPrefix.size()));
            return false;
        }

        DWORD oldProtect = 0;
        constexpr std::size_t kAtomicPatchBytes = sizeof(LONG);
        if (!VirtualProtect(
                addr,
                kAtomicPatchBytes,
                kPageExecuteReadWrite,
                &oldProtect)) {
            ROCK_LOG_ERROR(Init,
                "Native VR-grab suppression FAILED at module+0x{:X} (0x{:X}): VirtualProtect(RWX) failed "
                "(GetLastError={}). Verified native bytes remain unchanged.",
                offsets::kFunc_VRGrabInitiate,
                target.address(),
                GetLastError());
            return false;
        }

        /*
         * Revalidate after obtaining write access, then commit the first four
         * bytes with one compare/exchange. The fourth byte is identical in both
         * words. A racing patcher therefore yields either the complete native
         * word or the complete suppressed word, never a half-written function.
         */
        LONG observedWord = 0;
        bool wroteSuppression = false;
        if (std::memcmp(
                addr,
                kVrGrabInitiateExpectedPrefix.data(),
                kVrGrabInitiateExpectedPrefix.size()) == 0) {
            observedWord = InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(addr),
                kSuppressedEntryWord,
                kExpectedEntryWord);
            wroteSuppression = observedWord == kExpectedEntryWord;
        }

        bool instructionCacheFlushed = true;
        DWORD instructionCacheError = ERROR_SUCCESS;
        if (wroteSuppression) {
            instructionCacheFlushed =
                FlushInstructionCache(
                    GetCurrentProcess(),
                    addr,
                    kAtomicPatchBytes) != FALSE;
            if (!instructionCacheFlushed) {
                instructionCacheError = GetLastError();
            }
        }

        DWORD ignoredProtect = 0;
        const bool protectionRestored =
            VirtualProtect(
                addr,
                kAtomicPatchBytes,
                oldProtect,
                &ignoredProtect) != FALSE;
        const DWORD protectionRestoreError =
            protectionRestored ? ERROR_SUCCESS : GetLastError();
        if (!protectionRestored) {
            ROCK_LOG_CRITICAL(Init,
                "Native VR-grab patch at module+0x{:X} could not restore the original page protection "
                "(GetLastError={}); final code bytes are verified below, but the page may remain writable.",
                offsets::kFunc_VRGrabInitiate,
                protectionRestoreError);
        }

        const bool finalStateReadable =
            rock::hook_diagnostics::isReadable(
                target.address(),
                kVrGrabSuppressedPrefix.size());
        const bool finalStateSuppressed =
            finalStateReadable &&
            std::memcmp(
                addr,
                kVrGrabSuppressedPrefix.data(),
                kVrGrabSuppressedPrefix.size()) == 0;

        if (!instructionCacheFlushed) {
            ROCK_LOG_CRITICAL(Init,
                "Native VR-grab suppression bytes were committed at module+0x{:X}, but FlushInstructionCache failed "
                "(GetLastError={}). Final byte state is {}; restart is recommended.",
                offsets::kFunc_VRGrabInitiate,
                instructionCacheError,
                finalStateSuppressed ? "the complete [31 C0 C3] patch" : "not the complete patch");
        }

        if (finalStateSuppressed) {
            if (wroteSuppression) {
                ROCK_LOG_INFO(Init,
                    "Native VR-grab suppression ACTIVE at module+0x{:X} (0x{:X}): atomically installed "
                    "[31 C0 C3] (xor eax,eax; ret); instruction-cache flush={}, page-protection restore={}.",
                    offsets::kFunc_VRGrabInitiate,
                    target.address(),
                    instructionCacheFlushed ? "ok" : "FAILED",
                    protectionRestored ? "ok" : "FAILED");
            } else {
                ROCK_LOG_INFO(Init,
                    "Native VR-grab suppression already ACTIVE at module+0x{:X} (0x{:X}): a compatible "
                    "[31 C0 C3] patch appeared during validation. ROCK wrote no bytes; page-protection restore={}.",
                    offsets::kFunc_VRGrabInitiate,
                    target.address(),
                    protectionRestored ? "ok" : "FAILED");
            }
            return true;
        }

        ROCK_LOG_ERROR(Init,
            "Native VR-grab suppression FAILED at module+0x{:X} (0x{:X}): atomic compare/exchange observed "
            "0x{:08X}, and the final entry is not [31 C0 C3]. No partial ROCK patch was written.",
            offsets::kFunc_VRGrabInitiate,
            target.address(),
            static_cast<std::uint32_t>(observedWord));
        return false;
    }

    bool installNativeMeleeSuppressionHooks()
    {
        static bool weaponSwingInstalled = false;
        static bool hitFrameInstalled = false;
        static bool attackBlockInstalled = false;
        static bool playerWeaponSwingCallbackInstalled = false;
        static bool vrMeleeImpactInstalled = false;

        auto rollbackNativeMeleeSuppressionHooks = [&]() {
            bool rollbackOk = true;

            rollbackOk = restoreNativeMeleeVtableHook(offsets::kVtableEntry_PlayerCharacter_WeaponSwingCallBack,
                             &hookedPlayerWeaponSwingCallback,
                             g_originalPlayerWeaponSwingCallback,
                             playerWeaponSwingCallbackInstalled,
                             "PlayerCharacter::WeaponSwingCallBack") &&
                         rollbackOk;
            rollbackOk = restoreNativeMeleeVtableHook(offsets::kVtableEntry_AttackBlockHandler_ShouldHandleEvent,
                             &hookedAttackBlockShouldHandleEvent,
                             g_originalAttackBlockShouldHandleEvent,
                             attackBlockInstalled,
                             "AttackBlockHandler::ShouldHandleEvent") &&
                         rollbackOk;
            rollbackOk = restoreNativeMeleeVtableHook(
                             offsets::kVtableEntry_HitFrameHandler_Handle, &hookedHitFrameHandler, g_originalHitFrameHandler, hitFrameInstalled, "HitFrameHandler::Handle") &&
                         rollbackOk;
            rollbackOk = restoreNativeMeleeVtableHook(offsets::kVtableEntry_WeaponSwingHandler_Handle,
                             &hookedWeaponSwingHandler,
                             g_originalWeaponSwingHandler,
                             weaponSwingInstalled,
                             "WeaponSwingHandler::Handle") &&
                         rollbackOk;

            void* impactOriginal = reinterpret_cast<void*>(g_originalVrMeleeImpactCallback);
            rollbackOk = restoreEntryTrampolineHook("VRMeleeImpact",
                             offsets::kFunc_VRMeleeImpactCallback,
                             kVrMeleeImpactExpectedPrefix.data(),
                             kVrMeleeImpactExpectedPrefix.size(),
                             &hookedVrMeleeImpactCallback,
                             impactOriginal,
                             vrMeleeImpactInstalled) &&
                         rollbackOk;
            g_originalVrMeleeImpactCallback = reinterpret_cast<NativeVrMeleeImpactCallback_t>(impactOriginal);

            if (!rollbackOk) {
                ROCK_LOG_CRITICAL(Init, "Native melee suppression rollback was incomplete; one or more partial hooks may remain installed");
            }

            g_nativeMeleeSuppressionHooksInstalled.store(false, std::memory_order_release);
            return rollbackOk;
        };

        const bool allInstalled = weaponSwingInstalled && hitFrameInstalled && attackBlockInstalled && playerWeaponSwingCallbackInstalled && vrMeleeImpactInstalled;
        if (allInstalled) {
            g_nativeMeleeSuppressionHooksInstalled.store(true, std::memory_order_release);
            return true;
        }

        // Observe-only installs (below) legitimately leave exactly the
        // {vrMeleeImpact, playerWeaponSwingCallback} pair installed. The
        // partial-set fail-safe must not misread that pair as a stranded
        // suppression rollback on any future re-entry.
        static bool observationOnlyInstall = false;
        if (observationOnlyInstall) {
            return false;
        }

        const bool anyInstalled = weaponSwingInstalled || hitFrameInstalled || attackBlockInstalled || playerWeaponSwingCallbackInstalled || vrMeleeImpactInstalled;
        if (anyInstalled) {
            /*
             * Some but not all targets are already hooked on ENTRY to this function. The
             * only way to get here is a previous rollback that could not un-patch
             * everything, so treat it as the fail-safe case: unwind again and clear the
             * flags, exactly as the two failure paths below do. A partial hook set must
             * never be left live with suppression enabled.
             */
            // Snapshot BEFORE rolling back: the rollback helpers clear these by reference,
            // so reading them afterwards would report "no" for every target and hide which
            // ones had actually been patched.
            const char* const swingWas = weaponSwingInstalled ? "yes" : "no";
            const char* const hitFrameWas = hitFrameInstalled ? "yes" : "no";
            const char* const attackBlockWas = attackBlockInstalled ? "yes" : "no";
            const char* const playerSwingWas = playerWeaponSwingCallbackInstalled ? "yes" : "no";
            const char* const vrImpactWas = vrMeleeImpactInstalled ? "yes" : "no";

            const bool rollbackOk = rollbackNativeMeleeSuppressionHooks();
            g_rockConfig.rockNativeMeleeSuppressionEnabled = false;
            g_rockConfig.rockNativeMeleeSuppressWeaponSwing = false;
            g_rockConfig.rockNativeMeleeSuppressHitFrame = false;
            g_rockConfig.rockNativeMeleeFullSuppression = false;
            ROCK_LOG_ERROR(Init,
                "FEATURE DISABLED: native melee suppression is OFF for this session - the hook set was already partial on entry "
                "(weaponSwing={} hitFrame={} attackBlock={} playerSwingCallback={} vrMeleeImpact={}) and has been unwound (rollback {}). "
                "All four suppression flags are cleared, so any hook still in place passes straight through to the native implementation.",
                swingWas,
                hitFrameWas,
                attackBlockWas,
                playerSwingWas,
                vrImpactWas,
                rollbackOk ? "succeeded" : "was INCOMPLETE - see the CRITICAL line above");
            return false;
        }

        /*
         * ── OPT-IN GATE ───────────────────────────────────────────────────────────────
         *
         * Checked BEFORE any patching, and deliberately AFTER the partial-rollback block
         * above so a partially-installed set still gets cleaned up rather than stranded.
         *
         * Until now this feature could not install at all: two of its five hook offsets
         * were TODO_RE == 0, so validation failed on every machine and nothing was ever
         * patched. Both are now Ghidra-confirmed, which means this code path is about to
         * execute for the first time in the mod's life. Splicing a 14-byte entry detour
         * into VRMeleeImpact plus four vtable swaps is not something to switch on silently
         * in the very change that makes it possible — especially with an open report of
         * melee not working. So: available, documented, and OFF until asked for.
         *
         * Not installing is strictly safer than installing-and-passing-through: the hook
         * bodies do gate on these same flags at runtime, but declining to patch means the
         * native code is left completely untouched.
         */
        if (!g_rockConfig.rockNativeMeleeSuppressionEnabled) {
            /*
             * OBSERVE-ONLY MODE (Jul 31): install just the VRMeleeImpact entry
             * trampoline and the PlayerCharacter::WeaponSwingCallBack vtable
             * swap so player swings and native hits are logged with paired
             * counters. With suppression off, every policy in the hook bodies
             * returns CallNative ("suppression-disabled"), so both hooks are
             * pure pass-throughs. g_nativeMeleeSuppressionHooksInstalled stays
             * FALSE — it gates shouldSuppressNativeVrMeleeVelocity and must
             * only reflect the full suppression set. The other three hooks
             * (WeaponSwingHandler / HitFrameHandler animation handlers, the
             * AttackBlockHandler input gate) observe no hits and stay
             * uninstalled.
             */
            if (g_rockConfig.rockNativeMeleeObservationEnabled) {
                const bool impactValid = validateEntryTrampolineTarget(
                    "VRMeleeImpact",
                    offsets::kFunc_VRMeleeImpactCallback,
                    kVrMeleeImpactExpectedPrefix.data(),
                    kVrMeleeImpactExpectedPrefix.size());
                const bool swingValid = validateNativeMeleeVtableTarget(
                    offsets::kVtableEntry_PlayerCharacter_WeaponSwingCallBack,
                    offsets::kFunc_PlayerCharacter_WeaponSwingCallBack,
                    "PlayerCharacter::WeaponSwingCallBack");
                if (impactValid && swingValid) {
                    void* impactOriginal = reinterpret_cast<void*>(g_originalVrMeleeImpactCallback);
                    vrMeleeImpactInstalled = installEntryTrampolineHook("VRMeleeImpact",
                        offsets::kFunc_VRMeleeImpactCallback,
                        kVrMeleeImpactExpectedPrefix.data(),
                        kVrMeleeImpactExpectedPrefix.size(),
                        &hookedVrMeleeImpactCallback,
                        impactOriginal);
                    g_originalVrMeleeImpactCallback = reinterpret_cast<NativeVrMeleeImpactCallback_t>(impactOriginal);
                    if (vrMeleeImpactInstalled) {
                        playerWeaponSwingCallbackInstalled = installNativeMeleeVtableHook(
                            offsets::kVtableEntry_PlayerCharacter_WeaponSwingCallBack,
                            offsets::kFunc_PlayerCharacter_WeaponSwingCallBack,
                            &hookedPlayerWeaponSwingCallback,
                            g_originalPlayerWeaponSwingCallback,
                            "PlayerCharacter::WeaponSwingCallBack");
                    }
                    if (vrMeleeImpactInstalled && playerWeaponSwingCallbackInstalled) {
                        observationOnlyInstall = true;
                        g_nativeMeleeSuppressionHooksInstalled.store(false, std::memory_order_release);
                        ROCK_LOG_INFO(Init,
                            "Native melee OBSERVATION hooks installed (observe-only pass-through; suppression remains OFF). "
                            "Every player melee SWING and every native melee IMPACT (the game's own hit decision) is now logged "
                            "with paired counters at info level — a SWING with no following IMPACT is a native miss. "
                            "Disable with [PhysicsInteraction] bNativeMeleeObservationEnabled=0 and RESTART.");
                        return false;
                    }
                    // Partial observation install: unwind whichever half made it.
                    // The rollback helper restores only hooks whose installed
                    // flags are set, and the other three are untouched here.
                    const bool rollbackOk = rollbackNativeMeleeSuppressionHooks();
                    ROCK_LOG_ERROR(Init,
                        "Native melee OBSERVATION install was incomplete (vrMeleeImpact={} playerSwingCallback={}) and has been "
                        "rolled back ({}). Native melee is untouched; swing/hit logging is unavailable this session.",
                        vrMeleeImpactInstalled ? "yes" : "no",
                        playerWeaponSwingCallbackInstalled ? "yes" : "no",
                        rollbackOk ? "cleanly" : "INCOMPLETE - see the CRITICAL line above");
                } else {
                    ROCK_LOG_WARN(Init,
                        "Native melee OBSERVATION targets failed validation (vrMeleeImpact={} playerSwingCallback={}) — nothing was "
                        "patched; native melee is untouched and swing/hit logging is unavailable this session.",
                        impactValid ? "ok" : "FAILED",
                        swingValid ? "ok" : "FAILED");
                }
                g_nativeMeleeSuppressionHooksInstalled.store(false, std::memory_order_release);
                return false;
            }

            static std::atomic<bool> availabilityLogged{ false };
            if (!availabilityLogged.exchange(true, std::memory_order_acq_rel)) {
                ROCK_LOG_INFO(Init,
                    "Native melee suppression is AVAILABLE but DISABLED BY DEFAULT - no hooks were installed and native melee is fully intact. "
                    "Its five hook targets are now all Ghidra-confirmed for FO4VR 1.2.72 (the two that were unset are VRMeleeImpact "
                    "module+0x{:X} and the AttackBlockHandler::ShouldHandleEvent vtable slot module+0x{:X}), so the feature can install for the "
                    "first time - which is exactly why it is opt-in. To enable it set [PhysicsInteraction] bNativeMeleeSuppressionEnabled=1 in "
                    "Data\\F4SE\\Plugins\\Heisenberg_F4VR.ini (per-path: bNativeMeleeSuppressWeaponSwing, bNativeMeleeSuppressHitFrame) and "
                    "RESTART the game; hooks install once at "
                    "plugin init, so a save reload will not pick it up. Observe-only swing/hit logging "
                    "(bNativeMeleeObservationEnabled) is also disabled this session.",
                    offsets::kFunc_VRMeleeImpactCallback,
                    offsets::kVtableEntry_AttackBlockHandler_ShouldHandleEvent);
            }
            g_nativeMeleeSuppressionHooksInstalled.store(false, std::memory_order_release);
            return false;
        }

        if (!validateNativeMeleeSuppressionHookTargets()) {
            /*
             * FAIL-SAFE: validation is all-or-nothing and runs BEFORE the first byte is
             * written, so reaching here means NOTHING has been patched.
             *
             * Clear the feature flags here rather than relying on the caller. An equivalent
             * block exists in PhysicsInteraction::init(), but this safety property must not
             * depend on a different file continuing to do it: if native melee suppression
             * cannot install, every reader of these flags must see the feature as OFF or
             * the config echo will keep advertising suppression that has nothing behind it.
             */
            g_rockConfig.rockNativeMeleeSuppressionEnabled = false;
            g_rockConfig.rockNativeMeleeSuppressWeaponSwing = false;
            g_rockConfig.rockNativeMeleeSuppressHitFrame = false;
            g_rockConfig.rockNativeMeleeFullSuppression = false;
            g_nativeMeleeSuppressionHooksInstalled.store(false, std::memory_order_release);
            ROCK_LOG_ERROR(Init,
                "FEATURE DISABLED: native melee suppression is OFF for this session - target validation failed, so NOTHING was patched (the "
                "installer validates all five targets before writing any bytes; see the summary line above for which one and its found-vs-expected "
                "bytes). bNativeMeleeSuppressionEnabled, bNativeMeleeSuppressWeaponSwing, bNativeMeleeSuppressHitFrame and "
                "bNativeMeleeFullSuppression are all cleared. CONSEQUENCE: FO4VR's native melee - weapon swing, hit frame, the AttackBlock input "
                "gate and the VRInput velocity gate - is left completely intact and behaves exactly as vanilla.");
            return false;
        }

        if (!vrMeleeImpactInstalled) {
            void* impactOriginal = reinterpret_cast<void*>(g_originalVrMeleeImpactCallback);
            vrMeleeImpactInstalled = installEntryTrampolineHook("VRMeleeImpact",
                offsets::kFunc_VRMeleeImpactCallback,
                kVrMeleeImpactExpectedPrefix.data(),
                kVrMeleeImpactExpectedPrefix.size(),
                &hookedVrMeleeImpactCallback,
                impactOriginal);
            g_originalVrMeleeImpactCallback = reinterpret_cast<NativeVrMeleeImpactCallback_t>(impactOriginal);
        }
        if (!weaponSwingInstalled) {
            weaponSwingInstalled = installNativeMeleeVtableHook(
                offsets::kVtableEntry_WeaponSwingHandler_Handle,
                offsets::kFunc_WeaponSwingHandler_Handle,
                &hookedWeaponSwingHandler,
                g_originalWeaponSwingHandler,
                "WeaponSwingHandler::Handle");
        }
        if (!hitFrameInstalled) {
            hitFrameInstalled = installNativeMeleeVtableHook(
                offsets::kVtableEntry_HitFrameHandler_Handle,
                offsets::kFunc_HitFrameHandler_Handle,
                &hookedHitFrameHandler,
                g_originalHitFrameHandler,
                "HitFrameHandler::Handle");
        }
        if (!attackBlockInstalled) {
            attackBlockInstalled = installNativeMeleeVtableHook(
                offsets::kVtableEntry_AttackBlockHandler_ShouldHandleEvent,
                offsets::kFunc_AttackBlockHandler_ShouldHandleEvent,
                &hookedAttackBlockShouldHandleEvent,
                g_originalAttackBlockShouldHandleEvent,
                "AttackBlockHandler::ShouldHandleEvent");
        }
        if (!playerWeaponSwingCallbackInstalled) {
            playerWeaponSwingCallbackInstalled = installNativeMeleeVtableHook(
                offsets::kVtableEntry_PlayerCharacter_WeaponSwingCallBack,
                offsets::kFunc_PlayerCharacter_WeaponSwingCallBack,
                &hookedPlayerWeaponSwingCallback,
                g_originalPlayerWeaponSwingCallback,
                "PlayerCharacter::WeaponSwingCallBack");
        }

        if (!(weaponSwingInstalled && hitFrameInstalled && attackBlockInstalled && playerWeaponSwingCallbackInstalled && vrMeleeImpactInstalled)) {
            ROCK_LOG_ERROR(Init,
                "Native melee suppression hook installation was incomplete: weaponSwing={} hitFrame={} attackBlock={} playerSwingCallback={} vrMeleeImpact={}; rolling back",
                weaponSwingInstalled ? "yes" : "no",
                hitFrameInstalled ? "yes" : "no",
                attackBlockInstalled ? "yes" : "no",
                playerWeaponSwingCallbackInstalled ? "yes" : "no",
                vrMeleeImpactInstalled ? "yes" : "no");
            const bool rollbackOk = rollbackNativeMeleeSuppressionHooks();
            g_nativeMeleeSuppressionHooksInstalled.store(false, std::memory_order_release);

            /*
             * FAIL-SAFE, same contract as the validation branch: a partial hook set is
             * never left live. Every target that DID install has just been restored by
             * rollbackNativeMeleeSuppressionHooks(), so clear the feature flags too - the
             * hook bodies that survive a failed rollback must then fall through to the
             * native implementation instead of suppressing anything.
             */
            g_rockConfig.rockNativeMeleeSuppressionEnabled = false;
            g_rockConfig.rockNativeMeleeSuppressWeaponSwing = false;
            g_rockConfig.rockNativeMeleeSuppressHitFrame = false;
            g_rockConfig.rockNativeMeleeFullSuppression = false;
            ROCK_LOG_ERROR(Init,
                "FEATURE DISABLED: native melee suppression is OFF for this session - installation was incomplete and has been rolled back "
                "(rollback {}). All four suppression flags are cleared, so any hook that could not be un-patched now passes straight through to "
                "the native implementation. CONSEQUENCE: FO4VR's native melee behaves as vanilla.",
                rollbackOk ? "succeeded - no ROCK detours remain" : "was INCOMPLETE - see the CRITICAL line above; some detours may still be in place");
            return false;
        }

        g_nativeMeleeSuppressionHooksInstalled.store(true, std::memory_order_release);
        ROCK_LOG_INFO(Init, "Native melee suppression hooks installed: weaponSwing={} hitFrame={} attackBlock={} playerSwingCallback={} vrMeleeImpact={} enabled={} full={} suppressSwing={} suppressHitFrame={}",
            weaponSwingInstalled ? "yes" : "no", hitFrameInstalled ? "yes" : "no", attackBlockInstalled ? "yes" : "no",
            playerWeaponSwingCallbackInstalled ? "yes" : "no", vrMeleeImpactInstalled ? "yes" : "no", g_rockConfig.rockNativeMeleeSuppressionEnabled ? "yes" : "no",
            g_rockConfig.rockNativeMeleeFullSuppression ? "yes" : "no", g_rockConfig.rockNativeMeleeSuppressWeaponSwing ? "yes" : "no",
            g_rockConfig.rockNativeMeleeSuppressHitFrame ? "yes" : "no");
        return weaponSwingInstalled && hitFrameInstalled && attackBlockInstalled && playerWeaponSwingCallbackInstalled && vrMeleeImpactInstalled;
    }

    using ProcessConstraints_t = void (*)(void*, void*, void*, void*);
    static ProcessConstraints_t g_originalProcessConstraints = nullptr;

    // Host-held/player separation is an ownership invariant, not a tunable
    // contact policy. Run this exact world/body filter before taking ROCK's
    // configuration read lock so a live config reload, dormant ROCK runtime,
    // or disabled optional object filter cannot let a Heisenberg-held prop
    // feed displacement into the player's character controller.
    static void filterExternalHeldPlayerContacts(
        void* controller,
        void* manifold,
        void* simplexInput)
    {
        if (!HostHasExternalHeldBodies() ||
            !isPlayerCharacterController(controller)) {
            return;
        }

        performance_profiler::ScopedTimer profilerTimer(
            performance_profiler::Scope::CharacterControllerContactFilter);

        RE::bhkWorld* const playerBhkWorld =
            resolvePlayerBhkWorld();
        RE::hknpWorld* const playerHknpWorld =
            playerBhkWorld ?
                havok_runtime::getHknpWorldFromBhk(playerBhkWorld) :
                nullptr;
        if (!playerHknpWorld) {
            return;
        }

        const auto contactBuffers =
            held_grab_cc_policy::makeGeneratedContactBufferView(
                manifold,
                simplexInput);
        if (!contactBuffers.valid) {
            // Constraint-only rows do not expose a target body ID. Never drop
            // every player constraint as a fallback: that would also remove
            // floor/wall support. The normal collision-group path remains a
            // secondary safeguard for this uncommon native layout.
            return;
        }

        (void)held_grab_cc_policy::filterGeneratedContactBuffers(
            contactBuffers,
            [playerHknpWorld](const std::uint32_t bodyId) {
                return HostIsExternalHeldBody(
                    playerHknpWorld,
                    bodyId);
            });
    }

    /*
     * Keep the timer in this helper rather than the SEH-owning hook below.
     * MSVC forbids destructor-bearing RAII locals in a function containing
     * __try, and timing the outer hook would also charge Fallout's original
     * processConstraints solve to ROCK. This boundary contains only ROCK's
     * policy checks and manifold compaction.
     */
    static void filterRockCharacterControllerContacts(
        void* controller,
        void* manifold,
        void* simplexInput)
    {
        performance_profiler::ScopedTimer profilerTimer(
            performance_profiler::Scope::CharacterControllerContactFilter);

        const bool playerControllerFilterEnabled =
            g_rockConfig.
                rockNativeCharacterControllerObjectContactFilterEnabled;
        const bool playerController =
            isPlayerCharacterController(controller);
        RE::bhkWorld* const playerBhkWorld =
            playerController ? resolvePlayerBhkWorld() : nullptr;
        RE::hknpWorld* const playerHknpWorld =
            playerBhkWorld ?
                havok_runtime::getHknpWorldFromBhk(playerBhkWorld) :
                nullptr;
        const bool playerControllerFilterActive =
            playerControllerFilterEnabled &&
            playerController &&
            playerHknpWorld;

        auto* const pi =
            PhysicsInteraction::s_instance.load(
                std::memory_order_acquire);
        const bool piReady = pi && pi->isInitialized();
        const bool rightHolding =
            piReady &&
            pi->getRightHand().isHoldingAtomic();
        const bool leftHolding =
            piReady &&
            pi->getLeftHand().isHoldingAtomic();
        const bool diagnosticsEnabled =
            g_rockConfig.rockDebugGrabFrameLogging ||
            g_rockConfig.rockDebugVerboseLogging;
        const auto contactPolicy =
            held_grab_cc_policy::evaluateHeldGrabContactPolicy(
                held_grab_cc_policy::HeldGrabContactPolicyInput{
                    .hooksEnabled = piReady,
                    .holdingHeldObject =
                        rightHolding || leftHolding,
                    .diagnosticsEnabled = diagnosticsEnabled,
                });
        const bool heldFilterActive =
            piReady && contactPolicy.mayFilterBeforeOriginal;

        if (!heldFilterActive &&
            !playerControllerFilterActive) {
            return;
        }

        const auto contactBuffers =
            held_grab_cc_policy::makeGeneratedContactBufferView(
                manifold,
                simplexInput);
        if (!contactBuffers.valid) {
            /*
             * Fail open when Havok did not provide the manifold rows that
             * carry body IDs. Constraint-only rows cannot distinguish a
             * dying generated collider from a native car or small prop, so
             * clearing them would silently turn vanilla player collision
             * off whenever either hand happened to hold an object.
             */
            if (g_rockConfig.rockDebugVerboseLogging) {
                ROCK_LOG_SAMPLE_DEBUG(CC,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "Skipped character-controller pre-filter reason={} manifoldCount={} constraintCount={} heldFilter={} playerObjectFilter={}",
                    contactBuffers.reason,
                    contactBuffers.manifoldCount,
                    contactBuffers.constraintCount,
                    heldFilterActive ? "on" : "off",
                    playerControllerFilterActive ? "on" : "off");
            }
            return;
        }

        int removedHeldPairs = 0;
        int removedPlayerObjectPairs = 0;
        int removedPlayerGeneratedPairs = 0;
        int removedPlayerTransientPairs = 0;
        int preservedPlayerSupportPairs = 0;
        int preservedPlayerNativePairs = 0;
        const auto filterResult =
            held_grab_cc_policy::filterGeneratedContactBuffers(
                contactBuffers,
                [&](std::uint32_t bodyId) {
                    if (heldFilterActive) {
                        bool isHeld = false;
                        if (rightHolding) {
                            isHeld =
                                pi->getRightHand().
                                    isHeldBodyId(bodyId);
                        }
                        if (!isHeld && leftHolding) {
                            isHeld =
                                pi->getLeftHand().
                                    isHeldBodyId(bodyId);
                        }
                        if (isHeld) {
                            ++removedHeldPairs;
                            return true;
                        }
                    }

                    if (playerControllerFilterActive) {
                        const auto decision =
                            evaluatePlayerControllerTargetBody(
                                playerHknpWorld,
                                bodyId);
                        if (decision.suppress) {
                            ++removedPlayerObjectPairs;
                            if (std::string_view(
                                    decision.reason) ==
                                "rockGeneratedBody") {
                                ++removedPlayerGeneratedPairs;
                            } else {
                                ++removedPlayerTransientPairs;
                            }
                            return true;
                        }
                        if (std::string_view(decision.reason) ==
                            "supportLayer") {
                            ++preservedPlayerSupportPairs;
                        } else {
                            ++preservedPlayerNativePairs;
                        }
                    }
                    return false;
                });

        if (diagnosticsEnabled && filterResult.valid) {
            if (filterResult.removedPairCount > 0) {
                ROCK_LOG_SAMPLE_DEBUG(CC,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "Filtered {} character-controller contacts before original listener kept={} originalPairs={} rockHeldRemoved={} playerObjectRemoved={} playerGeneratedRemoved={} playerTransientRemoved={} playerSupportPreserved={} playerNativePreserved={}",
                    filterResult.removedPairCount,
                    filterResult.keptPairCount,
                    filterResult.originalPairCount,
                    removedHeldPairs,
                    removedPlayerObjectPairs,
                    removedPlayerGeneratedPairs,
                    removedPlayerTransientPairs,
                    preservedPlayerSupportPairs,
                    preservedPlayerNativePairs);
            } else if (
                g_rockConfig.rockDebugVerboseLogging) {
                ROCK_LOG_SAMPLE_DEBUG(CC,
                    g_rockConfig.rockLogSampleMilliseconds,
                    "Character-controller pre-filter kept native contacts originalPairs={} reason={} playerSupportPreserved={} playerNativePreserved={}",
                    filterResult.originalPairCount,
                    filterResult.reason,
                    preservedPlayerSupportPairs,
                    preservedPlayerNativePairs);
            }
        }
    }

    static void hookedProcessConstraintsCallbackImpl(void* controller, void* charProxy, void* manifold, void* simplexInput)
    {
        bool originalAttempted = false;
        if (!PhysicsInteraction::s_hooksEnabled.load(std::memory_order_acquire)) {
            if (g_originalProcessConstraints) {
                originalAttempted = true;
                g_originalProcessConstraints(controller, charProxy, manifold, simplexInput);
            }
            return;
        }

        __try {
            filterRockCharacterControllerContacts(
                controller,
                manifold,
                simplexInput);

            if (g_originalProcessConstraints) {
                originalAttempted = true;
                g_originalProcessConstraints(controller, charProxy, manifold, simplexInput);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int sehCount = 0;
            if (sehCount++ % 100 == 0) {
                logger::error("[ROCK::CC] SEH exception in hookedProcessConstraintsCallback (count={})", sehCount);
            }
            if (!originalAttempted && g_originalProcessConstraints) {
                __try {
                    originalAttempted = true;
                    g_originalProcessConstraints(controller, charProxy, manifold, simplexInput);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }

    // Counts the call to hookedProcessConstraintsCallbackImpl so destroyPhysicsInteraction
    // can drain in-flight physics-thread callbacks before freeing the PhysicsInteraction
    // instance this callback dereferences (pi->getRightHand()/getLeftHand() inside). The
    // counter deliberately lives OUTSIDE the Impl's __try: an RAII guard inside a __try whose
    // __except can fire would leak the count on unwind. Restored after the attempt-6 port
    // collapsed the wrapper/Impl split and left ROCKMain's drain loop a permanent no-op.
    void hookedProcessConstraintsCallback(void* controller, void* charProxy, void* manifold, void* simplexInput)
    {
        PhysicsInteraction::s_inFlightCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try {
            filterExternalHeldPlayerContacts(
                controller,
                manifold,
                simplexInput);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static std::atomic<std::uint32_t>
                externalHeldFilterFaultCount{ 0 };
            const auto faultCount =
                externalHeldFilterFaultCount.fetch_add(
                    1,
                    std::memory_order_relaxed) +
                1;
            if (faultCount == 1 ||
                faultCount % 100 == 0) {
                logger::error(
                    "[ROCK::CC] external held-body filter fault "
                    "(count={})",
                    faultCount);
            }
        }
        const bool configRead = g_rockConfig.tryEnterNativeRead();
        if (configRead) {
            hookedProcessConstraintsCallbackImpl(
                controller,
                charProxy,
                manifold,
                simplexInput);
            g_rockConfig.leaveNativeRead();
        } else if (g_originalProcessConstraints) {
            /*
             * A config mutation only pauses ROCK's filter. Preserve the
             * engine callback itself so hot reload cannot drop a native
             * character-controller solve.
             */
            __try {
                g_originalProcessConstraints(
                    controller,
                    charProxy,
                    manifold,
                    simplexInput);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (PhysicsInteraction::s_inFlightCallbacks.fetch_sub(
                1,
                std::memory_order_acq_rel) == 1) {
            PhysicsInteraction::s_inFlightCallbacks.notify_all();
        }
    }

    void installRefreshManifoldHook()
    {
        static bool installed = false;
        static bool installAttempted = false;
        if (installed)
            return;
        if (installAttempted)
            return;
        installAttempted = true;

        // Ghidra verified bhkCharProxyController::processConstraintsCallback at
        // 0x141E4B7E0 starts with whole prologue instructions through PUSH R12.
        // This callback owns the generated contact rows ROCK compacts, so it
        // uses the same fail-closed relocated-entry trampoline as
        // HandleBumpedCharacter instead of copying unvalidated bytes.
        constexpr std::array<std::uint8_t, 14> expectedPrefix{
            0x48, 0x8B, 0xC4,
            0x4C, 0x89, 0x48, 0x20,
            0x4C, 0x89, 0x40, 0x18,
            0x55,
            0x41, 0x54
        };

        void* original = reinterpret_cast<void*>(g_originalProcessConstraints);
        installed = installEntryTrampolineHook("ProcessConstraintsCallback",
            offsets::kFunc_ProcessConstraintsCallback,
            expectedPrefix.data(),
            expectedPrefix.size(),
            &hookedProcessConstraintsCallback,
            original);
        g_originalProcessConstraints = reinterpret_cast<ProcessConstraints_t>(original);
    }
}
