// Neuralynx loopback pipeline: SignalGenerator -> NeuralynxStreamer sends a test signal to this
// machine itself as a Neuralynx acquisition system would, and NeuralynxInput reads it back, so the
// two can be tried without the hardware. The generated signal and the input's samples are both
// recorded. Expect the input's samples to be the generator's channels 16 to 47 under the same
// sample numbers, each value within half an A/D step (0.15 µV) of the generated one.
//
//   SignalGenerator -> NeuralynxStreamer -> UDP to 127.0.0.1:5000 -> NeuralynxInput
//   SignalGenerator -> ring -> Recorder
//   NeuralynxInput  -> ring -> Recorder
#include "../core/engine.cpp"
#include "../processors/neuralynx_input.cpp"
#include "../processors/neuralynx_streamer.cpp"
#include "../processors/recorder.cpp"
#include "../processors/signal_generator.cpp"

// What the stand-in for the acquisition system sends, and the run of its channels the input takes.
using DeviceStream = StreamInfo<64, 32768.0, float>;
using InputStream = StreamInfo<32, 32768.0, float>;

// Connections between cores
// Half a second of samples each, so a write to the file that blocks for a while loses none.
SampleRing<DeviceStream, 16384> signal_generator_output_ring_to_core8{
    "signal_generator_output_ring_to_core8"};
SampleRing<InputStream, 16384> neuralynx_input_output_ring_to_core8{
    "neuralynx_input_output_ring_to_core8"};

void core4() {
    // Processors
    SignalGenerator<DeviceStream, {.waveform = Waveform::Sine, .frequency = 10, .amplitude = 100}>
        signal_generator{"signal_generator"};
    NeuralynxStreamer<DeviceStream, {.address = "127.0.0.1", .port = 5000}> neuralynx_streamer{
        "neuralynx_streamer"};

    // Connections
    // Read by the streamer on this core and by the recorder on another.
    SampleBuffer<DeviceStream> signal_generator_output_buffer{
        signal_generator_output_ring_to_core8};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_buffer});
        neuralynx_streamer.process({.input = signal_generator_output_buffer});
    }
}

void core5() {
    // Processors
    NeuralynxInput<InputStream,
                   {
                       .address = "127.0.0.1",
                       .port = 5000,
                       .first_channel = 16,
                       .last_channel = 47,
                       .sampling_rate = 32768,
                   }>
        neuralynx_input{"neuralynx_input"};

    while (next_pass()) {
        neuralynx_input.process({.output = neuralynx_input_output_ring_to_core8});
    }
}

void core8() {
    // Processors
    Recorder<Slots<Slot<"signal_generator_output", SampleBuffer<DeviceStream>>,
                   Slot<"neuralynx_input_output", SampleBuffer<InputStream>>>,
             {.file_name = "neuralynx_loopback.bin"}>
        recorder{"recorder"};

    while (next_pass()) {
        recorder.process({
            .inputs =
                {
                    signal_generator_output_ring_to_core8,
                    neuralynx_input_output_ring_to_core8,
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
