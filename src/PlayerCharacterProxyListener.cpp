#include "PlayerCharacterProxyListener.h"
#include "HandCollision.h"
#include "Physics.h"
#include "GrabConstraint.h"
#include "RE/Fallout.h"  // For RE::PlayerCharacter, etc.
#include <spdlog/spdlog.h>

namespace heisenberg
{
    namespace
    {
        // Trigger interaction dispatch calls beyond the slots we mapped on the
        // synthetic listener vtable, so this path must stay disabled until the
        // full hknpCharacterProxyListener ABI is understood.
        constexpr bool kEnableSyntheticProxyListenerRegistration = false;
    }

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

        if (!kEnableSyntheticProxyListenerRegistration) {
            _vtablePtr = nullptr;
            _initialized = true;
            spdlog::warn("[PROXY_LISTENER] Synthetic player proxy listener is hard-disabled; using pair filtering only");
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
        {
            std::lock_guard<std::mutex> lock(_grabbedBodyIdsMutex);
            _grabbedBodyIds.clear();
        }
        _activeBodyCount.store(0, std::memory_order_relaxed);

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

        if (!kEnableSyntheticProxyListenerRegistration) {
            static bool loggedDisabled = false;
            if (!loggedDisabled) {
                spdlog::warn("[PROXY_LISTENER] RegisterWithPlayer blocked: synthetic listener remains disabled until full ABI is mapped");
                loggedDisabled = true;
            }
            _playerProxy = nullptr;
            _registered = false;
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

        // VR: charController is at middleHigh + 0x3E8 (NOT CommonLibF4's 0x3E0)
        constexpr std::ptrdiff_t rawCharControllerOffset = 0x3E8;
        void* rawPtr = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(middleHigh) + rawCharControllerOffset);
        auto* charController = reinterpret_cast<RE::bhkCharacterController*>(rawPtr);
        if (!charController) {
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
                spdlog::warn("[PROXY_LISTENER] Player has no charController (middleHigh @ {:x}, raw ptr at +0x3E8 = 0)",
                    reinterpret_cast<uintptr_t>(middleHigh));
                lastLog = now;
            }
            return false;
        }
        spdlog::info("[PROXY_LISTENER] Found charController at {:p} via offset 0x3E8", rawPtr);

        spdlog::info("[PROXY_LISTENER] Got character controller at {:p}", (void*)charController);

        // Ghidra: bhkCharProxyController::Init reads *(param_1 + 0x480) for the proxy.
        // param_1 is the OBJECT BASE. GetCharController returns the bhkCharacterController
        // subobject at base + 0x10. So proxy = charController + (0x480 - 0x10) = charController + 0x470.
        // We read the proxy pointer directly without going through the wrapper.
        constexpr std::ptrdiff_t PROXY_POINTER_OFFSET_FROM_CONTROLLER = 0x470;

        uintptr_t controllerAddr = reinterpret_cast<uintptr_t>(charController);
        void** proxyPtr = reinterpret_cast<void**>(controllerAddr + PROXY_POINTER_OFFSET_FROM_CONTROLLER);
        void* hknpProxy = *proxyPtr;

        spdlog::info("[PROXY_LISTENER] Reading proxy pointer at charController + 0x{:X} = {:p}, value = {:p}",
                     PROXY_POINTER_OFFSET_FROM_CONTROLLER,
                     (void*)proxyPtr, hknpProxy);

        if (!hknpProxy) {
            spdlog::warn("[PROXY_LISTENER] hknpCharacterProxy is null - player may not be fully initialized");
            return false;
        }

        // Sanity check: proxy should be on the heap, not in code segment
        uintptr_t proxyAddr = reinterpret_cast<uintptr_t>(hknpProxy);
        if (proxyAddr > 0x7FF000000000ULL && proxyAddr < 0x800000000000ULL) {
            spdlog::error("[PROXY_LISTENER] Proxy address {:p} is in code segment — wrong offset!", hknpProxy);
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
        if (!kEnableSyntheticProxyListenerRegistration) {
            _playerProxy = nullptr;
            _registered = false;
            return;
        }

        if (!_registered && !_playerProxy) {
            return;
        }

        spdlog::info("[PROXY_LISTENER] Marking proxy listener as unregistered (proxy may be dead on save/load)");

        // DON'T call removeListener — during save/load the proxy may already be
        // destroyed. Just mark ourselves as unregistered. The old proxy's destruction
        // cleans up its own listener array. The new proxy after load gets re-registered
        // from HookPlayerCharacterUpdate.
        _playerProxy = nullptr;
        _registered = false;
    }

    // =========================================================================
    // HAND BODY ID TRACKING
    // =========================================================================

    void PlayerCharacterProxyListener::RegisterHandBodyId(std::uint32_t bodyId)
    {
        if (!kEnableSyntheticProxyListenerRegistration) {
            return;
        }

        std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
        auto result = _handBodyIds.insert(bodyId);
        if (result.second) _activeBodyCount.fetch_add(1, std::memory_order_relaxed);
        spdlog::info("[PROXY_LISTENER] Registered hand body ID 0x{:08X} (total: {})",
                     bodyId, _handBodyIds.size());
    }

