#pragma once

// Virtual Holsters API integration
// API docs: https://github.com/CylonSurfer/VirtualHolsters
// Updated to match upstream VirtualHolstersAPI.h (Feb 2026)

#include <cstdint>
#include <windows.h>

#ifndef VH_CALL
#define VH_CALL __cdecl
#endif

namespace VirtualHolsters
{
	/**
	 * VirtualHolsters API interface
	 *
	 * Provides access to holster zone detection, weapon storage state,
	 * and direct holster mutation (AddHolster).
	 *
	 * Holster indices:
	 *   0 = None (not in holster)
	 *   1 = Left Shoulder
	 *   2 = Right Shoulder
	 *   3 = Left Hip
	 *   4 = Right Hip
	 *   5 = Lower Back
	 *   6 = Left Chest
	 *   7 = Right Chest
	 */
	class IVirtualHolstersAPI
	{
	public:
		// NOTE: No virtual destructor! Must match upstream VirtualHolstersAPI vtable layout exactly.
		// Adding virtual ~IVirtualHolstersAPI() shifts all vtable slots by 1, breaking every call.

		// Returns the API version (1 = original, 2+ = with AddHolster)
		virtual std::uint32_t VH_CALL GetVersion() const = 0;

		// Returns true if the specified hand is currently within a holster zone
		virtual bool VH_CALL IsHandInHolsterZone(bool isLeft) const = 0;

		// Returns the current/most-recent holster index that a hand entered (0-7)
		virtual std::uint32_t VH_CALL GetCurrentHolster() const = 0;

		// Returns true if the specified holster slot is empty (no weapon stored)
		virtual bool VH_CALL IsHolsterFree(std::uint32_t holsterIndex) const = 0;

		// Returns the name of the weapon stored in a holster slot, or "Empty"
		virtual const char* VH_CALL GetHolsteredWeaponName(std::uint32_t holsterIndex) const = 0;

		// Check if a weapon (by name) is already stored in any holster
		virtual bool VH_CALL IsWeaponAlreadyHolstered(const char* weaponName) const = 0;

		// Get holster center position in world space
		virtual bool VH_CALL GetHolsterPosition(std::uint32_t holsterIndex,
			float& outX, float& outY, float& outZ) const = 0;

		// Get holster sphere radius
		virtual float VH_CALL GetHolsterRadius(std::uint32_t holsterIndex) const = 0;

		// Returns true if VirtualHolsters is fully initialized
		virtual bool VH_CALL IsInitialized() const = 0;

		// Returns true if grip button is assigned as the holster button
		virtual bool VH_CALL IsGripAssignedToHolster() const = 0;

		// Get the current holster button ID (OpenVR EVRButtonId)
		virtual std::uint32_t VH_CALL GetHolsterButtonId() const = 0;

		// Check if Virtual Holsters is in left-handed mode
		virtual bool VH_CALL IsLeftHandedMode() const = 0;

		// Switches to the opposite hand for holster interaction / detection
		virtual void VH_CALL switchHandMode() const = 0;

		// Add a grabbed weapon directly to a holster slot.
		// VH internally resolves weapon name, base form, and melee status from the ref.
		// @param holsterIndex  Holster index (1-7)
		// @param weaponRef     TESObjectREFR* of the grabbed weapon instance
		// @return true if weapon was successfully added to holster
		virtual bool VH_CALL AddHolster(std::uint32_t holsterIndex, void* weaponRef) = 0;
	};

	using _VHAPI_GetApi = IVirtualHolstersAPI* (*)();

	// Request the VirtualHolsters API interface
	// Returns nullptr if VirtualHolsters is not loaded
	inline IVirtualHolstersAPI* RequestVirtualHolstersAPI()
	{
		HMODULE hModule = GetModuleHandleA("VirtualHolsters.dll");
		if (!hModule) {
			return nullptr;
		}

		auto getApi = reinterpret_cast<_VHAPI_GetApi>(GetProcAddress(hModule, "VHAPI_GetApi"));
		if (!getApi) {
			return nullptr;
		}

		return getApi();
	}
}  // namespace VirtualHolsters
