#include "physics-interaction/object/ViewCasterSelectionPolicy.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace rock::viewcaster_selection_policy;

    require(chooseCandidateSource(true, true, true) ==
                CandidateSource::HostViewCaster,
        "the validated native ViewCaster target must beat incidental ROCK candidates");
    require(chooseCandidateSource(true, true, true, true) ==
                CandidateSource::RockNear,
        "the same ViewCaster reference must retain its close palm-seat geometry");
    require(chooseCandidateSource(false, true, true) ==
                CandidateSource::HostViewCaster,
        "a validated ViewCaster candidate must beat ordinary far selection");
    require(chooseCandidateSource(false, false, true) ==
                CandidateSource::RockFar,
        "ordinary ROCK far selection must remain the fallback");
    require(chooseCandidateSource(false, false, false) ==
                CandidateSource::None,
        "no valid candidate must produce no selection");

    require(shouldRunTargetedFallback(true, false, false, true),
        "a new queryable ViewCaster target missed by the normal cast needs an exact fallback cast");
    require(shouldRunTargetedFallback(true, true, false, true),
        "an incidental near object must not suppress exact ViewCaster validation");
    require(!shouldRunTargetedFallback(true, false, true, true),
        "the fallback cast is redundant when normal ROCK far selection found the exact target");
    require(!shouldRunTargetedFallback(true, false, false, false),
        "a target without usable scene data must not be queried");
    require(!shouldRunTargetedFallback(false, false, false, true),
        "an absent host target must not schedule a fallback cast");

    require(acceptsImportedCandidate(true, true, true),
        "the exact collision-validated published target may be imported");
    require(!acceptsImportedCandidate(true, true, false),
        "an unrelated body hit by the target-directed cast must be rejected");
    require(!acceptsImportedCandidate(true, false, true),
        "an invalid collision candidate must not be imported");

    constexpr std::string_view supportedTypes[] = {
        "MISC", "WEAP", "AMMO", "ALCH", "BOOK", "KEYM", "NOTE",
        "ARMO", "FLOR", "ACTI", "INGR", "CMPO", "LIGH",
    };
    for (const auto type : supportedTypes) {
        require(isSupportedLoosePickupFormType(type),
            "the loose-pickup whitelist unexpectedly rejected a supported type");
    }

    constexpr std::string_view rejectedTypes[] = {
        "CONT", "DOOR", "FURN", "TERM", "MSTT", "NPC_", "",
    };
    for (const auto type : rejectedTypes) {
        require(!isSupportedLoosePickupFormType(type),
            "the loose-pickup whitelist unexpectedly accepted an unsupported type");
    }

    std::cout << "ViewCasterSelectionPolicyTests passed\n";
    return 0;
}
