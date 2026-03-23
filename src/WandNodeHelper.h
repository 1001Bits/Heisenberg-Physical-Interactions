#pragma once

#include "VRInput.h"
#include "f4vr/PlayerNodes.h"

namespace heisenberg
{
    // Get the wand node for a PHYSICAL hand (isLeft = physical left controller).
    // The game swaps primaryWandNode/SecondaryWandNode in left-handed mode:
    //   Normal:  primaryWandNode = RIGHT physical,  SecondaryWandNode = LEFT physical
    //   LH mode: primaryWandNode = LEFT physical,   SecondaryWandNode = RIGHT physical
    // This helper accounts for the swap so callers just pass isLeft (physical).
    inline RE::NiNode* GetWandNode(f4cf::f4vr::PlayerNodes* nodes, bool isLeft)
    {
        bool isLH = VRInput::GetSingleton().IsLeftHandedMode();
        if (isLH) {
            return isLeft ? nodes->primaryWandNode : nodes->SecondaryWandNode;
        }
        return isLeft ? nodes->SecondaryWandNode : nodes->primaryWandNode;
    }
}
