#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include "../core/engine.cpp"

// SpikeDetector: on each sample where channels cross the threshold, emits one Spikes event with
// those channels, without waiting for the peak. The threshold's sign gives the direction.

// One event per sample: the mask of the channels that crossed the threshold on it.
template <typename StreamInfoType>
struct Spikes : StreamEvent {
    ChannelMask<StreamInfoType> channels;
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
        EventOutputBuffer<Spikes<InputStreamInfo>, 1> output;
    };

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        ChannelMask<InputStreamInfo> crossings;
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const unsigned beyond_mask =
                beyond_threshold_mask(connections.input.load(simd_vector_index)) &
                real_channel_mask(simd_vector_index);
            unsigned& was_beyond_mask =
                was_beyond_masks_[static_cast<std::size_t>(simd_vector_index)];
            // The first sample has no previous one to cross from, so it only records the state.
            const unsigned crossing_mask = has_seen_sample_ ? (beyond_mask & ~was_beyond_mask) : 0U;
            was_beyond_mask = beyond_mask;
            crossings.set_vector(simd_vector_index, {.bits = crossing_mask});
        }
        has_seen_sample_ = true;
        if (!crossings.is_empty()) {
            connections.output.push(connections.input.header,
                                    Spikes<InputStreamInfo>{.channels = crossings});
        }
    }

private:
    bool has_seen_sample_ = false;
    // One bit per channel: whether its previous sample was beyond the threshold.
    std::array<unsigned, InputStreamInfo::simd_vectors> was_beyond_masks_{};

    // `auto`, so the AVX512 branch's intrinsics depend on the call and aren't checked in an AVX2
    // build. Ordered comparisons, so a NaN never crosses.
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
