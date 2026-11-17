#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include "all_processors.cpp"
#include "core_layout.cpp"
#include "cpu_cores.cpp"
#include "pipeline_graph.cpp"
#include "processors_panel.cpp"
#include "tooltip.cpp"

// The pipeline designer: the panel of processors at the left, and a box for each CPU core in use,
// into which a processor is dragged from the panel. A connection is made by a click on an output
// and one on an input, in either order; Escape, a right click, or a click anywhere but on a port
// that fits forgets the first click. While no port is picked, a right click on an input removes
// its connections. Nothing here has coordinates of its own: where a processor is drawn follows
// from the graph.
class PipelineDesigner {
public:
    void render() {
        const ImVec2 space = ImGui::GetContentRegionAvail();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("ProcessorsPanel", ImVec2(processors_panel_width, space.y),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ProcessorsPanel::render();
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);

        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("Cores", ImVec2(space.x - processors_panel_width, space.y),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        render_isolation_note();
        block_rectangles_.clear();
        is_mouse_over_picked_or_fitting_port_ = false;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            forget_picked_port();
        }
        for (const int core_number : shown_core_numbers()) {
            render_core(core_number);
            ImGui::SameLine(0.0f, padding);
        }
        forget_picked_port_on_click_elsewhere();
        render_connections();
        scroll_while_dragging_at_an_edge();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

private:
    static constexpr float processors_panel_width = 380.0f;
    static constexpr float padding = 10.0f;
    static constexpr float block_width = 220.0f;
    static constexpr float gap_between_rows = 40.0f;
    static constexpr ImVec4 core_colour{0.95f, 0.95f, 0.96f, 1.0f};
    static constexpr ImVec4 not_isolated_core_colour{1.0f, 0.90f, 0.72f, 1.0f};
    static constexpr ImVec4 warning_colour{0.78f, 0.30f, 0.0f, 1.0f};
    static constexpr ImVec4 block_colour{0.82f, 0.90f, 1.0f, 1.0f};
    static constexpr ImVec4 faint_colour{0.45f, 0.45f, 0.45f, 1.0f};
    static constexpr ImVec4 samples_colour{0.10f, 0.40f, 0.85f, 1.0f};
    static constexpr ImVec4 events_colour{0.90f, 0.45f, 0.05f, 1.0f};
    static constexpr ImVec4 state_colour{0.10f, 0.58f, 0.28f, 1.0f};
    static constexpr ImVec4 any_kind_colour{0.40f, 0.40f, 0.40f, 1.0f};
    static constexpr ImVec4 marked_port_colour{0.60f, 0.90f, 0.60f, 1.0f};

    // Where a block was drawn in this frame, on the screen.
    struct BlockRectangle {
        ProcessorId processor = 0;
        ImVec2 top_left;
        ImVec2 bottom_right;
    };

    PipelineGraph graph_;
    CpuCores cpu_cores_;
    // The port clicked first, an output or an input, while its connection waits for the click on a
    // port of the other direction.
    std::variant<std::monostate, OutputReference, InputReference> picked_port_;
    std::vector<BlockRectangle> block_rectangles_;
    // Whether, in this frame, the mouse is over the picked port or over one that fits it: the
    // ports whose row handles a click itself while a port is picked.
    bool is_mouse_over_picked_or_fitting_port_ = false;

    [[nodiscard]] bool is_port_picked() const {
        return !std::holds_alternative<std::monostate>(picked_port_);
    }

    // The picked port if it is an output, or nullptr.
    [[nodiscard]] const OutputReference* picked_output() const {
        return std::get_if<OutputReference>(&picked_port_);
    }

    // The picked port if it is an input, or nullptr.
    [[nodiscard]] const InputReference* picked_input() const {
        return std::get_if<InputReference>(&picked_port_);
    }

    void forget_picked_port() { picked_port_ = std::monostate{}; }

    // Called after this frame's ports are drawn. A right click anywhere, or a click anywhere but
    // on the picked port or on one that fits it, lets the picked port go.
    void forget_picked_port_on_click_elsewhere() {
        if (!is_port_picked()) {
            return;
        }
        const bool is_clicked_elsewhere =
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !is_mouse_over_picked_or_fitting_port_;
        if (is_clicked_elsewhere || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            forget_picked_port();
        }
    }

    // One line above the cores saying which are isolated, with the others named in its tooltip.
    void render_isolation_note() const {
        const std::vector<int> isolated = cpu_cores_.isolated();
        const std::vector<int> not_isolated = cpu_cores_.not_isolated();
        if (isolated.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
            ImGui::TextUnformatted("No core of this machine is isolated (i)");
            ImGui::PopStyleColor();
            render_item_tooltip(
                "The OS and its interrupts can run on every core, so a processor can be held up "
                "at any moment and its latency isn't dependable. Falcon needs cores isolated from "
                "the OS, which is set where the machine is started (isolcpus).");
        } else {
            const std::string note =
                std::format("Isolated cores: {} (i)", core_list_text(isolated));
            ImGui::TextUnformatted(note.c_str());
            const std::string explanation = std::format(
                "These cores are kept free of the OS, so a processor on them isn't held up. "
                "Not isolated: {}. The OS and its interrupts run there, so don't put a realtime "
                "processor on them. A processor that isn't realtime, such as a recorder, is fine "
                "there.",
                not_isolated.empty() ? "none" : core_list_text(not_isolated));
            render_item_tooltip(explanation.c_str());
        }
        ImGui::Spacing();
    }

    // Called inside the cores' area. While a processor is dragged, the area scrolls sideways when
    // the mouse is near its left or right edge, so a core beyond the window's width can be reached.
    static void scroll_while_dragging_at_an_edge() {
        if (!is_processor_dragged()) {
            return;
        }
        constexpr float edge_width = 60.0f;
        constexpr float pixels_per_second = 1200.0f;
        const ImVec2 area_position = ImGui::GetWindowPos();
        const ImVec2 area_size = ImGui::GetWindowSize();
        const ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.y < area_position.y || mouse.y > area_position.y + area_size.y) {
            return;
        }
        const float step = pixels_per_second * ImGui::GetIO().DeltaTime;
        if (mouse.x > area_position.x + area_size.x - edge_width) {
            ImGui::SetScrollX(std::min(ImGui::GetScrollX() + step, ImGui::GetScrollMaxX()));
        } else if (mouse.x < area_position.x + edge_width && mouse.x >= area_position.x) {
            ImGui::SetScrollX(std::max(ImGui::GetScrollX() - step, 0.0f));
        }
    }

