// Spike detector pipeline: SignalGenerator -> ring -> SpikeDetector -> EventLogger, with no filter.
// Expect 5 spikes per second on each of the 32 channels: the square wave's falling edge crosses
// -50 µV once per period.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using TestStream = StreamInfo<32, 30000.0, float>;

// Connections between cores
SampleRing<TestStream, 64> signal_generator_output_ring_to_core5{
    "signal_generator_output_ring_to_core5"};

void core1() {
    // Processors
    SignalGenerator<TestStream, {.waveform = Waveform::Square, .frequency = 5, .amplitude = 100}>
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_ring_to_core5});
    }
}

void core2() {
    // Processors
    SpikeDetector<TestStream, {.threshold = -70}> spike_detector{"spike_detector"};
    EventLogger<Slots<Slot<"spike_detector_output", EventBuffer<Spikes<TestStream>, 1>>>>
        example_logger{"example_logger"};

    // Connections
    EventBuffer<Spikes<TestStream>, 1> spike_detector_output_buffer;

    while (next_pass()) {
        spike_detector.process({
            .input = signal_generator_output_ring_to_core5,
            .output = spike_detector_output_buffer,
        });
        example_logger.process({.inputs = {spike_detector_output_buffer}});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 4, .entry_point = core1},
                            {.core_number = 5, .entry_point = core2},
                        });
}
