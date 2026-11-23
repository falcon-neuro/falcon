#pragma once
#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include "../common/colours.cpp"
#include "../common/fonts.cpp"
#include "../common/icons.cpp"
#include "../ui_driver/ui_driver_targets.cpp"
#include "all_processors.cpp"
#include "processor_icons.cpp"
#include "tooltip.cpp"

// What a processor dragged out of the panel carries: its place in ALL_PROCESSORS, a std::size_t.
inline constexpr const char* PROCESSOR_TYPE_PAYLOAD = "falcon_processor_type";

[[nodiscard]] inline const char* port_direction_name(PortDirection direction) {
    return direction == PortDirection::Input ? "input" : "output";
}

[[nodiscard]] inline const char* port_kind_name(PortKind kind) {
    switch (kind) {
        case PortKind::Samples:
            return "samples";
        case PortKind::Events:
            return "events";
        case PortKind::State:
            return "a state value";
        case PortKind::Any:
            return "samples or events";
    }
    return "";
}

// "input of samples, optional": what a port is, in a few words.
[[nodiscard]] inline std::string port_summary(const PortDescription& port) {
    return std::format("{} of {}{}{}", port_direction_name(port.direction),
                       port_kind_name(port.kind), port.has_slots ? ", any number" : "",
                       port.is_optional ? ", optional" : "");
}

// The three kinds of processor, by their connections, in the order the panel lists them.
enum class ProcessorKind : std::uint8_t { Input, Operator, Output };

// An input processor has only outputs, an output processor only inputs, and an operator both. A
// processor without connections counts as an operator, so that the panel lists it all the same.
[[nodiscard]] inline ProcessorKind processor_kind(const ProcessorDescription& processor) {
    const bool has_input = std::ranges::any_of(processor.ports, [](const PortDescription& port) {
        return port.direction == PortDirection::Input;
    });
    const bool has_output = std::ranges::any_of(processor.ports, [](const PortDescription& port) {
        return port.direction == PortDirection::Output;
    });
    if (has_output && !has_input) {
        return ProcessorKind::Input;
    }
    if (has_input && !has_output) {
        return ProcessorKind::Output;
    }
    return ProcessorKind::Operator;
}

// Lists every processor Falcon was built with, grouped by kind, each with what its own source says
// of it: its description, its options and its connections, which a click on its row shows and
// hides. The info icon beside an option or a connection shows its description. A processor is
// dragged from here into a core of the pipeline designer.
class ProcessorsPanel {
public:
    static void render() {
        push_ui_font(PANEL_TEXT_SIZE, FontStyle::Regular);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, gap_between_rows));
        render_title();
        render_group("Inputs", ProcessorKind::Input);
        render_group("Operators", ProcessorKind::Operator);
        render_group("Outputs", ProcessorKind::Output);
        ImGui::PopStyleVar();
        pop_ui_font();
    }

