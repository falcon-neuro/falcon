#pragma once
#include "glad/gl.h"
#include "imgui.h"
#include "signal_parser.cpp"

class SignalViewer {
   private:
    SignalParser& signal_parser_;
    size_t ingestion_rate_ = 0;
    GLuint gl_custom_shader_line_program_ = 0;

   public:
    SignalViewer(SignalParser& signal_parser) : signal_parser_(signal_parser) {}
    void set_raster_program(GLuint program_id) { gl_custom_shader_line_program_ = program_id; }

    void render() {
        signal_parser_.calculate_rate(ingestion_rate_);
        auto fps = static_cast<size_t>(ImGui::GetIO().Framerate);
        ImGui::Text("UI FPS: %.1zu | Ingest Rate: %.1zu Hz", fps, ingestion_rate_);

        int pixel_width = static_cast<int>(ImGui::GetContentRegionAvail().x);
        if (pixel_width < 10) pixel_width = 10;

        signal_parser_.dispatch_downsample(static_cast<size_t>(pixel_width));

        GLuint render_tex = signal_parser_.get_render_texture();

        if (ImGui::BeginChild("ChannelChild_0", ImVec2(0, 60), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar)) {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + ImGui::GetContentRegionAvail().x, p_min.y + 60.0f);

            glUseProgram(gl_custom_shader_line_program_);
            glUniform1i(glGetUniformLocation(gl_custom_shader_line_program_, "u_channel_idx"), 0);
            glUniform1f(glGetUniformLocation(gl_custom_shader_line_program_, "u_scale"), 150.0f);

            draw_list->AddImage((ImTextureID) (uintptr_t) render_tex, p_min, p_max);

            ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "AP Channel 0 [GPU]");
        }
        ImGui::EndChild();
    }
};
