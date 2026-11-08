#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <type_traits>
#include "../core/engine.cpp"

// IirFilter: an IIR filter on every channel, a cascade of second-order (biquad) sections in
// Transposed Direct Form II, computed in double; its input and output are float32.

// H(z) = (b0 + b1·z⁻¹ + b2·z⁻²) / (1 + a1·z⁻¹ + a2·z⁻²), with a0 already divided out.
struct BiquadSection {
    double b0 = 0;
    double b1 = 0;
    double b2 = 0;
    double a1 = 0;
    double a2 = 0;
};

template <int SectionCount>
struct IirFilterOptions {
    double design_rate = 0;  // Hz, the sampling rate the coefficients were designed for
    double gain = 1;         // for the whole cascade
    std::array<BiquadSection, SectionCount> sections{};
};

// Both poles of a section lie inside the unit circle exactly when these hold. A mistyped
// coefficient usually breaks them, and an unstable filter's output grows without bound. A NaN or
// infinite coefficient fails them too.
template <std::size_t SectionCount>
[[nodiscard]] constexpr bool is_every_biquad_section_stable(
    const std::array<BiquadSection, SectionCount>& sections) {
    return std::ranges::all_of(sections, [](const BiquadSection& section) {
        return section.a2 < 1.0 && section.a2 > -1.0 && section.a1 < 1.0 + section.a2 &&
               section.a1 > -1.0 - section.a2;
    });
}

template <typename InputStreamInfo, IirFilterOptions options>
struct IirFilter {
    static constexpr std::size_t section_count = options.sections.size();

    static_assert(section_count > 0);
    static_assert(options.design_rate == InputStreamInfo::rate,
                  "the coefficients were designed for another sampling rate, so every cutoff "
                  "would shift");
    static_assert(is_every_biquad_section_stable(options.sections),
                  "a filter section is unstable: check its a1 and a2");

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
                const __m256 filtered_first_half =
                    _mm512_cvtpd_ps(run_sections(first_half, first_channel, is_restarting));
                const __m256 filtered_second_half = _mm512_cvtpd_ps(
                    run_sections(second_half, second_half_first_channel, is_restarting));
                const __m512d filtered_halves = _mm512_insertf64x4(
                    _mm512_castps_pd(_mm512_castps256_ps512(filtered_first_half)),
                    _mm256_castps_pd(filtered_second_half), 1);
                connections.output.store(simd_vector_index, _mm512_castpd_ps(filtered_halves));
            } else {
                const __m256d first_half = _mm256_cvtps_pd(_mm256_castps256_ps128(values));
                const __m256d second_half = _mm256_cvtps_pd(_mm256_extractf128_ps(values, 1));
                const __m128 filtered_first_half =
                    _mm256_cvtpd_ps(run_sections(first_half, first_channel, is_restarting));
                const __m128 filtered_second_half = _mm256_cvtpd_ps(
                    run_sections(second_half, second_half_first_channel, is_restarting));
                connections.output.store(
                    simd_vector_index, _mm256_set_m128(filtered_second_half, filtered_first_half));
            }
        }
    }

