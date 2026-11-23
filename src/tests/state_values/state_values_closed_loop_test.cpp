// State value test: two state values that cross cores, one in each direction, in a closed loop.
// It runs until Ctrl+C and then exits with 0.
// - signal_generator makes 2 ripple bursts a second, 500 ms apart.
// - gated_detector and free_detector, on another core, read the same samples. Neither detects
//   anything during the first 10 s, their burn-in.
// - detection_pauser, on a third core, reads gated_detector's is_in_lockout and switches its
//   is_detection_enabled off for 750 ms from the start of each lockout. free_detector has no state
//   value connected, so it shows what gated_detector would do without the loop.
// Expected:
// - free_logger shows at least one event in every burst, so never more than 15,000 samples from
//   one event to the next.
// - gated_logger shows one event a second, about 30,000 samples after the one before, since each
//   detection hides the rest of its burst and the whole of the next one.
// - After Ctrl+C, detection_pauser logs as many lockouts begun as gated_logger shows events, and
//   as many ended, or one fewer if the run stopped during a lockout. A lockout lasts 901 samples,
//   30.03 ms; how far the shortest and the longest are from that shows how much sooner or later
//   a change of the state arrives from one time to the next.
// What a failure looks like:
// - If is_in_lockout doesn't cross, detection_pauser saw no lockout, and gated_logger shows an
//   event in every burst, as free_logger does.
// - If is_detection_enabled doesn't cross, detection_pauser saw the lockouts, but gated_logger
//   still shows an event in every burst.
#include "../../core/engine.cpp"
#include "../../processors/event_logger.cpp"
#include "../../processors/ripple_detector.cpp"
#include "../../processors/signal_generator.cpp"
#include "state_values_test_processors.cpp"

using RippleTestStream = StreamInfo<4, 30000.0, float>;

// Connections between cores
SampleRing<RippleTestStream, 64> signal_generator_output_ring_to_core5{
    "signal_generator_output_ring_to_core5"};
CrossCoreStateValue<bool, false> gated_detector_is_in_lockout_cross_core_state;
CrossCoreStateValue<bool, true> detection_pauser_is_detection_enabled_cross_core_state;

void core4() {
    // Processors
    SignalGenerator<RippleTestStream,
                    {.waveform = Waveform::RippleBursts, .frequency = 2, .amplitude = 100}>
        signal_generator{"signal_generator"};

    while (next_pass()) {
        signal_generator.process({.output = signal_generator_output_ring_to_core5});
    }
}

void core5() {
    // Processors
    RippleDetector<RippleTestStream, RippleDetectorOptions{}> gated_detector{"gated_detector"};
    EventLogger<Slots<Slot<"gated_detector_output", EventBuffer<Ripple, 1>>>> gated_logger{
        "gated_logger"};
    RippleDetector<RippleTestStream, RippleDetectorOptions{}> free_detector{"free_detector"};
    EventLogger<Slots<Slot<"free_detector_output", EventBuffer<Ripple, 1>>>> free_logger{
        "free_logger"};

    // Connections
    EventBuffer<Ripple, 1> gated_detector_output_buffer;
    EventBuffer<Ripple, 1> free_detector_output_buffer;

    while (next_pass()) {
        gated_detector.process({
            .input = signal_generator_output_ring_to_core5,
            .is_detection_enabled = detection_pauser_is_detection_enabled_cross_core_state,
            .output = gated_detector_output_buffer,
            .is_in_lockout = gated_detector_is_in_lockout_cross_core_state,
        });
        gated_logger.process({.inputs = {gated_detector_output_buffer}});
        free_detector.process({
            .input = signal_generator_output_ring_to_core5,
            .output = free_detector_output_buffer,
        });
        free_logger.process({.inputs = {free_detector_output_buffer}});
    }
}

void core6() {
    // Processors
    DetectionPauser<> detection_pauser{"detection_pauser"};

    while (next_pass()) {
        detection_pauser.process({
            .is_in_lockout = gated_detector_is_in_lockout_cross_core_state,
            .is_detection_enabled = detection_pauser_is_detection_enabled_cross_core_state,
        });
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 4, .entry_point = core4},
                            {.core_number = 5, .entry_point = core5},
                            {.core_number = 6, .entry_point = core6},
                        });
}
