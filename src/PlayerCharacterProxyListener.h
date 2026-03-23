#pragma once

#include "RE/Havok/hkVector4.h"
#include "RE/Havok/hkArray.h"
#include "RE/Fallout.h"  // For REL::Relocation, REL::Offset
#include <set>
#include <mutex>

namespace heisenberg
{
    // Forward declarations
    struct hknpCharacterProxy;
    struct hknpCharacterProxyListener;
    struct hkSurfaceConstraintInfo;
    struct hkSimplexSolverInput;

    // =========================================================================
    // hkSurfaceConstraintInfo - Constraint data for character controller
    // From Havok SDK: Common/Internal/SimplexSolver/hkSimplexSolver.h
    // This is what we modify to prevent player pushback from hand collision bodies
    // =========================================================================
    struct hkSurfaceConstraintInfo
    {
        RE::hkVector4f m_plane;              // 0x00 - Plane normal (w = distance)
        RE::hkVector4f m_velocity;           // 0x10 - Velocity to ZERO for non-pushback
        float m_staticFriction;              // 0x20
        float m_extraUpStaticFriction;       // 0x24
        float m_extraDownStaticFriction;     // 0x28
        float m_dynamicFriction;             // 0x2C
        int m_priority;                      // 0x30
        // Padding to 0x38 if needed
    };
    // Size can vary based on alignment - 0x34 without padding, 0x38 with padding
    // static_assert(sizeof(hkSurfaceConstraintInfo) >= 0x34, 
    //               "hkSurfaceConstraintInfo too small");

    // =========================================================================
    // hkSimplexSolverInput - Input to simplex solver with constraint array
    // From Havok SDK: Common/Internal/SimplexSolver/hkSimplexSolver.h
    // =========================================================================
    struct hkSimplexSolverInput
    {
        RE::hkVector4f m_position;           // 0x00 - Particle position
        RE::hkVector4f m_velocity;           // 0x10 - Particle velocity
        RE::hkVector4f m_maxSurfaceVelocity; // 0x20 - Max velocity clamp
        RE::hkVector4f m_upVector;           // 0x30 - Up direction for friction
        float m_deltaTime;                   // 0x40 - Timestep
        float m_minDeltaTime;                // 0x44 - Min timestep
        hkSurfaceConstraintInfo* m_constraints; // 0x48 - Array of constraints
        int m_numConstraints;                // 0x50 - Number of constraints
    };

    // =========================================================================
    // hknpCharacterProxy::Contact - Contact information for character
    // From F4VR PDB: hknpCharacterProxy::Contact
    // Used in processConstraintsCallback to identify what the character hit
    // =========================================================================
    struct hknpCharacterProxyContact
    {
        // Structure is not fully known, but we need the body ID to check
        // if it's our hand collision body
        // Based on hknpCharacterProxyInternals::createSurfaceConstraint signature,
        // this likely contains collision info including body ID
        
        // Placeholder - will need to be filled in based on Ghidra analysis
        std::uint8_t data[0x80];  // Estimated size based on typical contact structures
    };

    // =========================================================================
    // hknpCharacterProxyListener - Virtual interface for character callbacks
    // F4VR equivalent of Skyrim's hkpCharacterProxyListener
    // 
    // From Ghidra analysis of fireConstraintsProcessed (0x1b0e9c0):
    // - Listeners are stored at proxy + 0x100 (array) and proxy + 0x108 (count)
    // - processConstraintsCallback is at vtable offset 0x20 (vtable[4])
    // =========================================================================
    
    // Function typedefs for listener vtable
    using ProcessConstraintsCallback_t = void(*)(
        hknpCharacterProxyListener* listener,
        hknpCharacterProxy* proxy,
        void* contacts,  // hkArray<hknpCharacterProxy::Contact>*
        hkSimplexSolverInput* input
    );

