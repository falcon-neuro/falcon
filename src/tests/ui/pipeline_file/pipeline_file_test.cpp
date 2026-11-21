// Tests the pipeline's writer and reader (src/ui/pipeline_designer/pipeline_writer.cpp and
// pipeline_reader.cpp) without the UI: it builds graphs, writes them, reads texts, and holds each
// result against what is expected. It prints a line for each test and exits with 1 if one failed.
// run.py in this folder runs it.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "all_processors.cpp"
#include "ui/pipeline_designer/pipeline_graph.cpp"
#include "ui/pipeline_designer/pipeline_reader.cpp"
#include "ui/pipeline_designer/pipeline_writer.cpp"
#include "ui/pipeline_designer/processor_list.cpp"

// The repository's folder, which run.py hands the program, for the tests that read the pipelines
// in it. Empty when the program is started without it.
inline std::filesystem::path repository_directory;

// What a file of the repository holds.
[[nodiscard]] inline std::string text_of_repository_file(const std::filesystem::path& path_in_it) {
    if (repository_directory.empty()) {
        throw std::logic_error(
            "the test reads a file of the repository, and the program wasn't "
            "given the repository's folder: run it with run.py");
    }
    std::ifstream file(repository_directory / path_in_it);
    if (!file) {
        throw std::logic_error(std::format("the test can't open {}", path_in_it.string()));
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// What a test found wrong, one text for each finding. A test that leaves it empty has passed.
using Failures = std::vector<std::string>;

inline void expect_equal_texts(Failures& failures, std::string_view what, std::string_view found,
                               std::string_view expected) {
    if (found != expected) {
        failures.push_back(
            std::format("{}:\n--- found ---\n{}\n--- expected ---\n{}", what, found, expected));
    }
}

inline void expect_text_holds(Failures& failures, std::string_view what, std::string_view text,
                              std::string_view part) {
    if (!text.contains(part)) {
        failures.push_back(std::format("{}: `{}` doesn't hold `{}`", what, text, part));
    }
}

[[nodiscard]] inline const ProcessorDescription& type_named(std::string_view type_name) {
    const auto* const type =
        std::ranges::find(ALL_PROCESSORS, type_name, &ProcessorDescription::name);
    if (type == ALL_PROCESSORS.end()) {
        throw std::logic_error(
            std::format("the test names a kind of processor, {}, that Falcon "
                        "doesn't have",
                        type_name));
    }
    return *type;
}

// A processor that a test's graph holds.
[[nodiscard]] inline GraphProcessor& processor_of(PipelineGraph& graph, ProcessorId processor_id) {
    GraphProcessor* processor = graph.find_processor(processor_id);
    if (processor == nullptr) {
        throw std::logic_error("the test names a processor that its graph doesn't hold");
    }
    return *processor;
}

[[nodiscard]] inline const GraphProcessor& processor_of(const PipelineGraph& graph,
                                                        ProcessorId processor_id) {
    const GraphProcessor* processor = graph.find_processor(processor_id);
    if (processor == nullptr) {
        throw std::logic_error("the test names a processor that its graph doesn't hold");
    }
    return *processor;
}

// What a new processor of a test's graph is: its kind, its name and its core.
struct NewProcessor {
    std::string_view type_name;
    std::string_view name;
    int core_number = 1;
};

// Adds a processor at the end of its core's list.
inline ProcessorId add_named(PipelineGraph& graph, const NewProcessor& new_processor) {
    const ProcessorId processor_id =
        graph.add_processor(type_named(new_processor.type_name),
                            {.core_number = new_processor.core_number, .next_processor = {}});
    processor_of(graph, processor_id).name = std::string(new_processor.name);
    return processor_id;
}

// One option of a processor with the value to give it, as written.
struct OptionValue {
    std::string_view option_name;
    std::string_view value;
};

inline void set_option(PipelineGraph& graph, ProcessorId processor_id,
                       const OptionValue& option_value) {
    GraphProcessor& processor = processor_of(graph, processor_id);
    const auto option = std::ranges::find(processor.type->options, option_value.option_name,
                                          &OptionDescription::name);
    if (option == processor.type->options.end()) {
        throw std::logic_error(std::format("the test names an option, {}, that a {} doesn't have",
                                           option_value.option_name, processor.type->name));
    }
    const auto option_index =
        static_cast<std::size_t>(std::ranges::distance(processor.type->options.begin(), option));
    processor.option_values.at(option_index) = std::string(option_value.value);
}

// Connects a processor's port named output to another's port named input.
inline void connect_output_to_input(PipelineGraph& graph, ProcessorId writer, ProcessorId reader) {
    if (!graph.connect({.processor = writer, .port = "output"},
                       {.processor = reader, .port = "input"})) {
        throw std::logic_error("the test makes a connection that the graph doesn't take");
    }
}

// A graph as a text that names everything it holds, with processors by their names, so that two
// graphs can be held against each other and a difference be read. Its lines are sorted: the order
// in which a graph came to hold its cores, processors and connections says nothing. The order of
// a core's list does, and is kept.
[[nodiscard]] inline std::string described(const PipelineGraph& graph) {
    std::vector<std::string> lines;
    for (const GraphCore& core : graph.cores) {
        std::string line = std::format("core {} ({}):", core.number,
                                       core.is_realtime ? "realtime" : "not realtime");
        for (const ProcessorId processor_id : core.processors) {
            line += std::format(" {}", processor_of(graph, processor_id).name);
        }
        lines.push_back(line);
    }
    for (const GraphProcessor& processor : graph.processors) {
        std::string line = std::format("processor {}: a {} on core {}, options", processor.name,
                                       processor.type->name, processor.core_number);
        for (const std::string& option_value : processor.option_values) {
            line += std::format(" [{}]", option_value);
        }
        if (processor.stream.has_value()) {
            const GraphStream stream = processor.stream.value_or(GraphStream{});
            line +=
                std::format(", stream of {} channels at {} Hz", stream.channel_count, stream.rate);
        }
        lines.push_back(line);
    }
    for (const GraphConnection& connection : graph.connections) {
        lines.push_back(std::format(
            "connection: {}.{} to {}.{}{}", processor_of(graph, connection.output.processor).name,
            connection.output.port, processor_of(graph, connection.input.processor).name,
            connection.input.port,
            connection.ring_slot_count.has_value()
                ? std::format(", ring of {} slots", connection.ring_slot_count.value_or(0))
                : std::string()));
    }
    std::ranges::sort(lines);
    std::string text = std::format("name: {}\ndescription: {}\n", graph.name, graph.description);
    for (const std::string& line : lines) {
        text += line + "\n";
    }
    return text;
}

// The pipeline of src/pipelines/monotonous_event_pipeline.cpp as a graph: an EventGenerator into
// an EventLogger, on core 1.
[[nodiscard]] inline PipelineGraph monotonous_event_graph() {
    PipelineGraph graph;
    graph.name = "Monotonous event";
    graph.description = "An EventGenerator's events, logged one by one.";
    const ProcessorId generator =
        add_named(graph, {.type_name = "EventGenerator", .name = "dummy_generator"});
    const ProcessorId logger =
        add_named(graph, {.type_name = "EventLogger", .name = "example_logger"});
    set_option(graph, generator, {.option_name = "rate", .value = "4"});
    connect_output_to_input(graph, generator, logger);
    return graph;
}

// What the writer writes of monotonous_event_graph(), after its header.
inline constexpr std::string_view MONOTONOUS_EVENT_CODE = R"(#include "core/engine.cpp"
#include "processors/event_generator.cpp"
#include "processors/event_logger.cpp"

void core1() {
    EventGenerator<{.rate = 4}> dummy_generator{"dummy_generator"};
    EventLogger<ScheduledEvent, 1, {.style = LoggingStyle::EveryEvent}> example_logger{"example_logger"};

    EventBuffer<ScheduledEvent, 1> dummy_generator_output_buffer;

    while (next_pass()) {
        dummy_generator.process({.output = dummy_generator_output_buffer});
        example_logger.process({.input = dummy_generator_output_buffer});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments, {
        {.core_number = 1, .entry_point = core1},
    });
}
)";

// The same pipeline as a person wrote it for an engine that takes a run's name: without a header,
// with comments, a layout and names of its own, and an option left out.
inline constexpr std::string_view MONOTONOUS_EVENT_BY_HAND =
    R"(// EventGenerator emits an event 4 times a second, EventLogger logs each one.
#include "../core/engine.cpp"
#include "../processors/event_generator.cpp"
#include "../processors/event_logger.cpp"

void first_core() {
    // Processors
    EventGenerator<{.rate = 4}> dummy_generator{"dummy_generator"};
    EventLogger<ScheduledEvent, 1> example_logger{"example_logger"};

    // Connections
    EventBuffer<ScheduledEvent, 1> dummy_generator_to_example_logger_event_buffer;

    while (next_pass()) {
        dummy_generator.process({.output = dummy_generator_to_example_logger_event_buffer});
        example_logger.process({
            .input = dummy_generator_to_example_logger_event_buffer,  /* the one event */
        });
    }
}

int main() {
    return run_on_cores("monotonous_event", {{.core_number = 1, .entry_point = first_core}});
}
)";

