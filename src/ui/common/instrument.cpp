#pragma once
#include <imgui.h>
#include <cstdint>
#include "colours.cpp"

// Parts drawn as on an instrument's front: a panel, keys that stand on it, fields set into it and
// a status lamp.

// A colour between two others: share 0 is the first, 1 the second.
[[nodiscard]] inline ImVec4 mixed_colour(const ImVec4& first, const ImVec4& second, float share) {
    return {first.x + ((second.x - first.x) * share), first.y + ((second.y - first.y) * share),
            first.z + ((second.z - first.z) * share), first.w + ((second.w - first.w) * share)};
}

inline constexpr ImVec4 WHITE{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr ImVec4 BLACK{0.0f, 0.0f, 0.0f, 1.0f};

// The colour of a hairline drawn on a ground of this colour: a shade darker than the ground.
[[nodiscard]] inline ImVec4 hairline_colour_on(const ImVec4& ground) {
    constexpr float hairline_share = 0.12f;
    return mixed_colour(ground, BLACK, hairline_share);
}

// Paints the whole of the window being drawn, under what it holds, in this colour, with a
// hairline along its top edge where it meets what lies above it.
inline void paint_window_as_panel(const ImVec4& colour) {
    // Half a pixel puts a line of one pixel on one row of pixels instead of across two.
    constexpr float half_pixel = 0.5f;
    const ImVec2 top_left = ImGui::GetWindowPos();
    const ImVec2 bottom_right(top_left.x + ImGui::GetWindowWidth(),
                              top_left.y + ImGui::GetWindowHeight());
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    // A window's own clip rectangle leaves out the edges of its padding.
    draw_list->PushClipRect(top_left, bottom_right, false);
    draw_list->AddRectFilled(top_left, bottom_right, ImGui::GetColorU32(colour));
    draw_list->AddLine(ImVec2(top_left.x, top_left.y + half_pixel),
                       ImVec2(bottom_right.x, top_left.y + half_pixel),
                       ImGui::GetColorU32(hairline_colour_on(colour)));
    draw_list->PopClipRect();
}

// A side of a window along which it meets the area beside it.
enum class WindowSide : std::uint8_t { Left, Right };

// Makes the window being drawn a side panel that reaches the bars it lies between: paints the
// gap of the given height above and under it in the panel's colour, and a hairline along the
// given side, where the panel meets the area beside it. The panel's own ground is the window's
// background colour, which the caller sets before the window begins: ImGui draws a window's
// scrollbar as the window begins, so a ground painted afterwards would cover it.
inline void draw_edges_of_side_panel(const ImVec4& colour, WindowSide hairline_side,
                                     float gap_to_bars) {
    // Half a pixel puts a line of one pixel on one column of pixels instead of across two.
    constexpr float half_pixel = 0.5f;
    const ImVec2 window_top_left = ImGui::GetWindowPos();
    const ImVec2 window_bottom_right(window_top_left.x + ImGui::GetWindowWidth(),
                                     window_top_left.y + ImGui::GetWindowHeight());
    const ImVec2 top_left(window_top_left.x, window_top_left.y - gap_to_bars);
    const ImVec2 bottom_right(window_bottom_right.x, window_bottom_right.y + gap_to_bars);
    const float hairline_x =
        hairline_side == WindowSide::Left ? top_left.x + half_pixel : bottom_right.x - half_pixel;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(top_left, bottom_right, false);
    draw_list->AddRectFilled(top_left, ImVec2(window_bottom_right.x, window_top_left.y),
                             ImGui::GetColorU32(colour));
    draw_list->AddRectFilled(ImVec2(window_top_left.x, window_bottom_right.y), bottom_right,
                             ImGui::GetColorU32(colour));
    draw_list->AddLine(ImVec2(hairline_x, top_left.y), ImVec2(hairline_x, bottom_right.y),
                       ImGui::GetColorU32(hairline_colour_on(colour)));
    draw_list->PopClipRect();
}

// A status lamp: a small dot of light in a thin recessed ring, with a soft halo when lit. Unlit,
// the dot shows its colour faintly, as an LED that is off.
inline void draw_status_lamp(ImDrawList* draw_list, ImVec2 centre, float radius,
                             const ImVec4& light_colour, bool is_lit) {
    constexpr float ring_gap = 1.5f;
    constexpr float ring_share = 0.18f;
    constexpr float unlit_share = 0.70f;
    constexpr float halo_reach = 2.4f;
    constexpr int halo_layer_count = 10;
    constexpr float halo_strength = 0.07f;
    constexpr float core_share = 0.35f;
    constexpr float core_size = 0.45f;
    const ImVec4 panel_colour = ImGui::GetStyle().Colors[ImGuiCol_ChildBg];
    // The recess the lamp sits in, a shade darker than the panel.
    draw_list->AddCircleFilled(centre, radius + ring_gap,
                               ImGui::GetColorU32(mixed_colour(panel_colour, BLACK, ring_share)));
    if (!is_lit) {
        draw_list->AddCircleFilled(
            centre, radius,
            ImGui::GetColorU32(mixed_colour(light_colour, panel_colour, unlit_share)));
        return;
    }
    for (int layer = halo_layer_count; layer >= 1; --layer) {
        const float share = static_cast<float>(layer) / static_cast<float>(halo_layer_count);
        ImVec4 halo = light_colour;
        halo.w = halo_strength * (1.0f - share);
        draw_list->AddCircleFilled(centre, radius * (1.0f + ((halo_reach - 1.0f) * share)),
                                   ImGui::GetColorU32(halo));
    }
    draw_list->AddCircleFilled(centre, radius, ImGui::GetColorU32(light_colour));
    // Where the light is brightest, a little whiter than the rest of the dot.
    draw_list->AddCircleFilled(centre, radius * core_size,
                               ImGui::GetColorU32(mixed_colour(light_colour, WHITE, core_share)));
}

// Draws the field drawn last as set into the panel: a hairline outline, and a faint shade along
// its upper inner edge.
inline void draw_last_item_as_sunken() {
    constexpr float outline_share = 0.16f;
    constexpr float inner_shade_share = 0.05f;
    constexpr float inner_shade_depth = 1.5f;
    constexpr float inner_shade_width = 2.0f;
    const ImVec2 top_left = ImGui::GetItemRectMin();
    const ImVec2 bottom_right = ImGui::GetItemRectMax();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImVec4 field_colour = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(ImVec2(top_left.x + rounding, top_left.y + inner_shade_depth),
                       ImVec2(bottom_right.x - rounding, top_left.y + inner_shade_depth),
                       ImGui::GetColorU32(mixed_colour(field_colour, BLACK, inner_shade_share)),
                       inner_shade_width);
    draw_list->AddRect(top_left, bottom_right,
                       ImGui::GetColorU32(mixed_colour(field_colour, BLACK, outline_share)),
                       rounding);
}

// A button drawn as a key that stands on the panel, with the same behaviour as ImGui's Button: a
// flat face, a hairline edge and a soft shadow under it, which goes when the key is pushed. The
// key is drawn in a layer under the button's, once ImGui has said whether the mouse is over it or
// pushes it.
inline bool instrument_key(const char* label, ImVec2 size = ImVec2(0.0f, 0.0f)) {
    constexpr float face_share = 0.60f;
    constexpr float hovered_face_share = 0.85f;
    constexpr float pushed_face_share = 0.05f;
    constexpr float edge_share = 0.18f;
    constexpr int shadow_layer_count = 3;
    constexpr float shadow_layer_alpha = 0.05f;
    constexpr float shadow_drop = 1.0f;
    constexpr ImVec4 transparent{0.0f, 0.0f, 0.0f, 0.0f};
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(1);
    ImGui::PushStyleColor(ImGuiCol_Button, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, transparent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, transparent);
    const bool is_clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    draw_list->ChannelsSetCurrent(0);
    const ImVec2 top_left = ImGui::GetItemRectMin();
    const ImVec2 bottom_right = ImGui::GetItemRectMax();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const bool is_pushed = ImGui::IsItemActive();
    const bool is_hovered = ImGui::IsItemHovered();
    if (!is_pushed) {
        ImVec4 shadow = BLACK;
        shadow.w = shadow_layer_alpha;
        for (int layer = 1; layer <= shadow_layer_count; ++layer) {
            const auto spread = static_cast<float>(layer - 1);
            draw_list->AddRectFilled(
                ImVec2(top_left.x - spread, top_left.y - spread + shadow_drop + spread),
                ImVec2(bottom_right.x + spread, bottom_right.y + spread + shadow_drop),
                ImGui::GetColorU32(shadow), rounding + spread);
        }
    }
    ImVec4 face = mixed_colour(KEY_COLOUR, WHITE, is_hovered ? hovered_face_share : face_share);
    if (is_pushed) {
        face = mixed_colour(KEY_COLOUR, BLACK, pushed_face_share);
    }
    draw_list->AddRectFilled(top_left, bottom_right, ImGui::GetColorU32(face), rounding);
    draw_list->AddRect(top_left, bottom_right,
                       ImGui::GetColorU32(mixed_colour(KEY_COLOUR, BLACK, edge_share)), rounding);
    draw_list->ChannelsMerge();
    return is_clicked;
}

// A key as low as a line of text, for a row that is text otherwise.
inline bool instrument_small_key(const char* label) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
    const bool is_clicked = instrument_key(label);
    ImGui::PopStyleVar();
    return is_clicked;
}
