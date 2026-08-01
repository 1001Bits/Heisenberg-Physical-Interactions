#pragma once

// bhkPhysicsSystem — Bethesda's physics body group manager
//
// A bhkPhysicsSystem groups related physics bodies together (e.g., a door =
// hinge body + frame body + constraint). Bethesda uses this for lifecycle
// management: creating instances in the world, tracking body IDs, handling
// world transitions, and managing constraints between grouped bodies.
//
// Relationship hierarchy:
//   TESObjectREFR → NiAVObject → bhkNPCollisionObject → bhkPhysicsSystem
//   bhkPhysicsSystem → hknpPhysicsSystemData (serialized template)
//   bhkPhysicsSystem → instance (runtime bodies in a specific hknpWorld)
//
// The system has two layers:
//   1. Data (at +0x10): serialized template with body shapes, materials,
//      constraint descriptors. Loaded from .nif files.
//   2. Instance (at +0x18): runtime state with actual body IDs in the
//      current hknpWorld. Created by CreateInstance(), destroyed on
//      cell transition.
//
// For modding, you mainly use this to:
//   - Query which bodies belong to an object (GetBodyId, ContainsBodyId)
//   - Add/remove grouped bodies from the world (AddToWorld)
//   - Handle cell transitions (ChangeWorld recreates the instance)
//
// RE'd from bhkPhysicsSystem methods at 0x141e0c320..0x141e0c740
// and hknpPhysicsSystem__addToWorld at 0x141565770 (Fallout4VR.exe).
//
// Usage:
//   // Get the physics system from a collision object
//   auto* colObj = static_cast<RE::bhkNPCollisionObject*>(niObject->GetCollisionObject());
//   auto* physSystem = colObj->spSystem.get();
//
//   // Check if it's active in the current world
//   if (physSystem->HasInstanceInWorld(bhkWorld)) {
//       // Get body IDs
//       RE::hknpBodyId bodyId;
//       physSystem->GetBodyId(&bodyId, 0);  // first body in the system
//   }
//
//   // Query if a specific body belongs to this system
//   if (physSystem->ContainsBodyId(someBodyId)) {
//       // This body is part of this physics system
//   }

#include "RE/Havok/hknpBodyId.h"

namespace RE
{
	class bhkWorld;
	class hkTransformf;
	class hknpWorld;

	/// Internal instance data created when a bhkPhysicsSystem is added to a world.
	/// Lives at bhkPhysicsSystem+0x18 when active.
	///
	/// This is a ref-counted object with body ID tracking.
	struct hknpPhysicsSystemInstance
	{
	public:
		void*         vtable;            // 00 — vtable pointer
		std::uint32_t refCountAndFlags;  // 08 — ref count in lower 16 bits
		std::uint16_t pad0C;             // 0C
		std::uint16_t pad0E;             // 0E
		std::uint64_t pad10;             // 10

		/// Pointer to the hknpWorld this instance is active in.
		/// Compared with bhkWorld+0x60 to check if instance is in a specific world.
		hknpWorld* world;  // 18

		/// Array of body IDs in this system (uint32_t each).
		/// Indexed by body index within the system (0-based).
		/// 0x7FFFFFFF entries mean "invalid/not created".
		std::uint32_t* bodyIds;  // 20

		/// Number of bodies in this system.
		std::int32_t bodyCount;  // 28

		std::uint32_t pad2C;  // 2C
	};
	static_assert(sizeof(hknpPhysicsSystemInstance) == 0x30);

	/// Serialized constraint descriptor within hknpPhysicsSystemData.
	/// Stride: 0x18 bytes per constraint.
	struct hknpPhysicsSystemConstraintDesc
	{
	public:
		void*        constraintData;  // 00 — hkpConstraintData* (polymorphic, ref-counted)
		std::int32_t bodyIndexA;      // 08 — index into body array (NOT body ID)
		std::int32_t bodyIndexB;      // 0C — index into body array
		std::uint8_t flags;           // 10 — constraint flags (bit 0: activation mode)
		std::uint8_t pad11[7];        // 11
	};
	static_assert(sizeof(hknpPhysicsSystemConstraintDesc) == 0x18);

	/// Serialized physics system data (loaded from .nif).
	/// Lives at bhkPhysicsSystem+0x10.
	///
	/// Contains the template for body shapes, materials, and constraints.
	/// Used to create runtime instances via CreateInstance().
	struct hknpPhysicsSystemData
	{
	public:
		void*         vtable;  // 00
		std::uint64_t pad08;   // 08

		/// Material creation info array (stride 0x50 per material).
		/// Used by GetMaterialCinfo to look up physics materials by index.
		void* materialCinfos;  // 10

		std::uint8_t pad18[0x38];  // 18

		/// Constraint descriptor array (stride 0x18).
		/// Each entry has a constraintData pointer and two body indices.
		hknpPhysicsSystemConstraintDesc* constraintDescs;  // 50

		/// Number of constraint descriptors.
		std::int32_t constraintCount;  // 58

		std::uint32_t pad5C;  // 5C
	};
	// Note: full size not confirmed — don't add static_assert

