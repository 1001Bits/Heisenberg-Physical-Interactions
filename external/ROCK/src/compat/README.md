# CommonLibF4VR compatibility layouts

ROCK's native Fallout 4 VR physics path requires reverse-engineered
CommonLib-style type/layout headers and the VR-compatible Papyrus dispatch
helpers from its pinned framework revision. They are vendored here so these
high-risk runtime layouts remain explicit and hash-identical to ROCK's
known-good framework workspace.

This directory is intentionally placed before CommonLibF4VR's include
directory for both `rock_engine` and the Heisenberg DLL. The headers contain
layouts and inline relocation wrappers only; they do not add linker objects.
