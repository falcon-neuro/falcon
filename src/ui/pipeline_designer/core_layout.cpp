#pragma once
#include <algorithm>
#include <cmath>
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
    // How far its block stands from the left of the box, in blocks. Parts of a block occur: a
    // processor stands centred over those that read it, or in the middle under those it reads.
    float column = 0.0f;
};

// A place among a core's chains at which a dragged processor can be dropped: the gap left of a
// chain, or the one right of the last chain.
struct DropPlace {
    // The column that the gap is at the left of, in blocks from the left of the box.
    float column = 0.0f;
    // The processor of the core's list in front of which a processor dropped here goes: the top
    // of the chain at its right. None for the end of the list.
    std::optional<ProcessorId> next_processor;
};

struct CoreLayout {
    // From top to bottom, each from left to right.
    std::vector<std::vector<PlacedProcessor>> rows;
    // Left of each chain and right of the last one, from left to right. A core without
    // processors has one.
    std::vector<DropPlace> drop_places;
    // How many blocks side by side the rows need.
    float width_in_blocks = 0.0f;
};

// A connection that places its reader, as the places of its two processors in the core's list.
struct PlacingConnection {
    std::size_t writer_place = 0;
    std::size_t reader_place = 0;
};

// The connections that are no part of a circle. Processors that feed each other in a circle have
// no top and no bottom among them, so a circle's connections place nothing. Those left never lead
// from a processor back to itself.
[[nodiscard]] inline std::vector<PlacingConnection> without_circles(
    const std::vector<PlacingConnection>& connections, std::size_t place_count) {
    // can_reach.at(start).at(end): whether connections lead from the one processor to the other.
    std::vector<std::vector<bool>> can_reach(place_count, std::vector<bool>(place_count, false));
    for (const PlacingConnection& connection : connections) {
        can_reach.at(connection.writer_place).at(connection.reader_place) = true;
    }
    for (std::size_t middle_place = 0; middle_place < place_count; ++middle_place) {
        for (std::size_t start_place = 0; start_place < place_count; ++start_place) {
            if (!can_reach.at(start_place).at(middle_place)) {
                continue;
            }
            for (std::size_t end_place = 0; end_place < place_count; ++end_place) {
                if (can_reach.at(middle_place).at(end_place)) {
                    can_reach.at(start_place).at(end_place) = true;
                }
            }
        }
    }
    std::vector<PlacingConnection> connections_outside_circles;
    for (const PlacingConnection& connection : connections) {
        if (!can_reach.at(connection.reader_place).at(connection.writer_place)) {
            connections_outside_circles.push_back(connection);
        }
    }
    return connections_outside_circles;
}

// The connections that place a processor of a core: those of samples and events between two of its
// processors, outside any circle. A state value's connection places nothing, and neither does one
// from another core.
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
    return without_circles(placing_connections, processor_ids.size());
}

