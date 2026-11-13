// FIR chain latency benchmark: a square wave on every channel passes unchanged through 7
// FirFilters, and once every channel has crossed the level, Trigger fires. LatencyBenchmark logs
// each trigger's time from its sample's arrival to the decision. Expect one trigger per rising
// edge, 47 per second.
//
//   SignalGenerator -> ring -> 7 pass-through FirFilters in series -> LevelCrossingDetector
//   -> Trigger -> LatencyBenchmark
#include <numbers>
#include "../core/engine.cpp"
#include "../processors/fir_filter.cpp"
#include "../processors/latency_benchmark.cpp"
#include "../processors/level_crossing_detector.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/trigger.cpp"

using LatencyStream = StreamInfo<10000, 40000.0, float>;

// y = x, so every stage passes the signal unchanged; a tap of 1 is that at any rate.
using PassThroughFilter =
    FirFilter<LatencyStream,
              FirFilterOptions<1>{.design_rate = LatencyStream::rate, .taps = {1.0}}>;

// Diagnostic: the channels' rising edges are spread evenly over this many samples, so the trigger
// sample, where the last channel crosses, carries about channels / CROSSING_SPREAD_SAMPLES events.
// At 1, every channel crosses on the same sample.
constexpr double CROSSING_SPREAD_SAMPLES = 1;
constexpr double WAVE_FREQUENCY = 47;  // Hz
static_assert(CROSSING_SPREAD_SAMPLES >= 1 &&
                  CROSSING_SPREAD_SAMPLES < LatencyStream::rate / WAVE_FREQUENCY,
              "the spread must stay within one period of the wave");
constexpr double WAVE_PHASE_PER_SAMPLE =
    2.0 * std::numbers::pi * WAVE_FREQUENCY / LatencyStream::rate;  // radians

using LatencySignalGenerator =
    SignalGenerator<LatencyStream,
                    {
                        .waveform = Waveform::Square,
                        .frequency = WAVE_FREQUENCY,
                        .amplitude = 900,
                        .phase_step_per_channel = (CROSSING_SPREAD_SAMPLES - 1) *
                                                  WAVE_PHASE_PER_SAMPLE / LatencyStream::channels,
                    }>;

// Connections between cores
SampleRing<LatencyStream, 8> signal_generator_to_pass_through_filter_1_sample_ring{
    "signal_generator_to_pass_through_filter_1_sample_ring"};

void core1() {
    // Processors
    LatencySignalGenerator signal_generator;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        SampleBuffer<LatencyStream>& slot =
            signal_generator_to_pass_through_filter_1_sample_ring.next_slot();
        signal_generator.process({.output = slot});
        signal_generator_to_pass_through_filter_1_sample_ring.publish();
    }
}

void core2() {
    // Processors
    // Diagnostic: the generator runs in this core's loop instead of core 1's, so no ring hop is
    // measured.
    LatencySignalGenerator signal_generator;
    [[maybe_unused]] PassThroughFilter pass_through_filter_1;
    [[maybe_unused]] PassThroughFilter pass_through_filter_2;
    [[maybe_unused]] PassThroughFilter pass_through_filter_3;
    [[maybe_unused]] PassThroughFilter pass_through_filter_4;
    [[maybe_unused]] PassThroughFilter pass_through_filter_5;
    [[maybe_unused]] PassThroughFilter pass_through_filter_6;
    [[maybe_unused]] PassThroughFilter pass_through_filter_7;
    LevelCrossingDetector<LatencyStream,
                          {
                              .threshold = 100,
                              .direction = CrossingDirection::Upward,
                              .post_detection_block = 2,
                          }>
        level_crossing_detector;
    Trigger<LatencyStream, LevelCrossing> trigger;
    LatencyBenchmark<TriggerEvent, 1, {.file_prefix = "fir_chain_latency_benchmark"}>
        latency_benchmark;

    // Connections
    SampleBuffer<LatencyStream> signal_generator_to_pass_through_filter_1_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_1_to_pass_through_filter_2_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_2_to_pass_through_filter_3_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_3_to_pass_through_filter_4_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_4_to_pass_through_filter_5_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_5_to_pass_through_filter_6_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_6_to_pass_through_filter_7_sample_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_7_to_level_crossing_detector_sample_buffer;
    EventBuffer<LevelCrossing, LatencyStream::channels>
        level_crossing_detector_to_trigger_event_buffer;
    EventBuffer<TriggerEvent, 1> trigger_to_latency_benchmark_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        // signal_generator_to_pass_through_filter_1_sample_ring.read(
        //     signal_generator_to_pass_through_filter_1_sample_buffer);
        signal_generator.process(
            {.output = signal_generator_to_pass_through_filter_1_sample_buffer});
        // Diagnostic: the 7 FirFilters are bypassed, so the ring's sample goes straight to the
        // detector.
        // pass_through_filter_1.process({
        //     .input = signal_generator_to_pass_through_filter_1_sample_buffer,
        //     .output = pass_through_filter_1_to_pass_through_filter_2_sample_buffer,
        // });
        // pass_through_filter_2.process({
        //     .input = pass_through_filter_1_to_pass_through_filter_2_sample_buffer,
        //     .output = pass_through_filter_2_to_pass_through_filter_3_sample_buffer,
        // });
        // pass_through_filter_3.process({
        //     .input = pass_through_filter_2_to_pass_through_filter_3_sample_buffer,
        //     .output = pass_through_filter_3_to_pass_through_filter_4_sample_buffer,
        // });
        // pass_through_filter_4.process({
        //     .input = pass_through_filter_3_to_pass_through_filter_4_sample_buffer,
        //     .output = pass_through_filter_4_to_pass_through_filter_5_sample_buffer,
        // });
        // pass_through_filter_5.process({
        //     .input = pass_through_filter_4_to_pass_through_filter_5_sample_buffer,
        //     .output = pass_through_filter_5_to_pass_through_filter_6_sample_buffer,
        // });
        // pass_through_filter_6.process({
        //     .input = pass_through_filter_5_to_pass_through_filter_6_sample_buffer,
        //     .output = pass_through_filter_6_to_pass_through_filter_7_sample_buffer,
        // });
        // pass_through_filter_7.process({
        //     .input = pass_through_filter_6_to_pass_through_filter_7_sample_buffer,
        //     .output = pass_through_filter_7_to_level_crossing_detector_sample_buffer,
        // });
        level_crossing_detector.process({
            // .input = pass_through_filter_7_to_level_crossing_detector_sample_buffer,
            .input = signal_generator_to_pass_through_filter_1_sample_buffer,
            .output = level_crossing_detector_to_trigger_event_buffer,
        });
        trigger.process({
            .input = level_crossing_detector_to_trigger_event_buffer,
            .output = trigger_to_latency_benchmark_event_buffer,
        });
        latency_benchmark.process({.input = trigger_to_latency_benchmark_event_buffer});
    }
}

int main() {
    return run_on_cores({
        // {.core_number = 1, .entry_point = core1},
        {.core_number = 2, .entry_point = core2},
    });
}
