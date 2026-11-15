// Logging test: a processor fails in process(). Expected: the three processors each log "built";
// a second later failing_processor ends the run with an error line; all three log "destroyed", on
// both cores; the program exits with 1.
#include "../../core/engine.cpp"
#include "logging_test_processors.cpp"

void core1() {
    // Processors
    LifetimeReporter<> same_core_neighbour{"same_core_neighbour"};
    LifetimeReporter<{.failure = TestFailure::InProcess}> failing_processor{"failing_processor"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        same_core_neighbour.process({});
        failing_processor.process({});
    }
}

void core2() {
    // Processors
    LifetimeReporter<> other_core_processor{"other_core_processor"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        other_core_processor.process({});
    }
}

int main() {
    return run_on_cores("logging_fail_in_process_test", {
        {.core_number = 1, .entry_point = core1},
        {.core_number = 2, .entry_point = core2},
    });
}
