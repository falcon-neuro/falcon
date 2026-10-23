#pragma once
#include <immintrin.h>
#include <atomic>
#include <iostream>
#include <vector>
#include "glad/gl.h"

#define GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX 0x9049
#define GL_VBO_FREE_MEMORY_ATI 0x87FB
#include <iostream>
#include <string>

const char* DEMUX_SHADER_NV = R"glsl(
#version 460 core
#extension GL_NV_gpu_shader5 : enable
layout(local_size_x = 12, local_size_y = 1) in;
layout(std430, binding = 0) readonly buffer RawInputPackets { int16_t raw_ap_data[]; };
layout(std430, binding = 1) writeonly buffer RawSignal { float samples[]; };
uniform int u_samples_history; uniform int u_write_idx; uniform int u_num_superframes;
void main() {
    int sf_idx = int(gl_WorkGroupID.x); int sample_in_sf = int(gl_LocalInvocationID.x); int c = int(gl_WorkGroupID.y);
    if (sf_idx >= u_num_superframes || sample_in_sf >= 12 || c >= 384) return;
    int total_sample_offset = sf_idx * 12 + sample_in_sf;
    int target_history_idx = (u_write_idx + total_sample_offset) % u_samples_history;
    int raw_packet_stride = 12 * 384;
    int raw_idx = (sf_idx * raw_packet_stride) + (sample_in_sf * 384) + c;
    float val = float(raw_ap_data[raw_idx]);
    int channel_offset = c * u_samples_history;
    samples[channel_offset + target_history_idx] = val;
}
)glsl";

const char* DEMUX_SHADER_GENERIC = R"glsl(
#version 460 core
layout(local_size_x = 12, local_size_y = 1) in;
layout(std430, binding = 0) readonly buffer RawInputPackets { int raw_ap_data[]; };
layout(std430, binding = 1) writeonly buffer RawSignal { float samples[]; };
uniform int u_samples_history; uniform int u_write_idx; uniform int u_num_superframes;
void main() {
    int sf_idx = int(gl_WorkGroupID.x); int sample_in_sf = int(gl_LocalInvocationID.x); int c = int(gl_WorkGroupID.y);
    if (sf_idx >= u_num_superframes || sample_in_sf >= 12 || c >= 384) return;
    int total_sample_offset = sf_idx * 12 + sample_in_sf;
    int target_history_idx = (u_write_idx + total_sample_offset) % u_samples_history;
    int raw_packet_stride = 12 * 384;
    int raw_idx = (sf_idx * raw_packet_stride) + (sample_in_sf * 384) + c;
    int word_idx = raw_idx / 2;
    int bit_shift = (raw_idx % 2) * 16;
    int packed_val = raw_ap_data[word_idx];
    int raw_val = (packed_val >> bit_shift) & 0xFFFF;
    if ((raw_val & 0x8000) != 0) { raw_val |= 0xFFFF0000; }
    float val = float(raw_val);
    int channel_offset = c * u_samples_history;
    samples[channel_offset + target_history_idx] = val;
}
)glsl";

const char* get_demux_shader_source() {
    const GLubyte* vendor_ptr = glGetString(GL_VENDOR);
    if (!vendor_ptr) {
        return DEMUX_SHADER_GENERIC;
    }
    std::string vendor(reinterpret_cast<const char*>(vendor_ptr));
    if (vendor.find("NVIDIA") != std::string::npos) {
        return DEMUX_SHADER_NV;
    }
    return DEMUX_SHADER_GENERIC;
}

const char* DOWN_COMPUTE_SHADER_SRC = R"glsl(
#version 460 core
layout(local_size_x = 256, local_size_y = 1) in;

