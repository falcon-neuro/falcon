// Logging test: a processor fails in its constructor. Expected: built_before and
// other_core_processor log "built"; failing_processor ends the run with an error line before any
// loop starts; never_built is never built; the two built processors log "destroyed", and
// failing_processor doesn't, since its constructor never finished; the program exits with 1.
#include "../../core/engine.cpp"
#include "logging_test_processors.cpp"

void core1() {
    // Processors
    LifetimeReporter<> built_before{"built_before"};
    LifetimeReporter<{.failure = TestFailure::InConstructor}> failing_processor{
        "failing_processor"};
    LifetimeReporter<> never_built{"never_built"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        built_before.process({});
        failing_processor.process({});
        never_built.process({});
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
    return run_on_cores("logging_fail_in_constructor_test", {
        {.core_number = 1, .entry_point = core1},
        {.core_number = 2, .entry_point = core2},
    });
}
