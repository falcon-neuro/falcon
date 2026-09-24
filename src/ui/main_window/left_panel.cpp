#pragma once
#include <imgui.h>
#include <functional>
#include "../common/fonts.cpp"

class LeftPanel {
   private:
    std::function<void()> m_onClose;

   public:
    explicit LeftPanel(std::function<void()> onClose) : m_onClose(std::move(onClose)) {}
    ~LeftPanel() = default;

    void render(float width, float height) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, (ImVec4) ImColor(0xffffffff));
        ImGui::BeginChild("LeftPanelContainer", ImVec2(width, height), true);

        if (ImGui::Button("X##CloseLeft") && m_onClose) {
            m_onClose();
        }

        ImVec2 size = ImGui::GetWindowSize();
        const char* text = "Left Panel";

        PushUIFont(32, FontStyle::Bold);

        ImVec2 text_size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((size.x - text_size.x) * 0.5f, (size.y - text_size.y) * 0.5f));
        ImGui::TextUnformatted(text);

        PopUIFont();

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
};