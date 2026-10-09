#pragma once
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <thread>
#include <vector>
#include "../../processors/neuropixels_input.cpp"

class SignalParser {
   private:
    NeuropixelsInput neuropixels_input_;
    std::thread worker_thread_;
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> running_{false};

    std::vector<float> ap_history_;
    size_t samples_history = 10 * 30000;
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

    void collect_loop() {
        neuropixels_input_.start();

        ap_history_.assign(samples_history * NP1_PROBE_CHANNEL_COUNT, 0.0f);
        electrodePacket* packet = nullptr;
        size_t tail_idx = 0;

        while (running_.load(std::memory_order_relaxed)) {
            bool processed_any = false;

            while (neuropixels_input_.peek_packet(packet, tail_idx)) {
                processed_any = true;

                for (size_t i = 0; i < NP1_PROBE_SUPERFRAMESIZE; ++i) {
                    const int16_t* src_row = packet->apData[i];

                    for (size_t c = 0; c < NP1_PROBE_CHANNEL_COUNT; c += 8) {
                        __m128i raw_int16 =
                            _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_row + c));
                        __m256i expanded_int32 = _mm256_cvtepi16_epi32(raw_int16);
                        __m256 float_converted = _mm256_cvtepi32_ps(expanded_int32);

                        __m128 lower_half = _mm256_castps256_ps128(float_converted);
                        __m128 upper_half = _mm256_extractf128_ps(float_converted, 1);

                        ap_history_[(c + 0) * samples_history + write_idx_] =
                            _mm_cvtss_f32(lower_half);
                        ap_history_[(c + 1) * samples_history + write_idx_] = _mm_cvtss_f32(
                            _mm_shuffle_ps(lower_half, lower_half, _MM_SHUFFLE(1, 1, 1, 1)));
                        ap_history_[(c + 2) * samples_history + write_idx_] = _mm_cvtss_f32(
                            _mm_shuffle_ps(lower_half, lower_half, _MM_SHUFFLE(2, 2, 2, 2)));
                        ap_history_[(c + 3) * samples_history + write_idx_] = _mm_cvtss_f32(
                            _mm_shuffle_ps(lower_half, lower_half, _MM_SHUFFLE(3, 3, 3, 3)));

                        ap_history_[(c + 4) * samples_history + write_idx_] =
                            _mm_cvtss_f32(upper_half);
                        ap_history_[(c + 5) * samples_history + write_idx_] = _mm_cvtss_f32(
                            _mm_shuffle_ps(upper_half, upper_half, _MM_SHUFFLE(1, 1, 1, 1)));
                        ap_history_[(c + 6) * samples_history + write_idx_] = _mm_cvtss_f32(
                            _mm_shuffle_ps(upper_half, upper_half, _MM_SHUFFLE(2, 2, 2, 2)));
                        ap_history_[(c + 7) * samples_history + write_idx_] = _mm_cvtss_f32(
                            _mm_shuffle_ps(upper_half, upper_half, _MM_SHUFFLE(3, 3, 3, 3)));
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

                update_ekg_buffer(next_producer_slot, target_width);
                producer_slot_.store(next_producer_slot, std::memory_order_release);
            }

            if (!processed_any) {
                std::this_thread::yield();
            }
        }
    }

    void update_ekg_buffer(size_t slot, size_t pixel_width) {
        size_t required_size = pixel_width * NP1_PROBE_CHANNEL_COUNT;
        if (ring_render_buffers_[slot].size() != required_size) {
            ring_render_buffers_[slot].assign(required_size, 0.0f);
        }

        uint64_t current_sample = total_samples_processed_;
        size_t current_cursor = static_cast<size_t>(current_sample % samples_history);
        size_t pixel_cursor = (current_cursor * pixel_width) / samples_history;
        if (pixel_cursor >= pixel_width) {
            pixel_cursor = pixel_width - 1;
        }

        size_t blank_zone = pixel_width > 40 ? 20 : 5;
        float* dest_base = ring_render_buffers_[slot].data();
        const float* src_base = ap_history_.data();

        size_t target_b_lower = (pixel_cursor + 1) % pixel_width;
        size_t target_b_upper = (pixel_cursor + blank_zone) % pixel_width;
        bool wrapped = target_b_lower > target_b_upper;

        std::vector<size_t> sample_offsets(pixel_width);
        std::vector<uint8_t> is_blank_arr(pixel_width, 0);

        for (size_t x = 0; x < pixel_width; ++x) {
            if (!wrapped) {
                if (x >= target_b_lower && x <= target_b_upper) {
                    is_blank_arr[x] = 1;
                }
            } else {
                if (x >= target_b_lower || x <= target_b_upper) {
                    is_blank_arr[x] = 1;
                }
            }
            size_t offset = (x * samples_history) / pixel_width;
            sample_offsets[x] = (offset >= samples_history) ? (samples_history - 1) : offset;
        }

        for (size_t c = 0; c < NP1_PROBE_CHANNEL_COUNT; ++c) {
            float* dest_row = dest_base + c * pixel_width;
            const float* src_channel = src_base + c * samples_history;

            for (size_t x = 0; x < pixel_width; ++x) {
                if (is_blank_arr[x]) {
                    dest_row[x] = 0.0f;
                } else {
                    dest_row[x] = src_channel[sample_offsets[x]];
                }
            }
        }
    }

   public:
    SignalParser() = default;
    ~SignalParser() { stop(); }

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

    void get_latest_render_data(std::vector<float>& local_ui_buf, size_t pixel_width,
                                size_t& out_ap_samples_per_sec) {
        current_width_.store(pixel_width, std::memory_order_relaxed);
        size_t latest_prod_slot = producer_slot_.load(std::memory_order_acquire);
        consumer_slot_.store(latest_prod_slot, std::memory_order_release);

        size_t target_size = pixel_width * NP1_PROBE_CHANNEL_COUNT;
        if (local_ui_buf.size() != target_size) [[unlikely]] {
            local_ui_buf.resize(target_size);
        }
        if (ring_render_buffers_[latest_prod_slot].size() == target_size) [[likely]] {
            std::memcpy(local_ui_buf.data(), ring_render_buffers_[latest_prod_slot].data(),
                        target_size * sizeof(float));
        }

        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed = now - last_rate_time_;
        if (elapsed.count() >= 0.5f) [[unlikely]] {
            uint64_t samples = samples_since_last_check_.exchange(0, std::memory_order_relaxed);
            size_t rate = (elapsed.count() > 0.0f)
                              ? static_cast<size_t>(static_cast<float>(samples) / elapsed.count())
                              : 0;
            samples_per_sec_.store(rate, std::memory_order_relaxed);
            last_rate_time_ = now;
        }
        out_ap_samples_per_sec = samples_per_sec_.load(std::memory_order_relaxed);
    }
};
