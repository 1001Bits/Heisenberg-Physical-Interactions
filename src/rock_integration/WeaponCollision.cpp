#include "WeaponCollision.h"
#include "../BethesdaPhysicsBody.h"
#include "../Config.h"
#include "../GrabConstraint.h"
#include "../Physics.h"
#include "../Utils.h"
#include "../DualWieldAPI.h"
#include "../DualWieldWeaponContact.h"
#include "../FingerCurves.h"         // GetTriangles — tight per-vertex weapon bounds
#include "weapon/WeaponGeometry.h"   // ported ROCK weapon-collision geometry math

#include "F4SE/F4SE.h"
#include "f4vr/PlayerNodes.h"
#include <spdlog/spdlog.h>

namespace heisenberg::rock_weapon_collision
{
    using BPB = heisenberg::bethesda_physics_body::BethesdaPhysicsBody;
    using MotionType = heisenberg::bethesda_physics_body::MotionType;

    constexpr float kConvexRadius = 0.10f;
    constexpr std::uint32_t kWeaponLayer = 43;   // share hand layer for the 0x000B-group filter
    constexpr std::uint32_t kWeaponFilterInfo = (0x000Bu << 16) | (kWeaponLayer & 0x7Fu);
    constexpr float kGameToHavok = 1.0f / 70.0f;

    // Fallback weapon hull when worldBound can't be read (rare). Sized to be a
    // rifle-ish guess so we always create SOMETHING when a weapon is detected.
    constexpr float kFallbackHalfX = 18.0f;
    constexpr float kFallbackHalfY = 1.5f;
    constexpr float kFallbackHalfZ = 3.0f;

    struct WeaponHand
    {
        BPB* body = nullptr;
        RE::NiPointer<RE::NiAVObject> weaponNode;  // "Weapon" or "WeaponLeft"
        bool wasEquipped = false;
        bool playerFilterApplied = false;  // DisableCollisionBetween(hull, playerBody) done?
        // Half-extents the body was created with (so we can detect "weapon swap" and rebuild).
        float halfX = 0.0f, halfY = 0.0f, halfZ = 0.0f;
        // AABB center in weapon-node-LOCAL space (game units). The hull box is centered at
        // body origin, so the body must be placed at node.world * localCenter each frame —
        // otherwise the box sits on the grip, not over the barrel, and the weapon mesh
        // penetrates objects before the offset-back hull touches them.
        RE::NiPoint3 localCenter{ 0.0f, 0.0f, 0.0f };
    };

    static WeaponHand g_right;
    static WeaponHand g_left;
    static void* g_hknpWorld = nullptr;
    static void* g_bhkWorld = nullptr;
    static bool s_initialized = false;
    static bool s_loggedDisabled = false;

    static bool WantsWeaponCollision()
    {
        return heisenberg::g_config.rockWeaponCollision ||
               HeisenbergPluginAPI::HasDualWieldStateProvider();
    }

    bool IsActive() { return s_initialized && WantsWeaponCollision(); }

    void Init()
    {
        if (!WantsWeaponCollision()) {
            if (!s_loggedDisabled) {
                spdlog::info("[ROCK::WeaponCollision] Disabled by config");
                s_loggedDisabled = true;
            }
            return;
        }
        spdlog::info("[ROCK::WeaponCollision] Init — tracking Weapon/WeaponLeft attachments (dual-wield provider can force-enable)");
        s_initialized = true;
    }

    // We hook equip/unequip indirectly: the Update loop watches whether the weapon
    // node has children (= weapon attached). These functions are explicit hooks if
    // a system wants to force re-evaluate (Heisenberg's weapon equip events could call them).
    void OnWeaponEquipped()   { if (IsActive()) spdlog::debug("[ROCK::WeaponCollision] OnWeaponEquipped — will pick up on next Update"); }
    void OnWeaponUnequipped() { if (IsActive()) spdlog::debug("[ROCK::WeaponCollision] OnWeaponUnequipped — will pick up on next Update"); }

