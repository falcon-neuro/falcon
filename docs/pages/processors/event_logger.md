---
description: "Falcon's EventLogger: writes the events of its connections to the log, each one or their number in each second. Its input, its option and the lines it writes."
---

# EventLogger

{{ api("EventLogger") }} writes the events it receives to the log: every event, or the number of events that arrived in each second. It takes events of any kind from any number of connections, and has no output.

The log is shown on the console and written to a file in the run's directory.

## Input

| Name | Carries |
|------|---------|
| `inputs` | Any number of connections, at least one. Each brings events of any kind. |

Each connection has a name. Every line starts with the name of the connection it is about, and the events of each connection are counted apart.

## Option

| Option | Meaning | Default |
|--------|---------|---------|
| `style` | What goes to the log: every event (`EveryEvent`), or once a second the number of events that arrived (`CountPerSecond`). | `EveryEvent` |

## Every event

The logger writes one line per event. After the connection's name comes the event's number, counted for that connection from 1 at the start of the run. What follows depends on the event:

| The event | Example of its line |
|-----------|---------------------|
| Belongs to a sample and names channels, e.g. [`Spikes`](../data_flow/events.md#spikes) | `spike_detector_output: event 3, sample 12345, channels 17, 40` |
| Belongs to a sample and names no channel, e.g. [`Ripple`](../data_flow/events.md#ripple) | `ripple_detector_output: event 3, sample 12345` |
| Belongs to no sample and holds a count, e.g. [`ScheduledEvent`](../data_flow/events.md#scheduledevent) | `event_generator_output: event 3, count 3` |
| Any other | `my_processor_output: event 3` |

Other values of an event don't appear in the log.

## Count per second

Once a second the logger writes one line for each connection, with the number of events that arrived through it in that second and the number since the start:

```text
ripple_detector_output: 12 events in the last second (340 in total)
```

One event that names channels can name several of them, so a count of such events is not a count of spikes or crossings. For these events the line also gives the number of channels named:

```text
spike_detector_output: 12 events with 31 channels in the last second (340 events in total)
```

A connection without events in a second gives a line with 0. The seconds are counted by the computer's clock, from the moment the logger first runs.

## Limitations

- The log takes at most one line from the logger every 100 ms. With every event logged, events that come faster than that are counted and not shown one by one.
- The logger writes no file of its own. [`Recorder`](recorder.md) writes events to a file with all their values.
