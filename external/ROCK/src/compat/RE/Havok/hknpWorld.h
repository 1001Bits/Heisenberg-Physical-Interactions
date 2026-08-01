#pragma once

// hknpWorld — Havok 2014 New Physics world (core simulation container)
//
// This is the central physics simulation object in Fallout 4 / FO4VR.
// It owns all bodies, motions, constraints, and event dispatchers.
// Bethesda wraps it in bhkWorld (BSHavok.h), but many operations require
// direct access to the underlying hknpWorld.
//
// Thread safety (Ghidra confirmed 2026-03-19):
//   The world has a read/write lock at offset +0x690 (hknpThreadSafetyCheck).
//   Structure at +0x690:
//     +0x00: uint32  state (reader count in bits 0-4, writer thread ID in bits 8-31)
//     +0x08: CRITICAL_SECTION (Windows critical section, ~40 bytes)
//     +0x30: byte    enabled flag
//
//   IMPORTANT: All collision query functions (CastRay, CastShape, QueryAabb,
//   GetClosestPoints) acquire their own read lock internally. Callers do NOT
//   need to manage locking for these functions.
//
//   Lock acquire: FUN_1415388f0(world+0x690) — EnterCriticalSection, increment reader count
//   Lock release: FUN_141538a30(world+0x690) — EnterCriticalSection, decrement reader count
//
//   bhkWorld::PickObject also handles its own locking (at bhkWorld+0x6D8,
//   separate from hknpWorld+0x690).
//
// Getting an hknpWorld pointer:
//   // Via bhkWorld (Bethesda wrapper):
//   RE::bhkWorld* bhk = cell->GetbhkWorld();
//   RE::hknpWorld* world = HavokUtils::GetHknpWorld(bhk);  // bhkWorld+0x60
//
// Coordinate space:
//   All hknpWorld operations use Havok space (1 unit ≈ 70 game units).
//   Convert with HavokUtils::kHavokScale (1.0f / 70.0f).
//
// Memory layout (key offsets from 'this'):
//   +0x20  = hknpBody*  body array (stride 0x90, indexed by hknpBodyId::value)
//   +0xE0  = void*      motion array (stride 0x80, indexed by hknpBody::motionIndex)
//   +0x128 = void*      constraint array (stride 0x38)
//   +0x178 = void*      collision query dispatcher
//   +0x5C8 = hknpMaterialLibrary*          material library (hkRefPtr)
//   +0x5D0 = hknpMotionPropertiesLibrary*  motion properties library (hkRefPtr)
//   +0x5D8 = hknpBodyQualityLibrary*       body quality library (hkRefPtr)
//   +0x648 = void*      event dispatcher (hknpEventDispatcher*)
//   +0x690 = void*      read/write lock (critical section)
//
// All REL::IDs verified against fo4_database.csv for Fallout 4 VR.
// Ghidra RE on Fallout4VR.exe.unpacked.exe.

#include "RE/Bethesda/bhkCharacterController.h"
#include "RE/Havok/hkArray.h"
#include "RE/Havok/hkVector4.h"
#include "RE/Havok/hknpAabbQuery.h"
#include "RE/Havok/hknpBody.h"
#include "RE/Havok/hknpBodyCinfo.h"
#include "RE/Havok/hknpBodyId.h"
#include "RE/Havok/hknpCollisionResult.h"
#include "RE/Havok/hknpConstraintCinfo.h"
#include "RE/Havok/hknpMaterialId.h"
#include "RE/Havok/hknpMotion.h"

namespace RE
{
	struct hknpBody;
	class hknpCollisionQueryCollector;
	class hknpShape;

	struct hknpWorldCinfo;

	/// hknpStepInput — parameters for a single physics step.
	/// TODO: RE internal fields (currently padded to 0x20 bytes).
	struct hknpStepInput
	{
	public:
		// members
		std::byte pad[0x20];  // TODO: deltaTime, gravity, solverIterations, etc.
	};

	/// hknpEventType — event type enum for the world's event dispatcher.
	///
	/// Subscribe to events via hknpWorld::GetEventSignal(eventType).
	/// Contact events (kContact = 3) are the most commonly used — they fire
	/// when bodies collide and are used by FOCollisionListener for gameplay.
	struct hknpEventType
	{
	public:
		enum Enum
		{
			kContact = 3,                             ///< Contact/collision events (FOCollisionListener uses this)
			kIslandActivation = 0xB,                  ///< Simulation island activated/deactivated
			kBroadPhaseExit = 0xC,                    ///< Body left the broadphase region
			kBreakableConstraintForceExceeded = 0x16  ///< Breakable constraint exceeded its force threshold
		};
	};
	static_assert(std::is_empty_v<hknpEventType>);

