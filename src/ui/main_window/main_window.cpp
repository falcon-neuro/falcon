#pragma once
#include <imgui.h>
#include <functional>
#include "../signal_viewer/signal_viewer.cpp"
#include "bottom_panel.cpp"
#include "left_panel.cpp"
#include "main_viewport.cpp"
#include "main_window_controller.cpp"
class MainWindow {
   private:
    MainWindowController& main_window_controller_;
    LeftPanel m_leftPanel;
    BottomPanel m_bottomPanel;
    MainViewport m_mainViewport;
    SignalViewer signal_viewer_;
    SignalParser signal_parser;

   public:
    MainWindow(MainWindowController& main_window_controller)
        : main_window_controller_(main_window_controller),
          m_leftPanel([this]() { main_window_controller_.setLeftSidebarVisible(false); }),
          m_bottomPanel([this]() { main_window_controller_.setBottomBarVisible(false); }),
          m_mainViewport([this]() { main_window_controller_.setLeftSidebarVisible(true); },
                         [this]() { main_window_controller_.setBottomBarVisible(true); }),
          signal_viewer_(signal_parser) {
        signal_parser.start();
    }

    ~MainWindow() { signal_parser.stop(); }

    void render() {
        ImVec2 space = ImGui::GetContentRegionAvail();

        ImGui::BeginGroup();
        {
            ImGui::BeginChild("MainViewportContainer", ImVec2(space.x, space.y), true);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
            signal_viewer_.render();
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }
        ImGui::EndGroup();
    }
};
