#pragma once
#include <x86intrin.h>
#include <algorithm>
#include <limits>
#include "../../core/engine.cpp"

// Test processors for the state value tests in this folder.

struct DetectionPauserOptions {
    // How long detection stays off, from the pass that sees a lockout begin.
    double pause_milliseconds = 750;
};

// DetectionPauser: switches a detector's detection off for a while after each of its detections,
// which it sees as the start of the detector's lockout. It reads one state value of the detector
// and writes another, so with the two on different cores a state value crosses in each direction.
// It times every lockout it sees, from the pass that sees it begin to the pass that sees it end,
// and logs the shortest and the longest when it is destroyed.
template <DetectionPauserOptions options = DetectionPauserOptions{}>
struct DetectionPauser {
    static_assert(options.pause_milliseconds > 0);

    struct Connections {
        StateInput<bool, false> is_in_lockout = UNCONNECTED;
        StateOutput<bool, true> is_detection_enabled;
    };

    explicit DetectionPauser(ProcessorName processor_name)
        : name(processor_name),
          pause_ticks_(static_cast<u64>(options.pause_milliseconds / milliseconds_per_second *
                                        tsc_ticks_per_second)) {}

    ~DetectionPauser() {
        if (lockout_end_count_ == 0) {
            log(name, "saw {} lockouts begin and none end", lockout_start_count_);
            return;
        }
        log(name, "saw {} lockouts begin and {} end, the shortest {} ms and the longest {} ms",
            lockout_start_count_, lockout_end_count_, milliseconds_of(shortest_lockout_ticks_),
            milliseconds_of(longest_lockout_ticks_));
    }

    DetectionPauser(const DetectionPauser&) = delete;
    DetectionPauser& operator=(const DetectionPauser&) = delete;
    DetectionPauser(DetectionPauser&&) = delete;
    DetectionPauser& operator=(DetectionPauser&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        const bool is_in_lockout = connections.is_in_lockout;
        const bool has_lockout_begun = is_in_lockout && !was_in_lockout_;
        const bool has_lockout_ended = !is_in_lockout && was_in_lockout_;
        was_in_lockout_ = is_in_lockout;
        const u64 now = __rdtsc();
        if (has_lockout_begun) {
            lockout_start_tsc_ = now;
            is_pausing_ = true;
            ++lockout_start_count_;
            connections.is_detection_enabled = false;
            return;
        }
        if (has_lockout_ended) {
            const u64 lockout_ticks = now - lockout_start_tsc_;
            shortest_lockout_ticks_ = std::min(shortest_lockout_ticks_, lockout_ticks);
            longest_lockout_ticks_ = std::max(longest_lockout_ticks_, lockout_ticks);
            ++lockout_end_count_;
        }
        if (is_pausing_ && now - lockout_start_tsc_ >= pause_ticks_) {
            is_pausing_ = false;
            connections.is_detection_enabled = true;
        }
    }

private:
    static constexpr double milliseconds_per_second = 1000;

    u64 pause_ticks_;
    u64 lockout_start_tsc_ = 0;
    u64 lockout_start_count_ = 0;
    u64 lockout_end_count_ = 0;
    u64 shortest_lockout_ticks_ = std::numeric_limits<u64>::max();
    u64 longest_lockout_ticks_ = 0;
    bool was_in_lockout_ = false;
    bool is_pausing_ = false;

    [[nodiscard]] static double milliseconds_of(u64 ticks) {
        return static_cast<double>(ticks) / tsc_ticks_per_second * milliseconds_per_second;
    }
};
