#pragma once
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include "processor_list.cpp"

// A pipeline as the pipeline designer holds it: what the user builds and the generator writes out.
// It holds no positions: where a processor is drawn follows from its core and its connections.

using ProcessorId = std::uint32_t;

struct GraphProcessor {
    ProcessorId id = 0;
    const ProcessorDescription* type = nullptr;
    std::string name;  // its variable's name in pipeline.cpp, unique in the pipeline
    int core_number = 0;
    std::vector<std::string> option_values;  // one per option of its type, as written
};

// An output or an input of a processor in the graph, named as in its Connections. They are two
// types so that the two ends of a connection can't be given the wrong way round.
struct OutputReference {
    ProcessorId processor = 0;
    std::string port;

    [[nodiscard]] bool operator==(const OutputReference&) const = default;
};

struct InputReference {
    ProcessorId processor = 0;
    std::string port;

    [[nodiscard]] bool operator==(const InputReference&) const = default;
};

struct GraphConnection {
    OutputReference output;
    InputReference input;
};

struct GraphCore {
    int number = 0;
    bool is_realtime = true;
    // A chain's place from left to right, so its turn in the core's loop, is that of its first
    // processor in this list.
    std::vector<ProcessorId> processors;
};

class PipelineGraph {
public:
    std::string name;
    std::vector<GraphCore> cores;
    std::vector<GraphProcessor> processors;
    std::vector<GraphConnection> connections;

    // A new processor of this type on this core, unconnected, with its type's default options.
    ProcessorId add_processor(const ProcessorDescription& type, int core_number) {
        GraphProcessor processor{
            .id = next_processor_id_++,
            .type = &type,
            .name = unused_name(type),
            .core_number = core_number,
            .option_values = {},
        };
        for (const OptionDescription& option : type.options) {
            processor.option_values.emplace_back(option.default_value);
        }
        core(core_number).processors.push_back(processor.id);
        processors.push_back(std::move(processor));
        return processors.back().id;
    }

    [[nodiscard]] const GraphProcessor* find_processor(ProcessorId processor_id) const {
        const auto found = std::ranges::find(processors, processor_id, &GraphProcessor::id);
        return found == processors.end() ? nullptr : &*found;
    }

    // The port of this name and direction that a processor in the graph has, or nullptr.
    [[nodiscard]] const PortDescription* find_port(ProcessorId processor_id,
                                                   std::string_view port_name,
                                                   PortDirection direction) const {
        const GraphProcessor* processor = find_processor(processor_id);
        if (processor == nullptr) {
            return nullptr;
        }
        const auto found = std::ranges::find_if(
            processor->type->ports, [port_name, direction](const PortDescription& port) {
                return port.direction == direction && port_name == port.name;
            });
        return found == processor->type->ports.end() ? nullptr : &*found;
    }

    // Whether this output may be connected to this input: they are ports of two processors, they
    // carry the same kind, the connection isn't there yet, and the input is free or has slots.
    // It compares kinds only, not yet a stream's channels and rate or an event's type.
    [[nodiscard]] bool can_connect(const OutputReference& output,
                                   const InputReference& input) const {
        const PortDescription* output_port =
            find_port(output.processor, output.port, PortDirection::Output);
        const PortDescription* input_port =
            find_port(input.processor, input.port, PortDirection::Input);
        if (output_port == nullptr || input_port == nullptr ||
            output.processor == input.processor) {
            return false;
        }
        const bool is_same_kind =
            output_port->kind == input_port->kind ||
            (input_port->kind == PortKind::Any && output_port->kind != PortKind::State);
        const bool is_input_taken = std::ranges::any_of(
            connections,
            [&input](const GraphConnection& connection) { return connection.input == input; });
        const bool is_already_there =
            std::ranges::any_of(connections, [&output, &input](const GraphConnection& connection) {
                return connection.output == output && connection.input == input;
            });
        return is_same_kind && !is_already_there && (!is_input_taken || input_port->has_slots);
    }

    // Returns whether the connection was made.
    bool connect(const OutputReference& output, const InputReference& input) {
        if (!can_connect(output, input)) {
            return false;
        }
        connections.push_back({.output = output, .input = input});
        return true;
    }

    void disconnect(const InputReference& input) {
        std::erase_if(connections, [&input](const GraphConnection& connection) {
            return connection.input == input;
        });
    }

private:
    ProcessorId next_processor_id_ = 1;

    // The core with this number, added if the pipeline has none yet.
    GraphCore& core(int core_number) {
        const auto found = std::ranges::find(cores, core_number, &GraphCore::number);
        if (found != cores.end()) {
            return *found;
        }
        cores.push_back({.number = core_number, .is_realtime = true, .processors = {}});
        return cores.back();
    }

    [[nodiscard]] bool is_name_used(std::string_view candidate) const {
        return std::ranges::any_of(processors, [candidate](const GraphProcessor& processor) {
            return processor.name == candidate;
        });
    }

    // The type's file name without its folder and ending, e.g. ripple_detector, which is how the
    // hand-written pipelines name a processor; a number is added when that name is taken.
    [[nodiscard]] std::string unused_name(const ProcessorDescription& type) const {
        std::string_view stem = type.file;
        if (const std::size_t slash = stem.rfind('/'); slash != std::string_view::npos) {
            stem.remove_prefix(slash + 1);
        }
        if (const std::size_t dot = stem.rfind('.'); dot != std::string_view::npos) {
            stem.remove_suffix(stem.size() - dot);
        }
        std::string candidate{stem};
        for (int number = 2; is_name_used(candidate); ++number) {
            candidate = std::format("{}_{}", stem, number);
        }
        return candidate;
    }
};
