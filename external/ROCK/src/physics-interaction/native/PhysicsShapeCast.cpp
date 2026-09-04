#include "physics-interaction/native/PhysicsShapeCast.h"

#include "physics-interaction/native/HavokOffsets.h"
#include "physics-interaction/native/HavokWorldLock.h"
#include "physics-interaction/native/HavokConvexShapeBuilder.h"
#include "physics-interaction/native/PhysicsShapeCastCachePolicy.h"
#include "physics-interaction/PhysicsLog.h"
#include "physics-interaction/native/PhysicsUtils.h"
#include "physics-interaction/performance/PerformanceProfiler.h"
#include "physics-interaction/TransformMath.h"

#include "RE/Bethesda/bhkCharacterController.h"
#include "RE/Havok/hknpShape.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <xmmintrin.h>

namespace rock::physics_shape_cast
{
    namespace
    {
        /*
         * ROCK selection needs swept-sphere body evidence, and FO4VR exposes that
         * through hknp's direct CastShape ABI. The wrapper below centralizes the
         * verified 0x80 query layout and uses
         * process-lifetime query spheres keyed by radius. We do not mutate an existing
         * sphere's radius after construction because the constructor initializes support
         * data along with +0x14; reusing the right constructed shape keeps the query ABI
         * stable while avoiding per-frame allocations.
         */
        RE::hknpShape* getSelectionSphereShape(float radiusHavok)
        {
            using CreateSphereShape_t = RE::hknpShape* (*)(RE::hkVector4f*, float);
            static REL::Relocation<CreateSphereShape_t> createSphere{ REL::Offset(offsets::kFunc_CreateSphereShape) };

            static std::mutex cacheMutex;
            static std::unordered_map<std::uint32_t, RE::hknpShape*> cachedShapes;

            const auto radiusKey = std::bit_cast<std::uint32_t>(radiusHavok);
            std::scoped_lock lock(cacheMutex);
            if (auto it = cachedShapes.find(radiusKey); it != cachedShapes.end()) {
                return it->second;
            }

            RE::hkVector4f center{ 0.0f, 0.0f, 0.0f, 0.0f };
            auto* shape = createSphere(&center, radiusHavok);
            if (!shape) {
                ROCK_LOG_ERROR(Hand, "Selection shape cast: failed to create hknp sphere shape radiusHk={:.4f}", radiusHavok);
                return nullptr;
            }

            cachedShapes.emplace(radiusKey, shape);
            return shape;
        }

        using BoxShapeKey =
            physics_shape_cast_cache_policy::QuantizedBoxKey;

        struct BoxShapeKeyHash
        {
            std::size_t operator()(const BoxShapeKey& key) const noexcept
            {
                std::size_t value = key.x;
                value = (value * 16777619u) ^ key.y;
                value = (value * 16777619u) ^ key.z;
                return value;
            }
        };

        RE::hknpShape* getConservativeBoxShape(
            const RE::NiPoint3& requestedHalfExtentsGame)
        {
            // Round outward to a quarter game unit.  This both preserves the
            // enclosure invariant and keeps the process-lifetime shape cache
            // finite for broadphase AABB quantization jitter.
            constexpr float kExtentQuantumGame = 0.25f;
            constexpr float kMinimumHalfExtentGame = 0.05f;
            constexpr float kMaximumHalfExtentGame = 2000.0f;
            const auto quantize = [&](const float extent) {
                if (!std::isfinite(extent) || extent < 0.0f ||
                    extent > kMaximumHalfExtentGame) {
                    return std::uint32_t{ 0 };
                }
                return static_cast<std::uint32_t>(std::ceil(
                    (std::max)(extent, kMinimumHalfExtentGame) /
                    kExtentQuantumGame));
            };

            const BoxShapeKey key{
                quantize(requestedHalfExtentsGame.x),
                quantize(requestedHalfExtentsGame.y),
                quantize(requestedHalfExtentsGame.z),
            };
            if (key.x == 0 || key.y == 0 || key.z == 0) {
                return nullptr;
            }

            static std::mutex cacheMutex;
            static std::unordered_map<BoxShapeKey,
                RE::hknpShape*,
                BoxShapeKeyHash>
                cachedShapes;
            static std::unordered_map<BoxShapeKey,
                RE::hknpShape*,
                BoxShapeKeyHash>
                cachedOverflowBoxes;
            std::scoped_lock lock(cacheMutex);
            if (const auto found = cachedShapes.find(key);
                found != cachedShapes.end()) {
                return found->second;
            }

            const auto buildBox = [&](const BoxShapeKey& buildKey) {
                const RE::NiPoint3 half{
                    static_cast<float>(buildKey.x) *
                        kExtentQuantumGame,
                    static_cast<float>(buildKey.y) *
                        kExtentQuantumGame,
                    static_cast<float>(buildKey.z) *
                        kExtentQuantumGame,
                };
                const float scale = gameToHavokScale();
                std::vector<RE::NiPoint3> points;
                points.reserve(8);
                for (const float x : { -half.x, half.x }) {
                    for (const float y : { -half.y, half.y }) {
                        for (const float z : { -half.z, half.z }) {
                            points.emplace_back(
                                x * scale,
                                y * scale,
                                z * scale);
                        }
                    }
                }
                return havok_convex_shape_builder::
                    buildConvexShapeFromLocalHavokPoints(points, 0.0f);
            };

            constexpr std::size_t kMaximumCachedBoxShapes = 256;
            if (cachedShapes.size() < kMaximumCachedBoxShapes) {
                auto* shape = buildBox(key);
                if (!shape) {
                    return nullptr;
                }
                cachedShapes.emplace(key, shape);
                return shape;
            }

            /*
             * Do not allocate/release a novel convex hull every frame after
             * the detailed cache fills.  Overflow rounds each axis outward to
             * its own power-of-two bin.  This keeps every axis below 2x its
             * requested extent (so a rifle never degenerates into a room-sized
             * cube) while retaining a bounded 14^3 theoretical key space at
             * quarter-unit quantization and the validated 2000-gu ceiling.
             * Only bins actually encountered allocate process-lifetime shapes.
             */
            const BoxShapeKey overflowKey =
                physics_shape_cast_cache_policy::logarithmicOverflowKey(key);
            if (const auto found = cachedOverflowBoxes.find(overflowKey);
                found != cachedOverflowBoxes.end()) {
                return found->second;
            }
            auto* overflowShape = buildBox(overflowKey);
            if (!overflowShape) {
                return nullptr;
            }
            cachedOverflowBoxes.emplace(overflowKey, overflowShape);
            return overflowShape;
        }

