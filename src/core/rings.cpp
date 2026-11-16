#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <limits>
#include <stdexcept>
#include <thread>
#include "connections.cpp"

// Engine, part 4 of 5: passes, the rings that carry samples and events between cores, and
// next_pass().

// --- Passes ---

// Counts this core's passes: next_pass() starts each one. A ring remembers the pass of its last
// read, so it is read once per pass however many processors read it.
thread_local u64 pass_number_of_this_thread = 0;
// Whether a ring has handed this core anything on this pass.
thread_local bool has_received_on_this_pass = false;
thread_local bool is_realtime_core_of_this_thread = true;  // set by run_on_cores()

// What a ring holds as the pass of its last read before its first one: no pass has this number.
inline constexpr u64 NO_PASS = std::numeric_limits<u64>::max();

// Whether a ring is read for the first time on this pass. Passes can be told apart only in a loop
// that next_pass() runs, so in any other loop a ring given as an input ends the run.
[[nodiscard]] bool is_first_read_on_this_pass(u64& last_read_pass, const char* ring_name) {
    const u64 pass_number = pass_number_of_this_thread;
    if (last_read_pass == pass_number) {
        return false;
    }
    if (pass_number == 0) {
        throw std::logic_error(
            std::format("{} is given to a processor as an input, so its core's loop must be "
                        "`while (next_pass())`",
                        ring_name));
    }
    last_read_pass = pass_number;
    return true;
}

// --- Rings ---

// Realtime cores never print, so a ring only counts what it drops and the main thread warns.
struct RingDropCounter {
    const char* ring_name = "";
    const char* dropped_kind = "";  // what the ring carries, for the warning: samples or events
    std::atomic<u64> dropped_count = 0;
    u64 reported_count = 0;                // touched only by the main thread
    RingDropCounter* next_ring = nullptr;  // the list of every ring's counter
};

// Every ring adds its counter when it is built, so none can be left out of the warnings. A linked
// list through the counters, since rings are globals: adding to it can't throw, and an exception
// while globals are built can't be caught.
inline RingDropCounter* first_ring_drop_counter = nullptr;

// The writer's side of a sample ring, without the slot count in its type, so an output and a
// sample buffer can refer to rings of any size.
template <typename StreamInfoType>
struct SampleRingWriterEnd {
    SampleRingWriterEnd(const SampleRingWriterEnd&) = delete;
    SampleRingWriterEnd& operator=(const SampleRingWriterEnd&) = delete;

    // Writer core: makes the sample written into next_slot() visible to the reader, if there is
    // one.
    void publish() {
        if (writer_buffer_.count_ == 0) {
            return;
        }
        if (writer_buffer_.sample_ == &writer_buffer_.storage_) {
            drop_counter_.dropped_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Only this core writes the drop count, so the relaxed load reads its own latest value.
        writer_buffer_.sample_->header.lost_sample_count +=
            drop_counter_.dropped_count.load(std::memory_order_relaxed);
        const u64 write_position = write_position_.load(std::memory_order_relaxed);
        write_position_.store(write_position + 1, std::memory_order_release);
    }

protected:
    using WriteFunction = void (*)(SampleRingWriterEnd&, const SampleBuffer<StreamInfoType>&);

    SampleRingWriterEnd(const char* ring_name, WriteFunction write_function) noexcept
        : write_function_(write_function) {
        drop_counter_.ring_name = ring_name;
        drop_counter_.dropped_kind = "samples";
        drop_counter_.next_ring = first_ring_drop_counter;
        first_ring_drop_counter = &drop_counter_;
    }
    ~SampleRingWriterEnd() = default;

private:
    template <typename BufferStreamInfoType>
    friend struct SampleBuffer;
    template <typename Kind>
    friend struct Out;
    template <typename RingStreamInfoType, int slot_count>
    friend struct SampleRing;

    // Apart from the other members, so the two cores share only the lines they must.
    alignas(BYTES_BETWEEN_CORES) std::atomic<u64> write_position_ = 0;
    SampleBuffer<StreamInfoType> writer_buffer_;  // writer core only; its storage is the spare
    RingDropCounter drop_counter_;

    // Writer core: copies a buffer's sample into the ring, for a ring that a buffer feeds.
    void write(const SampleBuffer<StreamInfoType>& samples) { write_function_(*this, samples); }

    // Called by the buffer that feeds this ring, before the loops start.
    void join_fed_rings(SampleRingWriterEnd* next_ring) {
        if (has_feeding_buffer_) {
            throw std::logic_error(
                std::format("two buffers feed {}, which has one writer", drop_counter_.ring_name));
        }
        has_feeding_buffer_ = true;
        next_fed_ring_ = next_ring;
    }

    // Copies into a slot, which only SampleRing can reach: it knows how many there are.
    WriteFunction write_function_;
    SampleRingWriterEnd* next_fed_ring_ = nullptr;  // the next ring that the same buffer feeds
    bool has_feeding_buffer_ = false;
};

// One writer core and one reader core. A full ring never makes the writer wait: the sample goes
// into spare storage and is dropped.
template <typename StreamInfoType, int slot_count>
struct SampleRing : SampleRingWriterEnd<StreamInfoType> {
    static_assert(slot_count > 0 && std::has_single_bit(static_cast<unsigned>(slot_count)),
                  "slot_count must be a power of 2");
    static_assert(std::atomic<u64>::is_always_lock_free);

