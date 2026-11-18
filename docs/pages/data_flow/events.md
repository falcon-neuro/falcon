---
description: "The events that Falcon's built-in processors emit: the processor that emits each one, and whether the Recorder writes it to its file."
---

# Events

A processor emits an event when something happens, for example when a channel crosses a threshold on a sample. The table lists the events that Falcon's built-in processors emit.

| Event | Emitted by | Written by the Recorder |
|-------|------------|-------------------------|
| `Ripple` | Ripple detector | Yes. It holds its header only. |
| `TriggerEvent` | Trigger | Yes. It holds its header only. |
| `Spikes` | [Spike detector](../processors/spike_detector.md) | No. It also holds the channels that crossed the threshold, which the file has no place for. |
| `LevelCrossings` | Level crossing detector | No. It also holds the channels that crossed the level, which the file has no place for. |
| `ScheduledEvent` | Event generator | No. It comes from a timer, so it has no sample's header. It also holds a count. |

## In the code

The events are the structs {{ api("Ripple") }}, {{ api("TriggerEvent") }}, {{ api("Spikes") }}, {{ api("LevelCrossings") }} and {{ api("ScheduledEvent") }}. The Recorder is the struct {{ api("Recorder") }}.
