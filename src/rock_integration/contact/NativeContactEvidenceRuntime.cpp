#include "rock_integration/contact/NativeContactEvidenceRuntime.h"

#include <atomic>

namespace heisenberg::native_contact_evidence
{
    namespace
    {
        heisenberg::rock_core::contact_evidence::NativeContactEvidenceCache g_cache;
        std::atomic<std::uint32_t> g_frame{ 1 };
        std::atomic<bool> g_recordingEnabled{ false };
    }

    void SetRecordingEnabled(bool enabled) { g_recordingEnabled.store(enabled, std::memory_order_relaxed); }
    bool IsRecordingEnabled() { return g_recordingEnabled.load(std::memory_order_relaxed); }

    void RecordHandContact(bool isLeft, std::uint32_t handBodyId, std::uint32_t otherBodyId,
                           const RE::NiPoint3& pointGame, const RE::NiPoint3& normalGame,
                           float penetrationGame, bool hasPoint)
    {
        heisenberg::rock_core::contact_evidence::NativeContactEvidenceRecord rec{};
        rec.frame = g_frame.load(std::memory_order_relaxed);
        rec.sourceBodyId = handBodyId;
        rec.targetBodyId = otherBodyId;
        rec.sourceKind = isLeft ? heisenberg::rock_core::contact_evidence::NativeContactEndpointKind::LeftHand
                                : heisenberg::rock_core::contact_evidence::NativeContactEndpointKind::RightHand;
        rec.sourceIsLeft = isLeft;
        rec.quality = hasPoint ? heisenberg::rock_core::contact_evidence::NativeContactQuality::RawPoint
                               : heisenberg::rock_core::contact_evidence::NativeContactQuality::BodyPairOnly;
        rec.contactPointGame = pointGame;
        rec.contactNormalGame = normalGame;
        rec.penetrationGame = penetrationGame;
        rec.hasContactPoint = hasPoint;
        g_cache.record(rec);
    }

    void Snapshot(heisenberg::rock_core::contact_evidence::NativeContactEvidenceSnapshot& out)
    {
        const auto frame = g_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_cache.snapshot(out, frame);
    }

    void Reset()
    {
        g_cache.reset();
    }
}
