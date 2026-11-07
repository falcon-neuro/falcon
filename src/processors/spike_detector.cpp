#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include "../core/engine.cpp"

// SpikeDetector: emits a Spike on the sample where a channel crosses the threshold, without waiting
// for the peak (see The prototype's SpikeDetector in spec.md). Unlike Falcon 1's, the threshold's
// sign gives the direction, so there is no option to invert the signal.

struct Spike : StreamEventRecord {
    u32 channel = 0;
};

struct SpikeDetectorOptions {
    double threshold = -50;  // µV: negative detects downward crossings, positive upward ones
};

template <typename InputStreamInfo, SpikeDetectorOptions options>
struct SpikeDetector {
    static_assert(options.threshold != 0,
                  "the threshold's sign gives the direction, so it can't be 0");

    struct Connections {
        SampleInputBuffer<InputStreamInfo> input;
        // Room for every channel crossing in the same sample, e.g. during a stimulation artifact.
        EventOutputBuffer<Spike, InputStreamInfo::channels> output;
    };

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const unsigned beyond_mask =
                beyond_threshold_mask(connections.input.load(simd_vector_index)) &
                real_channel_mask(simd_vector_index);
            unsigned& was_beyond_mask =
                was_beyond_masks_[static_cast<std::size_t>(simd_vector_index)];
            // The first sample has no previous one to cross from, so it only records the state.
            unsigned crossing_mask = has_seen_sample_ ? (beyond_mask & ~was_beyond_mask) : 0U;
            was_beyond_mask = beyond_mask;

            while (crossing_mask != 0U) {
                const int lane_index = std::countr_zero(crossing_mask);
                crossing_mask &= crossing_mask - 1U;
                const int channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) + lane_index;
                connections.output.push(connections.input.header,
                                        Spike{.channel = static_cast<u32>(channel)});
            }
        }
        has_seen_sample_ = true;
    }

private:
    bool has_seen_sample_ = false;
    // One bit per channel: whether its previous sample was beyond the threshold.
    std::array<unsigned, InputStreamInfo::simd_vectors> was_beyond_masks_{};

    // `auto`, so the AVX512 branch's intrinsics depend on the call and aren't checked in an AVX2
    // build (see One processor per stream in spec.md). Ordered comparisons, so a NaN never crosses.
    [[nodiscard]] static unsigned beyond_threshold_mask(auto values) {
        constexpr auto threshold = static_cast<float>(options.threshold);
        constexpr int predicate = options.threshold < 0 ? _CMP_LT_OQ : _CMP_GT_OQ;
        if constexpr (HAS_AVX512) {
            return _mm512_cmp_ps_mask(values, _mm512_set1_ps(threshold), predicate);
        } else {
            return static_cast<unsigned>(
                _mm256_movemask_ps(_mm256_cmp_ps(values, _mm256_set1_ps(threshold), predicate)));
        }
    }

    // Padding channels never produce spikes, whatever the producer left in them.
    [[nodiscard]] static constexpr unsigned real_channel_mask(int simd_vector_index) {
        const int first_channel = simd_vector_index * FLOATS_PER_SIMD_VECTOR;
        const int real_channels =
            std::clamp(InputStreamInfo::channels - first_channel, 0, FLOATS_PER_SIMD_VECTOR);
        return (1U << static_cast<unsigned>(real_channels)) - 1U;
    }
};
