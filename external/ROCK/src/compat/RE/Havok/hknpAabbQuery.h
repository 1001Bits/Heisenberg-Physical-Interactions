#pragma once

// hknpAabbQuery — query struct for hknpWorld::QueryAabb (0x40 bytes)
//
// Used for spatial proximity detection: finds all bodies whose shapes overlap
// a given axis-aligned bounding box. QueryAabb performs BOTH broadphase tree
// traversal AND narrowphase shape geometry tests.
//
// Memory layout confirmed via Ghidra RE of FUN_140cac140 (2026-03-19).
//
// Required setup:
//   1. filterRef (+0x00) — MUST be read from world internals under lock:
//        void* filterRef = *(void**)(*(uintptr_t*)(world + 0x150) + 0x5E8);
//      This is the collision filter configuration from the broadphase dispatcher.
//      Every collision query in the game (QueryAabb, CastRay, CastShape,
//      GetClosestPoints, PickObject) reads this value identically. It is stable
//      across frames and can be cached per cell-load.
//
//   2. collisionFilterInfo (+0x0C) — same bit format as body.collisionFilterInfo:
//      Bits 0-6: collision layer (determines which layers are queried)
//      Bits 16-31: collision group
//
//   3. aabbMin/aabbMax (+0x20/+0x30) — Havok space coordinates (1 unit ≈ 70 game units)
//
// Example:
//   hknpAabbQuery query{};
//   query.filterRef = getQueryFilterRef(world);  // see helper below
//   query.materialId = 0xFFFF;                    // any material
//   query.collisionFilterInfo = (0x000B << 16) | 45;  // group=11, layer=45
//   query.aabbMin = { minX, minY, minZ, 0 };
//   query.aabbMax = { maxX, maxY, maxZ, 0 };
//
//   hknpAllHitsCollector collector;
//   world->QueryAabb(&query, &collector);
//   for (int i = 0; i < collector.hits._size; i++) {
//       auto& hit = collector.hits._data[i];
//       // hit.hitBodyInfo.m_bodyId → resolve to TESObjectREFR
//   }
//
// All offsets confirmed via Ghidra RE on Fallout4VR.exe.unpacked.exe.

namespace RE
{
	/// hknpAabbQuery — AABB spatial query struct (0x40 bytes).
	///
	/// WARNING: The filterRef field is REQUIRED. Without it, the query will
	/// crash or return no results. Read it from the world's broadphase dispatcher:
	///   void* filterRef = *(void**)(*(uintptr_t*)(world + 0x150) + 0x5E8);
	struct hknpAabbQuery
	{
	public:
		// members
		void*         filterRef;            // 00 — collision filter config (from broadphase dispatcher, REQUIRED)
		std::uint16_t materialId;           // 08 — material filter (0xFFFF = accept any material)
		std::uint16_t pad0A;                // 0A
		std::uint32_t collisionFilterInfo;  // 0C — layer/group filter (bits 0-6 = layer, bits 16-31 = group)
		std::uint64_t reserved10;           // 10 — reserved (set to 0)
		std::uint8_t  reserved18;           // 18 — reserved (set to 0)
		std::uint8_t  pad19[7];             // 19 — padding to 0x20 alignment
		float         aabbMin[4];           // 20 — AABB minimum bounds (Havok space, W=0)
		float         aabbMax[4];           // 30 — AABB maximum bounds (Havok space, W=0)
	};
	static_assert(sizeof(hknpAabbQuery) == 0x40);
}