[[nodiscard]] inline std::string header_of_monotonous_event() {
    return std::format(
        "// falcon-pipeline-header-start\n"
        "// Name: Monotonous event\n"
        "// Description: An EventGenerator's events, logged one by one.\n"
        "// Generated by Falcon: {}\n"
        "// falcon-pipeline-header-end\n",
        VERSION_OF_THIS_FALCON);
}

// The pipeline of src/pipelines/spike_detector_pipeline.cpp as a graph: a SignalGenerator on core
// 4, whose stream a ring carries to a SpikeDetector on core 5, which an EventLogger reads there.
// It has no name and no description.
[[nodiscard]] inline PipelineGraph spike_detector_graph() {
    PipelineGraph graph;
    const ProcessorId generator = add_named(
        graph, {.type_name = "SignalGenerator", .name = "signal_generator", .core_number = 4});
    const ProcessorId detector = add_named(
        graph, {.type_name = "SpikeDetector", .name = "spike_detector", .core_number = 5});
    const ProcessorId logger =
        add_named(graph, {.type_name = "EventLogger", .name = "example_logger", .core_number = 5});
    set_option(graph, generator, {.option_name = "waveform", .value = "Waveform::Square"});
    set_option(graph, generator, {.option_name = "frequency", .value = "5"});
    set_option(graph, detector, {.option_name = "threshold", .value = "-70"});
    connect_output_to_input(graph, generator, detector);
    connect_output_to_input(graph, detector, logger);
    return graph;
}

// What the writer writes of spike_detector_graph(), after its header. Every option of the
// SignalGenerator is named, so a default that changes in its source changes this text.
inline constexpr std::string_view SPIKE_DETECTOR_CODE = R"(#include "core/engine.cpp"
#include "processors/event_logger.cpp"
#include "processors/signal_generator.cpp"
#include "processors/spike_detector.cpp"

