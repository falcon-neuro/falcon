#pragma once
#include <x86intrin.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include "../core/engine.cpp"

// LatencyBenchmark: logs, for each event, the time from its sample's arrival to when this processor
// runs. Called right before an output processor, it measures Falcon's work up to the decision. The
// rows stay in memory until the loop ends, since a realtime core makes no system calls.

struct LatencyBenchmarkOptions {
    Str file_prefix = "latency_benchmark";  // the log is <file_prefix>_<start time>.csv
};

template <std::derived_from<StreamEventRecord> Record, int Capacity,
          LatencyBenchmarkOptions options = LatencyBenchmarkOptions{}>
struct LatencyBenchmark {
    struct Connections {
        EventInputBuffer<Record, Capacity> input;
    };

    // Value-initialising the rows writes every page now, so the loop never takes a page fault on
    // its first row in a page.
    LatencyBenchmark() : rows_(row_capacity) {
        check_that_tsc_is_usable();
        csv_path_ = csv_path_for_now();
        csv_file_ = create_file(csv_path_);
        std::println("LatencyBenchmark: logging to {}", csv_path_);
        start_anchor_ = read_clock_anchor();
    }

    // A destructor must not throw, and the rows are the run's only copy, so a failure is printed.
    ~LatencyBenchmark() {
        try {
            write_log();
        } catch (const std::exception& error) {
            std::fputs("LatencyBenchmark: can't write the log: ", stderr);
            std::fputs(error.what(), stderr);
            std::fputs("\n", stderr);
        }
    }

    LatencyBenchmark(const LatencyBenchmark&) = delete;
    LatencyBenchmark& operator=(const LatencyBenchmark&) = delete;
    LatencyBenchmark(LatencyBenchmark&&) = delete;
    LatencyBenchmark& operator=(LatencyBenchmark&&) = delete;

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        // rdtscp waits until every earlier instruction has finished, so all the work before this
        // call is counted.
        unsigned processor_id = 0;
        const u64 decision_tsc = __rdtscp(&processor_id);
        for (const Record& event : connections.input) {
            if (recorded_count_ == row_capacity) {
                ++unrecorded_count_;
                continue;
            }
            rows_[recorded_count_] = {.header = event.header, .decision_tsc = decision_tsc};
            ++recorded_count_;
        }
    }

