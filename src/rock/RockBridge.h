#pragma once

// =============================================================================
// RockBridge — Heisenberg's client of brunocatani/ROCK (physics/collision/grab
// provider). See project_rock_integration_plan / ROCK_forceGrab_API_Request.md.
//
// ROCK is OPTIONAL. When ROCK.dll is loaded and config useRockPhysics is not
// forced off, IsActive() is true and Heisenberg delegates hand collision + grab
// to ROCK (consuming this bridge). When ROCK is absent, IsActive() is false and
// Heisenberg uses its own built-in systems unchanged (fallback). Nothing in
// Heisenberg's built-in path is removed — every delegation is gated on IsActive().
// =============================================================================

#include "rock/ROCKApi.h"

#include <cstdint>

namespace RE { class TESObjectREFR; }

namespace heisenberg
{
    class RockBridge
    {
    public:
        static RockBridge& GetSingleton();

        // Bind to ROCK's consumer API if ROCK.dll is loaded. Idempotent — safe to
        // call from kPostLoad and/or lazily. No effect if already bound.
        void Init();

        // ROCK.dll present AND consumer API bound (>= v4).
        bool IsPresent() const { return _present; }

        // The single switch the rest of Heisenberg checks before short-circuiting
        // its built-in physics/grab: ROCK present AND config not forced off.
        bool IsActive() const;

        // ROCK is present, enabled (iUseRockPhysics != 0), AND actively running its physics
        // interaction — i.e. NOT disabled at runtime / torn down. Use this for AUTO ownership
        // switching (hand collision, grab): when ROCK is turned off, IsRunning() goes false
        // and Heisenberg reclaims those automatically, no INI change. IsActive() only checks
        // presence + the toggle, so it stays true even while ROCK is runtime-disabled.
        bool IsRunning() const;

        // forceGrab/canForceGrab available (live ROCK reports getVersion() >= 5).
        bool HasForceGrab() const { return _hasForceGrab; }
        std::uint32_t Version() const { return _version; }
        const rock::api::ROCKApi* Api() const { return _api; }

        static rock::api::ROCKApi::Hand HandOf(bool isLeft)
        {
            return isLeft ? rock::api::ROCKApi::Hand::Left : rock::api::ROCKApi::Hand::Right;
        }

        // ---- thin typed wrappers (all return safe defaults when !IsActive) ----
        bool IsPhysicsReady() const;
        bool IsHandHolding(bool isLeft) const;
        RE::TESObjectREFR* GetHeldObject(bool isLeft) const;
        RE::TESObjectREFR* GetSelectedObject(bool isLeft) const;
        bool IsHandTouching(bool isLeft) const;
        RE::TESObjectREFR* GetLastTouchedObject(bool isLeft) const;
        std::uint32_t GetLastTouchedLayer(bool isLeft) const;

        bool ClaimObject(RE::TESObjectREFR* refr) const;
        bool ReleaseObject(RE::TESObjectREFR* refr) const;
        bool IsObjectClaimed(RE::TESObjectREFR* refr) const;

        void DisablePhysicsHand(bool isLeft) const;
        void EnablePhysicsHand(bool isLeft) const;
        void ForceDrop(bool isLeft) const;

        // True only if ROCK committed the grab (requires HasForceGrab). Caller
        // must check the return and fall back when false.
        bool ForceGrab(bool isLeft, RE::TESObjectREFR* refr) const;

        // ---- PREPARED for the future "ROCK owns the dynamic hold" model ----
        // (GrabMode::RockForceGrab). Not wired into the live grab path yet.
        //
        // True when ROCK can hold a Heisenberg-chosen object via its dynamic grab API
        // (needs forceGrab; ROCK < v5 returns false). The RockForceGrab grab mode is
        // gated on this so it safely degrades to keyframed until ROCK ships forceGrab.
        //
        // The integration primitive is the existing ForceGrab(isLeft, refr): Heisenberg
        // SELECTS, then hands the object to ROCK, and ROCK does the PLACEMENT (mesh/contact
        // grab pockets) + dynamic hold. We deliberately do NOT pass a Heisenberg offset —
        // ROCK seats the object at the real contact point, which supersedes our keyframed
        // dimension-table offset. Plain v5 forceGrab(hand, refr) is therefore sufficient.
        bool SupportsDynamicHold() const { return _hasForceGrab; }

        // F4SE message from ROCK (sender "ROCK"). Phase 0: log; Phase 2: dispatch
        // touch/grab/release into Heisenberg feature logic.
        void OnRockMessage(std::uint32_t type, const void* data, std::uint32_t dataLen);

    private:
        const rock::api::ROCKApi* _api = nullptr;
        bool          _present = false;
        bool          _hasForceGrab = false;
        bool          _initTried = false;
        std::uint32_t _version = 0;
    };
}
