---
description: "Falcon's rings: the memory that carries a connection's data from one CPU core to another, why a graph runs on several cores, how many samples a ring holds, and what happens when a ring is full."
---

# Rings

A ring carries the data of a connection from one core to another. A connection has a ring when its two processors run on different cores. Between two processors on the same core it has none.

A ring is a circular row of slots. The writing core fills the slots in turn, and the reading core reads them in the same order.

## Several cores

A CPU has several cores, each of which runs one thing at a time. Falcon gives each core a part of the graph: the processors placed on a core run one after another, once for every sample. At 30 kHz a core has 33.3 µs for each sample. A graph whose processors need more than that cannot run on one core, so its processors are spread over several cores, which work at the same time.

Rings are what connects these cores, so they are what lets a graph use more than one. They also keep the cores independent of each other, because the writing core never waits for the reading core. A processor that writes to a file, for example, can stall for a moment. On a core of its own, behind a ring, it holds up no other core.

## Hops

A sample's move through a ring is a hop. A hop takes time, because the data must travel between the two cores' caches. Every hop on the way from the input to an output therefore adds to the latency. Two connected processors on the same core pass their data without a hop.

## Size

A ring holds a set number of samples, which is a power of 2. This number is how far the reading core can fall behind the writing core before the ring is full. A ring of 64 samples at 30 kHz, for example, holds 2.1 ms of samples.

## A full ring

A ring fills up when the reading core takes samples more slowly than the writing core gives them, for longer than the ring can hold. The writing core doesn't wait for room, so the run goes on:

| | What happens |
|-|--------------|
| The sample that doesn't fit | It is dropped. |
| The samples after it | Their header holds the number of samples lost before them. |
| The console | It shows a warning with the ring's name and the number of samples the ring dropped. |
| A recording | It has a gap, which the sample numbers and the numbers of lost samples show. |

An event that doesn't fit into a ring is dropped as well and named in the warning. The events after it hold no sign of it.

!!! warning
    A ring that drops samples means that the reading core has more work than it can do in the time between two samples. The remedy is a change to the graph, for example moving a processor to another core, or faster hardware.

## In the code

A ring for samples is the struct {{ api("SampleRing") }}, and a ring for events the struct {{ api("EventRing") }}.
