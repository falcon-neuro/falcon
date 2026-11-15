#pragma once
#include <imgui.h>
#include <functional>

class MainViewport {
private:
    std::function<void()> on_show_left_;
    std::function<void()> on_show_bottom_;

public:
    MainViewport(std::function<void()> on_show_left, std::function<void()> on_show_bottom)
        : on_show_left_(std::move(on_show_left)), on_show_bottom_(std::move(on_show_bottom)) {}
    ~MainViewport() = default;

    void render(bool is_left_sidebar_visible, bool is_bottom_bar_visible) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));

        bool has_printed_button = false;
        if (!is_left_sidebar_visible && on_show_left_) {
            if (ImGui::Button("Reveal Left Panel")) {
                on_show_left_();
            }
            has_printed_button = true;
        }

        if (!is_bottom_bar_visible && on_show_bottom_) {
            if (has_printed_button) {
                ImGui::SameLine();
            }
            if (ImGui::Button("Reveal Status Panel")) {
                on_show_bottom_();
            }
        }

        ImVec2 size = ImGui::GetWindowSize();
        const char* text = "Main Area";
        ImVec2 text_size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((size.x - text_size.x) * 0.5f, (size.y - text_size.y) * 0.5f));
        ImGui::TextUnformatted(text);

        ImGui::PopStyleColor();
    }
};
