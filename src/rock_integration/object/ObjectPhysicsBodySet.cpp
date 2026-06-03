// ObjectPhysicsBodySet scanner — Heisenberg adaptation (#109).
//
// Walks a grabbed object's NIF subtree, finds each part's EXISTING hknp body (via the proven
// collisionObject->spSystem + systemBodyIdx + bhkPhysicsSystem::GetBodyId path that Grab.cpp uses),
// classifies it (ported physics_body_classifier), and records accepted bodies so a multi-part
// object can grab the right primary. Uses only existing Heisenberg primitives — no new offsets:
//   GetBodyId(0x1e0c460) · TryReadBodyFilterInfo · GetRefrFromBodyId · GetPlayerBodyId.
//
// NOTE: motionType is left Unknown for now (reading hknpBody flags needs a separate verified
// offset); classification therefore runs on layer/ref/owner evidence. This is the body-DISCOVERY
// core; wiring it into the grab (multi-body grab) is the next integration step. Default-off.

#include "rock_integration/object/ObjectPhysicsBodySet.h"

#include "../../Physics.h"
#include "../../GrabConstraint.h"

#include "RE/Fallout.h"
#include <spdlog/spdlog.h>
#include <Windows.h>

namespace rock::object_physics_body_set
{
    // ---- ObjectPhysicsBodySet accessor definitions (header declares these) ----
    std::size_t ObjectPhysicsBodySet::acceptedCount() const
    {
        std::size_t n = 0;
        for (const auto& r : records) if (r.accepted) ++n;
        return n;
    }
    std::size_t ObjectPhysicsBodySet::rejectedCount() const { return records.size() - acceptedCount(); }

    std::vector<std::uint32_t> ObjectPhysicsBodySet::acceptedBodyIds() const
    {
        std::vector<std::uint32_t> out;
        for (const auto& r : records) if (r.accepted) out.push_back(r.bodyId);
        return out;
    }
    std::vector<std::uint32_t> ObjectPhysicsBodySet::uniqueAcceptedMotionBodyIds() const
    {
        std::vector<std::uint32_t> out; std::unordered_set<std::uint32_t> seen;
        for (const auto& r : records) if (r.accepted && seen.insert(r.motionId).second) out.push_back(r.bodyId);
        return out;
    }
    std::vector<const ObjectPhysicsBodyRecord*> ObjectPhysicsBodySet::uniqueAcceptedMotionRecords() const
    {
        std::vector<const ObjectPhysicsBodyRecord*> out; std::unordered_set<std::uint32_t> seen;
        for (const auto& r : records) if (r.accepted && seen.insert(r.motionId).second) out.push_back(&r);
        return out;
    }
    bool ObjectPhysicsBodySet::containsAcceptedBody(std::uint32_t bodyId) const
    {
        for (const auto& r : records) if (r.accepted && r.bodyId == bodyId) return true;
        return false;
    }
    const ObjectPhysicsBodyRecord* ObjectPhysicsBodySet::findRecord(std::uint32_t bodyId) const
    {
        for (const auto& r : records) if (r.bodyId == bodyId) return &r;
        return nullptr;
    }
    const ObjectPhysicsBodyRecord* ObjectPhysicsBodySet::findAcceptedRecordByOwnerNode(RE::NiAVObject* ownerNode) const
    {
        for (const auto& r : records) if (r.accepted && r.owningNode == ownerNode) return &r;
        return nullptr;
    }
    PrimaryBodyChoice ObjectPhysicsBodySet::choosePrimaryBody(std::uint32_t preferredBodyId, PurePoint3 targetPointGame) const
    {
        PureBodySet pure;
        for (const auto& r : records) if (r.accepted)
            pure.records.push_back(PureBodyRecord{ r.bodyId, r.motionId, true, r.positionGame, reinterpret_cast<std::uintptr_t>(r.owningNode) });
        return pure.choosePrimaryBody(preferredBodyId, targetPointGame);
    }
    PrimaryBodyChoice ObjectPhysicsBodySet::choosePrimaryBodyWithSurfaceOwner(std::uint32_t preferredBodyId, RE::NiAVObject* ownerNode, PurePoint3 targetPointGame) const
    {
        PureBodySet pure;
        for (const auto& r : records) if (r.accepted)
            pure.records.push_back(PureBodyRecord{ r.bodyId, r.motionId, true, r.positionGame, reinterpret_cast<std::uintptr_t>(r.owningNode) });
        return pure.choosePrimaryBodyWithSurfaceOwner(preferredBodyId, reinterpret_cast<std::uintptr_t>(ownerNode), targetPointGame);
    }

    // ---- scanner ----

