#pragma once
#include <algorithm>
#include <cmath>
#include <iostream>
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

        constexpr int channel_count = 384;
        constexpr float row_height = 60.0f;
        constexpr float row_pitch = 62.0f;

        // Only channels inside the window's clip rect are drawn
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 list_start = ImGui::GetCursorScreenPos();
        float visible_top = draw_list->GetClipRectMin().y - list_start.y;
        float visible_bottom = draw_list->GetClipRectMax().y - list_start.y;
        int first = std::clamp(static_cast<int>(visible_top / row_pitch), 0, channel_count);
        int last = std::clamp(static_cast<int>(std::ceil(visible_bottom / row_pitch)), 0,
                              channel_count);

        ImU32 row_bg_color = ImGui::GetColorU32(ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
        for (int c = first; c < last; ++c) {
            float row_y = list_start.y + static_cast<float>(c) * row_pitch;
            draw_list->AddRectFilled(ImVec2(list_start.x, row_y),
                                     ImVec2(list_start.x + static_cast<float>(pixel_width),
                                            row_y + row_height),
                                     row_bg_color);
        }

        if (last > first) {
            ImVec2 origin(list_start.x, list_start.y + static_cast<float>(first) * row_pitch);
            signal_parser_.add_draw_callback(draw_list, first, last - first, origin, row_pitch);
        }

        ImU32 label_color = ImGui::GetColorU32(ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        char label[32];
        for (int c = first; c < last; ++c) {
            snprintf(label, sizeof(label), "AP Channel %d", c);
            draw_list->AddText(
                ImVec2(list_start.x + 8.0f, list_start.y + static_cast<float>(c) * row_pitch + 4.0f),
                label_color, label);
        }

        // Reserves the full scroll height for all channels
        ImGui::Dummy(ImVec2(static_cast<float>(pixel_width),
                            static_cast<float>(channel_count) * row_pitch - (row_pitch - row_height)));
    }
};
