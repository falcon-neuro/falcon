#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include "../../processors/neuropixels_input.cpp"

class SignalParser {
   private:
    NeuropixelsInput neuropixels_input_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    std::vector<float> ap_history_;
    size_t samples_history = 30000;
    size_t write_idx_ = 0;

    static constexpr size_t RING_BUFFER_SLOTS = 3;
    std::vector<float> ring_render_buffers_[RING_BUFFER_SLOTS];

    std::atomic<size_t> producer_slot_{0};
    std::atomic<size_t> consumer_slot_{0};
    std::atomic<size_t> current_width_{0};

    std::atomic<uint64_t> total_samples_received_{0};

    void collect_loop() {
        neuropixels_input_.start();
        ap_history_.assign(samples_history * NP1_PROBE_CHANNEL_COUNT, 0.0f);
        electrodePacket packet;

        while (running_.load(std::memory_order_relaxed)) {
            bool has_data = false;
            while (neuropixels_input_.pop(packet)) {
                has_data = true;
                for (size_t i = 0; i < NP1_PROBE_SUPERFRAMESIZE; ++i) {
                    for (size_t c = 0; c < NP1_PROBE_CHANNEL_COUNT; ++c) {
                        size_t history_idx = (c * samples_history) + write_idx_;
                        ap_history_[history_idx] = static_cast<float>(packet.apData[i][c]);
                    }
                    write_idx_ = (write_idx_ + 1) % samples_history;
                }
                total_samples_received_.fetch_add(NP1_PROBE_SUPERFRAMESIZE,
                                                  std::memory_order_relaxed);
            }

            size_t target_width = current_width_.load(std::memory_order_relaxed);
            if (has_data && target_width > 0) {
                size_t active_consumer = consumer_slot_.load(std::memory_order_acquire);
                size_t next_producer_slot =
                    (producer_slot_.load(std::memory_order_relaxed) + 1) % RING_BUFFER_SLOTS;

                if (next_producer_slot == active_consumer) {
                    next_producer_slot = (next_producer_slot + 1) % RING_BUFFER_SLOTS;
                }

                update_render_buffer_slot(next_producer_slot, target_width);
                producer_slot_.store(next_producer_slot, std::memory_order_release);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        neuropixels_input_.stop();
    }

    void update_render_buffer_slot(size_t slot, size_t pixel_width) {
        size_t required_size = pixel_width * NP1_PROBE_CHANNEL_COUNT;
        if (ring_render_buffers_[slot].size() != required_size) {
            ring_render_buffers_[slot].resize(required_size);
        }

        std::vector<size_t> src_indices(pixel_width);
        for (size_t i = 0; i < pixel_width; ++i) {
            src_indices[i] = (i * samples_history / pixel_width) % samples_history;
        }

        for (size_t c = 0; c < NP1_PROBE_CHANNEL_COUNT; ++c) {
            size_t channel_offset = c * samples_history;
            size_t dest_offset = c * pixel_width;

            float* dest_ptr = &ring_render_buffers_[slot][dest_offset];
            const float* src_ptr = &ap_history_[channel_offset];

            for (size_t i = 0; i < pixel_width; ++i) {
                dest_ptr[i] = src_ptr[src_indices[i]];
            }
        }
    }

   public:
    SignalParser() = default;
    ~SignalParser() { stop(); }

    void start() {
        if (!running_.load(std::memory_order_relaxed)) {
            running_.store(true, std::memory_order_relaxed);
            worker_thread_ = std::thread(&SignalParser::collect_loop, this);
        }

#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(6, &cpuset);
        pthread_setaffinity_np(worker_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }

    void stop() {
        if (running_.load(std::memory_order_relaxed)) {
            running_.store(false, std::memory_order_relaxed);
            if (worker_thread_.joinable()) worker_thread_.join();
        }
    }

    void get_latest_render_data(std::vector<float>& local_ui_buf, size_t pixel_width) {
        current_width_.store(pixel_width, std::memory_order_relaxed);

        size_t latest_prod_slot = producer_slot_.load(std::memory_order_acquire);
        consumer_slot_.store(latest_prod_slot, std::memory_order_release);

        size_t target_size = pixel_width * NP1_PROBE_CHANNEL_COUNT;
        if (local_ui_buf.size() != target_size) {
            local_ui_buf.resize(target_size);
        }

        if (ring_render_buffers_[latest_prod_slot].size() == target_size) {
            std::memcpy(local_ui_buf.data(), ring_render_buffers_[latest_prod_slot].data(),
                        target_size * sizeof(float));
        }
    }

    uint64_t flush_samples_count() {
        return total_samples_received_.exchange(0, std::memory_order_relaxed);
    }
};
