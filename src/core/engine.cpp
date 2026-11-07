#pragma once
#include <pmmintrin.h>
#include <pthread.h>
#include <xmmintrin.h>
#include <array>
#include <atomic>
#include <concepts>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <initializer_list>
#include <latch>
#include <print>
#include <stdexcept>
#include <thread>
#include <vector>

// Falcon engine (see src/core/spec.md). Only what the monotonous event pipeline needs so far:
// events, In and Out, and one pinned thread per core with the start barrier.

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- Events (see Events in spec.md) ---

// Base of every event record. Empty: an event from a timer has no sample to point to.
struct Event {};

// Event buffer: up to N records produced on this pass (see Capacity in spec.md).
// The records sit inside the buffer; pointing at a ring slot instead comes with event rings
// (beyond_prototype.md).
template <std::derived_from<Event> E, int N>
struct Events {
    std::array<E, N> records;
    int count = 0;
};

// --- In and Out (see process() signature in spec.md) ---
// Every process() parameter is an In<T> or an Out<T>, and each exposes only what its direction
// allows.

template <typename T>
struct In;
template <typename T>
struct Out;

template <std::derived_from<Event> E, int N>
struct In<Events<E, N>> {
    const Events<E, N>& buffer;

    In(const Events<E, N>& events) : buffer(events) {}

    [[nodiscard]] bool is_empty() const { return buffer.count == 0; }
    [[nodiscard]] int size() const { return buffer.count; }
    const E* begin() const { return buffer.records.data(); }
    const E* end() const { return buffer.records.data() + buffer.count; }
};

template <std::derived_from<Event> E, int N>
struct Out<Events<E, N>> {
    Events<E, N>& buffer;

    // Clears the buffer right before its producer runs (see Out clears an output buffer in
    // spec.md).
    Out(Events<E, N>& events) : buffer(events) { buffer.count = 0; }

    void push(const E& record) {
        buffer.records[buffer.count++] = record;
    }  // unchecked: the producer never pushes more than N per pass
};

// --- Cores (see Per-core loop and Start, stop and config in spec.md) ---

std::atomic<bool> running =
    true;  // cleared by Ctrl+C or SIGTERM; every core's loop runs while it is set
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

// Runs one pinned thread per core until stopped. Returns 1 if the graph didn't start, 0 otherwise.
// A core's thread that can't be started also means the graph doesn't start, so no exception leaves
// run().
int run(std::initializer_list<Core> cores) {
    std::latch latch(static_cast<std::ptrdiff_t>(cores.size()));
    built = &latch;
    std::signal(SIGINT, [](int) { running = false; });
    std::signal(SIGTERM, [](int) { running = false; });

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
    for (std::thread& thread : threads) {
        thread.join();
    }
    return failed ? 1 : 0;
}
