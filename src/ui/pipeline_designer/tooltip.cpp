#pragma once
#include <imgui.h>

// A tooltip for the item drawn last, shown while the mouse is over it. The app's style has black
// text and no padding, so a tooltip brings its own light background and padding.

inline constexpr ImVec4 TOOLTIP_BACKGROUND{1.0f, 0.988f, 0.957f, 1.0f};
inline constexpr ImVec2 TOOLTIP_PADDING{10.0f, 8.0f};

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
