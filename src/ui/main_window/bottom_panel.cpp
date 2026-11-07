#pragma once
#include <imgui.h>
#include <format>
#include <functional>

class BottomPanel {
   private:
    std::function<void()> on_close_;

   public:
    explicit BottomPanel(std::function<void()> on_close) : on_close_(std::move(on_close)) {}
    ~BottomPanel() = default;

    void render() {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.12f, 0.12f, 1.0f));

        if (ImGui::Button("X##CloseBottom") && on_close_) {
            on_close_();
        }

        ImVec2 size = ImGui::GetWindowSize();
        const char* text = "Bottom Panel";
        ImVec2 text_size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((size.x - text_size.x) * 0.5f, (size.y - text_size.y) * 0.5f));
        ImGui::TextUnformatted(text);

        ImGui::PopStyleColor();
    }
};
