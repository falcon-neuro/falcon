#pragma once
#include <immintrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

    std::vector<float> ap_history_;
    size_t samples_history = 3 * 30000;
    size_t write_idx_ = 0;

    static constexpr size_t RING_BUFFER_SLOTS = 3;
    std::vector<float> ring_render_buffers_[RING_BUFFER_SLOTS];

    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> producer_slot_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> consumer_slot_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> current_width_{0};

    uint64_t total_samples_processed_ = 0;

    alignas(std::hardware_destructive_interference_size)
        std::atomic<uint64_t> samples_since_last_check_{0};
    std::chrono::steady_clock::time_point last_rate_time_;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> samples_per_sec_{0};

    std::atomic<size_t> requested_history_{3 * 30000};
    bool resize_pending_ = false;

    SignalRenderer gpu_renderer_;

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
                        ap_history_[c * samples_history + write_idx_] =
                            static_cast<float>(packet->apData[i][c]);
                    }
                    write_idx_ = (write_idx_ + 1) % samples_history;
                }

                total_samples_processed_ += NP1_PROBE_SUPERFRAMESIZE;
                samples_since_last_check_.fetch_add(NP1_PROBE_SUPERFRAMESIZE,
                                                    std::memory_order_relaxed);
                neuropixels_input_.release_packet(tail_idx);
            }

            size_t target_width = current_width_.load(std::memory_order_relaxed);
            if (processed_any && target_width > 0) {
                size_t active_consumer = consumer_slot_.load(std::memory_order_acquire);
                size_t next_producer_slot =
                    (producer_slot_.load(std::memory_order_relaxed) + 1) % RING_BUFFER_SLOTS;
                if (next_producer_slot == active_consumer) {
                    next_producer_slot = (next_producer_slot + 1) % RING_BUFFER_SLOTS;
                }

                size_t total_elements = 384 * samples_history;
                if (ring_render_buffers_[next_producer_slot].size() != total_elements) {
                    ring_render_buffers_[next_producer_slot].resize(total_elements, 0.0f);
                }
                // TODO: don't copy the whole history, only copy the new samples
                std::copy_n(ap_history_.data(), total_elements,
                            ring_render_buffers_[next_producer_slot].data());

                producer_slot_.store(next_producer_slot, std::memory_order_release);
            }

            if (!processed_any) {
                std::this_thread::yield();
            }
        }
    }

   public:
    SignalParser() {

    };

    ~SignalParser() { stop(); }

    void set_voltage_scale(float scale) { gpu_renderer_.set_voltage_scale(scale); }

    void update_history_duration(float seconds) {
        requested_history_.store(static_cast<size_t>(std::round(seconds * 30000)),
                                 std::memory_order_relaxed);
        resize_pending_ = true;
    }

    void start() {
        gpu_renderer_.init_gl_resources();
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

        size_t active_producer = producer_slot_.load(std::memory_order_acquire);
        consumer_slot_.store(active_producer, std::memory_order_release);

        size_t total_elements = ring_render_buffers_[active_producer].size();
        size_t current_history_size = total_elements / 384;

        size_t current_cursor = static_cast<size_t>(
            total_samples_processed_ % (current_history_size > 0 ? current_history_size : 1));

        gpu_renderer_.execute_downsample(ring_render_buffers_[active_producer],
                                         current_history_size, current_cursor);
    }

    GLuint get_render_texture() const { return gpu_renderer_.get_render_texture(); }
};