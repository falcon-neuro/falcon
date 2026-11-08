#pragma once
#include <immintrin.h>
#include <pmmintrin.h>
#include <pthread.h>
#include <xmmintrin.h>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <initializer_list>
#include <latch>
#include <print>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// Falcon engine. Its design is described in src/core/spec.md.

using u32 = std::uint32_t;
using u64 = std::uint64_t;

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

// 0 or 1 sample. `sample_` points at the buffer's own storage, or at a ring slot, so nothing is
// copied across rings. Only In, Out and rings reach inside, so a processor that holds a buffer
// directly instead of through In or Out can't use it.
template <typename StreamInfoType>
struct SampleBuffer {
    SampleBuffer() = default;
    // A copy's `sample_` would still point at the original's storage.
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;
    template <typename RingStreamInfoType, int slot_count>
    friend struct SampleRing;

    SampleWithHeader<StreamInfoType> storage_{};
    SampleWithHeader<StreamInfoType>* sample_ = &storage_;
    int count_ = 0;
};

// --- Events ---

// Base of every event record. Empty: an event from a timer has no sample to point to.
struct EventRecord {};

// Base of every event record that comes from a sample; `header` is that sample's header.
struct StreamEventRecord : EventRecord {
    SampleHeader header;
};

// Up to Capacity records produced on this pass, held inside the buffer. Only In and Out reach
// inside, and it can't be copied, so a processor that holds a buffer directly instead of through In
// or Out can't use it or push into a copy.
template <std::derived_from<EventRecord> Record, int Capacity>
struct EventBuffer {
    EventBuffer() = default;
    EventBuffer(const EventBuffer&) = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;

    std::array<Record, Capacity> records_{};
    int count_ = 0;
};

// --- State values ---

// A number, bool or enum, so a state value fits in one register.
template <typename StateType>
concept StateValueType = std::is_arithmetic_v<StateType> || std::is_enum_v<StateType>;

// The producer's StateOutput fixes initial_value, so graph.cpp can't declare a different one.
template <StateValueType StateType, StateType initial_value>
struct StateValue {
    StateType value = initial_value;
};

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
    Out(SampleBuffer<StreamInfoType>& samples) : buffer_(samples) { buffer_.count_ = 0; }

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
};

template <std::derived_from<EventRecord> Record, int Capacity>
struct In<EventBuffer<Record, Capacity>> {
    In(const EventBuffer<Record, Capacity>& events) : buffer_(events) {}

    [[nodiscard]] bool is_empty() const { return buffer_.count_ == 0; }
    [[nodiscard]] int size() const { return buffer_.count_; }
    [[nodiscard]] const Record* begin() const { return buffer_.records_.data(); }
    [[nodiscard]] const Record* end() const { return buffer_.records_.data() + buffer_.count_; }

private:
    const EventBuffer<Record, Capacity>& buffer_;
};

template <std::derived_from<EventRecord> Record, int Capacity>
struct Out<EventBuffer<Record, Capacity>> {
    // Clears the buffer right before its producer runs.
    Out(EventBuffer<Record, Capacity>& events) : buffer_(events) { buffer_.count_ = 0; }

    // Neither push checks Capacity: a producer never pushes more than Capacity per pass.
    // A stream event can only be pushed with its sample's header, never without one.
    void push(const Record& record)
        requires(!std::derived_from<Record, StreamEventRecord>)
    {
        buffer_.records_[static_cast<std::size_t>(buffer_.count_++)] = record;
    }

    // The header is a parameter because designated initializers can't name a base's members.
    void push(const SampleHeader& header, const Record& record)
        requires std::derived_from<Record, StreamEventRecord>
    {
        Record& stored = buffer_.records_[static_cast<std::size_t>(buffer_.count_++)];
        stored = record;
        stored.header = header;
    }

private:
    EventBuffer<Record, Capacity>& buffer_;
};

// Takes a state value with any initial value: only its producer picks one.
template <StateValueType StateType>
struct In<StateType> {
    template <StateType initial_value>
    In(const StateValue<StateType, initial_value>& state) : value_(state.value) {}

    operator StateType() const { return value_; }

private:
    const StateType& value_;
};

// Never clears its state value, which keeps its value until the producer changes it.
template <StateValueType StateType, StateType initial_value>
struct Out<StateValue<StateType, initial_value>> {
    Out(StateValue<StateType, initial_value>& state) : value_(state.value) {}

    Out& operator=(StateType new_value) {
        value_ = new_value;
        return *this;
    }

private:
    StateType& value_;
};