    explicit SampleRing(const char* ring_name) noexcept
        : SampleRingWriterEnd<StreamInfoType>(ring_name, &write_sample) {}

    // Writer core: the buffer to pass to the producer's output, pointing at the next free slot.
    SampleBuffer<StreamInfoType>& next_slot() {
        const u64 write_position = this->write_position_.load(std::memory_order_relaxed);
        const u64 read_position = read_position_.load(std::memory_order_acquire);
        if (write_position - read_position == static_cast<u64>(slot_count)) {
            this->writer_buffer_.sample_ = &this->writer_buffer_.storage_;
        } else {
            this->writer_buffer_.sample_ = &slots_[write_position % static_cast<u64>(slot_count)];
        }
        return this->writer_buffer_;
    }

    // Reader core: points `buffer` at the next sample, or leaves it empty, and returns whether it
    // holds one. The slot handed out on the previous pass is freed only now, since processors read
    // it until then.
    bool read(SampleBuffer<StreamInfoType>& buffer) {
        const u64 read_position = read_position_.load(std::memory_order_relaxed);
        u64 next_read_position = read_position;
        if (is_holding_slot_) {
            next_read_position = read_position + 1;
            read_position_.store(next_read_position, std::memory_order_release);
            is_holding_slot_ = false;
        }
        const u64 write_position = this->write_position_.load(std::memory_order_acquire);
        if (write_position == next_read_position) {
            buffer.count_ = 0;
            return false;
        }
        buffer.sample_ = &slots_[next_read_position % static_cast<u64>(slot_count)];
        buffer.count_ = 1;
        is_holding_slot_ = true;
        return true;
    }

private:
    template <typename Kind>
    friend struct In;

    // Reader core: this pass's buffer, holding the ring's next sample or none. Only a pass's first
    // call reads the ring, so every processor that reads it on that pass gets the same sample.
    SampleBuffer<StreamInfoType>& reader_buffer() {
        if (is_first_read_on_this_pass(last_read_pass_, this->drop_counter_.ring_name) &&
            read(reader_buffer_)) {
            has_received_on_this_pass = true;
        }
        return reader_buffer_;
    }

    static void write_sample(SampleRingWriterEnd<StreamInfoType>& writer_end,
                             const SampleBuffer<StreamInfoType>& samples) {
        auto& ring = static_cast<SampleRing&>(writer_end);
        SampleBuffer<StreamInfoType>& slot = ring.next_slot();
        *slot.sample_ = *samples.sample_;
        slot.count_ = 1;
        ring.publish();
    }

    alignas(BYTES_BETWEEN_CORES) std::atomic<u64> read_position_ = 0;
    // Reader core only, as the two members after it.
    alignas(BYTES_BETWEEN_CORES) bool is_holding_slot_ = false;
    u64 last_read_pass_ = NO_PASS;
    SampleBuffer<StreamInfoType> reader_buffer_;
    alignas(BYTES_BETWEEN_CORES) std::array<SampleWithHeader<StreamInfoType>, slot_count> slots_{};
};

// The writer's side of an event ring, without the slot count in its type, so an output and an
// event buffer can refer to rings of any size.
template <std::derived_from<Event> EventType, int Capacity>
struct EventRingWriterEnd {
    EventRingWriterEnd(const EventRingWriterEnd&) = delete;
    EventRingWriterEnd& operator=(const EventRingWriterEnd&) = delete;

protected:
    using WriteFunction = void (*)(EventRingWriterEnd&, const EventBuffer<EventType, Capacity>&);

    EventRingWriterEnd(const char* ring_name, WriteFunction write_function) noexcept
        : write_function_(write_function) {
        drop_counter_.ring_name = ring_name;
        drop_counter_.dropped_kind = "events";
        drop_counter_.next_ring = first_ring_drop_counter;
        first_ring_drop_counter = &drop_counter_;
    }
    ~EventRingWriterEnd() = default;

private:
    template <std::derived_from<Event> BufferEventType, int BufferCapacity>
    friend struct EventBuffer;
    template <typename Kind>
    friend struct Out;
    template <std::derived_from<Event> RingEventType, int RingCapacity, int slot_count>
    friend struct EventRing;

    // Apart from the other members, so the two cores share only the lines they must.
    alignas(BYTES_BETWEEN_CORES) std::atomic<u64> write_position_ = 0;
    RingDropCounter drop_counter_;

