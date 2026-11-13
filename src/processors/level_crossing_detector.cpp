#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "../core/engine.cpp"

// LevelCrossingDetector: emits a LevelCrossing for each channel that crosses the level, then ignores
// that channel for its next post_detection_block samples. Blocks are per channel, so channels that
// cross a sample apart are all detected. The direction is its own option, since a signal may cross
// a positive level downward.

struct LevelCrossing : StreamEvent {
    u32 channel = 0;
};

enum class CrossingDirection : std::uint8_t { Upward, Downward };

struct LevelCrossingDetectorOptions {
    double threshold = 0;  // µV, or the stream's unit
    CrossingDirection direction = CrossingDirection::Upward;
    int post_detection_block = 2;  // samples a channel is ignored for after each of its crossings
};

template <typename InputStreamInfo, LevelCrossingDetectorOptions options>
struct LevelCrossingDetector {
    static_assert(options.post_detection_block >= 0);

    struct Connections {
        SampleInputBuffer<InputStreamInfo> input;
        // Room for every channel crossing in the same sample, e.g. at a test square wave's edge.
        EventOutputBuffer<LevelCrossing, InputStreamInfo::channels> output;
    };

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const auto vector_slot = static_cast<std::size_t>(simd_vector_index);
            const unsigned beyond_mask =
                beyond_level_mask(connections.input.load(simd_vector_index)) &
                real_channel_mask(simd_vector_index);
            // The first sample has no previous one to cross from, so it only records the state.
            const unsigned transition_mask =
                has_seen_sample_ ? (beyond_mask & ~was_beyond_masks_[vector_slot]) : 0U;
            // Recorded during a block too, so the sample that ends a channel's block is the one its
            // next transition counts from.
            was_beyond_masks_[vector_slot] = beyond_mask;

            unsigned crossing_mask = transition_mask;
            if constexpr (options.post_detection_block > 0) {
                crossing_mask = apply_blocks(blocked_samples_left_[vector_slot], transition_mask);
            }
            while (crossing_mask != 0U) {
                const int lane_index = std::countr_zero(crossing_mask);
                crossing_mask &= crossing_mask - 1U;
                const int channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) + lane_index;
                connections.output.push(connections.input.header,
                                        LevelCrossing{.channel = static_cast<u32>(channel)});
            }
        }
        has_seen_sample_ = true;
    }

private:
    // One 32-bit lane per channel, in the same lane as the channel's sample.
    using BlockCounterVector = std::conditional_t<HAS_AVX512, __m512i, __m256i>;

    bool has_seen_sample_ = false;
    // One bit per channel: whether its previous sample was beyond the level.
    std::array<unsigned, InputStreamInfo::simd_vectors> was_beyond_masks_{};
    // Per channel: how many of its next samples are still ignored.
    std::array<BlockCounterVector, InputStreamInfo::simd_vectors> blocked_samples_left_{};

    // Returns the transitions of the channels that aren't blocked, counts every block down by one
    // sample and starts a block for each channel that crossed. `auto`, so the AVX512 branch's
    // intrinsics depend on the call and aren't checked in an AVX2 build.
    [[nodiscard]] static unsigned apply_blocks(auto& samples_left, unsigned transition_mask) {
        if constexpr (HAS_AVX512) {
            const __m512i zero = _mm512_setzero_si512();
            const unsigned blocked_mask = _mm512_cmpgt_epi32_mask(samples_left, zero);
            const unsigned crossing_mask = transition_mask & ~blocked_mask;
            const __m512i counted_down =
                _mm512_max_epi32(_mm512_sub_epi32(samples_left, _mm512_set1_epi32(1)), zero);
            samples_left = _mm512_mask_set1_epi32(
                counted_down, static_cast<__mmask16>(crossing_mask), options.post_detection_block);
            return crossing_mask;
        } else {
            const __m256i zero = _mm256_setzero_si256();
            const auto blocked_mask = static_cast<unsigned>(
                _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(samples_left, zero))));
            const unsigned crossing_mask = transition_mask & ~blocked_mask;
            const __m256i counted_down =
                _mm256_max_epi32(_mm256_sub_epi32(samples_left, _mm256_set1_epi32(1)), zero);
            // Lane n holds bit n, so the comparison widens each bit of the mask to its whole lane.
            const __m256i lane_bits = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);
            const __m256i crossing_lanes = _mm256_cmpeq_epi32(
                _mm256_and_si256(_mm256_set1_epi32(static_cast<int>(crossing_mask)), lane_bits),
                lane_bits);
            samples_left = _mm256_blendv_epi8(
                counted_down, _mm256_set1_epi32(options.post_detection_block), crossing_lanes);
            return crossing_mask;
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
