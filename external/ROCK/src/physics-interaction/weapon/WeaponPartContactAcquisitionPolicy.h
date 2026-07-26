#pragma once

/*
 * Provider weapon-part targets have two deliberately different acquisition
 * contracts:
 *
 * - With no exclusive target set, ROCK keeps its normal palm-ranked proximity
 *   probe. That probe makes ordinary support grabbing forgiving.
 * - An exclusive target is a reload-session whitelist. Generic proximity to
 *   the weapon is not proof that the hand touched the selected part. Accept
 *   either a native hand/body contact or a reconstruction of the same contact
 *   by overlapping the live generated hand-collider samples with the matched
 *   part's own mesh triangles. Both paths are scoped to the exact matched body;
 *   neither may substitute a nearby barrel, receiver, or other weapon part.
 *
 * Keeping this decision as a tiny policy makes it difficult for future
 * refactors to accidentally reintroduce the barrel-near-bolt fallback.
 */
namespace rock::weapon_part_contact_acquisition_policy
{
    enum class AcquisitionMode
    {
        RankedPalmProbe,
        ExactTargetMeshContact,
        // Source-compatibility name retained for policy users built against
        // the first strict-whitelist implementation.
        ExactPhysicalContact = ExactTargetMeshContact,
    };

    [[nodiscard]] inline constexpr AcquisitionMode selectMode(bool exclusiveWhitelistActive)
    {
        return exclusiveWhitelistActive ? AcquisitionMode::ExactPhysicalContact : AcquisitionMode::RankedPalmProbe;
    }

    [[nodiscard]] inline constexpr bool mayUseRankedPalmProbe(AcquisitionMode mode)
    {
        return mode == AcquisitionMode::RankedPalmProbe;
    }

    [[nodiscard]] inline constexpr bool mayAcceptExactPhysicalContact(
        AcquisitionMode mode,
        bool targetMatched)
    {
        return mode == AcquisitionMode::ExactTargetMeshContact && targetMatched;
    }

    [[nodiscard]] inline constexpr bool mayRecoverExactTargetMeshContact(
        AcquisitionMode mode,
        bool targetMatched,
        bool handColliderOverlapsMatchedMesh)
    {
        return mode == AcquisitionMode::ExactTargetMeshContact &&
               targetMatched &&
               handColliderOverlapsMatchedMesh;
    }
}
