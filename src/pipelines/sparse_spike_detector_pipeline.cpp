// Sparse spike detector pipeline: SignalGenerator -> ring -> SpikeDetector -> EventLogger, with no
// filter. Expect 4 spikes per second, about 0.16 s apart: a 1 Hz square wave on 4 channels crosses
// -70 µV once per period on each, and the generator shifts each channel's wave by one radian.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
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
    SpikeDetector<SparseTestStream, {.threshold = -70}> spike_detector{"spike_detector"};
    EventLogger<Slots<Slot<"spike_detector_output", EventBuffer<Spikes<SparseTestStream>, 1>>>>
        example_logger{"example_logger"};

    // Connections
    EventBuffer<Spikes<SparseTestStream>, 1> spike_detector_output_buffer;

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
                            {.core_number = 4, .entry_point = core4},
                            {.core_number = 5, .entry_point = core5},
                        });
}
