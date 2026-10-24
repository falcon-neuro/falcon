#pragma once
#include <immintrin.h>
#include <atomic>
#include <iostream>
#include <vector>
#include "glad/gl.h"
#include "imgui.h"

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

layout(std430, binding = 2) writeonly buffer Envelope {
    vec2 envelope[];
};

uniform int u_total_samples;
uniform int u_cursor_idx;
uniform int u_width;
uniform float u_v_scale;
uniform float u_row_height;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int c = int(gl_GlobalInvocationID.y);

    if (x >= u_width || c >= 384) return;

    int history_stride = u_total_samples;
    float samples_per_pixel = float(u_total_samples) / float(u_width);
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

    float y_min = floor(clamp((n_min + 1.0) * 0.5 * u_row_height, 0.0, u_row_height - 1.0));
    float y_max = floor(clamp((n_max + 1.0) * 0.5 * u_row_height, 0.0, u_row_height - 1.0));

    int cursor_x = int((float(u_cursor_idx) / float(u_total_samples)) * float(u_width));

    // (0, -1) collapses the bar to zero height, leaving the sweep cursor gap empty
    envelope[c * u_width + x] =
        (near_cursor && abs(x - cursor_x) < 4) ? vec2(0.0, -1.0) : vec2(y_min, y_max);
}
)glsl";

// No vertex buffer: gl_VertexID picks the column and quad corner, gl_InstanceID the channel
const char* WAVEFORM_VERTEX_SHADER_SRC = R"glsl(
#version 460 core
layout(std430, binding = 2) readonly buffer Envelope {
    vec2 envelope[];
};

uniform int u_width;
uniform int u_first_channel;
uniform vec2 u_origin;
uniform float u_row_pitch;
uniform vec2 u_display_pos;
uniform vec2 u_display_size;

const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                                vec2(0, 1), vec2(1, 0), vec2(1, 1));

void main() {
    int col = gl_VertexID / 6;
    vec2 q = corners[gl_VertexID % 6];
    vec2 e = envelope[(u_first_channel + gl_InstanceID) * u_width + col];

    float x = u_origin.x + float(col) + q.x;
    float y = u_origin.y + float(gl_InstanceID) * u_row_pitch + mix(e.x, e.y + 1.0, q.y);

    vec2 ndc = (vec2(x, y) - u_display_pos) / u_display_size * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)glsl";

