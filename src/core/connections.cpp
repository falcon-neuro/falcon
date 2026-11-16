#pragma once
#include <immintrin.h>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include "events.cpp"

// Engine, part 3 of 5: state values, and the In and Out a processor's Connections is made of.

// --- State values ---

// A number, bool or enum, so a state value fits in one register.
template <typename StateType>
concept StateValueType = std::is_arithmetic_v<StateType> || std::is_enum_v<StateType>;

// The producer's StateOutput fixes initial_value, so pipeline.cpp can't declare a different one.
template <StateValueType StateType, StateType initial_value>
struct StateValue {
    StateType value = initial_value;
};

// A state value whose producer and readers are on different cores.
template <StateValueType StateType, StateType initial_value>
struct CrossCoreStateValue {
    static_assert(std::atomic<StateType>::is_always_lock_free);

    alignas(BYTES_BETWEEN_CORES) std::atomic<StateType> value = initial_value;
};

// Whether two state values hold the same bits. Unlike ==, it tells 0.0 from -0.0 and finds a NaN
// equal to itself.
template <StateValueType StateType>
[[nodiscard]] bool is_bitwise_equal(StateType first_value, StateType second_value) {
    using Bytes = std::array<std::byte, sizeof(StateType)>;
    return std::bit_cast<Bytes>(first_value) == std::bit_cast<Bytes>(second_value);
}

// What a processor initializes a state input with, so pipeline.cpp may leave it out of the call:
// the build rejects a call that leaves out a member without an initializer of its own.
struct Unconnected {};
inline constexpr Unconnected UNCONNECTED{};

// --- In and Out ---
// process() takes one struct, Connections, declared inside its processor. Every member of it is an
// In<Kind> or an Out<Kind>, and each exposes only what its direction allows.

template <typename Kind>
struct In;
template <typename Kind>
struct Out;

template <typename StreamInfoType>
struct In<SampleBuffer<StreamInfoType>> {
    In(const SampleBuffer<StreamInfoType>& samples)
        : header(samples.sample_->header), buffer_(samples) {}

    // A connection from another core: the ring's next sample, fetched once per pass.
    template <int slot_count>
    In(SampleRing<StreamInfoType, slot_count>& ring) : In(ring.reader_buffer()) {}

    const SampleHeader& header;

    [[nodiscard]] bool is_empty() const { return buffer_.count_ == 0; }

    [[nodiscard]] SimdVector load(int simd_vector_index) const {
        const float* address =
            &buffer_.sample_->values[static_cast<std::size_t>(simd_vector_index) *
                                     static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
        // Not if constexpr: nothing here depends on StreamInfoType, so the discarded branch would
        // still be type-checked, and an __m512 doesn't convert to the AVX2 build's SimdVector.
#ifdef __AVX512F__
        return _mm512_load_ps(address);
#else
        return _mm256_load_ps(address);
#endif
    }

private:
    const SampleBuffer<StreamInfoType>& buffer_;
};

template <typename StreamInfoType>
struct Out<SampleBuffer<StreamInfoType>> {
    // Clears the buffer right before its producer runs.
    Out(SampleBuffer<StreamInfoType>& samples)
        : buffer_(samples), first_ring_(samples.first_fed_ring_) {
        buffer_.count_ = 0;
    }

    // A connection to another core: the producer writes straight into the ring's next free slot.
    template <int slot_count>
    Out(SampleRing<StreamInfoType, slot_count>& ring) : Out(ring.next_slot()) {
        first_ring_ = &ring;
        is_ring_slot_ = true;
    }

    // Runs when the producer's call returns, so a sample for another core leaves right after its
    // producer, with no call in pipeline.cpp to forget or misplace.
    ~Out() {
        if (first_ring_ == nullptr || is_failing_of_this_thread) {
            return;
        }
        if (is_ring_slot_) {
            first_ring_->publish();
            return;
        }
        if (buffer_.count_ == 0) {
            return;
        }
        for (SampleRingWriterEnd<StreamInfoType>* ring = first_ring_; ring != nullptr;
             ring = ring->next_fed_ring_) {
            ring->write(buffer_);
        }
    }

    // A copy would hand the sample over a second time.
    Out(const Out&) = delete;
    Out& operator=(const Out&) = delete;

    void set_header(const SampleHeader& header) {
        buffer_.sample_->header = header;
        buffer_.count_ = 1;
    }

    void store(int simd_vector_index, SimdVector values) {
        float* address = &buffer_.sample_->values[static_cast<std::size_t>(simd_vector_index) *
                                                  static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
        // Not if constexpr, for the same reason as in load().
#ifdef __AVX512F__
        _mm512_store_ps(address, values);
#else
        _mm256_store_ps(address, values);
#endif
    }

private:
    SampleBuffer<StreamInfoType>& buffer_;
    // The ring whose slot the buffer is, or the first of the rings the buffer feeds.
    SampleRingWriterEnd<StreamInfoType>* first_ring_;
    bool is_ring_slot_ = false;
};

template <std::derived_from<Event> EventType, int Capacity>
struct In<EventBuffer<EventType, Capacity>> {
    In(const EventBuffer<EventType, Capacity>& events) : buffer_(events) {}

    // A connection from another core: the ring's waiting events, fetched once per pass.
    template <int slot_count>
    In(EventRing<EventType, Capacity, slot_count>& ring) : In(ring.reader_buffer()) {}

    [[nodiscard]] bool is_empty() const { return buffer_.count_ == 0; }
    [[nodiscard]] int size() const { return buffer_.count_; }
    [[nodiscard]] const EventType* begin() const { return buffer_.events_.data(); }
    [[nodiscard]] const EventType* end() const { return buffer_.events_.data() + buffer_.count_; }

private:
    const EventBuffer<EventType, Capacity>& buffer_;
};

template <std::derived_from<Event> EventType, int Capacity>
struct Out<EventBuffer<EventType, Capacity>> {
    // Clears the buffer right before its producer runs.
    Out(EventBuffer<EventType, Capacity>& events)
        : buffer_(events), first_ring_(events.first_fed_ring_) {
        buffer_.count_ = 0;
    }

