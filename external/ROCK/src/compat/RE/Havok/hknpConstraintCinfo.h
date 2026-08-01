#pragma once

// hknpConstraintCinfo — Creation info for physics constraints
//
// Constraints connect two bodies with a physical relationship (hinge, ball-socket,
// ragdoll limits, etc.). This struct is passed to hknpWorld::CreateConstraint().
//
// FO4/FO4VR uses the OLD Havok (hkp*) constraint data classes wrapped in the
// new hknp world. The constraintData pointer must be one of:
//   - hkpBallAndSocketConstraintData  (0x70 bytes) — simplest, single pivot point
//   - hkpRagdollConstraintData        (0x1A0 bytes) — twist/cone/plane limits
//   - hkpHingeConstraintData          — single-axis rotation
//   - hkpLimitedHingeConstraintData   — hinge with angle limits
//   - hkpFixedConstraintData          — rigid weld (no relative motion)
//   - hkpStiffSpringConstraintData    — distance constraint
//   - hkpPrismaticConstraintData      — sliding joint
//   - hknpBreakableConstraintData     — wrapper that breaks under force
//   - hknpMalleableConstraintData     — soft constraint
//
// For HIGGS-style grab mechanics (6-DOF grab):
//   Use hkpBallAndSocketConstraintData with the grab point as pivot.
//   The grabbed object stays DYNAMIC, the hand body stays KEYFRAMED.
//   The constraint tethers them, so the object swings naturally and
//   collides with the world while held.
//
// RE'd from hknpWorld::createConstraint at 0x1415469b0 and
// hknpConstraint::init at 0x1417e39c0 (Fallout4VR.exe.unpacked.exe).
//
// Usage:
//   // Create a ball-and-socket constraint for grab
//   auto* data = createBallAndSocketData(pivotA, pivotB);  // see below
//
//   RE::hknpConstraintCinfo cinfo;
//   cinfo.constraintData = data;
//   cinfo.bodyIdA = handBodyId.value;
//   cinfo.bodyIdB = objectBodyId.value;
//   cinfo.flags = 0;
//   uint32_t constraintId = world->CreateConstraint(cinfo);
//
//   // Later: destroy it
//   world->DestroyConstraints(&constraintId, 1);

#include "RE/Havok/hknpBodyId.h"

namespace RE
{
	class hkpConstraintData;

	/// hknpConstraintCinfo — parameters for creating a constraint between two bodies.
	///
	/// Memory layout: 0x18 bytes (confirmed via Ghidra RE).
	/// Passed by reference to hknpWorld::CreateConstraint().
	struct hknpConstraintCinfo
	{
	public:
		/// Polymorphic constraint data object (hkpBallAndSocketConstraintData, etc.)
		/// Must be ref-counted (inherits hkReferencedObject). The world takes a reference,
		/// so you can release yours after CreateConstraint returns.
		hkpConstraintData* constraintData;  // 00

		/// Body ID of the first body in the constraint pair.
		/// For grab: this is typically the hand/controller body (KEYFRAMED).
		std::uint32_t bodyIdA;  // 08

		/// Body ID of the second body in the constraint pair.
		/// For grab: this is the object being grabbed (DYNAMIC).
		std::uint32_t bodyIdB;  // 0C

		/// Constraint flags.
		/// Bit 0: activation mode flag (set by Bethesda for certain constraint types)
		/// Bit 1: used during constraint setup
		/// Typically 0 for user-created constraints.
		std::uint8_t flags;  // 10

		std::uint8_t pad11[7];  // 11
	};
	static_assert(sizeof(hknpConstraintCinfo) == 0x18);

	/// hknpConstraint — runtime constraint stored in the world's constraint array.
	///
	/// Memory layout: 0x38 bytes (stride in constraint array at hknpWorld+0x128).
	/// Created by hknpWorld::CreateConstraint, destroyed by DestroyConstraints.
	/// You typically don't construct these directly — they're managed by the world.
	struct hknpConstraint
	{
	public:
		std::uint32_t      bodyIdA;         // 00 — first body
		std::uint32_t      bodyIdB;         // 04 — second body
		hkpConstraintData* constraintData;  // 08 — ref-counted constraint data
		std::uint32_t      constraintId;    // 10 — ID returned by CreateConstraint
		std::uint16_t      nextFreeLink;    // 14 — free list link (0xFFFF = active/in-use)
		std::uint8_t       flags;           // 16 — bit 0x04 = disabled
		std::uint8_t       constraintType;  // 17 — type from constraintData vtable
		void*              atomChainPtr;    // 18 — pointer to constraint atom chain
		std::uint16_t      schemaSize;      // 20 — Jacobian schema workspace size
		std::int16_t       sizeOfSchemas;   // 22 — from hkpConstraintInfo.m_sizeOfSchemas (Ghidra-verified)
		std::uint8_t       pad24;           // 24
		std::uint8_t       pad25;           // 25
		std::int16_t       runtimeSize;     // 26 — solver runtime data size
		void*              runtimeData;     // 28 — allocated solver runtime buffer
		std::uint64_t      pad30;           // 30
	};
	static_assert(sizeof(hknpConstraint) == 0x38);

	// =========================================================================
	// Constraint Data Types
	//
	// These are the OLD Havok (hkp*) constraint data classes. FO4 uses them
	// directly inside the hknp world. They inherit hkReferencedObject and have
	// a vtable for the solver.
	//
	// You typically don't construct these via C++ new — use the Havok allocator
	// through TLS (see the pattern in the decompiled code). However, you CAN
	// allocate them on the Havok heap and set fields manually.
	// =========================================================================