        void prepareAllHitsCollector(RE::hknpAllHitsCollector& collector)
        {
            /*
             * CommonLib's constructor correctly points hkInplaceArray storage at this
             * collector's inline buffer. Default assignment from a temporary does not:
             * it copies the temporary's _data pointer and leaves CastShape writing into
             * dead stack memory. Reset the fields in place instead.
             */
            collector.hints = 0;
            collector.earlyOutThreshold.real = _mm_set1_ps((std::numeric_limits<float>::max)());
            collector.hits._data = reinterpret_cast<RE::hknpCollisionResult*>(reinterpret_cast<std::uintptr_t>(&collector) + 0x30);
            collector.hits._size = 0;
            collector.hits._capacityAndFlags = 0x8000000A;
        }

        bool normalize(RE::NiPoint3 value, RE::NiPoint3& out)
        {
            const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
            if (lengthSquared <= 1.0e-6f) {
                out = RE::NiPoint3{};
                return false;
            }

            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            out = RE::NiPoint3(value.x * inverseLength, value.y * inverseLength, value.z * inverseLength);
            return true;
        }

        bool runShapeCast(RE::hknpWorld* world,
            const RE::hknpShape* shape,
            const RE::NiTransform& startWorld,
            const RE::NiPoint3& directionGame,
            const float distanceGame,
            const std::uint32_t collisionFilterInfo,
            const bool collectStartPointHits,
            RE::hknpAllHitsCollector& collector)
        {
            if (!world || !shape || distanceGame <= 0.001f ||
                !shape->flags.all(
                    RE::hknpShape::FlagsEnum::kIsConvexShape)) {
                return false;
            }

            RE::NiPoint3 direction{};
            if (!normalize(directionGame, direction)) {
                return false;
            }
            auto* filterRef = getQueryFilterRef(world);
            if (!filterRef) {
                ROCK_LOG_WARN(Hand, "Shape cast skipped: missing hknp query filter");
                return false;
            }

            const auto startHavok = niPointToHkVector(startWorld.translate);
            const physics_shape_cast_math::ShapeCastVec4 start{
                startHavok.x,
                startHavok.y,
                startHavok.z,
                startHavok.w,
            };
            const physics_shape_cast_math::ShapeCastVec4 displacement{
                direction.x * distanceGame * gameToHavokScale(),
                direction.y * distanceGame * gameToHavokScale(),
                direction.z * distanceGame * gameToHavokScale(),
                1.0f,
            };
            auto query = physics_shape_cast_math::buildRuntimeShapeCastQuery(
                filterRef,
                const_cast<RE::hknpShape*>(shape),
                collisionFilterInfo,
                start,
                displacement);
            RE::hkTransformf transform{};
            transform.SetIdentity();
            transform.rotation = niRotToHkTransformRotation(startWorld.rotate);
            prepareAllHitsCollector(collector);
            {
                havok_world_lock::ScopedWorldReadLock worldReadLock(world);
                world->CastShape(
                    &query,
                    &transform,
                    &collector,
                    collectStartPointHits ? &collector : nullptr);
            }
            if (collector.hits._size > 0) {
                performance_profiler::addEventCount(
                    performance_profiler::Scope::SelectionCasts,
                    static_cast<std::uint64_t>(collector.hits._size));
            }
            return true;
        }
    }

