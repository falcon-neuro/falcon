#pragma once
#include <immintrin.h>
#include <atomic>
#include <iostream>
#include <vector>
#include "glad/gl.h"

const char* COMPUTE_SHADER_SRC = R"glsl(
#version 460 core
layout(local_size_x = 256, local_size_y = 1) in;

layout(std430, binding = 0) readonly buffer RawSignal {
    float samples[]; // Row-major: [channel * samples_history + sample_idx]
};

layout(rgba8, binding = 0) uniform writeonly image2D u_render_target;

uniform int u_total_samples;
uniform int u_cursor_idx;
uniform float u_v_scale;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int c = int(gl_GlobalInvocationID.y);

    if (x >= 4096 || c >= 384) return;

    int history_stride = u_total_samples;
    float samples_per_pixel = float(u_total_samples) / 4096.0;
    int start_idx = int(float(x) * samples_per_pixel);
    int end_idx = int(float(x + 1) * samples_per_pixel);
    start_idx = clamp(start_idx, 0, u_total_samples - 1);
    end_idx = clamp(end_idx, start_idx + 1, u_total_samples);

    float min_val = 1e10;
    float max_val = -1e10;
    bool near_cursor = false;

    int channel_offset = c * history_stride;

    for (int i = start_idx; i < end_idx; ++i) {
        float v = samples[channel_offset + i];
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

    int image_y_start = c * 60;

    for (int y = 0; y < 60; ++y) {
        vec4 color = vec4(0.06f, 0.06f, 0.08f, 1.0f);
        if (near_cursor && abs(x - int((float(u_cursor_idx) / u_total_samples) * 4096.0)) < 4) {
            color = vec4(0.0f, 0.0f, 0.0f, 0.0f);
        } else if (y >= y_min && y <= y_max) {
            color = vec4(0.0f, 1.0f, 0.0f, 1.0f);
        }
        imageStore(u_render_target, ivec2(x, image_y_start + y), color);
    }
}
)glsl";

class SignalShaders {
   public:
    static bool check_compile_errors(GLuint shader, const std::string& type) {
        GLint success;
        if (type != "PROGRAM") {
            glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
            if (!success) {
                GLint log_length = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
                std::vector<GLchar> infoLog(static_cast<std::size_t>(log_length));
                glGetShaderInfoLog(shader, log_length, NULL, infoLog.data());
                std::cerr << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n"
                          << infoLog.data() << "\n";
                return false;
            }
        } else {
            glGetProgramiv(shader, GL_LINK_STATUS, &success);
            if (!success) {
                GLint log_length = 0;
                glGetProgramiv(shader, GL_INFO_LOG_LENGTH, &log_length);
                std::vector<GLchar> infoLog(static_cast<std::size_t>(log_length));
                glGetProgramInfoLog(shader, log_length, NULL, infoLog.data());
                std::cerr << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n"
                          << infoLog.data() << "\n";
                return false;
            }
        }
        return true;
    }

    static GLuint compile_compute_program() {
        GLuint compute_shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(compute_shader, 1, &COMPUTE_SHADER_SRC, NULL);
        glCompileShader(compute_shader);

        if (!check_compile_errors(compute_shader, "COMPUTE")) {
            glDeleteShader(compute_shader);
            return 0;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, compute_shader);
        glDeleteShader(compute_shader);

        glLinkProgram(program);
        if (!check_compile_errors(program, "PROGRAM")) {
            glDeleteProgram(program);
            return 0;
        }

        return program;
    }
};

class SignalRenderer {
   private:
    GLuint compute_program_ = 0;
    GLuint ssbo_raw_ = 0;
    GLuint render_tex_ = 0;
    std::atomic<float> voltage_scale_{100.0f};

   public:
    SignalRenderer() = default;

    ~SignalRenderer() {
        if (ssbo_raw_) glDeleteBuffers(1, &ssbo_raw_);
        if (render_tex_) glDeleteTextures(1, &render_tex_);
    }

    void set_voltage_scale(float scale) { voltage_scale_.store(scale, std::memory_order_relaxed); }

    float get_voltage_scale() const { return voltage_scale_.load(std::memory_order_relaxed); }

    GLuint get_render_texture() const { return render_tex_; }

    void init_gl_resources() {
        compute_program_ = SignalShaders::compile_compute_program();

        glGenBuffers(1, &ssbo_raw_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_raw_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 384 * 10 * 30000 * sizeof(float), nullptr,
                     GL_DYNAMIC_DRAW);

        glGenTextures(1, &render_tex_);
        glBindTexture(GL_TEXTURE_2D, render_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4096, 23040, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void execute_downsample(const std::vector<float>& buffer, size_t history_size,
                            size_t cursor_idx) {
        size_t total_elements = buffer.size();
        if (total_elements > 0) [[likely]] {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_raw_);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            static_cast<GLsizeiptr>(total_elements * sizeof(float)), buffer.data());
        }

        glUseProgram(compute_program_);
        glUniform1i(glGetUniformLocation(compute_program_, "u_total_samples"),
                    static_cast<int>(history_size));
        glUniform1i(glGetUniformLocation(compute_program_, "u_cursor_idx"),
                    static_cast<int>(cursor_idx));
        glUniform1f(glGetUniformLocation(compute_program_, "u_v_scale"),
                    voltage_scale_.load(std::memory_order_relaxed));

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_raw_);
        glBindImageTexture(0, render_tex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        glDispatchCompute(16, 384, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
};
