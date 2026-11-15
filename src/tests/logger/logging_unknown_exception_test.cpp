// Logging test: a processor throws an int in process(). Expected: the three processors each log
// "built"; a second later throwing_processor ends the run with an error line that names the
// exception's type, since an int has no message; all three log "destroyed", on both cores; the
// program exits with 1.
#include "../../core/engine.cpp"
#include "logging_test_processors.cpp"

void core1() {
    // Processors
    LifetimeReporter<> same_core_neighbour{"same_core_neighbour"};
    LifetimeReporter<{.failure = TestFailure::NonStdExceptionInProcess}> throwing_processor{
        "throwing_processor"};

    if (!wait_at_start_barrier()) {
        return;
    }

    while (is_pipeline_running) {
        same_core_neighbour.process({});
        throwing_processor.process({});
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
    return run_on_cores("logging_unknown_exception_test", {
        {.core_number = 1, .entry_point = core1},
        {.core_number = 2, .entry_point = core2},
    });
}
