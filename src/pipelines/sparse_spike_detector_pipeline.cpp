// Sparse spike detector pipeline: SignalGenerator -> ring -> SpikeDetector -> EventLogger, with no
// filter. Expect 4 spikes per second, about 0.16 s apart: a 1 Hz square wave on 4 channels crosses
// -70 µV once per period on each, and the generator shifts each channel's wave by one radian.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using SparseTestStream = StreamInfo<4, 30000.0, float>;

// Connections between cores
SampleRing<SparseTestStream, 64> signal_generator_to_spike_detector_sample_ring{
    "signal_generator_to_spike_detector_sample_ring"};

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
            signal_generator_to_spike_detector_sample_ring.next_slot();
        signal_generator.process({.output = slot});
        signal_generator_to_spike_detector_sample_ring.publish();
    }
}

void core5() {
    // Processors
    SpikeDetector<SparseTestStream, {.threshold = -70}> spike_detector;
    EventLogger<Spikes<SparseTestStream>, 1> example_logger;

    // Connections
    SampleBuffer<SparseTestStream> signal_generator_to_spike_detector_sample_buffer;
    EventBuffer<Spikes<SparseTestStream>, 1> spike_detector_to_example_logger_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        signal_generator_to_spike_detector_sample_ring.read(
            signal_generator_to_spike_detector_sample_buffer);
        spike_detector.process({
            .input = signal_generator_to_spike_detector_sample_buffer,
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
