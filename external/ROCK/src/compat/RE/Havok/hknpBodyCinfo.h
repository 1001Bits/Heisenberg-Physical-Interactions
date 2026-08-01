#pragma once

// hknpBodyCinfo — Body creation info for hknpWorld::CreateBody()
//
// This struct configures all properties of a new physics body before creation.
// Fill it out, then pass to hknpWorld::CreateBody().
//
// CRITICAL: For non-static bodies, you MUST allocate a motion first:
//   cinfo.motionId = { world->AllocateMotion() };
//   Without this, the body has no motion slot and behavior is undefined.
//
// CRITICAL: Set userData = 0 for custom bodies (hand colliders, etc.)
//   FOIslandActivationListener dereferences userData unconditionally.
//   If it's garbage, the game crashes.
//
// Motion property presets:
//   STATIC (0)    — never moves, no gravity, no collision response
//   DYNAMIC (1)   — full simulation with gravity
//   KEYFRAMED (2) — position controlled externally (VR tracking), no gravity
//
// Layout verified via Ghidra decompilation of constructor at VR 0x141561dd0
// and initFromCinfo at VR 0x1415616f0 (reads shape from offset 0x00).
//
// Usage:
//   RE::hknpBodyCinfo cinfo;
//   cinfo.shape = myShape;
//   cinfo.motionId = { world->AllocateMotion() };
//   cinfo.motionPropertiesId = { 2 };  // KEYFRAMED for VR tracking
//   cinfo.position = { x * kHavokScale, y * kHavokScale, z * kHavokScale, 0 };
//   cinfo.userData = 0;  // MUST be 0 or valid pointer!
//   auto bodyId = world->CreateBody(cinfo);

#include "RE/Havok/hkRefPtr.h"
#include "RE/Havok/hkVector4.h"
#include "RE/Havok/hknpBodyId.h"
#include "RE/Havok/hknpMaterialId.h"
#include "RE/Havok/hknpShape.h"

namespace RE
{
	struct hknpMotionIdDiscriminant;
	struct hknpMotionPropertiesIdDiscriminant;
	struct hknpBodyQualityIdDiscriminant;

	/// Motion ID handle — indexes into the world's motion array.
	/// 0x7FFFFFFF = auto-assign (not recommended — prefer explicit AllocateMotion).
	/// 0 = shared static motion (for static bodies only).
	struct hknpMotionId :
		public hkHandle<std::uint32_t, 0x7FFF'FFFF, hknpMotionIdDiscriminant>
	{
	public:
	};
	static_assert(sizeof(hknpMotionId) == 0x4);

	/// Motion properties ID — selects the physics behavior preset.
	/// Lower byte is the preset index:
	///   0 = STATIC  (no velocity allowed)
	///   1 = DYNAMIC (gravity, solver-driven)
	///   2 = KEYFRAMED (externally driven, no gravity — for VR controllers)
	///   3 = FROZEN  (high damping, no gravity)
	///   4 = DEBRIS  (dynamic with aggressive deactivation)
	/// 0xFF = default (typically resolves to DYNAMIC).
	struct hknpMotionPropertiesId_Handle :
		public hkHandle<std::uint16_t, 0xFF, hknpMotionPropertiesIdDiscriminant>
	{
	public:
	};
	static_assert(sizeof(hknpMotionPropertiesId_Handle) == 0x2);

	/// Body quality ID — affects solver iteration count for this body.
	/// 0xFF = default quality.
	struct hknpBodyQualityId :
		public hkHandle<std::uint8_t, 0xFF, hknpBodyQualityIdDiscriminant>
	{
	public:
	};
	static_assert(sizeof(hknpBodyQualityId) == 0x1);

	/// hknpBodyCinfo — all parameters needed to create a physics body.
	///
	/// Memory layout: 0x60 bytes. Constructor at REL::ID(718403) initializes
	/// defaults (identity quaternion, auto-assign IDs, zero position).
	struct hknpBodyCinfo
	{
	public:
		/// Default constructor — sets safe defaults.
		/// After construction: shape=nullptr, bodyId=auto, motionId=auto,
		/// motionPropertiesId=DYNAMIC, identity orientation, zero position.
		hknpBodyCinfo()
		{
			typedef hknpBodyCinfo*         func_t(hknpBodyCinfo*);
			static REL::Relocation<func_t> func{ REL::ID(718403) };
			func(this);
		}

		// Layout verified via Ghidra decompilation of constructor (VR 0x141561dd0)
		// and initFromCinfo (VR 0x1415616f0) which reads shape from offset 0x00.

		// members
		const hknpShape*              shape;                       // 00 — collision shape (raw pointer, NOT ref-counted here)
		hknpBodyId                    bodyId;                      // 08 — 0x7FFFFFFF = auto-assign (default)
		hknpMotionId                  motionId;                    // 0C — motion slot index (MUST set for non-static bodies)
		hknpMotionPropertiesId_Handle motionPropertiesId;          // 10 — {0}=STATIC, {1}=DYNAMIC, {2}=KEYFRAMED, 0xFF=default→DYNAMIC
		hknpMaterialId                materialId;                  // 12 — physics material (friction/restitution)
		std::uint32_t                 collisionFilterInfo;         // 14 — collision layer bits
		std::uint32_t                 pad18;                       // 18
		std::uint32_t                 collisionLookAheadDistance;  // 1C — CCD look-ahead distance
		const char*                   name;                        // 20 — debug name string (can be nullptr)
		std::uintptr_t                userData;                    // 28 — user data (SET TO 0 for custom bodies!)
		hkVector4f                    position;                    // 30 — world-space position (Havok coordinates)
		hkVector4f                    orientation;                 // 40 — quaternion {x,y,z,w}, identity = {0,0,0,1}
		std::uint8_t                  qualityId;                   // 50 — solver quality preset
		std::uint8_t                  pad51[7];                    // 51
		std::uint64_t                 reserved58;                  // 58 — ref-counted by ctor/dtor, don't touch
	};
	static_assert(sizeof(hknpBodyCinfo) == 0x60);
}