    static void* BuildHull(float halfX, float halfY, float halfZ)
    {
        void* heMem = _aligned_malloc(sizeof(RE::NiPoint4), 16);
        if (!heMem) return nullptr;
        auto* hePtr = new (heMem) RE::NiPoint4(
            halfX * kGameToHavok, halfY * kGameToHavok, halfZ * kGameToHavok, 0.0f);
        void* bcMem = _aligned_malloc(sizeof(heisenberg::ConstraintFunctions::hknpConvexShapeBuildConfig), 16);
        if (!bcMem) { _aligned_free(heMem); return nullptr; }
        auto* bcPtr = reinterpret_cast<heisenberg::ConstraintFunctions::hknpConvexShapeBuildConfig*>(bcMem);
        heisenberg::ConstraintFunctions::BuildConfigCtor(bcPtr);
        // Convex radius in GAME units → havok (matches ROCK). See HandBoneColliderSet.
        void* shp = heisenberg::ConstraintFunctions::CreateConvexShapeFromHalfExtents(*hePtr, kConvexRadius * kGameToHavok, bcPtr);
        _aligned_free(bcMem);
        _aligned_free(heMem);
        return shp;
    }

    // Recursively walk the weapon NIF tree and combine each visible node's worldBound
    // into a single AABB in the weapon node's LOCAL space (so the body can be created
    // once with fixed half-extents and only have its TRANSFORM updated each frame).
    //
    // We work entirely in worldBound space (sphere center+radius in world coords),
    // transform back into weaponNode local, and inflate each sphere into its AABB.
    static void GatherBounds(RE::NiAVObject* node, const RE::NiAVObject* weaponRoot,
                             RE::NiPoint3& outMinLocal, RE::NiPoint3& outMaxLocal,
                             bool& any)
    {
        if (!node || (node->flags.flags & 0x1) /* hidden */) return;
        const float r = node->worldBound.fRadius;
        if (r > 0.001f) {
            // Sphere center in world → local
            RE::NiPoint3 worldC{ node->worldBound.center.x, node->worldBound.center.y, node->worldBound.center.z };
            // Local = inverseRot * (worldC - rootPos)  (using transposed rot for orthonormal R)
            RE::NiPoint3 rel = worldC - weaponRoot->world.translate;
            const auto& R = weaponRoot->world.rotate;
            RE::NiPoint3 local{
                R.entry[0][0]*rel.x + R.entry[1][0]*rel.y + R.entry[2][0]*rel.z,
                R.entry[0][1]*rel.x + R.entry[1][1]*rel.y + R.entry[2][1]*rel.z,
                R.entry[0][2]*rel.x + R.entry[1][2]*rel.y + R.entry[2][2]*rel.z };
            const RE::NiPoint3 lo{ local.x - r, local.y - r, local.z - r };
            const RE::NiPoint3 hi{ local.x + r, local.y + r, local.z + r };
            if (!any) {
                outMinLocal = lo;
                outMaxLocal = hi;
                any = true;
            } else {
                outMinLocal.x = (std::min)(outMinLocal.x, lo.x);
                outMinLocal.y = (std::min)(outMinLocal.y, lo.y);
                outMinLocal.z = (std::min)(outMinLocal.z, lo.z);
                outMaxLocal.x = (std::max)(outMaxLocal.x, hi.x);
                outMaxLocal.y = (std::max)(outMaxLocal.y, hi.y);
                outMaxLocal.z = (std::max)(outMaxLocal.z, hi.z);
            }
        }
        if (auto* asNode = node->IsNode()) {
            for (std::uint32_t i = 0; i < asNode->children.size(); ++i) {
                GatherBounds(asNode->children[i].get(), weaponRoot, outMinLocal, outMaxLocal, any);
            }
        }
    }