using SignalGeneratorStream = StreamInfo<32, 30000.0, float>;

SampleRing<SignalGeneratorStream, 64> signal_generator_output_ring_to_core5{"signal_generator_output_ring_to_core5"};

void core4() {
    SignalGenerator<SignalGeneratorStream, {.waveform = Waveform::Square, .frequency = 5, .amplitude = 100, .phase_step_per_channel = 1, .lfp_frequency = 0, .lfp_amplitude = 0, .ripple_frequency = 180, .ripple_burst_duration_milliseconds = 80, .ripple_background_amplitude = 10, .minimum_busy_spin_microseconds = 10}> signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_ring_to_core5});
    }
}

void core5() {
    SpikeDetector<SignalGeneratorStream, {.threshold = -70}> spike_detector{"spike_detector"};
    EventLogger<Spikes<SignalGeneratorStream>, 1, {.style = LoggingStyle::EveryEvent}> example_logger{"example_logger"};

    EventBuffer<Spikes<SignalGeneratorStream>, 1> spike_detector_output_buffer;

    while (next_pass()) {
        spike_detector.process({.input = signal_generator_output_ring_to_core5, .output = spike_detector_output_buffer});
        example_logger.process({.input = spike_detector_output_buffer});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments, {
        {.core_number = 4, .entry_point = core4},
        {.core_number = 5, .entry_point = core5},
    });
}
)";

// The same pipeline as src/pipelines/spike_detector_pipeline.cpp has it, written by hand: a
// stream, a ring and functions with names of their own, comments, and options left out.
inline constexpr std::string_view SPIKE_DETECTOR_BY_HAND =
    R"(// Spike detector pipeline: SignalGenerator -> ring -> SpikeDetector -> EventLogger, with no filter.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using TestStream = StreamInfo<32, 30000.0, float>;

// Connections between cores
SampleRing<TestStream, 64> signal_generator_to_spike_detector_sample_ring{
    "signal_generator_to_spike_detector_sample_ring"};

void core1() {
    // Processors
    SignalGenerator<TestStream, {.waveform = Waveform::Square, .frequency = 5, .amplitude = 100}>
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_to_spike_detector_sample_ring});
    }
}

void core2() {
    // Processors
    SpikeDetector<TestStream, {.threshold = -70}> spike_detector{"spike_detector"};
    EventLogger<Spikes<TestStream>, 1> example_logger{"example_logger"};

    // Connections
    EventBuffer<Spikes<TestStream>, 1> spike_detector_to_example_logger_event_buffer;

    while (next_pass()) {
        spike_detector.process({
            .input = signal_generator_to_spike_detector_sample_ring,
            .output = spike_detector_to_example_logger_event_buffer,
        });
        example_logger.process({.input = spike_detector_to_example_logger_event_buffer});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 4, .entry_point = core1},
                            {.core_number = 5, .entry_point = core2},
                        });
}
)";

// The header that the writer gives a pipeline without a name and a description.
[[nodiscard]] inline std::string header_without_name() {
    return std::format(
        "// falcon-pipeline-header-start\n"
        "// Name: \n"
        "// Description: \n"
        "// Generated by Falcon: {}\n"
        "// falcon-pipeline-header-end\n",
        VERSION_OF_THIS_FALCON);
}

// A text with every run of line breaks and spaces in its code made one space. The lines that
// start with // or # stay lines of their own, since a comment and an include end with their line.
[[nodiscard]] inline std::string on_one_line(std::string_view text) {
    std::string squeezed;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t line_end = line_end_from(text, position);
        const std::string_view line =
            without_spaces_around(text.substr(position, line_end - position));
        if (line.starts_with("//") || line.starts_with('#')) {
            squeezed += std::format("\n{}\n", line);
        } else if (!line.empty()) {
            squeezed += std::format("{} ", line);
        }
        position = line_end + 1;
    }
    return squeezed;
}

// The reason a text isn't read, or "it was read" for a text that is.
[[nodiscard]] inline std::string refusal_of_text(std::string_view text) {
    const ReadPipeline read = read_pipeline(text, ALL_PROCESSORS);
    return read.has_value() ? std::string("it was read") : read.error();
}

// The reason a graph isn't written, or "it was written" for a graph that is.
[[nodiscard]] inline std::string refusal_of_graph(const PipelineGraph& graph) {
    const WrittenText written = write_pipeline(graph);
    return written.has_value() ? std::string("it was written") : written.error();
}

// A part of a text, and what is to stand in its place.
struct Replacement {
    std::string_view part;
    std::string_view with;
};

// A text with one part of it replaced.
[[nodiscard]] inline std::string replaced_in(std::string_view text,
                                             const Replacement& replacement) {
    std::string result{text};
    const std::size_t part_position = result.find(replacement.part);
    if (part_position == std::string::npos) {
        throw std::logic_error(
            std::format("the test replaces `{}`, which the text doesn't hold", replacement.part));
    }
    result.replace(part_position, replacement.part.size(), replacement.with);
    return result;
}

// What a graph gives when it is written, read back, and described; or why that didn't work.
[[nodiscard]] inline std::string described_after_round_trip(const PipelineGraph& graph) {
    const WrittenText written = write_pipeline(graph);
    if (!written.has_value()) {
        return std::format("the graph wasn't written: {}", written.error());
    }
    const ReadPipeline read = read_pipeline(*written, ALL_PROCESSORS);
    return read.has_value() ? described(*read)
                            : std::format("what was written wasn't read: {}", read.error());
}

