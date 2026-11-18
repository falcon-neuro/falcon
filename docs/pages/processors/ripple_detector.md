---
description: "Falcon's RippleDetector: an event when the power of a ripple-band signal rises above a threshold that follows the signal. Its inputs, outputs and options, the statistics it keeps, its burn-in and its lockout."
---

# RippleDetector

{{ api("RippleDetector") }} detects ripples in a signal that is already filtered to the ripple band. It computes the signal's power on every sample and learns the power's usual level and spread while it runs. It emits an event when the power rises above a threshold derived from both.

The detector doesn't filter. Its input normally comes from a band-pass filter, for example [`IirFilter`](iir_filter.md) or [`FirFilter`](fir_filter.md).

## Inputs and outputs

| | Name | Carries |
|-|------|---------|
| Input | `input` | Samples, already filtered to the ripple band. |
| Input | `threshold_multiplier` | A state value: how far the power must rise above its mean to count as a ripple, in mean absolute deviations. It is 6 when nothing is connected. A change takes effect on the next sample. |
| Input | `is_detection_enabled` | A state value: whether the detector detects. It is on when nothing is connected. |
| Output | `output` | [`Ripple`](../data_flow/events.md#ripple) events: one for each detected ripple, on the sample at which the power crossed the threshold. |
| Output | `is_in_lockout` | A state value: on from the sample that crossed the threshold until the lockout has passed, off otherwise. It may be left unconnected. |

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `smoothing_time_seconds` | The time over which the detector averages the power to learn its mean and its deviation, in seconds. It must last at least one sample. | 10 |
| `lockout_milliseconds` | The time the detector waits after a detection before it can detect again, in milliseconds. 0 means no wait. It can't be negative. | 30 |

## What it computes

Let $v_c[n]$ be the value of channel $c$ on sample $n$ and $C$ the channel count. The power of a sample is the mean of its squared channel values:

$$
p[n] = \frac{1}{C} \sum_{c=1}^{C} v_c[n]^2
$$

The power is one value for the whole stream, so a ripple is detected from all channels together.

The detector keeps running statistics of the power: its mean $\mu$ and its mean absolute deviation $d$. Let $T$ be the smoothing time in seconds and $f$ the rate in Hz. A sample that is added to the statistics weighs $\alpha = 1 / (T f)$:

$$
\begin{align*}
\mu &\leftarrow (1 - \alpha) \mu + \alpha \, p[n] \\
d &\leftarrow (1 - \alpha) d + \alpha \, |p[n] - \mu|
\end{align*}
$$

The deviation is taken from the mean that already includes the sample.

A sample is tested before it is added. With the multiplier $m$, the detector computes the distance of the power from the mean and the threshold for that distance:

$$
x[n] = p[n] - \mu \qquad \theta = m \, d
$$

Sample $n$ is a crossing when its distance is above the threshold and the distance of the sample tested before it was not:

$$
\text{crossing}[n] = (x[n] > \theta) \land (x[n^-] \leq \theta)
$$

where $n^-$ is the last sample tested before $n$. On a crossing the detector emits one `Ripple` event with the header of sample $n$.

## Burn-in

For the first smoothing time after the start, the detector only learns the statistics and detects nothing. With the default of 10 s at 30 kHz, these are the first 300,000 samples.

During the burn-in, sample $k$ weighs $\alpha + (1 - \alpha) / k$ instead of $\alpha$. The statistics therefore start as plain averages of the first samples, where the weight $\alpha$ alone would let them rise slowly from 0.

The log says when the burn-in starts. When it ends, the log gives the mean and the deviation the detector has learned.

## Lockout

After a crossing, the detector waits for the lockout. The samples in it are neither tested nor added to the statistics, so the ripple that was detected doesn't raise the mean. `is_in_lockout` is on during the lockout.

The first sample after the lockout is compared with the sample that crossed. If the power is still above the threshold at that point, no new event is emitted. The power first has to fall to the threshold or below it and rise again.

## Which samples are tested

| Sample | Tested | Added to the statistics |
|--------|--------|-------------------------|
| During the burn-in | No | Yes |
| During a lockout | No | No |
| While `is_detection_enabled` is off | No | No |
| With a power that is infinite or not a number | No | No. The log gets a warning with the sample's number. |
| Any other | Yes | Yes |

## Lost samples

The detector doesn't treat a gap in its input differently. The burn-in and the lockout are counted in samples that arrive, so samples lost during either one make it last longer.

## Delay

The detector adds no delay: a crossing on sample $n$ is reported by an event made from sample $n$. The power has to rise to the threshold before that, so the event comes after the ripple's start.

The power is computed in 64-bit floating-point numbers, in which the square of a float32 value is exact.

## Choosing the values

!!! tip
    A lower `threshold_multiplier` detects more ripples and detects them earlier. A higher one detects fewer and detects them later. Since it is a state value, another processor can change it during a run.

A longer smoothing time gives a steadier threshold, which follows slow changes of the signal later.

`is_detection_enabled` can be switched off for a time in which the signal isn't neural, for example during a stimulation's artifact. The artifact is then neither detected as a ripple nor added to the statistics.

## Limitations

- The event marks the crossing of the threshold. It says neither how strong the ripple becomes nor how long it lasts.
- `is_in_lockout` lasts as long as the lockout. It doesn't follow the ripple's length.
- The power is the mean over all channels, with every channel weighing the same. A channel that carries no ripples lowers the power of a ripple on the other channels.
- The detector has a single threshold. It doesn't find a ripple's start or end.