    // Per-weapon profiling: traverse the equipped weapon's NIF, build a single AABB
    // that covers all visible geometry. Returns half-extents (game units).
    // Falls back to kFallback* when no bounds found.
    static void ProfileWeapon(RE::NiAVObject* weaponNode,
                              float& outHalfX, float& outHalfY, float& outHalfZ,
                              RE::NiPoint3& outCenterLocal)
    {
        RE::NiPoint3 lo{}, hi{};
        bool any = false;
        GatherBounds(weaponNode, weaponNode, lo, hi, any);
        if (!any) {
            outHalfX = kFallbackHalfX;
            outHalfY = kFallbackHalfY;
            outHalfZ = kFallbackHalfZ;
            outCenterLocal = { 0.0f, 0.0f, 0.0f };
            return;
        }
        // Tight AABB: half-extents = (hi - lo) / 2, center = (hi + lo) / 2.
        float hx = (hi.x - lo.x) * 0.5f, hy = (hi.y - lo.y) * 0.5f, hz = (hi.z - lo.z) * 0.5f;
        RE::NiPoint3 c{ (hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f };

        // SANITY GUARD: a child with an unset/garbage worldBound (center at origin while the
        // weapon node is far from origin) inflates the AABB to span the weapon out to (0,0,0)
        // — a giant box centered far away that shoves distant world clutter. No real weapon
        // exceeds ~kMaxHalf; if the box or center is unreasonable, the bounds are bad → fall
        // back to a small box at the node center instead of a world-spanning monster.
        constexpr float kMaxHalf = 60.0f;     // game units (~85cm) — bigger than any weapon
        constexpr float kMaxCenter = 60.0f;
        const float cMag2 = c.x * c.x + c.y * c.y + c.z * c.z;
        if (!std::isfinite(hx) || !std::isfinite(hy) || !std::isfinite(hz) ||
            hx > kMaxHalf || hy > kMaxHalf || hz > kMaxHalf ||
            !std::isfinite(cMag2) || cMag2 > kMaxCenter * kMaxCenter) {
            static int s_badBoundsLog = 0;
            if ((s_badBoundsLog++ % 300) == 0) {  // throttle: this can fire every frame
                spdlog::warn("[ROCK::WeaponCollision] Bad weapon bounds (half=({:.0f},{:.0f},{:.0f}) center=({:.0f},{:.0f},{:.0f})) — using fallback (throttled)",
                             hx, hy, hz, c.x, c.y, c.z);
            }
            outHalfX = kFallbackHalfX;
            outHalfY = kFallbackHalfY;
            outHalfZ = kFallbackHalfZ;
            outCenterLocal = { 0.0f, 0.0f, 0.0f };
            return;
        }
        outHalfX = ((std::max))(0.5f, hx);
        outHalfY = ((std::max))(0.5f, hy);
        outHalfZ = ((std::max))(0.5f, hz);
        outCenterLocal = c;
    }

    // Collect the weapon's triangle vertices in weapon-LOCAL space (R^T·rel, relative to the
    // weapon node origin — NOT centered), for building a true convex hull. local = R^T*(world-pos)
    // means world = pos + R*local, so driving the body to the node's world transform with
    // localCenter=0 reproduces the exact world vertex positions. Returns false if no triangles.
    static bool GatherWeaponLocalPoints(RE::NiAVObject* weaponNode, std::vector<RE::NiPoint3>& out)
    {
        if (!weaponNode) return false;
        std::vector<heisenberg::TriangleData> tris;
        tris.reserve(1024);
        heisenberg::GetTriangles(weaponNode, tris);
        if (tris.empty()) return false;
        const RE::NiPoint3& rootPos = weaponNode->world.translate;
        const auto& R = weaponNode->world.rotate;
        out.clear();
        out.reserve(tris.size() * 3);
        for (const auto& t : tris) {
            for (const RE::NiPoint3* v : { &t.v0, &t.v1, &t.v2 }) {
                const RE::NiPoint3 rel{ v->x - rootPos.x, v->y - rootPos.y, v->z - rootPos.z };
                out.push_back(RE::NiPoint3{
                    R.entry[0][0]*rel.x + R.entry[1][0]*rel.y + R.entry[2][0]*rel.z,
                    R.entry[0][1]*rel.x + R.entry[1][1]*rel.y + R.entry[2][1]*rel.z,
                    R.entry[0][2]*rel.x + R.entry[1][2]*rel.y + R.entry[2][2]*rel.z });
            }
        }
        return !out.empty();
    }

    // Build a TRUE convex hull from the weapon's local vertex cloud via the Ghidra-RE'd
    // hknpConvexShape::createFromVertices @ 0x16d4b30 (the engine builds the hull internally).
    // Points are weapon-local game units (node-relative); we cap the count (the hull only needs
    // extremes), pack as Havok-scaled hkVector4 (16-byte stride), and call the factory. Returns
    // null on failure → caller falls back to the tight AABB box.
    static void* BuildConvexHull(const std::vector<RE::NiPoint3>& localPoints)
    {
        namespace cf = heisenberg::ConstraintFunctions;
        namespace wg = heisenberg::rock_core::weapon_collision_geometry_math;
        if (localPoints.size() < 4) return nullptr;
        constexpr std::size_t kMaxHullPoints = 200;
        std::vector<RE::NiPoint3> pts = wg::limitPointCloud(localPoints, kMaxHullPoints);
        if (pts.size() < 4) return nullptr;

        std::vector<float> packed(pts.size() * 4);
        for (std::size_t i = 0; i < pts.size(); ++i) {
            packed[i*4+0] = pts[i].x * kGameToHavok;
            packed[i*4+1] = pts[i].y * kGameToHavok;
            packed[i*4+2] = pts[i].z * kGameToHavok;
            packed[i*4+3] = 0.0f;
        }
        cf::hkStridedVertices verts;
        verts.vertices   = packed.data();
        verts.numVertices = static_cast<std::int32_t>(pts.size());
        verts.striding    = 16;  // bytes per packed hkVector4

        void* bcMem = _aligned_malloc(sizeof(cf::hknpConvexShapeBuildConfig), 16);
        if (!bcMem) return nullptr;
        auto* bcPtr = reinterpret_cast<cf::hknpConvexShapeBuildConfig*>(bcMem);
        cf::BuildConfigCtor(bcPtr);
        void* shp = cf::CreateConvexShapeFromVertices(verts, kConvexRadius * kGameToHavok, bcPtr);
        _aligned_free(bcMem);
        return shp;
    }

    // Heuristic: returns true if the weapon node has children (a weapon NIF is attached).
    static bool WeaponNodeHasContent(RE::NiAVObject* weaponNode)
    {
        if (!weaponNode) return false;
        auto* asNode = weaponNode->IsNode();
        if (!asNode) return false;
        // If at least one child is present, treat as "weapon attached".
        for (std::uint32_t i = 0; i < asNode->children.size(); ++i) {
            if (asNode->children[i]) return true;
        }
        return false;
    }

    static void EnsureBody(
        WeaponHand& hand, void* hknpWorld, void* bhkWorld, const char* label, bool isLeft)
    {
        if (hand.body || !hand.weaponNode) return;
        namespace wg = heisenberg::rock_core::weapon_collision_geometry_math;

        // Swap-detection baseline MUST stay in the worldBound metric — the per-frame swap check
        // (below, in Update) re-profiles via ProfileWeapon/worldBound and compares to halfX/Y/Z.
        // It is decoupled from the actual shape sizing.
        float wbHx, wbHy, wbHz; RE::NiPoint3 wbCenter;
        ProfileWeapon(hand.weaponNode.get(), wbHx, wbHy, wbHz, wbCenter);

        void* shape = nullptr;
        const char* shapeKind = "AABB box";
        std::size_t nPts = 0;

        // Preferred: a TRUE convex hull from the weapon's vertex cloud (node-relative local pts).
        std::vector<RE::NiPoint3> localPts;
        if (GatherWeaponLocalPoints(hand.weaponNode.get(), localPts)) {
            nPts = localPts.size();
            shape = BuildConvexHull(localPts);
            if (shape) {
                shapeKind = "convex hull";
                hand.localCenter = { 0.0f, 0.0f, 0.0f };  // hull is node-relative
            }
        }

        // Fallback: tight per-vertex AABB box (or worldBound box if the NIF yielded no triangles).
        if (!shape) {
            float thx = wbHx, thy = wbHy, thz = wbHz;
            RE::NiPoint3 tCenter = wbCenter;
            if (!localPts.empty()) {
                RE::NiPoint3 lo = localPts.front(), hi = localPts.front();
                for (const auto& p : localPts) { lo = wg::pointMin(lo, p); hi = wg::pointMax(hi, p); }
                thx = (std::max)(0.5f, (hi.x - lo.x) * 0.5f);
                thy = (std::max)(0.5f, (hi.y - lo.y) * 0.5f);
                thz = (std::max)(0.5f, (hi.z - lo.z) * 0.5f);
                tCenter = { (hi.x + lo.x) * 0.5f, (hi.y + lo.y) * 0.5f, (hi.z + lo.z) * 0.5f };
                // Bad-vertex guard (mirrors ProfileWeapon): a world-spanning box → worldBound box.
                constexpr float kMaxHalf = 60.0f, kMaxCenter = 60.0f;
                const float cMag2 = tCenter.x*tCenter.x + tCenter.y*tCenter.y + tCenter.z*tCenter.z;
                if (thx > kMaxHalf || thy > kMaxHalf || thz > kMaxHalf || cMag2 > kMaxCenter*kMaxCenter) {
                    thx = wbHx; thy = wbHy; thz = wbHz; tCenter = wbCenter;
                }
            }
            shape = BuildHull(thx, thy, thz);
            hand.localCenter = tCenter;
        }

        if (!shape) {
            spdlog::error("[ROCK::WeaponCollision] {} shape build failed", label);
            return;
        }
        auto* bb = new BPB();
        if (!bb->Create(bhkWorld, hknpWorld, shape, kWeaponLayer, MotionType::Keyframed, label)) {
            spdlog::error("[ROCK::WeaponCollision] {} BPB::Create failed", label);
            delete bb;
            return;
        }
        heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, bb->GetBodyId(), kWeaponFilterInfo);
        hand.body = bb;
        hand.halfX = wbHx;  // swap-detection baseline (worldBound metric)
        hand.halfY = wbHy;
        hand.halfZ = wbHz;
        if (!heisenberg::dual_wield_contact::Subscribe(
                hknpWorld, bb->GetBodyId(), isLeft)) {
            spdlog::warn("[ROCK::WeaponCollision] {} contact bridge subscription failed", label);
        }
        spdlog::info("[ROCK::WeaponCollision] {} body created ({}, {} verts, swapHalf=({:.1f},{:.1f},{:.1f}))",
                     label, shapeKind, nPts, wbHx, wbHy, wbHz);
    }

