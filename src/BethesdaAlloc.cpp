#include "BethesdaAlloc.h"
#include "BethesdaPhysicsBodyOffsets.h"

#include "F4SE/F4SE.h"
#include <spdlog/spdlog.h>
#include <windows.h>

namespace heisenberg::bethesda_alloc
{
    using namespace bethesda_physics_body::offsets;

    // Engine signatures (verified via ROCK source + disasm)
    using AllocatorInit_t = void* (*)(void* pool, std::uint32_t* state);
    using AllocateFn_t    = void* (*)(void* pool, std::size_t size, std::uint32_t, char);

    static REL::Relocation<AllocatorInit_t> g_initFunc{ REL::Offset(kFunc_BethesdaAllocatorInit) };
    static REL::Relocation<AllocateFn_t>    g_allocFunc{ REL::Offset(kFunc_BethesdaAlloc) };
    static REL::Relocation<std::uintptr_t>  g_poolAddr{ REL::Offset(kData_BethesdaAllocatorPool) };

    // The pool struct's first u32 is the init state flag. ROCK source treats it as a
    // separate REL::Relocation but in the disasm it's just `*pool` — same address.
    static inline std::uint32_t* StatePtr() {
        return reinterpret_cast<std::uint32_t*>(g_poolAddr.address());
    }
    static inline void* Pool() {
        return reinterpret_cast<void*>(g_poolAddr.address());
    }

    // SEH-protected init check + (one-shot) init. The engine's BethesdaAllocatorInit takes
    // (pool, &state) and sets state := 2 on success.
    static bool EnsureInitialized()
    {
        auto* state = StatePtr();
        if (!state) return false;
        if (*state == 2) return true;
        __try {
            g_initFunc(Pool(), state);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            spdlog::error("[BethesdaAlloc] AV inside BethesdaAllocatorInit — engine heap not ready");
            return false;
        }
        if (*state != 2) {
            spdlog::warn("[BethesdaAlloc] init returned but state=0x{:x} (expected 2)", *state);
            return false;
        }
        spdlog::info("[BethesdaAlloc] engine heap initialized (pool={:p}, state=2)", Pool());
        return true;
    }

    static bool AllocateSeh(std::size_t size, void*& out)
    {
        __try {
            out = g_allocFunc(Pool(), size, 0u, '\0');
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out = nullptr;
            return false;
        }
    }

    void* Allocate(std::size_t size)
    {
        if (size == 0) return nullptr;
        if (!EnsureInitialized()) return nullptr;
        void* mem = nullptr;
        if (!AllocateSeh(size, mem)) {
            spdlog::error("[BethesdaAlloc] AV inside BethesdaAlloc({} bytes)", size);
            return nullptr;
        }
        if (!mem) {
            spdlog::warn("[BethesdaAlloc] BethesdaAlloc({}) returned nullptr", size);
        }
        return mem;
    }

    bool IsReady()
    {
        auto* state = StatePtr();
        return state && (*state == 2);
    }
}
