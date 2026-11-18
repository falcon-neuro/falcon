---
description: "Falcon's Trigger: a trigger event once every channel of a stream has been named by an event. Its input and output, the rule by which it fires, an example and its limitations."
---

# Trigger

{{ api("Trigger") }} reads events that name channels. It emits a trigger event once every channel of the stream has been named at least once since it last fired.

It is built for measuring latency with a test signal on every channel. On neural data, one channel that never has an event keeps it from firing.

## Input and output

| | Name | Carries |
|-|------|---------|
| Input | `input` | Events that each name the channels something happened on, e.g. [`Spikes`](../data_flow/events.md#spikes) or [`LevelCrossings`](../data_flow/events.md#levelcrossings). |
| Output | `output` | [`TriggerEvent`](../data_flow/events.md#triggerevent) events: one each time the trigger fires. |

The trigger has no options.

## When it fires

The trigger keeps the set of channels that have been named since it last fired. The set is empty at the start. For every event it reads:

1. It adds the channels that the event names to the set.
2. When the set then holds every channel of the stream, it emits one `TriggerEvent` and empties the set.

The `TriggerEvent` holds the header of the event that completed the set, which is the header of that event's sample.

A stream of 4 channels, numbered from 0:

| Event's sample | Channels the event names | Set afterwards | Fires |
|----------------|--------------------------|----------------|-------|
| 10 | 0, 1 | 0, 1 | No |
| 12 | 2 | 0, 1, 2 | No |
| 15 | 1, 3 | Empty | Yes, on sample 15 |
| 20 | 0, 1, 2, 3 | Empty | Yes, on sample 20 |
| 26 | 3 | 3 | No |

!!! note "Edge cases"
    - A channel that is named twice before the trigger fires counts once, as channel 1 does on samples 10 and 15.
    - One event that names every channel makes the trigger fire by itself, as on sample 20.
    - The channels that complete a set aren't kept for the next one. After sample 15 the set is empty, although the event also named channel 1.

## Delay

The trigger adds no delay: it fires on the event that completes the set and gives its own event that event's header.

It reads no clock. The same events therefore give the same trigger events.

## Limitations

- The trigger has no time window. Channels that are named far apart in time complete a set as well as channels named on one sample.
- It can't be set to fire on some of the channels. Every channel of the stream has to be named.
- The `TriggerEvent` doesn't say which events completed the set, apart from the last one's sample.
- An event that a full [ring](../data_flow/rings.md) drops before it reaches the trigger isn't counted, so the set is completed later or not at all.
