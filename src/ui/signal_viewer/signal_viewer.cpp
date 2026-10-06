#pragma once
#include "imgui.h"
#include "signal_viewer_controller.cpp"
class SignalViewer {
   private:
    SignalViewerController& controller_;

   public:
    SignalViewer(SignalViewerController& controller) : controller_(controller) {}
    void render() {
        auto fps = static_cast<double>(ImGui::GetIO().Framerate);
        controller_.update_benchmarks();

        ImGui::Text("UI FPS: %.1f | Ingest Rate: %.1f Hz", fps, controller_.get_ingestion_rate());

        int pixel_width = static_cast<int>(ImGui::GetContentRegionAvail().x);
        if (pixel_width < 10) pixel_width = 10;

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));

        static std::vector<float> ui_presentation_matrix;
        controller_.sync_presentation_buffer(ui_presentation_matrix,
                                             static_cast<size_t>(pixel_width));

        for (int c = 0; c < NP1_PROBE_CHANNEL_COUNT; ++c) {
            const float* channel_data_ptr = ui_presentation_matrix.data() + (c * pixel_width);

            std::string child_id = "ChannelChild_" + std::to_string(c);
            if (ImGui::BeginChild(child_id.c_str(), ImVec2(0, 60), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar)) {
                std::string plot_id = "##Plot_" + std::to_string(c);
                ImGui::PlotLines(plot_id.c_str(), channel_data_ptr, pixel_width, 0, nullptr,
                                 -150.0f, 150.0f, ImVec2(ImGui::GetContentRegionAvail().x, 60));

                ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "AP Channel %d", c);
            }
            ImGui::EndChild();
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }
};
