// Logging test: a processor fails in process(). Expected: the three processors each log "built";
// a second later failing_processor ends the run with an error line; all three log "destroyed", on
// both cores; the program exits with 1.
#include "../../core/engine.cpp"
#include "logging_test_processors.cpp"

void core1() {
    // Processors
    LifetimeReporter<> same_core_neighbour{"same_core_neighbour"};
    LifetimeReporter<{.failure = TestFailure::InProcess}> failing_processor{"failing_processor"};

    while (next_pass()) {
        same_core_neighbour.process({});
        failing_processor.process({});
    }
}

void core2() {
    // Processors
    LifetimeReporter<> other_core_processor{"other_core_processor"};

    while (next_pass()) {
        other_core_processor.process({});
    }
}

int main(int argument_count, char** arguments) {
    return run_on_cores(argument_count, arguments,
                        {
                            {.core_number = 1, .entry_point = core1},
                            {.core_number = 2, .entry_point = core2},
                        });
}
