// ROCK grab math/policy — Heisenberg port (tasks #111/#112 foundation).
//
// Self-contained, header-only math/policy ported verbatim (namespaces kept heisenberg::rock_core::*):
//   - TransformMath        : templated transform/vector/quat math (shared infra, #111)
//   - GrabConstraintMath   : grab constraint target/error math (#111)
//   - GrabNodeInfoMath     : per-weapon grab-node anchor geometry (#112)
//   - GrabNodeNamePolicy   : named grab-node matching/blacklist (#112)
//
// The grab-engine RUNTIME (GrabConstraint motors, GrabMotionController, GrabThreePhase,
// GrabContact, GrabFinger driver) is built on top of these and needs adaptation to
// Heisenberg's Physics/constraint helpers — that is the next layer.

#include "rock_integration/TransformMath.h"
#include "rock_integration/grab/GrabConstraintMath.h"
#include "rock_integration/grab/GrabNodeInfoMath.h"
#include "rock_integration/grab/GrabNodeNamePolicy.h"

namespace heisenberg::rock_grab
{
    // Compile-anchor: forces the ported headers to instantiate. Runtime wiring follows.
    bool GrabMathHeadersCompiled()
    {
        return !heisenberg::rock_core::grab_node_name_policy::defaultGrabNodeName(true).empty();
    }
}
