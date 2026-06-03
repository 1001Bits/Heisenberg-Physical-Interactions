#pragma once

#include <cstddef>
#include <cstdint>

// Thin wrapper over Fallout4VR's internal heap allocator. ROCK uses this for any memory
// that the engine's destructor will eventually free via TBB scalable_aligned_free — using
// _aligned_malloc here crashes with EXCEPTION_ACCESS_VIOLATION inside TBB during cleanup
// (Buffout 4's MemoryManager patch routes the engine's destructor through TBB, which
// faults on CRT-allocated pointers).
//
// Used for: bhkPhysicsSystem (0x28), bhkNPCollisionObject (0x30), NiNode (0x180), and any
// other engine-owned struct that gets refcount-released.
//
// Offsets discovered via Ghidra disassembly of ROCK.dll (May 2026). See
// BethesdaPhysicsBodyOffsets.h.

namespace heisenberg::bethesda_alloc
{
    // Allocate `size` bytes from the engine's heap. Returns nullptr on failure.
    //
    // The allocator is lazily initialized on first call (the engine's BethesdaAllocatorInit
    // is invoked once, then cached). Thread-safe via the engine's own init guard.
    //
    // Allocations made through this function may be freed by:
    //   - The engine's refcount-release (CAS-decrement on +0x08 then vtable[0] dtor) for
    //     ref-counted types like bhkNPCollisionObject.
    //   - bhkWorld::RemovePhysicsSystemInstance for hknpPhysicsSystemInstance.
    //   - Never freed by us via _aligned_free / std::free — that path crashes.
    void* Allocate(std::size_t size);

    // Returns true once the engine's heap is initialized + reachable.
    bool IsReady();
}
