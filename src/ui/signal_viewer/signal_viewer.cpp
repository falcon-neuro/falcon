#pragma once
#include <algorithm>
#include <iostream>
#include <string>
#include "glad/gl.h"
#include "imgui.h"
#include "signal_parser.cpp"

class SignalViewer {
   private:
    SignalParser& signal_parser_;
    size_t ingestion_rate_ = 0;
    size_t available_vram_kb = 0;

    float ui_duration_seconds_ = 1;
    float ui_voltage_scale_ = 100.0f;

   public:
    SignalViewer(SignalParser& signal_parser) : signal_parser_(signal_parser) {}

    void render() {
        signal_parser_.calculate_rate(ingestion_rate_);
        signal_parser_.calculate_available_vram_kb(available_vram_kb);

        auto fps = static_cast<size_t>(ImGui::GetIO().Framerate);

        ImGui::Text("UI FPS: %.1zu | Ingest Rate: %.1zu Hz", fps, ingestion_rate_);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(260.0f);
        char format_buf[128];

        size_t bytes_per_history_step = 384 * sizeof(float);
        size_t budget_bytes = static_cast<size_t>(available_vram_kb) * 1024 / 4;
        float vram_max_seconds =
            static_cast<float>(budget_bytes / bytes_per_history_step) / 30000.0f;
        float absolute_max_seconds = std::min(30.0f, vram_max_seconds);

        if (ui_duration_seconds_ > absolute_max_seconds) {
            ui_duration_seconds_ = absolute_max_seconds;
            signal_parser_.update_history_duration(ui_duration_seconds_);
        }

        double total_samples = static_cast<double>(ui_duration_seconds_ * 30000.0f);
        double ram_bytes = 384.0 * total_samples * sizeof(float);
        double ram_mb = ram_bytes / (1024.0 * 1024.0);

        snprintf(format_buf, sizeof(format_buf), "%.4f sec - %.f smp - %.1f MB",
                 static_cast<double>(ui_duration_seconds_), total_samples, ram_mb);

        if (ImGui::SliderFloat("##Duration", &ui_duration_seconds_, 0.0001f, absolute_max_seconds,
                               format_buf, ImGuiSliderFlags_NoRoundToFormat)) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            signal_parser_.update_history_duration(ui_duration_seconds_);
        }

        ImGui::SameLine();

        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::SliderFloat("##Scale", &ui_voltage_scale_, 10.0f, 1000.0f, "Scale: %.1f uV")) {
            signal_parser_.set_voltage_scale(ui_voltage_scale_);
        }

        int pixel_width = static_cast<int>(ImGui::GetContentRegionAvail().x);
        if (pixel_width < 10) pixel_width = 10;

        signal_parser_.dispatch_downsample(static_cast<size_t>(pixel_width));

        GLuint render_tex = signal_parser_.get_render_texture();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));

        for (int c = 0; c < 384; ++c) {
            std::string child_id = "ChannelChild_" + std::to_string(c);
            if (ImGui::BeginChild(child_id.c_str(), ImVec2(0, 60), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar)) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                ImVec2 p_min = ImGui::GetCursorScreenPos();
                ImVec2 p_max = ImVec2(p_min.x + ImGui::GetContentRegionAvail().x, p_min.y + 60.0f);

                float uv_y0 = (float(c) * 60.0f) / 23040.0f;
                float uv_y1 = (float(c + 1) * 60.0f) / 23040.0f;

                draw_list->AddImage((ImTextureID) (uintptr_t) render_tex, p_min, p_max,
                                    ImVec2(0.0f, uv_y0), ImVec2(1.0f, uv_y1));

                ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "AP Channel %d", c);
            }
            ImGui::EndChild();
        }

        ImGui::PopStyleVar();
    }
};
