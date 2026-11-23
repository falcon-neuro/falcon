#pragma once
#include <utility>

// The desktop's crosshair as the mouse's cursor. ImGui hands the desktop only cursors of its own
// list (ImGui::SetMouseCursor), which has no crosshair, so a part of the UI asks for it here while
// it draws a frame, and the window's loop gives it to the desktop for the frame after.
class CrosshairCursorRequest {
public:
    // To be called in every frame that wants the crosshair.
    void ask() { is_asked_for_ = true; }

    // Whether it was asked for since this was last called.
    [[nodiscard]] bool take() { return std::exchange(is_asked_for_, false); }

private:
    bool is_asked_for_ = false;
};

// One for the whole program: the UI's parts ask from wherever they are drawn.
[[nodiscard]] inline CrosshairCursorRequest& crosshair_cursor_request() {
    static CrosshairCursorRequest request;
    return request;
}