	/// hkpConstraintData — abstract base class for all constraint data types.
	///
	/// All concrete constraint data inherit this. The vtable determines the
	/// constraint solver behavior.
	///
	/// Key vtable methods:
	///   +0x20: GetType() — returns constraint type enum
	///   +0x28: GetConstraintInfo() — fills in solver atom info
	///   +0x90: GetRuntimeSize() — solver runtime buffer size
	///   +0xC0: GetWrappedData() — for compound types (0x0C, 0x0D)
	class hkpConstraintData
	{
	public:
		/// Constraint type enum (from vtable+0x20 GetType() return value).
		/// These identify which solver algorithm to use.
		enum class ConstraintType : std::uint8_t
		{
			kBallAndSocket = 0,  ///< Ball-and-socket (3 DOF rotation free)
			kHinge = 1,          ///< Single-axis hinge
			kLimitedHinge = 2,   ///< Hinge with angle limits
			kPrismatic = 6,      ///< Sliding joint
			kRagdoll = 7,        ///< Ragdoll (cone + twist + plane limits)
			kStiffSpring = 8,    ///< Fixed-distance spring
			kFixed = 0x17,       ///< Rigid weld (no relative motion)
			kBreakable = 0xC,    ///< Breakable wrapper (compound type)
			kMalleable = 0xD,    ///< Soft/malleable wrapper (compound type)
		};

		// members (hkReferencedObject base)
		void*         vtable;            // 00
		std::uint32_t refCountAndFlags;  // 08 — lower 16 bits = ref count
		std::uint16_t pad0C;             // 0C
		std::uint16_t pad0E;             // 0E
		std::uint64_t userData;          // 10 — constraint user data (often 0)
	};
	static_assert(sizeof(hkpConstraintData) == 0x18);

	/// hkpBallAndSocketConstraintData — simple ball-and-socket (spherical) joint.
	///
	/// Memory layout: 0x70 bytes (confirmed via constructor at VR 0x1419af690).
	///
	/// The simplest constraint type. Connects two bodies at a shared pivot point
	/// with all 3 rotation axes free. Perfect for HIGGS-style grab mechanics:
	/// the grabbed object can rotate freely around the grab point.
	///
	/// Pivot points are in each body's LOCAL space. When the constraint is
	/// active, the solver tries to keep both pivots at the same world position.
	///
	/// Usage:
	///   // Allocate on Havok heap (0x70 bytes)
	///   // Set vtable to hkpBallAndSocketConstraintData vtable
	///   // Set pivots:
	///   data->pivotA = grabPointInHandLocalSpace;   // +0x30
	///   data->pivotB = grabPointInObjectLocalSpace;  // +0x40
	///
	/// For grab: pivotA is typically {0,0,0} (hand center), pivotB is the
	/// grab point transformed into the object's local space.
	///
	/// VR addresses:
	///   Constructor: 0x1419af690
	///   SetPivots:   0x1419af6e0
	///   Vtable:      0x142e17a58
	struct hkpBallAndSocketConstraintData : public hkpConstraintData
	{
	public:
		// Atoms data (solver atoms start at +0x18 in base, but with alignment)
		std::uint8_t atomData[0x18];  // 18 — solver atom header (bridge atom type=3, etc.)

		/// Pivot point in body A's local space (hkVector4f).
		/// For grab: the point on the hand where the grab connects.
		/// Typically {0,0,0,0} for grabbing at hand center.
		hkVector4f pivotA;  // 30

		/// Pivot point in body B's local space (hkVector4f).
		/// For grab: the point on the grabbed object where contact was made,
		/// transformed into the object's local coordinate frame.
		hkVector4f pivotB;  // 40

		// Remaining atom/solver data
		std::uint8_t pad50[0x20];  // 50
	};
	static_assert(sizeof(hkpBallAndSocketConstraintData) == 0x70);

	/// hkpRagdollConstraintData — ragdoll constraint with cone, twist, and plane limits.
	///
	/// Memory layout: 0x1A0 bytes (confirmed via constructor at VR 0x1419b1d50).
	///
	/// The most complex standard constraint type. Used for NPC ragdoll joints.
	/// Has three rotation limits:
	///   - Cone limit: restricts the angle between twist axes
	///   - Twist limit: restricts rotation around the twist axis
	///   - Plane limit: restricts rotation in the remaining plane
	///
	/// Each body has a full reference frame (3 axes) defining the joint orientation.
	/// The constructor initializes with identity frames and default ragdoll limits.
	///
	/// For PLANCK-style NPC physics: create ragdoll constraints between NPC limb
	/// bodies to enable partial ragdoll (e.g., arms go limp on hit).
	///
	/// Key offsets:
	///   +0x30..+0x5F = body A reference frame (3x hkVector4f: twist, plane, normal)
	///   +0x60..+0x6F = zero padding
	///   +0x70..+0x9F = body B reference frame (3x hkVector4f)
	///   +0xC2 = cone limit angle flag
	///   +0xC4 = limit config (packed)
	///   +0x138..+0x13B = twist limit min angle
	///   +0x13C..+0x13F = twist limit max angle
	///   +0x15C = motor strength (float, 1.0 = full)
	///
	/// VR addresses:
	///   Constructor: 0x1419b1d50
	///   Vtable:      0x142e18298
	struct hkpRagdollConstraintData : public hkpConstraintData
	{
	public:
		std::uint8_t data[0x188];  // 18 — full ragdoll atom data (complex)
	};
	static_assert(sizeof(hkpRagdollConstraintData) == 0x1A0);
}
