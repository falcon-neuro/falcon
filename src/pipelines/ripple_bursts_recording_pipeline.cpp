// Ripple bursts recording pipeline: SignalGenerator -> ring -> Recorder. It records
// SignalGenerator's ripple bursts so they can be looked at: src/tests/plot_recording.py plots the
// file. Expect 2 bursts per second on each of the 4 channels, each 80 ms long, a 180 Hz
// oscillation that swells from 10 µV to 100 µV and back.
#include "../core/engine.cpp"
#include "../processors/recorder.cpp"
#include "../processors/signal_generator.cpp"

using RippleTestStream = StreamInfo<4, 30000.0, float>;

// Connections between cores
// Half a second of samples, so a write that blocks for a while loses none.
SampleRing<RippleTestStream, 16384> signal_generator_to_recorder_sample_ring{
    "signal_generator_to_recorder_sample_ring"};

void core4() {
    // Processors
    SignalGenerator<RippleTestStream,
                    {.waveform = Waveform::RippleBursts, .frequency = 2, .amplitude = 100}>
        signal_generator{"signal_generator"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_to_recorder_sample_ring});
    }
}

void core8() {
    // Processors
    Recorder<RecorderOptions<1>{.file_name = "ripple_bursts.bin", .slot_names = {"ripple_bursts"}},
             SampleBuffer<RippleTestStream>>
        recorder{"recorder"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (next_pass()) {
        recorder.process({.inputs = {signal_generator_to_recorder_sample_ring}});
    }
}

int main() {
    // The recorder writes to a file, so its core isn't a realtime one.
    return run_on_cores("ripple_bursts_recording",
                        {
                            {.core_number = 4, .entry_point = core4},
                            {.core_number = 8, .entry_point = core8, .is_realtime = false},
                        });
}
