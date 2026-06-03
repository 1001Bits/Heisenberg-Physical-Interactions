#include "HandBoneColliderSet.h"
#include "../BethesdaPhysicsBody.h"
#include "../Config.h"
#include "../GrabConstraint.h"
#include "../Physics.h"
#include "../Utils.h"

#include "F4SE/F4SE.h"
#include "f4vr/PlayerNodes.h"
#include "f4vr/F4VRUtils.h"
#include <spdlog/spdlog.h>

namespace heisenberg::rock_hand_collider
{
    using BPB = heisenberg::bethesda_physics_body::BethesdaPhysicsBody;
    using MotionType = heisenberg::bethesda_physics_body::MotionType;

    // ──────────────────────────────────────────────────────────────────────────
    // Role definitions: palm + 5 fingers × 3 segments = 16 bodies per hand.
    // ROCK-faithful: half-extents derived per-frame from BONE-PAIR DISTANCE rather
    // than hardcoded. Roles describe (boneA, boneB) — body length spans
    // |boneB - boneA|; radius comes from roleRadius() with PA scale.
    // ──────────────────────────────────────────────────────────────────────────
    constexpr int kRolesPerHand = 16;
    constexpr float kConvexRadius = 0.10f;       // matches ROCK roleConvexRadius
    constexpr std::uint32_t kHandLayer = 43;
    constexpr std::uint32_t kHandFilterInfo = (0x000Bu << 16) | (kHandLayer & 0x7Fu);
    constexpr float kPaScale = 1.55f;            // matches ROCK powerArmor scale

    enum class Segment : std::uint8_t { Palm = 0, Base, Middle, Tip };

    struct RoleSpec
    {
        const char* boneA;   // start bone (printf "%s..." — %s = L|R)
        const char* boneB;   // end bone (defines length)
        Segment seg;
    };

    // ROCK roleRadius(): palm 1.35, base 0.55, middle 0.46, tip 0.38 (game units, non-PA).
    static float roleRadius(Segment s, bool pa)
    {
        const float k = pa ? kPaScale : 1.0f;
        switch (s) {
            case Segment::Palm:   return 1.35f * k;
            case Segment::Base:   return 0.55f * k;
            case Segment::Middle: return 0.46f * k;
            case Segment::Tip:    return 0.38f * k;
        }
        return 0.5f;
    }

    // Roles: palm (LArm_Hand→LArm_Finger31 — wrist-to-middle-base for length anchor),
    // then 5 fingers × 3 segments. Each finger segment uses its own bone-to-next-bone
    // distance for length (last segment uses the bone's length to its tip child if any,
    // else a short default).
    static constexpr RoleSpec kRoles[kRolesPerHand] = {
        // Palm anchor — wrist to base of middle finger (good palm-length proxy)
        { "%sArm_Hand",     "%sArm_Finger31", Segment::Palm },
        // Thumb (Finger1)
        { "%sArm_Finger11", "%sArm_Finger12", Segment::Base   },
        { "%sArm_Finger12", "%sArm_Finger13", Segment::Middle },
        { "%sArm_Finger13", "",               Segment::Tip    },
        // Index (Finger2)
        { "%sArm_Finger21", "%sArm_Finger22", Segment::Base   },
        { "%sArm_Finger22", "%sArm_Finger23", Segment::Middle },
        { "%sArm_Finger23", "",               Segment::Tip    },
        // Middle (Finger3)
        { "%sArm_Finger31", "%sArm_Finger32", Segment::Base   },
        { "%sArm_Finger32", "%sArm_Finger33", Segment::Middle },
        { "%sArm_Finger33", "",               Segment::Tip    },
        // Ring (Finger4)
        { "%sArm_Finger41", "%sArm_Finger42", Segment::Base   },
        { "%sArm_Finger42", "%sArm_Finger43", Segment::Middle },
        { "%sArm_Finger43", "",               Segment::Tip    },
        // Pinky (Finger5)
        { "%sArm_Finger51", "%sArm_Finger52", Segment::Base   },
        { "%sArm_Finger52", "%sArm_Finger53", Segment::Middle },
        { "%sArm_Finger53", "",               Segment::Tip    },
    };
    static_assert(sizeof(kRoles) / sizeof(kRoles[0]) == kRolesPerHand, "role table mismatch");
    // Default fingertip length (game units) when no child bone — ROCK uses ~0.7×radius.
    constexpr float kTipLength = 1.5f;