    static void DestroyBody(WeaponHand& hand, const char* label, bool isLeft)
    {
        heisenberg::dual_wield_contact::Unsubscribe(isLeft);
        if (!hand.body) return;
        // Drop any proxy-cached support pointer to this body before freeing it
        // (refcounted dangling pointer = char-proxy crash). See Physics helper.
        heisenberg::Physics::ClearPlayerProxySupportBody();
        hand.body->Destroy(g_bhkWorld);
        delete hand.body;
        hand.body = nullptr;
        hand.playerFilterApplied = false;
        spdlog::info("[ROCK::WeaponCollision] {} body destroyed", label);
    }

    void Shutdown()
    {
        if (!s_initialized) return;
        DestroyBody(g_right, "RIGHT", false);
        DestroyBody(g_left,  "LEFT", true);
        g_right.weaponNode.reset();
        g_left.weaponNode.reset();
        g_right.wasEquipped = false;
        g_left.wasEquipped = false;
        g_hknpWorld = g_bhkWorld = nullptr;
        spdlog::info("[ROCK::WeaponCollision] Shutdown (bodies torn down; module stays live)");
        // Do NOT clear s_initialized — see HandBoneColliderSet::Shutdown. Bodies + cached
        // weapon nodes are dropped; Update() re-detects the equipped weapon and rebuilds.
    }

