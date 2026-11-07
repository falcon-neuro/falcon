#pragma once
#include <immintrin.h>
#include <pmmintrin.h>
#include <pthread.h>
#include <xmmintrin.h>
#include <array>
#include <atomic>
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

// Falcon engine (see src/core/spec.md). So far: streams and sample buffers, events, In and Out,
// and one pinned thread per core with the start barrier.

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- Instruction set (see One processor per stream in spec.md) ---

#ifndef __AVX2__
#error "Falcon needs AVX2: compile with -mavx2 or -march=native"
#endif

#ifdef __AVX512F__
inline constexpr bool AVX512 = true;
#else
inline constexpr bool AVX512 = false;
#endif

inline constexpr int FLOATS_PER_VECTOR = AVX512 ? 16 : 8;
using FloatVector = std::conditional_t<AVX512, __m512, __m256>;

// --- Streams and samples (see Stream type, Buffers and Sample type in spec.md) ---

template <int ChannelCount, double SampleRate, typename SampleType>
struct Stream {
    static_assert(ChannelCount > 0);
    static_assert(SampleRate > 0);
    // float16 would need load() and store() to convert (see Sample width: float16 in
    // beyond_prototype.md).
    static_assert(std::same_as<SampleType, float>, "only float32 samples are implemented");

    static constexpr int channels = ChannelCount;
    static constexpr double rate = SampleRate;  // Hz
    using Value = SampleType;
    // A multiple of 16, one AVX512 vector of float32, so SIMD loops have no leftover channels.
    static constexpr int padded_channels = (ChannelCount + 15) / 16 * 16;
    static constexpr int vectors = padded_channels / FLOATS_PER_VECTOR;
};

// Set by the input processor, travels with its sample (see Buffers in spec.md).
struct Header {
    u64 sample_number = 0;     // counted at the input's rate, derived from the device's clock
    u64 device_timestamp = 0;  // in the device's own unit; processors never calculate with it
    u64 arrival_tsc = 0;       // CPU timestamp counter when the sample arrived
};

// One sample with its header in front, the layout of a ring slot (see Rings in spec.md).
template <typename StreamType>
struct SampleSlot {
    Header header;
    alignas(64) std::array<typename StreamType::Value, StreamType::padded_channels> values;
};

// 0 or 1 sample. `slot` points at the buffer's own storage, or at a ring slot, so nothing is
// copied across rings (see graph.cpp in spec.md).
template <typename StreamType>
struct SampleBuffer {
    SampleSlot<StreamType> storage{};
    SampleSlot<StreamType>* slot = &storage;
    int count = 0;

    SampleBuffer() = default;
    // A copy's slot would still point at the original's storage.
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;
};

// --- Events (see Events in spec.md) ---

// Base of every event record. Empty: an event from a timer has no sample to point to.
struct Event {};

// Up to Capacity records produced on this pass (see Capacity in spec.md).
// The records sit inside the buffer; pointing at a ring slot instead comes with event rings
// (beyond_prototype.md).
template <std::derived_from<Event> Record, int Capacity>
struct EventBuffer {
    std::array<Record, Capacity> records;
    int count = 0;
};

// --- In and Out (see process() signature in spec.md) ---
// process() takes one struct, Connections, declared inside its processor. Every member of it is an
// In<Kind> or an Out<Kind>, and each exposes only what its direction allows.

template <typename Kind>
struct In;
template <typename Kind>
struct Out;

template <typename StreamType>
struct In<SampleBuffer<StreamType>> {
    In(const SampleBuffer<StreamType>& samples) : header(samples.slot->header), buffer_(samples) {}

    const Header& header;

    [[nodiscard]] bool is_empty() const { return buffer_.count == 0; }

    // Channel vector `vector_index` as float32, in the compiled instruction set's register.
    [[nodiscard]] FloatVector load(int vector_index) const {
        const float* address =
            &buffer_.slot->values[static_cast<std::size_t>(vector_index * FLOATS_PER_VECTOR)];
        // Not if constexpr: nothing here depends on StreamType, so the discarded branch would
        // still be type-checked, and an __m512 doesn't convert to the AVX2 build's FloatVector.
#ifdef __AVX512F__
        return _mm512_load_ps(address);
#else
        return _mm256_load_ps(address);
#endif
    }

private:
    const SampleBuffer<StreamType>& buffer_;
};

template <typename StreamType>
struct Out<SampleBuffer<StreamType>> {
    // Clears the buffer right before its producer runs (see Out clears an output buffer in
    // spec.md).
    Out(SampleBuffer<StreamType>& samples) : buffer_(samples) { buffer_.count = 0; }

    // Starts this pass's output sample (see The header travels with its sample in spec.md).
    void set_header(const Header& header) {
        buffer_.slot->header = header;
        buffer_.count = 1;
    }

    void store(int vector_index, FloatVector values) {
        float* address =
            &buffer_.slot->values[static_cast<std::size_t>(vector_index * FLOATS_PER_VECTOR)];
        // Not if constexpr, for the same reason as in load().
#ifdef __AVX512F__
        _mm512_store_ps(address, values);
#else
        _mm256_store_ps(address, values);
#endif
    }

private:
    SampleBuffer<StreamType>& buffer_;
};

