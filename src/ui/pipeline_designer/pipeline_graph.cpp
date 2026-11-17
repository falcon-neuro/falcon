#pragma once
#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "processor_list.cpp"

// A pipeline as the pipeline designer holds it: what the user builds and the generator writes out.
// It holds no positions: where a processor is drawn follows from its core and its connections.

using ProcessorId = std::uint32_t;

// What a samples connection carries besides its samples, which are float32: how many channels,
// and how many samples of each a second. A processor that starts a stream begins with these values.
struct GraphStream {
    int channel_count = 32;
    double rate = 30000.0;  // in Hz

    [[nodiscard]] bool operator==(const GraphStream&) const = default;
};

struct GraphProcessor {
    ProcessorId id = 0;
    const ProcessorDescription* type = nullptr;
    std::string name;  // its variable's name in pipeline.cpp, unique in the pipeline
    int core_number = 0;
    std::vector<std::string> option_values;  // one per option of its type, as written
    // The stream it starts, which the user sets; none for a processor that starts no stream.
    std::optional<GraphStream> stream;
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

// A place in a core's list: in front of next_processor, or at the end of the list when that is
// none.
struct CoreListPlace {
    int core_number = 0;
    std::optional<ProcessorId> next_processor;
};

// Whether an output may be connected to an input, and if not, why.
enum class ConnectionVerdict : std::uint8_t {
    Fits,
    UnknownPort,       // one of the two isn't a port of a processor in the graph
    SameProcessor,     // both are ports of one processor
    DifferentKind,     // they don't carry the same kind
    AlreadyConnected,  // these two are connected already
    InputTaken,        // the input takes one connection and has one
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

    // A new processor of this type at this place of a core's list, unconnected, with its type's
    // default options.
    ProcessorId add_processor(const ProcessorDescription& type, const CoreListPlace& place) {
        GraphProcessor processor{
            .id = next_processor_id_++,
            .type = &type,
            .name = unused_name(type),
            .core_number = place.core_number,
            .option_values = {},
            .stream = type.starts_stream ? std::optional<GraphStream>(GraphStream{}) : std::nullopt,
        };
        for (const OptionDescription& option : type.options) {
            processor.option_values.emplace_back(option.default_value);
        }
        std::vector<ProcessorId>& core_list = core(place.core_number).processors;
        core_list.insert(position_in_front_of(core_list, place.next_processor), processor.id);
        processors.push_back(std::move(processor));
        return processors.back().id;
    }

    // Puts a processor at this place of a core's list. The core may be its own, which changes
    // only its place in the list. Its connections stay as they are. Returns whether it was placed.
    bool place_processor(ProcessorId processor_id, const CoreListPlace& place) {
        const auto processor = std::ranges::find(processors, processor_id, &GraphProcessor::id);
        if (processor == processors.end() || place.next_processor == processor_id) {
            return false;
        }
        std::erase(core(processor->core_number).processors, processor_id);
        std::vector<ProcessorId>& core_list = core(place.core_number).processors;
        core_list.insert(position_in_front_of(core_list, place.next_processor), processor_id);
        processor->core_number = place.core_number;
        return true;
    }

    // Removes a processor and every connection of its ports. Returns whether there was one.
    bool remove_processor(ProcessorId processor_id) {
        const auto processor = std::ranges::find(processors, processor_id, &GraphProcessor::id);
        if (processor == processors.end()) {
            return false;
        }
        std::erase(core(processor->core_number).processors, processor_id);
        std::erase_if(connections, [processor_id](const GraphConnection& connection) {
            return connection.output.processor == processor_id ||
                   connection.input.processor == processor_id;
        });
        processors.erase(processor);
        return true;
    }