    // A connection to another core: the producer pushes into the ring's own buffer.
    template <int slot_count>
    Out(EventRing<EventType, Capacity, slot_count>& ring) : Out(ring.writer_buffer_) {
        first_ring_ = &ring;
        is_ring_buffer_ = true;
    }

    // Runs when the producer's call returns, so events for another core leave right after their
    // producer, with no call in pipeline.cpp to forget or misplace.
    ~Out() {
        if (first_ring_ == nullptr || buffer_.count_ == 0 || is_failing_of_this_thread) {
            return;
        }
        if (is_ring_buffer_) {
            first_ring_->write(buffer_);
            return;
        }
        for (EventRingWriterEnd<EventType, Capacity>* ring = first_ring_; ring != nullptr;
             ring = ring->next_fed_ring_) {
            ring->write(buffer_);
        }
    }

    // A copy would hand the events over a second time.
    Out(const Out&) = delete;
    Out& operator=(const Out&) = delete;

    // Neither push checks Capacity: a producer never pushes more than Capacity per pass.
    // A stream event can only be pushed with its sample's header, never without one.
    void push(const EventType& event)
        requires(!std::derived_from<EventType, StreamEvent>)
    {
        buffer_.events_[static_cast<std::size_t>(buffer_.count_++)] = event;
    }

    // The header is a parameter because designated initializers can't name a base's members.
    void push(const SampleHeader& header, const EventType& event)
        requires std::derived_from<EventType, StreamEvent>
    {
        EventType& stored = buffer_.events_[static_cast<std::size_t>(buffer_.count_++)];
        stored = event;
        stored.header = header;
    }

private:
    EventBuffer<EventType, Capacity>& buffer_;
    // The ring whose own buffer this is, or the first of the rings the buffer feeds.
    EventRingWriterEnd<EventType, Capacity>* first_ring_;
    bool is_ring_buffer_ = false;
};

// Left out of the call, a state input reads default_value for the whole run. Connected, it takes a
// state value with any initial value: its producer picks that one.
template <StateValueType StateType, StateType default_value>
struct In<StateValue<StateType, default_value>> {
    In(Unconnected /*marker*/) : value_(default_value) {}
    template <StateType initial_value>
    In(const StateValue<StateType, initial_value>& state) : value_(state.value) {}

    // A connection from another core: its latest value, read once, so it can't change during the
    // call. Relaxed, since nothing else is read on the strength of this value.
    template <StateType initial_value>
    In(const CrossCoreStateValue<StateType, initial_value>& state)
        : value_(state.value.load(std::memory_order_relaxed)) {}

    operator StateType() const { return value_; }

private:
    StateType value_;
};

// Never clears its state value, which keeps its value until the producer changes it.
template <StateValueType StateType, StateType initial_value>
struct Out<StateValue<StateType, initial_value>> {
    Out(StateValue<StateType, initial_value>& state)
        : value_(state.value), plain_value_(&state.value) {}

    // A connection to other cores. Only the producer's core writes the value, so the relaxed
    // load reads that core's own latest value.
    Out(CrossCoreStateValue<StateType, initial_value>& state)
        : value_(state.value.load(std::memory_order_relaxed)),
          cross_core_value_(&state.value),
          previous_value_(value_) {}

    // Runs when the producer's call returns. A cross-core value that didn't change is not written,
    // so the cores that read it keep its cache line.
    ~Out() {
        if (plain_value_ != nullptr) {
            *plain_value_ = value_;
            return;
        }
        if (is_failing_of_this_thread) {
            return;
        }
        if (!is_bitwise_equal(value_, previous_value_)) {
            cross_core_value_->store(value_, std::memory_order_relaxed);
        }
    }

    // A copy would write its state value a second time.
    Out(const Out&) = delete;
    Out& operator=(const Out&) = delete;

    Out& operator=(StateType new_value) {
        value_ = new_value;
        return *this;
    }

private:
    // The value itself, not a reference to a state value's: with a reference that could point at
    // a member of this output, the compiler keeps the whole output in memory.
    StateType value_;
    // One of these two is set: where the value goes when the call returns.
    StateType* plain_value_ = nullptr;
    std::atomic<StateType>* cross_core_value_ = nullptr;
    StateType previous_value_ = initial_value;
};

template <typename StreamInfoType>
using SampleInputBuffer = In<SampleBuffer<StreamInfoType>>;
template <typename StreamInfoType>
using SampleOutputBuffer = Out<SampleBuffer<StreamInfoType>>;
template <std::derived_from<Event> EventType, int Capacity>
using EventInputBuffer = In<EventBuffer<EventType, Capacity>>;
template <std::derived_from<Event> EventType, int Capacity>
using EventOutputBuffer = Out<EventBuffer<EventType, Capacity>>;
template <StateValueType StateType, StateType default_value>
using StateInput = In<StateValue<StateType, default_value>>;
template <StateValueType StateType, StateType initial_value>
using StateOutput = Out<StateValue<StateType, initial_value>>;

// A port with slots: one member of Connections that takes as many connections as pipeline.cpp
// gives it, each of its own kind (a SampleBuffer or an EventBuffer type). Its processor is a
// template over those kinds and goes through them with `template for`, where each slot has its
// own type.
template <typename... Kinds>
using InputSlots = std::tuple<In<Kinds>...>;
template <typename... Kinds>
using OutputSlots = std::tuple<Out<Kinds>...>;
