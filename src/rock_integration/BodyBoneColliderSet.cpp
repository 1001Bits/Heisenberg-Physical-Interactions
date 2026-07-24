#include "BodyBoneColliderSet.h"
#include "../BethesdaPhysicsBody.h"
#include "../Config.h"
#include "../GrabConstraint.h"
#include "../Physics.h"
#include "../Utils.h"

#include "F4SE/F4SE.h"
#include "f4vr/PlayerNodes.h"
#include "f4vr/F4VRUtils.h"
#include <cmath>
#include <spdlog/spdlog.h>

namespace heisenberg::rock_body_collider
{
    using BPB = heisenberg::bethesda_physics_body::BethesdaPhysicsBody;
    using MotionType = heisenberg::bethesda_physics_body::MotionType;

    constexpr float kConvexRadius = 0.10f;
    constexpr std::uint32_t kHandLayer = 43;
    constexpr std::uint32_t kBodyFilterInfo = (0x000Bu << 16) | (kHandLayer & 0x7Fu);
    constexpr float kGameToHavok = 1.0f / 70.0f;

    // 23-descriptor array, matching ROCK's kStandardBodyColliderDescriptors verbatim
    // (Source: SkeletonBoneDebugMath.h:135). Each row: startBone, endBone, length, radius (game units).
    struct Descriptor
    {
        const char* startBone;
        const char* endBone;
        float length;
        float radius;
    };

    // Standard (non-PA) descriptors — verbatim from ROCK kStandardBodyColliderDescriptors
    static constexpr Descriptor kStandard[] = {
        { "Pelvis","SPINE1",4.5f,0.50f },{ "SPINE1","SPINE2",5.0f,0.50f },{ "SPINE2","Chest",5.5f,0.50f },
        { "Chest","Neck",3.5f,0.40f },{ "Neck","Head",3.0f,0.35f },
        { "LArm_Collarbone","LArm_UpperArm",2.5f,0.30f },{ "LArm_UpperArm","LArm_ForeArm1",2.7f,0.30f },
        { "LArm_ForeArm1","LArm_ForeArm2",2.2f,0.25f },{ "LArm_ForeArm2","LArm_ForeArm3",2.0f,0.25f },
        { "LArm_ForeArm3","LArm_Hand",2.1f,0.25f },
        { "RArm_Collarbone","RArm_UpperArm",2.5f,0.30f },{ "RArm_UpperArm","RArm_ForeArm1",2.7f,0.30f },
        { "RArm_ForeArm1","RArm_ForeArm2",2.2f,0.25f },{ "RArm_ForeArm2","RArm_ForeArm3",2.0f,0.25f },
        { "RArm_ForeArm3","RArm_Hand",2.1f,0.25f },
        { "Pelvis","LLeg_Thigh",3.4f,0.35f },{ "LLeg_Thigh","LLeg_Calf",3.0f,0.35f },
        { "LLeg_Calf","LLeg_Foot",2.4f,0.30f },{ "LLeg_Foot","LLeg_Toe1",2.2f,0.25f },
        { "Pelvis","RLeg_Thigh",3.4f,0.35f },{ "RLeg_Thigh","RLeg_Calf",3.0f,0.35f },
        { "RLeg_Calf","RLeg_Foot",2.4f,0.30f },{ "RLeg_Foot","RLeg_Toe1",2.2f,0.25f },
    };
    // Power Armor variant — verbatim from ROCK kPowerArmorBodyColliderDescriptors.
    // Larger radii + lengths to match the bulkier armor silhouette.
    static constexpr Descriptor kPowerArmor[] = {
        { "Pelvis","SPINE1",6.5f,0.65f },{ "SPINE1","SPINE2",7.0f,0.65f },{ "SPINE2","Chest",7.5f,0.65f },
        { "Chest","Neck",5.0f,0.50f },{ "Neck","Head",4.0f,0.45f },
        { "LArm_Collarbone","LArm_UpperArm",4.2f,0.45f },{ "LArm_UpperArm","LArm_ForeArm1",4.5f,0.45f },
        { "LArm_ForeArm1","LArm_ForeArm2",3.8f,0.40f },{ "LArm_ForeArm2","LArm_ForeArm3",3.4f,0.40f },
        { "LArm_ForeArm3","LArm_Hand",3.5f,0.35f },
        { "RArm_Collarbone","RArm_UpperArm",4.2f,0.45f },{ "RArm_UpperArm","RArm_ForeArm1",4.5f,0.45f },
        { "RArm_ForeArm1","RArm_ForeArm2",3.8f,0.40f },{ "RArm_ForeArm2","RArm_ForeArm3",3.4f,0.40f },
        { "RArm_ForeArm3","RArm_Hand",3.5f,0.35f },
        { "Pelvis","LLeg_Thigh",5.2f,0.50f },{ "LLeg_Thigh","LLeg_Calf",4.8f,0.50f },
        { "LLeg_Calf","LLeg_Foot",4.0f,0.45f },{ "LLeg_Foot","LLeg_Toe1",3.5f,0.35f },
        { "Pelvis","RLeg_Thigh",5.2f,0.50f },{ "RLeg_Thigh","RLeg_Calf",4.8f,0.50f },
        { "RLeg_Calf","RLeg_Foot",4.0f,0.45f },{ "RLeg_Foot","RLeg_Toe1",3.5f,0.35f },
    };
    constexpr int kBodyCount = sizeof(kStandard) / sizeof(kStandard[0]);
    static_assert(kBodyCount == 23, "expected 23 body descriptors");
    static_assert(sizeof(kPowerArmor) / sizeof(kPowerArmor[0]) == 23, "PA variant should be 23");

