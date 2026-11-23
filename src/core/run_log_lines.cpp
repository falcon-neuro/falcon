#pragma once
#include <string_view>

// The lines of a run's log that say how far the run is. A program that starts an engine program,
// e.g. the UI, reads the run's state from them, so it takes their words from here.

// Every core's processors are built, and every core's loop runs.
inline constexpr std::string_view PIPELINE_IS_RUNNING_LINE =
    "all processors are built, the pipeline is running";
