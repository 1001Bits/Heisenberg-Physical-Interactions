#include "rock/RockBridge.h"

#include "Config.h"
#include "ThrownObjectTracker.h"

#include <spdlog/spdlog.h>

namespace heisenberg
{
    RockBridge& RockBridge::GetSingleton()
    {
        static RockBridge instance;
        return instance;
    }

    void RockBridge::Init()
    {
        if (_initTried) {
            return;
        }
        _initTried = true;

        // Bind requiring only v4 (the baseline consumer API). forceGrab is a v5
        // addition we gate separately, so we must NOT require v5 here or binding
        // would fail against current ROCK releases.
        const int rc = rock::api::ROCKApi::initialize(/*minVersion*/ 4);
        if (rc != 0) {
            // rc: 1 = ROCK.dll not loaded, 2 = no export, 3 = null api, 4 = version too low.
            _present = false;
            _api = nullptr;
            if (rc == 1) {
                spdlog::info("[ROCK] ROCK.dll not present — Heisenberg using built-in physics/grab (fallback).");
            } else {
                spdlog::warn("[ROCK] ROCK present but API bind failed (code {}) — using built-in fallback.", rc);
            }
            return;
        }

        _api = rock::api::ROCKApi::inst;
        if (!_api) {
            _present = false;
            spdlog::warn("[ROCK] ROCKApi::inst null after initialize — using built-in fallback.");
            return;
        }

        _present = true;
        _version = _api->getVersion ? _api->getVersion() : 0;
        // forceGrab is the last member; only safe to call when the live ROCK is
        // actually v5+ (older ROCK's struct doesn't contain the pointer).
        _hasForceGrab = (_version >= 5) && (_api->forceGrab != nullptr);

        const char* modVer = (_api->getModVersion ? _api->getModVersion() : nullptr);
        spdlog::info("[ROCK] Detected ROCK API v{} (mod '{}') — forceGrab={}. useRockPhysics={}.",
            _version, modVer ? modVer : "?", _hasForceGrab ? "yes" : "no",
            g_config.useRockPhysics);
        spdlog::info("[ROCK] Integration {}.", IsActive() ? "ACTIVE (delegating to ROCK)" : "present but disabled by config");
    }

    bool RockBridge::IsActive() const
    {
        if (!_present || !_api) {
            return false;
        }
        // -1 auto (on), 0 force off, 1 force on. Forcing on still requires presence.
        return g_config.useRockPhysics != 0;
    }

    bool RockBridge::IsRunning() const
    {
        // Present + enabled (IsActive) AND ROCK's physics interaction is actually live.
        // When ROCK is disabled in its INI or toggled off at runtime, it never creates
        // (or tears down) its PhysicsInteraction, so isPhysicsInteractionReady() is false
        // and Heisenberg auto-reclaims hand collision + grab.
        return IsActive() && IsPhysicsReady();
    }

    bool RockBridge::IsPhysicsReady() const
    {
        return _api && _api->isPhysicsInteractionReady && _api->isPhysicsInteractionReady();
    }

    bool RockBridge::IsHandHolding(bool isLeft) const
    {
        return _api && _api->isHandHolding && _api->isHandHolding(HandOf(isLeft));
    }

    RE::TESObjectREFR* RockBridge::GetHeldObject(bool isLeft) const
    {
        return (_api && _api->getHeldObject) ? _api->getHeldObject(HandOf(isLeft)) : nullptr;
    }

    RE::TESObjectREFR* RockBridge::GetSelectedObject(bool isLeft) const
    {
        return (_api && _api->getSelectedObject) ? _api->getSelectedObject(HandOf(isLeft)) : nullptr;
    }

    bool RockBridge::IsHandTouching(bool isLeft) const
    {
        return _api && _api->isHandTouching && _api->isHandTouching(HandOf(isLeft));
    }

    RE::TESObjectREFR* RockBridge::GetLastTouchedObject(bool isLeft) const
    {
        return (_api && _api->getLastTouchedObject) ? _api->getLastTouchedObject(HandOf(isLeft)) : nullptr;
    }

