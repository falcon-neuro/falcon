// Monotonous event pipeline: EventGenerator emits a ScheduledEvent 4 times a second, EventLogger
// prints each one to the console. Its core prints, so it is a non-realtime core (see Logging in
// src/core/spec.md).
#include "../core/engine.cpp"
#include "../processors/event_generator.cpp"
#include "../processors/event_logger.cpp"

void core1() {
    // Processors
    EventGenerator<{.rate = 4}> dummy_generator;
    EventLogger<ScheduledEvent, 1> example_logger;

    // Connections
    EventBuffer<ScheduledEvent, 1> dummy_generator_to_example_logger_event_buffer;

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_cores_running) {
        dummy_generator.process({.output = dummy_generator_to_example_logger_event_buffer});
        example_logger.process({.input = dummy_generator_to_example_logger_event_buffer});
    }
}

int main() {
    return run_on_cores({{.core_number = 1, .entry_point = core1}});
}
