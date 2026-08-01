#pragma once

// hknpCapsuleShape — Havok 2014 capsule collision shape
//
// A capsule (pill shape) defined by a line segment (start → end) and a radius.
// This is the primary shape used for VR hand colliders in HIGGS-FO4VR.
//
// Inheritance: hknpCapsuleShape → hknpConvexShape → hknpShape → hkReferencedObject
//
// Creating capsule shapes:
//   // Direct creation (preferred for custom physics bodies):
//   hkVector4f start = { 0, 0, -0.02f, 0 };   // Havok space
//   hkVector4f end   = { 0, 0,  0.02f, 0 };   // Havok space
//   auto* capsule = RE::hknpCapsuleShape::CreateCapsuleShape(start, end, 0.015f);
//
//   // Bethesda's cached factory (auto-scales, 10-entry cache):
//   // REL::ID(1526865) — bhkCharacterControllerShapeManager::GetCapsuleShape
//
// Typical hand collider dimensions (Havok space):
//   Length: ~0.04 units (2.8 game units)
//   Radius: ~0.015 units (1.05 game units)
//
// Memory layout: 0x70 bytes (16-byte aligned).
// Init() builds an 8-vertex convex hull internally and sets type = 0x1C3.
//
// The radius is stored in the parent hknpShape at offset +0x14 (convexRadius).

#include "RE/Havok/hkVector4.h"
#include "RE/Havok/hknpShape.h"

namespace RE
{
	/// hknpConvexShape — intermediate base for all convex primitives.
	/// Adds convex-specific data used by GJK/EPA collision algorithms.
	class hknpConvexShape :
		public hknpShape  // 00
	{
	public:
		// members
		std::uint32_t pad30;  // 30 — convex vertex data offset
		std::uint32_t pad34;  // 34 — num vertices
		std::uint32_t pad38;  // 38 — face data offset
		std::uint32_t pad3C;  // 3C — num faces
	};
	static_assert(sizeof(hknpConvexShape) == 0x40);

	/// hknpCapsuleShape — capsule (pill) collision shape.
	///
	/// Defined by two endpoints (a, b) and a radius (inherited convexRadius at +0x14).
	/// The capsule is the Minkowski sum of the line segment a→b and a sphere of the
	/// given radius.
	///
	/// Used for:
	///   - VR hand colliders (HIGGS-FO4VR)
	///   - Character controller shapes
	///   - Simple physics proxy shapes
	class hknpCapsuleShape :
		public hknpConvexShape  // 00
	{
	public:
		/// Create a new capsule shape (allocates 0x70 bytes internally).
		///
		/// @param a_start   One endpoint of the capsule's central axis (Havok space)
		/// @param a_end     Other endpoint of the capsule's central axis (Havok space)
		/// @param a_radius  Capsule radius (Havok space — divide game units by 70)
		/// @return Pointer to newly created capsule shape (ref-counted, caller owns reference)
		///
		/// Note: The shape is NOT automatically freed when bodies using it are destroyed.
		/// Use hkRefPtr<hknpShape> for automatic lifetime management, or manually track
		/// the pointer and ensure it outlives all bodies referencing it.
		static hknpCapsuleShape* CreateCapsuleShape(hkVector4f& a_start, hkVector4f& a_end, float a_radius)
		{
			using func_t = decltype(&hknpCapsuleShape::CreateCapsuleShape);
			static REL::Relocation<func_t> func{ REL::ID(1316723) };
			return func(a_start, a_end, a_radius);
		}

		/// Re-initialize the capsule with new endpoints (keeps existing radius).
		/// Rebuilds the internal 8-vertex convex hull.
		void Init(hkVector4f& a_start, hkVector4f& a_end)
		{
			using func_t = decltype(&hknpCapsuleShape::Init);
			static REL::Relocation<func_t> func{ REL::ID(647475) };
			return func(this, a_start, a_end);
		}

		// members
		std::uint64_t pad40;  // 40 — internal convex hull data
		std::uint64_t pad48;  // 48 — internal convex hull data
		hkVector4f    a;      // 50 — start endpoint of the capsule axis (Havok space)
		hkVector4f    b;      // 60 — end endpoint of the capsule axis (Havok space)
							  // Note: radius is at this->convexRadius (offset +0x14, inherited from hknpShape)
	};
	static_assert(sizeof(hknpCapsuleShape) == 0x70);
}
