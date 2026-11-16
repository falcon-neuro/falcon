#pragma once
#include <immintrin.h>
#include <array>
#include <concepts>
#include <cstddef>
#include <string>
#include <type_traits>
#include "logger.cpp"

// Engine, part 1 of 5: the instruction set, stream types, a sample's header and sample buffers.

// --- Instruction set ---

#ifndef __AVX2__
#error "Falcon needs AVX2: compile with -mavx2 or -march=native"
#endif

#ifndef __FMA__
#error "Falcon needs FMA: compile with -mfma or -march=native"
#endif

#ifdef __AVX512F__
inline constexpr bool HAS_AVX512 = true;
#else
inline constexpr bool HAS_AVX512 = false;
#endif

inline constexpr int FLOATS_PER_SIMD_VECTOR = HAS_AVX512 ? 16 : 8;
using SimdVector = std::conditional_t<HAS_AVX512, __m512, __m256>;

// --- String options ---

// A string a template argument can hold: std::string and pointers to string literals can't be one.
struct Str {
    static constexpr std::size_t capacity = 256;  // bytes, the terminating zero included
    std::array<char, capacity> characters{};

    constexpr Str() = default;

    // consteval, so a string longer than 255 characters is a compile error, never cut.
    consteval Str(const char* text) {
        std::size_t length = 0;
        while (text[length] != '\0') {
            if (length + 1 >= capacity) {
                throw std::length_error("a string option holds at most 255 characters");
            }
            characters[length] = text[length];
            ++length;
        }
    }

    [[nodiscard]] constexpr const char* c_str() const { return characters.data(); }
};

// --- Streams and samples ---

template <int ChannelCount, double SampleRate, typename SampleType>
struct StreamInfo {
    static_assert(ChannelCount > 0);
    static_assert(SampleRate > 0);
    static_assert(std::same_as<SampleType, float>, "only float32 samples are implemented");

    static constexpr int channels = ChannelCount;
    static constexpr double rate = SampleRate;  // Hz
    using ChannelValue = SampleType;
    // 16 floats are one AVX512 vector, so SIMD loops have no leftover channels.
    static constexpr int padded_channels = (ChannelCount + 15) / 16 * 16;
    static constexpr int simd_vectors = padded_channels / FLOATS_PER_SIMD_VECTOR;
};

// Set by the input processor. Other processors pass it on and never make one up.
struct SampleHeader {
    u64 sample_number = 0;     // counted at the input's rate, derived from the device's clock
    u64 device_timestamp = 0;  // in the device's own unit; processors never calculate with it
    u64 arrival_tsc = 0;       // CPU timestamp counter when the sample arrived
    // Samples lost upstream since the start: the input adds those the device skipped, each ring
    // those it dropped. It only grows, so any later header carries every earlier loss; a processor
    // sees one when the count differs from its previous sample's.
    u64 lost_sample_count = 0;
};

template <typename StreamInfoType>
struct SampleWithHeader {
    SampleHeader header;
    alignas(64)
        std::array<typename StreamInfoType::ChannelValue, StreamInfoType::padded_channels> values;
};

template <typename StreamInfoType>
struct SampleRingWriterEnd;
template <typename StreamInfoType, int slot_count>
struct SampleRing;

// 0 or 1 sample. `sample_` points at the buffer's own storage, or at a ring slot, so nothing is
// copied across rings. Only In, Out and rings reach inside, so a processor that holds a buffer
// directly instead of through In or Out can't use it.
template <typename StreamInfoType>
struct SampleBuffer {
    SampleBuffer() = default;

    // A buffer whose sample also goes to other cores, one per ring: when its producer's call
    // returns, the sample is copied into each of these rings.
    template <int... slot_counts>
    explicit SampleBuffer(SampleRing<StreamInfoType, slot_counts>&... rings) {
        (feed(rings), ...);
    }

    // A copy's `sample_` would still point at the original's storage.
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;
    template <typename RingStreamInfoType>
    friend struct SampleRingWriterEnd;
    template <typename RingStreamInfoType, int slot_count>
    friend struct SampleRing;

    void feed(SampleRingWriterEnd<StreamInfoType>& ring) {
        ring.join_fed_rings(first_fed_ring_);
        first_fed_ring_ = &ring;
    }

    SampleWithHeader<StreamInfoType> storage_{};
    SampleWithHeader<StreamInfoType>* sample_ = &storage_;
    int count_ = 0;
    SampleRingWriterEnd<StreamInfoType>* first_fed_ring_ = nullptr;
};
