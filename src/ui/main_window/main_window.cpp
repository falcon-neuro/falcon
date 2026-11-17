#pragma once
#include <imgui.h>
#include <functional>
#include "bottom_panel.cpp"
#include "left_panel.cpp"
#include "main_viewport.cpp"
#include "main_window_controller.cpp"

#include "../pipeline_designer/pipeline_designer.cpp"

class MainWindow {
private:
    MainWindowController& main_window_controller_;
    LeftPanel left_panel_;
    BottomPanel bottom_panel_;
    MainViewport main_viewport_;
    PipelineDesigner pipeline_designer_;

public:
    MainWindow(MainWindowController& main_window_controller)
        : main_window_controller_(main_window_controller),
          left_panel_([this] { main_window_controller_.set_left_sidebar_visible(false); }),
          bottom_panel_([this] { main_window_controller_.set_bottom_bar_visible(false); }),
          main_viewport_([this] { main_window_controller_.set_left_sidebar_visible(true); },
                         [this] { main_window_controller_.set_bottom_bar_visible(true); }) {}

    void render() { pipeline_designer_.render(); }
};