template <typename StreamInfoType>
using SampleInputBuffer = In<SampleBuffer<StreamInfoType>>;
template <typename StreamInfoType>
using SampleOutputBuffer = Out<SampleBuffer<StreamInfoType>>;
template <std::derived_from<EventRecord> Record, int Capacity>
using EventInputBuffer = In<EventBuffer<Record, Capacity>>;
template <std::derived_from<EventRecord> Record, int Capacity>
using EventOutputBuffer = Out<EventBuffer<Record, Capacity>>;
template <StateValueType StateType>
using StateInput = In<StateType>;
template <StateValueType StateType, StateType initial_value>
using StateOutput = Out<StateValue<StateType, initial_value>>;

// --- Rings ---

// Realtime cores never print, so a ring only counts its dropped samples and the main thread warns.
struct RingDropCounter {
    const char* ring_name = "";
    std::atomic<u64> dropped_samples = 0;
    u64 reported_samples = 0;              // touched only by the main thread
    RingDropCounter* next_ring = nullptr;  // the list of every ring's counter
};

// Every ring adds its counter when it is built, so none can be left out of the warnings. A linked
// list through the counters, since rings are globals: adding to it can't throw, and an exception
// while globals are built can't be caught.
inline RingDropCounter* first_ring_drop_counter = nullptr;

// One writer core and one reader core. A full ring never makes the writer wait: the sample goes
// into spare storage and is dropped.
template <typename StreamInfoType, int slot_count>
struct SampleRing {
    static_assert(slot_count > 0 && std::has_single_bit(static_cast<unsigned>(slot_count)),
                  "slot_count must be a power of 2");
    static_assert(std::atomic<u64>::is_always_lock_free);

    explicit SampleRing(const char* ring_name) noexcept {
        drop_counter_.ring_name = ring_name;
        drop_counter_.next_ring = first_ring_drop_counter;
        first_ring_drop_counter = &drop_counter_;
    }
    SampleRing(const SampleRing&) = delete;
    SampleRing& operator=(const SampleRing&) = delete;

    // Writer core: the buffer to pass to the producer's output, pointing at the next free slot.
    SampleBuffer<StreamInfoType>& next_slot() {
        const u64 write_position = write_position_.load(std::memory_order_relaxed);
        const u64 read_position = read_position_.load(std::memory_order_acquire);
        if (write_position - read_position == static_cast<u64>(slot_count)) {
            writer_buffer_.sample_ = &writer_buffer_.storage_;
        } else {
            writer_buffer_.sample_ = &slots_[write_position % static_cast<u64>(slot_count)];
        }
        return writer_buffer_;
    }

    // Writer core: makes the sample written into next_slot() visible to the reader, if there is
    // one.
    void publish() {
        if (writer_buffer_.count_ == 0) {
            return;
        }
        if (writer_buffer_.sample_ == &writer_buffer_.storage_) {
            drop_counter_.dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Only this core writes the drop count, so the relaxed load reads its own latest value.
        writer_buffer_.sample_->header.lost_sample_count +=
            drop_counter_.dropped_samples.load(std::memory_order_relaxed);
        const u64 write_position = write_position_.load(std::memory_order_relaxed);
        write_position_.store(write_position + 1, std::memory_order_release);
    }

    // Reader core: points `buffer` at the next sample, or leaves it empty. The slot handed out on
    // the previous pass is freed only now, since processors read it until then.
    void read(SampleBuffer<StreamInfoType>& buffer) {
        const u64 read_position = read_position_.load(std::memory_order_relaxed);
        u64 next_read_position = read_position;
        if (is_holding_slot_) {
            next_read_position = read_position + 1;
            read_position_.store(next_read_position, std::memory_order_release);
            is_holding_slot_ = false;
        }
        const u64 write_position = write_position_.load(std::memory_order_acquire);
        if (write_position == next_read_position) {
            buffer.count_ = 0;
            return;
        }
        buffer.sample_ = &slots_[next_read_position % static_cast<u64>(slot_count)];
        buffer.count_ = 1;
        is_holding_slot_ = true;
    }

private:
    // Each position on its own cache line, so the two cores share only the line they must.
    alignas(64) std::atomic<u64> write_position_ = 0;
    alignas(64) std::atomic<u64> read_position_ = 0;
    alignas(64) bool is_holding_slot_ = false;    // reader core only
    SampleBuffer<StreamInfoType> writer_buffer_;  // writer core only; its storage is the spare
    RingDropCounter drop_counter_;
    std::array<SampleWithHeader<StreamInfoType>, slot_count> slots_{};
};

// Written with to_chars and fputs, since println and format can throw and no exception may leave
// run_on_cores().
void print_number(u64 number) noexcept {
    std::array<char, 24> digits{};  // u64 has at most 20 digits
    const std::to_chars_result result =
        std::to_chars(digits.data(), digits.data() + digits.size() - 1, number);
    *result.ptr = '\0';
    std::fputs(digits.data(), stderr);
}

// Main thread only.
void warn_about_dropped_samples() noexcept {
    for (RingDropCounter* drop_counter = first_ring_drop_counter; drop_counter != nullptr;
         drop_counter = drop_counter->next_ring) {
        const u64 dropped_samples = drop_counter->dropped_samples.load(std::memory_order_relaxed);
        if (dropped_samples > drop_counter->reported_samples) {
            std::fputs("warning: ring ", stderr);
            std::fputs(drop_counter->ring_name, stderr);
            std::fputs(" is full, dropped ", stderr);
            print_number(dropped_samples - drop_counter->reported_samples);
            std::fputs(" samples (", stderr);
            print_number(dropped_samples);
            std::fputs(" in total)\n", stderr);
            drop_counter->reported_samples = dropped_samples;
        }
    }
}

// --- Cores ---

// Every core's loop runs while it is set. Cleared by run_on_cores() on a stop signal, or by a core
// whose constructor failed.
std::atomic<bool> is_pipeline_running = true;
// Set when a constructor threw on any core, or a core's thread couldn't be started.
std::atomic<bool> has_startup_failed = false;
std::latch* start_barrier = nullptr;  // created by run_on_cores()
thread_local bool has_arrived_at_start_barrier = false;
thread_local int core_number_of_this_thread = -1;  // set by run_on_cores()

// Every core calls this once its processors are built. Returns false if startup failed on any
// core, so the core returns without running its loop. The thread pins itself to its core only
// now: a thread that a library starts in a constructor (e.g. to receive packets) inherits its
// creator's CPUs, and on the realtime core it would compete with a loop that never yields.
bool wait_at_start_barrier() {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_number_of_this_thread, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        throw std::runtime_error(std::format("can't pin to CPU {}", core_number_of_this_thread));
    }
    has_arrived_at_start_barrier = true;
    start_barrier->arrive_and_wait();
    return !has_startup_failed;
}

