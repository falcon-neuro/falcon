#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <vector>
#include "pipeline_graph.cpp"
#include "processor_list.cpp"

// Where a core's processors are drawn, which follows from the graph alone: none of it is kept.

// A processor's place in a row of its core's box.
struct PlacedProcessor {
    ProcessorId processor = 0;
    // How far its block stands from the left of the box, in blocks. A half occurs where a
    // processor stands centred over those that read it.
    float column = 0.0f;
};

struct CoreLayout {
    // From top to bottom, each from left to right.
    std::vector<std::vector<PlacedProcessor>> rows;
    // How many blocks side by side the rows need. It is also the column of a processor added to
    // the core, which stands right of all that is there.
    std::size_t width_in_blocks = 0;
};

// A connection that places its reader, as the places of its two processors in the core's list.
struct PlacingConnection {
    std::size_t writer_place = 0;
    std::size_t reader_place = 0;
};

// The connections that place a processor of a core: those of samples and events between two of its
// processors. A state value's connection places nothing, and neither does one from another core.
[[nodiscard]] inline std::vector<PlacingConnection> placing_connections_of(
    const PipelineGraph& graph, const std::vector<ProcessorId>& processor_ids) {
    std::vector<PlacingConnection> placing_connections;
    for (const GraphConnection& connection : graph.connections) {
        const PortDescription* output_port = graph.find_port(
            connection.output.processor, connection.output.port, PortDirection::Output);
        if (output_port == nullptr || output_port->kind == PortKind::State) {
            continue;
        }
        const auto writer = std::ranges::find(processor_ids, connection.output.processor);
        const auto reader = std::ranges::find(processor_ids, connection.input.processor);
        if (writer == processor_ids.end() || reader == processor_ids.end()) {
            continue;
        }
        placing_connections.push_back({
            .writer_place =
                static_cast<std::size_t>(std::ranges::distance(processor_ids.begin(), writer)),
            .reader_place =
                static_cast<std::size_t>(std::ranges::distance(processor_ids.begin(), reader)),
        });
    }
    return placing_connections;
}

// The row of each processor of a core, by its place in the core's list: one under the lowest
// processor it reads from, and the top row, 0, when it reads from none.
[[nodiscard]] inline std::vector<std::size_t> rows_of_places(
    std::size_t place_count, const std::vector<PlacingConnection>& placing_connections) {
    std::vector<std::size_t> row_of_place(place_count, 0);
    if (place_count == 0) {
        return row_of_place;
    }
    // Connections that run in a circle have no top, so a row is never lower than the core has
    // processors, and the passes are as many: enough for any core without a circle.
    const std::size_t lowest_row = place_count - 1;
    for (std::size_t pass = 0; pass < place_count; ++pass) {
        bool has_changed = false;
        for (const PlacingConnection& connection : placing_connections) {
            const std::size_t row_under_writer =
                std::min(row_of_place.at(connection.writer_place) + 1, lowest_row);
            if (row_of_place.at(connection.reader_place) < row_under_writer) {
                row_of_place.at(connection.reader_place) = row_under_writer;
                has_changed = true;
            }
        }
        if (!has_changed) {
            break;
        }
    }
    return row_of_place;
}

// The processor each one stands under, by its place in the core's list: of those it reads from in
// the row above its own, the first in the core's list. None for a processor that reads from none
// there. Standing under a processor of a higher row only, no processor stands under itself, however
// the connections run.
[[nodiscard]] inline std::vector<std::optional<std::size_t>> places_stood_under(
    const std::vector<std::size_t>& row_of_place,
    const std::vector<PlacingConnection>& placing_connections) {
    std::vector<std::optional<std::size_t>> stood_under(row_of_place.size());
    for (const PlacingConnection& connection : placing_connections) {
        const bool is_writer_in_row_above = row_of_place.at(connection.writer_place) + 1 ==
                                            row_of_place.at(connection.reader_place);
        std::optional<std::size_t>& current = stood_under.at(connection.reader_place);
        if (is_writer_in_row_above &&
            (!current.has_value() || connection.writer_place < *current)) {
            current = connection.writer_place;
        }
    }
    return stood_under;
}

// A core's processors in rows. A processor is one row under the lowest processor of its core that
// it reads samples or events from, and stands under it; several that stand under one are side by
// side in the order of the core's list, and it is centred over them. Those that stand under none
// are side by side from the left in the order of the core's list.
[[nodiscard]] inline CoreLayout layout_of_core(const PipelineGraph& graph, int core_number) {
    const auto core = std::ranges::find(graph.cores, core_number, &GraphCore::number);
    if (core == graph.cores.end() || core->processors.empty()) {
        return {};
    }
    const std::vector<ProcessorId>& processor_ids = core->processors;
    const std::size_t place_count = processor_ids.size();
    const std::vector<PlacingConnection> placing_connections =
        placing_connections_of(graph, processor_ids);
    const std::vector<std::size_t> row_of_place = rows_of_places(place_count, placing_connections);
    const std::vector<std::optional<std::size_t>> stood_under =
        places_stood_under(row_of_place, placing_connections);

    // From the top row down, so that every processor comes after the one it stands under.
    std::vector<std::size_t> places_from_top(place_count);
    std::ranges::iota(places_from_top, std::size_t{0});
    std::ranges::stable_sort(places_from_top, {},
                             [&row_of_place](std::size_t place) { return row_of_place.at(place); });

    // A processor is as wide as those that stand under it, side by side. From the bottom row up,
    // each one's width is whole before it is added to that of the processor it stands under.
    std::vector<std::size_t> width_of_place(place_count, 0);
    for (const std::size_t place : std::views::reverse(places_from_top)) {
        width_of_place.at(place) = std::max<std::size_t>(width_of_place.at(place), 1);
        if (const std::optional<std::size_t> above = stood_under.at(place)) {
            width_of_place.at(*above) += width_of_place.at(place);
        }
    }

    // Where each processor's width starts, in blocks from the left of the box.
    std::vector<std::size_t> start_of_place(place_count, 0);
    std::vector<std::size_t> next_start_under_place(place_count, 0);
    std::size_t next_start_under_none = 0;
    for (const std::size_t place : places_from_top) {
        std::size_t* next_start = &next_start_under_none;
        if (const std::optional<std::size_t> above = stood_under.at(place)) {
            next_start = &next_start_under_place.at(*above);
        }
        start_of_place.at(place) = *next_start;
        *next_start += width_of_place.at(place);
        next_start_under_place.at(place) = start_of_place.at(place);
    }

    constexpr float half = 0.5f;
    CoreLayout layout;
    layout.width_in_blocks = next_start_under_none;
    layout.rows.resize(place_count);
    for (const std::size_t place : places_from_top) {
        layout.rows.at(row_of_place.at(place))
            .push_back({
                .processor = processor_ids.at(place),
                .column = static_cast<float>(start_of_place.at(place)) +
                          (static_cast<float>(width_of_place.at(place) - 1) * half),
            });
    }
    std::erase_if(layout.rows, [](const std::vector<PlacedProcessor>& row) { return row.empty(); });
    for (std::vector<PlacedProcessor>& row : layout.rows) {
        std::ranges::sort(row, {}, &PlacedProcessor::column);
    }
    return layout;
}
