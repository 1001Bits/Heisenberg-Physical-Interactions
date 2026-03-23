#include "PlayerCharacterProxyListener.h"
#include "HandCollision.h"
#include "Physics.h"
#include "GrabConstraint.h"
#include "RE/Fallout.h"  // For RE::PlayerCharacter, etc.
#include <spdlog/spdlog.h>

namespace heisenberg
{
    // =========================================================================
    // STATIC DATA
    // =========================================================================

    // Our custom vtable - initialized with default functions then patched
    PlayerCharacterProxyListener::Vtable PlayerCharacterProxyListener::_vtable = {};

    // Default no-op functions for vtable slots we don't use
    static void DefaultDestructor(void*) {}
    static void DefaultFunc1(void*, void*, void*) {}
    static void DefaultFunc2(void*, void*, void*) {}
    static void DefaultFunc3(void*, void*, void*, void*) {}
    static void DefaultFunc5(void*, void*, void*) {}
    static void DefaultFunc6(void*, void*, void*) {}
    static void DefaultFunc7(void*, void*, void*) {}
    static void DefaultFunc8(void*, void*, void*) {}
    static void DefaultFunc9(void*, void*, void*) {}

    // =========================================================================
    // SINGLETON
    // =========================================================================

    PlayerCharacterProxyListener& PlayerCharacterProxyListener::GetSingleton()
    {
        static PlayerCharacterProxyListener instance;
        return instance;
    }

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    bool PlayerCharacterProxyListener::Initialize()
    {
        if (_initialized) {
            spdlog::debug("[PROXY_LISTENER] Already initialized");
            return true;
        }

        spdlog::info("[PROXY_LISTENER] Initializing PlayerCharacterProxyListener...");

        // Build our custom vtable
        // We copy the base hknpCharacterProxyListener vtable and override processConstraintsCallback
        
        // Get the base vtable address
        auto baseVtable = REL::Relocation<void**>(REL::Offset(CharacterProxyOffsets::listenerVtable));
        
        spdlog::info("[PROXY_LISTENER] Base hknpCharacterProxyListener vtable at {:p}", (void*)baseVtable.get());

        // Initialize our vtable with default no-op functions
        _vtable.dtor = reinterpret_cast<void*>(&DefaultDestructor);
        _vtable.func1 = reinterpret_cast<void*>(&DefaultFunc1);
        _vtable.func2 = reinterpret_cast<void*>(&DefaultFunc2);
        _vtable.func3 = reinterpret_cast<void*>(&DefaultFunc3);
        _vtable.processConstraintsCallback = &ProcessConstraintsCallbackImpl;  // Our custom implementation!
        _vtable.func5 = reinterpret_cast<void*>(&DefaultFunc5);
        _vtable.func6 = reinterpret_cast<void*>(&DefaultFunc6);
        _vtable.func7 = reinterpret_cast<void*>(&DefaultFunc7);
        _vtable.func8 = reinterpret_cast<void*>(&DefaultFunc8);
        _vtable.func9 = reinterpret_cast<void*>(&DefaultFunc9);

        // Set up our fake listener object to point to our vtable
        _vtablePtr = &_vtable;

        spdlog::info("[PROXY_LISTENER] Custom vtable created at {:p}", (void*)&_vtable);
        spdlog::info("[PROXY_LISTENER] processConstraintsCallback at vtable[4] = {:p}", 
                     (void*)_vtable.processConstraintsCallback);

        _initialized = true;
        spdlog::info("[PROXY_LISTENER] Initialization complete");
        return true;
    }

    void PlayerCharacterProxyListener::Shutdown()
    {
        spdlog::info("[PROXY_LISTENER] Shutting down...");

        UnregisterFromPlayer();

        {
            std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
            _handBodyIds.clear();
        }

        _initialized = false;
        spdlog::info("[PROXY_LISTENER] Shutdown complete");
    }

    // =========================================================================
    // REGISTRATION WITH PLAYER PROXY
    // =========================================================================

    // Function pointers for hknpCharacterProxy::addListener and removeListener
    using AddListener_t = void(*)(void* proxy, void* listener);
    using RemoveListener_t = void(*)(void* proxy, void* listener);
    
    // Static relocations for the add/remove listener functions
    // These MUST be at namespace scope, not inside functions
    namespace
    {
        REL::Relocation<AddListener_t> s_addListener{ REL::Offset(CharacterProxyOffsets::addListener) };
        REL::Relocation<RemoveListener_t> s_removeListener{ REL::Offset(CharacterProxyOffsets::removeListener) };
    }

