---
description: "Falcon's LevelCrossingDetector: one event on every sample where channels cross a level in a chosen direction, with a number of samples for which a channel is then ignored. Its input, output, options and definition of a crossing."
---

# LevelCrossingDetector

{{ api("LevelCrossingDetector") }} compares every channel of its input with one level. It emits an event on the sample where a channel crosses the level in the chosen direction, then ignores that channel for a set number of samples.

Unlike [`SpikeDetector`](spike_detector.md), it takes the direction as an option of its own, accepts a level of 0 and ignores a channel for some samples after each of its crossings.

## Input and output

| | Name | Carries |
|-|------|---------|
| Input | `input` | Samples. |
| Output | `output` | [`LevelCrossings`](../data_flow/events.md#levelcrossings) events: one for each sample on which at least one channel crossed, none for the other samples. |

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `threshold` | The level, in µV or in whatever unit the stream has, the same for every channel. It may be positive, negative or 0. | 0 |
| `direction` | The direction in which a channel must cross the level: upward, from below the level to above it, or downward. | Upward |
| `post_detection_block` | The number of samples for which a channel is ignored after each of its crossings. 0 ignores nothing. It can't be negative. | 2 |

## What counts as a crossing

Let $v_c[n]$ be the value of channel $c$ on sample $n$ and $\theta$ the level. A channel is beyond the level when its value lies on the far side of it, in the chosen direction:

$$
\text{beyond}_c[n] =
\begin{cases}
v_c[n] > \theta & \text{if the direction is upward} \\
v_c[n] < \theta & \text{if the direction is downward}
\end{cases}
$$

A channel makes a transition on the sample where it is beyond the level and wasn't on the sample before:

$$
\text{transition}_c[n] = \text{beyond}_c[n] \land \lnot \, \text{beyond}_c[n-1]
$$

A transition is a crossing unless the channel is blocked. A crossing on sample $n$ blocks its channel for the next $B$ samples, $n + 1$ to $n + B$, where $B$ is `post_detection_block`. Each channel is blocked on its own, so channels that cross one sample apart are all detected.

One channel with a level of 0, the upward direction and a block of 2 samples:

| Sample | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|--------|---|---|---|---|---|---|---|
| Value | −1 | 2 | −1 | 3 | −1 | −2 | 4 |
| Beyond the level | no | yes | no | yes | no | no | yes |
| Transition | no | yes | no | yes | no | no | yes |
| Blocked | no | no | yes | yes | no | no | no |
| Crossing | no | yes | no | no | no | no | yes |

!!! note "Edge cases"
    - A value exactly equal to the level isn't beyond it.
    - A channel value that isn't a number is never beyond the level.
    - The first sample the detector receives is never a crossing, since no sample came before it. The detector only notes which channels are beyond the level on it.
    - A channel that goes beyond the level while it is blocked and stays there doesn't cross when the block ends, since it was already beyond the level on the sample before. It can cross again only after a sample on which it was back within the level.

## Lost samples

The detector doesn't treat a gap in its input differently. On the first sample after lost samples, it compares each channel with the last sample before the gap. A block is counted in samples that arrive, so samples lost during a block make it last longer.

## Delay

The detector adds no delay: a crossing on sample $n$ is reported by an event made from sample $n$.

## Limitations

- The detector does not remove an offset or a slow wave from a channel. Each value is compared with the level as it is.
- The level, the direction and the block are the same for all channels.
- The event holds no value and no waveform. It says only which channels crossed.
