#include "SceneGraphQueue.h"
#include "F4VROffsets.h"

namespace heisenberg
{
    int SceneGraphQueue::ProcessQueue()
    {
        // Take ownership of the queue
        std::queue<PendingOperation> opsToProcess;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::swap(opsToProcess, _pendingOps);
        }

        if (opsToProcess.empty()) {
            return 0;
        }

        spdlog::debug("[SceneGraphQueue] Processing {} pending operations", opsToProcess.size());

        int processed = 0;
        while (!opsToProcess.empty())
        {
            PendingOperation op = opsToProcess.front();
            opsToProcess.pop();

            // Validate node is still valid
            if (!op.child) {
                spdlog::warn("[SceneGraphQueue] Skipping operation - child is null");
                continue;
            }

            try
            {
                switch (op.type)
                {
                case OperationType::Detach:
                    if (op.child->parent) {
                        spdlog::debug("[SceneGraphQueue] Executing DETACH for '{}'", op.child->name.c_str());
                        op.child->parent->DetachChild(op.child);
                    }
                    break;

                case OperationType::Attach:
                    if (op.newParent) {
                        spdlog::debug("[SceneGraphQueue] Executing ATTACH for '{}' to '{}'", 
                                     op.child->name.c_str(), op.newParent->name.c_str());
                        op.newParent->AttachChild(op.child, true);
                        
                        if (op.updateTransform) {
                            RE::NiUpdateData updateData;
                            updateData.flags = 0;
                            updateData.time = 0.0f;
                            NiAVObject_Update(op.child, &updateData);
                        }
                    }
                    break;

                case OperationType::Reparent:
                    if (op.newParent) {
                        spdlog::debug("[SceneGraphQueue] Executing REPARENT for '{}' to '{}'", 
                                     op.child->name.c_str(), op.newParent->name.c_str());
                        
                        // Detach from current parent
                        if (op.child->parent) {
                            op.child->parent->DetachChild(op.child);
                        }
                        
                        // If preserving world transform, calculate new local transform
                        if (op.preserveWorldTransform) {
                            float newParentScale = op.newParent->world.scale;
                            float invScale = (newParentScale > 0.001f) ? (1.0f / newParentScale) : 1.0f;
                            RE::NiMatrix3 parentRotInv = op.newParent->world.rotate.Transpose();
                            
                            op.child->local.translate = parentRotInv * 
                                ((op.worldPos - op.newParent->world.translate) * invScale);
                            op.child->local.rotate = op.worldRot * parentRotInv;
                            op.child->local.scale = op.worldScale * invScale;
                            
                            // Also set world to ensure consistency
                            op.child->world.translate = op.worldPos;
                            op.child->world.rotate = op.worldRot;
                            op.child->world.scale = op.worldScale;
                        }
                        
                        // Attach to new parent
                        op.newParent->AttachChild(op.child, true);
                        
                        if (op.updateTransform) {
                            RE::NiUpdateData updateData;
                            updateData.flags = 0;
                            updateData.time = 0.0f;
                            NiAVObject_Update(op.child, &updateData);
                        }
                    }
                    break;
                }
                
                processed++;
            }
            catch (const std::exception& e)
            {
                spdlog::error("[SceneGraphQueue] Exception processing operation: {}", e.what());
            }
            catch (...)
            {
                spdlog::error("[SceneGraphQueue] Unknown exception processing operation");
            }
        }

        if (processed > 0) {
            spdlog::debug("[SceneGraphQueue] Processed {} operations successfully", processed);
        }

        return processed;
    }

} // namespace heisenberg
