// Spike recording pipeline: SignalGenerator -> SpikeDetector, with the signal and the spikes both
// recorded, so src/tests/plot_recording.py can show each spike on the channels it was detected on.
// Expect 5 spikes per second on each of the 70 channels: the square wave's falling edge crosses
// -70 µV once per period, each channel at its own time. 70 channels, so that a spike's channel
// mask takes two 64-bit words and the last of them is partly padding.
#include "../core/engine.cpp"
#include "../processors/recorder.cpp"
#include "../processors/signal_generator.cpp"
#include "../processors/spike_detector.cpp"

using TestStream = StreamInfo<70, 30000.0, float>;

// Connections between cores
SampleRing<TestStream, 64> signal_generator_output_ring_to_core5{
    "signal_generator_output_ring_to_core5"};
// Half a second of samples, so a write that blocks for a while loses none.
SampleRing<TestStream, 16384> signal_generator_output_ring_to_core8{
    "signal_generator_output_ring_to_core8"};
EventRing<Spikes<TestStream>, 1, 64> spike_detector_output_ring_to_core8{
    "spike_detector_output_ring_to_core8"};

void core4() {
    // Processors
    SignalGenerator<TestStream, {.waveform = Waveform::Square, .frequency = 5, .amplitude = 100}>
        signal_generator{"signal_generator"};

    // Connections
    // Read on two other cores, the spike detector's and the recorder's.
    SampleBuffer<TestStream> signal_generator_output_buffer{signal_generator_output_ring_to_core5,
                                                            signal_generator_output_ring_to_core8};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_buffer});
    }
}

void core5() {
    // Processors
    SpikeDetector<TestStream, {.threshold = -70}> spike_detector{"spike_detector"};

    while (next_pass()) {
        spike_detector.process({
            .input = signal_generator_output_ring_to_core5,
            .output = spike_detector_output_ring_to_core8,
        });
    }
}

void core8() {
    // Processors
    Recorder<Slots<Slot<"signal_generator_output", SampleBuffer<TestStream>>,
                   Slot<"spike_detector_output", EventBuffer<Spikes<TestStream>, 1>>>,
             {.file_name = "spike_recording.bin"}>
        recorder{"recorder"};

    while (next_pass()) {
        recorder.process({
            .inputs =
                {
                    signal_generator_output_ring_to_core8,
                    spike_detector_output_ring_to_core8,
                },
        });
    }
}

int main(int argument_count, char** arguments) {
    // The recorder writes to a file, so its core isn't a realtime one.
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 4, .entry_point = core4},
                            {.core_number = 5, .entry_point = core5},
                            {.core_number = 8, .entry_point = core8, .is_realtime = false},
                        });
}
