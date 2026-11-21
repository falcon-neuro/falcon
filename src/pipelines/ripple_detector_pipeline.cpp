// Ripple detector pipeline: SignalGenerator -> ring -> IirFilter -> RippleDetector -> EventLogger.
// The generator's ripple bursts, 2 per second, pass a ripple band-pass filter, and the logger
// shows each Ripple event with its sample number. Nothing is detected during the first 10 s, the
// detector's burn-in. The filtered signal and the Ripple events are recorded, so
// src/tests/plot_recording.py can show where in a burst each detection falls.
#include "../core/engine.cpp"
#include "../processors/event_logger.cpp"
#include "../processors/iir_filter.cpp"
#include "../processors/recorder.cpp"
#include "../processors/ripple_detector.cpp"
#include "../processors/signal_generator.cpp"

using RippleTestStream = StreamInfo<4, 30000.0, float>;

// Connections between cores
SampleRing<RippleTestStream, 64> signal_generator_to_ripple_filter_sample_ring{
    "signal_generator_to_ripple_filter_sample_ring"};
// Half a second of samples, so a write that blocks for a while loses none.
SampleRing<RippleTestStream, 16384> ripple_filter_to_recorder_sample_ring{
    "ripple_filter_to_recorder_sample_ring"};
EventRing<Ripple, 1, 64> ripple_detector_to_recorder_event_ring{
    "ripple_detector_to_recorder_event_ring"};

void core4() {
    // Processors
    SignalGenerator<RippleTestStream,
                    {.waveform = Waveform::RippleBursts, .frequency = 2, .amplitude = 100}>
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_to_ripple_filter_sample_ring});
    }
}

void core5() {
    // Processors
    // The IirFilter test's Chebyshev II band-pass: 150 to 250 Hz at 40 dB, designed for 30 kHz.
    IirFilter<RippleTestStream,
              IirFilterOptions<4>{
                  .design_rate = 30000.0,
                  .gain = 1.0,
                  .sections =
                      {
                          {
                              {
                                  .b0 = 0.009868124820699481,
                                  .b1 = -0.019678861529825818,
                                  .b2 = 0.009868124820699481,
                                  .a1 = -1.9869804675921499,
                                  .a2 = 0.9888344013978323,
                              },
                              {
                                  .b0 = 1.0,
                                  .b1 = -1.9995351666666534,
                                  .b2 = 0.9999999999999999,
                                  .a1 = -1.9886985816490874,
                                  .a2 = 0.9901424760854102,
                              },
                              {
                                  .b0 = 1.0,
                                  .b1 = -1.9971441554588587,
                                  .b2 = 1.0,
                                  .a1 = -1.9938876092589772,
                                  .a2 = 0.9959856961507008,
                              },
                              {
                                  .b0 = 1.0,
                                  .b1 = -1.9990528487991495,
                                  .b2 = 1.0000000000000002,
                                  .a1 = -1.9955729463532639,
                                  .a2 = 0.9968577002328733,
                              },
                          },
                      },
              }>
        ripple_filter{"ripple_filter"};
    RippleDetector<RippleTestStream, RippleDetectorOptions{}> ripple_detector{"ripple_detector"};
    EventLogger<Ripple, 1> ripple_logger{"ripple_logger"};

    // Connections
    // Each of these two is read on this core and also goes to the recorder's core.
    SampleBuffer<RippleTestStream> ripple_filter_to_ripple_detector_sample_buffer{
        ripple_filter_to_recorder_sample_ring};
    EventBuffer<Ripple, 1> ripple_detector_to_ripple_logger_event_buffer{
        ripple_detector_to_recorder_event_ring};

    while (next_pass()) {
        ripple_filter.process({
            .input = signal_generator_to_ripple_filter_sample_ring,
            .output = ripple_filter_to_ripple_detector_sample_buffer,
        });
        ripple_detector.process({
            .input = ripple_filter_to_ripple_detector_sample_buffer,
            .output = ripple_detector_to_ripple_logger_event_buffer,
        });
        ripple_logger.process({.input = ripple_detector_to_ripple_logger_event_buffer});
    }
}

void core8() {
    // Processors
    Recorder<RecorderOptions<2>{
                 .file_name = "ripple_detector.bin",
                 .slot_names = {"filtered_signal", "ripples"},
             },
             SampleBuffer<RippleTestStream>, EventBuffer<Ripple, 1>>
        recorder{"recorder"};

    while (next_pass()) {
        recorder.process({
            .inputs =
                {
                    ripple_filter_to_recorder_sample_ring,
                    ripple_detector_to_recorder_event_ring,
                },
        });
    }
}

int main() {
    // The recorder writes to a file, so its core isn't a realtime one.
    return run_on_cores("ripple_detector",
                        {
                            {.core_number = 4, .entry_point = core4},
                            {.core_number = 5, .entry_point = core5},
                            {.core_number = 8, .entry_point = core8, .is_realtime = false},
                        });
}