    // SEH leaf (no C++ objects with non-trivial dtors): read the body id from a collision object's
    // physics system the same way Grab.cpp does. Returns INVALID_BODY_ID on any fault.
    static std::uint32_t SafeGetBodyId(RE::bhkNPCollisionObject* npco)
    {
        std::uint32_t bodyId = INVALID_BODY_ID;
        __try {
            void* sys = npco->spSystem.get();
            if (sys) {
                heisenberg::ConstraintFunctions::BhkPhysicsSystemGetBodyId(sys, &bodyId, npco->systemBodyIdx);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            bodyId = INVALID_BODY_ID;
        }
        return bodyId;
    }

    struct ScanCtx
    {
        RE::bhkWorld* bhkWorld = nullptr;
        RE::hknpWorld* hknpWorld = nullptr;
        const BodySetScanOptions* options = nullptr;
        ObjectPhysicsBodySet* out = nullptr;
        std::unordered_set<std::uint32_t>* seen = nullptr;
        std::uint32_t playerBodyId = INVALID_BODY_ID;
    };

    static void ScanNode(RE::NiAVObject* node, int depth, ScanCtx& ctx)
    {
        if (!node || depth < 0) return;
        ++ctx.out->diagnostics.visitedNodes;

        if (auto* co = node->collisionObject.get()) {
            ++ctx.out->diagnostics.collisionObjects;
            auto* npco = static_cast<RE::bhkNPCollisionObject*>(co);
            const std::uint32_t bodyId = SafeGetBodyId(npco);
            if (bodyId != INVALID_BODY_ID && ctx.seen->insert(bodyId).second) {
                std::uint32_t filterInfo = 0;
                heisenberg::Physics::TryReadBodyFilterInfo(ctx.hknpWorld, bodyId, filterInfo);
                const std::uint32_t layer = filterInfo & 0x7Fu;
                RE::TESObjectREFR* resolved = heisenberg::Physics::GetRefrFromBodyId(ctx.bhkWorld, bodyId);

                physics_body_classifier::BodyClassificationInput in;
                in.bodyId = bodyId;
                in.layer = layer;
                in.filterInfo = filterInfo;
                in.motionType = physics_body_classifier::BodyMotionType::Unknown;  // body-flag read pending
                in.targetKind = ctx.options->targetKind;
                in.referenceDeletedOrDisabled = resolved && (resolved->IsDeleted() || resolved->IsDisabled());
                in.isPlayerBody = (ctx.playerBodyId != INVALID_BODY_ID && bodyId == ctx.playerBodyId);
                in.isRockHandBody = (bodyId == ctx.options->rightHandBodyId || bodyId == ctx.options->leftHandBodyId);
                in.isRockWeaponSourceBody = (bodyId == ctx.options->sourceWeaponBodyId);

                const auto cls = physics_body_classifier::classifyBody(in, ctx.options->mode);

                ObjectPhysicsBodyRecord rec;
                rec.bodyId = bodyId;
                rec.collisionLayer = layer;
                rec.filterInfo = filterInfo;
                rec.motionType = in.motionType;
                rec.owningNode = node;
                rec.collisionObject = co;
                rec.resolvedRef = resolved;
                rec.refResolutionKnown = (resolved != nullptr);
                rec.accepted = cls.accepted;
                rec.rejectReason = cls.reason;
                const auto& p = node->world.translate;
                rec.positionGame = PurePoint3{ p.x, p.y, p.z };
                if (!cls.accepted) {
                    const auto ri = static_cast<std::size_t>(cls.reason);
                    if (ri < ctx.out->diagnostics.rejectCounts.size()) ++ctx.out->diagnostics.rejectCounts[ri];
                }
                ctx.out->records.push_back(rec);
            }
        }

        if (auto* asNode = node->IsNode()) {
            for (std::uint32_t i = 0; i < asNode->children.size(); ++i) {
                ScanNode(asNode->children[i].get(), depth - 1, ctx);
            }
        }
    }

    bool hasCollisionObjectInSubtree(RE::NiAVObject* root, int maxDepth)
    {
        if (!root || maxDepth < 0) return false;
        if (root->collisionObject) return true;
        if (auto* asNode = root->IsNode()) {
            for (std::uint32_t i = 0; i < asNode->children.size(); ++i) {
                if (hasCollisionObjectInSubtree(asNode->children[i].get(), maxDepth - 1)) return true;
            }
        }
        return false;
    }

    ObjectPhysicsBodySet scanObjectPhysicsBodySet(RE::bhkWorld* bhkWorld, RE::hknpWorld* hknpWorld,
                                                  RE::TESObjectREFR* ref, const BodySetScanOptions& options)
    {
        ObjectPhysicsBodySet result;
        result.rootRef = ref;
        if (!bhkWorld || !hknpWorld || !ref || ref->IsDeleted() || ref->IsDisabled()) return result;
        auto* rootNode = ref->Get3D();
        result.rootNode = rootNode;
        if (!rootNode) return result;

        std::unordered_set<std::uint32_t> seen;
        ScanCtx ctx;
        ctx.bhkWorld = bhkWorld;
        ctx.hknpWorld = hknpWorld;
        ctx.options = &options;
        ctx.out = &result;
        ctx.seen = &seen;
        ctx.playerBodyId = heisenberg::Physics::GetPlayerBodyId();

        ScanNode(rootNode, (std::max)(0, options.maxDepth), ctx);

        result.diagnostics.duplicateMotionSkips = 0;
        spdlog::debug("[ROCK::ObjectBodySet] scanned '{}': {} bodies ({} accepted, {} rejected)",
                      ref->GetFormID(), result.records.size(), result.acceptedCount(), result.rejectedCount());
        return result;
    }
}
