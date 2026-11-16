#pragma once
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <format>
#include "samples.cpp"

// Engine, part 2 of 5: events, event buffers and channel masks.

// --- Events ---

// Base of every event. Empty: an event from a timer has no sample to point to.
struct Event {};

// Base of every event that comes from a sample; `header` is that sample's header.
struct StreamEvent : Event {
    SampleHeader header;
};

template <std::derived_from<Event> EventType, int Capacity>
struct EventRingWriterEnd;
template <std::derived_from<Event> EventType, int Capacity, int slot_count>
struct EventRing;

// Up to Capacity events produced on this pass, held inside the buffer. Only In, Out and event
// rings reach inside, and it can't be copied, so a processor that holds a buffer directly instead
// of through In or Out can't use it or push into a copy.
template <std::derived_from<Event> EventType, int Capacity>
struct EventBuffer {
    EventBuffer() = default;

    // A buffer whose events also go to other cores, one per ring: when its producer's call
    // returns, the events it pushed are copied into each of these rings.
    template <int... slot_counts>
    explicit EventBuffer(EventRing<EventType, Capacity, slot_counts>&... rings) {
        (feed(rings), ...);
    }

    EventBuffer(const EventBuffer&) = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;
    template <std::derived_from<Event> RingEventType, int RingCapacity, int slot_count>
    friend struct EventRing;

    void feed(EventRingWriterEnd<EventType, Capacity>& ring) {
        ring.join_fed_rings(first_fed_ring_);
        first_fed_ring_ = &ring;
    }

    std::array<EventType, Capacity> events_{};
    int count_ = 0;
    EventRingWriterEnd<EventType, Capacity>* first_fed_ring_ = nullptr;
};

// --- Channel masks ---

// The bits of one channel vector, as its comparison yields them: 16 on AVX512, 8 on AVX2, the
// lowest for the vector's first channel. A type of its own, so set_vector's two arguments can't be
// swapped.
struct ChannelVectorBits {
    unsigned bits = 0;
};

// One bit per channel of a stream, e.g. whether the channel crossed a threshold on this sample.
// Bit c is channel c on every instruction set. Padding channels' bits are always 0, so a padding
// channel can never seem to fire.
template <typename StreamInfoType>
class ChannelMask {
    static constexpr std::size_t word_count =
        (static_cast<std::size_t>(StreamInfoType::padded_channels) + 63) / 64;

public:
    [[nodiscard]] static constexpr ChannelMask every_channel() {
        ChannelMask mask;
        for (std::size_t channel = 0; channel < static_cast<std::size_t>(StreamInfoType::channels);
             ++channel) {
            mask.words_[channel / 64] |= u64{1} << (channel % 64);
        }
        return mask;
    }

    void set_vector(int simd_vector_index, ChannelVectorBits vector) {
        const auto first_channel = static_cast<std::size_t>(simd_vector_index) *
                                   static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR);
        const std::size_t word_index = first_channel / 64;
        const auto shift = static_cast<unsigned>(first_channel % 64);
        const u64 real_bits = static_cast<u64>(vector.bits) & real_channel_bits(first_channel);
        words_[word_index] = (words_[word_index] & ~(vector_bits << shift)) | (real_bits << shift);
    }

    [[nodiscard]] bool is_empty() const { return *this == ChannelMask{}; }

    [[nodiscard]] int count() const {
        int set_bit_count = 0;
        for (const u64 word : words_) {
            set_bit_count += std::popcount(word);
        }
        return set_bit_count;
    }

    [[nodiscard]] bool has_channel(u32 channel) const {
        if (channel >= static_cast<u32>(StreamInfoType::channels)) {
            return false;
        }
        return ((words_[channel / 64] >> (channel % 64)) & 1U) != 0;
    }

    ChannelMask& operator|=(const ChannelMask& other) {
        for (std::size_t word_index = 0; word_index < word_count; ++word_index) {
            words_[word_index] |= other.words_[word_index];
        }
        return *this;
    }

    [[nodiscard]] ChannelMask operator&(const ChannelMask& other) const {
        ChannelMask result = *this;
        for (std::size_t word_index = 0; word_index < word_count; ++word_index) {
            result.words_[word_index] &= other.words_[word_index];
        }
        return result;
    }

    [[nodiscard]] bool operator==(const ChannelMask& other) const = default;

    // Walks the channel numbers of the set bits, lowest first: `for (u32 channel : mask)`.
    class ChannelIterator {
    public:
        explicit ChannelIterator(const std::array<u64, word_count>& words)
            : words_(&words), remaining_bits_(words[0]) {
            skip_empty_words();
        }

        [[nodiscard]] u32 operator*() const {
            return static_cast<u32>((word_index_ * 64) +
                                    static_cast<std::size_t>(std::countr_zero(remaining_bits_)));
        }

        ChannelIterator& operator++() {
            remaining_bits_ &= remaining_bits_ - 1;  // clears the lowest set bit
            skip_empty_words();
            return *this;
        }

        [[nodiscard]] bool operator==(std::default_sentinel_t /*end*/) const {
            return word_index_ == word_count;
        }

    private:
        const std::array<u64, word_count>* words_;
        std::size_t word_index_ = 0;
        u64 remaining_bits_ = 0;

        void skip_empty_words() {
            while (remaining_bits_ == 0) {
                ++word_index_;
                if (word_index_ == word_count) {
                    return;
                }
                remaining_bits_ = (*words_)[word_index_];
            }
        }
    };

    [[nodiscard]] ChannelIterator begin() const { return ChannelIterator(words_); }
    [[nodiscard]] std::default_sentinel_t end() const { return {}; }

private:
    static constexpr u64 vector_bits =
        (u64{1} << static_cast<unsigned>(FLOATS_PER_SIMD_VECTOR)) - 1;

    std::array<u64, word_count> words_{};

    // The bits of a channel vector's real channels, from its first channel on.
    [[nodiscard]] static constexpr u64 real_channel_bits(std::size_t first_channel) {
        const auto channel_count = static_cast<std::size_t>(StreamInfoType::channels);
        if (first_channel >= channel_count) {
            return 0;
        }
        const std::size_t real_channels = channel_count - first_channel;
        if (real_channels >= static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)) {
            return vector_bits;
        }
        return (u64{1} << real_channels) - 1;
    }
};

// An event that holds a mask of a stream's channels, such as Spikes or LevelCrossings.
template <typename EventType, typename StreamInfoType>
concept ChannelMaskEvent =
    std::derived_from<EventType, StreamEvent> && requires(const EventType& event) {
        { event.channels } -> std::same_as<const ChannelMask<StreamInfoType>&>;
    };

// A message may carry a channel mask.
template <typename StreamInfoType>
inline constexpr bool IS_CHANNEL_MASK<ChannelMask<StreamInfoType>> = true;

// Lists a mask's channels, e.g. "3, 17, 40".
template <typename StreamInfoType>
struct std::formatter<ChannelMask<StreamInfoType>> {
    static constexpr std::format_parse_context::iterator parse(std::format_parse_context& context) {
        return context.begin();
    }

    static std::format_context::iterator format(const ChannelMask<StreamInfoType>& mask,
                                                std::format_context& context) {
        std::format_context::iterator output = context.out();
        bool is_first = true;
        for (const u32 channel : mask) {
            output = is_first ? std::format_to(output, "{}", channel)
                              : std::format_to(output, ", {}", channel);
            is_first = false;
        }
        if (is_first) {
            output = std::format_to(output, "none");
        }
        return output;
    }
};