    void PlayerCharacterProxyListener::UnregisterHandBodyId(std::uint32_t bodyId)
    {
        if (!kEnableSyntheticProxyListenerRegistration) {
            return;
        }

        std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
        if (_handBodyIds.erase(bodyId)) _activeBodyCount.fetch_sub(1, std::memory_order_relaxed);
        spdlog::info("[PROXY_LISTENER] Unregistered hand body ID 0x{:08X} (total: {})",
                     bodyId, _handBodyIds.size());
    }

    bool PlayerCharacterProxyListener::IsHandBodyId(std::uint32_t bodyId) const
    {
        if (!kEnableSyntheticProxyListenerRegistration) {
            return false;
        }

        std::lock_guard<std::mutex> lock(_handBodyIdsMutex);
        return _handBodyIds.find(bodyId) != _handBodyIds.end();
    }

    void PlayerCharacterProxyListener::RegisterGrabbedBodyId(std::uint32_t bodyId)
    {
        if (!kEnableSyntheticProxyListenerRegistration) {
            return;
        }

        std::lock_guard<std::mutex> lock(_grabbedBodyIdsMutex);
        auto result = _grabbedBodyIds.insert(bodyId);
        if (result.second) _activeBodyCount.fetch_add(1, std::memory_order_relaxed);
        spdlog::info("[PROXY_LISTENER] Registered grabbed body ID 0x{:08X} (total: {})",
                     bodyId, _grabbedBodyIds.size());
    }

    void PlayerCharacterProxyListener::UnregisterGrabbedBodyId(std::uint32_t bodyId)
    {
        if (!kEnableSyntheticProxyListenerRegistration) {
            return;
        }

        std::lock_guard<std::mutex> lock(_grabbedBodyIdsMutex);
        if (_grabbedBodyIds.erase(bodyId)) _activeBodyCount.fetch_sub(1, std::memory_order_relaxed);
        spdlog::info("[PROXY_LISTENER] Unregistered grabbed body ID 0x{:08X} (total: {})",
                     bodyId, _grabbedBodyIds.size());
    }

    bool PlayerCharacterProxyListener::IsGrabbedBodyId(std::uint32_t bodyId) const
    {
        if (!kEnableSyntheticProxyListenerRegistration) {
            return false;
        }

        std::lock_guard<std::mutex> lock(_grabbedBodyIdsMutex);
        return _grabbedBodyIds.find(bodyId) != _grabbedBodyIds.end();
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
        if (!kEnableSyntheticProxyListenerRegistration) {
            return;
        }

        // Safety checks
        if (!input || !input->m_constraints || input->m_numConstraints <= 0) {
            return;
        }

        auto& instance = GetSingleton();
        
        // =====================================================================
        // SELECTIVE CONSTRAINT FILTERING (HIGGS pattern for hknp)
        // Contact struct: 0x40 bytes per entry, bodyId at +0x28
        // Contacts array: *(void**)(contacts+0x00) = data, *(int*)(contacts+0x08) = count
        // Constraints and contacts are parallel arrays (contact[i] → constraint[i])
        // Constraint velocity at SurfaceConstraintInfo+0x10 (hkVector4f)
        // =====================================================================
        if (instance._activeBodyCount.load(std::memory_order_relaxed) == 0) {
            return;
        }

        // Read contacts array
        uintptr_t contactsAddr = reinterpret_cast<uintptr_t>(contacts);
        void* contactData = *reinterpret_cast<void**>(contactsAddr);
        int contactCount = *reinterpret_cast<int*>(contactsAddr + 0x08);

        if (!contactData || contactCount <= 0) {
            return;
        }

        int numConstraints = input->m_numConstraints;
        int count = (contactCount < numConstraints) ? contactCount : numConstraints;

        for (int i = 0; i < count; ++i) {
            // Read bodyId from Contact[i] at +0x28
            std::int32_t bodyId = *reinterpret_cast<std::int32_t*>(
                reinterpret_cast<uintptr_t>(contactData) + i * 0x40 + 0x28);

            if (bodyId == 0x7FFFFFFF) {
                continue;  // Invalid contact
            }

            std::uint32_t uBodyId = static_cast<std::uint32_t>(bodyId);

            // Check if this body is one of our hand bodies or grabbed objects
            bool shouldFilter = false;
            {
                std::lock_guard<std::mutex> lock1(instance._handBodyIdsMutex);
                if (instance._handBodyIds.find(uBodyId) != instance._handBodyIds.end()) {
                    shouldFilter = true;
                }
            }
            if (!shouldFilter) {
                std::lock_guard<std::mutex> lock2(instance._grabbedBodyIdsMutex);
                if (instance._grabbedBodyIds.find(uBodyId) != instance._grabbedBodyIds.end()) {
                    shouldFilter = true;
                }
            }

            if (shouldFilter) {
                // Zero this constraint's velocity to prevent player pushback
                hkSurfaceConstraintInfo& constraint = input->m_constraints[i];
                constraint.m_velocity.x = 0.0f;
                constraint.m_velocity.y = 0.0f;
                constraint.m_velocity.z = 0.0f;
                constraint.m_velocity.w = 0.0f;
            }
        }
    }

} // namespace heisenberg