    // Writer core: copies the events their producer pushed on this pass into the ring.
    void write(const EventBuffer<EventType, Capacity>& events) { write_function_(*this, events); }

    // Called by the buffer that feeds this ring, before the loops start.
    void join_fed_rings(EventRingWriterEnd* next_ring) {
        if (has_feeding_buffer_) {
            throw std::logic_error(
                std::format("two buffers feed {}, which has one writer", drop_counter_.ring_name));
        }
        has_feeding_buffer_ = true;
        next_fed_ring_ = next_ring;
    }

    // Copies into the slots, which only EventRing can reach: it knows how many there are.
    WriteFunction write_function_;
    EventRingWriterEnd* next_fed_ring_ = nullptr;  // the next ring that the same buffer feeds
    bool has_feeding_buffer_ = false;
};

// One writer core and one reader core, for events. Events are copied in and out, so a slot is free
// again as soon as its event has been read. A full ring never makes the writer wait: the event is
// dropped and counted.
template <std::derived_from<Event> EventType, int Capacity, int slot_count>
struct EventRing : EventRingWriterEnd<EventType, Capacity> {
    static_assert(slot_count > 0 && std::has_single_bit(static_cast<unsigned>(slot_count)),
                  "slot_count must be a power of 2");
    static_assert(std::atomic<u64>::is_always_lock_free);

    explicit EventRing(const char* ring_name) noexcept
        : EventRingWriterEnd<EventType, Capacity>(ring_name, &write_events) {}

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;

    // Reader core: this pass's buffer, holding the ring's waiting events or none. Only a pass's
    // first call reads the ring, so every processor that reads it on that pass gets the same
    // events.
    EventBuffer<EventType, Capacity>& reader_buffer() {
        if (is_first_read_on_this_pass(last_read_pass_, this->drop_counter_.ring_name) &&
            read(reader_buffer_)) {
            has_received_on_this_pass = true;
        }
        return reader_buffer_;
    }

    // Reader core: fills `events` with the waiting events, as many as it holds, or leaves it
    // empty, and returns whether it holds any. The others wait in the ring for the next pass.
    bool read(EventBuffer<EventType, Capacity>& events) {
        events.count_ = 0;
        u64 read_position = read_position_.load(std::memory_order_relaxed);
        const u64 write_position = this->write_position_.load(std::memory_order_acquire);
        if (read_position == write_position) {
            return false;
        }
        while (read_position != write_position && events.count_ < Capacity) {
            events.events_[static_cast<std::size_t>(events.count_)] =
                slots_[read_position % static_cast<u64>(slot_count)];
            ++events.count_;
            ++read_position;
        }
        read_position_.store(read_position, std::memory_order_release);
        return true;
    }

    // Writer core: the buffer keeps its events, so processors on the writer's core read them as
    // before.
    static void write_events(EventRingWriterEnd<EventType, Capacity>& writer_end,
                             const EventBuffer<EventType, Capacity>& events) {
        auto& ring = static_cast<EventRing&>(writer_end);
        u64 write_position = ring.write_position_.load(std::memory_order_relaxed);
        const u64 read_position = ring.read_position_.load(std::memory_order_acquire);
        for (int event_index = 0; event_index < events.count_; ++event_index) {
            if (write_position - read_position == static_cast<u64>(slot_count)) {
                ring.drop_counter_.dropped_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            ring.slots_[write_position % static_cast<u64>(slot_count)] =
                events.events_[static_cast<std::size_t>(event_index)];
            ++write_position;
        }
        ring.write_position_.store(write_position, std::memory_order_release);
    }

    alignas(BYTES_BETWEEN_CORES) std::atomic<u64> read_position_ = 0;
    // Reader core only, as the buffer after it.
    alignas(BYTES_BETWEEN_CORES) u64 last_read_pass_ = NO_PASS;
    EventBuffer<EventType, Capacity> reader_buffer_;
    // Writer core only: what a producer pushes into when this ring is its output.
    alignas(BYTES_BETWEEN_CORES) EventBuffer<EventType, Capacity> writer_buffer_;
    alignas(BYTES_BETWEEN_CORES) std::array<EventType, slot_count> slots_{};
};

// --- Waiting for data ---

// How a core that isn't realtime spends a pass that brought it nothing: asleep, so it doesn't
// spin. Its rings must hold what arrives meanwhile.
void wait_for_data() {
    constexpr std::chrono::milliseconds idle_sleep{1};
    std::this_thread::sleep_for(idle_sleep);
}

// Starts this core's next pass and returns whether the run goes on: a core's loop is
// `while (next_pass())`. A core that isn't realtime first waits if the pass before brought it
// nothing through a ring.
[[nodiscard]] bool next_pass() {
    if (!is_realtime_core_of_this_thread && !has_received_on_this_pass) {
        wait_for_data();
    }
    has_received_on_this_pass = false;
    ++pass_number_of_this_thread;
    return is_pipeline_running;
}
