#pragma once
#include <imgui.h>
#include "colours.cpp"

// A soft shadow around a child window, a little lower than it, as if it lay above what is around
// it: begin_shadow_under_next_child() before the child begins, end_shadow_under_last_child()
// after it ends. ImGui draws a child's background into the draw list of the window around it, so a
// shadow added to that list after the child would lie over the background. The two calls split
// that list in two layers, the child's background in the upper one and the shadow in the lower.
inline void begin_shadow_under_next_child() {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(1);
}

inline void end_shadow_under_last_child(float rounding) {
    constexpr int layer_count = 12;
    constexpr float layer_step = 1.0f;
    constexpr float drop = 3.0f;
    const ImVec2 top_left = ImGui::GetItemRectMin();
    const ImVec2 bottom_right = ImGui::GetItemRectMax();
    const ImU32 layer_colour = ImGui::GetColorU32(SHADOW_LAYER_COLOUR);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->ChannelsSetCurrent(0);
    for (int layer = 1; layer <= layer_count; ++layer) {
        const float spread = static_cast<float>(layer) * layer_step;
        draw_list->AddRectFilled(ImVec2(top_left.x - spread, top_left.y - spread + drop),
                                 ImVec2(bottom_right.x + spread, bottom_right.y + spread + drop),
                                 layer_colour, rounding + spread);
    }
    draw_list->ChannelsMerge();
}
