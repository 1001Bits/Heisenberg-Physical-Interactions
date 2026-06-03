// ROCK object-pipeline + weapon-interaction LOGIC foundation — Heisenberg port (#108/#109/#110).
//
// Self-contained policy/data headers ported verbatim (namespaces kept rock::*), validated here.
// These are the decision/classification layer that the functional runtimes build on:
//   - object/GrabTargetKind        : classify a grab target (clutter / actor / weapon / world …)
//   - object/PhysicsBodyClassifier : classify an hknp body by layer/role → grab-target kind
//   - object/GeometryBodyResolver  : resolve a NIF geometry node to a body descriptor
//   - weapon/WeaponTypes           : weapon interaction contact/decision data model
//   - weapon/WeaponSupport         : support-grip policy (canUseContactForSupportGrip, pose)
//   - weapon/WeaponInteraction     : routeWeaponInteraction() — SupportGrip vs PassiveTouch
//
// The functional cores that CONSUME these — ObjectPhysicsBodySet (multi-body trees, #109) and
// the contact runtime / SoftContactRuntime (#108) that produces WeaponInteractionContacts — are
// large engine-integrated adaptations still pending.

#include "rock_integration/object/GrabTargetKind.h"
#include "rock_integration/object/PhysicsBodyClassifier.h"
#include "rock_integration/object/GeometryBodyResolver.h"
#include "rock_integration/object/ObjectPhysicsBodySet.h"  // pure body-set selection logic (scanner .cpp pending)
#include "rock_integration/weapon/WeaponTypes.h"
#include "rock_integration/weapon/WeaponSupport.h"
#include "rock_integration/weapon/WeaponInteraction.h"

namespace heisenberg::rock_object_weapon
{
    bool ObjectWeaponFoundationCompiled() {
    rock::object_physics_body_set::PureBodySet set;  // anchor the multi-body selection logic
    return set.acceptedBodyIds().empty();
}
}
