#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include "../core/engine.cpp"

// Filter: an IIR filter on every channel, as a cascade of second-order (biquad) sections in
// Transposed Direct Form II, in float32. The coefficients are designed offline; the generator reads
// them from a coefficients file into graph.cpp (see Filter in spec.md).

// H(z) = (b0 + b1·z⁻¹ + b2·z⁻²) / (1 + a1·z⁻¹ + a2·z⁻²), with a0 already divided out.
struct BiquadSection {
    double b0 = 0;
    double b1 = 0;
    double b2 = 0;
    double a1 = 0;
    double a2 = 0;
};

template <int SectionCount>
struct FilterOptions {
    double design_rate = 0;  // Hz, the sampling rate the coefficients were designed for
    double gain = 1;         // for the whole cascade, as in Falcon 1's coefficients files
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

template <typename InputStreamInfo, FilterOptions options>
struct Filter {
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

        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const std::size_t first_channel = static_cast<std::size_t>(simd_vector_index) *
                                              static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR);
            auto values = connections.input.load(simd_vector_index);

            for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
                const FloatSection& section = float_sections[section_index];
                float* const state_z1 = &state_[section_index].z1[first_channel];
                float* const state_z2 = &state_[section_index].z2[first_channel];

                // Transposed Direct Form II:
                //   y  = b0·x + z1
                //   z1 = b1·x − a1·y + z2
                //   z2 = b2·x − a2·y
                // On the first sample, the state is set as if this input had always been there, so
                // the output starts at its steady value (0 for a band-pass) instead of jumping: a
                // DC offset would otherwise give every channel a start-up transient that a spike
                // detector reads as spikes.
                if constexpr (HAS_AVX512) {
                    const __m512 coefficient_b0 = _mm512_set1_ps(section.b0);
                    const __m512 coefficient_b1 = _mm512_set1_ps(section.b1);
                    const __m512 coefficient_b2 = _mm512_set1_ps(section.b2);
                    const __m512 coefficient_a1 = _mm512_set1_ps(section.a1);
                    const __m512 coefficient_a2 = _mm512_set1_ps(section.a2);
                    if (!has_initialized_state_) {
                        const __m512 steady_output =
                            _mm512_mul_ps(_mm512_set1_ps(section.dc_gain), values);
                        _mm512_store_ps(state_z1,
                                        _mm512_fnmadd_ps(coefficient_b0, values, steady_output));
                        _mm512_store_ps(state_z2,
                                        _mm512_fnmadd_ps(coefficient_a2, steady_output,
                                                         _mm512_mul_ps(coefficient_b2, values)));
                    }
                    const __m512 previous_z1 = _mm512_load_ps(state_z1);
                    const __m512 previous_z2 = _mm512_load_ps(state_z2);
                    const __m512 filtered = _mm512_fmadd_ps(coefficient_b0, values, previous_z1);
                    _mm512_store_ps(
                        state_z1,
                        _mm512_fmadd_ps(coefficient_b1, values,
                                        _mm512_fnmadd_ps(coefficient_a1, filtered, previous_z2)));
                    _mm512_store_ps(state_z2,
                                    _mm512_fnmadd_ps(coefficient_a2, filtered,
                                                     _mm512_mul_ps(coefficient_b2, values)));
                    values = filtered;
                } else {
                    const __m256 coefficient_b0 = _mm256_set1_ps(section.b0);
                    const __m256 coefficient_b1 = _mm256_set1_ps(section.b1);
                    const __m256 coefficient_b2 = _mm256_set1_ps(section.b2);
                    const __m256 coefficient_a1 = _mm256_set1_ps(section.a1);
                    const __m256 coefficient_a2 = _mm256_set1_ps(section.a2);
                    if (!has_initialized_state_) {
                        const __m256 steady_output =
                            _mm256_mul_ps(_mm256_set1_ps(section.dc_gain), values);
                        _mm256_store_ps(state_z1,
                                        _mm256_fnmadd_ps(coefficient_b0, values, steady_output));
                        _mm256_store_ps(state_z2,
                                        _mm256_fnmadd_ps(coefficient_a2, steady_output,
                                                         _mm256_mul_ps(coefficient_b2, values)));
                    }
                    const __m256 previous_z1 = _mm256_load_ps(state_z1);
                    const __m256 previous_z2 = _mm256_load_ps(state_z2);
                    const __m256 filtered = _mm256_fmadd_ps(coefficient_b0, values, previous_z1);
                    _mm256_store_ps(
                        state_z1,
                        _mm256_fmadd_ps(coefficient_b1, values,
                                        _mm256_fnmadd_ps(coefficient_a1, filtered, previous_z2)));
                    _mm256_store_ps(state_z2,
                                    _mm256_fnmadd_ps(coefficient_a2, filtered,
                                                     _mm256_mul_ps(coefficient_b2, values)));
                    values = filtered;
                }
            }
            connections.output.store(simd_vector_index, values);
        }
        has_initialized_state_ = true;
    }

private:
    // dc_gain is H(1), the section's output for a constant input of 1: what the state starts from.
    struct FloatSection {
        float b0 = 0;
        float b1 = 0;
        float b2 = 0;
        float a1 = 0;
        float a2 = 0;
        float dc_gain = 0;
    };

    // The cascade's gain is folded into the first section's b coefficients, which saves a
    // multiply per channel vector. 1 + a1 + a2 is never 0 for a stable section.
    static constexpr std::array<FloatSection, section_count> float_sections = [] {
        std::array<FloatSection, section_count> result{};
        for (std::size_t section_index = 0; section_index < section_count; ++section_index) {
            const BiquadSection& section = options.sections[section_index];
            const double gain = section_index == 0 ? options.gain : 1.0;
            const double scaled_b0 = gain * section.b0;
            const double scaled_b1 = gain * section.b1;
            const double scaled_b2 = gain * section.b2;
            result[section_index] = {
                .b0 = static_cast<float>(scaled_b0),
                .b1 = static_cast<float>(scaled_b1),
                .b2 = static_cast<float>(scaled_b2),
                .a1 = static_cast<float>(section.a1),
                .a2 = static_cast<float>(section.a2),
                .dc_gain = static_cast<float>((scaled_b0 + scaled_b1 + scaled_b2) /
                                              (1.0 + section.a1 + section.a2)),
            };
        }
        return result;
    }();

    struct SectionState {
        alignas(64) std::array<float, InputStreamInfo::padded_channels> z1{};
        alignas(64) std::array<float, InputStreamInfo::padded_channels> z2{};
    };

    std::array<SectionState, section_count> state_{};
    bool has_initialized_state_ = false;
};