    // Live descriptor pointer — flipped on PA state change, triggers rebuild.
    static const Descriptor* g_active = kStandard;
    static bool g_lastPaState = false;
    // Last fHandColliderRadiusPadding value the real capsule bodies were built with —
    // lets Update() detect a live MCM slider change and force a rebuild (see Update()).
    static float g_lastAppliedPadding = 0.0f;

    struct Instance
    {
        BPB* body = nullptr;
        RE::NiPointer<RE::NiAVObject> startBone;
        RE::NiPointer<RE::NiAVObject> endBone;
    };

    static Instance g_bodies[kBodyCount];
    static void* g_hknpWorld = nullptr;
    static void* g_bhkWorld = nullptr;
    static bool g_built = false;
    static bool g_playerFilterApplied = false;  // DisableCollisionBetween(capsule, playerBody) done?
    static bool s_initialized = false;
    static bool s_loggedDisabled = false;

    bool IsActive() { return s_initialized && heisenberg::g_config.rockBodyBoneColliderSet; }

    void Init()
    {
        if (!heisenberg::g_config.rockBodyBoneColliderSet) {
            if (!s_loggedDisabled) {
                spdlog::info("[ROCK::BodyBoneCollider] Disabled by config");
                s_loggedDisabled = true;
            }
            return;
        }
        spdlog::info("[ROCK::BodyBoneCollider] Init — will build {} capsule bodies on first Update", kBodyCount);
        s_initialized = true;
    }

