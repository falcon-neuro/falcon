#pragma once
class MainWindowController {
   private:
    bool show_left_sidebar_ = true;
    bool show_bottom_bar_ = true;

   public:
    MainWindowController() = default;
    ~MainWindowController() = default;

    [[nodiscard]] bool is_left_sidebar_visible() const { return show_left_sidebar_; }
    [[nodiscard]] bool is_bottom_bar_visible() const { return show_bottom_bar_; }

    void toggle_left_sidebar() { show_left_sidebar_ = !show_left_sidebar_; }
    void toggle_bottom_bar() { show_bottom_bar_ = !show_bottom_bar_; }

    void set_left_sidebar_visible(bool visible) { show_left_sidebar_ = visible; }
    void set_bottom_bar_visible(bool visible) { show_bottom_bar_ = visible; }
};
