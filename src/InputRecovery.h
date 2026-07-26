#pragma once

namespace heisenberg::InputRecovery
{
    /**
     * Arm the post-load watchdog. Call when LoadingMenu CLOSES (the anchor this codebase
     * already trusts as "the real load is done").
     */
    void OnLoadComplete();

    /**
     * Per-frame tick. Cheap no-op except at the armed post-load checkpoints.
     * Must be called from the game thread (it touches BSInputEnableManager).
     */
    void Tick();

    /**
     * Dump the engine's player-input enable state to the log under the given tag:
     * the cached/force flag words, every input-enable LAYER with its engine debug name,
     * and a raw dword dump of the manager so the real VR struct layout stays decodable
     * even if CommonLibF4's non-VR offsets are wrong here.
     */
    void LogInputEnableState(const char* tag);
}
