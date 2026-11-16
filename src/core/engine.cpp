#pragma once

// Falcon engine: the one file a processor includes. Its design is described in
// .claude/ongoing_work/engine.md. Its five parts build on each other in this order: samples,
// events, connections, rings, run.
#include "connections.cpp"
#include "events.cpp"
#include "rings.cpp"
#include "run.cpp"
#include "samples.cpp"
