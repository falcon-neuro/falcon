#pragma once
#include <imgui.h>
#include <cmath>
#include "pictures.cpp"

// Falcon's logo, the falcon, from assets/png/logo.png. The picture is a square with clear room
// above and under the falcon, which is left out here: the logo is the falcon alone.

// How high the logo is when it is this wide, in whole pixels, or 0 when its picture couldn't be
// loaded.
[[nodiscard]] inline float logo_height_at(float width) {
    const Picture& logo = picture_named("logo");
    const float falcon_width = logo.solid_bottom_right.x - logo.solid_top_left.x;
    const float falcon_height = logo.solid_bottom_right.y - logo.solid_top_left.y;
    return falcon_width > 0.0f ? std::round(width * falcon_height / falcon_width) : 0.0f;
}

// Draws the logo this wide, with its top left corner at the given place, in a colour.
inline void draw_logo(ImDrawList* draw_list, ImVec2 top_left, float width, const ImVec4& colour) {
    draw_solid_part_of_picture(draw_list, picture_named("logo"),
                               {.top_left = top_left, .size = ImVec2(width, logo_height_at(width))},
                               colour);
}
