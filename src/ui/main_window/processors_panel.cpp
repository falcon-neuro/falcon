#pragma once
#include <imgui.h>
#include <format>
#include <string>
#include "all_processors.cpp"

// Lists every processor Falcon was built with, each with what its own source says of it: its
// description, its options and its connections. The "(i)" beside an entry shows its description.
class ProcessorsPanel {
public:
    static void render() {
        const std::string title = std::format("Processors ({})", ALL_PROCESSORS.size());
        ImGui::TextUnformatted(title.c_str());
        ImGui::Separator();

        for (const ProcessorDescription& processor : ALL_PROCESSORS) {
            if (!ImGui::CollapsingHeader(processor.name)) {
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
                const std::string kind = std::format(
                    "{} of {}{}{}", direction_name(port.direction), kind_name(port.kind),
                    port.has_slots ? ", any number" : "", port.is_optional ? ", optional" : "");
                render_faint(kind.c_str());
            }
            ImGui::Unindent();
            ImGui::Spacing();
            ImGui::PopID();
        }
    }

private:
    static constexpr ImVec4 faint_colour{0.45f, 0.45f, 0.45f, 1.0f};
    static constexpr ImVec4 tooltip_background{1.0f, 1.0f, 0.92f, 1.0f};

    [[nodiscard]] static const char* direction_name(PortDirection direction) {
        return direction == PortDirection::Input ? "input" : "output";
    }

    [[nodiscard]] static const char* kind_name(PortKind kind) {
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

    // The app's style has black text and no padding, so the tooltip brings its own light
    // background and padding.
    static void render_info(const char* description) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted("(i)");
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_PopupBg, tooltip_background);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        if (ImGui::BeginItemTooltip()) {
            constexpr float tooltip_width_in_font_sizes = 28.0f;
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * tooltip_width_in_font_sizes);
            ImGui::TextUnformatted(description);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
};
