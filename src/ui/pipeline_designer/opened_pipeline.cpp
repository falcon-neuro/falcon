#pragma once
#include <filesystem>
#include <string>
#include "pipeline_graph.cpp"

// A pipeline as the designer is opened with it.
struct OpenedPipeline {
    std::filesystem::path directory;
    // What its pipeline.cpp holds. Of a new pipeline, its name alone.
    PipelineGraph graph;
    // The pipeline.cpp that the directory's executable was built from; empty without one.
    std::string compiled_text;
    bool is_new = false;  // its pipeline.cpp isn't written yet
};
