#pragma once
#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "all_processors.cpp"
#include "cpu_cores.cpp"
#include "pipeline_graph.cpp"
#include "processors_panel.cpp"
#include "tooltip.cpp"

// The pipeline designer: the panel of processors at the left, and a box for each CPU core in use,
// into which a processor is dragged from the panel. A connection is made by a click on an output
// and then on an input; Escape forgets the output, and a right click on an input removes its
// connections. Nothing here has coordinates of its own: where a processor is drawn follows from
// the graph.
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
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            picked_output_.reset();
        }
        for (const int core_number : shown_core_numbers()) {
            render_core(core_number);
            ImGui::SameLine(0.0f, padding);
        }
        render_connections();
        scroll_while_dragging_at_an_edge();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

private:
    static constexpr float processors_panel_width = 380.0f;
    static constexpr float padding = 10.0f;
    static constexpr float block_width = 220.0f;
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
    // The output clicked first, while its connection waits for the click on an input.
    std::optional<OutputReference> picked_output_;
    std::vector<BlockRectangle> block_rectangles_;

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

    // A core's box is as wide as the processors on its top row need, and at least one block wide.
    void render_core(int core_number) {
        const float box_height = ImGui::GetContentRegionAvail().y;
        const bool is_isolated = cpu_cores_.is_isolated(core_number);
        const std::vector<ProcessorId> processor_ids = processors_on(core_number);
        const auto block_count = static_cast<float>(std::max<std::size_t>(1, processor_ids.size()));
        const float box_width = (block_count * (block_width + padding)) + padding;

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
        for (const ProcessorId processor_id : processor_ids) {
            if (const GraphProcessor* processor = graph_.find_processor(processor_id)) {
                render_block(*processor);
                ImGui::SameLine(0.0f, padding);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ImGui marks the box while a processor is dragged over it.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(PROCESSOR_TYPE_PAYLOAD)) {
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
        ImGui::PopID();
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

    // A click picks the output as the start of a connection, and a second click lets it go.
    void render_output(const GraphProcessor& processor, const PortDescription& port) {
        const OutputReference output{.processor = processor.id, .port = port.name};
        const bool is_picked = picked_output_ == output;
        if (render_port_row(port, is_picked)) {
            if (is_picked) {
                picked_output_.reset();
            } else {
                picked_output_ = output;
            }
        }
    }

    // While an output is picked, the inputs it fits are marked and the others can't be clicked.
    void render_input(const GraphProcessor& processor, const PortDescription& port) {
        const InputReference input{.processor = processor.id, .port = port.name};
        const bool is_fitting =
            picked_output_.has_value() && graph_.can_connect(*picked_output_, input);
        ImGui::BeginDisabled(picked_output_.has_value() && !is_fitting);
        const bool is_clicked = render_port_row(port, is_fitting);
        const bool is_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::EndDisabled();
        if (is_clicked && is_fitting) {
            graph_.connect(*picked_output_, input);
            picked_output_.reset();
        }
        if (is_right_clicked) {
            graph_.disconnect(input);
        }
    }

    // A dot in the colour of what the port carries, then its name; the tooltip says the rest.
    // Returns whether the row was clicked.
    static bool render_port_row(const PortDescription& port, bool is_marked) {
        constexpr float dot_radius_in_line_heights = 0.27f;
        constexpr float half = 0.5f;
        constexpr float gap_after_dot = 6.0f;
        const float line_height = ImGui::GetTextLineHeight();
        const float dot_radius = line_height * dot_radius_in_line_heights;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(cursor.x + dot_radius, cursor.y + (line_height * half)), dot_radius,
            ImGui::GetColorU32(kind_colour(port.kind)));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (dot_radius * 2.0f) + gap_after_dot);
        if (port.is_optional) {
            ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        }
        ImGui::PushStyleColor(ImGuiCol_Header, marked_port_colour);
        ImGui::PushID(port.direction == PortDirection::Input ? "input" : "output");
        const bool is_clicked = ImGui::Selectable(port.name, is_marked);
        ImGui::PopID();
        ImGui::PopStyleColor();
        if (port.is_optional) {
            ImGui::PopStyleColor();
        }
        const std::string explanation =
            std::format("{}: {}\n\n{}", port.name, port_summary(port), port.description);
        render_item_tooltip(explanation.c_str());
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

    // Called inside the cores' area, after its blocks: a dot where each port meets its block, a
    // line for each connection, leaving its output downward and entering its input from above, and
    // one from the picked output to the mouse.
    void render_connections() const {
        constexpr float anchor_radius = 4.0f;
        const ImVec2 area_top_left = ImGui::GetWindowPos();
        const ImVec2 area_size = ImGui::GetWindowSize();
        ImDrawList* draw_list = ImGui::GetForegroundDrawList();
        draw_list->PushClipRect(
            area_top_left, ImVec2(area_top_left.x + area_size.x, area_top_left.y + area_size.y),
            true);
        for (const GraphProcessor& processor : graph_.processors) {
            for (const PortDescription& port : processor.type->ports) {
                if (const auto anchor = port_anchor(processor.id, port.name, port.direction)) {
                    draw_list->AddCircleFilled(*anchor, anchor_radius,
                                               ImGui::GetColorU32(kind_colour(port.kind)));
                }
            }
        }
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
        if (picked_output_) {
            const PortDescription* output_port = graph_.find_port(
                picked_output_->processor, picked_output_->port, PortDirection::Output);
            const auto at_output =
                port_anchor(picked_output_->processor, picked_output_->port, PortDirection::Output);
            if (output_port != nullptr && at_output) {
                add_connection_line(draw_list,
                                    {.at_output = *at_output, .at_input = ImGui::GetMousePos()},
                                    ImGui::GetColorU32(kind_colour(output_port->kind)));
            }
        }
        draw_list->PopClipRect();
    }

    // The two ends of a connection's line on the screen: at its output, and at its input or, while
    // the input isn't clicked yet, at the mouse.
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

    [[nodiscard]] std::vector<ProcessorId> processors_on(int core_number) const {
        const auto found = std::ranges::find(graph_.cores, core_number, &GraphCore::number);
        return found == graph_.cores.end() ? std::vector<ProcessorId>{} : found->processors;
    }
};
