#pragma once

// Heisenberg runtime around ROCK's NativeContactEvidence cache. The Havok type-3 contact
// callback (ContactImpulseListener) records each hand-body contact's point + normal here;
// the mode-3 soft-contact runtime snapshots it each frame and turns the freshest world-
// static contact into a native world candidate (more accurate than the 6-axis raycast).
// This is the boundary ROCK's NativeContactEvidence.h describes.

#include "rock_integration/contact/NativeContactEvidence.h"

#include <RE/Fallout.h>
#include <cstdint>

namespace heisenberg::native_contact_evidence
{
    // Called from the physics-thread contact callback for a hand-body contact captured from
    // a type-3 manifold event. pointGame/normalGame are game units; normal is unit length.
    // penetrationGame is the manifold's own depth (>=0, game units); hasPoint=false records a
    // normal-only ("reused jacobian") contact whose inline point/depth are absent this frame.
    void RecordHandContact(bool isLeft, std::uint32_t handBodyId, std::uint32_t otherBodyId,
                           const RE::NiPoint3& pointGame, const RE::NiPoint3& normalGame,
                           float penetrationGame, bool hasPoint);

    // Snapshot the cache for the current frame (advances the internal frame counter). Call
    // once per soft-contact update on the game thread.
    void Snapshot(heisenberg::rock_core::contact_evidence::NativeContactEvidenceSnapshot& out);

    void Reset();

    // Published by the game thread (HandWallPushback::Update) and read by the Havok worker-thread
    // contact callback — replaces an unsynchronized read of plain g_config int/bool fields.
    void SetRecordingEnabled(bool enabled);
    bool IsRecordingEnabled();
}
