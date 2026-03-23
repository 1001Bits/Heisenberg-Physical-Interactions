#pragma once

#include <mutex>

namespace heisenberg
{
    /**
     * Represents a selected/highlighted object that can be grabbed.
     * 
     * THREAD SAFETY: Uses RE::ObjectRefHandle for safe reference lookup.
     * The handle system is thread-safe and validates object existence.
     * 
     * LIFETIME SAFETY: Uses RE::NiPointer<> for scene graph nodes.
     * NiPointer is reference-counted and prevents dangling pointers.
     */
    struct Selection
    {
        // Handle-based reference - thread-safe and validates object existence
        RE::ObjectRefHandle refrHandle;
        
        // Smart pointer for scene graph node - reference counted
        RE::NiPointer<RE::NiAVObject> node;
        
        RE::NiPoint3 hitPoint;
        RE::NiPoint3 hitNormal;
        float distance = 0.0f;
        bool isClose = false;

        void Clear()
        {
            refrHandle.reset();
            node.reset();
            hitPoint = RE::NiPoint3();
            hitNormal = RE::NiPoint3();
            distance = 0.0f;
            isClose = false;
        }

        /**
         * Get the TESObjectREFR if it's still valid.
         * Returns nullptr if the object has been deleted.
         * Thread-safe via handle system.
         */
        RE::TESObjectREFR* GetRefr() const
        {
            if (!refrHandle) return nullptr;
            RE::NiPointer<RE::TESObjectREFR> refPtr = refrHandle.get();
            return refPtr.get();
        }

        /**
         * Set the reference from a raw pointer.
         * Creates a handle for safe lookup later.
         */
        void SetRefr(RE::TESObjectREFR* refr)
        {
            if (refr) {
                refrHandle = RE::ObjectRefHandle(refr);
            } else {
                refrHandle.reset();
            }
        }

        /**
         * Check if this selection is valid (reference still exists).
         */
        bool IsValid() const { return GetRefr() != nullptr; }
        bool IsClose() const { return isClose; }
    };
}