inline void test_writer_writes_the_monotonous_event_pipeline(Failures& failures) {
    const WrittenText written = write_pipeline(monotonous_event_graph());
    if (!written.has_value()) {
        failures.push_back(std::format("the graph wasn't written: {}", written.error()));
        return;
    }
    expect_equal_texts(failures, "the text that was written", *written,
                       header_of_monotonous_event() + std::string(MONOTONOUS_EVENT_CODE));
}

inline void test_reader_reads_back_what_the_writer_wrote(Failures& failures) {
    const PipelineGraph graph = monotonous_event_graph();
    const WrittenText written = write_pipeline(graph);
    if (!written.has_value()) {
        failures.push_back(std::format("the graph wasn't written: {}", written.error()));
        return;
    }
    const ReadPipeline read = read_pipeline(*written, ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("what was written wasn't read: {}", read.error()));
        return;
    }
    expect_equal_texts(failures, "the graph that was read back", described(*read),
                       described(graph));
    const WrittenText written_again = write_pipeline(*read);
    expect_equal_texts(failures, "the text written from the graph that was read back",
                       written_again.has_value() ? *written_again : written_again.error(),
                       *written);
}

inline void test_reader_ignores_the_layout(Failures& failures) {
    const PipelineGraph graph = monotonous_event_graph();
    const std::string text =
        on_one_line(header_of_monotonous_event() + std::string(MONOTONOUS_EVENT_CODE));
    const ReadPipeline read = read_pipeline(text, ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("the text on one line wasn't read: {}", read.error()));
        return;
    }
    expect_equal_texts(failures, "the graph read from one line", described(*read),
                       described(graph));
}

inline void test_reader_reads_a_pipeline_written_by_hand(Failures& failures) {
    const ReadPipeline read = read_pipeline(MONOTONOUS_EVENT_BY_HAND, ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("the hand-written text wasn't read: {}", read.error()));
        return;
    }
    // No header, so no name and no description; the logger's option has its default.
    PipelineGraph expected = monotonous_event_graph();
    expected.name = "";
    expected.description = "";
    expect_equal_texts(failures, "the graph read from the hand-written text", described(*read),
                       described(expected));
}

inline void test_reader_takes_a_core_that_is_not_realtime(Failures& failures) {
    const std::string text =
        replaced_in(MONOTONOUS_EVENT_CODE,
                    {
                        .part = "{.core_number = 1, .entry_point = core1}",
                        .with = "{.core_number = 8, .entry_point = core1, .is_realtime = false}",
                    });
    const ReadPipeline read = read_pipeline(text, ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("the text wasn't read: {}", read.error()));
        return;
    }
    expect_text_holds(failures, "the graph that was read", described(*read),
                      "core 8 (not realtime): dummy_generator example_logger");
    const WrittenText written = write_pipeline(*read);
    expect_text_holds(failures, "the text written from it",
                      written.has_value() ? *written : written.error(),
                      "{.core_number = 8, .entry_point = core8, .is_realtime = false},");
}

inline void test_writer_writes_a_stream_and_a_ring_between_two_cores(Failures& failures) {
    const WrittenText written = write_pipeline(spike_detector_graph());
    if (!written.has_value()) {
        failures.push_back(std::format("the graph wasn't written: {}", written.error()));
        return;
    }
    expect_equal_texts(failures, "the text that was written", *written,
                       header_without_name() + std::string(SPIKE_DETECTOR_CODE));
}

inline void test_reader_reads_back_a_stream_and_a_ring(Failures& failures) {
    const PipelineGraph graph = spike_detector_graph();
    expect_equal_texts(failures, "the graph that was read back", described_after_round_trip(graph),
                       described(graph));
    const ReadPipeline read =
        read_pipeline(header_without_name() + std::string(SPIKE_DETECTOR_CODE), ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("the text wasn't read: {}", read.error()));
        return;
    }
    const WrittenText written_again = write_pipeline(*read);
    expect_equal_texts(failures, "the text written from the graph that was read",
                       written_again.has_value() ? *written_again : written_again.error(),
                       header_without_name() + std::string(SPIKE_DETECTOR_CODE));
}

inline void test_reader_reads_a_stream_and_a_ring_written_by_hand(Failures& failures) {
    const ReadPipeline read = read_pipeline(SPIKE_DETECTOR_BY_HAND, ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("the hand-written text wasn't read: {}", read.error()));
        return;
    }
    expect_equal_texts(failures, "the graph read from the hand-written text", described(*read),
                       described(spike_detector_graph()));
}

inline void test_ring_keeps_the_slots_that_the_user_set(Failures& failures) {
    PipelineGraph graph = spike_detector_graph();
    graph.connections.front().ring_slot_count = 512;
    const WrittenText written = write_pipeline(graph);
    expect_text_holds(
        failures, "the text that was written", written.has_value() ? *written : written.error(),
        "SampleRing<SignalGeneratorStream, 512> signal_generator_output_ring_to_core5");
    expect_equal_texts(failures, "the graph that was read back", described_after_round_trip(graph),
                       described(graph));
    expect_text_holds(failures, "the graph's description", described(graph),
                      "signal_generator.output to spike_detector.input, ring of 512 slots");
}

