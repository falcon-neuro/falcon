#pragma once
class MainWindowController {
private:
    bool is_left_sidebar_visible_ = true;
    bool is_bottom_bar_visible_ = true;

public:
    MainWindowController() = default;
    ~MainWindowController() = default;

    [[nodiscard]] bool is_left_sidebar_visible() const { return is_left_sidebar_visible_; }
    [[nodiscard]] bool is_bottom_bar_visible() const { return is_bottom_bar_visible_; }

    void toggle_left_sidebar() { is_left_sidebar_visible_ = !is_left_sidebar_visible_; }
    void toggle_bottom_bar() { is_bottom_bar_visible_ = !is_bottom_bar_visible_; }

    void set_left_sidebar_visible(bool is_visible) { is_left_sidebar_visible_ = is_visible; }
    void set_bottom_bar_visible(bool is_visible) { is_bottom_bar_visible_ = is_visible; }
};
