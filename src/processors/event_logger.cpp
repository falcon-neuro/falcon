#pragma once
#include <concepts>
#include <print>
#include "../core/engine.cpp"

// EventLogger: prints every event it receives to the console, like Falcon 1's EventLogger.
// Printing is a system call, so it runs on a non-realtime core (see Logging in src/core/spec.md).
template <std::derived_from<EventRecord> Record, int Capacity>
struct EventLogger {
    struct Connections {
        EventInputBuffer<Record, Capacity> input;
    };

    u64 received = 0;  // events received since the start

    void process(Connections connections) {
        for (const Record& event : connections.input) {
            ++received;
            // Until C++26 reflection reads every field of any record (see Events in src/core/spec.md),
            // only a field named `count` is printed, if the record has one.
            if constexpr (requires { event.count; }) {
                std::println("EventLogger: event {}, count {}", received, event.count);
            } else {
                std::println("EventLogger: event {}", received);
            }
        }
    }
};
