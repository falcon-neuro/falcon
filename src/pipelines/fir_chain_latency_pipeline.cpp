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
SampleRing<LatencyStream, 512> signal_generator_output_ring_to_core2{
    "signal_generator_output_ring_to_core2"};

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
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_ring_to_core2});
    }
}

void core2() {
    // Processors
    PassThroughFilter pass_through_filter_1{"pass_through_filter_1"};
    PassThroughFilter pass_through_filter_2{"pass_through_filter_2"};
    PassThroughFilter pass_through_filter_3{"pass_through_filter_3"};
    PassThroughFilter pass_through_filter_4{"pass_through_filter_4"};
    PassThroughFilter pass_through_filter_5{"pass_through_filter_5"};
    PassThroughFilter pass_through_filter_6{"pass_through_filter_6"};
    PassThroughFilter pass_through_filter_7{"pass_through_filter_7"};
    LevelCrossingDetector<LatencyStream,
                          {
                              .threshold = 100,
                              .direction = CrossingDirection::Upward,
                              .post_detection_block = 2,
                          }>
        level_crossing_detector{"level_crossing_detector"};
    Trigger<LatencyStream, LevelCrossings<LatencyStream>> trigger{"trigger"};
    LatencyBenchmark<TriggerEvent, 1, {.file_name = "fir_chain_latency_benchmark.csv"}>
        latency_benchmark{"latency_benchmark"};

    // Connections
    SampleBuffer<LatencyStream> pass_through_filter_1_output_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_2_output_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_3_output_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_4_output_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_5_output_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_6_output_buffer;
    SampleBuffer<LatencyStream> pass_through_filter_7_output_buffer;
    EventBuffer<LevelCrossings<LatencyStream>, 1> level_crossing_detector_output_buffer;
    EventBuffer<TriggerEvent, 1> trigger_output_buffer;

    while (next_pass()) {
        pass_through_filter_1.process({
            .input = signal_generator_output_ring_to_core2,
            .output = pass_through_filter_1_output_buffer,
        });
        pass_through_filter_2.process({
            .input = pass_through_filter_1_output_buffer,
            .output = pass_through_filter_2_output_buffer,
        });
        pass_through_filter_3.process({
            .input = pass_through_filter_2_output_buffer,
            .output = pass_through_filter_3_output_buffer,
        });
        pass_through_filter_4.process({
            .input = pass_through_filter_3_output_buffer,
            .output = pass_through_filter_4_output_buffer,
        });
        pass_through_filter_5.process({
            .input = pass_through_filter_4_output_buffer,
            .output = pass_through_filter_5_output_buffer,
        });
        pass_through_filter_6.process({
            .input = pass_through_filter_5_output_buffer,
            .output = pass_through_filter_6_output_buffer,
        });
        pass_through_filter_7.process({
            .input = pass_through_filter_6_output_buffer,
            .output = pass_through_filter_7_output_buffer,
        });
        level_crossing_detector.process({
            .input = pass_through_filter_7_output_buffer,
            .output = level_crossing_detector_output_buffer,
        });
        trigger.process({
            .input = level_crossing_detector_output_buffer,
            .output = trigger_output_buffer,
        });
        latency_benchmark.process({.input = trigger_output_buffer});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 1, .entry_point = core1},
                            {.core_number = 2, .entry_point = core2},
                        });
}