    // Same proven aligned-malloc shape builder.
    static void* BuildHull(float lengthHalf, float radius)
    {
        void* heMem = _aligned_malloc(sizeof(RE::NiPoint4), 16);
        if (!heMem) return nullptr;
        auto* hePtr = new (heMem) RE::NiPoint4(
            lengthHalf * kGameToHavok, radius * kGameToHavok, radius * kGameToHavok, 0.0f);
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

    static bool BuildAll(RE::NiAVObject* skeletonRoot, void* hknpWorld, void* bhkWorld)
    {
        int created = 0;
        for (int i = 0; i < kBodyCount; ++i) {
            const auto& d = g_active[i];
            // f4vr::findAVObject = recursive name search (same helper PipboyInteraction
            // uses against commonNode). Utils::FindNode does the same logic but was
            // searching the wrong root (playerworldnode) — fixed at the call site below.
            auto* s = f4vr::findAVObject(skeletonRoot, d.startBone);
            auto* e = f4vr::findAVObject(skeletonRoot, d.endBone);
            if (!s || !e) continue;
            g_bodies[i].startBone.reset(s);
            g_bodies[i].endBone.reset(e);

            // fHandColliderRadiusPadding pads the REAL physics capsule radius here (not just
            // the geometry-only soft-contact copy in BuildBodyCapsules below) so resting
            // objects actually clip less against the forearm/torso, matching HandBoneColliderSet's
            // roleRadius() which reaches both its physics-body and soft-contact build sites.
            void* shape = BuildHull(d.length * 0.5f, d.radius + heisenberg::g_config.handColliderRadiusPadding);
            if (!shape) continue;

            auto* bb = new BPB();
            std::string name = std::string(d.startBone) + "->" + d.endBone;
            if (!bb->Create(bhkWorld, hknpWorld, shape, kHandLayer, MotionType::Keyframed, name.c_str())) {
                delete bb;
                continue;
            }
            heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, bb->GetBodyId(), kBodyFilterInfo);
            g_bodies[i].body = bb;
            ++created;
        }
        g_hknpWorld = hknpWorld;
        g_bhkWorld = bhkWorld;
        g_built = (created >= 1);
        // Same throttling as HandBoneColliderSet — only log on success or once every
        // ~600 frames (~10 s) of failure. Stops the per-frame log spam.
        static int s_failCount = 0;
        if (created > 0) {
            s_failCount = 0;
            spdlog::info("[ROCK::BodyBoneCollider] Built {}/{} avatar capsule bodies", created, kBodyCount);
        } else if ((s_failCount++ % 600) == 0) {
            spdlog::warn("[ROCK::BodyBoneCollider] Built 0/{} — bone resolution failed (root={}, attempts={}). FRIK avatar likely not ready.",
                         kBodyCount,
                         skeletonRoot && skeletonRoot->name.c_str() ? skeletonRoot->name.c_str() : "<noname>",
                         s_failCount);
        }
        return g_built;
    }

    void Shutdown()
    {
        if (!s_initialized) return;
        for (auto& inst : g_bodies) {
            if (inst.body) {
                inst.body->Destroy(g_bhkWorld);
                delete inst.body;
                inst.body = nullptr;
            }
            inst.startBone.reset();
            inst.endBone.reset();
        }
        g_built = false;
        g_hknpWorld = g_bhkWorld = nullptr;
        spdlog::info("[ROCK::BodyBoneCollider] Shutdown (bodies torn down; module stays live)");
        // Do NOT clear s_initialized — see HandBoneColliderSet::Shutdown. g_built=false above
        // makes Update() rebuild the capsule set in the new world.
    }

