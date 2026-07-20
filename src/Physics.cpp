#include "Physics.h"
#include "GrabConstraint.h"  // For BhkPhysicsSystemGetNumBodies
#include "HavokPhysicsKeyframe.h"  // For motion type offsets
#include "Utils.h"  // For IsPowerArmorPiece, IsPlayerInPowerArmor, GetTime
#include "F4VROffsets.h"  // For TESObjectCell_GetbhkWorld

#include <algorithm>  // For std::partial_sort
#include <unordered_set>
#include <unordered_map>
#include <shared_mutex>
#include <limits>
#include <vector>
#include <functional>  // For std::function in TryFindPlayerBodyAlternative
#include <cstring>
#include <intrin.h>  // For _mm_set1_ps SSE intrinsic

namespace heisenberg::Physics
{
    // ==================================================================================
    // HAVOK NATIVE SPHERE CAST STRUCTURES AND FUNCTIONS
    // ==================================================================================
    // These enable true physics sphere casting like Skyrim HIGGS uses via hkpWorld_LinearCast.
    // In F4VR, we use hknpWorld::castShape which is the equivalent for the hknp (New Physics) API.
    //
    // Key functions from fo4_database.csv (Status 4 = Verified):
    //   hknpWorld::castShape           @ 0x1415a6c00
    //   hknpSphereShape::createSphereShape @ 0x1415ff4e0
    //   TESHavokUtilities::FindBodyIdRef   @ 0x140626690
    // ==================================================================================

    // hkRotationf is a 3x3 rotation matrix stored as 3 column vectors (each hkVector4f)
    struct alignas(16) hkRotationf
    {
        RE::hkVector4f col0;  // 0x00
        RE::hkVector4f col1;  // 0x10
        RE::hkVector4f col2;  // 0x20

        void setIdentity()
        {
            col0 = RE::hkVector4f(1.0f, 0.0f, 0.0f, 0.0f);
            col1 = RE::hkVector4f(0.0f, 1.0f, 0.0f, 0.0f);
            col2 = RE::hkVector4f(0.0f, 0.0f, 1.0f, 0.0f);
        }
    };
    static_assert(sizeof(hkRotationf) == 0x30);

    // Local definition of hknpQueryFilterData
    struct LocalQueryFilterData
    {
        RE::hknpMaterialId materialId;      // 0x00
        std::uint32_t collisionFilterInfo;  // 0x04
        std::uint64_t userData;             // 0x08
    };
    static_assert(sizeof(LocalQueryFilterData) == 0x10);

    // hknpShapeCastQuery for sphere/shape casting
    // Based on analysis of function signatures and Havok 2014 documentation
    struct alignas(16) hknpShapeCastQuery
    {
        // hknpCollisionQuery base (0x20 bytes)
        void* filter;                           // 0x00 - hknpCollisionFilter*
        LocalQueryFilterData filterData;        // 0x08 - Material, collision filter, userData (0x10 bytes)
        std::int8_t levelOfDetail;              // 0x18
        std::int8_t pad19[7];                   // 0x19 - Padding to 0x20

        // Shape cast specific members
        RE::hknpShape* shape;                   // 0x20 - The shape to cast (8 bytes)
        std::uint8_t pad28[8];                  // 0x28 - Padding for alignment
        RE::hkVector4f origin;                  // 0x30 - Start position (aligned, 16 bytes)
        RE::hkVector4f castVector;              // 0x40 - Direction * distance (16 bytes)
        hkRotationf orientation;                // 0x50 - Shape orientation (3x3 matrix, 48 bytes)
        std::uint32_t flags;                    // 0x80 - Query flags
        float scale;                            // 0x84 - Scale factor (usually 1.0)
        std::uint8_t pad88[8];                  // 0x88 - Padding to 0x90
    };
    static_assert(sizeof(hknpShapeCastQuery) == 0x90);

    // ==================================================================================
    // HAVOK FUNCTION POINTERS (from fo4_database.csv - VR offsets)
    // ==================================================================================
    
    // hknpWorld::castShape(hknpShapeCastQuery&, hkRotationf&, hknpCollisionQueryCollector*, hknpCollisionQueryCollector*)
    using CastShape_t = void(*)(void* world, hknpShapeCastQuery& query, hkRotationf& orientation,
                                 void* hitCollector, void* startCollector);
    inline REL::Relocation<CastShape_t> hknpWorld_castShape{ REL::Offset(0x15a6c00) };

    // hknpSphereShape::createSphereShape(hkVector4f& center, float radius) -> hknpShape*
    using CreateSphereShape_t = RE::hknpShape*(*)(RE::hkVector4f& center, float radius);
    inline REL::Relocation<CreateSphereShape_t> hknpSphereShape_createSphereShape{ REL::Offset(0x15ff4e0) };

    // TESHavokUtilities::FindBodyIdRef(hknpBSWorld&, hknpBodyId) -> TESObjectREFR*
    using FindBodyIdRef_t = RE::TESObjectREFR*(*)(void* hknpBSWorld, RE::hknpBodyId bodyId);
    inline REL::Relocation<FindBodyIdRef_t> TESHavokUtilities_FindBodyIdRef{ REL::Offset(0x626690) };

    // ==================================================================================
    // HIGGS-STYLE PAIR COLLISION FILTER
    // hknpPairCollisionFilter::disableCollisionsBetween(hknpWorld*, hknpBodyId, hknpBodyId)
    // THIS IS A MEMBER FUNCTION — first param is 'this' (pairCollisionFilter*)
    // VR Offset from fo4_database.csv: 379288,0x1418ebbc0,0x14196de70,4
    // ==================================================================================
    using DisableCollisionsBetween_t = void(*)(void* pairCollisionFilter, void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB);
    inline REL::Relocation<DisableCollisionsBetween_t> hknpPairCollisionFilter_disableCollisionsBetween{ REL::Offset(0x196de70) };

