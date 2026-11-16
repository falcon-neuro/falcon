---
description: "Falcon's IIR filter: a cascade of second-order sections on every channel. Its input, output and options, the equations it computes, and how it starts without a transient."
---

# IIR filter

The IIR filter runs every channel of its input through the same recursive filter, for example a band-pass in front of a spike detector. It needs far fewer operations per sample than an FIR filter of the same sharpness. The price is a delay that differs from one frequency to the next.

The filter is a cascade of second-order sections, a form in which SciPy and MATLAB can give a filter design. Falcon doesn't design the filter: its coefficients come from such a tool (see [Designing the coefficients](#designing-the-coefficients)).

## Input and output

| | Name | Carries |
|-|------|---------|
| Input | `input` | Samples whose channel values are 32-bit floating-point numbers. |
| Output | `output` | The filtered samples: the same channels and the same rate as the input, each sample with the header of the input sample it was computed from. |

## Options

| Option | Meaning | Default |
|--------|---------|---------|
| `design_rate` | The rate, in Hz, that the coefficients were designed for. It must equal the input's rate. | None: it must be set. |
| `gain` | One factor for the whole cascade. | 1 |
| `sections` | The second-order sections, in the order the input passes through them. Each has five coefficients: `b0`, `b1`, `b2`, `a1` and `a2`. | None: at least one section. |

## What it computes

A section $k$ has the transfer function

$$
H_k(z) = \frac{b_0 + b_1 z^{-1} + b_2 z^{-2}}{1 + a_1 z^{-1} + a_2 z^{-2}}
$$

with $a_0$ already divided out, so that it is 1. The filter is the product of its $K$ sections and the gain $g$:

$$
H(z) = g \prod_{k=1}^{K} H_k(z)
$$

For every sample and every channel, a section computes its output $y$ from its input $x$ and two values, $s_1$ and $s_2$, that it keeps from one sample to the next:

$$
\begin{align*}
y[n] &= b_0 x[n] + s_1[n-1] \\
s_1[n] &= b_1 x[n] - a_1 y[n] + s_2[n-1] \\
s_2[n] &= b_2 x[n] - a_2 y[n]
\end{align*}
$$

This arrangement is known as Transposed Direct Form II. The output of one section is the input of the next. The gain costs nothing while the filter runs: it is multiplied into the first section's $b$ coefficients beforehand.

Every channel uses the same coefficients and keeps its own $s_1$ and $s_2$. The channel values arrive and leave as 32-bit numbers, but the filter computes with 64-bit ones: a section feeds its own rounded results back into itself, and 64-bit arithmetic keeps that error small. The channels are computed eight at a time, or four at a time on a CPU without AVX-512.

## Starting without a transient

A channel often carries a constant offset. A filter that started with $s_1 = s_2 = 0$ would see that offset as a sudden step, and its output would swing before settling. A spike detector reads such a swing as spikes, on every channel at once.

Falcon instead starts each section as if its first input value had always been its input. For a constant input $x[0]$, a section settles at

$$
y_0 = H_k(1) \, x[0], \qquad H_k(1) = \frac{b_0 + b_1 + b_2}{1 + a_1 + a_2}
$$

where the first section's $b$ coefficients include the gain. The two kept values that go with it are

$$
s_1 = y_0 - b_0 x[0], \qquad s_2 = b_2 x[0] - a_2 y_0
$$

Each section's output therefore starts at its settled value $y_0$, which is also the first input value of the next section. The filter's output starts settled too: at 0 for a band-pass.

## Lost samples

Every sample's header says how many samples were lost before it. When that count rises, the input has a gap, and the channel values on its two sides don't join. Filtering across the gap would give a swing of the same kind as a start from zero.

The filter therefore starts again, as described above, on the first sample after a gap. It also writes a warning to the log, such as "restarted after 12 lost samples, at sample 4500000": the number of samples lost, and the number of the sample at which it started again.

## What Falcon checks

Falcon refuses a graph, before it runs, when its IIR filter:

- has no section;
- was designed for another rate than its input's: every cutoff frequency would shift;
- has an unstable section.

A section is stable when both of its poles lie inside the unit circle, which holds exactly when

$$
-1 < a_2 < 1 \quad \text{and} \quad -(1 + a_2) < a_1 < 1 + a_2
$$

The output of an unstable filter grows without bound. A mistyped $a_1$ or $a_2$ usually breaks one of the two conditions, and so does one that is infinite or not a number. The $b$ coefficients are not checked.

## Delay

The filter doesn't wait for later samples: it computes sample $n$ of its output from the input's samples up to $n$.

What it adds is the delay of the design itself, its group delay. That delay differs from one frequency to the next, and it grows with the number of sections and with the sharpness of the band's edges. Where a detection must come early, design the filter no sharper than the experiment needs.

## Designing the coefficients

In SciPy, ask for second-order sections with `output="sos"`. Each row then holds `b0`, `b1`, `b2`, `a0`, `a1` and `a2`, with `a0` equal to 1. SciPy has already multiplied the gain into the first section, so `gain` stays 1.

```python
from scipy import signal

sections = signal.butter(2, [300, 6000], btype="bandpass", fs=30000, output="sos")
for b0, b1, b2, a0, a1, a2 in sections:
    print(b0, b1, b2, a1, a2)
```

This band-pass, from 300 Hz to 6000 Hz for an input at 30 kHz, has two sections. Rounded to ten digits:

| Section | `b0` | `b1` | `b2` | `a1` | `a2` |
|---------|------|------|------|------|------|
| 1 | `0.1906166001` | `0.3812332002` | `0.1906166001` | `-0.4235311646` | `0.2257098675` |
| 2 | `1` | `-2` | `1` | `-1.911551238` | `0.9156533005` |

With `design_rate` set to 30000, the filter passes 1500 Hz unchanged, lets 71% of the amplitude through at 300 Hz and at 6000 Hz, and removes a constant offset entirely.

In MATLAB, `[sos, g] = zp2sos(z, p, k)` gives rows in the same layout, and `g` is the value for `gain`.

## In the code

The filter is the struct {{ api("IirFilter") }}, its options are {{ api("IirFilterOptions") }}, and one section is a {{ api("BiquadSection") }}.