    // Build a column-major hkTransformf such that the box's X axis points from
    // start→end, position = midpoint, rotation oriented to that direction.
    // Uses a simple "look-along" — picks an arbitrary up vector (world Z) and
    // builds an orthonormal basis. Good enough for capsule-style segments.
    static void WriteCapsuleXform(float* outHkXform16,
                                  const RE::NiPoint3& startPos, const RE::NiPoint3& endPos)
    {
        const RE::NiPoint3 dGame = endPos - startPos;
        const float lenGame = std::sqrt(dGame.x*dGame.x + dGame.y*dGame.y + dGame.z*dGame.z);
        if (lenGame < 1e-3f) {
            // Degenerate — emit identity at start position
            for (int i = 0; i < 16; ++i) outHkXform16[i] = 0.0f;
            outHkXform16[0] = outHkXform16[5] = outHkXform16[10] = outHkXform16[15] = 1.0f;
            outHkXform16[12] = startPos.x * kGameToHavok;
            outHkXform16[13] = startPos.y * kGameToHavok;
            outHkXform16[14] = startPos.z * kGameToHavok;
            return;
        }
        const float invLen = 1.0f / lenGame;
        const RE::NiPoint3 axisX{ dGame.x * invLen, dGame.y * invLen, dGame.z * invLen };

        // Pick an up reference not parallel to axisX
        RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
        if (std::fabs(axisX.z) > 0.99f) {
            up = RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
        }
        // axisY = normalize(cross(up, axisX))
        RE::NiPoint3 axisY{
            up.y * axisX.z - up.z * axisX.y,
            up.z * axisX.x - up.x * axisX.z,
            up.x * axisX.y - up.y * axisX.x };
        float yLen = std::sqrt(axisY.x*axisY.x + axisY.y*axisY.y + axisY.z*axisY.z);
        if (yLen > 1e-6f) {
            axisY = { axisY.x / yLen, axisY.y / yLen, axisY.z / yLen };
        }
        // axisZ = cross(axisX, axisY)
        const RE::NiPoint3 axisZ{
            axisX.y * axisY.z - axisX.z * axisY.y,
            axisX.z * axisY.x - axisX.x * axisY.z,
            axisX.x * axisY.y - axisX.y * axisY.x };

        // hkTransformf column-major: col0=axisX, col1=axisY, col2=axisZ, col3=mid_havok
        outHkXform16[0] = axisX.x;  outHkXform16[1] = axisX.y;  outHkXform16[2] = axisX.z;  outHkXform16[3] = 0.0f;
        outHkXform16[4] = axisY.x;  outHkXform16[5] = axisY.y;  outHkXform16[6] = axisY.z;  outHkXform16[7] = 0.0f;
        outHkXform16[8] = axisZ.x;  outHkXform16[9] = axisZ.y;  outHkXform16[10] = axisZ.z; outHkXform16[11] = 0.0f;
        const RE::NiPoint3 midGame{ (startPos.x + endPos.x) * 0.5f, (startPos.y + endPos.y) * 0.5f, (startPos.z + endPos.z) * 0.5f };
        outHkXform16[12] = midGame.x * kGameToHavok;
        outHkXform16[13] = midGame.y * kGameToHavok;
        outHkXform16[14] = midGame.z * kGameToHavok;
        outHkXform16[15] = 1.0f;
    }

    static void TearDownAll()
    {
        // The player character proxy may cache one of these capsules as its refcounted
        // "support" body. Make it drop that pointer BEFORE we free anything, or it is
        // left dangling -> char-proxy crash. See Physics::ClearPlayerProxySupportBody.
        heisenberg::Physics::ClearPlayerProxySupportBody();
        for (auto& inst : g_bodies) {
            if (inst.body) {
                inst.body->Destroy(g_bhkWorld);
                delete inst.body;
                inst.body = nullptr;
            }
            inst.startBone.reset();
            inst.endBone.reset();
        }
        g_built = false;
        g_playerFilterApplied = false;
    }