inline void test_ring_to_a_core_that_is_not_realtime_has_more_slots(Failures& failures) {
    PipelineGraph graph = spike_detector_graph();
    for (GraphCore& core : graph.cores) {
        if (core.number == 5) {
            core.is_realtime = false;
        }
    }
    const WrittenText written = write_pipeline(graph);
    expect_text_holds(
        failures, "the text that was written", written.has_value() ? *written : written.error(),
        "SampleRing<SignalGeneratorStream, 16384> signal_generator_output_ring_to_core5");
    // The connection holds no number of slots of its own, before and after.
    expect_equal_texts(failures, "the graph that was read back", described_after_round_trip(graph),
                       described(graph));
}

inline void test_buffer_that_also_feeds_a_ring(Failures& failures) {
    PipelineGraph graph;
    const ProcessorId generator = add_named(
        graph, {.type_name = "SignalGenerator", .name = "signal_generator", .core_number = 4});
    const ProcessorId near_detector =
        add_named(graph, {.type_name = "SpikeDetector", .name = "near_detector", .core_number = 4});
    const ProcessorId far_detector =
        add_named(graph, {.type_name = "SpikeDetector", .name = "far_detector", .core_number = 5});
    connect_output_to_input(graph, generator, near_detector);
    connect_output_to_input(graph, generator, far_detector);
    const WrittenText written = write_pipeline(graph);
    const std::string text = written.has_value() ? *written : written.error();
    expect_text_holds(failures, "the buffer's declaration", text,
                      "    SampleBuffer<SignalGeneratorStream> signal_generator_output_buffer"
                      "{signal_generator_output_ring_to_core5};\n");
    expect_text_holds(failures, "the writer's call", text,
                      "signal_generator.process({.output = signal_generator_output_buffer});");
    expect_text_holds(failures, "the call of the reader on the same core", text,
                      "near_detector.process({.input = signal_generator_output_buffer});");
    expect_text_holds(failures, "the call of the reader on the other core", text,
                      "far_detector.process({.input = signal_generator_output_ring_to_core5});");
    expect_equal_texts(failures, "the graph that was read back", described_after_round_trip(graph),
                       described(graph));
}

inline void test_list_is_split_into_its_elements(Failures& failures) {
    const auto elements_as_text = [](std::string_view value, bool is_element_braced) {
        const std::optional<std::vector<std::string>> elements =
            list_elements_of(value, is_element_braced);
        if (!elements.has_value()) {
            return std::string("no list");
        }
        std::string text = std::format("{}:", elements.value_or(std::vector<std::string>{}).size());
        for (const std::string& element : elements.value_or(std::vector<std::string>{})) {
            text += std::format(" [{}]", element);
        }
        return text;
    };
    expect_equal_texts(failures, "two numbers", elements_as_text("{0.5, 0.5}", false),
                       "2: [0.5] [0.5]");
    expect_equal_texts(failures, "two numbers in doubled braces",
                       elements_as_text("{{0.5, 0.5}}", false), "2: [0.5] [0.5]");
    expect_equal_texts(failures, "one number", elements_as_text("{1.0}", false), "1: [1.0]");
    expect_equal_texts(failures, "no element", elements_as_text("{}", false), "0:");
    expect_equal_texts(failures, "two texts, one with a comma",
                       elements_as_text(R"({"left, right", "middle"})", false),
                       R"(2: ["left, right"] ["middle"])");
    expect_equal_texts(failures, "two structs in doubled braces",
                       elements_as_text("{{{.b0 = 1}, {.b0 = 2}}}", true),
                       "2: [{.b0 = 1}] [{.b0 = 2}]");
    expect_equal_texts(failures, "two structs", elements_as_text("{{.b0 = 1}, {.b0 = 2}}", true),
                       "2: [{.b0 = 1}] [{.b0 = 2}]");
    expect_equal_texts(failures, "one struct", elements_as_text("{{.b0 = 1, .b1 = 2}}", true),
                       "1: [{.b0 = 1, .b1 = 2}]");
    expect_equal_texts(failures, "one struct in doubled braces",
                       elements_as_text("{{{.b0 = 1, .b1 = 2}}}", true), "1: [{.b0 = 1, .b1 = 2}]");
    expect_equal_texts(failures, "a value that is no list", elements_as_text("0.5", false),
                       "no list");
    expect_equal_texts(
        failures, "a list laid out over lines",
        on_one_line_plainly("{\n    {\n        .b0 = 1.0,\n        .b1 = 2.0,\n    },\n}"),
        "{{.b0 = 1.0, .b1 = 2.0}}");
}

inline void test_writer_writes_a_filter_with_its_list(Failures& failures) {
    PipelineGraph graph;
    const ProcessorId generator = add_named(
        graph, {.type_name = "SignalGenerator", .name = "signal_generator", .core_number = 4});
    const ProcessorId filter =
        add_named(graph, {.type_name = "FirFilter", .name = "fir_filter", .core_number = 5});
    const ProcessorId detector = add_named(
        graph, {.type_name = "SpikeDetector", .name = "spike_detector", .core_number = 5});
    set_option(graph, filter, {.option_name = "design_rate", .value = "30000.0"});
    set_option(graph, filter, {.option_name = "taps", .value = "{0.5, 0.5}"});
    connect_output_to_input(graph, generator, filter);
    connect_output_to_input(graph, filter, detector);
    const WrittenText written = write_pipeline(graph);
    expect_text_holds(failures, "the filter's declaration",
                      written.has_value() ? *written : written.error(),
                      "    FirFilter<SignalGeneratorStream, FirFilterOptions<2>{.design_rate = "
                      "30000.0, .taps = {{0.5, 0.5}}}> fir_filter{\"fir_filter\"};\n");
    expect_equal_texts(failures, "the graph that was read back", described_after_round_trip(graph),
                       described(graph));
}

