#pragma once
#include <imgui.h>
#include <cstddef>
#include <format>
#include <string>
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

// Lists every processor Falcon was built with, each with what its own source says of it: its
// description, its options and its connections. The "(i)" beside an entry shows its description.
// A processor is dragged from here into a core of the pipeline designer.
class ProcessorsPanel {
public:
    static void render() {
        const std::string title = std::format("Processors ({})", ALL_PROCESSORS.size());
        ImGui::TextUnformatted(title.c_str());
        ImGui::Separator();

        for (std::size_t processor_index = 0; processor_index < ALL_PROCESSORS.size();
             ++processor_index) {
            const ProcessorDescription& processor = ALL_PROCESSORS.at(processor_index);
            const bool is_open = ImGui::CollapsingHeader(processor.name);
            render_drag_source(processor, processor_index);
            if (!is_open) {
                continue;
            }
            ImGui::PushID(processor.name);
            ImGui::Indent();
            render_wrapped(processor.description);
            render_faint(processor.file);

            if (!processor.options.empty()) {
                ImGui::SeparatorText("Options");
                for (const OptionDescription& option : processor.options) {
                    ImGui::TextUnformatted(option.name);
                    render_info(option.description);
                    const std::string value =
                        std::format("{}, {} by default", option.type, option.default_value);
                    render_faint(value.c_str());
                }
            }

            ImGui::SeparatorText("Connections");
            for (const PortDescription& port : processor.ports) {
                ImGui::TextUnformatted(port.name);
                render_info(port.description);
                render_faint(port_summary(port).c_str());
            }
            ImGui::Unindent();
            ImGui::Spacing();
            ImGui::PopID();
        }
    }

private:
    static constexpr ImVec4 faint_colour{0.45f, 0.45f, 0.45f, 1.0f};

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

    static void render_info(const char* description) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted("(i)");
        ImGui::PopStyleColor();
        render_item_tooltip(description);
    }
};
