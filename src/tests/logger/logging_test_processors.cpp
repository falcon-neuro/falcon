#pragma once
#include <x86intrin.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "../../core/engine.cpp"

// Test processors for the logging tests in this folder.

// How a LifetimeReporter ends the run.
enum class TestFailure : std::uint8_t {
    None,
    InConstructor,
    InProcess,
    NonStdExceptionInProcess,
};

struct LifetimeReporterOptions {
    TestFailure failure = TestFailure::None;
    double seconds_before_failure = 1;  // from its first pass, for a failure in process()
};

// LifetimeReporter: logs when it is built and when it is destroyed, so a run shows which
// processors existed and whose destructors ran. It can also end the run, in the way its options
// say.
template <LifetimeReporterOptions options = LifetimeReporterOptions{}>
struct LifetimeReporter {
    static_assert(options.seconds_before_failure >= 0);

    struct Connections {};

    explicit LifetimeReporter(ProcessorName processor_name) : name(processor_name) {
        if constexpr (options.failure == TestFailure::InConstructor) {
            fail(name, "failed in its constructor on purpose");
        } else {
            ticks_before_failure_ =
                static_cast<u64>(options.seconds_before_failure * tsc_ticks_per_second);
            log(name, "built");
        }
    }

    ~LifetimeReporter() { log(name, "destroyed"); }

    LifetimeReporter(const LifetimeReporter&) = delete;
    LifetimeReporter& operator=(const LifetimeReporter&) = delete;
    LifetimeReporter(LifetimeReporter&&) = delete;
    LifetimeReporter& operator=(LifetimeReporter&&) = delete;

    RegisteredName name;

    void process(Connections /*connections*/) {
        if constexpr (options.failure == TestFailure::InProcess ||
                      options.failure == TestFailure::NonStdExceptionInProcess) {
            const u64 now = __rdtsc();
            if (!has_started_) {
                first_pass_tsc_ = now;
                has_started_ = true;
            }
            if (now - first_pass_tsc_ < ticks_before_failure_) {
                return;
            }
            if constexpr (options.failure == TestFailure::InProcess) {
                fail(name, "failed in process() on purpose, {} s after its first pass",
                     options.seconds_before_failure);
            } else {
                // The test needs an exception that isn't a std::exception, which the engine can
                // only name by its type.
                // NOLINTNEXTLINE(bugprone-std-exception-baseclass)
                throw 42;
            }
        }
    }

private:
    u64 ticks_before_failure_ = 0;
    u64 first_pass_tsc_ = 0;
    bool has_started_ = false;
};

struct RepeatedWarnerOptions {
    double warnings_per_second = 1000;
    double seconds = 1;  // how long it warns, from its first pass
};

// RepeatedWarner: raises the same warning at a fixed rate for a while, then stays quiet. The rate
// is far above what the throttle lets through, so its lines show the throttle's counts, and its
// last line shows the count of a warning that has stopped. Each warning carries its number, so a
// line shows which occurrence its text came from.
template <RepeatedWarnerOptions options = RepeatedWarnerOptions{}>
struct RepeatedWarner {
    static_assert(options.warnings_per_second > 0);
    static_assert(options.seconds > 0);

    struct Connections {};

    explicit RepeatedWarner(ProcessorName processor_name)
        : name(processor_name),
          ticks_per_warning_(tsc_ticks_per_second / options.warnings_per_second) {}

    RegisteredName name;

    void process(Connections /*connections*/) {
        if (raised_count_ == warning_count) {
            return;
        }
        const u64 now = __rdtsc();
        if (!has_started_) {
            first_pass_tsc_ = now;
            has_started_ = true;
        }
        const u64 due_tsc = first_pass_tsc_ + static_cast<u64>(static_cast<double>(raised_count_) *
                                                               ticks_per_warning_);
        if (now < due_tsc) {
            return;
        }
        ++raised_count_;
        warn(name, "test warning, occurrence {} of {}", raised_count_, warning_count);
    }

private:
    static constexpr u64 warning_count =
        static_cast<u64>(options.warnings_per_second * options.seconds);

    double ticks_per_warning_;
    u64 first_pass_tsc_ = 0;
    u64 raised_count_ = 0;
    bool has_started_ = false;
};

enum class TestStage : std::uint8_t { Waiting, Running, Stopping };

// ArgumentLogger: raises one message for each kind of argument a message can carry, one per pass.
// From the loop, so each goes through its core's ring and the main thread formats it.
struct ArgumentLogger {
    struct Connections {};

    explicit ArgumentLogger(ProcessorName processor_name) : name(processor_name) {}

    RegisteredName name;

    void process(Connections /*connections*/) {
        switch (next_message_) {
            case 0:
                log(name, "no arguments");
                break;
            case 1:
                log(name, "numbers: {}, {}, {}, {:.3f} and {}", 42, -7, u64{1} << 40U, 0.125, true);
                break;
            case 2:
                log(name, "an enum, shown as its number: {}", TestStage::Stopping);
                break;
            case 3:
                log(name, "text: {} and {}", "a literal", std::string_view("a string_view"));
                break;
            case 4:
                warn(name, "text too long for a record, so cut: {}",
                     std::string_view(text_longer_than_a_record.data(),
                                      text_longer_than_a_record.size()));
                break;
            case 5:
                log(name, "a channel mask: channels {}", example_mask());
                break;
            default:
                return;
        }
        ++next_message_;
    }

private:
    using MaskStreamInfo = StreamInfo<32, 30000.0, float>;
    static constexpr auto channels_per_vector = static_cast<unsigned>(FLOATS_PER_SIMD_VECTOR);

