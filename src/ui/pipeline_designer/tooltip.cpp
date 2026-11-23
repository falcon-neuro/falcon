#pragma once
#include <imgui.h>
#include "../common/colours.cpp"
#include "../common/fonts.cpp"
#include "../common/icons.cpp"

// What the UI says of a thing beyond its label. The app's style has black text and no padding,
// so a tooltip brings its own light background and padding.

inline constexpr ImVec4 TOOLTIP_BACKGROUND{1.0f, 0.988f, 0.957f, 1.0f};
inline constexpr ImVec2 TOOLTIP_PADDING{10.0f, 8.0f};
// A tooltip's text has this size whatever the size of the text it is shown over.
inline constexpr int TOOLTIP_TEXT_SIZE = 16;

// A tooltip for the item drawn last, shown while the mouse is over it.
inline void render_item_tooltip(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BACKGROUND);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLTIP_PADDING);
    if (ImGui::BeginItemTooltip()) {
        constexpr float tooltip_width_in_font_sizes = 28.0f;
        push_ui_font(TOOLTIP_TEXT_SIZE, FontStyle::Regular);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * tooltip_width_in_font_sizes);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        pop_ui_font();
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