template <std::derived_from<Event> Record, int Capacity>
struct In<EventBuffer<Record, Capacity>> {
    In(const EventBuffer<Record, Capacity>& events) : buffer_(events) {}

    [[nodiscard]] bool is_empty() const { return buffer_.count == 0; }
    [[nodiscard]] int size() const { return buffer_.count; }
    [[nodiscard]] const Record* begin() const { return buffer_.records.data(); }
    [[nodiscard]] const Record* end() const { return buffer_.records.data() + buffer_.count; }

private:
    const EventBuffer<Record, Capacity>& buffer_;
};

template <std::derived_from<Event> Record, int Capacity>
struct Out<EventBuffer<Record, Capacity>> {
    // Clears the buffer right before its producer runs (see Out clears an output buffer in
    // spec.md).
    Out(EventBuffer<Record, Capacity>& events) : buffer_(events) { buffer_.count = 0; }

    void push(const Record& record) {
        buffer_.records[static_cast<std::size_t>(buffer_.count++)] = record;
    }  // unchecked: the producer never pushes more than Capacity per pass

private:
    EventBuffer<Record, Capacity>& buffer_;
};

// --- Cores (see Per-core loop and Start, stop and config in spec.md) ---

// Every core's loop runs while it is set. Cleared by run() on a stop signal, or by a core whose
// constructor failed.
std::atomic<bool> running = true;
std::atomic<bool> failed = false;   // set when a constructor threw on any core
std::latch* built = nullptr;        // the start barrier, created by run()
thread_local bool arrived = false;  // whether this core has reached the start barrier

// Start barrier: every core calls this once its processors are built.
// Returns false if a constructor failed on any core, so the core returns without running its loop.
bool all_built() {
    arrived = true;
    built->arrive_and_wait();
    return !failed;
}

struct Core {
    int cpu;         // CPU core the thread is pinned to
    void (*body)();  // declares the core's processors and buffers, calls all_built(), then runs its
                     // loop
};

// The stop signals: SIGINT (Ctrl+C) and SIGTERM (see Start, stop and config in spec.md).
sigset_t stop_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Waits for a stop signal while the cores run, then clears `running`. The timeout ends the wait
// when `running` was cleared without a signal (a constructor failed, or a core's thread couldn't be
// started).
void wait_for_stop_signal(const sigset_t& signals) {
    while (running) {
        const timespec timeout{.tv_sec = 0, .tv_nsec = 100'000'000};  // 100 ms
        const int signal_number = sigtimedwait(&signals, nullptr, &timeout);
        if (signal_number > 0) {
            // Temporary: printed directly until the logger exists (see Logging in spec.md). fputs,
            // unlike println, can't throw, so no exception leaves run().

            std::fputs(signal_number == SIGINT ? "\nCtrl+C received, stopping\n"
                                               : "\nSIGTERM received, stopping\n",
                       stderr);
            running = false;
        }
    }
}

// Runs one pinned thread per core until stopped. Returns 1 if the graph didn't start, 0 otherwise.
// A core's thread that can't be started also means the graph doesn't start, so no exception leaves
// run().
int run(std::initializer_list<Core> cores) {
    // Blocked before any core's thread starts, so every thread inherits the block and no stop
    // signal ever interrupts a core's loop. Only wait_for_stop_signal() takes them.
    const sigset_t signals = stop_signals();
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::fputs("can't block the stop signals\n", stderr);
        return 1;
    }

    std::latch latch(static_cast<std::ptrdiff_t>(cores.size()));
    built = &latch;

    std::vector<std::thread> threads;
    try {
        threads.reserve(cores.size());
        for (Core core : cores) {
            threads.emplace_back([core] {
                try {
                    cpu_set_t set;
                    CPU_ZERO(&set);
                    CPU_SET(core.cpu, &set);
                    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
                        throw std::runtime_error(std::format("can't pin to CPU {}", core.cpu));
                    }
                    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);  // see Flush-to-zero in spec.md
                    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
                    core.body();
                } catch (const std::exception& error) {
                    // A constructor failed (see Exceptions only outside the loop in spec.md).
                    std::println(stderr, "core {}: {}", core.cpu, error.what());
                    failed = true;
                    running = false;
                    if (!arrived) {
                        built->count_down();  // release the cores waiting at the barrier
                    }
                }
            });
        }
    } catch (const std::exception& error) {
        // A core's thread couldn't be started. Count down once for every core that never started,
        // so the started ones pass the start barrier, see failed and return. fputs, unlike
        // println, can't throw here.
        std::fputs("can't start a core's thread: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        failed = true;
        running = false;
        for (std::size_t missing = threads.size(); missing < cores.size(); ++missing) {
            latch.count_down();
        }
    }

    wait_for_stop_signal(signals);
    for (std::thread& thread : threads) {
        thread.join();
    }
    return failed ? 1 : 0;
}
