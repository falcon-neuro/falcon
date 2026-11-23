#pragma once
#include <imgui.h>
#include "../common/colours.cpp"
#include "../common/icons.cpp"

// What the UI says of a thing beyond its label. The app's style has black text and no padding,
// so a tooltip brings its own light background and padding.

inline constexpr ImVec4 TOOLTIP_BACKGROUND{1.0f, 1.0f, 0.92f, 1.0f};
inline constexpr ImVec2 TOOLTIP_PADDING{10.0f, 8.0f};

// A tooltip for the item drawn last, shown while the mouse is over it.
inline void render_item_tooltip(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BACKGROUND);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLTIP_PADDING);
    if (ImGui::BeginItemTooltip()) {
        constexpr float tooltip_width_in_font_sizes = 28.0f;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * tooltip_width_in_font_sizes);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// An info icon on the line of the item drawn last, which shows an explanation while the mouse
// is over it. Explanations show so, over their icon, and never over the thing they explain.
inline void render_info_icon(const char* explanation) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, FAINT_TEXT_COLOUR);
    ImGui::TextUnformatted(icon("information-line"));
    ImGui::PopStyleColor();
    render_item_tooltip(explanation);
}
