// Monotonous event pipeline: EventGenerator emits a ScheduledEvent every second, EventLogger prints
// each one to the console. Its core prints, so it is a non-realtime core (see Logging in
// src/core/spec.md).
#include "../core/engine.cpp"
#include "../processors/event_generator.cpp"
#include "../processors/event_logger.cpp"

void core1() {
    EventGenerator<{.rate = 100}> generator;
    EventLogger<ScheduledEvent, 1> logger;
    Events<ScheduledEvent, 1> events;

    if (!all_built())
        return;  // start barrier: wait for every core, return if a constructor failed on any of
                 // them

    while (running) {
        generator.process(
            events);  // Out clears events, then the generator pushes one when it's due
        logger.process(events);
    }
}

int main() {
    return run({{.cpu = 1, .body = core1}});
}
