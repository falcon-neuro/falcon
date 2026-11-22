#pragma once
#include <deque>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "../ui_driver/ui_driver_targets.cpp"
#include "build_problems.cpp"
#include "core_layout.cpp"
#include "pipeline_graph.cpp"
#include "processor_list.cpp"

// What a test that drives the UI reads back of the pipeline designer (see
// ../ui_driver/ui_driver.cpp): its graph, what is picked and selected, the cores it shows, and
// where the layout puts each processor. A processor is called by its name, which is unique.

// What the designer holds besides its graph.
struct DesignerStateBesidesGraph {
    std::optional<ProcessorId> selected_processor;
    // The picked port: at most one of the two isn't null.
    const OutputReference* picked_output = nullptr;
    const InputReference* picked_input = nullptr;
    std::vector<int> shown_core_numbers;
    std::vector<int> offered_core_numbers;
    // How the last compilation went: none, running, succeeded or failed.
    std::string_view build_state = "none";
    std::string build_text;  // what the compiler wrote, or why nothing was compiled
    std::span<const BuildProblem> problems;
    bool is_problems_panel_open = false;
    bool is_raw_output_shown = false;
    std::string_view keeping_note;  // why the pipeline isn't kept as it stands, or empty
    // How far the pipeline's run is: none, starting, running, stopping, start_failed or failed.
    std::string_view run_state = "none";
    std::string_view bar_text;  // what the bar at the bottom says of the pipeline
    const std::deque<std::string>* run_lines = nullptr;  // what the run's program wrote
};

// Joins JSON values into a JSON array.
[[nodiscard]] inline std::string json_array(const std::vector<std::string>& values_as_json) {
    std::string json = "[";
    for (const std::string& value : values_as_json) {
        if (json.size() > 1) {
            json += ',';
        }
        json += value;
    }
    json += ']';
    return json;
}

[[nodiscard]] inline std::string json_array_of_numbers(const std::vector<int>& numbers) {
    std::vector<std::string> values;
    values.reserve(numbers.size());
    for (const int number : numbers) {
        values.push_back(std::format("{}", number));
    }
    return json_array(values);
}

// A processor's name as JSON, or null for one the graph doesn't hold.
[[nodiscard]] inline std::string processor_name_as_json(const PipelineGraph& graph,
                                                        ProcessorId processor_id) {
    const GraphProcessor* processor = graph.find_processor(processor_id);
    return processor == nullptr ? "null" : json_text(processor->name);
}

[[nodiscard]] inline std::string port_as_json(const PipelineGraph& graph, ProcessorId processor_id,
                                              std::string_view port_name) {
    return std::format(R"({{"processor":{},"port":{}}})",
                       processor_name_as_json(graph, processor_id), json_text(port_name));
}

// A stream as JSON, or null for none.
[[nodiscard]] inline std::string stream_as_json(const std::optional<GraphStream>& stream) {
    return stream.has_value() ? std::format(R"({{"channel_count":{},"rate":{}}})",
                                            stream->channel_count, stream->rate)
                              : "null";
}

// A processor with the stream it starts, the stream it takes at its samples input, and the name
// of what each of its outputs gives.
[[nodiscard]] inline std::string processor_as_json(const PipelineGraph& graph,
                                                   const GraphProcessor& processor) {
    std::string options = "{";
    std::size_t option_index = 0;
    for (const OptionDescription& option : processor.type->options) {
        if (option_index < processor.option_values.size()) {
            if (options.size() > 1) {
                options += ',';
            }
            options += std::format("{}:{}", json_text(option.name),
                                   json_text(processor.option_values.at(option_index)));
        }
        ++option_index;
    }
    options += '}';
    std::string output_names = "{";
    for (const PortDescription& port : processor.type->ports) {
        if (port.direction != PortDirection::Output) {
            continue;
        }
        if (output_names.size() > 1) {
            output_names += ',';
        }
        output_names += std::format(
            "{}:{}", json_text(port.name),
            json_text(graph.name_of_output({.processor = processor.id, .port = port.name})));
    }
    output_names += '}';
    return std::format(R"({{"name":{},"type":{},"core":{},"options":{},"stream":{},)"
                       R"("takes_stream":{},"output_names":{}}})",
                       json_text(processor.name), json_text(processor.type->name),
                       processor.core_number, options, stream_as_json(processor.stream),
                       stream_as_json(graph.stream_taken_by(processor.id)), output_names);
}

