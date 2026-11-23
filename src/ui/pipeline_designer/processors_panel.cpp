#pragma once
#include <imgui.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include "../ui_driver/ui_driver_targets.cpp"
#include "all_processors.cpp"
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
// of it: its description, its options and its connections. The info icon beside an entry shows
// its description. A processor is dragged from here into a core of the pipeline designer.
class ProcessorsPanel {
public:
    static void render() {
        const std::string title = std::format("Processors ({})", ALL_PROCESSORS.size());
        ImGui::TextUnformatted(title.c_str());
        ImGui::Spacing();
        render_group("Inputs", ProcessorKind::Input);
        render_group("Operators", ProcessorKind::Operator);
        render_group("Outputs", ProcessorKind::Output);
    }

private:
    static constexpr ImVec4 faint_colour{0.431f, 0.396f, 0.341f, 1.0f};
    static constexpr ImVec4 hovered_row_colour{0.902f, 0.957f, 0.937f, 1.0f};
    static constexpr ImVec4 pressed_row_colour{0.839f, 0.918f, 0.886f, 1.0f};
    static constexpr float row_rounding = 6.0f;

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
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
                ImGui::TextUnformatted(title);
                ImGui::PopStyleColor();
                is_title_shown = true;
            }
            render_processor(processor, processor_index);
        }
        if (is_title_shown) {
            ImGui::Spacing();
        }
    }

    static void render_processor(const ProcessorDescription& processor,
                                 std::size_t processor_index) {
        // A plain row that lights up softly under the mouse.
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered_row_colour);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, pressed_row_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, row_rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
        const bool is_open = ImGui::CollapsingHeader(processor.name);
        name_last_item_for_ui_driver({"panel", processor.name});
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
        render_drag_source(processor, processor_index);
        if (!is_open) {
            return;
        }
        ImGui::PushID(processor.name);
        ImGui::Indent();
        render_wrapped(processor.description);
        render_faint(processor.file);

        if (!processor.options.empty()) {
            ImGui::SeparatorText("Options");
            for (const OptionDescription& option : processor.options) {
                ImGui::TextUnformatted(option.name);
                render_info_icon(option.description);
                const std::string value =
                    std::format("{}, {} by default", option.type, option.default_value);
                render_faint(value.c_str());
            }
        }

        ImGui::SeparatorText("Connections");
        for (const PortDescription& port : processor.ports) {
            ImGui::TextUnformatted(port.name);
            render_info_icon(port.description);
            render_faint(port_summary(port).c_str());
        }
        ImGui::Unindent();
        ImGui::Spacing();
        ImGui::PopID();
    }

    // The dragged processor's name follows the mouse in a tooltip of ImGui's own.
    static void render_drag_source(const ProcessorDescription& processor,
                                   std::size_t processor_index) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BACKGROUND);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLTIP_PADDING);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(PROCESSOR_TYPE_PAYLOAD, &processor_index,
                                      sizeof(processor_index));
            ImGui::TextUnformatted(processor.name);
            ImGui::EndDragDropSource();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    static void render_wrapped(const char* text) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }

    static void render_faint(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        render_wrapped(text);
        ImGui::PopStyleColor();
    }
};
