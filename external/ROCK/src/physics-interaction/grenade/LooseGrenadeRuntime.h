#pragma once

#include "RE/Bethesda/BSTSmartPointer.h"
#include "RE/Bethesda/BSPointerHandle.h"

#include <cstdint>

namespace RE
{
    class Actor;
    class BGSExplosion;
    class BGSObjectInstance;
    class BGSProjectile;
    class TESObjectREFR;
    class TESObjectWEAP;
    class TBO_InstanceData;
    class NiPoint3;
}

namespace rock::loose_grenade_runtime
{
    enum class GrenadeDetonationMode : std::uint8_t
    {
        TimedFuse,
        Impact
    };

    enum class GrenadeKind : std::uint8_t
    {
        NotGrenade,
        Generic,
        Molotov
    };

    struct GrenadeRuntimeData
    {
        RE::BGSProjectile* projectile{ nullptr };
        RE::BGSExplosion* explosion{ nullptr };
        float fuseSeconds{ 0.0f };
        GrenadeDetonationMode detonationMode{ GrenadeDetonationMode::TimedFuse };
    };

    struct PendingEquipRequest
    {
        bool active{ false };
        std::uint64_t requestId{ 0 };
        RE::TESObjectWEAP* weapon{ nullptr };
        RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData{};
        std::uint32_t stackId{ 0 };
        GrenadeRuntimeData runtime{};
    };

    struct DropResult
    {
        bool success{ false };
        const char* reason{ "not-attempted" };
        RE::ObjectRefHandle handle{};
        RE::TESObjectREFR* droppedRef{ nullptr };
        std::uint32_t stackId{ 0 };
    };

    /*
     * ── EQUIP-OBJECT ARBITRATION (2026-07-28) ────────────────────────────────
     * What ROCK does with a call to ActorEquipManager::EquipObject (0x0E6FEA0).
     * ConsumedEquipped/ConsumedBlocked BOTH mean "the caller must not run the
     * native function"; they differ only in the bool the detour returns to the
     * game, which is the exact distinction the previous in-ROCK detour made:
     *   - ConsumedEquipped -> the request was queued for the grenade transaction
     *     (detour returns true, i.e. "equip handled").
     *   - ConsumedBlocked  -> it IS a grenade but its projectile/explosion/fuse
     *     data would not resolve, so native equip is refused rather than run
     *     half-configured (detour returns false).
     */
    enum class EquipInterceptionResult : std::uint8_t
    {
        NotIntercepted,
        ConsumedEquipped,
        ConsumedBlocked,
    };

    /*
     * ROCK's loose-grenade equip interception, as a plain callable.
     *
     * ROCK DOES NOT PATCH 0x0E6FEA0. Only one raw entry detour can exist at a
     * function entry, and in this build Heisenberg's own EquipObject detour
     * (src/Hooks.cpp InstallEquipObjectHook / HookEquipObject) owns it and is
     * installed first. ROCK previously installed a second detour over the top,
     * memcmp'd the prologue Heisenberg had already replaced, failed validation,
     * and latched itself off -- which is why grenade interception had never once
     * run. The host detour now CALLS this function instead.
     *
     * `object` may be null (the caller's parameter is a pointer); a null object is
     * NotIntercepted. Safe to call before/without engine init.
     */
    [[nodiscard]] EquipInterceptionResult tryInterceptEquipObject(
        RE::Actor* actor,
        const RE::BGSObjectInstance* object,
        std::uint32_t stackId,
        std::uint32_t number);

    /*
     * The host reports whether ITS EquipObject detour is live. This is the only
     * thing standing between a menu equip and tryInterceptEquipObject, so it is
     * also the gate on the whole pending-equip queue: with no detour, nothing can
     * ever feed the queue and every consumer must take its native path.
     * `reasonWhenAbsent` is logged verbatim when installed == false; pass the
     * concrete reason (e.g. which config flags left the hook uninstalled).
     */
    void setHostEquipObjectDetourInstalled(bool installed, const char* reasonWhenAbsent);
    [[nodiscard]] bool isHostEquipObjectDetourInstalled();

    /*
     * Registers the seam above with the engine and reports what is actually
     * sitting at 0x0E6FEA0. Patches NOTHING. Returns false only when grenade
     * interception genuinely cannot run this session (no host detour).
     */
    [[nodiscard]] bool installEquipHook();

    /*
     * False when the interception feature is latched OFF for the session (no host
     * detour to call it): every grenade path must then fall back to native
     * behaviour. Never becomes true again without a relaunch (F4SE DLLs only load
     * at process start, so a retry has nothing new to find).
     */
    [[nodiscard]] bool isEquipInterceptionActive();

    /*
     * Re-entry guard, host-driven. The host detour must bracket its call to the
     * ORIGINAL EquipObject with these, so that an equip the game issues from
     * inside that call is not itself intercepted. This preserves the exact
     * semantics of the reentry guard the in-ROCK detour used to hold across its
     * own pass-through call. Thread-local; every begin needs its end (use an RAII
     * wrapper on the host side).
     */
    void beginHostEquipPassThrough() noexcept;
    void endHostEquipPassThrough() noexcept;

    [[nodiscard]] bool isGrenadeWeapon(const RE::TESObjectWEAP* weapon) noexcept;
    [[nodiscard]] bool isGrenadeRef(RE::TESObjectREFR* ref) noexcept;
    [[nodiscard]] GrenadeKind classifyGrenadeRef(RE::TESObjectREFR* ref) noexcept;
    [[nodiscard]] bool resolveGrenadeRuntimeData(
        RE::TESObjectWEAP* weapon,
        RE::TBO_InstanceData* instanceData,
        GrenadeRuntimeData& outRuntime) noexcept;
    [[nodiscard]] bool resolveGrenadeRuntimeDataForReference(
        RE::TESObjectREFR* ref,
        GrenadeRuntimeData& outRuntime) noexcept;

    [[nodiscard]] bool copyPendingEquipRequest(PendingEquipRequest& outRequest);
    [[nodiscard]] bool hasPendingEquipRequest();
    void discardPendingEquipRequest(std::uint64_t requestId);
    void clearPendingEquipRequest();

    [[nodiscard]] DropResult dropPendingEquipRequestToWorld(
        const PendingEquipRequest& request,
        const RE::NiPoint3& dropLocation);

    [[nodiscard]] bool createExplosionAtReference(RE::TESObjectREFR* ref, RE::BGSExplosion* explosion);
    [[nodiscard]] const char* detonationModeName(GrenadeDetonationMode mode) noexcept;
    [[nodiscard]] bool playPinPulledFeedbackAtReference(RE::TESObjectREFR* ref);
    [[nodiscard]] bool returnDroppedReferenceToInventory(RE::TESObjectREFR* ref);
    void disableAndDeleteReference(RE::TESObjectREFR* ref);
}
