// Logging test: two processors are given the same name. Expected: first_processor and
// other_core_processor log "built"; building second_processor ends the run with an error line
// that gives the shared name, before any loop starts; the two built processors log "destroyed";
// the program exits with 1.
#include "../../core/engine.cpp"
#include "logging_test_processors.cpp"

void core1() {
    // Processors
    LifetimeReporter<> first_processor{"same_name"};
    LifetimeReporter<> second_processor{"same_name"};

    while (next_pass()) {
        first_processor.process({});
        second_processor.process({});
    }
}

void core2() {
    // Processors
    LifetimeReporter<> other_core_processor{"other_core_processor"};

    while (next_pass()) {
        other_core_processor.process({});
    }
}

int main() {
    return run_on_cores("logging_duplicate_name_test", {
                                                           {.core_number = 1, .entry_point = core1},
                                                           {.core_number = 2, .entry_point = core2},
                                                       });
}
