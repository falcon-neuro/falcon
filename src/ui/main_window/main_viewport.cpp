#pragma once
#include <imgui.h>
#include <functional>

class MainViewport {
   private:
    std::function<void()> m_onShowLeft;
    std::function<void()> m_onShowBottom;

   public:
    MainViewport(std::function<void()> onShowLeft, std::function<void()> onShowBottom)
        : m_onShowLeft(std::move(onShowLeft)), m_onShowBottom(std::move(onShowBottom)) {}
    ~MainViewport() = default;

    void render(float width, float height, bool leftSidebarVisible, bool bottomBarVisible ) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
        ImGui::BeginChild("MainViewportContainer", ImVec2(width, height), true);

        bool printedButton = false;
        if (!leftSidebarVisible && m_onShowLeft) {
            if (ImGui::Button("Reveal Left Panel")) {
                m_onShowLeft();
            }
            printedButton = true;
        }

        if (!bottomBarVisible && m_onShowBottom) {
            if (printedButton) ImGui::SameLine();
            if (ImGui::Button("Reveal Status Panel")) {
                m_onShowBottom();
            }
        }

        ImVec2 size = ImGui::GetWindowSize();
        const char* text = "Main Area";
        ImVec2 text_size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((size.x - text_size.x) * 0.5f, (size.y - text_size.y) * 0.5f));
        ImGui::TextUnformatted(text);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
};
