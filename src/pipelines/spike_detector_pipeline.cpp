// Spike detector pipeline: SignalGenerator -> ring -> SpikeDetector -> EventLogger, with no filter.
// Expect 5 spikes per second on each of the 32 channels: the square wave's falling edge crosses
// -50 µV once per period.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using TestStream = StreamInfo<32, 30000.0, float>;

// Connections between cores
SampleRing<TestStream, 64> signal_generator_to_spike_detector_sample_ring{
    "signal_generator_to_spike_detector_sample_ring"};

void core1() {
    // Processors
    SignalGenerator<TestStream, {.waveform = Waveform::Square, .frequency = 5, .amplitude = 100}>
        signal_generator;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        SampleBuffer<TestStream>& slot = signal_generator_to_spike_detector_sample_ring.next_slot();
        signal_generator.process({.output = slot});
        signal_generator_to_spike_detector_sample_ring.publish();
    }
}

void core2() {
    // Processors
    SpikeDetector<TestStream, {.threshold = -70}> spike_detector;
    EventLogger<Spike, TestStream::channels> example_logger;

    // Connections
    SampleBuffer<TestStream> signal_generator_to_spike_detector_sample_buffer;
    EventBuffer<Spike, TestStream::channels> spike_detector_to_example_logger_event_buffer;

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
        {.core_number = 4, .entry_point = core1},
        {.core_number = 5, .entry_point = core2},
    });
}