	/// bhkPhysicsSystem — Bethesda wrapper that groups physics bodies and constraints.
	///
	/// Owned by bhkNPCollisionObject (via NiPointer at +0x20).
	/// Manages the lifecycle of grouped physics bodies within a cell's physics world.
	///
	/// Key lifecycle:
	///   1. Loaded from .nif → data pointer (at +0x10) populated
	///   2. Cell loads → CreateInstance() creates runtime bodies in hknpWorld
	///   3. Cell unloads → instance destroyed, bodies removed
	///   4. Cell transition → ChangeWorld() recreates instance in new world
	class bhkPhysicsSystem
	{
	public:
		void* vtable;  // 00

		std::uint32_t refCountAndFlags;  // 08 — ref count in lower 16 bits
		std::uint16_t pad0C;             // 0C
		std::uint16_t pad0E;             // 0E

		/// Serialized physics system data (shapes, materials, constraint descriptors).
		/// Loaded from the .nif file. Persists across cell transitions.
		hknpPhysicsSystemData* data;  // 10

		/// Runtime instance with live body IDs in the current hknpWorld.
		/// nullptr when not instantiated (between cell transitions).
		/// Created by CreateInstance(), destroyed by ChangeWorld/destructor.
		hknpPhysicsSystemInstance* instance;  // 18

		/// Activation flag passed to hknpPhysicsSystem::addToWorld.
		/// Controls whether bodies start active or sleeping.
		std::uint8_t activationFlag;  // 20

		std::uint8_t pad21[7];  // 21

		// =====================================================================
		// Methods
		// =====================================================================

		/// Create a runtime instance of this physics system in the given world.
		/// Allocates bodies and constraints from the serialized data template.
		///
		/// @param a_world     The bhkWorld to create bodies in
		/// @param a_transform Initial world transform for the system
		/// @return True if instance was created successfully
		///
		/// Only works if: data != nullptr (have template) AND instance == nullptr (not already active).
		/// If instance already exists in a different world, calls ChangeWorld internally.
		bool CreateInstance(bhkWorld& a_world, hkTransformf& a_transform)
		{
			using func_t = bool (*)(bhkPhysicsSystem*, bhkWorld&, hkTransformf&);
			static REL::Relocation<func_t> func{ REL::ID(1245299) };
			return func(this, a_world, a_transform);
		}

		/// Check if this system has an active instance in the given world.
		///
		/// @param a_world  The bhkWorld to check against
		/// @return True if instance exists and is in this world
		///
		/// Internally compares instance->world with bhkWorld+0x60 (the hknpWorld pointer).
		bool HasInstanceInWorld(bhkWorld& a_world)
		{
			using func_t = bool (*)(bhkPhysicsSystem*, bhkWorld&);
			static REL::Relocation<func_t> func{ REL::ID(792872) };
			return func(this, a_world);
		}

		/// Get the hknpBodyId of a body in this system by index.
		///
		/// @param a_outBodyId  [out] Receives the body ID (0x7FFFFFFF if invalid)
		/// @param a_index      Body index within the system (0-based)
		///
		/// The body index corresponds to the order bodies appear in the .nif data.
		/// For example, a door might have index 0 = door panel, index 1 = frame.
		void GetBodyId(hknpBodyId* a_outBodyId, std::int32_t a_index)
		{
			using func_t = void (*)(bhkPhysicsSystem*, hknpBodyId*, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(526734) };
			func(this, a_outBodyId, a_index);
		}

		/// Check if a specific body ID belongs to this physics system.
		///
		/// @param a_bodyId  Body ID to search for
		/// @return True if the body is part of this system
		///
		/// Iterates the instance's body ID array (linear scan).
		/// Returns false if no instance exists.
		bool ContainsBodyId(hknpBodyId a_bodyId)
		{
			using func_t = bool (*)(bhkPhysicsSystem*, hknpBodyId);
			static REL::Relocation<func_t> func{ REL::ID(260599) };
			return func(this, a_bodyId);
		}

		/// Add this system's bodies to the physics world simulation.
		/// Only works if an instance exists (CreateInstance was called).
		///
		/// Internally calls hknpPhysicsSystem::addToWorld which validates each
		/// body ID, checks if it's already active, and adds inactive bodies.
		///
		/// @return True if bodies were added
		bool AddToWorld()
		{
			using func_t = bool (*)(bhkPhysicsSystem*);
			static REL::Relocation<func_t> func{ REL::ID(512878) };
			return func(this);
		}

		/// Transition this system to a different physics world.
		/// Removes bodies from the old world, destroys the old instance,
		/// and creates a new instance in the target world.
		///
		/// @param a_newWorld   Target bhkWorld
		/// @param a_transform Transform for the new instance
		/// @return True if world change succeeded
		bool ChangeWorld(bhkWorld& a_newWorld, hkTransformf& a_transform)
		{
			using func_t = bool (*)(bhkPhysicsSystem*, bhkWorld&, hkTransformf&);
			static REL::Relocation<func_t> func{ REL::ID(153808) };
			return func(this, a_newWorld, a_transform);
		}

		/// Get the material creation info for a body at the given index.
		/// Returns a pointer into the data's material array (stride 0x50).
		///
		/// @param a_index  Material index
		/// @return Pointer to material cinfo data
		void* GetMaterialCinfo(std::int32_t a_index)
		{
			using func_t = void* (*)(bhkPhysicsSystem*, std::int32_t);
			static REL::Relocation<func_t> func{ REL::ID(139822) };
			return func(this, a_index);
		}
	};
	// Note: total size not fully confirmed — pad fields may extend further
}
