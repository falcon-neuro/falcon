#pragma once

// Falcon engine: the one file a processor includes. Its design is described in
// docs/ongoing_work/engine.md. Its five parts build on each other in this order: samples,
// events, connections, rings, run.
//
// The pragmas tell editors that what these files define counts as coming from this one, so that
// including this file is not reported as unused.
// IWYU pragma: begin_exports
#include "connections.cpp"
#include "events.cpp"
#include "rings.cpp"
#include "run.cpp"
#include "samples.cpp"
// IWYU pragma: end_exports
