#pragma once

#include <cstddef>
#include <cstdint>

// Port of ROCK's BethesdaPhysicsBody pipeline into Heisenberg. Replaces the
// _aligned_malloc-based body creation (which crashes in TBB scalable_aligned_free during
// the engine's destruction phase) with the engine's own allocator. Mirrors ROCK's
// behavior closely so the resulting body is treated identically to any game body.
//
// Layout (matches ROCK so we could later inter-operate):
//   +0x00  void*   _collisionObject  (bhkNPCollisionObject*)
//   +0x08  void*   _physicsSystem    (bhkPhysicsSystem*)
//   +0x10  void*   _systemData       (hknpPhysicsSystemData*)
//   +0x18  uint32  _bodyId
//   +0x1C  bool    _created
//
// Usage:
//   BethesdaPhysicsBody body;
//   body.Create(bhkWorld, hknpWorld, shape, filterInfo, MotionType::Keyframed, "hand");
//   body.SetTransform(hkXform);
//   body.SetVelocity(linVel, angVel);
//   body.Destroy(bhkWorld);  // safe to call at any time

namespace heisenberg::bethesda_physics_body
{
    enum class MotionType : int
    {
        Static    = 0,
        Dynamic   = 1,
        Keyframed = 2,
    };

    class BethesdaPhysicsBody
    {
    public:
        BethesdaPhysicsBody() = default;
        ~BethesdaPhysicsBody() = default;

        BethesdaPhysicsBody(const BethesdaPhysicsBody&) = delete;
        BethesdaPhysicsBody& operator=(const BethesdaPhysicsBody&) = delete;

        // Build a body: alloc systemData, populate cinfo (shape, filter, material), alloc the
        // bhkPhysicsSystem + bhkNPCollisionObject wrappers via the engine allocator, add to
        // world, fetch body id, promote motion type, enable contact-modifier flags, activate.
        //
        // Returns true on success (body is in the world). On failure, all partial allocs are
        // released via Destroy semantics so we don't leak.
        bool Create(void* bhkWorld, void* hknpWorld, void* hknpShape,
                    std::uint32_t collisionFilterInfo, MotionType motionType,
                    const char* debugName = "Heisenberg_Body");

        // Detach from world + release ref-counted wrappers. Idempotent.
        void Destroy(void* bhkWorld);

        // Drive functions — wrappers over Heisenberg's existing SetTransform / SetVelocity.
        bool SetTransform(const void* hkTransformf64);
        bool SetVelocity(const void* linVel4, const void* angVel4);

        bool IsValid() const { return _created && _collisionObject != nullptr; }
        std::uint32_t GetBodyId() const { return _bodyId; }
        void* GetCollisionObject() const { return _collisionObject; }
        void* GetPhysicsSystem() const { return _physicsSystem; }

    private:
        void* _collisionObject = nullptr;  // +0x00 (bhkNPCollisionObject*)
        void* _physicsSystem   = nullptr;  // +0x08 (bhkPhysicsSystem*)
        void* _systemData      = nullptr;  // +0x10 (hknpPhysicsSystemData*)
        std::uint32_t _bodyId  = 0x7FFFFFFF;
        bool _created          = false;
    };
}
