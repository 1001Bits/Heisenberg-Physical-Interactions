// ROCK contact pipeline — Heisenberg port (task #108).
//
// This TU currently anchors the ported contact DATA/POLICY headers so they are
// compiled and validated against Heisenberg's toolchain. The contact-pipeline
// RUNTIME (body→identity resolution, semantic routing, soft-contact runtime,
// push-assist) is being wired incrementally on top of these:
//   - ContactActivityTracker        : per-hand contact timing/activity state (header-only)
//   - ContactSignalSubscriptionPolicy: which Havok contact signals to subscribe (header-only)
//   - NativeContactEvidence         : raw contact evidence buffer from Havok callbacks (header-only)
//   - ContactTargetIdentity (.h)    : data model for "what did we touch" (resolution .cpp pending)
//
// Ported verbatim where self-contained; namespaces kept as `heisenberg::rock_core::*` (header-only
// inline → no clash with an external ROCK.dll).

#include "rock_integration/contact/ContactActivityTracker.h"
#include "rock_integration/contact/ContactSignalSubscriptionPolicy.h"
#include "rock_integration/contact/NativeContactEvidence.h"
#include "rock_integration/contact/ContactTargetIdentity.h"

namespace heisenberg::rock_contact
{
    // Compile-anchor: forces the ported headers to instantiate. Runtime wiring follows.
    bool ContactPipelineHeadersCompiled()
    {
        return heisenberg::rock_core::contact_evidence::kInvalidBodyId != 0u
            || heisenberg::rock_core::contact_target_identity::kUnknownLayer != 0u;
    }
}
