#include "rock_integration/contact/ContactTargetIdentity.h"

#include "Physics.h"
#include "rock_integration/grab/HavokRuntimeShim.h"

#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"

#include <cmath>

// 1:1 port of ROCK physics-interaction/contact/ContactTargetIdentity.cpp — resolves a hknp
// bodyId from a contact into FO4 ref/layer/filter/surface identity for the contact pipeline.
// ROCK's havok_runtime::tryReadFilterInfo / resolveBodyToRef map to Heisenberg's equivalents
// (Physics::TryReadBodyFilterInfo / GetRefrFromBodyId); getBody comes from the grab-core shim.
// ROCK's exact status machine is preserved: the filter read is TOLERANT (failure leaves
// layer/filterInfo unknown, no early return); getBody is what gates valid/motionIndex/
// BodyUnreadable. Rich text (form type / display name / editor id) is omitted — it is
// diagnostics-only and pulls in TESFullName lookups not needed for contact routing.

namespace heisenberg::rock_core::contact_target_identity
{
    namespace
    {
        RE::NiPoint3 normalizeOrZero(const RE::NiPoint3& value)
        {
            const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
            if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-6f) {
                return {};
            }
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            return RE::NiPoint3(value.x * inverseLength, value.y * inverseLength, value.z * inverseLength);
        }
    }

    ContactTargetIdentity resolveContactTarget(RE::bhkWorld* bhkWorld,
        RE::hknpWorld* hknpWorld,
        std::uint32_t bodyId,
        contact_evidence::NativeContactEndpointKind endpointKind,
        const RE::NiPoint3* contactPointGame,
        const RE::NiPoint3* contactNormalGame,
        ContactTargetResolutionOptions options)
    {
        ContactTargetIdentity identity{};
        identity.bodyId = bodyId;
        identity.endpointKind = endpointKind;

        if (contactPointGame && isFinitePoint(*contactPointGame)) {
            identity.hasContactPoint = true;
            identity.contactPointGame = *contactPointGame;
        }
        if (contactNormalGame && isFinitePoint(*contactNormalGame)) {
            identity.hasContactNormal = true;
            identity.contactNormalGame = normalizeOrZero(*contactNormalGame);
            identity.surfaceHint = classifySurfaceHint(identity.contactNormalGame);
        }

        if (!hknpWorld || !contact_evidence::isValidBodyId(bodyId)) {
            identity.status = ContactTargetResolutionStatus::InvalidInput;
            return identity;
        }

        // Tolerant filter read (ROCK: failure leaves layer/filterInfo unknown, no early return).
        std::uint32_t filterInfo = 0;
        if (heisenberg::Physics::TryReadBodyFilterInfo(reinterpret_cast<void*>(hknpWorld), bodyId, filterInfo)) {
            identity.filterInfo = filterInfo;
            identity.layer = filterInfo & 0x7Fu;
        }

        // getBody gates valid/motionIndex/BodyUnreadable (ROCK 1:1).
        auto* body = heisenberg::rock_core::havok_runtime::getBody(reinterpret_cast<void*>(hknpWorld), RE::hknpBodyId{ bodyId });
        if (!body) {
            identity.status = ContactTargetResolutionStatus::BodyUnreadable;
            return identity;
        }
        identity.valid = true;
        identity.motionIndex = body->motionIndex;
        identity.status = ContactTargetResolutionStatus::BodyResolved;

        if (!options.resolveReference) {
            return identity;
        }
        if (!bhkWorld) {
            identity.status = ContactTargetResolutionStatus::MissingBhkWorld;
            return identity;
        }

        auto* ref = heisenberg::Physics::GetRefrFromBodyId(reinterpret_cast<void*>(bhkWorld), bodyId);
        if (!ref) {
            identity.status = ContactTargetResolutionStatus::UnresolvedReference;
            return identity;
        }

        identity.status = ContactTargetResolutionStatus::ResolvedReference;
        identity.hasResolvedReference = true;
        identity.refFormId = ref->GetFormID();
        if (auto* baseForm = ref->GetObjectReference()) {
            identity.baseFormId = baseForm->GetFormID();
        }
        return identity;
    }
}