    // ──────────────────────────────────────────────────────────────────────────
    // Per-hand state
    // ──────────────────────────────────────────────────────────────────────────
    struct HandSet
    {
        BPB* bodies[kRolesPerHand] = { nullptr };
        RE::NiPointer<RE::NiAVObject> boneA[kRolesPerHand];   // anchor / start
        RE::NiPointer<RE::NiAVObject> boneB[kRolesPerHand];   // end (null for Tip)
        float lengths[kRolesPerHand] = { 0.0f };              // captured at build, game units
        void* hknpWorld = nullptr;
        void* bhkWorld = nullptr;
        bool built = false;
        bool builtForPa = false;
        bool playerFilterApplied = false;  // DisableCollisionBetween(body, playerBody) done?
    };

    static HandSet g_left;
    static HandSet g_right;
    static bool s_initialized = false;
    static bool s_loggedDisabled = false;

    // ──────────────────────────────────────────────────────────────────────────
    bool IsActive()
    {
        return s_initialized && heisenberg::g_config.rockHandBoneColliderSet
            && heisenberg::g_config.enableHandCollision
            && heisenberg::g_config.usePhysicsHandBodies
            && heisenberg::g_config.useBethesdaPhysicsBody;
    }

    void Init()
    {
        if (!heisenberg::g_config.rockHandBoneColliderSet) {
            if (!s_loggedDisabled) {
                spdlog::info("[ROCK::HandBoneCollider] Disabled by config (bRockHandBoneColliderSet=false)");
                s_loggedDisabled = true;
            }
            return;
        }
        spdlog::info("[ROCK::HandBoneCollider] Init — will build 16 bodies per hand on first Update with bones+worlds available");
        s_initialized = true;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Shape creation — same aligned-malloc trick as the cup-shape build,
    // proven to dodge MSVC's stack-alignment issue on `hknpConvexShapeBuildConfig`.
    // ──────────────────────────────────────────────────────────────────────────
    static void* buildHull(float hxGame, float hyGame, float hzGame)
    {
        constexpr float kGameToHavok = 1.0f / 70.0f;
        void* heMem = _aligned_malloc(sizeof(RE::NiPoint4), 16);
        if (!heMem) return nullptr;
        auto* hePtr = new (heMem) RE::NiPoint4(
            hxGame * kGameToHavok, hyGame * kGameToHavok, hzGame * kGameToHavok, 0.0f);
        void* bcMem = _aligned_malloc(sizeof(heisenberg::ConstraintFunctions::hknpConvexShapeBuildConfig), 16);
        if (!bcMem) { _aligned_free(heMem); return nullptr; }
        auto* bcPtr = reinterpret_cast<heisenberg::ConstraintFunctions::hknpConvexShapeBuildConfig*>(bcMem);
        heisenberg::ConstraintFunctions::BuildConfigCtor(bcPtr);
        // kConvexRadius is in GAME units — convert to havok like ROCK does
        // (BodyBoneColliderSet.cpp:501 passes convexRadius * gameToHavokScale). Passing it
        // raw made the rounding skin 0.10 HAVOK = 10cm (70× too big), so colliders pushed
        // objects ~10cm before the hand mesh touched.
        void* shp = heisenberg::ConstraintFunctions::CreateConvexShapeFromHalfExtents(*hePtr, kConvexRadius * kGameToHavok, bcPtr);
        _aligned_free(bcMem);
        _aligned_free(heMem);
        return shp;
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Build hand bodies. Each body's half-extents derived from BONE-PAIR distance
    // (length) + roleRadius() (radius). Tip segments use kTipLength default since
    // they have no child bone to measure to.
    // ──────────────────────────────────────────────────────────────────────────
    static void DestroyHand(HandSet& hs);  // fwd decl — BuildHand tears down partial sets

    static bool BuildHand(HandSet& hs, bool isLeft, RE::NiAVObject* skeletonRoot,
                          void* hknpWorld, void* bhkWorld, bool pa)
    {
        char ba[64], bb[64];
        const char* side = isLeft ? "L" : "R";
        int createdCount = 0;

        for (int i = 0; i < kRolesPerHand; ++i) {
            // 1. Resolve boneA (always required). f4vr::findAVObject is the same
            // recursive search the proven cup-finger code (HandCollision::UpdateFingerSegments)
            // uses against firstPersonSkeleton — Utils::FindNode was failing against
            // playerworldnode which is a VR-wand-attachment root, not a skeleton.
            std::snprintf(ba, sizeof(ba), kRoles[i].boneA, side);
            auto* nodeA = f4vr::findAVObject(skeletonRoot, ba);
            if (!nodeA) continue;
            hs.boneA[i].reset(nodeA);

            // 2. Resolve boneB if specified; compute length.
            float lenGame;
            if (kRoles[i].boneB && kRoles[i].boneB[0]) {
                std::snprintf(bb, sizeof(bb), kRoles[i].boneB, side);
                auto* nodeB = f4vr::findAVObject(skeletonRoot, bb);
                if (!nodeB) {
                    hs.boneA[i].reset();
                    continue;
                }
                hs.boneB[i].reset(nodeB);
                const auto d = nodeB->world.translate - nodeA->world.translate;
                lenGame = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
                if (lenGame < 0.1f) lenGame = kTipLength;  // degenerate fallback
            } else {
                // Tip — no child; default length.
                lenGame = kTipLength;
            }
            hs.lengths[i] = lenGame;

            // 3. Derive half-extents: length/2 along X, radius on Y/Z.
            const float r = roleRadius(kRoles[i].seg, pa);
            const float halfX = lenGame * 0.5f;
            const float halfY = r;
            // Palm is flatter than wide (matches ROCK kRoundedBoxThickness × radius for PalmAnchor).
            const float halfZ = (kRoles[i].seg == Segment::Palm) ? r * 0.45f : r;

            void* shape = buildHull(halfX, halfY, halfZ);
            if (!shape) {
                spdlog::error("[ROCK::HandBoneCollider] {} role {} shape build failed", side, ba);
                continue;
            }

            auto* body = new BPB();
            if (!body->Create(bhkWorld, hknpWorld, shape, kHandLayer, MotionType::Keyframed, ba)) {
                spdlog::error("[ROCK::HandBoneCollider] {} role {} BPB::Create failed", side, ba);
                delete body;
                continue;
            }
            heisenberg::Physics::TryWriteBodyFilterInfo(hknpWorld, body->GetBodyId(), kHandFilterInfo);
            hs.bodies[i] = body;
            ++createdCount;
        }
        hs.hknpWorld = hknpWorld;
        hs.bhkWorld = bhkWorld;
        hs.builtForPa = pa;

        // Completeness guard: only LATCH built when ALL roles resolved. A half-loaded
        // skeleton (FRIK not fully attached yet) used to latch a partial set (built once
        // createdCount>=1) that never filled in the missing fingers. Instead, tear the
        // partial set down and retry next frame — until complete, or a cap so a skeleton
        // genuinely missing a bone doesn't rebuild forever.
        static int s_attempts[2] = {0, 0};
        const int side01 = isLeft ? 0 : 1;
        constexpr int kMaxBuildAttempts = 180;  // ~3 s at 60 fps before accepting a partial set

        if (createdCount == kRolesPerHand || s_attempts[side01] >= kMaxBuildAttempts) {
            hs.built = (createdCount >= 1);
            if (hs.built) {
                spdlog::info("[ROCK::HandBoneCollider] {} hand built {}/{} bodies (PA={}{})",
                             isLeft ? "LEFT" : "RIGHT", createdCount, kRolesPerHand, pa ? "yes" : "no",
                             createdCount < kRolesPerHand ? ", PARTIAL after retry cap" : "");
            }
            s_attempts[side01] = 0;
            return hs.built;
        }

        // Incomplete — drop the partial bodies (avoid leaking them on the next rebuild) and
        // retry. Throttle the log to once per ~1 s.
        if ((s_attempts[side01]++ % 60) == 0) {
            spdlog::warn("[ROCK::HandBoneCollider] {} hand only {}/{} bones resolved (root={}, attempt {}); retrying",
                         isLeft ? "LEFT" : "RIGHT", createdCount, kRolesPerHand,
                         skeletonRoot && skeletonRoot->name.c_str() ? skeletonRoot->name.c_str() : "<noname>",
                         s_attempts[side01]);
        }
        DestroyHand(hs);
        return false;
    }

    static void DestroyHand(HandSet& hs)
    {
        // Drop any proxy-cached support pointer to these bodies before freeing them
        // (refcounted dangling pointer = char-proxy crash). See Physics helper.
        heisenberg::Physics::ClearPlayerProxySupportBody();
        for (int i = 0; i < kRolesPerHand; ++i) {
            if (hs.bodies[i]) {
                hs.bodies[i]->Destroy(hs.bhkWorld);
                delete hs.bodies[i];
                hs.bodies[i] = nullptr;
            }
            hs.boneA[i].reset();
            hs.boneB[i].reset();
            hs.lengths[i] = 0.0f;
        }
        hs.hknpWorld = nullptr;
        hs.bhkWorld = nullptr;
        hs.built = false;
        hs.playerFilterApplied = false;
    }

    void Shutdown()
    {
        if (!s_initialized) return;
        DestroyHand(g_left);
        DestroyHand(g_right);
        spdlog::info("[ROCK::HandBoneCollider] Shutdown (bodies torn down; module stays live)");
        // Do NOT clear s_initialized: this is called from the load handlers to drop bodies
        // before the old world is freed, NOT to permanently disable the module. Init() only
        // runs once at startup, so clearing it here left the module dead after the first load
        // (DestroyHand already set built=false, so Update rebuilds in the new world).
    }

    // ──────────────────────────────────────────────────────────────────────────
    // Per-frame drive. Build on first call. Each subsequent call SetTransforms.
    // ──────────────────────────────────────────────────────────────────────────
    // Build a capsule-style hkTransformf: column-major 4x4, X axis along bone direction,
    // position = midpoint between boneA and boneB. Same math as BodyBoneColliderSet.
    static void WriteSegmentXform(float* outHk,
                                  const RE::NiPoint3& a, const RE::NiPoint3& b,
                                  const RE::NiMatrix3& fallbackRot, float fallbackLenGame)
    {
        constexpr float kGameToHavok = 1.0f / 70.0f;
        RE::NiPoint3 d = b - a;
        float lenGame = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        RE::NiPoint3 mid;

        if (lenGame < 0.05f) {
            // Tip / degenerate — use boneA's rotation, project an X-axis length offset.
            outHk[0] = fallbackRot.entry[0][0]; outHk[1] = fallbackRot.entry[1][0]; outHk[2]  = fallbackRot.entry[2][0]; outHk[3]  = 0;
            outHk[4] = fallbackRot.entry[0][1]; outHk[5] = fallbackRot.entry[1][1]; outHk[6]  = fallbackRot.entry[2][1]; outHk[7]  = 0;
            outHk[8] = fallbackRot.entry[0][2]; outHk[9] = fallbackRot.entry[1][2]; outHk[10] = fallbackRot.entry[2][2]; outHk[11] = 0;
            // Tip position = boneA + halfTipLen * (its local X axis in world)
            const float halfLen = fallbackLenGame * 0.5f;
            mid.x = a.x + fallbackRot.entry[0][0] * halfLen;
            mid.y = a.y + fallbackRot.entry[1][0] * halfLen;
            mid.z = a.z + fallbackRot.entry[2][0] * halfLen;
        } else {
            const float invLen = 1.0f / lenGame;
            const RE::NiPoint3 ax{ d.x * invLen, d.y * invLen, d.z * invLen };
            // Pick up not parallel to ax
            RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
            if (std::fabs(ax.z) > 0.99f) up = { 0.0f, 1.0f, 0.0f };
            RE::NiPoint3 ay{ up.y*ax.z - up.z*ax.y, up.z*ax.x - up.x*ax.z, up.x*ax.y - up.y*ax.x };
            float yl = std::sqrt(ay.x*ay.x + ay.y*ay.y + ay.z*ay.z);
            if (yl > 1e-6f) { ay = { ay.x/yl, ay.y/yl, ay.z/yl }; }
            const RE::NiPoint3 az{ ax.y*ay.z - ax.z*ay.y, ax.z*ay.x - ax.x*ay.z, ax.x*ay.y - ax.y*ay.x };
            outHk[0] = ax.x; outHk[1] = ax.y; outHk[2]  = ax.z; outHk[3]  = 0;
            outHk[4] = ay.x; outHk[5] = ay.y; outHk[6]  = ay.z; outHk[7]  = 0;
            outHk[8] = az.x; outHk[9] = az.y; outHk[10] = az.z; outHk[11] = 0;
            mid.x = (a.x + b.x) * 0.5f;
            mid.y = (a.y + b.y) * 0.5f;
            mid.z = (a.z + b.z) * 0.5f;
        }
        outHk[12] = mid.x * kGameToHavok;
        outHk[13] = mid.y * kGameToHavok;
        outHk[14] = mid.z * kGameToHavok;
        outHk[15] = 1.0f;
    }

    static void DriveHand(HandSet& hs)
    {
        if (!hs.built) return;
        static alignas(16) float s_hk[16];
        for (int i = 0; i < kRolesPerHand; ++i) {
            if (!hs.bodies[i] || !hs.boneA[i]) continue;
            const auto& wa = hs.boneA[i]->world;
            const RE::NiPoint3 a = wa.translate;
            RE::NiPoint3 b;
            if (hs.boneB[i]) {
                b = hs.boneB[i]->world.translate;
            } else {
                // Tip — project along bone X
                b.x = a.x + wa.rotate.entry[0][0] * hs.lengths[i];
                b.y = a.y + wa.rotate.entry[1][0] * hs.lengths[i];
                b.z = a.z + wa.rotate.entry[2][0] * hs.lengths[i];
            }
            WriteSegmentXform(s_hk, a, b, wa.rotate, hs.lengths[i]);
            hs.bodies[i]->SetTransform(s_hk);
        }
    }

    // Fetch the current cell's hknp world (or nullptr). Used both for the build and for
    // detecting a cell change that invalidated the cached world pointer.
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

        // World-change detection: a cell load destroys the old bhk/hknp world. If a hand
        // was built against a now-stale world, tear it down so it rebuilds in the live one
        // — otherwise SetTransform drives bodies in freed memory (use-after-free / crash).
        void* liveWorld = CurrentHknpWorld();
        if (g_left.built && g_left.hknpWorld != liveWorld) {
            spdlog::info("[ROCK::HandBoneCollider] LEFT world changed → rebuild");
            DestroyHand(g_left);
        }
        if (g_right.built && g_right.hknpWorld != liveWorld) {
            spdlog::info("[ROCK::HandBoneCollider] RIGHT world changed → rebuild");
            DestroyHand(g_right);
        }

        const bool paNow = Utils::IsPlayerInPowerArmor();

        // PA state change → tear down + rebuild with PA-scaled radii.
        if (g_left.built && g_left.builtForPa != paNow) {
            spdlog::info("[ROCK::HandBoneCollider] LEFT PA state change → rebuild");
            DestroyHand(g_left);
        }
        if (g_right.built && g_right.builtForPa != paNow) {
            spdlog::info("[ROCK::HandBoneCollider] RIGHT PA state change → rebuild");
            DestroyHand(g_right);
        }

        if (!g_left.built || !g_right.built) {
            // Bone resolution must search getCommonNode() ("COM"), NOT firstPersonSkeleton.
            // Confirmed in-game: firstPersonSkeleton (root="skeleton.nif") resolves 0
            // finger bones — its LArm_Hand is a weapon-wrapper with no finger children
            // (see project_skeleton_dual_root). BodyBoneColliderSet builds 23/23 against
            // getCommonNode(), which carries the full arm + finger hierarchy.
            auto* root = f4cf::f4vr::getCommonNode();
            if (!root || !liveWorld) return;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->parentCell) return;
            auto* bhk = player->parentCell->GetbhkWorld();
            if (!bhk) return;

            if (!g_left.built)  BuildHand(g_left,  true,  root, liveWorld, bhk, paNow);
            if (!g_right.built) BuildHand(g_right, false, root, liveWorld, bhk, paNow);
        }

        // CRITICAL (crash fix): disable collision between each hand body and the PLAYER body.
        // The 0x000B group bits do NOT exclude us from the player character proxy (see
        // BodyBoneColliderSet for the decompiled reason). Without this, a hand body can be the
        // proxy's cached support/interaction body and dangle on teardown → crash.
        auto applyPlayerFilter = [](HandSet& hs, const char* label) {
            if (!hs.built || hs.playerFilterApplied || !hs.hknpWorld) return;
            const std::uint32_t playerBodyId = heisenberg::Physics::GetPlayerBodyId();
            if (playerBodyId == 0x7FFFFFFFu || playerBodyId == 0) return;
            int n = 0;
            for (int i = 0; i < kRolesPerHand; ++i) {
                if (hs.bodies[i] && heisenberg::Physics::DisableCollisionBetween(hs.hknpWorld, hs.bodies[i]->GetBodyId(), playerBodyId)) ++n;
            }
            hs.playerFilterApplied = true;
            spdlog::info("[ROCK::HandBoneCollider] {} player-pair filter applied to {} bodies vs player 0x{:08X}", label, n, playerBodyId);
        };
        applyPlayerFilter(g_left, "LEFT");
        applyPlayerFilter(g_right, "RIGHT");

        DriveHand(g_left);
        DriveHand(g_right);
    }
}
