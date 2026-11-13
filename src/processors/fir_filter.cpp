#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <type_traits>
#include "../core/engine.cpp"

// FirFilter: an FIR filter on every channel, computed in double; its input and output are float32.

// y[n] = taps[0]·x[n] + taps[1]·x[n−1] + … + taps[N−1]·x[n−N+1]
template <int TapCount>
struct FirFilterOptions {
    double design_rate = 0;  // Hz, the sampling rate the taps were designed for
    std::array<double, TapCount> taps{};
};

// A NaN tap would make every output NaN, which no detector ever reads as a crossing. A NaN fails
// both comparisons, an infinity one of them.
template <std::size_t TapCount>
[[nodiscard]] constexpr bool is_every_tap_finite(const std::array<double, TapCount>& taps) {
    return std::ranges::all_of(taps, [](double tap) {
        return tap >= -std::numeric_limits<double>::max() &&
               tap <= std::numeric_limits<double>::max();
    });
}

template <typename InputStreamInfo, FirFilterOptions options>
struct FirFilter {
    static constexpr std::size_t tap_count = options.taps.size();

    static_assert(tap_count > 0);
    static_assert(options.design_rate == InputStreamInfo::rate,
                  "the taps were designed for another sampling rate, so every cutoff would shift");
    static_assert(is_every_tap_finite(options.taps), "a tap is NaN or infinite");

    struct Connections {
        SampleInputBuffer<InputStreamInfo> input;
        SampleOutputBuffer<InputStreamInfo> output;
    };

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        connections.output.set_header(connections.input.header);

        // Where samples were lost the signal jumps, and filtering across the jump would give a
        // transient that a detector reads as spikes on every channel.
        const u64 lost_sample_count = connections.input.header.lost_sample_count;
        const bool is_restarting =
            !has_started_ || lost_sample_count != previous_lost_sample_count_;
        has_started_ = true;
        previous_lost_sample_count_ = lost_sample_count;

        newest_slot_ = (newest_slot_ + 1) % tap_count;
        // Computed once per sample, so the loop over channel vectors runs without a modulo.
        std::array<std::size_t, tap_count> tap_slots{};
        for (std::size_t tap_index = 0; tap_index < tap_count; ++tap_index) {
            tap_slots[tap_index] = (newest_slot_ + tap_count - tap_index) % tap_count;
        }

        constexpr auto doubles_per_simd_vector =
            static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR) / 2;
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const std::size_t first_channel = static_cast<std::size_t>(simd_vector_index) *
                                              static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR);
            const std::size_t second_half_first_channel = first_channel + doubles_per_simd_vector;
            // auto, not SimdVector: a type that depends on the template keeps the other
            // instruction set's branch from being compiled.
            const auto values = connections.input.load(simd_vector_index);
            if constexpr (HAS_AVX512) {
                const __m512d first_half = _mm512_cvtps_pd(_mm512_castps512_ps256(values));
                const __m512d second_half = _mm512_cvtps_pd(
                    _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(values), 1)));
                const __m256 filtered_first_half = _mm512_cvtpd_ps(
                    run_taps(first_half, first_channel, is_restarting, tap_slots));
                const __m256 filtered_second_half = _mm512_cvtpd_ps(
                    run_taps(second_half, second_half_first_channel, is_restarting, tap_slots));
                const __m512d filtered_halves = _mm512_insertf64x4(
                    _mm512_castps_pd(_mm512_castps256_ps512(filtered_first_half)),
                    _mm256_castps_pd(filtered_second_half), 1);
                connections.output.store(simd_vector_index, _mm512_castpd_ps(filtered_halves));
            } else {
                const __m256d first_half = _mm256_cvtps_pd(_mm256_castps256_ps128(values));
                const __m256d second_half = _mm256_cvtps_pd(_mm256_extractf128_ps(values, 1));
                const __m128 filtered_first_half = _mm256_cvtpd_ps(
                    run_taps(first_half, first_channel, is_restarting, tap_slots));
                const __m128 filtered_second_half = _mm256_cvtpd_ps(
                    run_taps(second_half, second_half_first_channel, is_restarting, tap_slots));
                connections.output.store(
                    simd_vector_index, _mm256_set_m128(filtered_second_half, filtered_first_half));
            }
        }
    }

private:
    struct HistorySlot {
        alignas(64) std::array<double, InputStreamInfo::padded_channels> values{};
    };

    // The last tap_count inputs of every channel, a circular buffer whose newest is at
    // newest_slot_.
    std::array<HistorySlot, tap_count> history_{};
    std::size_t newest_slot_ = 0;
    bool has_started_ = false;
    u64 previous_lost_sample_count_ = 0;

    // Runs one vector of doubles, the channels from first_channel on, through every tap.
    // tap_slots[k] is the slot holding x[n−k].
    template <typename DoubleVector>
    [[nodiscard]] DoubleVector run_taps(DoubleVector values, std::size_t first_channel,
                                        bool is_restarting,
                                        const std::array<std::size_t, tap_count>& tap_slots) {
        // A start fills every slot with this input, as if it had always been there, so the output
        // starts at its steady value instead of jumping: a DC offset would otherwise give every
        // channel a transient that a spike detector reads as spikes.
        if constexpr (std::is_same_v<DoubleVector, __m512d>) {
            if (is_restarting) {
                for (HistorySlot& slot : history_) {
                    _mm512_store_pd(&slot.values[first_channel], values);
                }
            } else {
                _mm512_store_pd(&history_[newest_slot_].values[first_channel], values);
            }
            __m512d filtered = _mm512_setzero_pd();
            for (std::size_t tap_index = 0; tap_index < tap_count; ++tap_index) {
                const __m512d past_values =
                    _mm512_load_pd(&history_[tap_slots[tap_index]].values[first_channel]);
                filtered =
                    _mm512_fmadd_pd(_mm512_set1_pd(options.taps[tap_index]), past_values, filtered);
            }
            return filtered;
        } else {
            static_assert(std::is_same_v<DoubleVector, __m256d>);
            if (is_restarting) {
                for (HistorySlot& slot : history_) {
                    _mm256_store_pd(&slot.values[first_channel], values);
                }
            } else {
                _mm256_store_pd(&history_[newest_slot_].values[first_channel], values);
            }
            __m256d filtered = _mm256_setzero_pd();
            for (std::size_t tap_index = 0; tap_index < tap_count; ++tap_index) {
                const __m256d past_values =
                    _mm256_load_pd(&history_[tap_slots[tap_index]].values[first_channel]);
                filtered =
                    _mm256_fmadd_pd(_mm256_set1_pd(options.taps[tap_index]), past_values, filtered);
            }
            return filtered;
        }
    }
};
