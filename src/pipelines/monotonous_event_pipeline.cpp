// Monotonous event pipeline: EventGenerator emits a ScheduledEvent 4 times a second, EventLogger
// logs each one.
#include "../core/engine.cpp"
#include "../processors/event_generator.cpp"
#include "../processors/event_logger.cpp"

void core1() {
    // Processors
    EventGenerator<{.rate = 4}> dummy_generator{"dummy_generator"};
    EventLogger<Slots<Slot<"dummy_generator_output", EventBuffer<ScheduledEvent, 1>>>>
        example_logger{"example_logger"};

    // Connections
    EventBuffer<ScheduledEvent, 1> dummy_generator_output_buffer;

    while (next_pass()) {
        dummy_generator.process({.output = dummy_generator_output_buffer});
        example_logger.process({.inputs = {dummy_generator_output_buffer}});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments, {{.core_number = 1, .entry_point = core1}});
}