struct CoreThread {
    int core_number;        // CPU core the thread pins itself to in wait_at_start_barrier()
    void (*entry_point)();  // declares the core's processors and buffers, calls
                            // wait_at_start_barrier(), then runs its loop
};

// The stop signals: SIGINT (Ctrl+C) and SIGTERM.
sigset_t stop_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Waits for a stop signal while the cores run, then clears `is_pipeline_running`. The timeout ends
// the wait when `is_pipeline_running` was cleared without a signal (startup failed).
void wait_for_stop_signal(const sigset_t& signals) {
    while (is_pipeline_running) {
        const timespec timeout{.tv_sec = 0, .tv_nsec = 100'000'000};  // 100 ms
        const int signal_number = sigtimedwait(&signals, nullptr, &timeout);
        if (signal_number > 0) {
            // fputs, unlike println, can't throw, so no exception leaves run_on_cores().

            std::fputs(signal_number == SIGINT ? "\nCtrl+C received, stopping\n"
                                               : "\nSIGTERM received, stopping\n",
                       stderr);
            is_pipeline_running = false;
        }
        warn_about_dropped_samples();
    }
}

// Runs one pinned thread per core until stopped. Returns 1 if the graph didn't start, 0 otherwise.
// A core's thread that can't be started also means the graph doesn't start, so no exception leaves
// run_on_cores().
int run_on_cores(std::initializer_list<CoreThread> core_threads) {
    // Blocked before any core's thread starts, so every thread inherits the block and no stop
    // signal ever interrupts a core's loop. Only wait_for_stop_signal() takes them.
    const sigset_t signals = stop_signals();
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::fputs("can't block the stop signals\n", stderr);
        return 1;
    }

    std::latch latch(static_cast<std::ptrdiff_t>(core_threads.size()));
    start_barrier = &latch;

    std::vector<std::thread> threads;
    try {
        threads.reserve(core_threads.size());
        for (CoreThread core_thread : core_threads) {
            threads.emplace_back([core_thread] {
                try {
                    core_number_of_this_thread = core_thread.core_number;
                    // Denormals, e.g. a filter's state on a flat channel, are far slower.
                    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
                    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
                    core_thread.entry_point();
                } catch (const std::exception& error) {
                    // A constructor failed.
                    std::println(stderr, "core {}: {}", core_thread.core_number, error.what());
                    has_startup_failed = true;
                    is_pipeline_running = false;
                    if (!has_arrived_at_start_barrier) {
                        start_barrier->count_down();  // release the cores waiting at the barrier
                    }
                }
            });
        }
    } catch (const std::exception& error) {
        // A core's thread couldn't be started. Count down once for every core that never started,
        // so the started ones pass the start barrier, see has_startup_failed and return. fputs,
        // unlike println, can't throw here.
        std::fputs("can't start a core's thread: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        has_startup_failed = true;
        is_pipeline_running = false;
        for (std::size_t missing = threads.size(); missing < core_threads.size(); ++missing) {
            latch.count_down();
        }
    }

    wait_for_stop_signal(signals);
    for (std::thread& thread : threads) {
        thread.join();
    }
    return has_startup_failed ? 1 : 0;
}