const char* WAVEFORM_FRAGMENT_SHADER_SRC = R"glsl(
#version 460 core
out vec4 frag_color;
void main() {
    frag_color = vec4(0.0, 1.0, 0.0, 1.0);
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

    static GLuint compile_render_program(const char* vs_src, const char* fs_src) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vs_src, NULL);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fs_src, NULL);
        glCompileShader(fs);
        if (!check_compile_errors(vs, "VERTEX") || !check_compile_errors(fs, "FRAGMENT")) {
            glDeleteShader(vs);
            glDeleteShader(fs);
            return 0;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
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
    static constexpr size_t MAX_WIDTH = 8192;

    struct DrawParams {
        int first_channel = 0;
        int channel_count = 0;
        ImVec2 origin;
        float row_pitch = 0.0f;
    };

    GLuint demux_program_ = 0;
    GLuint downsample_program_ = 0;
    GLuint render_program_ = 0;
    GLuint ssbo_staging_raw_ = 0;
    GLuint ssbo_signal_history_ = 0;
    GLuint ssbo_envelope_ = 0;
    GLuint empty_vao_ = 0;
    int width_ = 0;
    DrawParams draw_params_;
    std::atomic<float> voltage_scale_{100.0f};

    static void draw_callback(const ImDrawList*, const ImDrawCmd* cmd) {
        static_cast<SignalRenderer*>(cmd->UserCallbackData)->draw(cmd->ClipRect);
    }

    void draw(const ImVec4& clip) {
        const DrawParams& p = draw_params_;
        if (p.channel_count <= 0 || width_ <= 0) return;

        ImDrawData* draw_data = ImGui::GetDrawData();
        ImVec2 pos = draw_data->DisplayPos;
        ImVec2 size = draw_data->DisplaySize;
        ImVec2 scale = draw_data->FramebufferScale;

        // Backend keeps the previous command's scissor during callbacks, so set our own
        float fb_height = size.y * scale.y;
        glScissor(static_cast<GLint>((clip.x - pos.x) * scale.x),
                  static_cast<GLint>(fb_height - (clip.w - pos.y) * scale.y),
                  static_cast<GLsizei>((clip.z - clip.x) * scale.x),
                  static_cast<GLsizei>((clip.w - clip.y) * scale.y));

        glUseProgram(render_program_);
        glBindVertexArray(empty_vao_);
        glUniform1i(glGetUniformLocation(render_program_, "u_width"), width_);
        glUniform1i(glGetUniformLocation(render_program_, "u_first_channel"), p.first_channel);
        glUniform2f(glGetUniformLocation(render_program_, "u_origin"), p.origin.x, p.origin.y);
        glUniform1f(glGetUniformLocation(render_program_, "u_row_pitch"), p.row_pitch);
        glUniform2f(glGetUniformLocation(render_program_, "u_display_pos"), pos.x, pos.y);
        glUniform2f(glGetUniformLocation(render_program_, "u_display_size"), size.x, size.y);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_envelope_);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6 * width_, p.channel_count);
    }

   public:
    SignalRenderer() = default;

    ~SignalRenderer() {
        if (ssbo_staging_raw_) glDeleteBuffers(1, &ssbo_staging_raw_);
        if (ssbo_signal_history_) glDeleteBuffers(1, &ssbo_signal_history_);
        if (ssbo_envelope_) glDeleteBuffers(1, &ssbo_envelope_);
        if (empty_vao_) glDeleteVertexArrays(1, &empty_vao_);
    }

    void set_voltage_scale(float scale) { voltage_scale_.store(scale, std::memory_order_relaxed); }
    float get_voltage_scale() const { return voltage_scale_.load(std::memory_order_relaxed); }

    // Queues the waveform draw into ImGui's command stream, so it respects window clipping
    void add_draw_callback(ImDrawList* draw_list, int first_channel, int channel_count,
                           ImVec2 origin, float row_pitch) {
        draw_params_ = {first_channel, channel_count, origin, row_pitch};
        draw_list->AddCallback(&SignalRenderer::draw_callback, this);
        // ImGui defines this sentinel as an int-to-pointer cast
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

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
        render_program_ = SignalShaders::compile_render_program(WAVEFORM_VERTEX_SHADER_SRC,
                                                                WAVEFORM_FRAGMENT_SHADER_SRC);

        glGenBuffers(1, &ssbo_staging_raw_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_staging_raw_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4000 * 12 * 384 * sizeof(int16_t), nullptr,
                     GL_DYNAMIC_DRAW);

        glGenBuffers(1, &ssbo_signal_history_);
        resize_history_buffer(initial_history_size);

        glGenBuffers(1, &ssbo_envelope_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_envelope_);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     static_cast<GLsizeiptr>(MAX_WIDTH * 384 * 2 * sizeof(float)), nullptr,
                     GL_DYNAMIC_DRAW);

        glGenVertexArrays(1, &empty_vao_);

        // TODO: remove after Intel iGPU diagnostics
        GLint max_vs_ssbo = 0;
        glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &max_vs_ssbo);
        std::cout << "MAX_VERTEX_SSBO_BLOCKS: " << max_vs_ssbo << "\n"
                  << "envelope glGetError: 0x" << std::hex << glGetError() << std::dec << "\n";
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
                                      size_t current_write_idx, size_t total_processed,
                                      size_t width) {
        width_ = static_cast<int>(std::min(width, MAX_WIDTH));

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
        glUniform1i(glGetUniformLocation(downsample_program_, "u_width"), width_);
        glUniform1f(glGetUniformLocation(downsample_program_, "u_row_height"), 60.0f);

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_signal_history_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_envelope_);

        glDispatchCompute(static_cast<GLuint>((width_ + 255) / 256), 384, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    }
};
