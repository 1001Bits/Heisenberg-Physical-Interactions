#pragma once

#include <cstdint>

namespace heisenberg::dual_wield_contact
{
    // Register a ROCK weapon hull for body-specific CONTACT_STARTED events.
    // The Havok signal has no mapped unsubscribe function; Unsubscribe makes
    // any retained signal callback inert by clearing its world/body gate.
    bool Subscribe(void* hknpWorld, std::uint32_t bodyId, bool isLeft);
    void Unsubscribe(bool isLeft);
    void Reset();
}