inline void test_reader_reads_a_filter_written_by_hand(Failures& failures) {
    const ReadPipeline read =
        read_pipeline(text_of_repository_file("src/pipelines/filtered_spike_detector_pipeline.cpp"),
                      ALL_PROCESSORS);
    if (!read.has_value()) {
        failures.push_back(std::format("the pipeline wasn't read: {}", read.error()));
        return;
    }
    constexpr std::string_view sections =
        "{.b0 = 1.0, .b1 = 2.0, .b2 = 1.0, .a1 = -0.4116456379943422, .a2 = 0.1208387236075018}, "
        "{.b0 = 1.0, .b1 = -2.0, .b2 = 1.0, .a1 = -1.888701220483925, .a2 = 0.8929613419279413}";
    expect_text_holds(failures, "the filter as the graph holds it", described(*read),
                      std::format("processor ap_filter: a IirFilter on core 5, options [30000.0] "
                                  "[0.175014382674721] [{{{}}}]",
                                  sections));
    const WrittenText written = write_pipeline(*read);
    expect_text_holds(
        failures, "the filter as it is written again",
        written.has_value() ? *written : written.error(),
        std::format("IirFilter<SignalGeneratorStream, IirFilterOptions<2>{{.design_rate = 30000.0, "
                    ".gain = 0.175014382674721, .sections = {{{{{}}}}}}}> ap_filter",
                    sections));
}

// A pipeline of the repository, and why the reader refuses it; an empty reason for one it reads.
struct RepositoryPipeline {
    std::string_view file_name;
    std::string_view refusal;
};

inline void test_repository_pipelines_are_read_or_refused(Failures& failures) {
    // A Recorder's call gives its one input a buffer for each connection, which is what the
    // reader meets first of all it doesn't read of a Recorder.
    constexpr std::string_view recorder_refusal =
        "the reader doesn't read a port that is given several buffers";
    const std::vector<RepositoryPipeline> pipelines{
        {.file_name = "monotonous_event_pipeline.cpp", .refusal = ""},
        {.file_name = "spike_detector_pipeline.cpp", .refusal = ""},
        {.file_name = "sparse_spike_detector_pipeline.cpp", .refusal = ""},
        {.file_name = "filtered_spike_detector_pipeline.cpp", .refusal = ""},
        {.file_name = "filter_comparison_pipeline.cpp", .refusal = ""},
        {
            .file_name = "fir_chain_latency_pipeline.cpp",
            .refusal = "the reader reads `using` for a stream alone",
        },
        {.file_name = "ripple_bursts_recording_pipeline.cpp", .refusal = recorder_refusal},
        {.file_name = "ripple_detector_pipeline.cpp", .refusal = recorder_refusal},
        {.file_name = "spike_recording_pipeline.cpp", .refusal = recorder_refusal},
    };
    for (const RepositoryPipeline& pipeline : pipelines) {
        const ReadPipeline read = read_pipeline(
            text_of_repository_file(std::filesystem::path("src/pipelines") / pipeline.file_name),
            ALL_PROCESSORS);
        if (!pipeline.refusal.empty()) {
            expect_text_holds(failures, pipeline.file_name,
                              read.has_value() ? std::string("it was read") : read.error(),
                              pipeline.refusal);
        } else if (!read.has_value()) {
            failures.push_back(std::format("{} wasn't read: {}", pipeline.file_name, read.error()));
        } else {
            expect_equal_texts(failures, pipeline.file_name, described_after_round_trip(*read),
                               described(*read));
        }
    }
}

