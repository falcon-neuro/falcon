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
SampleRing<ComparisonTestStream, 4>
    signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_ring{
        "signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_ring"};

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
        signal_generator;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        SampleBuffer<ComparisonTestStream>& slot =
            signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_ring.next_slot();
        signal_generator.process({.output = slot});
        signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_ring.publish();
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
        ap_filter;
    SpikeDetector<ComparisonTestStream, {.threshold = -70}> filtered_spike_detector;
    EventLogger<Spikes<ComparisonTestStream>, 1,
                {.label = "filtered", .style = LoggingStyle::CountPerSecond}>
        filtered_logger;
    SpikeDetector<ComparisonTestStream, {.threshold = -70}> unfiltered_spike_detector;
    EventLogger<Spikes<ComparisonTestStream>, 1,
                {.label = "unfiltered", .style = LoggingStyle::CountPerSecond}>
        unfiltered_logger;

    // Connections
    SampleBuffer<ComparisonTestStream>
        signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_buffer;
    SampleBuffer<ComparisonTestStream> ap_filter_to_filtered_spike_detector_sample_buffer;
    EventBuffer<Spikes<ComparisonTestStream>, 1>
        filtered_spike_detector_to_filtered_logger_event_buffer;
    EventBuffer<Spikes<ComparisonTestStream>, 1>
        unfiltered_spike_detector_to_unfiltered_logger_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_ring.read(
            signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_buffer);
        ap_filter.process({
            .input = signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_buffer,
            .output = ap_filter_to_filtered_spike_detector_sample_buffer,
        });
        filtered_spike_detector.process({
            .input = ap_filter_to_filtered_spike_detector_sample_buffer,
            .output = filtered_spike_detector_to_filtered_logger_event_buffer,
        });
        filtered_logger.process({.input = filtered_spike_detector_to_filtered_logger_event_buffer});
        unfiltered_spike_detector.process({
            .input = signal_generator_to_ap_filter_and_unfiltered_spike_detector_sample_buffer,
            .output = unfiltered_spike_detector_to_unfiltered_logger_event_buffer,
        });
        unfiltered_logger.process(
            {.input = unfiltered_spike_detector_to_unfiltered_logger_event_buffer});
    }
}

int main() {
    return run_on_cores({
        {.core_number = 2, .entry_point = core4},
        {.core_number = 5, .entry_point = core5},
    });
}
