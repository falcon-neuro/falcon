# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy", "matplotlib"]
# ///
"""Plots a file that Recorder wrote. The file describes itself: its first line is JSON that names
each connection (a slot) and says how its records are laid out. Chunks follow, each a u32 for the
slot, a u32 for the number of records, then that many records of that slot. A sample's record is its
number as a u64 and its channels' values in order as float32; an event's record is its header's
sample number, device timestamp, arrival TSC and lost-sample count, each a u64. All little-endian.

    uv run src/tests/plot_recording.py ~/falcon/runs/<run>/ripple_detector.bin

One sample slot is plotted, a panel per channel: the first one, or the one named with --samples.
Every event slot is drawn as vertical lines, a colour per slot. With --mean-square, a last panel
shows the mean over the channels of their squared values, which is what RippleDetector compares
with its threshold.
"""

import argparse
import json
import pathlib
import struct

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
CHUNK_HEADER = struct.Struct("<II")
EVENT_COLOURS = ["tab:red", "tab:purple", "tab:orange", "tab:brown", "tab:pink"]


def record_type_of(slot: dict) -> np.dtype:
    if slot["kind"] == "samples":
        record_type = np.dtype([("sample_number", "<u8"), ("values", "<f4", (slot["channels"],))])
    elif slot["kind"] == "events":
        record_type = EVENT_TYPE
    else:
        raise SystemExit(f"slot {slot['name']} is of an unknown kind, {slot['kind']}")
    if record_type.itemsize != slot["record_bytes"]:
        raise SystemExit(
            f"slot {slot['name']} says its records take {slot['record_bytes']} bytes, "
            f"not the {record_type.itemsize} of its kind"
        )
    return record_type


def read_recording(path: pathlib.Path) -> tuple[dict, dict[str, np.ndarray]]:
    """Returns the file's description and each slot's records, by the slot's name."""
    with open(path, "rb") as file:
        description = json.loads(file.readline())
        data = file.read()
    if description.get("format") != "falcon_recording" or description.get("version") != 1:
        raise SystemExit(f"{path} is not a falcon_recording file of version 1")

    slots = description["slots"]
    record_types = [record_type_of(slot) for slot in slots]
    chunks: list[list[memoryview]] = [[] for _ in slots]
    view = memoryview(data)
    offset = 0
    while offset + CHUNK_HEADER.size <= len(data):
        slot_index, record_count = CHUNK_HEADER.unpack_from(data, offset)
        offset += CHUNK_HEADER.size
        if slot_index >= len(slots):
            raise SystemExit(f"{path} holds a chunk of slot {slot_index}, which it doesn't describe")
        byte_count = record_count * record_types[slot_index].itemsize
        if offset + byte_count > len(data):
            # The last chunk is cut short, e.g. because the run was killed.
            break
        chunks[slot_index].append(view[offset : offset + byte_count])
        offset += byte_count

    records = {
        slot["name"]: np.frombuffer(b"".join(chunks[slot_index]), dtype=record_types[slot_index])
        for slot_index, slot in enumerate(slots)
    }
    return description, records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("recording", type=pathlib.Path)
    parser.add_argument("--samples", help="the sample slot to plot; the first one if left out")
    parser.add_argument("--start", type=float, default=0.0, help="seconds after the first sample")
    parser.add_argument("--duration", type=float, default=2.0, help="seconds to plot")
    parser.add_argument(
        "--mean-square", action="store_true", help="add a panel with the channels' mean square"
    )
    parser.add_argument("--output", type=pathlib.Path, help="save a PNG instead of opening a window")
    arguments = parser.parse_args()

    description, records = read_recording(arguments.recording)
    slots = description["slots"]
    for slot in slots:
        print(f"slot {slot['name']}: {slot['kind']}, {records[slot['name']].size} records")

    sample_slots = [slot for slot in slots if slot["kind"] == "samples"]
    if arguments.samples is not None:
        sample_slots = [slot for slot in sample_slots if slot["name"] == arguments.samples]
    if not sample_slots:
        raise SystemExit("no such sample slot in the recording")
    sample_slot = sample_slots[0]
    samples = records[sample_slot["name"]]
    if samples.size == 0:
        raise SystemExit(f"slot {sample_slot['name']} holds no samples")
    channel_count = sample_slot["channels"]
    rate = sample_slot["rate"]

    sample_numbers = samples["sample_number"]
    first_sample_number = int(sample_numbers[0])
    # From the sample numbers, not from the position in the file, so lost samples leave a gap in
    # the plot instead of shifting everything after them.
    seconds = (sample_numbers - sample_numbers[0]) / rate
    lost_sample_count = int(sample_numbers[-1]) - first_sample_number + 1 - sample_numbers.size
    print(
        f"{sample_slot['name']}: samples {first_sample_number} to {sample_numbers[-1]}, "
        f"{seconds[-1]:.3f} s, {lost_sample_count} lost"
    )

    window_end = arguments.start + arguments.duration
    is_shown = (seconds >= arguments.start) & (seconds < window_end)
    if not is_shown.any():
        raise SystemExit(f"nothing to plot: the recording is {seconds[-1]:.3f} s long")
    shown_seconds = seconds[is_shown]
    shown_values = samples["values"][is_shown]

    panel_count = channel_count + (1 if arguments.mean_square else 0)
    figure, axes = plt.subplots(
        panel_count, 1, sharex=True, squeeze=False, figsize=(12, 2 * panel_count)
    )
    panels = axes[:, 0]
    for channel in range(channel_count):
        panels[channel].plot(shown_seconds, shown_values[:, channel], linewidth=0.6)
        panels[channel].set_ylabel(f"channel {channel}\n(µV)")
    if arguments.mean_square:
        mean_square = np.mean(shown_values.astype(np.float64) ** 2, axis=1)
        panels[-1].plot(shown_seconds, mean_square, linewidth=0.6, color="tab:green")
        panels[-1].set_ylabel("mean square\n(µV²)")

    event_slots = [slot for slot in slots if slot["kind"] == "events"]
    for event_slot_index, event_slot in enumerate(event_slots):
        events = records[event_slot["name"]]
        event_seconds = (events["sample_number"].astype(np.int64) - first_sample_number) / rate
        shown_event_seconds = event_seconds[
            (event_seconds >= arguments.start) & (event_seconds < window_end)
        ]
        print(f"{event_slot['name']}: {shown_event_seconds.size} of its events are in the plot:")
        for event_second in shown_event_seconds:
            print(f"  at {event_second:.6f} s")
        colour = EVENT_COLOURS[event_slot_index % len(EVENT_COLOURS)]
        for panel in panels:
            for event_second in shown_event_seconds:
                panel.axvline(event_second, color=colour, linewidth=0.8)
        if shown_event_seconds.size > 0:
            panels[0].axvline(
                shown_event_seconds[0], color=colour, linewidth=0.8, label=event_slot["name"]
            )
    if event_slots and panels[0].get_legend_handles_labels()[0]:
        panels[0].legend(loc="upper right")

    panels[-1].set_xlabel(f"time since sample {first_sample_number} (s)")
    figure.suptitle(
        f"{arguments.recording.name}, slot {sample_slot['name']}: {sample_numbers.size} samples, "
        f"{lost_sample_count} lost, {channel_count} channels at {rate:g} Hz"
    )
    figure.tight_layout()

    if arguments.output is not None:
        figure.savefig(arguments.output, dpi=150)
        print(f"saved {arguments.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