    static void* CurrentHknpWorld()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) return nullptr;
        auto* bhk = player->parentCell->GetbhkWorld();
        if (!bhk) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
    }

    void Update()
    {
        if (!IsActive()) return;

        // World-change detection: a cell load frees the old hknp world. If the capsule
        // set was built against a stale world, tear it down so it rebuilds in the live
        // one (otherwise SetTransform drives freed bodies → use-after-free).
        void* liveWorld = CurrentHknpWorld();
        if (g_built && g_hknpWorld != liveWorld) {
            spdlog::info("[ROCK::BodyBoneCollider] World changed → rebuild capsule set");
            TearDownAll();
        }

        // PA state change → swap descriptor table + rebuild.
        const bool paNow = Utils::IsPlayerInPowerArmor();
        if (paNow != g_lastPaState) {
            spdlog::info("[ROCK::BodyBoneCollider] PA state changed: {} -> {} — rebuilding capsule set",
                         g_lastPaState ? "PA" : "normal", paNow ? "PA" : "normal");
            g_lastPaState = paNow;
            g_active = paNow ? kPowerArmor : kStandard;
            if (g_built) TearDownAll();
        }

        // Radius-padding change detection: BuildAll() only reads
        // g_config.handColliderRadiusPadding at build time, so an MCM slider change mid-
        // session would otherwise have no visible effect until the next world/PA-triggered
        // rebuild. Force one here instead.
        const float paddingNow = heisenberg::g_config.handColliderRadiusPadding;
        if (paddingNow != g_lastAppliedPadding) {
            if (g_built) {
                spdlog::info("[ROCK::BodyBoneCollider] Radius padding changed {} -> {} — rebuilding capsule set",
                             g_lastAppliedPadding, paddingNow);
                TearDownAll();
            }
            g_lastAppliedPadding = paddingNow;
        }

        if (!g_built) {
            // Body bones (SPINE1, Pelvis, Chest, LArm_*, RArm_*, LLeg_*, RLeg_*) live
            // on FRIK's third-person avatar under getCommonNode() ("COM"). They are
            // NOT under firstPersonSkeleton (FP rig has only hand+weapon nodes) and
            // NOT under playerworldnode (VR UI/wand root).
            auto* root = f4cf::f4vr::getCommonNode();
            if (!root) return;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) return;
            auto* bhk = player->parentCell->GetbhkWorld();
            if (!bhk) return;
            void* hknp = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(bhk) + 0x60);
            if (!hknp) return;
            BuildAll(root, hknp, bhk);
        }

        // CRITICAL (crash fix): disable collision between each capsule and the PLAYER body.
        // The 0x000B group bits do NOT exclude us from the player character proxy (the proxy
        // isn't in group 0x000B, so the engine's pair filter falls through to the layer matrix
        // and layer 43 collides). These capsules sit on the torso/legs/feet — exactly what the
        // proxy's CheckSupport picks as "ground" — and when torn down/rebuilt the proxy's cached
        // support-body pointer dangles → access-violation crash. DisableCollisionBetween is the
        // real per-pair filter the proxy honors. Applied once the player body resolves; reset on
        // teardown so it re-applies after a rebuild.
        if (g_built && !g_playerFilterApplied) {
            const std::uint32_t playerBodyId = heisenberg::Physics::GetPlayerBodyId();
            if (g_hknpWorld && playerBodyId != 0x7FFFFFFFu && playerBodyId != 0) {
                int n = 0;
                for (auto& inst : g_bodies) {
                    if (inst.body && heisenberg::Physics::DisableCollisionBetween(g_hknpWorld, inst.body->GetBodyId(), playerBodyId)) {
                        ++n;
                    }
                }
                g_playerFilterApplied = true;
                spdlog::info("[ROCK::BodyBoneCollider] Player-pair filter applied to {} capsules vs player 0x{:08X}", n, playerBodyId);
            }
        }

        static alignas(16) float s_hk[16];
        for (auto& inst : g_bodies) {
            if (!inst.body || !inst.startBone || !inst.endBone) continue;
            WriteCapsuleXform(s_hk, inst.startBone->world.translate, inst.endBone->world.translate);
            inst.body->SetTransform(s_hk);
        }
    }

    // Geometry-only build of the 23 body capsules from the live FRIK skeleton (getCommonNode),
    // independent of the physics bodies. Same descriptors + radii. For the mode-3 soft-contact
    // runtime (hand-vs-body). PA state derived from the player so it works standalone.
    int BuildBodyCapsules(std::array<heisenberg::rock_core::soft_contact_math::Capsule, kCapsuleCount>& out)
    {
        for (auto& c : out) {
            c = {};
        }
        auto* root = f4cf::f4vr::getCommonNode();
        if (!root) {
            return 0;
        }
        const Descriptor* desc = Utils::IsPlayerInPowerArmor() ? kPowerArmor : kStandard;
        int n = 0;
        for (int i = 0; i < kBodyCount; ++i) {
            auto* s = f4vr::findAVObject(root, desc[i].startBone);
            auto* e = f4vr::findAVObject(root, desc[i].endBone);
            if (!s || !e) {
                continue;
            }
            out[i].start = s->world.translate;
            out[i].end = e->world.translate;
            out[i].radius = desc[i].radius + heisenberg::g_config.handColliderRadiusPadding;
            out[i].id = static_cast<std::uint32_t>(300 + i);
            out[i].valid = true;
            ++n;
        }
        return n;
    }
}
