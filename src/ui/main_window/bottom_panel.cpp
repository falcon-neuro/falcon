#pragma once
#include <imgui.h>
#include <functional>

class BottomPanel {
private:
    std::function<void()> m_onClose;

public:
    explicit BottomPanel(std::function<void()> onClose) : m_onClose(std::move(onClose)) {}
    ~BottomPanel() = default;

    void render(float width, float height) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.12f, 0.12f, 1.0f));
        ImGui::BeginChild("BottomPanelContainer", ImVec2(width, height), true);

        if (ImGui::Button("X##CloseBottom") && m_onClose) {
            m_onClose();
        }

        ImVec2 size = ImGui::GetWindowSize();
        const char* text = "Bottom Panel";
        ImVec2 text_size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((size.x - text_size.x) * 0.5f, (size.y - text_size.y) * 0.5f));
        ImGui::TextUnformatted(text);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
};
