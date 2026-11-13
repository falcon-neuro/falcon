// FIR chain latency benchmark: a square wave on every channel passes unchanged through 7
// FirFilters, and once every channel has crossed the level, Trigger fires. LatencyBenchmark logs
// each trigger's time from its sample's arrival to the decision. Expect one trigger per rising
// edge, 47 per second.
//
//   SignalGenerator -> ring -> 7 pass-through FirFilters in series -> LevelCrossingDetector
//   -> Trigger -> LatencyBenchmark
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

// Connections between cores
SampleRing<LatencyStream, 512> signal_generator_to_pass_through_filter_1_sample_ring{
    "signal_generator_to_pass_through_filter_1_sample_ring"};

void core1() {
    // Processors
    // A phase step of 0 puts the same wave on every channel, so all channels cross on one sample.
    SignalGenerator<LatencyStream,
                    {
                        .waveform = Waveform::Square,
                        .frequency = 47,
                        .amplitude = 900,
                        .phase_step_per_channel = 0,
                    }>
        signal_generator;

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
    PassThroughFilter pass_through_filter_1;
    PassThroughFilter pass_through_filter_2;
    PassThroughFilter pass_through_filter_3;
    PassThroughFilter pass_through_filter_4;
    PassThroughFilter pass_through_filter_5;
    PassThroughFilter pass_through_filter_6;
    PassThroughFilter pass_through_filter_7;
    LevelCrossingDetector<LatencyStream,
                          {
                              .threshold = 100,
                              .direction = CrossingDirection::Upward,
                              .post_detection_block = 2,
                          }>
        level_crossing_detector;
    Trigger<LatencyStream, LevelCrossings<LatencyStream>> trigger;
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
    EventBuffer<LevelCrossings<LatencyStream>, 1> level_crossing_detector_to_trigger_event_buffer;
    EventBuffer<TriggerEvent, 1> trigger_to_latency_benchmark_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        signal_generator_to_pass_through_filter_1_sample_ring.read(
            signal_generator_to_pass_through_filter_1_sample_buffer);
        pass_through_filter_1.process({
            .input = signal_generator_to_pass_through_filter_1_sample_buffer,
            .output = pass_through_filter_1_to_pass_through_filter_2_sample_buffer,
        });
        pass_through_filter_2.process({
            .input = pass_through_filter_1_to_pass_through_filter_2_sample_buffer,
            .output = pass_through_filter_2_to_pass_through_filter_3_sample_buffer,
        });
        pass_through_filter_3.process({
            .input = pass_through_filter_2_to_pass_through_filter_3_sample_buffer,
            .output = pass_through_filter_3_to_pass_through_filter_4_sample_buffer,
        });
        pass_through_filter_4.process({
            .input = pass_through_filter_3_to_pass_through_filter_4_sample_buffer,
            .output = pass_through_filter_4_to_pass_through_filter_5_sample_buffer,
        });
        pass_through_filter_5.process({
            .input = pass_through_filter_4_to_pass_through_filter_5_sample_buffer,
            .output = pass_through_filter_5_to_pass_through_filter_6_sample_buffer,
        });
        pass_through_filter_6.process({
            .input = pass_through_filter_5_to_pass_through_filter_6_sample_buffer,
            .output = pass_through_filter_6_to_pass_through_filter_7_sample_buffer,
        });
        pass_through_filter_7.process({
            .input = pass_through_filter_6_to_pass_through_filter_7_sample_buffer,
            .output = pass_through_filter_7_to_level_crossing_detector_sample_buffer,
        });
        level_crossing_detector.process({
            .input = pass_through_filter_7_to_level_crossing_detector_sample_buffer,
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
        {.core_number = 1, .entry_point = core1},
        {.core_number = 2, .entry_point = core2},
    });
}