    // A core's box is as wide as its rows of processors need, and at least one block wide.
    void render_core(int core_number) {
        const float box_height = ImGui::GetContentRegionAvail().y;
        const bool is_isolated = cpu_cores_.is_isolated(core_number);
        const CoreLayout layout = layout_of_core(graph_, core_number);
        const bool is_drop_place_shown = is_processor_dragged();
        const std::size_t width_in_blocks =
            std::max<std::size_t>(1, layout.width_in_blocks + (is_drop_place_shown ? 1 : 0));
        const float box_width =
            (static_cast<float>(width_in_blocks) * (block_width + padding)) + padding;

        ImGui::PushID(core_number);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              is_isolated ? core_colour : not_isolated_core_colour);
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("Core", ImVec2(box_width, box_height),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        const std::string title = std::format("Core {}", core_number);
        ImGui::TextUnformatted(title.c_str());
        if (!is_isolated) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
            ImGui::TextUnformatted("not isolated (i)");
            ImGui::PopStyleColor();
            render_item_tooltip(
                "The OS and its interrupts run on this core, so a processor here can be held up "
                "at any moment. Keep realtime processors on isolated cores. A processor that "
                "isn't realtime, such as a recorder, is fine here.");
        }
        ImGui::Separator();
        ImGui::Spacing();
        render_rows(layout, is_drop_place_shown);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        accept_dropped_processor(core_number);
        ImGui::PopID();
    }

    // Called inside a core's box: its rows of blocks from top to bottom and, while a processor is
    // dragged from the panel, the place it would be dropped at.
    void render_rows(const CoreLayout& layout, bool is_drop_place_shown) {
        const float left_edge = ImGui::GetCursorPosX();
        bool is_top_row = true;
        for (const std::vector<PlacedProcessor>& row : layout.rows) {
            bool is_first_of_row = true;
            for (const PlacedProcessor& placed : row) {
                const GraphProcessor* processor = graph_.find_processor(placed.processor);
                if (processor == nullptr) {
                    continue;
                }
                move_to_column(left_edge, placed.column, is_first_of_row);
                render_block(*processor);
                is_first_of_row = false;
            }
            if (is_top_row && is_drop_place_shown) {
                move_to_column(left_edge, static_cast<float>(layout.width_in_blocks),
                               is_first_of_row);
                render_drop_place();
            }
            is_top_row = false;
            // Room for the connections' lines to bend between two rows.
            ImGui::Dummy(ImVec2(0.0f, gap_between_rows));
        }
        if (layout.rows.empty() && is_drop_place_shown) {
            render_drop_place();
        }
    }

    // Moves to where a block of this column starts, on the line of the block drawn before it
    // unless it is the first of its row.
    static void move_to_column(float left_edge, float column, bool is_first_of_row) {
        if (!is_first_of_row) {
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(left_edge + (column * (block_width + padding)));
    }

    // Called right after a core's box, which it makes a place to drop a processor dragged from the
    // panel; ImGui marks the box while one is dragged over it.
    void accept_dropped_processor(int core_number) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PROCESSOR_TYPE_PAYLOAD)) {
            std::size_t processor_index = 0;
            if (std::cmp_equal(payload->DataSize, sizeof(processor_index))) {
                std::memcpy(&processor_index, payload->Data, sizeof(processor_index));
                if (processor_index < ALL_PROCESSORS.size()) {
                    graph_.add_processor(ALL_PROCESSORS.at(processor_index), core_number);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Where a processor dragged from the panel is drawn once dropped into this core: an empty
    // place with a dashed outline, at the end of the core's top row.
    static void render_drop_place() {
        constexpr float height_in_text_lines = 4.0f;
        constexpr float half = 0.5f;
        constexpr const char* label = "Drop here";
        const ImVec2 size(block_width,
                          ImGui::GetTextLineHeightWithSpacing() * height_in_text_lines);
        ImGui::Dummy(size);
        const ImVec2 top_left = ImGui::GetItemRectMin();
        const ImVec2 bottom_right = ImGui::GetItemRectMax();
        const ImU32 colour = ImGui::GetColorU32(faint_colour);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        add_dashed_line(draw_list, top_left, ImVec2(bottom_right.x, top_left.y), colour);
        add_dashed_line(draw_list, ImVec2(bottom_right.x, top_left.y), bottom_right, colour);
        add_dashed_line(draw_list, bottom_right, ImVec2(top_left.x, bottom_right.y), colour);
        add_dashed_line(draw_list, ImVec2(top_left.x, bottom_right.y), top_left, colour);
        const ImVec2 label_size = ImGui::CalcTextSize(label);
        draw_list->AddText(ImVec2(top_left.x + ((size.x - label_size.x) * half),
                                  top_left.y + ((size.y - label_size.y) * half)),
                           colour, label);
    }

    static void add_dashed_line(ImDrawList* draw_list, ImVec2 line_start, ImVec2 line_end,
                                ImU32 colour) {
        constexpr float dash_length = 7.0f;
        constexpr float gap_length = 5.0f;
        constexpr float thickness = 1.5f;
        const float length = std::hypot(line_end.x - line_start.x, line_end.y - line_start.y);
        if (length <= 0.0f) {
            return;
        }
        const ImVec2 direction((line_end.x - line_start.x) / length,
                               (line_end.y - line_start.y) / length);
        const int dash_count = static_cast<int>(std::ceil(length / (dash_length + gap_length)));
        for (int dash = 0; dash < dash_count; ++dash) {
            const float dash_start = static_cast<float>(dash) * (dash_length + gap_length);
            const float dash_end = std::min(dash_start + dash_length, length);
            draw_list->AddLine(ImVec2(line_start.x + (direction.x * dash_start),
                                      line_start.y + (direction.y * dash_start)),
                               ImVec2(line_start.x + (direction.x * dash_end),
                                      line_start.y + (direction.y * dash_end)),
                               colour, thickness);
        }
    }

    // A block: its inputs on top, its name and type, its outputs at the bottom, as data flows.
    void render_block(const GraphProcessor& processor) {
        ImGui::PushID(static_cast<int>(processor.id));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, block_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiChildFlags block_flags = ImGuiChildFlags_Borders |
                                            ImGuiChildFlags_AlwaysUseWindowPadding |
                                            ImGuiChildFlags_AutoResizeY;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Processor", ImVec2(block_width, 0.0f), block_flags);
        if (has_port(*processor.type, PortDirection::Input)) {
            render_ports(processor, PortDirection::Input);
            ImGui::Separator();
        }
        ImGui::TextUnformatted(processor.name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted(processor.type->name);
        ImGui::PopStyleColor();
        if (has_port(*processor.type, PortDirection::Output)) {
            ImGui::Separator();
            render_ports(processor, PortDirection::Output);
        }
        ImGui::EndChild();
        block_rectangles_.push_back({
            .processor = processor.id,
            .top_left = ImGui::GetItemRectMin(),
            .bottom_right = ImGui::GetItemRectMax(),
        });
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    [[nodiscard]] static bool has_port(const ProcessorDescription& type, PortDirection direction) {
        return std::ranges::any_of(type.ports, [direction](const PortDescription& port) {
            return port.direction == direction;
        });
    }

    void render_ports(const GraphProcessor& processor, PortDirection direction) {
        for (const PortDescription& port : processor.type->ports) {
            if (port.direction != direction) {
                continue;
            }
            if (direction == PortDirection::Output) {
                render_output(processor, port);
            } else {
                render_input(processor, port);
            }
        }
    }

    // A click picks the output as one end of a connection, and a second click lets it go. While a
    // port is picked, the outputs that fit it are marked and every other port can't be clicked.
    void render_output(const GraphProcessor& processor, const PortDescription& port) {
        const OutputReference output{.processor = processor.id, .port = port.name};
        const OutputReference* const picked = picked_output();
        const bool is_picked = picked != nullptr && *picked == output;
        const InputReference* const waiting_input = picked_input();
        const bool is_fitting =
            waiting_input != nullptr && graph_.can_connect(output, *waiting_input);
        const bool is_picked_or_fitting = is_picked || is_fitting;
        ImGui::BeginDisabled(is_port_picked() && !is_picked_or_fitting);
        const bool is_clicked = render_port_row(port, is_picked_or_fitting);
        if (is_picked_or_fitting && ImGui::IsItemHovered()) {
            is_mouse_over_picked_or_fitting_port_ = true;
        }
        ImGui::EndDisabled();
        if (is_clicked) {
            if (is_fitting) {
                graph_.connect(output, *waiting_input);
                forget_picked_port();
            } else if (is_picked) {
                forget_picked_port();
            } else {
                picked_port_ = output;
            }
        }
    }

    // A click picks the input as one end of a connection, and a second click lets it go. While a
    // port is picked, the inputs that fit it are marked and every other port can't be clicked.
    void render_input(const GraphProcessor& processor, const PortDescription& port) {
        const InputReference input{.processor = processor.id, .port = port.name};
        const InputReference* const picked = picked_input();
        const bool is_picked = picked != nullptr && *picked == input;
        const OutputReference* const waiting_output = picked_output();
        const bool is_fitting =
            waiting_output != nullptr && graph_.can_connect(*waiting_output, input);
        const bool is_picked_or_fitting = is_picked || is_fitting;
        ImGui::BeginDisabled(is_port_picked() && !is_picked_or_fitting);
        const bool is_clicked = render_port_row(port, is_picked_or_fitting);
        if (is_picked_or_fitting && ImGui::IsItemHovered()) {
            is_mouse_over_picked_or_fitting_port_ = true;
        }
        // While a port is picked, a right click only lets it go.
        const bool is_right_clicked =
            !is_port_picked() && ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::EndDisabled();
        if (is_clicked) {
            if (is_fitting) {
                graph_.connect(*waiting_output, input);
                forget_picked_port();
            } else if (is_picked) {
                forget_picked_port();
            } else {
                picked_port_ = input;
            }
        }
        if (is_right_clicked) {
            graph_.disconnect(input);
        }
    }

    // A dot in the colour of what the port carries, then its name; the tooltip says the rest. The
    // dot and the name are one place to click. Returns whether it was clicked.
    static bool render_port_row(const PortDescription& port, bool is_marked) {
        constexpr float dot_radius_in_line_heights = 0.7f;
        constexpr float half = 0.5f;
        constexpr float gap_after_dot = 8.0f;
        const float line_height = ImGui::GetTextLineHeight();
        const float dot_radius = line_height * dot_radius_in_line_heights;
        const float row_height = dot_radius * 2.0f;
        const ImVec2 row_top_left = ImGui::GetCursorScreenPos();

        ImGui::PushStyleColor(ImGuiCol_Header, marked_port_colour);
        ImGui::PushID(port.direction == PortDirection::Input ? "input" : "output");
        ImGui::PushID(port.name);
        const bool is_clicked = ImGui::Selectable("##port", is_marked, ImGuiSelectableFlags_None,
                                                  ImVec2(0.0f, row_height));
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopStyleColor();
        const std::string explanation =
            std::format("{}: {}\n\n{}", port.name, port_summary(port), port.description);
        render_item_tooltip(explanation.c_str());

        // Drawn after the row so that they lie over its highlight.
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddCircleFilled(ImVec2(row_top_left.x + dot_radius, row_top_left.y + dot_radius),
                                   dot_radius, ImGui::GetColorU32(kind_colour(port.kind)));
        const ImVec4 name_colour =
            port.is_optional ? faint_colour : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        draw_list->AddText(ImVec2(row_top_left.x + row_height + gap_after_dot,
                                  row_top_left.y + ((row_height - line_height) * half)),
                           ImGui::GetColorU32(name_colour), port.name);
        return is_clicked;
    }

    // Where a port's line meets its block: the inputs spread along the block's top edge and the
    // outputs along its bottom edge, each in its order in the block.
    [[nodiscard]] std::optional<ImVec2> port_anchor(ProcessorId processor_id,
                                                    std::string_view port_name,
                                                    PortDirection direction) const {
        const GraphProcessor* processor = graph_.find_processor(processor_id);
        const auto rectangle =
            std::ranges::find(block_rectangles_, processor_id, &BlockRectangle::processor);
        if (processor == nullptr || rectangle == block_rectangles_.end()) {
            return std::nullopt;
        }
        int port_count = 0;
        int port_place = -1;
        for (const PortDescription& port : processor->type->ports) {
            if (port.direction != direction) {
                continue;
            }
            if (port_name == port.name) {
                port_place = port_count;
            }
            ++port_count;
        }
        if (port_place < 0) {
            return std::nullopt;
        }
        constexpr float half = 0.5f;
        const float width = rectangle->bottom_right.x - rectangle->top_left.x;
        const float along =
            (static_cast<float>(port_place) + half) / static_cast<float>(port_count);
        return ImVec2(rectangle->top_left.x + (width * along), direction == PortDirection::Input
                                                                   ? rectangle->top_left.y
                                                                   : rectangle->bottom_right.y);
    }

    // Called inside the cores' area, after its blocks: a line for each connection, leaving its
    // output downward and entering its input from above, and one between the picked port and the
    // mouse.
    void render_connections() const {
        const ImVec2 area_top_left = ImGui::GetWindowPos();
        const ImVec2 area_size = ImGui::GetWindowSize();
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        draw_list->PushClipRect(
            area_top_left, ImVec2(area_top_left.x + area_size.x, area_top_left.y + area_size.y),
            true);
        for (const GraphConnection& connection : graph_.connections) {
            const PortDescription* output_port = graph_.find_port(
                connection.output.processor, connection.output.port, PortDirection::Output);
            const auto at_output = port_anchor(connection.output.processor, connection.output.port,
                                               PortDirection::Output);
            const auto at_input = port_anchor(connection.input.processor, connection.input.port,
                                              PortDirection::Input);
            if (output_port != nullptr && at_output && at_input) {
                add_connection_line(draw_list, {.at_output = *at_output, .at_input = *at_input},
                                    ImGui::GetColorU32(kind_colour(output_port->kind)));
            }
        }
        if (const OutputReference* const output = picked_output()) {
            const PortDescription* output_port =
                graph_.find_port(output->processor, output->port, PortDirection::Output);
            const auto at_output =
                port_anchor(output->processor, output->port, PortDirection::Output);
            if (output_port != nullptr && at_output) {
                add_connection_line(draw_list,
                                    {.at_output = *at_output, .at_input = ImGui::GetMousePos()},
                                    ImGui::GetColorU32(kind_colour(output_port->kind)));
            }
        }
        if (const InputReference* const input = picked_input()) {
            const PortDescription* input_port =
                graph_.find_port(input->processor, input->port, PortDirection::Input);
            const auto at_input = port_anchor(input->processor, input->port, PortDirection::Input);
            if (input_port != nullptr && at_input) {
                add_connection_line(draw_list,
                                    {.at_output = ImGui::GetMousePos(), .at_input = *at_input},
                                    ImGui::GetColorU32(kind_colour(input_port->kind)));
            }
        }
        draw_list->PopClipRect();
    }

    // The two ends of a connection's line on the screen: each at its port or, while that port
    // isn't clicked yet, at the mouse.
    struct LineEnds {
        ImVec2 at_output;
        ImVec2 at_input;
    };

    // A curve that leaves the output downward and reaches the input from above, so it bends into
    // an S when the input isn't below the output.
    static void add_connection_line(ImDrawList* draw_list, const LineEnds& ends, ImU32 colour) {
        constexpr float bend = 60.0f;
        constexpr float thickness = 2.0f;
        draw_list->AddBezierCubic(ends.at_output, ImVec2(ends.at_output.x, ends.at_output.y + bend),
                                  ImVec2(ends.at_input.x, ends.at_input.y - bend), ends.at_input,
                                  colour, thickness);
    }

    [[nodiscard]] static ImVec4 kind_colour(PortKind kind) {
        switch (kind) {
            case PortKind::Samples:
                return samples_colour;
            case PortKind::Events:
                return events_colour;
            case PortKind::State:
                return state_colour;
            case PortKind::Any:
                return any_kind_colour;
        }
        return any_kind_colour;
    }

    // The cores that hold a processor, or the first core offered while none does. While a
    // processor is dragged from the panel, the next free core is shown too, as a place to drop it.
    [[nodiscard]] std::vector<int> shown_core_numbers() const {
        std::vector<int> core_numbers;
        for (const GraphCore& core : graph_.cores) {
            if (!core.processors.empty()) {
                core_numbers.push_back(core.number);
            }
        }
        std::ranges::sort(core_numbers);
        const std::vector<int>& offered = cpu_cores_.offered();
        if (core_numbers.empty() && !offered.empty()) {
            core_numbers.push_back(offered.front());
        }
        if (is_processor_dragged()) {
            const auto next_free = std::ranges::find_if(offered, [&core_numbers](int core_number) {
                return !std::ranges::contains(core_numbers, core_number);
            });
            if (next_free != offered.end()) {
                core_numbers.push_back(*next_free);
            }
        }
        return core_numbers;
    }

    [[nodiscard]] static bool is_processor_dragged() {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        return payload != nullptr && payload->IsDataType(PROCESSOR_TYPE_PAYLOAD);
    }
};
