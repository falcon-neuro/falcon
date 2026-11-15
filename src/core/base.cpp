#pragma once
#include <x86intrin.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <thread>

// What both the logger and the engine build on.

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- Run state ---

// Every core's loop runs while it is set. Cleared by run_on_cores() on a stop signal, or by a core
// whose processor threw.
std::atomic<bool> is_pipeline_running = true;

// --- Processor names ---

[[nodiscard]] constexpr bool is_letter_or_digit(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
}

// A processor's name, as the user gave it in the UI: the same as its variable's in pipeline.cpp.
// consteval, so it is a string literal, whose text outlives the processor: the main thread may
// print a line about it after it is gone.
class ProcessorName {
public:
    consteval ProcessorName(const char* text) : text_(text) {
        if (text[0] == '\0') {
            throw std::invalid_argument("a processor's name can't be empty");
        }
        if (text[0] >= '0' && text[0] <= '9') {
            throw std::invalid_argument("a processor's name, like its variable's, can't start with "
                                        "a digit");
        }
        for (std::size_t index = 0; text[index] != '\0'; ++index) {
            if (!is_letter_or_digit(text[index]) && text[index] != '_') {
                throw std::invalid_argument("a processor's name holds only letters, digits and _, "
                                            "like its variable's");
            }
        }
    }

    [[nodiscard]] constexpr const char* c_str() const { return text_; }

private:
    const char* text_;
};

// --- Clocks ---

// The TSC and CLOCK_MONOTONIC_RAW at the start of the run. The TSC ticks at a constant rate,
// measured against that clock over the run so far.
u64 run_start_tsc = 0;
double run_start_seconds = 0;
// Measured before any core's thread starts, so a constructor can turn a duration into TSC ticks
// and its loop then times it without a system call.
double tsc_ticks_per_second = 0;

[[nodiscard]] timespec read_clock(clockid_t clock) noexcept {
    timespec time{};
    clock_gettime(clock, &time);
    return time;
}

[[nodiscard]] double seconds_of(const timespec& time) noexcept {
    constexpr double seconds_per_nanosecond = 1e-9;
    return static_cast<double>(time.tv_sec) +
           (static_cast<double>(time.tv_nsec) * seconds_per_nanosecond);
}

struct ClockReading {
    u64 tsc = 0;
    double monotonic_seconds = 0;
    timespec wall_clock{};
};

[[nodiscard]] ClockReading read_clocks() noexcept {
    return {.tsc = __rdtsc(),
            .monotonic_seconds = seconds_of(read_clock(CLOCK_MONOTONIC_RAW)),
            .wall_clock = read_clock(CLOCK_REALTIME),
    };
}

void start_run_clock() {
    run_start_tsc = __rdtsc();
    run_start_seconds = seconds_of(read_clock(CLOCK_MONOTONIC_RAW));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const ClockReading now = read_clocks();
    tsc_ticks_per_second =
        static_cast<double>(now.tsc - run_start_tsc) / (now.monotonic_seconds - run_start_seconds);
}

// The wall-clock time of a TSC reading taken shortly before `now`.
[[nodiscard]] timespec wall_clock_of(u64 tsc, const ClockReading& now) noexcept {
    const double elapsed_seconds = now.monotonic_seconds - run_start_seconds;
    if (tsc > now.tsc || now.tsc == run_start_tsc || elapsed_seconds <= 0) {
        return now.wall_clock;
    }
    const double seconds_before = static_cast<double>(now.tsc - tsc) * elapsed_seconds /
                                  static_cast<double>(now.tsc - run_start_tsc);
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
    constexpr double nanoseconds_per_second_as_double = 1e9;
    const std::int64_t nanoseconds =
        (static_cast<std::int64_t>(now.wall_clock.tv_sec) * nanoseconds_per_second) +
        now.wall_clock.tv_nsec -
        static_cast<std::int64_t>(seconds_before * nanoseconds_per_second_as_double);
    return {.tv_sec = static_cast<std::time_t>(nanoseconds / nanoseconds_per_second),
            .tv_nsec = static_cast<long>(nanoseconds % nanoseconds_per_second),
    };
}
