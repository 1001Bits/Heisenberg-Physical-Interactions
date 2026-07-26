#include "GrabPosePolicy.h"

#include <cstdlib>
#include <iostream>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace heisenberg::grab_pose_policy;

    Require(
        UsesAuthoredPose(true, false, true),
        "a saved placement and its saved curls must remain one authored pose");
    Require(
        !UsesAuthoredPose(false, true, true),
        "a natural touch placement must not reuse curls authored for a snap pose");
    Require(
        !UsesAuthoredPose(true, true, true),
        "natural posing must override an otherwise available saved profile");
    Require(
        !UsesAuthoredPose(true, false, false),
        "a saved transform without saved curls still needs a geometry pose");

    Require(
        !ShouldRecalculateGeometryCurls(true, false, true),
        "geometry must not overwrite a complete authored pose");
    Require(
        ShouldRecalculateGeometryCurls(false, true, true),
        "natural placement must continue to follow live mesh geometry");

    Require(
        !ShouldCommitRenderedHandRebase(true, true, true),
        "an authored pose must not freeze its hand relation during pull");
    Require(
        !ShouldCommitRenderedHandRebase(true, false, false),
        "an authored pose must not freeze against a transitioning finger pose");
    Require(
        ShouldCommitRenderedHandRebase(true, false, true),
        "a settled authored transform/curl pair may be attached to the rendered hand");
    Require(
        ShouldCommitRenderedHandRebase(false, true, false),
        "live geometry/touch placement keeps its immediate rebase behavior");

    return 0;
}
