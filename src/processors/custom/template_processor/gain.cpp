#pragma once
#include "../../../core/engine.cpp"

namespace template_processor {

// Gain: multiplies every channel by a constant factor.

struct GainOptions {
    /// The factor every channel's value is multiplied by. 1 leaves the signal as it is, and a
    /// negative factor turns it upside down.
    double factor = 1;
};

/// Multiplies every channel by a constant factor. It is the example a lab starts its own processor
/// from.
template <typename InputStreamInfo, GainOptions options>
struct Gain {
    struct Connections {
        /// The signal to multiply.
        SampleInputBuffer<InputStreamInfo> input;
        /// The multiplied signal: one sample for each input sample, with the same channels and
        /// rate.
        SampleOutputBuffer<InputStreamInfo> output;
    };

    explicit Gain(ProcessorName processor_name) : name(processor_name) {}

    RegisteredName name;

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        connections.output.set_header(connections.input.header);
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            // auto, not SimdVector: a type that depends on the template keeps the other
            // instruction set's branch from being compiled.
            const auto values = connections.input.load(simd_vector_index);
            if constexpr (HAS_AVX512) {
                connections.output.store(simd_vector_index,
                                         _mm512_mul_ps(values, _mm512_set1_ps(factor)));
            } else {
                connections.output.store(simd_vector_index,
                                         _mm256_mul_ps(values, _mm256_set1_ps(factor)));
            }
        }
    }

private:
    static constexpr auto factor = static_cast<float>(options.factor);
};

}  // namespace template_processor
