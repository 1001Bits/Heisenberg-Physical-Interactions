#pragma once

namespace heisenberg::IntroCeremonyState
{
    // Register the F4SE co-save callbacks. Safe to call once during plugin load.
    // Returns false when the serialization interface is unavailable; runtime
    // behavior still falls back to the legacy holotape-presence inference.
    bool Initialize();

    // A state is unresolved only for a legacy save that has no Heisenberg
    // co-save record yet. PipboyInteraction resolves that once the player's
    // inventory and the intro holotape form are both available.
    bool IsResolved();
    bool IsComplete();
    bool NeedsLegacyInference();

    // kPreLoadGame guard. F4SE does not invoke a plugin's load callback when the
    // entire legacy .f4se co-save file is absent, so explicitly discard the
    // previous save's state before the engine begins reading the next save.
    void PrepareForLoad();

    // Legacy migration rule: the intro holotape in this save's inventory is
    // the only save-local evidence that the ceremony already ran. The global
    // plugin INI is deliberately not consulted.
    void ResolveLegacyFromHolotapePresence(bool hasIntroHolotape);

    // New games always begin incomplete. Completion is latched as soon as the
    // opening ceremony is delivered and is written to that save's F4SE co-save.
    void ResetForNewGame();
    void MarkComplete();
}
