#include "HandBumpHook.h"

#include "Physics.h"
#include "rock_integration/CollisionLayerPolicy.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>

namespace heisenberg::HandBumpHook
{
    namespace
    {
        using HandleBumpedCharacter_t = void (*)(void*, void*, void*);

        HandleBumpedCharacter_t g_original = nullptr;
        std::atomic<bool>       g_enabled{ false };
        bool                    g_installed = false;

        // FO4VR 1.2.72: HandleBumpedCharacter @ 0x141E24980 -> RVA 0x1E24980.
        constexpr std::uintptr_t kFunc_HandleBumpedCharacter = 0x1E24980;

        // 14-byte absolute jump:  FF 25 00000000  ; <8-byte target>  (jmp [rip+0]; dq target)
        void writeAbsoluteJump(std::uint8_t* dest, std::uintptr_t target)
        {
            dest[0] = 0xFF;
            dest[1] = 0x25;
            dest[2] = 0x00;
            dest[3] = 0x00;
            dest[4] = 0x00;
            dest[5] = 0x00;
            std::memcpy(dest + 6, &target, sizeof(target));
        }

        bool isPlayerCharacterController(void* controller)
        {
            if (!controller) {
                return false;
            }
            auto* player = heisenberg::Physics::GetPlayerCharacterController();
            return player && controller == static_cast<void*>(player);
        }

        // Runs on the Havok/physics thread. Only PODs live in the __try scope (no C++
        // objects requiring unwinding), so SEH is legal here. The outer __try also
        // protects the player-controller pointer walk inside isPlayerCharacterController.
        void hookedHandleBumpedCharacter(void* controller, void* bumpedCC, void* contactInfo)
        {
            bool originalAttempted = false;

            if (!g_enabled.load(std::memory_order_acquire)) {
                if (g_original) {
                    g_original(controller, bumpedCC, contactInfo);
                }
                return;
            }

            __try {
                const auto decision = heisenberg::rock_core::collision_layer_policy::evaluatePlayerCharacterControllerContact(
                    heisenberg::rock_core::collision_layer_policy::PlayerCharacterControllerContactPolicyInput{
                        .filterEnabled = true,
                        .playerController = isPlayerCharacterController(controller),
                        .targetLayerKnown = bumpedCC != nullptr,
                        .targetLayer = heisenberg::rock_core::collision_layer_policy::FO4_LAYER_CHARCONTROLLER,
                    });

                if (decision.suppress) {
                    // CTD guard: drop the player-proxy bump our hand bodies generated.
                    return;
                }

                if (g_original) {
                    originalAttempted = true;
                    g_original(controller, bumpedCC, contactInfo);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (!originalAttempted && g_original) {
                    __try {
                        g_original(controller, bumpedCC, contactInfo);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                    }
                }
            }
        }

        // Explicit entry-point trampoline (ported from ROCK PhysicsHooks.cpp). The native
        // function starts with ordinary prologue bytes (not a branch), so CommonLib's
        // write_branch can't derive a callable original — we relocate the stolen bytes
        // into our own trampoline and validate the exact prologue before patching.
        bool installEntryTrampolineHook(const char* label, std::uintptr_t targetOffset,
            const std::uint8_t* expectedPrefix, std::size_t stolenBytes, void* hook, void*& original)
        {
            if (stolenBytes < 14) {
                spdlog::error("[HandBumpHook] {} install failed: stolen bytes {} cannot hold an absolute jump", label, stolenBytes);
                return false;
            }

            REL::Relocation<std::uintptr_t> target{ REL::Offset(targetOffset) };
            auto* targetAddr = reinterpret_cast<std::uint8_t*>(target.address());
            if (!targetAddr || !expectedPrefix) {
                return false;
            }
            if (std::memcmp(targetAddr, expectedPrefix, stolenBytes) != 0) {
                spdlog::error("[HandBumpHook] {} validation failed at 0x{:X}; native bytes changed — not installing", label, target.address());
                return false;
            }

            constexpr std::size_t kJumpBytes = 14;
            const std::size_t trampolineBytes = stolenBytes + kJumpBytes;
            auto* trampolineMem = reinterpret_cast<std::uint8_t*>(
                VirtualAlloc(nullptr, trampolineBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (!trampolineMem) {
                spdlog::error("[HandBumpHook] {} install failed: trampoline allocation failed", label);
                return false;
            }

            std::memcpy(trampolineMem, targetAddr, stolenBytes);
            writeAbsoluteJump(trampolineMem + stolenBytes, target.address() + stolenBytes);

            DWORD oldTrampolineProtect = 0;
            if (!VirtualProtect(trampolineMem, trampolineBytes, PAGE_EXECUTE_READ, &oldTrampolineProtect)) {
                VirtualFree(trampolineMem, 0, MEM_RELEASE);
                return false;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(targetAddr, stolenBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                VirtualFree(trampolineMem, 0, MEM_RELEASE);
                return false;
            }

            writeAbsoluteJump(targetAddr, reinterpret_cast<std::uintptr_t>(hook));
            for (std::size_t i = kJumpBytes; i < stolenBytes; ++i) {
                targetAddr[i] = 0x90;  // NOP the remainder of the stolen prologue
            }

            FlushInstructionCache(GetCurrentProcess(), targetAddr, stolenBytes);
            VirtualProtect(targetAddr, stolenBytes, oldProtect, &oldProtect);

            original = trampolineMem;
            spdlog::info("[HandBumpHook] installed {} at 0x{:X} (original trampoline 0x{:X})",
                         label, target.address(), reinterpret_cast<std::uintptr_t>(trampolineMem));
            return true;
        }
    }

    bool Install()
    {
        if (g_installed) {
            return true;
        }
        // Ghidra-verified prologue of HandleBumpedCharacter @ 0x141E24980:
        //   48 89 5C 24 08   mov [rsp+8], rbx
        //   48 89 74 24 18   mov [rsp+18], rsi
        //   57               push rdi
        //   48 83 EC 70      sub rsp, 70
        constexpr std::array<std::uint8_t, 15> expectedPrefix{
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x74, 0x24, 0x18,
            0x57,
            0x48, 0x83, 0xEC, 0x70
        };

        void* original = nullptr;
        g_installed = installEntryTrampolineHook(
            "HandleBumpedCharacter", kFunc_HandleBumpedCharacter,
            expectedPrefix.data(), expectedPrefix.size(),
            reinterpret_cast<void*>(&hookedHandleBumpedCharacter), original);
        g_original = reinterpret_cast<HandleBumpedCharacter_t>(original);

        if (!g_installed) {
            spdlog::warn("[HandBumpHook] NOT installed — physics hand bodies are unsafe (keep bUsePhysicsHandBodies off)");
        }
        return g_installed;
    }

    void SetEnabled(bool enabled)
    {
        if (!g_installed && enabled) {
            spdlog::warn("[HandBumpHook] enable requested but hook is not installed — suppression inactive");
        }
        g_enabled.store(enabled, std::memory_order_release);
        spdlog::info("[HandBumpHook] player-proxy bump suppression {}", enabled ? "ENABLED" : "disabled");
    }

    bool IsInstalled()
    {
        return g_installed;
    }
}
