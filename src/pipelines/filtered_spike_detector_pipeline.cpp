// Filtered spike detector pipeline: SignalGenerator -> ring -> IirFilter -> SpikeDetector ->
// EventLogger. The filter is a 4th-order Bessel band-pass, 300-6000 Hz at 30 kHz. A 1 Hz square
// wave has no energy in that band except at its edges: each falling edge comes through as a short
// negative transient that crosses -70 µV. Expect 4 spikes per second, about 0.16 s apart, as in the
// sparse spike detector pipeline, which has no filter.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/iir_filter.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using SparseTestStream = StreamInfo<4, 30000.0, float>;

// Connections between cores
SampleRing<SparseTestStream, 64> signal_generator_output_ring_to_core5{
    "signal_generator_output_ring_to_core5"};

void core4() {
    // Processors
    SignalGenerator<SparseTestStream,
                    {.waveform = Waveform::Square, .frequency = 1, .amplitude = 100}>
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_ring_to_core5});
    }
}

void core5() {
    // Processors
    // From Falcon 1's bessel_spike_30000.filter
    IirFilter<SparseTestStream,
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
    SpikeDetector<SparseTestStream, {.threshold = -70}> spike_detector{"spike_detector"};
    EventLogger<Slots<Slot<"spike_detector_output", EventBuffer<Spikes<SparseTestStream>, 1>>>>
        example_logger{"example_logger"};

    // Connections
    SampleBuffer<SparseTestStream> ap_filter_output_buffer;
    EventBuffer<Spikes<SparseTestStream>, 1> spike_detector_output_buffer;

    while (next_pass()) {
        ap_filter.process({
            .input = signal_generator_output_ring_to_core5,
            .output = ap_filter_output_buffer,
        });
        spike_detector.process({
            .input = ap_filter_output_buffer,
            .output = spike_detector_output_buffer,
        });
        example_logger.process({.inputs = {spike_detector_output_buffer}});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 4, .entry_point = core4},
                            {.core_number = 5, .entry_point = core5},
                        });
}
