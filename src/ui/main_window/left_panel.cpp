#pragma once
#include <imgui.h>
#include <functional>
#include "../common/fonts.cpp"

class LeftPanel {
   private:
    std::function<void()> on_close_;

   public:
    explicit LeftPanel(std::function<void()> on_close) : on_close_(std::move(on_close)) {}
    ~LeftPanel() = default;

    void render() {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImColor(0xffffffff).Value);
        if (ImGui::Button("X##CloseLeft") && on_close_) {
            on_close_();
        }
        {
            push_ui_font(32, FontStyle::Bold);
            {
                ImVec2 size = ImGui::GetWindowSize();
                const char* text = "Left Panel";
                ImVec2 text_size = ImGui::CalcTextSize(text);
                ImGui::SetCursorPos(
                    ImVec2((size.x - text_size.x) * 0.5f, (size.y - text_size.y) * 0.5f));
                ImGui::TextUnformatted("Left Panel");
            }
            pop_ui_font();
        }
        ImGui::PopStyleColor();
    }
};