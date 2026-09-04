#pragma once

#include <functional>

namespace RE
{
    class Actor;
    class TESForm;
}

namespace heisenberg::ss2
{
    enum class DiseaseCureDispatchResult
    {
        PreflightFailed,
        Accepted,
        RejectedAfterInventoryCommit
    };

    // Compare against NPC_RPGManager's live DiseaseCureForm property. This is
    // load-order and localisation independent and follows SS2 patches that
    // redirect the manager to another cure form.
    bool IsDiseaseCureForm(RE::TESForm* form);

    // Resolve SS2's live manager quest and Papyrus objects first, then invoke
    // commitInventory immediately before submitting the script call. The
    // callback returns false when the active grab backend could not surrender
    // ownership; that aborts before inventory mutation or Papyrus dispatch.
    // This keeps a failed preflight/release from taking the held medicine away
    // while ensuring SS2 can consume it from PlayerRef when the call starts.
    DiseaseCureDispatchResult DispatchDiseaseCure(
        RE::Actor* target,
        RE::TESForm* heldCure,
        const std::function<bool()>& commitInventory);
}
