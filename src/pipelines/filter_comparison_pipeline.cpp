// Filter comparison pipeline: the same signal goes through two branches, one with IirFilter and one
// without, so the filter's effect shows in the console side by side.
//
//   SignalGenerator -> ring -+-> IirFilter -> SpikeDetector -> EventLogger "filtered"
//                            +--------------> SpikeDetector -> EventLogger "unfiltered"
//
// Each of the 2 channels carries 3 spikes per second (troughs of 200 µV) on top of an 8 Hz, 500 µV
// LFP wave, and each logger prints its count once a second. Expect "filtered" to count 6 per
// second, 3 per channel: the band-pass (300-6000 Hz) keeps the spikes and attenuates the LFP about
// 1000-fold. Expect "unfiltered" to count at least 16: every LFP trough crosses -70 µV on each
// channel, and a spike shows only where the LFP happens to be near zero.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/iir_filter.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using ComparisonTestStream = StreamInfo<2, 30000.0, float>;

// Connections between cores
SampleRing<ComparisonTestStream, 4> signal_generator_output_ring_to_core5{
    "signal_generator_output_ring_to_core5"};

void core4() {
    // Processors
    SignalGenerator<ComparisonTestStream,
                    {
                        .waveform = Waveform::SpikeTrain,
                        .frequency = 3,
                        .amplitude = 200,
                        .lfp_frequency = 8,
                        .lfp_amplitude = 500,
                    }>
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_ring_to_core5});
    }
}

void core5() {
    // Processors
    // From Falcon 1's bessel_spike_30000.filter
    IirFilter<ComparisonTestStream,
              IirFilterOptions<2>{
                  .design_rate = 30000.0,
                  .gain = 0.175014382674721,
                  .sections =
                      {
                          {
                              {
                                  .b0 = 1.0,
                                  .b1 = 2.0,
                                  .b2 = 1.0,
                                  .a1 = -0.4116456379943422,
                                  .a2 = 0.1208387236075018,
                              },
                              {
                                  .b0 = 1.0,
                                  .b1 = -2.0,
                                  .b2 = 1.0,
                                  .a1 = -1.888701220483925,
                                  .a2 = 0.8929613419279413,
                              },
                          },
                      },
              }>
        ap_filter{"ap_filter"};
    SpikeDetector<ComparisonTestStream, {.threshold = -70}> filtered_spike_detector{
        "filtered_spike_detector"};
    EventLogger<
        Slots<Slot<"filtered_spike_detector_output", EventBuffer<Spikes<ComparisonTestStream>, 1>>>,
        {.style = LoggingStyle::CountPerSecond}>
        filtered_logger{"filtered_logger"};
    SpikeDetector<ComparisonTestStream, {.threshold = -70}> unfiltered_spike_detector{
        "unfiltered_spike_detector"};
    EventLogger<Slots<Slot<"unfiltered_spike_detector_output",
                           EventBuffer<Spikes<ComparisonTestStream>, 1>>>,
                {.style = LoggingStyle::CountPerSecond}>
        unfiltered_logger{"unfiltered_logger"};

    // Connections
    SampleBuffer<ComparisonTestStream> ap_filter_output_buffer;
    EventBuffer<Spikes<ComparisonTestStream>, 1> filtered_spike_detector_output_buffer;
    EventBuffer<Spikes<ComparisonTestStream>, 1> unfiltered_spike_detector_output_buffer;

    while (next_pass()) {
        ap_filter.process({
            .input = signal_generator_output_ring_to_core5,
            .output = ap_filter_output_buffer,
        });
        filtered_spike_detector.process({
            .input = ap_filter_output_buffer,
            .output = filtered_spike_detector_output_buffer,
        });
        filtered_logger.process({.inputs = {filtered_spike_detector_output_buffer}});
        unfiltered_spike_detector.process({
            .input = signal_generator_output_ring_to_core5,
            .output = unfiltered_spike_detector_output_buffer,
        });
        unfiltered_logger.process({.inputs = {unfiltered_spike_detector_output_buffer}});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 2, .entry_point = core4},
                            {.core_number = 5, .entry_point = core5},
                        });
}
