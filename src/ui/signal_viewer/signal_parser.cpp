#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <cmath>
#include <immintrin.h>
#include "glad/gl.h"
#include "../../processors/neuropixels_input.cpp"

class SignalParser {
   private:
    NeuropixelsInput neuropixels_input_;
    std::thread worker_thread_;
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> running_{false};

    std::vector<float> ap_history_;
    size_t samples_history = 3 * 30000;
    size_t write_idx_ = 0;

    static constexpr size_t RING_BUFFER_SLOTS = 3;
    std::vector<float> ring_render_buffers_[RING_BUFFER_SLOTS];

    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> producer_slot_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> consumer_slot_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> current_width_{0};

    uint64_t total_samples_processed_ = 0;

    alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> samples_since_last_check_{0};
    std::chrono::steady_clock::time_point last_rate_time_;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> samples_per_sec_{0};

    GLuint compute_program_ = 0;
    GLuint ssbo_raw_ = 0;
    GLuint render_tex_ = 0;

    std::atomic<size_t> requested_history_{3 * 30000};
    std::atomic<float> voltage_scale_{100.0f};
    bool resize_pending_ = false;

    void collect_loop() {
        neuropixels_input_.start();

        ap_history_.assign(384 * samples_history, 0.0f);
        electrodePacket* packet = nullptr;
        size_t tail_idx = 0;

        while (running_.load(std::memory_order_relaxed)) {
            if (resize_pending_) {
                size_t new_size = requested_history_.load(std::memory_order_relaxed);
                ap_history_.assign(384 * new_size, 0.0f);
                samples_history = new_size;
                write_idx_ = 0;
                total_samples_processed_ = 0;
                resize_pending_ = false;
            }

            bool processed_any = false;

            while (neuropixels_input_.peek_packet(packet, tail_idx)) {
                processed_any = true;

                for (size_t i = 0; i < NP1_PROBE_SUPERFRAMESIZE; ++i) {
                    for (size_t c = 0; c < 384; ++c) {
                        ap_history_[c * samples_history + write_idx_] = static_cast<float>(packet->apData[i][c]);
                    }
                    write_idx_ = (write_idx_ + 1) % samples_history;
                }

                total_samples_processed_ += NP1_PROBE_SUPERFRAMESIZE;
                samples_since_last_check_.fetch_add(NP1_PROBE_SUPERFRAMESIZE, std::memory_order_relaxed);

                neuropixels_input_.release_packet(tail_idx);
            }

            size_t target_width = current_width_.load(std::memory_order_relaxed);
            if (processed_any && target_width > 0) {
                size_t active_consumer = consumer_slot_.load(std::memory_order_acquire);
                size_t next_producer_slot = (producer_slot_.load(std::memory_order_relaxed) + 1) % RING_BUFFER_SLOTS;
                if (next_producer_slot == active_consumer) {
                    next_producer_slot = (next_producer_slot + 1) % RING_BUFFER_SLOTS;
                }

                update_ekg_buffer(next_producer_slot, target_width);
                producer_slot_.store(next_producer_slot, std::memory_order_release);
            }

            if (!processed_any) {
                std::this_thread::yield();
            }
        }
    }

    void update_ekg_buffer(size_t slot, size_t pixel_width) {
        size_t total_elements = 384 * samples_history;
        if (ring_render_buffers_[slot].size() != total_elements) {
            ring_render_buffers_[slot].resize(total_elements, 0.0f);
        }
        std::copy_n(ap_history_.data(), total_elements, ring_render_buffers_[slot].data());
    }

   public:
    SignalParser() = default;

    ~SignalParser() {
        stop();
        if (ssbo_raw_) glDeleteBuffers(1, &ssbo_raw_);
        if (render_tex_) glDeleteTextures(1, &render_tex_);
    }

    void set_compute_program(GLuint program) { compute_program_ = program; }
    void set_voltage_scale(float scale) { voltage_scale_.store(scale, std::memory_order_relaxed); }

    void update_history_duration(int seconds) {
        requested_history_.store(seconds * 30000, std::memory_order_relaxed);
        resize_pending_ = true;
    }

    void init_gl_resources() {
        glGenBuffers(1, &ssbo_raw_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_raw_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 384 * 10 * 30000 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

        glGenTextures(1, &render_tex_);
        glBindTexture(GL_TEXTURE_2D, render_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4096, 23040, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void start() {
        if (!running_.load(std::memory_order_relaxed)) {
            last_rate_time_ = std::chrono::steady_clock::now();
            samples_since_last_check_.store(0, std::memory_order_relaxed);
            samples_per_sec_.store(0, std::memory_order_relaxed);

            running_.store(true, std::memory_order_relaxed);
            worker_thread_ = std::thread(&SignalParser::collect_loop, this);
#if defined(__linux__)
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(6, &cpuset);
            pthread_setaffinity_np(worker_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
        }
    }

    void stop() {
        if (running_.load(std::memory_order_relaxed)) {
            running_.store(false, std::memory_order_relaxed);
            if (worker_thread_.joinable()) worker_thread_.join();
        }
    }

    void calculate_rate(size_t& ingestion_rate) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - last_rate_time_).count();
        if (duration >= 1000000) {
            size_t current_total = samples_since_last_check_.exchange(0, std::memory_order_relaxed);
            size_t calculated = (current_total * 1000000) / duration;
            samples_per_sec_.store(calculated, std::memory_order_relaxed);
            last_rate_time_ = now;
        }
        ingestion_rate = samples_per_sec_.load(std::memory_order_relaxed);
    }

    void dispatch_downsample(size_t pixel_width) {
        current_width_.store(pixel_width, std::memory_order_relaxed);

        size_t active_producer = producer_slot_.load(std::memory_order_acquire);
        consumer_slot_.store(active_producer, std::memory_order_release);

        size_t total_elements = ring_render_buffers_[active_producer].size();
        size_t current_history_size = total_elements / 384;

        if (total_elements > 0) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_raw_);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, total_elements * sizeof(float), ring_render_buffers_[active_producer].data());
        }

        size_t current_cursor = static_cast<size_t>(total_samples_processed_ % (current_history_size > 0 ? current_history_size : 1));

        glUseProgram(compute_program_);
        glUniform1i(glGetUniformLocation(compute_program_, "u_total_samples"), static_cast<int>(current_history_size));
        glUniform1i(glGetUniformLocation(compute_program_, "u_cursor_idx"), static_cast<int>(current_cursor));
        glUniform1f(glGetUniformLocation(compute_program_, "u_v_scale"), voltage_scale_.load(std::memory_order_relaxed));

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo_raw_);
        glBindImageTexture(0, render_tex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

        glDispatchCompute(16, 384, 1); // 16 * 256 = 4096 X-groups, 384 Y-groups
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    GLuint get_render_texture() const { return render_tex_; }
};
