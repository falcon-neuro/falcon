"""Checks IirFilter against SciPy, three ways.

src/tests/iir_filter_test.cpp runs IirFilter under several designs on one test input, and writes
the input, each design's output, its coefficients and the filter they claim to be. For every design
this script checks:

1. Design: SciPy designs the claimed filter itself, and its frequency response must match the
   coefficients'. For the hand-copied Bessel spike band this is an independent check: it catches a
   mistyped coefficient or a wrong gain convention, which comparing outputs made from the same
   coefficients can't. For the designs scripts/python/design_iir_test_filters.py generated, it
   checks that they reached the C++ code intact.
2. Output: SciPy's sosfilt filters the same input, started the way IirFilter starts (sosfilt_zi
   scaled by each channel's first value, and again from the first sample after each loss), and
   every output value must match within the tolerance below.
Then, once, on the Bessel spike band:
3. The check itself: outputs with deliberate mistakes (a start from zero, no restart after a loss,
   two channels swapped, one sample of delay) must miss by far more than the tolerance, so a PASS
   can't come from a comparison too weak to see such mistakes. The comparison is the same code for
   every design.

Usage, from the repository's root:
    uv run --project scripts/python scripts/python/compare_iir_filter.py [directory]
The directory defaults to build/iir_filter_test.
"""

import json
import sys
from pathlib import Path

import numpy as np
from scipy import signal

# IirFilter computes in double but outputs float32, so each value may be off by up to half a float32
# step, 2^-24 of the value. The tolerance allows a whole step, plus a floor for values near 0.
RELATIVE_TOLERANCE = 2.0**-23
ABSOLUTE_TOLERANCE_MICROVOLTS = 1e-3
# A deliberate mistake counts as caught only if it misses by this many tolerances.
MISTAKE_MARGIN = 100.0
# Relative to the largest response. Coefficients carry at least 15 significant digits.
DESIGN_TOLERANCE = 1e-9
MISTAKE_DESIGN = "bessel_spike_band"


def read_samples(path: Path, sample_count: int, channel_count: int) -> np.ndarray:
    values = np.fromfile(path, dtype="<f4")
    if values.size != sample_count * channel_count:
        raise ValueError(f"{path} holds {values.size} values, expected {sample_count * channel_count}")
    return values.reshape(sample_count, channel_count).astype(np.float64)


def sos_of(design_description: dict) -> np.ndarray:
    # SciPy's sections are rows of b0, b1, b2, a0, a1, a2; IirFilter's have a0 = 1 divided out and
    # one gain for the whole cascade, which it folds into the first section.
    sections = np.array(design_description["sections"], dtype=np.float64)
    sos = np.column_stack([sections[:, 0:3], np.ones(len(sections)), sections[:, 3:5]])
    sos[0, 0:3] *= design_description["gain"]
    return sos


def filter_with_scipy(
    sos: np.ndarray,
    input_samples: np.ndarray,
    restart_samples: list[int],
    is_starting_in_steady_state: bool = True,
) -> np.ndarray:
    output_samples = np.empty_like(input_samples)
    steady_state_for_unit_input = signal.sosfilt_zi(sos)  # (sections, 2)
    segment_starts = [0, *restart_samples]
    segment_ends = [*restart_samples, len(input_samples)]
    for segment_start, segment_end in zip(segment_starts, segment_ends):
        segment = input_samples[segment_start:segment_end]
        # (sections, 2, channels): each channel starts as if its first value had always been there.
        initial_state = steady_state_for_unit_input[:, :, np.newaxis] * segment[0]
        if not is_starting_in_steady_state:
            initial_state = np.zeros_like(initial_state)
        output_samples[segment_start:segment_end], _ = signal.sosfilt(
            sos, segment, axis=0, zi=initial_state
        )
    return output_samples


def tolerance_ratio(output: np.ndarray, reference: np.ndarray) -> np.ndarray:
    """Each value's difference as a multiple of its tolerance: 1 or less passes."""
    tolerance = ABSOLUTE_TOLERANCE_MICROVOLTS + (RELATIVE_TOLERANCE * np.abs(reference))
    return np.abs(output - reference) / tolerance


def response_of_sections(sos: np.ndarray, angular_frequencies: np.ndarray) -> np.ndarray:
    z_inverse = np.exp(-1j * angular_frequencies)
    response = np.ones_like(z_inverse)
    for b0, b1, b2, a0, a1, a2 in sos:
        numerator = b0 + (b1 * z_inverse) + (b2 * z_inverse**2)
        denominator = a0 + (a1 * z_inverse) + (a2 * z_inverse**2)
        response *= numerator / denominator
    return response