    // VR Offsets for character proxy functions
    namespace CharacterProxyOffsets
    {
        // hknpCharacterProxy member functions
        constexpr std::ptrdiff_t addListener = 0x1b0e750;       // Add listener to proxy
        constexpr std::ptrdiff_t removeListener = 0x1b0e7f0;    // Remove listener
        constexpr std::ptrdiff_t fireConstraintsProcessed = 0x1b0e9c0;  // Fire callbacks
        
        // hknpCharacterProxyListener vtable location
        constexpr std::ptrdiff_t listenerVtable = 0x38b1a70;    // Base vtable
        
        // bhkCharProxyController vtables
        constexpr std::ptrdiff_t bhkCharProxyController_vtable0 = 0x2e892b8;
        constexpr std::ptrdiff_t bhkCharProxyController_vtable1 = 0x2e89328;  // Listener vtable
        
        // Proxy structure offsets (from Ghidra analysis)
        constexpr std::ptrdiff_t proxy_listeners = 0x100;       // hkArray<hknpCharacterProxyListener*>
        constexpr std::ptrdiff_t proxy_listenerCount = 0x108;   // int
    }

    // =========================================================================
    // PlayerCharacterProxyListener - Our custom listener to prevent pushback
    // 
    // When physics hand bodies collide with the player character controller,
    // this listener zeros out the constraint velocities to prevent the player
    // from being pushed away.
    // 
    // Based on HIGGS Skyrim pattern from physics.cpp:534
    // =========================================================================
    class PlayerCharacterProxyListener
    {
    public:
        // Virtual function table - must match hknpCharacterProxyListener layout
        // We create our own vtable with custom processConstraintsCallback
        struct Vtable
        {
            void* dtor;                      // 0x00 - Destructor
            void* func1;                     // 0x08 - Unknown (contactAdded?)
            void* func2;                     // 0x10 - Unknown (contactRemoved?)  
            void* func3;                     // 0x18 - Unknown (characterInteraction?)
            ProcessConstraintsCallback_t processConstraintsCallback;  // 0x20 - Our callback!
            void* func5;                     // 0x28 - Unknown
            void* func6;                     // 0x30 - Unknown
            void* func7;                     // 0x38 - Unknown
            void* func8;                     // 0x40 - Unknown
            void* func9;                     // 0x48 - Unknown (onCharacterProxyAdded?)
        };

        static PlayerCharacterProxyListener& GetSingleton();

        // Initialize/shutdown the listener system
        bool Initialize();
        void Shutdown();

        // Register/unregister with player's character proxy
        bool RegisterWithPlayer();
        void UnregisterFromPlayer();

        // Track which body IDs are hand collision bodies
        void RegisterHandBodyId(std::uint32_t bodyId);
        void UnregisterHandBodyId(std::uint32_t bodyId);
        bool IsHandBodyId(std::uint32_t bodyId) const;
        
        // Get the current player proxy (if registered)
        hknpCharacterProxy* GetPlayerProxy() const { return _playerProxy; }

    private:
        PlayerCharacterProxyListener() = default;
        ~PlayerCharacterProxyListener() = default;
        PlayerCharacterProxyListener(const PlayerCharacterProxyListener&) = delete;
        PlayerCharacterProxyListener& operator=(const PlayerCharacterProxyListener&) = delete;

        // The actual callback implementation
        static void ProcessConstraintsCallbackImpl(
            hknpCharacterProxyListener* listener,
            hknpCharacterProxy* proxy,
            void* contacts,
            hkSimplexSolverInput* input
        );

        // Our custom vtable instance
        static Vtable _vtable;
        
        // Fake listener object with vtable pointer
        // This is what gets registered with the character proxy
        void* _vtablePtr = nullptr;  // Points to _vtable

        // Current player proxy we're registered with
        hknpCharacterProxy* _playerProxy = nullptr;
        
        // Set of hand body IDs to check against
        std::set<std::uint32_t> _handBodyIds;
        mutable std::mutex _handBodyIdsMutex;
        
        bool _initialized = false;
        bool _registered = false;
    };

    // Global singleton access
    inline PlayerCharacterProxyListener& GetPlayerProxyListener()
    {
        return PlayerCharacterProxyListener::GetSingleton();
    }

} // namespace heisenberg
