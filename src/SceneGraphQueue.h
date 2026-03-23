#pragma once

#include <queue>
#include <mutex>
#include <functional>
#include "RE/Fallout.h"

namespace heisenberg
{
    /**
     * SceneGraphQueue - Thread-safe queue for scene graph modifications
     * 
     * Problem: Calling DetachChild/AttachChild while the render thread is
     * traversing the scene graph can corrupt the NiNode children array,
     * causing crashes in unrelated nodes (like flashlight activation crash).
     * 
     * Solution: Queue all scene graph modifications and process them at
     * a known-safe point at the start of each frame, before the render
     * thread begins its traversal.
     */
    class SceneGraphQueue
    {
    public:
        enum class OperationType
        {
            Detach,     // Detach child from parent
            Attach,     // Attach child to parent
            Reparent    // Detach from current parent, attach to new parent
        };

        struct PendingOperation
        {
            OperationType type;
            RE::NiAVObject* child;       // Node to move (as NiAVObject*)
            RE::NiNode* newParent;       // For Attach/Reparent: new parent
            bool updateTransform;        // Should we call NiAVObject_Update after?
            
            // For Reparent: optional world transform to preserve
            bool preserveWorldTransform;
            RE::NiPoint3 worldPos;
            RE::NiMatrix3 worldRot;
            float worldScale;
        };

        static SceneGraphQueue& GetSingleton()
        {
            static SceneGraphQueue instance;
            return instance;
        }

        // Queue a detach operation
        void QueueDetach(RE::NiAVObject* child)
        {
            if (!child) return;
            
            std::lock_guard<std::mutex> lock(_mutex);
            PendingOperation op;
            op.type = OperationType::Detach;
            op.child = child;
            op.newParent = nullptr;
            op.updateTransform = false;
            op.preserveWorldTransform = false;
            _pendingOps.push(op);
            
            spdlog::debug("[SceneGraphQueue] Queued DETACH for node '{}'", 
                         child->name.c_str());
        }

        // Queue an attach operation
        void QueueAttach(RE::NiAVObject* child, RE::NiNode* newParent, bool update = true)
        {
            if (!child || !newParent) return;
            
            std::lock_guard<std::mutex> lock(_mutex);
            PendingOperation op;
            op.type = OperationType::Attach;
            op.child = child;
            op.newParent = newParent;
            op.updateTransform = update;
            op.preserveWorldTransform = false;
            _pendingOps.push(op);
            
            spdlog::debug("[SceneGraphQueue] Queued ATTACH for node '{}' to parent '{}'", 
                         child->name.c_str(), newParent->name.c_str());
        }

        // Queue a reparent operation (detach + attach with optional world transform preservation)
        void QueueReparent(RE::NiAVObject* child, RE::NiNode* newParent, 
                          bool preserveWorld = false,
                          const RE::NiPoint3& worldPos = RE::NiPoint3(),
                          const RE::NiMatrix3& worldRot = RE::NiMatrix3(),
                          float worldScale = 1.0f)
        {
            if (!child || !newParent) return;
            
            std::lock_guard<std::mutex> lock(_mutex);
            PendingOperation op;
            op.type = OperationType::Reparent;
            op.child = child;
            op.newParent = newParent;
            op.updateTransform = true;
            op.preserveWorldTransform = preserveWorld;
            op.worldPos = worldPos;
            op.worldRot = worldRot;
            op.worldScale = worldScale;
            _pendingOps.push(op);
            
            spdlog::debug("[SceneGraphQueue] Queued REPARENT for node '{}' to parent '{}'", 
                         child->name.c_str(), newParent->name.c_str());
        }

        // Process all pending operations - call this at start of frame
        // Returns number of operations processed
        int ProcessQueue();

        // Check if it's safe to modify scene graph
        // Call this before any direct scene graph modifications
        bool IsSafeToModify() const { return _isSafeToModify; }

        // Set safe state - called at known-safe points in game loop
        void SetSafeToModify(bool safe) { _isSafeToModify = safe; }

        // Clear all pending operations (e.g., on load game)
        void ClearQueue()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            while (!_pendingOps.empty()) {
                _pendingOps.pop();
            }
            spdlog::info("[SceneGraphQueue] Cleared all pending operations");
        }

        // Get pending operation count
        size_t GetPendingCount() const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return _pendingOps.size();
        }

    private:
        SceneGraphQueue() = default;
        ~SceneGraphQueue() = default;
        SceneGraphQueue(const SceneGraphQueue&) = delete;
        SceneGraphQueue& operator=(const SceneGraphQueue&) = delete;

        mutable std::mutex _mutex;
        std::queue<PendingOperation> _pendingOps;
        std::atomic<bool> _isSafeToModify{ true };
    };

    // Helper to ensure we're in a safe state for scene graph mods
    class SceneGraphSafeGuard
    {
    public:
        SceneGraphSafeGuard()
        {
            SceneGraphQueue::GetSingleton().SetSafeToModify(true);
        }
        ~SceneGraphSafeGuard()
        {
            SceneGraphQueue::GetSingleton().SetSafeToModify(false);
        }
    };

} // namespace heisenberg
