#pragma once
#include <imgui.h>
#include <functional>

#include "bottom_panel.cpp"
#include "left_panel.cpp"
#include "main_viewport.cpp"
#include "main_window_controller.cpp"
class MainWindow {
   private:
    bool m_isVisible = false;
    MainWindowController m_controller;
    LeftPanel m_leftPanel;
    BottomPanel m_bottomPanel;
    MainViewport m_mainViewport;

   public:
    MainWindow()
        : m_leftPanel([this]() { m_controller.setLeftSidebarVisible(false); }),
          m_bottomPanel([this]() { m_controller.setBottomBarVisible(false); }),
          m_mainViewport([this]() { m_controller.setLeftSidebarVisible(true); },
                         [this]() { m_controller.setBottomBarVisible(true); }) {}

    ~MainWindow() = default;

    void show() { m_isVisible = true; }
    void hide() { m_isVisible = false; }

    void render() {
        if (!m_isVisible) return;

        ImVec2 space = ImGui::GetContentRegionAvail();

        bool leftVisible = m_controller.isLeftSidebarVisible();
        bool bottomVisible = m_controller.isBottomBarVisible();

        float leftW = leftVisible ? 240.0f : 0.0f;
        float bottomH = bottomVisible ? 160.0f : 0.0f;

        float centerW = space.x - leftW;
        float centerH = space.y - bottomH;

        ImGui::BeginGroup();
        {
            if (leftVisible) {
                m_leftPanel.render(leftW, space.y);
                ImGui::SameLine();
            }

            ImGui::BeginGroup();
            {
                m_mainViewport.render(centerW, centerH, leftVisible, bottomVisible);

                if (bottomVisible) {
                    m_bottomPanel.render(centerW, bottomH);
                }
            }
            ImGui::EndGroup();
        }
        ImGui::EndGroup();
    }
};
