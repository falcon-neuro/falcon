---
description: "Falcon's SignalGenerator: a test signal on every channel in place of acquisition hardware. Its output and options, its waveforms with their formulas, and how it keeps the samples on time."
---

# SignalGenerator

{{ api("SignalGenerator") }} replaces acquisition hardware for testing a graph. It makes one sample at a time at the stream's rate, with the same waveform on every channel. A slow wave can be added under the waveform, so that the effect of a filter can be seen.

The generator keeps the samples on time itself, so it needs a core of its own.

## Output

| Name | Carries |
|------|---------|
| `output` | Samples in µV, at the stream's rate. |

The generator has no input.

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `waveform` | The shape of the signal: `Sine`, `Square`, `SpikeTrain` or `RippleBursts`. | `Sine` |
| `frequency` | How often the shape repeats, in Hz: the frequency of the sine or square wave, the spikes per second of a spike train, the bursts per second of ripple bursts. It must be above 0 and below half the stream's rate. A spike train takes fewer than 500 spikes per second, so that the spikes don't overlap. | 10 |
| `amplitude` | The size of the signal, in µV from zero to peak. For a spike train it scales the spike. For ripple bursts it is the oscillation's amplitude at a burst's peak. It can't be negative. | 100 |
| `phase_step_per_channel` | The shift of each channel against the channel before it, in radians. With 0, every channel carries the same signal. | 1 |
| `lfp_frequency` | The frequency of the slow wave that is added under the signal, in Hz. It counts only when `lfp_amplitude` is above 0. It must then be above 0 and below half the stream's rate. | 0 |
| `lfp_amplitude` | The size of that slow wave, in µV from zero to peak. With 0, none is added. It can't be negative. | 0 |
| `ripple_frequency` | For ripple bursts: the frequency of the oscillation inside the bursts, in Hz. It must be above 0 and below half the stream's rate. | 180 |
| `ripple_burst_duration_milliseconds` | For ripple bursts: the time from a burst's start to its end, in milliseconds. It must be above 0 and short enough that the bursts don't overlap. | 80 |
| `ripple_background_amplitude` | For ripple bursts: the oscillation's amplitude between two bursts, in µV from zero to peak. It lies between 0 and `amplitude`. | 10 |
| `minimum_busy_spin_microseconds` | The shortest time before a sample is due at which the generator stops sleeping and waits actively, in microseconds (see [Timing](#timing)). It can't be negative. | 10 |

## What it computes

Let $n$ be the sample's number, counted from 0, and $f_s$ the stream's rate. The sample's time is $t = n / f_s$. Let $c$ be the channel's number, counted from 0, $\Delta$ the phase step, $f$ the frequency and $A$ the amplitude.

### Sine

$$
v_c[n] = A \sin(2 \pi f t + c \Delta)
$$

### Square

$$
v_c[n] =
\begin{cases}
A & \text{if } \sin(2 \pi f t + c \Delta) \geq 0 \\
-A & \text{otherwise}
\end{cases}
$$

### Spike train

Every channel carries one spike per period. The phase step shifts a channel's spikes by the fraction $c \Delta / (2 \pi)$ of the period. With $\tau$ the time since the channel's last spike started and $g$ a Gaussian,

$$
g(\tau; m, s) = e^{-(\tau - m)^2 / (2 s^2)}
$$

a spike is a sharp trough followed by a smaller and slower rebound:

$$
v_c[n] =
\begin{cases}
A \, \bigl( 0.3 \, g(\tau; 0.7, 0.25) - g(\tau; 0.3, 0.1) \bigr) & \text{if } \tau < 2 \\
0 & \text{otherwise}
\end{cases}
$$

with $\tau$, $m$ and $s$ in milliseconds.

| Part of the spike | Time after the spike starts | Value |
|-------------------|-----------------------------|-------|
| Trough | 0.29 ms | $-0.92 A$ |
| Rebound | 0.70 ms | $0.30 A$ |
| End | 2 ms | 0 |

The trough is shallower than $A$ because the rebound already rises under it.

### Ripple bursts

The oscillation runs all the time at `ripple_frequency`, $f_r$. Its amplitude swells within each burst, from the background amplitude $B$ up to $A$ and back. With $\tau$ the time since the last burst started and $D$ the burst's duration, the amplitude is

$$
a(\tau) =
\begin{cases}
B + (A - B) \, \frac{1}{2} \bigl( 1 - \cos(2 \pi \tau / D) \bigr) & \text{if } \tau < D \\
B & \text{otherwise}
\end{cases}
$$

and the signal is

$$
v_c[n] = a(\tau) \sin(2 \pi f_r t + c \Delta)
$$

A burst comes on every channel at the same time, as a ripple does. The phase step shifts only the oscillation inside it. A burst starts every $1 / f$ seconds and peaks half its duration later.

### The slow wave

With `lfp_amplitude` $A_L$ above 0 and `lfp_frequency` $f_L$, every waveform gets a sine added:

$$
A_L \sin(2 \pi f_L t + c \Delta)
$$

## The samples' headers

| Part of the header | Value |
|--------------------|-------|
| The sample's number | Counted from 0. |
| The time the device took it | The sample's number, since no device took it. |
| The time it reached Falcon | Read when all channels of the sample are computed, so a latency measured from it holds none of the generator's work. |
| The number of samples lost before it | Always 0. |

## Timing

Sample $n$ is due $n / f_s$ seconds after the first one. Each due time is counted from the first sample, so a late sample doesn't delay the ones after it.

The generator sleeps until shortly before a sample is due and then waits actively until the due time. Sleeping alone would end too late for a period of 33 µs, and waiting actively all the time would keep the core fully busy. The active wait lasts `minimum_busy_spin_microseconds` plus the longest delay with which a sleep ended recently. After a sleep that ended late, the generator therefore sleeps less for a while.

A generator that falls behind emits the samples it missed one right after another. It skips none.

## Limitations

- The signal holds no noise and is the same in every run.
- The samples are timed with the computer's clock, so their rate is not the rate of any acquisition device's clock.
- How close a sample comes to its due time depends on the operating system. This is not measured.