// The row of each processor of a core, by its place in the core's list: one under the lowest
// processor it reads from, and the top row, 0, when it reads from none.
[[nodiscard]] inline std::vector<std::size_t> rows_of_places(
    std::size_t place_count, const std::vector<PlacingConnection>& placing_connections) {
    std::vector<std::size_t> row_of_place(place_count, 0);
    if (place_count == 0) {
        return row_of_place;
    }
    // As many passes as the core has processors are enough for connections that run in no circle,
    // and no row is then lower than the core has processors. Both are bounded all the same, so
    // that a circle among the connections could neither run on nor place a row outside the box.
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

// What a core's layout is worked out from. Each vector is by a processor's place in the core's
// list.
struct CoreStructure {
    std::vector<PlacingConnection> placing_connections;
    std::vector<std::size_t> row_of_place;
    // The processor each one stands under, if any.
    std::vector<std::optional<std::size_t>> stood_under;
    // From the top row down, so that every processor comes after the one it stands under.
    std::vector<std::size_t> places_from_top;
};

[[nodiscard]] inline CoreStructure core_structure_of(
    const PipelineGraph& graph, const std::vector<ProcessorId>& processor_ids) {
    CoreStructure structure;
    structure.placing_connections = placing_connections_of(graph, processor_ids);
    structure.row_of_place = rows_of_places(processor_ids.size(), structure.placing_connections);
    structure.stood_under =
        places_stood_under(structure.row_of_place, structure.placing_connections);
    structure.places_from_top.resize(processor_ids.size());
    std::ranges::iota(structure.places_from_top, std::size_t{0});
    std::ranges::stable_sort(structure.places_from_top, {}, [&structure](std::size_t place) {
        return structure.row_of_place.at(place);
    });
    return structure;
}

// Where each processor of a core stands, in whole blocks. Each vector but top_places is by a
// processor's place in the core's list.
struct CoreSpans {
    // How many blocks wide a processor is with all that stands under it, side by side.
    std::vector<std::size_t> width_of_place;
    // Where that width starts, in blocks from the left of the box.
    std::vector<std::size_t> start_of_place;
    // The processors that stand under none, which are the tops of the core's chains, from left
    // to right.
    std::vector<std::size_t> top_places;
    // Which of top_places a processor is, or stands under through as many as there are.
    std::vector<std::size_t> top_rank_of_place;
    std::size_t width_in_blocks = 0;
};

// Every processor under the one it stands under: several that stand under one are side by side in
// the order of the core's list. Those that stand under none are side by side from the left in the
// order of the core's list.
[[nodiscard]] inline CoreSpans spans_by_standing_under(const CoreStructure& structure) {
    const std::size_t place_count = structure.row_of_place.size();
    CoreSpans spans;

    // From the bottom row up, each one's width is whole before it is added to that of the
    // processor it stands under.
    spans.width_of_place.assign(place_count, 0);
    for (const std::size_t place : std::views::reverse(structure.places_from_top)) {
        spans.width_of_place.at(place) = std::max<std::size_t>(spans.width_of_place.at(place), 1);
        if (const std::optional<std::size_t> above = structure.stood_under.at(place)) {
            spans.width_of_place.at(*above) += spans.width_of_place.at(place);
        }
    }

    spans.start_of_place.assign(place_count, 0);
    spans.top_rank_of_place.assign(place_count, 0);
    std::vector<std::size_t> next_start_under_place(place_count, 0);
    for (const std::size_t place : structure.places_from_top) {
        std::size_t* next_start = &spans.width_in_blocks;
        if (const std::optional<std::size_t> above = structure.stood_under.at(place)) {
            next_start = &next_start_under_place.at(*above);
            spans.top_rank_of_place.at(place) = spans.top_rank_of_place.at(*above);
        } else {
            spans.top_rank_of_place.at(place) = spans.top_places.size();
            spans.top_places.push_back(place);
        }
        spans.start_of_place.at(place) = *next_start;
        *next_start += spans.width_of_place.at(place);
        next_start_under_place.at(place) = spans.start_of_place.at(place);
    }
    return spans;
}

// The column of each processor of a core, by its place in the core's list, and the blocks side by
// side that they need.
struct CoreColumns {
    std::vector<float> column_of_place;
    float width_in_blocks = 0.0f;
};

// Every processor centred over those that stand under it.
[[nodiscard]] inline CoreColumns columns_of_spans(const CoreSpans& spans) {
    constexpr float half = 0.5f;
    const std::size_t place_count = spans.start_of_place.size();
    CoreColumns columns;
    columns.width_in_blocks = static_cast<float>(spans.width_in_blocks);
    columns.column_of_place.resize(place_count);
    for (std::size_t place = 0; place < place_count; ++place) {
        columns.column_of_place.at(place) =
            static_cast<float>(spans.start_of_place.at(place)) +
            (static_cast<float>(spans.width_of_place.at(place) - 1) * half);
    }
    return columns;
}

// The middle of the processors that one reads from in rows above its own, or none when it reads
// from fewer than two there.
[[nodiscard]] inline std::optional<float> middle_under_writers(
    std::size_t reader_place, const CoreStructure& structure,
    const std::vector<float>& column_of_place) {
    std::vector<std::size_t> writer_places;
    for (const PlacingConnection& connection : structure.placing_connections) {
        const bool is_writer_above = structure.row_of_place.at(connection.writer_place) <
                                     structure.row_of_place.at(reader_place);
        if (connection.reader_place == reader_place && is_writer_above &&
            !std::ranges::contains(writer_places, connection.writer_place)) {
            writer_places.push_back(connection.writer_place);
        }
    }
    if (writer_places.size() < 2) {
        return std::nullopt;
    }
    float column_sum = 0.0f;
    for (const std::size_t writer_place : writer_places) {
        column_sum += column_of_place.at(writer_place);
    }
    return column_sum / static_cast<float>(writer_places.size());
}

// For each processor, whether it is the given one or stands under it, directly or through others.
[[nodiscard]] inline std::vector<bool> places_standing_under(std::size_t top_place,
                                                             const CoreStructure& structure) {
    std::vector<bool> is_under(structure.stood_under.size(), false);
    is_under.at(top_place) = true;
    for (const std::size_t place : structure.places_from_top) {
        const std::optional<std::size_t> above = structure.stood_under.at(place);
        if (above.has_value() && is_under.at(*above)) {
            is_under.at(place) = true;
        }
    }
    return is_under;
}

// Columns nearer than this count as the same, and blocks nearer than a block less this as
// overlapping: the columns are sums of halves and of means, so they aren't exact.
inline constexpr float COLUMN_TOLERANCE = 0.01f;

// Whether the marked processors can all move sideways by this much: none leaves the box, and none
// comes nearer than a block to one of its row that stays.
[[nodiscard]] inline bool is_room_to_move(const std::vector<bool>& is_moving, float shift,
                                          const CoreStructure& structure,
                                          const CoreColumns& columns) {
    const float rightmost_column = columns.width_in_blocks - 1.0f;
    for (std::size_t place = 0; place < is_moving.size(); ++place) {
        if (!is_moving.at(place)) {
            continue;
        }
        const float moved_column = columns.column_of_place.at(place) + shift;
        if (moved_column < -COLUMN_TOLERANCE ||
            moved_column > rightmost_column + COLUMN_TOLERANCE) {
            return false;
        }
        for (std::size_t other = 0; other < is_moving.size(); ++other) {
            const bool is_staying_in_same_row =
                !is_moving.at(other) &&
                structure.row_of_place.at(other) == structure.row_of_place.at(place);
            const float distance = std::abs(columns.column_of_place.at(other) - moved_column);
            if (is_staying_in_same_row && distance < 1.0f - COLUMN_TOLERANCE) {
                return false;
            }
        }
    }
    return true;
}

// A processor that reads from several moves into the middle under them, with all that stands under
// it, where there is room for that; where there is none it stays under the first of them. From the
// top row down, so that those it reads from are where they stay.
inline void centre_under_writers(const CoreStructure& structure, CoreColumns& columns) {
    for (const std::size_t place : structure.places_from_top) {
        const std::optional<float> middle =
            middle_under_writers(place, structure, columns.column_of_place);
        if (!middle.has_value()) {
            continue;
        }
        const float shift = *middle - columns.column_of_place.at(place);
        if (std::abs(shift) < COLUMN_TOLERANCE) {
            continue;
        }
        const std::vector<bool> is_moving = places_standing_under(place, structure);
        if (!is_room_to_move(is_moving, shift, structure, columns)) {
            continue;
        }
        for (std::size_t moved_place = 0; moved_place < is_moving.size(); ++moved_place) {
            if (is_moving.at(moved_place)) {
                columns.column_of_place.at(moved_place) += shift;
            }
        }
    }
}

// A core's processors in rows. A processor is one row under the lowest processor of its core that
// it reads samples or events from. It stands under that one, or in the middle under all it reads
// from where there is room.
[[nodiscard]] inline CoreLayout layout_of_core(const PipelineGraph& graph, int core_number) {
    CoreLayout layout;
    const auto core = std::ranges::find(graph.cores, core_number, &GraphCore::number);
    if (core == graph.cores.end() || core->processors.empty()) {
        layout.drop_places.push_back({.column = 0.0f, .next_processor = std::nullopt});
        return layout;
    }
    const std::vector<ProcessorId>& processor_ids = core->processors;
    const CoreStructure structure = core_structure_of(graph, processor_ids);
    const CoreSpans spans = spans_by_standing_under(structure);
    CoreColumns columns = columns_of_spans(spans);
    centre_under_writers(structure, columns);

    layout.width_in_blocks = columns.width_in_blocks;
    for (const std::size_t top_place : spans.top_places) {
        layout.drop_places.push_back({
            .column = static_cast<float>(spans.start_of_place.at(top_place)),
            .next_processor = processor_ids.at(top_place),
        });
    }
    layout.drop_places.push_back(
        {.column = columns.width_in_blocks, .next_processor = std::nullopt});
    layout.rows.resize(processor_ids.size());
    for (const std::size_t place : structure.places_from_top) {
        layout.rows.at(structure.row_of_place.at(place))
            .push_back({
                .processor = processor_ids.at(place),
                .column = columns.column_of_place.at(place),
            });
    }
    std::erase_if(layout.rows, [](const std::vector<PlacedProcessor>& row) { return row.empty(); });
    for (std::vector<PlacedProcessor>& row : layout.rows) {
        std::ranges::sort(row, {}, &PlacedProcessor::column);
    }
    return layout;
}

// The top of a processor's chain: the processor it stands under, through as many as there are,
// and itself when it stands under none. Its place in the core's list is the chain's.
[[nodiscard]] inline ProcessorId chain_top_of(const PipelineGraph& graph,
                                              ProcessorId processor_id) {
    const GraphProcessor* processor = graph.find_processor(processor_id);
    if (processor == nullptr) {
        return processor_id;
    }
    const auto core = std::ranges::find(graph.cores, processor->core_number, &GraphCore::number);
    if (core == graph.cores.end()) {
        return processor_id;
    }
    const std::vector<ProcessorId>& processor_ids = core->processors;
    const auto found = std::ranges::find(processor_ids, processor_id);
    if (found == processor_ids.end()) {
        return processor_id;
    }
    const CoreSpans spans = spans_by_standing_under(core_structure_of(graph, processor_ids));
    const auto place =
        static_cast<std::size_t>(std::ranges::distance(processor_ids.begin(), found));
    return processor_ids.at(spans.top_places.at(spans.top_rank_of_place.at(place)));
}