// A core's list, and its rows as the layout places them.
[[nodiscard]] inline std::string core_as_json(const PipelineGraph& graph, const GraphCore& core) {
    std::vector<std::string> names;
    names.reserve(core.processors.size());
    for (const ProcessorId processor_id : core.processors) {
        names.push_back(processor_name_as_json(graph, processor_id));
    }
    const CoreLayout layout = layout_of_core(graph, core.number);
    std::vector<std::string> rows;
    rows.reserve(layout.rows.size());
    for (const std::vector<PlacedProcessor>& row : layout.rows) {
        std::vector<std::string> placed_processors;
        placed_processors.reserve(row.size());
        for (const PlacedProcessor& placed : row) {
            placed_processors.push_back(std::format(R"({{"processor":{},"column":{}}})",
                                                    processor_name_as_json(graph, placed.processor),
                                                    placed.column));
        }
        rows.push_back(json_array(placed_processors));
    }
    return std::format(R"({{"number":{},"processors":{},"rows":{}}})", core.number,
                       json_array(names), json_array(rows));
}

[[nodiscard]] inline std::string picked_port_as_json(const PipelineGraph& graph,
                                                     const DesignerStateBesidesGraph& state) {
    if (state.picked_output != nullptr) {
        return std::format(R"({{"direction":"output","processor":{},"port":{}}})",
                           processor_name_as_json(graph, state.picked_output->processor),
                           json_text(state.picked_output->port));
    }
    if (state.picked_input != nullptr) {
        return std::format(R"({{"direction":"input","processor":{},"port":{}}})",
                           processor_name_as_json(graph, state.picked_input->processor),
                           json_text(state.picked_input->port));
    }
    return "null";
}

[[nodiscard]] inline std::string designer_state_as_json(const PipelineGraph& graph,
                                                        const DesignerStateBesidesGraph& state) {
    std::vector<std::string> processors;
    processors.reserve(graph.processors.size());
    for (const GraphProcessor& processor : graph.processors) {
        processors.push_back(processor_as_json(graph, processor));
    }
    std::vector<std::string> cores;
    cores.reserve(graph.cores.size());
    for (const GraphCore& core : graph.cores) {
        cores.push_back(core_as_json(graph, core));
    }
    std::vector<std::string> connections;
    connections.reserve(graph.connections.size());
    for (const GraphConnection& connection : graph.connections) {
        connections.push_back(
            std::format(R"({{"output":{},"input":{}}})",
                        port_as_json(graph, connection.output.processor, connection.output.port),
                        port_as_json(graph, connection.input.processor, connection.input.port)));
    }
    const std::string selected = state.selected_processor.has_value()
                                     ? processor_name_as_json(graph, *state.selected_processor)
                                     : "null";
    std::vector<std::string> problems;
    problems.reserve(state.problems.size());
    for (const BuildProblem& problem : state.problems) {
        problems.push_back(std::format(
            R"({{"title":{},"body":{},"place":{},"processor":{}}})", json_text(problem.title),
            json_text(problem.body), json_text(problem.place), json_text(problem.processor_name)));
    }
    // The latest lines only: a test reads this text in every frame.
    constexpr std::size_t most_lines_told = 200;
    std::vector<std::string> run_lines;
    if (state.run_lines != nullptr) {
        const std::size_t first_line = state.run_lines->size() > most_lines_told
                                           ? state.run_lines->size() - most_lines_told
                                           : 0;
        for (std::size_t line_index = first_line; line_index < state.run_lines->size();
             ++line_index) {
            run_lines.push_back(json_text(state.run_lines->at(line_index)));
        }
    }
    const std::string build = std::format(
        R"({{"state":{},"text":{},"problems":{},"is_panel_open":{},"is_raw_shown":{}}})",
        json_text(state.build_state), json_text(state.build_text), json_array(problems),
        state.is_problems_panel_open ? "true" : "false",
        state.is_raw_output_shown ? "true" : "false");
    return std::format(
        R"({{"screen":"designer","processors":{},"cores":{},"connections":{},"selected":{},"picked":{},)"
        R"("shown_cores":{},"offered_cores":{},"build":{},"keeping_note":{},)"
        R"("run":{{"state":{},"lines":{}}},"bar":{}}})",
        json_array(processors), json_array(cores), json_array(connections), selected,
        picked_port_as_json(graph, state), json_array_of_numbers(state.shown_core_numbers),
        json_array_of_numbers(state.offered_core_numbers), build, json_text(state.keeping_note),
        json_text(state.run_state), json_array(run_lines), json_text(state.bar_text));
}