    // Repeats the digits 0 to 9, so the line shows where the text was cut.
    static constexpr std::array<char, MESSAGE_ARGUMENTS_CAPACITY + 50> text_longer_than_a_record =
        [] {
            std::array<char, MESSAGE_ARGUMENTS_CAPACITY + 50> text{};
            for (std::size_t index = 0; index < text.size(); ++index) {
                text[index] = static_cast<char>('0' + (index % 10));
            }
            return text;
        }();

    int next_message_ = 0;

    [[nodiscard]] static ChannelMask<MaskStreamInfo> example_mask() {
        ChannelMask<MaskStreamInfo> mask;
        for (const unsigned channel : {3U, 17U, 31U}) {
            ChannelMask<MaskStreamInfo> channel_mask;
            channel_mask.set_vector(static_cast<int>(channel / channels_per_vector),
                                    {.bits = 1U << (channel % channels_per_vector)});
            mask |= channel_mask;
        }
        return mask;
    }
};

struct SampleSourceOptions {
    double seconds = 1;  // how long it outputs samples, from its first pass
};

// SampleSource: outputs samples of zeros at its stream's rate for a while, numbered from 0. It
// times them by the TSC and never waits, so other processors can share its core. When destroyed,
// it logs how many samples it output.
template <typename OutputStreamInfo, SampleSourceOptions options = SampleSourceOptions{}>
struct SampleSource {
    static_assert(options.seconds > 0);

    struct Connections {
        SampleOutputBuffer<OutputStreamInfo> output;
    };

    explicit SampleSource(ProcessorName processor_name)
        : name(processor_name),
          ticks_per_sample_(tsc_ticks_per_second / OutputStreamInfo::rate) {}

    ~SampleSource() { log(name, "output {} samples", output_count_); }

    SampleSource(const SampleSource&) = delete;
    SampleSource& operator=(const SampleSource&) = delete;
    SampleSource(SampleSource&&) = delete;
    SampleSource& operator=(SampleSource&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        if (output_count_ == sample_count) {
            return;
        }
        const u64 now = __rdtsc();
        if (!has_started_) {
            first_pass_tsc_ = now;
            has_started_ = true;
        }
        const u64 due_tsc = first_pass_tsc_ + static_cast<u64>(static_cast<double>(output_count_) *
                                                               ticks_per_sample_);
        if (now < due_tsc) {
            return;
        }
        connections.output.set_header({
            .sample_number = output_count_,
            .device_timestamp = output_count_,
            .arrival_tsc = now,
            .lost_sample_count = 0,
        });
        for (int simd_vector_index = 0; simd_vector_index < OutputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            if constexpr (HAS_AVX512) {
                connections.output.store(simd_vector_index, _mm512_setzero_ps());
            } else {
                connections.output.store(simd_vector_index, _mm256_setzero_ps());
            }
        }
        ++output_count_;
    }

private:
    static constexpr u64 sample_count =
        static_cast<u64>(OutputStreamInfo::rate * options.seconds);

    double ticks_per_sample_;
    u64 first_pass_tsc_ = 0;
    u64 output_count_ = 0;
    bool has_started_ = false;
};

struct SlowSampleCounterOptions {
    double milliseconds_per_sample = 2;  // how long it keeps its core busy for each sample
};

// SlowSampleCounter: stands for a processor too slow for its stream. It keeps its core busy for a
// while on each sample, so a ring that feeds it fills up and drops samples. When destroyed, it
// logs how many samples it received and the lost-sample count of the last one's header.
template <typename InputStreamInfo, SlowSampleCounterOptions options = SlowSampleCounterOptions{}>
struct SlowSampleCounter {
    static_assert(options.milliseconds_per_sample >= 0);

    struct Connections {
        SampleInputBuffer<InputStreamInfo> input;
    };

    explicit SlowSampleCounter(ProcessorName processor_name)
        : name(processor_name),
          busy_ticks_per_sample_(static_cast<u64>(options.milliseconds_per_sample /
                                                  milliseconds_per_second * tsc_ticks_per_second)) {}

    ~SlowSampleCounter() {
        log(name, "received {} samples; the last one's header counts {} lost before it",
            received_count_, lost_sample_count_);
    }

    SlowSampleCounter(const SlowSampleCounter&) = delete;
    SlowSampleCounter& operator=(const SlowSampleCounter&) = delete;
    SlowSampleCounter(SlowSampleCounter&&) = delete;
    SlowSampleCounter& operator=(SlowSampleCounter&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        ++received_count_;
        lost_sample_count_ = connections.input.header.lost_sample_count;
        const u64 busy_start_tsc = __rdtsc();
        while (__rdtsc() - busy_start_tsc < busy_ticks_per_sample_) {
            _mm_pause();
        }
    }

private:
    static constexpr double milliseconds_per_second = 1000;

    u64 busy_ticks_per_sample_;
    u64 received_count_ = 0;
    u64 lost_sample_count_ = 0;
};