layout(std430, binding = 1) readonly buffer RawSignal {
    float samples[];
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

    static GLuint compile_shader_program(const char* src, GLenum type) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, NULL);
        glCompileShader(shader);
        if (!check_compile_errors(shader, type == GL_COMPUTE_SHADER ? "COMPUTE" : "OTHER")) {
            glDeleteShader(shader);
            return 0;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, shader);
        glDeleteShader(shader);
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
    GLuint demux_program_ = 0;
    GLuint downsample_program_ = 0;
    GLuint ssbo_staging_raw_ = 0;
    GLuint ssbo_signal_history_ = 0;
    GLuint render_tex_ = 0;
    std::atomic<float> voltage_scale_{100.0f};

   public:
    SignalRenderer() = default;

    ~SignalRenderer() {
        if (ssbo_staging_raw_) glDeleteBuffers(1, &ssbo_staging_raw_);
        if (ssbo_signal_history_) glDeleteBuffers(1, &ssbo_signal_history_);
        if (render_tex_) glDeleteTextures(1, &render_tex_);
    }

    void set_voltage_scale(float scale) { voltage_scale_.store(scale, std::memory_order_relaxed); }
    float get_voltage_scale() const { return voltage_scale_.load(std::memory_order_relaxed); }
    GLuint get_render_texture() const { return render_tex_; }

    size_t get_universal_available_vram_kb() {
        const size_t fallback_size = 1024 * 512;
        const GLubyte* vendor_ptr = glGetString(GL_VENDOR);
        const GLubyte* renderer_ptr = glGetString(GL_RENDERER);

        if (!vendor_ptr) return fallback_size;

        std::string vendor(reinterpret_cast<const char*>(vendor_ptr));
        std::string renderer(renderer_ptr ? reinterpret_cast<const char*>(renderer_ptr) : "");

        // Transform to uppercase for comparison consistency
        std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::toupper);
        std::transform(renderer.begin(), renderer.end(), renderer.begin(), ::toupper);

        // 1. NVIDIA Dedicated GPU Tracking
        if (vendor.find("NVIDIA") != std::string::npos) {
            GLint nvidia_available_kb = 0;
            glGetIntegerv(GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX, &nvidia_available_kb);
            if (nvidia_available_kb > 0) {
                return static_cast<size_t>(nvidia_available_kb);
            }
        }

        // 2. AMD / ATI Dedicated GPU Tracking
        if (vendor.find("AMD") != std::string::npos || vendor.find("ATI") != std::string::npos) {
            GLint amd_free_mem_info[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VBO_FREE_MEMORY_ATI, amd_free_mem_info);
            if (amd_free_mem_info[0] > 0) {
                return static_cast<size_t>(amd_free_mem_info[0]);
            }
        }

        // 3. INTEL or Integrated Silicon Fallback
        // Intel integrated GPUs do not have dedicated physical VRAM pools; they dynamically
        // carve space directly out of host system RAM up to an OS-managed maximum limit.
        if (vendor.find("INTEL") != std::string::npos ||
            renderer.find("INTEL") != std::string::npos) {
            return fallback_size;
        }

        return fallback_size;
    }
    void init_gl_resources(size_t initial_history_size) {
        demux_program_ =
            SignalShaders::compile_shader_program(get_demux_shader_source(), GL_COMPUTE_SHADER);
        downsample_program_ =
            SignalShaders::compile_shader_program(DOWN_COMPUTE_SHADER_SRC, GL_COMPUTE_SHADER);

        glGenBuffers(1, &ssbo_staging_raw_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_staging_raw_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4000 * 12 * 384 * sizeof(int16_t), nullptr,
                     GL_DYNAMIC_DRAW);

        glGenBuffers(1, &ssbo_signal_history_);
        resize_history_buffer(initial_history_size);

        glGenTextures(1, &render_tex_);
        glBindTexture(GL_TEXTURE_2D, render_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4096, 23040, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void resize_history_buffer(size_t history_size) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_signal_history_);

        GLsizeiptr buffer_size = static_cast<GLsizeiptr>(384 * history_size * sizeof(float));

        glBufferData(GL_SHADER_STORAGE_BUFFER, buffer_size, nullptr, GL_DYNAMIC_DRAW);

        float clear_value = 0.0f;
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, &clear_value);
    }

    void execute_demux_and_downsample(const int16_t* raw_packets_ptr, size_t total_elements,
                                      size_t num_superframes, size_t history_size,
                                      size_t current_write_idx, size_t total_processed) {
        if (num_superframes > 0) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_staging_raw_);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            static_cast<long>(total_elements * sizeof(int16_t)), raw_packets_ptr);

            glUseProgram(demux_program_);
            glUniform1i(glGetUniformLocation(demux_program_, "u_samples_history"),
                        static_cast<int>(history_size));
            glUniform1i(glGetUniformLocation(demux_program_, "u_write_idx"),
                        static_cast<int>(current_write_idx));
            glUniform1i(glGetUniformLocation(demux_program_, "u_num_superframes"),
                        static_cast<int>(num_superframes));

            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_staging_raw_);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_signal_history_);

            glDispatchCompute(static_cast<GLuint>(num_superframes), 384, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }

        glUseProgram(downsample_program_);
        glUniform1i(glGetUniformLocation(downsample_program_, "u_total_samples"),
                    static_cast<int>(history_size));

        size_t current_cursor = total_processed % history_size;
        glUniform1i(glGetUniformLocation(downsample_program_, "u_cursor_idx"),
                    static_cast<int>(current_cursor));
        glUniform1f(glGetUniformLocation(downsample_program_, "u_v_scale"),
                    voltage_scale_.load(std::memory_order_relaxed));

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_signal_history_);
        glBindImageTexture(0, render_tex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        glDispatchCompute(16, 384, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
};
