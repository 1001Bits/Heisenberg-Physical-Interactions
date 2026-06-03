#include "rock/RockBridge.h"

#include "Config.h"

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

    void RockBridge::OnRockMessage(std::uint32_t type, const void* /*data*/, std::uint32_t /*dataLen*/)
    {
        // Phase 0: observe ROCK's physics lifecycle/contact messages. Phase 2 will
        // dispatch kOnGrab/kOnRelease into Heisenberg's release-action + throwable
        // logic. Keep this light; it runs on the F4SE messaging thread.
        using PM = rock::api::ROCKApi::PhysicsMessage;
        switch (type) {
        case PM::kOnPhysicsInit:
            spdlog::info("[ROCK] kOnPhysicsInit");
            break;
        case PM::kOnPhysicsShutdown:
            spdlog::info("[ROCK] kOnPhysicsShutdown");
            break;
        default:
            // kOnTouch/kOnTouchEnd/kOnGrab/kOnRelease/kOnGrabEvent — handled in Phase 2.
            break;
        }
    }
}
