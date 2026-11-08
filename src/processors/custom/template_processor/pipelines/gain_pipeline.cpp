// Gain pipeline: SignalGenerator -> ring -> Gain -> SpikeDetector -> EventLogger.
// The sine wave goes down to -50 µV. Gain multiplies it by 3.000001, so once per cycle it goes
// just below -150 µV, the spike threshold. With 4 channels at 3 Hz, that is 12 spikes per second.
// With a factor of exactly 3, the wave would reach -150 µV without going below it, and nothing
// would be detected.
#include "../../../../core/engine.cpp"
#include "../../../event_logger.cpp"
#include "../../../signal_generator.cpp"
#include "../../../spike_detector.cpp"
#include "../gain.cpp"

using TemplateStream = StreamInfo<4, 30000.0, float>;

// Connections between cores
SampleRing<TemplateStream, 1024> signal_generator_to_gain_sample_ring{
    "signal_generator_to_gain_sample_ring"};

void core4() {
    // Processors
    SignalGenerator<TemplateStream, {.waveform = Waveform::Sine, .frequency = 3, .amplitude = 50}>
        signal_generator;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        SampleBuffer<TemplateStream>& slot = signal_generator_to_gain_sample_ring.next_slot();
        signal_generator.process({.output = slot});
        signal_generator_to_gain_sample_ring.publish();
    }
}

void core5() {
    // Processors
    template_processor::Gain<TemplateStream, {.factor = 3.000001}> gain;
    SpikeDetector<TemplateStream, {.threshold = -150}> spike_detector;
    EventLogger<Spike, TemplateStream::channels,
                {.label = "gain", .style = LoggingStyle::CountPerSecond}>
        event_logger;

    // Connections
    SampleBuffer<TemplateStream> signal_generator_to_gain_sample_buffer;
    SampleBuffer<TemplateStream> gain_to_spike_detector_sample_buffer;
    EventBuffer<Spike, TemplateStream::channels> spike_detector_to_event_logger_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        signal_generator_to_gain_sample_ring.read(signal_generator_to_gain_sample_buffer);
        gain.process({
            .input = signal_generator_to_gain_sample_buffer,
            .output = gain_to_spike_detector_sample_buffer,
        });
        spike_detector.process({
            .input = gain_to_spike_detector_sample_buffer,
            .output = spike_detector_to_event_logger_event_buffer,
        });
        event_logger.process({.input = spike_detector_to_event_logger_event_buffer});
    }
}

int main() {
    return run_on_cores({
        {.core_number = 4, .entry_point = core4},
        {.core_number = 5, .entry_point = core5},
    });
}
