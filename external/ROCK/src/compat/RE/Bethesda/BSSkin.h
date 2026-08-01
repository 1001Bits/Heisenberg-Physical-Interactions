#pragma once

// BSSkin — Fallout 4 skinning system (replaces NiSkinInstance)
//
// BSSkin::Instance is the per-geometry skinning data, holding bone arrays,
// skin-to-bone transforms, and the BSSkin::BoneData reference.
//
// BSSkin::BoneData holds per-bone skinning transforms (skin-to-bone matrices)
// with bounding sphere data per bone.
//
// STRUCT LAYOUTS VERIFIED via Ghidra decompilation (2026-03-31):
//   - BSSkin::Instance constructor: 0x141c33670
//   - BSSkin::Instance destructor:  0x141c33740
//   - BSSkin::Instance factory:     0x141c335e0 (allocates 0xC0 bytes, align 0x10)
//   - BSSkin::Instance::LoadBinary: 0x141c33050, 0x141c34070
//   - BSSkin::BoneData constructor: 0x141c35140
//   - BSSkin::BoneData factory:     0x141c32f30
//   - GetSkinnedVertex:             0x141cdaa10
//   - ApplySkinningToGeometry:      0x141db2be0
//
// Constructor analysis (0x141c33670):
//   FUN_141b93950 initializes a simple {ptr(8), count(4)} array struct (NOT NiTArray).
//   FUN_141b93680 initializes a single uint32 field.
//   These are lightweight Bethesda-specific array wrappers, NOT NiTObjectArray.
//
// NOTE: FO4 uses BSSkin::Instance (NOT NiSkinInstance) for BSTriShape.
// The skinInstance pointer in BSTriShape at VR offset +0x180 points to
// BSSkin::Instance (vtable 0x142e59a48), NOT NiSkinInstance.

#include "RE/NetImmerse/NiObject.h"
#include "RE/NetImmerse/NiSmartPointer.h"
#include "RE/NetImmerse/NiTransform.h"

namespace RE
{
	class NiAVObject;
	class NiNode;

	namespace BSSkin
	{
		/// BSSkin::BoneData — per-bone skinning transform data.
		///
		/// Contains an array of per-bone entries (0x50 bytes each) with:
		///   - Bounding sphere center (for culling)
		///   - 3x4 skin-to-bone matrix (rotation + translation)
		///   - Scale factor
		///
		/// Vtable: 0x142e598e8
		/// Constructor: 0x141c35140
		/// Factory: 0x141c32f30
		/// AllocateTransforms: 0x141c357b0
		class BoneData : public NiObject
		{
		public:
			/// Per-bone transform entry (0x50 = 80 bytes per bone).
			/// Stored in a contiguous aligned array at boneData+0x10.
			struct BoneTransform
			{
				float boundingSphereCenter[3];  // 00 — bone-local bounding sphere center
				float pad0C;                    // 0C — padding (0)
				float skinToBone[12];           // 10 — 3x4 matrix: 3x3 rotation + translation
												//      [0-2]=row0, [3-5]=row1, [6-8]=row2, [9-11]=translate
				float pad40[3];                 // 40 — extra data
				float scale;                    // 4C — scale factor (default 1.0f)
			};
			static_assert(sizeof(BoneTransform) == 0x50);

			// members
			std::uintptr_t transformArrayData;  // 10 — pointer to BoneTransform array
			std::uint32_t  transformCount;      // 18 — capacity
			std::uint32_t  pad1C;               // 1C
			std::uintptr_t secondaryData;       // 20 — secondary container (init by FUN_141b93680)
		};
		static_assert(sizeof(BoneData) == 0x28);

		/// BSSkin::Instance — per-geometry skinning instance data.
		///
		/// Memory layout: 0xC0 bytes (factory at 0x141c335e0 allocates 0xC0, align 0x10).
		/// Vtable: 0x142e59a48
		/// Constructor: 0x141c33670
		/// Destructor: 0x141c33740
		///
		/// Internal array types are NOT NiTObjectArray — they are lightweight
		/// Bethesda-specific {ptr, count} structs initialized by FUN_141b93950.
		/// FUN_141b93950 writes: *ptr = 0 (8 bytes), *(uint32*)(ptr+8) = 0 (4 bytes).
		///
		/// Access from BSTriShape:
		///   auto* skinInst = *reinterpret_cast<BSSkin::Instance**>(
		///       reinterpret_cast<uintptr_t>(triShape) + 0x180);  // VR offset
		class Instance : public NiObject
		{
		public:
			// +0x10: Bone node array — simple {data_ptr(8), count(4)} struct
			// Initialized by FUN_141b93950. Data pointer at +0x18 holds NiNode** array.
			// GetSkinnedVertex reads: *(ptr*)(skinInst + 0x28) + boneIdx * 8
			// Note: GetSkinnedVertex uses +0x28 which reads bones.data for bone lookups
			void*         bonesData;   // 10 — pointer to bone NiNode* array
			std::uint32_t bonesCount;  // 18 — number of bones
			std::uint32_t pad1C;       // 1C

			// +0x20: Bone array metadata — initialized by FUN_141b93680 (single uint32)
			std::uint32_t bonesCapacity;  // 20
			std::uint32_t pad24;          // 24

			// +0x28: Bone world transform array — same {data_ptr(8), count(4)} pattern
			void*         boneWorldXformData;   // 28 — pointer to bone world transform array
			std::uint32_t boneWorldXformCount;  // 30
			std::uint32_t pad34;                // 34

			// +0x38: Bone world transform metadata
			std::uint32_t boneWorldXformCapacity;  // 38
			std::uint32_t pad3C;                   // 3C

			// +0x40: Pointer to BSSkin::BoneData (shared skin-to-bone transforms)
			// GetSkinnedVertex accesses: *(ptr*)(skinInst + 0x40) → boneData+0x10
			NiPointer<BoneData> boneData;  // 40

			// +0x48: Root node of the skeleton this instance is bound to
			NiAVObject* rootNode;  // 48

			// +0x50: Pre-computed bone world transform buffer (raw alloc, 0x10 per entry)
			void* boneWorldTransformBuffer;  // 50

			std::uint32_t unk58;  // 58 — set to 0 in ctor
			std::uint32_t unk5C;  // 5C — set to 0 in ctor

			// +0x60: Skin-to-root transform (NiTransform = 0x40 bytes in CommonLibF4VR)
			// Constructor loads identity matrix from DAT_ globals, scale = 1.0f at +0x9C
			NiTransform skinToRootTransform;  // 60 — ends at A0

			// +0xA0: Secondary bone world transforms buffer (released in dtor)
			void* boneWorldTransforms;  // A0
			void* secondaryBuffer;      // A8

			std::uint8_t  padB0[0x08];  // B0
			std::uint32_t unkB8;        // B8 — set to 0 in ctor
			std::int32_t  sentinel;     // BC — initialized to -1 (0xFFFFFFFF) in ctor
		};
		static_assert(sizeof(Instance) == 0xC0);
	}
}
