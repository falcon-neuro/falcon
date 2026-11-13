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
SampleRing<SparseTestStream, 64> signal_generator_to_ap_filter_sample_ring{
    "signal_generator_to_ap_filter_sample_ring"};

void core4() {
    // Processors
    SignalGenerator<SparseTestStream,
                    {.waveform = Waveform::Square, .frequency = 1, .amplitude = 100}>
        signal_generator;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        SampleBuffer<SparseTestStream>& slot =
            signal_generator_to_ap_filter_sample_ring.next_slot();
        signal_generator.process({.output = slot});
        signal_generator_to_ap_filter_sample_ring.publish();
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
        ap_filter;
    SpikeDetector<SparseTestStream, {.threshold = -70}> spike_detector;
    EventLogger<Spikes<SparseTestStream>, 1> example_logger;

    // Connections
    SampleBuffer<SparseTestStream> signal_generator_to_ap_filter_sample_buffer;
    SampleBuffer<SparseTestStream> ap_filter_to_spike_detector_sample_buffer;
    EventBuffer<Spikes<SparseTestStream>, 1> spike_detector_to_example_logger_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        signal_generator_to_ap_filter_sample_ring.read(signal_generator_to_ap_filter_sample_buffer);
        ap_filter.process({
            .input = signal_generator_to_ap_filter_sample_buffer,
            .output = ap_filter_to_spike_detector_sample_buffer,
        });
        spike_detector.process({
            .input = ap_filter_to_spike_detector_sample_buffer,
            .output = spike_detector_to_example_logger_event_buffer,
        });
        example_logger.process({.input = spike_detector_to_example_logger_event_buffer});
    }
}

int main() {
    return run_on_cores({
        {.core_number = 4, .entry_point = core4},
        {.core_number = 5, .entry_point = core5},
    });
}
