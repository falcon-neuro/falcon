# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Plots a file that SampleRecorder wrote: raw little-endian records, each a sample's number as a
u64 followed by its channels' values in order as float32. The file holds neither its channel count
nor its rate, so both are arguments; SampleRecorder logs them when it starts.

    uv run scripts/plot_recording.py ~/falcon/runs/<run>/ripple_bursts.bin --channels 4 --rate 30000

With --events, a file that EventRecorder wrote (per event, its header's sample number, device
timestamp, arrival TSC and lost-sample count, each a u64) is drawn as a vertical line per event.
With --mean-square, a last panel shows the mean over the channels of their squared values, which
is what RippleDetector compares with its threshold.
"""

import argparse
import pathlib

import matplotlib.pyplot as plt
import numpy as np

EVENT_TYPE = np.dtype(
    [
        ("sample_number", "<u8"),
        ("device_timestamp", "<u8"),
        ("arrival_tsc", "<u8"),
        ("lost_sample_count", "<u8"),
    ]
)


def read_records(path: pathlib.Path, record_type: np.dtype, hint: str) -> np.ndarray:
    byte_count = path.stat().st_size
    if byte_count % record_type.itemsize != 0:
        raise SystemExit(
            f"{path} holds {byte_count} bytes, which is not a whole number of records of "
            f"{record_type.itemsize} bytes{hint}"
        )
    return np.fromfile(path, dtype=record_type)


def read_recording(path: pathlib.Path, channel_count: int) -> np.ndarray:
    """Returns the records: `["sample_number"]` per sample, `["values"]` per sample and channel."""
    record_type = np.dtype([("sample_number", "<u8"), ("values", "<f4", (channel_count,))])
    records = read_records(path, record_type, ": is --channels right?")
    if records.size == 0:
        raise SystemExit(f"{path} holds no samples")
    return records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("recording", type=pathlib.Path)
    parser.add_argument("--channels", type=int, required=True, help="the stream's channel count")
    parser.add_argument("--rate", type=float, required=True, help="the stream's rate in Hz")
    parser.add_argument("--start", type=float, default=0.0, help="seconds after the first sample")
    parser.add_argument("--duration", type=float, default=2.0, help="seconds to plot")
    parser.add_argument("--events", type=pathlib.Path, help="a file that EventRecorder wrote")
    parser.add_argument(
        "--mean-square", action="store_true", help="add a panel with the channels' mean square"
    )
    parser.add_argument("--output", type=pathlib.Path, help="save a PNG instead of opening a window")
    arguments = parser.parse_args()

    records = read_recording(arguments.recording, arguments.channels)
    sample_numbers = records["sample_number"]
    first_sample_number = int(sample_numbers[0])
    # From the sample numbers, not from the position in the file, so lost samples leave a gap in
    # the plot instead of shifting everything after them.
    seconds = (sample_numbers - sample_numbers[0]) / arguments.rate
    lost_sample_count = int(sample_numbers[-1]) - first_sample_number + 1 - sample_numbers.size
    print(
        f"{sample_numbers.size} samples, numbers {first_sample_number} to {sample_numbers[-1]}, "
        f"{seconds[-1]:.3f} s, {lost_sample_count} lost"
    )

    window_end = arguments.start + arguments.duration
    is_shown = (seconds >= arguments.start) & (seconds < window_end)
    if not is_shown.any():
        raise SystemExit(f"nothing to plot: the recording is {seconds[-1]:.3f} s long")
    shown_seconds = seconds[is_shown]
    shown_values = records["values"][is_shown]

    shown_event_seconds = np.array([])
    if arguments.events is not None:
        events = read_records(arguments.events, EVENT_TYPE, "")
        event_seconds = (
            events["sample_number"].astype(np.int64) - first_sample_number
        ) / arguments.rate
        shown_event_seconds = event_seconds[
            (event_seconds >= arguments.start) & (event_seconds < window_end)
        ]
        print(f"{events.size} events, {shown_event_seconds.size} of them in the plot:")
        for event_second in shown_event_seconds:
            print(f"  at {event_second:.6f} s")

    panel_count = arguments.channels + (1 if arguments.mean_square else 0)
    figure, axes = plt.subplots(
        panel_count, 1, sharex=True, squeeze=False, figsize=(12, 2 * panel_count)
    )
    panels = axes[:, 0]
    for channel in range(arguments.channels):
        panels[channel].plot(shown_seconds, shown_values[:, channel], linewidth=0.6)
        panels[channel].set_ylabel(f"channel {channel}\n(µV)")
    if arguments.mean_square:
        mean_square = np.mean(shown_values.astype(np.float64) ** 2, axis=1)
        panels[-1].plot(shown_seconds, mean_square, linewidth=0.6, color="tab:green")
        panels[-1].set_ylabel("mean square\n(µV²)")
    for panel in panels:
        for event_second in shown_event_seconds:
            panel.axvline(event_second, color="tab:red", linewidth=0.8)
    panels[-1].set_xlabel(f"time since sample {first_sample_number} (s)")
    title = (
        f"{arguments.recording.name}: {sample_numbers.size} samples, {lost_sample_count} lost, "
        f"{arguments.channels} channels at {arguments.rate:g} Hz"
    )
    if arguments.events is not None:
        title += f"; red lines: {arguments.events.name}"
    figure.suptitle(title)
    figure.tight_layout()

    if arguments.output is not None:
        figure.savefig(arguments.output, dpi=150)
        print(f"saved {arguments.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
