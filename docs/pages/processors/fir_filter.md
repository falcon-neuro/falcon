---
description: "Falcon's FirFilter: a weighted sum of the newest input values on every channel. Its input, output and options, the equation it computes, its start without a transient and its delay."
---

# FirFilter

{{ api("FirFilter") }} applies the same FIR filter to every channel of its input. Each output value is a weighted sum of the newest input values of its channel, with the filter's taps as the weights.

An FIR filter with symmetric taps delays every frequency by the same time, so it keeps the shape of a waveform. It needs more operations per sample than an [`IirFilter`](iir_filter.md) of the same sharpness. Falcon doesn't design the filter: its taps come from a tool such as SciPy or MATLAB (see [Designing the taps](#designing-the-taps)).

## Input and output

| | Name | Carries |
|-|------|---------|
| Input | `input` | Samples whose channel values are 32-bit floating-point numbers. |
| Output | `output` | The filtered samples: the same channels and the same rate as the input, each sample with the header of the input sample it was computed from. |

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `design_rate` | The rate, in Hz, that the taps were designed for. It must equal the input's rate: at another rate every cutoff frequency would shift. | Required. |
| `taps` | The filter's taps, in the order of the input values they weigh: the tap for the newest value first, the tap for the oldest last. Every tap must be a finite number. | Required: at least one tap. |

## What it computes

Let $x[n]$ be a channel's value on sample $n$ and $h_0$ to $h_{N-1}$ the $N$ taps. The channel's output is

$$
y[n] = \sum_{k=0}^{N-1} h_k \, x[n-k]
$$

Every channel uses the same taps and keeps its own last $N$ input values. The channel values arrive and leave as 32-bit numbers, and the sum is computed with 64-bit numbers.

Each output value takes one multiplication and one addition per tap, so the work for a sample grows with the number of taps.

## Starting without a transient

A channel often carries a constant offset. A filter that started with zeros in place of the earlier input values would see that offset as a step, so its output would swing for $N - 1$ samples. A spike detector after the filter would report such a swing as spikes on every channel.

Falcon instead starts the filter as if its first input value had always been its input. The output then starts at the value the filter settles at for a constant input $x[0]$:

$$
y[0] = x[0] \sum_{k=0}^{N-1} h_k
$$

For taps that sum to 0, the output starts at 0.

## Lost samples

Every sample's header says how many samples were lost before it. When that count rises, the input has a gap whose two sides don't join. Filtering across the gap would give a swing of the same kind as a start from zeros.

The filter therefore starts again, as described above, on the first sample after a gap. It also writes a warning to the log, such as "restarted after 12 lost samples, at sample 4500000".

## Delay

The filter doesn't wait for later samples: it computes sample $n$ of its output from the input's samples up to $n$.

The delay it adds is the group delay of the design. For symmetric taps, which most design tools give, it is the same at every frequency:

$$
\frac{N - 1}{2} \text{ samples}
$$

A filter of 101 taps at 30 kHz, for example, delays its input by 50 samples, which is 1.67 ms. A sharper filter needs more taps, so it delays more.

## Designing the taps

In SciPy, `firwin` gives the taps of a filter in the order `taps` takes them:

```python
from scipy import signal

taps = signal.firwin(101, [300, 6000], pass_zero=False, fs=30000)
print(", ".join(repr(float(tap)) for tap in taps))
```

This band-pass, from 300 Hz to 6000 Hz for an input at 30 kHz, has 101 symmetric taps. With `design_rate` set to 30000, it delays its input by 50 samples.

In MATLAB, `b = fir1(100, [300 6000] / 15000)` gives 101 taps of the same kind.