    static void DriveWeaponBody(WeaponHand& hand)
    {
        if (!hand.body || !hand.weaponNode) return;
        const auto& w = hand.weaponNode->world;
        // Geometry centroid in world: world = node.translate + node.rotate * localCenter
        // (GatherBounds derived localCenter via local = node.rotate^T * (world - translate)).
        const auto& R = w.rotate;
        const RE::NiPoint3& lc = hand.localCenter;
        const RE::NiPoint3 worldCenter{
            w.translate.x + (R.entry[0][0] * lc.x + R.entry[0][1] * lc.y + R.entry[0][2] * lc.z),
            w.translate.y + (R.entry[1][0] * lc.x + R.entry[1][1] * lc.y + R.entry[1][2] * lc.z),
            w.translate.z + (R.entry[2][0] * lc.x + R.entry[2][1] * lc.y + R.entry[2][2] * lc.z) };
        static alignas(16) float s_hk[16];
        s_hk[0] = w.rotate.entry[0][0]; s_hk[1] = w.rotate.entry[1][0]; s_hk[2]  = w.rotate.entry[2][0]; s_hk[3]  = 0.0f;
        s_hk[4] = w.rotate.entry[0][1]; s_hk[5] = w.rotate.entry[1][1]; s_hk[6]  = w.rotate.entry[2][1]; s_hk[7]  = 0.0f;
        s_hk[8] = w.rotate.entry[0][2]; s_hk[9] = w.rotate.entry[1][2]; s_hk[10] = w.rotate.entry[2][2]; s_hk[11] = 0.0f;
        s_hk[12] = worldCenter.x * kGameToHavok;
        s_hk[13] = worldCenter.y * kGameToHavok;
        s_hk[14] = worldCenter.z * kGameToHavok;
        s_hk[15] = 1.0f;
        hand.body->SetTransform(s_hk);
    }

