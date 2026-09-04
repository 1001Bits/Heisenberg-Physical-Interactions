#pragma once

#include "physics-interaction/collision/PhysicsShapeCastMath.h"
#include "physics-interaction/hand/HandSelection.h"

#include "RE/Bethesda/BSHavok.h"
#include "RE/Havok/hknpAllHitsCollector.h"
#include "RE/Havok/hknpWorld.h"
#include "RE/NetImmerse/NiPoint.h"
#include "RE/NetImmerse/NiTransform.h"

namespace rock::physics_shape_cast
{
    constexpr std::uint32_t kSelectionQueryCollisionFilterInfo = selection_query_policy::kDefaultShapeCastFilterInfo;

    struct SphereCastInput
    {
        RE::NiPoint3 startGame{};
        RE::NiPoint3 directionGame{};
        float distanceGame = 0.0f;
        float radiusGame = 0.0f;
        std::uint32_t collisionFilterInfo = kSelectionQueryCollisionFilterInfo;
        bool collectStartPointHits = true;
    };

    struct SphereCastDiagnostics
    {
        bool attempted = false;
        bool shapeReady = false;
        bool castRan = false;
        int hitCount = 0;
        float distanceGame = 0.0f;
        float radiusGame = 0.0f;
        std::uint32_t collisionFilterInfo = 0;
    };

    struct ShapeCastInput
    {
        const RE::hknpShape* shape = nullptr;
        RE::NiTransform startWorld{};
        RE::NiPoint3 directionGame{};
        float distanceGame = 0.0f;
        std::uint32_t collisionFilterInfo = kSelectionQueryCollisionFilterInfo;
        bool collectStartPointHits = false;
    };

    struct ShapeCastDiagnostics
    {
        bool attempted = false;
        bool shapeReady = false;
        bool castRan = false;
        int hitCount = 0;
        float distanceGame = 0.0f;
        std::uint32_t collisionFilterInfo = 0;
    };

    /*
     * hknp only accepts convex query shapes.  A held body may instead expose a
     * mesh/static-compound root, so rotational CCD uses a cached convex box that
     * encloses the body's live AABB expressed back in body-local space.  Unlike
     * the old circumscribed-sphere fallback, this proxy rotates with the body and
     * can therefore witness pure angular wall crossings.
     */
    struct OrientedBoxCastInput
    {
        RE::NiPoint3 localBoundsMinGame{};
        RE::NiPoint3 localBoundsMaxGame{};
        RE::NiTransform startBodyWorld{};
        RE::NiPoint3 directionGame{};
        float distanceGame = 0.0f;
        std::uint32_t collisionFilterInfo =
            kSelectionQueryCollisionFilterInfo;
        bool collectStartPointHits = false;
    };

    bool castSelectionSphere(RE::hknpWorld* world, const SphereCastInput& input, RE::hknpAllHitsCollector& collector, SphereCastDiagnostics* diagnostics = nullptr);
    bool castShape(RE::hknpWorld* world,
        const ShapeCastInput& input,
        RE::hknpAllHitsCollector& collector,
        ShapeCastDiagnostics* diagnostics = nullptr);
    bool castConservativeOrientedBox(
        RE::hknpWorld* world,
        const OrientedBoxCastInput& input,
        RE::hknpAllHitsCollector& collector,
        ShapeCastDiagnostics* diagnostics = nullptr);
}
