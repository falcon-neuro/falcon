#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include "../core/engine.cpp"

// RippleDetector: reduces each sample of a stream already filtered to the ripple band to its
// envelope, the mean over the channels of their squared values, and emits a Ripple event when the
// envelope rises more than threshold_multiplier mean absolute deviations above its running mean.

// One event per detection, with the header of the sample whose envelope crossed the threshold.
struct Ripple : Event {
    SampleHeader header{};
};

struct RippleDetectorOptions {
    /// The time over which the detector averages the signal to learn its usual level and spread,
    /// in seconds. For the first such period after the start it only learns and detects nothing.
    /// It must last at least one sample. A longer time gives a steadier threshold, which follows
    /// slow changes of the signal later.
    double smoothing_time_seconds = 10;
    /// How long the detector waits after a detection before it can detect again, in milliseconds.
    /// During the wait, samples are neither tested nor added to the running statistics, so one
    /// ripple gives one event. 0 means no wait; it can't be negative.
    double lockout_milliseconds = 30;
};

/// Detects ripples in a signal already filtered to the ripple band, and emits an event for each. It
/// follows the signal's power and detects when that rises well above its usual level, which it
/// learns as it runs.
template <typename InputStreamInfo, RippleDetectorOptions options>
struct RippleDetector {
    static_assert(options.smoothing_time_seconds * InputStreamInfo::rate >= 1,
                  "the smoothing time is shorter than one sample");
    static_assert(options.lockout_milliseconds >= 0, "the lockout can't be negative");

    struct Connections {
        /// The signal to detect ripples in: samples already filtered to the ripple band. The
        /// detector follows the signal's power, the mean over all channels of their squared
        /// values, so every channel of the stream should carry ripples.
        SampleInputBuffer<InputStreamInfo> input;
        /// How far the signal's power must rise above its usual level to count as a ripple, in
        /// mean absolute deviations above its running mean. A lower value detects more ripples
        /// and earlier, a higher one fewer and later. A change takes effect on the next sample.
        StateInput<float, 6.0F> threshold_multiplier = UNCONNECTED;
        /// Whether the detector is detecting. While it is off, e.g. during a stimulation's
        /// artifact, samples are neither tested nor added to the running statistics.
        StateInput<bool, true> is_detection_enabled = UNCONNECTED;
        /// One event for each detected ripple, on the sample at which the signal's power crossed
        /// the threshold.
        EventOutputBuffer<Ripple, 1> output;
        /// On from the sample that crossed the threshold until the lockout has passed, off
        /// otherwise. It lasts as long as the lockout, not as long as the ripple.
        std::optional<StateOutput<bool, false>> is_in_lockout = std::nullopt;
    };

    explicit RippleDetector(ProcessorName processor_name) : name(processor_name) {}

    RegisteredName name;

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        const double envelope = mean_square(connections.input);
        // A NaN or an infinite value would stay in the running statistics for the rest of the run,
        // and nothing would be detected again.
        if (!std::isfinite(envelope)) {
            warn(name, "skipped sample {}, whose envelope isn't finite",
                 connections.input.header.sample_number);
            return;
        }
        if (burn_in_samples_done_ < burn_in_samples) {
            burn_in(envelope);
            return;
        }
        if (lockout_samples_left_ > 0) {
            --lockout_samples_left_;
            return;
        }
        if (connections.is_in_lockout) {
            *connections.is_in_lockout = false;
        }
        if (!connections.is_detection_enabled) {
            return;
        }

        const float threshold_multiplier = connections.threshold_multiplier;
        const double threshold =
            static_cast<double>(threshold_multiplier) * mean_absolute_deviation_;
        const double distance = envelope - mean_;
        const bool is_crossing = previous_distance_ <= threshold && distance > threshold;
        previous_distance_ = distance;
        if (is_crossing) {
            lockout_samples_left_ = lockout_samples;
            connections.output.push(connections.input.header, Ripple{});
            if (connections.is_in_lockout) {
                *connections.is_in_lockout = true;
            }
        }
        add_to_statistics(envelope, smoothing_weight);
    }

