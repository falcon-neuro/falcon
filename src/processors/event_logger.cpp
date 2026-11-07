#pragma once
#include <concepts>
#include <print>
#include "../core/engine.cpp"

// EventLogger: prints every event it receives to the console, like Falcon 1's EventLogger.
// Printing is a system call, so it runs on a non-realtime core (see Logging in src/core/spec.md).
template <std::derived_from<Event> E, int N>
struct EventLogger {
    u64 received = 0;   // events received since the start

    void process(In<Events<E, N>> incoming_events) {
        for (const E& _ : incoming_events) {
            std::println("EventLogger: event {}", ++received);
        }
    }
};