inline void test_reader_refuses_what_it_can_not_read(Failures& failures) {
    expect_text_holds(failures, "an unknown kind of processor",
                      refusal_of_text(replaced_in(MONOTONOUS_EVENT_CODE,
                                                  {
                                                      .part = "EventGenerator<",
                                                      .with = "Oscillator<",
                                                  })),
                      "line 6: `Oscillator` is no kind of processor");
    expect_text_holds(failures, "an option the kind doesn't have",
                      refusal_of_text(replaced_in(MONOTONOUS_EVENT_CODE,
                                                  {
                                                      .part = ".rate = 4",
                                                      .with = ".speed = 4",
                                                  })),
                      "line 6: a EventGenerator has no option speed");
    expect_text_holds(failures, "a variable outside the cores' functions",
                      refusal_of_text(replaced_in(MONOTONOUS_EVENT_CODE,
                                                  {
                                                      .part = "void core1",
                                                      .with = "static int count = 0;\nvoid core1",
                                                  })),
                      "line 5: the reader doesn't read what starts with `static`");
    expect_text_holds(
        failures, "a type that is named once and used for several",
        refusal_of_text(replaced_in(
            MONOTONOUS_EVENT_CODE,
            {
                .part = "void core1",
                .with = "using QuickGenerator = EventGenerator<{.rate = 4}>;\nvoid core1",
            })),
        "line 5: the reader reads `using` for a stream alone");
    expect_text_holds(
        failures, "a buffer that no output writes",
        refusal_of_text(replaced_in(
            MONOTONOUS_EVENT_CODE,
            {
                .part = "dummy_generator.process({.output = dummy_generator_output_buffer});",
                .with = "dummy_generator.process({});",
            })),
        "line 13: no output writes the buffer dummy_generator_output_buffer");
    expect_text_holds(
        failures, "a processor that the loop doesn't call",
        refusal_of_text(replaced_in(
            MONOTONOUS_EVENT_CODE,
            {
                .part =
                    "        example_logger.process({.input = dummy_generator_output_buffer});\n",
                .with = "",
            })),
        "example_logger is declared, and the loop never calls it");
    expect_text_holds(failures, "a stream that the file doesn't name",
                      refusal_of_text(replaced_in(
                          MONOTONOUS_EVENT_CODE,
                          {
                              .part = "EventGenerator<{.rate = 4}> dummy_generator",
                              .with = "SignalGenerator<Stream, {.frequency = 5}> dummy_generator",
                          })),
                      "line 6: `Stream` names no stream of this file");
    expect_text_holds(
        failures, "an option whose value names a stream",
        refusal_of_text(replaced_in(SPIKE_DETECTOR_CODE,
                                    {
                                        .part = "{.threshold = -70}",
                                        .with = "{.threshold = SignalGeneratorStream::rate}",
                                    })),
        "which names the stream SignalGeneratorStream: its value must be written out");
    expect_text_holds(
        failures, "a ring that no output writes",
        refusal_of_text(replaced_in(
            SPIKE_DETECTOR_CODE,
            {
                .part =
                    "signal_generator.process({.output = signal_generator_output_ring_to_core5});",
                .with = "signal_generator.process({});",
            })),
        "no output writes the ring signal_generator_output_ring_to_core5");
    expect_text_holds(failures, "a function that main() doesn't name",
                      refusal_of_text(replaced_in(MONOTONOUS_EVENT_CODE,
                                                  {
                                                      .part = ".entry_point = core1",
                                                      .with = ".entry_point = core2",
                                                  })),
                      "main() names core2, which the file doesn't define");
    expect_text_holds(
        failures, "a processor that takes an argument for each connection",
        refusal_of_text(replaced_in(
            MONOTONOUS_EVENT_CODE,
            {
                .part = "EventLogger<ScheduledEvent, 1, {.style = LoggingStyle::EveryEvent}>",
                .with = "Recorder<RecorderOptions<1>{.file_name = \"events.bin\", .slot_names = "
                        "{\"events\"}}, EventBuffer<ScheduledEvent, 1>>",
            })),
        "line 7: the reader doesn't read a Recorder, whose type takes an argument for each of its "
        "connections");
    expect_text_holds(failures, "a header block without its end",
                      refusal_of_text(std::format("{}\n// Name: Unfinished\n{}",
                                                  PIPELINE_HEADER_START, MONOTONOUS_EVENT_CODE)),
                      "line 1: the header block has no line");
}

inline void test_writer_calls_a_processor_after_those_it_reads_from(Failures& failures) {
    // Two chains on one core, with the readers in front of their writers in the core's list.
    PipelineGraph graph;
    const ProcessorId second_logger =
        add_named(graph, {.type_name = "EventLogger", .name = "second_logger"});
    const ProcessorId first_logger =
        add_named(graph, {.type_name = "EventLogger", .name = "first_logger"});
    const ProcessorId first_generator =
        add_named(graph, {.type_name = "EventGenerator", .name = "first_generator"});
    const ProcessorId second_generator =
        add_named(graph, {.type_name = "EventGenerator", .name = "second_generator"});
    connect_output_to_input(graph, first_generator, first_logger);
    connect_output_to_input(graph, second_generator, second_logger);
    const WrittenText written = write_pipeline(graph);
    if (!written.has_value()) {
        failures.push_back(std::format("the graph wasn't written: {}", written.error()));
        return;
    }
    expect_text_holds(
        failures, "the loop that was written", *written,
        "    while (next_pass()) {\n"
        "        first_generator.process({.output = first_generator_output_buffer});\n"
        "        first_logger.process({.input = first_generator_output_buffer});\n"
        "        second_generator.process({.output = second_generator_output_buffer});\n"
        "        second_logger.process({.input = second_generator_output_buffer});\n"
        "    }\n");
}

inline void test_writer_refuses_what_it_can_not_write(Failures& failures) {
    PipelineGraph with_recorder = monotonous_event_graph();
    add_named(with_recorder, {.type_name = "Recorder", .name = "recorder"});
    expect_text_holds(failures, "a processor that takes an argument for each connection",
                      refusal_of_graph(with_recorder),
                      "recorder: the writer doesn't write a Recorder, whose type takes an argument "
                      "for each of its connections");

    PipelineGraph with_list_without_braces = monotonous_event_graph();
    const ProcessorId filter =
        add_named(with_list_without_braces, {.type_name = "FirFilter", .name = "fir_filter"});
    set_option(with_list_without_braces, filter, {.option_name = "taps", .value = "0.5"});
    expect_text_holds(failures, "a list that isn't in braces",
                      refusal_of_graph(with_list_without_braces),
                      "fir_filter: its option taps must be a list in braces, and is `0.5`");

    PipelineGraph with_stream_without_rate = spike_detector_graph();
    for (GraphProcessor& processor : with_stream_without_rate.processors) {
        if (processor.stream.has_value()) {
            processor.stream = GraphStream{.channel_count = 32, .rate = 0.0};
        }
    }
    expect_text_holds(failures, "a stream at 0 Hz", refusal_of_graph(with_stream_without_rate),
                      "signal_generator: its stream needs one channel at least and a rate above 0");

    PipelineGraph with_two_streams_of_one_name;
    add_named(with_two_streams_of_one_name, {.type_name = "SignalGenerator", .name = "left_probe"});
    add_named(with_two_streams_of_one_name,
              {.type_name = "SignalGenerator", .name = "left__probe"});
    expect_text_holds(failures, "two streams that would get one name",
                      refusal_of_graph(with_two_streams_of_one_name),
                      "the streams they start would both be named LeftProbeStream");

    PipelineGraph with_state_value;
    const ProcessorId first_detector =
        add_named(with_state_value, {.type_name = "RippleDetector", .name = "first_detector"});
    const ProcessorId second_detector =
        add_named(with_state_value, {.type_name = "RippleDetector", .name = "second_detector"});
    if (!with_state_value.connect({.processor = first_detector, .port = "is_in_lockout"},
                                  {.processor = second_detector, .port = "is_detection_enabled"})) {
        throw std::logic_error("the test makes a connection that the graph doesn't take");
    }
    expect_text_holds(failures, "a connection of a state value", refusal_of_graph(with_state_value),
                      "first_detector.is_in_lockout to second_detector.is_detection_enabled: the "
                      "writer doesn't write a connection of a state value");

    PipelineGraph with_one_name_twice = monotonous_event_graph();
    add_named(with_one_name_twice, {.type_name = "EventGenerator", .name = "example_logger"});
    expect_text_holds(failures, "two processors of one name", refusal_of_graph(with_one_name_twice),
                      "example_logger: 2 processors have this name");

    PipelineGraph with_a_name_of_two_lines = monotonous_event_graph();
    with_a_name_of_two_lines.name = "Monotonous\nint main() {}";
    expect_text_holds(failures, "a name of two lines", refusal_of_graph(with_a_name_of_two_lines),
                      "must each fit on one line");

    expect_text_holds(failures, "a pipeline without a processor", refusal_of_graph({}),
                      "the pipeline holds no processor");
}

