// Logging test: what a run logs when nothing fails. It runs until Ctrl+C and then exits with 0.
// - lifetime_reporter logs from its constructor and its destructor. Expected: "built" before any
//   line from a loop, and "destroyed" after the line that reports Ctrl+C.
// - first_warner raises 1000 warnings a second and second_warner 250 a second, for one second
//   each, far more than the throttle shows. Expected: about one line per processor per 100 ms,
//   first_warner's with (×100) and second_warner's with (×25), each showing the occurrence its
//   text came from; after a second the lines stop, and the counts add up to 1000 and 250.
// - argument_logger raises one message for each kind of argument. Expected: six lines, showing
//   no arguments, numbers, an enum as its number, two texts, a text cut to end in "...", and the
//   channels 3, 17 and 31 of a mask.
// - sample_source writes 1000 samples in one second into a 4-slot ring, and slow_sample_counter
//   takes 2 ms for each, so it reads about half of them. Expected: about one warning per 100 ms
//   from the ring, with the samples it dropped; after Ctrl+C, sample_source logs the 1000 it
//   output and slow_sample_counter how many it received, which with the ring's total of dropped
//   samples make 1000.
#include "../../core/engine.cpp"
#include "logging_test_processors.cpp"

using TestStream = StreamInfo<16, 1000.0, float>;

// Connections between cores
SampleRing<TestStream, 4> sample_source_to_slow_sample_counter_sample_ring{
    "sample_source_to_slow_sample_counter_sample_ring"};

void core1() {
    // Processors
    LifetimeReporter<> lifetime_reporter{"lifetime_reporter"};
    RepeatedWarner<{.warnings_per_second = 1000}> first_warner{"first_warner"};
    RepeatedWarner<{.warnings_per_second = 250}> second_warner{"second_warner"};
    SampleSource<TestStream> sample_source{"sample_source"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        lifetime_reporter.process({});
        first_warner.process({});
        second_warner.process({});
        SampleBuffer<TestStream>& slot = sample_source_to_slow_sample_counter_sample_ring.next_slot();
        sample_source.process({.output = slot});
        sample_source_to_slow_sample_counter_sample_ring.publish();
    }
}

void core2() {
    // Processors
    ArgumentLogger argument_logger{"argument_logger"};
    SlowSampleCounter<TestStream> slow_sample_counter{"slow_sample_counter"};

    // Connections
    SampleBuffer<TestStream> sample_source_to_slow_sample_counter_sample_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        argument_logger.process({});
        sample_source_to_slow_sample_counter_sample_ring.read(
            sample_source_to_slow_sample_counter_sample_buffer);
        slow_sample_counter.process({.input = sample_source_to_slow_sample_counter_sample_buffer});
    }
}

int main() {
    return run_on_cores("logging_tour_test", {
        {.core_number = 1, .entry_point = core1},
        {.core_number = 2, .entry_point = core2},
    });
}