    static void HandleHand(
        WeaponHand& hand,
        RE::NiAVObject* skeletonRoot,
        const char* nodeName,
        const char* label,
        bool isLeft,
        RE::NiAVObject* providerNode = nullptr,
        bool providerAuthoritative = false)
    {
        // Throttled collision-check log (~every 120 frames) so the weapon hull state is
        // visible without spamming. Covers all three cases: node missing, equipped/empty,
        // body built/not.
        static int s_throttle = 0;
        const bool logNow = ((s_throttle++ % 120) == 0);

        if (providerAuthoritative && hand.weaponNode.get() != providerNode) {
            DestroyBody(hand, label, isLeft);
            hand.weaponNode.reset(providerNode);
            hand.wasEquipped = false;
        } else if (!providerAuthoritative && !hand.weaponNode) {
            auto* wn = Utils::FindNode(skeletonRoot, nodeName);
            if (!wn) {
                if (logNow) spdlog::info("[ROCK::WeaponCollision] {} check: node '{}' not found under firstPersonSkeleton (no weapon attach point)", label, nodeName);
                return;
            }
            hand.weaponNode.reset(wn);
            spdlog::info("[ROCK::WeaponCollision] {} found weapon attach node '{}'", label, nodeName);
        }
        const bool equipped = providerAuthoritative ?
            providerNode != nullptr :
            WeaponNodeHasContent(hand.weaponNode.get());
        if (logNow) {
            spdlog::info("[ROCK::WeaponCollision] {} check: equipped={} hull={} (half {:.2f},{:.2f},{:.2f})",
                         label, equipped ? "yes" : "no", hand.body ? "built" : "none",
                         hand.halfX, hand.halfY, hand.halfZ);
        }

        // Weapon-swap detection: while a body exists, re-profile the equipped weapon's
        // bounds; if the AABB diverges significantly from what we built with (≥25% on
        // any axis), tear down + rebuild with the new size. Captures "swap rifle → pistol".
        if (equipped && hand.body) {
            float nhx, nhy, nhz;
            RE::NiPoint3 nCenter{};
            ProfileWeapon(hand.weaponNode.get(), nhx, nhy, nhz, nCenter);
            const float divX = std::fabs(nhx - hand.halfX) / ((std::max))(0.5f, hand.halfX);
            const float divY = std::fabs(nhy - hand.halfY) / ((std::max))(0.5f, hand.halfY);
            const float divZ = std::fabs(nhz - hand.halfZ) / ((std::max))(0.5f, hand.halfZ);
            if (divX > 0.25f || divY > 0.25f || divZ > 0.25f) {
                spdlog::info("[ROCK::WeaponCollision] {} weapon size changed ({:.1f},{:.1f},{:.1f})->({:.1f},{:.1f},{:.1f}) — rebuild",
                             label, hand.halfX, hand.halfY, hand.halfZ, nhx, nhy, nhz);
                DestroyBody(hand, label, isLeft);
            }
        }

        if (equipped && !hand.body) {
            EnsureBody(hand, g_hknpWorld, g_bhkWorld, label, isLeft);
        } else if (!equipped && hand.wasEquipped) {
            DestroyBody(hand, label, isLeft);
        }
        hand.wasEquipped = equipped;

        // Crash fix: keep the weapon hull out of the player character proxy (the 0x000B group
        // bits don't exclude it — see BodyBoneColliderSet). Per-pair disable vs the player body.
        if (hand.body && !hand.playerFilterApplied && g_hknpWorld) {
            const std::uint32_t pb = heisenberg::Physics::GetPlayerBodyId();
            if (pb != 0x7FFFFFFFu && pb != 0) {
                heisenberg::Physics::DisableCollisionBetween(g_hknpWorld, hand.body->GetBodyId(), pb);
                hand.playerFilterApplied = true;
                spdlog::info("[ROCK::WeaponCollision] {} player-pair filter applied (hull vs player 0x{:08X})", label, pb);
            }
        }

        if (equipped) DriveWeaponBody(hand);
    }