def design_error(sos: np.ndarray, design: dict, design_rate: float) -> float:
    cutoffs = design["cutoffs"][0] if len(design["cutoffs"]) == 1 else design["cutoffs"]
    zeros, poles, gain = signal.iirfilter(
        design["prototype_order"],
        cutoffs,
        rp=design["passband_ripple"],
        rs=design["stopband_attenuation"],
        btype=design["band_type"],
        ftype=design["family"],
        fs=design_rate,
        output="zpk",
    )
    frequencies = np.linspace(0.0, design_rate / 2.0, 4097)
    _, designed_response = signal.freqz_zpk(zeros, poles, gain, worN=frequencies, fs=design_rate)
    tested_response = response_of_sections(sos, 2.0 * np.pi * frequencies / design_rate)
    return float(
        np.max(np.abs(tested_response - designed_response)) / np.max(np.abs(designed_response))
    )


def check_design_and_output(
    design_description: dict,
    input_samples: np.ndarray,
    falcon_output: np.ndarray,
    restart_samples: list[int],
) -> bool:
    sos = sos_of(design_description)
    relative_design_error = design_error(
        sos, design_description["design"], design_description["design_rate"]
    )
    scipy_output = filter_with_scipy(sos, input_samples, restart_samples)
    ratios = tolerance_ratio(falcon_output, scipy_output)
    worst_sample, worst_channel = np.unravel_index(np.argmax(ratios), ratios.shape)
    worst_difference = abs(
        falcon_output[worst_sample, worst_channel] - scipy_output[worst_sample, worst_channel]
    )
    is_design_passing = relative_design_error < DESIGN_TOLERANCE
    is_output_passing = bool(ratios.max() <= 1.0)
    print(
        f"{design_description['name']} ({len(sos)} sections): "
        f"1. design {'PASS' if is_design_passing else 'FAIL'}, off by {relative_design_error:.1e}; "
        f"2. output {'PASS' if is_output_passing else 'FAIL'}, worst {ratios.max():.2f} of the "
        f"tolerance: {worst_difference:.2g} µV at "
        f"{scipy_output[worst_sample, worst_channel]:.6g} µV (sample {worst_sample}, "
        f"channel {worst_channel}); largest output {np.abs(scipy_output).max():.4g} µV"
    )
    return is_design_passing and is_output_passing


def check_that_mistakes_are_caught(
    design_description: dict,
    input_samples: np.ndarray,
    falcon_output: np.ndarray,
    restart_samples: list[int],
) -> bool:
    sos = sos_of(design_description)
    scipy_output = filter_with_scipy(sos, input_samples, restart_samples)
    channel_count = input_samples.shape[1]
    mistaken_outputs = {
        "starting from zero instead of steady state": filter_with_scipy(
            sos, input_samples, restart_samples, is_starting_in_steady_state=False
        ),
        "running across the losses instead of restarting": filter_with_scipy(
            sos, input_samples, []
        ),
        "channels 0 and 1 swapped": scipy_output[:, [1, 0, *range(2, channel_count)]],
        "the output one sample late": np.vstack([scipy_output[:1], scipy_output[:-1]]),
    }
    are_all_caught = True
    print(f"3. the check itself, on {design_description['name']}, against deliberate mistakes:")
    for mistake, mistaken_output in mistaken_outputs.items():
        worst_ratio = tolerance_ratio(falcon_output, mistaken_output).max()
        is_caught = bool(worst_ratio > MISTAKE_MARGIN)
        print(
            f"   {mistake}: would miss by up to {worst_ratio:.3g} tolerances: "
            f"{'caught' if is_caught else 'NOT CAUGHT'}"
        )
        are_all_caught = are_all_caught and is_caught
    print(f"   {'PASS' if are_all_caught else 'FAIL'}")
    return are_all_caught


def main() -> int:
    repository_root = Path(__file__).resolve().parents[2]
    directory = Path(sys.argv[1]) if len(sys.argv) > 1 else repository_root / "build" / "iir_filter_test"
    description = json.loads((directory / "test.json").read_text())
    channel_count = description["channel_count"]
    sample_count = description["sample_count"]
    restart_samples = description["restart_samples"]
    input_samples = read_samples(directory / "input.f32", sample_count, channel_count)

    is_passing = True
    for design_description in description["designs"]:
        falcon_output = read_samples(
            directory / f"{design_description['name']}.f32", sample_count, channel_count
        )
        is_passing &= check_design_and_output(
            design_description, input_samples, falcon_output, restart_samples
        )
        if design_description["name"] == MISTAKE_DESIGN:
            is_passing &= check_that_mistakes_are_caught(
                design_description, input_samples, falcon_output, restart_samples
            )
    print(f"{'PASS' if is_passing else 'FAIL'}: {len(description['designs'])} designs")
    return 0 if is_passing else 1


if __name__ == "__main__":
    sys.exit(main())
