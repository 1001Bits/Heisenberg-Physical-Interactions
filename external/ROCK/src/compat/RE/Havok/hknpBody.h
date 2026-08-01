#pragma once

// hknpBody — Havok 2014 (New Physics) rigid body descriptor
//
// Each body lives in a flat array inside hknpWorld (world+0x20, stride 0x90 per body).
// Bodies reference motions (separate array at world+0xE0, stride 0x80) via motionIndex.
//
// Key relationships:
//   hknpBody -> hknpMotion  (via motionIndex)
//   hknpBody -> hknpShape   (via shape pointer at +0x48)
//   hknpBody -> NiAVObject  (via userData at +0x88, when valid — see validation notes below)
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │  IMPORTANT: userData validation                                        │
// │  body.userData (+0x88) is NOT always a TESObjectREFR* or valid pointer.│
// │  FOIslandActivationListener dereferences it unconditionally, so if you │
// │  create bodies, set cinfo.userData = 0 unless you have a valid pointer.│
// │  Before dereferencing: check alignment (% 8 == 0), pointer range      │
// │  (0x10000..0x7FFFFFFFFFFF), and vtable validity.                       │
// └─────────────────────────────────────────────────────────────────────────┘
//
// STRUCT LAYOUT VERIFIED via Ghidra decompilation (2026-03-17):
//   - hknpBody__initFromCinfo (0x1415616f0): maps cinfo fields to body offsets
//   - hknpWorld__enableBodyFlags (0x14153c090): writes flags at body+0x40
//   - hknpBSWorld__setBodyCollisionFilterInfo (0x141df5b80→0x14153af00): writes filterInfo at body+0x44
//   - bhkNPCollisionObject__GetCollisionFilterInfo (0x141e08d60): reads body+0x44
//
// Offsets RE'd via Ghidra on Fallout4VR.exe.unpacked.exe and confirmed by
// cross-referencing multiple functions that read/write body fields.
//
// Usage:
//   auto& body = world->GetBody(bodyId);
//   uint32_t filter = body.collisionFilterInfo;  // +0x44
//   uint32_t layer = filter & 0x7F;
//   auto* shape = body.shape;                     // +0x48
//   auto* motion = world->GetBodyMotion(bodyId);  // via motionIndex at +0x68

#include "RE/Havok/hknpBodyId.h"
#include "RE/Havok/hknpMaterialId.h"

namespace RE
{
	class hknpShape;

	/// hknpBody — per-body data stored in the world's body array.
	///
	/// Memory layout: 0x90 bytes per body (stride confirmed via Ghidra).
	/// The body array is at hknpWorld+0x20 and indexed directly by hknpBodyId::value.
	///
	/// Bodies come in three motion types (controlled by motionPropertiesId at +0x72):
	///   STATIC (0)    — never moves, motionIndex typically 0 (shared static motion)
	///   DYNAMIC (1)   — simulated by solver, affected by gravity and impulses
	///   KEYFRAMED (2) — position set externally (e.g., VR controller tracking)
	///
	/// To check if a body is dynamic: (motionPropertiesId & 0xFF) == 1
	/// To check if a body is static:  motionIndex == 0 (shared static motion slot)
	struct hknpBody
	{
	public:
		// +0x00..+0x3F: hkTransform — 3x3 rotation (COLUMN-MAJOR) + translation.
		// Ghidra-verified 2026-03-25 via hknpBody::initFromCinfo (0x1415616f0):
		//   FUN_1417213a0 converts cinfo quaternion → 3 columns of 4 floats.
		//   Column 0 (+0x00): local X-axis in world coords (xyz) + W = AABB/COM data
		//   Column 1 (+0x10): local Y-axis in world coords (xyz) + W = AABB/COM data
		//   Column 2 (+0x20): local Z-axis in world coords (xyz) + W = AABB/COM data
		//   Translation (+0x30): world position in Havok space (xyz, w)
		// To convert world→local: localPos = dot(col_i, worldDelta) for each axis i.
		// The W components of columns 0-2 are preserved from initialization (AABB/COM).
		float transformCol0[4];  // 00 — rotation column 0 (local X-axis in world) + W
		float transformCol1[4];  // 10 — rotation column 1 (local Y-axis in world) + W
		float transformCol2[4];  // 20 — rotation column 2 (local Z-axis in world) + W
		float translation[4];    // 30 — world translation (Havok space)