    bool PlayerCharacterProxyListener::RegisterWithPlayer()
    {
        if (!_initialized) {
            spdlog::error("[PROXY_LISTENER] Cannot register - not initialized");
            return false;
        }

        if (_registered) {
            spdlog::debug("[PROXY_LISTENER] Already registered with player proxy");
            return true;
        }

        // Get player's character controller - may not be available until player is fully loaded
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            // Log once per second to avoid spam
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
                spdlog::debug("[PROXY_LISTENER] Waiting for player...");
                lastLog = now;
            }
            return false;
        }

        auto* process = player->currentProcess;
        if (!process) {
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
                spdlog::debug("[PROXY_LISTENER] Waiting for player process...");
                lastLog = now;
            }
            return false;
        }

        auto* middleHigh = process->middleHigh;
        if (!middleHigh) {
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
                spdlog::debug("[PROXY_LISTENER] Waiting for middleHigh process...");
                lastLog = now;
            }
            return false;
        }

        auto* charController = middleHigh->charController.get();
        if (!charController) {
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
                // Dump the raw pointer at offset 0x3E0 to see if it's zero or if smart pointer is broken
                void* rawPtr = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(middleHigh) + 0x3E0);
                spdlog::warn("[PROXY_LISTENER] Player has no charController (middleHigh @ {:x}, raw ptr at +0x3E0 = {:x})",
                    reinterpret_cast<uintptr_t>(middleHigh), reinterpret_cast<uintptr_t>(rawPtr));
                lastLog = now;
            }
            return false;
        }

        spdlog::info("[PROXY_LISTENER] Got character controller at {:p}", (void*)charController);

        // The character controller in F4VR is bhkCharProxyController which has the proxy inside
        // bhkCharProxyController inherits from bhkCharacterController (at offset 0) 
        // and has the hknpCharacterProxy at some offset
        // 
        // In Skyrim HIGGS:
        //   bhkCharProxyController : hkpCharacterProxyListener, bhkCharacterController
        //   offset 0x340: bhkCharacterProxy proxy
        //   proxy.characterProxy is the hkpCharacterProxy*
        //
        // For F4VR we need to find the hknpCharacterProxy*
        // Looking at bhkCharacterController, it's 0x450 bytes
        // bhkCharProxyController likely has proxy after that

        // Try to find the hknpCharacterProxy by checking known offsets
        // The bhkCharProxyController has 2 vtables at 0x2e892b8 and 0x2e89328
        // The second vtable (0x2e89328) is the listener vtable
        
        // For now, let's try to find the proxy by exploring the structure
        // In Skyrim, it's at offset 0x340 after the bhkCharacterController base
        // F4VR's bhkCharacterController is 0x450 bytes, so proxy might be at 0x450 or thereabouts
        
        constexpr std::ptrdiff_t PROXY_OFFSET_FROM_CONTROLLER = 0x450;  // After bhkCharacterController
        
        uintptr_t controllerAddr = reinterpret_cast<uintptr_t>(charController);
        void* proxyHolder = reinterpret_cast<void*>(controllerAddr + PROXY_OFFSET_FROM_CONTROLLER);
        
        spdlog::info("[PROXY_LISTENER] Checking for proxy at controller + 0x{:X} = {:p}", 
                     PROXY_OFFSET_FROM_CONTROLLER, proxyHolder);

        // The proxy structure should have the hknpCharacterProxy* at offset 0x10 (like Skyrim's bhkCharacterProxy)
        // bhkCharacterProxy : bhkSerializable { hkRefPtr<hknpCharacterProxy> characterProxy; // 0x10 }
        void** proxyPtr = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(proxyHolder) + 0x10);
        void* hknpProxy = *proxyPtr;

        if (!hknpProxy) {
            spdlog::warn("[PROXY_LISTENER] hknpCharacterProxy is null - player may not be fully initialized");
            return false;
        }

        spdlog::info("[PROXY_LISTENER] Found hknpCharacterProxy at {:p}", hknpProxy);
        _playerProxy = reinterpret_cast<hknpCharacterProxy*>(hknpProxy);

        // Now add our listener to the proxy's listener array
        // The proxy has listeners at offset 0x100 (array) and 0x108 (count) based on Ghidra analysis
        // But we should use the proper addListener function at 0x1b0e750
        
        spdlog::info("[PROXY_LISTENER] Calling hknpCharacterProxy::addListener at {:p}", 
                     (void*)s_addListener.address());

        // Pass our fake listener object (which has vtablePtr as first member pointing to our vtable)
        void* ourListener = &_vtablePtr;
        
        s_addListener(hknpProxy, ourListener);

        _registered = true;
        spdlog::info("[PROXY_LISTENER] Successfully registered with player's character proxy!");
        spdlog::info("[PROXY_LISTENER] Our listener object at {:p}, vtable at {:p}", 
                     ourListener, (void*)&_vtable);

        return true;
    }

    void PlayerCharacterProxyListener::UnregisterFromPlayer()
    {
        if (!_registered || !_playerProxy) {
            return;
        }

        spdlog::info("[PROXY_LISTENER] Unregistering from player's character proxy...");
        
        void* ourListener = &_vtablePtr;
        s_removeListener(_playerProxy, ourListener);

        _playerProxy = nullptr;
        _registered = false;

        spdlog::info("[PROXY_LISTENER] Unregistered from player proxy");
    }

    // =========================================================================
    // HAND BODY ID TRACKING
    // =========================================================================

    void PlayerCharacterProxyListener::RegisterHandBodyId(std::uint32_t bodyId)
    {
        std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
        _handBodyIds.insert(bodyId);
        spdlog::info("[PROXY_LISTENER] Registered hand body ID 0x{:08X} (total: {})", 
                     bodyId, _handBodyIds.size());
    }

    void PlayerCharacterProxyListener::UnregisterHandBodyId(std::uint32_t bodyId)
    {
        std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
        _handBodyIds.erase(bodyId);
        spdlog::info("[PROXY_LISTENER] Unregistered hand body ID 0x{:08X} (total: {})", 
                     bodyId, _handBodyIds.size());
    }

    bool PlayerCharacterProxyListener::IsHandBodyId(std::uint32_t bodyId) const
    {
        std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
        return _handBodyIds.find(bodyId) != _handBodyIds.end();
    }

    // =========================================================================
    // THE MAIN CALLBACK - PREVENTS PLAYER PUSHBACK FROM HAND BODIES
    // =========================================================================
    //
    // This is called by the character proxy before the simplex solver runs.
    // We iterate the contact constraints and zero out velocities for any
    // constraint that involves our hand collision bodies.
    //
    // From HIGGS Skyrim physics.cpp:534:
    //   void PlayerCharacterProxyListener::processConstraintsCallback(
    //       const hkpCharacterProxy *proxy, 
    //       const hkArray<hkpRootCdPoint> &manifold, 
    //       hkpSimplexSolverInput &input)
    //   {
    //       for (i = 0; i < manifold.getSize(); i++) {
    //           hkpRigidBody *rigidBody = hkpGetRigidBody(manifold[i].m_rootCollidableB);
    //           if (rigidBody && IsMoveableEntity(rigidBody)) {
    //               UInt32 layer = GetCollisionLayer(rigidBody);
    //               bool isBiped = layer == kCollisionLayer_Biped || layer == kCollisionLayer_BipedNoCC;
    //               if (IsMoveableEntity(rigidBody) && !isBiped) {
    //                   hkpSurfaceConstraintInfo &surface = input.m_constraints[i];
    //                   surface.m_velocity.setZero4();
    //               }
    //           }
    //       }
    //   }
    //
    // For F4VR, we adapt this to check if the contacted body is one of our
    // hand collision bodies, and if so, zero out the constraint velocity
    // to prevent the player from being pushed.
    // =========================================================================

    void PlayerCharacterProxyListener::ProcessConstraintsCallbackImpl(
        hknpCharacterProxyListener* listener,
        hknpCharacterProxy* proxy,
        void* contacts,  // hkArray<hknpCharacterProxy::Contact>*
        hkSimplexSolverInput* input)
    {
        // Safety checks
        if (!input || !input->m_constraints || input->m_numConstraints <= 0) {
            return;
        }

        auto& instance = GetSingleton();
        
        // Quick check - if no hand bodies registered, nothing to do
        if (instance._handBodyIds.empty()) {
            return;
        }

        // For now, log that the callback was hit (for debugging)
        static int callCount = 0;
        if (++callCount % 1000 == 0) {
            spdlog::debug("[PROXY_LISTENER] processConstraintsCallback hit {} times, {} constraints", 
                         callCount, input->m_numConstraints);
        }

        // AGGRESSIVE ZEROING: Zero velocity for ALL constraints, not just hand-body ones.
        //
        // Trade-off analysis:
        //   PRO: Completely prevents player pushback from hand collision bodies.
        //   CON: Also zeroes constraints from world geometry, NPCs, etc., which can
        //        cause the player to clip through walls in edge cases.
        //
        // Proper fix (deferred - requires reverse engineering):
        //   The hkSurfaceConstraintInfo struct has body ID fields at unknown offsets.
        //   To filter correctly, we'd need to:
        //   1. Reverse engineer hkSurfaceConstraintInfo to find bodyIdA/bodyIdB offsets
        //   2. Compare against our hand body IDs from HandCollision::GetHandBody()
        //   3. Only zero velocities for constraints where one body is our hand body
        //   This is blocked on mapping the contact struct layout from Havok 2014+ SDK.
        
        for (int i = 0; i < input->m_numConstraints; ++i) {
            hkSurfaceConstraintInfo& constraint = input->m_constraints[i];
            
            // Zero out the velocity to prevent pushback
            constraint.m_velocity.x = 0.0f;
            constraint.m_velocity.y = 0.0f;
            constraint.m_velocity.z = 0.0f;
            constraint.m_velocity.w = 0.0f;
        }
    }

} // namespace heisenberg