    // Helper: get the pair collision filter from hknpWorld
    // Path: hknpWorld+0x150 → modifierManager → +0x5E8 → collisionFilter (IS-A pairCollisionFilter)
    inline void* GetPairCollisionFilter(void* hknpWorld) {
        if (!hknpWorld) return nullptr;
        void* modifierManager = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + 0x150);
        if (!modifierManager) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(modifierManager) + 0x5E8);
    }

    // ==================================================================================
    // RE-ENABLE PAIR COLLISION (selective)
    // Key = (min(bodyA,bodyB) | (max(bodyA,bodyB) << 32))
    // hkMapBase::remove(Key) at VR offset 0x196ec50
    // The pair filter map lives at pairCollisionFilter + 0x18
    // ==================================================================================
    using RemoveCollisionPair_t = void(*)(void* mapBase, std::uint64_t key);
    inline REL::Relocation<RemoveCollisionPair_t> hkMapBase_remove{ REL::Offset(0x196ec50) };

    using ClearAllCollisionPairs_t = void(*)(void* pairCollisionFilter);
    inline REL::Relocation<ClearAllCollisionPairs_t> hknpPairCollisionFilter_clearAll{ REL::Offset(0x196e0f0) };

    // ==================================================================================
    // CACHED SPHERE SHAPES
    // ==================================================================================
    static RE::hknpShape* g_sphereShape = nullptr;
    static bool g_sphereShapeInitialized = false;
    static constexpr uint32_t INVALID_BODY_ID = 0x7FFFFFFF;
    static constexpr uint32_t INVALID_BODY_ID_ALT = 0xFFFFFFFF;
    // hknp body IDs: lower 16 bits = body index, upper 16 bits = generation counter
    static constexpr uint32_t HKNP_BODY_ID_INDEX_MASK = 0x0000FFFF;
    static constexpr uint32_t MAX_REASONABLE_BODY_INDEX = 0x00100000;

    inline bool IsInvalidBodyId(std::uint32_t bodyId)
    {
        return bodyId == 0 || bodyId == INVALID_BODY_ID || bodyId == INVALID_BODY_ID_ALT;
    }

    inline std::uint32_t GetBodyBufferIndex(std::uint32_t bodyId)
    {
        return bodyId & HKNP_BODY_ID_INDEX_MASK;
    }

    inline bool IsReasonableBodyIndex(std::uint32_t bodyIndex)
    {
        return bodyIndex != HKNP_BODY_ID_INDEX_MASK && bodyIndex < MAX_REASONABLE_BODY_INDEX;
    }

    inline std::uint32_t NormalizeShiftedBodyIdCandidate(std::uint32_t rawBodyId)
    {
        if (IsInvalidBodyId(rawBodyId)) {
            return rawBodyId;
        }

        if (IsReasonableBodyIndex(GetBodyBufferIndex(rawBodyId))) {
            return rawBodyId;
        }

        // Some VR character-controller reads surface as 0xXXXX0000 where the
        // upper 16 bits are the actual body id and the low 16 bits are zero.
        if ((rawBodyId & 0xFFFFu) == 0) {
            const std::uint32_t shiftedBodyId = rawBodyId >> 16;
            if (!IsInvalidBodyId(shiftedBodyId) &&
                IsReasonableBodyIndex(GetBodyBufferIndex(shiftedBodyId))) {
                return shiftedBodyId;
            }
        }

        return rawBodyId;
    }

    // hknpClosestHitCollector vtable address (VR offset from fo4_database.csv ID 1568866)
    // Format: id,fo4_addr,vr_addr,status,name
    // 1568866,0x1436cc678,0x1436f2698,3,hknpClosestHitCollector
    inline REL::Relocation<void*> hknpClosestHitCollector_vtable{ REL::Offset(0x36f2698) };

    // hknpAllHitsCollector vtable address (VR offset from fo4_database.csv)
    // 1254641,0x1436ceaf0,0x1436f4c80,3,hknpAllHitsCollector
    inline REL::Relocation<void*> hknpAllHitsCollector_vtable{ REL::Offset(0x36f4c80) };

    // ==================================================================================
    // HAVOK WORLD SCALE
    // ==================================================================================
    // Havok uses scaled coordinates: 1 game unit = HAVOK_WORLD_SCALE Havok units (≈1/70)
    // All native Havok queries must convert game coords to Havok coords by multiplying.
    // ==================================================================================
    constexpr float HAVOK_WORLD_SCALE = 0.0142875f;  // 1/70 approximately
    constexpr float INVERSE_HAVOK_SCALE = 70.0f;     // For converting back to game units

    // Flag to enable/disable native Havok sphere casting
    // DISABLED January 31, 2026 - causing exceptions, use ViewCaster instead
    static bool g_useNativeSphereCast = false;
    
    // Flag to enable/disable native physics queries for GetObjectsInRadius
    // DISABLED January 31, 2026 - causing exceptions every frame
    static bool g_useNativeObjectsQuery = false;
    // ==================================================================================
    // PERFORMANCE: BLACKLIST RESULT CACHE WITH LRU EVICTION
    // ==================================================================================
    // IsBlacklistedByName() is expensive (string ops + 80+ substring searches).
    // Cache results by formID since object names don't change at runtime.
    // Use shared_mutex for thread-safe read-heavy access pattern.
    // LRU eviction: track access time and evict oldest entries when full.
    // ==================================================================================
    struct BlacklistCacheEntry
    {
        bool isBlacklisted;
        double lastAccessTime;
    };
    static std::unordered_map<RE::TESFormID, BlacklistCacheEntry> g_blacklistCache;
    static std::shared_mutex g_blacklistCacheMutex;
    static constexpr size_t MAX_CACHE_SIZE = 2048;  // Limit cache size
    static constexpr size_t EVICTION_COUNT = 256;   // Evict this many on overflow
    
    // Clear cache (call on cell change if needed)
    void ClearBlacklistCache()
    {
        std::unique_lock lock(g_blacklistCacheMutex);
        g_blacklistCache.clear();
    }
    
    // Evict oldest entries (called under write lock, rate-limited to once per second)
    static void EvictOldestEntries()
    {
        if (g_blacklistCache.size() < MAX_CACHE_SIZE) return;

        // Rate-limit eviction to prevent frame spikes from partial_sort
        static auto lastEvictionTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEvictionTime).count() < 1000) {
            return;
        }
        lastEvictionTime = now;

        // Find the oldest entries by collecting (formID, accessTime) pairs
        std::vector<std::pair<RE::TESFormID, double>> entries;
        entries.reserve(g_blacklistCache.size());
        for (const auto& [formID, entry] : g_blacklistCache) {
            entries.emplace_back(formID, entry.lastAccessTime);
        }
        
        // Partial sort to find EVICTION_COUNT oldest entries
        size_t evictCount = (std::min)(EVICTION_COUNT, entries.size());  // Parentheses prevent Windows min macro
        std::partial_sort(entries.begin(), entries.begin() + evictCount, entries.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Evict the oldest entries
        for (size_t i = 0; i < evictCount; ++i) {
            g_blacklistCache.erase(entries[i].first);
        }
    }
    // ==================================================================================
    // OFFSET-BASED ACCESS TO bhkWorld AND hknpBSWorld MEMBERS
    // ==================================================================================
    // CommonLibF4VR has STUB versions of bhkWorld and hknpBSWorld (forward-declared only).
    // The full CommonLibF4 has the proper definitions but we use CommonLibF4VR via F4VRCommonFramework.
    // So we use raw offset access to get what we need.
    //
    // From full CommonLibF4's BSHavok.h, bhkWorld layout (size = 0x180):
    //   0x00-0x10: NiObject (vtbl + refcount)
    //   0x10: hkRefPtr<hkTaskQueue> taskQueue (0x08)
    //   0x18: BSTArray<bhkIWorldStepListener*> stepListenerA (0x18)
    //   0x30: hkRefPtr<hkbnpPhysicsInterface> behaviorInterface (0x08)
    //   0x38: BSReadWriteLock charProxyManagerLock (0x08)
    //   0x40: BSReadWriteLock charRigidBodyManagerLock (0x08)
    //   0x48: BSReadWriteLock stepListenerALock (0x08)
    //   0x50: hkVector4f origin (0x10)
    //   0x60: hkRefPtr<hknpBSWorld> worldNP (0x08)  <-- THIS IS WHAT WE NEED
    //
    // From full CommonLibF4's hknpBSWorld.h, hknpBSWorld layout (size = 0x730):
    //   0x000-0x6D0: hknpWorld base class
    //   0x6D0: void* userData (0x08)
    //   0x6D8: BSReadWriteLock worldLock (0x08)  <-- THIS IS WHAT WE NEED FOR LOCKING
    // ==================================================================================
    constexpr std::size_t BHKWORLD_WORLDNP_OFFSET = 0x60;
    constexpr std::size_t HKNPBSWORLD_WORLDLOCK_OFFSET = 0x6D8;

    // Get hknpBSWorld* from bhkWorld using raw offset (returned as void* since type is incomplete)
    static void* GetWorldNPRaw(RE::bhkWorld* bhkWorld)
    {
        if (!bhkWorld) {
            return nullptr;
        }
        // hkRefPtr<T> is just { T* ptr; }, so we dereference as pointer-to-pointer
        auto* worldNPPtr = reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(bhkWorld) + BHKWORLD_WORLDNP_OFFSET);
        return worldNPPtr ? *worldNPPtr : nullptr;
    }

    // Get or create a cached sphere shape with radius 1.0 (we scale in query)
    static RE::hknpShape* GetCachedSphereShape()
    {
        if (!g_sphereShapeInitialized) {
            RE::hkVector4f center(0.0f, 0.0f, 0.0f, 0.0f);
            g_sphereShape = hknpSphereShape_createSphereShape(center, 1.0f);
            g_sphereShapeInitialized = true;
            if (g_sphereShape) {
                spdlog::info("[PHYSICS] Created cached sphere shape for native casting");
            } else {
                spdlog::warn("[PHYSICS] Failed to create cached sphere shape");
            }
        }
        return g_sphereShape;
    }

    // Public wrapper (RockGrabCore proxy body needs a shape; reuses the cached one + its
    // CleanupCachedShapes lifecycle instead of creating a private sphere).
    RE::hknpShape* GetSharedSphereShape()
    {
        return GetCachedSphereShape();
    }

    void CleanupCachedShapes()
    {
        // Note: Havok shapes are reference-counted internally.
        // We null our pointer to release our reference.
        g_sphereShape = nullptr;
        g_sphereShapeInitialized = false;
        spdlog::info("[PHYSICS] Cleaned up cached sphere shape");
    }

    // SEH helper: calls TESHavokUtilities_FindBodyIdRef with exception protection.
    // Must be a leaf function with NO C++ objects to be compatible with __try.
    // hknpBodyId is passed as raw uint32 and reinterpreted in place.
    #pragma warning(push)
    #pragma warning(disable: 4731)  // frame pointer modified by inline assembly
    static RE::TESObjectREFR* FindBodyIdRefWithSEH(void* worldNP, void* bodyIdPtr)
    {
        RE::TESObjectREFR* refr = nullptr;
        __try {
            refr = TESHavokUtilities_FindBodyIdRef(worldNP, *static_cast<RE::hknpBodyId*>(bodyIdPtr));
            if (refr && refr->formID == 0) {
                refr = nullptr;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            refr = nullptr;
        }
        return refr;
    }
    #pragma warning(pop)

    // Get TESObjectREFR from hknpBodyId using TESHavokUtilities
    // SEH-protected: the returned REFR is a raw pointer valid only for immediate use.
    // Do NOT store across frame boundaries without ref-counting.
    static RE::TESObjectREFR* GetRefrFromBodyId(RE::bhkWorld* bhkWorld, RE::hknpBodyId bodyId)
    {
        if (!bhkWorld || bodyId.value == INVALID_BODY_ID) {
            return nullptr;
        }

        void* worldNP = GetWorldNPRaw(bhkWorld);
        if (!worldNP) {
            return nullptr;
        }

        return FindBodyIdRefWithSEH(worldNP, &bodyId);
    }

    RE::TESObjectREFR* GetRefrFromBodyId(void* bhkWorld, std::uint32_t bodyId)
    {
        if (!bhkWorld || bodyId == INVALID_BODY_ID || bodyId == INVALID_BODY_ID_ALT || bodyId == 0) {
            return nullptr;
        }

        RE::hknpBodyId id{};
        id.value = bodyId;
        return GetRefrFromBodyId(reinterpret_cast<RE::bhkWorld*>(bhkWorld), id);
    }

    // Get BSReadWriteLock* from hknpBSWorld using raw offset
    static RE::BSReadWriteLock* GetWorldLock(void* hknpBSWorld)
    {
        if (!hknpBSWorld) {
            return nullptr;
        }
        return reinterpret_cast<RE::BSReadWriteLock*>(
            reinterpret_cast<std::uintptr_t>(hknpBSWorld) + HKNPBSWORLD_WORLDLOCK_OFFSET);
    }

    // Helper: Get TESObjectREFR from NiAVObject using Bethesda's built-in function
    static RE::TESObjectREFR* GetRefrFromNode(RE::NiAVObject* node)
    {
        if (!node) {
            return nullptr;
        }
        return RE::TESObjectREFR::FindReferenceFor3D(node);
    }

    RE::bhkWorld* GetWorld()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return nullptr;
        }

        // Get bhkWorld from cell using F4VR offset
        return f4vr::TESObjectCell_GetbhkWorld(player->parentCell);
    }

    RE::bhkWorld* GetWorldFromCell(RE::TESObjectCELL* cell)
    {
        if (!cell) {
            return nullptr;
        }
        return f4vr::TESObjectCell_GetbhkWorld(cell);
    }

    // Check if a ray intersects a sphere (for bounding sphere tests)
    static bool RayIntersectsSphere(
        const RE::NiPoint3& rayOrigin,
        const RE::NiPoint3& rayDir,
        const RE::NiPoint3& sphereCenter,
        float sphereRadius,
        float maxDistance,
        float& outHitDistance)
    {
        // Ray-sphere intersection test
        RE::NiPoint3 oc = rayOrigin - sphereCenter;
        float a = rayDir.x * rayDir.x + rayDir.y * rayDir.y + rayDir.z * rayDir.z;
        float b = 2.0f * (oc.x * rayDir.x + oc.y * rayDir.y + oc.z * rayDir.z);
        float c = (oc.x * oc.x + oc.y * oc.y + oc.z * oc.z) - sphereRadius * sphereRadius;
        float discriminant = b * b - 4 * a * c;
        
        if (discriminant < 0) {
            return false;
        }
        
        float sqrtD = std::sqrt(discriminant);
        float t1 = (-b - sqrtD) / (2 * a);
        float t2 = (-b + sqrtD) / (2 * a);
        
        // We want the closest positive intersection
        float t = t1;
        if (t < 0) t = t2;
        if (t < 0 || t > maxDistance) {
            return false;
        }
        
        outHitDistance = t;
        return true;
    }

    // SEH-compatible helper: Calls TESObjectCELL::Pick() with exception handling.
    // Must be in a separate function because __try cannot coexist with C++ destructors.
    // The crash analysis showed hknpCompressedMeshShape::castRayImpl can null-deref
    // when shape data becomes stale (race with physics step or detection job thread).
    // Pick() internally does Havok raycasts, so it's vulnerable to the same issue.
    static bool CallPickWithSEH(
        RE::TESObjectCELL* cell,
        RE::bhkPickData& pickData,
        RE::NiAVObject** outHitNode)
    {
        *outHitNode = nullptr;
        __try {
            *outHitNode = cell->Pick(pickData);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            spdlog::warn("[RAYCAST] Exception during Pick() - stale physics data in cell");
            return false;
        }
    }

    RaycastResult CastRay(
        const RE::NiPoint3& origin,
        const RE::NiPoint3& direction,
        float maxDistance,
        RE::TESObjectREFR* ignoreRefr)
    {
        RaycastResult result;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return result;
        }

        // Get bhkWorld for locking
        auto* bhkWorld = GetWorldFromCell(player->parentCell);
        if (!GetWorldNPRaw(bhkWorld)) {
            spdlog::trace("[RAYCAST] No physics world available");
            return result;
        }

        // Calculate normalized direction
        RE::NiPoint3 normalizedDir = direction;
        float length = std::sqrt(normalizedDir.x * normalizedDir.x + 
                                  normalizedDir.y * normalizedDir.y + 
                                  normalizedDir.z * normalizedDir.z);
        if (length > 0.0f) {
            normalizedDir.x /= length;
            normalizedDir.y /= length;
            normalizedDir.z /= length;
        }

        // =====================================================================
        // CRITICAL: Lock the physics world for the entire raycast operation
        // =====================================================================
        // HIGGS Skyrim locks world during all raycasts to prevent race conditions
        // with the physics simulation thread. Without this lock, accessing object
        // positions and scene graph nodes can crash if physics is updating them.
        // We use a READ lock since we're only querying, not modifying.
        WorldReadLock worldLock(bhkWorld);
        if (!worldLock.IsLocked()) {
            spdlog::warn("[RAYCAST] Failed to acquire world read lock");
            return result;
        }

        // =====================================================================
        // PHASE 1: Check all nearby physics objects using ray-sphere intersection
        // =====================================================================
        // This bypasses the Pick() method which doesn't detect dynamic physics objects.
        // We test each nearby physics object's bounding sphere against the ray.
        
        auto nearbyObjects = GetObjectsInRadius(origin, maxDistance, player);
        
        RE::TESObjectREFR* closestRefr = nullptr;
        float closestDistance = maxDistance;
        
        for (auto* refr : nearbyObjects) {
            if (!refr) continue;
            if (ignoreRefr && refr == ignoreRefr) continue;
            
            // Get object position and approximate bounding radius
            RE::NiPoint3 refrPos = refr->GetPosition();
            
            // Use a generous bounding radius for small items (about 15 units = 15cm)
            // For larger items, use the distance from position to bounds
            float boundingRadius = 15.0f;
            
            // If object has a 3D model, try to get better bounds
            if (auto* node3D = refr->Get3D()) {
                // Use world bound if available
                if (node3D->IsNode()) {
                    auto* asNode = static_cast<RE::NiNode*>(node3D);
                    // Approximate radius from world bound center
                    float dx = asNode->worldBound.center.x - refrPos.x;
                    float dy = asNode->worldBound.center.y - refrPos.y;
                    float dz = asNode->worldBound.center.z - refrPos.z;
                    float centerOffset = std::sqrt(dx*dx + dy*dy + dz*dz);
                    boundingRadius = asNode->worldBound.fRadius + centerOffset;
                    if (boundingRadius < 10.0f) boundingRadius = 10.0f;
                    if (boundingRadius > 100.0f) boundingRadius = 100.0f;
                    
                    // Use world bound center instead of refr position for more accuracy
                    refrPos = asNode->worldBound.center;
                }
            }
            
            // Ray-sphere intersection test
            float hitDistance = 0.0f;
            if (RayIntersectsSphere(origin, normalizedDir, refrPos, boundingRadius, maxDistance, hitDistance)) {
                if (hitDistance < closestDistance) {
                    closestDistance = hitDistance;
                    closestRefr = refr;
                }
            }
        }
        
        if (closestRefr) {
            result.hit = true;
            result.hitFraction = closestDistance / maxDistance;
            result.hitPoint = origin + normalizedDir * closestDistance;
            result.hitRefr = closestRefr;
            result.hitNode = closestRefr->Get3D();
            result.hitNormal = RE::NiPoint3(0, 0, 1);  // Default up normal
            
            spdlog::trace("[RAYCAST-SPHERE] HIT physics object: refr={:08X} dist={:.1f}",
                closestRefr->formID, closestDistance);
            return result;
        }

        // =====================================================================
        // PHASE 2: Fall back to Pick() for static geometry
        // =====================================================================
        // Re-verify cell is still valid (could change during Phase 1 iteration)
        RE::TESObjectCELL* pickCell = player->parentCell;
        if (!pickCell) {
            spdlog::trace("[RAYCAST] Cell became null during Phase 1, skipping Pick");
            return result;
        }

        RE::NiPoint3 endPoint = origin + normalizedDir * maxDistance;
        RE::bhkPickData pickData;
        pickData.SetStartEnd(origin, endPoint);

        // Pick() internally does Havok raycasts (hknpHybridBroadPhase::castRay ->
        // hknpCompressedMeshShape::castRayImpl). Wrap in SEH to catch null-deref
        // crashes from stale shape data during concurrent physics updates.
        RE::NiAVObject* hitNode = nullptr;
        bool pickSucceeded = CallPickWithSEH(pickCell, pickData, &hitNode);

        if (pickSucceeded && pickData.HasHit()) {
            result.hit = true;
            result.hitFraction = pickData.GetHitFraction();
            result.hitPoint = origin + normalizedDir * (maxDistance * result.hitFraction);
            result.hitNode = hitNode;
            
            result.hitNormal.x = pickData.result.normal.x;
            result.hitNormal.y = pickData.result.normal.y;
            result.hitNormal.z = pickData.result.normal.z;
            result.bodyId = pickData.result.hitBodyInfo.m_bodyId;
            result.hitRefr = GetRefrFromNode(hitNode);
            
            if (ignoreRefr && result.hitRefr == ignoreRefr) {
                result.hit = false;
                result.hitRefr = nullptr;
                result.hitNode = nullptr;
            }
            
            spdlog::trace("[RAYCAST-PICK] HIT static: refr={:08X} node={} fraction={:.3f}",
                result.hitRefr ? result.hitRefr->formID : 0,
                result.hitNode ? result.hitNode->name.c_str() : "null",
                result.hitFraction);
        }
        
        return result;
    }

    // Helper to build perpendicular vectors for sphere cast
    static void GetPerpendicularBasis(const RE::NiPoint3& dir, RE::NiPoint3& outRight, RE::NiPoint3& outUp)
    {
        // Find a vector that's not parallel to dir
        RE::NiPoint3 arbitrary = (std::abs(dir.x) < 0.9f) ? RE::NiPoint3(1, 0, 0) : RE::NiPoint3(0, 1, 0);
        
        // Cross product: right = dir x arbitrary
        outRight.x = dir.y * arbitrary.z - dir.z * arbitrary.y;
        outRight.y = dir.z * arbitrary.x - dir.x * arbitrary.z;
        outRight.z = dir.x * arbitrary.y - dir.y * arbitrary.x;
        
        // Normalize right
        float len = std::sqrt(outRight.x * outRight.x + outRight.y * outRight.y + outRight.z * outRight.z);
        if (len > 0.0001f) {
            outRight.x /= len;
            outRight.y /= len;
            outRight.z /= len;
        }
        
        // Cross product: up = right x dir
        outUp.x = outRight.y * dir.z - outRight.z * dir.y;
        outUp.y = outRight.z * dir.x - outRight.x * dir.z;
        outUp.z = outRight.x * dir.y - outRight.y * dir.x;
    }

    // ==================================================================================
    // NATIVE HAVOK SPHERE CAST
    // ==================================================================================
    // Uses hknpWorld::castShape to perform a true physics sphere cast like HIGGS Skyrim.
    // This is more accurate than multi-ray casting because it tests against actual physics meshes.
    // ==================================================================================
    
    // SEH-compatible helper function - calls hknpWorld_castShape with exception handling
    // Must not have any objects with destructors (hence out parameters instead of return values)
    static bool CallCastShapeWithSEH(
        void* worldNP,
        hknpShapeCastQuery& query,
        hkRotationf& castOrientation,
        RE::hknpClosestHitCollector* collector,
        bool* outHasHit)
    {
        *outHasHit = false;
        __try {
            hknpWorld_castShape(worldNP, query, castOrientation, collector, nullptr);
            *outHasHit = collector->hasHit;
            return true;  // Call succeeded
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            spdlog::warn("[SPHERECAST-NATIVE] Exception during castShape - disabling native casting");
            g_useNativeSphereCast = false;
            return false;  // Call failed with exception
        }
    }
    
    static RaycastResult CastSphereNative(
        const RE::NiPoint3& origin,
        const RE::NiPoint3& normalizedDir,
        float radius,
        float maxDistance,
        [[maybe_unused]] RE::TESObjectREFR* ignoreRefr)
    {
        RaycastResult result;
        
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return result;
        }
        
        auto* bhkWorld = GetWorldFromCell(player->parentCell);
        void* worldNP = GetWorldNPRaw(bhkWorld);
        if (!worldNP) {
            spdlog::trace("[SPHERECAST-NATIVE] No physics world");
            return result;
        }
        
        // Get or create the cached sphere shape
        RE::hknpShape* sphereShape = GetCachedSphereShape();
        if (!sphereShape) {
            spdlog::trace("[SPHERECAST-NATIVE] No sphere shape available");
            return result;
        }
        
        // Build the shape cast query
        // CRITICAL: Havok uses scaled coordinates (game coords * HAVOK_WORLD_SCALE)
        hknpShapeCastQuery query{};
        query.filter = nullptr;
        query.filterData.materialId.value = 0;
        query.filterData.collisionFilterInfo = 0x2C;  // CustomPick2 layer - hits everything
        query.filterData.userData = 0;
        query.levelOfDetail = 0;
        query.shape = sphereShape;
        query.origin = RE::hkVector4f(
            origin.x * HAVOK_WORLD_SCALE, 
            origin.y * HAVOK_WORLD_SCALE, 
            origin.z * HAVOK_WORLD_SCALE, 
            0.0f);
        query.castVector = RE::hkVector4f(
            normalizedDir.x * maxDistance * HAVOK_WORLD_SCALE,
            normalizedDir.y * maxDistance * HAVOK_WORLD_SCALE,
            normalizedDir.z * maxDistance * HAVOK_WORLD_SCALE,
            0.0f
        );
        query.orientation.setIdentity();
        query.flags = 0;
        query.scale = radius * HAVOK_WORLD_SCALE;  // Scale the unit sphere to desired radius (in Havok units)
        
        // Output orientation (identity)
        hkRotationf castOrientation;
        castOrientation.setIdentity();
        
        // Create collector on heap with proper alignment
        // We manually set fields because CommonLibF4VR's constructor uses REL::ID
        // that doesn't exist in the VR address library.
        struct AlignedDeleter { void operator()(void* p) const { _aligned_free(p); } };
        constexpr size_t kCollectorSize = 0x90;
        std::unique_ptr<std::uint8_t, AlignedDeleter> collectorStorage(
            static_cast<std::uint8_t*>(_aligned_malloc(kCollectorSize, 16)));
        if (!collectorStorage) {
            spdlog::error("[PHYSICS] CastSphere: Failed to allocate collector");
            return result;
        }
        std::memset(collectorStorage.get(), 0, kCollectorSize);
        auto* collector = reinterpret_cast<RE::hknpClosestHitCollector*>(collectorStorage.get());

        // Set vtable pointer to VR address — validate it's in the game's code section
        void* vtablePtr = hknpClosestHitCollector_vtable.get();
        if (!vtablePtr || reinterpret_cast<uintptr_t>(vtablePtr) < REL::Module::get().base()) {
            spdlog::error("[PHYSICS] CastSphere: Invalid vtable pointer {:p}", vtablePtr);
            return result;
        }
        *reinterpret_cast<void**>(collector) = vtablePtr;

        // Initialize base class members (hknpCollisionQueryCollector)
        collector->hints = 0;
        collector->earlyOutThreshold.real = _mm_set1_ps((std::numeric_limits<float>::max)());

        // Initialize result struct
        collector->result.queryType.reset();
        collector->result.fraction.storage = (std::numeric_limits<float>::max)();
        collector->result.position = RE::NiPoint3();
        collector->result.normal = RE::NiPoint3();
        collector->result.queryBodyInfo.m_bodyId.value = 0x7FFFFFFF;
        collector->result.queryBodyInfo.m_shapeKey.storage = static_cast<std::uint32_t>(-1);
        collector->result.queryBodyInfo.m_shapeMaterialId.value = static_cast<std::uint16_t>(-1);
        collector->result.queryBodyInfo.m_shapeCollisionFilterInfo.storage = 0;
        collector->result.queryBodyInfo.m_shapeUserData.storage = 0;
        collector->result.hitBodyInfo.m_bodyId.value = 0x7FFFFFFF;
        collector->result.hitBodyInfo.m_shapeKey.storage = static_cast<std::uint32_t>(-1);
        collector->result.hitBodyInfo.m_shapeMaterialId.value = static_cast<std::uint16_t>(-1);
        collector->result.hitBodyInfo.m_shapeCollisionFilterInfo.storage = 0;
        collector->result.hitBodyInfo.m_shapeUserData.storage = 0;
        collector->hasHit = false;
        
        // Lock world and perform cast - RAII prevents lock leaks on exception paths
        WorldReadLock worldLock(bhkWorld);
        if (!worldLock.IsLocked()) {
            spdlog::warn("[SPHERECAST-NATIVE] Failed to acquire world read lock");
            return result;
        }

        bool hasHit = false;
        bool castSucceeded = CallCastShapeWithSEH(worldNP, query, castOrientation, collector, &hasHit);

        if (castSucceeded && hasHit) {
            const RE::hknpCollisionResult& hit = collector->result;

            result.hit = true;
            result.hitFraction = hit.fraction.storage;
            result.hitPoint.x = origin.x + normalizedDir.x * maxDistance * result.hitFraction;
            result.hitPoint.y = origin.y + normalizedDir.y * maxDistance * result.hitFraction;
            result.hitPoint.z = origin.z + normalizedDir.z * maxDistance * result.hitFraction;
            result.hitNormal.x = hit.normal.x;
            result.hitNormal.y = hit.normal.y;
            result.hitNormal.z = hit.normal.z;
            result.bodyId = hit.hitBodyInfo.m_bodyId;

            // Try to get TESObjectREFR from body ID
            result.hitRefr = GetRefrFromBodyId(bhkWorld, result.bodyId);
            if (result.hitRefr) {
                result.hitNode = result.hitRefr->Get3D();
            }

            spdlog::trace("[SPHERECAST-NATIVE] HIT: bodyId={:08X} refr={:08X} fraction={:.3f}",
                result.bodyId.value,
                result.hitRefr ? result.hitRefr->formID : 0,
                result.hitFraction);
        } else if (!castSucceeded) {
            spdlog::trace("[SPHERECAST-NATIVE] Exception occurred");
        } else {
            spdlog::trace("[SPHERECAST-NATIVE] MISS");
        }
        // worldLock released automatically by RAII destructor
        
        return result;
    }

    // Multi-ray sphere cast fallback (original implementation)
    static RaycastResult CastSphereMultiRay(
        const RE::NiPoint3& origin,
        const RE::NiPoint3& normalizedDir,
        float radius,
        float maxDistance,
        RE::TESObjectREFR* ignoreRefr)
    {
        // First try the center ray
        RaycastResult bestResult = CastRay(origin, normalizedDir, maxDistance, ignoreRefr);
        
        // If radius is negligible, just return center ray result
        if (radius < 0.1f) {
            return bestResult;
        }
        
        // Get perpendicular basis vectors for the cone pattern
        RE::NiPoint3 right, up;
        GetPerpendicularBasis(normalizedDir, right, up);
        
        // Cast rays in a pattern around the center
        // 8 rays in a circle at the sphere radius
        constexpr int numRays = 8;
        constexpr float PI = 3.14159265f;
        
        for (int i = 0; i < numRays; ++i) {
            float angle = (2.0f * PI * i) / numRays;
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            
            // Offset origin by radius in the perpendicular plane
            RE::NiPoint3 offsetOrigin;
            offsetOrigin.x = origin.x + radius * (right.x * cosA + up.x * sinA);
            offsetOrigin.y = origin.y + radius * (right.y * cosA + up.y * sinA);
            offsetOrigin.z = origin.z + radius * (right.z * cosA + up.z * sinA);
            
            RaycastResult rayResult = CastRay(offsetOrigin, normalizedDir, maxDistance, ignoreRefr);
            
            // Keep the closest hit
            if (rayResult.hit) {
                if (!bestResult.hit || rayResult.hitFraction < bestResult.hitFraction) {
                    bestResult = rayResult;
                }
            }
        }
        
        return bestResult;
    }

    RaycastResult CastSphere(
        const RE::NiPoint3& origin,
        const RE::NiPoint3& direction,
        float radius,
        float maxDistance,
        RE::TESObjectREFR* ignoreRefr)
    {
        // Normalize the main direction
        RE::NiPoint3 normalizedDir = direction;
        float length = std::sqrt(normalizedDir.x * normalizedDir.x + 
                                  normalizedDir.y * normalizedDir.y + 
                                  normalizedDir.z * normalizedDir.z);
        if (length > 0.0f) {
            normalizedDir.x /= length;
            normalizedDir.y /= length;
            normalizedDir.z /= length;
        }
        
        // Try native Havok sphere cast first (like HIGGS Skyrim's LinearCast)
        if (g_useNativeSphereCast) {
            RaycastResult nativeResult = CastSphereNative(origin, normalizedDir, radius, maxDistance, ignoreRefr);
            if (nativeResult.hit) {
                // Check if we should ignore this reference
                if (ignoreRefr && nativeResult.hitRefr == ignoreRefr) {
                    // Native hit the ignored refr - return miss (no fallback)
                    spdlog::trace("[SPHERECAST] Native hit ignored refr, returning miss");
                    return RaycastResult();
                }
                return nativeResult;
            }
            // Native miss - fall through to multi-ray fallback
        }
        
        // Multi-ray fallback - safer than native Havok queries
        RaycastResult bestResult = CastSphereMultiRay(origin, normalizedDir, radius, maxDistance, ignoreRefr);
        
        if (bestResult.hit) {
            spdlog::trace("[SPHERECAST] HIT: refr={:08X} fraction={:.3f} radius={:.2f}",
                bestResult.hitRefr ? bestResult.hitRefr->formID : 0,
                bestResult.hitFraction, radius);
        } else {
            spdlog::trace("[SPHERECAST] MISS: radius={:.2f} dist={:.2f}", radius, maxDistance);
        }
        
        return bestResult;
    }

    // Helper: Calculate distance squared between two points
    static float DistanceSquared(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    // Forward declaration - defined below
    static bool IsBlacklistedByName(RE::TESObjectREFR* refr);

    // ==================================================================================
    // NATIVE HAVOK PHYSICS QUERY FOR NEARBY OBJECTS (HIGGS-style)
    // ==================================================================================
    // Uses hknpWorld::castShape with hknpAllHitsCollector to find all physics bodies
    // in a radius, then converts body IDs to TESObjectREFRs. This is the same approach
    // HIGGS Skyrim uses with hkpWorld_LinearCast + CdPointCollector.
    // ==================================================================================

    // SEH-compatible helper for castShape with AllHits collector
    static bool CallCastShapeAllHitsWithSEH(
        void* worldNP,
        hknpShapeCastQuery& query,
        hkRotationf& castOrientation,
        void* collector)
    {
        __try {
            hknpWorld_castShape(worldNP, query, castOrientation, collector, nullptr);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            spdlog::warn("[PHYSICS-NATIVE] Exception during AllHits castShape");
            // Don't disable - keep testing
            return false;
        }
    }

    // Native implementation using Havok physics queries
    static std::vector<RE::TESObjectREFR*> GetObjectsInRadiusNative(
        const RE::NiPoint3& center,
        float radius,
        RE::TESObjectREFR* ignoreRefr)
    {
        std::vector<RE::TESObjectREFR*> objects;
        
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return objects;
        }
        
        auto* bhkWorld = GetWorldFromCell(player->parentCell);
        void* worldNP = GetWorldNPRaw(bhkWorld);
        if (!worldNP) {
            return objects;
        }
        
        // Get or create the cached sphere shape
        RE::hknpShape* sphereShape = GetCachedSphereShape();
        if (!sphereShape) {
            return objects;
        }
        
        // Build a shape cast query with zero cast vector (point query)
        // This will find all bodies that the sphere overlaps at the center position
        // CRITICAL: Havok uses scaled coordinates (game coords * HAVOK_WORLD_SCALE)
        hknpShapeCastQuery query{};
        query.filter = nullptr;
        query.filterData.materialId.value = 0;
        query.filterData.collisionFilterInfo = 0x2C;  // CustomPick2 layer
        query.filterData.userData = 0;
        query.levelOfDetail = 0;
        query.shape = sphereShape;
        query.origin = RE::hkVector4f(
            center.x * HAVOK_WORLD_SCALE, 
            center.y * HAVOK_WORLD_SCALE, 
            center.z * HAVOK_WORLD_SCALE, 
            0.0f);
        // Cast in all directions by doing a small cast - this gets overlapping bodies
        query.castVector = RE::hkVector4f(0.0f, 0.0f, 0.01f * HAVOK_WORLD_SCALE, 0.0f);  // Tiny cast (scaled)
        query.orientation.setIdentity();
        query.flags = 0;
        query.scale = radius * HAVOK_WORLD_SCALE;  // Scale sphere to the query radius (in Havok units)
        
        hkRotationf castOrientation;
        castOrientation.setIdentity();
        
        // Use hknpClosestHitCollector instead of AllHitsCollector
        // ClosestHit works reliably, AllHits has structure issues
        // For object selection, we only need one hit per cast anyway
        struct AlignedDeleter2 { void operator()(void* p) const { _aligned_free(p); } };
        constexpr size_t kCollectorSize2 = 0x90;
        std::unique_ptr<std::uint8_t, AlignedDeleter2> collectorStorage(
            static_cast<std::uint8_t*>(_aligned_malloc(kCollectorSize2, 16)));
        if (!collectorStorage) {
            spdlog::error("[PHYSICS] GetObjectsInRadiusNative: Failed to allocate collector");
            return objects;
        }
        std::memset(collectorStorage.get(), 0, kCollectorSize2);
        auto* collector = reinterpret_cast<RE::hknpClosestHitCollector*>(collectorStorage.get());

        // Set vtable pointer to ClosestHitCollector (this works!)
        *reinterpret_cast<void**>(collector) = hknpClosestHitCollector_vtable.get();

        // Initialize base class members (hknpCollisionQueryCollector)
        collector->hints = 0;
        collector->earlyOutThreshold.real = _mm_set1_ps((std::numeric_limits<float>::max)());

        // Initialize result struct
        collector->result.queryType.reset();
        collector->result.fraction.storage = (std::numeric_limits<float>::max)();
        collector->result.position = RE::NiPoint3();
        collector->result.normal = RE::NiPoint3();
        collector->result.queryBodyInfo.m_bodyId.value = 0x7FFFFFFF;
        collector->result.queryBodyInfo.m_shapeKey.storage = static_cast<std::uint32_t>(-1);
        collector->result.queryBodyInfo.m_shapeMaterialId.value = static_cast<std::uint16_t>(-1);
        collector->result.queryBodyInfo.m_shapeCollisionFilterInfo.storage = 0;
        collector->result.queryBodyInfo.m_shapeUserData.storage = 0;
        collector->result.hitBodyInfo.m_bodyId.value = 0x7FFFFFFF;
        collector->result.hitBodyInfo.m_shapeKey.storage = static_cast<std::uint32_t>(-1);
        collector->result.hitBodyInfo.m_shapeMaterialId.value = static_cast<std::uint16_t>(-1);
        collector->result.hitBodyInfo.m_shapeCollisionFilterInfo.storage = 0;
        collector->result.hitBodyInfo.m_shapeUserData.storage = 0;
        collector->hasHit = false;
        
        // DIAGNOSTIC: Log everything before the call
        void* vtableAddr = hknpClosestHitCollector_vtable.get();
        spdlog::info("[PHYSICS-NATIVE] Pre-cast: worldNP={:p} shape={:p} vtable={:p}",
            worldNP, (void*)query.shape, vtableAddr);
        spdlog::info("[PHYSICS-NATIVE] Query: origin=({:.3f},{:.3f},{:.3f}) cast=({:.3f},{:.3f},{:.3f}) scale={:.3f}",
            query.origin.x, query.origin.y, query.origin.z,
            query.castVector.x, query.castVector.y, query.castVector.z,
            query.scale);
        
        // Lock world and perform cast - RAII prevents lock leaks on exception paths
        WorldReadLock worldLock(bhkWorld);
        if (!worldLock.IsLocked()) {
            spdlog::warn("[PHYSICS-NATIVE] Failed to acquire world read lock");
            return objects;
        }

        bool castSucceeded = CallCastShapeAllHitsWithSEH(worldNP, query, castOrientation, collectorStorage.get());

        if (castSucceeded && collector->hasHit) {
            // ClosestHitCollector only returns ONE hit - the closest
            const RE::hknpCollisionResult& hit = collector->result;
            RE::hknpBodyId bodyId = hit.hitBodyInfo.m_bodyId;

            spdlog::debug("[PHYSICS-NATIVE] GetObjectsInRadius: center=({:.1f},{:.1f},{:.1f}) radius={:.1f} -> 1 hit (closest)",
                center.x, center.y, center.z, radius);

            if (bodyId.value != INVALID_BODY_ID) {
                // Convert body ID to TESObjectREFR
                RE::TESObjectREFR* refr = GetRefrFromBodyId(bhkWorld, bodyId);
                if (refr && refr != player && refr != ignoreRefr && !refr->IsDeleted() && !refr->IsDisabled()) {
                    auto* baseObj = refr->data.objectReference;
                    bool skipObject = false;

                    if (baseObj && Utils::IsPlayerInPowerArmor()) {
                        auto formType = baseObj->GetFormType();
                        // Skip Power Armor pieces when player is in Power Armor
                        if (formType == RE::ENUM_FORM_ID::kARMO && Utils::IsPowerArmorPiece(baseObj)) {
                            skipObject = true;  // Grabbing PA pieces causes armor to fall off
                        }
                        // Skip weapons when player is in Power Armor - PA has its own weapon system
                        else if (formType == RE::ENUM_FORM_ID::kWEAP) {
                            skipObject = true;  // Let native game handle PA weapon pickup (minigun, etc.)
                        }
                    }

                    if (!skipObject) {
                        objects.push_back(refr);
                    }
                }
            }
        } else if (castSucceeded) {
            spdlog::debug("[PHYSICS-NATIVE] GetObjectsInRadius: center=({:.1f},{:.1f},{:.1f}) radius={:.1f} -> no hits",
                center.x, center.y, center.z, radius);
        }
        // worldLock released automatically by RAII destructor
        
        return objects;
    }

    // Grabbable form types for filtering (used by fallback)
    static const std::unordered_set<RE::ENUM_FORM_ID> grabbableTypes = {
        RE::ENUM_FORM_ID::kMISC,
        RE::ENUM_FORM_ID::kWEAP,
        RE::ENUM_FORM_ID::kARMO,
        RE::ENUM_FORM_ID::kBOOK,
        RE::ENUM_FORM_ID::kINGR,
        RE::ENUM_FORM_ID::kALCH,
        RE::ENUM_FORM_ID::kAMMO,
        RE::ENUM_FORM_ID::kKEYM,
        RE::ENUM_FORM_ID::kNOTE,
        RE::ENUM_FORM_ID::kCMPO,
        RE::ENUM_FORM_ID::kMSTT,
        RE::ENUM_FORM_ID::kLIGH,
    };

    // Fallback implementation using cell reference iteration
    static std::vector<RE::TESObjectREFR*> GetObjectsInRadiusFallback(
        const RE::NiPoint3& center,
        float radius,
        RE::TESObjectREFR* ignoreRefr)
    {
        std::vector<RE::TESObjectREFR*> objects;
        objects.reserve(8);

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return objects;
        }

        RE::TESObjectCELL* cell = player->parentCell;
        float radiusSq = radius * radius;
        
        const std::uint32_t refCount = static_cast<std::uint32_t>(cell->references.size());
        for (std::uint32_t i = 0; i < refCount; ++i) {
            RE::TESObjectREFR* refr = cell->references[i].get();
            if (!refr) continue;
            if (refr == ignoreRefr) continue;
            if (refr == player) continue;
            if (refr->IsDeleted() || refr->IsDisabled()) continue;
            
            auto baseObj = refr->data.objectReference;
            if (!baseObj) continue;
            
            auto formType = baseObj->GetFormType();
            if (grabbableTypes.find(formType) == grabbableTypes.end()) continue;
            
            if (formType == RE::ENUM_FORM_ID::kMSTT && IsBlacklistedByName(refr)) continue;
            
            // Skip items when player is in Power Armor that would cause issues
            bool inPA = Utils::IsPlayerInPowerArmor();
            if (inPA) {
                // Skip Power Armor pieces - grabbing causes armor to fall off
                if (formType == RE::ENUM_FORM_ID::kARMO && Utils::IsPowerArmorPiece(baseObj)) {
                    spdlog::debug("[PA-FILTER] Skipping PA piece '{}' in Power Armor", 
                        refr->GetDisplayFullName() ? refr->GetDisplayFullName() : "unknown");
                    continue;
                }
                // Skip weapons - PA has its own weapon mount system (minigun, etc.)
                if (formType == RE::ENUM_FORM_ID::kWEAP) {
                    spdlog::debug("[PA-FILTER] Skipping weapon '{}' in Power Armor - using native behavior", 
                        refr->GetDisplayFullName() ? refr->GetDisplayFullName() : "unknown");
                    continue;
                }
            }
            
            RE::NiPoint3 refrPos = refr->GetPosition();
            float roughDistSq = DistanceSquared(center, refrPos);
            
            constexpr float marginFactor = 1.5f;
            if (roughDistSq > radiusSq * (marginFactor * marginFactor)) continue;
            
            if (auto* obj3d = refr->Get3D()) {
                refrPos = obj3d->world.translate;
                roughDistSq = DistanceSquared(center, refrPos);
            }
            
            if (roughDistSq <= radiusSq) {
                objects.push_back(refr);
            }
        }
        
        return objects;
    }

    std::vector<RE::TESObjectREFR*> GetObjectsInRadius(
        const RE::NiPoint3& center,
        float radius,
        RE::TESObjectREFR* ignoreRefr)
    {
        std::vector<RE::TESObjectREFR*> objects;
        
        // Check flag to decide which implementation to use
        if (g_useNativeObjectsQuery) {
            // Native Havok query using ClosestHitCollector
            objects = GetObjectsInRadiusNative(center, radius, ignoreRefr);
        } else {
            // Fallback: iterate cell references (safer but slower)
            objects = GetObjectsInRadiusFallback(center, radius, ignoreRefr);
        }
        
        spdlog::trace("[PHYSICS] GetObjectsInRadius: center=({:.2f},{:.2f},{:.2f}) radius={:.2f} found={} (native={})",
            center.x, center.y, center.z, radius, objects.size(), g_useNativeObjectsQuery);

        return objects;
    }

    bool HasPhysics([[maybe_unused]] RE::TESObjectREFR* refr)
    {
        if (!refr) {
            return false;
        }

        auto obj3d = refr->Get3D();
        if (!obj3d) {
            return false;
        }

        // Check for collision object via the NiAVObject member
        // In CommonLibF4VR: NiAVObject has NiPointer<NiCollisionObject> collisionObject at 0x100
        return obj3d->collisionObject.get() != nullptr;
    }

    // Helper to check if an object name contains any blacklisted words (case-insensitive)
    // Used to filter out large static objects like cars, bollards, barriers, etc.
    // OPTIMIZED: Uses hash sets for O(1) word lookups and caches results by formID
    static bool IsBlacklistedByNameUncached(RE::TESObjectREFR* refr)
    {
        // Get base object name
        auto baseObj = refr->data.objectReference;
        if (!baseObj) return false;
        
        // Get the name using TESFullName::GetFullName (returns string_view), then fall back to EditorID
        std::string nameStr;
        auto fullName = RE::TESFullName::GetFullName(*baseObj, false);
        if (!fullName.empty()) {
            nameStr = std::string(fullName);  // Convert string_view to string
        }
        if (nameStr.empty()) {
            const char* editorId = baseObj->GetFormEditorID();
            if (editorId && editorId[0] != '\0') {
                nameStr = editorId;
            }
        }
        if (nameStr.empty()) return false;
        
        // Convert to lowercase for case-insensitive matching
        for (char& c : nameStr) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        
        // WHITELIST - if name contains these words, it's a small grabbable item
        // Check whitelist FIRST to allow "toy truck", "mini car", etc.
        // Using static unordered_set for O(1) contains checks
        static const std::unordered_set<std::string> whitelistWords = {
            "toy", "mini", "model", "small", "little", "tiny",
            "bottle", "cup", "mug", "glass", "can",
            "plate", "bowl", "dish",
            "ashtray", "cigarette", "cigar",
            "pencil", "pen", "clipboard",
            "ball", "globe",
            "alarm", "clock",
            "phone", "telephone",
            "radio",
            "lamp",
            "figurine", "statue",
            "nuka",
        };
        
        // BLACKLIST - large static objects that shouldn't be grabbed
        static const std::unordered_set<std::string> blacklistWords = {
            "car", "vehicle", "truck", "bus", "van", "vertibird",
            "bollard", "barrier", "barricade",
            "sandbag", "sandbags",
            "crate", "container",
            "dumpster", "trashcan",
            "mailbox",
            "barrel",
            "tank",
            "generator",
            "safe",
            "locker",
            "cabinet",
            "desk",
            "shelf", "shelving",
            "bench",
            "chair",
            "couch", "sofa",
            "bed",
            "table",
            "counter",
            "sink",
            "toilet",
            "fridge", "refrigerator",
            "washer", "dryer",
            "stove", "oven",
            "terminal",
            "turret",
            "fence", "fencing",
            "railing",
            "pipe",
            "duct",
            "vent",
            "sign",
            "pole",
            "post",
            "skeleton",
            "corpse", "body",
        };
        
        // OPTIMIZED: Split name into words and check each word against hash sets
        // This is O(words * avg_word_len) instead of O(whitelist_size * name_len)
        std::string currentWord;
        for (size_t i = 0; i <= nameStr.size(); ++i) {
            char c = (i < nameStr.size()) ? nameStr[i] : ' ';
            if (std::isalnum(static_cast<unsigned char>(c))) {
                currentWord += c;
            } else if (!currentWord.empty()) {
                // Check whitelist first - if ANY word matches, allow grab
                if (whitelistWords.count(currentWord)) {
                    return false;  // NOT blacklisted
                }
                // Check blacklist
                if (blacklistWords.count(currentWord)) {
                    return true;  // Blacklisted
                }
                currentWord.clear();
            }
        }
        
        return false;  // Not blacklisted by default
    }
    
    // Cached version of IsBlacklistedByName - uses formID as cache key with LRU eviction
    static bool IsBlacklistedByName(RE::TESObjectREFR* refr)
    {
        if (!refr) return false;
        
        RE::TESFormID formID = refr->formID;
        double currentTime = heisenberg::Utils::GetTime();
        
        // Try to read from cache first (shared lock for reads)
        {
            std::shared_lock readLock(g_blacklistCacheMutex);
            auto it = g_blacklistCache.find(formID);
            if (it != g_blacklistCache.end()) {
                // Note: We don't update lastAccessTime on read to avoid write lock
                // This is an approximation of LRU - actual LRU would need upgrade to write lock
                return it->second.isBlacklisted;  // Cache hit!
            }
        }
        
        // Cache miss - compute and store result
        bool isBlacklisted = IsBlacklistedByNameUncached(refr);
        
        {
            std::unique_lock writeLock(g_blacklistCacheMutex);
            // Evict oldest entries if cache is full (LRU-style)
            if (g_blacklistCache.size() >= MAX_CACHE_SIZE) {
                EvictOldestEntries();
            }
            g_blacklistCache[formID] = BlacklistCacheEntry{ isBlacklisted, currentTime };
        }
        
        return isBlacklisted;
    }

    // Helper to check if an object has ragdoll/articulated physics (multiple bodies)
    // Ragdoll objects have >1 body and should not be grabbed with our simple keyframed method
    // OPTIMIZATION: Only check for form types that CAN have ragdolls (actors, creatures)
    // Inventory items (MISC, WEAP, etc) never have ragdolls.
    static bool HasRagdollPhysics(RE::TESObjectREFR* refr, RE::ENUM_FORM_ID formType)
    {
        // OPTIMIZATION: Only actors/NPCs can have ragdolls
        // Skip the expensive physics check for inventory items
        if (formType != RE::ENUM_FORM_ID::kACHR && 
            formType != RE::ENUM_FORM_ID::kNPC_ &&
            formType != RE::ENUM_FORM_ID::kMSTT)  // kMSTT can have skeleton debris
        {
            return false;  // Inventory items never have ragdolls
        }
        
        auto obj3d = refr->Get3D();
        if (!obj3d) return false;
        
        auto* collisionObj = obj3d->collisionObject.get();
        if (!collisionObj) return false;
        
        // Cast to bhkNPCollisionObject to access spSystem
        auto* npCollObj = reinterpret_cast<RE::bhkNPCollisionObject*>(collisionObj);
        if (!npCollObj->spSystem) return false;
        
        // Get number of bodies in the physics system
        void* physicsSystem = npCollObj->spSystem.get();
        if (!physicsSystem) return false;
        
        std::uint32_t numBodies = heisenberg::ConstraintFunctions::BhkPhysicsSystemGetNumBodies(physicsSystem);
        
        // Ragdolls have multiple bodies (one per bone)
        // Normal objects have exactly 1 body
        if (numBodies > 1) {
            spdlog::trace("[PHYSICS] ref {:08X}: Has {} physics bodies (ragdoll)", refr->formID, numBodies);
            return true;
        }
        
        return false;
    }

    // Helper to check if an object has DYNAMIC physics (like Skyrim HIGGS)
    // Only DYNAMIC objects should be grabbable - STATIC/KEYFRAMED are world geometry
    static bool IsDynamicPhysics(RE::TESObjectREFR* refr)
    {
        __try {
            auto obj3d = refr->Get3D();
            if (!obj3d) return false;
            
            auto* collisionObj = obj3d->collisionObject.get();
            if (!collisionObj) return false;
            
            // Cast to bhkNPCollisionObject
            auto* npCollObj = reinterpret_cast<RE::bhkNPCollisionObject*>(collisionObj);
            if (!npCollObj) return false;
            
            // Check spSystem pointer AND its internal system pointer
            if (!npCollObj->spSystem) return false;
            auto* physSystem = npCollObj->spSystem.get();
            if (!physSystem) return false;
            
            // Additional safety: check systemBodyIdx is reasonable
            // (crash was in GetBodyId with invalid index)
            if (npCollObj->systemBodyIdx == 0xFFFFFFFF || npCollObj->systemBodyIdx > 0xFFFF) {
                spdlog::info("[PHYSICS] ref {:08X}: Invalid systemBodyIdx=0x{:X}", refr->formID, npCollObj->systemBodyIdx);
                return false;
            }
            
            // Use AccessBody to get the hknpBody, then read motion properties ID
            void* body = heisenberg::bhkNPCollisionObject_AccessBody(npCollObj);
            if (!body) return false;
            
            // Read motionId from body (offset 0x04)
            std::uint32_t motionId = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(body) + heisenberg::Offsets::hknpBody_motionId);
            
            // STATIC bodies have invalid motionId (0xFFFFFFFF or very high values)
            // Only DYNAMIC bodies have valid motion indices
            if (motionId == 0xFFFFFFFF || motionId > 0x00FFFFFF) {
                spdlog::info("[PHYSICS] ref {:08X}: STATIC (invalid motionId=0x{:X})", refr->formID, motionId);
                return false;  // Static body - not grabbable
            }
            
            // Get world to access motion
            void* world = heisenberg::bhkNPCollisionObject_AccessWorld(npCollObj);
            if (!world) return false;
            
            // Get motion from world - with additional validation
            void* motion = heisenberg::hknpBSWorld_accessMotion(world, motionId);
            if (!motion) {
                spdlog::info("[PHYSICS] ref {:08X}: No motion for motionId={}", refr->formID, motionId);
                return false;
            }
            
            // Validate motion pointer before reading
            // Motion struct should be at least 0x10 bytes
            if (reinterpret_cast<std::uintptr_t>(motion) < 0x10000) {
                spdlog::info("[PHYSICS] ref {:08X}: Invalid motion pointer 0x{:X}", refr->formID, 
                             reinterpret_cast<std::uintptr_t>(motion));
                return false;
            }
            
            // Read motionPropertiesId from motion (offset 0x02)
            std::uint16_t motionPropsId = *reinterpret_cast<std::uint16_t*>(
                reinterpret_cast<std::uintptr_t>(motion) + heisenberg::Offsets::hknpMotion_motionPropertiesId);
            
            // In hknp: 0=STATIC, 1=DYNAMIC, 2=KEYFRAMED
            // Only DYNAMIC (1) should be grabbable
            bool isDynamic = (motionPropsId == heisenberg::hknpMotionPropertiesId::DYNAMIC);
            
            // TEMP: Always log motion type for debugging
            spdlog::info("[PHYSICS] ref {:08X}: motionPropsId={} (0=STATIC,1=DYNAMIC,2=KEYFRAMED) isDynamic={}", 
                         refr->formID, motionPropsId, isDynamic);
            
            return isDynamic;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            // Access violation or other SEH exception - assume not dynamic
            spdlog::warn("[PHYSICS] ref {:08X}: Exception in IsDynamicPhysics, assuming not grabbable", refr->formID);
            return false;
        }
    }

    bool IsGrabbable(RE::TESObjectREFR* refr)
    {
        if (!refr) {
            return false;
        }

        // OPTIMIZATION: Get base object and form type FIRST (cheap check)
        // This allows early-out before expensive physics checks
        auto baseObj = refr->data.objectReference;
        if (!baseObj) {
            return false;
        }

        auto formType = baseObj->GetFormType();
        
        // Use the namespace-level grabbableTypes set for O(1) lookup
        // Early out if not a grabbable type
        if (grabbableTypes.find(formType) == grabbableTypes.end()) {
            return false;
        }
        
        // kMSTT needs blacklist check BEFORE physics (cheaper than physics check)
        if (formType == RE::ENUM_FORM_ID::kMSTT) {
            if (IsBlacklistedByName(refr)) {
                return false;
            }
        }

        // Now do physics checks (more expensive) — acquire world read lock
        // to prevent torn reads while Havok physics thread is updating
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return false;

        auto* bhkWorld = heisenberg::TESObjectCell_GetbhkWorld(player->parentCell);
        if (bhkWorld) {
            WorldReadLock lock(bhkWorld);
            if (!lock.IsLocked()) return false;

            if (!HasPhysics(refr)) {
                return false;
            }

            // RAGDOLL CHECK: Pass formType to avoid redundant checks for non-ragdoll types
            if (HasRagdollPhysics(refr, formType)) {
                return false;
            }
        } else {
            // No physics world — fallback to unprotected check
            if (!HasPhysics(refr)) return false;
            if (HasRagdollPhysics(refr, formType)) return false;
        }

        return true;
    }

    void ApplyImpulse(
        [[maybe_unused]] RE::TESObjectREFR* refr,
        [[maybe_unused]] const RE::NiPoint3& impulse,
        [[maybe_unused]] const RE::NiPoint3& point)
    {
        if (!refr) {
            return;
        }

        // TODO: Get rigid body and apply impulse via hknp motion manager
        spdlog::trace("[PHYSICS] ApplyImpulse: not yet implemented");
    }

    void SetVelocity(
        [[maybe_unused]] RE::TESObjectREFR* refr,
        [[maybe_unused]] const RE::NiPoint3& linear,
        [[maybe_unused]] const RE::NiPoint3& angular)
    {
        if (!refr) {
            return;
        }

        // TODO: Set velocity via hknp motion manager
        spdlog::trace("[PHYSICS] SetVelocity: not yet implemented");
    }

    // ==================================================================================
    // WORLD LOCKING IMPLEMENTATION
    // ==================================================================================
    // Based on Havok tutorials (mmmovania HavokPhysicsTutorials):
    //   https://github.com/mmmovania/HavokPhysicsTutorials
    //
    // Key insight: Havok physics runs on a separate thread. Any modifications to
    // physics state (body creation, motion type changes, transform updates) MUST
    // be protected by world locks to prevent race conditions and crashes.
    //
    // Pattern from tutorials:
    //   void HandleMouseDown(int x, int y) {
    //       ...
    //       g_pWorld->lock();  // CRITICAL: Lock before modification
    //       gSelectedActor->setMotionType(hkpMotion::MOTION_FIXED);
    //       g_pWorld->unlock();
    //   }
    //
    //   void MoveActor(const hkVector4& position) {
    //       g_pWorld->lock();
    //       gSelectedActor->setTransform(trans);
    //       g_pWorld->unlock();
    //   }
    //
    //   void HandleMouseUp() {
    //       g_pWorld->lock();
    //       gSelectedActor->setMotionType(hkpMotion::MOTION_DYNAMIC);
    //       g_pWorld->unlock();
    //   }
    //
    // In F4VR, the lock is a BSReadWriteLock at offset 0x6D8 in hknpBSWorld.
    // ==================================================================================

    void* GetHknpWorldFromBhk(RE::bhkWorld* bhkWorld)
    {
        return GetWorldNPRaw(bhkWorld);
    }

    bool LockWorldForRead(RE::bhkWorld* bhkWorld)
    {
        auto* world = GetWorldNPRaw(bhkWorld);
        auto* lock = GetWorldLock(world);
        if (lock) {
            lock->lock_read();
            return true;
        }
        return false;
    }

    void UnlockWorldRead(RE::bhkWorld* bhkWorld)
    {
        auto* world = GetWorldNPRaw(bhkWorld);
        auto* lock = GetWorldLock(world);
        if (lock) {
            lock->unlock_read();
        }
    }

    bool LockWorldForWrite(RE::bhkWorld* bhkWorld)
    {
        auto* world = GetWorldNPRaw(bhkWorld);
        auto* lock = GetWorldLock(world);
        if (lock) {
            lock->lock_write();
            return true;
        }
        return false;
    }

    void UnlockWorldWrite(RE::bhkWorld* bhkWorld)
    {
        auto* world = GetWorldNPRaw(bhkWorld);
        auto* lock = GetWorldLock(world);
        if (lock) {
            lock->unlock_write();
        }
    }

    // ==================================================================================
    // RAII LOCK IMPLEMENTATIONS
    // ==================================================================================

    // Helper to validate that a bhkWorld pointer is still valid
    static bool IsWorldValid(RE::bhkWorld* bhkWorld)
    {
        if (!bhkWorld) return false;
        
        // Try to read from the world to validate it
        // Check if the internal hknpWorld pointer is non-null
        auto* worldNP = GetWorldNPRaw(bhkWorld);
        if (!worldNP) return false;
        
        return true;
    }

    WorldReadLock::WorldReadLock(RE::bhkWorld* bhkWorld)
    {
        if (bhkWorld && IsWorldValid(bhkWorld)) {
            auto* world = GetWorldNPRaw(bhkWorld);
            _lock = GetWorldLock(world);
            if (_lock) {
                _lock->lock_read();
                _locked = true;
            }
        }
    }

    WorldReadLock::~WorldReadLock()
    {
        // Always unlock if we successfully locked — don't re-check IsWorldValid (TOCTOU fix)
        if (_locked && _lock) {
            _lock->unlock_read();
        }
    }

    WorldWriteLock::WorldWriteLock(RE::bhkWorld* bhkWorld)
    {
        if (bhkWorld && IsWorldValid(bhkWorld)) {
            auto* world = GetWorldNPRaw(bhkWorld);
            _lock = GetWorldLock(world);
            if (_lock) {
                _lock->lock_write();
                _locked = true;
            }
        }
    }

    WorldWriteLock::~WorldWriteLock()
    {
        // Always unlock if we successfully locked — don't re-check IsWorldValid (TOCTOU fix)
        if (_locked && _lock) {
            _lock->unlock_write();
        }
    }

    // ==================================================================================
    // HIGGS-STYLE PAIR COLLISION FILTERING
    // ==================================================================================

    void DisableCollisionsBetween(void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB)
    {
        // DEAD PATH (see DisableCollisionBetween): the engine pair-filter call
        // hknpPairCollisionFilter::disableCollisionsBetween (0x196de70) faults on
        // every invocation on F4VR 1.2.72 — wrong mechanism for this build. Unlike
        // its SEH-guarded sibling, THIS variant has no __try, so a live call would
        // hard-crash. Neutralized to a no-op. Collider-vs-player exclusion is done
        // via the layer-43 / 0x000B002B filter approach instead.
        (void)hknpWorld; (void)bodyIdA; (void)bodyIdB;
        static std::atomic<bool> warned{ false };
        if (!warned.exchange(true)) {
            spdlog::warn("[PHYSICS] DisableCollisionsBetween is a no-op (engine pair-filter faults on F4VR; using layer-43 filtering instead)");
        }
    }

    // Saved original player collision filter info for restoration
    // Thread safety: accessed from physics thread and main thread
    static std::atomic<std::uint32_t> g_savedPlayerCollisionFilterInfo{0};
    static std::atomic<bool> g_playerCollisionModified{false};
    static std::atomic<std::uint32_t> g_playerBodyId{INVALID_BODY_ID};  // Cached player body ID

    bool TryFindPlayerBodyAlternative(void* bhkWorld, void* hknpWorld, std::uint32_t& outBodyId);
    bool TryReadBodyFilterInfo(void* hknpWorld, std::uint32_t bodyId, std::uint32_t& outFilterInfo);
    bool TryReadMiddleHighCharControllerImpl(void* middleHigh, RE::bhkCharacterController*& outController);
    bool TryGetCharacterControllerBodyId(RE::bhkCharacterController* charController, void* hknpWorld, std::uint32_t& outBodyId);
    bool TryFindPlayerBodyViaProxyManager(RE::bhkWorld* bhkWorld, void* hknpWorld, std::uint32_t& outBodyId);

    void InvalidatePlayerBodyId()
    {
        g_playerBodyId = INVALID_BODY_ID;
    }

    // bhkCharacterController::SetSupportBody(bhkNPCollisionObject*) — VR 0x1e23150.
    // Ghidra-confirmed: the player character proxy caches its "ground" support body
    // REFCOUNTED (refCount++ on set, refCount-- + DeleteThis() at 0 on replace). If we
    // free one of our keyframed ROCK collider bodies while the proxy still caches it as
    // support, the proxy is left with a dangling supportBody._ptr -> next-frame vf066
    // reads the freed object's vtable -> the recurring bhkCharProxyController crash.
    using SetSupportBody_t = void(*)(RE::bhkCharacterController*, void* /*bhkNPCollisionObject*/);
    inline REL::Relocation<SetSupportBody_t> bhkCharacterController_SetSupportBody{ REL::Offset(0x1e23150) };

    // Force the player character proxy to drop whatever it currently caches as its
    // support body. MUST be called at the START of any ROCK collider-body teardown —
    // before we release our own refs — so the proxy releases its ref (and runs the
    // final DeleteThis itself if it held the last ref) while our body is still alive,
    // instead of being left pointing at memory we are about to free. Safe to call any
    // time (no-op if the proxy isn't holding a body). SEH-guarded: the engine setter
    // dereferences the cached body's vtable on replace.
    void ClearPlayerProxySupportBody()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto* actor = static_cast<RE::Actor*>(player);
        auto* process = actor ? actor->currentProcess : nullptr;
        auto* middleHigh = process ? process->middleHigh : nullptr;
        if (!middleHigh) return;
        RE::bhkCharacterController* charController = nullptr;
        TryReadMiddleHighCharControllerImpl(middleHigh, charController);
        if (!charController) return;
        __try {
            bhkCharacterController_SetSupportBody(charController, nullptr);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            spdlog::error("[PHYSICS] Exception in ClearPlayerProxySupportBody");
        }
    }

    RE::bhkCharacterController* GetPlayerCharacterController()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        auto* actor = static_cast<RE::Actor*>(player);
        auto* process = actor ? actor->currentProcess : nullptr;
        auto* middleHigh = process ? process->middleHigh : nullptr;
        if (!middleHigh) return nullptr;
        RE::bhkCharacterController* charController = nullptr;
        TryReadMiddleHighCharControllerImpl(middleHigh, charController);
        return charController;
    }

    // =====================================================================
    // HELD-OBJECT MASS / INERTIA (1:1 with ROCK GrabConstraint.cpp)
    // =====================================================================
    // hknpMotion packs 4x bfloat16 (int16) at motion+0x20: [0..2] = inverse
    // inertia diagonal, [3] = inverse mass. ROCK reads mass from packed[3] and
    // clamps the inverse-inertia ratio so long/thin held objects don't spin
    // wildly under the grab motor, then rebuilds the solver's derived mass props.
    namespace {
        constexpr std::ptrdiff_t kMotionPackedInertiaOffset = 0x20;

        inline float UnpackBfloat16(std::int16_t packed)
        {
            std::uint32_t bits = static_cast<std::uint32_t>(static_cast<std::uint16_t>(packed)) << 16;
            float f; std::memcpy(&f, &bits, sizeof(f)); return f;
        }
        inline std::int16_t RepackBfloat16(float value)
        {
            std::int32_t raw; std::memcpy(&raw, &value, sizeof(raw));
            std::int32_t s = raw >> 16;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            return static_cast<std::int16_t>(s);
        }

        // hknpWorld::rebuildMotionMassProperties(hknpMotionId) — VR 0x1546570 (Ghidra).
        using RebuildMotionMassProperties_t = void(*)(void* hknpWorld, std::uint32_t motionId);
        REL::Relocation<RebuildMotionMassProperties_t> hknpWorld_rebuildMotionMassProperties{ REL::Offset(0x1546570) };

        // hknpBody field offsets (Ghidra-verified against AccessBody/setBodyLinearVelocity):
        // AccessBody returns bodies.data + bodyId*0x90; in that body the MOTION INDEX is at
        // +0x68 (NOT the +0x04 in Offsets::hknpBody_motionId, which is wrong and is why mass
        // reads returned 0). accessMotion(world, motionIndex) -> motions.data + idx*0x80.
        constexpr std::ptrdiff_t kHknpBody_motionIndex = 0x68;
        // hknpMotion velocity offsets (Ghidra): linearVelocity @ +0x40, angularVelocity @ +0x50.
        constexpr std::ptrdiff_t kHknpMotion_linearVelocity = 0x40;
        constexpr std::ptrdiff_t kHknpMotion_angularVelocity = 0x50;

        // Resolve writable hknpMotion + world + motionId for a collision object. False if static/invalid.
        inline bool ResolveMotion(RE::bhkNPCollisionObject* obj, void*& outWorld, void*& outMotion, std::uint32_t& outMotionId)
        {
            outWorld = outMotion = nullptr; outMotionId = 0xFFFFFFFFu;
            if (!obj) return false;
            void* body = heisenberg::bhkNPCollisionObject_AccessBody(obj);
            if (!body) return false;
            std::uint32_t motionId = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<std::uintptr_t>(body) + kHknpBody_motionIndex);
            if (motionId == 0xFFFFFFFFu || motionId > 0x00FFFFFFu) return false;  // static
            void* world = heisenberg::bhkNPCollisionObject_AccessWorld(obj);
            if (!world) return false;
            void* motion = heisenberg::hknpBSWorld_accessMotion(world, motionId);
            if (!motion || reinterpret_cast<std::uintptr_t>(motion) < 0x10000) return false;
            outWorld = world; outMotion = motion; outMotionId = motionId; return true;
        }

        // Leaf SEH wrapper — engine call may deref freed/invalid state.
        bool SafeRebuildMotionMass(void* world, std::uint32_t motionId)
        {
            __try { hknpWorld_rebuildMotionMassProperties(world, motionId); return true; }
            __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    float GetHeldObjectMass(RE::bhkNPCollisionObject* obj)
    {
        void *world, *motion; std::uint32_t motionId;
        if (!ResolveMotion(obj, world, motion, motionId)) {
            spdlog::info("[PHYSICS] GetHeldObjectMass: ResolveMotion failed (obj={:p})", (void*)obj);
            return 0.0f;
        }
        auto* packed = reinterpret_cast<std::int16_t*>(reinterpret_cast<char*>(motion) + kMotionPackedInertiaOffset);
        float invMass = UnpackBfloat16(packed[3]);
        // Diagnostic: dump the raw packed inverse inertia/mass so we can tell whether a
        // "mass=0" held object is genuinely immovable (invMass~0) or a read/timing issue.
        spdlog::info("[PHYSICS] GetHeldObjectMass: motion={} packed=[{},{},{},{}] invI=[{:.3e},{:.3e},{:.3e}] invMass={:.4e}",
                     motionId, packed[0], packed[1], packed[2], packed[3],
                     UnpackBfloat16(packed[0]), UnpackBfloat16(packed[1]), UnpackBfloat16(packed[2]), invMass);
        if (!std::isfinite(invMass) || invMass <= 0.0001f) return 0.0f;
        return 1.0f / invMass;
    }

    // Read the held object's current linear + angular velocity (HAVOK units) from its
    // hknpMotion (linearVelocity @ +0x40, angularVelocity @ +0x50). False if unresolved.
    bool GetHeldObjectVelocity(RE::bhkNPCollisionObject* obj, RE::NiPoint3& outLinearHavok, RE::NiPoint3& outAngularHavok)
    {
        void *world, *motion; std::uint32_t motionId;
        if (!ResolveMotion(obj, world, motion, motionId)) return false;
        const float* lin = reinterpret_cast<const float*>(reinterpret_cast<char*>(motion) + kHknpMotion_linearVelocity);
        const float* ang = reinterpret_cast<const float*>(reinterpret_cast<char*>(motion) + kHknpMotion_angularVelocity);
        outLinearHavok = { lin[0], lin[1], lin[2] };
        outAngularHavok = { ang[0], ang[1], ang[2] };
        return std::isfinite(outLinearHavok.x) && std::isfinite(outAngularHavok.x);
    }

    // Clamp the inverse-inertia ratio to maxRatio (ROCK default 10). Returns true + saved[3]
    // if it modified anything (so EndGrab can restore). No-op return false if ratio already OK.
    bool NormalizeHeldObjectInertia(RE::bhkNPCollisionObject* obj, float maxRatio, std::int16_t savedOut[3])
    {
        void *world, *motion; std::uint32_t motionId;
        if (!ResolveMotion(obj, world, motion, motionId)) return false;
        auto* packed = reinterpret_cast<std::int16_t*>(reinterpret_cast<char*>(motion) + kMotionPackedInertiaOffset);
        savedOut[0] = packed[0]; savedOut[1] = packed[1]; savedOut[2] = packed[2];
        if (packed[0] <= 0 || packed[1] <= 0 || packed[2] <= 0) return false;
        float invI[3] = { UnpackBfloat16(packed[0]), UnpackBfloat16(packed[1]), UnpackBfloat16(packed[2]) };
        if (invI[0] <= 0.0f || invI[1] <= 0.0f || invI[2] <= 0.0f) return false;
        const float minI = (std::min)({ invI[0], invI[1], invI[2] });
        const float maxI = (std::max)({ invI[0], invI[1], invI[2] });
        if (minI <= 0.0f || maxRatio <= 1.0f || (maxI / minI) <= maxRatio) return false;  // no clamp needed
        const float maxAllowed = minI * maxRatio;
        for (int i = 0; i < 3; ++i) if (invI[i] > maxAllowed) invI[i] = maxAllowed;
        packed[0] = RepackBfloat16(invI[0]); packed[1] = RepackBfloat16(invI[1]); packed[2] = RepackBfloat16(invI[2]);
        if (!SafeRebuildMotionMass(world, motionId)) return false;
        spdlog::debug("[PHYSICS] Normalized held-object inertia ratio {:.1f}x -> {:.1f}x (motion {})",
                      maxI / minI, maxRatio, motionId);
        return true;
    }

    void RestoreHeldObjectInertia(RE::bhkNPCollisionObject* obj, const std::int16_t saved[3])
    {
        void *world, *motion; std::uint32_t motionId;
        if (!ResolveMotion(obj, world, motion, motionId)) return;
        auto* packed = reinterpret_cast<std::int16_t*>(reinterpret_cast<char*>(motion) + kMotionPackedInertiaOffset);
        packed[0] = saved[0]; packed[1] = saved[1]; packed[2] = saved[2];
        SafeRebuildMotionMass(world, motionId);
    }

    std::uint32_t GetPlayerBodyId()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            spdlog::debug("[PHYSICS] GetPlayerBodyId: No player singleton");
            return INVALID_BODY_ID;
        }

        auto* actor = static_cast<RE::Actor*>(player);
        auto* cell = player->GetParentCell();
        auto* bhkWorld = cell ? heisenberg::TESObjectCell_GetbhkWorld(cell) : nullptr;
        void* hknpWorld = bhkWorld ? GetHknpWorldFromBhk(bhkWorld) : nullptr;

        std::uint32_t cachedBodyId = g_playerBodyId.load();
        std::uint32_t cachedFilterInfo = 0;
        if (hknpWorld && !IsInvalidBodyId(cachedBodyId) && TryReadBodyFilterInfo(hknpWorld, cachedBodyId, cachedFilterInfo)) {
            return cachedBodyId;
        }
        if (hknpWorld && !IsInvalidBodyId(cachedBodyId)) {
            g_playerBodyId = INVALID_BODY_ID;
        }

        auto* process = actor ? actor->currentProcess : nullptr;
        auto* middleHigh = process ? process->middleHigh : nullptr;
        if (middleHigh && hknpWorld) {
            // VR: middleHigh->charController is at +0x3E8, NOT CommonLibF4's +0x3E0
            RE::bhkCharacterController* charController = nullptr;
            TryReadMiddleHighCharControllerImpl(middleHigh, charController);

            if (!charController) {
                static auto lastLog = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 2) {
                    spdlog::warn("[PHYSICS] GetPlayerBodyId: charController is null (middleHigh={:p})", (void*)middleHigh);
                    lastLog = now;
                }
            } else {
                // Try reading body ID directly from hknpCharacterProxy at charController+0x470
                // The proxy stores m_shapePhantomBodyId at +0x34
                __try {
                    uintptr_t ctrlAddr = reinterpret_cast<uintptr_t>(charController);
                    void* proxy = *reinterpret_cast<void**>(ctrlAddr + 0x470);
                    if (proxy) {
                        std::uint32_t proxyBodyId = *reinterpret_cast<std::uint32_t*>(
                            reinterpret_cast<uintptr_t>(proxy) + 0x34);
                        if (proxyBodyId != 0x7FFFFFFF && proxyBodyId != 0xFFFFFFFF && proxyBodyId != 0) {
                            g_playerBodyId = proxyBodyId;
                            spdlog::info("[PHYSICS] GetPlayerBodyId: Resolved via proxy body ID: 0x{:08X}", proxyBodyId);
                            return proxyBodyId;
                        }
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {}

                // Fallback: try the complex resolution path
                std::uint32_t controllerBodyId = INVALID_BODY_ID;
                if (TryGetCharacterControllerBodyId(charController, hknpWorld, controllerBodyId)) {
                    g_playerBodyId = controllerBodyId;
                    spdlog::info("[PHYSICS] GetPlayerBodyId: Resolved via charController: 0x{:08X}", controllerBodyId);
                    return controllerBodyId;
                } else {
                    static auto lastLog2 = std::chrono::steady_clock::now();
                    auto now2 = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now2 - lastLog2).count() >= 2) {
                        spdlog::warn("[PHYSICS] GetPlayerBodyId: charController found at {:p} but body ID resolution failed", (void*)charController);
                        lastLog2 = now2;
                    }
                }
            }
        }

        if (bhkWorld && hknpWorld) {
            std::uint32_t proxyManagerBodyId = INVALID_BODY_ID;
            if (TryFindPlayerBodyViaProxyManager(bhkWorld, hknpWorld, proxyManagerBodyId)) {
                g_playerBodyId = proxyManagerBodyId;
                spdlog::info("[PHYSICS] GetPlayerBodyId: Found player body via proxy manager: 0x{:08X}", proxyManagerBodyId);
                return proxyManagerBodyId;
            }

            std::uint32_t altBodyId = INVALID_BODY_ID;
            if (TryFindPlayerBodyAlternative(bhkWorld, hknpWorld, altBodyId)) {
                g_playerBodyId = altBodyId;
                spdlog::info("[PHYSICS] GetPlayerBodyId: Found player body via NiNode search: 0x{:08X}", altBodyId);
                return altBodyId;
            }
        }

        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
            spdlog::debug("[PHYSICS] GetPlayerBodyId: No character controller and no alternative player body found yet");
            lastLog = now;
        }
        return INVALID_BODY_ID;
    }
    
    // ==================================================================================
    // PLAYER COLLISION FILTER MODIFICATION
    // Used to temporarily disable player collision with world objects
    // ==================================================================================
    
    // hknpBody offsets (verified via Ghidra)
    namespace hknpBody_Offsets {
        constexpr std::ptrdiff_t transform = 0x00;
        constexpr std::ptrdiff_t flags = 0x40;
        constexpr std::ptrdiff_t collisionFilterInfo = 0x44;
        constexpr std::ptrdiff_t shape = 0x48;
        constexpr std::ptrdiff_t motionId = 0x68;
        constexpr std::ptrdiff_t stride = 0x90;
    }
    
    // hknpWorld offsets (verified via Ghidra)
    namespace hknpWorld_Offsets {
        constexpr std::ptrdiff_t bodyBuffer = 0x20;
    }

    constexpr std::size_t MIDDLEHIGHPROCESS_CHARCONTROLLER_OFFSET = 0x3E8;  // VR: +8 byte shift from flat FO4
    constexpr std::size_t BHKWORLD_CHARPROXYMANAGER_OFFSET = 0x68;
    constexpr std::size_t BHKCHARPROXYMANAGER_PROXYCONTROLLERS_OFFSET = 0x10;
    constexpr std::size_t BHKCHARACTERCONTROLLER_USERDATA_OFFSET = 0x3E0;

    struct BSTPointerArrayView
    {
        void* data = nullptr;
        std::uint32_t capacity = 0;
        std::uint32_t pad0 = 0;
        std::uint32_t size = 0;
        std::uint32_t pad1 = 0;
    };
    static_assert(sizeof(BSTPointerArrayView) == 0x18);

    bool TryReadBodyFilterInfo(void* hknpWorld, std::uint32_t bodyId, std::uint32_t& outFilterInfo)
    {
        if (!hknpWorld || IsInvalidBodyId(bodyId)) {
            return false;
        }

        const std::uint32_t bodyIndex = GetBodyBufferIndex(bodyId);
        if (!IsReasonableBodyIndex(bodyIndex)) {
            return false;
        }

        __try {
            void* bodyBuffer = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + hknpWorld_Offsets::bodyBuffer);
            if (!bodyBuffer) {
                return false;
            }

            void* bodyPtr = reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(bodyBuffer) + static_cast<uintptr_t>(bodyIndex) * hknpBody_Offsets::stride
            );
            outFilterInfo = *reinterpret_cast<std::uint32_t*>(
                reinterpret_cast<uintptr_t>(bodyPtr) + hknpBody_Offsets::collisionFilterInfo
            );
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ROCK-style filter-info write. ROCK ORs bit 14 (kSuppressionNoCollideBit = 0x4000) into
    // the body's collisionFilterInfo when suppressing collision for a held/grabbed body —
    // the engine's collision filter rejects pairs with this bit set, including against the
    // player character proxy. This is much more reliable than the pair-filter disable
    // (which depends on resolving the volatile player body id every frame).
    //
    // NOTE: this writes the filter info field directly. Some engine paths re-derive collision
    // pairs lazily, so the filter takes effect on the next broadphase iteration.
    // Jul 18: per-body CCD look-ahead (anti-tunneling for released objects vs thin hand
    // colliders). See Physics.h. SEH rule: Relocation outside __try.
    static void SetBodyCollisionLookAheadNative(void* hknpWorld, std::uint32_t bodyId, float dist, const float* vec4)
    {
        using Fn = void (*)(void*, std::uint32_t, float, const float*);
        static REL::Relocation<Fn> s_fn{ REL::Offset(0x153B120) };
        s_fn(hknpWorld, bodyId, dist, vec4);
    }

    bool TrySetBodyCollisionLookAhead(void* hknpWorld, std::uint32_t bodyId, float distanceHavok)
    {
        if (!hknpWorld || IsInvalidBodyId(bodyId)) {
            return false;
        }
        static const float kZeroVec[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        __try {
            SetBodyCollisionLookAheadNative(hknpWorld, bodyId, distanceHavok, kZeroVec);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH rule: REL::Relocation has a constructor, so it cannot live inside the __try below.
    static void SetBodyCollisionFilterInfoNative(void* hknpWorld, std::uint32_t bodyId, std::uint32_t filterInfo)
    {
        using SetFilter_t = void (*)(void*, std::uint32_t, std::uint32_t);
        static REL::Relocation<SetFilter_t> s_setFilter{ REL::Offset(0x1DF5B80) };  // hknpBSWorld::setBodyCollisionFilterInfo
        s_setFilter(hknpWorld, bodyId, filterInfo);
    }

    bool TryWriteBodyFilterInfo(void* hknpWorld, std::uint32_t bodyId, std::uint32_t filterInfo)
    {
        if (!hknpWorld || IsInvalidBodyId(bodyId)) {
            return false;
        }
        const std::uint32_t bodyIndex = GetBodyBufferIndex(bodyId);
        if (!IsReasonableBodyIndex(bodyIndex)) {
            return false;
        }

        __try {
            void* bodyBuffer = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + hknpWorld_Offsets::bodyBuffer);
            if (!bodyBuffer) {
                return false;
            }
            // Jul 18 (post-release dead hand-object collision): write through the ENGINE's
            // setter, hknpBSWorld::setBodyCollisionFilterInfo @0x1DF5B80 (same native the
            // embedded ROCK uses — HavokRuntime.h setFilterInfo). The old raw field write
            // changed the filter word but never refreshed broadphase PAIRING, so a pair
            // created as filtered-out during a hold (hand vs held object) kept its cached
            // no-collide verdict after release — the hand permanently stopped colliding
            // with any object it had grabbed until cell reload.
            SetBodyCollisionFilterInfoNative(hknpWorld, bodyId, filterInfo);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    constexpr std::uint32_t kSuppressionNoCollideBit = 1u << 14;  // ROCK match: 0x4000

    bool SetBodyNoCollideBit(void* hknpWorld, std::uint32_t bodyId, bool enable, std::uint32_t* outOriginalFilter)
    {
        std::uint32_t cur = 0;
        if (!TryReadBodyFilterInfo(hknpWorld, bodyId, cur)) return false;
        if (outOriginalFilter) *outOriginalFilter = cur;
        const std::uint32_t next = enable ? (cur | kSuppressionNoCollideBit) : (cur & ~kSuppressionNoCollideBit);
        if (next == cur) return true;
        return TryWriteBodyFilterInfo(hknpWorld, bodyId, next);
    }

    bool TryReadCharacterControllerSystemBodyIdxImpl(RE::bhkCharacterController* charController, std::uint32_t& outBodyId)
    {
        if (!charController) {
            return false;
        }

        __try {
            // Try multiple offsets to find systemBodyIdx
            uintptr_t addr = reinterpret_cast<uintptr_t>(charController);

            // Attempt 1: charController IS the base, systemBodyIdx at +0x28
            std::uint32_t val1 = *reinterpret_cast<std::uint32_t*>(addr + 0x28);
            // Attempt 2: charController is subobject at base+0x10, so base+0x28 = addr+0x18
            std::uint32_t val2 = *reinterpret_cast<std::uint32_t*>(addr + 0x18);
            // Attempt 3: CommonLibF4 systemBodyIdx offset on bhkNPCollisionObject
            std::uint32_t val3 = charController->systemBodyIdx;

            // Attempt 4: Read from hknpCharacterProxy at charController+0x470 (adjusted for subobject)
            // The proxy has m_shapePhantomBodyId at offset +0x34
            void* proxy = *reinterpret_cast<void**>(addr + 0x470);
            std::uint32_t val4 = 0x7FFFFFFF;
            if (proxy) {
                val4 = *reinterpret_cast<std::uint32_t*>(reinterpret_cast<uintptr_t>(proxy) + 0x34);
            }

            static auto lastDiagLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDiagLog).count() >= 3) {
                spdlog::info("[PHYSICS] systemBodyIdx diagnostic: addr={:p}, +0x28=0x{:08X}, +0x18=0x{:08X}, CommonLib=0x{:08X}, proxy={:p}, proxy+0x34=0x{:08X}",
                             (void*)addr, val1, val2, val3, proxy, val4);
                lastDiagLog = now;
            }

            // Try proxy body ID first — it's the most reliable for the character capsule.
            // hknp body IDs have generation bits in upper bytes; pass full value to Havok.
            if (val4 != 0x7FFFFFFF && val4 != 0xFFFFFFFF && val4 != 0) {
                outBodyId = val4;
                return true;
            }
            // Fallback: direct offset reads (may be 0 for proxy controllers)
            if (val1 != 0x7FFFFFFF && val1 != 0xFFFFFFFF && val1 != 0 && IsReasonableBodyIndex(val1)) { outBodyId = val1; return true; }
            if (val3 != 0x7FFFFFFF && val3 != 0xFFFFFFFF && val3 != 0 && IsReasonableBodyIndex(val3)) { outBodyId = val3; return true; }
            return false;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool TryReadMiddleHighCharControllerImpl(void* middleHigh, RE::bhkCharacterController*& outController)
    {
        outController = nullptr;
        if (!middleHigh) {
            return false;
        }

        __try {
            void* rawPtr = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(middleHigh) + MIDDLEHIGHPROCESS_CHARCONTROLLER_OFFSET);
            outController = reinterpret_cast<RE::bhkCharacterController*>(rawPtr);
            return outController != nullptr;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            outController = nullptr;
            return false;
        }
    }

    bool TryReadCharacterControllerPhysicsSystemImpl(RE::bhkCharacterController* charController, void*& outPhysicsSystem)
    {
        outPhysicsSystem = nullptr;
        if (!charController) {
            return false;
        }

        __try {
            outPhysicsSystem = charController->spSystem.get();
            return outPhysicsSystem != nullptr;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            outPhysicsSystem = nullptr;
            return false;
        }
    }

    bool TryReadCharacterControllerBodyIdVirtualImpl(RE::bhkCharacterController* charController, std::uint32_t& outBodyId)
    {
        outBodyId = INVALID_BODY_ID;
        if (!charController) {
            return false;
        }

        __try {
            using GetBodyIdImpl_t = std::uint32_t(__fastcall*)(const RE::bhkCharacterController*);
            auto** vtable = *reinterpret_cast<GetBodyIdImpl_t***>(charController);
            if (!vtable || !(*vtable)) {
                return false;
            }

            // VR: vtable slot is 0x43 (+2 from flat FO4's 0x41)
            // Also: charController is the subobject at base+0x10, so the vtable
            // here is bhkCharacterController's vtable, which should have GetBodyIdImpl.
            // BUT if this vtable doesn't have it at 0x43, use the base object instead.
            constexpr std::size_t kGetBodyIdImplVtableSlot = 0x43;

            // Try the subobject vtable first
            GetBodyIdImpl_t getBodyIdImpl = reinterpret_cast<GetBodyIdImpl_t>(
                reinterpret_cast<void**>(*reinterpret_cast<uintptr_t*>(charController))[kGetBodyIdImplVtableSlot]);
            if (!getBodyIdImpl) {
                return false;
            }

            outBodyId = getBodyIdImpl(charController);
            return !IsInvalidBodyId(outBodyId);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            outBodyId = INVALID_BODY_ID;
            return false;
        }
    }

    bool TryReadPhysicsSystemBodyIdImpl(void* physicsSystem, std::uint32_t systemBodyIdx, std::uint32_t& outBodyId)
    {
        outBodyId = INVALID_BODY_ID;
        if (!physicsSystem || IsInvalidBodyId(systemBodyIdx)) {
            return false;
        }

        __try {
            ConstraintFunctions::BhkPhysicsSystemGetBodyId(physicsSystem, &outBodyId, systemBodyIdx);
            return !IsInvalidBodyId(outBodyId);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            outBodyId = INVALID_BODY_ID;
            return false;
        }
    }

    RE::bhkNPCollisionObject* TryResolveProxyTargetCollisionObject(RE::NiCollisionObject* collisionObject)
    {
        if (!collisionObject) {
            return nullptr;
        }

        __try {
            uintptr_t proxyAddr = reinterpret_cast<uintptr_t>(collisionObject);
            constexpr std::ptrdiff_t kProxyTargetOffsets[] = { 0x20, 0x28, 0x30 };
            for (std::ptrdiff_t offset : kProxyTargetOffsets) {
                auto* target = *reinterpret_cast<RE::bhkNPCollisionObject**>(proxyAddr + offset);
                if (target) {
                    return target;
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }

        return nullptr;
    }

    RE::bhkNPCollisionObject* TryResolveNpcCollisionObjectImpl(RE::NiCollisionObject* collisionObject)
    {
        if (!collisionObject) {
            return nullptr;
        }

        __try {
            auto* rtti = collisionObject->GetRTTI();
            const char* typeName = (rtti && rtti->GetName()) ? rtti->GetName() : nullptr;
            if (!typeName) {
                return nullptr;
            }

            if (std::strcmp(typeName, "bhkNPCollisionProxyObject") == 0) {
                return TryResolveProxyTargetCollisionObject(collisionObject);
            }

            if (std::strcmp(typeName, "bhkNPCollisionObject") == 0) {
                return reinterpret_cast<RE::bhkNPCollisionObject*>(collisionObject);
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }

        return nullptr;
    }

    bool TryResolveCollisionObjectBodyIdImpl(RE::NiCollisionObject* collisionObject, std::uint32_t& outBodyId)
    {
        outBodyId = INVALID_BODY_ID;

        RE::bhkNPCollisionObject* npcCollisionObject = TryResolveNpcCollisionObjectImpl(collisionObject);
        if (!npcCollisionObject) {
            return false;
        }

        std::uint32_t systemBodyIdx = INVALID_BODY_ID;
        void* physicsSystem = nullptr;
        __try {
            systemBodyIdx = npcCollisionObject->systemBodyIdx;
            physicsSystem = npcCollisionObject->spSystem ? npcCollisionObject->spSystem.get() : nullptr;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (TryReadPhysicsSystemBodyIdImpl(physicsSystem, systemBodyIdx, outBodyId)) {
            return true;
        }

        if (!IsInvalidBodyId(systemBodyIdx) && IsReasonableBodyIndex(systemBodyIdx)) {
            outBodyId = systemBodyIdx;
            return true;
        }

        return false;
    }

    bool TryReadCharacterControllerOwnerDataImpl(RE::bhkCharacterController* controller, void*& outOwnerData)
    {
        if (!controller) {
            return false;
        }

        __try {
            outOwnerData = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + BHKCHARACTERCONTROLLER_USERDATA_OFFSET);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            outOwnerData = nullptr;
            return false;
        }
    }

    bool TryReadCharacterControllerPositionImpl(RE::bhkCharacterController* controller, RE::hkVector4f& outPosition)
    {
        if (!controller) {
            return false;
        }

        __try {
            controller->GetPositionImpl(outPosition, false);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool TryGetCharacterControllerBodyId(RE::bhkCharacterController* charController, void* hknpWorld, std::uint32_t& outBodyId)
    {
        if (!charController || !hknpWorld) {
            return false;
        }

        std::uint32_t rawSystemBodyIdx = INVALID_BODY_ID;
        TryReadCharacterControllerSystemBodyIdxImpl(charController, rawSystemBodyIdx);

        const std::uint32_t systemBodyIdx = NormalizeShiftedBodyIdCandidate(rawSystemBodyIdx);
        if (!IsInvalidBodyId(rawSystemBodyIdx) && systemBodyIdx != rawSystemBodyIdx) {
            spdlog::debug("[PHYSICS] TryGetCharacterControllerBodyId: normalized shifted systemBodyIdx 0x{:08X} -> 0x{:08X}",
                          rawSystemBodyIdx,
                          systemBodyIdx);
        }

        void* physicsSystem = nullptr;
        TryReadCharacterControllerPhysicsSystemImpl(charController, physicsSystem);

        struct BodyCandidate
        {
            const char* source;
            std::uint32_t bodyId;
        };

        BodyCandidate candidates[3] = {
            { "GetBodyIdImpl", INVALID_BODY_ID },
            { "physicsSystem", INVALID_BODY_ID },
            { "systemBodyIdx", INVALID_BODY_ID }
        };

        TryReadCharacterControllerBodyIdVirtualImpl(charController, candidates[0].bodyId);
        if (physicsSystem) {
            TryReadPhysicsSystemBodyIdImpl(physicsSystem, systemBodyIdx, candidates[1].bodyId);
        }
        if (!IsInvalidBodyId(systemBodyIdx)) {
            candidates[2].bodyId = systemBodyIdx;
        }

        auto tryCandidate = [&](int candidateIndex) {
            const BodyCandidate& candidate = candidates[candidateIndex];
            if (IsInvalidBodyId(candidate.bodyId)) {
                return false;
            }

            for (int previousIndex = 0; previousIndex < candidateIndex; ++previousIndex) {
                if (candidates[previousIndex].bodyId == candidate.bodyId) {
                    return false;
                }
            }

            const std::uint32_t bodyIndex = GetBodyBufferIndex(candidate.bodyId);
            if (!IsReasonableBodyIndex(bodyIndex)) {
                spdlog::debug("[PHYSICS] TryGetCharacterControllerBodyId: rejecting {} candidate 0x{:08X} (index 0x{:06X})",
                              candidate.source,
                              candidate.bodyId,
                              bodyIndex);
                return false;
            }

            std::uint32_t filterInfo = 0;
            if (!TryReadBodyFilterInfo(hknpWorld, candidate.bodyId, filterInfo)) {
                spdlog::debug("[PHYSICS] TryGetCharacterControllerBodyId: failed to read filter info for {} candidate 0x{:08X} (systemBodyIdx=0x{:08X})",
                              candidate.source,
                              candidate.bodyId,
                              systemBodyIdx);
                return false;
            }

            const std::uint32_t layer = filterInfo & 0x7F;
            if (layer != 31) {
                spdlog::debug("[PHYSICS] TryGetCharacterControllerBodyId: rejecting {} candidate 0x{:08X} (systemBodyIdx=0x{:08X}, layer={})",
                              candidate.source,
                              candidate.bodyId,
                              systemBodyIdx,
                              layer);
                return false;
            }

            outBodyId = candidate.bodyId;
            spdlog::debug("[PHYSICS] TryGetCharacterControllerBodyId: resolved player controller body 0x{:08X} via {} (systemBodyIdx=0x{:08X})",
                          candidate.bodyId,
                          candidate.source,
                          systemBodyIdx);
            return true;
        };

        for (int candidateIndex = 0; candidateIndex < 3; ++candidateIndex) {
            if (tryCandidate(candidateIndex)) {
                return true;
            }
        }

        return false;
    }

    bool TryFindPlayerBodyViaProxyManager(RE::bhkWorld* bhkWorld, void* hknpWorld, std::uint32_t& outBodyId)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !bhkWorld || !hknpWorld) {
            return false;
        }

        constexpr float kMaxPlayerProxyDistanceHavok = 32.0f;

        WorldReadLock worldLock(bhkWorld);

        auto* controllersView = reinterpret_cast<const BSTPointerArrayView*>(
            reinterpret_cast<uintptr_t>(bhkWorld) + BHKWORLD_CHARPROXYMANAGER_OFFSET + BHKCHARPROXYMANAGER_PROXYCONTROLLERS_OFFSET);
        if (!controllersView) {
            return false;
        }

        auto* controllers = reinterpret_cast<RE::bhkCharacterController**>(controllersView->data);
        const std::uint32_t controllerCount = controllersView->size;
        if (!controllers || controllerCount == 0 || controllerCount > 256) {
            return false;
        }

        const RE::NiPoint3 playerPos = player->GetPosition();
        const RE::hkVector4f playerPosHavok(
            playerPos.x * HAVOK_WORLD_SCALE,
            playerPos.y * HAVOK_WORLD_SCALE,
            playerPos.z * HAVOK_WORLD_SCALE,
            0.0f);

        std::uint32_t bestBodyId = INVALID_BODY_ID;
        float bestScore = (std::numeric_limits<float>::max)();
        std::uint32_t bestLayer = 0xFF;
        bool bestOwnerMatch = false;

        for (std::uint32_t i = 0; i < controllerCount; ++i) {
            auto* controller = controllers[i];
            if (!controller) {
                continue;
            }

            std::uint32_t candidateBodyId = INVALID_BODY_ID;
            if (!TryGetCharacterControllerBodyId(controller, hknpWorld, candidateBodyId)) {
                continue;
            }

            if (!IsReasonableBodyIndex(GetBodyBufferIndex(candidateBodyId))) {
                continue;
            }

            std::uint32_t filterInfo = 0;
            const bool hasFilterInfo = TryReadBodyFilterInfo(hknpWorld, candidateBodyId, filterInfo);
            if (!hasFilterInfo) {
                continue;
            }

            void* ownerData = nullptr;
            TryReadCharacterControllerOwnerDataImpl(controller, ownerData);

            RE::hkVector4f controllerPos;
            const bool positionValid = TryReadCharacterControllerPositionImpl(controller, controllerPos);
            if (!positionValid) {
                continue;
            }

            const float dx = controllerPos.x - playerPosHavok.x;
            const float dy = controllerPos.y - playerPosHavok.y;
            const float dz = controllerPos.z - playerPosHavok.z;
            float score = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (score > kMaxPlayerProxyDistanceHavok) {
                continue;
            }

            const bool ownerMatch = ownerData == player;
            const std::uint32_t layer = filterInfo & 0x7F;
            if (layer != 31) {
                continue;
            }
            if (!ownerMatch) {
                score += 5.0f;
            }

            if (score < bestScore) {
                bestScore = score;
                bestBodyId = candidateBodyId;
                bestLayer = layer;
                bestOwnerMatch = ownerMatch;
            }
        }

        if (IsInvalidBodyId(bestBodyId)) {
            return false;
        }

        outBodyId = bestBodyId;
        spdlog::info(
            "[PHYSICS] Proxy manager candidate body=0x{:08X} ownerMatch={} layer={} score={:.2f}",
            bestBodyId,
            bestOwnerMatch,
            bestLayer,
            bestScore);
        return true;
    }
    
    // Alternative: Try to find player body through NiNode collision object
    bool TryFindPlayerBodyAlternative(void* bhkWorld, void* hknpWorld, std::uint32_t& outBodyId)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        // Try to get collision object from player's 3D node
        RE::NiAVObject* root3D = player->Get3D();
        if (!root3D) {
            spdlog::debug("[PHYSICS] TryFindPlayerBodyAlternative: no 3D node");
            return false;
        }
        
        RE::NiNode* rootNode = root3D->IsNode();
        if (!rootNode) {
            spdlog::debug("[PHYSICS] TryFindPlayerBodyAlternative: 3D is not a node");
            return false;
        }

        static auto lastSearchLog = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto searchLogNow = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(searchLogNow - lastSearchLog).count() >= 1) {
            spdlog::debug("[PHYSICS] Searching for player collision body...");
            spdlog::debug("[PHYSICS]   Root node: {} @ {:x}", rootNode->name.c_str(), reinterpret_cast<uintptr_t>(rootNode));
            lastSearchLog = searchLogNow;
        }

        auto tryCollisionNode = [&](RE::NiAVObject* collisionNode) {
            if (!collisionNode || !collisionNode->collisionObject) {
                return false;
            }

            std::uint32_t candidateBodyId = INVALID_BODY_ID;
            if (!TryResolveCollisionObjectBodyIdImpl(collisionNode->collisionObject.get(), candidateBodyId)) {
                return false;
            }

            std::uint32_t filterInfo = 0;
            if (!TryReadBodyFilterInfo(hknpWorld, candidateBodyId, filterInfo)) {
                return false;
            }

            const std::uint32_t layer = filterInfo & 0x7F;
            spdlog::debug("[PHYSICS]   Collision candidate on '{}': body=0x{:08X}, layer={}",
                          collisionNode->name.c_str(),
                          candidateBodyId,
                          layer);

            if (layer != 31) {
                return false;
            }

            outBodyId = candidateBodyId;
            return true;
        };

        if (tryCollisionNode(rootNode)) {
            return true;
        }

        // Try searching child nodes for collision
        // Uses index-based loop (not range-based) for SEH compatibility
        std::function<bool(RE::NiNode*, int)> searchNode = [&](RE::NiNode* node, int depth) -> bool {
            if (depth > 12) return false;  // Limit depth

            auto& children = node->children;
            uint32_t childCount = children.size();
            if (childCount > 1000) return false;  // Sanity guard

            for (uint32_t ci = 0; ci < childCount; ++ci) {
                auto* childPtr = children[ci].get();
                if (!childPtr) continue;

                if (tryCollisionNode(childPtr)) {
                    return true;
                }

                RE::NiNode* childNode = childPtr->IsNode();
                if (childNode && searchNode(childNode, depth + 1)) {
                    return true;
                }
            }
            return false;
        };

        if (searchNode(rootNode, 0)) {
            return true;
        }

        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 1) {
            spdlog::warn("[PHYSICS] Could not find player body via NiNode search");
            lastLog = now;
        }
        return false;
    }

    bool SetPlayerCollisionEnabled(bool enabled)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: no player");
            return false;
        }
        
        // Get the physics world first - we need it for both approaches
        auto* cell = player->GetParentCell();
        if (!cell) {
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 5) {
                spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: no cell");
                lastLog = now;
            }
            return false;
        }
        
        auto* bhkWorld = heisenberg::TESObjectCell_GetbhkWorld(cell);
        if (!bhkWorld) {
            spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: no bhkWorld");
            return false;
        }
        
        void* hknpWorld = GetHknpWorldFromBhk(bhkWorld);
        if (!hknpWorld) {
            spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: no hknpWorld");
            return false;
        }
        
        std::uint32_t bodyId = GetPlayerBodyId();
        if (IsInvalidBodyId(bodyId)) {
            static auto lastLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLog).count() >= 5) {
                spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: could not resolve player body via controller, proxy manager, or 3D scan");
                lastLog = now;
            }
            return false;
        }
        
        if (IsInvalidBodyId(bodyId)) {
            return false;
        }
        
        // Re-validate cell hasn't changed since function entry (guards against zone transition)
        if (player->GetParentCell() != cell) {
            spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: cell changed during operation");
            return false;
        }

        // Calculate body pointer: bodyBuffer + bodyId * 0x90
        void* bodyBuffer = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hknpWorld) + hknpWorld_Offsets::bodyBuffer);
        if (!bodyBuffer) {
            spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: null body buffer");
            return false;
        }
        
        // Mask the bodyId index out of its packed Havok handle.
        // Havok 2014 hknpBodyId encodes <generation:upper><index:lower 16>;
        // multiplying the unmasked 32-bit value by stride dereferences gigabytes
        // past the body buffer (crash on read at +0x44). Same mask the rest of
        // the codebase uses (HandCollision.cpp / Physics.cpp:143).
        std::uint32_t bodyIndex = GetBodyBufferIndex(bodyId);
        if (!IsReasonableBodyIndex(bodyIndex)) {
            spdlog::warn("[PHYSICS] SetPlayerCollisionEnabled: unreasonable bodyIndex {} (raw 0x{:08X})",
                         bodyIndex, bodyId);
            return false;
        }
        void* bodyPtr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(bodyBuffer) + static_cast<uintptr_t>(bodyIndex) * hknpBody_Offsets::stride
        );
        
        // Access collisionFilterInfo at offset 0x44
        std::uint32_t* filterInfoPtr = reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<uintptr_t>(bodyPtr) + hknpBody_Offsets::collisionFilterInfo
        );
        
        WorldWriteLock lock(bhkWorld);
        
        if (!enabled) {
            // Disable collision - save original and set to kNonCollidable (15)
            if (!g_playerCollisionModified) {
                g_savedPlayerCollisionFilterInfo = *filterInfoPtr;
                g_playerCollisionModified = true;
                spdlog::info("[PHYSICS] Saved player collision filter: 0x{:08X} (layer {})",
                            g_savedPlayerCollisionFilterInfo.load(), g_savedPlayerCollisionFilterInfo.load() & 0x7F);
            }
            
            // Set layer to kNonCollidable (15), preserve other bits
            std::uint32_t newFilterInfo = (g_savedPlayerCollisionFilterInfo & ~0x7F) | 15;
            *filterInfoPtr = newFilterInfo;
            spdlog::info("[PHYSICS] Player collision DISABLED (filter -> 0x{:08X}, layer 15)", newFilterInfo);
        } else {
            // Re-enable collision - restore original
            if (g_playerCollisionModified) {
                *filterInfoPtr = g_savedPlayerCollisionFilterInfo;
                g_playerCollisionModified = false;
                spdlog::info("[PHYSICS] Player collision ENABLED (filter -> 0x{:08X}, layer {})",
                            g_savedPlayerCollisionFilterInfo.load(), g_savedPlayerCollisionFilterInfo.load() & 0x7F);
            }
        }
        
        return true;
    }
    
    bool IsPlayerCollisionDisabled()
    {
        return g_playerCollisionModified;
    }

    bool DisableCollisionBetween(void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB)
    {
        // DEAD PATH: hknpPairCollisionFilter::disableCollisionsBetween (0x196de70)
        // FAULTS on every call on F4VR 1.2.72 — it is the wrong mechanism for this
        // build (it produced ~50 SEH exceptions at load with zero effect, one per
        // hand/body/weapon collider). Collider-vs-player exclusion is handled by the
        // layer-43 / 0x000B002B filter approach, and the char-proxy dangling-body
        // crash by ClearPlayerProxySupportBody(). Short-circuit to a no-op so we
        // stop burning SEH at load. Callers already treat false as "not applied".
        (void)hknpWorld; (void)bodyIdA; (void)bodyIdB;
        static std::atomic<bool> warned{ false };
        if (!warned.exchange(true)) {
            spdlog::warn("[PHYSICS] DisableCollisionBetween is a no-op (engine pair-filter faults on F4VR; using layer-43 filtering + ClearPlayerProxySupportBody instead)");
        }
        return false;
    }

    void EnableCollisionBetween(void* hknpWorld, std::uint32_t bodyIdA, std::uint32_t bodyIdB)
    {
        if (!hknpWorld || bodyIdA == 0x7FFFFFFF || bodyIdB == 0x7FFFFFFF) return;
        // Build the key the same way disableCollisionsBetween does: min first, max second
        std::uint32_t lo = (bodyIdA < bodyIdB) ? bodyIdA : bodyIdB;
        std::uint32_t hi = (bodyIdA < bodyIdB) ? bodyIdB : bodyIdA;
        std::uint64_t key = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);

        // The pair filter map is at hknpWorld offset determined by disableCollisionsBetween
        // which accesses param_1 + 0x18 in the filter object. But disableCollisionsBetween
        // takes the world as first param and finds the filter internally.
        // For remove, we need the filter's map directly. The filter is accessed from the world.
        // For now, use a SEH-protected approach.
        __try {
            // The pairCollisionFilter is found via the hknpWorld. The disableCollisionsBetween
            // function accesses it internally. We need to find the same filter object.
            // From Ghidra: disableCollisionsBetween's first param IS the filter object, not the world.
            // The caller passes the filter. Let's find it from the world.
            // hknpWorld has a collision filter at a known offset. For now, try the world directly
            // since disableCollisionsBetween worked with it.
            // Actually, looking at the decompilation more carefully:
            // param_1 = pairCollisionFilter (NOT hknpWorld)
            // param_2 = hknpWorld
            // So we need the filter object. It lives at a fixed offset in hknpWorld.
            // From the database name: "disableCollisionsBetween(hknpWorld*, hknpBodyId, hknpBodyId)"
            // But the decompilation shows param_1 used as the filter (param_1 + 0x18 = map).
            // This suggests the REL::Relocation wrapping adjusts the 'this' parameter.
            // The remove function takes the map directly at filter + 0x18.
            // Since disableCollisionsBetween works with (world, bodyA, bodyB), the function
            // internally finds the filter. We can't easily get the filter object.
            //
            // WORKAROUND: Just call disableCollisionsBetween again — it's idempotent (increments count).
            // To truly re-enable, we'd need to decrement to 0. But for our use case,
            // we only disable once and clear on grab end. Use clearAll as last resort.
            spdlog::debug("[PHYSICS] EnableCollisionBetween: pair filter selective remove not yet implemented");
            spdlog::debug("[PHYSICS]   bodyA=0x{:08X}, bodyB=0x{:08X} — collision will restore on next physics reset", bodyIdA, bodyIdB);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            spdlog::error("[PHYSICS] Exception in EnableCollisionBetween");
        }
    }
}
