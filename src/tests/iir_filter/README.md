# IirFilter test

Checks IirFilter (`src/processors/iir_filter.cpp`) against SciPy: its coefficients against SciPy's own designs, and its output against SciPy's on the same input.

## What it does

- **`iir_filter_test.cpp`** filters a known input on 37 channels, so every lane of every vector half carries its own signal: DC offsets, chirps, noise, impulses and square waves, different on each channel, and losses that make the filter restart, twice apart and twice in a row. It checks that each output header is its input's, and writes the input, each design's output, its coefficients and the filter they claim to be to `build/iir_filter_test/`.
- **The designs** are Falcon 1's Bessel spike band, copied by hand with its separate gain, and nine that `design_iir_test_filters.py` designs with SciPy and writes into the tracked `iir_filter_test_designs.cpp`, since the coefficients must be compile-time constants (a small preview of the `pipeline.cpp` generator). They cover other families (Butterworth, Chebyshev I and II, elliptic, whose numerators aren't 1, ±2, 1), every band type, a first-order section, 1 to 12 sections, and the low cutoffs that are the reason IirFilter computes in double (theta, a 1 Hz high-pass, the 150–250 Hz ripple band).
- **`compare_iir_filter.py`** checks, for every design, that SciPy's own design of the claimed filter has the coefficients' frequency response (for the hand-copied Bessel this catches a mistyped coefficient), and that SciPy's `sosfilt`, started the same way, agrees with every output value within one float32 step plus 0.001 µV, since IirFilter rounds its double result to float32. Once, it checks that outputs with deliberate mistakes (a start from zero, no restart after a loss, two channels swapped, one sample of delay) miss by more than 100 tolerances, so the comparison can see such mistakes.
- **`pyproject.toml` and `uv.lock`** describe the Python environment of the two scripts (NumPy and SciPy), which uv sets up.

## How to run it

From the repository's root, with `add_executable(falcon …)` in `CMakeLists.txt` pointing at `src/tests/iir_filter/iir_filter_test.cpp`:

```
uv run --project src/tests/iir_filter src/tests/iir_filter/design_iir_test_filters.py   # only to change the designs
scripts/make.sh
./build/falcon
uv run --project src/tests/iir_filter src/tests/iir_filter/compare_iir_filter.py
```

Running the design script again leaves every coefficient as it is, but indents nine lines of `iir_filter_test_designs.cpp` one space further than the tracked file has them.

## Results

Measured on 2026-11-08. All runs used the AVX2 version: the machine has no AVX512.

On 2026-11-15 the final run was repeated with the AVX512 version, on a machine that has AVX512: PASS on every check for all ten designs, each worst output difference again at most half a float32 step at its value. Its numbers differ slightly from the table below, since the test seeds its noise from the system's random source, so every run has its own input.

### Final run: 10 designs, 37 channels

**Result: PASS on every check.**

Test input: 37 channels, 30,000 samples (1 s at 30 kHz). Each channel carries its own mix:
- a DC offset from −45 to +45 mV;
- a chirp from 1 Hz to 10 kHz, with its own amplitude and phase;
- white noise;
- an impulse every 0.1 s, at its own moment;
- a square wave at its own frequency.

The lost-sample count changes at samples 12,000, 20,000 and 20,001, so IirFilter restarts there. Every output header matched its input's.

Tolerances:
- **Design:** the coefficients' frequency response must match SciPy's own design of the claimed filter within 10⁻⁹ of the largest response.
- **Output:** every value must match SciPy's `sosfilt` (started with `sosfilt_zi`, and restarted the same way) within one float32 step of the value plus 0.001 µV. IirFilter rounds its double result to float32, which alone can cost up to half a step.