		// +0x40: Body flags (uint32)
		// Written by enableBodyFlags/disableBodyFlags.
		// initFromCinfo sets: (cinfo.pad18 & 0xFFFFFFF0) | 0x400
		// initializeDynamicBody adds: | 0x2
		// Key bits (Ghidra-verified 2026-03-25 via SetMotionType + initializeDynamicBody):
		//   0x00000001 = static body (no own motion, shares motion 0). Set by setBodyStatic.
		//   0x00000002 = has motion / dynamic-initialized (permanent). Set by initializeDynamicBody.
		//   0x00000004 = keyframed. Set by setBodyKeyframed, cleared on un-keyframe.
		//   0x00000080 = Bethesda per-body contact subscription bookkeeping (NOT a Havok flag)
		//   0x00000400 = committed to world (set by initFromCinfo)
		//   0x00020000 = contact modifier enabled (Havok contact event generation)
		//   0x08000000 = Bethesda CC grabbed-body bookkeeping flag (does NOT prevent Havok
		//                 deactivation — activateBody checks bit 3 not bit 27). Use explicit
		//                 hknpWorld::activateBody per-frame to keep bodies awake.
		std::uint32_t flags;  // 40

		// +0x44: Collision filter info (uint32)
		// Written by setBodyCollisionFilterInfo, copied from cinfo+0x14.
		// Bits 0-6:   collision layer (0-46)
		// Bits 16-31: collision group
		// Used by bhkCollisionFilter to decide if two bodies can collide.
		std::uint32_t collisionFilterInfo;  // 44

		// +0x48: Shape pointer
		// The collision shape (hknpCapsuleShape, hknpConvexShape, etc.)
		// Copied from cinfo.shape. Raw pointer — NOT ref-counted in the body.
		const hknpShape* shape;  // 48

		// +0x50..+0x5F: Internal data (broadphase AABB quantized, set during commit)
		std::uint8_t pad50[0x10];  // 50

		// +0x60: Body ID (matches the array index, set by initFromCinfo)
		hknpBodyId bodyId;  // 60
		// +0x64: Next attached body ID (compound body linked list, 0xFFFFFFFF = none).
		std::uint32_t nextAttachedBodyId;  // 64

		// +0x68: Motion index — indexes into the motion array (world+0xE0, stride 0x80).
		// motionIndex == 0 means "shared static motion" (body is static).
		// IMPORTANT: Must bounds-check (> 4096 likely invalid / garbage).
		// Compound objects share the same motionId (e.g., bodies 1593-1597 all use motionId=165).
		std::uint32_t motionIndex;  // 68

		// +0x6C: Deactivation island ID (-1 = none / not in any island).
		std::int32_t deactivationIslandId;  // 6C

		// +0x70: Material ID (uint16) — from cinfo.materialId
		hknpMaterialId materialId;  // 70

		// +0x72: Motion properties ID — determines physics behavior.
		// Lower byte is the preset: 0=STATIC, 1=DYNAMIC, 2=KEYFRAMED, 3=FROZEN
		// Set by initializeDynamicBody from cinfo.motionPropertiesId.
		// Use (motionPropertiesId & 0xFF) to get the base preset.
		std::uint8_t motionPropertiesId;  // 72

		std::uint8_t pad73[0x0B];  // 73

		// +0x7E: Quality ID — affects solver iteration count.
		// Set from cinfo.qualityId. 0xFF = default.
		// KEYFRAMED bodies should use quality 2.
		std::uint8_t qualityId;  // 7E

		// +0x7F: Shape complexity flag (derived from shape during initFromCinfo)
		std::uint8_t shapeComplexity;  // 7F

		// +0x80..+0x87: Internal data
		std::uint8_t pad80[0x08];  // 80

		// +0x88: User data — often a TESObjectREFR* but MUST be validated before use.
		// Set from cinfo.userData. Set to 0 for custom bodies (hand colliders, etc.)
		// to avoid crashes in FOIslandActivationListener.
		std::uintptr_t userData;  // 88
	};
	static_assert(sizeof(hknpBody) == 0x90);

	/// Helper to check if a body is dynamic (affected by physics simulation)
	inline bool IsBodyDynamic(const hknpBody& body)
	{
		return (body.motionPropertiesId & 0xFF) == 1;
	}

	/// Helper to check if a body is keyframed (position set externally)
	inline bool IsBodyKeyframed(const hknpBody& body)
	{
		return (body.motionPropertiesId & 0xFF) == 2;
	}

	/// Helper to check if a body is static (never moves)
	inline bool IsBodyStatic(const hknpBody& body)
	{
		return body.motionIndex == 0;
	}
}
