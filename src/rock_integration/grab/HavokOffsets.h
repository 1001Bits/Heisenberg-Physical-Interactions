#pragma once

// FO4VR 1.2.72 offsets the grab-constraint engine TU needs (ROCK ships these as a stub for its
// own build to fill; Heisenberg fills them from Ghidra). Namespace kept as heisenberg::rock_core::offsets.

#include <cstdint>

namespace heisenberg::rock_core::offsets
{
    // The game's hkpConstraintData atom-info walker (FUN_141a4ad20). Ghidra-confirmed: it is what
    // hkpRagdollConstraintData::getConstraintInfo (vf005 @ 0x1419b27f0) tail-calls as
    //   util(atomsStart /*this+0x20*/, atomsSizeBytes, hkpConstraintInfo* out).
    // ROCK's getConstraintInfoCallback calls it with (this+0x20, ATOMS_SIZE=0x148, infoOut).
    inline constexpr std::uintptr_t kFunc_GetConstraintInfoUtil = 0x1A4AD20;
}
