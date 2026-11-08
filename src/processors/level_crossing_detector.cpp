#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include "../core/engine.cpp"

// LevelCrossingDetector: emits one LevelCrossing for each sample in which any channel crosses the
// level, then ignores the next post_detection_block samples. The direction is its own option, since
// a signal may cross a positive level downward.

struct LevelCrossing : StreamEventRecord {
    u32 channel = 0;  // the lowest channel that crossed in this sample
};

enum class CrossingDirection : std::uint8_t { Upward, Downward };

struct LevelCrossingDetectorOptions {
    double threshold = 0;  // µV, or the stream's unit
    CrossingDirection direction = CrossingDirection::Upward;
    int post_detection_block = 2;  // samples ignored after each detection
};

template <typename InputStreamInfo, LevelCrossingDetectorOptions options>
struct LevelCrossingDetector {
    static_assert(options.post_detection_block >= 0);

    struct Connections {
        SampleInputBuffer<InputStreamInfo> input;
        EventOutputBuffer<LevelCrossing, 1> output;
    };

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        // The sample that ends a block becomes the previous one, so only a transition after the
        // block counts.
        if (blocked_samples_left_ > 0) {
            --blocked_samples_left_;
            if (blocked_samples_left_ == 0) {
                record_beyond_masks(connections);
            }
            return;
        }

        bool has_crossed = false;
        int first_crossing_channel = 0;
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const unsigned beyond_mask =
                beyond_level_mask(connections.input.load(simd_vector_index)) &
                real_channel_mask(simd_vector_index);
            unsigned& was_beyond_mask =
                was_beyond_masks_[static_cast<std::size_t>(simd_vector_index)];
            // The first sample has no previous one to cross from, so it only records the state.
            const unsigned crossing_mask =
                has_seen_sample_ ? (beyond_mask & ~was_beyond_mask) : 0U;
            was_beyond_mask = beyond_mask;
            if (!has_crossed && crossing_mask != 0U) {
                has_crossed = true;
                first_crossing_channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) +
                                         std::countr_zero(crossing_mask);
            }
        }
        has_seen_sample_ = true;

        if (has_crossed) {
            connections.output.push(
                connections.input.header,
                LevelCrossing{.channel = static_cast<u32>(first_crossing_channel)});
            blocked_samples_left_ = options.post_detection_block;
        }
    }

private:
    bool has_seen_sample_ = false;
    int blocked_samples_left_ = 0;
    // One bit per channel: whether its previous sample was beyond the level.
    std::array<unsigned, InputStreamInfo::simd_vectors> was_beyond_masks_{};

    void record_beyond_masks(const Connections& connections) {
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            was_beyond_masks_[static_cast<std::size_t>(simd_vector_index)] =
                beyond_level_mask(connections.input.load(simd_vector_index)) &
                real_channel_mask(simd_vector_index);
        }
    }

    // `auto`, so the AVX512 branch's intrinsics depend on the call and aren't checked in an AVX2
    // build. Ordered comparisons, so a NaN never crosses.
    [[nodiscard]] static unsigned beyond_level_mask(auto values) {
        constexpr auto level = static_cast<float>(options.threshold);
        constexpr int predicate =
            options.direction == CrossingDirection::Upward ? _CMP_GT_OQ : _CMP_LT_OQ;
        if constexpr (HAS_AVX512) {
            return _mm512_cmp_ps_mask(values, _mm512_set1_ps(level), predicate);
        } else {
            return static_cast<unsigned>(
                _mm256_movemask_ps(_mm256_cmp_ps(values, _mm256_set1_ps(level), predicate)));
        }
    }

    // Padding channels never cross, whatever the producer left in them.
    [[nodiscard]] static constexpr unsigned real_channel_mask(int simd_vector_index) {
        const int first_channel = simd_vector_index * FLOATS_PER_SIMD_VECTOR;
        const int real_channels =
            std::clamp(InputStreamInfo::channels - first_channel, 0, FLOATS_PER_SIMD_VECTOR);
        return (1U << static_cast<unsigned>(real_channels)) - 1U;
    }
};