    [[nodiscard]] GraphProcessor* find_processor(ProcessorId processor_id) {
        const auto found = std::ranges::find(processors, processor_id, &GraphProcessor::id);
        return found == processors.end() ? nullptr : &*found;
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

    // The stream an output carries: the one its processor starts, or the one its processor takes
    // at its input, followed back through the processors that pass their input's stream on. None
    // for an output without samples, and while no processor that starts a stream is connected
    // before it, which is also so for processors that feed each other in a circle.
    [[nodiscard]] std::optional<GraphStream> stream_of_output(OutputReference output) const {
        for (std::size_t step = 0; step <= processors.size(); ++step) {
            const GraphProcessor* processor = find_processor(output.processor);
            const PortDescription* port =
                find_port(output.processor, output.port, PortDirection::Output);
            if (processor == nullptr || port == nullptr) {
                return std::nullopt;
            }
            if (port->stream == PortStream::Output) {
                return processor->stream;
            }
            const GraphConnection* feeding = samples_connection_into(output.processor);
            if (port->stream != PortStream::Input || feeding == nullptr) {
                return std::nullopt;
            }
            output = feeding->output;
        }
        return std::nullopt;
    }

    // The stream a processor takes at its samples input, or none while it isn't known (see
    // stream_of_output).
    [[nodiscard]] std::optional<GraphStream> stream_taken_by(ProcessorId processor_id) const {
        const GraphConnection* feeding = samples_connection_into(processor_id);
        return feeding == nullptr ? std::nullopt : stream_of_output(feeding->output);
    }

    // Whether this output may be connected to this input, and if not, the first of the reasons in
    // ConnectionVerdict's order that holds. It compares kinds only, not yet a stream's channels
    // and rate or an event's type.
    [[nodiscard]] ConnectionVerdict connection_verdict(const OutputReference& output,
                                                       const InputReference& input) const {
        const PortDescription* output_port =
            find_port(output.processor, output.port, PortDirection::Output);
        const PortDescription* input_port =
            find_port(input.processor, input.port, PortDirection::Input);
        if (output_port == nullptr || input_port == nullptr) {
            return ConnectionVerdict::UnknownPort;
        }
        if (output.processor == input.processor) {
            return ConnectionVerdict::SameProcessor;
        }
        const bool is_same_kind =
            output_port->kind == input_port->kind ||
            (input_port->kind == PortKind::Any && output_port->kind != PortKind::State);
        if (!is_same_kind) {
            return ConnectionVerdict::DifferentKind;
        }
        const bool is_already_there =
            std::ranges::any_of(connections, [&output, &input](const GraphConnection& connection) {
                return connection.output == output && connection.input == input;
            });
        if (is_already_there) {
            return ConnectionVerdict::AlreadyConnected;
        }
        const bool is_input_taken = std::ranges::any_of(
            connections,
            [&input](const GraphConnection& connection) { return connection.input == input; });
        if (is_input_taken && !input_port->has_slots) {
            return ConnectionVerdict::InputTaken;
        }
        return ConnectionVerdict::Fits;
    }

    [[nodiscard]] bool can_connect(const OutputReference& output,
                                   const InputReference& input) const {
        return connection_verdict(output, input) == ConnectionVerdict::Fits;
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

    // The connection into a processor's samples input, by which it takes its stream, or nullptr
    // while none is connected.
    [[nodiscard]] const GraphConnection* samples_connection_into(ProcessorId processor_id) const {
        const auto feeding = std::ranges::find_if(
            connections, [this, processor_id](const GraphConnection& connection) {
                const PortDescription* input_port = find_port(
                    connection.input.processor, connection.input.port, PortDirection::Input);
                return connection.input.processor == processor_id && input_port != nullptr &&
                       input_port->stream == PortStream::Input;
            });
        return feeding == connections.end() ? nullptr : &*feeding;
    }

    // Where in a core's list a processor goes that is to stand in front of next_processor: the
    // end of the list when that is none or isn't in the list.
    [[nodiscard]] static std::vector<ProcessorId>::iterator position_in_front_of(
        std::vector<ProcessorId>& core_list, std::optional<ProcessorId> next_processor) {
        return next_processor.has_value() ? std::ranges::find(core_list, *next_processor)
                                          : core_list.end();
    }

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