private:
    struct Row {
        SampleHeader header;
        u64 decision_tsc = 0;
    };

    // A TSC value and a CLOCK_MONOTONIC_RAW value read at the same moment.
    struct ClockAnchor {
        u64 tsc = 0;
        std::int64_t nanoseconds = 0;
    };

    struct FileCloser {
        void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
    };

    static constexpr std::size_t row_capacity = std::size_t{1} << 20U;

    std::vector<Row> rows_;
    std::size_t recorded_count_ = 0;
    u64 unrecorded_count_ = 0;
    std::string csv_path_;
    std::unique_ptr<std::FILE, FileCloser> csv_file_;
    ClockAnchor start_anchor_;

    // The arrival and the decision are read on different cores, and ticks must be a fixed unit of
    // time to become ns.
    static void check_that_tsc_is_usable() {
        std::ifstream cpu_info("/proc/cpuinfo");
        std::string line;
        bool has_found_flags = false;
        while (std::getline(cpu_info, line)) {
            if (line.starts_with("flags")) {
                has_found_flags = true;
                break;
            }
        }
        const std::size_t colon_position = line.find(':');
        if (!has_found_flags || colon_position == std::string::npos) {
            throw std::runtime_error("LatencyBenchmark: no CPU flags in /proc/cpuinfo");
        }
        std::istringstream flag_words(line.substr(colon_position + 1));
        bool has_constant_tsc = false;
        bool has_nonstop_tsc = false;
        std::string flag;
        while (flag_words >> flag) {
            if (flag == "constant_tsc") {
                has_constant_tsc = true;
            }
            if (flag == "nonstop_tsc") {
                has_nonstop_tsc = true;
            }
        }
        if (!has_constant_tsc || !has_nonstop_tsc) {
            throw std::runtime_error(
                "LatencyBenchmark: the CPU's TSC is not invariant (/proc/cpuinfo lacks "
                "constant_tsc or nonstop_tsc), so its ticks are not a fixed unit of time");
        }

        std::ifstream clock_source_file(
            "/sys/devices/system/clocksource/clocksource0/current_clocksource");
        std::string clock_source;
        clock_source_file >> clock_source;
        if (clock_source != "tsc") {
            throw std::runtime_error(std::format(
                "LatencyBenchmark: the kernel's clock source is '{}', not tsc, so the cores' TSCs "
                "may disagree",
                clock_source));
        }
    }

    [[nodiscard]] static std::string csv_path_for_now() {
        const std::time_t start_time = std::time(nullptr);
        std::tm local_start_time{};
        if (localtime_r(&start_time, &local_start_time) == nullptr) {
            throw std::runtime_error("LatencyBenchmark: can't read the local time");
        }
        std::array<char, 32> start_time_text{};
        if (std::strftime(start_time_text.data(), start_time_text.size(), "%Y%m%d_%H%M%S",
                          &local_start_time) == 0) {
            throw std::runtime_error("LatencyBenchmark: can't format the start time");
        }
        return std::format("{}_{}.csv", options.file_prefix.c_str(), start_time_text.data());
    }

    // "x" makes fopen fail if the file exists, so a run never overwrites another's log.
    [[nodiscard]] static std::unique_ptr<std::FILE, FileCloser> create_file(
        const std::string& path) {
        std::FILE* const file = std::fopen(path.c_str(), "wx");
        if (file == nullptr) {
            throw std::runtime_error(std::format("LatencyBenchmark: can't create {}: {}", path,
                                                 std::system_category().message(errno)));
        }
        return std::unique_ptr<std::FILE, FileCloser>(file);
    }

    // The narrowest of several TSC, clock, TSC brackets was not interrupted, so its midpoint pairs
    // the clock's value with the TSC's.
    [[nodiscard]] static ClockAnchor read_clock_anchor() {
        constexpr int bracket_count = 32;
        constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
        ClockAnchor best_anchor;
        u64 best_width = std::numeric_limits<u64>::max();
        for (int bracket_index = 0; bracket_index < bracket_count; ++bracket_index) {
            unsigned processor_id = 0;
            const u64 tsc_before = __rdtscp(&processor_id);
            timespec clock_value{};
            const int clock_result = clock_gettime(CLOCK_MONOTONIC_RAW, &clock_value);
            const u64 tsc_after = __rdtscp(&processor_id);
            if (clock_result != 0 || tsc_after <= tsc_before) {
                continue;
            }
            const u64 width = tsc_after - tsc_before;
            if (width < best_width) {
                best_width = width;
                best_anchor = {
                    .tsc = tsc_before + (width / 2),
                    .nanoseconds =
                        (static_cast<std::int64_t>(clock_value.tv_sec) * nanoseconds_per_second) +
                        clock_value.tv_nsec,
                };
            }
        }
        return best_anchor;
    }

    // Nearest rank, so the value is one that was measured.
    [[nodiscard]] static double value_at_fraction(const std::vector<double>& sorted_values,
                                                  double fraction) {
        const auto last_index = static_cast<double>(sorted_values.size() - 1);
        return sorted_values[static_cast<std::size_t>(std::lround(fraction * last_index))];
    }

    void write_log() {
        // Measured over the whole run instead of at start: more exact, and nothing waits at start.
        const ClockAnchor end_anchor = read_clock_anchor();
        if (end_anchor.tsc <= start_anchor_.tsc ||
            end_anchor.nanoseconds <= start_anchor_.nanoseconds) {
            throw std::runtime_error("the TSC or the clock didn't advance during the run");
        }
        const double nanoseconds_per_tick =
            static_cast<double>(end_anchor.nanoseconds - start_anchor_.nanoseconds) /
            static_cast<double>(end_anchor.tsc - start_anchor_.tsc);

        std::println(csv_file_.get(), "sample_number,device_timestamp,latency_ns");
        std::vector<double> latencies_nanoseconds;
        latencies_nanoseconds.reserve(recorded_count_);
        for (std::size_t row_index = 0; row_index < recorded_count_; ++row_index) {
            const Row& row = rows_[row_index];
            // Signed, so a decision read before the arrival, which only cores whose TSCs disagree
            // can produce, shows as a negative latency instead of a huge one.
            const auto latency_ticks =
                static_cast<std::int64_t>(row.decision_tsc - row.header.arrival_tsc);
            const double latency_nanoseconds =
                static_cast<double>(latency_ticks) * nanoseconds_per_tick;
            latencies_nanoseconds.push_back(latency_nanoseconds);
            std::println(csv_file_.get(), "{},{},{:.0f}", row.header.sample_number,
                         row.header.device_timestamp, latency_nanoseconds);
        }
        std::FILE* const file = csv_file_.release();
        if (std::fclose(file) != 0) {
            throw std::runtime_error(std::format("can't finish writing {}", csv_path_));
        }

        if (latencies_nanoseconds.empty()) {
            std::println("LatencyBenchmark: no events, {} has its header only", csv_path_);
        } else {
            std::ranges::sort(latencies_nanoseconds);
            constexpr double nanoseconds_per_microsecond = 1000.0;
            std::println(
                "LatencyBenchmark: {} events, µs from arrival: min {:.2f}, median {:.2f}, 99th "
                "percentile {:.2f}, max {:.2f} (each event in {})",
                latencies_nanoseconds.size(),
                latencies_nanoseconds.front() / nanoseconds_per_microsecond,
                value_at_fraction(latencies_nanoseconds, 0.5) / nanoseconds_per_microsecond,
                value_at_fraction(latencies_nanoseconds, 0.99) / nanoseconds_per_microsecond,
                latencies_nanoseconds.back() / nanoseconds_per_microsecond, csv_path_);
        }
        if (unrecorded_count_ > 0) {
            std::println("LatencyBenchmark: the log was full, so {} later events weren't kept",
                         unrecorded_count_);
        }
    }
};
