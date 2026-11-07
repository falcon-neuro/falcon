#pragma once
#include <chrono>
#include <thread>
#include "../core/engine.cpp"

// EventGenerator: emits a ScheduledEvent at a fixed rate, like Falcon 1's EventSource.
// It is an input processor, so it may sleep its core's thread: each pass sleeps until the next
// event is due, then emits it, so the core doesn't spin. A stop signal is seen only after the
// current sleep ends, so stopping can take up to one period (1 / rate). ScheduledEvent comes from a
// timer, not a stream, so it derives from EventRecord directly and has no header.
struct ScheduledEvent : EventRecord {
    u32 count = 0;  // number of this event since the start, from 1
};

struct EventGeneratorOptions {
    double rate = 1;  // events per second (Hz)
};

template <EventGeneratorOptions options>
struct EventGenerator {
    struct Connections {
        EventOutputBuffer<ScheduledEvent, 1> output;
    };

    std::chrono::steady_clock::time_point start;  // set on the first pass
    std::chrono::steady_clock::time_point due;    // when the next event is due
    u64 emitted = 0;                              // events emitted so far

    void process(Connections connections) {
        using namespace std::chrono;
        if (emitted == 0) {
            start = due = steady_clock::now();  // the first pass starts the clock
        }
        std::this_thread::sleep_until(due);

        ++emitted;
        connections.output.push(ScheduledEvent{.count = static_cast<u32>(emitted)});
        due = start + duration_cast<steady_clock::duration>(duration<double>(
                          static_cast<double>(emitted) /
                          options.rate));  // on a fixed schedule, so the events don't drift
    }
};
