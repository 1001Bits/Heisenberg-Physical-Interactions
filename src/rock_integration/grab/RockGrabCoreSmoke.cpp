// Smoke translation unit (P1): forces every ported pure grab-core header to compile against
// Heisenberg's RE types + the minimal RockGrabConfig. Not wired into runtime — its only job is
// to prove the ported headers are self-consistent and dependency-complete. Safe to delete once
// the real RockGrabCoreManager (P3) includes these headers.

#include "rock_integration/grab/RockGrabConfig.h"
#include "rock_integration/grab/DebugMath.h"
#include "rock_integration/grab/PhysicsScale.h"
#include "rock_integration/grab/GrabInertiaPolicy.h"
#include "rock_integration/grab/GrabMotionController.h"
#include "rock_integration/grab/HandColliderTypes.h"
#include "rock_integration/grab/HandFrame.h"
#include "rock_integration/grab/GrabConstraintMath.h"
#include "rock_integration/grab/GrabThreePhase.h"
#include "rock_integration/grab/GrabContact.h"
#include "rock_integration/grab/GrabHeldObject.h"
#include "rock_integration/grab/GrabCore.h"

namespace
{
    [[maybe_unused]] volatile int g_rockGrabCoreSmokeAnchor = 0;
}
