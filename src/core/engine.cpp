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

// Falcon engine (see src/core/spec.md).

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- Instruction set (see One processor per stream in spec.md) ---

#ifndef __AVX2__
#error "Falcon needs AVX2: compile with -mavx2 or -march=native"
#endif

#ifdef __AVX512F__
inline constexpr bool HAS_AVX512 = true;
#else
inline constexpr bool HAS_AVX512 = false;
#endif

inline constexpr int FLOATS_PER_SIMD_VECTOR = HAS_AVX512 ? 16 : 8;
using SimdVector = std::conditional_t<HAS_AVX512, __m512, __m256>;

// --- Streams and samples (see Stream type, Buffers and Sample type in spec.md) ---

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

// Set by the input processor. Other processors pass it on and never make one up (see Buffers in
// spec.md).
struct SampleHeader {
    u64 sample_number = 0;     // counted at the input's rate, derived from the device's clock
    u64 device_timestamp = 0;  // in the device's own unit; processors never calculate with it
    u64 arrival_tsc = 0;       // CPU timestamp counter when the sample arrived
};

template <typename StreamInfoType>
struct SampleWithHeader {
    SampleHeader header;
    alignas(64) std::array<typename StreamInfoType::ChannelValue, StreamInfoType::padded_channels> values;
};

// 0 or 1 sample. `sample` points at the buffer's own storage, or at a ring slot, so nothing is
// copied across rings (see graph.cpp in spec.md).
template <typename StreamInfoType>
struct SampleBuffer {
    SampleWithHeader<StreamInfoType> storage{};
    SampleWithHeader<StreamInfoType>* sample = &storage;
    int count = 0;

    SampleBuffer() = default;
    // A copy's `sample` would still point at the original's storage.
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;
};

// --- Events (see Events in spec.md) ---

// Base of every event record. Empty: an event from a timer has no sample to point to.
struct EventRecord {};

// Up to Capacity records produced on this pass (see Capacity in spec.md).
// The records sit inside the buffer; pointing at a ring slot instead comes with event rings
// (beyond_prototype.md).
template <std::derived_from<EventRecord> Record, int Capacity>
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

template <typename StreamInfoType>
struct In<SampleBuffer<StreamInfoType>> {
    In(const SampleBuffer<StreamInfoType>& samples) : header(samples.sample->header), buffer_(samples) {}

    const SampleHeader& header;

    [[nodiscard]] bool is_empty() const { return buffer_.count == 0; }

    [[nodiscard]] SimdVector load(int simd_vector_index) const {
        const float* address =
            &buffer_.sample->values[static_cast<std::size_t>(simd_vector_index * FLOATS_PER_SIMD_VECTOR)];
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
    // Clears the buffer right before its producer runs (see Out clears an output buffer in
    // spec.md).
    Out(SampleBuffer<StreamInfoType>& samples) : buffer_(samples) { buffer_.count = 0; }

    void set_header(const SampleHeader& header) {
        buffer_.sample->header = header;
        buffer_.count = 1;
    }

    void store(int simd_vector_index, SimdVector values) {
        float* address =
            &buffer_.sample->values[static_cast<std::size_t>(simd_vector_index * FLOATS_PER_SIMD_VECTOR)];
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

    [[nodiscard]] bool is_empty() const { return buffer_.count == 0; }
    [[nodiscard]] int size() const { return buffer_.count; }
    [[nodiscard]] const Record* begin() const { return buffer_.records.data(); }
    [[nodiscard]] const Record* end() const { return buffer_.records.data() + buffer_.count; }

private:
    const EventBuffer<Record, Capacity>& buffer_;
};

template <std::derived_from<EventRecord> Record, int Capacity>
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

template <typename StreamInfoType>
using SampleInputBuffer = In<SampleBuffer<StreamInfoType>>;
template <typename StreamInfoType>
using SampleOutputBuffer = Out<SampleBuffer<StreamInfoType>>;
template <std::derived_from<EventRecord> Record, int Capacity>
using EventInputBuffer = In<EventBuffer<Record, Capacity>>;
template <std::derived_from<EventRecord> Record, int Capacity>
using EventOutputBuffer = Out<EventBuffer<Record, Capacity>>;
template <typename StateType>
using StateInput = In<StateType>;
template <typename StateType>
using StateOutput = Out<StateType>;

// --- Cores (see Per-core loop and Start, stop and config in spec.md) ---

// Every core's loop runs while it is set. Cleared by run_on_cores() on a stop signal, or by a core
// whose constructor failed.
std::atomic<bool> is_cores_running = true;
// Set when a constructor threw on any core, or a core's thread couldn't be started.
std::atomic<bool> has_startup_failed = false;
std::latch* start_barrier = nullptr;  // created by run_on_cores()
thread_local bool has_arrived_at_start_barrier = false;

// Every core calls this once its processors are built. Returns false if startup failed on any
// core, so the core returns without running its loop.
bool wait_at_start_barrier() {
    has_arrived_at_start_barrier = true;
    start_barrier->arrive_and_wait();
    return !has_startup_failed;
}

struct CoreThread {
    int core_number;        // CPU core the thread is pinned to
    void (*entry_point)();  // declares the core's processors and buffers, calls
                            // wait_at_start_barrier(), then runs its loop
};

// The stop signals: SIGINT (Ctrl+C) and SIGTERM (see Start, stop and config in spec.md).
sigset_t stop_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Waits for a stop signal while the cores run, then clears `is_cores_running`. The timeout ends the
// wait when `is_cores_running` was cleared without a signal (startup failed).
void wait_for_stop_signal(const sigset_t& signals) {
    while (is_cores_running) {
        const timespec timeout{.tv_sec = 0, .tv_nsec = 100'000'000};  // 100 ms
        const int signal_number = sigtimedwait(&signals, nullptr, &timeout);
        if (signal_number > 0) {
            // Temporary: printed directly until the logger exists (see Logging in spec.md). fputs,
            // unlike println, can't throw, so no exception leaves run_on_cores().

            std::fputs(signal_number == SIGINT ? "\nCtrl+C received, stopping\n"
                                               : "\nSIGTERM received, stopping\n",
                       stderr);
            is_cores_running = false;
        }
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
                    cpu_set_t set;
                    CPU_ZERO(&set);
                    CPU_SET(core_thread.core_number, &set);
                    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
                        throw std::runtime_error(
                            std::format("can't pin to CPU {}", core_thread.core_number));
                    }
                    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);  // see Flush-to-zero in spec.md
                    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
                    core_thread.entry_point();
                } catch (const std::exception& error) {
                    // A constructor failed (see Exceptions only outside the loop in spec.md).
                    std::println(stderr, "core {}: {}", core_thread.core_number, error.what());
                    has_startup_failed = true;
                    is_cores_running = false;
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
        is_cores_running = false;
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