private:
    // dc_gain is H(1), the section's output for a constant input of 1: what a start sets the state
    // from. The cascade's gain is folded into the first section's b coefficients, which saves a
    // multiply per channel vector. 1 + a1 + a2 is never 0 for a stable section.
    struct ScaledSection {
        double b0 = 0;
        double b1 = 0;
        double b2 = 0;
        double a1 = 0;
        double a2 = 0;
        double dc_gain = 0;
    };

    static constexpr std::array<ScaledSection, section_count> scaled_sections = [] {
        std::array<ScaledSection, section_count> result{};
        for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
            const BiquadSection& section = options.sections[section_index];
            const double gain = section_index == 0 ? options.gain : 1.0;
            const double scaled_b0 = gain * section.b0;
            const double scaled_b1 = gain * section.b1;
            const double scaled_b2 = gain * section.b2;
            result[section_index] = {
                .b0 = scaled_b0,
                .b1 = scaled_b1,
                .b2 = scaled_b2,
                .a1 = section.a1,
                .a2 = section.a2,
                .dc_gain = (scaled_b0 + scaled_b1 + scaled_b2) / (1.0 + section.a1 + section.a2),
            };
        }
        return result;
    }();

    struct SectionState {
        alignas(64) std::array<double, InputStreamInfo::padded_channels> z1{};
        alignas(64) std::array<double, InputStreamInfo::padded_channels> z2{};
    };

    std::array<SectionState, section_count> state_{};
    bool has_started_ = false;
    u64 previous_lost_sample_count_ = 0;

    // Runs one vector of doubles, the channels from first_channel on, through every section.
    template <typename DoubleVector>
    [[nodiscard]] DoubleVector run_sections(DoubleVector values, std::size_t first_channel,
                                            bool is_restarting) {
        for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
            const ScaledSection& section = scaled_sections[section_index];
            double* const state_z1 = &state_[section_index].z1[first_channel];
            double* const state_z2 = &state_[section_index].z2[first_channel];

            // Transposed Direct Form II:
            //   y  = b0·x + z1
            //   z1 = b1·x − a1·y + z2
            //   z2 = b2·x − a2·y
            // A start sets the state as if this input had always been there, so the output starts
            // at its steady value (0 for a band-pass) instead of jumping: a DC offset would
            // otherwise give every channel a transient that a spike detector reads as spikes.
            if constexpr (std::is_same_v<DoubleVector, __m512d>) {
                const __m512d coefficient_b0 = _mm512_set1_pd(section.b0);
                const __m512d coefficient_b1 = _mm512_set1_pd(section.b1);
                const __m512d coefficient_b2 = _mm512_set1_pd(section.b2);
                const __m512d coefficient_a1 = _mm512_set1_pd(section.a1);
                const __m512d coefficient_a2 = _mm512_set1_pd(section.a2);
                if (is_restarting) {
                    const __m512d steady_output =
                        _mm512_mul_pd(_mm512_set1_pd(section.dc_gain), values);
                    _mm512_store_pd(state_z1,
                                    _mm512_fnmadd_pd(coefficient_b0, values, steady_output));
                    _mm512_store_pd(state_z2,
                                    _mm512_fnmadd_pd(coefficient_a2, steady_output,
                                                     _mm512_mul_pd(coefficient_b2, values)));
                }
                const __m512d previous_z1 = _mm512_load_pd(state_z1);
                const __m512d previous_z2 = _mm512_load_pd(state_z2);
                const __m512d filtered = _mm512_fmadd_pd(coefficient_b0, values, previous_z1);
                _mm512_store_pd(
                    state_z1,
                    _mm512_fmadd_pd(coefficient_b1, values,
                                    _mm512_fnmadd_pd(coefficient_a1, filtered, previous_z2)));
                _mm512_store_pd(state_z2,
                                _mm512_fnmadd_pd(coefficient_a2, filtered,
                                                 _mm512_mul_pd(coefficient_b2, values)));
                values = filtered;
            } else {
                static_assert(std::is_same_v<DoubleVector, __m256d>);
                const __m256d coefficient_b0 = _mm256_set1_pd(section.b0);
                const __m256d coefficient_b1 = _mm256_set1_pd(section.b1);
                const __m256d coefficient_b2 = _mm256_set1_pd(section.b2);
                const __m256d coefficient_a1 = _mm256_set1_pd(section.a1);
                const __m256d coefficient_a2 = _mm256_set1_pd(section.a2);
                if (is_restarting) {
                    const __m256d steady_output =
                        _mm256_mul_pd(_mm256_set1_pd(section.dc_gain), values);
                    _mm256_store_pd(state_z1,
                                    _mm256_fnmadd_pd(coefficient_b0, values, steady_output));
                    _mm256_store_pd(state_z2,
                                    _mm256_fnmadd_pd(coefficient_a2, steady_output,
                                                     _mm256_mul_pd(coefficient_b2, values)));
                }
                const __m256d previous_z1 = _mm256_load_pd(state_z1);
                const __m256d previous_z2 = _mm256_load_pd(state_z2);
                const __m256d filtered = _mm256_fmadd_pd(coefficient_b0, values, previous_z1);
                _mm256_store_pd(
                    state_z1,
                    _mm256_fmadd_pd(coefficient_b1, values,
                                    _mm256_fnmadd_pd(coefficient_a1, filtered, previous_z2)));
                _mm256_store_pd(state_z2,
                                _mm256_fnmadd_pd(coefficient_a2, filtered,
                                                 _mm256_mul_pd(coefficient_b2, values)));
                values = filtered;
            }
        }
        return values;
    }
};