private:
    static constexpr auto burn_in_samples =
        static_cast<u64>(options.smoothing_time_seconds * InputStreamInfo::rate);
    static constexpr auto lockout_samples =
        static_cast<u64>(options.lockout_milliseconds * InputStreamInfo::rate / 1000.0);
    // What one sample weighs in the running statistics.
    static constexpr double smoothing_weight =
        1.0 / (options.smoothing_time_seconds * InputStreamInfo::rate);

    u64 burn_in_samples_done_ = 0;
    u64 lockout_samples_left_ = 0;
    double mean_ = 0;
    double mean_absolute_deviation_ = 0;
    // Of the last tested sample: it stands still during a lockout and while detection is disabled,
    // so the first sample after a lockout never counts as a crossing.
    double previous_distance_ = 0;

    void burn_in(double envelope) {
        if (burn_in_samples_done_ == 0) {
            log(name, "burning in for {} s, during which nothing is detected",
                options.smoothing_time_seconds);
        }
        ++burn_in_samples_done_;
        // Sample n weighs at least 1/n, so the statistics start as plain averages instead of
        // creeping up from 0.
        const double weight = smoothing_weight + ((1.0 - smoothing_weight) /
                                                  static_cast<double>(burn_in_samples_done_));
        add_to_statistics(envelope, weight);
        if (burn_in_samples_done_ == burn_in_samples) {
            log(name, "burn-in done: the envelope's mean is {} and its mean absolute deviation {}",
                mean_, mean_absolute_deviation_);
        }
    }

    // The deviation is taken from the mean that already includes this sample.
    void add_to_statistics(double envelope, double weight) {
        mean_ = ((1.0 - weight) * mean_) + (weight * envelope);
        mean_absolute_deviation_ =
            ((1.0 - weight) * mean_absolute_deviation_) + (weight * std::abs(envelope - mean_));
    }

    // The mean over the real channels of their squared values. In double, where a float32's square
    // is exact, so a reference computed in float64 reproduces every detection.
    [[nodiscard]] static double mean_square(const SampleInputBuffer<InputStreamInfo>& input) {
        double sum_of_squares = 0;
        // auto, not SimdVector: a type that depends on the template keeps the other instruction
        // set's branch from being compiled.
        if constexpr (HAS_AVX512) {
            __m512d first_half_sum = _mm512_setzero_pd();
            __m512d second_half_sum = _mm512_setzero_pd();
            for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
                 ++simd_vector_index) {
                const auto real_lanes = static_cast<__mmask16>(
                    (1U << static_cast<unsigned>(real_channel_count(simd_vector_index))) - 1U);
                const auto values = _mm512_maskz_mov_ps(real_lanes, input.load(simd_vector_index));
                const __m512d first_half = _mm512_cvtps_pd(_mm512_castps512_ps256(values));
                const __m512d second_half = _mm512_cvtps_pd(
                    _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(values), 1)));
                first_half_sum = _mm512_fmadd_pd(first_half, first_half, first_half_sum);
                second_half_sum = _mm512_fmadd_pd(second_half, second_half, second_half_sum);
            }
            sum_of_squares = _mm512_reduce_add_pd(_mm512_add_pd(first_half_sum, second_half_sum));
        } else {
            const __m256i lane_numbers = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
            __m256d first_half_sum = _mm256_setzero_pd();
            __m256d second_half_sum = _mm256_setzero_pd();
            for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
                 ++simd_vector_index) {
                // All ones in the lanes whose number is below the real channel count.
                const __m256 real_lanes = _mm256_castsi256_ps(_mm256_cmpgt_epi32(
                    _mm256_set1_epi32(real_channel_count(simd_vector_index)), lane_numbers));
                const auto values = _mm256_and_ps(input.load(simd_vector_index), real_lanes);
                const __m256d first_half = _mm256_cvtps_pd(_mm256_castps256_ps128(values));
                const __m256d second_half = _mm256_cvtps_pd(_mm256_extractf128_ps(values, 1));
                first_half_sum = _mm256_fmadd_pd(first_half, first_half, first_half_sum);
                second_half_sum = _mm256_fmadd_pd(second_half, second_half, second_half_sum);
            }
            const __m256d lane_sums = _mm256_add_pd(first_half_sum, second_half_sum);
            const __m128d pair_sums =
                _mm_add_pd(_mm256_castpd256_pd128(lane_sums), _mm256_extractf128_pd(lane_sums, 1));
            sum_of_squares =
                _mm_cvtsd_f64(_mm_add_sd(pair_sums, _mm_unpackhi_pd(pair_sums, pair_sums)));
        }
        return sum_of_squares / static_cast<double>(InputStreamInfo::channels);
    }

    // How many of a channel vector's lanes hold real channels. The others are padding, which holds
    // whatever its producer left in it, so it is zeroed before it is squared.
    [[nodiscard]] static constexpr int real_channel_count(int simd_vector_index) {
        const int first_channel = simd_vector_index * FLOATS_PER_SIMD_VECTOR;
        return std::clamp(InputStreamInfo::channels - first_channel, 0, FLOATS_PER_SIMD_VECTOR);
    }
};
