#include "HandCollision.h"
#include "Config.h"
#include "Physics.h"
#include "GrabConstraint.h"
#include <cstring>

// =====================================================================
// bhkNPCollisionProxyObject - Proxy collision object structure
// Inherits from bhkNPCollisionObjectBase (NOT bhkNPCollisionObject!)
// =====================================================================
// Helper to get the target collision object from a proxy
inline RE::bhkNPCollisionObject* GetProxyTarget(RE::NiCollisionObject* proxyObj)
{
    if (!proxyObj) return nullptr;
    
    uintptr_t proxyAddr = reinterpret_cast<uintptr_t>(proxyObj);
    
    // Try offset 0x20 first
    RE::bhkNPCollisionObject** targetPtrAt20 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x20);
    if (*targetPtrAt20) return *targetPtrAt20;
    
    // Try offset 0x28
    RE::bhkNPCollisionObject** targetPtrAt28 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x28);
    if (*targetPtrAt28) return *targetPtrAt28;
    
    // Try offset 0x30 
    RE::bhkNPCollisionObject** targetPtrAt30 = reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + 0x30);
    if (*targetPtrAt30) return *targetPtrAt30;
    
    return nullptr;
}

// =========================================================================
// SEH HELPER FUNCTIONS (Must be in separate functions with no C++ objects)
// These use raw function pointers to avoid any C++ object creation
// =========================================================================

// Flag to track if SEH caught an exception (can't use C++ objects in SEH functions)
static volatile bool g_sehExceptionCaught = false;

// Helper to safely call hknpWorld::createBody
// Returns bodyId on success, 0x7FFFFFFE on SEH exception, actual return value otherwise
static std::uint32_t SafeCallCreateBody(void* funcPtr, void* hknpWorld, void* bodyCinfo)
{
    using Func_t = std::uint32_t(__fastcall*)(void*, void*, int, unsigned char);
    g_sehExceptionCaught = false;
    __try {
        // AdditionMode = 0 (ADD_ACTIVE), AdditionFlags = 0 (default)
        return ((Func_t)funcPtr)(hknpWorld, bodyCinfo, 0, 0);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_sehExceptionCaught = true;
        return 0x7FFFFFFE;  // Different value to indicate SEH exception
    }
}

