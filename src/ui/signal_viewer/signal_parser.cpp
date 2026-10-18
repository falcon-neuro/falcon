#pragma once
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include "../../processors/neuropixels_input.cpp"
#include "signal_renderer.cpp"

class SignalParser {
   private:
    NeuropixelsInput neuropixels_input_;
    std::thread worker_thread_;
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> running_{false};

    std::vector<int16_t> cpu_ingest_buffer_;
    std::vector<int16_t> cpu_swap_buffer_;
    std::mutex buffer_mutex_;

    size_t samples_history = 1 * 30000;
    size_t write_idx_ = 0;
    uint64_t total_samples_processed_ = 0;

    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> current_width_{0};
    alignas(std::hardware_destructive_interference_size)
        std::atomic<uint64_t> samples_since_last_check_{0};
    std::chrono::steady_clock::time_point last_rate_time_;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> samples_per_sec_{0};

    std::atomic<size_t> requested_history_{1 * 30000};
    bool resize_pending_ = false;

    SignalRenderer gpu_renderer_;

    void collect_loop() {
        neuropixels_input_.start();
        electrodePacket* packet = nullptr;
        size_t tail_idx = 0;

        while (running_.load(std::memory_order_relaxed)) {
            bool processed_any = false;

            while (neuropixels_input_.peek_packet(packet, tail_idx)) {
                processed_any = true;

                {
                    std::lock_guard<std::mutex> lock(buffer_mutex_);
                    size_t current_size = cpu_ingest_buffer_.size();
                    cpu_ingest_buffer_.resize(current_size + (12 * 384));
                    std::memcpy(cpu_ingest_buffer_.data() + current_size, packet->apData,
                                12 * 384 * sizeof(int16_t));
                }

                samples_since_last_check_.fetch_add(12, std::memory_order_relaxed);
                neuropixels_input_.release_packet(tail_idx);
            }

            if (!processed_any) {
                std::this_thread::yield();
            }
        }
    }

   public:
    SignalParser() = default;
    ~SignalParser() { stop(); }

    void calculate_available_vram_kb(size_t& available_vram_kb) {
        available_vram_kb = gpu_renderer_.get_universal_available_vram_kb();
    }

    void set_voltage_scale(float scale) { gpu_renderer_.set_voltage_scale(scale); }

    void update_history_duration(float seconds) {
        requested_history_.store(static_cast<size_t>(std::round(seconds * 30000)),
                                 std::memory_order_relaxed);
        resize_pending_ = true;
    }

    void start() {
        gpu_renderer_.init_gl_resources(samples_history);
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
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(now - last_rate_time_).count();
        if (duration >= 1000000) {
            size_t current_total = samples_since_last_check_.exchange(0, std::memory_order_relaxed);
            size_t calculated = (current_total * 1000000) / static_cast<size_t>(duration);
            samples_per_sec_.store(calculated, std::memory_order_relaxed);
            last_rate_time_ = now;
        }
        ingestion_rate = samples_per_sec_.load(std::memory_order_relaxed);
    }

    void dispatch_downsample(size_t pixel_width) {
        current_width_.store(pixel_width, std::memory_order_relaxed);

        if (resize_pending_) {
            size_t new_size = requested_history_.load(std::memory_order_relaxed);
            gpu_renderer_.resize_history_buffer(new_size);
            samples_history = new_size;
            write_idx_ = 0;
            total_samples_processed_ = 0;
            resize_pending_ = false;
        }

        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            cpu_swap_buffer_.swap(cpu_ingest_buffer_);
        }

        size_t total_elements = cpu_swap_buffer_.size();
        size_t num_superframes = total_elements / (12 * 384);
        size_t new_samples_count = num_superframes * 12;

        gpu_renderer_.execute_demux_and_downsample(cpu_swap_buffer_.data(), total_elements,
                                                   num_superframes, samples_history, write_idx_,
                                                   total_samples_processed_);

        write_idx_ = (write_idx_ + new_samples_count) % samples_history;
        total_samples_processed_ += new_samples_count;

        cpu_swap_buffer_.clear();
    }

    GLuint get_render_texture() const { return gpu_renderer_.get_render_texture(); }
};