    std::uint32_t RockBridge::GetLastTouchedLayer(bool isLeft) const
    {
        return (_api && _api->getLastTouchedLayer) ? _api->getLastTouchedLayer(HandOf(isLeft)) : 0u;
    }

    bool RockBridge::ClaimObject(RE::TESObjectREFR* refr) const
    {
        return refr && _api && _api->claimPhysicsObject && _api->claimPhysicsObject(refr);
    }

    bool RockBridge::ReleaseObject(RE::TESObjectREFR* refr) const
    {
        return refr && _api && _api->releasePhysicsObject && _api->releasePhysicsObject(refr);
    }

    bool RockBridge::IsObjectClaimed(RE::TESObjectREFR* refr) const
    {
        return refr && _api && _api->isPhysicsObjectClaimed && _api->isPhysicsObjectClaimed(refr);
    }

    void RockBridge::DisablePhysicsHand(bool isLeft) const
    {
        if (_api && _api->disablePhysicsHand) {
            _api->disablePhysicsHand(HandOf(isLeft));
        }
    }

    void RockBridge::EnablePhysicsHand(bool isLeft) const
    {
        if (_api && _api->enablePhysicsHand) {
            _api->enablePhysicsHand(HandOf(isLeft));
        }
    }

    void RockBridge::ForceDrop(bool isLeft) const
    {
        if (_api && _api->forceDropObject) {
            _api->forceDropObject(HandOf(isLeft));
        }
    }

    bool RockBridge::ForceGrab(bool isLeft, RE::TESObjectREFR* refr) const
    {
        if (!refr || !_hasForceGrab || !_api || !_api->forceGrab) {
            return false;
        }
        return _api->forceGrab(HandOf(isLeft), refr);
    }

    void RockBridge::OnRockMessage(std::uint32_t type, const void* data, std::uint32_t dataLen)
    {
        // Observe ROCK's physics lifecycle + grab events. When ROCK owns the grab
        // (bDelegateWorldGrabToRock), its Released event is the ONLY signal that an
        // object was thrown — our own EndGrab->OnThrown path never runs. Translate it
        // so Heisenberg's thrown-object pipeline (impact damage / knockback / NPC aggro)
        // still engages. Keep this light; it runs on the F4SE messaging thread, but
        // ROCK dispatches grab events from its game-thread update (same context our own
        // EndGrab->OnThrown runs in), so a direct call is symmetric and safe.
        using API = rock::api::ROCKApi;
        using PM = API::PhysicsMessage;
        switch (type) {
        case PM::kOnPhysicsInit:
            spdlog::info("[ROCK] kOnPhysicsInit");
            break;
        case PM::kOnPhysicsShutdown:
            spdlog::info("[ROCK] kOnPhysicsShutdown");
            break;
        case PM::kOnGrabEvent: {
            if (!IsActive() || !data || dataLen < sizeof(API::GrabEventData)) {
                break;
            }
            const auto* ev = static_cast<const API::GrabEventData*>(data);
            // Only a genuine throw/release with a resolved body and a trustworthy
            // release velocity feeds the thrown-object tracker. Gentle drops are
            // filtered downstream by OnThrown's min-speed gate.
            if (ev->type != API::GrabEventType::Released ||
                !ev->refr || ev->primaryBodyId == 0x7FFF'FFFFu ||
                !(ev->flags & API::kGrabEventFlagVelocityValid)) {
                break;
            }
            const RE::NiPoint3 vel(ev->velocityGame[0], ev->velocityGame[1], ev->velocityGame[2]);
            spdlog::debug("[ROCK] Released {:08X} body=0x{:08X} vel=({:.0f},{:.0f},{:.0f}) — feeding ThrownObjectTracker",
                          ev->formID, ev->primaryBodyId, vel.x, vel.y, vel.z);
            ThrownObjectTracker::GetSingleton().OnThrown(ev->refr, ev->primaryBodyId, vel);
            break;
        }
        default:
            // kOnTouch/kOnTouchEnd/kOnGrab/kOnRelease — not consumed yet.
            break;
        }
    }
}
