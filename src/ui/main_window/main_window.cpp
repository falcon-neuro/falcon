#pragma once
#include <imgui.h>
#include <functional>
#include "bottom_panel.cpp"
#include "left_panel.cpp"
#include "main_viewport.cpp"
#include "main_window_controller.cpp"
#include "processors_panel.cpp"

#include "../signal_viewer/signal_viewer.cpp"

class MainWindow {
private:
    MainWindowController& main_window_controller_;
    LeftPanel left_panel_;
    BottomPanel bottom_panel_;
    MainViewport main_viewport_;
    SignalViewer signal_viewer_;
    SignalParser signal_parser_;
    bool is_initialized_ = false;

public:
    MainWindow(MainWindowController& main_window_controller)
        : main_window_controller_(main_window_controller),
          left_panel_([this] { main_window_controller_.set_left_sidebar_visible(false); }),
          bottom_panel_([this] { main_window_controller_.set_bottom_bar_visible(false); }),
          main_viewport_([this] { main_window_controller_.set_left_sidebar_visible(true); },
                         [this] { main_window_controller_.set_bottom_bar_visible(true); }),
          signal_viewer_(signal_parser_) {}

    ~MainWindow() { signal_parser_.stop(); }

    void init_opengl_resources() {
        if (is_initialized_) {
            return;
        }

        signal_parser_.start();
        is_initialized_ = true;
    }

    void render() {
        if (!is_initialized_) {
            init_opengl_resources();
        }

        ImVec2 space = ImGui::GetContentRegionAvail();
        constexpr float processors_panel_width = 380.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("ProcessorsPanel", ImVec2(processors_panel_width, space.y),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ProcessorsPanel::render();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, 0.0f);

        ImGui::BeginGroup();
        {
            ImGui::BeginChild("MainViewportContainer",
                              ImVec2(space.x - processors_panel_width, space.y),
                              ImGuiChildFlags_Borders);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
            signal_viewer_.render();
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }
        ImGui::EndGroup();
    }
};