struct NamedTest {
    std::string_view name;
    void (*run)(Failures&);
};

// Runs every test and prints its outcome. Returns how many failed.
[[nodiscard]] inline std::size_t run_tests() {
    const std::vector<NamedTest> tests{
        {
            .name = "writer_writes_the_monotonous_event_pipeline",
            .run = test_writer_writes_the_monotonous_event_pipeline,
        },
        {
            .name = "reader_reads_back_what_the_writer_wrote",
            .run = test_reader_reads_back_what_the_writer_wrote,
        },
        {
            .name = "reader_ignores_the_layout",
            .run = test_reader_ignores_the_layout,
        },
        {
            .name = "reader_reads_a_pipeline_written_by_hand",
            .run = test_reader_reads_a_pipeline_written_by_hand,
        },
        {
            .name = "reader_takes_a_core_that_is_not_realtime",
            .run = test_reader_takes_a_core_that_is_not_realtime,
        },
        {
            .name = "writer_writes_a_stream_and_a_ring_between_two_cores",
            .run = test_writer_writes_a_stream_and_a_ring_between_two_cores,
        },
        {
            .name = "reader_reads_back_a_stream_and_a_ring",
            .run = test_reader_reads_back_a_stream_and_a_ring,
        },
        {
            .name = "reader_reads_a_stream_and_a_ring_written_by_hand",
            .run = test_reader_reads_a_stream_and_a_ring_written_by_hand,
        },
        {
            .name = "ring_keeps_the_slots_that_the_user_set",
            .run = test_ring_keeps_the_slots_that_the_user_set,
        },
        {
            .name = "ring_to_a_core_that_is_not_realtime_has_more_slots",
            .run = test_ring_to_a_core_that_is_not_realtime_has_more_slots,
        },
        {
            .name = "buffer_that_also_feeds_a_ring",
            .run = test_buffer_that_also_feeds_a_ring,
        },
        {
            .name = "list_is_split_into_its_elements",
            .run = test_list_is_split_into_its_elements,
        },
        {
            .name = "writer_writes_a_filter_with_its_list",
            .run = test_writer_writes_a_filter_with_its_list,
        },
        {
            .name = "reader_reads_a_filter_written_by_hand",
            .run = test_reader_reads_a_filter_written_by_hand,
        },
        {
            .name = "repository_pipelines_are_read_or_refused",
            .run = test_repository_pipelines_are_read_or_refused,
        },
        {
            .name = "reader_refuses_what_it_can_not_read",
            .run = test_reader_refuses_what_it_can_not_read,
        },
        {
            .name = "writer_calls_a_processor_after_those_it_reads_from",
            .run = test_writer_calls_a_processor_after_those_it_reads_from,
        },
        {
            .name = "writer_refuses_what_it_can_not_write",
            .run = test_writer_refuses_what_it_can_not_write,
        },
    };
    std::size_t failed_count = 0;
    for (const NamedTest& test : tests) {
        Failures failures;
        try {
            test.run(failures);
        } catch (const std::exception& error) {
            failures.push_back(std::format("the test stopped: {}", error.what()));
        }
        std::cout << (failures.empty() ? "PASS  " : "FAIL  ") << test.name << '\n';
        for (const std::string& failure : failures) {
            std::cout << "    " << failure << '\n';
        }
        if (!failures.empty()) {
            ++failed_count;
        }
    }
    std::cout << std::format("{} passed, {} failed\n", tests.size() - failed_count, failed_count);
    return failed_count;
}

int main(int argument_count, char** arguments) {
    try {
        const std::span<char*> all_arguments(arguments, static_cast<std::size_t>(argument_count));
        if (all_arguments.size() > 1) {
            repository_directory = all_arguments[1];
        }
        return run_tests() == 0 ? 0 : 1;
    } catch (...) {
        std::fputs("the tests stopped: something was thrown outside a test\n", stderr);
        return 2;
    }
}