| Design | Sections | Design off by | Worst output difference | at a value of | Share of tolerance | Largest output |
|---|---|---|---|---|---|---|
| Bessel band-pass 300–6000 Hz (Falcon 1's, hand-copied, separate gain) | 2 | 2.0×10⁻¹⁴ | 5.6×10⁻⁵ µV | 1047.91 µV | 0.05 | 1203 µV |
| Butterworth low-pass 300 Hz | 2 | 7.6×10⁻¹⁴ | 0.0019 µV | 32,778.3 µV | 0.40 | 45,910 µV |
| Butterworth low-pass 100 Hz, first order | 1 | 7.3×10⁻¹⁵ | 0.0019 µV | 32,774.9 µV | 0.40 | 45,810 µV |
| Butterworth high-pass 1 Hz | 1 | 1.0×10⁻¹⁰ | 6.2×10⁻⁵ µV | 1074.4 µV | 0.06 | 2345 µV |
| Butterworth band-pass 4–12 Hz (theta) | 2 | 5.5×10⁻¹¹ | 1.5×10⁻⁵ µV | 261.74 µV | 0.01 | 439 µV |
| Butterworth band-stop 45–55 Hz | 2 | 6.4×10⁻¹² | 0.002 µV | 32,847.7 µV | 0.40 | 46,890 µV |
| Butterworth band-pass 300–6000 Hz | 12 | 1.9×10⁻¹³ | 6.1×10⁻⁵ µV | −1038.33 µV | 0.05 | 1222 µV |
| Chebyshev I low-pass 6 kHz, 1 dB ripple | 3 | 4.1×10⁻¹⁵ | 0.002 µV | 32,799.5 µV | 0.40 | 41,180 µV |
| Chebyshev II band-pass 150–250 Hz, 40 dB | 4 | 7.1×10⁻¹³ | 3×10⁻⁵ µV | −522.506 µV | 0.03 | 917.4 µV |
| Elliptic band-pass 150–250 Hz, 0.5 dB / 40 dB | 4 | 1.1×10⁻¹² | 6.1×10⁻⁵ µV | 1054.51 µV | 0.05 | 1279 µV |

What the numbers mean:
- **Every worst difference is half a float32 step at its value**, the most that rounding a correct double result to float32 can produce. A float32 step is 1.2×10⁻⁴ µV between 1024 and 2048 µV, and 0.0039 µV between 32,768 and 65,536 µV. So the double arithmetic matches SciPy's for every design, including the low cutoffs (1 Hz high-pass, theta, the ripple band) that are the reason IirFilter computes in double.
- **The hand-copied Bessel matches SciPy's own design to 2×10⁻¹⁴**: no coefficient was mistyped, and the gain is applied the right way.
- **The generated designs match to 10⁻¹⁰ or better.** The largest gaps, for the 1 Hz high-pass and theta, come from computing the response near poles at the edge of stability, where sections and poles/zeros round differently. The coefficients themselves are the same.

**The check itself**, on the Bessel design: outputs with deliberate mistakes must miss by more than 100 tolerances.

| Deliberate mistake | Misses by (tolerances) |
|---|---|
| Starting from zero instead of steady state | 6.98×10⁶ |
| Running across the losses instead of restarting | 9.02×10⁵ |
| Channels 0 and 1 swapped | 8.89×10⁵ |
| The output one sample late | 8.95×10⁵ |

All four were caught by about a million tolerances or more.

### Earlier runs

**Bessel only, 37 channels, fixed tolerance 0.001 µV.** PASS.
- Design off by 2.0×10⁻¹⁴.
- Largest output difference 6×10⁻⁵ µV, at sample 6673, channel 24 (1044.01 µV); largest output 1139 µV.
- Deliberate mistakes missed by:
  - start from zero: 38,300 µV;
  - no restart: 928 µV;
  - channels 0 and 1 swapped: 877 µV;
  - one sample late: 898 µV.

**Bessel only, 3 channels, fixed tolerance 0.001 µV.** PASS. This run left most vector halves unused; see below.

| Channel | Input | Largest difference | Largest output |
|---|---|---|---|
| 0 | +50 mV DC, chirp | 1.53×10⁻⁵ µV | 591.1 µV |
| 1 | −20 mV DC, noise, impulses | 2.71×10⁻⁵ µV | 822.4 µV |
| 2 | 7 Hz square wave | 6.35×10⁻⁶ µV | 168 µV |

Its worst difference was 2.71×10⁻⁵ µV, at sample 6, channel 1 (−592.964 µV).

The 3-channel run was replaced because IirFilter splits each SIMD vector into two halves of doubles, and 3 channels only fill the first half of the first vector. The 37-channel input gives every lane of every half its own signal, on both instruction sets.

### IirFilter in the filter comparison pipeline

Pipeline: `src/pipelines/filter_comparison_pipeline.cpp`. It runs a 2-channel spike train (3 spikes/s) on an 8 Hz, 500 µV LFP through IirFilter and SpikeDetector at −70 µV, next to an unfiltered SpikeDetector. Without losses it expects 6 filtered and 17 unfiltered detections per second.

- **No losses:** 6 filtered and 17 unfiltered per second, every second; the first second after a start was also clean.
- **Ring drops, before IirFilter restarted after a loss** (4-slot ring, busy machine): filtered 7, 8, 10, 8, 13 and 7 per second while the ring dropped bursts of up to 1748 samples, against 6 expected. That's about 17 false spikes in 6 s. Unfiltered stayed at 17.
- **Ring drops, with the restart** (same setup): about 13,710 samples dropped over 47 s. Filtered gave 6 every second but one, which gave 5: 281 in total, 6 × 47 − 1. So no false spikes, and one spike was lost in a gap.
- **Ring check** (64-slot ring, a checker reading the sample numbers): over 1019 filtered spikes, 0 were out of place. There were 2 breaks in the sample numbers, 57 samples each, 114 in total, exactly the ring's own drop warning. So the ring handed over every published sample exactly once.

## Not yet tested

Most important first:
- Extreme inputs, to document what happens: NaN or infinity (likely spoiling that channel's output until the next restart), values at an ADC's limit, long runs of zeros (denormals).
- Long runs and other channel counts (1, exactly 16): least needed, since a stable filter forgets its rounding errors and 37 channels already reach the padding.
