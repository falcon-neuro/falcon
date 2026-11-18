---
description: "Falcon's spike detector: one event on every sample where channels cross a fixed threshold, without waiting for the spike's peak. Its input, output, option and definition of a crossing."
---

# SpikeDetector

{{ api("SpikeDetector") }} compares every channel of its input with one fixed threshold. It emits an event on the sample where a channel crosses the threshold. It does not wait for the spike's peak, so the processors after it in the graph can act sooner.

The detector does not remove an offset or a slow wave from a channel. Either one shifts the channel's values against the threshold, so the input is normally band-pass filtered first, for example by [`IirFilter`](iir_filter.md).

## Input and output

| | Name | Carries |
|-|------|---------|
| Input | `input` | Samples. |
| Output | `output` | [`Spikes`](../data_flow/events.md#spikes) events: one for each sample on which at least one channel crossed. |

## Option

| Option | Meaning | Default |
|--------|---------|---------|
| `threshold` | The threshold, in microvolts, the same for every channel. Its sign gives the direction: a negative threshold detects downward crossings, a positive one upward crossings. It can't be 0, because 0 has no sign. | −50 |

## What counts as a crossing

Let $v_c[n]$ be the value of channel $c$ on sample $n$ and $\theta$ the threshold. A channel is beyond the threshold when its value lies on the far side of it:

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

A channel that stays beyond the threshold for several samples therefore crosses once, on the first of them.

One channel with a threshold of −50 µV:

| Sample | 1 | 2 | 3 | 4 | 5 | 6 |
|--------|---|---|---|---|---|---|
| Value (µV) | −20 | −60 | −75 | −40 | −50 | −55 |
| Beyond the threshold | no | yes | yes | no | no | yes |
| Crossing | no | yes | no | no | no | yes |

!!! note "Edge cases"
    - A value exactly equal to the threshold isn't beyond it, as on sample 5 above.
    - A channel value that isn't a number is never beyond the threshold.
    - The first sample the detector receives is never a crossing, since no sample came before it. The detector only notes which channels are beyond the threshold on it.

## Limitations

- The detector has no dead time: nothing sets a minimum time between two crossings of a channel. A channel whose values hover around the threshold therefore gives several events for what may be one spike.
- The detector doesn't measure the noise. The threshold is one fixed value for all channels.

## Lost samples

The detector doesn't treat a gap in its input differently. On the first sample after lost samples, it compares each channel with the last sample before the gap: a channel that was within the threshold then and is beyond it now counts as a crossing.

## Delay

The detector adds no delay: a crossing on sample $n$ is reported by an event made from sample $n$.

## Choosing the threshold

!!! tip
    Extracellular spikes mostly point downward, which is why the default threshold is negative. A common choice for its size is four to five times the standard deviation of the noise.

Quian Quiroga and colleagues[^quiroga] estimate that standard deviation from the band-pass filtered channel as

$$
\sigma = \frac{\operatorname{median}(|v|)}{0.6745}
$$

which spikes change little, unlike the plain standard deviation. Falcon doesn't compute it. Compute it beforehand from a stretch of filtered recording, for example with NumPy, where `filtered` holds one row per sample and one column per channel:

```python
import numpy as np

noise = np.median(np.abs(filtered), axis=0) / 0.6745
print(-4 * noise)
```

This gives one value per channel, while the detector has one threshold for all of them: look at the values across the channels before choosing.

[^quiroga]: Quian Quiroga R, Nadasdy Z, Ben-Shaul Y (2004). Unsupervised spike detection and sorting with wavelets and superparamagnetic clustering. *Neural Computation* 16, 1661–1687. <https://doi.org/10.1162/089976604774201631>