    void Update()
    {
        // Also handles a late dependency handshake without requiring a reload.
        if (!s_initialized && HeisenbergPluginAPI::HasDualWieldStateProvider()) {
            Init();
        }
        if (!IsActive()) {
            // A provider can force-enable this module over a disabled legacy
            // setting. Tear its bodies down on provider loss instead of leaving
            // ghost keyframed hulls behind until the next world shutdown.
            if (s_initialized &&
                (g_right.body || g_left.body || g_right.weaponNode.get() ||
                 g_left.weaponNode.get())) {
                Shutdown();
            }
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return;
        auto* bhk = player->parentCell->GetbhkWorld();
        if (!bhk) return;
        void* hknp = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
        if (!hknp) return;

        // World-change detection: a cell load frees the old hknp/bhk world. If we cached a
        // stale world (and possibly built weapon bodies in it), drop the bodies + cached
        // weapon-node pointers and re-cache, so we never drive bodies in freed memory.
        if (g_hknpWorld && g_hknpWorld != hknp) {
            spdlog::info("[ROCK::WeaponCollision] World changed → drop weapon bodies");
            DestroyBody(g_right, "RIGHT", false);
            DestroyBody(g_left, "LEFT", true);
            g_right.weaponNode.reset();
            g_left.weaponNode.reset();
            g_right.wasEquipped = false;
            g_left.wasEquipped = false;
        }
        g_bhkWorld = bhk;
        g_hknpWorld = hknp;

        // The equipped-weapon node ("Weapon" / "WeaponLeft") lives under firstPersonSkeleton
        // (getWeaponNode() == findNode(firstPersonSkeleton,"Weapon")) — NOT playerworldnode.
        // Searching playerworldnode found nothing, so weapon collision never built.
        auto* root = f4cf::f4vr::getFirstPersonSkeleton();
        if (!root) return;

        if (HeisenbergPluginAPI::HasDualWieldStateProvider()) {
            auto rightState = HeisenbergPluginAPI::MakeHandState(
                HeisenbergPluginAPI::PhysicalHand::kRight);
            auto leftState = HeisenbergPluginAPI::MakeHandState(
                HeisenbergPluginAPI::PhysicalHand::kLeft);
            const bool hasRight = HeisenbergPluginAPI::QueryDualWieldHandState(
                HeisenbergPluginAPI::PhysicalHand::kRight, rightState);
            const bool hasLeft = HeisenbergPluginAPI::QueryDualWieldHandState(
                HeisenbergPluginAPI::PhysicalHand::kLeft, leftState);

            auto getRoot = [](bool available, const HeisenbergPluginAPI::DualWieldHandState& state) {
                const bool occupied = (state.flags & HeisenbergPluginAPI::kHandStateOccupied) != 0;
                const bool rootValid = (state.flags & HeisenbergPluginAPI::kHandStateWeaponRootValid) != 0;
                const bool categoryValid = state.itemCategory != HeisenbergPluginAPI::ItemCategory::kNone;
                return available && occupied && rootValid && categoryValid ? state.weaponRoot : nullptr;
            };

            HandleHand(
                g_right, root, "Weapon", "RIGHT", false,
                getRoot(hasRight, rightState), true);
            HandleHand(
                g_left, root, "WeaponLeft", "LEFT", true,
                getRoot(hasLeft, leftState), true);
        } else {
            // Build-2 behavior remains the fallback when no provider owns the slot.
            HandleHand(g_right, root, "Weapon", "RIGHT", false);
            HandleHand(g_left, root, "WeaponLeft", "LEFT", true);
        }
    }

    // Geometry-only weapon capsule for the mode-3 soft-contact runtime. Finds the equipped
    // weapon node under firstPersonSkeleton, profiles its AABB (reusing ProfileWeapon), and
    // returns a world-space capsule along the weapon's longest local axis. Independent of the
    // physics hull / bWeaponCollision.
    bool BuildWeaponCapsule(bool isLeft, heisenberg::rock_core::soft_contact_math::Capsule& out)
    {
        out = {};
        auto* root = f4cf::f4vr::getFirstPersonSkeleton();
        if (!root) {
            return false;
        }
        const char* nodeName = isLeft ? "WeaponLeft" : "Weapon";
        auto* wn = Utils::FindNode(root, nodeName);
        if (!wn || !WeaponNodeHasContent(wn)) {
            return false;
        }

        float hx = 0.0f, hy = 0.0f, hz = 0.0f;
        RE::NiPoint3 cLocal{};
        ProfileWeapon(wn, hx, hy, hz, cLocal);

        // Capsule along the weapon's LONGEST local axis; radius = larger of the other two.
        int axis = 0;
        float halfLen = hx;
        float radius = (std::max)(hy, hz);
        if (hy >= hx && hy >= hz) {
            axis = 1;
            halfLen = hy;
            radius = (std::max)(hx, hz);
        } else if (hz >= hx && hz >= hy) {
            axis = 2;
            halfLen = hz;
            radius = (std::max)(hx, hy);
        }

        RE::NiPoint3 axisLocal{ 0.0f, 0.0f, 0.0f };
        if (axis == 0) {
            axisLocal.x = halfLen;
        } else if (axis == 1) {
            axisLocal.y = halfLen;
        } else {
            axisLocal.z = halfLen;
        }
        const RE::NiPoint3 startL{ cLocal.x - axisLocal.x, cLocal.y - axisLocal.y, cLocal.z - axisLocal.z };
        const RE::NiPoint3 endL{ cLocal.x + axisLocal.x, cLocal.y + axisLocal.y, cLocal.z + axisLocal.z };

        // world = pos + R * local (matches GatherBounds' local = R^T*(world - pos)).
        const auto& W = wn->world;
        const auto& R = W.rotate;
        auto toWorld = [&](const RE::NiPoint3& l) -> RE::NiPoint3 {
            return RE::NiPoint3(
                W.translate.x + R.entry[0][0] * l.x + R.entry[0][1] * l.y + R.entry[0][2] * l.z,
                W.translate.y + R.entry[1][0] * l.x + R.entry[1][1] * l.y + R.entry[1][2] * l.z,
                W.translate.z + R.entry[2][0] * l.x + R.entry[2][1] * l.y + R.entry[2][2] * l.z);
        };
        out.start = toWorld(startL);
        out.end = toWorld(endL);
        out.radius = (std::max)(0.5f, radius);
        out.id = isLeft ? 501u : 500u;
        out.valid = true;
        return true;
    }
}
