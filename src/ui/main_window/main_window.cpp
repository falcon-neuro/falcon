#pragma once
#include <imgui.h>
#include <functional>
#include "bottom_panel.cpp"
#include "left_panel.cpp"
#include "main_viewport.cpp"
#include "main_window_controller.cpp"

#include <iostream>
#include "../signal_viewer/signal_viewer.cpp"

const char* COMPUTE_SHADER_SRC = R"glsl(
#version 460 core
layout(local_size_x = 256) in;

layout(std430, binding = 0) readonly buffer RawSignal {
    float samples[];
};

layout(rgba8, binding = 0) uniform writeonly image2D u_render_target;

uniform int u_total_samples;
uniform int u_cursor_idx;
uniform float u_v_scale;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    if (x >= 4096) return;

    float samples_per_pixel = float(u_total_samples) / 4096.0;
    int start_idx = int(float(x) * samples_per_pixel);
    int end_idx = int(float(x + 1) * samples_per_pixel);
    start_idx = clamp(start_idx, 0, u_total_samples - 1);
    end_idx = clamp(end_idx, start_idx + 1, u_total_samples);

    float min_val = 1e10;
    float max_val = -1e10;
    bool near_cursor = false;

    for (int i = start_idx; i < end_idx; ++i) {
        float v = samples[i];
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
        if (abs(i - u_cursor_idx) < 150) {
            near_cursor = true;
        }
    }

    float n_min = min_val / u_v_scale;
    float n_max = max_val / u_v_scale;

    int y_min = int(clamp((n_min + 1.0) * 0.5 * 60.0, 0.0, 59.0));
    int y_max = int(clamp((n_max + 1.0) * 0.5 * 60.0, 0.0, 59.0));

    for (int y = 0; y < 60; ++y) {
        vec4 color = vec4(0.06f, 0.06f, 0.08f, 1.0f);
        if (near_cursor && abs(x - int((float(u_cursor_idx) / u_total_samples) * 4096.0)) < 4) {
            color = vec4(0.0f, 0.0f, 0.0f, 0.0f);
        } else if (y >= y_min && y <= y_max) {
            color = vec4(0.0f, 1.0f, 0.0f, 1.0f);
        }
        imageStore(u_render_target, ivec2(x, y), color);
    }
}
)glsl";

const char* FRAGMENT_SHADER_SRC = R"glsl(
#version 460 core
out vec4 frag_color;
void main() {
    frag_color = vec4(1.0);
}
)glsl";

const char* VERTEX_SHADER_SRC = R"glsl(
#version 460 core
void main() {
    gl_Position = vec4(0.0);
}
)glsl";

class ShaderCompiler {
   private:
    static bool check_compile_errors(GLuint shader, std::string type) {
        GLint success;
        GLchar infoLog[1024];
        if (type != "PROGRAM") {
            glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
            if (!success) {
                glGetShaderInfoLog(shader, 1024, NULL, infoLog);
                std::cerr << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << "\n";
                return false;
            }
        } else {
            glGetProgramiv(shader, GL_LINK_STATUS, &success);
            if (!success) {
                glGetProgramInfoLog(shader, 1024, NULL, infoLog);
                std::cerr << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n" << infoLog << "\n";
                return false;
            }
        }
        return true;
    }

   public:
    static GLuint compile_compute_program() {
        GLuint compute_shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(compute_shader, 1, &COMPUTE_SHADER_SRC, NULL);
        glCompileShader(compute_shader);
        if (!check_compile_errors(compute_shader, "COMPUTE")) return 0;

        GLuint program = glCreateProgram();
        glAttachShader(program, compute_shader);
        glLinkProgram(program);
        if (!check_compile_errors(program, "PROGRAM")) return 0;

        glDeleteShader(compute_shader);
        return program;
    }

    static GLuint compile_raster_program() {
        GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex_shader, 1, &VERTEX_SHADER_SRC, NULL);
        glCompileShader(vertex_shader);
        if (!check_compile_errors(vertex_shader, "VERTEX")) return 0;

        GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment_shader, 1, &FRAGMENT_SHADER_SRC, NULL);
        glCompileShader(fragment_shader);
        if (!check_compile_errors(fragment_shader, "FRAGMENT")) return 0;

        GLuint program = glCreateProgram();
        glAttachShader(program, vertex_shader);
        glAttachShader(program, fragment_shader);
        glLinkProgram(program);
        if (!check_compile_errors(program, "PROGRAM")) return 0;

        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return program;
    }
};

class MainWindow {
   private:
    MainWindowController& main_window_controller_;
    LeftPanel m_leftPanel;
    BottomPanel m_bottomPanel;
    MainViewport m_mainViewport;
    SignalViewer signal_viewer_;
    SignalParser signal_parser;
    bool is_initialized_ = false;

    int ui_duration_seconds_ = 3;
    float ui_voltage_scale_ = 100.0f;

   public:
    MainWindow(MainWindowController& main_window_controller)
        : main_window_controller_(main_window_controller),
          m_leftPanel([this]() { main_window_controller_.setLeftSidebarVisible(false); }),
          m_bottomPanel([this]() { main_window_controller_.setBottomBarVisible(false); }),
          m_mainViewport([this]() { main_window_controller_.setLeftSidebarVisible(true); },
                         [this]() { main_window_controller_.setBottomBarVisible(true); }),
          signal_viewer_(signal_parser) {}

    ~MainWindow() { signal_parser.stop(); }

    void init_opengl_resources() {
        if (is_initialized_) return;

        GLuint compute_handle = ShaderCompiler::compile_compute_program();
        GLuint raster_handle = ShaderCompiler::compile_raster_program();

        signal_parser.set_compute_program(compute_handle);
        signal_viewer_.set_raster_program(raster_handle);
        signal_parser.init_gl_resources();
        signal_parser.start();
        is_initialized_ = true;
    }

    void render() {
        if (!is_initialized_) {
            init_opengl_resources();
        }

        ImGui::Begin("Controls");
        if (ImGui::SliderInt("Duration (sec)", &ui_duration_seconds_, 1, 10)) {
            signal_parser.update_history_duration(ui_duration_seconds_);
        }
        if (ImGui::SliderFloat("Scale (uV)", &ui_voltage_scale_, 10.0f, 1000.0f, "%.1f")) {
            signal_parser.set_voltage_scale(ui_voltage_scale_);
        }
        ImGui::End();

        ImVec2 space = ImGui::GetContentRegionAvail();

        ImGui::BeginGroup();
        {
            ImGui::BeginChild("MainViewportContainer", ImVec2(space.x, space.y), true);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.08f, 1.0f));
            signal_viewer_.render();
            ImGui::PopStyleColor();
            ImGui::EndChild();
        }
        ImGui::EndGroup();
    }
};