	/// hknpWorld — the main Havok physics simulation world.
	///
	/// Typical cell statistics (confirmed in FO4VR):
	///   ~2047 bodies total (~1566 static, ~479 dynamic, 0-2 keyframed)
	///   Body IDs are indices into the body array (0 to ~2047)
	///   Motion index 0 is the shared static motion
	///
	/// Body lifecycle:
	///   1. AllocateMotion()     — get a motion slot (skip for static bodies)
	///   2. hknpBodyCinfo setup  — set shape, motionId, motionPropertiesId, etc.
	///   3. CreateBody(cinfo)    — creates body and adds to world
	///   4. ... use body ...
	///   5. RemoveBodies()       — remove from simulation
	///   6. DestroyBodies()      — free body and motion slots
	class hknpWorld
	{
	public:
		// =====================================================================
		// Construction / Configuration
		// =====================================================================

		/// Construct a new physics world with the given configuration.
		hknpWorld(hknpWorldCinfo& a_cinfo)
		{
			typedef hknpWorld*             func_t(hknpWorld*, hknpWorldCinfo&);
			static REL::Relocation<func_t> func{ REL::ID(864328) };
			func(this, a_cinfo);
		}

		/// Destroy the world and all contained bodies/constraints.
		~hknpWorld()
		{
			typedef void                   func_t(hknpWorld*);
			static REL::Relocation<func_t> func{ REL::ID(748654) };
			func(this);
		}

		/// Get the current world configuration (inverse of constructor).
		void GetCinfo(hknpWorldCinfo& a_cinfo)
		{
			using func_t = decltype(&hknpWorld::GetCinfo);
			static REL::Relocation<func_t> func{ REL::ID(624763) };
			return func(this, a_cinfo);
		}

		// =====================================================================
		// Collision Queries
		//
		// These query the broadphase + narrowphase for collisions.
		// Results are collected into hknpCollisionQueryCollector instances.
		// Use hknpClosestHitCollector for single closest hit, or
		// hknpAllHitsCollector for all intersecting bodies.
		// =====================================================================

		/// Cast a ray through the world, collecting hits into the collector.
		///
		/// Self-locking: acquires read lock at world+0x690 internally.
		/// Callers do NOT need to manage locking.
		///
		/// @param a_query     Raycast query struct (start point, direction, max distance)
		/// @param a_collector Receives hit results (use hknpClosestHitCollector or hknpAllHitsCollector)
		///
		/// Note: For most gameplay raycasts, prefer bhkWorld::PickObject() which
		/// also handles NiAVObject resolution automatically.
		void CastRay(void* a_query, hknpCollisionQueryCollector* a_collector)
		{
			using func_t = decltype(&hknpWorld::CastRay);
			static REL::Relocation<func_t> func{ REL::ID(1440166) };
			return func(this, a_query, a_collector);
		}

		/// Sweep a shape through the world (linear cast / shape cast).
		/// This is the Havok 2014 equivalent of "linearCast" from older Havok versions.
		///
		/// Self-locking: acquires read lock at world+0x690 internally.
		/// Callers do NOT need to manage locking.
		///
		/// NOTE: The hknpShapeCastQuery struct is ~0x78 bytes with complex precomputed
		/// inverse direction fields (rcpps + movmskps). Consider using QueryAabb +
		/// post-filter as a simpler alternative for spatial proximity detection.
		///
		/// @param a_query              Shape cast query (~0x78 bytes, partially RE'd)
		/// @param a_queryShapeInfo     Shape info for the query shape
		/// @param a_collector          Receives hit results
		/// @param a_startPointCollector Optional: collects hits at the start position (can be nullptr)
		void CastShape(void* a_query, void* a_queryShapeInfo, hknpCollisionQueryCollector* a_collector, hknpCollisionQueryCollector* a_startPointCollector)
		{
			using func_t = decltype(&hknpWorld::CastShape);
			static REL::Relocation<func_t> func{ REL::ID(110287) };
			return func(this, a_query, a_queryShapeInfo, a_collector, a_startPointCollector);
		}

		/// Find the closest points between a shape and the world geometry.
		///
		/// Self-locking: acquires read lock at world+0x690 internally.
		/// Callers do NOT need to manage locking.
		///
		/// @param a_query     Point query struct
		/// @param a_transform Transform for the query shape
		/// @param a_collector Receives closest point results
		void GetClosestPoints(void* a_query, hkTransformf& a_transform, hknpCollisionQueryCollector* a_collector)
		{
			using func_t = decltype(&hknpWorld::GetClosestPoints);
			static REL::Relocation<func_t> func{ REL::ID(1053489) };
			return func(this, a_query, a_transform, a_collector);
		}

