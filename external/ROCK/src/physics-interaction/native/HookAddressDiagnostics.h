#pragma once

/*
 * ── HOOK VALIDATION DIAGNOSTICS ───────────────────────────────────────────────
 *
 * Rule these helpers exist to enforce (2026-07-28 hook-validation post-mortem):
 * a hook-validation failure log states WHAT WAS FOUND and WHAT WAS EXPECTED, and
 * nothing else. It never names a presumed culprit.
 *
 * The previous generation of these messages hardcoded
 *     "prefix validation failed - another mod likely hooks EquipObject"
 * Nothing in that code path had probed for another mod. The guess was later read
 * back as evidence and sent two separate investigations chasing a plugin that was
 * never loaded on either machine. Log findings, not theories.
 *
 * Instead of guessing, RESOLVE the address. GetModuleHandleEx(FROM_ADDRESS) plus
 * GetMappedFileName say which loaded image actually owns a pointer, so a report
 * can read "Heisenberg_F4VR.dll+0x1234" instead of "some other mod, probably".
 * When NO module owns an address that is logged explicitly and loudly: an address
 * inside no loaded image is itself the proof that the pointer is not a game
 * function -- either a misread constant (e.g. a TODO_RE offset of 0 resolving to
 * the PE header) or a VirtualAlloc'd detour trampoline.
 */

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rock::hook_diagnostics
{
    namespace detail
    {
        [[nodiscard]] inline const char* moduleLeafName(const char* path, DWORD length) noexcept
        {
            const char* leaf = path;
            for (DWORD i = 0; i < length; ++i) {
                if (path[i] == '\\' || path[i] == '/') {
                    leaf = path + i + 1;
                }
            }
            return leaf;
        }

        // K32GetMappedFileNameA is exported by kernel32 on every supported Windows,
        // so resolving it dynamically keeps this header free of a psapi.lib link
        // dependency (the ROCK static lib does not link psapi today).
        using GetMappedFileNameA_t = DWORD(WINAPI*)(HANDLE, LPVOID, LPSTR, DWORD);

        [[nodiscard]] inline GetMappedFileNameA_t mappedFileNameProc() noexcept
        {
            static const GetMappedFileNameA_t proc = []() noexcept -> GetMappedFileNameA_t {
                auto* kernel32 = GetModuleHandleA("kernel32.dll");
                if (!kernel32) {
                    return nullptr;
                }
                return reinterpret_cast<GetMappedFileNameA_t>(GetProcAddress(kernel32, "K32GetMappedFileNameA"));
            }();
            return proc;
        }
    }

    /*
     * True when the whole [address, address + length) range is committed and
     * readable. Every byte dump below is gated on this: a validation failure must
     * not turn into an access violation while trying to describe itself.
     */
    [[nodiscard]] inline bool isReadable(std::uintptr_t address, std::size_t length) noexcept
    {
        if (!address || length == 0) {
            return false;
        }

        std::uintptr_t cursor = address;
        const std::uintptr_t end = address + length;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi)) {
                return false;
            }
            if (mbi.State != MEM_COMMIT) {
                return false;
            }
            constexpr DWORD kNoReadAccess = PAGE_NOACCESS | PAGE_GUARD;
            if ((mbi.Protect & kNoReadAccess) != 0 || mbi.Protect == 0) {
                return false;
            }
            cursor = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return true;
    }

    /*
     * "Fallout4VR.exe+0x1E24980 (base 0x7FF6096B0000)" when a loaded image owns the
     * address, otherwise an explicit statement that no loaded module does, with the
     * region facts that prove it. Never returns an empty or speculative string.
     */
    [[nodiscard]] inline std::string describeAddress(std::uintptr_t address)
    {
        if (address == 0) {
            return "null (offset resolved to 0 - the constant is unset)";
        }

        char buffer[640]{};

        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address),
                &module) &&
            module) {
            char path[MAX_PATH]{};
            const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
            const char* name = (length != 0) ? detail::moduleLeafName(path, length) : "<unnamed-module>";
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            std::snprintf(buffer,
                sizeof(buffer),
                "%s+0x%llX (module base 0x%llX)",
                name,
                static_cast<unsigned long long>(address - base),
                static_cast<unsigned long long>(base));
            return buffer;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return "NO LOADED MODULE owns this address (VirtualQuery failed - address is not mapped at all)";
        }

        char mappedFile[MAX_PATH]{};
        const char* mappedName = "<none>";
        if (auto* proc = detail::mappedFileNameProc(); proc && mbi.State == MEM_COMMIT) {
            const DWORD mappedLength = proc(GetCurrentProcess(), reinterpret_cast<LPVOID>(address), mappedFile, static_cast<DWORD>(sizeof(mappedFile)));
            if (mappedLength != 0) {
                mappedName = detail::moduleLeafName(mappedFile, mappedLength);
            }
        }

        std::snprintf(buffer,
            sizeof(buffer),
            "NO LOADED MODULE owns this address (allocBase=0x%llX state=0x%lX protect=0x%lX type=0x%lX mappedFile=%s)",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(mbi.AllocationBase)),
            static_cast<unsigned long>(mbi.State),
            static_cast<unsigned long>(mbi.Protect),
            static_cast<unsigned long>(mbi.Type),
            mappedName);
        return buffer;
    }

    /// "4C 8B DC 49 89 53 10" - or an explicit unreadable marker. Never faults.
    [[nodiscard]] inline std::string formatBytes(const void* address, std::size_t length)
    {
        const auto numeric = reinterpret_cast<std::uintptr_t>(address);
        if (!isReadable(numeric, length)) {
            return "<unreadable>";
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
        std::string out;
        out.reserve(length * 3);
        char pair[4]{};
        for (std::size_t i = 0; i < length; ++i) {
            std::snprintf(pair, sizeof(pair), "%02X", bytes[i]);
            if (i != 0) {
                out.push_back(' ');
            }
            out.append(pair);
        }
        return out;
    }

    /*
     * Decode the branch that is sitting on a patched function entry, if the found
     * bytes are one. This is the single most useful FACT a "native bytes changed"
     * report can carry: it names the module the entry now jumps into. Supported
     * forms are the ones detour installers in this ecosystem actually emit.
     * Returns an empty string when the bytes are not a recognised branch - in that
     * case we say nothing rather than invent a cause.
     */
    [[nodiscard]] inline std::string describeBranchAtAddress(std::uintptr_t address)
    {
        if (!isReadable(address, 16)) {
            return {};
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
        char buffer[768]{};

        // FF 25 <rel32> : jmp qword ptr [rip + rel32]  (the 14-byte absolute detour)
        if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            const auto rel = *reinterpret_cast<const std::int32_t*>(bytes + 2);
            const auto slot = address + 6u + static_cast<std::intptr_t>(rel);
            if (!isReadable(slot, sizeof(std::uintptr_t))) {
                return "found branch: jmp [rip+rel32] with unreadable pointer slot";
            }
            const auto destination = *reinterpret_cast<const std::uintptr_t*>(slot);
            std::snprintf(buffer,
                sizeof(buffer),
                "found branch: jmp [rip+0x%X] -> 0x%llX, which lives in %s",
                static_cast<unsigned>(rel),
                static_cast<unsigned long long>(destination),
                describeAddress(destination).c_str());
            return buffer;
        }

        // E9 <rel32> : jmp rel32   /   E8 <rel32> : call rel32
        if (bytes[0] == 0xE9 || bytes[0] == 0xE8) {
            const auto rel = *reinterpret_cast<const std::int32_t*>(bytes + 1);
            const auto destination = address + 5u + static_cast<std::intptr_t>(rel);
            std::snprintf(buffer,
                sizeof(buffer),
                "found branch: %s rel32 -> 0x%llX, which lives in %s",
                bytes[0] == 0xE9 ? "jmp" : "call",
                static_cast<unsigned long long>(destination),
                describeAddress(destination).c_str());
            return buffer;
        }

        // 48 B8 <imm64> FF E0 : mov rax, imm64 ; jmp rax
        if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
            const auto destination = *reinterpret_cast<const std::uintptr_t*>(bytes + 2);
            std::snprintf(buffer,
                sizeof(buffer),
                "found branch: mov rax,0x%llX ; jmp rax -> which lives in %s",
                static_cast<unsigned long long>(destination),
                describeAddress(destination).c_str());
            return buffer;
        }

        return {};
    }

    /*
     * The full found-vs-expected report for a byte-prefix validation failure:
     * both byte strings, the index of the first difference, who owns the target
     * address, and - when the found bytes are a branch - where that branch goes.
     */
    [[nodiscard]] inline std::string describePrefixMismatch(std::uintptr_t targetAddress, const std::uint8_t* expectedPrefix, std::size_t length)
    {
        std::string expected = "<none>";
        if (expectedPrefix) {
            expected = formatBytes(expectedPrefix, length);
        }

        std::size_t firstDiff = length;
        if (expectedPrefix && isReadable(targetAddress, length)) {
            const auto* found = reinterpret_cast<const std::uint8_t*>(targetAddress);
            for (std::size_t i = 0; i < length; ++i) {
                if (found[i] != expectedPrefix[i]) {
                    firstDiff = i;
                    break;
                }
            }
        }

        std::string report;
        report.reserve(512);
        report.append("target 0x");
        {
            char address[32]{};
            std::snprintf(address, sizeof(address), "%llX", static_cast<unsigned long long>(targetAddress));
            report.append(address);
        }
        report.append(" in ").append(describeAddress(targetAddress));
        report.append("; found=[").append(formatBytes(reinterpret_cast<const void*>(targetAddress), length)).append("]");
        report.append(" expected=[").append(expected).append("]");
        if (firstDiff < length) {
            char diff[48]{};
            std::snprintf(diff, sizeof(diff), " firstDifference=+%llu", static_cast<unsigned long long>(firstDiff));
            report.append(diff);
        }
        if (auto branch = describeBranchAtAddress(targetAddress); !branch.empty()) {
            report.append("; ").append(branch);
        }
        return report;
    }
}