// Helper to safely read vtable from a pointer
static void* SafeReadVtable(void* ptr)
{
    __try {
        return *reinterpret_cast<void**>(ptr);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Helper to safely call hknpBSWorld::applyHardKeyFrame for moving bodies
static bool SafeCallApplyHardKeyFrame(void* funcPtr, void* world, std::uint32_t bodyId, void* position, void* orientation, float deltaTime)
{
    using Func_t = void(__fastcall*)(void*, std::uint32_t, void*, void*, float);
    __try {
        ((Func_t)funcPtr)(world, bodyId, position, orientation, deltaTime);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper to safely call hknpWorld::destroyBodies
static bool SafeCallDestroyBodies(void* funcPtr, void* hknpWorld, std::uint32_t* bodyIds, int count)
{
    using Func_t = void(__fastcall*)(void*, std::uint32_t*, int);
    __try {
        ((Func_t)funcPtr)(hknpWorld, bodyIds, count);
        return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace heisenberg
{
    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    bool HandCollision::Initialize()
    {
        if (_initialized) {
            return true;
        }

        spdlog::info("[HAND_COLLISION] Physics-based hand collision system initializing...");
        
        // Clear state
        _leftHandBody.Invalidate();
        _rightHandBody.Invalidate();
        _leftContact.reset();
        _rightContact.reset();

        _initialized = true;
        spdlog::info("[HAND_COLLISION] Hand collision system initialized (bodies will be created on first update)");
        return true;
    }

    void HandCollision::Shutdown()
    {
        spdlog::info("[HAND_COLLISION] Shutting down hand collision system...");

        // Destroy hand bodies if they exist
        {
            std::scoped_lock lock(_handBodyMutex);
            DestroyPhysicsHandBody(_leftHandBody);
            DestroyPhysicsHandBody(_rightHandBody);
        }

        _initialized = false;
    }

    // =========================================================================
    // MAIN UPDATE
    // =========================================================================

    void HandCollision::Update(const RE::NiPoint3& leftHandPos, const RE::NiPoint3& rightHandPos,
                                const RE::NiPoint3& leftHandVel, const RE::NiPoint3& rightHandVel,
                                float deltaTime)
    {
        if (!g_config.enableHandCollision) {
            return;
        }

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return;
        }

        // Store hand state
        _leftHandPos = leftHandPos;
        _rightHandPos = rightHandPos;
        _leftHandVel = leftHandVel;
        _rightHandVel = rightHandVel;

        // Get current physics world
        void* hknpWorld = GetCurrentHknpWorld();
        void* bhkWorld = GetCurrentBhkWorld();
        
        // Only create physics bodies if enabled (experimental feature)
        if (g_config.usePhysicsHandBodies && hknpWorld && bhkWorld) {
            std::scoped_lock lock(_handBodyMutex);

            // Check if we need to create hand bodies (first time, or world changed)
            bool worldChanged = (_leftHandBody.hknpWorld != hknpWorld && _leftHandBody.IsValid());

            if (worldChanged) {
                spdlog::info("[HAND_COLLISION] Physics world changed, recreating hand bodies");
                DestroyPhysicsHandBody(_leftHandBody);
                DestroyPhysicsHandBody(_rightHandBody);
            }

            // Create left hand body if needed
            if (!_leftHandBody.IsValid()) {
                spdlog::info("[HAND_COLLISION] Creating LEFT hand body at ({:.1f}, {:.1f}, {:.1f})",
                             leftHandPos.x, leftHandPos.y, leftHandPos.z);
                CreatePhysicsHandBody(_leftHandBody, hknpWorld, bhkWorld, leftHandPos, true);
            }

            // Create right hand body if needed
            if (!_rightHandBody.IsValid()) {
                spdlog::info("[HAND_COLLISION] Creating RIGHT hand body at ({:.1f}, {:.1f}, {:.1f})",
                             rightHandPos.x, rightHandPos.y, rightHandPos.z);
                CreatePhysicsHandBody(_rightHandBody, hknpWorld, bhkWorld, rightHandPos, false);
            }

            // Update hand body positions
            if (_leftHandBody.IsValid()) {
                UpdateHandBodyPosition(_leftHandBody, leftHandPos, leftHandVel, deltaTime);
            }

            if (_rightHandBody.IsValid()) {
                UpdateHandBodyPosition(_rightHandBody, rightHandPos, rightHandVel, deltaTime);
            }
        }

        // Also do proximity-based collision as fallback/supplement
        // This catches cases where physics bodies might not have callbacks set up
        CheckProximityCollisions(leftHandPos, leftHandVel, true);
        CheckProximityCollisions(rightHandPos, rightHandVel, false);
    }

    // =========================================================================
    // PHYSICS HAND BODY CREATION (Using hknpWorld::createBody directly)
    // =========================================================================

    bool HandCollision::CreatePhysicsHandBody(PhysicsHandBody& handBody, void* hknpWorld, void* bhkWorld,
                                               const RE::NiPoint3& position, bool isLeft)
    {
        if (!hknpWorld) {
            spdlog::error("[HAND_COLLISION] CreatePhysicsHandBody: No hknpWorld provided");
            return false;
        }
        
        spdlog::info("[HAND_COLLISION] CreatePhysicsHandBody: Creating {} hand body at ({:.1f}, {:.1f}, {:.1f})",
                     isLeft ? "LEFT" : "RIGHT", position.x, position.y, position.z);
        
        // =====================================================================
        // 1. CREATE SHAPE (Box shape like Skyrim HIGGS)
        // =====================================================================
        
        // Half extents for hand collision box (in Havok units)
        constexpr float handSizeGameUnits = 5.0f;  // ~5 game units = small box
        RE::NiPoint4 halfExtents(
            handSizeGameUnits * HAVOK_WORLD_SCALE_COLLISION,
            handSizeGameUnits * HAVOK_WORLD_SCALE_COLLISION,
            handSizeGameUnits * HAVOK_WORLD_SCALE_COLLISION,
            0.0f
        );
        
        constexpr float convexRadius = 0.05f;  // Small convex radius
        
        // Create BuildConfig using game's constructor
        ConstraintFunctions::hknpConvexShapeBuildConfig buildConfig;
        spdlog::debug("[HAND_COLLISION] Calling BuildConfigCtor...");
        ConstraintFunctions::BuildConfigCtor(&buildConfig);
        
        // Create convex shape from half extents
        void* shape = ConstraintFunctions::CreateConvexShapeFromHalfExtents(halfExtents, convexRadius, &buildConfig);
        
        if (!shape) {
            spdlog::error("[HAND_COLLISION] Failed to create convex shape for hand");
            return false;
        }
        
        spdlog::info("[HAND_COLLISION] Created shape at {:p}", shape);

        // =====================================================================
        // 2. CREATE BODY CINFO (Construction info)
        // =====================================================================
        
        hknpBodyCinfo bodyCinfo;
        
        // Call game's default constructor to initialize properly
        spdlog::debug("[HAND_COLLISION] Calling BodyCinfoCtor...");
        ConstraintFunctions::BodyCinfoCtor(&bodyCinfo);
        
        // Set our values
        bodyCinfo.shape = shape;
        
        // Position in Havok units
        bodyCinfo.position = RE::NiPoint4(
            position.x * HAVOK_WORLD_SCALE_COLLISION,
            position.y * HAVOK_WORLD_SCALE_COLLISION,
            position.z * HAVOK_WORLD_SCALE_COLLISION,
            0.0f
        );
        
        // Identity orientation
        bodyCinfo.orientation = RE::NiPoint4(0.0f, 0.0f, 0.0f, 1.0f);
        
        // =====================================================================
        // 3. SET MOTION PROPERTIES (KEYFRAMED - we control position manually)
        // =====================================================================
        
        // qualityId = 2 means KEYFRAMED (gravity-less, we control it)
        // The motionId will be assigned by createBody based on qualityId
        bodyCinfo.qualityId = static_cast<std::uint8_t>(hknpMotionPropertiesId::KEYFRAMED);
        
        // =====================================================================
        // 4. SET COLLISION FILTER 
        // =====================================================================
        
        // Use a simple collision filter - no collision with player, collision with props
        // Layer 56 (0x38) is often used for props/items
        std::uint32_t filterInfo = 0x38;  // Layer 56 - collides with world objects
        filterInfo |= (1 << 15);  // Bit 15 enables some collision flag
        
        // Ragdoll bits: left hand = 1, right hand = 2 (in bits 8-12)
        std::uint8_t ragdollBits = isLeft ? 1 : 2;
        filterInfo |= (ragdollBits << 8);
        
        bodyCinfo.collisionFilterInfo = filterInfo;
        
        spdlog::info("[HAND_COLLISION] Body cinfo: qualityId={}, filterInfo=0x{:08X}, shape={:p}", 
                     bodyCinfo.qualityId, filterInfo, bodyCinfo.shape);
        spdlog::info("[HAND_COLLISION] hknpWorld={:p}, bhkWorld={:p}", hknpWorld, bhkWorld);

        // =====================================================================
        // 5. CALL hknpWorld::createBody DIRECTLY
        // =====================================================================
        
        // hknpWorld::createBody(hknpBodyCinfo&, AdditionMode, AdditionFlags)
        // VR offset: 0x1543ff0 (from fo4_database, Status 4)
        // AdditionMode: 0 = ADD_ACTIVE, 1 = ADD_INACTIVE  
        // AdditionFlags: 0 = default
        using hknpWorld_createBody_t = std::uint32_t(__fastcall*)(void* world, hknpBodyCinfo* cinfo, int additionMode, unsigned char flags);
        static REL::Relocation<hknpWorld_createBody_t> hknpWorld_createBody{ REL::Offset(0x1543ff0) };
        
        spdlog::info("[HAND_COLLISION] Calling hknpWorld::createBody at {:p} with world={:p}, cinfo={:p}...",
                     (void*)hknpWorld_createBody.address(), hknpWorld, (void*)&bodyCinfo);
        
        // Try calling directly without SEH first to see actual error
        std::uint32_t bodyId = 0x7FFFFFFF;
        
        // Check if hknpWorld looks valid by reading a value from it
        // hknpWorld should have a vtable at offset 0
        void* worldVtable = SafeReadVtable(hknpWorld);
        if (!worldVtable) {
            spdlog::error("[HAND_COLLISION] Failed to read hknpWorld vtable - invalid world pointer!");
            return false;
        }
        spdlog::info("[HAND_COLLISION] hknpWorld vtable={:p}", worldVtable);
        
        void* createBodyFuncPtr = (void*)hknpWorld_createBody.address();
        bodyId = SafeCallCreateBody(createBodyFuncPtr, hknpWorld, &bodyCinfo);
        
        if (g_sehExceptionCaught) {
            spdlog::error("[HAND_COLLISION] hknpWorld::createBody CRASHED (SEH exception caught)!");
            return false;
        }
        
        if (bodyId == 0x7FFFFFFF) {
            spdlog::error("[HAND_COLLISION] hknpWorld::createBody returned invalid body ID (0x7FFFFFFF) - function rejected our cinfo");
            return false;
        }
        
        spdlog::info("[HAND_COLLISION] SUCCESS! Created {} hand body with ID: 0x{:08X}", 
                     isLeft ? "LEFT" : "RIGHT", bodyId);

        // =====================================================================
        // 6. STORE RESULTS
        // =====================================================================
        
        handBody.bodyId = bodyId;
        handBody.shape = shape;
        handBody.hknpWorld = hknpWorld;
        handBody.bhkWorld = bhkWorld;
        handBody.valid = true;
        handBody.collisionEnabled = true;
        handBody.createdTime = 0.0;
        handBody.physicsSystem = nullptr;  // Not using physics system wrapper
        
        return true;
    }

    void HandCollision::DestroyPhysicsHandBody(PhysicsHandBody& handBody)
    {
        if (!handBody.IsValid()) {
            return;
        }
        
        spdlog::info("[HAND_COLLISION] Destroying hand body id=0x{:08X}", handBody.bodyId);
        
        if (handBody.hknpWorld) {
            // Call hknpWorld::destroyBodies to remove the body
            ConstraintFunctions::DestroyBodies(handBody.hknpWorld, &handBody.bodyId, 1, 0);
        }
        
        handBody.Invalidate();
    }

    void HandCollision::UpdateHandBodyPosition(PhysicsHandBody& handBody, 
                                                const RE::NiPoint3& position,
                                                const RE::NiPoint3& velocity,
                                                float deltaTime)
    {
        if (!handBody.IsValid() || !handBody.hknpWorld) {
            return;
        }
        
        // Convert position to Havok units
        RE::NiPoint4 hkPosition(
            position.x * HAVOK_WORLD_SCALE_COLLISION,
            position.y * HAVOK_WORLD_SCALE_COLLISION,
            position.z * HAVOK_WORLD_SCALE_COLLISION,
            0.0f
        );
        
        // Try using hknpWorld::setBodyPosition first (simpler, like working sample code)
        // ActivationBehavior: 0 = DO_NOT_ACTIVATE
        try {
            ConstraintFunctions::hknpWorld_setBodyPosition(
                handBody.hknpWorld,
                handBody.bodyId,
                hkPosition,
                0  // DO_NOT_ACTIVATE - since it's keyframed
            );
        } catch (...) {
            spdlog::error("[HAND_COLLISION] Exception in setBodyPosition");
        }
    }

    void HandCollision::SetHandCollisionEnabled(PhysicsHandBody& handBody, bool enabled)
    {
        if (!handBody.IsValid()) {
            return;
        }
        
        // TODO: Update collision filter to enable/disable collision
        // This would involve modifying bit 14 of the collision filter info
        handBody.collisionEnabled = enabled;
    }

    // =========================================================================
    // WORLD ACCESS
    // =========================================================================

    void* HandCollision::GetCurrentHknpWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }
        
        // Get bhkWorld from player's cell
        auto* cell = player->parentCell;
        
        // TESObjectCell::GetbhkWorld - offset 0x39b070
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        void* bhkWorld = GetbhkWorld(cell);
        if (!bhkWorld) {
            return nullptr;
        }
        
        // bhkWorld has hknpBSWorld* at offset 0x60 (typical Bethesda pattern)
        // This may need adjustment based on actual bhkWorld layout
        void* hknpWorld = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(bhkWorld) + 0x60);
        
        return hknpWorld;
    }

    void* HandCollision::GetCurrentBhkWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }
        
        auto* cell = player->parentCell;
        
        using GetbhkWorld_t = void*(*)(RE::TESObjectCELL*);
        static REL::Relocation<GetbhkWorld_t> GetbhkWorld{ REL::Offset(0x39b070) };
        
        return GetbhkWorld(cell);
    }

    // =========================================================================
    // COLLISION HANDLING
    // =========================================================================

    void HandCollision::CheckProximityCollisions(const RE::NiPoint3& handPos, 
                                                  const RE::NiPoint3& handVel,
                                                  bool isLeft)
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        float collisionRadius = g_config.handCollisionRadius;
        
        // Clear contact
        if (isLeft) {
            _leftContact.reset();
        } else {
            _rightContact.reset();
        }

        // Check nearby objects
        auto nearby = Physics::GetObjectsInRadius(handPos, collisionRadius, player);
        for (auto* refr : nearby) {
            if (refr && Physics::IsGrabbable(refr)) {
                // Set contact (using handle for safe reference)
                if (isLeft) {
                    _leftContact = RE::ObjectRefHandle(refr);
                } else {
                    _rightContact = RE::ObjectRefHandle(refr);
                }
                
                // Hand push disabled - was causing unwanted object movement
                // float velMag = handVel.Length();
                // if (velMag > g_config.handPushVelocityThreshold) {
                //     ApplyPushForce(refr, handPos, handVel, 1.0f / 90.0f);
                // }
                break;  // Only affect first/closest object
            }
        }
    }

    // Helper to safely cast collision objects - follows bhkNPCollisionProxyObject to its target
    // since proxy objects don't have their own physics system
    static RE::bhkNPCollisionObject* SafeCastCollisionObject(RE::NiCollisionObject* collObj)
    {
        if (!collObj) return nullptr;
        
        // Check RTTI to verify the actual type
        auto* rtti = collObj->GetRTTI();
        if (!rtti || !rtti->GetName()) {
            return nullptr;
        }
        
        const char* typeName = rtti->GetName();
        
        // Handle proxy objects - follow the target pointer to get the real collision object
        if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
            spdlog::trace("[HAND_COLLISION] Found ProxyObject - following target pointer");
            RE::bhkNPCollisionObject* target = GetProxyTarget(collObj);
            if (!target) {
                spdlog::trace("[HAND_COLLISION] ProxyObject has null target!");
                return nullptr;
            }
            return target;
        }
        
        // Accept bhkNPCollisionObject directly
        if (std::strcmp(typeName, "bhkNPCollisionObject") == 0) {
            return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
        }
        
        // Check inheritance chain for bhkNPCollisionObject
        for (auto iter = rtti; iter; iter = iter->GetBaseRTTI()) {
            if (iter->GetName() && std::strcmp(iter->GetName(), "bhkNPCollisionObject") == 0) {
                return reinterpret_cast<RE::bhkNPCollisionObject*>(collObj);
            }
        }
        
        return nullptr;
    }

    void HandCollision::ApplyPushForce(RE::TESObjectREFR* refr, const RE::NiPoint3& handPos,
                                        const RE::NiPoint3& handVel, float /*deltaTime*/)
    {
        if (!refr) return;

        // Get the collision object
        auto* node = refr->Get3D();
        if (!node) return;

        RE::bhkNPCollisionObject* colObj = nullptr;
        
        // Try to get collision object from the node directly - with safe casting
        if (node->collisionObject) {
            colObj = SafeCastCollisionObject(node->collisionObject.get());
        }
        
        // If not found, try children (for NiNode)
        if (!colObj && node->IsNode()) {
            auto* asNode = static_cast<RE::NiNode*>(node);
            // Safety bound to prevent infinite loop if children array is corrupted
            uint32_t childCount = asNode->children.size();
            if (childCount > 100) childCount = 100;
            for (uint32_t i = 0; i < childCount; ++i) {
                auto* child = asNode->children[i].get();
                if (child && child->collisionObject) {
                    colObj = SafeCastCollisionObject(child->collisionObject.get());
                    if (colObj) break;
                }
            }
        }

        if (!colObj) return;

        // Validate physics system
        if (!colObj->spSystem) {
            spdlog::trace("[HAND_COLLISION] Object {:08X} has no physics system, skipping push", refr->formID);
            return;
        }

        // Apply velocity in direction hand is moving
        float pushMultiplier = g_config.handPushForceMultiplier;
        RE::NiPoint4 pushVel(
            handVel.x * HAVOK_WORLD_SCALE_COLLISION * pushMultiplier,
            handVel.y * HAVOK_WORLD_SCALE_COLLISION * pushMultiplier,
            handVel.z * HAVOK_WORLD_SCALE_COLLISION * pushMultiplier,
            0.0f
        );

        if (CollisionFunctions::IsCollisionObjectValid(colObj)) {
            CollisionFunctions::SetLinearVelocity(colObj, pushVel);
            
            spdlog::trace("[HAND_COLLISION] Pushed object {:08X} with vel ({:.1f}, {:.1f}, {:.1f})",
                          refr->formID, handVel.x, handVel.y, handVel.z);
        }
    }

    // =========================================================================
    // ACCESSORS
    // =========================================================================

    bool HandCollision::IsInContact(bool isLeft) const
    {
        const auto& handle = isLeft ? _leftContact : _rightContact;
        return static_cast<bool>(handle);
    }

    RE::TESObjectREFR* HandCollision::GetContactObject(bool isLeft) const
    {
        const auto& handle = isLeft ? _leftContact : _rightContact;
        if (!handle) return nullptr;
        RE::NiPointer<RE::TESObjectREFR> refPtr = handle.get();
        return refPtr.get();
    }

    const PhysicsHandBody& HandCollision::GetHandBody(bool isLeft) const
    {
        return isLeft ? _leftHandBody : _rightHandBody;
    }

    void HandCollision::TriggerCollisionHaptics(bool isLeft, float intensity, float duration)
    {
        // TODO: Integrate with VR haptics system
        // This would call into SteamVR/OpenXR haptics
        (void)isLeft;
        (void)intensity;
        (void)duration;
    }
}
