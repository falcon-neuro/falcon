---
description: "Falcon's spike detector: one event on every sample where channels cross a fixed threshold, without waiting for the spike's peak. Its input, output and option, and exactly what counts as a crossing."
---

# Spike detector

The spike detector watches every channel of its input and reports the moment a channel crosses a threshold. It reports on the very sample of the crossing, without waiting for the spike's peak, so that what follows it in the graph can act as early as possible.

It compares the channel values as they are with one fixed threshold. An offset or a slow wave on a channel shifts its values against that threshold, so the input is normally band-pass filtered first, for example by the [IIR filter](iir_filter.md).

## Input and output

| | Name | Carries |
|-|------|---------|
| Input | `input` | Samples. |
| Output | `output` | `Spikes` events: one for each sample on which at least one channel crossed, none for the other samples. |

## Option

| Option | Meaning | Default |
|--------|---------|---------|
| `threshold` | The threshold, in microvolts, the same for every channel. Its sign gives the direction: a negative threshold detects downward crossings, a positive one upward crossings. | −50 |

## What counts as a crossing

Call $v_c[n]$ the value of channel $c$ on sample $n$, and $\theta$ the threshold. A channel is beyond the threshold when its value lies on the far side of it:

$$
\text{beyond}_c[n] =
\begin{cases}
v_c[n] < \theta & \text{if } \theta < 0 \\
v_c[n] > \theta & \text{if } \theta > 0
\end{cases}
$$

A channel crosses on the sample where it is beyond the threshold and wasn't on the sample before:

$$
\text{crossing}_c[n] = \text{beyond}_c[n] \land \lnot \, \text{beyond}_c[n-1]
$$

It follows that:

- A value exactly equal to the threshold isn't beyond it.
- A channel that stays beyond the threshold for several samples crosses once, on the first of them. It can cross again only after a sample on which it was back within the threshold.
- A channel value that isn't a number is never beyond the threshold.
- The first sample the detector receives is never a crossing, since no sample came before it. The detector only notes which channels are beyond the threshold on it.

One channel with a threshold of −50 µV:

| Sample | 1 | 2 | 3 | 4 | 5 | 6 |
|--------|---|---|---|---|---|---|
| Value (µV) | −20 | −60 | −75 | −40 | −50 | −55 |
| Beyond the threshold | no | yes | yes | no | no | yes |
| Crossing | no | yes | no | no | no | yes |

## The event

On a sample where at least one channel crosses, the detector emits one `Spikes` event. It holds:

- the header of that sample: its number and its times;
- a mask with one bit per channel, set for every channel that crossed on that sample.

Channels that cross on the same sample share one event.

## What it leaves out

The detector does the comparison above and nothing more:

- It marks the crossing, not the peak: the spike's peak comes on that sample or later.
- It has no dead time. Nothing sets a minimum time between two crossings of a channel, so a channel whose values hover around the threshold gives several events for what may be one spike.
- It doesn't keep the waveform: the event says which channels crossed, not what the spike looks like.
- It doesn't measure the noise: the threshold is a fixed value and never adapts to a channel.

## Lost samples

The detector doesn't treat a gap in its input differently. On the first sample after lost samples, it compares each channel with the last sample before the gap: a channel that was within the threshold then and is beyond it now counts as a crossing.

## What Falcon checks

Falcon refuses a graph, before it runs, when its spike detector's threshold is 0: the sign gives the direction, and 0 has none.

## Delay

The detector adds no delay: a crossing on sample $n$ is reported by an event made from sample $n$. From one sample to the next it keeps a single bit per channel, whether the channel was beyond the threshold.

The channels are compared 16 at a time, or 8 at a time on a CPU without AVX-512.

## Choosing the threshold

Extracellular spikes mostly point downward, and the default threshold is negative to match.

For the size, a common choice is four to five times the standard deviation of the noise. Quian Quiroga and colleagues estimate that standard deviation from the band-pass filtered channel as

$$
\sigma = \frac{\operatorname{median}(|v|)}{0.6745}
$$

which the spikes themselves barely change, unlike the plain standard deviation. Falcon doesn't compute it. Compute it beforehand from a stretch of filtered recording, for example with NumPy, where `filtered` holds one row per sample and one column per channel:

```python
import numpy as np

noise = np.median(np.abs(filtered), axis=0) / 0.6745
print(-4 * noise)
```

This gives one value per channel, while the detector has one threshold for all of them: look at the values across the channels before choosing.

- Quian Quiroga R, Nadasdy Z, Ben-Shaul Y (2004). Unsupervised spike detection and sorting with wavelets and superparamagnetic clustering. *Neural Computation* 16, 1661–1687. <https://doi.org/10.1162/089976604774201631>

## In the code

The detector is the struct {{ api("SpikeDetector") }}, its option is in {{ api("SpikeDetectorOptions") }}, and the event is {{ api("Spikes") }}.
