#pragma once
#include <chrono>
#include "../core/engine.cpp"

// EventGenerator: emits a ScheduledEvent at a fixed rate, like Falcon 1's EventSource.
// process() never waits (see Processors in src/core/spec.md): it checks the clock on every pass and returns at once until the next event is due.
// ScheduledEvent comes from a timer, not a stream, so it derives from Event directly and has no header.
struct ScheduledEvent : Event {};

struct EventGeneratorOptions {
    double rate = 1;   // events per second (Hz)
};

template <EventGeneratorOptions O>
struct EventGenerator {
    std::chrono::steady_clock::time_point start;   // set on the first pass
    std::chrono::steady_clock::time_point due;     // when the next event is due
    u64 emitted = 0;                               // events emitted so far

    void process(Out<Events<ScheduledEvent, 1>> out) {
        using namespace std::chrono;
        steady_clock::time_point now = steady_clock::now();
        if (emitted == 0) {
            start = due = now;   // the first pass starts the clock
        }
        if (now < due) {
            return;
        }

        out.push(ScheduledEvent {});
        ++emitted;
        due = start + duration_cast<steady_clock::duration>(duration<double>(emitted / O.rate));   // on a fixed schedule, so the events don't drift
    }
};