		/// Query all bodies whose shapes overlap the given AABB.
		///
		/// Performs BOTH broadphase tree traversal AND narrowphase shape geometry
		/// tests. This is NOT a coarse AABB-only check — it tests the actual shape
		/// of each candidate body against the query AABB. Results are filtered by
		/// collisionFilterInfo.
		///
		/// Self-locking: acquires read lock at world+0x690 internally.
		/// Callers do NOT need to manage locking.
		///
		/// Internal behavior (Ghidra confirmed):
		///   1. Acquires read lock at world+0x690
		///   2. Converts AABB to broadphase quantized shorts
		///   3. Broadphase tree query → candidate body ID array
		///   4. For each candidate: collision filter check + narrowphase overlap test
		///   5. Releases read lock, frees candidates
		///
		/// @param a_query     hknpAabbQuery struct (0x40 bytes, see hknpAabbQuery.h)
		/// @param a_collector Receives hits (use hknpAllHitsCollector for multiple results)
		///
		/// WARNING: Use collector.hits._size / collector.hits._data to access results.
		/// The virtual GetNumHits()/GetHits() methods cause linker errors when called
		/// directly — they are declared but have no compiled implementations in the game binary.
		void QueryAabb(hknpAabbQuery* a_query, hknpCollisionQueryCollector* a_collector)
		{
			typedef void                   func_t(hknpWorld*, hknpAabbQuery*, hknpCollisionQueryCollector*);
			static REL::Relocation<func_t> func{ REL::ID(990501) };
			func(this, a_query, a_collector);
		}

		/// Query all bodies whose shapes overlap the given AABB (array output overload).
		///
		/// Same as above but returns results in an hkArray instead of a collector.
		///
		/// @param a_query     hknpAabbQuery struct (0x40 bytes)
		/// @param a_hitsOut   Array that receives body IDs of overlapping bodies
		void QueryAabb(hknpAabbQuery* a_query, hkArray<hknpBodyId>* a_hitsOut)
		{
			typedef void                   func_t(hknpWorld*, hknpAabbQuery*, hkArray<hknpBodyId>*);
			static REL::Relocation<func_t> func{ REL::ID(1536729) };
			func(this, a_query, a_hitsOut);
		}

		// =====================================================================
		// Motion Allocation
		//
		// Motions are separate from bodies and must be allocated before creating
		// non-static bodies. Static bodies use the shared motion at index 0.
		// =====================================================================

		/// Allocate a motion slot from the world's motion free list.
		///
		/// MUST be called before CreateBody() for any non-static body.
		/// Static bodies should NOT call this — they use the shared motion at index 0.
		///
		/// The returned motionId goes into hknpBodyCinfo::motionId.
		///
		/// VR addresses: FUN_141546350 (motion allocator), FUN_1417a2fc0 (motion cinfo ctor)
		///
		/// @return Allocated motion index to set in hknpBodyCinfo::motionId.
		///
		/// Example:
		///   RE::hknpBodyCinfo cinfo;
		///   cinfo.shape = myShape;
		///   cinfo.motionId = { world->AllocateMotion() };  // allocate motion first!
		///   cinfo.motionPropertiesId = { 2 };  // KEYFRAMED
		///   auto bodyId = world->CreateBody(cinfo);
		std::uint32_t AllocateMotion()
		{
			// Construct default motion creation info (0x70 bytes)
			alignas(16) std::uint8_t                  motionCinfo[0x70];
			typedef void*                             motionCinfoCtor_t(void*);
			static REL::Relocation<motionCinfoCtor_t> motionCinfoCtor{ REL::Offset(0x17A2FC0) };
			motionCinfoCtor(motionCinfo);

			// Allocate and initialize motion from the world's motion manager
			std::int32_t                          motionId = 0;
			typedef std::int32_t*                 allocMotion_t(hknpWorld*, std::int32_t*, void*);
			static REL::Relocation<allocMotion_t> allocMotion{ REL::Offset(0x1546350) };
			allocMotion(this, &motionId, motionCinfo);

			return static_cast<std::uint32_t>(motionId);
		}

		// =====================================================================
		// Body Management
		//
		// Bodies are created from hknpBodyCinfo and identified by hknpBodyId.
		// Remember: allocate a motion FIRST for non-static bodies.
		// =====================================================================

		/// Create a new body in the world.
		///
		/// @param a_cinfo         Body creation info (shape, motion, position, etc.)
		/// @param a_additionMode  0 = deferred (call CommitAddBodies later), 1 = immediate (default)
		/// @param a_flags         Creation flags (0 = default)
		/// @return The new body's ID (use to reference the body in all other methods)
		///
		/// IMPORTANT: For non-static bodies, you MUST call AllocateMotion() first
		/// and set cinfo.motionId to the returned value. Without this, the body will
		/// have no motion slot and behavior is undefined.
		///
		/// Example:
		///   RE::hknpBodyCinfo cinfo;
		///   cinfo.shape = capsule;
		///   cinfo.motionId = { world->AllocateMotion() };
		///   cinfo.motionPropertiesId = { 2 };  // KEYFRAMED
		///   cinfo.position = { x, y, z, 0 };   // Havok space
		///   cinfo.userData = 0;                  // MUST be 0 or valid pointer
		///   auto bodyId = world->CreateBody(cinfo);
		hknpBodyId CreateBody(hknpBodyCinfo& a_cinfo, std::int32_t a_additionMode = 1, std::uint8_t a_flags = 0)
		{
			typedef hknpBodyId*            func_t(hknpWorld*, hknpBodyId*, hknpBodyCinfo&, std::int32_t, std::uint8_t);
			static REL::Relocation<func_t> func{ REL::ID(72830) };
			hknpBodyId                     bodyId;
			func(this, &bodyId, a_cinfo, a_additionMode, a_flags);
			return bodyId;
		}