private:
    static constexpr ImVec4 faint_colour{0.431f, 0.396f, 0.341f, 1.0f};
    static constexpr ImVec4 hovered_row_colour{0.902f, 0.957f, 0.937f, 1.0f};
    static constexpr ImVec4 pressed_row_colour{0.839f, 0.918f, 0.886f, 1.0f};
    static constexpr ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
    static constexpr float row_rounding = 6.0f;
    static constexpr ImVec2 row_padding{8.0f, 7.0f};
    static constexpr float gap_between_rows = 2.0f;
    // How far right of the panel's padding every text starts, so that a row's highlight has room
    // left of its name.
    static constexpr float text_inset = 6.0f;
    // An icon is shown in the size of the grid it is drawn on, where its strokes are sharpest.
    static constexpr float icon_size = 24.0f;
    static constexpr float gap_after_icon = 8.0f;
    static constexpr float gap_above_group = 14.0f;
    static constexpr float details_indent = 14.0f;
    static constexpr float gap_above_details = 4.0f;
    static constexpr float gap_under_details = 10.0f;
    static constexpr float gap_above_details_label = 10.0f;
    static constexpr float gap_above_entry = 4.0f;
    static constexpr float half = 0.5f;

    static void move_cursor_to_text_inset() {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + text_inset);
    }

    // The panel's name and, fainter, how many processors Falcon was built with.
    static void render_title() {
        constexpr float gap_before_count = 8.0f;
        move_cursor_to_text_inset();
        render_styled_text("Processors", PANEL_TEXT_SIZE, FontStyle::Bold, TEXT_COLOUR);
        ImGui::SameLine(0.0f, gap_before_count);
        const std::string count = std::format("{}", ALL_PROCESSORS.size());
        render_styled_text(count.c_str(), PANEL_TEXT_SIZE, FontStyle::Regular, faint_colour);
    }

    // The processors of one kind under its title, or nothing when Falcon has none of that kind.
    static void render_group(const char* title, ProcessorKind kind) {
        bool is_title_shown = false;
        for (std::size_t processor_index = 0; processor_index < ALL_PROCESSORS.size();
             ++processor_index) {
            const ProcessorDescription& processor = ALL_PROCESSORS.at(processor_index);
            if (processor_kind(processor) != kind) {
                continue;
            }
            if (!is_title_shown) {
                ImGui::Dummy(ImVec2(0.0f, gap_above_group));
                move_cursor_to_text_inset();
                render_styled_text(title, PANEL_NOTE_SIZE, FontStyle::Regular, faint_colour);
                is_title_shown = true;
            }
            render_processor(processor, processor_index);
        }
    }

    static void render_processor(const ProcessorDescription& processor,
                                 std::size_t processor_index) {
        ImGui::PushID(processor.name);
        const float text_left = ImGui::GetCursorScreenPos().x + text_inset;
        // ImGui's header takes the row's clicks and keys and draws its highlight under the mouse.
        // Its own label and triangle are left out: the name and a chevron are drawn over it.
        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered_row_colour);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, pressed_row_colour);
        ImGui::PushStyleColor(ImGuiCol_Text, transparent);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, row_rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, row_padding);
        const bool is_open = ImGui::CollapsingHeader("##row");
        name_last_item_for_ui_driver({"panel", processor.name});
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        draw_icon_name_and_chevron_on_last_row(processor, text_left, is_open);
        render_drag_source(processor, processor_index);
        if (is_open) {
            render_details(processor, text_left);
        }
        ImGui::PopID();
    }

    // The processor's icon and name at the row's left and, at its right, a chevron that points
    // right while the row is closed and down while it is open. A processor without an icon keeps
    // the icon's room, so that the names stand in one column.
    static void draw_icon_name_and_chevron_on_last_row(const ProcessorDescription& processor,
                                                       float text_left, bool is_open) {
        const ImVec2 row_top_left = ImGui::GetItemRectMin();
        const ImVec2 row_bottom_right = ImGui::GetItemRectMax();
        const float row_middle = (row_top_left.y + row_bottom_right.y) * half;
        const float text_top =
            row_top_left.y +
            ((row_bottom_right.y - row_top_left.y - ImGui::GetTextLineHeight()) * half);
        const char* chevron = is_open ? icon("arrow-down-s-line") : icon("arrow-right-s-line");
        const float chevron_left =
            row_bottom_right.x - row_padding.x - ImGui::CalcTextSize(chevron).x;
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_picture(draw_list, icon_of_processor(processor),
                     {
                         .top_left = ImVec2(text_left, row_middle - (icon_size * half)),
                         .size = ImVec2(icon_size, icon_size),
                     },
                     TEXT_COLOUR);
        draw_list->AddText(ImVec2(text_left + icon_size + gap_after_icon, text_top),
                           ImGui::GetColorU32(TEXT_COLOUR), processor.name);
        draw_list->AddText(ImVec2(chevron_left, text_top), ImGui::GetColorU32(faint_colour),
                           chevron);
    }

    // What the processor's source says of it, under its row and indented from its name, with a
    // hairline down the left that ties it to the row.
    static void render_details(const ProcessorDescription& processor, float text_left) {
        // Half a pixel puts a line of one pixel on one column of pixels instead of across two.
        constexpr float half_pixel = 0.5f;
        ImGui::Indent(text_inset + details_indent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                               text_inset);
        ImGui::Dummy(ImVec2(0.0f, gap_above_details));
        const float details_top = ImGui::GetCursorScreenPos().y;
        ImGui::TextUnformatted(processor.description);
        render_styled_text(processor.file, PANEL_NOTE_SIZE, FontStyle::Regular, faint_colour);

        if (!processor.options.empty()) {
            render_details_label("Options");
            for (const OptionDescription& option : processor.options) {
                const std::string value =
                    std::format("{}, {} by default", option.type, option.default_value);
                render_entry({
                    .name = option.name,
                    .explanation = option.description,
                    .what_it_is = value.c_str(),
                });
            }
        }

        render_details_label("Connections");
        for (const PortDescription& port : processor.ports) {
            const std::string summary = port_summary(port);
            render_entry({
                .name = port.name,
                .explanation = port.description,
                .what_it_is = summary.c_str(),
            });
        }
        const float details_bottom = ImGui::GetItemRectMax().y;
        ImGui::Dummy(ImVec2(0.0f, gap_under_details));
        ImGui::PopTextWrapPos();
        ImGui::Unindent(text_inset + details_indent);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(text_left + half_pixel, details_top),
                                            ImVec2(text_left + half_pixel, details_bottom),
                                            ImGui::GetColorU32(CARD_BORDER_COLOUR));
    }

    // The name of a part of the details: the options or the connections.
    static void render_details_label(const char* text) {
        ImGui::Dummy(ImVec2(0.0f, gap_above_details_label));
        render_styled_text(text, PANEL_NOTE_SIZE, FontStyle::Bold, faint_colour);
    }

    // An option or a connection, as the details show it.
    struct Entry {
        const char* name;
        const char* explanation;
        const char* what_it_is;
    };

    // The entry's name, with an info icon that explains it, and under it, fainter, what it is.
    static void render_entry(const Entry& entry) {
        ImGui::Dummy(ImVec2(0.0f, gap_above_entry));
        ImGui::TextUnformatted(entry.name);
        render_info_icon(entry.explanation);
        render_styled_text(entry.what_it_is, PANEL_NOTE_SIZE, FontStyle::Regular, faint_colour);
    }

    // The dragged processor's name follows the mouse in a tooltip of ImGui's own.
    static void render_drag_source(const ProcessorDescription& processor,
                                   std::size_t processor_index) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BACKGROUND);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLTIP_PADDING);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(PROCESSOR_TYPE_PAYLOAD, &processor_index,
                                      sizeof(processor_index));
            push_ui_font(TOOLTIP_TEXT_SIZE, FontStyle::Regular);
            ImGui::TextUnformatted(processor.name);
            pop_ui_font();
            ImGui::EndDragDropSource();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
};
