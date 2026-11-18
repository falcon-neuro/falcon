---
description: "Falcon's EventLogger: writes the events it receives to the log, each one or their number in each second. Its input, its option and the lines it writes."
---

# EventLogger

{{ api("EventLogger") }} writes the events it receives to the log: every event, or the number of events that arrived in each second. It takes events of any kind and has no output.

The log is shown on the console and written to a file in the run's directory.

## Input

| Name | Carries |
|------|---------|
| `input` | Events of any kind. |

## Option

| Option | Meaning | Default |
|--------|---------|---------|
| `style` | What goes to the log: every event (`EveryEvent`), or once a second the number of events that arrived (`CountPerSecond`). | `EveryEvent` |

## Every event

The logger writes one line per event. The line starts with the event's number, counted from 1 at the start of the run. What follows depends on the event:

| The event | Example of its line |
|-----------|---------------------|
| Belongs to a sample and names channels, e.g. [`Spikes`](../data_flow/events.md#spikes) | `event 3, sample 12345, channels 17, 40` |
| Belongs to a sample and names no channel, e.g. [`Ripple`](../data_flow/events.md#ripple) | `event 3, sample 12345` |
| Belongs to no sample and holds a count, e.g. [`ScheduledEvent`](../data_flow/events.md#scheduledevent) | `event 3, count 3` |
| Any other | `event 3` |

Other values of an event don't appear in the log.

## Count per second

Once a second the logger writes one line with the number of events that arrived in that second and the number since the start:

```text
12 events in the last second (340 in total)
```

One event that names channels can name several of them, so a count of such events is not a count of spikes or crossings. For these events the line also gives the number of channels named:

```text
12 events with 31 channels in the last second (340 events in total)
```

A second without events gives a line with 0. The seconds are counted by the computer's clock, from the moment the logger first runs.

## Limitations

- The log takes at most one line from the logger every 100 ms. With every event logged, events that come faster than that are counted and not shown one by one.
- The logger writes no file of its own. [`Recorder`](recorder.md) writes events to a file with all their values.