    bool castSelectionSphere(RE::hknpWorld* world, const SphereCastInput& input, RE::hknpAllHitsCollector& collector, SphereCastDiagnostics* diagnostics)
    {
        performance_profiler::ScopedTimer profilerTimer(performance_profiler::Scope::SelectionCasts);

        if (diagnostics) {
            *diagnostics = SphereCastDiagnostics{};
            diagnostics->attempted = true;
            diagnostics->distanceGame = input.distanceGame;
            diagnostics->radiusGame = input.radiusGame;
            diagnostics->collisionFilterInfo = input.collisionFilterInfo;
        }

        if (!world || input.distanceGame <= 0.001f || input.radiusGame <= 0.001f) {
            return false;
        }

        const float radiusHavok = input.radiusGame * gameToHavokScale();
        auto* shape = getSelectionSphereShape(radiusHavok);
        if (diagnostics) {
            diagnostics->shapeReady = shape != nullptr;
        }
        if (!shape) {
            return false;
        }

        RE::NiTransform startWorld{};
        startWorld.rotate.MakeIdentity();
        startWorld.translate = input.startGame;
        startWorld.scale = 1.0f;
        if (!runShapeCast(
                world,
                shape,
                startWorld,
                input.directionGame,
                input.distanceGame,
                input.collisionFilterInfo,
                input.collectStartPointHits,
                collector)) {
            return false;
        }

        if (diagnostics) {
            diagnostics->castRan = true;
            diagnostics->hitCount = collector.hits._size;
        }
        return true;
    }

    bool castShape(RE::hknpWorld* world,
        const ShapeCastInput& input,
        RE::hknpAllHitsCollector& collector,
        ShapeCastDiagnostics* diagnostics)
    {
        performance_profiler::ScopedTimer profilerTimer(
            performance_profiler::Scope::SelectionCasts);
        if (diagnostics) {
            *diagnostics = ShapeCastDiagnostics{};
            diagnostics->attempted = true;
            diagnostics->shapeReady =
                input.shape &&
                input.shape->flags.all(
                    RE::hknpShape::FlagsEnum::kIsConvexShape);
            diagnostics->distanceGame = input.distanceGame;
            diagnostics->collisionFilterInfo = input.collisionFilterInfo;
        }

        if (!runShapeCast(
                world,
                input.shape,
                input.startWorld,
                input.directionGame,
                input.distanceGame,
                input.collisionFilterInfo,
                input.collectStartPointHits,
                collector)) {
            return false;
        }
        if (diagnostics) {
            diagnostics->castRan = true;
            diagnostics->hitCount = collector.hits._size;
        }
        return true;
    }

    bool castConservativeOrientedBox(
        RE::hknpWorld* world,
        const OrientedBoxCastInput& input,
        RE::hknpAllHitsCollector& collector,
        ShapeCastDiagnostics* diagnostics)
    {
        const RE::NiPoint3 span{
            input.localBoundsMaxGame.x - input.localBoundsMinGame.x,
            input.localBoundsMaxGame.y - input.localBoundsMinGame.y,
            input.localBoundsMaxGame.z - input.localBoundsMinGame.z,
        };
        const float absoluteScale = std::abs(input.startBodyWorld.scale);
        if (!std::isfinite(span.x) || !std::isfinite(span.y) ||
            !std::isfinite(span.z) || span.x < 0.0f || span.y < 0.0f ||
            span.z < 0.0f || !std::isfinite(absoluteScale) ||
            absoluteScale <= 0.0001f) {
            return false;
        }

        const RE::NiPoint3 localCenter{
            0.5f * (input.localBoundsMinGame.x +
                       input.localBoundsMaxGame.x),
            0.5f * (input.localBoundsMinGame.y +
                       input.localBoundsMaxGame.y),
            0.5f * (input.localBoundsMinGame.z +
                       input.localBoundsMaxGame.z),
        };
        const RE::NiPoint3 physicalHalfExtents{
            0.5f * span.x * absoluteScale,
            0.5f * span.y * absoluteScale,
            0.5f * span.z * absoluteScale,
        };
        auto* boxShape =
            getConservativeBoxShape(physicalHalfExtents);
        if (!boxShape) {
            return false;
        }

        RE::NiTransform boxWorld = input.startBodyWorld;
        boxWorld.translate = transform_math::localPointToWorld(
            input.startBodyWorld,
            localCenter);
        boxWorld.scale = 1.0f;
        return castShape(
            world,
            ShapeCastInput{
                .shape = boxShape,
                .startWorld = boxWorld,
                .directionGame = input.directionGame,
                .distanceGame = input.distanceGame,
                .collisionFilterInfo = input.collisionFilterInfo,
                .collectStartPointHits = input.collectStartPointHits,
            },
            collector,
            diagnostics);
    }
}