		/// Remove bodies from the simulation (but don't free their slots).
		/// Use DestroyBodies() to fully release body and motion slots.
		///
		/// @param a_bodyIds        Array of body IDs to remove
		/// @param a_count          Number of IDs in the array
		/// @param a_activationMode 0 = default activation behavior
		void RemoveBodies(hknpBodyId* a_bodyIds, std::int32_t a_count, std::int32_t a_activationMode = 0)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId*, std::int32_t, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(201384) };
			func(this, a_bodyIds, a_count, a_activationMode);
		}

		/// Destroy bodies: remove from simulation AND free body+motion slots.
		/// After this call, the body IDs are invalid and may be reused.
		///
		/// @param a_bodyIds        Array of body IDs to destroy
		/// @param a_count          Number of IDs in the array
		/// @param a_activationMode 0 = default activation behavior
		void DestroyBodies(hknpBodyId* a_bodyIds, std::int32_t a_count, std::int32_t a_activationMode = 0)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId*, std::int32_t, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(1037327) };
			func(this, a_bodyIds, a_count, a_activationMode);
		}

		/// Flush deferred body additions (when CreateBody was called with additionMode=0).
		/// Only needed if you batch-create bodies with deferred mode.
		void CommitAddBodies()
		{
			using func_t = decltype(&hknpWorld::CommitAddBodies);
			static REL::Relocation<func_t> func{ REL::ID(121748) };
			return func(this);
		}

		/// Attach bodies to the broadphase so they participate in collision detection.
		/// MUST be called after CreateBody + CommitAddBodies for bodies to actually collide.
		/// Without this, bodies exist in the body array but the broadphase never checks them.
		///
		/// @param a_bodyIds   Array of body IDs to attach
		/// @param a_count     Number of bodies
		void AttachBodies(const hknpBodyId* a_bodyIds, int a_count)
		{
			using func_t = decltype(&hknpWorld::AttachBodies);
			static REL::Relocation<func_t> func{ REL::ID(609545) };
			return func(this, a_bodyIds, a_count);
		}

		// =====================================================================
		// Body Properties
		//
		// Modify body state after creation. All positions/transforms are in
		// Havok space (1 unit ≈ 70 game units).
		// =====================================================================

		/// Set a body's world transform (position + rotation).
		///
		/// WARNING 1: For keyframed/grabbed objects, using SetBodyTransform every frame
		/// fights the Havok solver and causes visible stutter. Use velocity-driven
		/// movement instead (ComputeHardKeyFrame + SetBodyVelocity).
		///
		/// WARNING 2 (Ghidra confirmed 2026-03-19): SetBodyTransform has a known COM
		/// offset drift issue. Internally, updateMotionAndAttachedBodies computes:
		///   motion_pos = rotation * (body[3], body[7], body[0xB]) + translation
		/// The W components (body[3/7/11]) contain AABB initialization data and are
		/// preserved across SetBodyTransform calls. This causes a rotation-dependent
		/// position offset when the body has a non-zero offset from its creation origin.
		/// Bodies at their creation position (zero offset) work correctly in all directions.
		/// For bodies with position offsets, the collider will drift depending on facing
		/// direction (e.g., correct when facing West, drifts when facing South).
		///
		/// @param a_bodyId              Target body
		/// @param a_transform           New world transform (Havok space)
		/// @param a_activationBehavior  0 = auto-activate if sleeping
		void SetBodyTransform(hknpBodyId a_bodyId, hkTransformf& a_transform, std::int32_t a_activationBehavior = 0)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkTransformf&, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(367136) };
			func(this, a_bodyId, a_transform, a_activationBehavior);
		}

		/// Replace a body's collision shape.
		void SetBodyShape(hknpBodyId a_bodyId, const hknpShape* a_shape)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, const hknpShape*);
			static REL::Relocation<func_t> func{ REL::ID(531033) };
			func(this, a_bodyId, a_shape);
		}

		/// Change a body's physics material (affects friction, restitution, etc.).
		///
		/// @param a_rebuildCachesMode 0 = rebuild collision caches immediately
		void SetBodyMaterial(hknpBodyId a_bodyId, hknpMaterialId a_materialId, std::int32_t a_rebuildCachesMode = 0)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hknpMaterialId, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(50770) };
			func(this, a_bodyId, a_materialId, a_rebuildCachesMode);
		}

		/// Switch a body's motion type at runtime.
		/// This is how you toggle between DYNAMIC, KEYFRAMED, and STATIC.
		///
		/// Common use: grab an object by switching it to KEYFRAMED,
		/// then switch back to DYNAMIC on release for throw physics.
		///
		/// @param a_motionPropertiesId  Preset: {0}=STATIC, {1}=DYNAMIC, {2}=KEYFRAMED
		///
		/// Example:
		///   // Grab: switch to keyframed
		///   world->SetBodyMotionProperties(bodyId, { 2 });  // KEYFRAMED
		///   // Release: switch back to dynamic for throw
		///   world->SetBodyMotionProperties(bodyId, { 1 });  // DYNAMIC
		void SetBodyMotionProperties(hknpBodyId a_bodyId, hknpMotionPropertiesId_Handle a_motionPropertiesId)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hknpMotionPropertiesId_Handle);
			static REL::Relocation<func_t> func{ REL::ID(275629) };
			func(this, a_bodyId, a_motionPropertiesId);
		}

		/// Assign a body to a different motion slot.
		/// Used when transferring bodies between motion groups (e.g., compound objects).
		///
		/// @param a_bodyId    Body to reassign
		/// @param a_motionId  New motion index
		void SetBodyMotion(hknpBodyId a_bodyId, std::uint32_t a_motionId)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, std::uint32_t);
			static REL::Relocation<func_t> func{ REL::ID(618186) };
			func(this, a_bodyId, a_motionId);
		}

		/// Get a body's axis-aligned bounding box.
		/// Output is 32 bytes: 16 bytes min (hkVector4f), 16 bytes max (hkVector4f).
		void GetBodyAabb(hknpBodyId a_bodyId, void* a_aabbOut)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, void*);
			static REL::Relocation<func_t> func{ REL::ID(249572) };
			func(this, a_bodyId, a_aabbOut);
		}

		/// Predict a body's future transform at a given time delta.
		/// Uses current or previous-step velocities depending on dt magnitude.
		/// Useful for rendering interpolation between physics steps.
		///
		/// @param a_bodyId       Body to predict
		/// @param a_deltaTime    Time delta for prediction (seconds)
		/// @param a_transformOut [out] Predicted transform (16 floats: 3x4 rotation + translation)
		void PredictBodyTransform(hknpBodyId a_bodyId, float a_deltaTime, float* a_transformOut)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, float, float*);
			static REL::Relocation<func_t> func{ REL::ID(823517) };
			func(this, a_bodyId, a_deltaTime, a_transformOut);
		}

		// =====================================================================
		// Velocity & Impulse
		//
		// For VR controller tracking, use ComputeHardKeyFrame + SetBodyVelocity
		// every frame instead of SetBodyTransform (avoids solver fighting).
		//
		// For throw mechanics, use ApplyBodyImpulseAt on release with the
		// hand's velocity (multiply by ~5.0 for satisfying throw feel).
		// =====================================================================

		/// Set a body's linear velocity directly.
		/// @param a_velocity  Linear velocity in Havok space (units/second)
		void SetBodyLinearVelocity(hknpBodyId a_bodyId, hkVector4f& a_velocity)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(1082668) };
			func(this, a_bodyId, a_velocity);
		}

		/// Set a body's angular velocity directly.
		/// Set to {0,0,0,0} to prevent gyroscope spinning while grabbed.
		/// @param a_velocity  Angular velocity in radians/second
		void SetBodyAngularVelocity(hknpBodyId a_bodyId, hkVector4f& a_velocity)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(1068862) };
			func(this, a_bodyId, a_velocity);
		}

		/// Set both linear and angular velocity in one call.
		/// Preferred over calling SetBodyLinearVelocity + SetBodyAngularVelocity separately.
		void SetBodyVelocity(hknpBodyId a_bodyId, hkVector4f& a_linearVelocity, hkVector4f& a_angularVelocity)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(868461) };
			func(this, a_bodyId, a_linearVelocity, a_angularVelocity);
		}

		/// Get a body's current angular velocity.
		/// @param a_velocityOut  [out] Angular velocity in radians/second
		void GetBodyAngularVelocity(hknpBodyId a_bodyId, hkVector4f& a_velocityOut)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(405369) };
			func(this, a_bodyId, a_velocityOut);
		}

		/// Apply a linear impulse to a body's center of mass.
		/// Immediately changes the body's velocity: dv = impulse * inverseMass.
		/// @param a_impulse  Impulse vector in Havok space
		void ApplyBodyLinearImpulse(hknpBodyId a_bodyId, hkVector4f& a_impulse)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(654985) };
			func(this, a_bodyId, a_impulse);
		}

		/// Apply an angular impulse (torque impulse) to a body.
		/// @param a_impulse  Angular impulse in Havok space (radians * mass)
		void ApplyBodyAngularImpulse(hknpBodyId a_bodyId, hkVector4f& a_impulse)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(1345477) };
			func(this, a_bodyId, a_impulse);
		}

		/// Apply an impulse at a specific world point (creates both linear + angular response).
		/// This is the key method for throw mechanics — apply hand velocity as impulse at
		/// the grab point for physically correct throwing.
		///
		/// @param a_impulse  Impulse vector in Havok space
		/// @param a_point    World-space application point in Havok space
		///
		/// Example (throw on grip release):
		///   hkVector4f throwImpulse = handVelocity * 5.0f;  // 5x multiplier feels good
		///   // Clamp: if (throwImpulse.Length() > 20.0f) throwImpulse = throwImpulse.GetNormalized() * 20.0f;
		///   world->ApplyBodyImpulseAt(bodyId, throwImpulse, grabPoint);
		void ApplyBodyImpulseAt(hknpBodyId a_bodyId, hkVector4f& a_impulse, hkVector4f& a_point)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, hkVector4f&, hkVector4f&);
			static REL::Relocation<func_t> func{ REL::ID(800106) };
			func(this, a_bodyId, a_impulse, a_point);
		}

		/// Compute velocity needed to move a keyframed body to a target pose.
		/// This is THE method for smooth VR controller tracking — call it every frame
		/// with the VR controller's target position/rotation, then feed the outputs
		/// into SetBodyVelocity.
		///
		/// @param a_bodyId       Keyframed body to track
		/// @param a_targetPos    Target position (float[4], Havok space)
		/// @param a_targetRot    Target rotation (float[4], quaternion xyzw)
		/// @param a_deltaTime    deltaTime — function internally computes 1/dt via RCPPS+Newton-Raphson
		/// @param a_linVelOut    [out] Computed linear velocity (float[4])
		/// @param a_angVelOut    [out] Computed angular velocity (float[4])
		///
		/// Example (VR hand tracking):
		///   float targetPos[4] = { handX * kHavokScale, handY * kHavokScale, handZ * kHavokScale, 0 };
		///   float targetRot[4] = { qx, qy, qz, qw };
		///   float linVel[4], angVel[4];
		///   world->ComputeHardKeyFrame(handBodyId, targetPos, targetRot, dt, linVel, angVel);
		///   hkVector4f lv(linVel[0], linVel[1], linVel[2]);
		///   hkVector4f av(angVel[0], angVel[1], angVel[2]);
		///   world->SetBodyVelocity(handBodyId, lv, av);
		void ComputeHardKeyFrame(hknpBodyId a_bodyId, float* a_targetPos, float* a_targetRot, float a_deltaTime, float* a_linVelOut, float* a_angVelOut)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, float*, float*, float, float*, float*);
			static REL::Relocation<func_t> func{ REL::ID(865502) };
			func(this, a_bodyId, a_targetPos, a_targetRot, a_deltaTime, a_linVelOut, a_angVelOut);
		}

		/// Full hard keyframe wrapper: lock -> ComputeHardKeyFrame -> SetBodyVelocity -> unlock.
		/// Convenience method that does the complete keyframe tracking in one call.
		/// Prefer this over manually calling ComputeHardKeyFrame + SetBodyVelocity when
		/// you don't need the intermediate velocity values.
		///
		/// @param a_bodyId    Keyframed body to track
		/// @param a_targetPos Target position (float[4], Havok space)
		/// @param a_targetRot Target rotation (float[4], quaternion xyzw)
		/// @param a_deltaTime Frame delta time
		void ApplyHardKeyFrame(hknpBodyId a_bodyId, float* a_targetPos, float* a_targetRot, float a_deltaTime)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, float*, float*, float);
			static REL::Relocation<func_t> func{ REL::Offset(0x153ABD0) };
			func(this, a_bodyId, a_targetPos, a_targetRot, a_deltaTime);
		}

		// =====================================================================
		// Activation
		// =====================================================================

		/// Wake up a sleeping body so it participates in simulation.
		/// Bodies automatically go to sleep when they stop moving.
		/// Call this before applying impulses to a potentially sleeping body.
		void ActivateBody(hknpBodyId a_bodyId)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId);
			static REL::Relocation<func_t> func{ REL::ID(904571) };
			func(this, a_bodyId);
		}

		// =====================================================================
		// Constraints
		//
		// Constraints connect two bodies (e.g., hinges, ball-sockets, 6-DOF).
		// Heisenberg uses 6-DOF constraints for grab mechanics as an alternative
		// to velocity-driven tracking.
		// =====================================================================

		/// Create a constraint between two bodies.
		/// @param a_outId  Output: receives the constraint ID (0xFFFFFFFF on failure)
		/// @param a_cinfo  Constraint creation info (type, bodies, limits, etc.)
		///
		/// NOTE: Ghidra confirms the actual ABI uses an output pointer for the ID,
		/// NOT a return value. Signature: uint* createConstraint(this, uint* outId, cinfo*)
		void CreateConstraint(std::uint32_t* a_outId, hknpConstraintCinfo& a_cinfo)
		{
			static REL::Relocation<std::uintptr_t> addr{ REL::ID(441087) };
			auto                                   fn = reinterpret_cast<std::uint32_t* (*)(hknpWorld*, std::uint32_t*, hknpConstraintCinfo*)>(addr.address());
			fn(this, a_outId, &a_cinfo);
		}

		/// Destroy one or more constraints, freeing their slots.
		void DestroyConstraints(std::uint32_t* a_constraintIds, std::int32_t a_count)
		{
			typedef void                   func_t(hknpWorld*, std::uint32_t*, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(334340) };
			func(this, a_constraintIds, a_count);
		}

		/// Temporarily disable a constraint (bodies behave as if unconnected).
		void DisableConstraint(std::uint32_t a_constraintId)
		{
			typedef void                   func_t(hknpWorld*, std::uint32_t);
			static REL::Relocation<func_t> func{ REL::ID(320495) };
			func(this, a_constraintId);
		}

		/// Re-enable a previously disabled constraint.
		void EnableConstraint(std::uint32_t a_constraintId, std::int32_t a_activationMode = 0)
		{
			typedef void                   func_t(hknpWorld*, std::uint32_t, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(399921) };
			func(this, a_constraintId, a_activationMode);
		}

		// =====================================================================
		// Events
		//
		// Subscribe to physics events via signals. The event dispatcher lives at
		// world+0x648. Use hkSignal2::subscribe to register callbacks:
		//   - GlobalSlot: REL::ID(277607)
		//   - MemberSlot: REL::ID(427291)
		//
		// Contact modifier flag: 0x20000 — set via hknpModifierManager::addModifier
		// REL::ID(1455691) to enable contact callbacks for specific bodies.
		// =====================================================================

		/// Get the event signal for a given event type (world-wide).
		/// @param a_eventType  One of hknpEventType::Enum values
		/// @return Signal pointer to subscribe to (use hkSignal2::subscribe)
		void* GetEventSignal(hknpEventType::Enum a_eventType)
		{
			typedef void*                  func_t(hknpWorld*, std::int16_t);
			static REL::Relocation<func_t> func{ REL::ID(1029853) };
			return func(this, static_cast<std::int16_t>(a_eventType));
		}

		/// Get the event signal for a specific body (per-body filtering).
		/// Only fires for events involving the specified body.
		void* GetEventSignalForBody(hknpEventType::Enum a_eventType, hknpBodyId a_bodyId)
		{
			typedef void*                  func_t(hknpWorld*, std::int32_t, std::uint32_t);
			static REL::Relocation<func_t> func{ REL::ID(497909) };
			return func(this, static_cast<std::int32_t>(a_eventType), a_bodyId.value);
		}

		// =====================================================================
		// Simulation
		// =====================================================================

		/// Run collision detection for one step.
		/// @param a_input      Step parameters (time, gravity, etc.)
		/// @param a_taskQueue  Thread pool task queue (nullptr for single-threaded)
		/// @param a_solverData [in/out] Solver data from previous step
		void StepCollide(hknpStepInput& a_input, void* a_taskQueue, void*& a_solverData)
		{
			typedef void                   func_t(hknpWorld*, hknpStepInput&, void*, void*&);
			static REL::Relocation<func_t> func{ REL::ID(687604) };
			func(this, a_input, a_taskQueue, a_solverData);
		}

		/// Shift the broadphase origin (used during cell transitions to prevent
		/// floating point precision issues far from origin).
		void ShiftBroadPhase(hkVector4f& a_offset, hkVector4f& a_offset2, void* a_bodyIdArray)
		{
			typedef void                   func_t(hknpWorld*, hkVector4f&, hkVector4f&, void*);
			static REL::Relocation<func_t> func{ REL::ID(218299) };
			func(this, a_offset, a_offset2, a_bodyIdArray);
		}

		// =====================================================================
		// Body Flags & Collision Filter
		//
		// Enable/disable body flags (e.g., keep-awake flag 0x8000000 for grab).
		// Change collision filter info to switch layers at runtime (e.g., switch
		// grabbed objects to BIPED_NO_CC layer to avoid character controller bumping).
		// =====================================================================

		/// Enable flag bits on a body.
		/// The most common flag is 0x8000000 (keep-awake), used during grab to prevent
		/// the body from going to sleep while held.
		///
		/// @param a_bodyId            Target body
		/// @param a_flags             Flag bits to enable (OR'd into body flags at body+0x40)
		/// @param a_rebuildCachesMode 0 = don't rebuild, 1 = rebuild collision caches
		void EnableBodyFlags(hknpBodyId a_bodyId, std::uint32_t a_flags, std::int32_t a_rebuildCachesMode = 1)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, std::uint32_t, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(987833) };
			func(this, a_bodyId, a_flags, a_rebuildCachesMode);
		}

		/// Disable (clear) flag bits on a body.
		///
		/// @param a_bodyId            Target body
		/// @param a_flags             Flag bits to clear
		/// @param a_rebuildCachesMode 0 = don't rebuild, 1 = rebuild collision caches
		void DisableBodyFlags(hknpBodyId a_bodyId, std::uint32_t a_flags, std::int32_t a_rebuildCachesMode = 1)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, std::uint32_t, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(1506647) };
			func(this, a_bodyId, a_flags, a_rebuildCachesMode);
		}

		/// Change a body's collision filter info at runtime.
		/// This changes which layers the body collides with.
		///
		/// Common use: switch grabbed objects from CLUTTER (4) to BIPED_NO_CC (33)
		/// to prevent the character controller from bumping them during grab.
		///
		/// @param a_bodyId            Target body
		/// @param a_filterInfo        New filter info (bits 0-6 = layer, bits 7+ = group)
		/// @param a_rebuildCachesMode 0 = don't rebuild, 1 = rebuild collision caches
		///
		/// Example (grab with layer change):
		///   uint32_t origFilter = collObj->GetCollisionFilterInfo();
		///   uint32_t newFilter = (origFilter & ~0x7F) | static_cast<uint32_t>(COL_LAYER::kBipedNoCC);
		///   world->SetBodyCollisionFilterInfo(bodyId, newFilter);
		///   // On release: world->SetBodyCollisionFilterInfo(bodyId, origFilter);
		void SetBodyCollisionFilterInfo(hknpBodyId a_bodyId, std::uint32_t a_filterInfo, std::int32_t a_rebuildCachesMode = 1)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId, std::uint32_t, std::int32_t);
			static REL::Relocation<func_t> func{ REL::Offset(0x153AF00) };
			func(this, a_bodyId, a_filterInfo, a_rebuildCachesMode);
		}

		/// Rebuild collision caches for a body after changing its properties.
		/// Call after SetBodyCollisionFilterInfo if rebuildCachesMode was 0.
		void RebuildBodyCollisionCaches(hknpBodyId a_bodyId)
		{
			typedef void                   func_t(hknpWorld*, hknpBodyId);
			static REL::Relocation<func_t> func{ REL::ID(135033) };
			func(this, a_bodyId);
		}

		// =====================================================================
		// Direct Memory Access Helpers
		//
		// These provide type-safe access to the world's internal arrays.
		// Prefer these over raw pointer arithmetic.
		// =====================================================================

		// =====================================================================
		// Material Library Access
		//
		// The material library lives at world+0x5C8. It's a hkFreeListArray
		// with stride 0x50 per material. Materials are indexed by materialId
		// (uint16, stored at body+0x70).
		//
		// Key material field offsets (within 0x50 byte struct):
		//   +0x10 = triggerType (uint8, 0=normal)
		//   +0x11 = dynamicFriction (quantized byte 0-255)
		//   +0x12 = staticFriction (hkHalf16)
		//   +0x28 = restitution (hkHalf16)
		//   +0x0C = flags (auto-computed, call updateFlags after changes)
		// =====================================================================

		/// Get the material library pointer.
		/// The material array base is at *(matLib + 0x28).
		void* GetMaterialLibrary() const
		{
			return *reinterpret_cast<void* const*>(
				reinterpret_cast<std::uintptr_t>(this) + 0x5C8);
		}

		/// Get a raw pointer to a material by ID.
		/// Returns pointer to an 0x50-byte hknpMaterial struct.
		/// No bounds checking — caller must ensure materialId is valid.
		void* GetMaterial(std::uint16_t a_materialId) const
		{
			auto* matLib = GetMaterialLibrary();
			if (!matLib)
				return nullptr;
			auto arrayBase = *reinterpret_cast<std::uintptr_t*>(
				reinterpret_cast<std::uintptr_t>(matLib) + 0x28);
			return reinterpret_cast<void*>(arrayBase + a_materialId * 0x50);
		}

		/// Get the material count (number of allocated material slots).
		std::int32_t GetMaterialCount() const
		{
			auto* matLib = GetMaterialLibrary();
			if (!matLib)
				return 0;
			return *reinterpret_cast<std::int32_t*>(
				reinterpret_cast<std::uintptr_t>(matLib) + 0x30);
		}

		/// Get the body array pointer (indexed by hknpBodyId::value).
		/// Each body is 0x90 bytes. See hknpBody for the struct layout.
		hknpBody* GetBodyArray() const
		{
			return *reinterpret_cast<hknpBody* const*>(
				reinterpret_cast<std::uintptr_t>(this) + 0x20);
		}

		/// Get a specific body by ID.
		/// No bounds checking — caller must ensure bodyId is valid.
		hknpBody& GetBody(hknpBodyId a_bodyId) const
		{
			return GetBodyArray()[a_bodyId.value];
		}

		/// Get the raw motion array pointer.
		/// Index with body.motionIndex * sizeof(hknpMotion).
		/// Motion index 0 is the shared static motion.
		hknpMotion* GetMotionArray() const
		{
			return *reinterpret_cast<hknpMotion* const*>(
				reinterpret_cast<std::uintptr_t>(this) + 0xE0);
		}

		/// Get the motion for a specific body.
		/// Returns nullptr if the body has an invalid motionIndex.
		hknpMotion* GetBodyMotion(hknpBodyId a_bodyId) const
		{
			auto& body = GetBody(a_bodyId);
			if (body.motionIndex > 4096) {  // bounds check — garbage motionIndex seen in practice
				return nullptr;
			}
			return &GetMotionArray()[body.motionIndex];
		}

		/// Get the world-space position of a body (from its motion).
		/// Returns the position in Havok space. Multiply by 70.0 for game units.
		/// Returns zero vector if the body has an invalid motion.
		hkVector4f GetBodyPosition(hknpBodyId a_bodyId) const
		{
			auto* motion = GetBodyMotion(a_bodyId);
			if (!motion) {
				return hkVector4f(0, 0, 0, 0);
			}
			return motion->position;
		}
	};
}
