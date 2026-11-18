---
description: "Falcon's LatencyBenchmark: for each event, the time from the arrival of the event's sample to the moment the benchmark runs. What it measures and what it leaves out, the file it writes and its summary in the log."
---

# LatencyBenchmark

{{ api("LatencyBenchmark") }} measures how long Falcon takes. For each event it receives, it takes the time from the arrival of the event's sample at Falcon to the moment the benchmark itself runs. When the run ends, it writes one row per event to a file and a summary to the log.

Placed right before an output processor, it measures all of Falcon's work up to the decision to act.

## Input

| Name | Carries |
|------|---------|
| `input` | Events that belong to a sample, for example [`TriggerEvent`](../data_flow/events.md#triggerevent). |

An event that belongs to no sample, such as [`ScheduledEvent`](../data_flow/events.md#scheduledevent), has no arrival to count from, so it can't be connected.

## Option

| Option | Meaning | Default |
|--------|---------|---------|
| `file_name` | The name of the file the measurements are written to, in the run's directory. It is a name only, without a folder. | `latency_benchmark.csv` |

## What it measures

When a sample reaches Falcon, the input processor reads the CPU's timestamp counter and stores the value in the sample's header. An event made from that sample carries the header along. The benchmark reads the counter again when it runs. With $c_a$ the counter at the arrival, $c_b$ the counter at the benchmark and $\tau$ the length of one tick of the counter in nanoseconds, the latency of an event is

$$
(c_b - c_a) \cdot \tau
$$

The benchmark measures $\tau$ over the whole run. At the start and at the end it reads the counter together with the operating system's clock. $\tau$ is the time between the two readings divided by the ticks between them.

| Part of the way | Measured |
|-----------------|----------|
| Before the sample reached Falcon: the acquisition hardware and the way to the computer. | No |
| From the input processor to the benchmark: the rings the data passed through and every processor that ran on the way. | Yes |
| After the benchmark: the work of the output processor and the output hardware. | No |

The benchmark reads the counter once each time it runs. Events that it receives together therefore get the same reading.

## The file

The file is a CSV file with one row per event, in the order in which the events arrived:

| Column | Holds |
|--------|-------|
| `sample_number` | The number of the event's sample. |
| `device_timestamp` | The time at which the device took the sample, in the device's own unit. |
| `latency_ns` | The latency, in whole nanoseconds. |

During the run the rows are kept in memory. The file is written when the run ends, so the benchmark writes to no file while it measures.

## The summary

When the run ends, the benchmark writes one line to the log. The line holds the number of events and their shortest, median, 99th-percentile and longest latency in microseconds. The median and the percentile are measured values, not interpolated ones.

## Requirements

- The CPU's timestamp counter ticks at a constant rate: the CPU has the flags `constant_tsc` and `nonstop_tsc`.
- The operating system's clock source is the timestamp counter (`tsc`). With another clock source, the counters of two cores may disagree.
- The file doesn't exist yet. Two benchmarks in one graph therefore need different file names.

If a requirement isn't met, the run fails at its start with an error message.

## Limitations

- The benchmark keeps 1,048,576 events. Later events aren't kept. The log then gives their number in a warning.
- A run that is killed leaves a file without rows.
- A negative latency means that the timestamp counters of two cores disagree.
